# CalcEngine CEmu Pipeline

Date: 2026-03-27

## Objective

This document defines the real-ROM, hardware-fidelity validation path for CalcEngine `.8xp` modules inside CEmu. It also records what already exists in the repository, what was found on this workstation, and which scripts form the supported execution path.

The short version:

- The repository already has a substantial CEmu integration.
- This workstation already has the required binaries for real execution.
- The foundation entrypoint is now `scripts/cemu-foundation.ps1`.
- The actual bar is **not** "the emulator is pretty good"; it is "we have covered enough of the real shipped artifact/ROM/jailbreak/input/output behavior that the first hardware attempt should be confirmation, not debugging."

## Configure the Windows runner

Run the examples from the repository root.

- Place your ROM at `rom/Rom.rom`.
- Set `CEMU_GUI_PATH` to your installed `CEmu.exe` if you use the GUI.
- Set `CEMU_AUTOTESTER_PATH` to your built `autotester.exe`.
- Build the headless wrapper at `tools/emulator/cemu-headless/build/Release/cemu-headless.exe`.
- Stage the manifest-matched package in `deploy/hardware-ready`. Pass `-DownloadsDir` if you staged it elsewhere.

## Existing Integration Already In The Repo

The main CEmu-related building blocks were already present before this pass.

### Environment and setup

- `scripts/setup-cemu.ps1`
  - Validates `rom/Rom.rom`
  - Resolves or downloads `CEmu.exe`
  - Resolves or documents how to build `autotester.exe`
  - Optionally runs a boot verification step

### Focused AutoTester smoke lane

- `tools/emulator/run-cemu-autotest.ps1`
  - Generates an AutoTester JSON config
  - Populates `transfer_files` with the launcher and split modules
  - Invokes `autotester.exe`

- `scripts/cemu-test-runner.ps1`
  - Wraps the focused AutoTester suite
  - Asserts:
    - CEmu boots
    - `.8xp` transfer happened
    - CalcEngine launches
    - `2+3`
    - `6*7`
    - prompt round-trip
    - menu reachability

- `tests/emulator/autotest.cemu-e2e.json`
  - Existing real-ROM focused hash suite
  - Already covers launch, `2+3`, `6*7`, `-5+10`, `3.14*2`, and divide-by-zero

### Headless real-ROM pipeline

- `scripts/cemu_pipeline.py`
  - Validates the ROM
  - Resolves the transfer package from the manifest
  - Stages the full package
  - Boots the repo `cemu-headless` wrapper
  - Runs smoke checkpoints and the accuracy audit

- `scripts/cemu/cemu_runner.py`
  - JSON-line client for `cemu-headless`
  - Supports `load_rom`, `load_app`, `inject_key`, `step`, `get_screen`, `snapshot`, `get_state`, `reset`, `exit`

- `scripts/cemu/pipeline_support.py`
  - Manifest-backed transfer resolution
  - Program staging
  - Smoke checkpoint definitions
  - Expected CRC tracking

- `tests/cemu/`
  - Hardened real/fake validation lane
  - Visible-smoke cases
  - Edge-case batteries
  - CI wrapper config

### arTIfiCE / OS 5.8 jailbreak lane

- `tools/emulator/run-artifice-e2e-verification.ps1`
  - Runs the real-ROM jailbroken path
  - Compares captured hashes against a locked baseline

- `tools/emulator/CalcEmuArtificePipeline.psm1`
  - Builds the AutoTester template for `prgmA`
  - Stages arTIfiCE plus the CalcEngine package
  - Encodes the `CLEAR -> MODE -> PRGM` exit choreography

- `tools/emulator/record-artifice-baseline.ps1`
  - Captures or refreshes the official six-screen baseline

- `tests/emulator/autotest.artifice-smoke.json`
  - Existing larger jailbroken smoke suite
  - Already covers launch, `2+3`, `6*7`, menu, settings, and more

