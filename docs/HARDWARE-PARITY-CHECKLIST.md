# CalcEngine Hardware Parity Checklist

> **Public source note:** the staged `deploy/hardware-ready/` transfer package is not part of this repository. Build the modules from source with CEdev as described in the [README](../README.md).

**Purpose:** Items that MUST be confirmed on real TI-84 Plus CE hardware before CalcEngine can be called "working." This checklist closes the gap between the emulator's deterministic scaffold and the real ASIC.

**Cross-reference:** `docs/EMULATOR-PARITY.md` describes the known emulator gaps. Each section below maps to those gaps and turns them into concrete pass/fail hardware checks.

**Status legend:** `[ ]` = not yet verified on hardware, `[x]` = confirmed passing.

---

## Preconditions

- [ ] Calculator OS version is recorded (target: 5.4 or 5.8 with arTIfiCE).
- [ ] If OS is 5.5 – 5.8, arTIfiCE has been activated for this boot.
- [ ] All `35` CalcEngine `.8xp` files plus `arTIfiCE.8xp` are present on the calculator.
- [ ] All 4 runtime `.8xv` AppVars (`libload`, `graphx`, `fileioc`, `keypadc`) are present.
- [ ] The transferred file set matches the staged blank-slate package from `deploy/hardware-ready/`.
- [ ] Support modules are archived in Mem Mgmt before beginning.

---

## 1. Keypad behavior (parity gap: debounce / scan latency / hold-repeat)

The emulator models keypresses as instantaneous bit-clears with no debounce or scan delay. Real hardware scans the matrix asynchronously.

- [ ] **Rapid ENTER presses:** Type `2+2`, press ENTER twice quickly. Result should appear without a double-submit or crash.
- [ ] **Key hold auto-repeat:** Hold the RIGHT arrow key on a result screen for 2 seconds. The scroll or cursor should repeat cleanly without freezing.
- [ ] **CLEAR on empty input:** Press CLEAR with an empty input buffer. CalcEngine should exit cleanly to the TI-OS home screen — no hang.
- [ ] **CLEAR during a long calculation:** Start `sum(1/n^2,n,1,9999)` and press CLEAR mid-run. The calculation should cancel within ~1 second and return to the prompt.
- [ ] **2nd + CLEAR (menu):** The function menu should open, navigate correctly, and close with CLEAR or ENTER.
- [ ] **No ghost keys:** Pressing multiple keys simultaneously (e.g. during menu navigation) should not produce phantom input.

---

## 2. Display rendering (parity gap: LCD pipeline / glyph timing / pixel fidelity)

The emulator writes to software text and graph buffers rather than through the real TI-84 CE LCD controller.

- [ ] **Basic text prompt visible:** The `Expr (MODE exits):` prompt is legible and correctly positioned at boot.
- [ ] **Result text legible:** After evaluating `x^2+1`, the result text is clearly readable and not clipped.
- [ ] **MathPrint stacked fraction:** Enter `(x+1)/(x-1)` — a stacked fraction should render, not linear `/` text.
- [ ] **MathPrint radical:** Enter `sqrt(x^2+1)` — the radical symbol and nested exponent should be readable.
- [ ] **MathPrint bounded form:** Enter `int(x^2,x,0,3)` or `sum(n,n,1,10)` — the bounds should be positioned correctly above/below the integral/sigma symbol.
- [ ] **MathPrint long expression:** Enter a 40+ character expression. Text should wrap or scroll without corrupting adjacent characters.
- [ ] **Graph axes:** Press `Y=`, enter `x^2`, press `GRAPH`. Axes should be clearly drawn and labeled.
- [ ] **Graph curve:** The parabola should be a smooth continuous curve with no missing segments.
- [ ] **Graph trace:** Press TRACE — the cursor should appear on the curve and the x/y display should update correctly as you move.
- [ ] **Shade region:** Enter `shade(x^2, x^3, 0, 1)` (or equivalent). The shaded region between the two curves should be filled correctly.
- [ ] **No LCD tearing:** After a result or graph renders, the screen should not flicker or show partial frames.
- [ ] **Screen after module swap:** After returning from a routed module (e.g. CEALG), the CalcEngine prompt must be fully redrawn — no stale pixels or garbled text.

