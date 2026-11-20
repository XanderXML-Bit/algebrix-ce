"""Opt-in real-ROM check of the CALCENG full-text result viewer.

Skips unless all of these exist (never in hosted CI; the ROM is never committed):
  ALGEBRIX_ROM       owned TI-84 Plus CE ROM (OS 5.8.x for bundled arTIfiCE 2.1)
  CEMU_HEADLESS      cemu-headless built with tools/emulator/linux-realrom/cemu-headless-debug.patch
  bin/*/*.8xp        a fresh `make all` of this checkout

For intstrategy(x*sin(x),x), int(x*sin(x),x) and 2+3 it reads the launcher's
viewer state from RAM (addresses from bin/launcher/CALCENG.map), checks that
the wrapped line count matches a Python port of the wrap rule in
src/core/launcher/result_view.h applied to the CE_RESP text, and checks that
UP and DOWN change the screen and CLEAR restores the identical home frame.
It also checks a parse error after an answer: the typed
invalid input stays editable, s_status holds a readable error, the viewer
opens the new error rather than the earlier answer, and history persists
the failed submission without changing Ans.
Last, with CEBASE left off the calculator, "Module missing"
is saved in the failed entry's own history slot before CALCENG exits, so it
survives a relaunch and a following answer, with Ans and settings unchanged.
Screen hashes prove only that frames differ or match; inspect the pixels
(realrom.py cases, or tests/harness/launcher_view_harness.c for the text).
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MODULES = sorted(ROOT.glob("bin/*/*.8xp"))
MAP = ROOT / "bin/launcher/CALCENG.map"

pytestmark = pytest.mark.skipif(
    not (ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file() and len(MODULES) >= 35),
    reason="real-ROM lane needs ALGEBRIX_ROM, a debug CEMU_HEADLESS and a built bin/",
)

COLS, MIN_BREAK = 26, 13


def wrap_count(text: str) -> int:
    """Rows the viewer gives text (result_view.h rv_next_line, one segment)."""
    n, pos = 0, 0
    while pos < len(text):
        end = text.find("\n", pos)
        line = text[pos:] if end < 0 else text[pos:end]
        if len(line) <= COLS:
            n += 1
            pos = len(text) if end < 0 else end + 1
            continue
        brk = next((k for k in range(COLS, MIN_BREAK - 1, -1) if line[k] == " "), 0)
        n += 1
        pos += brk + 1 if brk else COLS
    return n


def test_wrap_count_port_matches_header_examples() -> None:
    assert wrap_count("= 5") == 1
    assert wrap_count("a\n\nb\n") == 3
    assert wrap_count("1) integration by parts (score 80): matched by CEIPRT") == 3
    assert wrap_count("int = ((X*(-cos(X)))-(1*(-sin(X))))+C") == 2


@pytest.fixture(scope="module")
def launched_state(tmp_path_factory) -> str:
    import subprocess
    out = tmp_path_factory.mktemp("realrom_view")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return str(out / "launched.ce")


# Check step content and derive wrapped lines from the complete response.
# Label compaction must not turn a fixed historical line count into a failure.
@pytest.mark.parametrize("expr,prefix,min_lines,steps", [
    ("intstrategy(x*sin(x),x)", "Best: integration by parts", 9, False),
    ("int(x*sin(x),x)", "int = ", 9, True),
    ("2+3", "= 5", 1, False),
])
def test_viewer_on_real_rom(launched_state: str, expr: str, prefix: str, min_lines: int,
                            steps: bool) -> None:
    import realrom

    sym = {s: realrom.map_symbol(MAP, s) for s in
           ("_g_response", "_s_view_valid", "_s_view_lines")}
    version = realrom.ipc_version()
    with realrom.Emu(binary=CEMU, image=launched_state) as e:
        e.run(300)
        e.cmd(f"fill {sym['_g_response']:06X} 2 0")
        e.type(expr)
        e.cmd("key enter")
        e.run(9000)
        raw = realrom._peek(e, sym["_g_response"], 4511)
        resp = realrom.parse_response(raw, prefix, version)
        # Packed IPC6: header 5, settings 21, Ans 4, result 512, error 128.
        step_count = raw[670]
        step_text = [raw[671 + k * 120:671 + (k + 1) * 120].split(b"\0", 1)[0].decode("latin1")
                     for k in range(min(step_count, 32))]
        home = e.screen_hash()
        e.key("up", 600)                 # open the viewer
        valid = realrom._peek(e, sym["_s_view_valid"], 1)[0]
        lines = struct.unpack("<H", realrom._peek(e, sym["_s_view_lines"], 2))[0]
        top = e.screen_hash()
        e.key("down", 600)
        down = e.screen_hash()
        e.key("clear", 600)              # back home
        back = e.screen_hash()
        log = e.quit()

    assert not realrom.reset_seen(log), "calculator reset"
    assert resp.get("ok"), resp
    assert valid == 1, "launcher did not keep the full answer for the viewer"
    if steps:
        assert 2 <= step_count <= 32, step_text
        assert any("By parts" in text for text in step_text), step_text
        assert any("Factor:" in text for text in step_text), step_text
        expected = wrap_count(resp["text"]) + 1 + sum(map(wrap_count, step_text))
        assert lines == expected and lines >= min_lines, (lines, expected, step_text)
    else:
        assert lines == wrap_count(resp["text"]) and lines >= min_lines, (lines, resp["text"])
    assert top != home, "UP did not open the viewer"
    if lines > 8:
        assert down != top, "DOWN did not scroll a multi-page result"
    else:
        assert down == top, "DOWN moved a one-page result"
    assert back == home, "CLEAR did not restore the home screen"


@pytest.mark.parametrize("expr,message", [
    ("+", "Syntax error"),
    ("d(x^3,x)", "Syntax error: d/dx(expr)"),
    ("foo(x)", "Syntax error"),
])
def test_parse_error_keeps_input_on_real_rom(launched_state: str, expr: str, message: str) -> None:
    import realrom

    sym = {s: realrom.map_symbol(MAP, s) for s in
           ("_s_input", "_s_ilen", "_s_status", "_s_view_valid", "_s_view_lines",
            "_g_shared", "_g_quit_requested")}
    with realrom.Emu(binary=CEMU, image=launched_state) as e:
        e.run(300)
        e.type("2+3")
        e.cmd("key enter")
        e.run(9000)
        shared_size = sym["_g_quit_requested"] - sym["_g_shared"]
        assert 3000 < shared_size < 4000
        before = realrom._peek(e, sym["_g_shared"], shared_size)
        # The packed CE_STATE layout: last_expression precedes hist_count,
        # hist_write and history[16][128]. Locate history[0] on this build.
        hist = before.rfind(b"2+3\0")
        assert hist > 128 and before[hist - 2:hist] == b"\x01\x01"
        results = hist + 16 * 128 + 64     # history_results follows last_result
        assert before[results] != 0, "seed calculation has no stored result"
        e.type(expr)
        e.key("enter", 12000)
        err = e.screen_hash()
        ilen = realrom._peek(e, sym["_s_ilen"], 1)[0]
        text = realrom._peek(e, sym["_s_input"], len(expr) + 1)
        status = realrom._peek(e, sym["_s_status"], len(message) + 1)
        after = realrom._peek(e, sym["_g_shared"], shared_size)
        e.key("up", 600)                  # newest submission is now an error
        valid = realrom._peek(e, sym["_s_view_valid"], 1)[0]
        lines = struct.unpack("<H", realrom._peek(e, sym["_s_view_lines"], 2))[0]
        e.key("clear", 600)               # back home: input and history kept
        back = e.screen_hash()
        back_status = realrom._peek(e, sym["_s_status"], 1)
        back_ilen = realrom._peek(e, sym["_s_ilen"], 1)[0]
        back_text = realrom._peek(e, sym["_s_input"], len(expr) + 1)
        log = e.quit()

    assert not realrom.reset_seen(log), "calculator reset"
    assert ilen == len(expr) and text == expr.upper().encode() + b"\0", (ilen, text)
    assert status == message.encode() + b"\0", status
    assert after[hist - 2:hist] == b"\x02\x02", "failed input not added to history"
    assert after[hist:hist + 128] == before[hist:hist + 128], "old input changed"
    assert after[hist + 128:hist + 128 + len(text)] == text
    assert after[results:results + 64] == before[results:results + 64], "old result changed"
    assert after[results + 64:results + 64 + len(status)] == status
    assert after[:hist - 130] == before[:hist - 130], "settings, Ans or module state changed"
    assert valid == 0 and lines == wrap_count(message), (valid, lines)
    assert back != err, "transient status was not cleared on return"
    assert back_status == b"\0" and back_ilen == len(expr) and back_text == text


@pytest.fixture(scope="module")
def no_cebase_state(tmp_path_factory) -> str:
    """Launched state with every built module except CEBASE."""
    import subprocess
    tree = tmp_path_factory.mktemp("no_cebase_tree")
    for m in MODULES:
        if m.name == "CEBASE.8xp":
            continue
        dst = tree / "bin" / m.parent.name / m.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.symlink_to(m)
    hw = tree / "deploy/hardware-ready"
    hw.mkdir(parents=True)
    for n in ("arTIfiCE.8xp", "fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv"):
        (hw / n).symlink_to(ROOT / "deploy/hardware-ready" / n)
    out = tmp_path_factory.mktemp("no_cebase_state")
    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(tree), str(out), "--rom", ROM],
                   check=True, timeout=600)
    return str(out / "launched.ce")


def test_module_missing_keeps_its_history_slot_on_real_rom(no_cebase_state: str) -> None:
    import realrom

    sym = {s: realrom.map_symbol(MAP, s) for s in
           ("_g_shared", "_g_quit_requested", "_g_last_answer")}
    shared_size = sym["_g_quit_requested"] - sym["_g_shared"]
    assert 3000 < shared_size < 4000
    with realrom.Emu(binary=CEMU, image=no_cebase_state) as e:
        e.run(300)
        e.type("d/dx(x^3)")                 # CECALC answers
        e.key("enter", 9000)
        before = realrom._peek(e, sym["_g_shared"], shared_size)
        # A derivative leaves Ans at 0, so give Ans a recognisable value
        # (bytes 40 40 40 40) that the failure must carry through the exit.
        e.cmd(f"fill {sym['_g_last_answer']:06X} 4 40")
        home = e.screen_hash()
        e.type("2+3")                       # routes to the missing CEBASE
        e.key("enter", 6000)                # CALCENG exits to the arTIfiCE shell
        shell = e.screen_hash()
        e.key("enter", 6000)                # run CALCENG again
        relaunched = e.screen_hash()
        failed = realrom._peek(e, sym["_g_shared"], shared_size)
        e.type("d/dx(x^2)")                 # a following answer
        e.key("enter", 9000)
        following = realrom._peek(e, sym["_g_shared"], shared_size)
        log = e.quit()

    assert not realrom.reset_seen(log), "calculator reset"
    assert shell not in (home, relaunched), "CALCENG did not exit and relaunch"
    # Packed CE_STATE: pending_module[9], last_expression[128], hist_count,
    # hist_write, history[16][128], last_result[64], history_results[16][64].
    # last_expression also holds "2+3", so take the last match: history[1].
    h0 = failed.rfind(b"2+3\0") - 128
    assert h0 > 139 and failed[h0 - 2:h0] == b"\x02\x02", "failed input not in history"
    results = h0 + 16 * 128 + 64
    slot1 = slice(results + 64, results + 64 + len("Module missing") + 1)
    assert failed[slot1] == b"Module missing\0", "error not in the failed entry's slot"
    assert following[h0 - 2:h0] == b"\x03\x03"
    assert following[slot1] == b"Module missing\0", "a following answer hid the error"
    pm = h0 - 139                           # pending_module: cleared, its tail varies
    assert failed[:pm - 4] == before[:pm - 4] and failed[pm] == 0, "settings changed"
    assert failed[pm - 4:pm] == b"\x40" * 4, "Ans changed or was not saved before the exit"
