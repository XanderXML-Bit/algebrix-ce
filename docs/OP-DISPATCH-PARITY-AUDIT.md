# Operator dispatch parity audit

The audit used `d4ab104` as its `main` baseline. All results come from host builds; no emulator or physical calculator was used.

## Result

On `main`, 52 operator checks fail. Two of the operators are eigenval() and eigenvec(). They fail on both checked paths, and nothing else in the test is marked to hide them. The other 22 operators that fail are listed under [Other operators with the same gap](#other-operators-with-the-same-gap). The test runs them as strict expected failures, so CI stays usable while each one waits for its own fix.

On the eigen fix branch `fix/cemat-eigen-dispatch` (PR #117, `113c561`), all checks pass: 108 passed and 48 expected failures.

## The regression test

`tests/cemu/test_op_dispatch_parity.py` follows each operator from the input line to the module that answers it. Each module is built with its own flags.

1. `tests/harness/op_dispatch_harness.c` is built with the `PROGRAM_CFLAGS` of `split/Makefile.launcher`. It includes `src/core/launcher/main.c`, parses one sample input per operator, and reports the following facts:
   - the parsed node and operator
   - the `classify.c` domain
   - `ce_domain_program()`
   - the program `route_program()` picks
   - the node and operator after the astwire round trip
2. Router programs (CEALG, CECINT, CECLIM, CECPOL, CECIAD) run through `tests/module_parity/router_driver.c` with their own flags until the chain reaches a worker.
3. The worker is built from its makefile's `PROGRAM_CFLAGS` and `PROGRAM_SOURCES`. That includes `HW_BUILD_SPLIT` and the `CE_PROC_*` flags. It runs the sample input through `tests/module_parity/module_driver.c` or `strategy_driver.c`. A generic refusal counts as "no handler". Examples are "Polar/advanced op (use at top level)" and "Feature is routed to another module". An answer or an input error counts as handled.

The test checks two paths for every operator:

- `test_launcher_dispatch_reaches_a_handler` follows the program CALCENG really runs.
- `test_classify_domain_module_has_handler` covers every shipped module whose `CE_PROC_*` flags serve the `classify.c` domain. The flags come from the preprocessor through `split/processor.h`. At least one of those modules must handle the operator.

The test covers all 61 `polar_op_t` values plus 16 other node samples. Three guard tests keep the table complete:

- A new `polar_op_t` value without a sample fails.
- A sample that parses to a different operator fails, and so does one the astwire round trip changes.
- An operator listed without a sample must have no parse rule.

Run the test in one of these ways:

- `make op-dispatch-parity`
- `make test`, which runs the desktop suite and then this test
- `python -m pytest tests/cemu/test_op_dispatch_parity.py -q`

CI runs it in the existing `cemu-python` job (`python -m pytest tests/cemu`). It needs a POSIX host C compiler and skips on Windows. A full run takes about 30 s.

Failure output names the operator, the domain, the dispatch chain, the module, and its makefile:

```
POLOP_EIGENVAL (eigenval(mat(2))): the program CALCENG dispatches to has no handler.
  classify.c domain : CE_DOMAIN_MATRIX
  dispatch chain    : CALCENG -> CEBASE
  CEBASE built with split/Makefile.base flags answers: 'Polar/advanced op (use at top level)'

POLOP_EIGENVAL (eigenval(mat(2))): classify.c routes it to CE_DOMAIN_MATRIX, but no module built with that domain's flags (CE_PROC_MATRIX plus HW_BUILD_SPLIT) handles it.
  ce_domain_program(): CEMAT
  modules built for CE_DOMAIN_MATRIX:
    CEMAT (split/Makefile.matrix): 'Feature is routed to another module'
```

## Eigen classification audit

The table traces eigenval() and eigenvec() from input to module on `main`.

| Step | Source | Finding |
|---|---|---|
| Token | `src/token.c` line 306 | `eigenval` and `eigenvec` map to `TOK_EIGENVAL` and `TOK_EIGENVEC`. |
| Parse | `src/parser.c` lines 2631 to 2650 | Builds `NODE_POLAR` with op `POLOP_EIGENVAL` (58) or `POLOP_EIGENVEC` (59), the matrix in `expr1`, and `var = 0`. Correct. |
| AST wire | `src/split/astwire.c` lines 191 and 430 | The op is one byte and survives the round trip. The harness confirms 58 and 59 for `eigenval(mat(2))`, `eigenvec(mat(1,2;3,4))`, and `eigenval(5)`. Correct. |
| Domain | `src/split/classify.c` | Both ops map to `CE_DOMAIN_MATRIX`. `ce_domain_program()` gives `CEMAT`. Correct. |
| Launcher route | `src/core/launcher/main.c` `route_polar_program()` | No case for either op, so the default `CE_PRGM_BASE` applies, and CALCENG sends them to CEBASE, not CEMAT. This disagrees with `classify.c`. |
| Module | `src/split/processor.c` lines 2514 to 2620 | The handler is inside `#ifndef HW_BUILD_SPLIT` (lines 2236 to 2666). Every shipped module defines `HW_BUILD_SPLIT`. CEMAT answers "Feature is routed to another module". CEBASE builds with `CE_EVAL_ENABLE_ADV_POLAR=0`, so `eval.c` answers "Polar/advanced op (use at top level)". |

Two eigen-specific defects exist, and this card fixes neither:

1. The launcher route is missing. The card brief says `classify.c` sends eigen to CEMAT. The launcher does not use that domain for `NODE_POLAR`, so the real ROM path is CEBASE. That path explains the reported text. PR #117 already adds `case POLOP_EIGENVAL: case POLOP_EIGENVEC: return CE_PRGM_MAT;` (commit `3a8d2b5`). A second copy here would only conflict with it. A route fix alone would also change the refusal text without giving an answer.
2. CEMAT has no handler. This is the parallel worker's scope (PR #117).

The tests cover both defects. The launcher test fails on defect 1. The classify test fails on defect 2.

Two more findings are not specific to eigen:

- `ce_ast_deserialize()` copies the op byte into `polar_op_t` without a range check. An out-of-range op reaches the module's default branch. The input comes from the launcher, so the risk is low.
- `classify.c` is a fallback in the launcher. `route_program()` decides by node type and polar op first. It uses the domain only for nodes it does not name.

## Other operators with the same gap

These findings are not fixed in this card. Each needs its own scope. The "Launcher chain" column is what CALCENG runs. "Answer" is that module's reply, built with its own flags.

| Operator | Sample | Launcher chain | Answer | Cause |
|---|---|---|---|---|
| newton, bisect, secant, pdiff, gradient, dblint, critical | `newton(x^2-2,x,1)` | CEBASE | Polar/advanced op | The handlers live in `eval.c` under `CE_EVAL_ENABLE_ADV_POLAR`. CEBASE builds with `=0`. `classify.c` says `CALC_TANGENT` or `CALC_POLAR`, but no module there handles them. |
| euler, slopefield | `euler(x+y,x,y,0,1,1,0.5)` | CECPOL, then CECPNM | Polar/advanced op | CECPNM's `CE_PROC_CALC_EULER` handler calls `eval_expr()`. CECPNM builds with `CE_EVAL_ENABLE_ADV_POLAR=0`, which removes the eval code it needs. |
| dsolve, cross, grad, curl, laplace, ilaplace, dsolve2, graphimpl | `dsolve(y,y,x)` | CEBASE | Symbolic op or Polar/advanced op | The handler exists only in processor.c's `#ifndef HW_BUILD_SPLIT` block, the same class as eigen. |
| polydiv | `polydiv(x^2-1,x-1,x)` | CEAPDV | Feature is routed to another module | CEAPDV handles `a/b` division only. It has no `POLOP_POLYDIV` branch. |
| graph, grapheq | `graph(x^2,x)` | CEGRPH | Graph module loaded. Enter graph() expressions in CALCENG. | CEGRPH answers with placeholder text. A host test cannot show whether a graph is drawn elsewhere, so check this on the ROM. |
| areabetween, improper | `improper(1/x^2,x,1,inf)` | CEBASE | Polar/advanced op | The handler is in CECIDF. Open PR #123 routes both to CECIDF, and its head passes the launcher check. `classify.c` has no case for them, so they also get the default `CE_DOMAIN_CALC`. |

The classify path has one more set of findings. For polararea, polararclen, dot, and div, the launcher sends the operator to CEBASE, and CEBASE's eval answers. That path works. But `classify.c` names `CALC_POLAR` or `CALC_TANGENT`, and no module built for those domains handles these operators.

## Files

The change adds or edits these files:

- `tests/cemu/test_op_dispatch_parity.py`, the test
- `tests/harness/op_dispatch_harness.c`, which reports the launcher's classification and route
- `tests/module_parity/router_driver.c`, which runs router modules with their own flags
- `Makefile`, which gains the `op-dispatch-parity` and `test` targets
