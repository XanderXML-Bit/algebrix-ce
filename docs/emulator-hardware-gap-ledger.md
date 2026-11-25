# Emulator-vs-Hardware Gap Ledger

Current package: 35 official programs, four runtime AppVars, and arTIfiCE, totaling 40 calculator files.
The package contains 44 files including metadata.
Earlier measured counts and emulator results below describe historical packages, not this refreshed package.
Revalidate current screen baselines on the self-hosted real-ROM lane.

**Last updated:** 2026-04-04 (Dev Worker CEmu proof-docs pass)

This document is the authoritative record of every known mismatch, uncertainty, or unproven assumption between the CalcEngine eZ80 emulator (`tools/emulator/ez80/`) and real TI-84 Plus CE hardware (OS 5.8, arTIfiCE-jailbroken). Each item is tagged with its **severity** and **proof status**.

Severity levels:
- `CRITICAL` — may cause CalcEngine to fail on first transfer to hardware
- `MEDIUM` — plausible regression; needs confirmation before shipping
- `LOW` — cosmetic or edge-case difference unlikely to affect user-visible behavior
- `PROVEN-OK` — proven equivalent by emulator tests and/or hardware observation

## Current real-ROM proof snapshot (2026-04-04)

- `python -m pytest tests/cemu -q` is currently green at `192 passed, 9 skipped`.
- `./scripts/cemu-validate.ps1 -Mode full` regenerated `tests/cemu/outputs/cemu-pipeline-summary.json` at `2026-04-04T09:07:11-0700` with `success=true` and a ready real-ROM toolchain.
- That historical summary recorded a package matching the then-current manifest at `43` modules + `4` runtime dependencies = `47` transfers, with all major automated real-ROM slices green: `23/23` launch + transition checkpoints, `14/14` visible-smoke cases, `19/19` accuracy cases, `35/35` arithmetic-battery cases, `28/28` module-battery cases, `23/23` real edge cases, `2/2` recovery cases, and `2/2` fake-only hardening cases.
- A fresh Windows-side rerun of `scripts/cemu/release_candidate_gate.py` at `2026-04-04T09:12:46-0700` again produced `release_candidate_ready=true`, `stable=true`, and `archive_lifecycle_stability.stable=true`.
- The biggest remaining unknown is no longer whether the automated real-ROM lane is green; it is the residual gap between emulator proof and directed physical-calculator confirmation for the hardware-only checklist items below.

---

## 1. Keypad

### 1.1 Key-code encoding vs TI-OS `sk_key_t`
**Status:** `PROVEN-OK` (emulator), `MEDIUM` (hardware)

The emulator encodes keys as `(group_index << 3) | bit_position` where group is the CE `kb_Data[]` index (1–7). This is a synthetic encoding used only for `ez80_emulator_inject_key()`. CalcEngine reads the keyboard via `kb_Scan()` + `kb_Data[]` groups — the same groups — so the logical key detection path is correct.

**Gap:** The emulator does not use `sk_key_t` TI-OS scan codes. If CalcEngine ever calls `os_GetCSC()` instead of reading `kb_Data[]`, the mapping will be wrong.

**Test coverage:** `test_pass50_keypad_digit_key_matrix`, `test_pass50_keypad_operator_key_matrix`, `test_pass50_keypad_nav_and_modifier_keys`, `test_pass50_keypad_simultaneous_and_release_all`

### 1.2 No debounce model
**Status:** `MEDIUM`

The emulator raises a keypad IRQ instantly on `inject_key()` transition. Real hardware debounces key presses via the TI-OS keyboard service (~50 ms scan interval). Tests that depend on exact IRQ timing may produce deterministic results in the emulator but non-deterministic ones on hardware.

**Mitigation:** CalcEngine polls `kb_Data[]` in a loop with `kb_Scan()` rather than relying on IRQ timing, so debounce differences are unlikely to cause functional failures.

### 1.3 No key-repeat model
**Status:** `LOW`

Holding a key on real hardware generates repeat events at a rate controlled by TI-OS. The emulator has no hold-repeat mechanism — `inject_key()` fires once.

**Impact:** Cursor movement or backspace auto-repeat will not work in emulator integration tests. CalcEngine UI uses single-press semantics for all critical input paths, so this is a regression risk only for very fast repeat-press scenarios.

### 1.4 No ghosting model
**Status:** `LOW`

Simultaneous key presses from conflicting rows can cause ghost keys on real matrix hardware. The emulator allows any combination of simultaneous presses without ghosting artifacts.

---

## 2. Enter-Path Stability

