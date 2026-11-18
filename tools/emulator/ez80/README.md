# eZ80 Emulator Scaffold (Pass 2)

This folder is the C-facing scaffold for the custom TI-84 CE emulator lane.
It now includes executable stubs for a minimal instruction subset and memory model
while preserving stable interfaces for future full emulation passes.

Current scaffold capabilities:

- emulator lifecycle (`init`, `reset`, ROM load, single-step, bounded run)
- headless native CLI (`tools/emulator/ez80/ez80_cli.exe`) with `--interactive`, `--screen-state`,
  `--screenshot-png`, `--list-programs`, `--home-text`, and JSON/text output modes
- agent protocol for interactive automation, including `step`, `get_screen_state`, `write_lcd_png`,
  `load_8xp`, `list_programs`, memory/port inspection, snapshots, and trace controls
- ROM metadata probing contract with deterministic size/CRC/header metadata
- instruction decode/execute contract with expanded opcode subset:
  `NOP`, `HALT`, `LD` family (`LD r,r`, `LD r,n`, `LD rr,nn`, memory-indirect loads/stores),
  `INC/DEC` (8-bit + 16-bit pairs), full 8-bit ALU block (`ADD/ADC/SUB/SBC/AND/XOR/OR/CP`
  with register/immediate/indirect forms), `ADD/ADC/SBC` 16-bit forms (`HL` pair variants),
  `CB` prefixed rotate/shift/bit/set/res operations, `DD/FD` indexed (`IX/IY`) memory/register ops,
  `DD/FD CB` indexed rotate/shift/bit/set/res forms, `ED` prefixed extended ops (`NEG`, `LD I,A`,
  `LD A,I`, `LDI`, `LDD`, `LDIR`, `LDDR`, `CPI`, `CPD`, `CPIR`, `CPDR`, `INI`, `IND`, `INIR`,
  `INDR`, `OUTI`, `OUTD`, `OUTIR`, `OUTDR`, `RETN`, `RETI`), exchange/loop control
  (`EX AF,AF'`, `EXX`, `EX DE,HL`, `EX (SP),HL`, `EX (SP),IX`, `EX (SP),IY`, `DJNZ e`),
  `JR e`, `JP nn`, `JP (HL)`, `CALL nn`, `RET`, `PUSH/POP qq`, `IN A,(n)`, `OUT (n),A`
- memory model contract (`flash` window, `RAM`, `ports`, and `MMIO` stub address spaces)
- register-state mutation during step/run for supported instructions
- deterministic MMIO trace registers for latest IN/OUT activity
- deterministic DNGRADE simulation lane with explicit strategy modeling (direct write, unlock+write, cert-unlock+write),
  timeout/watchdog no-hang cutoffs, and MMIO telemetry for last-attempt status/failure/cycle outcomes

Current files:

- `include/ez80_emulator.h`: core emulator state and lifecycle API
- `include/ez80_rom.h`: ROM metadata/probe API
- `include/ez80_decode.h`: decode API and decoded-instruction shape
- `src/ez80_emulator_stub.c`: argument-validated stubs
- `src/ez80_rom_stub.c`: argument-validated ROM probe stubs
- `src/ez80_decode_stub.c`: decode stubs for initial opcodes
- `run-ez80-scaffold-cli.ps1`: API-first CLI probe for ROM metadata/CRC telemetry
- `tests/ez80.scaffold.contract.json`: expected files/symbols for this scaffold
- `tests/ez80_stub_runtime_tests.c`: executable runtime tests for decode/memory/step contracts
- `tools/emulator/tests/run-ez80-scaffold-runtime-tests.ps1`: MSVC compile+run wrapper for runtime tests

Validation:

```powershell
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-ez80-scaffold-tests.ps1
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-ez80-scaffold-cli-tests.ps1
powershell -ExecutionPolicy Bypass -File tools/emulator/tests/run-ez80-scaffold-runtime-tests.ps1
npx vitest run ez80.cli.test.mjs ez80.runtime.test.mjs
```

Design intent:

- Keep interfaces stable so future implementation passes can replace stub internals
  without breaking callers.
- Keep status/error contracts explicit now, before implementation complexity grows.
- Make ROM loading, decode, and bounded execution testable independently from
  full TI-OS boot support.
- Provide deterministic, machine-parseable ROM telemetry now (size/CRC/header probe)
  so CI and agentic tooling can validate ROM inputs before CPU emulation lands.
