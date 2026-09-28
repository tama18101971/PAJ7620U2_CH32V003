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

CI (`.github/workflows/ci.yml`) builds nine config variants with
`-Wextra -Werror` on project sources (`PLATFORMIO_BUILD_SRC_FLAGS`, so SDK
and library sources are not affected) plus a size-ceiling guard
(RAM ≤ 600 B, flash ≤ 13000 B for the default build).

Variants: default, polling, no-calibration, debug-output, external-exti-isr,
rotated-module, proximity-demo, no-ps-mode, no-own-systick.

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
| `examples/basic/main.c` | Example usage (polling + interrupt + optional proximity demo) |
| `platformio.ini` | Build config, library deps (`I2C-CH32V003`, pinned `#v7.1.0`), LTO on |
| `library.json` | PlatformIO library manifest (v1.0.0, excludes `examples/`) |
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
- **Reading 0x43/0x44 clears the flags.** Therefore `paj_available()` decodes
  the gesture into a cache (`g_gesture`/`g_has_gesture`) and
  `paj_readGesture()` returns the cached value first. Any new code that reads
  the flag registers must go through `poll_gesture()`, or it will destroy a
  pending gesture.
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
- **Power states differ from the APDS driver.** `paj_shutdown()` performs a
  real suspend (Enable = 0 + write 0x01 to `0x03`), so `paj_wakeup()` must run
  the I2C wake-up command. `paj_sleep()` only clears Enable and keeps the
  configuration. Applications that sleep and resume MUST call `paj_wakeup()`
  in the resume path — this is the single most common porting mistake
  (see INTEGRATION_RU.md §4).
- **Sensor readiness**: after power-on (T1 > 700 µs) and after the wake-up
  command, the sensor does not ACK until it is ready. `wait_ready()` polls
  bank 0 register `0x00` for `0x20` and ignores expected NACKs; a readback of
  `0xFF` means "something else answers at 0x73" → `PAJ_ERR_INVALID_ID`
  (terminal, `paj_init()` does not retry it).
- **Calibration** runs in `paj_init()` → `calibrate_proximity()`: samples
  `REG_PS_DATA` (0x6C), takes the median + N·σ and stores the thresholds for
  later `sensor_reinit()` calls. The PS block is only updated in proximity
  mode, so in gesture mode the samples are zero and the configured defaults
  stay in place — that is the expected default behaviour, not a bug.
  Disable with `PAJ_ENABLE_CALIBRATION=0` (`paj_recalibrate()` then returns
  `PAJ_ERR_UNSUPPORTED`).
- **I2C error policy**: a single failed register access is not retried inside
  `rd()`/`wr()`; bus-level recovery is the I2C library's job. Sensor-level
  recovery (`recover_sensor()` → `sensor_reinit()`) is attempted from
  `paj_available()` and `paj_readGesture()` up to `PAJ_RETRY_LIMIT` times,
  then reported as `PAJ_ERR_SENSOR_HANG` (deliberately not masked as
  `PAJ_ERR_I2C`).
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
  SysTick when `PAJ_OWN_SYSTICK=1` and never calls `Delay_*` inside the loop.
- The PAJ7620U2 IR LED draws 720…860 mA pulses; VLED needs its own rail and
  local decoupling. Marginal sensor modules fail to detect fast gestures.
- The sensor keeps only the LATEST gesture: polling slower than the gesture
  rate drops gestures. The INT-driven path does not.
- `PAJ_ERR_FIFO_OVERFLOW` (value 2) is kept for source compatibility with the
  APDS driver but is never returned — there is no host FIFO.
- `RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE)` must run before
  `GPIO_EXTILineConfig()`; otherwise the EXTI mapping is silently ignored.
- The EXTI ISR in `paj_int_config.c` is intentionally **strong**: the
  NoneOS-SDK weak fallback handler is an infinite loop.
- Reference sizes (examples/basic, LTO on): RAM 508 B, Flash 10792 B. The
  driver itself is ~1.5 KB of flash and 15 bytes of static RAM.

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