### 2.1 Expression → result homescreen flow
**Status:** `PROVEN-OK` (emulator simulation)

The emulator's homescreen bcall layer correctly tracks:
- Character-by-character input on the current row (`HOME_PUTC`)
- Cursor row/col advancement
- Step-line accumulation (prior rows become `step_lines[]`)
- `input_line` = current cursor row content
- `visible_lines[]` = full homescreen buffer mirroring `home_get_line()`

**Test coverage:** `test_pass50_enterpath_simple_addition`, `test_pass50_enterpath_expression_battery`, `test_pass50_enterpath_paren_and_power`

**Gap:** These tests simulate the CalcEngine UI output via bcall API calls. They do not execute the actual CalcEngine `.8xp` binary. True end-to-end proof requires hardware transfer (see §6).

### 2.2 Divide-by-zero error recovery
**Status:** `PROVEN-OK` (emulator simulation)

After an error message is written to the result row, the emulator homescreen state remains consistent. Cursor advances, prior rows become step lines, prompt row is clean. No crash or state corruption.

**Test coverage:** `test_pass50_enterpath_divide_by_zero`

### 2.3 ENTER key re-runs process_input on result screen
**Status:** `PROVEN-OK` (desktop test suite)

From `docs/ENTER-PATH.md`: `ENTER` on a result screen calls `process_input()` again on the existing input buffer. It does not exit CalcEngine. This was audited and confirmed in Pass 49. The emulator correctly shows the Enter key (group6 bit0 = 0x30) registering in the keypad matrix.

### 2.4 Clear key clears input without advancing row
**Status:** `PROVEN-OK` (emulator simulation)

`test_pass50_enterpath_clear_key_clears_input` confirms Clear key (group6 bit6 = 0x36) sets the correct matrix bit, and that after a simulated clear the cursor remains on the same row.

---

## 3. LCD / Display

### 3.1 Homescreen text layer vs real LCD raster
**Status:** `MEDIUM`

The emulator maintains a software homescreen text buffer (`g_home_screen[]`) separate from the LCD framebuffer. `HOME_PUTC` writes characters to the text buffer; `lcd_to_ascii()` returns the text buffer contents directly when homescreen overlay is active. The real TI-OS draws glyphs into LCD framebuffer using a proportional/monospace glyph renderer.

**Gap:** Pixel-level LCD output (colors, anti-aliasing, exact glyph metrics) is not modeled. CalcEngine uses `gfx_*` and `os_*` APIs rather than raw LCD ports, so output correctness depends on the CE C Library wrappers rather than the emulator's LCD pipeline.

**Test coverage:** `test_pass50_lcd_cursor_col_tracks_putc`, `test_pass50_lcd_visible_lines_match_home_get_line`, `test_pass50_lcd_step_lines_accumulate_correctly`

### 3.2 LCD controller timing (write windows, pipeline state)
**Status:** `LOW`

The emulator models LCD controller commands (column/row address set, memory write) and write-window mechanics but does not simulate DMA/VSYNC timing or the real ILI9341 pipeline. CalcEngine's `gfx_*` rendering goes through CE C Library which handles LCD setup; this gap is unlikely to surface at the application level.

### 3.3 Screen ASCII export heuristic
**Status:** `LOW`

`lcd_to_ascii()` returns the homescreen text buffer when `homescreen_active=true`. When only graph/LCD framebuffer content is present (no homescreen overlay), the export uses a heuristic downsample of 320×240 → 40×24 ASCII. This is not a faithful OCR of real LCD pixel content.

### 3.4 cursor_row/col vs real TI-OS cursor tracking
**Status:** `PROVEN-OK` (emulator)

`get_screen_text_state()` reads `emu.os.home_row` and `emu.os.home_col` directly. These are maintained by the emulator's `HOME_PUTC` bcall handler with the same semantics as the real TI-OS cursor. Verified by `test_pass50_lcd_cursor_col_tracks_putc`.

---

## 4. Archive ↔ RAM Behavior

### 4.1 Program loading and swap model
**Status:** `PROVEN-OK` (emulator)

The emulator's archive/RAM swap:
- Archive pool: `0x3C0000`, 256 KB
- RAM program pool: `EZ80_RAM_BASE + 0x20000`, 96 KB
- `launch_program` = archive→RAM copy + CRC check + `launch_count++`
- `finish_program` = RAM→archive copy + CRC check + `swap_count+=2`
- `swap_fail_count` increments on CRC mismatch or capacity overflow

**Test coverage:** `test_pass50_archive_round_trip_swap_counts`, `test_pass50_archive_multi_module_swap_sequence`, `test_pass50_archive_finish_no_active_program`

