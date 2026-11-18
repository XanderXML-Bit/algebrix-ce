# ROM Acquisition for CEmu

## Requirement

CEmu requires a ROM dump from a physical TI-84 Plus CE family calculator.

## Legal and Policy Notes

- Dump ROM only from hardware you own or are authorized to service.
- Do not commit ROM images to git.
- Keep ROM files in a local, ignored path (for example `C:\ti-roms\`).

## Prerequisites

1. TI-84 Plus CE (or CE-T / CE Python variant)
2. USB cable
3. TI Connect CE installed on desktop
4. ROM dump utility from CEmu wiki instructions

## Process

1. Install TI Connect CE:
- https://education.ti.com/en/products/computer-software/ti-connect-ce-sw

2. Follow CEmu ROM dump instructions:
- https://raw.githubusercontent.com/wiki/CE-Programming/CEmu/Dumping-a-ROM.md

3. Transfer the ROM dump program/appvar to calculator using TI Connect CE.

4. Run the dump program on calculator.
- This can take several minutes.
- Keep cable connected and avoid power interruption.

5. Export resulting dump files back to desktop with TI Connect CE.

6. Configure local emulator scripts with that ROM path:
```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/run-cemu-autotest.ps1 `
  -RomPath "C:\ti-roms\ti84ce.rom" `
  -AutotesterPath "C:\tools\cemu\autotester.exe"
```

## Recommended Storage Layout

```text
C:\ti-roms\
  ti84ce-os53.rom
  ti84ce-os55.rom
  ti84ce-os56.rom
  ti84ce-os57.rom
```

This supports the OS compatibility test matrix in `docs/OS-VERSION-TEST-MATRIX.md`.
