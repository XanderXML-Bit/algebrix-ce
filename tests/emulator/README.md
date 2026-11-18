# Emulator Test Assets

Current package: 35 official programs plus four runtime AppVars and arTIfiCE, totaling 40 calculator files.
Dated replay results below remain historical evidence and do not validate the refreshed binaries.

Files:
- `autotest.cemu-e2e.json` - focused real-ROM end-to-end AutoTester suite used by `scripts/cemu-test-runner.ps1`, covering boot, CalcEngine launch, prompt round-trip, `2+3`, `6*7`, `-5+10`, `3.14*2`, and divide-by-zero through deterministic screen CRC checks.
- `autotest.comprehensive.json` - real-ROM comprehensive AutoTester flow covering smoke arithmetic, precedence, trig/log, menu states, ANS/history, CAS, continuity, tab navigation, settings persistence, symbol insertion, recents, long-expression Enter-paths, CLEAR behavior, explicit 2nd-key handling, finance TVM, and complex-mode arithmetic.
- `autotest.comprehensive.baselined.json` - captured real-ROM CRC baseline for the comprehensive suite, ready for `-Mode Verify`.
- `autotest.comprehensive.crc-catalog.json` - canonical machine-readable catalog of every comprehensive-suite hash, description, and accepted CRC.
- `autotest.comprehensive.crc-catalog.md` - human-readable CRC catalog generated from the same baselined suite.
- `autotest.enter-path-proof.json` - focused real-ROM AutoTester proof for the canonical `Expr -> Enter` path, covering empty Enter, result-screen Enter, rapid Enter bursts, error recovery, long-expression re-entry, very large numbers, repeated chained Enter cycles, and TI-OS error-dialog quit navigation.
- `autotest.enter-path-proof.baselined.json` - captured CRC baseline for the Enter-path proof suite.
- `autotest.enter-path-proof.crc-catalog.json` / `.md` - generated CRC catalog artifacts for the Enter-path proof suite.
- `autotest.artifice-smoke.json` - dedicated real-ROM arTIfiCE smoke suite covering shell load, the verified `CLEAR -> MODE -> PRGM` exit choreography back to TI-OS, CalcEngine launch, directed arithmetic smoke (`2+3`, `6*7`, `-5+10`, `3.14*2`, `2+3*4`, `(4+5)*3`, `2^8`, `1/0`), prompt return, menu/settings navigation, and lightweight `mean(...)` / `nderiv(...)` module probes.
- `autotest.artifice-smoke.baselined.json` - captured CRC baseline for the arTIfiCE smoke suite.
- `autotest.artifice-smoke.crc-catalog.json` / `.md` - generated CRC catalog artifacts for the arTIfiCE smoke suite.
- `outputs/artifice-e2e.summary.json` - default summary artifact written by `tools/emulator/run-artifice-e2e-verification.ps1`, capturing the official arTIfiCE v2 (`prgmA`) end-to-end verification result.
- `outputs/enter-path-proof.summary.json` - machine-readable summary artifact written by `tools/emulator/run-enter-path-proof.ps1`.
- `outputs/enter-path-proof.parsed.json` - normalized per-hash pass/fail view of the Enter-path proof verify stage.
- `outputs/artifice-smoke.summary.json` - machine-readable summary artifact written by `tools/emulator/run-artifice-smoke.ps1`.
- `outputs/multi-module-smoke.summary.json` - machine-readable wrapper artifact written by `tools/emulator/run-multi-module-smoke.ps1`.
- `outputs/multi-module-smoke.artifice.summary.json` / `outputs/multi-module-smoke.environment.summary.json` - delegated stage summaries emitted by the multi-module smoke wrapper.
- `outputs/cemu-pipeline-checklist.summary.json` - machine-readable full end-to-end checklist artifact written by `tools/emulator/run-cemu-pipeline-checklist.ps1`.
- `manifest.json` - canonical transfer manifest for the real-hardware package: 35 `.8xp` programs, 4 `.8xv` runtime dependencies, and file-level CRC32 expectations.
- `autotest.clean-exit.json` - focused real-ROM AutoTester probe for result-screen quit, app exit, and clean TI-OS homescreen return.
- `autotest.clean-exit.baselined.json` - captured CRC baseline for the clean-exit probe.
- `autotest.clean-exit.crc-catalog.json` / `.md` - generated CRC catalog artifacts for the clean-exit probe.
- `autotest.smoke.template.json` - base AutoTester config for CalcEngine smoke tests.
- `autotest.smoke.generated.json` - generated at runtime by `tools/emulator/run-cemu-autotest.ps1`.
- `autotest.functional.json` - strict multi-hash continuous-session AutoTester flow covering prompt boot, repeated expression entry + Enter, return to prompt, divide-by-zero, and `MODE` menu entry/exit.
- `matrix-gap-analysis.md` - pass-64 write-up of the failed real-ROM matrix literal entry attempts and the capabilities required to close the gap.
- `autotest.module-coverage.json` - additive real-ROM AutoTester suite covering integral, series, continuity, derivative, polynomial, and probability `.8xp` modules not reached by `autotest.comprehensive.json`: CECIDF, CEIPRT, CEITRG, CEIUSB, CEIPFR, CEISTR, CECSER, CECLIC, CEAPDV, CECPOL, CESTPRB. Runs strictly through `tools/emulator/cemu-headless/` (no GUI fallback). CRC placeholders still require capture on the self-hosted real-ROM lane.
- `autotest.module-coverage.crc-catalog.md` - human-readable CRC catalog for the module-coverage suite (placeholder `00000000` CRCs until baselined).
- `MODULE-COVERAGE-MATRIX.md` - 35-module emulator coverage audit matrix (manifest × existing + new hashes, with gap analysis).
- `terminal-harness-output.json` - optional JSON artifact generated by `tools/emulator/run-terminal-harness.ps1`.
- `terminal-regression.pass2.cases.json` - canonical multi-case regression suite for `tools/emulator/run-terminal-regression.ps1`.
- `terminal-regression.pass3.cases.json` - strict CEmu hash-asserted regression suite for `tools/emulator/run-terminal-regression.ps1` (supports pass-4 capture + pass-5 stabilized baseline capture from CRC placeholders).
- `terminal-regression.pass3.baselined.cases.json` - captured pass-5 CRC baseline file verified locally against `rom/Rom.rom`.
- `terminal-regression.pass6.cases.json` - comprehensive 36-case mock regression suite covering all arithmetic operators, precedence, parentheses, unary operators, decimals, chained operations, error conditions, ignored keys, pending state, alias normalization, and edge cases.
- `terminal-regression.pass7.cases.json` - focused edge-case suite validating unary/exponent precedence, negative exponent handling, overflow diagnostics, and malformed-expression errors.
- `terminal-regression.pass8.cases.json` - strict CEmu Enter-path hash scaffold for isolated interactive submissions.
- `terminal-regression.pass8.baselined.cases.json` - captured pass-5/6-style CRC baseline file for the expanded strict CEmu Enter-path suite.
- `terminal-regression.pass10.cases.json` - packaged-suite archive/load stress scaffold used with manifest-backed module auto-discovery, repeated archive->RAM->archive churn, and constrained package-pressure checks.
- `terminal-regression-summary.json` - optional regression summary artifact generated by `tools/emulator/run-terminal-regression.ps1`.
- `outputs/prehardware-validation-gate.summary.json` - machine-readable pre-hardware gate artifact generated by `tools/emulator/run-prehardware-validation-gate.ps1`.
- `outputs/cemu-release-candidate-confidence.json` - machine-readable final real-ROM confidence artifact generated by `scripts/cemu/release_candidate_gate.py`.
- `outputs/comprehensive-environment-verification.summary.json` - machine-readable package/ROM/multi-module verification artifact generated by `tools/emulator/run-comprehensive-environment-verification.ps1`.
- `outputs/runtime-integrity.summary.json` - machine-readable pass-64 runtime-integrity artifact covering blank-slate verify, clean exit, memory-pressure evidence, and flash-write safety evidence.
- `outputs/check-crc-drift.summary.json` - machine-readable CRC drift report generated by `tools/emulator/check-crc-drift.ps1`.

