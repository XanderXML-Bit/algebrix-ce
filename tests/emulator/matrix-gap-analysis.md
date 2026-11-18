# Matrix Literal Entry Gap Analysis

Updated: 2026-03-21 (Pass 64)

## Scope

This document records the exact real-ROM AutoTester experiments attempted for matrix-expression coverage and why no matrix hashes were added to the comprehensive `87`-hash suite.

The missing capability is not matrix math itself. Desktop coverage already exercises `det`, `inverse`, `transpose`, `rref`, LU, rank/nullity, dot, and cross products. The blocker is deterministic matrix-literal entry in the real-ROM CEmu AutoTester lane.

## Attempted Paths

### 1. Direct semicolon key injection

Artifact:
- `tests/emulator/outputs/pass63-probe-semicolon.json`

Attempt:
- Launch CalcEngine.
- Send a literal `key|;`.
- Hash the screen after the attempted semicolon insertion.

Why it failed:
- AutoTester does not expose a stable direct semicolon keypath for the TI-84 Plus CE homescreen.
- A matrix literal needs row separators, so without a reliable semicolon event there is no deterministic way to enter `mat(1,2;3,4)` from plain text.

Conclusion:
- `key|;` is not a usable real-ROM input primitive for matrix coverage.

### 2. `APPS -> ;` semicolon entry during a typed matrix literal

Artifact:
- `tests/emulator/outputs/pass63-probe-matrix.json`

Attempt:
- Build expressions such as:
  - `det(mat(1,2;3,4))`
  - `inverse(mat(1,2;3,4))`
  - `transpose(mat(1,2;3,4;5,6))`
- Replace each required semicolon with `key|apps`, matching the TI-OS semicolon entry path.

What was observed:
- `key|apps` definitely changes the screen state on the real ROM.
- After that transition, follow-on digits, commas, and parens do not produce a stable, repeatable continuation of the matrix literal.
- Because the post-`APPS` state is unstable, no trustworthy result hash can be captured for the final matrix expression.

Why it failed:
- AutoTester operates at a high-level key-name layer and does not provide enough visibility into the transient `APPS` state to guarantee that the next character stream is going into the intended text field as `;`.
- The CRC changes prove that the key landed somewhere; they do not prove that the resulting character stream is a valid matrix literal.

Conclusion:
- `key|apps` is not deterministic enough for matrix-literal hashing on the current real-ROM path.

### 3. `2nd + CLEAR` template/browser fallback

Artifacts:
- `tests/emulator/outputs/pass63-probe-templates.json`
- `tests/emulator/outputs/pass63-probe-template-matdet.json`

Attempt:
- Open the function/template browser with `2nd` + `CLEAR`.
- Navigate to the quick-template pages that could, in principle, preinsert matrix-related syntax without requiring manual semicolon entry.
- Hash the inserted template/result state.

What was observed:
- The template/browser path is reachable.
- The real-ROM navigation path is not stable enough to guarantee that the same matrix-related insertion lands on every run.
- The quick-template insertion path can drift between browser states, which makes any captured CRC unsafe as a regression oracle.

Why it failed:
- The current AutoTester abstraction cannot assert enough about the intermediate browser focus/selection state.
- Without lower-level display polling or menu-state introspection, the template route is still non-deterministic.

Conclusion:
- Template-based fallback is not strong enough to baseline as a real-ROM matrix hash.

## Why No Matrix Hash Was Added

Adding a matrix hash now would create false confidence. The problem is not that the resulting matrix operation is unknown; the problem is that the input path cannot yet prove the same matrix literal was entered on each run.

The comprehensive suite only carries hashes whose input path is deterministic enough to defend.

## What a Future Solution Requires

At least one of these capabilities is needed:

1. Native CEmu LCD/state polling during input so the runner can confirm that `APPS` actually inserted `;` before continuing.
2. Lower-level keypad injection that drives the TI-84 CE keyboard matrix directly instead of only using the current high-level AutoTester key-name layer.
3. A headless CEmu fork or extended API that exposes homescreen text, cursor position, and active menu state between keypresses.
4. A purpose-built scriptable calculator control tool that can gate each follow-on key on observed LCD state rather than blind delays.

## Resolution: mat2() parser extension

A bounded parser change was added to `src/parser.c` (`parse_mat2_literal`) and
`src/token.c/h` (`TOK_MAT2`) that introduces a `mat2(a,b,c,d)` convenience constructor.

`mat2(a,b,c,d)` accepts exactly four comma-separated elements and produces an identical
`NODE_MATRIX_OP(MAT_LITERAL, rows=2, cols=2)` to `mat(a,b;c,d)` — without requiring a
semicolon. This makes matrix-literal entry fully deterministic in the AutoTester key layer.

The CEMAT probe `s99_cemat_det_mat2` uses `det(mat2(1,2,3,4))` = -2 as the baseline
expression. Key sequence types "DET(MAT2(1,2,3,4))" entirely via alpha keys and digits.

## Current Status

- Matrix math implementation: covered on the desktop/native harness.
- Real-ROM matrix hash coverage in CEmu AutoTester: **UNBLOCKED** via `mat2()` extension.
- Probe `s99_cemat_det_mat2` added to `autotest.module-coverage.json`.
- The four attempted semicolon-injection paths documented above remain blocked; the mat2
  workaround makes them unnecessary for the 2×2 case that suffices to prove CEMAT dispatch.
