# cemu-headless

**Status:** Wrapper-backed Phase 2 baseline (Pass 70, 2026-03-21). Native `cemu-core` integration is still pending, but the binary now works without it by replaying sessions through `autotester.exe`.

`cemu-headless` is the CalcEngine headless control binary for the TI-84 Plus CE emulator lane. In the current repository state, it communicates via stdin/stdout JSON lines and uses the proven `autotester.exe` path as its execution backend when `cemu-core/` is absent. Once the upstream CEmu core is cloned, the same CLI is intended to swap to native core calls.

## Current State

| Component | Status |
|-----------|--------|
| CMake build scaffolding | Done |
| JSON-line command loop | Done |
| `autotester.exe` wrapper backend | Done |
| Session replay for `load_rom`/`load_app`/`inject_key`/`step` | Done |
| CRC capture via `get_screen` | Done |
| JSON state snapshot export | Done |
| Native `cemu-core` integration | TODO |
| LCD PNG export | TODO |
| ASCII screen extraction | TODO |
| True emulator state restore | TODO |

Without `cemu-core/`, the binary still builds and remains useful: it generates a temporary AutoTester config, launches `autotester.exe`, parses the observed CRC from stdout, and returns structured JSON responses.

## Requirements

| Requirement | Notes |
|-------------|-------|
| CMake >= 3.15 | Build system |
| C compiler (C99) | Required now |
| C++ compiler (C++17) | Required once `cemu-core` is added |
| `autotester.exe` | Required for wrapper mode |
| zlib | Required only for native `cemu-core` builds |
| CEmu source | Clone manually for native mode |
| `stb_image_write.h` | Optional, for future PNG export |

## Adding The CEmu Core

```bash
cd tools/emulator/cemu-headless
git clone https://github.com/CE-Programming/CEmu.git cemu-core
```

You can also override the core path with `-DCEMU_CORE_DIR=/path/to/CEmu`.

## Build

Wrapper mode is now the default when `cemu-core/` is missing.

### Windows

```powershell
cd tools\emulator\cemu-headless
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -B build .
cmake --build build --config Release
```

### Linux / macOS

```bash
cd tools/emulator/cemu-headless
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

## Usage

```text
cemu-headless [--version] [--help] [script.json]
```

Each input line is a JSON object with a `"cmd"` field. Each response is a one-line JSON object with `"ok"` and `"cmd"`.

### Commands

`load_rom`

```json
{"cmd":"load_rom","path":"rom/Rom.rom","autotester_path":"C:/Users/user/Tools/CEmu/v2.0/autotester.exe"}
```

`load_app`

```json
{"cmd":"load_app","path":"C:/Users/user/Downloads/CalcEngine/CALCENG.8xp"}
{"cmd":"load_app","path":"C:/Users/user/Downloads/CalcEngine"}
```

`inject_key`

```json
{"cmd":"inject_key","key":"2"}
{"cmd":"inject_key","key":"enter","duration_ms":2000}
```

`step`

```json
{"cmd":"step","cycles":480000}
```

`get_screen`

```json
{"cmd":"get_screen","format":"crc"}
```

Wrapper-mode note: `format="crc"` works today. `format="ascii"` returns an explicit capability error until native core wiring exists.

`snapshot`

```json
{"cmd":"snapshot","path":"session-state.json"}
```

Wrapper-mode note: non-PNG paths write a JSON state snapshot containing ROM path, transfer queue, replayed keys, pending delays, and the latest observed CRC. `.png` paths return a capability error because AutoTester exposes CRC hashes, not framebuffer pixels.

`get_state`, `reset`, and `exit` return structured session metadata, clear the replay state while preserving queued transfer files, and terminate the process respectively.
