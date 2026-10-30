# AGENTS.md

## Project

PAJ7620U2 gesture driver for the CH32V003 microcontroller (WCH, RISC-V).
Detects 9 gestures: UP, DOWN, LEFT, RIGHT, FORWARD, BACKWARD, CW, CCW, WAVE.
Bare-metal PlatformIO project, NoneOS-SDK framework, no RTOS.

The driver replaces the APDS-9960 driver (`APDS-9960_CH32V003`) on the same
footprint: the pinout, the API shape and the error codes are identical by
design, so porting a project is a rename plus one behavioural fix (the
PAJ7620U2 suspend state requires `paj_wakeup()`).

## Build & Upload

```bash
pio run            # build
pio run -t upload  # flash via WCH-Link
pio run -t size    # size report
```

CI (`.github/workflows/ci.yml`) builds 20 config variants with
`-Wextra -Werror` on project sources (`PLATFORMIO_BUILD_SRC_FLAGS`, so SDK
and library sources are not affected) plus a size-ceiling guard
(RAM ≤ 600 B, flash ≤ 13000 B for the default build).

Variants: default, polling, no-calibration, debug-output, external-exti-isr,
rotated-module, proximity-demo, gesture-trace, no-ps-mode, no-own-systick,
reinit-threshold-1, calibrate-on-init, axis-thresholds-tuned, accept-ambiguous,
all-gestures-mask, z-axis-priority, no-confirm-window, ps-mask, no-reset-info,
power-test.

The example build pins the gesture mask to 4 directions in `platformio.ini`
(`-DPAJ_GESTURE_MASK=0x0F -DPAJ_WAVE_MASK=0`); the `all-gestures-mask` variant
cancels those with `PLATFORMIO_BUILD_UNFLAGS` before setting its own values —
a second `-D` of the same macro is a gcc `redefined` warning and `-Werror`
turns it into a build failure.

No test framework configured. Manual testing via UART output (PD5, 115200 baud).

## Platform Constraints

- **RAM:** 2048 bytes, **Flash:** 16384 bytes
- **No float** — integer-only arithmetic
- **No dynamic allocation** — no malloc/free
- **Framework:** NoneOS-SDK (NOT Arduino, NOT FreeRTOS)
- **Toolchain:** ch32v PlatformIO platform, `ch32v003f4p6_evt_r0` board
- **Stack:** linker script reserves 256 bytes. The driver's peak frame is
  `calibrate_proximity()` (16-byte buffer) plus the I2C library frames —
  keep new automatics small.
- The driver is **not reentrant**: static state, one execution context,
  never call it from an ISR.

## Key Files

| File | Purpose |
|------|---------|
| `src/paj7620.h` | Public API + `PAJ_ERR_*` codes |
| `src/paj7620_config.h` | ALL tunables with `#ifndef` defaults + `#error` range validation |
| `src/paj7620.c` | Driver implementation (register tables, wake-up, gestures, calibration) |
| `src/paj7620_regs.h` | PAJ7620U2 register map (bank 0/1, flag bits) |
| `src/paj_int_config.h` / `src/paj_int_config.c` | EXTI setup (INT → PC3) |
| `examples/basic/main.c` | Non-blocking polling/interrupt example, TIM2 ms timebase, trace and proximity demo (prints the effective gesture mask) |
| `platformio.ini` | Build config, project gesture mask (4 directions), library deps (`I2C-CH32V003`, pinned `#v7.1.0`), LTO on, `default_envs`, extra `power_test` env (sensor suspended + heartbeat) |
| `library.json` | PlatformIO library manifest (v1.2.0, excludes `examples/`) |
| `INTEGRATION_RU.md` | APDS-9960 → PAJ7620U2 migration, incl. DriverMD6 |
| `LICENSE` | MIT license |
| `.clangd` | Removes the RISC-V target flags from the compile DB for native clang/clangd |
| `compile_commands.json` | Generated (`pio run -t compiledb`), git-ignored — feeds clangd/cpptools |
| **Library:** `I2C-CH32V003` | I2C driver (via `lib_deps`, pinned `#v7.1.0`) |

## Tooling: language server (clangd) and symbol tools

C projects need an LSP backend; without one the symbol tools
(`serena_find_symbol`, `serena_get_symbols_overview`, diagnostics) fail with
"Active language servers: []" and only `search_for_pattern`/Grep work.