## First-time Baseline

Top-level Windows entry points:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-cemu.ps1
powershell -ExecutionPolicy Bypass -File scripts\cemu-test-runner.ps1
```

`scripts\setup-cemu.ps1` downloads the official Windows `CEmu.exe` release when needed, validates `rom\Rom.rom`, reuses an existing `autotester.exe` when available, and records exact manual build steps when the standalone AutoTester CLI is not installed locally.

`scripts\cemu-test-runner.ps1` is the focused real-ROM end-to-end lane. It delegates execution to the upstream-backed AutoTester wrapper, persists the generated config plus parsed summary, and reports the requested high-level assertions:
- CEmu boots successfully
- `.8xp` payloads are transferred into the emulator lane and the archive-load proof is established via transfer plus successful launch
- CalcEngine launches
- Basic calculations match expected screen output

1. Build program:
```powershell
make
```

2. Run smoke test with local ROM:
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 `
  -RomPath "C:\ti-roms\ti84ce.rom" `
  -AutotesterPath "C:\tools\cemu\autotester.exe"
```
On Windows, the official CEmu release currently ships `CEmu.exe` but not a standalone `autotester.exe`. Build `tests/autotester/autotester_cli.cpp` from the matching upstream CEmu tag and point `-AutotesterPath` at that built binary.
By default the wrapper resolves `bin/launcher/CALCENG.8xp`, discovers the current manifest-backed package, and writes all `39` transfer files (`35` `.8xp` modules plus `4` runtime `.8xv` dependencies) into the generated AutoTester config.

