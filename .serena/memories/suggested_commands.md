# suggested_commands

Shell is Windows PowerShell 5.1 — `&&` does not work, chain with `; if ($?) {...}`.

## Build / flash / inspect

```powershell
pio run                                    # build the example (default env)
pio run -t upload                          # flash via WCH-Link
pio run -t size                            # size report
pio run -e power_test -t upload            # power diagnosis: sensor suspended + heartbeat
pio run -t clean
pio run -t clean; if ($?) { pio run }     # forced full rebuild
```

`[platformio] default_envs` limits `pio run` to `ch32v003f4p6_evt_r0`; the
`power_test` env is opt-in via `-e` (it has its own `.pio/libdeps`, so the first
build downloads the I2C library again).

## Reproducing a CI variant locally

CI passes variant flags through env vars; `PLATFORMIO_BUILD_SRC_FLAGS` is what
carries `-Wextra -Werror` and only affects `src/`+`examples/` (not SDK/library):

```powershell
$env:PLATFORMIO_BUILD_FLAGS = '-DPAJ_GESTURE_ROTATE=2'
$env:PLATFORMIO_BUILD_SRC_FLAGS = '-Wextra -Werror'
pio run
Remove-Item Env:PLATFORMIO_BUILD_FLAGS, Env:PLATFORMIO_BUILD_SRC_FLAGS
```

Variants: `''` (default, project mask = 4 directions), `-DPAJ_INT_MODE=0`,
`-DPAJ_ENABLE_CALIBRATION=0`, `-DPAJ7620U2_DEBUG`,
`-DPAJ_INT_MODE=1 -DPAJ_PROVIDE_EXTI_ISR=0`, `-DPAJ_GESTURE_ROTATE=1|2|3`,
`-DPAJ_EXAMPLE_PS_MODE=1`, `-DPAJ_ENABLE_PS_MODE=0`, `-DPAJ_OWN_SYSTICK=0`,
`-DPAJ_REINIT_THRESHOLD=1`, `-DPAJ_CALIBRATE_ON_INIT=1`,
`-DPAJ_XDIR_THRESHOLD=0x0A -DPAJ_YDIR_THRESHOLD=0x0D`, `-DPAJ_REJECT_AMBIGUOUS=0`,
`-DPAJ_Z_OVERRIDES_XY=1 -DPAJ_ZDIR_THRESHOLD=0x50`, `-DPAJ_GESTURE_CONFIRM_MS=0`,
`-DPAJ_PS_MASK=1`, `-DPAJ_EXAMPLE_RESET_INFO=0`, `-DPAJ_EXAMPLE_POWER_TEST=1`.

`platformio.ini` already defines `-DPAJ_GESTURE_MASK=0x0F -DPAJ_WAVE_MASK=0`.
Overriding those requires removing the project values first, otherwise gcc warns
`redefined` (fatal with `-Werror`):

```powershell
$env:PLATFORMIO_BUILD_UNFLAGS = '-DPAJ_GESTURE_MASK=0x0F -DPAJ_WAVE_MASK=0'
$env:PLATFORMIO_BUILD_FLAGS   = '-DPAJ_GESTURE_MASK=PAJ_GESMASK_ALL -DPAJ_WAVE_MASK=1'
```

## Measuring the driver's own footprint (LTO hides it in `*.o`)

```powershell
& "$env:USERPROFILE\.platformio\packages\toolchain-riscv\bin\riscv-wch-elf-nm.exe" `
    --print-size --radix=d .pio\build\ch32v003f4p6_evt_r0\firmware.elf |
    Select-String ' [tT] (paj_|poll_gesture|configure_sensor|recover_sensor|wait_ready|bank_sel|write_seq|g_init_seq)'
```

With LTO on, `riscv-none-elf-size` on the `.o` files reports 0 — use `nm` on the
final ELF instead.

## GitHub

```powershell
git status -sb; git log --oneline
gh run list --limit 5
$j = gh run view <run-id> --json status,conclusion,jobs | ConvertFrom-Json
$j.jobs | ForEach-Object { "{0,-34} {1}" -f $_.name, $_.conclusion }
```

## Useful searches (ripgrep-style via Grep tool)

- all config macros: `PAJ_[A-Z_]+` in `src/paj7620_config.h`
- public API surface: `^\w[\w ]* (paj_\w+)\(`
- datasheet-derived tables in code: `\[\]\[3\] = {` in `src/paj7620.c`

See `mem:task_completion` for what must pass before a change is done.
