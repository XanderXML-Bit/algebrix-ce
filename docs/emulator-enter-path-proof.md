# Emulator Enter-Path Proof

Current package: 35 official programs, four runtime AppVars, and arTIfiCE, totaling 40 calculator files.
The package contains 44 files including metadata.
Earlier measured counts and emulator results below describe historical packages, not this refreshed package.
Revalidate current screen baselines on the self-hosted real-ROM lane.

Date: 2026-03-22

This document records the dedicated emulator proof for the canonical CalcEngine interaction path:

`launch -> type expression -> press Enter -> stay inside CalcEngine -> continue`

The proof is intentionally narrower than overall emulator parity. It does not claim full keypad or LCD equivalence. It proves that the real-ROM CEmu lane no longer has an unresolved "Enter crashes / exits / weird returns" blocker for the main homescreen flow.

## Canonical Runner

Run the dedicated proof with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/emulator/run-enter-path-proof.ps1 `
  -RomPath rom\Rom.rom `
  -AutotesterPath "C:\Users\user\Tools\CEmu\v2.0\autotester.exe" `
  -DownloadsDir "C:\Users\user\Downloads\CalcEngine" `
  -Mode Verify
```

Committed evidence artifacts:

- `tests/emulator/autotest.enter-path-proof.json`
- `tests/emulator/autotest.enter-path-proof.baselined.json`
- `tests/emulator/autotest.enter-path-proof.crc-catalog.json`
- `tests/emulator/autotest.enter-path-proof.crc-catalog.md`
- `tests/emulator/outputs/enter-path-proof.summary.json`
- `tests/emulator/outputs/enter-path-proof.parsed.json`

Current verified result on this workstation:

- Package validation: `43` modules + `4` runtime `.8xv` files + `README.md` all present and CRC-valid
- Backend: canonical `autotester.exe` real-ROM lane
- Verify result: `19/19` hashes passed
- Summary line: `[Autotest passed] Out of 19 tests attempted, 19 passed, and 0 failed.`

## What Was Proven

| Proof point | Hash | CRC | What it rules out |
| --- | --- | --- | --- |
| Boot prompt visible | `ep00_boot_prompt` | `B76BC405` | App launches to the expected homescreen prompt before any Enter-path checks |
| Enter on empty prompt | `ep01_empty_enter_stays_on_prompt` | `9F1FC020` | Plain Enter with an empty buffer does not exit or crash |
| Basic expression result | `ep02_add_result_5` | `B9B5D5E0` | `2+3 -> Enter` produces the expected result screen |
| Enter on result returns to app | `ep03_result_enter_returns_prompt` | `93BEC16E` | Plain Enter from a result screen returns to an in-app prompt state instead of TI-OS |
| Next expression after result-enter | `ep04_mul_result_42_after_result_enter` | `BFE0B809` | After returning with Enter, a fresh expression still evaluates normally |
| Rapid Enter burst | `ep05_rapid_enter_burst_stays_in_app` | `0B324DDD` | Repeated quick Enter presses after a result do not freeze or exit the app |
| Error path still works after rapid Enter | `ep06_div_zero_error_after_rapid_enter` | `32BFD188` | The session remains coherent enough to route through the normal error screen |
| CLEAR recovery after error | `ep07_prompt_after_error_clear` | `284B7FFE` | Error recovery returns to the CalcEngine prompt rather than leaving the app |
| Valid solve after error recovery | `ep08_error_recovery_10` | `05439989` | A normal post-error Enter submission still works |
| Long expression intact before Enter | `ep09_long_expr_input` | `A7F92410` | Long input survives entry without prompt corruption |
| Long expression result | `ep10_long_expr_result_16` | `392DF11A` | The Enter path remains stable for a longer expression, not just trivial smoke math |
| Enter after long-result returns to app | `ep11_post_long_expr_enter_prompt` | `02A80E3E` | Post-result Enter behavior remains stable after the longer solve path |
| Very large-number input intact before Enter | `ep12_large_mul_input` | `CBAE5596` | Very large-number entry survives the input line without truncation or immediate corruption |
| Very large-number result | `ep13_large_mul_result` | `A479ADEF` | The Enter path stays deterministic for very large multiplication output |
| Enter after large-number result returns to app | `ep14_post_large_mul_enter_prompt` | `102C5FDB` | The post-result Enter path remains stable even after a large-number solve |
| Third chained solve still works | `ep15_third_chain_result_5` | `1EEF38D7` | Multiple result-enter cycles do not leak state and break later submissions |
| Third chained Enter returns to prompt | `ep16_post_third_chain_enter_prompt` | `22919447` | Prompt restoration remains stable across repeated chained Enter cycles |
| TI-OS error dialog still reached | `ep17_error_screen_before_quit` | `32BFD188` | Divide-by-zero still routes through the expected TI-OS error dialog after multiple chained cycles |
| `2nd+MODE` exits the TI-OS error dialog cleanly | `ep18_error_quit_to_tios_home` | `0ED201DD` | Error-screen quit navigation remains deterministic and returns to the TI-OS homescreen |

## Known Claims This Proof Now Supports

- The main homescreen Enter path is stable in the real-ROM emulator lane.
- Plain Enter is not reproducing the old "app exits to TI-OS" concern in this focused path.
- Plain Enter is not reproducing a crash or frozen state in the tested result, error, or long-expression flows.
- Error recovery does not poison the next Enter-driven solve.
- Very large-number entry/output is stable in the tested homescreen loop.
- Repeated chained result-enter cycles stay inside CalcEngine until the explicit TI-OS error-dialog quit path is invoked.

## Limits

- This is still emulator evidence on the real ROM, not physical-device confirmation.
- This proof does not close keypad debounce / hold-repeat parity.
- This proof does not close LCD / pixel-fidelity parity.
- This proof is specific to the homescreen expression loop, not every possible ENTER consumer in every menu or graph view.
