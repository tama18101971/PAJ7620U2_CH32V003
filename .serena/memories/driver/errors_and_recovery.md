# driver/errors_and_recovery

## Codes

`PAJ_ERR_NONE 0`, `PAJ_ERR_I2C 1`, `PAJ_ERR_FIFO_OVERFLOW 2` (never returned —
kept at the APDS value for source compatibility; PAJ7620U2 has no host FIFO),
`PAJ_ERR_SENSOR_HANG 3`, `PAJ_ERR_INVALID_ID 4`, `PAJ_ERR_UNSUPPORTED 5`.
`paj_getLastI2CStatus()` returns the raw i2c.h code
(`I2C_OK 0, I2C_NACK 1, I2C_ERR_TIMEOUT 2, I2C_ERR_CLK 3, I2C_ERR_BERR 4,
I2C_ERR_ARLO 5`), written **only on a failed transaction**; success never
clears it.

## Identification without PART_ID

There is no PART_ID register. `paj_init()` instead: wake-up command → poll
bank 0 `0x00` for `PAJ_WAKEUP_DONE (0x20)`; then write the config; then
`enable_sensor()` reads `bank1 0x72` back and requires bit0. A readback of
`0xFF` at `0x73` means something that is not a PAJ7620U2 answers there →
`PAJ_ERR_INVALID_ID`, and `paj_init()` returns immediately instead of burning
all retries (retrying cannot change that answer).

Expected NACKs during the wake-up poll are **not** recorded in
`g_last_i2c_status` — otherwise a successful `paj_init()` would still report
`I2C_NACK` and corrupt the diagnostic contract.

## Re-init policy

- One failed register access is never retried inside `rd()`/`wr()`; bus-level
  recovery is the I2C library's job.
- An I2C fault in `paj_available()` or inside `paj_readGesture()` calls
  `recover_sensor()`, but only after `PAJ_REINIT_THRESHOLD` (3) **consecutive**
  failures. `g_fail_streak` counts them and any successful `poll_gesture()`
  zeroes it. A lone NACK is normal on a marginal bus (sensor busy with AE, or
  the transaction collides with the LED pulse) and re-initialising on it costs
  milliseconds of silence in which every gesture is lost — a self-inflicted
  instability. Below the threshold the fault surfaces only through
  `paj_getLastI2CStatus()`.
- `recover_sensor()` then calls `sensor_reinit()` (full wake-up + `g_init_seq` +
  axis/proximity thresholds) at most `PAJ_RETRY_LIMIT` times.
- `g_reinit_count` survives across faults and is zeroed by any successful
  read; `>= PAJ_RETRY_LIMIT` is the "sensor considered dead" signal that maps
  to `PAJ_ERR_SENSOR_HANG`. `SENSOR_HANG` is deliberately *not* masked as
  `I2C` — otherwise the public API cannot distinguish "a read failed" from
  "a read failed and recovery failed".
- Calibration: a bus fault or too few valid samples is **not** fatal, the
  configured thresholds stay; only a failed write of the computed thresholds
  returns `false` (the configuration would be incomplete).
- Calibration does **not** run in `paj_init()` by default
  (`PAJ_CALIBRATE_ON_INIT=0`). In gesture mode the sensor does not update
  `REG_PS_DATA` (0x6C), so it returns a value tracking ambient light —
  measured: one boot `median=45 sigma=0 -> 45/33`, the next kept `200/64`.
  Use `paj_setProximityMode()` + `paj_recalibrate()`.
- `paj_recalibrate()` with `PAJ_ENABLE_CALIBRATION=0` returns `false` +
  `PAJ_ERR_UNSUPPORTED` rather than pretending to have done something.
