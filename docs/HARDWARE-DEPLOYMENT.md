# CalcEngine Hardware Deployment

> **Public source note:** the staged `deploy/hardware-ready/` transfer package is not part of this repository. Build the modules from source with CEdev as described in the [README](../README.md).

## OS compatibility

| TI-84 Plus CE OS | Launch mode | Notes |
| --- | --- | --- |
| 5.0 - 5.4 | Native | No jailbreak required |
| 5.5 - 5.8 | arTIfiCE jailbreak | Run arTIfiCE before launching CalcEngine |

## Files to transfer

Transfer every file below. Do not transfer only `CALCENG.8xp`.

### Launcher

- `CALCENG.8xp`

### Top-level routed modules

- `CEALG.8xp`
- `CECALC.8xp`
- `CELIMS.8xp`
- `CEMAT.8xp`
- `CESTPRB.8xp`
- `CEFIN.8xp`
- `CEGRPH.8xp`
- `CELSCX.8xp`

### Worker and support modules

- `CEBASE.8xp`
- `CEALGF.8xp`
- `CEALGSY.8xp`
- `CEALGEO.8xp`
- `CEAPDV.8xp`
- `CEASLV.8xp`
- `CEASLA.8xp`
- `CECLIM.8xp`
- `CECLMB.8xp`
- `CECLIA.8xp`
- `CECLIC.8xp`
- `CECLIR.8xp`
- `CECINT.8xp`
- `CECIDF.8xp`
- `CECISM.8xp`
- `CECIAD.8xp`
- `CEIPRT.8xp`
- `CEITRG.8xp`
- `CEIUSB.8xp`
- `CEIPFR.8xp`
- `CEISTR.8xp`
- `CECODE.8xp`
- `CECSER.8xp`
- `CECSEQ.8xp`
- `CECPOL.8xp`
- `CECPNM.8xp`

### Blank-slate OS 5.5–5.8 launch payload

- `arTIfiCE.8xp`

### Required runtime dependency appvars

- `libload.8xv`
- `fileioc.8xv`
- `graphx.8xv`
- `keypadc.8xv`

## Transfer instructions

### USB and TI Connect CE

1. Check the calculator OS version from `[2nd]` `[+]` -> `1: About`.
2. If the calculator is on OS `5.5` through `5.8`, run arTIfiCE first.
3. Connect the TI-84 Plus CE to the computer with the USB cable.
4. Open TI Connect CE.
5. Choose `Send to Calculators` or drag the contents of `deploy/hardware-ready/` into TI Connect CE.
6. Select all top-level calculator transfer files listed above — all `35` CalcEngine `.8xp` modules, `arTIfiCE.8xp`, and the `4` required `.8xv` dependency AppVars — and send them in one transfer pass.
7. Confirm the transfer queue contains `40` calculator files total and that the four runtime `.8xv` files are included.
8. Wait for TI Connect CE to finish before disconnecting the cable.

Notes:

- The support modules are required even though normal users should only launch `CALCENG`.
- The `.8xv` dependency appvars are also required on blank-slate hardware; do not omit them.
- Ignore the optional `_clibs` helper folder if the top-level `.8xv` files are already being transferred.

## First-run diagnostic instructions

1. Leave the calculator on the home screen.
2. Press `[prgm]`.
3. On OS `5.5` through `5.8`, run `arTIfiCE` before `CALCENG`.
4. Run `CALCENG`.
5. If the staged package boots, expect a text prompt that starts with `CalcEngine` and then `Expr (MODE exits):`.
6. Enter `2+2` and press `[enter]`. The [v2.0.0 physical test on 2026-04-14](V2_HARDWARE_BUGS.md) failed. Record the result for the build you are testing as diagnostic evidence, not release signoff; no current build is certified on physical hardware.
7. Record the exact screen/result, any literal format strings, crashes, extra keypresses, or prompt-return behavior before changing the package.

Normal usage expectations after hardware fixes are verified:

- Launch only `CALCENG` from `[prgm]`.
- Use `2nd` + `CLEAR` for the function menu.
- Use `Y=` and `GRAPH` for graphing inside CalcEngine.
- The launcher handles module routing and archive-to-RAM swaps automatically.

## Troubleshooting

### `ERR: INVALID`

- OS `5.5` through `5.8` requires arTIfiCE before running CalcEngine.
- Re-run the arTIfiCE activation path after a RAM reset.