### Transfer package integrity

- `tests/emulator/manifest.json`
  - Canonical package manifest
  - Currently declares:
    - `35` `.8xp` modules
    - `4` `.8xv` runtime dependencies
    - `README.md`
  - Includes file sizes and CRC32 values

## Upstream AutoTester CLI Contract

The authoritative upstream references are:

- `CE-Programming/CEmu` `tests/autotester/json_file_documentation.txt`
- `CE-Programming/CEmu` `tests/autotester/autotester_cli.cpp`

From the upstream `v2.0` source:

- AutoTester takes a single JSON config file path on the command line.
- `-d` enables debug mode.
- It loads the ROM, clears the home screen, transfers `transfer_files`, executes the configured sequence, and exits.
- When execution reaches the end of the sequence without JSON/runtime errors, the process exit code is the hash failure count.
- `0` means every hash passed.
- Non-zero means one or more hashes failed, or the config/runtime setup failed.

The JSON schema fields that matter for CalcEngine are:

- `rom`
  - Path to the ROM file.
- `transfer_files`
  - Files to send into the emulator after boot.
- `target`
  - Program name and `isASM` flag for `action|launch`.
- `delay_after_key`
  - Global key settle delay in milliseconds.
- `delay_after_step`
  - Global sequence-step settle delay in milliseconds.
- `sequence`
  - Sequential commands such as:
    - `action|launch`
    - `action|reset`
    - `delay|N`
    - `hash|name`
    - `hashWait|name`
    - `key|name`
    - `hold|name`
    - `release|name`
    - `sendCSC|...`
- `hashes`
  - Named CRC assertions over regions such as `vram_start` with sizes such as `vram_16_size`

CalcEngine currently relies on this upstream contract exactly. The repository does not implement a custom AutoTester format.

## Architecture

### Tier 0: Build artifacts

Inputs:

- `bin/launcher/CALCENG.8xp`
- split-module `.8xp` files under `bin/`
- runtime `.8xv` dependencies
- `tests/emulator/manifest.json`

Outputs:

- canonical transfer package
- manifest-backed package integrity proof

### Tier 1: Focused real-ROM smoke via AutoTester

Entry:

- `scripts/cemu-test-runner.ps1`

Back-end:

- `tools/emulator/run-cemu-autotest.ps1`
- upstream `autotester.exe`

Purpose:

- prove the minimum viable end-to-end lane on the real ROM
- prove launcher boot
- prove simple arithmetic
- prove prompt recovery
- prove menu reachability

Artifacts:

- generated AutoTester JSON
- delegated runner summary
- parsed per-hash summary
- top-level focused summary

### Tier 2: Manifest-backed real-ROM pipeline via `cemu-headless`

Entry:

- `scripts/cemu_pipeline.py`

Back-end:

- `tools/emulator/cemu-headless/build/Release/cemu-headless.exe`
- `scripts/cemu/cemu_runner.py`

Purpose:

- stage the full package from the manifest
- boot the real ROM through the repo wrapper
- inject keys programmatically
- capture CRC-based screen checkpoints
- run the arithmetic/accuracy battery
- write machine-readable artifacts for CI

Artifacts:

- `tests/cemu/outputs/cemu-pipeline-summary.json`
- `tests/cemu/outputs/cemu-pipeline-snapshot.json`

### Tier 3: arTIfiCE / OS 5.8 path

Entry:

- `tools/emulator/run-artifice-e2e-verification.ps1`

Back-end:

- upstream `autotester.exe`
- arTIfiCE v2 `.8xp` payload
- official real ROM

Purpose:

- prove the real TI-OS launch path when direct ASM launch is not the deployment path
- prove `prgmA`
- prove exit from arTIfiCE shell back to TI-OS
- prove launch of CalcEngine from TI-OS program selection
- prove simple post-jailbreak interaction

Artifacts:

