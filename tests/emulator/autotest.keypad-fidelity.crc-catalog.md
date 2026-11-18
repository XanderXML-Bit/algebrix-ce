# CalcEngine autotest.keypad-fidelity CRC Catalog

Generated: 2026-04-13T20:17:56.872789-07:00
Source: `C:\src\algebrix-ce\tests\emulator\autotest.keypad-fidelity.baselined.json`
Hashes: 27

| # | Hash | Section | Expected CRCs | Description |
| --- | --- | --- | --- | --- |
| 1 | `kf00_boot_prompt` | `UNSPECIFIED` | `B76BC405` | KF00: launcher prompt immediately after launch and ENTER |
| 2 | `kf01_math_menu_open` | `UNSPECIFIED` | `8C3C5304` | KF01: MATH menu opens from the prompt |
| 3 | `kf02_math_menu_browse_down` | `UNSPECIFIED` | `58715C0B` | KF02: DOWN browses within the MATH menu |
| 4 | `kf03_math_menu_select_insert` | `UNSPECIFIED` | `D1B52764` | KF03: ENTER selects the highlighted MATH item and inserts its token |
| 5 | `kf04_prompt_after_math_clear` | `UNSPECIFIED` | `128C50A4` | KF04: CLEAR exits the MATH selection state back to the prompt |
| 6 | `kf05_2nd_sin_asin_input` | `UNSPECIFIED` | `407B4B08` | KF05: 2nd+SIN inserts asin( |
| 7 | `kf06_2nd_cos_acos_input` | `UNSPECIFIED` | `C5287417` | KF06: 2nd+COS inserts acos( |
| 8 | `kf07_2nd_tan_atan_input` | `UNSPECIFIED` | `A5FC9505` | KF07: 2nd+TAN inserts atan( |
| 9 | `kf08_2nd_sqrt_input` | `UNSPECIFIED` | `66086351` | KF08: 2nd+x^2 inserts sqrt( |
| 10 | `kf09_2nd_abs_input` | `UNSPECIFIED` | `E4A58031` | KF09: 2nd+x^-1 inserts abs( |
| 11 | `kf10_alpha_solve_input` | `UNSPECIFIED` | `95976A1A` | KF10: ALPHA spelling renders solve on the input line |
| 12 | `kf11_repeat_11111_input` | `UNSPECIFIED` | `851AF85D` | KF11: five repeated presses of 1 render 11111 without dropping a key |
| 13 | `kf12_arrow_anchor_12345` | `UNSPECIFIED` | `029FABB4` | KF12: 12345 is visible before cursor movement |
| 14 | `kf13_arrow_left_mid_12345` | `UNSPECIFIED` | `029FABB4` | KF13: LEFT moves the cursor into the middle of 12345 |
| 15 | `kf14_arrow_right_mid_12345` | `UNSPECIFIED` | `029FABB4` | KF14: RIGHT returns the cursor toward the end of 12345 |
| 16 | `kf15_del_backspace_mid_1235` | `UNSPECIFIED` | `CFDD38DC` | KF15: DEL performs backspace semantics after cursor movement inside 12345 |
| 17 | `kf16_basic_low_digits_expr` | `UNSPECIFIED` | `268D7BA4` | KF16: 0+1-2*3/4 is visible before ENTER as a basic low-digit/operator proof |
| 18 | `kf17_basic_low_digits_result` | `UNSPECIFIED` | `FB45519D` | KF17: ENTER evaluates 0+1-2*3/4 using 0,1,2,3,4,+,-,*,/ and ENTER |
| 19 | `kf18_basic_high_digits_expr` | `UNSPECIFIED` | `092D5464, 50C9E53D` | KF18: (-)5.6+(7-8)/9 is visible before ENTER as a negation/decimal/parentheses proof |
| 20 | `kf19_basic_high_digits_result` | `UNSPECIFIED` | `B9B5D5E0, BC6E36DB` | KF19: ENTER evaluates (-)5.6+(7-8)/9 using 5,6,7,8,9,(-),.,(,) and ENTER |
| 21 | `kf20_prompt_after_basic_clear` | `UNSPECIFIED` | `70643879, A32840C8` | KF20: CLEAR returns from the basic-key result path to the prompt |
| 22 | `kf21_prgm_menu_open` | `UNSPECIFIED` | `C84EC5D0` | KF21: PRGM opens from the prompt |
| 23 | `kf22_prgm_menu_browse_down` | `UNSPECIFIED` | `C84EC5D0` | KF22: DOWN is sent while the PRGM menu is open as a browse proof |
| 24 | `kf23_prompt_after_prgm_clear` | `UNSPECIFIED` | `A32840C8, D583ACD8` | KF23: CLEAR exits the PRGM menu back to the prompt |
| 25 | `kf24_long_expression_wrap` | `UNSPECIFIED` | `15B58686` | KF24: a long wrapped expression is visible before long-range cursor movement |
| 26 | `kf25_long_left_cursor` | `UNSPECIFIED` | `15B58686` | KF25: repeated LEFT presses move the cursor into a wrapped long expression |
| 27 | `kf26_long_right_cursor` | `UNSPECIFIED` | `15B58686` | KF26: repeated RIGHT presses move the cursor back across the wrapped long expression |

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