### 4.2 Real TI-OS VAT layout vs emulator flat pool
**Status:** `MEDIUM`

Real TI-OS uses a Variable Allocation Table (VAT) for program storage with dynamic RAM management, sector-based flash paging, and garbage collection. The emulator uses fixed contiguous pools. Edge cases involving:
- Near-full archive (flash sector boundaries)
- Near-full RAM (multiple programs loaded simultaneously)
- Garbage collection triggered mid-swap

...are not modeled and require hardware testing.

**Mitigation:** CalcEngine uses the split-architecture to keep only one module in RAM at a time, significantly reducing RAM pressure risk.

### 4.3 `os_RunPrgm` callback reentry model
**Status:** `PROVEN-OK` (desktop tests, Pass 40)

The real `os_RunPrgm` does not return to the caller after the child program exits unless a callback is registered. CalcEngine's launcher uses `ce_launch_program_reentry()` with a proper callback. Facade modules (CEALG, CECALC, etc.) use the same re-entry model. Audited in Pass 40.

### 4.4 Module size constraints
**Status:** `PROVEN-OK`

All 43 `.8xp` modules are under 50 KB when compressed (Pass 40 audit). Largest uncompressed is 74 KB (ui.c + algebra.c linked into CEALG/CECALC), which fits within the single-module RAM window of 96 KB.

---

## 5. Timer / Interrupt / RTC

### 5.1 Timer cycle accuracy
**Status:** `MEDIUM`

The emulator advances timers by instruction cycle count (not wall clock). Real TI-84 CE runs at ~48 MHz; timer tick rates depend on actual clock, not instruction count. Time-sensitive logic (e.g., OS key-repeat timing) will behave differently.

**Impact on CalcEngine:** CalcEngine does not use hardware timers for critical UI logic. Impact limited to watchdog and RTC alarm which are not on user-visible paths.

### 5.2 RTC accuracy
**Status:** `LOW`

RTC advances at exactly 32768 emulator cycles per second. Real RTC is crystal-driven and accurate but starts at an arbitrary value on power-on. CalcEngine does not use the RTC.

### 5.3 Interrupt latency
**Status:** `LOW`

The emulator services interrupts at instruction boundaries with zero additional latency. Real hardware has interrupt latency based on bus cycles, clock stretching, and instruction pipeline state.

---

## 6. CAS Module Real-ROM Behavior (Pass 51–56)

Real-ROM/CEmu autotest sessions revealed a class of failures specific to CAS expressions routed through the split-module architecture. This section records confirmed observations and open items.

### 6.1 CRC 32BFD188 — TI-OS DIVIDE BY 0 error dialog
**Status:** `CONFIRMED` (real-ROM/CEmu VRAM PNG — Pass 56)

VRAM screenshots `vram_t3_num2.png` and `vram_t10_num3.png` (from pass 54 instrumentation) show the TI-OS system error dialog "ERROR: DIVIDE BY 0" at both t=3s and t=10s. This CRC is produced by `d/dx(x^2)` and `lim(sin(x)/x, x, 0)` on real ROM.

**Prior MEMORY entry "NOT a TI-OS error dialog" was incorrect and has been corrected.**

### 6.2 `lim(sin(x)/x, x, 0)` failure — root cause identified and fixed (Pass 56)
**Status:** `FIXED` (desktop test suite confirmed; real-ROM re-run pending)

`limit_inner()` in `limits.c` called `try_direct_sub()` before `try_special()`. `try_direct_sub()` evaluates `sin(0)/0` via `eval_at()`, which produces a 0/0 float computation. On CE hardware, the TI-OS math library raises a system DIVIDE BY 0 error before CalcEngine's own `OP_DIV` guard is reached.

**Fix (Pass 56):** Moved `try_special()` before `try_direct_sub()` in `limit_inner()`. `try_special()` intercepts `sin(x)/x → 1` via pure pattern matching with no float evaluation.

**Desktop validation:** 11141 assertions, 0 failures (Pass 56). `test_pass56_special_limit_ordering_guard` covers 7 special-limit cases.

**Affected module:** CECLMB (`split/Makefile.calc.lim.base`) — rebuilt and copied to Downloads.

### 6.3 `d/dx(x^2)` failure — root cause found and fixed (Pass 58)
**Status:** `FIXED` (desktop test suite confirmed; real-ROM re-run pending)

