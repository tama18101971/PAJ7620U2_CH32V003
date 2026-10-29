# driver/gesture_semantics

## Consumption model

- Datasheet: "The gesture detection interrupt flag will be cleared by reading
  Register Bank 0, Addr 0x43 and 0x44." A read is destructive.
- Therefore the driver separates *acquire* from *consume*:
  - `read_gesture_flags()` reads 0x43/0x44 as one burst where the I2C buffer
    API is available; `I2C_LITE` falls back to two reads; when wave is disabled
    and `PAJ_PS_MASK == 0` only 0x43 is read (one transaction less per poll);
  - `paj_available()` is the legacy immediate poll and caches the gesture;
  - `paj_readGesture()` is the legacy blocking consumer;
  - `paj_poll(now_ms)` is the recommended non-blocking service and
    `paj_takeGesture()` consumes its single ready slot;
  - `paj_clearInterrupt()` also goes through the flag read, so an INT clear
    never destroys a gesture that is already in hand.
- A failing second byte costs only the wave flag; a partially read 0x43
  gesture is retained because it cannot be re-read.
- An unread cached result is never overwritten — and while it is unread no
  flag read happens at all, so the active-low INT line stays asserted until
  the application takes the gesture.
- Adding any other 0x43/0x44 read is a bug: it eats the gesture the caller
  was about to receive.
- Burst read from 0x43 to 0x44 is documented in datasheet V0.8 (Burst Read
  Protocol), so the default path uses it and saves one transaction.

## Masking (compile-time and runtime)

- `g_enabled_mask1` / `g_enabled_wave` start from `PAJ_GESTURE_MASK` /
  `PAJ_WAVE_MASK` and are changed by `paj_setGestureMask()` (getters:
  `paj_getGestureMask()`, `paj_isWaveEnabled()`).
- The read bytes are ANDed with the mask **before** decode and before the
  contradiction check: 0x41/0x42 only gate the INT pin, the chip keeps
  setting all flags. Without the software mask a disabled Forward/Backward
  pair would still win the decode or discard a valid Left.
- 0x41/0x42 are rewritten only when `g_int_masks_on` is set (init with
  `PAJ_INT_MODE=1`, or after `paj_enableInterrupt()`), so changing the mask
  never resurrects interrupts that were turned off.
- Presets: `PAJ_GESMASK_DIRECTIONS` (0x0F), `PAJ_GESMASK_SIX_AXIS` (0x3F),
  `PAJ_GESMASK_ALL` (0xFF). Changing the mask resets pending **and** ready
  gesture state; repeating the same values is a no-op (no bus traffic).

## Decode order

`Forward, Backward, CW, CCW`, then wave (`0x44 bit0`), then
`Up, Down, Left, Right`. Z gestures intentionally outrank a simultaneous
directional bit. The sensor does not expose the time order of flags, so this
is policy rather than reconstructed sequence. `forward`/`backward`/`wave` are
rotation-independent.

Exception: with `PAJ_Z_OVERRIDES_XY=0` (default, palm control) Z bits are
dropped from a read that also carries an XY bit, because a wide palm produces
exactly that combination (object size/brightness jump). `=1` keeps the old
Z-first behaviour for finger operation.

Before decoding, `contradictory_mask()` finds every antipodal pair
(`GES_PAIR_*`) that is fully set and, with `PAJ_REJECT_AMBIGUOUS=1` (default),
clears **only those bits**; the remaining bits stay decodable
(`Left|Forward|Backward` → `Left`). Measured ambiguity on the bench was
`0x43 = 0x3F`. Counted in `g_ambiguous` (`paj_getAmbiguousCount()`) after
masking, so disabled gestures never inflate it. A high counter means weak
signal (IR LED rail, hand distance, gesture speed) or a bad
`PAJ_XDIR_THRESHOLD`/`PAJ_YDIR_THRESHOLD`, not a driver bug.
`PAJ_REJECT_AMBIGUOUS=0` keeps the bits and uses the decode priority above.

## Rotation

`PAJ_GESTURE_ROTATE` 0..3 remaps chip axes to physical ones purely with
compile-time macros (`PAJ_DIR_*`); at 0 the macros are identity. A module
rotated 90° CW reports physical UP as chip LEFT, hence
`PAJ_DIR_LEFT = GESTURE_UP`, and CW/CCW swap for 90/270°.

## Loops and latency

- Preferred idiom: `if (paj_poll(now_ms)) g = paj_takeGesture();`
  with application-supplied monotonic milliseconds.
- `paj_poll()` reads at most every `PAJ_POLL_INTERVAL_MS` (5 ms), holds
  directional gestures for `PAJ_GESTURE_CONFIRM_MS` (20 ms) and applies
  `PAJ_GESTURE_COOLDOWN_MS` (230 ms) after publication.
- Palm policy (`PAJ_Z_OVERRIDES_XY=0`): a Z flag never cancels a pending
  direction, and a `GESTURE_NONE` poll after arming publishes the pending
  direction immediately instead of waiting out the window — earlier than the
  spurious Z flag from the trailing edge of the palm, which then lands in
  cooldown.
- Finger policy (`=1`): Z is published immediately and cancels a pending
  direction; the pending direction waits for the full confirm window.
- EXTI is an accelerator, not the service loop: call `paj_poll()` regularly so
  a directional confirmation window expires without another edge.
- The sensor and the driver each hold only one latest result, so an unread
  ready slot can lose newer events. A small queue is not implemented.
- Legacy `paj_available()`/`paj_readGesture()` retain the immediate/blocking
  behaviour. `PAJ_OWN_SYSTICK=1` uses SysTick HCLK/8 for the legacy wait;
  `=0` uses an iteration budget with `Delay_Ms(1)`. No new API path calls
  `Delay_*`.
