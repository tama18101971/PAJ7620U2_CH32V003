# tech_stack

- **MCU**: CH32V003F4P6, WCH QingKe V4F (RISC-V `rv32ecxw`, `ilp32e`),
  48 MHz HSI, 2048 B RAM / 16384 B flash. No floating point, no RTOS.
- **Framework**: NoneOS-SDK (`ch32v00x.h`, `debug.h` for `Delay_Ms/Delay_Us/printf`).
  `system_ch32v00x.c` provides `SystemInit`/`SystemCoreClock`.
- **Build**: PlatformIO, `src_dir = .` with
  `build_src_filter = +<src/> +<examples/basic/>`; `build_flags = -Isrc`;
  `board_build.use_lto = yes`.
- **Platform `ch32v` is not in the official PIO registry** — it comes from
  `https://github.com/Community-PIO-CH32V/platform-ch32v.git` (CI installs it
  explicitly; a local `pio` already has it).
- **Dependencies** (both pinned, both matter):
  - `I2C-CH32V003#v7.1.0` — I2C1 driver, bus recovery, `i2c_probe_address()`.
    Used through `lib_deps` in `platformio.ini` and `library.json`.
    API used by the driver: `i2c_start/i2c_send_addr/i2c_stop`,
    `i2c_read_register`, `i2c_write_register`.
    Caveat: a `len == 0` `i2c_write_buffer` still sends the register address,
    so the datasheet wake-up command is built from the low-level calls instead.
  - `i2c.h` is at `.pio/libdeps/ch32v003f4p6_evt_r0/I2C-CH32V003/src/i2c.h`.
- **Toolchain**: `~/.platformio/packages/toolchain-riscv/bin/riscv-wch-elf-gcc`.
  `pio` itself: `~/.platformio/penv/Scripts/pio.exe` (on PATH).
- **Datasheets**: `docs/PAJ7620U2_DS.pdf` (V0.8) and
  `docs/PAJ7620U2_datasheet.pdf` (V0.7) — same bank-based register map.
- **Language**: C, `gnu99`; SDK built as C++ (`cpp_support.cpp`) but project
  sources are C only.
- **Sibling projects** (same conventions, same author):
  `E:\GitProjects\APDS-9960_CH32V003` (driver being replaced),
  `E:\GitProjects\DriverMD6` (migration target), `E:\GitProjects\I2C-CH32V003`.
