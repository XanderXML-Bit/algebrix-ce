"""Static guard for the TI-OS program hand-off contract (no emulator needed).

Real-ROM trace, 2026-11-08 (OS 5.8.0.0022 + arTIfiCE 2.1, CEmu headless):
every ``os_RunPrgm(name, NULL, 0, NULL)`` hop leaves a 209-byte CEdev runprgm
stub frame on the 4 KB OS stack until the launched program *returns*. The old
scheme made each worker relaunch its parent fire-and-forget, so the frames were
never released: ``int(...)`` looped CECINT -> CECINT 21 times and hit the stack
limit (NMI -> "RAM Cleared"), and even single-hop modules leaked 418 bytes per
evaluation.

The contract is:
* parents launch children with a re-entry callback;
* workers return from main() (never relaunch the parent);
* every callback marks re-entry before it re-runs main().
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _code(rel: str) -> str:
    return _strip_comments((SRC / rel).read_text(encoding="utf-8"))


def test_worker_finish_never_relaunches_parent() -> None:
    code = _code("split/module_runtime.c")
    assert "os_RunPrgm" not in code
    assert "ce_launch_program" not in code


def test_launcher_dispatch_uses_reentry_callback() -> None:
    code = _code("core/launcher/main.c")
    assert re.search(r"ce_launch_program_reentry\s*\(\s*program\s*,\s*launcher_reentry\s*\)", code)
    assert not re.search(r"\bce_launch_program\s*\(", code)
    body = re.search(r"static int launcher_reentry\s*\([^)]*\)\s*\{(.*?)\n\}", code, re.S)
    assert body, "launcher_reentry definition missing"
    assert body.group(1).index("ce_module_runtime_mark_reentry") < body.group(1).index("main()")


def test_forward_callback_marks_reentry() -> None:
    code = _code("split/module_forward.c")
    body = re.search(r"static int module_reentry\s*\([^)]*\)\s*\{(.*?)\n\}", code, re.S)
    assert body, "module_reentry definition missing"
    assert body.group(1).index("ce_module_runtime_mark_reentry") < body.group(1).index("main()")
    assert re.search(r"ce_launch_program_reentry\s*\(\s*program\s*,\s*module_reentry\s*\)", code)



def test_children_launch_from_archive() -> None:
    """Unarchiving a child before os_RunPrgm kept a second RAM copy per level."""
    offenders = []
    for path in SRC.rglob("*.c"):
        rel = path.relative_to(SRC).as_posix()
        if rel == "split/ipc.c":
            continue
        if "ce_program_ensure_ram" in _strip_comments(path.read_text(encoding="utf-8", errors="replace")):
            offenders.append(rel)
    assert offenders == [], offenders


def test_strategy_children_do_not_forward() -> None:
    """CECIAD re-reads CE_REQ on resume; a forwarding strategy child would clobber it."""
    for mk in ("Makefile.calc.int.pfrac", "Makefile.calc.int.trig", "Makefile.calc.int.usub",
               "Makefile.calc.int.parts"):
        text = (ROOT / "split" / mk).read_text(encoding="utf-8")
        assert "module_forward.c" not in text, mk
        for src in re.findall(r"src/(modules/\S+\.c)", text):
            assert "ce_module_runtime_forward" not in _code(src), (mk, src)


def test_strategy_report_resumes_from_scratch() -> None:
    """CEISTR is reloaded with zeroed statics after every strategy child.

    It must peek at the returned child (not hand its response up), keep its
    progress in the CEISTRS scratch AppVar saved before every forward, and
    forward int(expr,var), which is what the strategy children accept.
    """
    code = _code("modules/calc_int/report_main.c")
    assert "ce_module_runtime_resume_peek" in code
    assert "ce_module_runtime_resume_forward" not in code
    assert "ce_istr_scratch_load" in code and "ce_istr_record_child" in code
    assert "ce_istr_integral_request" in code
    run = code[code.index("static void ce_istr_run"):]
    assert run.index("ce_istr_scratch_save") < run.index("ce_module_runtime_forward"), \
        "scratch must be saved before the forward that never returns on CE"
    mk = (ROOT / "split" / "Makefile.calc.int.report").read_text(encoding="utf-8")
    assert "module_forward.c" in mk


def test_no_null_callback_run_prgm_outside_ipc() -> None:
    offenders = []
    for path in SRC.rglob("*.c"):
        rel = path.relative_to(SRC).as_posix()
        if rel == "split/ipc.c":
            continue
        code = _strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        if re.search(r"\bos_RunPrgm\s*\(", code) or re.search(r"\bce_launch_program\s*\(", code):
            offenders.append(rel)
    assert offenders == [], offenders


def test_every_reentry_callback_marks_reentry() -> None:
    """Any os_RunPrgm callback that re-runs main() must mark re-entry first."""
    found = set()
    for path in SRC.rglob("*.c"):
        code = _strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in re.finditer(r"static\s+int\s+(\w+)\s*\([^)]*\)\s*\{", code):
            if m.group(1) == "main":
                continue
            # Body up to the matching closing brace.
            depth, i = 1, m.end()
            while depth and i < len(code):
                depth += {"{": 1, "}": -1}.get(code[i], 0)
                i += 1
            body = code[m.end():i]
            if re.search(r"\bmain\s*\(\s*\)", body):
                found.add(m.group(1))
                assert "ce_module_runtime_mark_reentry" in body, (path.name, m.group(1))
                assert body.index("ce_module_runtime_mark_reentry") < re.search(r"\bmain\s*\(\s*\)", body).start(), (path.name, m.group(1))
    assert {"launcher_reentry", "module_reentry", "core_reentry"} <= found, found
