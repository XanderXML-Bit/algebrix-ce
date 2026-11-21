# CalcEngine v2.0.0 Hardware Bug Report

> **Public source note:** the staged `deploy/hardware-ready/` transfer package is not part of this repository. Build the modules from source with CEdev as described in the [README](../README.md).

> **Historical note (2026-04-16):** this file preserves the first failed v2.0.0 real-hardware report captured on 2026-04-14. Later changes may have addressed some of these issues. See the [current status](../README.md#current-status); no current build is certified on physical hardware.

**Hardware test date:** 2026-04-14
**Tester:** Xander
**Platform:** TI-84 Plus CE, OS 5.8, arTIfiCE jailbreak
**Build:** v2.0.0 — CALCENG.8xp 28,373 B (zx0 compressed), CE_IPC_VERSION 5

---

## Executive Summary

v2.0.0 is **not functional on hardware**. Of the 7 issues reported below, 3 are regressions
that pre-date v2.0.0 (meaning Phase 1's claimed fixes did not actually work on hardware), and
4 are new issues introduced in v2.0.0. None of these were caught by the 185-test automated
suite. See [Section 8 — Emulator Validation Gap](#8-emulator-validation-gap) for a full
post-mortem on why.

---

## Bug #1 — `%.6g` Appears Literally in Output

> **Status (2026-11-08): fixed in source by PR #95, emulator-verified, not hardware-verified.**
> The root cause is confirmed: CEdev v11.2's libc printf (nanoprintf, not newlib) does not
> implement `%g`/`%e`/`%f`. `src/ce_fmt.c` is a self-contained `snprintf` family that
> `split/program.mk` force-includes into every CE module. It renders `%e`/`%g` itself, so
> the `%.*f` approach below was not needed. On a real OS 5.8 ROM in CEmu, `factor(x^2-5x+6)`
> shows `((X-3)*(X-2))`. `tests/cemu/test_ce_numeric_formatting.py` guards all of `src/`.
> The analysis below is kept as history.

**Severity:** CRITICAL — CalcEngine is entirely non-functional; no result ever displays.

**Observed behavior:** Evaluating `2` (or any expression) shows `%.6g` on screen instead of
the numeric result.

**Expected behavior:** The result of the evaluation (e.g., `2`, `3.14159`) is displayed.

**Regression status:** This bug predates v2.0.0. Phase 1 (PR #43) was supposed to fix it by
"routing all output through snprintf buffers." It did not fix it.

**Root cause analysis:**

The primary suspect is that the ez80 `newlib` libc bundled with CEdev **does not support the
`%g` format specifier** in `snprintf`/`vsnprintf`. The `%g` specifier is non-trivial
(it switches between `%f` and `%e` based on magnitude) and is commonly omitted from
embedded/size-optimized libcs. When an unsupported conversion specifier is encountered,
some libcs output the format string literally rather than aborting or returning an error.

Evidence:
- `processor.c` uses `proc_okf(st, ...)` which calls `vsnprintf(st->result, ..., fmt, ap)`
  with format strings like `"= %.6g"`, `"Result = %.6g"`, `"det = %.6g"`, etc.
- `eval.c` uses `steps_addf("%.6g", ...)` for step recording.
- The desktop test harness uses the host `libc` where `%g` is fully supported, so all
  desktop tests pass without catching this.
- The CEmu (real-ROM) tests that test for `%.6g` regression
  (`test_phase1_ui.py::TestPhase1UI::test_arithmetic_result_no_format_spec`) are **skipped**
  on every machine that doesn't have the full hardware stack provisioned. They have never
  actually run.

**Secondary cause — widespread use:** `%.6g` and `%.4g` appear in at least these files:
- `src/split/processor.c` — the primary result-formatting path (dozens of occurrences)
- `src/eval.c` — step recording
- `src/algebra.c` — step recording and result strings
- `src/stats.c`, `src/matrix.c`, `src/finance.c`, etc.

**Fix required:** Replace every `%g` / `%.Ng` with a custom `ce_ftoa()` function that:
1. Checks if the value is an exact integer (no fractional part, within int32 range) and
   formats as `%d`.
2. Otherwise formats with `%.*f` up to the configured precision, then strips trailing
   zeros and optionally the decimal point.
3. Handles special cases: `INFINITY` → `"inf"`, `NAN` → `"nan"`, very large/small values
   via explicit `%e`-style formatting.

This is a pervasive change across all worker modules and the processor.

---

## Bug #2 — `2nd+CLEAR` Does Not Open a Menu

**Severity:** HIGH — key binding documented as functional but is inoperative.

**Observed behavior:** Pressing `2nd` then `CLEAR` does nothing.

**Expected behavior:** Per the v2.0.0 design, `2nd+CLEAR` was intended to clear history
or trigger a secondary action.

**Root cause:** Unknown. Possible causes:
1. The keyscan loop does not check for the `2nd` modifier before `CLEAR`.
2. The `2nd` latch is being consumed or not set before `CLEAR` is scanned.
3. The binding was specified in design but not yet implemented in `main.c`.

**Investigation needed:** Search `ce_input_scan()` and `home_draw()` in `main.c` for the
`CLEAR`-key handling path and verify whether the `2nd` modifier state is read correctly
before the CLEAR action is dispatched.

---

## Bug #3 — UI Is Terminal/readline Style, Not TI-OS Home Screen Style

**Severity:** HIGH — fundamental UX model is wrong.

**Observed behavior:**
- The home screen does not scroll like a TI-84 CE home screen.
- Pressing UP/DOWN cycles through prior expressions *inside the input line* (readline-style
  history recall), rather than scrolling through visible history entries above the input.
- Results and expressions from prior calculations are not displayed as stacked rows above
  the current input line in the way the real TI-84 CE home screen works.

**Expected behavior:** The TI-84 CE home screen model:
- 8 rows total. Row 7 is the input line with `>` prefix.
- Rows 0-6 display prior expression/result pairs, most-recent at the bottom, older entries
  at the top.
- When a new result arrives, all prior rows shift up by the height of the new entry.
  When history fills the 7 visible rows, the oldest entry scrolls off screen (is no longer
  visible, though it remains in the ring buffer for ENTRY recall).
- UP/DOWN do NOT change the input line content — they scroll the history view if the screen
  is not full, or they are used for other navigation (e.g., cursor within input).
- ENTRY recall (2nd+ENTER) is the mechanism for copying a prior expression into the input.

**Current implementation:** `home_draw()` in `main.c` does implement a basic history
display, but the key navigation (UP/DOWN in `ce_input_scan()`) likely moves the `s_hist_nav`
cursor which cycles expressions INTO `s_input`, creating the readline behavior. This is
the wrong model.

**Fix required:** The key scan loop needs to distinguish between:
- History display scrolling (cosmetic, doesn't change input) — not needed if full history
  always fits on screen.
- ENTRY recall (2nd+ENTER) — copies expression to input.
- The UP/DOWN keys in the input area should either do nothing or move the input cursor
  (if the input wraps), NOT cycle expressions into the buffer.

---

## Bug #4 — Extra Enter Required After Result Display

**Severity:** HIGH — every calculation requires two Enter presses.

**Observed behavior:** After entering an expression and pressing Enter:
1. The result is shown (possibly on a graphx full-screen render, Phase 4).
2. The calculator sits waiting for another keypress before returning to the input line.

**Expected behavior:** After Enter, the result is added to history and the cursor is
immediately at the `>` input line, ready for the next expression. No extra confirmation.

**Root cause:** Phase 4 (`pp_show_result()`) was added to `main.c` and **explicitly waits
for a keypress** before calling `gfx_End()` and returning. This is the pause.

Specific call sites in `main.c`:
- Re-entry path (after `has_pending_response` block): `if (g_shared.last_result[0]) pp_show_result();`
- Main loop (after `dispatch_to_program`): same pattern.

`pp_show_result()` itself contains a `while (!os_GetCSC()) kb_Scan();` (or equivalent)
wait loop before exiting the graphx mode.

**Fix required:** Either:
- Remove the graphx pretty-print screen entirely and display the result inline in the
  home screen history (the TI-OS home screen model), OR
- If graphx pretty-print is kept, make it non-blocking: display the result, then
  immediately return to the home screen after a short delay without waiting for keypress.

The blocking wait was an intentional design choice in Phase 4 ("wait for key") but it
is wrong — it breaks the natural calculator flow.

---

## Bug #5 — Crash After 3-4 Calculations (Screen Overflow)

**Severity:** CRITICAL — renders the calculator non-functional after minimal use.

**Observed behavior:** After 3-4 calculations, when the next result would push the
history display beyond the visible screen area, the program crashes.

**Expected behavior:** Older history entries scroll off the top of the screen. The home
screen is always stable, never crashing due to display overflow.

**Root cause analysis:** `home_draw()` in `main.c` loops over history entries and calls
`os_SetCursorPos(row, 0)` and `os_PutStrFull()`. Likely failure modes:

1. **Row overflow:** `os_SetCursorPos(row, col)` with `row >= HOME_ROWS` (8) is undefined
   behavior on TI-OS. The desktop mock probably clamps silently; the real OS may crash.

2. **History window calculation:** The code computes `show_count` entries to display and
   iterates backwards, but the row counter may advance past `HOME_ROWS - 2` due to
   multi-line results (results containing `\n`) not being counted as multiple rows.

3. **Result strings with newlines:** `processor.c` builds results with embedded `\n`
   (e.g., `"F(x) = ...\n= 3.14"`). Each `\n` in an `os_PutStrFull()` call advances to
   the next line. If this is not accounted for in the row counter, the cursor position
   calculation gets out of sync and eventually goes out of bounds.

**Investigation needed:** The `home_draw()` function at `main.c:202` — trace the row
arithmetic with a multi-line result and verify `os_SetCursorPos` is never called with
`row >= HOME_ROWS`.

---

## Bug #6 — Pretty-Print Result Rendering (Status Unknown)

**Severity:** MEDIUM — cannot be evaluated until Bug #1 (%.6g) is fixed.

**Observed behavior:** Unknown. Since `%.6g` is outputting format strings, `pp_show_result()`
is trying to parse `"= %.6g"` as a math expression, which would fail to parse, triggering
the linear-text fallback — which would display `"= %.6g"` centered on the graphx screen.

**Expected behavior:** After fix of Bug #1, results like `6/2` should render as a stacked
fraction; `2^8` as raised exponent; `sqrt(16)` as a radical with vinculum.

**Dependency:** This bug cannot be independently verified until Bug #1 is fixed and results
contain actual numeric output.

---

## Bug #7 — MODE Settings Menu Works

**Not a bug.** Xander confirmed MODE correctly opens the settings menu. Phase 3 is
functionally correct on hardware.

---

## 8. Emulator Validation Gap — Post-Mortem

This section documents honestly why 185 automated tests passed while none of these hardware
failures were detected.

### 8.1 What the 185 tests actually tested

| Test category | Count | What they test | Hardware coverage |
|---|---|---|---|
| Desktop C harness (mock env) | ~9,800 assertions | Math correctness, AST, parser, eval, algebra, etc. | **Zero** — runs on host CPU, host libc, host `snprintf`. The `%g` specifier works on any modern x86/ARM libc. |
| `test_stack_check.py` | 41 | `.su` file parser, stack threshold logic | Not hardware-specific; always skipped on CE. |
| CEmu harness (real ROM) — **SKIPPED** | 12 (Phase 2-4) | Phases 2-4 end-to-end behavior | **Never ran** — all skip when cemu-headless binary absent. |
| CEmu harness (real ROM) — SKIPPED | Remaining 71 | Boot, launch, expression entry, CRC checks | Skipped on machines without full hardware stack. |
| CRC catalog tests | 5 | JSON consistency between manifest and catalog | No CE execution whatsoever — pure JSON validation. |
| Pipeline support tests | ~50 | CEmu runner infrastructure, file staging | Mock backends; no real CE code execution. |

**The fundamental issue:** Every test that passed ran either on the host machine (desktop
C harness) or validated JSON files against each other (catalog tests) or was skipped
entirely (real-ROM emulator tests). **Zero tests ran CalcEngine code through the real CE
C library (CEdev newlib) or the real TI-OS.**

### 8.2 Why `%.6g` was not caught

The desktop C harness compiles with GCC/Clang on Windows/Linux. Host `libc` fully supports
`%g`. The `snprintf("%.6g", 3.14f)` call works perfectly on the host, producing `"3.14"`.

The CE toolchain uses a size-optimized `newlib` port. This libc may not implement `%g` (it
is an optional conversion specifier in several embedded newlib configurations). The
emulator tests that *would* have caught this — `test_phase1_ui.py::test_arithmetic_result_no_format_spec`
— require cemu-headless + ROM + arTIfiCE + CALCENG to be present and were **always
skipped** in CI and on the development machine.

### 8.3 Why the scroll/UX model was not caught

The CEmu real-ROM tests that test for `"4" in screen_text` (Phase 2) and `"3+5" in
screen_text` (Phase 1) were all skipped. Even if they had run, they would test whether
the number appears *somewhere* on screen, not whether the *layout* matches the TI-OS home
screen model.

The TI-OS home screen scroll behavior is a visual/layout concern that is impossible to
validate with:
- Desktop tests (no display at all)
- CRC-hash tests (CRC of a wrong-but-stable layout passes indefinitely once baselined)
- Text-content tests that only check `"4" in text`

### 8.4 Why the crash was not caught

The desktop mock for `os_SetCursorPos(row, col)` silently ignores out-of-bounds row values
(no crash). The real TI-OS home screen likely has an assertion or memory corruption when
`row >= 8`. Three or four calculations is all it takes to overflow the 8-row display, but
no desktop test ever renders more than a few entries.

### 8.5 Why Phase 4's blocking wait was not caught

The pretty-print `pp_show_result()` function is inside `#ifndef DESKTOP_BUILD`. It is
invisible to the desktop harness. The Phase 5 emulator tests for this behavior were all
skipped. The code was written, compiled, and deployed without ever being executed in
an automated test.

### 8.6 The deeper problem: test-shaped testing

The test suite was built to pass, not to fail on real hardware bugs. Specifically:
- Tests were written *after* code, checking that the code's output matches expectations
  set by the same code running on the desktop.
- CRC baselines were generated from the emulator, not compared against real hardware.
- The "emulator should catch it" assumption was never validated. The emulator *can* catch
  these issues — but only if the tests are run, and the tests require an investment
  (hardware stack: CEmu binary, ROM, arTIfiCE) that was never fully provisioned in CI.

### 8.7 Required changes to the test approach

1. **Provision the full hardware stack in CI.** `cemu-headless`, `Rom.rom`, `arTIfiCE.8xp`
   must be present so the Phase 1/2/4/5 emulator tests actually execute rather than skip.

2. **Add a `%.6g` detection test that runs.** A test that builds CALCENG with the CE
   toolchain and executes it under cemu-headless, then checks that the string `"%.6g"` does
   NOT appear on screen, would have caught this immediately.

3. **Add a screen layout test.** After submitting 5 expressions, capture screen text and
   verify that 4 expression/result pairs are visible in rows 0-6, not just that a number
   appears somewhere.

4. **Add a stability test.** Submit 8+ expressions and verify the program does not crash.

5. **Replace `%g` in the CE toolchain path** with a known-working format. Until the CE
   libc situation is understood, `%f` or a custom `ce_ftoa()` should be used everywhere.

6. **Remove or quarantine `#ifdef DESKTOP_BUILD`-gated code from automated testing claims.**
   If code only runs on hardware, it must not be listed as "tested" in the test summary.

### 8.8 Module flags can drop features the desktop suite still tests

The desktop runner builds the processor with every feature on. Each CE module is built from
`split/Makefile.<module>` with its own `PROGRAM_CFLAGS` and `PROGRAM_SOURCES`. CECALC, which
runs `d/dx`, `nderiv` and `tangent`, was built with `-DCE_DERIV_USE_SIMPLIFY=0` and without
`src/simplify.c`. On a real OS 5.8 ROM in CEmu, `d/dx(x^3+2x)` therefore showed
`((3*(X^(3-1)))+(2*1))` while the desktop CLI printed `((3*(x^2))+2)`.

- CECALC now links `src/simplify.c`. Built from source, it grows from 36,796 B to about 48.3 KB,
  under the 51,200 B ceiling. The staged `deploy/hardware-ready/CECALC.8xp` (41,389 B) predates
  the #95 formatter change, which made every module smaller; the package refresh is a separate
  step.
- Recursion is bounded for the 4 KB OS stack. A stack high-water probe (paint the free stack,
  evaluate, find the deepest changed byte) on the real ROM showed that `d/dx` of a 32-term sum
  overflowed it on main (stack-limit NMI, RAM Cleared).
  - Step-only buffers and prints in `deriv.c` are now compiled out when step recording is off.
  - `ast_print` keeps its scratch buffer out of the recursive frame.
  - CECALC sets `-DDIFF_MAX_DEPTH=36` and `-DSIMPLIFY_MAX_DEPTH=24`.
  - The worst 127-character inputs now leave at least 680 B to the limit.
  - Input nested deeper than 36 levels gets "Cannot differentiate this expression".
  - Subtrees deeper than 24 levels are left unsimplified on the CE, while the desktop simplifies
    to 50 levels, so very deep results can differ.
- When the node pool runs out, `deriv.c` returns an error instead of printing a tree with a
  missing operand, such as `(0+(0+))` or `cos()`. Once an allocation fails during a call,
  `simplify.c` stops rewriting; `memory.c` counts the failures.
- `tests/cemu/test_module_flag_parity.py` compiles a module's processor on the desktop with
  that module's flags and sources (driver: `tests/module_parity/module_driver.c`) and checks
  the output. A static check requires every built module that handles `d/dx` to link simplify.
- `nderiv(f,x,n)` is Algebrix's n-th derivative, as the APPS catalog and help text describe
  ("nderiv( nth derivative"). It is not TI-OS `nDeriv(f,x,value)`, so `nderiv(x^2,x,1)` is
  `2*X`, not `2`.
- Integral results such as `int(x^2,x)` were still unsimplified on target
  (`((X^(2+1))/(2+1))`), because the calc.int workers did not link simplify either. See 8.9.

Emulator evidence only. Not verified on hardware.

### 8.9 Integral results on target: fold where simplify does not fit

Every module that links `src/integrate.c` (CECISM, the four strategy workers CEIPRT, CEITRG,
CEIUSB and CEIPFR, and CECODE) was built with `-DCE_INTEG_USE_SIMPLIFY=0` and without
`src/simplify.c`. On a real OS 5.8 ROM in CEmu, `int(x^2,x)` showed `((X^(2+1))/(2+1)) + C`, and
`ode_sep(x^2,x,y)` showed `(1*y) = ((x^(2+1))/(2+1)) + C`. The same flag also broke
u-substitution: CEIUSB compares the simplified `du` with the factors of the integrand, so with
the identity stub every `usub(...)` failed with "u-substitution not applicable".

simplify.c costs about 11 KB. CECISM, CEIPFR and CEITRG are within a few KB of the 51,200 B
ceiling (closer still with on-target steps), so the fix depends on the module:

| Module | Fix | Size on main b570ebb → branch |
|---|---|---|
| CECISM (int) | `src/fold_lite.c` | 45,334 → 46,728 B |
| CEIPFR (partial fractions) | `src/fold_lite.c` | 43,383 → 44,946 B |
| CEITRG (trig substitution) | `src/fold_lite.c` | 42,561 → 44,117 B |
| CEIUSB (u-substitution) | `src/simplify.c`, `SIMPLIFY_MAX_DEPTH=24` | 22,091 → 26,164 B (compressed) |
| CEIPRT (by parts) | `src/simplify.c`, `SIMPLIFY_MAX_DEPTH=24` | 28,316 → 40,111 B |
| CECODE (ODEs) | `src/simplify.c`, `SIMPLIFY_MAX_DEPTH=24` | 33,507 → 45,215 B |

- `src/fold_lite.c` (`CE_INTEG_USE_FOLD_LITE=1`, about 1.4 KB) folds the finished
  antiderivative only.
  - Integer arithmetic is exact. A division folds only when it is exact (`2/3` stays `2/3`),
    and values beyond ±2047 stay unfolded, so every product fits the eZ80's 24-bit `int`.
    32-bit `long` arithmetic made the first version 4.2 KB.
  - It applies `E+0`, `E-0`, `E*1`, `1*E`, `-1*E`, `E/1`, `E^1`, `0*E` and `-(-E)`. `E/(-1)`
    is left as it is, as on the desktop (`((x^-1)/-1)`); the rule did not fit in CECISM's
    budget.
  - Sign cleanup (`FOLD_LITE_SIGN_RULES`, about 170 B): `a+(-b)` becomes `a-b`, `a-(-b)`
    becomes `a+b`, and `(-a)+b` becomes `b-a`. CEIPFR and CEITRG have it. CECISM builds without
    it, because with on-target steps (#101) it sits within a few hundred bytes of the ceiling,
    so it shows `((-cos(x))+sin(x))` for `int(sin(x)+cos(x),x)`.
  - It cancels a coefficient when the denominator divides it, so `3*((x^3)/3)` becomes `x^3`.
    This runs as a loop, not recursion, so a long division chain cannot deepen the stack.
  - It never allocates, so it cannot exhaust the pool.
  - The tree walk is its only recursion, and it is capped at 40 levels; deeper subtrees are
    left as they are. A sum is left-deep, so in the antiderivative of a sum of more than about
    38 terms the leftmost terms stay unfolded, for example `((x^(1+1))/(1+1))`.
  - It does not do what simplify does beyond that: `x^((1/2)+1)` stays as it is, and so does
    `((x^3)/3)/2`. Results are still correct, just less tidy.
  - Its NaN test (`!(v >= -2047 && v <= 2047)`) assumes the CEdev soft-float compare treats
    NaN as unordered. That has not been checked on target; a NaN number node in an
    antiderivative is not expected.
- `integrate.c` checks the finished antiderivative with `ast_tree_complete()` before and after
  the final simplify/fold. A tree with a hole left by pool exhaustion is now the error "Out of
  memory while simplifying antiderivative"; before, it was printed. `deriv.c` uses the same check,
  which moved to `ast.c`.
- `ast_clone()` freed the operands of a failed `ast_binop`/`ast_unop`/`ast_func` a second time
  (those constructors already free them). The pool's guard ignored the second free only
  because nothing was allocated in between. Under pool exhaustion this happened in every
  integration worker and in CECALC: main shows 15 double frees for
  `nderiv(x^2*sin(x)*cos(x)*e^x,x,9)` alone. The redundant frees are removed. Desktop builds
  now count ignored double frees (`mem_double_frees()`), and with `-DMEM_POISON_ON_FREE` they
  overwrite freed nodes.
- `tests/cemu/test_module_flag_parity.py` now also runs the integration workers with their own
  flags. CECISM and CECODE go through the processor driver. The strategy workers run their own
  `strategy_main.c` through `tests/module_parity/strategy_driver.c`. The drivers also report
  leaked pool nodes and double frees, and the parity builds poison freed nodes. The tests
  check:
  - exact output;
  - that no unfolded constants remain;
  - that folded and unfolded results are numerically equal, for CECISM, CEITRG and CEIPFR;
  - pool exhaustion at 0..120 free nodes under ASan/UBSan, with no hole, leak or double free;
  - `tests/module_parity/fold_lite_driver.c`: a 200-division chain past the depth cap folds
    without recursion;
  - static rules: every built module linking `integrate.c` either simplifies or folds, and
    only fold_lite's depth-capped walk recurses.

  The file has 26 tests (20 of them new); against main's sources, 17 fail.
- Stack: on the real ROM, the worst 127-character inputs reach the same depth as on main,
  byte for byte. Three of them overflow on main as well and still do: `int(x+x+...)` with 58
  terms, `int(x^2+...)` with 30 terms, and `diffeq(x+x+...,x,y)` with 55 terms all hit the
  stack-limit NMI (RAM Cleared). The cause is integrate.c's sum recursion, before any
  simplification. §8.10 fixes it.

Emulator evidence only. Not verified on hardware.

### 8.10 Long sums in `int()` and `diffeq()` overflowed the OS stack

On a real OS 5.8 ROM in CEmu, three inputs that fit the 127-character line hit the stack-limit
NMI at 0xD19881, which shows as RAM Cleared: `int(x+x+...)` with 58 terms, `int(x^2+...)` with
30 terms, and `diffeq(x+x+...,x,y)` with 55 terms. Others did the same:
- `int(x*2*2*...*2,x)` and `int(x/2/.../2,x)`;
- `diffeq(y+x+x+...,x,y)`;
- runs of unary minus in front of a sum.

With on-target steps (#101), `int(3x^2-3x^2-...)` with 24 terms left only 63 B of stack.

- **Cause.**
  - `integ()` recursed once per term of a sum, and once per constant factor, with a 113 B
    frame. LTO had inlined every rule helper into it.
  - `ast_contains_var()` recursed down the left side of every chain, about 30 B per term.
  - Step text such as "Sum rule on %s" printed sub-expressions with `ast_print()`. It recurses
    through the whole tree, about 36 B per level, even after its 60 B buffer is full.
- **Small recursive spine.**
  - The linear rules now recurse through `integ_node()`, whose CE frame is 9 B. They are sums,
    negation, `c*f`, `f*c` and `f/c`.
  - The other rules run in `noinline integ_rules()`, which is reached only at leaves or by
    strategies capped by `depth > 8`.
- **Step text.**
  - Some steps name an expression: the header, the sum and constant-multiple steps,
    `integ_const()`, the power, linear-function and exponential rules, `try_parts()` and the
    nonlinear u-sub. They get their text from
    `noinline integ_text()`, which prints into a static buffer, so no print buffer sits in a
    recursive frame. `try_parts()` lost its 60 B buffer and the u-sub its 2×96 B buffers this
    way. Reusing this helper in the power and linear-function rules also saves 35 B in
    CECISM, offsetting the shared recovery growth from #119 without dropping any rules or
    changing ordinary step text. After rebasing onto #99, the `a^x` steps use the same helper,
    recovering a further 20 B. All these callers consume the shared text before any next print.
  - `...` names an expression whose leftmost path is longer than 24 levels, for example
    `int(...) dx` and `Sum rule on ...`. On that leftmost path it also names any node kind other
    than a number, variable, constant, binary or unary operator or function, such as a nested
    integral. Other text is unchanged.
- **Cap.**
  - `integ()` counts the calls that are open. Past `INTEG_MAX_DEPTH` it fails with
    "Expression too deep".
  - CECISM answers that itself instead of forwarding to the strategy workers, which would hit
    the same cap.
  - The default is 64. Sums and constant-factor chains that fit the line nest at most 61 calls,
    so they stay under it. A run of unary minus signs costs one character per level and can
    reach it, for example 48 signs in front of a 35-term sum.
  - Because the antiderivative's tree is about as deep as the nesting, the cap also bounds the
    result the processor prints.
  - CECODE uses 48. Its answer is printed under the processor's 256 B result buffer, and ODE
    sums run out of pool nodes past about 54 terms anyway.
- **Other stack fixes.**
  - `ast_contains_var()` loops down left operands and single-child nodes, and recurses only on
    right operands.
  - `ode_solve_separable()` and `ode_solve_linear()` print their answer straight into the result
    buffer, without four 120 B buffers.
  - CECODE builds with `DIFF_MAX_DEPTH=36`, as CECALC does. The linear-ODE split
    differentiates the whole right-hand side.
- **Size.** Rebased on aa9a4f3 (#99 and #119), CECISM with this change is 51,191 B,
  9 B under the unchanged 51,200 B ceiling. Any further change to `integrate.c` needs
  a fresh combined build and map diff.
- **Scope and residuals.** The stack-safety measurements below cover indefinite `int()`,
  strategy workers and `diffeq()`, not every definite-integral path. Fresh reviewer-requested
  126-character definite sums and constant-factor chains both return "Could not decide integral"
  without an NMI, but leave only 25 B on this candidate. These CECIDF inputs were not compared
  against `main`. A long constant-one base `(2-1+0+...)^x` also returns a suspicious, truncated
  `a^x/ln(a)` answer. This pass did not verify the full result or compare it against `main`.
- **Stack high-water.** It was measured with the probe that paints the free stack, on 63 inputs
  that can be typed on the CE. The measured headroom is summarized below.
  - The 14 worst cases from §8.9 keep at least 804 B. On main, three of them hit the NMI, and
    `int(x*x*...*x,x)` keeps 486 B.
  - The other long sums and chains keep at least 704 B.
  - Unary-minus runs at and past the cap keep at least 956 B, whether the leaf uses rules or
    strategies (sin(ax+b), 1/(ax+b), e^(2x), x*e^x, intparts, usub, trigsub and `diffeq`).
  - Chains inside a right operand (`int(x+x*x*...*x,x)`) keep at least 740 B.
  - Long constant chains in a power base, exponent or function coefficient keep at least
    738 B in the original measurement. Examples: `(2+2+...)^x`, `x^(2+2+...)` and
    `sin((2*2*...)x)`. All three now use `integ_text()` for step text.
  - Integrals and derivatives nested in the integrand keep at least 1,189 B, with one exception.
    `int(int(-----...2x+x+...))` still hits the NMI, as it does on main. Without a variable
    argument, the inner integral counts as a constant (the default variable is lowercase `x`;
    see the follow-up card). The constant rule then clones the 84-deep tree, and the processor
    prints the result through `ast_print()`. Bounding that printer is the follow-up to #112.
  - `%` and `!` build deeper postfix chains, but the CE launcher has no key for either.
- **Behaviour.**
  - The 58-term sum now shows a result instead of resetting.
  - Some long `diffeq()` sums give an error message instead of an answer. Main's desktop build
    fails the same way, because the 512-node pool runs out.
- **Tests.**
  - `tests/test_main.c` `test_integ_recursion_bound`:
    - the cap at exactly 64 nested calls, and the error past it;
    - no leak and no double free;
    - the flag resetting on every request, including a null one;
    - the `...` step text;
    - ODE answers cut to a short buffer;
    - `ast_contains_var` on a 200-deep chain.
  - `tests/cemu/test_module_flag_parity.py`:
    - long inputs with CECISM's and CECODE's own flags;
    - CECISM and CECODE built with `INTEG_MAX_DEPTH=8`, which shows the clean error and that
      CECISM does not forward it;
    - the shipped cap reached by a unary-minus run;
    - a static guard on the caps, the `noinline` layout and the step printer.

Emulator evidence only. Not verified on hardware.
