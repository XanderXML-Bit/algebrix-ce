# CalcEngine CEmu Pipeline

Current package: 35 official programs plus four runtime AppVars and arTIfiCE, totaling 40 calculator files.
Dated replay results below remain historical evidence and do not validate the refreshed binaries.

This directory contains the hardened TI-84 Plus CE ROM validation lane for CalcEngine. It validates the shipped transfer package against the real `cemu-headless` wrapper path, adds explicit edge-case coverage, adds visible transition coverage, and includes a CI wrapper that emits a machine-readable JSON summary.

## Scope

- Real-ROM boot, smoke, visible transition, module-load, accuracy, edge-case, and recovery validation
- Fake-backend hardening for wrapper gaps that cannot be proven on the current real wrapper interface
- CI entrypoint and JSON summary generation

The pipeline does not modify calculator source, ROM contents, or other test directories.

## Key Files

- [conftest.py](../../tests/cemu/conftest.py): pytest fixtures for real and fake runners
- [fake_backend.py](../../tests/cemu/fake_backend.py): reusable fake CEmu server with timeout, memory, and synthetic OOM hooks
- [cemu_hardening.py](../../tests/cemu/cemu_hardening.py): case loader, timeout-safe suite runner, module verification, and summary helpers
- [run_cemu_validation.py](../../tests/cemu/run_cemu_validation.py): Python orchestration entrypoint for the hardened pipeline
- [release_candidate_gate.py](../../scripts/cemu/release_candidate_gate.py): focused real-ROM release-candidate confidence gate that chains the arTIfiCE launch proof with the minimum smoke suite, repeats the proof across multiple cold attempts, and emits one aggregate machine-readable JSON report plus per-attempt summaries
- [visible_smoke.py](../../tests/cemu/visible_smoke.py): visible-flow transition loader and two-stage screen assertion runner
- [visible-smoke-cases.json](../../tests/cemu/visible-smoke-cases.json): launcher, arithmetic, and representative module-dispatch transition baselines
- [real-edge-cases.json](../../tests/cemu/edge-cases/real-edge-cases.json): locked real-ROM CRC baselines for edge and recovery cases
- [fake-hardening-cases.json](../../tests/cemu/edge-cases/fake-hardening-cases.json): fake-only hardening cases for unsupported wrapper capabilities
- [cemu-validate-config.json](../../tests/cemu/ci-config/cemu-validate-config.json): default timeout, paths, and exit-code configuration
- [release-candidate-gate.json](../../tests/cemu/ci-config/release-candidate-gate.json): selected smoke cases, repeated-attempt policy, and artifact paths for the release-candidate gate
- [cemu-validate.ps1](../../scripts/cemu-validate.ps1): CI wrapper script

## Real-ROM Requirements

- `rom/Rom.rom` (you supply it; never committed)
- `cemu-headless.exe`
- `autotester.exe`
- Complete manifest-backed transfer package: 35 `.8xp` modules and 4 `.8xv` runtime dependencies

Dependency resolution still uses the existing manifest-driven search path:

1. Explicit CLI or environment override
2. `C:\Users\user\Downloads\CalcEngine`
3. Repo build outputs under `bin\`
4. A local copy of the CE C library sources under `_research_tmp\toolchain\src\`, if you keep one (not included)

## What The Hardened Lane Covers

- ROM validation and availability checks
- Transfer-package integrity checks against the manifest size and CRC values
- Module loading verification for all 35 `.8xp` files plus 4 runtime `.8xv` files
- Owner smoke checklist coverage on the real ROM lane:
  - direct launch to the CalcEngine prompt
  - `2+3`
  - return to prompt after dismissing the result
  - `6*7`
  - `(-)5+10`
  - `3.14*2`
  - `(4+5)*3`
  - `2^8`
  - divide-by-zero error rendering
  - menu open/close through `PRGM`
- Visible two-stage smoke assertions for:
  - launcher prompt to PRGM menu
  - arithmetic result to prompt recovery
  - divide-by-zero error to prompt recovery
  - representative module-dispatch result to prompt recovery
- Existing 17-case accuracy audit
- Edge cases:
  - division by zero
  - overflow-path baseline
  - underflow/small-result baseline
  - 50+ character expression
  - rapid sequential operations
  - negative-number handling
  - scientific-notation display path
  - `log(0)` and `log(-1)`
  - `sin`, `cos`, and `tan` at `0`, `pi/2`, `pi`, and `2pi`
- Recovery cases:
  - syntax-error recovery
  - interrupted-operation recovery
- Fake-only hardening cases:
  - STO/RCL orchestration coverage
  - synthetic out-of-memory recovery
  - timeout handling for hung captures
- Repeated release-candidate proof:
  - default `3` full real-ROM attempts through jailbreak, launch, and direct smoke
  - required `3/3` passing attempts before `release_candidate_ready=true`
  - identical smoke-test CRC signatures enforced across all repeated attempts
  - archive lifecycle counts must remain stable from the arTIfiCE jailbreak handoff through the CalcEngine launch verifier and across all repeated attempts
  - when checkpoint snapshots expose `transfer_files`, the gate also requires deterministic manifest-backed archive inventory order/signatures and count-to-inventory consistency at both stages

## Wrapper Limits

The current real wrapper still exposes CRC-only screen capture and a limited key map. Because of that:

- Real-ROM assertions are screen-CRC based, not OCR based.
- Real `STO/RCL` keys are not yet exposed by the wrapper, so that lane is covered on the fake backend only.
- A genuine real-ROM out-of-memory key path is not currently automatable through the wrapper key map, so recovery logic is covered with a scripted fake-backend OOM case.
- Trig boundary cases use decimal approximations for `pi/2`, `pi`, and `2pi` because the wrapper key map does not currently expose a dedicated `pi` key.

## Running The Tests

Run the pytest lane:

```powershell
python -m pytest tests/cemu -q
```

Run only the hardening tests:

```powershell
python -m pytest tests/cemu/test_cemu_hardening.py -q
```

Run the Python validation driver directly:

```powershell
python tests/cemu/run_cemu_validation.py
```

Run fake-only validation:

```powershell
python tests/cemu/run_cemu_validation.py --mode fake-only
```

Run the CI wrapper:

```powershell
.\scripts\cemu-validate.ps1
```

Run the release-candidate confidence gate from a **Windows PowerShell / Windows Python** context so `autotester.exe` resolves correctly on this workstation:

```powershell
Set-Location 'C:\Users\user\src\algebrix-ce'
python .\scripts\cemu\release_candidate_gate.py
```

Override the repeated-attempt policy when you need a narrower local probe:

```powershell
Set-Location 'C:\Users\user\src\algebrix-ce'
python .\scripts\cemu\release_candidate_gate.py --attempts 1 --required-passing-attempts 1
```

For ad-hoc local validation without dirtying the tracked `tests/emulator/outputs/` evidence files, send the report and evidence tree to a temp directory:

```powershell
Set-Location 'C:\Users\user\src\algebrix-ce'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$root = Join-Path $env:TEMP ("calcengine-dev-worker\release-candidate-$stamp")
python .\scripts\cemu\release_candidate_gate.py `
  --output (Join-Path $root 'release-candidate-confidence.json') `
  --evidence-dir (Join-Path $root 'release-candidate-gate')
```

> Note: invoking `scripts/cemu/release_candidate_gate.py` directly from WSL/Linux Python can false-fail the availability stage on this machine with `autotester.exe is unavailable` even though the Windows toolchain is installed. The gate should be treated as a Windows-side real-ROM lane.

## CI Usage

Current status on 2026-04-04:

- `python -m pytest tests/cemu -q` passes locally at `192 passed, 9 skipped`.
- `./scripts/cemu-validate.ps1 -Mode full` completed successfully on the main repo with a fresh `tests/cemu/outputs/cemu-pipeline-summary.json` generated at `2026-04-04T09:07:11-0700`.
- That fresh full pipeline proves the manifest-backed transfer package is intact (`43` modules + `4` runtime dependencies = `47` staged transfers) and that the current real-ROM wrapper/toolchain is available.
- The same summary reports all major validation slices green: `23/23` launch + transition checkpoints, `14/14` visible-smoke cases (`28` assertions), `19/19` accuracy cases, `35/35` arithmetic-battery cases, `28/28` module-battery cases, `23/23` real edge cases, `2/2` recovery cases, and `2/2` fake-only hardening cases.
- The real headless ROM lane remains defined in `.github/workflows/test.yml` as job `cemu-headless`, gated by repository variable `CEMU_HEADLESS_ENABLED=true`.
- The real-ROM release-candidate gate remains defined in `.github/workflows/test.yml` as separate job `cemu-release-candidate`, gated by repository variable `CEMU_RELEASE_CANDIDATE_GATE_ENABLED=true` or the manual `workflow_dispatch` input `run_release_candidate_gate=true`.
- `scripts/cemu-validate.ps1` resolves `python.exe` or `py -3` automatically and passes absolute paths to the validation driver.
- `tests/cemu/ci-config/cemu-validate-config.json` uses repository-relative paths, so the same wrapper/config pair works on developer machines and CI runners.
- The TypeScript smoke runner defaults to temp artifacts under CI instead of mutating tracked files in `tests/cemu/outputs/`.
- The dedicated self-hosted release-candidate job writes the confidence report plus the full `release-candidate-gate` evidence tree into a separate runner-temp root and uploads it as artifact `cemu-release-candidate-artifacts`.
- A fresh Windows-side rerun of `scripts/cemu/release_candidate_gate.py` on `2026-04-04T09:12:46-0700` again produced `release_candidate_ready=true`, `stable=true`, and `archive_lifecycle_stability.stable=true` with the arTIfiCE jailbreak and launch-verification stages both succeeding at `47` transfers / `48` loaded archive entries.
- The release-candidate gate preserves one `summary.json` per attempt plus the aggregate root report, so failures include the exact attempt that broke repeated real-ROM confidence.
- Archive lifecycle enforcement now requires stable `loaded_archive_count` / `transfer_count` telemetry and deterministic manifest-backed inventory snapshots when the wrapper exposes `transfer_files`, while preserving count-only fallback compatibility for older/minimal snapshots.
- The release-candidate gate prunes stale single-attempt artifacts and out-of-range `attempt-*` directories before a rerun, so the evidence tree stays deterministic for release review.

The wrapper returns:

- `0`: all requested suites passed
- `1`: one or more suites completed but failed
- `2`: setup or runtime error prevented the requested validation mode from completing

Example CI step:

```powershell
.\scripts\cemu-validate.ps1 `
  -Mode full `
  -Output tests/cemu/outputs/cemu-pipeline-summary.json `
  -Snapshot tests/cemu/outputs/cemu-pipeline-snapshot.json
```

