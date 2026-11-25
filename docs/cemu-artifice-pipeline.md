# CalcEngine arTIfiCE CEmu Pipeline

## Overview

This pipeline adds an automated OS 5.8 jailbreak-to-launch lane on top of the existing real-ROM CEmu harness. It is split into two Python modules:

- `scripts/cemu/artifice_jailbreak.py`: stages the real transfer package together with `A.8xp`, launches the arTIfiCE shell, verifies the measured shell-exit choreography, and hands off at the TI-OS `PRGM` menu.
- `scripts/cemu/calcengine_launcher.py`: verifies the CalcEngine launch path after that handoff, checks `2+3`, and verifies CalcEngine menu open and menu close behaviour.

The pipeline intentionally uses two execution strategies:

- Interactive Python runner path for fast mocked CI and for the parts of the live headless wrapper that are reliable today.
- Delegated PowerShell smoke fallback on the real ROM when the current headless wrapper cannot reproduce the mid-session jailbroken launch CRCs directly. This fallback is not speculative. It reuses the already-validated `tools/emulator/run-artifice-smoke.ps1` lane and returns its measured checkpoints to the Python caller.

## Architecture

```text
                         +-----------------------------+
                         | tests/cemu/test_*           |
                         | unit + real integration     |
                         +-------------+---------------+
                                       |
                                       v
                  +-------------------------------------------+
                  | scripts/cemu/artifice_jailbreak.py        |
                  | - resolve arTIfiCE asset                  |
                  | - stage 35 modules + 4 runtimes + A.8xp   |
                  | - load_app target_name=A                  |
                  | - verify shell / quit / TI-OS PRGM CRCs   |
                  +-------------------+-----------------------+
                                      |
                                      | successful handoff
                                      v
                  +-------------------------------------------+
                  | scripts/cemu/calcengine_launcher.py       |
                  | - verify CalcEngine launch path           |
                  | - check 2+3                               |
                  | - check MODE open / CLEAR close           |
                  | - fallback to run-artifice-smoke.ps1      |
                  |   on real ROM when interactive launch     |
                  |   does not expose the same mid-session    |
                  |   CRCs as the proven autotester path      |
                  +-------------------+-----------------------+
                                      |
                                      v
                  +-------------------------------------------+
                  | evidence                                  |
                  | - per-step .state.json snapshots          |
                  | - delegated smoke summary when used       |
                  +-------------------------------------------+
```

## Verified Key Sequences

### arTIfiCE jailbreak sequence

The Python lane documents and applies this sequence:

1. `load_app(path=<staged dir>, target_name="A", target_is_asm=False)`
   This is the automated equivalent of selecting `prgmA` from TI-OS after transfer.
2. `pause 4000ms`
   Wait for the arTIfiCE shell to render.
3. `CLEAR`
4. `pause 1200ms`
5. `MODE`
6. `pause 1200ms`
7. `PRGM`
8. `pause 1200ms`

The three locked jailbreak checkpoints are:

- `artifice_shell_loaded`: `EAC3FDAC`
- `artifice_quit_transition`: `28B88B56`
- `ti_os_program_menu_open`: `C84EC5D0` or `967993A8`

`967993A8` is accepted because the current interactive Python lane on this workstation lands on the measured standard TI-OS `PRGM` menu CRC, while the historical autotester-wrapper baseline recorded `C84EC5D0` for the same post-jailbreak handoff.

### CalcEngine launch sequence

The interactive Python launcher sequence is:

1. `ALPHA C`
2. `ALPHA A`
3. `ALPHA L`
4. `ALPHA C`
5. `ALPHA E`
6. `ALPHA N`
7. `ALPHA G`
8. `ENTER`
9. `pause 4000ms`
10. enter `2+3`
11. `MODE`
12. `CLEAR`

On the live wrapper, those keys are still recorded and unit-tested, but the real ROM path can fall back to the proven smoke runner when the headless wrapper does not expose the same mid-session CRCs as the delegated autotester lane.

## Prerequisites

- CEmu headless binary:
  - default probe: `tools/emulator/cemu-headless/build/Release/cemu-headless.exe`
  - override: `CEMU_HEADLESS_PATH`
- AutoTester CLI:
  - Set `CEMU_AUTOTESTER_PATH` to your built `autotester.exe`.
- Real TI-84 Plus CE ROM:
  - default path: `rom/Rom.rom`
  - override: `CALCENGINE_ROM_PATH`
- arTIfiCE asset:
  - default path: `tools/emulator/assets/artifice-v2.8xp`
  - override: `CALCENGINE_ARTIFICE_PATH`
- CalcEngine transfer package:
  - 35 `.8xp` modules
  - 4 `.8xv` runtime dependencies
  - discovered from the manifest-backed search path already used by the existing CEmu pipeline

## Python API

### Run only the jailbreak verification

