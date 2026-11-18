# Emulator LCD Fidelity

Date: 2026-03-24

This document records the dedicated real-ROM LCD / visual proof lane for CalcEngine:

- Suite template: `tests/emulator/autotest.lcd-fidelity.json`
- Baselined suite: `tests/emulator/autotest.lcd-fidelity.baselined.json`
- CRC catalog: `tests/emulator/autotest.lcd-fidelity.crc-catalog.md`
- Baseline summary: `tests/emulator/outputs/lcd-fidelity.baseline.summary.json`
- Verify summary: `tests/emulator/outputs/lcd-fidelity.verify.summary.json`
- Runner: `tools/emulator/run-lcd-fidelity.ps1`

On 2026-03-24 the dedicated LCD suite captured `18` VRAM hashes on the real ROM at `rom/Rom.rom`, then verified cleanly at `18/18` with `autotester.exe`.

## Proof Summary

| Visual flow | Hashes | Accepted CRCs | What is proven | Notes |
| --- | --- | --- | --- | --- |
| Clean startup prompt | `lf00_startup_prompt_clean` | `B76BC405` | The post-launch homescreen prompt is stable before any expression entry. | This is the visual anchor for the suite. |
| Mid-typing expression layout | `lf01_expression_mid_typing` | `ED086E35` | `2+3*4` renders deterministically while still being edited. | This is the direct “expression during typing” checkpoint. |
| Cursor placement in a short expression | `lf02_cursor_left_in_expression`, `lf03_cursor_right_in_expression` | `0F5653E2`, `0F5653E2` | The left/right cursor path is exercised inside `12345`. | The cursor path is real, but left and right still collapse to the same visible CRC. |
| Multi-line result layout | `lf04_result_multiline_14` | `6D2A3D67` | `2+3*4` renders the expression on line 1 and the result on line 2. | This is the clearest multi-line proof in the suite. |
| Post-result recovery to a clean prompt | `lf05_prompt_after_result_clear`, `lf06_clean_prompt_after_result` | `A32840C8`, `A32840C8` | Two consecutive `CLEAR` presses recover from the result view back to the prompt family. | The first and second prompt renders share the same CRC family in this lane. |
| Large-number input and right-aligned result | `lf07_large_expression_input`, `lf08_large_result_right_aligned`, `lf09_prompt_after_large_result` | `17D3F931`, `AAB42ABA`, `A32840C8` | The suite proves the large-number result layout and prompt recovery after a visibly wider result screen. | `lf08` is the main right-alignment proof. |
| Long-expression wrap | `lf10_long_expression_wrap` | `B30E2D84` | A long expression wraps across the homescreen input area without corrupting layout. | This is the strongest dedicated wrap-state proof. |
| Cursor path inside a wrapped expression | `lf10_long_cursor_left`, `lf10_long_cursor_right` | `B30E2D84`, `B30E2D84` | Repeated left/right navigation is exercised inside the wrapped expression. | The cursor path executes, but the visual CRC does not distinguish the phases. |
| MATH menu rendering and browse | `lf11_math_menu_open`, `lf12_math_menu_browse`, `lf13_prompt_after_math_menu` | `8C3C5304`, `58715C0B`, `A32840C8` | Menu items render consistently, the highlighted row moves with `DOWN`, and `CLEAR` returns to the prompt. | This gives the LCD lane a dedicated menu-render checkpoint instead of borrowing only from the comprehensive suite. |
| Error dialog rendering | `lf14_error_dialog_divide_by_zero`, `lf15_post_error_prompt` | `32BFD188`, `DAE8FE30` | The divide-by-zero dialog layout is stable and `CLEAR` returns to a prompt-ready state. | `32BFD188` is the same TI-OS divide-by-zero family seen elsewhere in the emulator evidence. |

## Coverage Status

| Visual behavior | Status | Evidence |
| --- | --- | --- |
| Homescreen prompt appearance | HIGH | `lf00` proves the clean startup prompt. |
| Expression display during typing | HIGH | `lf01` proves the typed expression render before submission. |
| Result layout and multi-line expression/result view | HIGH | `lf04` proves expression on line 1 and result on line 2. |
| Right-aligned large result positioning | HIGH | `lf07` through `lf09`, especially `lf08`, cover the wide-number result layout. |
| Long-expression wrapping | HIGH | `lf10_long_expression_wrap` proves stable wrapped rendering. |
| MATH menu rendering and navigation | HIGH | `lf11` through `lf13` prove menu open, browse, and close. |
| Error dialog rendering | HIGH | `lf14` and `lf15` prove the error dialog and recovery path. |
| Cursor placement | MEDIUM | The suite exercises both short-expression and wrapped-expression cursor paths, but neither yields an independently visible left-vs-right CRC. |
| Cursor blink phase | LOW | The suite proves cursor-family states only; it does not isolate blink timing or phase changes. |

## Remaining Gaps

- The dedicated LCD lane is still CRC-first. It does not archive screenshots automatically; the proof artifact is the accepted VRAM CRC family.
- Cursor movement is exercised in both short and wrapped expressions, but the screen-state CRC does not distinguish left from right in this focused lane.
- Blink timing remains unproven. The suite captures cursor-position families, not blink cadence or phase.
- The suite is homescreen-focused. It does not attempt pixel-level proof for arbitrary graphics primitives outside the TI-OS text / menu / error flows used here.
