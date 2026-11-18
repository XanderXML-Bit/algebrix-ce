# CalcEngine autotest.lcd-fidelity CRC Catalog

Generated: 2026-04-13T20:17:56.875299-07:00
Source: `C:\src\algebrix-ce\tests\emulator\autotest.lcd-fidelity.baselined.json`
Hashes: 18

| # | Hash | Section | Expected CRCs | Description |
| --- | --- | --- | --- | --- |
| 1 | `lf00_startup_prompt_clean` | `UNSPECIFIED` | `B76BC405` | LF00: clean startup prompt after launch and ENTER |
| 2 | `lf01_expression_mid_typing` | `UNSPECIFIED` | `ED086E35` | LF01: 2+3*4 is visible during typing before submission |
| 3 | `lf02_cursor_left_in_expression` | `UNSPECIFIED` | `0F5653E2` | LF02: cursor moves left into the middle of 12345 after two LEFT presses |
| 4 | `lf03_cursor_right_in_expression` | `UNSPECIFIED` | `0F5653E2` | LF03: cursor moves right back across 12345 after the prior left movement |
| 5 | `lf04_result_multiline_14` | `UNSPECIFIED` | `6D2A3D67` | LF04: 2+3*4 renders the expression on line 1 and the result on line 2 |
| 6 | `lf05_prompt_after_result_clear` | `UNSPECIFIED` | `A32840C8` | LF05: first CLEAR after a result returns to the prompt family |
| 7 | `lf06_clean_prompt_after_result` | `UNSPECIFIED` | `A32840C8` | LF06: second CLEAR after a result returns to a clean prompt |
| 8 | `lf07_large_expression_input` | `UNSPECIFIED` | `17D3F931` | LF07: large multiplication input is visible before submission |
| 9 | `lf08_large_result_right_aligned` | `UNSPECIFIED` | `AAB42ABA` | LF08: large multiplication result exercises result positioning and right-alignment |
| 10 | `lf09_prompt_after_large_result` | `UNSPECIFIED` | `A32840C8` | LF09: CLEAR returns from the large-result screen to the prompt family |
| 11 | `lf10_long_expression_wrap` | `UNSPECIFIED` | `B30E2D84` | LF10: a long expression wraps across the homescreen input area without corrupting layout |
| 12 | `lf10_long_cursor_left` | `UNSPECIFIED` | `B30E2D84` | LF10: repeated LEFT presses move the cursor into the wrapped long expression |
| 13 | `lf10_long_cursor_right` | `UNSPECIFIED` | `B30E2D84` | LF10: repeated RIGHT presses move the cursor back across the wrapped long expression |
| 14 | `lf11_math_menu_open` | `UNSPECIFIED` | `8C3C5304` | LF11: MATH menu rendering is visible from the prompt |
| 15 | `lf12_math_menu_browse` | `UNSPECIFIED` | `58715C0B` | LF12: MATH menu browse state remains visually stable after DOWN |
| 16 | `lf13_prompt_after_math_menu` | `UNSPECIFIED` | `A32840C8` | LF13: CLEAR exits the MATH menu back to the prompt |
| 17 | `lf14_error_dialog_divide_by_zero` | `UNSPECIFIED` | `32BFD188` | LF14: divide-by-zero renders the expected error dialog layout |
| 18 | `lf15_post_error_prompt` | `UNSPECIFIED` | `DAE8FE30` | LF15: CLEAR from the error dialog returns to a prompt-ready state |

## Official Module Inventory

Manifest: `tests/emulator/manifest.json`
Verified Official Modules: 35/35

| # | Module | Size (Bytes) | Manifest CRC32 | Resolved CRC32 | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | `CALCENG.8xp` | 29826 | `B05E8849` | `B05E8849` | `matched` |
| 2 | `CEALG.8xp` | 12506 | `9D13EE10` | `9D13EE10` | `matched` |
| 3 | `CEALGEO.8xp` | 35506 | `FA7B8ADF` | `FA7B8ADF` | `matched` |
| 4 | `CEALGF.8xp` | 29416 | `5241D296` | `5241D296` | `matched` |
| 5 | `CEALGSY.8xp` | 47320 | `79E14C4B` | `79E14C4B` | `matched` |
| 6 | `CEAPDV.8xp` | 19536 | `4031AA38` | `4031AA38` | `matched` |
| 7 | `CEASLA.8xp` | 25231 | `11155598` | `11155598` | `matched` |
| 8 | `CEASLV.8xp` | 23437 | `A600914D` | `A600914D` | `matched` |
| 9 | `CEBASE.8xp` | 47546 | `AD723AC5` | `AD723AC5` | `matched` |
| 10 | `CECALC.8xp` | 50532 | `F97C6304` | `F97C6304` | `matched` |
| 11 | `CECIAD.8xp` | 13600 | `6EA8DF57` | `6EA8DF57` | `matched` |
| 12 | `CECIDF.8xp` | 51169 | `2DD0128E` | `2DD0128E` | `matched` |
| 13 | `CECINT.8xp` | 12557 | `43520C7B` | `43520C7B` | `matched` |
| 14 | `CECISM.8xp` | 50137 | `8BCDB7F3` | `8BCDB7F3` | `matched` |
| 15 | `CECLIA.8xp` | 46804 | `FF3CD805` | `FF3CD805` | `matched` |
| 16 | `CECLIC.8xp` | 24825 | `42721ABC` | `42721ABC` | `matched` |
| 17 | `CECLIM.8xp` | 7526 | `BA93C85C` | `BA93C85C` | `matched` |
| 18 | `CECLIR.8xp` | 21110 | `C0BEA067` | `C0BEA067` | `matched` |
| 19 | `CECLMB.8xp` | 49963 | `4E98FD1E` | `4E98FD1E` | `matched` |
| 20 | `CECODE.8xp` | 46088 | `72C60AB5` | `72C60AB5` | `matched` |
| 21 | `CECPNM.8xp` | 39869 | `78C657C0` | `78C657C0` | `matched` |
| 22 | `CECPOL.8xp` | 12415 | `933A00E9` | `933A00E9` | `matched` |
| 23 | `CECSEQ.8xp` | 50405 | `BBCAF79E` | `BBCAF79E` | `matched` |
| 24 | `CECSER.8xp` | 51169 | `C3C31B56` | `C3C31B56` | `matched` |
| 25 | `CEFIN.8xp` | 38270 | `F7C496C2` | `F7C496C2` | `matched` |
| 26 | `CEGRPH.8xp` | 9107 | `AC565F11` | `AC565F11` | `matched` |
| 27 | `CEIPFR.8xp` | 49633 | `793B9158` | `793B9158` | `matched` |
| 28 | `CEIPRT.8xp` | 45170 | `32ABB79B` | `32ABB79B` | `matched` |
| 29 | `CEISTR.8xp` | 16658 | `1827B660` | `1827B660` | `matched` |
| 30 | `CEITRG.8xp` | 50059 | `3B0F34E0` | `3B0F34E0` | `matched` |
| 31 | `CEIUSB.8xp` | 32042 | `56D02034` | `56D02034` | `matched` |
| 32 | `CELIMS.8xp` | 24642 | `FE821EC0` | `FE821EC0` | `matched` |
| 33 | `CELSCX.8xp` | 23767 | `07FAF24B` | `07FAF24B` | `matched` |
| 34 | `CEMAT.8xp` | 50264 | `1D6E38A2` | `1D6E38A2` | `matched` |
| 35 | `CESTPRB.8xp` | 27097 | `8372D308` | `8372D308` | `matched` |
