# conventions

Follows the sibling `APDS-9960_CH32V003` driver; keep the two diffable.

- **Comments/identifiers**: prose in Russian, identifiers and API names in
  English. Every block above a function states what it does, and — where it is
  not obvious — *why* (datasheet quote, bank, bit layout).
- **Formatting**: 4 spaces, `K&R`-ish braces, `if (cond) {` on one line,
  declarations at the top of a block (no C99 mixed declarations in the driver).
- **Indentation of macro config** uses aligned values, not tabs.
- **Error handling pattern** (do not invent a second one):
  - internal helpers: `bool` return, `note_i2c(st)` records the raw i2c.h code,
    caller decides the public error;
  - public `bool` API: set `g_last_error` and return `false`;
  - void-API (`paj_enableInterrupt` etc.): record `PAJ_ERR_I2C`, **never
    overwrite `g_last_error` on success**;
  - `g_last_i2c_status` is only ever written on a failed transaction.
- **Static state**: file-scope `static` in `paj7620.c`
  (`g_bank`, `g_last_error`, `g_last_i2c_status`, `g_reinit_count`,
  `g_gesture`/`g_has_gesture`, `g_cal_*`, `g_ticks_per_ms`). No globals in headers
  except the ISR-shared `g_paj_int_flag` (declared `extern volatile`).
- **Register tables** are `static const uint8_t (*)[3]` of `{bank, reg, value}`
  taken verbatim from the datasheet, with 0xEF bank writes stripped
  (`bank_sel()` handles them) and the Enable write extracted.
  Never turn them into code.
- **Config**: all tunables live in `paj7620_config.h` behind `#ifndef`, each with
  a `#error` range check. Documented as build-flag overrides only — a `#define`
  in the app's `.c` does **not** reach `paj7620.c`.
- **Enums**: `gesture_t` keeps APDS values 0..4 for compatibility; new
  gestures are appended, never renumbered.
- **Macros with an identity default** (e.g. gesture rotation) must compile to
  nothing at value 0 — use compile-time `#if` branches, not runtime checks.
- **ISR in `paj_int_config.c` is intentionally strong**, never `weak`
  (the NoneOS weak fallback is an infinite loop); the user hook
  `paj_exti_callback()` is the weak one.
- Helper idioms in use: `ARRAY_LEN()` macro, Newton `isqrt`, insertion
  `sort_u8`, `delay-with-deadline` timebase on own SysTick.
- No `printf` unless `PAJ7620U2_DEBUG`; it costs flash. `paj7620.c` still
  includes `<stdio.h>` so the guard is a macro, not a header change.