Current verified workstation path:

- `C:\Users\user\Tools\CEmu\v2.0\CEmu.exe`
- `C:\Users\user\Tools\CEmu\v2.0\autotester.exe`
- `rom/Rom.rom`
- `C:\Users\user\Downloads\CalcEngine`

3. Replace `__REPLACE_WITH_BASELINE_CRC__` in `autotest.smoke.template.json`
with the known-good CRC reported by AutoTester.

Run wrapper tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-cemu-autotest-tests.ps1
```

Run comprehensive-runner tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-comprehensive-autotest-tests.ps1
```

Run the official arTIfiCE smoke runner:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-artifice-smoke.ps1 `
  -RomPath "rom\Rom.rom" `
  -AutotesterPath "C:\Users\user\Tools\CEmu\v2.0\autotester.exe" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -EmulatorBackend Auto `
  -Mode Verify
```

`-EmulatorBackend Auto` is now the preferred default for this runner: when the checked-in `cemu-headless` wrapper is available, the delegated comprehensive stage can surface direct reported CRCs instead of falling back to provenance-honest inferred pass rows.

If the workstation does not already have `tools/emulator/assets/artifice-v2.8xp`, the same runner can now bootstrap the external jailbreak prerequisite first:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-artifice-smoke.ps1 `
  -RomPath "rom\Rom.rom" `
  -AutotesterPath "C:\Users\user\Tools\CEmu\v2.0\autotester.exe" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -EmulatorBackend Auto `
  -Mode Verify `
  -BootstrapArtificeAsset
```

Run the dedicated Enter-path proof:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-enter-path-proof.ps1 `
  -RomPath "rom\Rom.rom" `
  -AutotesterPath "C:\Users\user\Tools\CEmu\v2.0\autotester.exe" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -Mode Verify
```

Run the multi-module smoke wrapper:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-multi-module-smoke.ps1 `
  -RomPath "rom\Rom.rom" `
  -AutotesterPath "C:\Users\user\Tools\CEmu\v2.0\autotester.exe" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine"
