# CalcEngine autotest.enter-path-proof CRC Catalog

Generated: 2026-04-13T20:17:56.870283-07:00
Source: `C:\src\algebrix-ce\tests\emulator\autotest.enter-path-proof.baselined.json`
Hashes: 19

| # | Hash | Section | Expected CRCs | Description |
| --- | --- | --- | --- | --- |
| 1 | `ep00_boot_prompt` | `UNSPECIFIED` | `B76BC405` | Enter proof: launcher prompt is visible after boot and initial clear |
| 2 | `ep01_empty_enter_stays_on_prompt` | `UNSPECIFIED` | `9F1FC020` | Enter proof: pressing Enter on an empty prompt does not exit CalcEngine or crash the session |
| 3 | `ep02_add_result_5` | `UNSPECIFIED` | `B9B5D5E0` | Enter proof: 2+3 submitted through the canonical Enter path renders the result screen |
| 4 | `ep03_result_enter_returns_prompt` | `UNSPECIFIED` | `93BEC16E` | Enter proof: pressing Enter on a result screen returns to an in-app prompt state instead of exiting |
| 5 | `ep04_mul_result_42_after_result_enter` | `UNSPECIFIED` | `BFE0B809` | Enter proof: a second expression evaluates successfully after returning from the prior result with Enter |
| 6 | `ep05_rapid_enter_burst_stays_in_app` | `UNSPECIFIED` | `0B324DDD` | Enter proof: a rapid burst of Enter presses after a result leaves CalcEngine alive and on-screen |
| 7 | `ep06_div_zero_error_after_rapid_enter` | `UNSPECIFIED` | `32BFD188` | Enter proof: the app still routes through the normal error screen after the rapid Enter burst |
| 8 | `ep07_prompt_after_error_clear` | `UNSPECIFIED` | `284B7FFE` | Enter proof: CLEAR recovers from the error screen back to the prompt without leaving CalcEngine |
| 9 | `ep08_error_recovery_10` | `UNSPECIFIED` | `05439989` | Enter proof: a valid expression still evaluates after an error recovery cycle |
| 10 | `ep09_long_expr_input` | `UNSPECIFIED` | `A7F92410` | Enter proof: a long expression remains intact on the input line before submission |
| 11 | `ep10_long_expr_result_16` | `UNSPECIFIED` | `392DF11A` | Enter proof: the long expression still evaluates correctly through the Enter path |
| 12 | `ep11_post_long_expr_enter_prompt` | `UNSPECIFIED` | `02A80E3E` | Enter proof: pressing Enter after the long-expression result returns to a prompt-ready in-app state |
| 13 | `ep12_large_mul_input` | `UNSPECIFIED` | `CBAE5596` | Enter proof: a very large multiplication input survives entry intact before submission |
| 14 | `ep13_large_mul_result` | `UNSPECIFIED` | `A479ADEF` | Enter proof: a very large multiplication still renders a deterministic result screen |
| 15 | `ep14_post_large_mul_enter_prompt` | `UNSPECIFIED` | `102C5FDB` | Enter proof: pressing Enter after the large-number result returns to an in-app prompt state |
| 16 | `ep15_third_chain_result_5` | `UNSPECIFIED` | `1EEF38D7` | Enter proof: a third chained solve still succeeds after multiple result-enter cycles |
| 17 | `ep16_post_third_chain_enter_prompt` | `UNSPECIFIED` | `22919447` | Enter proof: the prompt remains stable after a third chained result-enter cycle |
| 18 | `ep17_error_screen_before_quit` | `UNSPECIFIED` | `32BFD188` | Enter proof: divide-by-zero still lands on the TI-OS error dialog after multiple chained Enter cycles |
| 19 | `ep18_error_quit_to_tios_home` | `UNSPECIFIED` | `0ED201DD` | Enter proof: quitting the TI-OS error dialog with 2nd+MODE returns cleanly to the TI-OS homescreen |

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