---

## 3. Memory layout and VAT behavior (parity gap: TI-OS allocator / archive swap)

The emulator models archive and RAM as flat arrays with fixed pool addresses. Real hardware uses the TI-OS Variable Allocation Table (VAT) and dynamic heap.

- [ ] **Archive-to-RAM swap:** After a fresh boot with all modules archived, run an algebra expression. The module unarchives, runs, and re-archives without error.
- [ ] **Rapid domain switching:** Evaluate algebra → calculus → stats → matrix → graphing → algebra in a single session. Each swap should complete and the launcher should be responsive throughout.
- [ ] **VAT pressure:** After 10+ consecutive calculations across different domains, `CALCENG` should still launch modules correctly (no `ERR: MEMORY` due to fragmented RAM).
- [ ] **Module re-archive confirmed:** After each routed domain call, go to Mem Mgmt and confirm the invoked module is archived again. The launcher must not leave modules in RAM.
- [ ] **Settings survive swap:** Angle mode, step detail, and precision settings should be identical before and after a module call.
- [ ] **Ans survives swap:** Evaluate `2+3`, then evaluate `Ans*4`. The result should be `20`, not an error or stale value.
- [ ] **History survives swap:** The last 3 history entries should still be accessible via UP arrow after a cross-domain calculation.
- [ ] **RAM clear recovery:** After a RAM clear, confirm all archived modules are still present. Only programs stored in RAM (none by default in the transfer package) would be lost.
- [ ] **CEASLA (46 KB) loads cleanly:** The advanced solver module is one of the largest. Run `solve(x^3-6x^2+11x-6=0)` and confirm it loads and produces roots without error.
- [ ] **CECLIA (44 KB) loads cleanly:** Run `lim((1-cos(x))/x^2,x,0)` to exercise the advanced limit module and confirm it returns `1/2` without error.

---

## 4. Timer and RTC behavior (parity gap: real-time characteristics, jitter)

The emulator advances timers at a fixed rate tied to instruction boundaries. Real hardware uses the eZ80 hardware timer peripheral and RTC crystal oscillator.

- [ ] **Progress indicator timing:** Run `sum(1/n^2,n,1,5000)`. The `Computing...` indicator should appear within ~1 second of pressing ENTER (not immediately and not after the result).
- [ ] **No timeout on long calculation:** A long series sum should run to completion without the calculator resetting or returning an unexpected error. (The watchdog is not expected to fire during normal arithmetic.)
- [ ] **Keypad responsiveness during calculation:** While a long calculation is running, pressing CLEAR should be detected within ~500 ms. The emulator is deterministic here; hardware has real scan latency.
- [ ] **No off-by-one in repeat timing:** Hold DOWN arrow while scrolling a step-by-step result. The auto-repeat should not skip steps or fire extra events.

---

## 5. Enter-path: interactive flow (parity gap: re-entry callback, IPC handshake)

The emulator simulates the OS re-entry callback synchronously. Real hardware re-enters the launcher via the OS `RunPrgm` return path after the worker program exits.

