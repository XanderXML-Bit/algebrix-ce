"""On-target steps for derivatives and int/solve/lim.

Before this, the int, solve and lim workers were built with
-DCE_DISABLE_STEP_RECORDING=1, so CE_RESP carried no steps although MODE
offered "Show work". The steps now reach the screen through the launcher's
full-result viewer (#103), which TRACE also opens.

The static checks always run. The real-ROM check is opt-in, like
test_realrom_module_handoff.py: it needs ALGEBRIX_ROM (an owned ROM, never
committed), CEMU_HEADLESS and a fresh `make all`.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPLIT = ROOT / "split"
LAUNCHER = ROOT / "src/core/launcher/main.c"

# Workers that produce derivative/int/solve/lim results. Routers (CECINT, CECLIM,
# CECIAD) hand the child's CE_RESP up unchanged, so they need no steps code.
STEP_WORKERS = {
    "calculus": "CECALC",
    "calc.int.simple": "CECISM",
    "calc.int.parts": "CEIPRT",
    "calc.int.pfrac": "CEIPFR",
    "calc.int.trig": "CEITRG",
    "calc.int.usub": "CEIUSB",
    "calc.lim.base": "CECLMB",
    "calc.lim.adv": "CECLIA",
    "algsolve": "CEASLV",
    "algsolve.adv": "CEASLA",
}
# Workers that use eval only as a numeric helper drop eval's own step text.
EVAL_HELPER_ONLY = set(STEP_WORKERS)


def test_definite_worker_keeps_steps_out_for_size() -> None:
    """CECIDF links integrate.c + deriv.c for exact F(b) - F(a) values
    (internal review). With step recording on it measured 56,050-60,307 B against
    the 51,200 B module limit, so it stays a value-only worker. Lift this when
    it fits; the desktop build still records the Simpson steps."""
    text = _mk("calc.int.def")
    assert "NAME = CECIDF" in text
    assert "-DCE_DISABLE_STEP_RECORDING=1" in text
    assert "src/integrate.c" in text


def _mk(name: str) -> str:
    return (SPLIT / f"Makefile.{name}").read_text()


@pytest.mark.parametrize("mod", sorted(STEP_WORKERS))
def test_worker_records_steps(mod: str) -> None:
    text = _mk(mod)
    assert f"NAME = {STEP_WORKERS[mod]}" in text
    assert "CE_DISABLE_STEP_RECORDING" not in text, f"{mod} compiles step recording out"
    assert re.search(r"^\s+src/steps\.c\b", text, re.M), f"{mod} does not link src/steps.c"


@pytest.mark.parametrize("mod", sorted(EVAL_HELPER_ONLY))
def test_eval_helper_steps_dropped(mod: str) -> None:
    text = _mk(mod)
    if "src/eval.c" in text:
        assert "-DCE_EVAL_RECORD_STEPS=0" in text, f"{mod} pays for eval step text it never shows"


def test_eval_step_switch_defaults_on() -> None:
    src = (ROOT / "src/eval.c").read_text()
    assert "#define CE_EVAL_RECORD_STEPS 1" in src
    assert "#define steps_addf(...) ((void)0)" in src


def test_abs_solver_logs_only_when_abs_found() -> None:
    src = (ROOT / "src/algebra.c").read_text()
    body = src[src.index("int algebra_solve_abs("):]
    body = body[:body.index("\n}\n")]
    assert body.index('"No absolute value detected"') < body.index('"Solve abs equation'), (
        "the abs solver runs first for every equation; logging before detection puts "
        "'Solve abs equation' in front of plain linear/quadratic solves")


def _func_body(src: str, signature: str) -> str:
    body = src[src.index(signature):]
    return body[:body.index("\n}\n")]


def test_launcher_trace_opens_result_viewer_with_steps() -> None:
    """Steps reach the screen through the #103 full-result viewer: its
    document is the newest result followed by "Steps:" and CE_RESP steps.
    TRACE opens it like UP, and MODE "Show work" decides whether steps show."""
    src = LAUNCHER.read_text()
    assert re.search(r"case sk_Trace:(?:\s*/\*.*?\*/)?\s*show_result_view\(\);", src, re.S)
    build = _func_body(src, "static void view_build(rv_doc_t *doc)")
    assert "g_settings.show_steps" in build and "g_response.step_count" in build
    assert 'rv_doc_add(doc, "Steps:"' in build
    menu = _func_body(src, "static void show_mode_menu(void)")
    assert "s_view_dirty      = true;" in menu, "toggling Show work must refresh the viewer cache"


# Every active CE module, including launcher, routers, shared runtime and
# recursively included headers. UTF-8 bytes are TI font codes on the CE.
from ce_source_graph import _assignment, ce_source_closure, ce_module_sources


@pytest.mark.parametrize("name", ["WORKER_MODULES", "SPLIT_MODULES", "PROGRAM_SOURCES",
                                  "PROGRAM_RUNTIME_SOURCES"])
def test_ce_graph_includes_literal_additions_and_rejects_expansions(name: str) -> None:
    assert _assignment(f"{name} = first\n{name} += second\n", name) == "first second"
    assert _assignment(f"{name} ?= first\n{name} ?= ignored\n{name} += second\n", name) == "first second"
    with pytest.raises(AssertionError, match="unsupported expansion"):
        _assignment(f"{name} = first\n{name} += $(wildcard src/*.c)\n", name)


STEP_SOURCES = ce_source_closure()


def _string_literals(text: str) -> list[str]:
    # Tokenize comments, strings and character constants together: comment
    # markers inside a string (e.g. a URL) must not hide the rest of the line.
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'',
                         text.replace("\\\n", ""), re.S)
    return [m[0] for m in tokens if m[0].startswith('"')]


def test_ce_string_guard_covers_shipped_sources() -> None:
    modules = ce_module_sources()
    assert {"launcher", "matrix", "calc.ode"} <= modules.keys()
    assert {"src/core/launcher/main.c", "src/core/launcher/result_view.h",
            "src/ce_fmt.h", "src/split/module_runtime.c", "src/split/processor.c"} <= set(STEP_SOURCES)
    # These are desktop/legacy-monolith features, not CE output paths.
    assert not {"src/ui.c", "src/dsolve.c", "src/vector.c", "src/units.c"} & set(STEP_SOURCES)


def test_string_scanner_preserves_comments_inside_strings() -> None:
    text = '/* "ignored λ" */ "https://example/λ"; "/*λ*/"; // "ignored λ"\n"λ"'
    assert _string_literals(text) == ['"https://example/λ"', '"/*λ*/"', '"λ"']


@pytest.mark.parametrize("path", STEP_SOURCES)
def test_step_worker_strings_are_ascii(path: str) -> None:
    bad = []
    for lit in _string_literals((ROOT / path).read_text(encoding="utf-8")):
        for ch in lit:
            if ord(ch) > 127:
                bad.append(lit)
                break
    assert not bad, f"non-ASCII text in {path} would print as TI font codes: {bad[:5]}"


def test_launcher_maps_left_bracket_to_ti_glyph() -> None:
    """ASCII '[' (0x5B) is theta in the TI-OS large font; home_print_row (used
    for every history, result and steps line) must map it to 0xC1."""
    src = LAUNCHER.read_text()
    assert "#define TI_CHAR_LBRACKET '\\xC1'" in src
    body = _func_body(src, "static void home_print_row(")
    assert "ti_fix_chars(buf, len);" in body
    view = _func_body(src, "static void show_result_view(void)")
    assert "ti_fix_chars(screen[r], RV_COLS);" in view


def test_definite_integral_worker_reaches_handler() -> None:
    """CECIDF builds with CE_PROC_CALC_INT=1 but SYMBOLIC=0. The NODE_INTEGRAL
    dispatch must not require SYMBOLIC, or int(f,x,a,b) answers "Feature is
    routed to another module" on the CE."""
    src = (ROOT / "src/split/processor.c").read_text()
    call = re.search(r"if \(root->type == NODE_INTEGRAL\) \{\s*handle_integral\(root, &st\);", src)
    assert call, "NODE_INTEGRAL dispatch not found"
    guards = re.findall(r"^#if (\w+)\s*$", src[:call.start()], re.M)
    assert guards and guards[-1] == "CE_PROC_CALC_INT", guards[-1:] if guards else None
    eval_src = (ROOT / "src/eval.c").read_text()
    assert "Numeric: Simpson's rule from" in eval_src
    # The TI-OS large font draws '[' (0x5B) as theta; keep it out of step text.
    simpson = eval_src[eval_src.index("Numeric: Simpson"):][:400]
    assert "[" not in simpson
    # Nested integrands (int(int(...)), int(sum(...))) must not flood the
    # steps: the integrand's own step lines are rolled back.
    # The NODE_INTEGRAL body lives out of line in eval_numeric_integral()
    # (noinline, for the recursive eval stack frame; PR #99).
    assert "return eval_numeric_integral(ctx, node);" in eval_src
    start = eval_src.index("static ce_float_t eval_numeric_integral(")
    case = eval_src[start:eval_src.index("Numeric: Simpson", start)]
    assert "steps_save()" in case and "steps_restore(" in case


