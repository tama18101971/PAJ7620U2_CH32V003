# driver/register_map

## Two banks, one accessor pair

- `0xEF` (`REG_BANK_SEL`) exists in both banks and selects bank 0 or 1.
  Addresses overlap between banks with different meaning (0x44 = gesture/PS
  flags in bank 0, PS gain in bank 1; 0x72 = Enable in bank 1 only).
- `rd(bank, reg, out)` / `wr(bank, reg, val)` are the only accessors; the bank
  argument is mandatory on purpose — dropping it "works" until a bank switch
  happens and then silently reads the wrong register.
- `bank_sel()` skips the 0xEF write when `g_bank` already matches, so the
  hot polling path costs one extra comparison, not one extra transaction.
- `PAJ_BANK_UNKNOWN` (0xFF) is written by `wakeup_cmd()` and
  `paj_shutdown()`; the next access re-selects explicitly.
- `wait_ready()` must (and does) call `bank_sel(PAJ_B0)` on every poll
  iteration: the wake-up status register 0x00 is in bank 0, the bank is not
  retained across suspend, and a write while the sensor sleeps NACKs.

## Addresses actually used

Bank 0: `0x00` wake-up status (`0x20` = done), `0x03` suspend command (w `0x01`),
`0x41` gesture interrupt mask, `0x42` bit1 PS mask / bit0 wave mask,
`0x43` gesture flags, `0x44` bit1 PS flag / bit0 wave flag, `0x45` state[1:0],
`0x69/0x6A` PS hysteresis, `0x6B` PS approach, `0x6C` PS data, `0xB0..0xB2`
object brightness/size.
Bank 1: `0x72` Enable (bit0), `0x44` PS gain, `0x67..0x6E` state-machine steps.

## Gesture flags (0x43) bit order

`bit0 Up, bit1 Down, bit2 Left, bit3 Right, bit4 Forward, bit5 Backward,
bit6 CW, bit7 CCW`; wave is `0x44 bit0`.

Decode is not simply "lowest set bit". The sensor sets bits independently, so
a weak signal yields several at once — `0x43 = 0x3F` (up+down+left+right+
forward+backward) was measured on the bench. `flags_contradictory()` rejects
any read containing both halves of an antipodal pair (`GES_PAIR_*` in
`paj7620_regs.h`) while `PAJ_REJECT_AMBIGUOUS=1` (default); events are counted
in `g_ambiguous` / `paj_getAmbiguousCount()`. `PAJ_REJECT_AMBIGUOUS=0`
restores the lowest-set-bit behaviour. Wave loses to any directional flag.

## Gesture thresholds

Bank 0 `0x93 R_XDirectionThd[4:0]` (left/right) and `0x94 R_YDirectionThd[4:0]`
(up/down) are in the datasheet "Register Map" but **absent from the "i. Initial"
array**, so they stay at the power-on value and cannot be tuned through the
table. Measured power-on: `0x93=0x0D`, `0x94=0x0A`. They are now
`PAJ_XDIR_THRESHOLD` / `PAJ_YDIR_THRESHOLD` (range 0..0x1F) written by
`apply_axis_thresholds()`, so the left/right and up/down sensitivity is the
one lever over these four gestures.

Every other gesture-engine register *is* in the init array: `0x90`
no-motion count, `0x95` Z threshold, `0x96`/`0x97` forward/backward, `0x9A`
rotate, `0x9C` IIR filter weight + distance, `0x9F` bit4..7 = rotate / Z / Y /
X detection enables (init writes `0xF9`, i.e. all four groups on), `0xCF`
wave count + angle, `0xD0` abort count + XY ratio.

## Tables

`g_init_seq` (47 entries) = datasheet "i. Initial" minus the 0xEF writes and
minus `{bank1, 0x72, 0x01}` (Enable is written by `enable_sensor()` so
`paj_wakeup()` can reuse it). `g_ps_seq` (26) = "iii. Change to PS mode",
`g_gesture_seq` (24) = "v. Change to Gesture mode" (its trailing mask writes
are replaced by config-driven masks). Order is datasheet order — do not
"optimize" it.

## Two register maps in the wild — this driver follows the official one

Widely copied community code (Adafruit-style PAJ7620 ports) assumes
PART_ID `0xB1` at `0x00`, a gesture *code* 1..6 at `0x43`, and bank select in
`SYSCTL1` bit4. That map contradicts both PixArt revisions in `docs/`, where
`0x43` is a one-hot bitmask and bank select is `0xEF`. The official map also
explains the shipped Seeed/Grove library's flag constants
(`GES_UP_FLAG == 0x01` … `GES_COUNT_CLOCKWISE_FLAG == 0x80`).
If real hardware disagrees, only `paj7620_regs.h` and the three tables in
`paj7620.c` need to change.
