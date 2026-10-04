# driver/gesture_semantics

## Consumption model

- Datasheet: "The gesture detection interrupt flag will be cleared by reading
  Register Bank 0, Addr 0x43 and 0x44." A read is destructive.
- Therefore the driver separates *acquire* from *consume*:
  - `poll_gesture()` reads 0x43, decodes and caches into
    `g_gesture`/`g_has_gesture`, then reads 0x44 and adds wave only if 0x43
    yielded nothing;
  - `paj_available()` = `poll_gesture()` + "is something pending";
  - `paj_readGesture()` = take the cache, or block-wait for a new one;
  - `paj_clearInterrupt()` calls `poll_gesture()` too, so an INT clear never
    destroys a gesture that is already in hand.
- The two registers are read in that order and decoded separately on purpose:
  a failing 0x44 read costs only the wave flag. `paj_available()` /
  `paj_readGesture()` still hand the cached 0x43 gesture to the caller and
  report `PAJ_ERR_I2C` — the flags cannot be re-read, so dropping the gesture
  would lose it for good.
- Adding any other 0x43/0x44 read is a bug: it eats the gesture the caller
  was about to receive.
- Flags are read as two single-register transactions, not one 2-byte block:
  auto-increment from 0x43 is not documented in the datasheet.

## Decode order

`Up, Down, Left, Right, Forward, Backward, CW, CCW`, then wave (`0x44 bit0`) as
the fallback. `forward`/`backward`/`wave` are rotation-independent.

Before decoding, `flags_contradictory()` rejects a read holding both halves of
an antipodal pair (`GES_PAIR_*`): one physical gesture cannot be up *and*
down. Measured ambiguity on the bench was `0x43 = 0x3F`. With
`PAJ_REJECT_AMBIGUOUS=1` (default) such a read yields `GESTURE_NONE` and
increments `g_ambiguous` (`paj_getAmbiguousCount()`), because emitting the
lowest set bit turned indecision into a confidently wrong gesture — the exact
failure mode users report as unstable recognition. A high counter means weak
signal (IR LED rail, hand distance, gesture speed) or a bad
`PAJ_XDIR_THRESHOLD`/`PAJ_YDIR_THRESHOLD`, not a driver bug.
`PAJ_REJECT_AMBIGUOUS=0` restores the lowest-set-bit behaviour.

## Rotation

`PAJ_GESTURE_ROTATE` 0..3 remaps chip axes to physical ones purely with
compile-time macros (`PAJ_DIR_*`); at 0 the macros are identity. A module
rotated 90° CW reports physical UP as chip LEFT, hence
`PAJ_DIR_LEFT = GESTURE_UP`, and CW/CCW swap for 90/270°.

## Loops and latency

- Idiom: `while (guard-- && paj_available()) paj_readGesture();`
  (the APDS drain loop keeps working unchanged).
- The blocking wait polls every ~1 ms; gesture rate is 120 Hz, so a gesture
  arrives within one poll interval. `PAJ_GESTURE_POLL` is derived from
  `g_ticks_per_ms`, not from an I2C timing assumption.
- The sensor keeps only the **latest** gesture: poll slower than the gesture
  rate and gestures are lost. The INT-driven path loses none.
- `PAJ_OWN_SYSTICK=1` starts its own SysTick (HCLK/8, no `Delay_*` inside the
  loop) because the SDK's `Delay_*` zeroes and stops `SysTick->CNT` on every
  call — `SysTick->CNT` is not a timebase here. `=0` switches to an iteration
  budget with `Delay_Ms(1)`.
