"""CECIAD, the advanced-integral forwarder, under CE-style re-entry, and the
CE int() chain answer compared with the desktop CLI (internal review).

On the CE, int() goes CECINT -> CECISM; what CECISM's basic rules cannot
solve it forwards (CEIADVREQ) to CECIAD, which tries the strategy workers
CEIPFR, CEITRG, CEIUSB and CEIPRT in turn. When all four refused, CECIAD
used to hand up the last worker's own reason, so int(int(x)) showed
"Integration by parts requires a product-like integrand". The desktop CLI
says "Cannot find symbolic antiderivative", and now the CE does too.

1. tests/harness/ceciad_reentry_harness.c runs CECIAD's real main() under an
   os_RunPrgm mock that behaves like the TI-84 Plus CE (never returns on a
   successful launch, zeroes CECIAD's statics before the callback re-enters
   main()) and checks the refusal, the reasons that are kept (too deep, out
   of memory, unsupported request, launch failures, silent children, direct
   intparts), matches, Ans, and the CE_STATE hand-off.
2. The chain parity test runs the pieces with their real module flags:
   CECISM's processor (module driver) must forward, each real strategy
   worker (strategy driver) gives its real answer, the CECIAD harness turns
   those four answers into the answer the CE shows, and that must equal the
   desktop CLI (bin/calcengine --solve).
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

import test_module_flag_parity as parity

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")

DEFINES = ["-DDESKTOP_BUILD", "-DHW_BUILD_SPLIT", "-DCE_DISABLE_STEP_RECORDING=1",
           "-DCE_AST_ENABLE_STAT_NAMES=0"]
FLAGS = ["-std=c99", "-O0", "-g", f"-I{ROOT / 'src'}", f"-I{ROOT / 'tests/mock'}", *DEFINES]
SOURCES = ["tests/harness/ceciad_reentry_harness.c", "src/split/module_runtime.c",
           "src/split/ipc.c", "src/split/astwire.c", "src/memory.c", "src/ast.c",
           "src/split/runtime_globals.c", "tests/mock/fileioc_mock.c"]

NO_ANTIDERIV = "Cannot find symbolic antiderivative"
PARTS_REASON = "Integration by parts requires a product-like integrand"

needs_cc = pytest.mark.skipif(not CC or os.name == "nt",
                              reason="needs a POSIX host C compiler")


def _cflags_from_makefile() -> list[str]:
    """CECIAD's own PROGRAM_CFLAGS must be what the harness builds with."""
    values = parity._makefile_vars(ROOT / "split" / "Makefile.calc.int.adv")
    return values["PROGRAM_CFLAGS"].split()


@pytest.fixture(scope="module")
def harness(tmp_path_factory: pytest.TempPathFactory) -> Path:
    if not CC or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    tmp = tmp_path_factory.mktemp("ceciad")

    def cc(*args: str) -> None:
        subprocess.run([CC, *FLAGS, *args], check=True, cwd=ROOT, timeout=300)

    # adv_main.c and module_forward.c both define/call main(); rename it so
    # the harness can own main() while module_reentry() still re-enters CECIAD.
    cc("-DCE_IADV_HARNESS", "-Dmain=ceciad_main", "-c",
       "src/modules/calc_int/adv_main.c", "-o", str(tmp / "adv_main.o"))
    cc("-Dmain=ceciad_main", "-c", "src/split/module_forward.c",
       "-o", str(tmp / "module_forward.o"))
    exe = tmp / "ceciad_harness"
    cc(*SOURCES, str(tmp / "adv_main.o"), str(tmp / "module_forward.o"),
       "-lm", "-o", str(exe))
    return exe


def test_harness_flags_match_the_module() -> None:
    assert set(_cflags_from_makefile()) <= {"-DHW_BUILD_SPLIT", *DEFINES}, \
        "split/Makefile.calc.int.adv changed its flags: update DEFINES"


