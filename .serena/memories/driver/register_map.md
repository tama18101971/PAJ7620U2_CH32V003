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
bit6 CW, bit7 CCW`; wave is `0x44 bit0`. Decode picks the **lowest** set bit
for determinism.

## Tables

`g_init_seq` (43 entries) = datasheet "i. Initial" minus the 0xEF writes and
minus `{bank1, 0x72, 0x01}` (Enable is written by `enable_sensor()` so
`paj_wakeup()` can reuse it). `g_ps_seq` (25) = "iii. Change to PS mode",
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
