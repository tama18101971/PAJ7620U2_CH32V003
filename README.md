# PAJ7620U2_CH32V003

Gesture driver for the **PAJ7620U2** sensor on the **CH32V003** MCU (WCH,
RISC-V): bare-metal PlatformIO, no RTOS, no dynamic allocation, no floating
point.

The API mirrors the [APDS-9960_CH32V003](https://github.com/tama18101971/APDS-9960_CH32V003)
driver (same function names with the `paj_` prefix, same error codes, same
pinout), so moving a project from the APDS-9960 is a rename — see
[INTEGRATION_RU.md](INTEGRATION_RU.md) (Russian).

* Gestures: **up, down, left, right, forward, backward, clockwise,
  counter-clockwise, wave** (recognised by the sensor itself at 120 Hz).
* Proximity readout with automatic threshold calibration.
* INT → EXTI (PC3) interrupts or polling.
* Sensor readiness check and automatic re-initialisation after I2C faults.
* Footprint: ~1.5 KB of flash with LTO, 15 bytes of static RAM (plus the
  calibration buffer on the stack).

## Wiring

Identical to the APDS-9960 driver — the board is reused as is:

| CH32V003 | PAJ7620U2 | Notes |
|---|---|---|
| `PC1` | SDA | I2C, open-drain, 4.7 kΩ pull-up to 3.3 V |
| `PC2` | SCL | I2C, open-drain |
| `PC3` | INT | open-drain, active-low, input with pull-up |
| `PD5` | UART TX | 115200 baud (example/debug only) |
| 3.3 V | VBUS, VDD | 2.8…3.6 V |
| 3.3 V | VLED | **separate rail**: 3.0…4.2 V, IR LED peak current up to 860 mA |
| GND | GND (2 pins) | LED ground and logic ground tied together |

> **LED supply.** The sensor draws 2.8 mA in operation (including LED peaks)
> and pulses up to 720…860 mA when a gesture is detected. The VLED rail, its
> decoupling and the layout around the VLED pin must handle that current.

## Installation

```ini
lib_deps =
    https://github.com/tama18101971/PAJ7620U2_CH32V003.git#v1.0.0
```

The library pulls in `I2C-CH32V003` (pinned `#v7.1.0`). The application must
initialise the bus before the first driver call:

```c
i2c_init(400000);        /* 400 kHz is the PAJ7620U2 maximum */
paj_init();
```

## Quick start (polling)

```c
#include "i2c.h"
#include "paj7620.h"

i2c_init(400000);
if (!paj_init()) {
    /* paj_getLastError(): PAJ_ERR_SENSOR_HANG / PAJ_ERR_INVALID_ID / PAJ_ERR_I2C */
}

for (;;) {
    if (paj_available()) {
        gesture_t g = paj_readGesture();
        if (g == GESTURE_UP)    { /* ... */ }
    }
}
```

Interrupt mode adds `paj_exti_init()` + `paj_enableInterrupt()` and wakes the
main loop on `g_paj_int_flag`. A complete example with UART output, both
modes and a proximity demo lives in [examples/basic/main.c](examples/basic/main.c).

## API

| Function | Purpose |
|---|---|
| `bool paj_init(void)` | I2C wake-up, datasheet configuration, enable, calibration |
| `bool paj_sleep(void)` | Passive: clear Enable, configuration retained |
| `bool paj_wakeup(void)` | I2C wake-up + Enable, gesture state reset |
| `bool paj_shutdown(void)` | Enable = 0 + suspend (IDD_SUS ≈ 15 µA) |
| `bool paj_available(void)` | Non-blocking check; the gesture is cached |
| `gesture_t paj_readGesture(void)` | Consume the gesture, waiting up to `PAJ_GESTURE_TIMEOUT_MS` |
| `bool paj_readProximity(uint8_t *v)` | PS data 0…255 (proximity mode required) |
| `bool paj_readStatus(uint8_t *v)` | Sensor state machine: 0 = OP, 1 = S1, 2 = S2 |
| `uint8_t paj_getLastError(void)` | `PAJ_ERR_NONE` / `I2C` / `SENSOR_HANG` / `INVALID_ID` / `UNSUPPORTED` |
| `uint8_t paj_getLastI2CStatus(void)` | Raw status of the last failed I2C transaction |
| `uint8_t paj_getReinitCount(void)` | Consecutive re-initialisations |
| `bool paj_recalibrate(void)` | Recompute proximity thresholds from sensor data |
| `void paj_enableInterrupt(void)` / `void paj_disableInterrupt(void)` | Interrupt masks (0x41/0x42) |
| `void paj_clearInterrupt(void)` | Release INT (reads the flag registers) |
| `bool paj_setProximityMode(void)` / `bool paj_setGestureMode(void)` | Mode switch (`PAJ_ENABLE_PS_MODE=1`) |

`gesture_t`: `GESTURE_NONE`, `LEFT`, `RIGHT`, `UP`, `DOWN` (values 0…4, same
as the APDS driver), then `FORWARD`, `BACKWARD`, `CW`, `CCW`, `WAVE`.

## Configuration

Every tunable lives in [src/paj7620_config.h](src/paj7620_config.h) behind
`#ifndef` defaults with `#error` range checks. Override them with
`build_flags` (not with `#define` in your own `.c` — the driver is compiled
separately).

| Macro | Default | Purpose |
|---|---|---|
| `PAJ_GESTURE_MASK` | `0xFF` | Which of the 8 gestures raise an interrupt |
| `PAJ_WAVE_MASK` / `PAJ_PS_MASK` | `1` / `0` | Wave and proximity interrupt masks |
| `PAJ_GESTURE_ROTATE` | `0` | Module rotation 0/90/180/270° (software remap) |
| `PAJ_PROX_THRESHOLD` / `PAJ_PROX_EXIT_TH` | `200` / `64` | Proximity thresholds (datasheet defaults) |
| `PAJ_ENABLE_CALIBRATION`, `PAJ_CAL_*` | `1`, `16`, `3`, `2` | Threshold calibration |
| `PAJ_GESTURE_TIMEOUT_MS` | `300` | Blocking `paj_readGesture()` deadline |
| `PAJ_WAKEUP_TIMEOUT_MS` | `20` | Sensor readiness timeout (T1 > 700 µs) |
| `PAJ_OWN_SYSTICK` | `1` | `0` keeps the application SysTick untouched |
| `PAJ_RETRY_LIMIT` | `6` | Init/re-init retry budget |
| `PAJ_INT_MODE` | `1` | `0` = polling, `1` = interrupts at start-up |
| `PAJ_PROVIDE_EXTI_ISR` | `1` | `0` = the application owns `EXTI7_0_IRQHandler` |
| `PAJ_ENABLE_PS_MODE` | `1` | `0` removes proximity mode (~250 B of flash) |
| `PAJ7620U2_DEBUG` | — | `-DPAJ7620U2_DEBUG` enables the driver's printf |

## How it works

* **Gestures are recognised by the sensor.** The host reads the bit flags in
  `0x43`/`0x44` (bank 0) and decodes them; no pixel data is processed on the
  MCU. Reading those registers **clears** the flags, so the driver caches the
  decoded gesture for `paj_readGesture()`.
* **Two register banks**, selected through `0xEF`; the driver tracks the
  current bank and avoids redundant I2C transactions.
* **Wake-up.** The datasheet requires an I2C wake-up command
  (START + slave ID + STOP), T1 > 700 µs and T2 > 400 µs. The sensor does not
  ACK until wake-up completes — the driver expects that and does not treat it
  as an error.
* **Initialisation** uses the official datasheet sequence ("i. Initial",
  43 writes), then proximity thresholds, then `Enable`, then interrupt masks.
  Enabling is confirmed by reading bank 1 `0x72`.
* **Fault tolerance.** An I2C fault inside `paj_available()` or
  `paj_readGesture()` triggers a full re-initialisation (up to
  `PAJ_RETRY_LIMIT` times) and finally reports `PAJ_ERR_SENSOR_HANG`. A fault
  during calibration is not fatal: the configured thresholds are kept.
* **Power states.** `paj_shutdown()` enters suspend (IDD_SUS ≈ 15 µA) and
  requires `paj_wakeup()` (which performs the I2C wake-up) to resume.
  `paj_sleep()` is the cheaper option: configuration is retained.

## Caveats

* The driver and the I2C library own static state and are **not reentrant** —
  never call them from an ISR.
* 7-bit address `0x73` only; no clock stretching; bus speed up to 400 kHz.
* The sensor keeps only the **latest** gesture, so slow polling can drop
  gestures (an INT-driven read does not lose them).
* Proximity data is not updated in gesture mode: `paj_readProximity()` returns
  0 until `paj_setProximityMode()` is called.
* `SysTick->CNT` is **not** a time base while `Delay_Ms()/Delay_Us()` are in
  use (they reset and stop the counter). `paj_readGesture()` either runs its
  own SysTick (`PAJ_OWN_SYSTICK=1`) or uses an iteration budget (`=0`).
* Debug output costs flash: `PAJ7620U2_DEBUG` pulls in `printf`.

## Build

```bash
pio run                 # build the example
pio run -t upload       # flash via WCH-Link
pio run -t size         # firmware size
```

Reference build of `examples/basic` (LTO on): **RAM 508 B, Flash 10792 B** of
2048 B RAM / 16384 B flash. CI (`.github/workflows/ci.yml`) builds nine
configurations with `-Wextra -Werror` and enforces size ceilings.

## License

MIT — see [LICENSE](LICENSE). Sensor documentation: `docs/PAJ7620U2_DS.pdf`
(PixArt V0.8) and `docs/PAJ7620U2_datasheet.pdf` (V0.7).