Self-hosted GitHub Actions run the same wrapper with runner-temp artifact paths:

```powershell
.\scripts\cemu-validate.ps1 `
  -Mode full `
  -Output "$env:RUNNER_TEMP\calcengine-cemu\cemu-pipeline-summary.json" `
  -Snapshot "$env:RUNNER_TEMP\calcengine-cemu\cemu-pipeline-snapshot.json"
```

The dedicated self-hosted release-candidate job runs only the focused gate and writes its own artifact root:

```powershell
python scripts/cemu/release_candidate_gate.py `
  --output "$env:RUNNER_TEMP\calcengine-cemu-release-candidate\release-candidate-confidence.json" `
  --evidence-dir "$env:RUNNER_TEMP\calcengine-cemu-release-candidate\release-candidate-gate"
```

Manual GitHub Actions trigger options:

- Set workflow-dispatch input `run_cemu_headless=true` to run the self-hosted full headless validation job.
- Set workflow-dispatch input `run_release_candidate_gate=true` to run only the self-hosted real-ROM release-candidate gate.
- Set repository variable `CEMU_HEADLESS_ENABLED=true` to keep the full self-hosted headless lane enabled on push and pull request events.
- Set repository variable `CEMU_RELEASE_CANDIDATE_GATE_ENABLED=true` to keep the dedicated release-candidate job enabled on push and pull request events.

## Summary Output

Default summary path:

- `tests/cemu/outputs/cemu-pipeline-summary.json` (written by a local run)

Default snapshot path:

- `tests/cemu/outputs/cemu-pipeline-snapshot.json` (written by a local run)

Default release-candidate confidence report:

- `tests/emulator/outputs/cemu-release-candidate-confidence.json` (written by a local run)

The JSON summary includes:

- environment availability and blockers
- transfer package metadata
- module verification results
- smoke checkpoint results
- visible smoke transition results
- accuracy audit results
- real edge-case results
- recovery results
- fake hardening results
- top-level success and exit code

The release-candidate confidence report includes:

- real-ROM availability and blockers
- locked ROM provenance under `availability.rom_validation`, including SHA-256 / CRC32, recognized-fingerprint status, model, OS version, firmware family, and any ROM warnings/errors
- one representative attempt payload for arTIfiCE jailbreak, CalcEngine launch, `2+3`, menu open/close, `6*7`, and base->matrix dispatch switching
- `attempts_summary` with configured, executed, passed, failed, and halted attempt counts
- `stability` metadata showing whether repeated smoke-test CRC signatures matched across attempts
- `archive_lifecycle` metadata showing the expected transfer count plus the observed jailbreak and launch archive counts for the selected attempt, and archive inventory details when checkpoint snapshots expose `transfer_files`
- `archive_lifecycle_stability` metadata showing whether the archive lifecycle signature matched across repeated attempts
- fallback launch checkpoints may include `screen_payload.observed_crc_source` / `screen_payload.observed_crc_inferred` so callers can distinguish a raw delegated CRC from a checkpoint value inferred from the locked baseline when the delegated smoke summary omits CRCs on passing rows
- one per-attempt `summary.json` file under the evidence tree
- aggregate release readiness and exit status

## Notes

- Each hardening case has its own timeout and defaults to `30` seconds through [cemu-validate-config.json](../../tests/cemu/ci-config/cemu-validate-config.json).
- Hung cases are force-failed and the runner is restarted for subsequent cases.
- Prompt recovery is intentionally asserted against a small family of locked prompt CRCs rather than one exact hash, because the real ROM exposes several stable prompt baselines depending on the immediately preceding interaction path.
- The real-ROM edge baselines in [real-edge-cases.json](../../tests/cemu/edge-cases/real-edge-cases.json) were captured on March 25, 2026 against the current local TI-84 Plus CE ROM and wrapper toolchain.
- Real archive swap, true OOM, and STO/RCL flows are still wrapper gaps; those remain covered by the PowerShell harness and fake-backend hardening cases until the headless backend exposes them directly.
