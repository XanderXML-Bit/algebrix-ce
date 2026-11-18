"""Every operator the launcher accepts must reach a module that handles it.

The CE build splits Algebrix into separate programs.  CALCENG (the launcher)
parses the input, picks a program with route_program() in
src/core/launcher/main.c, and sends it the AST.  Router programs (CEALG,
CECINT, CECLIM, CECPOL, CECIAD) forward the AST to a worker.  A worker built
from src/split/processor.c only contains the handlers its own PROGRAM_CFLAGS
enable.  Code inside ``#ifndef HW_BUILD_SPLIT`` is in no CE module at all.

So an operator can parse, classify, and route, and still reach a module
without its handler.  The module then answers with a generic refusal such as
"Polar/advanced op (use at top level)".  That happened to eigenval() and
eigenvec() (internal review): classify.c returns CE_DOMAIN_MATRIX (CEMAT),
the launcher sends them to CEBASE, and their handler sits in processor.c's
``#ifndef HW_BUILD_SPLIT`` block, which neither module compiles.

This test checks the whole chain on the host for one sample input per
operator:

1. The launcher harness (tests/harness/op_dispatch_harness.c, built with
   split/Makefile.launcher's flags) reports the parsed node, the classify.c
   domain, ce_domain_program(domain), and route_program().
2. Router programs are run with their own flags
   (tests/module_parity/router_driver.c) until the chain reaches a worker.
3. The worker is built with its own PROGRAM_CFLAGS and PROGRAM_SOURCES
   (tests/module_parity/module_driver.c or strategy_driver.c) and runs the
   input.  An answer, or an error about the input, counts as handled.  One of
   the generic "not compiled here" refusals in REFUSALS does not.

Two paths are checked for each operator: the program the launcher really
dispatches to, and the shipped modules whose CE_PROC_* flags (resolved by
the preprocessor through split/processor.h) serve classify.c's domain.  At
least one of those must handle the operator.  Known gaps that need their
own fix are listed in KNOWN_GAPS and run as strict xfail: the test fails if
a gap closes without the list being updated, and fails for any new gap.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SPLIT = ROOT / "split"
DISPATCH_HARNESS = ROOT / "tests" / "harness" / "op_dispatch_harness.c"
ROUTER_DRIVER = ROOT / "tests" / "module_parity" / "router_driver.c"
MODULE_DRIVER = ROOT / "tests" / "module_parity" / "module_driver.c"
STRATEGY_DRIVER = ROOT / "tests" / "module_parity" / "strategy_driver.c"

# Generic answers that mean "this module has no handler for the operator".
REFUSALS = (
    "use at top level",                      # eval.c: Polar/advanced op, Symbolic op
    "Feature is routed to another module",   # processor.c without CE_PROC_GENERIC
    "Unknown node type",                     # eval.c default branch
    "Not a limit expression",                # CELIMS
    "worker did not finish",                 # strategy_driver.c
    "Graph module loaded",                   # CEGRPH placeholder text
)

# One sample input for every polar_op_t value in src/ast.h.  The test checks
# that each input parses to the operator it is listed under.
POLAR_SAMPLES: dict[str, str | None] = {
    "POLOP_TO_RECT": "polar(2,1)",
    "POLOP_FROM_RECT": "rect(1,1)",
    "POLOP_ARCLEN": "arclen(x^2,x,0,1)",
    "POLOP_ARCLEN_PARAM": "arclen(cos(t),sin(t),t,0,1)",
    "POLOP_SURFREV": "surfarea(x,x,0,1)",
    "POLOP_DISK": "disk(x,x,0,1)",
    "POLOP_WASHER": "washer(x,x^2,x,0,1)",
    "POLOP_SHELL": "shell(x,x,0,1)",
    "POLOP_RELRATE": "relrate(x^2+y^2=25)",
    "POLOP_CSQUARE": "csquare(x^2+4x+1,x)",
    "POLOP_PFRAC": "pfrac(1/(x^2-1),x)",
    "POLOP_DESCARTES": "descartes(x^3-x,x)",
    "POLOP_LINE2PT": "line(0,0,1,1)",
    "POLOP_LINEPT": "linept(2,1,1)",
    "POLOP_PARABOLA": "parabola(0,0,1,1)",
    "POLOP_CONTINUITY": "continuity(x^2,x,1)",
    "POLOP_TOFRAC": "tofrac(0.5)",
    "POLOP_RECSEQ": "recseq(2*a,a,1,3)",
    "POLOP_UNITCIRC": "unitcirc(30)",
    "POLOP_NLSOLVE": "nlsolve(x+y-3,x-y-1,x,y)",
    "POLOP_ARITHSUM": "arithsum(1,1,10)",
    "POLOP_GEOMSUM": "geomsum(1,2,5)",
    "POLOP_MIDPOINT": "midpoint(0,0,2,2)",
    "POLOP_DISTANCE": "distance(0,0,3,4)",
    "POLOP_SLOPE": "slope(0,0,1,2)",
    "POLOP_NEWTON": "newton(x^2-2,x,1)",
    "POLOP_BISECT": "bisect(x^2-2,x,0,2)",
    "POLOP_SECANT": "secant(x^2-2,x,1,2)",
    "POLOP_PDIFF": "pdiff(x^2*y,x,y)",
    "POLOP_GRADIENT": "gradient(x^2+y^2,x,y)",
    "POLOP_DBLINT": "dblint(x*y,x,0,1,y,0,1)",
    "POLOP_CRITICAL": "critical(x^3-3x,x)",
    "POLOP_POLYDIV": "polydiv(x^2-1,x-1,x)",
    "POLOP_GRAPH": "graph(x^2,x)",
    "POLOP_GRAPHEQ": "grapheq(x^2,2,x)",
    "POLOP_ODE_SEP": "ode_sep(x,x,y)",
    "POLOP_ODE_LINEAR": "ode_linear(x,x,y)",
    "POLOP_DIFFEQ": "diffeq(x,x,y)",
    "POLOP_EULER": "euler(x+y,x,y,0,1,1,0.5)",
    "POLOP_SLOPEFIELD": "slopefield(x+y,x,y)",
    "POLOP_INTPARTS": "intparts(x*e^x,x)",
    "POLOP_TRIGSUB": "trigsub(sqrt(1-x^2),x)",
    "POLOP_USUB": "usub(2x*cos(x^2),x,x^2)",
    "POLOP_INTSTRATEGY": "intstrategy(x*e^x,x)",
    "POLOP_AREABETWEEN": "areabetween(x,x^2,x,0,1)",
    "POLOP_IMPROPER": "improper(1/x^2,x,1,inf)",
    "POLOP_DSOLVE": "dsolve(y,y,x)",
    "POLOP_POLARAREA": "polararea(1,t,0,1)",
    "POLOP_POLARARCLEN": "polararclen(1,t,0,1)",
    "POLOP_VEC_DOT": "dot(1,2,3,4)",
    "POLOP_VEC_CROSS": "cross(1,0,0,0,1,0)",
    "POLOP_VEC_GRAD": "grad(x^2+y^2,x,y)",
    "POLOP_VEC_DIV": "div(x,y,x,y)",
    "POLOP_VEC_CURL": "curl(y,x,z,x,y,z)",
    "POLOP_LAPLACE": "laplace(t,t,s)",
    "POLOP_ILAPLACE": "ilaplace(1/s^2,s,t)",
    # The parser never builds POLOP_CONVERT: "convert" is a token with no
    # parse rule, so no input reaches classify.c's CE_DOMAIN_CORE entry.
    "POLOP_CONVERT": None,
    "POLOP_DSOLVE2": "dsolve2(1,0,1,0,x)",
    "POLOP_EIGENVAL": "eigenval(mat(2))",
    "POLOP_EIGENVEC": "eigenvec(mat(2))",
    "POLOP_GRAPHIMPL": "graphimpl(x^2+y^2=1,x,y)",
}

# Other node types the launcher routes to a dedicated worker.
NODE_SAMPLES: dict[str, str] = {
    "NODE_DERIV": "d/dx(x^2)",
    "NODE_TANGENT": "tangent(x^2,x,1)",
    "NODE_LIMIT": "lim(sin(x)/x,x,0)",
    "NODE_INTEGRAL": "int(x^2,x)",
    "NODE_INTEGRAL definite": "int(x^2,x,0,1)",
    "NODE_TAYLOR": "taylor(e^x,x,0,3)",
    "NODE_FOURIER": "fourier(x,x,2)",
    "NODE_SUM": "sum(n,n,1,10)",
    "NODE_EXPAND": "expand((x+1)^2)",
    "NODE_FACTOR": "factor(x^2-1)",
    "NODE_SOLVE": "solve(x^2-4,x)",
    "NODE_STAT": "mean({1,2,3})",
    "NODE_STAT finance": "pv(10,0.05,0,-1000)",
    "NODE_STAT probability": "normalcdf(0,1,0,1)",
    "NODE_MATRIX_OP": "det(mat(1,2;3,4))",
    "NODE_LIST_LITERAL": "{1,2,3}",
}

# Known gaps outside the scope of an internal review, which found them.  Each
# needs its own fix.  Keys are (path, op key).  path "launcher" is the
# program CALCENG really dispatches to, and path "classify" is the set of
# modules built with the CE_PROC_* flags of classify.c's domain.  The tests
# run these as strict xfail: when a fix lands, the test fails until its
# entry is removed.  Never list eigenval/eigenvec here: an internal review
# fixes them, and this test must fail until it does.
_CEBASE_EVAL_ADV = ("CEBASE builds with CE_EVAL_ENABLE_ADV_POLAR=0, so "
                    "eval.c refuses; no module has the handler")
_SPLIT_EXCLUDED = ("handler only in processor.c #ifndef HW_BUILD_SPLIT, "
                   "which no CE module compiles")
_CECPNM_EVAL = ("CECPNM's CE_PROC_CALC_EULER handler calls eval_expr(), "
                "but CECPNM builds with CE_EVAL_ENABLE_ADV_POLAR=0")
_GRAPH_STUB = "CEGRPH answers only its placeholder text"
_CLASSIFY_CALC = ("classify.c has no case, so the default CE_DOMAIN_CALC "
                  "applies; the handler is in CECIDF")
_CLASSIFY_BASE = ("classify.c names a domain whose modules lack the "
                  "handler; CEBASE's eval.c answers it instead")
_LAUNCHER_GAPS = {
    "POLOP_NEWTON": _CEBASE_EVAL_ADV,
    "POLOP_BISECT": _CEBASE_EVAL_ADV,
    "POLOP_SECANT": _CEBASE_EVAL_ADV,
    "POLOP_PDIFF": _CEBASE_EVAL_ADV,
    "POLOP_GRADIENT": _CEBASE_EVAL_ADV,
    "POLOP_DBLINT": _CEBASE_EVAL_ADV,
    "POLOP_CRITICAL": _CEBASE_EVAL_ADV,
    "POLOP_GRAPH": _GRAPH_STUB,
    "POLOP_GRAPHEQ": _GRAPH_STUB,
    "POLOP_EULER": _CECPNM_EVAL,
    "POLOP_SLOPEFIELD": _CECPNM_EVAL,
    "POLOP_DSOLVE": _SPLIT_EXCLUDED,
    "POLOP_VEC_CROSS": _SPLIT_EXCLUDED,
    "POLOP_VEC_GRAD": _SPLIT_EXCLUDED,
    "POLOP_VEC_CURL": _SPLIT_EXCLUDED,
    "POLOP_LAPLACE": _SPLIT_EXCLUDED,
    "POLOP_ILAPLACE": _SPLIT_EXCLUDED,
    "POLOP_DSOLVE2": _SPLIT_EXCLUDED,
    "POLOP_GRAPHIMPL": _SPLIT_EXCLUDED,
}
KNOWN_GAPS: dict[tuple[str, str], str] = {
    **{("launcher", k): v for k, v in _LAUNCHER_GAPS.items()},
    **{("classify", k): v for k, v in _LAUNCHER_GAPS.items()},
    ("classify", "POLOP_AREABETWEEN"): _CLASSIFY_CALC,
    ("classify", "POLOP_IMPROPER"): _CLASSIFY_CALC,
    ("classify", "POLOP_POLARAREA"): _CLASSIFY_BASE,
    ("classify", "POLOP_POLARARCLEN"): _CLASSIFY_BASE,
    ("classify", "POLOP_VEC_DOT"): _CLASSIFY_BASE,
    ("classify", "POLOP_VEC_DIV"): _CLASSIFY_BASE,
}


# ── Makefile reading (same rules as test_module_flag_parity.py) ──────────

def _makefile_vars(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8").replace("\\\n", " ")
    values: dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r"^([A-Z_]+)\s*[:?]?=\s*(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2).strip()
    return values


@lru_cache(maxsize=None)
def _shipped_modules() -> dict[str, Path]:
    """Program NAME -> makefile, for every module the top Makefile builds."""
    top = (ROOT / "Makefile").read_text(encoding="utf-8").replace("\\\n", " ")
    out: dict[str, Path] = {}
    for var in ("SPLIT_MODULES", "WORKER_MODULES"):
        match = re.search(rf"^{var}\s*:=\s*(.*)$", top, re.M)
        assert match, f"{var} not found in Makefile"
        for name in match.group(1).split():
            makefile = SPLIT / f"Makefile.{name}"
            prog = _makefile_vars(makefile)["NAME"]
            assert prog not in out, f"two shipped makefiles build {prog}"
            out[prog] = makefile
    return out


def _sources(prog: str) -> list[str]:
    """PROGRAM_SOURCES plus PROGRAM_RUNTIME_SOURCES, as split/program.mk
    links them."""
    values = _makefile_vars(_shipped_modules()[prog])
    return (values["PROGRAM_SOURCES"].split() +
            values.get("PROGRAM_RUNTIME_SOURCES", "").split())


def _kind(prog: str) -> str:
    """processor: links src/split/processor.c (module_driver.c runs it).
    strategy: an integration strategy worker (strategy_driver.c runs it).
    router: its main is in ROUTER_MAINS: it forwards every request and
    computes nothing itself (router_driver.c runs it).  other: has its own main and no host driver
    here; only the programs in OTHER_WORKERS may be reached."""
    srcs = _sources(prog)
    if "src/split/processor.c" in srcs:
        return "processor"
    if "src/modules/calc_int/strategy_main.c" in srcs:
        return "strategy"
    if any(s in srcs for s in ROUTER_MAINS):
        return "router"
    return "other"


# Entry files of the programs that only forward (no math of their own).
ROUTER_MAINS = (
    "src/modules/algebra/main.c",       # CEALG
    "src/modules/calc_int/main.c",      # CECINT
    "src/modules/calc_int/adv_main.c",  # CECIAD
    "src/modules/calc_lim/main.c",      # CECLIM
    "src/modules/calc_pol/main.c",      # CECPOL
)


# Workers with no host driver, and the operators each one accepts.
# CEISTR ranks integration strategies (src/modules/calc_int/report_main.c
# ce_is_report_request()); CELIMS answers lim() only.
OTHER_WORKERS: dict[str, tuple[str, ...]] = {
    "CEISTR": ("POLOP_INTSTRATEGY",),
    "CELIMS": ("NODE_LIMIT",),
}


# ── Host builds ───────────────────────────────────────────────────────────

def _host_cc() -> str | None:
    if sys.platform == "win32":
        return None
    return shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _require_cc() -> str:
    cc = _host_cc()
    assert cc is not None, "host C compiler disappeared"
    return cc


def _gc_flags() -> list[str]:
    if sys.platform == "darwin":
        return ["-Wl,-dead_strip"]
    return ["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]


@pytest.fixture(scope="module")
def build_dir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    if _host_cc() is None:
        pytest.skip("needs a POSIX host C compiler")
    return tmp_path_factory.mktemp("op_dispatch")


_BUILT: dict[str, Path] = {}


def _cc(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          timeout=600)
    assert proc.returncode == 0, proc.stderr[-4000:]


def _build_dispatch(out_dir: Path) -> Path:
    if "launcher" in _BUILT:
        return _BUILT["launcher"]
    flags = _makefile_vars(SPLIT / "Makefile.launcher")["PROGRAM_CFLAGS"].split()
    exe = out_dir / "op_dispatch_harness"
    _cc([_require_cc(), "-std=gnu99", "-O0", "-w",
         "-I", str(ROOT / "src"),
         "-I", str(ROOT / "tests" / "harness" / "launcher_mock"),
         "-I", str(ROOT / "tests" / "mock"),
         "-DDESKTOP_BUILD", "-DCE_DISABLE_STEP_RECORDING=1", *flags,
         str(DISPATCH_HARNESS),
         *[str(ROOT / s) for s in (
             "src/split/runtime_globals.c", "src/split/ipc.c",
             "src/split/astwire.c", "src/split/classify.c",
             "src/memory.c", "src/token.c", "src/ast.c", "src/parser.c",
             "src/split/module_runtime.c", "tests/mock/fileioc_mock.c")],
         "-lm", "-o", str(exe)])
    _BUILT["launcher"] = exe
    return exe


def _build_module(prog: str, out_dir: Path) -> Path:
    if prog in _BUILT:
        return _BUILT[prog]
    makefile = _shipped_modules()[prog]
    values = _makefile_vars(makefile)
    kind = _kind(prog)
    srcs = _sources(prog)
    extra = ["src/ce_fmt.c", "src/token.c", "src/parser.c",
             "tests/mock/fileioc_mock.c"]
    if kind == "processor":
        driver, main_flags = MODULE_DRIVER, []
        # The module's own main() needs the TI runtime; the driver calls
        # ce_processor_run_preparsed() as module_entry.c does.
        srcs = [s for s in srcs if not s.endswith("main.c") or
                s.startswith("src/split/processor")]
        srcs = [s for s in srcs if s != "src/split/module_forward.c"]
        extra = ["src/split/module_runtime.c", "src/split/module_forward.c",
                 *extra]
    elif kind == "strategy":
        driver, main_flags = STRATEGY_DRIVER, ["-Dmain=ce_worker_main"]
    elif kind == "router":
        driver, main_flags = ROUTER_DRIVER, ["-Dmain=ce_router_main"]
        srcs = [s for s in srcs if s != "src/split/module_forward.c"]
    else:
        raise AssertionError(f"{prog}: no host driver for this module kind")
    exe = out_dir / f"{prog.lower()}_{kind}"
    _cc([_require_cc(), "-std=c99", "-O0", "-w",
         "-I", str(ROOT / "src"), "-I", str(ROOT / "tests" / "mock"),
         "-DDESKTOP_BUILD",
         *values.get("PROGRAM_CFLAGS", "").split(),
         *main_flags,
         "-DCE_FMT_REDIRECT", "-DCE_FMT_REAL=float",
         "-include", str(ROOT / "src" / "ce_fmt.h"),
         str(driver),
         *[str(ROOT / s) for s in srcs + extra],
         *_gc_flags(), "-lm", "-o", str(exe)])
    _BUILT[prog] = exe
    return exe


def _run(exe: Path, exprs: list[str]) -> list[str]:
    proc = subprocess.run([str(exe), *exprs], capture_output=True, text=True,
                          timeout=120, encoding="latin-1")
    assert proc.returncode == 0, proc.stderr[-4000:]
    rows = proc.stdout.splitlines()
    assert len(rows) == len(exprs), proc.stdout
    return rows


# ── Enum names ────────────────────────────────────────────────────────────

def _enum(text: str, typedef: str) -> list[str]:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    body = re.search(r"typedef enum \{([^{}]*)\}\s*" + typedef + r"\s*;",
                     text)
    assert body, typedef
    clean = body.group(1)
    names = []
    for item in clean.split(","):
        item = item.strip()
        if not item:
            continue
        name, _, value = (p.strip() for p in item.partition("="))
        # Only a leading "= 0" keeps the implicit numbering.
        assert not value or (value == "0" and not names), (
            f"{typedef}: explicit value in {item}")
        names.append(name)
    return names


@lru_cache(maxsize=None)
def _polar_ops() -> list[str]:
    return _enum((ROOT / "src" / "ast.h").read_text(encoding="utf-8"),
                 "polar_op_t")


@lru_cache(maxsize=None)
def _node_types() -> list[str]:
    return _enum((ROOT / "src" / "ast.h").read_text(encoding="utf-8"),
                 "node_type_t")


@lru_cache(maxsize=None)
def _domains() -> list[str]:
    return _enum((ROOT / "src" / "split" / "ipc.h").read_text(encoding="utf-8"),
                 "ce_domain_t")


# ── Dispatch resolution ───────────────────────────────────────────────────

ALL_SAMPLES: dict[str, str] = {
    **{k: v for k, v in POLAR_SAMPLES.items() if v is not None}, **NODE_SAMPLES}


@pytest.fixture(scope="module")
def dispatch(build_dir: Path) -> dict[str, dict[str, str]]:
    """op key -> launcher facts for its sample input."""
    exe = _build_dispatch(build_dir)
    keys = list(ALL_SAMPLES)
    rows = _run(exe, [ALL_SAMPLES[k] for k in keys])
    out = {}
    for key, row in zip(keys, rows):
        if row == "parse error":
            out[key] = {"error": row}
            continue
        node, pop, dom, dom_prog, route, wnode, wpop = row.split("\t")
        out[key] = {
            "node": _node_types()[int(node)],
            "op": _polar_ops()[int(pop)] if int(pop) >= 0 else "",
            "domain": _domains()[int(dom)],
            "classify": dom_prog,
            "launcher": route,
            "wire": "" if int(wnode) == -2 else (
                _node_types()[int(wnode)] + ":" +
                (_polar_ops()[int(wpop)] if int(wpop) >= 0 else "")),
        }
    return out


def _resolve(prog: str, expr: str, out_dir: Path) -> list[str]:
    """Follow router programs from prog to the worker; return the chain."""
    chain = [prog]
    while _kind(chain[-1]) == "router":
        assert len(chain) < 6, f"{expr}: routing loop {chain}"
        [nxt] = _run(_build_module(chain[-1], out_dir), [expr])
        assert nxt != "parse error", f"{chain[-1]}: cannot parse {expr!r}"
        if nxt == "-":
            break
        assert nxt in _shipped_modules(), (
            f"{expr}: {chain[-1]} forwards to {nxt}, which no shipped "
            "makefile builds")
        chain.append(nxt)
    return chain


def _answer(prog: str, expr: str, out_dir: Path, key: str = "") -> str:
    kind = _kind(prog)
    if kind == "router":
        return f"{prog} answered without forwarding"
    if kind == "other":
        assert prog in OTHER_WORKERS, (
            f"{prog}: no host driver; add one or list it in OTHER_WORKERS")
        if key in OTHER_WORKERS[prog]:
            return "(not run: accepted by OTHER_WORKERS)"
        return f"{prog} does not take {key}"
    [row] = _run(_build_module(prog, out_dir), [expr])
    return row.split("\t")[1] if "\t" in row else row


def _refusal(answer: str) -> str | None:
    return next((r for r in REFUSALS if r in answer), None)


def _check_launcher(key: str, dispatch: dict[str, dict[str, str]],
                    out_dir: Path) -> None:
    facts = dispatch[key]
    expr = ALL_SAMPLES[key]
    chain = _resolve(facts["launcher"], expr, out_dir)
    worker = chain[-1]
    answer = _answer(worker, expr, out_dir, key)
    assert (_refusal(answer) is None and _kind(worker) != "router" and
            " does not take " not in answer), (
        f"{key} ({expr}): the program CALCENG dispatches to has no handler.\n"
        f"  classify.c domain : {facts['domain']}\n"
        f"  dispatch chain    : CALCENG -> {' -> '.join(chain)}\n"
        f"  {worker} built with split/{_shipped_modules()[worker].name} "
        f"flags answers: {answer!r}")


# classify.c domain -> the processor feature flags (src/split/processor.h)
# that compile handlers for it.  A module serves the domain when one of its
# flags resolves to nonzero under the module's PROGRAM_CFLAGS.
DOMAIN_FLAGS: dict[str, tuple[str, ...]] = {
    "CE_DOMAIN_CORE": ("CE_PROC_GENERIC",),
    "CE_DOMAIN_ALG": ("CE_PROC_ALG_SOLVE", "CE_PROC_ALG_POLYDIV"),
    "CE_DOMAIN_ALG_EXPFAC": ("CE_PROC_ALG_EXPFAC",),
    "CE_DOMAIN_ALG_ADV": ("CE_PROC_ALG_ADV_SYM",),
    "CE_DOMAIN_ALG_GEO": ("CE_PROC_ALG_ADV_GEO",),
    "CE_DOMAIN_CALC": ("CE_PROC_CALC_DERIV", "CE_PROC_CALC_ODE"),
    "CE_DOMAIN_CALC_TANGENT": ("CE_PROC_CALC_TANGENT", "CE_PROC_CALC_EULER"),
    "CE_DOMAIN_CALC_LIMIT": ("CE_PROC_CALC_LIMIT",
                             "CE_PROC_CALC_LIMIT_CONTINUITY",
                             "CE_PROC_CALC_LIMIT_RELRATE"),
    "CE_DOMAIN_CALC_INT": ("CE_PROC_CALC_INT", "CE_PROC_CALC_INT_STRATEGY"),
    "CE_DOMAIN_CALC_POLAR": ("CE_PROC_CALC_POLAR_NUM",
                             "CE_PROC_CALC_INT_DEFINITE_POLAR"),
    "CE_DOMAIN_CALC_SERIES": ("CE_PROC_CALC_SERIES_TF",),
    "CE_DOMAIN_CALC_SEQ": ("CE_PROC_CALC_SERIES_SEQ",),
    "CE_DOMAIN_STAT": ("CE_PROC_STAT",),
    "CE_DOMAIN_PROB": ("CE_PROC_STAT",),
    "CE_DOMAIN_MATRIX": ("CE_PROC_MATRIX",),
    "CE_DOMAIN_GRAPH": ("CE_PROC_GRAPH",),
    "CE_DOMAIN_FIN": ("CE_PROC_FIN",),
    "CE_DOMAIN_CMPLX": ("CE_PROC_CMPLX",),
}
# Integration workers without a processor serve CE_DOMAIN_CALC_INT.
DOMAIN_EXTRA_KINDS = {"CE_DOMAIN_CALC_INT": ("strategy",)}
OTHER_DOMAINS = {"CEISTR": ("CE_DOMAIN_CALC_INT",),
                 "CELIMS": ("CE_DOMAIN_CALC_LIMIT",)}


@lru_cache(maxsize=None)
def _proc_flags(prog: str) -> dict[str, int]:
    """CE_PROC_* values after src/split/processor.h resolves its defaults
    under the module's PROGRAM_CFLAGS, read from the preprocessor."""
    names = sorted({f for flags in DOMAIN_FLAGS.values() for f in flags})
    probe = '#include "split/processor.h"\n' + "".join(
        f"PARITY_{n} = {n} ;\n" for n in names)
    cflags = _makefile_vars(_shipped_modules()[prog]).get(
        "PROGRAM_CFLAGS", "").split()
    proc = subprocess.run(
        [_require_cc(), "-E", "-P", "-x", "c", "-I", str(ROOT / "src"),
         "-I", str(ROOT / "tests" / "mock"), "-DDESKTOP_BUILD", *cflags, "-"],
        input=probe, cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stderr[-2000:]
    out = {}
    for m in re.finditer(r"PARITY_(\w+) = ([^;]*);", proc.stdout):
        expr = m.group(2).strip()
        assert re.fullmatch(r"[\d\s()|&!<>=+-]*", expr), (prog, m.group(0))
        out[m.group(1)] = int(eval(expr.replace("||", " or ")  # noqa: S307
                                       .replace("&&", " and ")
                                       .replace("!", " not ")))
    assert len(out) == len(names), (prog, proc.stdout[-2000:])
    return out


def _domain_modules(domain: str) -> list[str]:
    mods = []
    for prog in _shipped_modules():
        kind = _kind(prog)
        if kind == "processor":
            if any(_proc_flags(prog)[f] for f in DOMAIN_FLAGS[domain]):
                mods.append(prog)
        elif kind in DOMAIN_EXTRA_KINDS.get(domain, ()):
            mods.append(prog)
        elif kind == "other" and domain in OTHER_DOMAINS.get(prog, ()):
            mods.append(prog)
    return mods


def _check_classify(key: str, dispatch: dict[str, dict[str, str]],
                    out_dir: Path) -> None:
    facts = dispatch[key]
    expr = ALL_SAMPLES[key]
    domain = facts["domain"]
    mods = _domain_modules(domain)
    answers = {m: _answer(m, expr, out_dir, key) for m in mods}
    handled = [m for m, a in answers.items()
               if _refusal(a) is None and " does not take " not in a]
    detail = "".join(
        f"\n    {m} (split/{_shipped_modules()[m].name}): {a!r}"
        for m, a in answers.items())
    assert handled, (
        f"{key} ({expr}): classify.c routes it to {domain}, but no module "
        f"built with that domain's flags ({' or '.join(DOMAIN_FLAGS[domain])}"
        f" plus HW_BUILD_SPLIT) handles it.\n"
        f"  ce_domain_program(): {facts['classify']}\n"
        f"  modules built for {domain}:{detail or ' none'}")


def _params(path: str) -> list:
    out = []
    for key in ALL_SAMPLES:
        reason = KNOWN_GAPS.get((path, key))
        marks = [pytest.mark.xfail(strict=True, reason=reason)] if reason else []
        out.append(pytest.param(key, id=key.replace(" ", "_"), marks=marks))
    return out


# ── Tests ─────────────────────────────────────────────────────────────────

def test_every_polar_op_has_a_sample() -> None:
    """A new polar_op_t value needs a sample here, so it is checked too."""
    missing = [op for op in _polar_ops() if op not in POLAR_SAMPLES]
    stale = [op for op in POLAR_SAMPLES if op not in _polar_ops()]
    assert not missing and not stale, (missing, stale)


def test_samples_parse_to_their_operator(
        dispatch: dict[str, dict[str, str]]) -> None:
    """Each sample parses to its operator in the launcher build, and the
    worker receives the same node type and operator over astwire."""
    bad = []
    for key, facts in dispatch.items():
        if "error" in facts:
            bad.append(f"{key}: {ALL_SAMPLES[key]!r} does not parse")
        elif key.startswith("POLOP_"):
            if facts["node"] != "NODE_POLAR" or facts["op"] != key:
                bad.append(f"{key}: {ALL_SAMPLES[key]!r} parses as {facts}")
        elif facts["node"] != key.split()[0]:
            bad.append(f"{key}: {ALL_SAMPLES[key]!r} parses as {facts}")
        if "error" not in facts and \
                facts["wire"] != f"{facts['node']}:{facts['op']}":
            bad.append(f"{key}: the astwire round trip changes "
                       f"{facts['node']}:{facts['op']} to {facts['wire']!r}")
    assert not bad, "\n".join(bad)


def test_known_gaps_name_real_operators() -> None:
    for path, key in KNOWN_GAPS:
        assert path in ("launcher", "classify") and key in ALL_SAMPLES, (path, key)
        assert key not in ("POLOP_EIGENVAL", "POLOP_EIGENVEC")


@pytest.mark.parametrize("key", _params("launcher"))
def test_launcher_dispatch_reaches_a_handler(
        key: str, dispatch: dict[str, dict[str, str]], build_dir: Path) -> None:
    """The program CALCENG really runs for the operator handles it."""
    _check_launcher(key, dispatch, build_dir)


@pytest.mark.parametrize("key", _params("classify"))
def test_classify_domain_module_has_handler(
        key: str, dispatch: dict[str, dict[str, str]], build_dir: Path) -> None:
    """A module built with the CE_PROC_* flags of classify.c's domain (and
    HW_BUILD_SPLIT) handles the operator."""
    _check_classify(key, dispatch, build_dir)


def test_ops_without_a_sample_have_no_parse_rule() -> None:
    """An operator listed with no sample must really be unreachable from
    input: src/parser.c never assigns it."""
    parser = (ROOT / "src" / "parser.c").read_text(encoding="utf-8")
    for op, sample in POLAR_SAMPLES.items():
        if sample is None:
            assert not re.search(rf"\b{op}\b", parser), (
                f"{op} has a parse rule now; give it a sample input")