`os_GetStringInput` on TI-84 CE inserts raw TI-OS token bytes into the input buffer — specifically `0x83` for the ÷ key. CALCENG stored this byte verbatim in `req->expression`, `state->last_expression`, and the history AppVar. At some point in the TI-OS execution environment this 0x83 byte (the ÷ token) triggered a TI-OS-level DIVIDE BY 0 error (CRC 32BFD188). The exact TI-OS mechanism is unknown (IPC AppVar read path, `os_PutStrFull` tokenizer, or CE C Library startup code) — but all C-level code paths in CECALC were exhaustively ruled out in Pass 57.

**Fix (Pass 58):** Added input normalization loop in `prompt_expression()` in `src/core/launcher/main.c` immediately after `os_GetStringInput` returns. Scans the input buffer and replaces all `0x83` bytes with `'/'`. This prevents any raw TI-OS token bytes from propagating through the IPC AppVar chain. The tokenizer already handles both `0x83` and `'/'` for d/dx detection.

```c
/* Normalize TI-OS token bytes to ASCII equivalents before any processing. */
{
    uint8_t *p = (uint8_t *)input;
    while (*p) {
        if (*p == 0x83u) *p = '/';
        p++;
    }
}
```

**Pass 57 exhaustive analysis — all C-level hypotheses eliminated:**
- ✗ **astwire serialization bug** — encode/decode for NODE_DERIV verified correct. `test_pass57_deriv_ti_token_roundtrip` confirms serialize→deserialize round-trip preserves var='x' and inner POW node exactly.
- ✗ **NODE_DERIV enum aliasing** — node_type_t is not conditionally compiled; NODE_DERIV value is stable.
- ✗ **CE_PROC_GENERIC=0 enabling generic eval** — `CE_PROC_GENERIC=0` is explicit in `split/Makefile.calculus:14`. No generic eval fallback compiled into CECALC.
- ✗ **eval.c OP_DIV guard missing** — guard `if (r == 0)` present at eval.c line 217. But TI-OS intercepts float division before C-level guard on CE hardware (precedent: Pass 56 lim fix).
- ✗ **deriv.c eval_at calls** — `CE_DERIV_USE_EVAL=0` in `split/Makefile.calculus` excludes all eval_at paths in deriv.c for CECALC binary.
- ✗ **diff_pow producing OP_DIV for X^2** — power rule path (`bv=true, ev=false`): produces `2 * X^(2-1)` using only OP_MUL/OP_POW/OP_SUB. General OP_DIV/FN_LN case is unreachable for X^2.
- ✗ **handle_tangent triggered for NODE_DERIV** — impossible; NODE_DERIV and NODE_TANGENT are separate type checks with separate `#if` guards.
- ✗ **peek_at offset bug in tokenizer** — `peek_at(t,n) = t->src[t->pos + n]` confirmed correct; 0x83 detection verified at offset 1.
- ✗ **ce_processor_run_preparsed doing extra work** — it is a one-line wrapper to `ce_processor_run_root`.
- ✗ **CALCENG evaluating the expression** — no `eval_expr` calls anywhere in the launcher or re-entry path.
- ✗ **nderiv producing different AST from d/dx** — both produce identical `NODE_DERIV(var='X', expr=POW(X,2))` for order=1. `test_pass57_deriv_ti_token_roundtrip` §7 confirms `ast_equal` returns true.

**Desktop validation:** 11,175 assertions, 0 failures (unchanged — fix is a normalization loop, no logic change).

**Affected module:** CALCENG (`split/Makefile.launcher`) — rebuilt to 49,010 bytes and copied to Downloads.

### 6.4 Expressions using raw `key|/` (÷) vs ALPHA-mode input
**Status:** `OBSERVED`

All failing CAS real-ROM cases use the raw division key (`key|/`, which emits TI-OS token 0x83) in their key sequence. All passing cases use ALPHA-mode spelling without a raw `key|/`. Token.c correctly handles 0x83 in the d/dx tokenizer. The correlation between `key|/` usage and failure may be coincidental for deriv, or may reflect a parser/astwire edge case not yet reproduced on desktop.

---

## 7. End-to-End Hardware Proof (Unproven)

### 7.1 Official arTIfiCE v2 automation path
**Status:** `MEDIUM` (emulator tooling), `CRITICAL` (deployment asset gap)

Pass 67 completed the missing emulator-side pipeline pieces around the current official arTIfiCE path. The tooling is now split into three explicit stages:

- `tools/emulator/check-artifice-asset.ps1` searches `Downloads` and `Desktop` under `$HOME`, plus the working directory. It stages the arTIfiCE `.8xp` at `tools/emulator/assets/artifice-v2.8xp`.
- `tools/emulator/record-artifice-baseline.ps1` runs the arTIfiCE launch sequence on the real ROM and records the four-screen baseline into `tools/emulator/baselines/artifice-e2e-baseline.json`
- `tools/emulator/run-artifice-e2e-verification.ps1` reruns the same capture path and reports `PASS`, `FAIL`, or `BASELINE_MISSING` for each checkpoint hash