```

That wrapper also forwards `-BootstrapArtificeAsset` into the delegated arTIfiCE smoke stage.

Run the full CEmu pipeline checklist:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-pipeline-checklist.ps1 `
  -RomPath "rom\Rom.rom" `
  -AutotesterPath "C:\Users\user\Tools\CEmu\v2.0\autotester.exe" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -OutputPath "tests/emulator/outputs/cemu-pipeline-checklist.summary.json"
```

Run the arTIfiCE smoke-runner tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-artifice-smoke-tests.ps1
```

Run the Enter-path proof-runner tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-enter-path-proof-tests.ps1
```

Run the multi-module smoke-runner tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-multi-module-smoke-tests.ps1
```

Run the full-checklist runner tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-cemu-pipeline-checklist-tests.ps1
```

## Terminal Harness Usage

Run the API-first terminal harness with deterministic output:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -Format json `
  -OutputPath "tests/emulator/terminal-harness-output.json"
```

If CEmu paths are available (`-RomPath` and `-AutotesterPath`), the harness runs the existing AutoTester wrapper first. If not, it falls back to deterministic mock mode unless `-NoMockFallback` is set.

Archive<->RAM swap simulation example:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -ForceMock `
  -ArchiveRamDispatchModulePath "C:\path\to\CEALG.8xp" `
  -ArchiveRamSwapRamCapacityBytes 65536 `
  -ArchiveRamFailurePhase none
```

Strict CEmu hash verification example:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -ExpectedVramCrcs "A1B2C3D4" `
  -CEmuHashName "addition_result"
```

Run harness unit tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-terminal-harness-tests.ps1
```

## Terminal Regression Runner (Pass 2+)

Run multiple deterministic key-sequence cases through the terminal harness in one command:

```powershell
# Pass 2: basic 5-case smoke suite
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass2.cases.json" `
  -ForceMock `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"

# Pass 6: comprehensive 36-case mock regression suite
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass6.cases.json" `
  -ForceMock `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"

# Pass 7: unary/exponent + overflow edge-case suite
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass7.cases.json" `
  -ForceMock `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"

# Pass 10: manifest-backed packaged-suite archive/load stress
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass10.cases.json" `
  -ForceMock `
  -ModuleProgramPathSource manifest_transfer_suite `
  -ArchiveRamDispatchModulePath "deploy\hardware-ready\CECLIC.8xp" `
  -ArchiveRamRapidSwapCycles 2 `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"

# Pass 10 pressure variant: reserve archive swap RAM to force deterministic package-pressure partitioning
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass10.cases.json" `
  -ForceMock `
  -ModuleProgramPathSource manifest_transfer_suite `
  -ArchiveRamDispatchModulePath "deploy\hardware-ready\CECLIC.8xp" `
  -ArchiveRamSwapRamCapacityBytes 45000 `
  -ArchiveRamReservedRamBytes 5000 `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```

Run the same case file in strict CEmu mode:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass8.baselined.cases.json" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```

Run the direct continuous-session AutoTester flow:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -TemplatePath "tests/emulator/autotest.functional.json" `
  -OutputConfigPath "tests/emulator/outputs/autotest.functional.generated.json"
```

Run the focused CAS isolation probe to compare `deriv` / `lim` against a baseline `summary.json` from an earlier probe run with minimal required module sets. Produce that baseline first with a run of your own and pass its path:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cas-gap-isolation.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -BaselineSummaryPath "<path to your baseline summary.json>" `
  -OutputDir "tests/emulator/outputs/cas-gap-isolation" `
  -ConvertPngIfAvailable
```

What it captures:
- one clean-launch case each for `d/dx(x^2)`, `lim(sin(x)/x,x,0)`, and the routed `expand((x+1)^2)` control
- minimal transfer manifests per case so unrelated modules are removed from the experiment
- wrapper logs, generated AutoTester configs, and a machine-readable `summary.json`
- raw `vram_*.ppm` images, failure dumps, and optional `.png` conversions when the current AutoTester build emits them
- pass-54 timeline comparison so the runner reports whether the stripped transfer set still lands on the same real-ROM CRC family

Run the isolation-runner test:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-cas-gap-isolation-tests.ps1
```

