# CE string-encoding audit

Scope: active `make all` split build, baseline `8e8e232` (2026-11-09).

TI-OS large-font output is not UTF-8: bytes >= 0x80 select TI glyphs.
Keep ordinary result/step/error literals ASCII. Deliberate TI glyph escapes
(such as the launcher's `\xC1` left bracket) remain explicit and ROM-checked.

## Reported files and actual screen reachability

| File | Active CE compilation | Reported UTF-8 text reaches CE pixels? | Disposition |
|---|---|---|---|
| `src/split/processor.c` | Shared by CECALC, CEMAT, CESTPRB, CEFIN, CEGRPH, CELSCX, CEBASE, CEALGF, CEALGSY, CEALGEO, CEAPDV, CEASLV, CEASLA, CECLMB, CECLIA, CECLIC, CECLIR, CECIDF, CECISM, CECODE, CECSER, CECSEQ, CECPNM | No at the time of the audit: eigenval/eigenvec and their three lambda strings were inside `#ifndef HW_BUILD_SPLIT` (since moved to CEMAT's `handle_eigen`, ASCII only) | Replace the three literals with ASCII `lambda` as preventive cleanup |
| `src/dsolve.c` | None | No | Preserve desktop-only partial-derivative step labels |
| `src/vector.c` | None | No | Preserve desktop-only gradient/partial-derivative labels |
| `src/units.c` | None | No | Preserve Unicode input aliases; canonical output names already use `um`, `deg`, `c`, `f`, `us` |
| `src/ui.c` | None in active build | No | Preserve desktop pretty-math superscripts/subscripts; legacy `split/Makefile.core` references this file, but top-level `split-core` aliases `split-launcher`, not that makefile |

The legacy monolithic `HW_BUILD=0` configuration is not the shipped split
suite. This change does not enable its advanced operations on the CE.

## Guard

`tests/cemu/test_steps_on_target.py::test_step_worker_strings_are_ascii`
now derives its historical `STEP_SOURCES` name from both active top-level
module lists, each module makefile, shared runtime/force-included sources,
and recursive quoted project headers. The snapshot covers 35 modules,
50 C files and 40 headers, rather than nine hand-listed C files. No lambda
allowlist remains. Literal `+=` additions are accumulated; variable-based
module/source/runtime-source lists fail closed for explicit guard maintenance.
The scanner handles comments, quoted comment markers, character constants,
escaped quotes and line continuations. It forbids raw non-ASCII string text,
not explicit escaped bytes (which require separate semantic review).

## Verification and limitations

- RED: expanded guard rejected exactly the three lambda literals in processor.c.
- GREEN: full CE build, 35 stack-usage reports; all modules <=51,200 bytes.
- All 35 rebuilt variable payloads/checksums are identical to the baseline
  build after the 55-byte TI file header. Thus this source cleanup does not
  change shipped CE code. No generated package/CRC rewrite is necessary;
  the broader package refresh is owned by PR #102.
- Desktop and CE-float formatter suites each passed 13,511 checks; base-module
  harness passed 337 checks.
- Owned OS 5.8 ROM, fresh build and arTIfiCE launch: `eigenval(mat(2))` and
  `eigenvec(mat(2))` both return the pre-existing unsupported-operation error
  (`Polar/advanced op (use at top level)`), without a reset. Follow-up
  `an internal review` owns the missing CE eigen handler. (Since resolved:
  the launcher routes both to CEMAT, which runs `handle_eigen`.)
- **Renderer-only pixel fixture:** after a real `2+3` evaluation, emulator
  debug writes replaced the launcher's result buffer with representative
  ASCII lambda text, then the unmodified TRACE viewer rendered it. Inspected
  pixels show `lambda = {3, 1}`, `lambda1`, `lambda2`, and both vector labels
  with correct square brackets. This verifies ASCII/TI-bracket rendering,
  **not** eigenvalue functionality or numerical correctness.
- No physical calculator test, transfer, hardware-ready promotion, Downloads
  bundle, tag or release is claimed.
