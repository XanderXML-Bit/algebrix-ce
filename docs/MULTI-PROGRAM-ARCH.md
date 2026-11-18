# CalcEngine Multi-Program Architecture

## Goal
CalcEngine is split into a launcher plus feature modules so each TI-84 CE program stays below practical RAM loading limits. The launcher parses input, routes work, and persists shared state through AppVars.

## Program Layout

### Launcher
- Program: `CALCENG`
- Entry: `src/core/launcher/main.c`
- Responsibilities:
  - Main input/result UI loop
  - Module menu (`2nd` + `CLEAR`) for manual module launch
  - Expression parsing and AST classification
  - AppVar request/response/state lifecycle
  - Cross-module state restore (`settings`, `Ans`, history)

### Feature modules
Each module is independently built as a separate `.8xp` and has its own `main` entry file under `src/modules/<name>/main.c`.

- `CEALG` (`src/modules/algebra/`): algebra + general symbolic/numeric fallback
- `CECALC` (`src/modules/calculus/`): derivatives + integrals + calculus operations
- `CELIMS` (`src/modules/limits_series/`): limits + series + sequence tools
- `CEMAT` (`src/modules/matrix/`): matrix operations
- `CESTPRB` (`src/modules/stats_prob/`): statistics + probability
- `CEFIN` (`src/modules/finance/`): finance/TVM operations
- `CEGRPH` (`src/modules/graphing/`): graphing domain module
- `CELSCX` (`src/modules/lists_complex/`): list + complex workflows

Shared module runtime entry is in `src/modules/common/module_entry.c`.

## Shared AppVar Protocol
Protocol definitions are centralized in `src/split/ipc.h` and exposed for core code via `src/core/protocol/appvar_protocol.h`.

### AppVars
- Request AppVar: `CE_REQ`
- Response AppVar: `CE_RESP`
- Shared state AppVar: `CE_STATE`

### Versioning
- `magic`: `0xCE84`
- `version`: `2`

### Request payload (`ce_request_t`)
Binary packed struct:
- Header: `magic`, `version`, `domain`, `flags`, `ast_size`
- Shared context snapshot: `settings`, `last_answer`
- Program chaining metadata: `caller`, `return_to`
- Expression text: `expression`
- Serialized AST payload: `ast_payload` (up to `CE_AST_WIRE_MAX`)

Flags:
- `CE_REQ_FLAG_HAD_EQUALS`
- `CE_REQ_FLAG_HAS_AST`

### Response payload (`ce_response_t`)
Binary packed struct:
- Header: `magic`, `version`, `ok`, `show_steps`
- Updated context: `settings`, `last_answer`
- Result strings: `result`, `error`
- Step output: `step_count`, `steps[MAX_STEPS][MAX_STEP_LEN]`

### Shared state (`ce_shared_state_t`)
Binary packed struct persisted between launches:
- Header: `magic`, `version`
- Flow flags: `has_pending_response`, `pending_module`
- User context: `settings`, `last_answer`, `last_expression`
- History ring buffer: `hist_count`, `hist_write`, `history[]`

## Control Flow
1. User enters expression in launcher.
2. Launcher tokenizes/parses expression and classifies target domain.
3. Launcher writes `CE_REQ` + updates `CE_STATE`.
4. Launcher ensures target module is in RAM, then calls `os_RunPrgm()`.
5. Module reads `CE_REQ`, executes domain processor, writes `CE_RESP`, updates `CE_STATE`.
6. Launcher resumes, reads `CE_RESP`, restores state, re-archives module.
7. Launcher redraws result/steps and remains in main loop.

## Source Tree Boundaries
- Core launcher entrypoint: `src/core/launcher/`
- Core protocol surface: `src/core/protocol/`
- Module common runtime: `src/modules/common/`
- Module-specific entries: `src/modules/<module>/`
- Existing runtime internals retained under `src/split/` for compatibility (`ipc`, `classify`, `processor`, `astwire`, globals).

## Build System
Top-level `Makefile` now builds:
- Suite: `make` (same as `make split-suite`)
- Individual modules: `make split-launcher`, `make split-algebra`, `make split-calculus`, `make split-limits_series`, `make split-matrix`, `make split-stats_prob`, `make split-finance`, `make split-graphing`, `make split-lists_complex`
- Desktop tests: `make desktop-tests` (delegates to `tests`)

Legacy split targets are retained as aliases (for example `split-core`, `split-calc`, `split-prob`) to avoid breaking existing workflows.

## Desktop Test Compatibility
Desktop tests still compile the full monolithic engine from `tests/Makefile` (`../src/*.c` test source list). New split entry files are additive and do not change the existing test compile set.

## Validation
- Desktop routing coverage now verifies domain-to-program mapping through the split processor for algebra, calculus, limits/series, matrix, stats/probability, finance, graphing, and lists/complex paths (`tests/test_main.c`).
- Emulator runtime coverage now launches and finishes each archived split module (`CEALG`, `CECALC`, `CELIMS`, `CEMAT`, `CESTPRB`, `CEFIN`, `CEGRPH`, `CELSCX`) and asserts active-program MMIO telemetry plus archive↔RAM state transitions (`tools/emulator/ez80/tests/ez80_stub_runtime_tests.c`).

## OS Compatibility Notes
- TI-OS 5.4: supported in native mode.
- TI-OS 5.5-5.8: supported with arTIfiCE jailbreak environment.
- Architecture avoids monolithic RAM load by chaining smaller programs and persisting context through AppVars.