- `.serena/project.yml` sets `language_servers: [cpp]` (Serena ships its own
  pinned clangd in `~/.serena/language_servers/static/…`, nothing to install).
  `.serena/memories/` holds the project knowledge graph — root is `core`.
- `compile_commands.json` at the project root is required: refresh it with
  `pio run -t compiledb` after changing `platformio.ini` (it is git-ignored,
  as are `.cache/clangd/` and `.serena/{cache,project.local.yml}`).
- `.clangd` is **mandatory**: the compile DB contains `-march=rv32ecxw
  -mabi=ilp32e -msmall-data-limit=0 -msave-restore -flto`, which native clang
  rejects with "unknown target CPU 'rv32ecxw'" and then returns an invalid AST
  for every file. The `CompileFlags.Remove` list there fixes it; the real build
  is unaffected.
- A change to `language_servers` (or a fresh `.serena` config) only takes effect
  after the Serena agent is re-created: switch to another project and back, or
  reload the IDE window / start a new session.
- Quick self-check: `serena project health-check` (run it with
  `PYTHONIOENCODING=utf-8`, otherwise the final "✅" line raises a cp1251
  `UnicodeEncodeError` on a Russian console — the check itself already passed).
- VS Code: PlatformIO IDE does not create `.vscode/` until the folder is opened
  as a project; `.vscode/tasks.json` provides `PIO: Build/Upload/Monitor` that
  work without the extension.

## Architecture Notes

- **Gesture detection is hardware-driven.** The PAJ7620U2 gesture engine runs
  at 120 Hz (Normal mode) and sets bit flags in `REG_INT_FLAG1` (0x43) /
  `REG_INT_FLAG2` (0x44). The driver only decodes flags — there is no FIFO,
  no pixel data, and `APDS_FIFO_*` / `APDS_GESTURE_SENSITIVITY` analogues do
  not exist.
- **Reading 0x43/0x44 clears the flags.** `read_gesture_flags()` performs a
  burst read of both registers where the I2C library supports it and falls back
  to two reads in `I2C_LITE` mode; when wave is disabled (`g_enabled_wave == 0`)
  and `PAJ_PS_MASK == 0` it reads only 0x43 — one transaction less per poll.
  The gesture is cached in `g_gesture`/`g_has_gesture`; an unread result is
  never overwritten. Any new code that reads the flag registers must go through
  this path or it will destroy a pending gesture.
- **The gesture mask is applied in software, not just to INT.** 0x41/0x42 only
  gate the INT pin; the chip keeps setting every bit in 0x43/0x44.
  `read_gesture_flags()` therefore ANDs the read bytes with
  `g_enabled_mask1`/`g_enabled_wave` (from `PAJ_GESTURE_MASK`/`PAJ_WAVE_MASK`,
  changed at runtime by `paj_setGestureMask()`) **before** decoding and before
  the contradiction check. Without that, a disabled gesture (Forward/Backward,
  CW/CCW) still won `decode_gesture()` and could discard an enabled direction.
  `g_int_masks_on` remembers whether 0x41/0x42 currently hold real masks, so
  changing the mask never re-enables interrupts that `paj_disableInterrupt()`
  (or a polling build) turned off. `paj_setGestureMask()` writes the hardware
  masks **before** committing the new values to `g_enabled_mask1`/`g_enabled_wave`:
  a failed write leaves sensor and driver consistent on the old mask (no
  half-applied state, no `gesture_reset()` on the failure path).
- **`paj_poll(now_ms)` is the recommended non-blocking path.** It reads flags
  at most every `PAJ_POLL_INTERVAL_MS` (5 ms), holds directional gestures for
  `PAJ_GESTURE_CONFIRM_MS` (20 ms), and applies `PAJ_GESTURE_COOLDOWN_MS`
  (230 ms — a wide palm crosses the sensor slower than a finger). With
  `PAJ_Z_OVERRIDES_XY=0` (default, palm control) a Forward/Backward flag does
  not cancel a pending direction and the pending direction is published as soon
  as the sensor stops reporting it; with `=1` (finger) Z is emitted immediately
  and cancels a pending direction. `paj_takeGesture()` consumes the one ready
  slot. `paj_available()`/`paj_readGesture()` remain as legacy
  immediate/blocking APIs. Every EXTI design must still call `paj_poll()`
  regularly so a directional window completes without a new edge, and must take
  the gesture promptly: while a ready gesture is unread no flag read happens,
  so the active-low INT stays asserted.
