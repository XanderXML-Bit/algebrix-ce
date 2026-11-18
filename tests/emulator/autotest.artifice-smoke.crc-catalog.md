# CalcEngine autotest.artifice-smoke CRC Catalog

Generated: 2026-04-19T05:16:42.0098546-07:00
Source: `C:\Users\user\AppData\Local\Temp\calcengine-wrapper-rebaseline-pass1\tests\emulator\autotest.artifice-smoke.baselined.json`
Hashes: 19

| # | Hash | Section | Expected CRCs | Description |
| --- | --- | --- | --- | --- |
| 1 | `artifice_shell_loaded` | `UNSPECIFIED` | `EAC3FDAC` | arTIfiCE shell screen after launching prgmA |
| 2 | `artifice_quit_transition` | `UNSPECIFIED` | `28B88B56` | Screen state after the verified CLEAR -> MODE shell-exit choreography, before reopening PRGM |
| 3 | `ti_os_program_menu_open` | `UNSPECIFIED` | `C84EC5D0` | TI-OS PRGM menu after the verified CLEAR -> MODE -> PRGM arTIfiCE exit sequence |
| 4 | `calcengine_launch_screen` | `UNSPECIFIED` | `28B88B56` | CalcEngine landing screen after exiting arTIfiCE and launching CALCENG |
| 5 | `calcengine_result_2plus3` | `UNSPECIFIED` | `ED152D3E` | CalcEngine result screen for 2+3 through the jailbroken path |
| 6 | `calcengine_prompt_after_2plus3` | `UNSPECIFIED` | `A32840C8` | Prompt restored after dismissing the 2+3 result |
| 7 | `calcengine_result_6times7` | `UNSPECIFIED` | `46B2FD53` | CalcEngine result screen for 6*7 through the jailbroken path |
| 8 | `calcengine_result_neg5_plus10` | `UNSPECIFIED` | `4D746D07` | CalcEngine result screen for -5+10 through the jailbroken path |
| 9 | `calcengine_result_decimal_6_28` | `UNSPECIFIED` | `F9FE44E6` | CalcEngine result screen for 3.14*2 through the jailbroken path |
| 10 | `calcengine_result_precedence_14` | `UNSPECIFIED` | `93C9C32E` | CalcEngine result screen for 2+3*4 through the jailbroken path |
| 11 | `calcengine_result_parens_27` | `UNSPECIFIED` | `3956B827` | CalcEngine result screen for (4+5)*3 through the jailbroken path |
| 12 | `calcengine_result_power_256` | `UNSPECIFIED` | `C1B0FC3A` | CalcEngine result screen for 2^8 through the jailbroken path |
| 13 | `calcengine_stat_mean_3` | `UNSPECIFIED` | `B9B5D5E0` | CalcEngine result screen for mean(1,2,3,4,5) through the jailbroken path |
| 14 | `calcengine_nderiv_x2_at1` | `UNSPECIFIED` | `B9B5D5E0` | CalcEngine result screen for nderiv(x^2,x,1) through the jailbroken path |
| 15 | `calcengine_divide_by_zero_error` | `UNSPECIFIED` | `32BFD188` | CalcEngine error screen for 1/0 through the jailbroken path |
| 16 | `calcengine_menu_open` | `UNSPECIFIED` | `D12593CE` | CalcEngine menu screen after pressing MODE |
| 17 | `calcengine_menu_recents` | `UNSPECIFIED` | `D12593CE` | CalcEngine Recently Used tab after MODE then 2nd+RIGHT |
| 18 | `calcengine_menu_settings` | `UNSPECIFIED` | `CDBBAC20` | CalcEngine Settings tab after MODE then 2nd+RIGHT twice |
| 19 | `calcengine_menu_closed` | `UNSPECIFIED` | `828489BA` | CalcEngine prompt after closing the menu with CLEAR |