# ── opt-in real-ROM pixels ───────────────────────────────────────────────

ROM = os.environ.get("ALGEBRIX_ROM", "")
CEMU = os.environ.get("CEMU_HEADLESS", "")
MODULES = sorted(ROOT.glob("bin/*/*.8xp"))
REALROM = bool(ROM and Path(ROM).is_file() and CEMU and Path(CEMU).is_file() and len(MODULES) >= 35)


def _text_rows(bmp: str) -> list[int]:
    """Home-screen text rows (0-9) that contain dark pixels, below the status bar."""
    Image = pytest.importorskip("PIL.Image")
    im = Image.open(bmp).convert("L")
    w, h = im.size
    rows = set()
    for y in range(30, h):
        if any(im.getpixel((x, y)) < 100 for x in range(w)):
            rows.add((y - 39) // 20)
    return sorted(r for r in rows if r >= 0)


@pytest.mark.skipif(not REALROM, reason="real-ROM lane needs ALGEBRIX_ROM, CEMU_HEADLESS and a built bin/")
def test_realrom_trace_shows_steps(tmp_path: Path) -> None:
    """int/solve/lim: the home screen offers the viewer (hint row
    under the result), TRACE opens it with the steps, CLEAR returns."""
    import subprocess
    sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))
    import realrom

    subprocess.run([sys.executable, str(ROOT / "tools/emulator/linux-realrom/realrom.py"),
                    "--cemu", CEMU, "deploy", str(ROOT), str(tmp_path), "--rom", ROM],
                   check=True, timeout=600)
    # Viewer rows in use (header 0, body 1..n, footer 9) per case.
    # CECIDF (definite int) records no steps on the CE; see
    # test_definite_worker_keeps_steps_out_for_size.
    cases = {"d/dx(x^3+2x)": 7, "int(x^2,x)": 6, "solve(x+2,x)": 5,
             "lim(x^2,x,2)": 4}
    res = realrom.run_cases(CEMU, str(tmp_path / "launched.ce"), tmp_path / "cases",
                            list(cases), then=["trace", "clear"])
    for r, body in zip(res, cases.values()):
        assert not r["reset"], r
        # expression, result (one or two rows since #106 wraps it), the
        # "UP=full result (N ln)" hint, input line; nothing on rows 8-9
        home = _text_rows(r["bmp"])
        assert home[:3] == [0, 1, 2] and home[-1] == 7 and len(home) in (4, 5), (r["expr"], home)
        trace, back = r["then"]
        rows = _text_rows(trace["bmp"])
        assert rows[0] == 0 and rows[-1] == 9 and len(rows) >= 4, (r["expr"], rows)
        assert rows == list(range(0, min(body, 8) + 1)) + [9], (r["expr"], rows)
        assert back["screen_hash"] == r["screen_hash"], "CLEAR must return to the same home screen"
