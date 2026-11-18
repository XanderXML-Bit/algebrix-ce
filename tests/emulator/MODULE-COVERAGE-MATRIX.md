# CalcEngine Module × Emulator-Layer Coverage Matrix

Generated: 2026-04-13 (updated gap-close pass)
Scope: 35 official `.8xp` modules per `tests/emulator/manifest.json` (module_count=35).
Harness: headless TI-84 CE emulator at `tools/emulator/cemu-headless/` only. No desktop, no GUI CEmu.
Reference suite: `tests/emulator/autotest.comprehensive.json` (211 hash assertions — PR #19 style).
Module-coverage suite: `tests/emulator/autotest.module-coverage.json` (31 hashes: 1 boot + 12 original + 18 gap-close).
mat2 parser extension: `src/parser.c` `parse_mat2_literal` — enables CEMAT probe without semicolons.

**Coverage rule.** A module is "covered" when at least one emulator-layer assertion
(a) loads the module's `.8xp` via the CalcEngine dispatch path in the emulator, and
(b) captures a deterministic VRAM CRC on a post-result screen.
CRC-baseline is the PR #19 approach; `cemu-headless` does not yet expose LCD text extraction,
so text-substring assertions are deferred to a separate tool-capability PR (see "Gaps" below).

## Summary

| Inventory contract | Count |
| --- | --- |
| Official programs in manifest | 35 |
| Programs assigned direct probes in this matrix | 34 |
| Programs assigned a nearest-neighbor probe | 1 |

These assignments describe probe coverage, not a fresh replay of the rebuilt package.
Re-capture screen baselines on the self-hosted real-ROM lane before accepting changed rendering.
See [hardware deployment](../../docs/HARDWARE-DEPLOYMENT.md#retired-modules) for retired binaries.

## Matrix

Legend: **comp** = covered by existing `autotest.comprehensive.json`; **new** = covered by
`autotest.module-coverage.json` added in this PR; **gap** = still uncovered, follow-up work required.

| # | Module (.8xp) | Bin dir | Role / Dispatcher target | Existing coverage (comprehensive.json hash) | New coverage (this PR) | Status |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | CALCENG   | core / launcher / monolith | Launcher home prompt; dispatches every worker                            | `s00_boot_home`; every launch path     | —                               | **comp** |
| 2 | CEALG     | alg / algebra              | Algebra dispatcher → CEASLV / CEALGF / CEALGSY / CEAPDV / CEALGEO         | `s10_solve_x_plus2`, `s10_expand_x1_sq` | —                              | **comp** |
| 3 | CEALGEO   | algeo                      | Algebra+geometry (midpoint, distance, slope)                             | —                                       | `s99_cealgeo_slope`             | **new**  |
| 4 | CEALGF    | algf                       | Polynomial factor/expand                                                 | `s10_expand_x1_sq`                      | —                               | **comp** |
| 5 | CEALGSY   | algsy                      | Algebra symbolic                                                         | —                                       | `s99_cealgsy_csquare`           | **new**  |
| 6 | CEAPDV    | algpdv                     | Partial derivatives                                                      | —                                       | `s99_pdiff_x2_plus_y2`          | **new**  |
| 7 | CEASLA    | algsolve_adv               | Solve advanced (Newton/bisect/secant fallback)                           | —                                       | `s99_ceasla_solve_cubic`        | **new**  |
| 8 | CEASLV    | algsolve                   | Solve primary                                                            | `s10_solve_x_plus2`                     | —                               | **comp** |
| 9 | CEBASE    | base                       | Base-N conversion                                                        | —                                       | `s99_cebase_newton`             | **new**  |
| 10 | CECALC    | calculus                   | Core calculus dispatcher (d/dx, nderiv, pure arith passthrough)          | S01–S07 arith, `s10_deriv_x2`, `s10_nderiv_x2_at1` | —                    | **comp** |
| 11 | CECIAD    | calc_int_adv               | Integral advanced dispatcher → CEIPFR / CEITRG / CEIUSB / CEIPRT / CEISTR | —                                      | `s99_ceciad_int_xlnx`            | **new**  |
| 12 | CECIDF    | calc_int_def               | Definite integral                                                        | —                                       | `s99_int_x2_def_0_1`            | **new**  |
| 13 | CECINT    | calc_int                   | Integral dispatcher → CECIDF / CECISM / CECIAD / CECODE                  | `s10_int_x2` (via CECISM)               | `s99_int_x2_def_0_1` (via CECIDF) | **comp** |
| 14 | CECISM    | calc_int_simple            | Indefinite integral                                                      | `s10_int_x2`                            | —                               | **comp** |
| 15 | CECLIA    | calc_lim_adv               | Limits advanced fallback                                                 | —                                       | `s99_ceclia_lim_xx`             | **new**  |
| 16 | CECLIC    | calc_lim_cont              | Continuity test                                                          | —                                       | `s99_continuity_x_at_0`         | **new**  |
| 17 | CECLIM    | calc_lim                   | Limits dispatcher → CECLIC / CECLIR / CECLMB                             | `s10_lim_sinx_over_x` (via CECLMB)      | `s99_continuity_x_at_0` (via CECLIC) | **comp** |
| 18 | CECLIR    | calc_lim_rel               | Related rates                                                            | —                                       | `s99_ceclir_relrate`            | **new**  |
| 19 | CECLMB    | calc_lim_base              | Limits base worker                                                       | `s10_lim_sinx_over_x`                   | —                               | **comp** |
| 20 | CECODE    | calc_ode                   | ODE solver                                                               | —                                       | `s99_cecode_diffeq`             | **new**  |
| 21 | CECPNM    | calc_pol_num               | Polynomial numeric ops                                                   | —                                       | `s99_cecpnm_arclen`             | **new**  |
| 22 | CECPOL    | calc_pol                   | Polynomial division / polar helpers                                      | —                                       | `s99_polydiv_x2m1_xp1`          | **new**  |
| 23 | CECSEQ    | calc_seq                   | Sequences                                                                | —                                       | `s99_cecseq_arithsum`           | **new**  |
| 24 | CECSER    | calc_series                | Series (sum, product)                                                    | —                                       | `s99_sum_k_1_to_5`              | **new**  |
| 25 | CEFIN     | fin / finance              | TVM (pv/fv/pmt)                                                          | `s10_fin_pv_*`, `s10_fin_fv_*`, `s10_fin_pmt_*` | —                        | **comp** |
| 26 | CEGRPH    | graph / graphing           | Graphing / plotting                                                      | —                                       | `s99_cegrph_graph`              | **new**  |
| 27 | CEIPFR    | calc_int_pfrac             | Partial-fraction integration                                             | —                                       | `s99_pfrac_1_over_x`            | **new**  |
| 28 | CEIPRT    | calc_int_parts             | Integration by parts                                                     | —                                       | `s99_intparts_x`                | **new**  |
| 29 | CEISTR    | calc_int_report            | Integration strategy report                                              | —                                       | `s99_intstrategy_x`             | **new**  |
| 30 | CEITRG    | calc_int_trig              | Trig-substitution integration                                            | —                                       | `s99_trigsub_sinx`              | **new**  |
| 31 | CEIUSB    | calc_int_usub              | u-substitution integration                                               | —                                       | `s99_usub_2x`                   | **new**  |
| 32 | CELIMS    | limits_series              | Limits+series combined worker                                            | —                                       | `s99_celims_nearest` (nearest-neighbor CECLMB) | **gap (bypassed)** |
| 33 | CELSCX    | lists_complex              | Lists with complex elements                                              | —                                       | `s99_celscx_dim_l1`             | **new**  |
| 34 | CEMAT     | mat / matrix               | Matrix operations (det, rref, inv, …)                                    | — (was blocked — see `matrix-gap-analysis.md`) | `s99_cemat_det_mat2` (mat2 parser fix) | **new**  |
| 35 | CESTPRB   | stats_prob                 | Probability distributions (normal, binom, t, chi2)                       | —                                       | `s99_normalpdf_0_0_1`           | **new**  |

## Launcher routing gap

The remaining nearest-neighbor assignment is below.

**CELIMS** — Limits+series combined worker. Module exists and is built, but
`route_program` in `src/core/launcher/main.c` sends `NODE_LIMIT` directly to
`CE_PRGM_CALCLM` ("CECLIM"), bypassing `CE_PRGM_LIMSER` ("CELIMS") entirely.
CELIMS is only reachable from the `ipc.c` domain-routing path which CALCENG does not use.
Nearest-neighbor probe: `s99_celims_nearest` → CECLMB.
Fix: refactor `route_program` to route complex limits and series via CELIMS.

## Tool-capability gap (informational)

The user directive was for assertions to "capture LCD text … assert the expected substring
appears." The headless fork at `tools/emulator/cemu-headless/` currently exposes `get_screen`
only in CRC and PNG modes — no text extraction. The eZ80 scaffold has
`ez80_emulator_get_screen_text_state` but lives on a separate path.

This PR follows PR #19's CRC-baseline approach (identical to `autotest.comprehensive.json`)
to stay inside the supported capability envelope. Text-substring assertion capability is
a prerequisite follow-up before it can be enabled here.

## Runner

Use the existing orchestrator with the new suite path. `run-comprehensive-autotest.ps1`
is hardcoded to its own suite, so the new suite is invoked either by a thin wrapper or
by direct `autotester.exe` / `cemu-headless.exe` invocation using the same `autotest.*.json`
config shape. See `PR-DESCRIPTION.md` for the `-EmulatorBackend CEmuHeadless` path.

Strict backend policy enforced by caller (no GUI fallback): if
`tools/emulator/cemu-headless/cemu-headless.exe` is missing or fails to run headless,
the runner stops and flags — it does not fall through to a system-installed CEmu.