- **Two register banks** (selected by 0xEF). `bank_sel()` caches the current
  bank in `g_bank` to avoid an extra I2C transaction per access;
  `PAJ_BANK_UNKNOWN` forces re-selection (set after wake-up/suspend).
  Every `rd()`/`wr()` call takes an explicit bank argument — do not "simplify"
  it by dropping the parameter.
- **Register tables are datasheet sequences** (`g_init_seq`, `g_ps_seq`,
  `g_gesture_seq`), stored as `{bank, reg, value}` triples with the 0xEF
  bank-select writes removed (handled by `bank_sel()`) and the `Enable` write
  (`bank1 0x72 = 0x01`) removed because `paj_wakeup()` and
  `enable_sensor()` need it separately.
- **The gesture axis thresholds are NOT in the datasheet init array.** Bank 0
  `0x93` (`R_XDirectionThd`, left/right) and `0x94` (`R_YDirectionThd`,
  up/down) are documented in the "Register Map" but absent from the
  "i. Initial" write array, so they keep their power-on value and were
  untunable. Measured on the bench module: `0x93=0x0D`, `0x94=0x0A` — those are
  now `PAJ_XDIR_THRESHOLD` / `PAJ_YDIR_THRESHOLD`. Bank 0 `0x48`
  (`R_ZDirectionThd`, push/pull) *is* in the init array as a fixed `0x3C`, so it
  was equally untunable; it is now `PAJ_ZDIR_THRESHOLD` (raise to `0x48..0x55`
  when a wide palm produces false Forward/Backward). All three are written by
  `apply_axis_thresholds()` (X/Y range 0..0x1F, validated by `#error`) after
  every table, and the 0x48 entries are stripped from `g_init_seq` /
  `g_gesture_seq` so the config value always wins — including after
  `paj_setGestureMode()`, which would otherwise restore the PS-mode `0x20`.
  Every other gesture threshold (`0x90`, `0x95`..`0x97`, `0x9A`, `0x9C`, `0x9F`,
  `0xCF`, `0xD0`) is in the init array.
- **Ambiguous flags are cleared per pair, not per read.** The sensor sets flag
  bits independently, so a weak signal can make one gesture produce several
  bits — `0x43 = 0x3F` (up+down+left+right+forward+backward) was observed on
  the bench. `contradictory_mask()` finds every antipodal pair that is fully
  set and, with `PAJ_REJECT_AMBIGUOUS=1` (default), only those bits are
  cleared: `Left|Forward|Backward` now decodes as `Left` instead of being
  discarded entirely, and the check runs on the already-masked flags so a
  disabled pair cannot spoil an enabled gesture. Counted in `g_ambiguous`,
  read via `paj_getAmbiguousCount()`. Rising counts point at hardware/signal
  quality (IR LED rail, hand distance, gesture speed), not at the driver.
- **Power states differ from the APDS driver.** `paj_shutdown()` performs a
  real suspend (Enable = 0 + write 0x01 to `0x03`), so `paj_wakeup()` must run
  the I2C wake-up command. `paj_sleep()` only clears Enable and keeps the
  configuration. Applications that sleep and resume MUST call `paj_wakeup()`
  in the resume path — this is the single most common porting mistake
  (see INTEGRATION_RU.md §4).
- **Sensor readiness**: after power-on (T1 > 700 µs) and after the wake-up
  command, the sensor does not ACK until it is ready. `wait_ready(budget)`
  polls bank 0 register `0x00` for `0x20` and ignores expected NACKs; a
  readback of `0xFF` means "something else answers at 0x73" →
  `PAJ_ERR_INVALID_ID` (terminal, `paj_init()` does not retry it). The budget
  counts nominal 1 ms iterations, but each iteration holds an I2C transaction
  that can run up to `I2C_TIMEOUT_MS` (40 ms) on a dead bus: `paj_init()` and
  `paj_wakeup()` pass the full `PAJ_WAKEUP_TIMEOUT_MS` (20), the fault-recovery
  path (`sensor_reinit()`, called from the nominally non-blocking
  `paj_available()`/`paj_readGesture()`) passes the reduced
  `PAJ_REINIT_WAKEUP_MS` (5) — worst fault-path call ≈ 0.4 s, not ≈ 1 s.
