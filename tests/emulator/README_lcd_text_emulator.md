# LCD text emulator suite (hardware-equivalent port of PR #20)

This directory contains a hardware-equivalent port of
`tests/test_lcd_text.py` — the 59-expression validation suite from PR #20
that drives the **desktop** `bin/calcengine.exe --solve` binary and asserts
on expected text substrings. The desktop path skips TI-OS, the module
dispatcher, and the eZ80 RAM path where real hardware bugs live (such as
the "2+3 RAM Cleared" regression). This suite drives the **real** `.8xp`
bundle inside the in-tree headless CEmu fork at
`tools/emulator/cemu-headless/`, so every expression flows through the
actual hardware code path.

## Files

| File | Purpose |
|------|---------|
| `test_lcd_text_emulator.py` | Pytest suite. 59 parametrized tests, one per expression. |
| `capture_lcd_text_baseline.py` | One-shot script to populate `expected_crcs` by running the emulator against a known-good build. |
| `_lcd_text_emulator_shared.py` | Shared plumbing: path resolution, encoder extension for `=`, headless session driver, baseline I/O. |
| `lcd_text_emulator_baseline.json` | The 59 expressions + their expected CRC list (populated by capture script). |

## Hard rules (user-enforced 2026-04-13)

1. **Pinned binary only.** The suite resolves the headless binary to a file
   under `tools/emulator/cemu-headless/` and refuses anything else. No PATH
   lookup. No GUI CEmu fallback. If the pinned binary is missing the suite
   HARD-FAILS at collection time (`pytest.exit`) rather than silently
   routing to a system CEmu install.
2. **Absolute paths only.** The ROM, bundle, and autotester are all
   resolved to absolute paths before anything launches.
3. **Containment check.** The shared module re-validates that the resolved
   binary lives under `tools/emulator/cemu-headless/` before starting the
   subprocess, as defence in depth.

## Why CRC and not substring?

The current headless fork exposes `get_screen` in two formats: `crc` (an
8-hex-char CRC of the LCD frame) and `png` (a PNG blob). ASCII/text
extraction is tracked as TODO in
`tools/emulator/cemu-headless/README.md` pending native cemu-core wiring.

Until text extraction lands, the strictest hardware-equivalent substitute
for PR #20's substring assertions is per-expression CRC comparison against
a captured baseline. The expected substrings from PR #20 are preserved in
the baseline JSON (`expected_text_substrings`) as a human-auditable record
of what each CRC represents, and are surfaced in failure messages.

When ASCII extraction lands, swapping this suite from CRC to substring is
a localized change: replace `get_screen format=crc` with
`get_screen format=ascii` in `HeadlessSession._capture_marker` and update
the assertion in `test_lcd_text_emulator.py::test_expression_lcd_crc` to
do `all(frag in text for frag in expected_substrings)` instead of CRC set
membership.

## Running the verify suite

```powershell
# Windows (where the headless fork runs)
python -m pytest tests/emulator/test_lcd_text_emulator.py -v
```

### Expected states

| Situation | Behaviour |
|-----------|-----------|
| Pinned headless binary missing | HARD FAIL at collection (`pytest.exit`) |
| `rom/Rom.rom` (and env var) missing | Module-wide skip with clear message |
| Bundle dir missing | Module-wide skip with clear message |
| `expected_crcs: []` for a given expression | That test SKIPS, points at capture script |
| `expected_crcs` populated, CRC matches | PASS |
| `expected_crcs` populated, CRC mismatches | FAIL (signal: real hardware-path regression OR stale baseline) |

## Capturing a fresh baseline

```powershell
# Full capture of all 59 expressions
python tests/emulator/capture_lcd_text_baseline.py

# Capture a single expression only (useful when iterating on one bug)
python tests/emulator/capture_lcd_text_baseline.py --only basic_arith_01_add

# Capture many, but do not write - inspect first
python tests/emulator/capture_lcd_text_baseline.py --dry-run

# Reset (overwrite) the CRC list for captured expressions instead of merging
python tests/emulator/capture_lcd_text_baseline.py --reset
```

The capture script does the following:

1. Resolve and sanity-check the pinned headless binary, ROM, bundle.
2. Load the existing `lcd_text_emulator_baseline.json`.
3. Launch `cemu-headless`, load ROM + bundle, boot TI-OS.
4. Press `clear`, then `prgm`, then `enter` to launch CalcEngine.
5. For each target expression: type it, press enter, wait for render,
   capture a CRC, press clear.
6. Merge captured CRCs into each expression's `expected_crcs` list (or
   overwrite under `--reset`).
7. Record `generated_utc`, `headless_binary_sha256`, `rom_sha256`, and
   `bundle_sha256` for drift detection.
8. Atomically rewrite the baseline file.

By default the capture script **merges** (dedup-safe append), so repeated
runs accumulate a small multiset of acceptable CRCs for render timing
flap. This matches the pattern used by `autotest.comprehensive.json` and
its sibling `.baselined.json`.

## Troubleshooting

### `ERROR: Headless CEmu binary not found`

The pinned path is
`tools/emulator/cemu-headless/cemu-headless.exe`
(with a `build_native/cemu-headless.exe` fallback, also under the fork).

Build it with `tools/emulator/cemu-headless/build.ps1` from a Developer
PowerShell prompt. Do NOT point at the GUI `CEmu.exe` — the suite refuses
to launch it.

### Screen capture count mismatch

If the session raises `Expected N get_screen responses, saw M`, the
emulator dropped a frame capture. Most common causes:

- An expression typed a symbol the encoder does not know, so the key
  sequence raised partway through. Check `_lcd_text_emulator_shared.encode_expression_tokens`
  for missing mappings.
- CalcEngine emitted an error and swallowed the CLEAR press. Add a longer
  `AFTER_ENTER_RESULT_MS` override for that one expression.
- The emulator ran out of wall-clock time inside one `step` call. Raise
  `HeadlessSession.SESSION_TIMEOUT_S`.

### CRC mismatch on one expression

The whole point of this suite. Walk the following tree:

1. Inspect recent `src/` changes affecting the processor that owns the
   expression (e.g. `deriv.c` for derivative cases, `integrate.c` for
   integration cases, `limits.c` for limits, etc.).
2. Rebuild the `.8xp` bundle and stage it in `deploy/hardware-ready`. Set `CALCENGINE_BUNDLE_DIR` for a different staging directory.
3. Rerun the suite. If the CRC now matches, a recent change broke the
   hardware path and has been fixed.
4. If the new CRC is provably correct (e.g. you deliberately changed a
   formatter), rerun `capture_lcd_text_baseline.py` to update the baseline
   and land both changes together.

## Desktop layer reminder

`tests/test_lcd_text.py` remains useful as a fast smoke test of the solver
core. Keep it; do not delete. This suite is the emulator-layer companion
that catches bugs the desktop path cannot see. The two together cover the
full stack.
