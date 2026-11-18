# TI-84 Plus CE OS Version Test Matrix

Last updated: 2026-02-19

This document tracks cross-version compatibility validation for CalcEngine `.8xp` builds.

## OS Popularity Snapshot (Research-Based)

TI does not publish installed-base telemetry by OS version, so prevalence below is inferred from TI's current OS distribution page and community jailbreak usage/docs.

| OS family | Likely prevalence (2026) | Why |
|---|---|---|
| 5.7+ (especially 5.8.x) | High | TI's current downloadable OS for TI-84 Plus CE is now in the 5.8.x line (latest listed: 5.8.4, Sep 2025), so new units and updated units concentrate here. |
| 5.5-5.6 | Medium | Large school fleets often lag updates; these are common "legacy but modern" versions in jailbreak/community support docs. |
| 5.3-5.4 | Low | Older baseline mostly on long-unupdated hardware. |

## Expected Behavior Matrix

| OS version | ROM file | Stock launch expectation (no jailbreak) | Launch expectation with arTIfiCE v2 | Emulator smoke | Physical calculator | Notes |
|---|---|---|---|---|---|---|
| 5.3 | `ti84ce-os53.rom` | Expected to run without arTIfiCE on this era of OS | Expected to run | Pending | Pending | arTIfiCE docs say it is only needed on 5.5.1+. Community reports note 5.3.1 blocked typing the Asm token manually. |
| 5.5 | `ti84ce-os55.rom` | Expected blocked (native ASM/C execution removed in 5.5.1) | Expected to run | Pending | Pending | Transition point where jailbreak becomes required. |
| 5.6 | `ti84ce-os56.rom` | Expected blocked | Expected to run | Pending | Pending | Same execution model as 5.5+; verify no regressions in launch/startup latency. |
| 5.7+ | `ti84ce-os57.rom` | Expected blocked | Expected to run with arTIfiCE v2 | Pending | Pending | arTIfiCE v1 exploit is incompatible on 5.8.3+; use arTIfiCE v2. |

## Known Incompatibilities and Constraints

- TI has progressively restricted native ASM program execution in CE OS updates:
  - 5.3.1: manual Asm token entry blocked (community report).
  - 5.5.1: ability to run assembly programs removed without jailbreak.
- arTIfiCE v2 is the currently relevant jailbreak path for modern OS lines (supports OS 5.3+, needed on 5.5.1+; v1 exploit path no longer works on 5.8.3+).
- CE toolchain docs call out an interrupt-library incompatibility on newer hardware revisions (`intce` vectors on hardware rev I+). This is not a CalcEngine blocker today (CalcEngine does not depend on `intce`), but it is relevant for future low-level timing/input experiments.
- Physical validation is still required because emulator behavior can differ from real hardware timing/input behavior.

## Execution Plan

1. Acquire one ROM per OS version family (see `docs/ROM-ACQUISITION.md`).
2. Run smoke autotest for each ROM:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 -RomPath "C:\ti-roms\ti84ce-os53.rom" -AutotesterPath "C:\tools\cemu\autotester.exe"
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 -RomPath "C:\ti-roms\ti84ce-os55.rom" -AutotesterPath "C:\tools\cemu\autotester.exe"
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 -RomPath "C:\ti-roms\ti84ce-os56.rom" -AutotesterPath "C:\tools\cemu\autotester.exe"
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 -RomPath "C:\ti-roms\ti84ce-os57.rom" -AutotesterPath "C:\tools\cemu\autotester.exe"
```

3. For each OS family, test two paths:
   - Stock OS (no jailbreak): verify expected fail/pass behavior.
   - Jailbroken path (arTIfiCE v2 where applicable): verify launch and core math smoke operations.
4. Record pass/fail and behavioral deltas directly in this table.
5. Confirm on physical hardware for at least one device per OS family.

## Blocking Inputs

- ROM images are not in repository (by design).
- Physical devices for each target OS are required for final confirmation.

## Sources

- TI OS download index (latest TI-84 Plus CE OS 5.8.4 listing): https://education.ti.com/en/software/search/ti-84-plus-ce-os
- arTIfiCE FAQ / changelog (OS compatibility and 5.8.3+ exploit change): https://yvantt.github.io/arTIfiCE/
- Cemetech news summary of TI OS hardening (5.3.1 and 5.5.1 changes): https://www.cemetech.net/news/2023/6/971/_/new-arTIfiCE-jailbreak-tool-released-for-ti-84-plus-ce-with-os-58-r971
- CE toolchain library docs (`intce` hardware revision caveat): https://ce-programming.github.io/toolchain/libraries.html
