# Automated Testing Pipeline Design

> **Public source note:** the staged `deploy/hardware-ready/` transfer package is not part of this repository. Build the modules from source with CEdev as described in the [README](../README.md).

Current package: 35 official programs, four runtime AppVars, and arTIfiCE, totaling 40 calculator files.
The package contains 44 files including metadata.
Earlier measured counts and emulator results below describe historical packages, not this refreshed package.
Revalidate current screen baselines on the self-hosted real-ROM lane.

## Objectives

1. Keep fast desktop regression checks on every push.
2. Add emulator validation for `.8xp` behavior before hardware runs.
3. Separate deterministic checks (hash/assertion) from visual/manual checks.

## Pipeline Tiers

## Tier 1: Desktop Unit/Integration Tests (existing)

- Runner: GitHub Actions Ubuntu
- Command: `cd tests && make && make run`
- Purpose: parser, evaluator, symbolic logic, safety regressions

## Tier 2: Emulator Smoke Tests (new)

- Runner: self-hosted Windows (or Linux with CEmu + AutoTester installed)
- Windows setup note: use the official CEmu release for `CEmu.exe`; build standalone `autotester.exe` from the matching upstream CEmu source tag because release assets do not currently ship that CLI binary.
- Inputs:
- official split-suite launcher `bin/launcher/CALCENG.8xp`
- 34 support `.8xp` artifacts auto-discovered from `Makefile`
- ROM path provided via secure runner path/secret mapping
- Command:
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH"
```
- Python real-ROM pipeline command (Pass 89 ROM validation + accuracy audit lane):
```powershell
python scripts/cemu_pipeline.py --rom "$env:CEMU_ROM_PATH" --autotester "$env:CEMU_AUTOTESTER_PATH"
```
- CI-friendly terminal harness command (parseable JSON + strict emulator requirement):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-harness.ps1 `
  -Keys "2,+,3,enter" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -Format json `
  -OutputPath "tests/emulator/terminal-harness-output.json"
```
- Multi-case terminal regression command (Pass 6 comprehensive mock suite — 36 cases):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass6.cases.json" `
  -ForceMock `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```
- Archive<->RAM swap fidelity command (deterministic dispatch/return simulation):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass2.cases.json" `
  -ForceMock `
  -ArchiveRamDispatchModulePath "C:\path\to\CEALG.8xp" `
  -ArchiveRamSwapRamCapacityBytes 65536 `
  -ArchiveRamFailurePhase none `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```
- Manifest-backed packaged-suite archive/load stress command (repeated load/unload across the shipped `.8xp` package):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass10.cases.json" `
  -ForceMock `
  -ModuleProgramPathSource manifest_transfer_suite `
  -ArchiveRamDispatchModulePath "deploy\hardware-ready\CECLIC.8xp" `
  -ArchiveRamRapidSwapCycles 2 `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```
- Manifest-backed constrained package-pressure command (reserve swap RAM and inspect deterministic failures):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass10.cases.json" `
  -ForceMock `
  -ModuleProgramPathSource manifest_transfer_suite `
  -ArchiveRamDispatchModulePath "deploy\hardware-ready\CECLIC.8xp" `
  -ArchiveRamSwapRamCapacityBytes 45000 `
  -ArchiveRamReservedRamBytes 5000 `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```
- Multi-case terminal regression command (Pass 3 strict hash verification):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-terminal-regression.ps1 `
  -CaseFile "tests/emulator/terminal-regression.pass3.cases.json" `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -NoMockFallback `
  -SummaryPath "tests/emulator/terminal-regression-summary.json"
```
- Baseline-capture command for pass-4 placeholder CRC workflows:
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
- Stabilized baseline-capture command for pass-5 CRC consensus workflows:
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
- Pre-hardware validation gate command (strict):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode strict `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```
- Pre-hardware validation gate command (minimal):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode minimal `
  -SkipDesktopStage `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```
- Optional strict CEmu gate extension (when ROM/autotester are available):
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-prehardware-validation-gate.ps1 `
  -Mode strict `
  -EnableCEmuChecks `
  -RomPath "$env:CEMU_ROM_PATH" `
  -AutotesterPath "$env:CEMU_AUTOTESTER_PATH" `
  -OutputPath "tests/emulator/outputs/prehardware-validation-gate.summary.json"