- **Calibration does NOT run in `paj_init()`** (`PAJ_CALIBRATE_ON_INIT=0`,
  default). In gesture mode the sensor does not update `REG_PS_DATA` (0x6C),
  so the register returns a value that tracks ambient light rather than the
  proximity signal. Measured: one boot produced `median=45 sigma=0 -> 45/33`,
  the next discarded every sample and kept `200/64`. Calibrate explicitly with
  `paj_setProximityMode()` + `paj_recalibrate()`; set `PAJ_CALIBRATE_ON_INIT=1`
  for the old behaviour. `PAJ_ENABLE_CALIBRATION=0` compiles
  `paj_recalibrate()` out (it then returns `PAJ_ERR_UNSUPPORTED`).
- **`poll_gesture()` reads `0x43` before `0x44` and decodes `0x43` separately.**
  A failing `0x44` read must not discard a gesture that has already been read
  out of the sensor — the flags are cleared on read and cannot be re-read. It
  costs only the wave flag; `paj_available()` and `paj_readGesture()` still
  hand the cached gesture to the caller and report `PAJ_ERR_I2C`.
- **I2C error policy**: a single failed register access is not retried inside
  `rd()`/`wr()`; bus-level recovery is the I2C library's job. Sensor-level
  recovery (`recover_sensor()` → `sensor_reinit()`) is attempted from
  `paj_available()` and `paj_readGesture()` up to `PAJ_RETRY_LIMIT` times,
  then reported as `PAJ_ERR_SENSOR_HANG` (deliberately not masked as
  `PAJ_ERR_I2C`).
- **A full re-init needs `PAJ_REINIT_THRESHOLD` consecutive failures** (3), not
  one. `g_fail_streak` counts failures and is cleared by any successful
  `poll_gesture()`. Rationale: a lone NACK is normal on a marginal bus (the
  sensor is busy with AE, or the transaction collides with the LED pulse);
  re-initialising on it costs milliseconds during which the sensor is silent
  and every gesture is lost — a self-inflicted instability. Below the
  threshold the fault is only visible through `paj_getLastI2CStatus()`. The
  re-init's readiness wait uses the reduced `PAJ_REINIT_WAKEUP_MS` budget (see
  "Sensor readiness" above), so the recovery path cannot stall the caller for
  the full wake-up timeout.
- **Void-API interrupt functions** (`paj_enableInterrupt`,
  `paj_disableInterrupt`, `paj_clearInterrupt`) record `PAJ_ERR_I2C` on
  failure and never overwrite `g_last_error` on success.
- **Gesture rotation** is a compile-time remap (`PAJ_GESTURE_ROTATE`,
  0..3). At 0 the macros are identity and no code is emitted.
- **`PAJ_ENABLE_PS_MODE=0`** removes `paj_setProximityMode()` /
  `paj_setGestureMode()` and their tables (~250 bytes of flash). The example
  only calls them with `-DPAJ_EXAMPLE_PS_MODE=1`, which is why the
  "no-ps-mode" and "proximity-demo" CI variants exist.

## Gotchas

- `SysTick->CNT` counts UP on CH32V003, and NoneOS-SDK `Delay_Ms()`/`Delay_Us()`
  zero CNT and stop the counter after each call — SysTick is NOT a free-running
  timebase while `Delay_*` is in use. `paj_readGesture()` starts its own
  SysTick when `PAJ_OWN_SYSTICK=1` and never calls `Delay_*` inside the loop;
  `timebase_stop()` (STE=0) runs on every exit path, so the free-running
  counter never survives the call (a stale CMP=0xFFFFFFFF would otherwise
  fire an unexpected interrupt ~24 min later if the application had STIE set).
- The PAJ7620U2 IR LED draws 720…860 mA pulses; VLED needs its own rail and
  local decoupling. Marginal sensor modules fail to detect fast gestures.
- The sensor keeps only the LATEST gesture: polling slower than the gesture
  rate drops gestures. The INT-driven path does not.
