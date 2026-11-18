# Limit / solve worker stack bounds

This is emulator evidence, not physical-calculator verification.

## Root causes and scope

On the owned OS 5.8 ROM in debug CEmu, a 55-term quotient limit and a
55-term linear equation reached the stack-limit NMI. Launcher parsing and
AST-wire transfer had already completed. The failing chains were
`eval_expr` / `eval_expr_inner` in CECLMB and `ast_to_poly` in CEASLV.
Polynomial conversion had roughly 210-byte recursive frames.

Only the six `calc.lim.*` / `algsolve*` makefiles now opt into eval depth 12
and polynomial depth 8. Polynomial ADD/SUB/MUL/DIV left spines are iterative;
right subtrees and powers remain bounded. Non-inline power scratch avoids
paying its full frame at every recursive level. Limit strategy scratch is
also non-inline so split-rule recursion does not retain every strategy's
buffers. The CECLIA `limit_inner` frame decreased from 376 to 35 bytes in
the measured build. A frame report alone is NOT a high-water proof.

Other modules retain eval depth 50 and its historical `CE_ERR_OVERFLOW`
status. They are not claimed safe for every deeply nested input by this
change. The default polynomial depth is 64; the bounded conversion still
returns false on unsupported trees. Integration/ODE stack work is owned by
the separate integration change; no integration source is edited here.

## Error-code audit

`CE_ERR_DEPTH` is appended after `CE_ERR_CANCELLED` in `ce_error_t`; no
previous numeric value changes. Only builds with `CE_EVAL_MAX_DEPTH < 50`
emit it at the eval guard. A default build still emits OVERFLOW. The source
audit covered `src/` C/headers, including series, integrate, ODE, graph,
stats, finance, main, launcher and split IPC/runtime.

- No error-code-indexed text array, upper-bound check through CANCELLED,
  or error-code-indexed storage was found. IPC carries success/error text,
  not this enum; `src/split/ipc.c` copies text with fixed bounds.
- The OVERFLOW-sensitive antiderivative-bound classification in
  `integrate.c` is unchanged: those makefiles do not opt into depth 12.
- Lexer overflow and the desktop main's token-overflow message are unchanged.
- Continuity distinguishes actual depth refusal before treating f(a) as
  undefined: it reports `could not determine`, not removable discontinuity.

## Numeric-caller audit

The scoped solve workers expose the single-variable solver, not the
separate multi-variable/system solver. The latter's unmodified helpers
are not made reachable by this change.

- `newton_eval`: checks the original sample and both finite-difference
  samples before using them. Each must have CE_OK and be finite. Candidate
  roots and final residuals must be finite before acceptance/deduplication.
  The final evaluation detects DEPTH and returns a clean refusal.
- A polynomial conversion depth refusal is terminal before Newton, in both
  solve workers. Reviews found finite false roots in the pre-existing
  heuristic numeric fallback on scaled equations and near poles. This
  stack-safety change does not expand that heuristic to deeper polynomial
  or rational trees. A general numeric-solver redesign is out of scope.
- `solve_log_exp`: checks eval status for its base before using it. The
  pattern solvers use the bounded polynomial conversion for inner trees.
- `try_direct_sub`: requires CE_OK and finite value.
- `classify_quotient`: checks both errors before classifying either side.
- `try_factor_cancel` and `try_rationalize`: check CE_OK before accepting
  evaluated results; rationalization rechecks after its offset retry.
- `tends_to_zero` and `tends_to_inf`: reject evaluation errors before
  using samples. The latter retains its existing nonfinite-as-large
  heuristic, not a new depth-as-infinity rule.
- `try_infinity`: checks all three errors before comparing samples.
- `approach_side` / `try_numerical`: invalid/error samples do not increment
  the valid sample count; convergence needs valid samples.
- `try_lhopital`: differentiates, then uses the same bounded `limit_inner`
  dispatcher; it does not accept an unchecked numeric sample itself.
- Point evaluation helper returns CE_OK explicitly before its value is used.

## Intentional support trade-offs

Bounded refusal is safer than a reset. Some formerly working deep
non-polynomial sums now refuse in these workers; continuity of a 13-term
sum can be indeterminate. Deeply nested linear and Horner equations now
explicitly refuse rather than falling through to a heuristic numeric
answer. This is an intentional support reduction to avoid false roots,
not a claim that the mathematical problem has no solution. Long *linear*
sum spines remain exact, including the original 55-term reproducer.

The related-rates worker's flags are defensive for future numeric paths;
its current symbolic derivative recursion is not claimed fixed or measured
by this corpus. Other modules can still have pre-existing recursion risks.

`proc_failf` formats an error before clearing the result because solve can
pass that result as a `%s` source; clearing first erased advanced routing
and clean-refusal diagnostics. No IPC version change is needed.

## Reproduction and assertions

Build all modules serially with CEdev, then run:

```sh
ALGEBRIX_ROM=/path/to/owned.rom CEMU_HEADLESS=/path/to/debug/cemu-headless \
  python -m pytest tests/cemu/test_realrom_worker_stack.py -q
python -m pytest tests/cemu/test_worker_stack_bounds.py -q
```

The ROM fixture checks source/header/makefile freshness before transfer.
The high-water test paints the actual free OS stack and requires at least
600 bytes untouched after every expression, no reset, and a newly stamped
response. Parser refusals are counted separately using the live Syntax
error status; they are never counted as worker execution. Every worker
case checks a known answer or explicit refusal. Successful solve outputs
are independently checked by evaluating the fixed test equation at each
reported finite root. Nonzero deep limits prevent an ignored depth error
returning zero from passing. A separate same-emulator sequence checks
error/success recovery and basic-to-advanced routing.

Do not commit ROM/state files, refresh transfer packages concurrently, or
infer hardware verification from these tests. Package refresh remains
with the existing release/package task.
