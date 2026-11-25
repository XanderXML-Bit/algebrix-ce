# CalcEngine Validation Coverage

> **Public source note:** the staged `deploy/hardware-ready/` transfer package is not part of this repository. Build the modules from source with CEdev as described in the [README](../README.md).

Current package: 35 official programs, four runtime AppVars, and arTIfiCE, totaling 40 calculator files.
The package contains 44 files including metadata.
Earlier measured counts and emulator results below describe historical packages, not this refreshed package.
Revalidate current screen baselines on the self-hosted real-ROM lane.

**Status:** HISTORICAL VALIDATION SNAPSHOT — not current hardware certification
**Historical sign-off date:** 2026-04-14
**Main branch HEAD at historical sign-off:** `8d4117b`

> This file preserves the 2026-04-14 validation/certification record as historical evidence only.
> Current main should be treated as **release-candidate / pre-hardware-confirmation** work until a newer real-device certification supersedes this snapshot.
> For current posture, see the [README status](../README.md#current-status). The [v2.0.0 physical test report](V2_HARDWARE_BUGS.md) records that build's failure.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Change Summary](#2-change-summary)
3. [Test Surface](#3-test-surface)
4. [Module Coverage](#4-module-coverage)
5. [LCD Baseline Validation](#5-lcd-baseline-validation)
6. [Error-Path Coverage](#6-error-path-coverage)
7. [Known Non-Issues and Deferred Items](#7-known-non-issues-and-deferred-items)
8. [Reproducing the Historical Sign-Off Snapshot](#8-reproducing-the-historical-sign-off-snapshot)
9. [Coordinator Log](#9-coordinator-log)
10. [Stack-Usage Build-Time Guard](#10-stack-usage-build-time-guard)

---

## 1. Executive Summary

This document preserves the historical 2026-04-14 validation sign-off snapshot that
originally declared CalcEngine hardware-ready. That sign-off is still useful as
an evidence record for what passed at the time, but it is **not** a statement
about the current `main` branch. Current `main` remains in pre-hardware-confirmation /
release-candidate validation until a newer successful real-device certification
supersedes this record.

| Metric | Value |
|--------|-------|
| Node/Vitest unit + integration tests | 34 pass, 33 skip (0 fail) |
| Python pytest integration tests | 185 pass, 108 skip (0 fail) |
| Build-time stack-usage guard | 41/41 regression tests PASS |
| Hardware emulator QA checks (autotester.exe v2.0) | 21/21 PASS |
| `2+3=5` end-to-end emulator confirmation | PASS (VRAM CRC `B9B5D5E0`) |
| LCD text assertion checks (desktop) | 59/59 PASS |
| Visual QA CRC fingerprints | 211/211 PASS |
| Emulator module coverage | 43/43 worker modules accounted for |
| UI flow validation | 5/5 flows verified (opt-in, requires real ROM) |

All validation runs that require hardware-path fidelity execute through the in-tree
headless CEmu fork at `tools/emulator/cemu-headless/`, which boots real TI-OS,
installs the arTIfiCE jailbreak, and launches CalcEngine from the `.8xp` bundle.
The desktop `bin/calcengine.exe` binary is used only for fast CAS-core smoke tests
and explicitly does not substitute for emulator-layer coverage.

---

## 2. Change Summary

The following pull requests constitute the validation work integrated during the
coordinator session. They are listed in merge order.

| PR | Title | Key additions |
|----|-------|---------------|
| #18 | TMPCALC.8xp added to deploy package | Deploy package completeness |
| #19 | Expand emulator validation to 20 expressions — 21/21 PASS | `2+3=5` confirmed on real ROM; full basic+advanced expression set |
| #20 | LCD text assertion suite — 59/59 pass | 59-expression desktop CAS smoke suite |
| #21 | Fidelity gate + TODO-comprehensive conflict resolution | Pass 62 keypad/LCD fidelity; release-candidate gate |
| #22 | diff() depth guard (50 levels) + re-enable 8 Pass 72 disambig tests | Stack-overflow protection; test regression fix |
| #23 | Visual QA sweep — 211/211 CRC fingerprints | Full VRAM fingerprint catalogue; 4 TI-OS screenshots |
| #24 | 43-module emulator coverage audit (13/43 to 25/43) | First direct module probe set; MODULE-COVERAGE-MATRIX.md |
| #25 | kb_On honored in check_cancel() + 7-class error-path suite | Hardware break-key fix; error-path emulator JSON |
| #27 | Headless UI-flow validation suite | 5 flows: menu, settings, history, help, custom functions; 7 draft bugs fixed |
| #28 | Emulator LCD CRC suite — 59-expr hardware-equivalent | Hardware-layer sibling to PR #20; desktop-only banner applied |
| #29 | Coordinator final sign-off (PRs #27–28) | VERSION.txt, certification log |
| #30 | Error-path regression suite baseline (Pass 63) | Key-token fixes (`^2`, `xton`); real CRC baselines; guard test |
| #32 | ASCII screen extraction + substring assertions | 95-glyph graphx OCR; `format="text"` in cemu-headless; dual CRC+text capture |
| #34 | Close 18-module coverage gap — 43/43 covered | `mat2()` parser extension; 18 new CRC probes; 4 nearest-neighbor documented |
| #35 | Coordinator iteration 10 log | VERSION.txt, certification log |

---

## 3. Test Surface

CalcEngine validation is organized into four independent layers. The layers are
complementary, not redundant: each catches a class of bugs the others cannot.

### 3.1 Vitest unit and integration tests

**Location:** `tests/*.test.ts`  
**Command:** `npm test`  
**Count:** 34 pass, 33 skip (opt-in real-hardware tests), 0 fail

These tests verify the TypeScript harness layer that bridges the host toolchain
to the CEmu pipeline: configuration resolution, key-sequence encoding,
screen-assertion logic, CRC correlation, and the UI-flow drivers. Five tests in
`tests/test_ui_flows.test.ts` exercise full CalcEngine UI flows (menu navigation,
settings persistence, history recall, help pages, custom-function definition) but
skip unless `CALCENGINE_ALLOW_REAL_CEMU_IN_CI=true` is set and the real ROM,
autotester, and headless binary are present.

### 3.2 Python pytest integration tests

**Location:** `tests/cemu/`  
**Command:** `python -m pytest tests/cemu -q`  
**Count:** 185 pass, 108 skip, 0 fail  
**Test files:** 25 (see listing below)

The pytest suite covers the full Python-side pipeline: CEmu process management,
arTIfiCE jailbreak sequencing, autotest JSON validation, CRC catalog integrity,
archive-swap correctness, module load/unload, keypad fidelity, LCD fidelity,
error-path template guards, release-candidate gate logic, and ROM validation.
Tests that require the real ROM or `cemu-headless.exe` skip cleanly when those
assets are absent; they never fall through to any substitute binary.

Key test files:

| File | Coverage area |
|------|---------------|
| `test_artifice_jailbreak.py` | arTIfiCE OS 5.8 lane; jailbreak sequencing |
| `test_artifice_pipeline_e2e.py` | End-to-end arTIfiCE + CALCENG launch |
| `test_autotest_crc_catalog.py` | CRC catalog integrity across all autotest JSON files |
| `test_calcengine_launcher.py` | CALCENG launch verification; module dispatching |
| `test_cemu_hardening.py` | Crash/timeout/recovery hardening |
| `test_cemu_headless_native.py` | Native CEmu core headless fork |
| `test_cemu_load_modules.py` | Archive swap contract for all 35 current modules |
| `test_error_paths_template.py` | Guard: no placeholder CRCs, no unsupported key tokens in error-path JSON |
| `test_fidelity_catalogs.py` | Keypad and LCD fidelity catalogs |
| `test_release_candidate_gate.py` | Release-readiness gate: all required suites present and green |
| `test_visible_smoke.py` | `2+3=5` emulator confirmation; visible smoke checkpoints |
| `test_stack_check.py` | Build-time stack-usage guard regression tests (64 tests, always run) |

### 3.3 Headless emulator full-stack tests

**Location:** `tests/emulator/`  
**Harness:** `tools/emulator/cemu-headless/` — the in-tree CEmu fork  
**Binary candidates:** paths under `tools/emulator/cemu-headless/` only. No PATH
lookup, no system GUI CEmu, no substitution. A hard `pytest.exit(2)` fires at
collection time if the pinned binary is absent.

These tests run the full TI-OS hardware path: boot TI-OS on a real ROM image,
install the arTIfiCE jailbreak, launch CalcEngine, inject key sequences, and
capture VRAM CRC fingerprints (and, on the native-core build, ASCII-OCR text).
The desktop binary is never consulted.

Two suites live here:

- **`test_lcd_text_emulator.py`** — 59 parametrized cases, one subprocess, one
  boot, 59 expression cycles. Asserts CRC against a captured baseline; when the
  native-core OCR extraction is available, also asserts text substrings from the
  PR #20 expression catalogue.
- **`autotest.module-coverage.json`** — 31-hash autotester script with current worker probe assignments
  (see Section 4). Its zero placeholders still require real-ROM capture.

### 3.4 Autotester.exe QA suite

**Tool:** `autotester.exe v2.0` (official CEmu autotest binary)  
**Launch method:** `CE_KEY_ASM + PRGM + CALCENG + ENTER` (`isASM:true`)  
**Suite:** `tests/emulator/autotest.comprehensive.json` — 211 hash assertions  
**Result:** 21/21 hardware emulator QA checks PASS; `2+3=5` confirmed (CRC `B9B5D5E0`)

The autotester suite injects real key sequences into TI-OS running on the real ROM
and compares VRAM CRC fingerprints of the top 16 pixel rows against recorded
baselines. It is the strictest hardware-equivalent validation layer.

---

## 4. Module Coverage

### 4.1 Summary

The current package contains 35 official `.8xp` programs, including the `CALCENG` launcher.
The matrix assigns probes to these programs without claiming a fresh real-ROM replay.

| Category | Count |
|----------|-------|
| Modules in manifest (`tests/emulator/manifest.json`) | 35 |
| Programs assigned direct emulator probes | 34 |
| Programs assigned a nearest-neighbor probe | 1 |
| Total inventory rows | 35 |

The suite file is `tests/emulator/autotest.module-coverage.json` (31 hashes,
788 sequence entries). Full per-module detail is in
[`tests/emulator/MODULE-COVERAGE-MATRIX.md`](../tests/emulator/MODULE-COVERAGE-MATRIX.md).

### 4.2 Module categories

| Category | Modules | Coverage status |
|----------|---------|----------------|
| Launcher | CALCENG | Direct (comprehensive suite) |
| Algebra core | CEALG, CEASLV, CEALGF | Direct (comprehensive suite) |
| Algebra advanced | CEALGSY, CEALGEO, CEASLA, CEAPDV | Direct (PR #24 / PR #34) |
| Calculus core | CECALC | Direct (comprehensive suite) |
| Calculus integral | CECINT, CECISM, CECIDF, CECIAD | Direct (PR #24 / PR #34) |
| Calculus integral advanced | CEIPFR, CEITRG, CEIUSB, CEIPRT, CEISTR | Direct (PR #24 / PR #34) |
| Calculus limits | CECLIM, CECLMB, CECLIC, CECLIA, CECLIR | Direct (PR #24 / PR #34) |
| Calculus ODE / sequences | CECODE, CECSEQ, CECPOL, CECPNM | Direct (PR #34) |
| Series | CECSER | Direct (PR #24) |
| Limits+series combined | CELIMS | Nearest-neighbor (routing bypass — see 4.3) |
| Base conversion | CEBASE | Direct (PR #34) |
| Matrix | CEMAT | Direct via `mat2()` constructor (PR #34) |
| Lists+complex | CELSCX | Direct (PR #34) |
| Statistics | CESTPRB | Direct (comprehensive suite / PR #24) |
| Finance | CEFIN | Direct (comprehensive suite) |
| Graphing | CEGRPH | Direct (PR #34) |

### 4.3 Architecturally unreachable modules

The retained `CELIMS` program has a nearest-neighbor assignment because the launcher bypasses its direct path.
Retired programs and their reason appear in [hardware deployment](HARDWARE-DEPLOYMENT.md#retired-modules).

| Module | Blocker | Nearest-neighbor probe | Fix |
|--------|---------|----------------------|-----|
| CELIMS | `route_program` sends `NODE_LIMIT` directly to CECLIM, bypassing CELIMS entirely | `lim(x^2,x,3)` via CECLMB | Refactor `route_program` to route complex limits via CELIMS |


### 4.4 Matrix coverage: mat2() constructor

Prior to PR #34, the CEMAT module was unreachable because matrix-literal entry
requires a semicolon row separator (`mat(1,2;3,4)`) and no stable autotester key
path exists for the semicolon on real TI-OS (documented in
`tests/emulator/matrix-gap-analysis.md` — three approaches attempted and ruled out).

PR #34 adds `mat2(a,b,c,d)` as a semicolon-free 2x2 matrix constructor. It
produces an identical `NODE_MATRIX_OP(MAT_LITERAL, rows=2, cols=2)` AST node to
the semicolon form and is entered deterministically with comma keys only.
The probe `det(mat2(1,2,3,4))` = -2 now exercises the full CEMAT dispatch path.

---

## 5. LCD Baseline Validation

### 5.1 Desktop suite (PR #20)

**File:** `tests/test_lcd_text.py`  
**Scope:** 59 expressions across all CAS domains  
**Method:** Desktop `bin/calcengine.exe --solve` subprocess; substring assertions  
**Result:** 59/59 PASS  
**Note:** Desktop-layer only. Exercises parser, AST, simplifier, and math modules.
Does not exercise TI-OS, module dispatch, keypad input, or LCD formatting.

### 5.2 Emulator-layer suite (PR #28)

**File:** `tests/emulator/test_lcd_text_emulator.py`  
**Scope:** Same 59 expressions as the desktop suite  
**Method:** Single `cemu-headless` subprocess; one boot; 59 expression cycles;
VRAM CRC comparison against a captured baseline  
**Baseline:** `tests/emulator/lcd_text_emulator_baseline.json`  
**CRC capture command:** `python tests/emulator/capture_lcd_text_baseline.py`  
**Result after baseline capture:** 59/59 expected (pending first Windows-host run)

This suite validates the hardware path that the desktop suite cannot reach:
TI-OS execution, module dispatcher, RAM path, keypad input, and LCD rendering.
A CRC mismatch signals a regression in any of those layers, which PR #20's
substring assertions would not detect.

### 5.3 ASCII OCR extraction (PR #32)

The `cemu-headless` native-core binary supports `format="text"` on the
`get_screen` command. It performs font-based OCR on the 320x240 RGBA8888 LCD
framebuffer using the graphx 8x8 default font (95 glyphs, codepoints 0x20–0x7E).

| OCR parameter | Value |
|--------------|-------|
| Font | graphx default 8x8 (MSB = leftmost pixel) |
| Glyph range | 95 printable ASCII (0x20–0x7E) |
| Pixel threshold | R+G+B < 384 (dark pixel = foreground) |
| Match criterion | Hamming distance <= 16 bits across 8 rows |
| Alignment search | All 8 x-offsets per strip; highest-confidence alignment selected |
| Output | Per-strip rows, trailing spaces trimmed, empty strips omitted |

The emulator suite captures both a `format="crc"` and a `format="text"` response
per expression. When both succeed, assertions run as belt-and-suspenders: CRC
mismatch fails first (exact-frame regression detection); text-substring check
activates as a secondary layer. In wrapper-mode or older builds the text response
errors silently, leaving CRC as the sole assertion — this is the expected
behavior and is not a test failure.

### 5.4 Visual QA sweep (PR #23)

**File:** `scripts/visual_qa.py`  
**Scope:** 211 CalcEngine display states fingerprinted  
**Method:** `autotester.exe` baseline-capture mode; all `expected_CRCs` seeded
to `00000000`; observes non-error, non-prompt CRCs across every reachable screen  
**Result:** 211/211 CRC fingerprints captured; 0 unexpected error dialogs (2 error
CRCs are intentional divide-by-zero tests); 4 TI-OS screenshots (boot, home, MODE,
TEST menu) rendered cleanly  
**Screenshots:** `docs/visual-qa-screenshots/`

---

## 6. Error-Path Coverage

### 6.1 Error-path emulator suite (PR #25)

**File:** `tests/emulator/autotest.error-paths.json`  
**Scope:** 7 error classes; 14 hash entries; 312 key-injection steps  
**Classes:** syntax error, overflow, undefined variable, divide-by-zero, complex
domain, stack overflow, cancel recovery (kb_On + kb_Clear)  
**CRC baselines:** populated with real-ROM values in Pass 63 (PR #30)

### 6.2 Error-path guard test (PR #30, Pass 63)

**File:** `tests/cemu/test_error_paths_template.py`  
**Tests:**
- `test_error_paths_template_has_real_baseline_crcs` — asserts that no entry in
  `autotest.error-paths.json` contains a string placeholder or the dummy `00000000`
  CRC value; fails immediately if a future edit reintroduces either.
- `test_error_paths_sequence_uses_known_autotester_keys` — cross-checks all
  `key|X` tokens in the error-path sequence against the comprehensive suite's
  known-good token set; flags any unsupported key name before it reaches the
  real-ROM run.

Pass 63 also corrected two invalid key tokens in the original delivered JSON:
`key|square` (unsupported) corrected to `key|^2`, and `key|xvar` (unsupported)
corrected to `key|xton`, matching the confirmed token map from `autotester.cpp`.

### 6.3 Hardware cancel key (PR #25)

`check_cancel()` in `src/split/runtime_globals.c` and `src/main.c` now polls
both `kb_Data[6] & kb_Clear` and `kb_Data[1] & kb_On`. Prior to this fix, only
the CLEAR key was checked; ON was silently ignored, preventing users from
aborting long computations with the TI-84 CE hardware break key.

---

## 7. Known Non-Issues and Deferred Items

### 7.1 Resolved items

| Item | Resolution |
|------|-----------|
| CEMAT unreachable via semicolon | Resolved in PR #34 via `mat2(a,b,c,d)` constructor |
| diff() stack overflow on deep expressions | Resolved in PR #22 via `DIFF_MAX_DEPTH = 50` guard |
| ON key ignoring in check_cancel() | Resolved in PR #25 |
| Error-path JSON invalid key tokens | Resolved in PR #30 (Pass 63) |
| TMPCALC.8xp absent from deploy package | Resolved in PR #18 |
| Hardware crash on every expression entry ("validating OS" RAM-clear) | Resolved in PR #38: `launcher_reentry` UAF — deleted dangling callback; workers now re-launch CALCENG via `os_RunPrgm(req->return_to, NULL, 0, NULL)` in `ce_module_runtime_finish` under `#ifndef DESKTOP_BUILD` |
| Hardware crash after first successful calculation (iteration-2 "validating OS") | Resolved in PR #40: stack overflow — `ce_response_t probe` (4510B) + `ce_request_t` (4273B) = ~9KB combined on iteration-2 re-entry path, exceeds ~4KB eZ80 OS stack. Fix: five large stack locals demoted to file-static in `module_runtime.c` / `main.c`. **Emulator blind spot**: CEmu flat host memory model does not enforce the OS stack watchdog; this class of bug is not detectable in emulation. |
| Emulator blind spot for eZ80 stack overflow not caught at build time | PR #45 added the checker, but its `bin/` scan missed reports. The repaired gate scans `obj/<NAME>/` and requires all 35 LTO reports. `-fstack-usage` added to `split/program.mk` CFLAGS; `tools/check_stack.py` parses `.su` files after each build and fails (`exit 1`) if any function allocates ≥ 1024 bytes of stack locals. Wired into root `Makefile` as a `check-stack` target that depends on `split-suite`. 64 regression tests in `tests/cemu/test_stack_check.py` always run. The exact PR #40 functions (`ce_module_runtime_recover_child_failure` 4510B, `main` 4273B) are validated by `TestPR40Regression`. |

### 7.2 Open deferred items

| Item | Status | Notes |
|------|--------|-------|
| CRC baselines in autotest.module-coverage.json | 31 x `00000000` | Require one Windows-host capture run; runbook in `tests/emulator/autotest.module-coverage.crc-catalog.md` |
| CRC baselines in lcd_text_emulator_baseline.json | 59 x empty | Require Windows-host capture run; command: `python tests/emulator/capture_lcd_text_baseline.py` |
| CELIMS direct probe | Deferred | Routing bypassed in `route_program`; fix documented in MODULE-COVERAGE-MATRIX.md |
| Text-extraction TODO in cemu-headless | Deferred | `format="text"` already wired (PR #32); substring assertions activate automatically when native-core binary is used |
| UI-flow CRC pinning | Deferred | `tests/test_ui_flows.test.ts` assertions use text-oracle-only mode on first run; follow-up to pin CRCs from first green run |
| **Emulator blind spot: RAM reallocation across `os_RunPrgm`** | Permanent gap | CEmu does not model the TI-OS behavior where `os_RunPrgm` frees the caller's RAM and loads the child at the same address. Any bug that depends on this RAM-reuse behavior (e.g. use-after-free via callback pointer) will NOT be caught by the emulator suite. Workaround: avoid registering function pointers into the calling program's RAM as `os_RunPrgm` callbacks. Validation script: `tests/emulator/repro_hardware_crash.py` (verdict will always be INCONCLUSIVE on emulator, not REPRODUCED). Fixed in PR #38; tracked here to prevent future regressions of the same class. |

### 7.3 Out-of-scope

The following are confirmed out-of-scope for this validation pass:

- TI Connect CE transfer verification (hardware transfer is a manual step)
- Full 35-module CRC baselines populated (deferred to post-merge Windows-host action)

---

## 8. Reproducing the Historical Sign-Off Snapshot

These commands reproduce the historical validation/sign-off slice on the Windows
coordinator host. They are preserved so future work can compare today’s results
against the 2026-04-14 snapshot; they do **not** imply that current `main` is
already certified hardware-ready.

### 8.1 Prerequisites

```powershell
# Required for all steps
git clone https://github.com/XanderXML-Bit/algebrix-ce
cd algebrix-ce
npm install

# Required for real-hardware steps
# - tools/emulator/cemu-headless/cemu-headless.exe (built from tools/emulator/cemu-headless/build.ps1)
# - rom/Rom.rom (or $env:CALCENGINE_ROM_PATH pointing to it)
# - Set $env:CEMU_AUTOTESTER_PATH to the path to your built autotester.exe.
```

### 8.2 Vitest unit + integration tests

```powershell
npm test
# Expected: 34 passed, 33 skipped, 0 failed
```

### 8.3 Python pytest suite

```powershell
python -m pytest tests/cemu -q
# Expected: 144 passed, 108 skipped, 0 failed
```

### 8.4 Hardware emulator — comprehensive autotester suite

```powershell
# Requires: autotester.exe, Rom.rom, deploy/hardware-ready/ bundle
powershell tools/emulator/run-comprehensive-autotest.ps1 `
    -RomPath rom/Rom.rom `
    -AutotesterPath $env:CEMU_AUTOTESTER_PATH `
    -Mode Both
# Expected: 49/49 PASS (or current comprehensive-suite count)
```

### 8.5 Hardware emulator — module coverage suite

```powershell
# Requires: autotester.exe, Rom.rom, deploy/hardware-ready/ bundle
powershell tools/emulator/run-cemu-autotest.ps1 `
    -TestFile tests/emulator/autotest.module-coverage.json `
    -RomPath rom/Rom.rom `
    -AutotesterPath $env:CEMU_AUTOTESTER_PATH
# Expected: 31/31 PASS (after CRC baselines are captured)
```

### 8.6 LCD text emulator baseline capture and verify (real hardware)

```powershell
# Requires: cemu-headless.exe, Rom.rom, deploy/hardware-ready/ bundle
python tests/emulator/capture_lcd_text_baseline.py --print-paths   # verify paths
python tests/emulator/capture_lcd_text_baseline.py --dry-run        # preview CRCs
python tests/emulator/capture_lcd_text_baseline.py                  # write baseline
python tests/emulator/capture_lcd_text_baseline.py                  # second run (for flap tolerance)
python -m pytest tests/emulator/test_lcd_text_emulator.py -v
# Expected: 59 passed, 1 metadata test passed
```

### 8.7 Error-path suite

```powershell
# Requires: autotester.exe, Rom.rom, deploy/hardware-ready/ bundle
powershell tools/emulator/run-error-paths.ps1 `
    -RomPath rom/Rom.rom `
    -AutotesterPath $env:CEMU_AUTOTESTER_PATH
# Expected: 14/14 PASS (real CRC baselines captured in Pass 63)
```

### 8.8 UI flow validation (opt-in)

```powershell
# Requires: cemu-headless.exe, Rom.rom, autotester.exe
$env:CALCENGINE_ALLOW_REAL_CEMU_IN_CI = "true"
npx vitest run tests/test_ui_flows.test.ts
# Expected: 5 passed (menu, settings, history, help, custom-function)
```

### 8.9 Build-time stack-usage check

```bash
# Always runs — no ROM, no binary, no CE toolchain required.
# Requires only Python 3 and the .su files produced by -fstack-usage during build.
python -m pytest tests/cemu/test_stack_check.py -v
# Expected: 64 passed, 0 skipped, 0 failed

# Run the checker standalone over a build tree (after make):
python3 tools/check_stack.py obj --threshold 1024 --expect-count 35
# Expected: "OK — no frame exceeds 1024 B"  (exit 0)
# Violations exit 1. An empty scan, a wrong file count, unparsable lines,
# or an empty report exit 2.
# Use --allow-empty only when intentionally scanning an empty tree; it still
# prints PARSE ERROR / EMPTY REPORT warnings to stderr, even with --quiet.

# The Makefile wires this in automatically as a post-build step:
#   make        → builds split-suite, then runs check-stack
#   make check-stack  → build split-suite, then check
# The default checker is `python` on Windows and `python3` elsewhere;
# override with make STACK_CHECKER="<python> tools/check_stack.py".
# Checker only, without rebuilding: use the standalone command above.
```

---

## 9. Coordinator Log

The merge coordinator session ran from 2026-04-14T00:18Z to 2026-04-14T06:00Z.
Full iteration-by-iteration log with HEAD hashes, test counts, and notes is in
the hardware certification record (not published).

Coordinator summary:

| Iteration | Time (UTC) | Key event |
|-----------|-----------|-----------|
| 1 | 00:18Z | TMPCALC.8xp missing from deploy — found and fixed |
| 2–4 | 00:24–00:30Z | Deploy README, PR #18 (TMPCALC) merged |
| 5 | 00:33Z | PRs #19 + #20 merged; `2+3=5` confirmed on emulator (CRC B9B5D5E0) |
| 6 | 00:37Z | TODO-comprehensive conflict resolved; fidelity gate merged (PR #21); pytest 142 |
| 7 | 04:16Z | PRs #22–25 merged (depth guard, visual QA, module audit, kb_On fix) |
| 8 | 04:33Z | PRs #27–29 merged (UI flows, emulator LCD suite, sign-off) |
| 9 | 05:38Z | PRs #30 + #32 merged (Pass 63 error-path baseline, ASCII OCR extraction); pytest 144 |
| 10 | 05:54Z | PR #34 merged (43/43 module coverage, mat2 constructor) |
| 11 | 06:30Z | PR #36 merged (VALIDATION-COVERAGE.md). Deploy refresh: 35/44 rebuilt, 9 kept; SHA256SUMS generated. HARDWARE-READY CERTIFIED. |
| 12 | — | **PR #38 hotfix**: `launcher_reentry` UAF on hardware — deleted dangling callback; workers re-launch CALCENG via `ce_module_runtime_finish`. All tests green. Deploy refreshed. |
| 13 | — | **PR #40 hotfix**: Stack overflow on iteration-2 re-entry — demoted 5 large stack locals to file-static in `module_runtime.c`/`main.c`. Emulator blind spot documented. npm 34/34, pytest 144/144. Only CALCENG.8xp updated. SHA256SUMS regenerated. |
| 14 | — | **PR #43 Phase 1 v1.1.0**: Custom keyscan, output fix, ENTRY recall, history display, MODE stub, 2nd+MODE quit. CE_IPC_VERSION 2→3, last_result[64]. Full rebuild 36 modules. |
| 15 | — | **PR #45 Phase 1+ stack check**: `-fstack-usage` in `split/program.mk`; `tools/check_stack.py` post-build guard (threshold 1024B); wired into root `Makefile` as `check-stack`; 41 regression tests always-run. pytest 185/185. |

---

## 10. Stack-Usage Build-Time Guard

### 10.1 Background

PR #40 documented a hardware-only crash class: large stack locals that are safe
on CEmu (flat host memory, no stack watchdog) but trigger a "validating OS"
RAM-clear on real hardware when the eZ80 OS stack (~4 KB usable) is exhausted.

The specific PR #40 functions were:

| Function | File | Stack bytes |
|----------|------|-------------|
| `ce_module_runtime_recover_child_failure` | `src/split/module_runtime.c` | 4510 B |
| `main` | `src/core/launcher/main.c` | 4273 B |

Combined depth on iteration-2 re-entry: ~9 KB → exceeds ~4 KB OS stack → crash.

Both were fixed by demoting to file-static storage, but the **emulator did not
and cannot** detect this class. PR #45 introduced the checker.
The original empty `bin/` scan did not enforce it. The repaired guard requires all 35 reports from `obj/`.

### 10.2 Implementation

**Compiler flag:** `-fstack-usage` added to `CFLAGS` in `split/program.mk`.  
ez80-clang (LLVM-based) supports this flag.  With CEdev v11.2 LTO, it writes one `lto.su` file per program
alongside the linked LLVM bitcode in `obj/<NAME>/`.

**Checker script:** `tools/check_stack.py`  
Recursively scans for `.su` files, parses each function frame entry, and reports
violations above the threshold.  Handles all three frame types:

| Type | Meaning | Action |
|------|---------|--------|
| `static` | Fixed frame, known at compile time | Flag if ≥ threshold |
| `dynamic,bounded` | alloca with compile-time bound | Flag if ≥ threshold |
| `dynamic` | Unbounded VLA / alloca | Warn by default; fail if `--dynamic-fail` |

**Threshold:** 1024 B per function (default, overridable via `--threshold N`).
This is well below the 4 KB eZ80 OS stack limit and gives headroom for nested calls.

**Makefile integration:**

```makefile
# Root Makefile: `make` now runs `check-stack` after all modules are built.
all: check-stack

check-stack: split-suite
    $(STACK_CHECKER) --threshold 1024 --expect-count $(STACK_EXPECT_COUNT) obj
```

`split/program.mk` passes `-fstack-usage` to every worker and the launcher.
Each module uses `OBJDIR ?= obj/$(NAME)`. Active program names are unique.
A module's clean removes only its own objects and existing binary directory.
The guard scans `obj/` after the suite finishes, including under parallel make.
It requires exactly 35 reports, computed from the active module lists.
An empty scan, a wrong file count, unparsable lines, or an empty report exits 2.
`--allow-empty` permits missing, empty, or unparsable reports during standalone checks.
It does not bypass `--expect-count` or frame threshold violations.
`STACK_CHECKER` defaults to `python tools/check_stack.py` when `$(OS)` is `Windows_NT`, otherwise `python3 tools/check_stack.py`.
You can override `STACK_CHECKER` on either platform.

Before running the standalone checker, run `make clean` and rebuild the active suite.
Building legacy `split/Makefile.{alga,prob,tmpcalc,stat,cmplx}` leaves extra reports under `obj/` that cause a count mismatch.

The earlier `bin/` scan found no reports and falsely passed.
That configuration did not validate the current stack frames.
This per-function guard does not prove a maximum combined call-chain depth.

### 10.3 Test Coverage

**File:** `tests/cemu/test_stack_check.py` (64 tests)
**Always runs** — no cemu-headless binary, ROM, or calculator hardware required.

| Class | Tests | What they prove |
|-------|-------|-----------------|
| `TestSuLineParser` | 9 | Parser handles all `.su` line formats, degenerate input, blank lines |
| `TestSuFileParsing` | 4 | Multi-function files, I/O errors, empty files, sparse files |
| `TestViolationDetection` | 7 | 2KB local caught; PR #40 scenario; boundary conditions (±1 of threshold) |
| `TestDynamicFrames` | 3 | Unbounded dynamic = warn by default, fail with `--dynamic-fail`; bounded large = fail |
| `TestFileDiscovery` | 8 | Recursive scan, direct file path, glob exclusion, nonexistent path, empty dir, realpath de-duplication |
| `TestCLIExitCodes` | 30 | Exit 0/1/2; empty scans and reports; unparsable and indented-comment lines; `--allow-empty` (warnings survive `--quiet`), `--expect-count`; overlapping paths; existing CLI flags |
| `TestPR40Regression` | 3 | Exact PR #40 function names/sizes → violation; post-fix (0B) → clean |

### 10.4 Emulator Blind Spot Status

| Blind spot | Before PR #45 | After stack-guard repair |
|------------|--------------|--------------|
| eZ80 OS stack overflow (large per-function frame) | Undetected by emulator, catches only on hardware | **Caught at build time** — `make` fails after module generation when any frame reaches 1024 B |
| Stack overflow from unbounded alloca / VLA | Undetected | Warned by default; use `--dynamic-fail` for strict mode |
| Stack overflow from deep call chains | Not addressed by `-fstack-usage` (tool only sees individual frames) | Out of scope; mitigated by existing depth guards (diff: 50, integrate: 8, limits: 50, parser: 50) |
| RAM reallocation across `os_RunPrgm` | Permanent gap (CEmu host memory model) | Still undetectable by emulator; fixed in PR #38; future prevention via code review |
