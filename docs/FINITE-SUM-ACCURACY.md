# Check finite-sum accuracy

CEBASE, CECSEQ, and CELSCX use an exact integer accumulator followed by compensated floating-point summation for finite sums.

## Covered routes

- `sum(f,n,a,b)` runs in CECSEQ.
- `0+sum(f,n,a,b)` runs in CEBASE.
- `sum(f,n,a,b)->A` runs in CELSCX.

`CE_EVAL_SUM_COMPENSATED` defaults to one on desktop and zero on eZ80. Only these three module makefiles enable it.

`CE_EVAL_SUM_DIRECT_DISPATCH` defaults to zero everywhere. Only CELSCX enables it, calling the unchanged helper directly from `eval_expr` after the common checks.

This call site avoids retaining an `eval_node` frame beneath each nested sum. CEBASE and CECSEQ retain their original call site. Infinite-series dispatch remains unchanged.

Sums nested in other workers also retain naive accumulation. This includes integration, limit, and derivative workers. Do not infer accuracy coverage from desktop builds.

## Acceptance bound

Let `t_i` be each float32 term produced by the evaluator. Define `T = sum(t_i)`, `A = sum(abs(t_i))`, and `u = 2^-24`.

The empirical accumulation guard is `abs(S-T) <= 4u*abs(T) + u*A`. This is not a universal proof under CE arithmetic. It does not bound term-evaluation error.

For integer terms with `abs(t_i) < 2^24`, the accumulator remains exact while every partial satisfies `abs(P) < 2^31`. Final conversion rounds ties to even. A failed integer condition switches permanently to Kahan summation and carries the integer conversion residual.

The span limit remains `b-a <= 10000`, so a request can contain 10001 inclusive terms. Cancellation keeps its existing 256-index cadence. Existing error messages, variable restoration, and Ans preservation remain unchanged.

Infinite series, formatting, IPC, routing, and the launcher remain outside this change.

## Run the checks

Run the desktop CE-semantics checks with a host C compiler and pytest:

```sh
python -m pytest tests/cemu/test_sum_ce_semantics.py \
  tests/cemu/test_sum_direct_dispatch.py \
  tests/cemu/test_module_flag_parity.py -q
```

The test extracts the complete production helper without changing its body. It compiles that helper with term-array evaluator doubles and a C truncating-add model. The model matches 798 recorded owned-ROM samples. Two subnormal samples remain outside that calibration set.

The negative controls remove naive-accumulation protection, integer accumulation, compensation, the partial-overflow guard, the transition residual, or a makefile flag. Every control must fail an assertion.

After a fresh CE build, run the owned-ROM tests:

```sh
ALGEBRIX_ROM=/path/to/owned.rom CEMU_HEADLESS=/path/to/debug/cemu-headless \
  python -m pytest tests/cemu/test_realrom_finite_sum.py \
  tests/cemu/test_realrom_worker_stack.py \
  tests/cemu/test_realrom_cebase_stack.py \
  tests/cemu/test_realrom_celscx_stack.py -q
```

These runs provide emulator evidence, not physical-device validation. ROMs, emulator states, and memory dumps must not be committed.