- [ ] **2+2 → ENTER produces result:** The most basic enter-path: type `2+2`, press ENTER, confirm `= 4` appears.
- [ ] **ENTER on result screen returns to prompt:** After seeing a result, press ENTER. The input prompt should return and the input buffer should be ready for a new expression. No exit to TI-OS home screen.
- [ ] **ENTER reveals next checkpoint (step-by-step mode):** Enable step-by-step mode in settings. Evaluate `deriv(x^3,x)`. Each ENTER press should reveal the next checkpoint — not exit.
- [ ] **ENTER after all checkpoints exits nav, not the app:** After all checkpoints are revealed, the next ENTER should return to the reprocess state (reruns the expression), not exit CalcEngine.
- [ ] **ENTER in tutorial:** On first launch (or after clearing settings), ENTER should advance tutorial pages and not crash.
- [ ] **ENTER in function menu:** Navigate the menu with arrow keys, then press ENTER on a menu item. The template text should be inserted into the input buffer and the menu should close.
- [ ] **ENTER in settings menu:** Navigate to a setting (e.g. angle mode) and press ENTER to toggle it. The setting should change and persist across the next calculation.
- [ ] **ENTER in WINDOW editor:** Press Y=, enter a function, press WINDOW, change xMin, press ENTER. The viewport should update and the graph should redraw with the new range.
- [ ] **ENTER in ZOOM menu:** Open the ZOOM menu, select ZDecimal, press ENTER. The graph viewport should switch to [-4.7, 4.7] range.
- [ ] **Repeated rapid ENTER:** Press ENTER 10 times in quick succession on the result screen. The calculator should not crash, freeze, or exit.
- [ ] **ENTER with empty buffer:** Press ENTER with nothing typed. CalcEngine should either ignore it or display a brief error — it must not exit or crash.

---

## 6. Core correctness (smoke-level functional checks)

These are the minimum correctness checks that must pass before any performance or edge-case work.

- [ ] `2+2` → `4`
- [ ] `1/3+1/6` → `1/2` (or equivalent decimal)
- [ ] `factor(x^2-5x+6)` → `(x-2)(x-3)` (or equivalent)
- [ ] `solve(x^2-5x+6=0)` → roots `2` and `3`
- [ ] `deriv(x^3,x)` → `3*x^2`
- [ ] `int(x^2,x)` → `x^3/3 + C`
- [ ] `lim(sin(x)/x,x,0)` → `1`
- [ ] `taylor(sin(x),x,0,5)` → `x - x^3/6 + x^5/120`
- [ ] `det(mat(1,2;3,4))` → `-2`
- [ ] `mean(1,2,3,4,5)` → `3`
- [ ] `normalcdf(-1,1,0,1)` → approximately `0.6827`
- [ ] `fv(0.05,10,100,0)` → finite numeric result, no hang
- [ ] `seq(x,x,1,5,1)` → five-element list

---

## 7. Edge-case and failure path behavior

- [ ] `lim(1/x,x,0)` → reports DNE or limit does not exist — no crash.
- [ ] `continuity(1/x,x,0)` → reports discontinuity classification — no crash.
- [ ] `1/0` → error message on screen, returns to prompt cleanly.
- [ ] `sqrt(-1)` in real mode → error or `undefined` — no crash.
- [ ] `recseq(x/2+1,x,10,5)` → five-element recursive sequence.
- [ ] Deliberately missing support file: remove `CEALG.8xp` from the calculator, evaluate `factor(x^2-1)`. The launcher should display a recoverable error, not corrupt state.
- [ ] Very large expression `sum(k,k,1,10000)` completes or cancels cleanly with CLEAR.

---

## 8. Known hardware watch items (Pass 45 build)

- [ ] Emulator strict-mode gate passed on 2026-03-14. Confirm the same staged package behaves identically on hardware.
- [ ] `CEASLA.8xp` (advanced solve) remains one of the larger routed workers; watch for slow unarchive or memory errors on devices with low free RAM.
- [ ] `CECLIA.8xp` (advanced limit) remains one of the larger routed workers; watch for slow unarchive on devices with low free RAM.
- [ ] Watch for stale `Ans`, missing history, or lost settings after returning from any routed module.
- [ ] Watch for any launch failure that appears only after 5+ domain swaps in one session.
- [ ] Watch for LCD artifacts (stale characters or partial frames) after a module swap on OS 5.7 / 5.8.
- [ ] arTIfiCE must be verified to survive sleep/wake cycles on OS 5.5 – 5.8 before marking the checklist complete.

---

## Sign-off

| Check category | Verified by | Date | Notes |
|----------------|-------------|------|-------|
| Keypad behavior | | | |
| Display rendering | | | |
| Memory / VAT | | | |
| Timer / RTC | | | |
| Enter-path flow | | | |
| Core correctness | | | |
| Edge-case / failure paths | | | |
| Hardware watch items | | | |

**Overall hardware parity status:** Not yet verified — ready for directed smoke pass.
