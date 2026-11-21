"""Build and run tests/harness/launcher_view_harness.c (needs a host C compiler).

The harness runs the real CALCENG launcher main() (src/core/launcher/main.c)
against a recorded 10x26 home screen and a scripted keypad. Its os_RunPrgm
mock plays the worker the CE way (CE_RESP + CE_STATE, then the launcher's
re-entry callback). The harness checks the full-text result viewer: the home
hint, the wrapped viewer page, scrolling and paging, errors, steps, and a parse
error after an answer (the typed input stays, "Syntax error" shows under it until
the next key, the cursor
goes to the unknown character or word, and a persisted history/error pair
records the failed submission without changing Ans),
the newest of two answers,
the desktop synchronous path, an empty and a cleared history, the
"Parsed:" echo, a launch failure, a silent child, Module missing, a
relaunch with saved history and the 16-slot history ring wrap.

Launcher-side dispatch and recovery failures (IPC write and
state-save failures via fileioc_mock_fail_writes, Module missing, a launch
failure, a silent child and an invalid response) must each persist their
readable error in their own history slot before the launcher exits, survive a
following submission and quitting/relaunching, leave Ans and settings alone,
and never let the viewer resurrect an older full answer after a full ring wrap.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "src" / "core" / "launcher" / "main.c"
CC = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")


def _launcher_cflags() -> list[str]:
    """PROGRAM_CFLAGS of split/Makefile.launcher, so the harness builds the
    launcher with the same feature flags as CALCENG.8xp."""
    text = (ROOT / "split" / "Makefile.launcher").read_text(encoding="utf-8")
    m = re.search(r"^PROGRAM_CFLAGS\s*=\s*(.*)$", text, re.M)
    assert m, "PROGRAM_CFLAGS missing from split/Makefile.launcher"
    return m.group(1).split()


FLAGS = ["-std=gnu99", "-O0", "-g", f"-I{ROOT / 'src'}",
         f"-I{ROOT / 'tests/harness/launcher_mock'}", f"-I{ROOT / 'tests/mock'}",
         "-DDESKTOP_BUILD", "-DCE_DISABLE_STEP_RECORDING=1", *_launcher_cflags()]
SOURCES = ["tests/harness/launcher_view_harness.c", "src/split/runtime_globals.c",
           "src/split/ipc.c", "src/split/astwire.c", "src/split/classify.c",
           "src/memory.c", "src/token.c", "src/ast.c", "src/parser.c",
           "src/split/module_runtime.c", "tests/mock/fileioc_mock.c"]


@pytest.mark.skipif(not CC or os.name == "nt", reason="needs a POSIX host C compiler")
def test_launcher_result_viewer_harness(tmp_path: Path) -> None:
    def cc(*args: str) -> None:
        subprocess.run([CC, *FLAGS, *args], check=True, cwd=ROOT, timeout=300)

    # The harness owns main(); the launcher's main() becomes launcher_main()
    # and its re-entry callback still calls it.
    cc("-Dmain=launcher_main", "-c", "src/core/launcher/main.c", "-o", str(tmp_path / "main.o"))
    exe = tmp_path / "launcher_view_harness"
    cc(*SOURCES, str(tmp_path / "main.o"), "-lm", "-o", str(exe))
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60,
                       encoding="latin-1")  # TI font bytes (0xCE ellipsis)
    assert r.returncode == 0 and "PASSED: 0 failure(s)" in r.stdout, r.stdout + r.stderr
    assert r.stdout.count("ok: ") >= 230, r.stdout


def test_viewer_keys_documented_and_wired() -> None:
    text = LAUNCHER.read_text(encoding="utf-8")
    up = re.search(r"case sk_Up:(.*?)return 0;", text[text.index("static int8_t process_key"):], re.S)
    assert up and "show_result_view();" in up.group(1)
    assert "input_load_history" not in up.group(1)
    assert "UP=full result" in (ROOT / "src/core/launcher/result_view.h").read_text(encoding="utf-8")
    # Every path that stores a fresh g_response answer marks the viewer.
    assert text.count("view_mark_response();") == 2
