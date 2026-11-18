# Emulator Keypad Fidelity

Date: 2026-03-24

This document records the dedicated real-ROM keypad proof lane for CalcEngine:

- Suite template: `tests/emulator/autotest.keypad-fidelity.json`
- Baselined suite: `tests/emulator/autotest.keypad-fidelity.baselined.json`
- CRC catalog: `tests/emulator/autotest.keypad-fidelity.crc-catalog.md`
- Baseline summary: `tests/emulator/outputs/keypad-fidelity.baseline.summary.json`
- Verify summary: `tests/emulator/outputs/keypad-fidelity.verify.summary.json`
- Runner: `tools/emulator/run-keypad-fidelity.ps1`

On 2026-03-24 the dedicated keypad suite captured `27` VRAM hashes on the real ROM at `rom/Rom.rom`, then verified cleanly at `27/27` with `autotester.exe`.

## Proof Summary

| Area | Hashes | Accepted CRCs | What is proven | Notes |
| --- | --- | --- | --- | --- |
| Boot prompt | `kf00_boot_prompt` | `B76BC405` | CalcEngine reaches the launcher prompt from the CEmu launch path before any keypad proof begins. | This is the anchor state for the rest of the suite. |
| MATH open / browse / select / clear | `kf01_math_menu_open`, `kf02_math_menu_browse_down`, `kf03_math_menu_select_insert`, `kf04_prompt_after_math_clear` | `8C3C5304`, `58715C0B`, `D1B52764`, `128C50A4` | `MATH`, `DOWN`, `ENTER`, and `CLEAR` all drive the TI-OS math menu path deterministically inside the launcher session. | This is the strongest menu-selection proof in the keypad lane. |
| Shifted function chords | `kf05_2nd_sin_asin_input`, `kf06_2nd_cos_acos_input`, `kf07_2nd_tan_atan_input`, `kf08_2nd_sqrt_input`, `kf09_2nd_abs_input` | `407B4B08`, `C5287417`, `A5FC9505`, `66086351`, `E4A58031` | `2nd+SIN`, `2nd+COS`, `2nd+TAN`, `2nd+x^2`, and `2nd+x^-1` insert the expected shifted tokens. | This is direct keypad-to-token evidence, not an inferred downstream result. |
| ALPHA text entry | `kf10_alpha_solve_input` | `95976A1A` | ALPHA-mode spelling of `solve` is reproduced on the input line. | This verifies the letter-entry path rather than only a later CAS result. |
| Rapid repeated presses | `kf11_repeat_11111_input` | `851AF85D` | Five consecutive `1` presses render `11111` without dropping a digit. | This proves repeated taps, not hardware hold-repeat timing. |
| Short editor path | `kf12_arrow_anchor_12345`, `kf13_arrow_left_mid_12345`, `kf14_arrow_right_mid_12345`, `kf15_del_backspace_mid_1235` | `029FABB4`, `029FABB4`, `029FABB4`, `CFDD38DC` | The suite exercises left/right navigation and then a distinct DEL backspace state inside a short expression. | DEL is visually distinct; left/right execute but do not yield independent CRCs in this dedicated lane. |
| Basic keys `0-4`, operators, ENTER | `kf16_basic_low_digits_expr`, `kf17_basic_low_digits_result` | `268D7BA4`, `FB45519D` | `0`, `1`, `2`, `3`, `4`, `+`, `-`, `*`, `/`, and `ENTER` are proven through the stable `0+1-2*3/4` path. | The result hash confirms the submit path as well as token insertion. |
| Basic keys `5-9`, negation, decimal, parentheses, CLEAR | `kf18_basic_high_digits_expr`, `kf19_basic_high_digits_result`, `kf20_prompt_after_basic_clear` | `092D5464` / `50C9E53D`, `B9B5D5E0` / `BC6E36DB`, `70643879` / `A32840C8` | `5`, `6`, `7`, `8`, `9`, `(-)`, `.`, `(`, `)`, and `CLEAR` are proven through the `(-)5.6+(7-8)/9` path and the post-result prompt recovery. | These states exposed two stable CRC families across repeated real-ROM runs, so both are accepted. |
| PRGM open / browse / clear | `kf21_prgm_menu_open`, `kf22_prgm_menu_browse_down`, `kf23_prompt_after_prgm_clear` | `C84EC5D0`, `C84EC5D0`, `A32840C8` / `D583ACD8` | `PRGM` opens from the prompt and `CLEAR` exits back to the prompt family. | The browse step is executed but shares the same visible CRC as the open state. |
| Wrapped long-expression cursor path | `kf24_long_expression_wrap`, `kf25_long_left_cursor`, `kf26_long_right_cursor` | `15B58686`, `15B58686`, `15B58686` | Repeated left/right navigation is exercised inside a wrapped long expression. | The path is real, but the dedicated suite still does not isolate a visually distinct left-vs-right cursor render. |

## Coverage Status

| Key behavior | Status | Evidence |
| --- | --- | --- |
| Digits `0-9` | HIGH | `kf16` through `kf19` cover every digit across low-digit and high-digit expression paths. |
| Operators `+ - * /` | HIGH | Directly proven by `kf16` and the matching result `kf17`. |
| Negation `(-)`, decimal `.`, parentheses | HIGH | Directly proven by `kf18` and `kf19`. |
| `ENTER` submit | HIGH | Proven by `kf03`, `kf17`, and `kf19`. |
| `CLEAR` recovery | HIGH | Proven by `kf04`, `kf20`, and `kf23`. |
| `DEL` backspace | HIGH | `kf15` lands on its own stable post-delete CRC family. |
| `2nd+key` common functions | HIGH | `kf05` through `kf09` prove `asin(`, `acos(`, `atan(`, `sqrt(`, and `abs(`. |
| ALPHA letters | HIGH | `kf10` proves free-text ALPHA entry. |
| MATH menu browse and select | HIGH | `kf01` through `kf04` prove open, browse, select, and clear. |
| PRGM menu browse | MEDIUM | `kf21` through `kf23` prove the path executes, but the browse step is not visually distinct from open. |
| Left / right cursor movement | MEDIUM | The suite sends the key paths in both short and wrapped editors, but the dedicated lane does not isolate a distinct left-vs-right cursor CRC. |
| Rapid repeated taps | MEDIUM | `kf11` proves repeated discrete taps; it does not model hold-repeat or debounce timing. |

## Remaining Gaps

- The dedicated keypad lane still does not isolate a visually distinct LEFT vs RIGHT cursor render. Both the short-expression and wrapped-expression cursor hashes collapse to a shared CRC family.
- The PRGM browse step is exercised, but `kf22_prgm_menu_browse_down` shares the same visible CRC as `kf21_prgm_menu_open`.
- Hold-repeat, debounce timing, and ghosting are still outside the AutoTester model. This suite proves discrete key events only.
- The shifted-key matrix is selective. It now covers the common calculator chords that matter for CalcEngine, not every possible `2nd+key` combination.
