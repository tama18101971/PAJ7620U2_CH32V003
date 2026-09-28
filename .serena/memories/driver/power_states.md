# driver/power_states

| Call | Sensor effect | Cost | Resume requirement |
|---|---|---|---|
| `paj_sleep()` | `bank1 0x72 = 0x00` only | engine + IR LED off, registers kept | `paj_wakeup()` (single Enable write) |
| `paj_shutdown()` | `0x72 = 0x00` **+** `bank0 0x03 = 0x01` | IDD_SUS ≈ 15 µA | `paj_wakeup()` with the I2C wake-up command |
| `paj_wakeup()` | wake-up command → poll `0x00` for `0x20` → `0x72 = 0x01` → masks | ~2 ms | — |

- **Datasheet wake-up**: "Write slave ID or I2C read command to process I2C
  wake-up", T1 > 700 µs after power-on, T2 > 400 µs after the command.
  Implemented as `i2c_start()` + `i2c_send_addr(0x73, I2C_DIR_TX)` +
  `i2c_stop()`; the ACK of that transaction is *not* trusted (the sensor may
  NACK until it is awake).
- The suspend state is the whole porting trap: APDS-9960's `apds_shutdown()`
  only cleared PON with the configuration intact, so resume needed nothing.
  After `paj_shutdown()` the sensor is deaf until `paj_wakeup()`.
- `paj_wakeup()`/`paj_shutdown()` reset the gesture cache and, on wake,
  `g_reinit_count` — a gesture from before the sleep cycle must not be
  reported as a fresh one.
- `paj_shutdown()` invalidates `g_bank` (bank is not retained across suspend).
- Retained registers mean wake-up does **not** re-run `g_init_seq`; only
  Enable and the masks are rewritten. Thresholds are re-applied by
  `sensor_reinit()`, not by `paj_wakeup()`.
- Re-init and calibration are also reachable at runtime:
  `paj_recalibrate()` and mode switches are in `mem:driver/errors_and_recovery`
  and `mem:hardware/paj7620u2`.
