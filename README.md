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
* Per-gesture enable mask, both compile-time (`PAJ_GESTURE_MASK`) and at
  runtime (`paj_setGestureMask()`); disabled gestures are filtered out of the
  read flags, so they cannot interfere with enabled ones.
* Proximity readout with automatic threshold calibration.
* INT → EXTI (PC3) interrupts or polling.
* Non-blocking `paj_poll()` with a 5 ms schedule, entry filter and cooldown,
  tuned for palm control (spurious Z-axis flags are ignored).
* Sensor readiness check and automatic re-initialisation after I2C faults.
* Footprint: about 2 KB of flash with LTO, ~32 bytes of static RAM (plus the
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

> ⚠️ **Do not power the module from the WCH-Link 3.3 V pin** (~100 mA). Measured
> on the bench: with VDD/VLED fed from the WCH-Link the MCU enters a POR reset
> loop as soon as the sensor starts scanning, while the same board runs for
> minutes with the sensor suspended (`pio run -e power_test`). Feed VDD/VLED from
> a separate regulator with ≥100…470 µF bulk plus 10 µF/100 nF at the module and
> a common ground; let the WCH-Link power only the MCU/logic.

## Installation

```ini
lib_deps =
    https://github.com/tama18101971/PAJ7620U2_CH32V003.git#v1.2.0
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
    /* now_ms: the application's monotonic millisecond counter (TIM2 in the example). */
    if (paj_poll(now_ms)) {
        gesture_t g = paj_takeGesture();
        if (g == GESTURE_UP)    { /* ... */ }
    }
}
```

Interrupt mode adds `paj_exti_init()` + `paj_enableInterrupt()`, but the main
loop must still call `paj_poll(now_ms)` regularly: subsequent calls complete the
20 ms directional entry filter even without a new INT edge. The example uses a
1 ms TIM2 timebase that also wakes `__WFI()`. A complete example with UART
output, both modes, trace counters and a proximity demo lives in
[examples/basic/main.c](examples/basic/main.c).

## Enabling only the gestures you need

```c
/* Compile time (build_flags), before the first init:
 *   -DPAJ_GESTURE_MASK=PAJ_GESMASK_DIRECTIONS -DPAJ_WAVE_MASK=0
 * Runtime, any time after paj_init(): */
paj_setGestureMask(PAJ_GESMASK_DIRECTIONS, false);  /* 4 directions only */
```

`PAJ_GESMASK_DIRECTIONS` (`0x0F`), `PAJ_GESMASK_SIX_AXIS` (`0x3F`) and
`PAJ_GESMASK_ALL` (`0xFF`) are prepared sets in `paj7620_config.h`; any `GES_*`
combination works. The mask has two effects: registers `0x41`/`0x42` stop the
INT pin from reacting, **and** the driver strips disabled bits from the flags it
reads back — otherwise the sensor would still report every gesture and, for
example, a spurious `Forward|Backward` pair would discard a valid `Left`.
`paj_getGestureMask()` / `paj_isWaveEnabled()` return the active mask.

The example itself is built with `-DPAJ_GESTURE_MASK=0x0F -DPAJ_WAVE_MASK=0`
(`platformio.ini`), i.e. only UP/DOWN/LEFT/RIGHT are active; the banner prints
the effective mask (`PAJ7620U2 ready. Gesture mask 0x0F, wave off`). Replace the
values in `build_flags` with `PAJ_GESMASK_ALL` / `1` to re-enable everything.

> Redefining the same macro twice (e.g. in `platformio.ini` **and** via
> `PLATFORMIO_BUILD_FLAGS`) is not a silent override: gcc reports
> `warning: "PAJ_GESTURE_MASK" redefined` and `-Werror` fails the build. Remove
> the project value with `build_unflags` / `PLATFORMIO_BUILD_UNFLAGS` — that is
> what the `all-gestures-mask` CI variant does.

## API

| Function | Purpose |
|---|---|
| `bool paj_init(void)` | I2C wake-up, datasheet configuration, enable, calibration |
| `bool paj_sleep(void)` | Passive: clear Enable, configuration retained |
| `bool paj_wakeup(void)` | I2C wake-up + Enable, gesture state reset |
| `bool paj_shutdown(void)` | Enable = 0 + suspend (IDD_SUS ≈ 15 µA) |
| `bool paj_poll(uint32_t now_ms)` | Recommended non-blocking service: scheduling, entry filter and cooldown |
| `gesture_t paj_takeGesture(void)` | Consume a ready gesture without waiting |
| `bool paj_available(void)` | Legacy immediate poll without time filtering; the gesture is cached |
| `gesture_t paj_readGesture(void)` | Legacy consume, waiting up to `PAJ_GESTURE_TIMEOUT_MS` |
| `bool paj_readProximity(uint8_t *v)` | PS data 0…255 (proximity mode required) |
| `bool paj_readStatus(uint8_t *v)` | Sensor state machine: 0 = OP, 1 = S1, 2 = S2 |
| `uint8_t paj_getLastError(void)` | `PAJ_ERR_NONE` / `I2C` / `SENSOR_HANG` / `INVALID_ID` / `UNSUPPORTED` |
| `uint8_t paj_getLastI2CStatus(void)` | Raw status of the last failed I2C transaction |
| `uint8_t paj_getReinitCount(void)` | Consecutive re-initialisations |
| `uint8_t paj_getAmbiguousCount(void)` | Flag reads with no decisive direction (enabled gestures only) |
| `bool paj_setGestureMask(uint8_t mask1, bool wave)` | Runtime gesture mask: filters flags and updates `0x41`/`0x42` |
| `uint8_t paj_getGestureMask(void)` / `bool paj_isWaveEnabled(void)` | Active mask / wave state |
| `bool paj_recalibrate(void)` | Recompute proximity thresholds from sensor data |
| `void paj_enableInterrupt(void)` / `void paj_disableInterrupt(void)` | Interrupt masks (0x41/0x42) from the active gesture mask |
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
| `PAJ_GESTURE_MASK` | `PAJ_GESMASK_ALL` (`0xFF`) | Enabled gestures (interrupt **and** software filter) |
| `PAJ_WAVE_MASK` / `PAJ_PS_MASK` | `1` / `0` | Wave and proximity interrupt masks |
| `PAJ_GESTURE_ROTATE` | `0` | Module rotation 0/90/180/270° (software remap) |
| `PAJ_REJECT_AMBIGUOUS` | `1` | `0` keeps contradictory bits instead of clearing the pairs |
| `PAJ_XDIR_THRESHOLD` | `0x0D` | Left/right threshold (`bank0 0x93`, absent from the datasheet init array) |
| `PAJ_YDIR_THRESHOLD` | `0x0A` | Up/down threshold (`bank0 0x94`, absent from the datasheet init array) |
| `PAJ_ZDIR_THRESHOLD` | `0x3C` | Push/pull threshold (`bank0 0x48`); raise to `0x48…0x55` for palm control |
| `PAJ_PROX_THRESHOLD` / `PAJ_PROX_EXIT_TH` | `200` / `64` | Proximity thresholds (datasheet defaults) |
| `PAJ_ENABLE_CALIBRATION`, `PAJ_CAL_*` | `1`, `16`, `3`, `2` | Threshold calibration |
| `PAJ_CALIBRATE_ON_INIT` | `0` | Legacy behaviour: calibrate inside `paj_init()` |
| `PAJ_POLL_INTERVAL_MS` | `5` | Flag-read period in `paj_poll()` |
| `PAJ_GESTURE_CONFIRM_MS` | `20` | Directional entry filter; `0` disables the delay |
| `PAJ_GESTURE_COOLDOWN_MS` | `230` | Event suppression after publication; `0` disables it |
| `PAJ_Z_OVERRIDES_XY` | `0` | `0` = palm (Z cannot cancel a direction), `1` = finger (Z wins) |
| `PAJ_GESTURE_TIMEOUT_MS` | `300` | Blocking `paj_readGesture()` deadline |
| `PAJ_WAKEUP_TIMEOUT_MS` | `20` | Sensor readiness timeout (T1 > 700 µs) |
| `PAJ_OWN_SYSTICK` | `1` | `0` keeps the application SysTick untouched |
| `PAJ_RETRY_LIMIT` | `6` | Init/re-init retry budget |
| `PAJ_REINIT_THRESHOLD` | `3` | Consecutive I2C faults before a full re-init |
| `PAJ_INT_MODE` | `1` | `0` = polling, `1` = interrupts at start-up |
| `PAJ_PROVIDE_EXTI_ISR` | `1` | `0` = the application owns `EXTI7_0_IRQHandler` |
| `PAJ_ENABLE_PS_MODE` | `1` | `0` removes proximity mode (~250 B of flash) |
| `PAJ_EXAMPLE_TRACE` | `0` | Diagnostic output in `examples/basic` |
| `PAJ7620U2_DEBUG` | — | `-DPAJ7620U2_DEBUG` enables the driver's printf |

## How it works

* **Gestures are recognised by the sensor.** The host reads the bit flags in
  `0x43`/`0x44` (bank 0) and decodes them; no pixel data is processed on the
  MCU. Reading those registers **clears** the flags, so the driver caches the
  decoded gesture for `paj_readGesture()`. `0x43` is read and decoded first and
  separately from `0x44`, so a failing `0x44` read costs only the wave flag and
  never an already decoded gesture. When wave is disabled and `PAJ_PS_MASK=0`
  the second register is not read at all — one transaction less per poll.
* **Disabled gestures are filtered twice.** `0x41`/`0x42` only gate the INT
  pin; the chip keeps setting every flag bit in `0x43`/`0x44`. The driver
  therefore ANDs the read bytes with the active mask (`PAJ_GESTURE_MASK` /
  `PAJ_WAVE_MASK`, runtime `paj_setGestureMask()`) before decoding. Without
  that step a disabled gesture still won the decode and could invalidate an
  enabled one.
* **Contradictory pairs are cleared, not the whole read.** The sensor sets flag
  bits independently, so one physical gesture can set several at once when the
  signal is weak (`0x43 = 0x3F` was observed on the bench). Each antipodal pair
  (`Up|Down`, `Left|Right`, `Forward|Backward`, `CW|CCW`) is dropped on its
  own and counted in `paj_getAmbiguousCount()`, while the remaining bits stay
  decodable: `Left|Forward|Backward` now yields `Left` instead of nothing.
  A rising count means a weak signal: IR emitter supply, hand distance, gesture
  speed, or unsuitable `PAJ_XDIR_THRESHOLD` / `PAJ_YDIR_THRESHOLD`.
* **Non-blocking filter.** `paj_poll(now_ms)` reads flags at most every 5 ms,
  holds a directional gesture for 20 ms of confirmation and suppresses repeats
  for 230 ms (a wide palm crosses the sensor slower than a finger). With
  `PAJ_Z_OVERRIDES_XY=0` (default) a spurious `Forward/Backward` cannot cancel
  a pending direction — a palm edge produces exactly that — and the pending
  direction is published as soon as the sensor stops reporting it.
  `PAJ_Z_OVERRIDES_XY=1` restores the finger behaviour: Z gestures are emitted
  immediately and cancel a pending direction. `paj_takeGesture()` consumes the
  single ready slot, which is never overwritten. In interrupt mode
  `paj_poll()` must still run regularly so the time window completes without
  another INT edge; taking the gesture promptly also releases the active-low
  INT line, whose flags are cleared by the same read.
* **Two register banks**, selected through `0xEF`; the driver tracks the
  current bank and avoids redundant I2C transactions.
* **Wake-up.** The datasheet requires an I2C wake-up command
  (START + slave ID + STOP), T1 > 700 µs and T2 > 400 µs. The sensor does not
  ACK until wake-up completes — the driver expects that and does not treat it
  as an error.
* **Initialisation** uses the official datasheet sequence ("i. Initial",
  45 writes: the interrupt-mask and Z-threshold entries are written separately
  from the configuration), then the gesture axis thresholds
  (`0x93`/`0x94`/`0x48`), then proximity thresholds, then `Enable`, then
  interrupt masks. Enabling is confirmed by reading bank 1 `0x72`.
* **Proximity calibration does not run in `paj_init()`**
  (`PAJ_CALIBRATE_ON_INIT=0`). In gesture mode the sensor does not update
  `0x6C` ("Only functional at proximity detection mode"): the value tracks
  ambient light, so calibrating from it is non-deterministic — on one bench
  module a boot produced `median=45, sigma=0 -> 45/33` while the next one
  discarded every sample and kept `200/64`. Use `paj_setProximityMode()` +
  `paj_recalibrate()` for a meaningful calibration.
* **Fault tolerance.** A single I2C fault inside `paj_available()` or
  `paj_readGesture()` is only recorded in `paj_getLastI2CStatus()`; the full
  re-initialisation (wake-up plus a complete register rewrite) runs after
  `PAJ_REINIT_THRESHOLD` consecutive faults, because the sensor does not answer
  during it and every gesture in that window is lost. After `PAJ_RETRY_LIMIT`
  rounds the driver reports `PAJ_ERR_SENSOR_HANG`. A fault
  during calibration is not fatal: the configured thresholds are kept.
* **Power states.** `paj_shutdown()` enters suspend (IDD_SUS ≈ 15 µA) and
  requires `paj_wakeup()` (which performs the I2C wake-up) to resume.
  `paj_sleep()` is the cheaper option: configuration is retained.

## Caveats

* The driver and the I2C library own static state and are **not reentrant** —
  never call them from an ISR.
* 7-bit address `0x73` only; no clock stretching; bus speed up to 400 kHz.
* The sensor keeps only the **latest** gesture and the driver has one ready
  slot. Slow polling or not consuming that slot can drop events; EXTI reduces
  latency but does not replace regular `paj_poll()` service.
* Proximity data is not updated in gesture mode: `paj_readProximity()` returns
  0 until `paj_setProximityMode()` is called.
* `SysTick->CNT` is **not** a time base while `Delay_Ms()/Delay_Us()` are in
  use (they reset and stop the counter). `paj_readGesture()` either runs its
  own SysTick (`PAJ_OWN_SYSTICK=1`) or uses an iteration budget (`=0`).
* Debug output costs flash: `PAJ7620U2_DEBUG` pulls in `printf`.

## If the MCU reboots

A repeated `PAJ7620U2 gesture driver init...` banner means the MCU restarted —
not a driver re-init (that path prints `SENSOR HANG ... re-init` +
`Sensor recovered.`). The example prints the RCC reset flags at boot and clears
them, so each run names its cause: `Reset: PIN POR` (power-on / brown-out),
`Reset: POR SW` (supply dip, then the debugger re-attached), `Reset: PIN`
(external NRST: button, WCH-Link, auto-reset USB-UART), `Reset: SW` (debugger
flash), `IWDG`/`WWDG` (not enabled here).

A **reset loop** (banner every ~0.3 s, `ready` line cut mid-word) is almost always
a supply problem: from `paj_init()` on, the sensor scans continuously and its IR
LED pulses 720…860 mA. Diagnose in this order:

1. Flash the diagnostic build: `pio run -e power_test -t upload` (or add
   `-DPAJ_EXAMPLE_POWER_TEST=1` to `build_flags`). The sensor is suspended
   (~15 µA) and only `alive N` is printed. Resets stop → the sensor's
   current/supply is the culprit; resets continue → MCU, NRST or the WCH-Link.
2. No rebuild needed: unplug the module's VDD/VLED — the normal build reports
   `ERROR: PAJ7620U2 not responding!` and must stay alive.
3. Check voltages: **VDD ≤ 3.6 V**, **VLED ≤ 4.2 V**. A bare module fed from 5 V
   produces exactly this reboot loop.
4. Fix the rail: separate LDO for VLED, ≥100 µF bulk plus 10 µF/100 nF at the
   module, short ground return. The WCH-Link 3.3 V pin (~100 mA) cannot feed it.

Confirmed on the bench: with the module powered from the WCH-Link 3.3 V the MCU
enters a POR loop after ~20 s of continuous gestures (bursts every 30…40 ms),
while the same board with `power_test` (sensor suspended) reported `alive` > 250
(two minutes) without a single reset.

Ruled out: the driver has no reset path; the example's `HardFault_Handler` is an
endless loop (a crash would hang, not reboot); no watchdog is started; driver
stack frames measured with `-fstack-usage` are ≤20 B (`paj_exti_init` 48 B,
`main` 36 B) against a 256 B stack. The reset report costs ~276 B of flash and
can be disabled with `-DPAJ_EXAMPLE_RESET_INFO=0`.

## Build

```bash
pio run                 # build the example
pio run -t upload       # flash via WCH-Link
pio run -t size         # firmware size
```

Reference build of `examples/basic` (LTO on): **RAM 528 B, Flash 11108 B** of
2048 B RAM / 16384 B flash. CI (`.github/workflows/ci.yml`) builds 20
configurations with `-Wextra -Werror` and enforces size ceilings.

## License

MIT — see [LICENSE](LICENSE). Sensor documentation: `docs/PAJ7620U2_DS.pdf`
(PixArt V0.8) and `docs/PAJ7620U2_datasheet.pdf` (V0.7).
