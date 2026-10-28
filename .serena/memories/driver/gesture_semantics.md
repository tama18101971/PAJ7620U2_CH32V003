# driver/gesture_semantics

## Consumption model

- Datasheet: "The gesture detection interrupt flag will be cleared by reading
  Register Bank 0, Addr 0x43 and 0x44." A read is destructive.
- Therefore the driver separates *acquire* from *consume*:
  - `read_gesture_flags()` reads 0x43/0x44 as one burst where the I2C buffer
    API is available; `I2C_LITE` falls back to two reads;
  - `paj_available()` is the legacy immediate poll and caches the gesture;
  - `paj_readGesture()` is the legacy blocking consumer;
  - `paj_poll(now_ms)` is the recommended non-blocking service and
    `paj_takeGesture()` consumes its single ready slot;
  - `paj_clearInterrupt()` also goes through the flag read, so an INT clear
    never destroys a gesture that is already in hand.
- A failing second byte costs only the wave flag; a partially read 0x43
  gesture is retained because it cannot be re-read.
- An unread cached result is never overwritten.
- Adding any other 0x43/0x44 read is a bug: it eats the gesture the caller
  was about to receive.
- Burst read from 0x43 to 0x44 is documented in datasheet V0.8 (Burst Read
  Protocol), so the default path uses it and saves one transaction.

## Decode order

`Forward, Backward, CW, CCW`, then wave (`0x44 bit0`), then
`Up, Down, Left, Right`. Z gestures intentionally outrank a simultaneous
directional bit. The sensor does not expose the time order of flags, so this
is policy rather than reconstructed sequence. `forward`/`backward`/`wave` are
rotation-independent.

Before decoding, `flags_contradictory()` rejects a read holding both halves of
an antipodal pair (`GES_PAIR_*`): one physical gesture cannot be up *and*
down. Measured ambiguity on the bench was `0x43 = 0x3F`. With
`PAJ_REJECT_AMBIGUOUS=1` (default) such a read yields `GESTURE_NONE` and
increments `g_ambiguous` (`paj_getAmbiguousCount()`), because emitting the
lowest set bit turned indecision into a confidently wrong gesture — the exact
failure mode users report as unstable recognition. A high counter means weak
signal (IR LED rail, hand distance, gesture speed) or a bad
`PAJ_XDIR_THRESHOLD`/`PAJ_YDIR_THRESHOLD`, not a driver bug.
`PAJ_REJECT_AMBIGUOUS=0` accepts the read and uses the decode priority above.

## Rotation

`PAJ_GESTURE_ROTATE` 0..3 remaps chip axes to physical ones purely with
compile-time macros (`PAJ_DIR_*`); at 0 the macros are identity. A module
rotated 90° CW reports physical UP as chip LEFT, hence
`PAJ_DIR_LEFT = GESTURE_UP`, and CW/CCW swap for 90/270°.

## Loops and latency

- Preferred idiom: `if (paj_poll(now_ms)) g = paj_takeGesture();`
  with application-supplied monotonic milliseconds.
- `paj_poll()` reads at most every `PAJ_POLL_INTERVAL_MS` (5 ms), holds
  directional gestures for `PAJ_GESTURE_CONFIRM_MS` (40 ms), emits Z gestures
  immediately and applies `PAJ_GESTURE_COOLDOWN_MS` (150 ms) after publication.
- EXTI is an accelerator, not the service loop: call `paj_poll()` regularly so
  a directional confirmation window expires without another edge.
- The sensor and the driver each hold only one latest result, so an unread
  ready slot can lose newer events. A small queue is not implemented.
- Legacy `paj_available()`/`paj_readGesture()` retain the immediate/blocking
  behaviour. `PAJ_OWN_SYSTICK=1` uses SysTick HCLK/8 for the legacy wait;
  `=0` uses an iteration budget with `Delay_Ms(1)`. No new API path calls
  `Delay_*`.