Capture and persist pass-4 hash baselines from `__BASELINE_REQUIRED__` placeholders:

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

Use pass-5 stabilization to require consensus before writing captured CRCs:

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

Notes:
- Without `-CaptureCEmuHashBaselines`, placeholder CRCs fail fast with a setup message.
- Baseline capture uses a seed CRC (`-BaselineSeedCrc`, default `00000000`) to force mismatch and reads observed CRCs from harness output.
- `-BaselineCaptureSampleCount` (default `1`) controls capture attempts per placeholder case; `-BaselineCaptureConsensus` (default `1`) is the required frequency for a single CRC before the case is updated.
- `run-terminal-harness.ps1` can auto-resolve support modules with `-ModuleProgramPathSource official_split_suite` or `-ModuleProgramPathSource manifest_transfer_suite` when `-ModuleProgramPaths` is omitted. The manifest-backed source now prefers exact matches from `deploy/hardware-ready/` before falling back to stale `bin/` copies.
- Archive swap simulation options (`-ArchiveRamDispatchModulePath`, `-ArchiveRamSwapRamCapacityBytes`, `-ArchiveRamFailurePhase`, `-ArchiveRamRapidSwapCycles`, `-ArchiveRamReservedRamBytes`, `-ArchiveRamSimulatePartialLoad`) are forwarded into summary JSON under `options.archive_ram_*` and per-case diagnostics under `actual.archive_ram_swap.*`, including module counts, dispatch/return event totals, effective RAM capacity, and per-module item telemetry.

Run regression-runner tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-terminal-regression-tests.ps1
```

## Pre-hardware Validation Gate

Run strict pre-hardware gate:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode strict `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```

Run minimal pre-hardware gate:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode minimal `
  -SkipDesktopStage `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```

