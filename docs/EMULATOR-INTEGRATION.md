# Emulator Integration

## Goal

Build a **complete 1:1 TI-84 CE emulator/simulator** with API, terminal, and agentic interfaces that enables:
- **100% pre-hardware verification** — CalcEngine should work first-try on real hardware after passing all emulator tests
- **AI agent interactive testing** — agents can operate the emulator exactly like a physical calculator (enter problems, navigate menus, verify LCD output)
- **Automated regression** — every CalcEngine function testable without physical hardware
- **DNGRADE verification** — test OS downgrade tool behavior safely before risking real hardware


## Acceptance standard

The goal is that the first real-device run confirms behaviour rather than discovering bugs. A test lane counts only if it speaks to keypad and control parity, visual fidelity, the real interactive path (open the program, type an expression, press Enter), or the exact artifact set users install. If a lane cannot speak to one of these, its limits are stated rather than over-claimed.

## Candidate Review

| Emulator | Result | Why |
|---|---|---|
| CEmu | Selected | Open source, TI-84 Plus CE focused, documented command-line options, AutoTester framework in upstream repo. |
| TI-SmartView CE | Not selected | Official classroom emulator, but no public CLI/API workflow comparable to CEmu AutoTester. |
| tilem2 | Not selected | Legacy Z80 family focus; TI-84 Plus CE/eZ80 automation path is not practical for this project. |

## Chosen Stack

- Emulator: CEmu
- Automation runner: CEmu AutoTester CLI
- Test config format: AutoTester JSON (`tests/autotester/json_file_documentation.txt` in CEmu upstream)
- Local wrapper for this repo: `tools/emulator/run-cemu-autotest.ps1`
- Terminal harness (API-first lane, passes 1-8): `tools/emulator/run-terminal-harness.ps1` + `tools/emulator/CalcEmuTerminalHarness.psm1`
- Native eZ80 stub CLI + agent surface: `tools/emulator/ez80/ez80_cli.exe`

## Native eZ80 CLI / Agent Surface

The custom stub now exposes a local native CLI for deterministic automation without CEmu:

```powershell
tools/emulator/ez80/ez80_cli.exe --rom rom/Rom.rom --json --screen-state --dump-state --quiet
tools/emulator/ez80/ez80_cli.exe --rom rom/Rom.rom --archived --load bin/launcher/CALCENG.8xp --launch CALCENG --json --list-programs --quiet
```

Interactive agent mode is line-delimited JSON over stdin/stdout:

```powershell
@(
  '{"cmd":"load_rom","id":1,"path":"rom/Rom.rom"}'
  '{"cmd":"step","id":2}'
  '{"cmd":"list_programs","id":3}'
  '{"cmd":"quit","id":4}'
) | tools/emulator/ez80/ez80_cli.exe --interactive
```

Key automation surfaces:
- `step` returns both `pc` and `next_pc` for deterministic instruction-by-instruction control.
- `get_screen_state` exposes visible lines, step lines, current input line, cursor position, and ASCII LCD render.
- `write_lcd_png` exports the current LCD framebuffer for visual QA.
- `list_programs` enumerates loaded program slots with archive/RAM/active-state metadata for multi-program launch validation.

## Local Setup