### `ERR: MEMORY` or an immediate return to the home screen

- Re-send all `35` CalcEngine `.8xp` modules, `arTIfiCE.8xp`, and the `4` required `.8xv` dependency AppVars, not just `CALCENG.8xp`.
- Confirm that no file transfer was skipped or renamed.
- Pay extra attention to advanced solve and advanced limit flows; `CEASLA.8xp` and `CECLIA.8xp` remain among the larger routed workers in the current manifest.

### `Missing library` / `Need LibLoad`

- The runtime dependency appvars were not fully transferred.
- Re-send these four top-level files from `deploy/hardware-ready/`:
  - `libload.8xv`
  - `fileioc.8xv`
  - `graphx.8xv`
  - `keypadc.8xv`
- You do **not** need the optional `_clibs` helper folder if those top-level `.8xv` files were already sent.

### A feature appears to be missing

- Missing support modules break routed features even when `CALCENG.8xp` is present.
- Re-transfer the full package from `deploy/hardware-ready/`.

### `deploy/hardware-ready/` looks incomplete or stale

- The current blank-slate package should contain `40` calculator transfer files: `35` CalcEngine `.8xp` modules, `arTIfiCE.8xp`, and the `4` runtime `.8xv` AppVars.
- The directory should contain `44` files total once `README.md`, `README.txt`, `SHA256SUMS.txt`, and `VERSION.txt` are included.
- If those counts do not match, refresh the package from current `main` before opening TI Connect CE.

### A support module was launched directly from `[prgm]`

- That is not the normal workflow.
- Some support programs may display messages such as `Use CALCENG.` or otherwise refuse standalone use.
- Return to the home screen and launch `CALCENG` instead.

### TI Connect CE does not see the calculator

- Make sure the calculator is powered on.
- Try a different USB port.
- Try a data-capable cable.
- Restart TI Connect CE and reconnect the calculator.

## Retired modules

`CEALGA`, `CEALGS`, `CEASYM`, `CECDRV`, `CECMPLX`, `CEPROB`, `CESTAT`, and `TMPCALC` are retired.
`make all` does not build them, and no current source dispatch path launches them.
The five surviving legacy makefiles remain available for historical reference only.

## Revalidate screen baselines

File CRC inventories describe the rebuilt programs. They do not replace VRAM screen baselines.
Every existing `entries[].expected_crcs` value remains unchanged in this refresh.
PR #91 moves locals to static storage and does not change intended screen output.
PR #86 changes desktop header declarations and adds no intended CE rendering change.
PRs #76, #79, and #80 change result presentation, numeric formatting, or home key behavior.

The following suites need real-ROM capture and review on the self-hosted lane.
Only replace baselines where replay confirms an intended pixel change.

| Suite | Expected baseline impact |
| --- | --- |
| `autotest.comprehensive` | Result, numeric, menu, and navigation checkpoints can shift. Boot checkpoints may remain stable. |
| `autotest.artifice-smoke` | CalcEngine result and navigation checkpoints can shift. Removing programs can change the TI-OS program menu. The arTIfiCE shell itself is unchanged. |
| `autotest.enter-path-proof` | Result dismissal and repeated Enter checkpoints can shift after the overlay removal. |
| `autotest.keypad-fidelity` | Home key handling and post-result navigation can shift. Unaffected typing checkpoints may remain stable. |
| `autotest.lcd-fidelity` | Result screens and numeric text can shift. Unaffected input checkpoints may remain stable. |
| `autotest.clean-exit` | The result screen and return-to-input path can shift. TI-OS home may remain stable. |
| `autotest.module-coverage` | The 31 existing zero placeholders need first capture. Probe IDs remain stable, with retired labels corrected to current workers. |
| `outputs/autotest.cemu-e2e` | Historical result and prompt checkpoints can shift. Replay before using this archived catalog as current evidence. |

Capture the six existing suite baselines and the module-coverage placeholders using the corresponding real-ROM baseline tools.
Revalidate the archived CEmu end-to-end suite if it is still consumed.
Then enable `run_release_candidate_gate` in `.github/workflows/test.yml` on the self-hosted Windows CEmu runner.
The gate verifies confidence and does not automatically recapture every suite.
This WSL host has no usable headless CEmu build, so capture remains pending.