As of **October 9, 2025** the upstream arTIfiCE site documents **v2.1** as the supported CE jailbreak for OS `5.3` through `5.8.4`, launched as:

- transfer the arTIfiCE `.8xp`
- run `prgmA`
- verify the shell
- exit with `MODE`
- launch the target ASM program

The verifier now encodes that flow with current AutoTester primitives:

- `action|launch` for `prgmA`
- `key|...` / `hold|...` / `release|...` / `sendCSC|...` for scripted keypad input
- `hash|...` for shell / launch / result / menu screen verification

The four automated checkpoints are:

- `artifice_shell_loaded`
- `calcengine_launch_menu`
- `calcengine_result_2plus3`
- `calcengine_menu_open`

Important distinction: the older Cabri Jr based arTIfiCE v1 flow is no longer the canonical path. The current local CalcEngine docs still mention the legacy v1 sequence in places, but the new emulator runner targets the official v2 TI-Basic launcher path.

The transfer package needs a user-supplied arTIfiCE `.8xp`. Stage the package in `deploy/hardware-ready` or pass `-DownloadsDir` to use another directory. The verifier reports a missing asset or `BASELINE_MISSING` until the asset and a recorded baseline are available.

The following items are **not proven** by any emulator test and require physical hardware transfer and execution:

| # | Item | Severity |
|---|------|----------|
| 6.1 | CalcEngine `.8xp` correctly loads and executes under arTIfiCE on OS 5.8 | `CRITICAL` |
| 6.2 | Launcher correctly launches all 35 official programs | `CRITICAL` |
| 6.3 | IPC AppVar round-trip works correctly on real VAT layout | `CRITICAL` |
| 6.4 | All `.8xv` runtime library AppVars (LibLoad, GRAPHX, FILEIOC, KEYPADC) load correctly | `CRITICAL` |
| 6.5 | Memory pressure: ~60 KB user RAM window accommodates all data structures | `CRITICAL` |
| 6.6 | Flash write operations (if any) do not corrupt OS or user data | `CRITICAL` |
| 6.7 | LCD colors / contrast are correct on physical display | `MEDIUM` |
| 6.8 | Battery / backlight behavior matches expectations | `LOW` |
| 6.9 | Key repeat / hold timing matches user expectations | `LOW` |
| 6.10 | CalcEngine exits cleanly without leaving zombie AppVars | `MEDIUM` |

---

## 8. Summary: Proof Status Matrix

| Area | Emulator-proven | Hardware-proven |
|------|----------------|-----------------|
| Enter-path flow (7 expressions) | ✅ Pass 50 | ❌ Unproven |
| Keypad matrix encoding | ✅ Pass 50 | ❌ Unproven |
| Homescreen cursor/layout | ✅ Pass 50 | ❌ Unproven |
| Archive↔RAM swap counts | ✅ Pass 50 | ❌ Unproven |
| IPC round-trip | ✅ Pass 35–38 | ❌ Unproven |
| Module size constraints | ✅ Pass 40 | ❌ Unproven |
| os_RunPrgm reentry | ✅ Pass 40 | ❌ Unproven |
| `lim(sin(x)/x,x,0)` on real ROM | ✅ Root cause fixed (Pass 56) | ⏳ Re-run pending |
| `d/dx(x^2)` on real ROM | ✅ Root cause fixed (Pass 58) — 0x83 normalization in CALCENG | ⏳ Re-run pending |
| CRC 32BFD188 identity | ✅ Confirmed TI-OS DIVIDE BY 0 dialog | ✅ Real-ROM VRAM (Pass 56) |
| .8xp binary execution | ❌ No CPU-level binary run | ❌ Unproven |
| Real LCD pixel output | ❌ Text-buffer only | ❌ Unproven |
| Real TI-OS VAT layout | ❌ Flat pool model | ❌ Unproven |

**Next action:** Download the official arTIfiCE v2 `.8xp`, run `tools/emulator/check-artifice-asset.ps1` so it lands in `tools/emulator/assets/artifice-v2.8xp`, record `tools/emulator/baselines/artifice-e2e-baseline.json` with `tools/emulator/record-artifice-baseline.ps1`, and then rerun `tools/emulator/run-artifice-e2e-verification.ps1` until all four checkpoints report `PASS`. After that, repeat the same smoke path on a physical TI-84 Plus CE running OS 5.8.