```
- Pass criterion:
- AutoTester exit code `0`
- Stable screen/VRAM hash checks for smoke scenarios
- `scripts/cemu_pipeline.py` writes a summary with `rom.integrity_ok=true`, no ROM-validation blockers, and `accuracy_summary.failed_count=0`
- Harness `success=true` with `mode=cemu`
- Regression runner `totals.failed=0` with exit code `0` and `actual.cemu_hash.matched=true` for cases that declare `expect.cemu_hash`
- Archive<->RAM simulations (when requested) report `actual.archive_ram_swap.pass=true` with dispatch/return telemetry in summary output
- Manifest-backed packaged-suite stress runs report `options.module_program_paths_source=manifest_transfer_suite`, include the full shipped support-module count in `actual.archive_ram_swap.modules.requested_count`, preserve effective-capacity / dispatch-partition telemetry under constrained pressure, and prefer exact `deploy/hardware-ready/` artifacts before stale `bin/` copies when both exist.
- Pre-hardware gate returns `exit_code=0` with `totals.required_failed=0` in `tests/emulator/outputs/prehardware-validation-gate.summary.json`

Current status on 2026-03-27:
- The PowerShell harness lane is green locally:
  - `tools/emulator/tests/run-terminal-harness-tests.ps1` -> 40 passed, 0 failed
  - `tools/emulator/tests/run-terminal-regression-tests.ps1` -> 25 passed, 0 failed
  - `tools/emulator/tests/run-cemu-foundation-tests.ps1` -> 3 passed, 0 failed
  - `tools/emulator/tests/run-comprehensive-autotest-tests.ps1` -> 5 passed, 0 failed
- The Python hardening lane is green locally:
  - `python -m pytest tests/cemu -q` -> 32 passed
- The TypeScript real-ROM smoke lane is green locally:
  - `npm run --silent test:cemu -- --json` -> status `passed`
- CI wiring now has two emulator layers:
  - `.github/workflows/test.yml` always runs `python -m pytest tests/cemu -q` on hosted Ubuntu for the fake-backed hardening suite plus real-lane skip validation.
  - `.github/workflows/test.yml` also defines a self-hosted Windows `cemu-headless` job, gated by repository variable `CEMU_HEADLESS_ENABLED=true` or workflow-dispatch input `run_cemu_headless=true`, that runs the Python validation wrapper and the TypeScript headless smoke suite against the real ROM/toolchain.
  - `.github/workflows/test.yml` now defines a separate self-hosted Windows `cemu-release-candidate` job, gated by repository variable `CEMU_RELEASE_CANDIDATE_GATE_ENABLED=true` or workflow-dispatch input `run_release_candidate_gate=true`, that runs `python scripts/cemu/release_candidate_gate.py` against the real ROM/toolchain.
- The self-hosted headless and release-candidate jobs both write evidence into `${RUNNER_TEMP}` instead of tracked repo paths, which removes worktree churn and reduces cross-run artifact collisions.
- The release-candidate gate now also enforces archive lifecycle stability by checking that `transfer_count` and `loaded_archive_count` stay consistent across the arTIfiCE handoff, the CalcEngine launch verifier, and all repeated attempts before declaring `release_candidate_ready=true`.
- The release-candidate evidence root is now pruned for stale single-attempt artifacts and out-of-range `attempt-*` directories before each rerun, so release artifacts reflect only the current execution.
- `tests/cemu/ci-config/cemu-validate-config.json` is now repository-relative instead of workstation-absolute, so the same wrapper/config pair can run on CI without local path edits.

Current status on 2026-04-04:
- `python -m pytest tests/cemu -q` is green locally at `192 passed, 9 skipped`.
- `./scripts/cemu-validate.ps1 -Mode full` completed successfully on the main repo and wrote a fresh `tests/cemu/outputs/cemu-pipeline-summary.json` at `2026-04-04T09:07:11-0700`.
- That full pipeline snapshot confirms the real-ROM toolchain is available and the manifest-backed transfer package is intact at `43` modules + `4` runtime dependencies = `47` staged transfers.
- The same summary reports every major full-pipeline slice green: `23/23` launch + transition checkpoints, `14/14` visible-smoke cases (`28` assertions), `19/19` accuracy cases, `35/35` arithmetic-battery cases, `28/28` module-battery cases, `23/23` real edge cases, `2/2` recovery cases, and `2/2` fake-only hardening cases.
- A fresh **Windows-side** rerun of `python .\scripts\cemu\release_candidate_gate.py` at `2026-04-04T09:12:46-0700` again produced `release_candidate_ready=true`, `stable=true`, and `archive_lifecycle_stability.stable=true`.
- On this workstation, the release-candidate gate should be invoked from Windows PowerShell / Windows Python. A direct WSL/Linux-Python invocation can false-fail the availability stage with `autotester.exe is unavailable` even though the Windows toolchain is installed.
- Physical calculator confirmation is still the remaining top-level validation blocker, but the automated real-ROM / arTIfiCE / repeated-smoke evidence is currently green.

Pass 93 addition:
- The arTIfiCE-aware OS 5.8 runners now accept `-BootstrapArtificeAsset`, which attempts to fetch/stage the official upstream arTIfiCE payload through `scripts/get-artifice-asset.ps1` before declaring the jailbreak lane blocked on a missing local asset.
- The bootstrap attempt is persisted into the artifice runner summary so CI/manual logs show whether the lane used an already-staged asset or had to recover the prerequisite dynamically.
Pass 89 additions:
- `scripts/cemu_pipeline.py` now validates the ROM before launch with size, signature, CRC32, SHA-1, and SHA-256 checks.
- The same pipeline now reports detected TI firmware metadata, including the mapped firmware family when the ROM fingerprint or embedded version strings are recognized.
- A new locked accuracy baseline lives at `tests/cemu/cemu_accuracy_baseline.json` and drives canonical arithmetic, trigonometric, logarithmic, statistics, and calculus probes.
- The pipeline summary now distinguishes unique-CRC accuracy cases from shared-CRC cases so lower-specificity baselines are visible instead of implied.

## Tier 3: Hardware Confirmation Lane (manual/periodic)

- Trigger: release candidate tags or planned validation runs
- Matrix:
- OS 5.3, 5.5, 5.6, 5.7+
- Device checks:
- launch success
- input/editing
- representative solve/integrate/limit workflows
- output rendering integrity

## Proposed CI Wiring

1. Keep `.github/workflows/test.yml` for Tier 1.
2. Keep the hosted `cemu-python` job in `.github/workflows/test.yml` as the always-on emulator hardening lane.
3. Enable the self-hosted `cemu-headless` job in `.github/workflows/test.yml` by setting repository variable `CEMU_HEADLESS_ENABLED=true` on the Windows runner installation that already has `rom/Rom.rom`, `autotester.exe`, and `cemu-headless.exe`.
4. Enable the self-hosted `cemu-release-candidate` job in `.github/workflows/test.yml` by setting repository variable `CEMU_RELEASE_CANDIDATE_GATE_ENABLED=true` when continuous real-ROM release-candidate evidence is desired, or use `workflow_dispatch` for manual one-off runs.
5. Gate release workflow so Tier 2 must pass before packaging after the self-hosted lanes are marked required in branch protection.

## Artifact and Baseline Strategy

- Store AutoTester templates in `tests/emulator/`.
- Keep expected CRC hashes in version control.
- Use `tests/emulator/terminal-regression.pass3.cases.json` as the placeholder scaffold for fresh captures, then commit the generated baselined case file before enabling or refreshing the strict lane in CI.
- Current captured file: `tests/emulator/terminal-regression.pass3.baselined.cases.json` (captured on 2026-03-11 from `rom/Rom.rom` with pass-5 consensus settings and re-verified in strict mode).
- CEmu pipeline accuracy baseline: `tests/cemu/cemu_accuracy_baseline.json`.
- Prefer pass-5 capture stabilization in CI/manual refreshes (`-BaselineCaptureSampleCount` + `-BaselineCaptureConsensus`) to avoid persisting one-off CRC outliers.
- Update hashes only when UI/result changes are intentional.
- Collect screenshots (`-s` mode) as optional CI artifacts for failed runs.
- Collect `tests/emulator/terminal-harness-output.json` for deterministic terminal assertions.
- Collect `tests/emulator/terminal-regression-summary.json` for multi-case deterministic assertions.
- Collect `tests/emulator/outputs/prehardware-validation-gate.summary.json` for required-stage gate decisions and per-stage diagnostics.
- Collect `tests/cemu/outputs/cemu-pipeline-summary.json` for ROM metadata plus the visible-smoke, accuracy-audit, and module-dispatch transition reports.
- Collect the dedicated release-candidate workflow artifact `cemu-release-candidate-artifacts`, which contains `release-candidate-confidence.json` plus the full `release-candidate-gate/` evidence directory captured from the self-hosted real-ROM gate run.
- Review `release-candidate-confidence.json` for both `stability` and `archive_lifecycle_stability` before treating the real-ROM gate as deterministic release evidence.
- Review `release-candidate-confidence.json.capability_depth` for the broader pre-hardware evidence stack: it now summarizes the first-try confirmation checklist, launch/menu visual-surface smoke coverage, direct matrix-literal module-dispatch proof, and archive-lifecycle stability in one machine-readable block so the remaining hardware-confidence gap is explicit instead of implied.
- When a launch checkpoint came from the delegated `run-artifice-smoke.ps1` fallback, also inspect `screen_payload.observed_crc_source`: `reported` means the delegated summary supplied the CRC directly, while `inferred_expected_crc` means the pass row omitted a raw CRC and the checkpoint value was filled from the locked baseline for continuity/reporting.

## Failure Triage

1. Re-run Tier 2 locally with the same ROM and config.
2. If only hashes changed, inspect screenshot artifact.
3. If behavior changed, add/adjust targeted unit tests in `tests/test_main.c`.
4. If emulator-only issue, capture CEmu and AutoTester versions in bug report.

## Remaining Gaps

- Archive<->RAM swap edge cases are still simulator-driven in the PowerShell harness. `partial_load`, `memory_pressure`, and `archive_full` are now regression-tested, but they are not yet observable through the real `cemu-headless` wrapper.
- Real STO/RCL and genuine calculator OOM pathways are still fake-backed in `tests/cemu/edge-cases/fake-hardening-cases.json` because the wrapper key map does not expose those flows.
- Physical calculator confirmation remains outside CI and is still required before release sign-off.
- The release workflow is not automatically chained to the self-hosted CEmu lane until that job is enforced through repository branch protection / required checks.
