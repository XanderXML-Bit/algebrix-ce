# CE C/C++ Toolchain — Setup & Development Guide

## Prerequisites

- TI-84 Plus CE calculator
- USB cable (mini-USB)
- Windows/Linux/macOS computer
- Git installed

## Setup

### 1. Install CE Toolchain
- Download latest release from: https://ce-programming.github.io/toolchain/
- Extract to a folder (e.g., `C:\ce-toolchain` on Windows)
- Add `bin` folder to system PATH

### 2. Verify Installation
```bash
# Should show toolchain version
make --version
```

### 3. Create Project from Template
```bash
# Clone the toolchain examples
git clone https://github.com/CE-Programming/toolchain.git
# Copy a template project
cp -r toolchain/examples/hello_world CalcEngine
```

### 4. Project Makefile
```makefile
NAME = CALCENG
DESCRIPTION = "CalcEngine Math Suite"
COMPRESSED = NO
ARCHIVED = YES
include $(CEDEV)/meta/makefile.mk
```

### 5. Build & Transfer
```bash
make          # Produces bin/CALCENG.8xp (+ .8xv chunks if oversized)
# Transfer ALL produced files via TI Connect CE
```

If the linked program exceeds one TI variable page (~65,512 bytes), the
toolchain emits a small launcher `.8xp` plus numbered AppVar chunks
(`.8xp.0.8xv`, `.8xp.1.8xv`, ...). The launcher requires all chunks to exist.

## Key Libraries

| Library | Include | Purpose |
|---------|---------|---------|
| `graphx` | `<graphx.h>` | 2D graphics, sprites, text |
| `keypadc` | `<keypadc.h>` | Direct keypad scanning |
| `fileioc` | `<fileioc.h>` | AppVar read/write |
| `fontlibc` | `<fontlibc.h>` | Custom font rendering |

## Screen Specs

- Resolution: 320×240 pixels
- Color: RGB565 (16-bit, 65,536 colors)
- Text: 26 columns × 10 rows (default font)
- Graphics: Full pixel-level control via graphx

## Memory Limits

- **RAM:** ~50KB usable (after OS overhead)
- **Flash:** Up to ~4MB for app storage
- **Stack:** ~4KB default
- Strategy: Static allocation, expression node pool, display buffers

## Calculator OS Compatibility

| OS Version | C Programs | Notes |
|------------|------------|-------|
| ≤ 5.4 | ✅ Native | Works directly |
| 5.5 - 5.6 | ⚠️ Jailbreak | Needs arTIfiCE |
| 5.7+ | ⚠️ Jailbreak | Needs arTIfiCE |

### arTIfiCE Jailbreak
If OS is 5.5+, re-enable C program execution:
1. Download arTIfiCE from https://yatis.music.free.fr/
2. Transfer to calculator via TI Connect CE
3. Run once to unlock — persists until OS update

## Development Workflow

1. Edit C source in VS Code (with eZ80 extension)
2. `make` to compile
3. Transfer .8xp to calculator via USB
4. Test on hardware
5. Debug with `os_PutStrFull()` for print debugging

## Useful Resources

- **Official Docs:** https://ce-programming.github.io/toolchain/
- **API Reference:** https://ce-programming.github.io/toolchain/libraries.html
- **Examples:** https://github.com/CE-Programming/toolchain/tree/master/examples
- **Community:** https://www.cemetech.net/
- **CEmu Emulator:** https://ce-programming.github.io/CEmu/ (test without hardware)