Top-level Windows scripts:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-cemu.ps1
powershell -ExecutionPolicy Bypass -File scripts\cemu-test-runner.ps1
```

`scripts\setup-cemu.ps1` provisions the official Windows `CEmu.exe` release, validates `rom\Rom.rom`, reuses an installed `autotester.exe` when present, and records the manual AutoTester build steps when the standalone CLI is not available locally.

`scripts\cemu-test-runner.ps1` is the focused real-ROM proof lane built on top of the existing AutoTester wrapper. It uses `tests/emulator/autotest.cemu-e2e.json` and reports boot, transfer, launch, prompt round-trip, and basic arithmetic screen assertions as one structured summary.

1. Install CEmu from:
- https://ce-programming.github.io/CEmu/
- Windows note: the official CEmu release provides `CEmu.exe`, but not a standalone `autotester.exe`. On Windows, build `tests/autotester/autotester_cli.cpp` from the matching upstream CEmu tag (for example `v2.0`) and point `-AutotesterPath` / `$env:CEMU_AUTOTESTER_PATH` at that built binary.

2. Obtain a ROM image from a physical TI-84 Plus CE:
- See `docs/ROM-ACQUISITION.md`.

3. Build CalcEngine:
```powershell
make
```

4. Run the smoke autotest template:
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 `
  -RomPath "C:\path\to\ti84ce.rom" `
  -AutotesterPath "C:\path\to\autotester.exe"
```
By default the wrapper resolves the official split-suite launcher from `bin/launcher/CALCENG.8xp`,
discovers the current 34 support `.8xp` artifacts from `Makefile`, and sends the full 35-file
deployment set to AutoTester.

5. Update template expected CRC values after first baseline run:
- `tests/emulator/autotest.smoke.template.json`

## Terminal Harness (Pass 1)

The terminal harness provides deterministic parseable output for assertion workflows while still using the existing CEmu AutoTester wrapper when CEmu inputs are available.

### JSON output (default)

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -RomPath "C:\path\to\ti84ce.rom" `
  -AutotesterPath "C:\path\to\autotester.exe"
```

### Text output

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -Format text `
  -RomPath "C:\path\to\ti84ce.rom" `
  -AutotesterPath "C:\path\to\autotester.exe"
```

### Explicit mock mode (no emulator dependency)

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -ForceMock `
  -Format json
```

### Strict CEmu hash assertion mode (Pass 3)

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -ExpectedVramCrcs "A1B2C3D4" `
  -CEmuHashName "addition_result"
```

### Archive<->RAM swap simulation mode (Pass 8)

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -ForceMock `
  -ArchiveRamDispatchModulePath "C:\path\to\CEALG.8xp" `
  -ArchiveRamSwapRamCapacityBytes 65536 `
  -ArchiveRamFailurePhase none
```

### Output contract highlights

- `mode`: `cemu`, `mock`, or `error`
- `input.keys_normalized`: canonical `KEY_*` tokens
- `screen.text` and `screen.*`: deterministic text/JSON assertion surface
- `autotest.*`: existing wrapper invocation details and exit code (when attempted)
- `autotest.hash_assertion.*`: optional hash assertion metadata (`enabled`, hash target, expected CRC list, observed CRC list, last observed CRC, and `matched`)
- `archive_ram_swap.*`: optional archive<->RAM simulation diagnostics (`simulated`, `pass`, `failure_phase`, `dispatch`, `return_to_archive`, `state`, `diagnostics`, `errors`)
- `fallback.reasons`: actionable reason(s) when falling back to mock mode

## Terminal Regression Runner (Pass 2/3)

Run multiple deterministic harness assertions from one JSON case file:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass2.cases.json" `
  -ForceMock `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```

Strict CEmu lane (no fallback) passthrough:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass3.cases.json" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```

Pass 4 baseline capture for placeholder CRCs:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass3.cases.json" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -CaptureCEmuHashBaselines `
  -BaselineOutputCaseFile "tests/emulator/terminal-regression.pass3.baselined.cases.json" `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```

Pass 5 stabilized baseline capture (multi-attempt consensus for flaky CRC runs):

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass3.cases.json" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -CaptureCEmuHashBaselines `
  -BaselineCaptureSampleCount 3 `
  -BaselineCaptureConsensus 2 `
  -BaselineOutputCaseFile "tests/emulator/terminal-regression.pass3.baselined.cases.json" `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```

Case file contract:
- Top-level JSON array or object with `cases`.
- Each case requires `name`, `keys`, and `expect`.
- `expect` must include `result`, `status`, `error`, and `ignored_keys`.
- Optional `expect.mode` and `expect.harness_success` allow explicit mode/exit assertions.
- Optional `expect.archive_ram_swap` enables explicit archive<->RAM simulation assertions in mock/cemu runs:
  - `simulated`, `pass`, `failure_phase`, `dispatch_loaded_to_ram`, `return_to_archive_completed`