@needs_cc
def test_ceciad_main_survives_ce_style_reentry(harness: Path) -> None:
    r = subprocess.run([str(harness)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and "PASSED: 0 failure(s)" in r.stdout, r.stdout + r.stderr
    assert r.stdout.count("ok: ") >= 33, r.stdout
    assert "FAIL" not in r.stdout, r.stdout


@pytest.fixture(scope="module")
def cecism(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return parity._build_module_driver(parity.SPLIT / "Makefile.calc.int.simple",
                                       tmp_path_factory.mktemp("cecism_iadv"))


@pytest.fixture(scope="module")
def workers(tmp_path_factory: pytest.TempPathFactory) -> list[Path]:
    """The four strategy workers in CECIAD's try order."""
    out = tmp_path_factory.mktemp("iadv_workers")
    names = ["Makefile.calc.int.pfrac", "Makefile.calc.int.trig",
             "Makefile.calc.int.usub", "Makefile.calc.int.parts"]
    return [parity._build_module_driver(parity.SPLIT / n, out, strategy=True)
            for n in names]


@pytest.fixture(scope="module")
def cli() -> Path:
    if not CC or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    subprocess.run(["make", "-C", str(ROOT / "tests"), "../bin/calcengine"],
                   check=True, capture_output=True, timeout=600)
    return ROOT / "bin" / "calcengine"


def _cli_line(cli: Path, expr: str) -> str:
    """First line of the CLI answer: a result on stdout, or a refusal on
    stderr with a non-zero exit status."""
    p = subprocess.run([str(cli), "--solve", expr], capture_output=True,
                       text=True, timeout=30)
    if p.returncode == 0:
        return p.stdout.strip().split("\n")[0]
    assert not p.stdout.strip(), (expr, p.stdout)
    return p.stderr.strip().split("\n")[0]


def _ce_chain(harness: Path, cecism: Path, workers: list[Path],
              expr: str) -> tuple[bool, str, list[tuple[bool, str]]]:
    [(ok, text)] = parity._run(cecism, [expr])
    assert (ok, text) == (False, "CEIADVREQ"), (expr, ok, text)
    answers = [parity._run(w, [expr])[0] for w in workers]
    args = []
    for w_ok, w_text in answers:
        args += ["1" if w_ok else "0", w_text]
    r = subprocess.run([str(harness), "--chain", "int", *args],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    final_ok, final_text = r.stdout.rstrip("\n").split("\t", 1)
    return final_ok == "1", final_text, answers


# Inputs every strategy refuses. Upper case is what the CE launcher types.
REFUSED = [
    "int(int(x,x),x)",
    "int(int(x))",
    "INT(INT(X))",
    "INT(INT(X),X)",
    "int(int(--2x+x+x))",
    "INT(INT(--2X+X+X))",
    "INT(SUM(K^2,K,1,N),N)",
    "int(e^(x^2),x)",
]


@needs_cc
@pytest.mark.parametrize("expr", REFUSED)
def test_ce_int_chain_refusal_matches_desktop_cli(
        expr: str, harness: Path, cecism: Path, workers: list[Path],
        cli: Path) -> None:
    ok, text, answers = _ce_chain(harness, cecism, workers, expr)
    # What reached the screen before: CEIPRT, the last worker, refuses with
    # its own by-parts reason.
    assert answers[-1] == (False, PARTS_REASON), answers
    assert not any(a_ok for a_ok, _ in answers), answers
    assert (ok, text) == (False, NO_ANTIDERIV), (expr, text)
    assert _cli_line(cli, expr) == NO_ANTIDERIV, expr


@needs_cc
def test_ce_int_chain_match_is_unchanged(harness: Path, cecism: Path,
                                         workers: list[Path], cli: Path) -> None:
    """A strategy that matches still answers: x*sin(x) by parts."""
    expr = "int(x*sin(x),x)"
    ok, text, answers = _ce_chain(harness, cecism, workers, expr)
    assert answers[-1][0] and (ok, text) == answers[-1], (answers, text)
    assert text == _cli_line(cli, expr) == "int = x*(-cos(x))+sin(x) + C"


@needs_cc
def test_direct_intparts_keeps_the_by_parts_reason(harness: Path) -> None:
    """intparts() asks for integration by parts alone, so CEIPRT's reason is
    the true answer to it and is not rewritten."""
    args = ["0", "unused", "0", "unused", "0", "unused", "0", PARTS_REASON]
    r = subprocess.run([str(harness), "--chain", "intparts", *args],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    assert r.stdout == f"0\t{PARTS_REASON}\n", r.stdout


def test_refusal_texts_match_their_sources() -> None:
    """CECIAD prints INTEG_NO_ANTIDERIV_MSG, which must be the desktop
    processor's literal (literal because test_ce_numeric_formatting.py
    rejects non-literal formats), and it rewrites only the two by-parts
    no-match reasons that integrate.c itself sets."""
    header = (ROOT / "src" / "integrate.h").read_text(encoding="utf-8")
    assert f'#define INTEG_NO_ANTIDERIV_MSG "{NO_ANTIDERIV}"' in header
    assert f'"{PARTS_REASON}"' in header
    proc = (ROOT / "src" / "split" / "processor.c").read_text(encoding="utf-8")
    assert f'proc_failf(st, "{NO_ANTIDERIV}");' in proc
    adv = (ROOT / "src" / "modules" / "calc_int" / "adv_main.c").read_text(encoding="utf-8")
    assert "strcpy(resp->error, INTEG_NO_ANTIDERIV_MSG)" in adv
    integ = (ROOT / "src" / "integrate.c").read_text(encoding="utf-8")
    assert "set_integrate_fail_reason(INTEG_PARTS_NOT_PRODUCT_MSG)" in integ
    assert "set_integrate_fail_reason(INTEG_PARTS_NO_SPLIT_MSG)" in integ