- `tests/emulator/outputs/artifice-e2e.summary.json`
- `tools/emulator/baselines/artifice-e2e-baseline.json`

### Tier 4: Unified foundation wrapper

Entry:

- `scripts/cemu-foundation.ps1`

Purpose:

- provide one repo-facing command for the current foundation lanes
- run any of:
  - `Focused`
  - `Pipeline`
  - `Artifice`
  - `All`
- aggregate delegated stage summaries into one JSON report

Primary output:

- `tests/cemu/outputs/cemu-foundation.summary.json`

## How `.8xp` Modules Are Loaded Into CEmu

CalcEngine does not directly mutate CEmu archive state through a custom API. The loading path is:

1. Build or stage the `.8xp` and `.8xv` files.
2. Validate the package against `tests/emulator/manifest.json`.
3. Generate an AutoTester JSON config with `transfer_files`.
4. Boot the ROM.
5. Let AutoTester transfer the files into the running calculator session.
6. Launch the selected program through `action|launch` or the TI-OS/arTIfiCE interaction path.

Current repository proof of correct loading is:

- the generated config contains the expected transfer set
- the target program launches successfully on the real ROM
- the expected screen hashes pass

Important limitation:

- AutoTester does not expose a supported archive inventory API.
- That means archive residency is inferred from transfer plus successful launch, not from a direct archive listing.

## How The OS 5.8 arTIfiCE Sequence Works

CalcEngine currently encodes the jailbreak path in `tools/emulator/CalcEmuArtificePipeline.psm1`.

The verified sequence is:

1. Stage the normal CalcEngine package into a temporary transfer directory.
2. Copy the arTIfiCE payload into that same directory as `A.8xp`.
3. Generate an AutoTester config with `target.name = "A"` and `isASM = false`.
4. Run `action|launch` to execute `prgmA`.
5. Hash `artifice_shell_loaded`.
6. Send:
   - `key|clear`
   - `delay|1200`
   - `key|mode`
   - `delay|1200`
7. Hash `artifice_quit_transition`.
8. Re-open TI-OS program selection:
   - `key|prgm`
   - `delay|1200`
9. Hash `ti_os_program_menu_open`.
10. Alpha-type `CALCENG` from the PRGM menu.
11. Press `enter`.
12. Wait for CalcEngine to load.
13. Hash the CalcEngine launch screen and follow-up proof screens.

The critical correction already present in the repo is that a single `MODE` press is not treated as sufficient proof of a clean exit. The verified path is the measured `CLEAR -> MODE -> PRGM` choreography.

## How Keypress Injection Works

There are two active key injection paths.

### AutoTester sequence strings

Used by:

- `run-cemu-autotest.ps1`
- `cemu-test-runner.ps1`
- `run-artifice-e2e-verification.ps1`

Format:

- `key|2`
- `key|+`
- `key|enter`
- `key|mode`
- `hold|...`
- `release|...`
- `sendCSC|...`

Current CalcEngine-focused key vocabulary is captured in:

- `scripts/cemu/keypress_sequences.py`

This repo already normalizes keys such as:

- digits `0` through `9`
- `+`, `-`, `*`, `/`, `^`, `.`, `(`, `)`
- `(-)`
- `enter`
- `clear`
- `mode`
- `2nd`
- `alpha`
- `math`
- `apps`
- `prgm`
- arrows

### `cemu-headless` JSON-line commands

Used by:

- `scripts/cemu_pipeline.py`
- `tests/cemu/run_cemu_validation.py`

Commands:

- `{"cmd":"inject_key","key":"2"}`
- `{"cmd":"inject_key","key":"enter","duration_ms":2000}`
- `{"cmd":"step","cycles":480000}`

The repo wraps these through `CEmuRunner.send_sequence()`.

## How Screen Output Is Captured And Asserted

### Today: CRC-first assertions

The current validated path is CRC-based, not OCR-based.

AutoTester:

