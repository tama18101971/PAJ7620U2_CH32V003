# task_completion

No test framework, no linter, no formatter configured. "Done" means:

1. **All nine CI variants build clean** — `-Wextra -Werror`, no new warnings
   (see `mem:suggested_commands` for the env-var incantation):
   default, `PAJ_INT_MODE=0`, `PAJ_ENABLE_CALIBRATION=0`, `PAJ7620U2_DEBUG`,
   `PAJ_PROVIDE_EXTI_ISR=0`, `PAJ_GESTURE_ROTATE=1..3`, `PAJ_EXAMPLE_PS_MODE=1`,
   `PAJ_ENABLE_PS_MODE=0`, `PAJ_OWN_SYSTICK=0`.
   A variant that stops compiling when its feature is switched off is a bug.
2. **Size ceilings hold** (footprint guard, default config): RAM ≤ 600 B,
   Flash ≤ 13000 B. Baseline 508 B / 10484 B. Raise a ceiling only deliberately,
   in the same commit, with a reason.
3. **No float / no malloc / no unbounded stack frame** introduced; peak frame
   stays `calibrate_proximity()`'s `PAJ_CAL_SAMPLES` buffer (16 B).
4. **Public API and error codes stay drop-in compatible** with the APDS driver
   unless the change is explicitly a break (`mem:port/apds_migration`).
5. **Docs updated** when behaviour changes: `AGENTS.md` (agent-facing),
   `README_RU.md` / `README.md`, `INTEGRATION_RU.md` for anything affecting
   `DriverMD6`. `AGENTS.md` carries the reference sizes — update them.
6. **Datasheet statements are quoted, not paraphrased from memory**; if a
   register value changes, cite the datasheet section ("i. Initial",
   "iii. Change to PS mode", "v. Change to Gesture mode", "vi./vii. Suspend/Resume").

Commit style (matches `APDS-9960_CH32V003`): Conventional Commits, English
subject, body only when non-obvious, e.g. `feat: v1.0.0 — …`,
`build: add VS Code tasks …`. Push to `main` of
`tama18101971/PAJ7620U2_CH32V003`; CI must go green.

Hardware verification is still required for anything touching the register
map, the wake-up sequence or the interrupt masks — CI cannot substitute.
