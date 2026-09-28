# port/apds_migration

Source: `E:\GitProjects\APDS-9960_CH32V003`; target: `E:\GitProjects\DriverMD6`.
Full write-up lives in `INTEGRATION_RU.md` (keep the two in sync).

## Name mapping

| APDS | PAJ |
|---|---|
| `apds_init/sleep/wakeup/shutdown/available/readGesture/readProximity/readStatus` | same verbs, `paj_` prefix |
| `apds_getLastError/getLastI2CStatus/getReinitCount/recalibrate` | same, `paj_` |
| `apds_enableInterrupt/disableInterrupt/clearInterrupt` | same, `paj_` |
| `apds_exti_init/enable/disable`, `apds_handle_exti`, `apds_clear_exti`, `g_apds_int_flag` | `paj_…`, `g_paj_int_flag` |
| `int_config.h/.c` | `paj_int_config.h/.c` (distinct file names on purpose, so having both libraries in `lib_deps` does not shadow headers) |
| `APDS_INT_MODE`, `APDS_PROVIDE_EXTI_ISR`, `APDS_OWN_SYSTICK`, `APDS_RETRY_LIMIT`, `APDS_GESTURE_TIMEOUT_MS`, `APDS_ENABLE_CALIBRATION`, `APDS_CAL_*`, `APDS_PROX_*`, `APDS9960_DEBUG` | same shape, `PAJ_` prefix |
| — | new: `PAJ_GESTURE_MASK`, `PAJ_WAVE_MASK`, `PAJ_PS_MASK`, `PAJ_GESTURE_ROTATE`, `PAJ_ENABLE_PS_MODE`, `PAJ_WAKEUP_TIMEOUT_MS` |
| `APDS_GESTURE_SENSITIVITY`, all `APDS_FIFO_*`, LED drive/gain knobs | **no analogue** — hardware gesture engine |

`gesture_t` 0..4 match; PAJ adds `FORWARD/BACKWARD/CW/CCW/WAVE` (5..9).
I2C layer is the same library (`I2C-CH32V003`), so `i2c_init(400000)` /
`i2c_deinit()` stay as they are. Bus address moves `0x39` → `0x73`.

## DriverMD6 call sites (as of the v1.3.0 APDS integration)

- `platformio.ini`: `lib_deps` APDS → PAJ, `-DAPDS_INT_MODE=1` → `-DPAJ_INT_MODE=1`,
  `-DAPDS_PROVIDE_EXTI_ISR=0` → `-DPAJ_PROVIDE_EXTI_ISR=0` (its
  `EXTI7_0_IRQHandler` owns the shared EXTI0-7 vector).
- `src/main.c`: includes (`apds9960.h` → `paj7620.h`, `int_config.h` →
  `paj_int_config.h`); `apds_ok`→`paj_ok` (project-local name);
  `apds_available/readGesture/clearInterrupt/enableInterrupt/shutdown/init`.
- `src/standby.c`: `apds_enableInterrupt/disableInterrupt/shutdown/handle_exti`;
  `include/standby.h`: `APDS_PORT/APDS_PIN` (GPIOC/GPIO_Pin_3) →
  `PAJ_INT_PORT/PAJ_INT_PIN`; `apds_int_pending` is the project's own flag —
  leave the name.
- `src/ui.c`: includes + `apds_disableInterrupt/shutdown/wakeup`.
- **Mandatory logic change** in `standby.c` resume path (~line 275): after
  `i2c_init(400000)` and before `paj_enableInterrupt()` add `paj_wakeup();`
  (see `mem:driver/power_states`). Without it the sensor is still suspended:
  `paj_enableInterrupt()` returns `PAJ_ERR_I2C` and gestures never arrive.

## Behavioural differences to communicate

- Flags are consumed by the read → `paj_available()` caches; `paj_clearInterrupt()`
  no longer loses data.
- Only the latest gesture is retained; slow polling drops gestures.
- Identification is by `0x00 == 0x20` + Enable readback, not by an ID register.
- `PAJ_ERR_FIFO_OVERFLOW` is never returned.
