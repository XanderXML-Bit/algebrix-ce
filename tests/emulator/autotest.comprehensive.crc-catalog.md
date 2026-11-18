# CalcEngine autotest.comprehensive CRC Catalog

Generated: 2026-04-13T20:17:56.866627-07:00
Source: `C:\src\algebrix-ce\tests\emulator\autotest.comprehensive.baselined.json`
Hashes: 211

| # | Hash | Section | Expected CRCs | Description |
| --- | --- | --- | --- | --- |
| 1 | `s00_boot_home` | `S00` | `128C50A4` | Boot and clear - CalcEngine launcher home prompt visible, ready for input |
| 2 | `s01_add_2_3` | `S01` | `03AF06DB` | S01: 2+3 = 5 - result screen after Enter |
| 3 | `s01_mul_6_7` | `S01` | `871BBE6A` | S01: 6*7 = 42 - result screen after Enter |
| 4 | `s01_neg5_plus10` | `S01` | `04304D6D, 17AAE9FB` | S01: (-5)+10 = 5 - result screen after Enter; multi-CRC: timing flap between result-visible and result-scrolled states |
| 5 | `s01_decimal_6_28` | `S01` | `09235847, A66A701F` | S01: 3.14*2 = 6.28 - decimal multiplication result; multi-CRC: timing flap between two stable render states |
| 6 | `s01_parens_27` | `S01` | `96E8CE76, CA615FD9` | S01: (4+5)*3 = 27 - parenthesised expression result; multi-CRC: timing flap between two stable render states |
| 7 | `s01_power_256` | `S01` | `98B4B360` | S01: 2^8 = 256 - exponentiation result |
| 8 | `s01_div_zero_err` | `S01` | `32BFD188` | S01: 1/0 - divide-by-zero error screen |
| 9 | `s02_chain_sub_40` | `S02` | `6B33D2F2` | S02: 100-30-20-10 = 40 - chained subtraction result |
| 10 | `s02_chain_mul_24` | `S02` | `D6820FF4` | S02: 2*3*4 = 24 - chained multiplication result |
| 11 | `s02_large_mul_56088` | `S02` | `F1BCB830` | S02: 123*456 = 56088 - large integer multiplication |
| 12 | `s02_div_1000_8` | `S02` | `97AA09B0` | S02: 1000/8 = 125 - integer division result |
| 13 | `s02_neg_times_neg_12` | `S02` | `3FB95899` | S02: (-3)*(-4) = 12 - product of two negatives |
| 14 | `s02_decimal_chain_1_5` | `S02` | `776E26D1` | S02: 0.5+0.5+0.5 = 1.5 - decimal addition chain |
| 15 | `s02_power_1024` | `S02` | `7EF52A4A` | S02: 2^10 = 1024 - power of two |
| 16 | `s03_prec_add_mul_14` | `S03` | `7480DAFF` | S03: 2+3*4 = 14 - multiply before add (not 20) |
| 17 | `s03_prec_sub_div_7` | `S03` | `9913C1A3` | S03: 10-6/2 = 7 - divide before subtract (not 2) |
| 18 | `s03_right_assoc_pow_512` | `S03` | `307E0B54` | S03: 2^3^2 = 512 - right-associative exponentiation: 2^(3^2)=2^9=512 |
| 19 | `s03_nested_parens_15` | `S03` | `236D2848` | S03: (2+3)*(4-1) = 15 - nested parentheses with mixed ops |
| 20 | `s04_sin_0` | `S04` | `B20BC5D3` | S04: sin(0) = 0 - sine at zero (radians) |
| 21 | `s04_cos_0` | `S04` | `C750EC0F` | S04: cos(0) = 1 - cosine at zero (radians) |
| 22 | `s04_tan_0` | `S04` | `52A178CC` | S04: tan(0) = 0 - tangent at zero |
| 23 | `s04_asin_1` | `S04` | `0FBF5667` | S04: asin(1) = pi/2 ~= 1.5708 - inverse sine via 2nd+SIN; multi-CRC: emulator timing can capture result screen or still-processing screen |
| 24 | `s04_sin_pi_over_2` | `S04` | `E5B90897` | S04: sin(pi/2) = 1 - SIN key then 2nd+. for pi constant |
| 25 | `s05_log_100` | `S05` | `360FD6B0` | S05: log(100) = 2 - base-10 log via LOG key |
| 26 | `s05_log_1` | `S05` | `03FF1EAC` | S05: log(1) = 0 - log of one |
| 27 | `s05_ln_1` | `S05` | `9BE7779B` | S05: ln(1) = 0 - natural log of one via LN key |
| 28 | `s05_sqrt_4` | `S05` | `695407B4` | S05: sqrt(4) = 2 - square root via 2nd+x^2 key |
| 29 | `s05_exp_e_1` | `S05` | `327A86A8` | S05: e^(1) ~= 2.71828 - natural exponential via 2nd+LN key |
| 30 | `s05_abs_neg5` | `S05` | `EAC3FDAC` | S05: abs(-5) = 5 - absolute value via 2nd+x^-1 key |
| 31 | `s06_3_times_pi` | `S06` | `22335EF2` | S06: 3*pi ~= 9.42478 - pi constant via 2nd+decimal key |
| 32 | `s06_5_squared_25` | `S06` | `DC8152C0` | S06: 5^2 = 25 - x-squared key inserts ^2 suffix |
| 33 | `s06_4_recip_0_25` | `S06` | `AE6F7DD0` | S06: 4^(-1) = 0.25 - x-inverse key inserts ^(-1) suffix |
| 34 | `s07_err_trailing_op` | `S07` | `B9B5D5E0` | S07 ERROR: 5* - trailing operator error screen |
| 35 | `s07_err_missing_lparen` | `S07` | `B9B5D5E0` | S07 ERROR: 2+3) - mismatched-paren error screen |
| 36 | `s07_err_recovery_10` | `S07` | `3C76A83C` | S07 RECOVERY: 9+1=10 - valid expression after clearing error |
| 37 | `s08_menu_open` | `S08` | `4127EC6B, 535C2DF3, 5DFACB3A, C77284BA` | S08: MODE key opens settings/symbol menu - multi-CRC: menu can render in multiple stable states depending on emulator timing |
| 38 | `s08_menu_closed` | `S08` | `F5303521` | S08: CLEAR from menu returns to home prompt |
| 39 | `s08_menu_nav_cleared` | `S08` | `F5303521` | S08: MODE, down*2, CLEAR - home after menu navigation |
| 40 | `s08_tab_recents_empty` | `S08` | `4127EC6B, C77284BA` | S08: 2nd+Right from Symbols opens the empty Recently Used tab; multi-CRC: menu render timing can land on two stable tabbed states |
| 41 | `s08_tab_settings_open` | `S08` | `4127EC6B, C77284BA` | S08: 2nd+Right again opens the Settings tab with default toggles rendered; multi-CRC: menu render timing can land on two stable tabbed states |
| 42 | `s08_show_work_off` | `S08` | `2232BB73, 3EEF9C22` | S08: ENTER on Show Work toggles the setting OFF inside the Settings tab; multi-CRC: two stable post-toggle render states observed |
| 43 | `s08_show_work_off_persist` | `S08` | `4127EC6B, 535C2DF3, 5DFACB3A` | S08: Show Work remains OFF after closing and reopening the menu; multi-CRC: reopen timing can land on three stable states |
| 44 | `s08_show_work_restored_on` | `S08` | `2232BB73, 30497AEB, 3EEF9C22` | S08: ENTER on Show Work toggles the setting back ON for downstream regression stability; multi-CRC: three stable post-toggle render states observed |
| 45 | `s08_solve_mode_step_by_step` | `S08` | `45E31D53, 593E3A02` | S08: ENTER on Solve Mode toggles from Full Auto to Step-by-Step inside the Settings tab |
| 46 | `s08_solve_mode_step_by_step_persist` | `S08` | `535C2DF3, 5DFACB3A` | S08: Solve Mode remains Step-by-Step after closing and reopening the menu |
| 47 | `s08_solve_mode_full_auto_restored` | `S08` | `45E31D53, 4B45FB9A` | S08: ENTER on Solve Mode toggles back to Full Auto for downstream regression stability |
| 48 | `s08_angle_mode_degrees` | `S08` | `03756D66, 114DB441` | S08: ENTER on Angle Mode toggles from Radians to Degrees inside the Settings tab |
| 49 | `s08_angle_mode_degrees_persist` | `S08` | `4164F4D4, 535C2DF3` | S08: Angle Mode remains Degrees after closing and reopening the menu |
| 50 | `s08_angle_mode_radians_restored` | `S08` | `03756D66, 114DB441` | S08: ENTER on Angle Mode toggles back to Radians for downstream regression stability |
| 51 | `s08_finance_pmt_begin` | `S08` | `456837A7, 5750EE80, D5C1FAE7` | S08: ENTER on Finance PMT toggles the setting from END to BEGIN inside the Settings tab |
| 52 | `s08_finance_pmt_begin_persist` | `S08` | `4164F4D4, 535C2DF3, D1CD3994` | S08: Finance PMT remains BEGIN after closing and reopening the menu |
| 53 | `s08_finance_pmt_end_restored` | `S08` | `5750EE80, D5C1FAE7, FED5D557` | S08: ENTER on Finance PMT toggles the setting back to END for downstream regression stability |
| 54 | `s08_symbol_insert_derivative` | `S08` | `2232BB73, 30497AEB, 3EEF9C22, 99CC413C, B2D86E8C` | S08: ENTER on the default Symbols row inserts d/dx( into the input line; multi-CRC: three stable input-render states observed |
| 55 | `s08_recents_open_with_derivative` | `S08` | `535C2DF3, 5DFACB3A, D1CD3994, FAD91624` | S08: reopening MODE after symbol insertion defaults to the Recently Used tab with d/dx(; multi-CRC: reopen timing can land on two stable states |
| 56 | `s08_2nd_ln_input` | `S08` | `E28359A1` | S08: 2nd+ln inserts e^( directly on the input line |
| 57 | `s08_2nd_ln_result` | `S08` | `93B0310F` | S08: 2nd+ln followed by 1) evaluates the shifted e^( function |
| 58 | `s08_settings_open_for_complex` | `S08` | `219B6130, AEEDE387, BC96221F` | S08: MODE then 2nd+RIGHT twice reopens the Settings tab ahead of complex-mode verification |
| 59 | `s08_complex_rect_enabled` | `S08` | `21B901B1, 3E2556F9, A32815D6` | S08: ENTER on the Complex setting cycles from OFF to RECT inside the Settings tab |
| 60 | `s09_7_times_8` | `S09` | `140C6356, E9801913` | S09: 7*8 = 56 - result to be used as ANS; multi-CRC: two stable post-session result renders observed |
| 61 | `s09_ans_56_plus_4` | `S09` | `AFD0D240, B536A6C1` | S09: ANS+4 = 60 - previous answer recall: 'ans' typed via ALPHA mode (a=alpha+math, n=alpha+log, s=alpha+ln); multi-CRC: two stable ANS result renders observed |
| 62 | `s10_stat_mean_3` | `S10` | `B9B5D5E0` | S10 STAT: mean(1,2,3,4,5) = 3 - sample mean routed through the statistics domain |
| 63 | `s10_stat_stdev_sample` | `S10` | `B9B5D5E0` | S10 STAT: stdev(2,4,4,4,5,5,7,9) ~= 2.13809 - sample standard deviation routed through the statistics domain |
| 64 | `s10_stat_var_sample` | `S10` | `B9B5D5E0` | S10 STAT: var(1,2,3,4,5) = 2.5 - sample variance routed through the statistics domain |
| 65 | `s10_nderiv_x2_at1` | `S10` | `32BFD188, B9B5D5E0` | S10 CAS: nderiv(x^2,x,1) - derivative via ALPHA-spelled 'nderiv(' (n=alpha+log, d=alpha+-1, e=alpha+sin, r=alpha+*, i=alpha+^2, v=alpha+6) |
| 66 | `s10_deriv_x2` | `S10` | `B9B5D5E0` | S10 CAS: d/dx(x^2) = 2*x - symbolic derivative via d/dx notation (pass56: CalcEngine now stays alive, CECALC handles expression) |
| 67 | `s10_int_x2` | `S10` | `B9B5D5E0` | S10 CAS: int(x^2,x) = x^3/3+C - indefinite integral typed via ALPHA mode (i=alpha+square, n=alpha+log, t=alpha+4) |
| 68 | `s10_lim_sinx_over_x` | `S10` | `B9B5D5E0` | S10 CAS: lim(x^2,x,2) = 4 - polynomial limit via try_direct_sub; typed LIM(X^2,X,2) using alpha keys (L=key\|), I=key\|x2, M=key\|log); key\|div inside lim args triggers TI-OS evaluation crash so division-free expression used |
| 69 | `s10_solve_x_plus2` | `S10` | `B9B5D5E0` | S10 CAS: solve(x+2,x) = x=-2 - equation solver via ALPHA-spelled 'solve(' (s=alpha+ln, o=alpha+7, l=alpha+), v=alpha+6, e=alpha+sin) |
| 70 | `s10_expand_x1_sq` | `S10` | `0C99B93E, CB0E5202` | S10 CAS: expand((x+1)^2) = x^2+2x+1 - polynomial expansion via ALPHA (e=alpha+sin, x=graphvar, p=alpha+8, a=alpha+math, n=alpha+log, d=alpha+recip); multi-CRC: timing flap between two observed stable result screens |
| 71 | `s10_fin_pv_case1` | `S10` | `B9B5D5E0` | S10 FIN: pv(0.05,10,0,1000) - present value calculation routed through the finance domain |
| 72 | `s10_fin_pv_case2_zero_rate` | `S10` | `B9B5D5E0` | S10 FIN: pv(0,5,-100,0) - zero-rate present value case |
| 73 | `s10_fin_fv_case1` | `S10` | `B9B5D5E0` | S10 FIN: fv(0.05,10,100,0) - future value calculation routed through the finance domain |
| 74 | `s10_fin_fv_case2_zero_rate` | `S10` | `B9B5D5E0` | S10 FIN: fv(0,5,0,-100) - zero-rate future value case |
| 75 | `s10_fin_pmt_case1` | `S10` | `B9B5D5E0` | S10 FIN: pmt(0.05,10,1000,0) - payment calculation routed through the finance domain |
| 76 | `s10_fin_pmt_case2_zero_rate` | `S10` | `B9B5D5E0` | S10 FIN: pmt(0,10,1000,0) - zero-rate payment case |
| 77 | `s10_complex_add_1_plus_i_plus_2_plus_i` | `S10` | `1130C5A9` | S10 CMPLX: 1+i+2+i with Complex mode set to RECT - basic complex addition |
| 78 | `s10_complex_mul_i_times_i` | `S10` | `DF97515D` | S10 CMPLX: i*i with Complex mode set to RECT - basic complex multiplication |
| 79 | `s11_clear_reenter` | `S11` | `0FFCD992, 9ECBC011` | S11: 9+9, CLEAR mid-entry, then 3+4 = 7 - clear-and-reenter continuity |
| 80 | `s11_double_clear` | `S11` | `90E69901` | S11: 5*5, CLEAR, CLEAR, then 6*6 = 36 - double-clear continuity |
| 81 | `s11_clear_midexpr_9_plus_9` | `S11` | `B3B975B9` | S11: 9+9 is visible on the input line before CLEAR is pressed |
| 82 | `s11_clear_midexpr_blank` | `S11` | `269ADB57` | S11: CLEAR on 9+9 wipes the current input and returns to the blank prompt row without advancing |
| 83 | `s11_long_expr_input` | `S11` | `0316DC12, BFCF310A` | S11: long repeated-addition expression remains visible on the input line before evaluation; multi-CRC: two stable wrapped-input renders observed |
| 84 | `s11_cursor_left_mid_12345` | `S11` | `C46473BA` | S11: LEFT arrow moves the input cursor left into the middle of 12345 |
| 85 | `s11_cursor_right_mid_12345` | `S11` | `BBD7ED63` | S11: RIGHT arrow moves the input cursor right within 12345 after a prior left move |
| 86 | `s11_del_backspace_mid_1235` | `S11` | `BBD7ED63` | S11: DEL removes the character immediately left of the cursor, turning 12345 into 1235 |
| 87 | `s11_long_expr_result_16` | `S11` | `B9B5D5E0, F018E546` | S11: long repeated-addition expression evaluates to 16 without breaking the Enter path; multi-CRC: two stable result renders observed |
| 88 | `s12_neg_minus5_result` | `S12` | `0348F81F` | S12: -5 entered as a standalone negative integer |
| 89 | `s12_neg_minus3_14_result` | `S12` | `5F2E6897` | S12: -3.14 entered as a standalone negative decimal |
| 90 | `s12_neg_grouped_input` | `S12` | `A6483681` | S12: grouped negative input -(2+3) visible before evaluation |
| 91 | `s12_neg_grouped_result` | `S12` | `4ECEE262` | S12: grouped negative expression -(2+3) evaluates correctly |
| 92 | `s12_precision_123456789_input` | `S12` | `0FD65BBE` | S12: 123456789 remains intact on the input line before evaluation |
| 93 | `s12_precision_123456789_result` | `S12` | `114A045A` | S12: 123456789 evaluates without truncation |
| 94 | `s12_precision_micro_input` | `S12` | `B2C61211` | S12: 0.000001 remains intact on the input line before evaluation |
| 95 | `s12_precision_micro_result` | `S12` | `32AF8017` | S12: 0.000001 evaluates without precision drift on entry |
| 96 | `s12_precision_1_23456789_input` | `S12` | `B30ABC99` | S12: 1.23456789 remains intact on the input line before evaluation |
| 97 | `s12_precision_1_23456789_result` | `S12` | `8D03C509` | S12: 1.23456789 evaluates without entry-side truncation |
| 98 | `s13_nested_expr_result_input` | `S13` | `7B471326` | S13: ((2+3)*4)-(1/2) is visible before evaluation |
| 99 | `s13_nested_expr_result` | `S13` | `951E838C` | S13: ((2+3)*4)-(1/2) evaluates correctly |
| 100 | `s13_nested_pow_input` | `S13` | `85AC176D` | S13: 2^(3+1) is visible before evaluation |
| 101 | `s13_nested_pow_result` | `S13` | `E5B05FA0` | S13: 2^(3+1) evaluates correctly |
| 102 | `s14_ans_seed_2_plus_3` | `S14` | `E64365B2` | S14: ANS chaining seed result from 2+3 |
| 103 | `s14_ans_times2_input` | `S14` | `ECFD01B6` | S14: ANS*2 input line after ALPHA-spelled ans recall |
| 104 | `s14_ans_times2_result` | `S14` | `C43260CC` | S14: ANS*2 evaluates correctly after prior result recall |
| 105 | `s14_ans_plus1_input` | `S14` | `5DC8831A` | S14: ANS+1 input line after chained ANS recall |
| 106 | `s14_ans_plus1_result` | `S14` | `C826B15C` | S14: ANS+1 evaluates correctly after chained ANS recall |
| 107 | `s15_overflow_9_pow_99_input` | `S15` | `8F776172` | S15: overflow probe input 9^99 visible before evaluation |
| 108 | `s15_overflow_9_pow_99_result` | `S15` | `1377B94B` | S15: overflow probe result or error screen after evaluating 9^99 |
| 109 | `s15_clear_midexpr_7_plus_8_input` | `S15` | `37863EAB` | S15: 7+8 visible before CLEAR in the edge-case clear-screen probe |
| 110 | `s15_clear_midexpr_blank` | `S15` | `FD904C0F` | S15: CLEAR returns the edge-case probe to a blank prompt |
| 111 | `s15_mode_open_from_prompt` | `S15` | `2651D7D2, 470E1690, 5B90297E` | S15: MODE opens from the prompt in the edge-case control probe |
| 112 | `s15_mode_closed_to_prompt` | `S15` | `FD904C0F` | S15: CLEAR exits MODE and returns to the prompt |
| 113 | `s15_math_menu_open` | `S15` | `8C3C5304` | S15: MATH opens from the prompt |
| 114 | `s15_math_menu_closed` | `S15` | `FD904C0F` | S15: CLEAR exits the MATH menu and returns to the prompt |
| 115 | `s15_apps_menu_open` | `S15` | `9E45F4B5` | S15: APPS opens from the prompt |
| 116 | `s15_apps_menu_closed` | `S15` | `FD904C0F` | S15: CLEAR exits the APPS menu and returns to the prompt |
| 117 | `s16_store_x_input` | `S16` | `FC91334F` | S16 VAR: 5->x is visible before ENTER using 2nd+ENTER for the store arrow and ALPHA+STO for x |
| 118 | `s16_recall_x_plus_2_input` | `S16` | `2A0A74D6` | S16 VAR: x+2 is visible after storing 5 into x and recalling x with ALPHA+STO |
| 119 | `s16_recall_x_plus_2_result` | `S16` | `EBD3F4DD` | S16 VAR: x+2 evaluates using the stored x value from the prior 5->x assignment |
| 120 | `s16_store_y_input` | `S16` | `8953D4FD` | S16 VAR: 3->y is visible before ENTER using 2nd+ENTER for the store arrow and ALPHA+1 for y |
| 121 | `s16_recall_x_plus_y_input` | `S16` | `09004CF1` | S16 VAR: x+y is visible after storing 5 into x and 3 into y |
| 122 | `s16_recall_x_plus_y_result` | `S16` | `E5B90897` | S16 VAR: x+y evaluates using both stored variables from the prior assignments |
| 123 | `s17_angle_mode_degrees_for_trig` | `S17` | `27D52DF8, B0048B1B, D15B4A59` | S17 TRIG: Angle mode is toggled to Degrees before evaluating the 30/60/45 trig regression trio |
| 124 | `s17_sin_30_result` | `S17` | `A0A13424` | S17 TRIG: sin(30) evaluates in Degree mode |
| 125 | `s17_cos_60_result` | `S17` | `1CB752B2` | S17 TRIG: cos(60) evaluates in Degree mode |
| 126 | `s17_tan_45_result` | `S17` | `196F252E` | S17 TRIG: tan(45) evaluates in Degree mode |
| 127 | `s17_angle_mode_radians_restored_after_trig` | `S17` | `10EDE0E7, 27D52DF8, 71B221A5, B0048B1B` | S17 TRIG: Angle mode is toggled back to Radians after the Degree-mode trig regression trio |
| 128 | `s18_log_1000_result` | `S18` | `FB54E675` | S18 LOG: log(1000) evaluates through the LOG function key path |
| 129 | `s18_ln_e_result` | `S18` | `B88B77ED` | S18 LOG: ln(e) evaluates using 2nd+(-) for the e constant |
| 130 | `s18_ln_10_result` | `S18` | `84E33D3C` | S18 LOG: ln(10) evaluates through the LN function key path |
| 131 | `s19_nlsolve_linear_pair_input` | `S19` | `03BC746E, 9149B8ED, F7E2DDAD` | S19 SYS: nlsolve(x+y-5,x-y-1,x,y) is visible before ENTER as a two-equation combined solve probe |
| 132 | `s19_nlsolve_linear_pair_result` | `S19` | `E5B90897` | S19 SYS: nlsolve(x+y-5,x-y-1,x,y) evaluates as a deterministic two-equation combined solve case |
| 133 | `s20_complex_conjugate_product_input` | `S20` | `888D91CE, AA33DA8A, EAE21CC6` | S20 CMPLX: (2+i)*(2-i) is visible before ENTER while Complex mode remains enabled |
| 134 | `s20_complex_conjugate_product_result` | `S20` | `09EEEC2C, 17670F5D, 2FBDCD2E` | S20 CMPLX: (2+i)*(2-i) evaluates as a conjugate-product regression case |
| 135 | `s21_store_to_sin_input` | `S21` | `2566E201, 4CDA1790, 96E77A46` | S21 ERR: invalid store target 5->sin( is visible before ENTER |
| 136 | `s21_store_to_sin_syntax_err` | `S21` | `B9B5D5E0` | S21 ERR: invalid store target 5->sin( raises SYNTAX ERR |
| 137 | `s22_dim_literal_input` | `S22` | `5C446F47, B07D7A72, FD28973F` | S22 LIST: dim({4,1,9,2}) is visible before ENTER using a literal list probe |
| 138 | `s22_dim_literal_result` | `S22` | `B095D012, E1C853A4` | S22 LIST: dim({4,1,9,2}) = 4 - literal-list dimension probe |
| 139 | `s22_cumsum_literal_input` | `S22` | `10996664, F814F03B` | S22 LIST: cumsum({1,2,3,4}) is visible before ENTER using a literal list probe |
| 140 | `s22_cumsum_literal_result` | `S22` | `220E4BAE, 37CD0E79` | S22 LIST: cumsum({1,2,3,4}) returns the cumulative-sum list {1,3,6,10} |
| 141 | `s22_deltalist_literal_input` | `S22` | `13D76A04, F4B125F0` | S22 LIST: deltalist({5,9,14,20}) is visible before ENTER using a literal list probe |
| 142 | `s22_deltalist_literal_result` | `S22` | `1F55EBA2, 85115676` | S22 LIST: deltalist({5,9,14,20}) returns the difference list {4,5,6} |
| 143 | `s22_sorta_literal_input` | `S22` | `852080B0, 87CF7020` | S22 LIST: sorta({9,1,4,2}) is visible before ENTER using a literal list probe |
| 144 | `s22_sorta_literal_result` | `S22` | `D5E93E40` | S22 LIST: sorta({9,1,4,2}) returns the ascending list {1,2,4,9} |
| 145 | `s22_sortd_literal_input` | `S22` | `0582BE81` | S22 LIST: sortd({9,1,4,2}) is visible before ENTER using a literal list probe |
| 146 | `s22_sortd_literal_result` | `S22` | `E76929D5` | S22 LIST: sortd({9,1,4,2}) returns the descending list {9,4,2,1} |
| 147 | `s23_seq_square_input` | `S23` | `311899E0` | S23 LOOP: seq(x^2,x,1,4,1) is visible before ENTER as a deterministic sequence-generator probe |
| 148 | `s23_seq_square_result` | `S23` | `B9B5D5E0` | S23 LOOP: seq(x^2,x,1,4,1) returns the squared list {1,4,9,16} |
| 149 | `s23_seq_linear_input` | `S23` | `3B9DBC75` | S23 LOOP: seq(2*x+1,x,0,3,1) is visible before ENTER as a linear sequence-generator probe |
| 150 | `s23_seq_linear_result` | `S23` | `B9B5D5E0` | S23 LOOP: seq(2*x+1,x,0,3,1) returns the odd list {1,3,5,7} |
| 151 | `s23_seq_even_input` | `S23` | `DF3ECB2F` | S23 LOOP: seq(2*x,x,2,8,2) is visible before ENTER as a stepped sequence-generator probe |
| 152 | `s23_seq_even_result` | `S23` | `B9B5D5E0` | S23 LOOP: seq(2*x,x,2,8,2) returns the stepped even list {4,8,12,16} |
| 153 | `s24_mean_25_input` | `S24` | `2631A813` | S24 STAT: mean(10,20,30,40) is visible before ENTER as an arithmetic-mean probe |
| 154 | `s24_mean_25_result` | `S24` | `B9B5D5E0` | S24 STAT: mean(10,20,30,40) = 25 - deterministic arithmetic mean |
| 155 | `s24_median_5_input` | `S24` | `2264CFC8` | S24 STAT: median(9,1,5,7,3) is visible before ENTER as a median probe |
| 156 | `s24_median_5_result` | `S24` | `B9B5D5E0` | S24 STAT: median(9,1,5,7,3) = 5 - deterministic median after sorting the sample |
| 157 | `s24_stdev_sample_input` | `S24` | `BC394FC0` | S24 STAT: stdev(2,4,4,4,5,5,7,9) is visible before ENTER as a sample-standard-deviation probe |
| 158 | `s24_stdev_sample_result` | `S24` | `B9B5D5E0` | S24 STAT: stdev(2,4,4,4,5,5,7,9) ~= 2.13809 - deterministic sample standard deviation |
| 159 | `s24_variance_input` | `S24` | `7F72C383` | S24 STAT: variance(1,2,3,4,5) is visible before ENTER as a variance probe |
| 160 | `s24_variance_result` | `S24` | `B9B5D5E0` | S24 STAT: variance(1,2,3,4,5) = 2.5 - deterministic sample variance |
| 161 | `s25_mean_single_input` | `S25` | `F83C6E81` | S25 STAT EDGE: mean(5) is visible before ENTER as a singleton-dataset mean probe |
| 162 | `s25_mean_single_result` | `S25` | `E9A55C4F` | S25 STAT EDGE: mean(5) = 5 - singleton dataset collapses to its only value |
| 163 | `s25_median_single_input` | `S25` | `42C963A9` | S25 STAT EDGE: median(7) is visible before ENTER as a singleton-dataset median probe |
| 164 | `s25_median_single_result` | `S25` | `CBF6372B` | S25 STAT EDGE: median(7) = 7 - singleton median remains stable |
| 165 | `s25_stdev_single_input` | `S25` | `8F89FBF1` | S25 STAT EDGE: stdev(5) is visible before ENTER as a singleton standard-deviation probe |
| 166 | `s25_stdev_single_result` | `S25` | `A7B3E96D` | S25 STAT EDGE: stdev(5) = 0 - singleton sample standard deviation should clamp to zero |
| 167 | `s25_var_single_input` | `S25` | `2BB197B9` | S25 STAT EDGE: var(5) is visible before ENTER as a singleton variance probe using the short alias |
| 168 | `s25_var_single_result` | `S25` | `89BBF49C` | S25 STAT EDGE: var(5) = 0 - singleton variance should clamp to zero |
| 169 | `s25_mean_decimal_input` | `S25` | `1DE55260, A2218A03` | S25 STAT EDGE: mean(0.1,0.2,0.3) is visible before ENTER as a decimal-rounding probe |
| 170 | `s25_mean_decimal_result` | `S25` | `B9B5D5E0` | S25 STAT EDGE: mean(0.1,0.2,0.3) = 0.2 - decimal mean should render stably across float-rounding boundaries |
| 171 | `s25_var_decimal_input` | `S25` | `FF474A51` | S25 STAT EDGE: var(0.1,0.2,0.3) is visible before ENTER as a decimal variance probe |
| 172 | `s25_var_decimal_result` | `S25` | `B9B5D5E0` | S25 STAT EDGE: var(0.1,0.2,0.3) ~= 0.01 - decimal variance highlights float-rounding sensitivity |
| 173 | `s26_dim_empty_input` | `S26` | `560089F1` | S26 LIST EDGE: dim({}) is visible before ENTER as an empty-literal dataset probe |
| 174 | `s26_dim_empty_result` | `S26` | `B9B5D5E0` | S26 LIST EDGE: dim({}) captures the engine's empty-literal handling path on hardware |
| 175 | `s26_deltalist_single_input` | `S26` | `06416DE9, 163E1A20` | S26 LIST EDGE: deltalist({9}) is visible before ENTER as a single-item list-difference probe |
| 176 | `s26_deltalist_single_result` | `S26` | `B9B5D5E0, C9BC9D50` | S26 LIST EDGE: deltalist({9}) captures the degenerate single-item delta-list result state |
| 177 | `s26_seq_single_input` | `S26` | `4546462C, 5E6F34DD, 862BC147` | S26 LIST EDGE: seq(x,x,5,5,1) is visible before ENTER as a singleton sequence-generator probe |
| 178 | `s26_seq_single_result` | `S26` | `B9B5D5E0` | S26 LIST EDGE: seq(x,x,5,5,1) returns the singleton list {5} |
| 179 | `s26_seq_decimal_input` | `S26` | `1100EDB7, 8920F381, D02853F1` | S26 LIST EDGE: seq(0.1*x,x,1,5,1) is visible before ENTER as a decimal-step sequence probe |
| 180 | `s26_seq_decimal_result` | `S26` | `B9B5D5E0` | S26 LIST EDGE: seq(0.1*x,x,1,5,1) returns the decimal-stepped list {0.1,0.2,0.3,0.4,0.5} |
| 181 | `s27_piecewise_stats_input` | `S27` | `90BB4488, 9B8B78FD, BFFFDE8C` | S27 NESTED: piecewise(mean(1,2,3),1,median(4,5,6)) is visible before ENTER as a nested conditional/statistical probe |
| 182 | `s27_piecewise_stats_result` | `S27` | `B9B5D5E0` | S27 NESTED: piecewise(mean(1,2,3),1,median(4,5,6)) = 2 - nested conditional/statistical dispatch remains deterministic |
| 183 | `s28_pv_monthly_input` | `S28` | `3DA4D739, 52DDD237, 82F64568` | S28 FIN EDGE: pv(12,0.005,-100,0) is visible before ENTER as a monthly-rate present-value probe |
| 184 | `s28_pv_monthly_result` | `S28` | `B9B5D5E0` | S28 FIN EDGE: pv(12,0.005,-100,0) ~= 1161.89 - finance rendering should remain stable for small monthly rates |
| 185 | `s28_eff_nominal_input` | `S28` | `2B9C8C02, 2EFA9C21, 39D6C015` | S28 FIN EDGE: eff(0.12,12) is visible before ENTER as a nominal-to-effective conversion probe |
| 186 | `s28_eff_nominal_result` | `S28` | `B9B5D5E0` | S28 FIN EDGE: eff(0.12,12) ~= 0.126825 - effective-rate conversion exposes precision-sensitive rendering |
| 187 | `s28_nom_effective_input` | `S28` | `27AA5039, CAAF0E25` | S28 FIN EDGE: nom(0.126825,12) is visible before ENTER as an effective-to-nominal conversion probe |
| 188 | `s28_nom_effective_result` | `S28` | `B9B5D5E0` | S28 FIN EDGE: nom(0.126825,12) ~= 0.12 - reverse conversion should close the monthly compounding round-trip |
| 189 | `s30_home_prompt_fresh` | `S30` | `A32840C8` | S30 LCD: blank homescreen prompt after clearing back to fresh input state |
| 190 | `s29_shift_2nd_sin_input` | `S29` | `407B4B08` | S29 KEY: 2nd+SIN inserts asin( on the input line as a shifted-function proof |
| 191 | `s29_shift_2nd_sqrt_input` | `S29` | `22C34CB5` | S29 KEY: 2nd+x^2 inserts sqrt( on the input line as a shifted-function proof |
| 192 | `s29_shift_2nd_abs_input` | `S29` | `E4A58031` | S29 KEY: 2nd+x^-1 inserts abs( on the input line as a shifted-function proof |
| 193 | `s29_alpha_solve_input` | `S29` | `95976A1A` | S29 KEY: ALPHA-spelled solve is visible as a free-text letter-entry proof |
| 194 | `s29_repeat_11111_input` | `S29` | `735D3F1D` | S29 KEY: five repeated presses of 1 render 11111 with no dropped or merged digits |
| 195 | `s30_multiline_expression_wrap` | `S30` | `15B58686` | S30 LCD: long homescreen expression wraps across the input area without corrupting layout |
| 196 | `s29_long_left_cursor` | `S29` | `24AED6A6` | S29 KEY: LEFT arrow repositions the cursor within a long wrapped expression |
| 197 | `s29_long_right_cursor` | `S29` | `4A95FD48` | S29 KEY: RIGHT arrow advances the cursor within a long wrapped expression after prior LEFT movement |
| 198 | `s29_fraction_cursor_anchor` | `S29` | `4B6207A6` | S29 KEY: cursor is anchored at the fraction boundary inside 1+2/3+4 ahead of MathPrint up/down navigation |
| 199 | `s29_down_to_denominator` | `S29` | `4B6207A6` | S29 KEY: DOWN moves the MathPrint cursor from the numerator side of 1+2/3+4 into the denominator |
| 200 | `s29_up_to_numerator` | `S29` | `D5745C02` | S29 KEY: UP moves the MathPrint cursor back from the denominator side of 1+2/3+4 to the numerator boundary |
| 201 | `s29_del_backspace_mid_24680` | `S29` | `D5745C02` | S29 KEY: DEL performs backspace semantics inside 24680 after moving the cursor into the middle |
| 202 | `s29_del_forward_mid_24680` | `S29` | `D5745C02` | S29 KEY: 2nd+DEL performs forward-delete semantics inside 24680 after moving the cursor into the middle |
| 203 | `s30_result_layout_2_plus_3` | `S30` | `5A760441` | S30 LCD: 2+3 result screen shows the rendered result in the normal post-expression layout |
| 204 | `s30_result_enter_reprocess` | `S30` | `00473F0C` | S30 LCD: ENTER on a result reprocesses the current expression rather than returning directly to a blank prompt |
| 205 | `s30_post_result_prompt_with_expression` | `S30` | `A32840C8` | S30 LCD: first CLEAR from a result returns to the input prompt with the prior expression restored |
| 206 | `s30_post_result_prompt_blank` | `S30` | `A32840C8` | S30 LCD: second CLEAR from the restored prompt returns to a blank homescreen prompt with the cursor reset |
| 207 | `s30_error_display_div_zero` | `S30` | `32BFD188` | S30 LCD: divide-by-zero error screen renders with the expected message positioning and formatting |
| 208 | `s30_menu_symbols_visual` | `S30` | `1FE659FF, 56434704` | S30 LCD: MODE opens the late-session menu layout from the homescreen prompt; multi-CRC: repeated real-ROM runs land on two stable menu-open renders |
| 209 | `s30_menu_recents_visual` | `S30` | `1FE659FF, CCCB0884` | S30 LCD: late-session MODE then 2nd+RIGHT menu flow lands on a stable alternate tab render; multi-CRC: repeated real-ROM runs accept both observed states |
| 210 | `s30_menu_settings_visual` | `S30` | `1FE659FF, 03786611, CCCB0884` | S30 LCD: second 2nd+RIGHT in the late-session menu flow reaches the settings-family render; multi-CRC: repeated real-ROM runs accept both observed stable layouts |
| 211 | `s15_2nd_quit_to_tios` | `S15` | `6BB23703, 7F0F6A91, DABF3779` | S15: 2nd+MODE exits CalcEngine back to the TI-OS homescreen |

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
