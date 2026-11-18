"""Build and run tests/harness/ceistr_reentry_harness.c (needs a host C compiler).

The harness runs CEISTR's real main() (src/modules/calc_int/report_main.c)
under an os_RunPrgm mock that behaves like the TI-84 Plus CE: it never returns
on a successful launch, zeroes CEISTR's statics before the callback re-enters
main(), and returns non-zero on a launch failure. It covers the normal path, a
child with no response, a launch failure, a missing strategy program, a stale
CEISTRS scratch AppVar, an unexpected child, a re-entry with nothing pending,
a re-entry after CE_STATE was lost, a definite integral (an internal review,
PR #97 reviews), and children whose failure messages are long: the trig-sub
and u-sub workers' real messages must arrive whole, and 127-char errors must
be clipped at a word with "..." while the ranking still fits CE_RESP (card
an internal review).
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")

DEFINES = ["-DDESKTOP_BUILD", "-DHW_BUILD_SPLIT", "-DCE_DISABLE_STEP_RECORDING=1",
           "-DCE_AST_ENABLE_STAT_NAMES=0"]
FLAGS = ["-std=c99", "-O0", "-g", f"-I{ROOT / 'src'}", f"-I{ROOT / 'tests/mock'}", *DEFINES]
SOURCES = ["tests/harness/ceistr_reentry_harness.c", "src/split/module_runtime.c",
           "src/split/ipc.c", "src/split/astwire.c", "src/memory.c", "src/ast.c",
           "src/split/runtime_globals.c", "tests/mock/fileioc_mock.c"]


@pytest.mark.skipif(not CC or os.name == "nt", reason="needs a POSIX host C compiler")
def test_ceistr_main_survives_ce_style_reentry(tmp_path: Path) -> None:
    def cc(*args: str) -> None:
        subprocess.run([CC, *FLAGS, *args], check=True, cwd=ROOT, timeout=300)

    # report_main.c and module_forward.c both define/call main(); rename it so
    # the harness can own main() while module_reentry() still re-enters CEISTR.
    cc("-DCE_ISTR_HARNESS", "-Dmain=ceistr_main", "-c", "src/modules/calc_int/report_main.c",
       "-o", str(tmp_path / "report_main.o"))
    cc("-Dmain=ceistr_main", "-c", "src/split/module_forward.c",
       "-o", str(tmp_path / "module_forward.o"))
    exe = tmp_path / "ceistr_harness"
    cc(*SOURCES, str(tmp_path / "report_main.o"), str(tmp_path / "module_forward.o"),
       "-lm", "-o", str(exe))
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and "PASSED: 0 failure(s)" in r.stdout, r.stdout + r.stderr
    assert r.stdout.count("ok: ") >= 47, r.stdout