- **The sensor names gestures relative to ITSELF.** Which way is "sensor UP"
  depends on how the module is mounted (datasheet Figure 4), so until the
  mounting is known, a physically correct swipe can be reported as a different
  direction. `PAJ_GESTURE_ROTATE` (0..3) is the software fix; determine it by
  comparing one physical movement against the module's own axes.
- `PAJ_ERR_FIFO_OVERFLOW` (value 2) is kept for source compatibility with the
  APDS driver but is never returned — there is no host FIFO.
- `RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE)` must run before
  `GPIO_EXTILineConfig()`; otherwise the EXTI mapping is silently ignored.
- The EXTI ISR in `paj_int_config.c` is intentionally **strong**: the
  NoneOS-SDK weak fallback handler is an infinite loop. Defining another
  `EXTI7_0_IRQHandler` in the application is a duplicate-symbol link error —
  use `paj_exti_callback()` or build with `PAJ_PROVIDE_EXTI_ISR=0`. The
  strong handler snapshots `EXTI->INTFR & 0xFF` on entry and clears those
  pending bits after the callback returns, so an unhandled foreign EXTI line
  0..7 cannot loop the shared vector forever (that event is lost, the MCU is
  not); a bit raised *during* the callback is outside the snapshot and
  re-fires normally. `paj_exti_callback()` must fit within one I2C byte time
  (~23 µs at 400 kHz — the I2C library's `I2C_ATOMIC_CRITICAL=0` contract);
  long work goes behind a flag into the main loop.
- Reference sizes (examples/basic, LTO on): RAM 528 B, Flash 11108 B. The
  driver itself is about 2 KB of flash and ~35 bytes of static RAM.
- The example is built with `-DPAJ_GESTURE_MASK=0x0F -DPAJ_WAVE_MASK=0`
  (4 directions only). Do not add the same `-D` again through
  `PLATFORMIO_BUILD_FLAGS`: gcc warns `redefined` and `-Werror` fails. Use
  `PLATFORMIO_BUILD_UNFLAGS` to cancel the project value first (as the
  `all-gestures-mask` CI variant does).
- `PAJ_EXAMPLE_TRACE=1` in `examples/basic` is the bring-up harness: it prints
  the decoded gesture, `REG_STATE` (0x45), the ambiguous-read counter and the
  raw 0x93..0xCF register values after `paj_init()`. It is what identified the
  `0x3F` ambiguity and the untouched `0x93`/`0x94` on the bench.
- **A reprinted `PAJ7620U2 gesture driver init...` banner means an MCU reset**,
  not a driver re-init (that path prints `SENSOR HANG ... re-init`). The example
  prints the RCC reset flags (`Reset: PIN POR ...`, ~276 B of flash,
  `PAJ_EXAMPLE_RESET_INFO=0` removes it) and clears them. A brown-out from the
  720…860 mA IR-LED pulses on a shared/weak rail is the usual cause; HardFault
  would hang (`HardFault_Handler` is an endless loop), no watchdog is started,
  and measured stack frames are tiny (`-fstack-usage`: ≤20 B in the driver,
  `paj_exti_init` 48 B, `main` 36 B vs the 256 B linker stack). For a reset
  **loop** build with `PAJ_EXAMPLE_POWER_TEST=1` (`pio run -e power_test
  -t upload`): the example suspends the sensor (LED off, ~15 µA) and prints a
  heartbeat only — if the reboots stop, the sensor's supply/LED current is the
  fault, not the MCU. Confirmed on the bench: powering the module from the
  WCH-Link 3.3 V pin (~100 mA) gives a POR loop after ~20 s of gestures, while
  `power_test` ran 250+ heartbeats reset-free on the same board. The driver has
  no lever to lower the LED current — this must be fixed in hardware.

## Datasheet sources

`docs/PAJ7620U2_DS.pdf` (V0.8, 2014-06-11) is the primary reference for the
register map and the "Programing Sequence and Function Application" section
with the initial / PS-mode / gesture-mode / suspend / resume register arrays
used verbatim in `paj7620.c`. `docs/PAJ7620U2_datasheet.pdf` (V0.7) is the
older revision of the same document.

Note: there is a widely circulated *community* register map for "PAJ7620"
(PART_ID 0xB1 at 0x00, gesture code 1..6 at 0x43). It does **not** match the
official PixArt documents in `docs/`: there 0x43 is a bit flag register and
0x45 is the state indicator. This driver follows the official map.