- `hash|name`
- `hashWait|name`
- hashes memory regions such as `vram_start`
- typically uses `vram_16_size`

`cemu-headless` wrapper:

- `get_screen` currently supports `format = "crc"`
- returns structured JSON with the observed CRC

Repository assertion models:

- focused smoke hashes in `tests/emulator/autotest.cemu-e2e.json`
- comprehensive AutoTester baselines under `tests/emulator/`
- visible smoke and hardening baselines under `tests/cemu/`
- arTIfiCE baseline under `tools/emulator/baselines/`

### Planned but not yet the current proof path

- native framebuffer extraction from `cemu-core`
- PNG export from `cemu-headless`
- OCR or text-grid assertions against the TI-OS screen

The repo already documents those as future native-core work, not current capability.

## Dependencies And Setup Requirements

Required:

- proprietary ROM at `rom/Rom.rom`
- official CEmu `v2.0` GUI binary
- standalone `autotester.exe` built from the matching upstream CEmu tag
- repo `cemu-headless.exe`
- Python for `scripts/cemu_pipeline.py` and `tests/cemu/run_cemu_validation.py`
- staged transfer package at `deploy/hardware-ready`

Required package contents:

- `35` `.8xp` files
- `4` `.8xv` runtime dependencies
- `README.md`

Optional but required for the jailbreak lane:

- local arTIfiCE `.8xp` asset, usually resolved from `tools/emulator/assets/artifice-v2.8xp`

Recommended setup commands:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup-cemu.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\cemu-foundation.ps1 -Mode Focused
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\cemu-foundation.ps1 -Mode Pipeline
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\cemu-foundation.ps1 -Mode Artifice
```

Run all foundation lanes:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\cemu-foundation.ps1 -Mode All
```

## Basic Automated Foundation Cases

The basic cases required by this task already exist and are now exposed through the foundation wrapper.

Focused real-ROM cases:

- launch to prompt
- `2+3`
- `6*7`
- `(-)5+10`
- `3.14*2`
- `(4+5)*3`
- `2^8`
- divide-by-zero
- prompt recovery after result dismissal
- menu open/close

The Python `cemu-headless` checklist now mirrors that owner-directed smoke list directly. The canonical sequential checkpoints are:

- `boot_home`
- `addition_result`
- `prompt_after_addition`
- `multiplication_result`
- `prompt_after_multiplication`
- `negative_result`
- `decimal_result`
- `parenthesized_result`
- `power_result`
- `divide_by_zero_error`
- `program_menu_open`
- `prompt_after_menu_close`

The actual baseline definitions live in:

- `tests/emulator/autotest.cemu-e2e.json`
- `scripts/cemu-test-runner.ps1`

## Known Constraints

- The ROM is proprietary and must never be committed or dumped.
- AutoTester does not provide a supported archive listing API.
- Current real assertions are CRC-based and tied to the exact ROM/UI state.
- Some calculator keys and advanced UI paths are still easier to prove through wrapper/fake coverage than through the current real key map.
- The repo `cemu-headless` binary is still wrapper-backed when native `cemu-core` is absent.

## Recommended Validation Order

1. Run `scripts/setup-cemu.ps1` if the environment has not been prepared.
2. Run `scripts/cemu-foundation.ps1 -Mode Focused`.
3. Run `scripts/cemu-foundation.ps1 -Mode Pipeline`.
4. Run `scripts/cemu-foundation.ps1 -Mode Artifice` if the arTIfiCE asset is present.
5. Run the broader hardened lane under `tests/cemu/` when deeper validation is needed.

## Conclusion

CalcEngine does not need a speculative CEmu plan anymore. The repo already contains:

- a real AutoTester smoke lane
- a manifest-backed headless real-ROM lane
- a verified OS 5.8 arTIfiCE lane
- hash baselines for launch, arithmetic, prompt recovery, and menu interaction

The remaining work for future passes is expansion and native-core fidelity, not initial bring-up.
