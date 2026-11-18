# Algebrix CE

A computer algebra system for the TI-84 Plus CE graphing calculator, written in C99.

Algebrix CE was first developed as CalcEngine. The on-calculator program is still named `CALCENG`, and many source files, scripts and documents still use the old name.

![Language](https://img.shields.io/badge/language-C99-informational)
![Platform](https://img.shields.io/badge/platform-TI--84%20Plus%20CE-orange)
![Status](https://img.shields.io/badge/status-pre--release-yellow)
![License](https://img.shields.io/badge/license-all%20rights%20reserved-lightgrey)

## What it is

You type an expression such as `d/dx(x^2*sin(x))` or `int(1/(x*(x+1)),x)`. Algebrix CE works out which kind of problem it is, solves it symbolically, and can show the steps.

The TI-84 Plus CE is a tight target:

- an eZ80 processor with no floating-point hardware;
- about 256 KB of RAM and a 51,200-byte limit per program module;
- a 320×240 screen and the calculator keypad as the only input.

The engine therefore uses no dynamic allocation at all. Expressions live in a fixed pool of 512 syntax-tree nodes. The program is split into a launcher and 34 worker modules that the launcher swaps in from archive memory as needed.

## What it covers

| Area | Examples |
|---|---|
| Derivatives | power, product, quotient and chain rules; trigonometric, hyperbolic, exponential and logarithmic functions; implicit and higher-order derivatives; tangent lines |
| Integrals | u-substitution, integration by parts, trigonometric substitution, partial fractions, definite and improper integrals, Simpson and trapezoidal rules |
| Limits | substitution, factoring, rationalisation, L'Hôpital's rule, one-sided limits and limits at infinity |
| Series | convergence tests, Taylor and Maclaurin series, radius and interval of convergence |
| Differential equations | separable and first-order linear equations, second-order linear equations with constant coefficients, Euler's method |
| Algebra | equation and polynomial solving, polynomial division, factoring |
| Matrices | arithmetic, determinant, inverse, row reduction, eigenvalues up to 3×3 |
| Statistics and probability | descriptive statistics, regressions, hypothesis tests, common distributions |
| Other | complex numbers, lists, finance (time value of money), graphing, stored variables |

Coverage is not the same as correctness on every input. The next section says what is and is not verified.

## Current status

- **Desktop build:** the full C test harness builds and passes on Linux with GCC. See "Run the tests".
- **Emulator:** the program is tested on the CEmu emulator with a real OS 5.8 ROM through `tools/emulator/linux-realrom`. That battery still has **known failures**. They are recorded in `tests/cemu/edge-cases/module-battery.json`, and a release is blocked until they are fixed.
- **Real calculator:** no current build is certified on physical hardware.

Some older documents in `docs/` describe an earlier emulator lane that drove the TI-OS home screen rather than Algebrix CE. Those results are historical and are not evidence for the current build. Where a document carries that warning, trust the warning.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the engine: tokenizer, Pratt parser, syntax tree, evaluator, simplifier and each solver |
| `src/core/launcher/` | the on-calculator launcher, home screen and result viewer |
| `src/modules/`, `src/split/` | worker modules and the protocol the launcher uses to call them |
| `split/` | one makefile per worker module for the calculator build |
| `tests/` | the desktop C harness (`test_main.c`), focused unit harnesses, mocks of the TI libraries, Vitest suites and the Python CEmu tests |
| `tools/emulator/` | emulator automation, including the headless CEmu wrapper and the real-ROM runner |
| `tools/desktop/` | a desktop command-line solver |
| `scripts/` | test runners and emulator helpers |
| `docs/` | design notes, audits, the user manual and the toolchain guide |
| `examples/` | small examples of the memory pool and simplifier patterns |

## Run the tests

You need GCC (or Clang), GNU Make and the C maths library. On Debian or Ubuntu, `sudo apt install build-essential` is enough.

```bash
cd tests
make run
```

`make run` builds and runs the main harness and the focused harnesses for formatting, floating-point classification, tiny values and the split modules. Each prints a pass and fail count, and the command exits non-zero on any failure.

An AddressSanitizer build is also available:

```bash
cd tests
make run-asan
```

The Vitest suites (`npm ci`, then `npm test`) need Node.js 22. The Python CEmu tests need Python 3 with `pip install pytest pytest-timeout pyyaml`, then `python -m pytest tests/cemu -q`. Most CEmu tests skip themselves unless you supply an emulator binary, a ROM and built modules, as described below.

## Build for the calculator

Install the [CE C/C++ Toolchain](https://ce-programming.github.io/toolchain/) (CEdev). `docs/TOOLCHAIN.md` walks through the setup.

```bash
make
```

The default target builds the `CALCENG` launcher and every worker module from the makefiles in `split/`, then checks each module's stack use. Each module must stay at or below 51,200 bytes.

To run the program you also need the CE C libraries (LibLoad, GRAPHX, FILEIOC and KEYPADC). They are published with the CE toolchain and are not included here. Calculators on OS 5.5 or later block unsigned assembly programs. Running Algebrix CE on them needs a third-party jailbreak, which is also not included.

## Test on the emulator

The emulator lanes need a TI-84 Plus CE ROM image. TI's ROM is copyrighted, so no ROM is included. `docs/ROM-ACQUISITION.md` explains how to dump the ROM from a calculator you own.

`tools/emulator/linux-realrom/README.md` describes the current real-ROM runner. Several older PowerShell scripts in `tools/emulator/` assume a Windows host and take their paths as parameters.

## Design notes

- **Static memory only.** There is no `malloc` or `free` anywhere. Nodes come from a 512-node pool with a free stack, so allocation and release are O(1). A bitmap guards against double frees.
- **Bounded stack.** The eZ80 stack is small, so recursion depth is limited and measured per worker module. `docs/CE-WORKER-STACK-BOUNDS.md` has the measurements.
- **Soft floating point.** The calculator's `float` is 32-bit and implemented in software. `src/ce_float_ops.h` and `src/ce_float_class.h` work around rounding and overflow behaviour measured on the real OS 5.8 ROM.
- **Module routing.** The launcher classifies each input and sends it to the worker that can answer it. `docs/MULTI-PROGRAM-ARCH.md` describes the protocol.

## Documentation

| Document | Topic |
|---|---|
| [`docs/USER-MANUAL.md`](docs/USER-MANUAL.md) | using the program on the calculator |
| [`docs/QUICK-REFERENCE.md`](docs/QUICK-REFERENCE.md) | keys and input notation |
| [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md) | setting up CEdev |
| [`docs/MULTI-PROGRAM-ARCH.md`](docs/MULTI-PROGRAM-ARCH.md) | the launcher and worker modules |
| [`docs/CE-WORKER-STACK-BOUNDS.md`](docs/CE-WORKER-STACK-BOUNDS.md) | stack measurements |
| [`docs/FINITE-SUM-ACCURACY.md`](docs/FINITE-SUM-ACCURACY.md) | accuracy of finite sums |
| [`docs/V2_HARDWARE_BUGS.md`](docs/V2_HARDWARE_BUGS.md) | hardware and emulator defects found so far |
| [`docs/ROM-ACQUISITION.md`](docs/ROM-ACQUISITION.md) | dumping a ROM for emulator use |
| [`CHANGELOG.md`](CHANGELOG.md) | change history |

## Third-party code

Three third-party pieces keep their own licences. They are listed in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md), and the licence texts are in `LICENSES/`.

- `tools/emulator/cemu-headless/vendor/stb_image_write.h`, by Sean Barrett: MIT or public domain.
- The GRAPHX font table in `tools/emulator/cemu-headless/main.c`, from the CE-Programming toolchain: LGPL-3.0.
- `tools/emulator/linux-realrom/cemu-headless-debug.patch`, a patch against CEmu: GPL-3.0.

TI-84 Plus CE and TI-OS are trademarks of Texas Instruments. This project is not affiliated with or endorsed by Texas Instruments.

## License

Copyright © 2026 Xander M. Lopez (Zendren). All rights reserved. The source is published to read. It is not licensed for reuse or redistribution. See [`LICENSE`](LICENSE). The third-party pieces above are excepted and keep their own licences.

Built by [Xander M. Lopez](https://xandermlopez.com) · [Zendren](https://zendren.net)
