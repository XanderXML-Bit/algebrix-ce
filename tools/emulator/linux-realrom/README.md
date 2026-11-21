# Linux real-ROM lane (upstream CEmu `cemu-headless`)

This lane runs the built `.8xp` set on a real TI-84 Plus CE OS image inside
CEmu on Linux/WSL, with no sudo, cmake or Qt. It is emulator evidence only.
It does not verify hardware.

**ROM policy:** never download or commit a ROM. Point `ALGEBRIX_ROM` at a ROM
dumped from a calculator you own. arTIfiCE 2.1 supports OS ≤ 5.8.4.

**Files you supply:** `deploy` sends the built modules from `bin/` plus five
files that are not in this repository. Put them in `deploy/hardware-ready/`
first:

- `arTIfiCE.8xp`, from the [arTIfiCE releases](https://github.com/YvanTT/arTIfiCE/releases);
- `fileioc.8xv`, `graphx.8xv`, `keypadc.8xv` and `libload.8xv`, from the
  CE C libraries published with the
  [CE toolchain releases](https://github.com/CE-Programming/toolchain/releases).

## Build the emulator

```sh
git clone https://github.com/CE-Programming/CEmu cemu
cd cemu
git checkout 430510c539cce0886b60fd51c8ff1b8d0709c2f9   # tested commit
# Optional debug additions (peek/fill/regs/bp commands, [HOP] program-entry
# log, stack dump on stack-limit NMI). These are needed for `realrom.py hops`
# and tests/cemu/test_realrom_module_handoff.py.
git submodule update --init core/debug/zdis
git apply /path/to/Algebrix/tools/emulator/linux-realrom/cemu-headless-debug.patch
make -C tests/autotester -j8 cemu-headless DEBUG_SUPPORT=1 \
  CXXFLAGS="-std=c++11 -O2 -Wno-shadow -DGLOB_SUPPORT -DDEBUG_SUPPORT"
export CEMU_HEADLESS=$PWD/tests/autotester/cemu-headless
```

The patched `gui_debug_open` logs every exec breakpoint as `[HOP]` and then
continues, so this binary is a tracing tool. It is not suitable for general
interactive CEmu debugging.

Stock gcc/g++/make/zlib are enough. For a plain (non-debug) build, run
`make -C tests/autotester -j8 cemu-headless`. Upstream `-Werror=shadow`
trips on `debug.h` only when `DEBUG_SUPPORT=1`, which is why the CXXFLAGS
override is there.

## Build Algebrix on Linux

```sh
gh release download v11.2 -R CE-Programming/toolchain -p CEdev-Linux.tar.gz
tar xzf CEdev-Linux.tar.gz
export CEDEV=$PWD/CEdev PATH=$PWD/CEdev/bin:$PATH
make -j8 all STACK_CHECKER="python3 tools/check_stack.py"
```

## Drive it

```sh
export ALGEBRIX_ROM=/path/to/owned/Rom.rom
python3 tools/emulator/linux-realrom/realrom.py deploy . /tmp/rr     # ~20 s; saves /tmp/rr/launched.ce
python3 tools/emulator/linux-realrom/realrom.py cases /tmp/rr/launched.ce /tmp/rr/cases "int(x^2,x)" "lim(x^2,x,2)"
python3 tools/emulator/linux-realrom/realrom.py hops  /tmp/rr/launched.ce "int(x^2,x)" "solve(x+2,x)"
ALGEBRIX_ROM=... CEMU_HEADLESS=... python3 -m pytest tests/cemu/test_realrom_module_handoff.py
```

`battery` replays `tests/cemu/edge-cases/module-battery.json` (or `--cases FILE`).
Each case runs in a fresh emulator. It records the screen CRC, computed as the
CEmu autotester `hash` step does it (CRC-32C over `vram_start` / `vram_16_size`),
plus a BMP. `ALGEBRIX_ROM=... CEMU_HEADLESS=... python3 -m pytest
tests/cemu/test_realrom_module_battery.py` checks the committed baselines.

```sh
python3 tools/emulator/linux-realrom/realrom.py battery /tmp/rr/launched.ce /tmp/rr/battery
```

Rebaseline a case only after you inspect its BMP. If the screen is wrong, mark
the case `known_failure` with a card id and leave `expected_crcs` empty.
`tests/cemu/test_module_battery_catalog.py` enforces the structure and rejects known bad captures. It cannot prove new pixels or mathematical correctness: that requires inspection and review.

The older autotester-wrapper lane (`tools/emulator/cemu-headless` in wrapper
mode, plus `autotester`) launches with `Asm(prgmCALCENG`. OS 5.5+ blocks that,
so the lane types every case on the TI-OS home screen, and its CRCs hash TI-OS
screens.

`cases` writes one 320×240 BMP per expression. Add `--then trace,down,clear`
to press keys after the result and save `cNN-kK.bmp` after each one; TRACE
opens the launcher's result viewer (result, then steps). Inspect the pixels: a passing
exit code or a matching hash proves nothing about what the screen shows.
`result STATE PREFIX EXPR...` reads multi-line results the home screen cannot
show on its one result row. On the device, UP opens the launcher's result
viewer; `result` reads the same whole CE_RESP from the
launcher's static `g_response` without pressing keys. `result` resolves `_g_response` from
`--map` (default `bin/launcher/CALCENG.map`, which must match the deployed
CALCENG), clears its magic before each expression, and prints the text only
when a fresh, valid response starts with PREFIX. For example:
`realrom.py result /tmp/rr/launched.ce "Best: " "intstrategy(x*sin(x),x)"`.

`hops` prints every program entry (at `userMem`, 0xD1A881) with the stack
pointer. Each evaluation must return to `CALCENG` at the same SP.

## What it found (2026-11-08, OS 5.8.0.0022)

`int(...)` and `solve(...)` reset the calculator. The CEmu log showed
`NMI reset caused by writing to the stack limit at address 0xd19881`, and
the stack was full of CEdev `os_RunPrgm` stub frames:

- Each `os_RunPrgm` call parks a 209-byte stub on the 4 KB OS stack. The
  stub is released only when the launched program *returns*. Workers used to
  relaunch their parent with `os_RunPrgm(return_to, NULL, 0, NULL)`, which
  never returns, so every hop leaked 209 bytes. A single-module call such as
  `factor` leaked 418 bytes per evaluation.
- A router (`CECINT`, `CEALG`) forwards with `return_to = itself`. The child
  relaunched the router, and the router's `resume_forward` then re-dispatched
  to itself. That loop ran 21 times until the stack limit tripped.

The fix: parents launch children with a re-entry callback, workers return
from `main()`, and the callback marks re-entry so nested forwarders resume
correctly. `tests/cemu/test_module_handoff_contract.py` guards this
statically.

## Fail-closed downstream gates

Known-failure cases have no passing CRC. The Python hardening, visible-smoke and
release-candidate gates therefore fail when selecting them. TypeScript screen
assertions also fail when neither a CRC nor nonempty text expectations are
available. The gate currently selects the untypeable matrix case, so it is
blocked until that case is fixed. Do not switch it to an unrelated green case
or restore a TI-OS CRC to make it pass. The other arithmetic/edge/visible-smoke
catalogs remain untrusted because the wrapper captured the TI-OS home screen.