- Optional `expect.cemu_hash` enables strict framebuffer verification in CEmu mode:
  - `expected_crcs` (required when `cemu_hash` is present): one or more 8-digit hex CRC values (optional `0x` prefix accepted).
  - Placeholder `__BASELINE_REQUIRED__` is supported only with `-CaptureCEmuHashBaselines`; otherwise the runner fails with a targeted setup message.
  - `name`, `start`, `size`, `timeout_ms` are optional and default to `terminal_vram`, `vram_start`, `vram_16_size`, and `8000`.
  - If `expect.cemu_hash` is provided, the case fails unless harness mode is `cemu` and hash verification matches.
  - In baseline-capture mode, the runner intentionally applies `-BaselineSeedCrc` (default `00000000`) to force an AutoTester mismatch, parses `autotest.hash_assertion.observed_crcs`, and writes an updated case file.
  - `-BaselineCaptureSampleCount` and `-BaselineCaptureConsensus` (both default `1`) add pass-5 stabilization so baselines are updated only after deterministic CRC agreement across multiple capture attempts.

Runner outputs:
- Human-readable per-case summary.
- Machine-parseable JSON summary in stdout between `JSON_SUMMARY_START` and `JSON_SUMMARY_END`.
- Optional persisted summary JSON via `-SummaryPath`.
- Baseline-capture metadata under `summary.baseline_capture.*` when capture mode is enabled, including per-case attempt/sample/count telemetry.
- Archive-swap diagnostics under `summary.options.archive_ram_*` and `summary.cases[*].actual.archive_ram_swap.*`.
- Non-zero exit on any failed case.

## Pre-hardware Validation Gate (Xander Directive)

Run the additive pre-hardware gate lane:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode strict `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```

Fast deterministic lane (skip desktop + edge suite, keep required harness/regression/hardware checks):

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode minimal `
  -SkipDesktopStage `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```

Optional CEmu-dependent lane (only when ROM + autotester paths are available):

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode strict `
  -EnableCEmuChecks `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```

Gate behavior:
- Ordered stages: desktop/unit -> emulator harness -> emulator regression suite(s) -> hardware-constraint checks -> optional CEmu harness/regression.
- Auto-discovered artifact validation resolves the official split-suite outputs from `Makefile`, prefers `bin/launcher/CALCENG.8xp` as the deployed launcher, and checks all 34 non-launcher `.8xp` artifacts instead of legacy or experimental `bin/` leftovers.
- When `-EnableCEmuChecks` is set, the CEmu harness forwards the same launcher + 34 support artifacts into `run-cemu-autotest.ps1` and now requires a real VRAM hash for the `2,+,3,enter` screen instead of accepting a zero-test AutoTester pass.
- The optional strict CEmu regression stage now uses `tests/emulator/terminal-regression.pass8.baselined.cases.json`, a six-case hash-asserted Enter-path suite covering `2+3`, `6*7`, `-5+10`, `3.14*2`, `(4+5)*3`, and divide-by-zero under `-NoMockFallback`.
- Required-stage failures always produce non-zero exit.
- Stage logs are emitted as `[PASS]`, `[FAIL]`, `[SKIP]`.
- A machine-readable summary JSON artifact is always written to `tests/emulator/outputs/` (or custom `-OutputPath`).
- Passing this gate means the shipped artifact set has survived the currently-implemented pre-hardware proof bundle; it does **not** by itself mean "good enough." The remaining obligation is to keep closing emulator-confidence gaps until the first real-device attempt should be a confirmation step rather than a debugging session.

Continuous-session functional CEmu run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -TemplatePath "tests/emulator/autotest.functional.json" `
  -OutputConfigPath "tests/emulator/outputs/autotest.functional.generated.json"
```