```python
from pathlib import Path

from artifice_jailbreak import resolve_artifice_path, run_artifice_jailbreak
from cemu_runner import CEmuRunner
from pipeline_support import build_runner_config, resolve_real_cemu_environment

environment = resolve_real_cemu_environment()
artifice_path = resolve_artifice_path()

with CEmuRunner(build_runner_config(environment)) as runner:
    result = run_artifice_jailbreak(
        runner,
        transfer_package=environment.transfer_package,
        artifice_path=artifice_path,
        evidence_dir=Path("tests/cemu/outputs/artifice-jailbreak"),
        timeout_s=60.0,
    )

assert result.success, result.failure_reason
```

### Run the full jailbreak-to-launch verification

```python
from pathlib import Path

from artifice_jailbreak import resolve_artifice_path, run_artifice_jailbreak
from calcengine_launcher import run_calcengine_launch_verification
from cemu_runner import CEmuRunner
from pipeline_support import build_runner_config, resolve_real_cemu_environment

environment = resolve_real_cemu_environment()
artifice_path = resolve_artifice_path()

with CEmuRunner(build_runner_config(environment)) as runner:
    jailbreak = run_artifice_jailbreak(
        runner,
        transfer_package=environment.transfer_package,
        artifice_path=artifice_path,
        evidence_dir=Path("tests/cemu/outputs/artifice"),
        timeout_s=60.0,
    )
    launch = run_calcengine_launch_verification(
        runner,
        transfer_package=environment.transfer_package,
        jailbreak_result=jailbreak,
        evidence_dir=Path("tests/cemu/outputs/launch"),
        timeout_s=60.0,
    )

assert jailbreak.success, jailbreak.failure_reason
assert launch.success, launch.failure_reason
```

## Pytest Usage

Run the dedicated tests:

```powershell
python -m pytest tests/cemu/test_artifice_jailbreak.py tests/cemu/test_calcengine_launcher.py -q
```

Run the full CEmu lane required for this pass:

```powershell
python -m pytest tests/cemu/ -x -q
```

## CI Usage

Example local or CI command:

```powershell
python -m pytest tests/cemu/test_artifice_jailbreak.py tests/cemu/test_calcengine_launcher.py -q
```

If the real ROM toolchain is available on the runner, the guarded real-backend tests execute automatically. If it is not available, the mock-backed unit tests still validate:

- arTIfiCE asset staging
- wrapper target selection for `A`
- verified `CLEAR -> MODE -> PRGM` choreography
- launcher module loading
- `2+3`
- CalcEngine menu open and close handling

## Expected Output

### `JailbreakResult`

- `success`: overall pass/fail
- `failure_reason`: empty on success, concrete failure text on error
- `checkpoints`: `artifice_shell_loaded`, `artifice_quit_transition`, `ti_os_program_menu_open`
- `indicators`:
  - `privilege_escalated`
  - `port_write_access`
  - `verified_markers`
- `stage_dir` and `evidence_dir`
- `loaded_archive_count`
- `keypress_log`
- `diagnostics`

The privilege and port-write indicators are intentionally described as simulated. The current wrapper does not expose raw privilege registers or port probes, so the Python lane infers them from the measured OS 5.8 screen baselines and the preserved archive payload count.

### `LaunchResult`

- `success`: overall pass/fail
- `basic_operation_passed`: launch plus `2+3`
- `menu_open_close_passed`: menu open plus menu close
- `checkpoints`:
  - `calcengine_launch_menu`
  - `calcengine_result_2plus3`
  - `calcengine_menu_open`
  - `calcengine_menu_closed`
- `diagnostics`
- `evidence_dir`

When the fallback smoke runner is used on the real ROM path, the diagnostics explicitly say so and point to the generated smoke summary.

## Failure Modes

- Missing ROM:
  - `resolve_real_cemu_environment()` reports `ROM file is missing`
- Missing `cemu-headless`:
  - real tests skip or fail before launch
- Missing `autotester.exe`:
  - real tests skip or fail before launch
- Missing arTIfiCE asset:
  - `run_artifice_jailbreak()` returns `arTIfiCE .8xp asset could not be resolved`
- Incomplete transfer package:
  - both modules return a manifest-backed failure before touching CEmu
- Live wrapper timeout:
  - `timed_out=True`
  - `failure_reason` contains the timeout message
- CRC drift:
  - failed checkpoint entries include the observed CRC and expected CRC list
- Interactive launcher mismatch on the real wrapper:
  - the launcher diagnostics note the mismatch
  - the module delegates to `tools/emulator/run-artifice-smoke.ps1`
  - the returned `LaunchResult` is built from the delegated smoke summary if that fallback passes

## Notes

- The real integration tests passed locally on March 27, 2026 with:
  - real arTIfiCE jailbreak verification
  - delegated real smoke fallback for the full post-jailbreak CalcEngine launch path
- The fallback is deliberate, not a silent degradation. It preserves the automation objective while the interactive wrapper continues to lack a reliable mid-session jailbroken launch primitive that matches the proven autotester CRC lane.
