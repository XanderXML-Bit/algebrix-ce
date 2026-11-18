# Third-party notices

| File | Author | Licence |
|---|---|---|
| `tools/emulator/cemu-headless/vendor/stb_image_write.h` | Sean Barrett and contributors | MIT or public domain (Unlicense), at your choice. The full text is at the end of the file. |
| `g_graphx_font` table in `tools/emulator/cemu-headless/main.c` | The 8×8 default font of the GRAPHX library from the [CE-Programming toolchain](https://github.com/CE-Programming/toolchain) | LGPL-3.0, the toolchain's licence. The table is used only to read text off emulator screenshots. The licence texts are in [`LICENSES/LGPL-3.0.txt`](LICENSES/LGPL-3.0.txt) and [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt). |
| `tools/emulator/linux-realrom/cemu-headless-debug.patch` | A patch against [CEmu](https://github.com/CE-Programming/CEmu), which contains CEmu context lines | GPL-3.0, CEmu's licence. The text is in [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt). |

Not included, and needed only for calculator or emulator runs:

- the CE C/C++ toolchain and its runtime libraries (LibLoad, GRAPHX, FILEIOC, KEYPADC), available from the CE-Programming project;
- CEmu, the TI-84 Plus CE emulator, available from the CE-Programming project;
- a TI-84 Plus CE ROM image, which is copyrighted by Texas Instruments and must be dumped from a calculator you own.

TI-84 Plus CE and TI-OS are trademarks of Texas Instruments. This project is not affiliated with or endorsed by Texas Instruments.