Current verified state on 2026-03-12:
- `run-prehardware-validation-gate.ps1 -Mode strict` passes locally against the official split-suite launcher plus all 34 support programs.
- `run-prehardware-validation-gate.ps1 -Mode strict -EnableCEmuChecks -RomPath rom/Rom.rom -AutotesterPath "$env:CEMU_AUTOTESTER_PATH"` passes locally on this Windows workstation after installing the official CEmu `v2.0` binary and building the standalone AutoTester CLI from the matching upstream `v2.0` source tree.
- The optional CEmu gate no longer relies on hashless AutoTester runs that could pass with `0 unique tests found`; the harness stage now requires the `basic_addition` VRAM hash.
- `run-terminal-regression.ps1 -CaseFile "tests/emulator/terminal-regression.pass8.baselined.cases.json" -NoMockFallback` passes locally with six real Enter-path hashes.
- `run-cemu-autotest.ps1 -TemplatePath "tests/emulator/autotest.functional.json"` now passes locally with 11 real hashes covering a continuous session, including result-to-prompt return and `MODE` menu entry/exit.
- `run-terminal-regression.ps1 -CaseFile "tests/emulator/terminal-regression.pass3.baselined.cases.json" -NoMockFallback` passes locally with the captured CRCs.
- The original scaffold file `tests/emulator/terminal-regression.pass3.cases.json` intentionally still contains `__BASELINE_REQUIRED__` placeholders so CRCs can be recaptured when the ROM or UI output changes. Real hardware confirmation is still pending, and these hashes still do not claim full hardware equivalence.

## OS 5.8 arTIfiCE bootstrap assist (Pass 93)

The arTIfiCE-aware real-ROM runners now support an explicit bootstrap assist for the external jailbreak payload:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-artifice-e2e-verification.ps1 `
  -RomPath "rom\Rom.rom" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -BootstrapArtificeAsset
```

What the bootstrap assist does:
- Keeps the existing local-asset behavior unchanged when `tools/emulator/assets/artifice-v2.8xp` already exists.
- If the asset is missing and `-BootstrapArtificeAsset` is set, runs `scripts/get-artifice-asset.ps1` before failing the OS 5.8 verification lane.
- Records the bootstrap attempt, exit code, output lines, staged asset path, SHA-256, and size in the runner summary JSON.

The same switch is also supported by:
- `tools/emulator/record-artifice-baseline.ps1`
- `tools/emulator/run-artifice-smoke.ps1`
- `tools/emulator/run-multi-module-smoke.ps1`

This still does not bundle arTIfiCE into the repo. It only automates the documented upstream fetch/stage step so the CEmu validation workflow can move forward without a separate manual asset-prep command when the workstation allows network access.

Summary JSON contract highlights:
- Top-level: `schema_version`, `generated_at_utc`, `runner`, `mode`, `success`, `exit_code`, `totals`, `options`, `stages`.
- Per-stage: `name`, `required`, `status`, `success`, `exit_code`, `duration_ms`, `command`, `output_lines`, `artifacts`, `notes`, `errors`.
- `totals.required_failed` is the gate decision input for required-stage enforcement.

## Scriptable Interfaces Used

From CEmu command-line documentation:
- `--rom`, `--send`, `--launch`, `--settings`, `--statefile`, `--screenshot`, `--python`

From CEmu AutoTester:
- JSON sequence commands (`action`, `delay`, `key`, `hash`, `hashWait`, `sendCSC`, `saveVar`)
- Exit code equals number of failed hash checks

## Sources

- CEmu site: https://ce-programming.github.io/CEmu/
- CEmu command-line args: https://raw.githubusercontent.com/wiki/CE-Programming/CEmu/Command-line-arguments.md
- CEmu AutoTester JSON format: https://github.com/CE-Programming/CEmu/blob/master/tests/autotester/json_file_documentation.txt
- CEmu AutoTester CLI source: https://github.com/CE-Programming/CEmu/blob/master/tests/autotester/autotester_cli.cpp
- TI-SmartView CE product page: https://education.ti.com/en/products/computer-software/ti-smartview-ce
- TI-84 Plus CE emulator recommendation thread: https://www.ticalc.org/archives/news/articles/15/152/152273.html
