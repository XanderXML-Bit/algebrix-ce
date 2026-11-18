# CalcEngine autotest.module-coverage CRC Catalog

Generated: 2026-04-13 (gap-close pass; CRCs are placeholder `00000000` until captured on real ROM)
Source: `tests/emulator/autotest.module-coverage.json`
Hashes: 31 (1 boot + 12 original module probes + 18 gap-close probes)
Harness: `tools/emulator/cemu-headless/` only — strict, no GUI fallback.

| # | Hash | Section / Module | Expected CRCs | Description |
| --- | --- | --- | --- | --- |
| 1  | `s99_boot_home`            | boot / CALCENG     | `00000000` (to baseline) | Boot and clear - CalcEngine launcher home prompt visible, ready for input (module-coverage suite) |
| 2  | `s99_int_x2_def_0_1`       | CECIDF             | `00000000` (to baseline) | `int(x^2,x,0,1)` = 1/3 — definite integral, CECINT -> CECIDF dispatch |
| 3  | `s99_intparts_x`           | CEIPRT             | `00000000` (to baseline) | `intparts(x,x)` = x^2/2 — integration by parts, CECINT -> CECIAD -> CEIPRT |
| 4  | `s99_trigsub_sinx`         | CEITRG             | `00000000` (to baseline) | `trigsub(sin(x),x)` = -cos(x)+C — trig substitution, CECINT -> CECIAD -> CEITRG |
| 5  | `s99_usub_2x`              | CEIUSB             | `00000000` (to baseline) | `usub(2*x,x)` = x^2+C — u-substitution, CECINT -> CECIAD -> CEIUSB |
| 6  | `s99_pfrac_1_over_x`       | CEIPFR             | `00000000` (to baseline) | `pfrac(1/x,x)` = ln\|x\|+C — partial-fraction integration, CECINT -> CECIAD -> CEIPFR |
| 7  | `s99_intstrategy_x`        | CEISTR             | `00000000` (to baseline) | `intstrategy(x,x)` — strategy report, CECINT -> CECIAD -> CEISTR |
| 8  | `s99_sum_k_1_to_5`         | CECSER             | `00000000` (to baseline) | `sum(k,k,1,5)` = 15 — series partial sum |
| 9  | `s99_continuity_x_at_0`    | CECLIC             | `00000000` (to baseline) | `continuity(x,x,0)` = continuous — CECLIM -> CECLIC |
| 10 | `s99_pdiff_x2_plus_y2`     | CEAPDV             | `00000000` (to baseline) | `pdiff(x^2+y^2,x)` = 2x — partial derivative, CEALG -> CEAPDV |
| 11 | `s99_polydiv_x2m1_xp1`     | CECPOL             | `00000000` (to baseline) | `polydiv(x^2-1,x+1)` = x-1 — polynomial division |
| 12 | `s99_ncr_5_2`              | CESTPRB             | `00000000` (to baseline) | `ncr(5,2)` = 10 — combination count |
| 13 | `s99_normalpdf_0_0_1`      | CESTPRB            | `00000000` (to baseline) | `normalpdf(0,0,1)` ~= 0.3989 — standard-normal PDF |
| 14 | `s99_cealgeo_slope`        | CEALGEO            | `00000000` (to baseline) | `slope(1,2,3,4)` = 1 — CALCENG -> CEALGEO (POLOP_SLOPE) |
| 15 | `s99_cealgsy_csquare`      | CEALGSY            | `00000000` (to baseline) | `csquare(x^2+4x,x)` — CALCENG -> CEALGSY (POLOP_CSQUARE) |
| 16 | `s99_cealga_descartes` | CEALGSY | `00000000` (to baseline) | `descartes(x^3-6x+4,x)` via CEALGSY |
| 17 | `s99_cealgs_tofrac` | CEALGEO | `00000000` (to baseline) | `tofrac(0.25)` = 1/4 via CEALGEO |
| 18 | `s99_ceasla_solve_cubic`   | CEASLA             | `00000000` (to baseline) | `solve(x^3-8,x)` — cubic triggers CEAADVREQ in CEASLV -> CEASLA advanced solver |
| 19 | `s99_ceasym_pfrac_x` | CEALGSY | `00000000` (to baseline) | `pfrac(x,x)` via CEALGSY |
| 20 | `s99_cebase_newton`        | CEBASE             | `00000000` (to baseline) | `newton(x^2-2,x,1)` ~= sqrt(2) — POLOP_NEWTON default route -> CEBASE |
| 21 | `s99_ceciad_int_xlnx`      | CECIAD             | `00000000` (to baseline) | `int(x*ln(x),x)` — CECISM fails -> CECINT forwards to CECIAD dispatcher |
| 22 | `s99_ceclia_lim_xx`        | CECLIA             | `00000000` (to baseline) | `lim(x^x,x,0)` = 1 — 0^0 indeterminate forces CECLMB -> CECLIA |
| 23 | `s99_ceclir_relrate`       | CECLIR             | `00000000` (to baseline) | `relrate(x^2+y^2,t)` — CALCENG -> CECLIM (POLOP_RELRATE) -> CECLIR |
| 24 | `s99_cecode_diffeq`        | CECODE             | `00000000` (to baseline) | `diffeq(y,x,y)` y'=y -> y=Ce^x — CECINT (POLOP_DIFFEQ) -> CECODE |
| 25 | `s99_cecpnm_arclen`        | CECPNM             | `00000000` (to baseline) | `arclen(sin(x),x,0,1)` — CALCENG -> CECPOL (POLOP_ARCLEN) -> CECPNM default |
| 26 | `s99_cecseq_arithsum`      | CECSEQ             | `00000000` (to baseline) | `arithsum(1,2,5)` = 25 — CALCENG -> CECSEQ (POLOP_ARITHSUM) |
| 27 | `s99_cegrph_graph`         | CEGRPH             | `00000000` (to baseline) | `graph(x^2)` — CALCENG -> CEGRPH (POLOP_GRAPH); VRAM CRC of rendered LCD plot |
| 28 | `s99_celims_nearest`       | CELIMS (bypassed, nearest-neighbor CECLMB)     | `00000000` (to baseline) | `lim(x^2,x,3)` = 9 — CELIMS bypassed by CALCENG direct dispatch; CECLMB nearest-neighbor |
| 29 | `s99_celscx_dim_l1`        | CELSCX             | `00000000` (to baseline) | `dim(L1)` — NODE_LIST_OP -> CALCENG -> CELSCX (CE_PRGM_LISTCMPLX) |
| 30 | `s99_cemat_det_mat2`       | CEMAT              | `00000000` (to baseline) | `det(mat2(1,2,3,4))` = -2 — new mat2() parser extension; NODE_MATRIX_OP -> CEMAT |
| 31 | `s99_tmpcalc_int_x3` | CECIDF via CECINT | `00000000` (to baseline) | `int(x^3,x,0,1)` = 1/4 via CECIDF |

