# CalcEngine Hardware Test Checklist

> **Public source note:** the staged `deploy/hardware-ready/` transfer package is not part of this repository. Build the modules from source with CEdev as described in the [README](../README.md).

## Preconditions

- [ ] The calculator OS version is recorded.
- [ ] If the OS is `5.5` through `5.8`, arTIfiCE has been activated for this boot.
- [ ] `deploy/hardware-ready/` contains the full blank-slate package: `35` CalcEngine `.8xp` files + `arTIfiCE.8xp` + `4` runtime `.8xv` AppVars.
- [ ] All `35` CalcEngine `.8xp` files plus `arTIfiCE.8xp` and the `4` runtime `.8xv` AppVars have been transferred.
- [ ] The four runtime `.8xv` AppVars are archived after transfer.

## Directed first-try diagnostic capture

This section is the minimum real-device diagnostic pass. The pre-hardware emulator/tester lane covers the same items, but it does not certify a build on physical hardware. Record which build you tested and whether it matched the emulator. Do **not** treat this checklist as release certification. See the [historical v2.0.0 failure](V2_HARDWARE_BUGS.md) and the [current status](../README.md#current-status).

- [ ] `CALCENG` launches from `[prgm]` without `ERR: INVALID` or `ERR: MEMORY`.
- [ ] The first prompt shows `CalcEngine` and `Expr (MODE exits):`.
- [ ] `2+3` returns `5`.
- [ ] `6*7` returns `42`.
- [ ] `-5+10` returns `5`.
- [ ] `3.14*2` returns `6.28` or the calculator's equivalent rounded decimal.
- [ ] `(4+5)*3` returns `27`.
- [ ] `2^8` returns `256`.
- [ ] `1/0` shows a recoverable divide-by-zero error instead of crashing.
- [ ] Open and close the CalcEngine menu, then confirm the prompt is still responsive.
- [ ] After each result/error/menu check, return to the `Expr (MODE exits):` prompt before continuing.
- [ ] Record the exact screen/result, any literal format strings, crashes, extra keypresses, prompt-return drift, or menu-key behavior.

## Broader smoke test

- [ ] `1/3+1/6` returns `1/2` or an equivalent decimal result.
- [ ] Pressing any key after a result returns to the input prompt cleanly.

## Core functionality

- [ ] Algebra: `factor(x^2-5x+6)` returns `(x-2)(x-3)`.
- [ ] Solve: `solve(x^2-5x+6=0)` returns roots `2` and `3`.
- [ ] Advanced solve fallback: `nlsolve(x^2+y^2-25,y-x,x,y)` returns intersection points without crashing.
- [ ] Calculus: `deriv(x^3,x)` returns `3*x^2`.
- [ ] Integral: `int(x^2,x)` returns `x^3/3 + C`.
- [ ] Limit: `lim(sin(x)/x,x,0)` returns `1`.
- [ ] Advanced limit case: run at least one nontrivial indeterminate limit such as `lim((1-cos(x))/x^2,x,0)` and confirm it finishes without a memory error.
- [ ] Series: `taylor(sin(x),x,0,5)` returns `x - x^3/6 + x^5/120`.
- [ ] Matrix: `det(mat(1,2;3,4))` returns `-2`.
- [ ] Statistics: `mean(1,2,3,4,5)` returns `3`.
- [ ] Probability: `normalcdf(-1,1,0,1)` returns about `0.6827`.
- [ ] Finance: `fv(0.05,10,100,0)` returns a finite numeric result and does not hang.
- [ ] Lists: `seq(x,x,1,5,1)` returns a five-item list.
- [ ] Complex mode: enable Complex mode, then evaluate `(3+2i)*(1-i)` and confirm a complex result is shown.

## Graphing and MathPrint

- [ ] Press `Y=`, enter `x^2`, then press `GRAPH`; a parabola is drawn.
- [ ] Press `WINDOW`, change the view, and redraw successfully.
- [ ] Press `TRACE` on a graph and confirm the calculator remains responsive.
- [ ] Enter `(x+1)/(x-1)` and confirm MathPrint draws a stacked fraction rather than unreadable linear text.
- [ ] Enter `sqrt(x^2+1)` and confirm the radical and exponent layout are readable.
- [ ] Enter `int(x^2,x,0,3)` or `sum(n,n,1,10)` and confirm bounded MathPrint layout is readable.

## Module loading and archive-to-RAM behavior

- [ ] Archive the transferred support modules in Mem Mgmt before testing routed expressions.
- [ ] From `CALCENG`, run one expression in each major domain: algebra, calculus, limits/series, matrix, stats/probability, finance, graphing, and lists/complex.
- [ ] After each routed calculation, return to Mem Mgmt and confirm the invoked module is archived again.
- [ ] Switch domains rapidly in one session: algebra -> calculus -> stats -> matrix -> graphing -> algebra.
- [ ] Confirm `Ans`, settings, and recent history survive cross-module jumps.
- [ ] Confirm graphing returns cleanly to the launcher instead of leaving the calculator in a broken state.

## Edge cases and failure paths

- [ ] Press `CLEAR` during a long-running command such as `sum(1/n^2,n,1,9999)` and confirm the calculation cancels cleanly.
- [ ] `lim(1/x,x,0)` reports a non-existent limit rather than crashing.
- [ ] `continuity(1/x,x,0)` reports a discontinuity classification rather than crashing.
- [ ] `recseq(x/2+1,x,10,5)` returns a valid sequence.
- [ ] If the calculator is RAM-cleared, confirm CalcEngine must be re-sent only if it was stored in RAM, and confirm arTIfiCE must be reactivated on OS `5.5` through `5.8`.
- [ ] If a support file is removed on purpose, confirm routed features fail in a visible and recoverable way rather than corrupting state.

## Known watch items for this hardware pass

- [ ] Pre-hardware validation is green on the official staged package; use this pass to capture calculator-only divergence from the emulator result, not to skip physical certification.
- [x] `tools/emulator/run-prehardware-validation-gate.ps1 -Mode strict -EnableCEmuChecks` was rerun successfully on 2026-03-11 against the same staged package; use this pass to look for calculator-only divergence from the emulator result.
- [ ] Watch advanced solve flows closely. `CEASLA.8xp` remains one of the larger routed workers in the current manifest.
- [ ] Watch advanced limit flows closely. `CECLIA.8xp` remains one of the larger routed workers in the current manifest.
- [ ] Watch for any launch failures that only appear after multiple domain swaps in one session.
- [ ] Watch for stale results, missing `Ans`, or lost settings after returning from a routed module.
