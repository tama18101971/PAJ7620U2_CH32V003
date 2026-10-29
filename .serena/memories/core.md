# PAJ7620U2_CH32V003 — core

Bare-metal gesture driver for PAJ7620U2 on WCH CH32V003 (RISC-V, NoneOS-SDK,
PlatformIO). Replaces the APDS-9960 driver `E:\GitProjects\APDS-9960_CH32V003`
on the same pinout/API. Target of migration: `E:\GitProjects\DriverMD6`.

## Source map

| Path | Role |
|---|---|
| `src/paj7620.h` | public API, `gesture_t`, `PAJ_ERR_*` (mirrors `apds9960.h` 1:1) |
| `src/paj7620.c` | driver: I2C wrappers, register tables, wake-up, gesture decode, calibration, public API |
| `src/paj7620_config.h` | every tunable, `#ifndef` defaults + `#error` range checks |
| `src/paj7620_regs.h` | register map (bank 0/1), flag bits — no logic |
| `src/paj_int_config.h/.c` | EXTI3 on PC3, `g_paj_int_flag` |
| `examples/basic/main.c` | example: polling + interrupt modes, optional PS demo |
| `docs/PAJ7620U2_DS.pdf` | **authoritative** datasheet (V0.8, 2014-06-11) |

## Non-negotiable invariants

- Sensor recognizes gestures in hardware at 120 Hz; the MCU only decodes
  flag bits. No pixel data, no host FIFO, no `*_FIFO_*`/`*_SENSITIVITY`
  config knobs.
- **Reading 0x43/0x44 clears the flags.** All flag reads go through
  `poll_gesture()`, which caches the decoded gesture for `paj_readGesture()`.
  Any new direct register read of 0x43/0x44 destroys a pending gesture.
- Two register banks selected via 0xEF. `rd()`/`wr()` take an explicit bank
  argument; `bank_sel()` caches the selection in `g_bank`
  (`PAJ_BANK_UNKNOWN` forces re-selection after wake-up/suspend).
- `paj_shutdown()` = real suspend; **`paj_wakeup()` must follow it in every
  resume path** — the single most common porting mistake.
- No float, no malloc, no dynamic allocation, no ISR context for the driver.
- Driver + I2C library hold static state: not reentrant.

## Memories

- Build/verification: `mem:tech_stack`, `mem:suggested_commands`, `mem:task_completion`
- Code style: `mem:conventions`
- Driver internals: `mem:driver/register_map` (banks, datasheet tables, community-map
  conflict), `mem:driver/gesture_semantics` (flag consumption, decode order,
  rotation), `mem:driver/power_states` (sleep/shutdown/wakeup),
  `mem:driver/errors_and_recovery` (codes, terminal vs retriable, re-init policy)
- Sensor facts from the datasheet (LED current, timing, modes):
  `mem:hardware/paj7620u2`
- Migration from APDS-9960 / DriverMD6 call sites: `mem:port/apds_migration`
- Editor/tooling quirks (VS Code, serena, no C language server):
  `mem:tooling/editor_setup`

## Reference state

GitHub `tama18101971/PAJ7620U2_CH32V003` (public, `main`), v1.2.0, MIT.
CI: 20 build variants with `-Wextra -Werror` + size guard.
Measured footprint (`examples/basic`, LTO on, project mask 4 directions):
RAM 528 B, Flash 11108 B (driver alone: ~1.4–1.6 KB of `paj_*`/table symbols +
bss; small helpers are partly inlined into their callers by LTO).

Gesture mask is set at build time (`PAJ_GESTURE_MASK`/`PAJ_WAVE_MASK` — the
example's `platformio.ini` pins 4 directions, `PAJ_WAVE_MASK=0`) and at runtime
(`paj_setGestureMask()`), and is enforced in software on the read flags
(see `mem:driver/gesture_semantics`). Palm operation is the default
(`PAJ_Z_OVERRIDES_XY=0`, confirm 20 ms, cooldown 230 ms).