## Baseline capture

After applying this patch, capture the CRCs on the real ROM via the headless emulator.
This now covers all 31 hashes (1 boot + 12 original + 18 gap-close).
Expected invocation shape (mirrors `run-comprehensive-autotest.ps1 -Mode Baseline`):

```
tools\emulator\cemu-headless\cemu-headless.exe `
    --rom rom\Rom.rom `
    --autotest tests\emulator\autotest.module-coverage.json `
    --mode baseline `
    --output tests\emulator\autotest.module-coverage.baselined.json
```

Once `autotest.module-coverage.baselined.json` exists with captured CRCs, regenerate this
catalog from it (same generator used for `autotest.comprehensive.crc-catalog.md`) and
replace the `00000000` placeholders above.

## ALPHA key map reference (for auditing the sequence)

From `scripts/cemu/artifice_jailbreak.py:ALPHA_KEY_MAP`:

```
A=math   B=apps   C=prgm   D=(-1)   E=sin    F=cos    G=tan    H=^
I=^2     J=,      K=(      L=)      M=/      N=7      O=8      P=9
Q=*      R=4      S=5      T=6      U=-      V=1      W=2      X=3
Y=+      Z=0
```

Every letter in an identifier is entered as `key|alpha` followed by the mapped key.
`key|xton` enters the literal variable X (via the `X,T,θ,n` key, not via ALPHA).
`key|alpha, key|+` enters the literal variable Y.
