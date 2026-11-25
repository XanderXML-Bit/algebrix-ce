# Algebrix CE documentation

## Install and use the calculator program

- [Installation](INSTALLATION.md) covers transfer files, calculator OS requirements, and how to launch the program.
- [User manual](USER-MANUAL.md) explains input, result steps, settings, and solver modes.
- [Quick reference](QUICK-REFERENCE.md) lists keypad shortcuts and expression syntax.

## Build and understand the engine

- [CE toolchain setup](TOOLCHAIN.md) explains how to build the launcher and workers.
- [Multi-program architecture](MULTI-PROGRAM-ARCH.md) describes worker selection and the inter-program protocol.
- [Enter path](ENTER-PATH.md) traces an expression from the keypad to the result.
- [Worker stack bounds](CE-WORKER-STACK-BOUNDS.md) records stack limits and measurements.
- [Finite-sum accuracy](FINITE-SUM-ACCURACY.md) describes where numerical error can accumulate.
- [State replacement](STATE-REPLACEMENT.md) explains how the save path handles write failures.
- The [operator dispatch audit](OP-DISPATCH-PARITY-AUDIT.md), [result display audit](AST-DISPLAY-AUDIT.md), and [text encoding audit](CE-TEXT-ENCODING-AUDIT.md) document discrepancies and their checks.

## Test and validate

Start with the [current status](../README.md#current-status) and the [real-ROM runner](../tools/emulator/linux-realrom/README.md). Older reports carry their own dates and validation limits.

- [ROM acquisition](ROM-ACQUISITION.md) explains how to dump a ROM from your calculator.
- [Validation coverage](VALIDATION-COVERAGE.md) and the [automated test pipeline](AUTOMATED-TEST-PIPELINE.md) describe how tests exercise the engine.
- [Emulator integration](EMULATOR-INTEGRATION.md) documents the headless tools and the pre-hardware validation gate.
- The [CEmu pipeline](cemu-pipeline.md) and [arTIfiCE pipeline](cemu-artifice-pipeline.md) document earlier emulator workflows.
- [Emulator parity](EMULATOR-PARITY.md) and the [hardware gap ledger](emulator-hardware-gap-ledger.md) track differences between the emulator and hardware.
- The [LCD](emulator-lcd-fidelity.md), [keypad](emulator-keypad-fidelity.md), and [Enter path](emulator-enter-path-proof.md) reports record checks on those emulator paths.
- [Hardware deployment](HARDWARE-DEPLOYMENT.md), the [hardware test checklist](HARDWARE-TEST-CHECKLIST.md), and the [OS version test matrix](OS-VERSION-TEST-MATRIX.md) cover device setup and validation.
- [Hardware parity checklist](HARDWARE-PARITY-CHECKLIST.md) tracks tests that need a calculator.
- [v2.0.0 hardware report](V2_HARDWARE_BUGS.md) records defects from the 2026-04-14 test.