Enable optional CEmu-dependent gate stages:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode strict `
  -EnableCEmuChecks `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```

Current verified state on 2026-03-21:
- `-Mode strict` passes locally against the official split-suite launcher plus all `42` support `.8xp` files.
- `-EnableCEmuChecks` now uses the same split-suite transfer set in AutoTester, requires a hashed `basic_addition` smoke screen in `emulator_harness_cemu`, and runs the strict pass-8 Enter-path hash suite without mock fallback.
- The strict CEmu lane now passes locally on this workstation with the official CEmu `v2.0` binary and a standalone `autotester.exe` built from the matching upstream `v2.0` source tree.
- Pass-5 CRC baselines were captured into `tests/emulator/terminal-regression.pass3.baselined.cases.json`, and a strict rerun against that file passes locally.
- `tests/emulator/terminal-regression.pass8.baselined.cases.json` now passes locally for six hashed Enter-path screens, and `tests/emulator/autotest.functional.json` now passes locally for an 11-hash continuous session.
- `tests/emulator/autotest.comprehensive.baselined.json` now carries `87` hashed screens covering the original smoke/UI flow plus settings toggles, statistics, cursor movement, DEL/backspace, symbol insertion, recents, long-expression Enter-paths, CLEAR mid-expression behavior, explicit `2nd`+`ln`, six finance TVM cases, and complex-mode enable/add/multiply coverage, and `tools/emulator/run-comprehensive-autotest.ps1` emits matching CRC catalog artifacts.
- Pass 77 hardens the dedicated arTIfiCE smoke lane to `19` passing hashes: `tools/emulator/run-artifice-smoke.ps1` plus `tests/emulator/autotest.artifice-smoke.baselined.json` now pass locally on the real ROM for shell load, the verified `CLEAR -> MODE -> PRGM` exit sequence back to TI-OS, launch, `2+3`, prompt return, `6*7`, `-5+10`, `3.14*2`, `2+3*4`, `(4+5)*3`, `2^8`, `mean(1,2,3,4,5)`, `nderiv(x^2,x,1)`, divide-by-zero, and menu/settings open-close navigation.
- Pass 77 also expands the dedicated Enter-path proof lane: `tools/emulator/run-enter-path-proof.ps1` now verifies `19` real-ROM hashes for empty Enter, result-screen Enter returning to prompt, rapid Enter bursts, divide-by-zero recovery, post-error re-entry, long-expression re-entry, very large-number input/results, repeated chained Enter cycles, and TI-OS error-dialog quit navigation using `tests/emulator/autotest.enter-path-proof.baselined.json`.
- Matrix-expression coverage is still blocked in AutoTester because the real-ROM `APPS -> ;` semicolon path is not deterministic enough to enter row-separated matrix literals after the keypress lands.
- `tools/emulator/run-comprehensive-environment-verification.ps1` now passes locally against the same ROM/package, proving the `43`-program transfer manifest, OS `5.8.0` ROM signature, full-package CEmu boot, and scaffolded launch/finish round-trips for every `.8xp`.
- `tools/emulator/run-runtime-integrity-verification.ps1` now passes locally from a fresh staged package, proving a blank-slate `87`-hash verify, a deterministic clean-exit TI-OS homescreen hash, and pass-64 memory/flash integrity evidence.
- `tools/emulator/run-artifice-e2e-verification.ps1` now passes locally against a captured six-hash baseline (`EAC3FDAC`, `28B88B56`, `C84EC5D0`, `28B88B56`, `C23818C9`, `151319C8`) on the verified `prgmA -> CLEAR -> MODE -> PRGM -> CALCENG` launch path.
- `tools/emulator/run-multi-module-smoke.ps1` now passes locally, pairing the real-ROM arTIfiCE smoke lane with the manifest-driven `43`-program dispatch sweep and reporting `48` staged transfers, `43` dispatch programs, and `19/19` smoke hashes.
- Pass 79 adds `tools/emulator/run-cemu-pipeline-checklist.ps1`, a canonical one-command checklist that runs the strict CEmu-enabled prehardware gate, explicit arTIfiCE E2E verification, multi-module smoke, blank-slate runtime integrity, and CRC drift detection, while preserving each delegated summary as a first-class artifact.
- `tools/emulator/check-crc-drift.ps1` now passes locally and returns structured mismatch context if any comprehensive CRC drifts.
- Pass 65 hardens the real-ROM lane with `autotester.exe` auto-discovery, a consistent `rom/Rom.rom` preflight failure, manifest-driven package validation, and `tools/emulator/parse-autotest-result.ps1` for per-hash pass/fail summaries.
- The placeholder scaffolds `terminal-regression.pass3.cases.json` and `terminal-regression.pass8.cases.json` remain available for future recapture when ROM or UI output changes. Real hardware confirmation is still pending.

Gate summary contract:
- Top-level: `schema_version`, `runner`, `mode`, `success`, `exit_code`, `totals`, `options`, `stages`.
- Stage-level: `name`, `required`, `status`, `success`, `exit_code`, `duration_ms`, `command`, `output_lines`, `artifacts`, `notes`, `errors`.
- Required-stage enforcement: gate fails when `totals.required_failed > 0`.

Run pre-hardware gate tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-prehardware-validation-gate-tests.ps1
```

## Comprehensive Environment Verification

Run the pass-62 environment verifier:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-comprehensive-environment-verification.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/comprehensive-environment-verification.summary.json"
```

What it verifies:
- the top-level `C:\Users\user\Downloads\CalcEngine\` package matches the expected `35` `.8xp` + `4` `.8xv` + `README.md` manifest
- `rom/Rom.rom` contains the OS `5.8.0` certificate bytes and the matching header signature
- the comprehensive CEmu suite still passes with the full `39`-file transfer set against the real ROM
- every `.8xp` program can be loaded archived and launch/finish cleanly through the eZ80 scaffold's TI-OS dispatch model under simulated OS `5.8` + arTIfiCE bypass

Current limitation:
- Matrix-expression coverage is still blocked because AutoTester cannot reliably continue text entry after the real-ROM `APPS` semicolon transition, and the `2nd`+`CLEAR` template path is likewise non-deterministic.
- CEmu AutoTester does not expose a robust per-module launcher-path API for all `35` programs in one deterministic UI run, so the verifier splits proof into two stages: full-package CEmu boot on the real ROM, plus per-program scaffolded dispatch round-trips.

## Runtime Integrity Verification

Run the pass-64 runtime-integrity verifier:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-runtime-integrity-verification.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/runtime-integrity.summary.json"
```

