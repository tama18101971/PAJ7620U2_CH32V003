# suggested_commands

Shell is Windows PowerShell 5.1 — `&&` does not work, chain with `; if ($?) {...}`.

## Build / flash / inspect

```powershell
pio run                                    # build the example
pio run -t upload                          # flash via WCH-Link
pio run -t size                            # size report
pio run -t clean
pio run -t clean; if ($?) { pio run }     # forced full rebuild
```

## Reproducing a CI variant locally

CI passes variant flags through env vars; `PLATFORMIO_BUILD_SRC_FLAGS` is what
carries `-Wextra -Werror` and only affects `src/`+`examples/` (not SDK/library):

```powershell
$env:PLATFORMIO_BUILD_FLAGS = '-DPAJ_GESTURE_ROTATE=2'
$env:PLATFORMIO_BUILD_SRC_FLAGS = '-Wextra -Werror'
pio run
Remove-Item Env:PLATFORMIO_BUILD_FLAGS, Env:PLATFORMIO_BUILD_SRC_FLAGS
```

Variants: `''` (default), `-DPAJ_INT_MODE=0`, `-DPAJ_ENABLE_CALIBRATION=0`,
`-DPAJ7620U2_DEBUG`, `-DPAJ_INT_MODE=1 -DPAJ_PROVIDE_EXTI_ISR=0`,
`-DPAJ_GESTURE_ROTATE=1|2|3`, `-DPAJ_EXAMPLE_PS_MODE=1`,
`-DPAJ_ENABLE_PS_MODE=0`, `-DPAJ_OWN_SYSTICK=0`.

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