What it verifies:
- stages a fresh temporary transfer package with only the shipped `35` `.8xp`, `4` `.8xv`, and `README.md`
- validates that staged package against `tests/emulator/manifest.json` before invoking CEmu, including file-level CRC32 expectations
- reruns the full real-ROM comprehensive suite from that blank-slate package and confirms all `87` hashes still pass
- reruns the dedicated clean-exit probe and confirms `2nd`+`MODE` returns to CalcEngine input first, then to the TI-OS homescreen
- runs the comprehensive environment verifier on the staged package to pair real-ROM settings/history writes with the `35`-program archive->RAM->archive dispatch sweep

Current limitation:
- arTIfiCE assets are still treated as an external prerequisite because the Downloads package does not bundle them today; the runtime-integrity summary reports that explicitly.
- CESETTNG / CEHISTOR VAT internals are still indirectly proven rather than directly inspected.

## arTIfiCE E2E Verification

Run the Pass 66 arTIfiCE-aware real-ROM lane:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-artifice-e2e-verification.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/artifice-e2e.summary.json"
```

Bootstrap the external arTIfiCE prerequisite during the same run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-artifice-e2e-verification.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -BootstrapArtificeAsset `
  -OutputPath "tests/emulator/outputs/artifice-e2e.summary.json"
```

What it verifies:
- the current official arTIfiCE v2 control path (`prgmA`) is represented explicitly instead of relying on AutoTester's hidden `Asm(` launch helper
- the AutoTester sequence hashes the arTIfiCE shell, exits with `MODE`, alpha-types `CALCENG`, and then hashes the `2+3` result screen
- the runner stops with a structured blocker when the external arTIfiCE `.8xp` is missing locally, rather than reporting a misleading pass
- when `-BootstrapArtificeAsset` is set, the summary also records the bootstrap attempt, exit code, output lines, staged asset path, SHA-256, and size

## CRC Drift Detection

Run the comprehensive CRC drift checker:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/check-crc-drift.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/check-crc-drift.summary.json"
```

What it verifies:
- reruns the comprehensive suite in `Verify` mode
- compares the run against `tests/emulator/autotest.comprehensive.baselined.json`
- reports every drifting hash with its name, accepted CRCs, observed CRC, and description

Run drift-checker tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-check-crc-drift-tests.ps1
```

## Autotest Result Parser

Parse a comprehensive verify summary into a per-hash pass/fail/missing report:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/parse-autotest-result.ps1 `
  -InputPath "tests/emulator/outputs/check-crc-drift.runner.summary.json"
```

Write the parsed summary back to JSON:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/parse-autotest-result.ps1 `
  -InputPath "tests/emulator/outputs/check-crc-drift.runner.summary.json" `
  -OutputPath "tests/emulator/outputs/check-crc-drift.parsed.json"
```

What it reports:
- pass/fail/missing status for each hash in the verify summary
- total pass count, total fail count, and total missing count
- exit code `0` when all hashes pass, `1` when any hash fails or is missing

## eZ80 Scaffold Contract (Pass 0)

Validate the new custom-emulator C interface scaffolding (headers + stub sources):

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-ez80-scaffold-tests.ps1
```

Probe ROM metadata through the API-first scaffold CLI:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/ez80/run-ez80-scaffold-cli.ps1 `
  -Command probe-rom `
  -RomPath "rom/Rom.rom" `
  -Format json
```

Run CLI behavior tests:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-ez80-scaffold-cli-tests.ps1
```

## Notes

- Keep ROM files out of this repository.
- Keep this suite small and deterministic.
- Add additional JSON configs for scenario-specific workflows as needed.
