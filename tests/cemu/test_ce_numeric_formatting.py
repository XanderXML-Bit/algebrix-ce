from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROCESSOR = ROOT / "src" / "split" / "processor.c"
LIMITS_SERIES = ROOT / "src" / "modules" / "limits_series" / "main.c"
UNSAFE_FLOAT_FORMATS = ("%g", "%.4g", "%.6g")


def _unsafe_lines(text: str, tokens: tuple[str, ...]) -> list[tuple[int, str]]:
    lines: list[tuple[int, str]] = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        if "steps_addf" not in line and "proc_okf" not in line and "snprintf" not in line:
            continue
        if any(token in line for token in tokens):
            lines.append((line_no, line.strip()))
    return lines


def test_direct_eval_result_uses_ce_safe_decimal_formatter() -> None:
    text = PROCESSOR.read_text(encoding="utf-8")

    assert "static void proc_format_decimal" in text
    assert "proc_format_result(st.result, sizeof(st.result), val)" in text
    assert 'snprintf(st.result, sizeof(st.result), "= %.6g"' not in text


def test_split_processor_result_and_step_paths_avoid_ce_unsafe_g_formats() -> None:
    text = PROCESSOR.read_text(encoding="utf-8")

    unsafe = _unsafe_lines(text, UNSAFE_FLOAT_FORMATS)

    assert unsafe == []


def test_limits_series_worker_uses_ce_safe_decimal_formatter() -> None:
    text = LIMITS_SERIES.read_text(encoding="utf-8")

    assert "static void celims_format_decimal" in text
    assert "celims_format_result(buf, sizeof(buf), lim_val)" in text
    assert _unsafe_lines(text, UNSAFE_FLOAT_FORMATS) == []


# --------------------------------------------------------------------------
# All of src/: CE-safe formatting.
#
# The CEdev v11.2 libc printf prints %g/%e/%f literally, so on the calculator
# every number came out as "%.6g". The CE build now force-includes
# src/ce_fmt.h into every file (split/program.mk), which routes
# snprintf/sprintf/vsnprintf/vsprintf through src/ce_fmt.c. These checks keep
# that wiring in place and keep every format string inside the subset
# ce_fmt.c implements. Desktop builds use the host libc and cannot catch a
# regression, which is why this is a static check.
# --------------------------------------------------------------------------
import re

SRC = ROOT / "src"
SPLIT = ROOT / "split"
CE_FMT_C = SRC / "ce_fmt.c"
PRINTF_NAMES = (
    "snprintf", "sprintf", "vsnprintf", "vsprintf", "ce_snprintf", "ce_sprintf",
    "steps_addf", "steps_set_checkpoint_resultf", "proc_okf", "proc_failf",
)
# The subset ce_fmt.c renders (see its header comment).
# ce_fmt ignores '#' (and '0' on floats), so the guard rejects them; 'l'
# applies to integer conversions only.
_INT_SPEC = (r"(?:[-+ 0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh?|l)?[diuxX]"
             r"|-?(?:[1-9]\d*|\*)?(?:\.(?:\d+|\*))?[cs])")
_FLOAT_SPEC = r"[-+ ]*(?:[1-9]\d*|\*)?(?:\.(?:\d+|\*))?[eg]"
SUPPORTED_SPEC = re.compile(rf"%(?:%|{_INT_SPEC}|{_FLOAT_SPEC})")
ANY_SPEC = re.compile(r"%[-+ 0#]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[a-zA-Z%]?")


def _strip_comments(text: str) -> str:
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _call_args(text: str, start: int) -> str:
    """Return the text between the '(' at start and its matching ')'."""
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        i += 1
    return text[start + 1:]


def _string_literals(args: str) -> list[str]:
    return [m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', args)]


def _src_files() -> list[Path]:
    files = sorted(p for p in SRC.rglob("*") if p.suffix in (".c", ".h"))
    return [p for p in files if p != CE_FMT_C]


# Zero-based index of the format argument for each printf-like function.
FORMAT_ARG = {
    "snprintf": 2, "ce_snprintf": 2, "vsnprintf": 2,
    "sprintf": 1, "ce_sprintf": 1, "vsprintf": 1,
    "steps_addf": 0, "steps_set_checkpoint_resultf": 0,
    "proc_okf": 2, "proc_failf": 1,
}


def _split_args(args: str) -> list[str]:
    """Split a call's argument text at top-level commas."""
    parts, depth, cur, i, n = [], 0, [], 0, len(args)
    while i < n:
        c = args[i]
        if c in "\"'":
            j = i + 1
            while j < n and args[j] != c:
                j += 2 if args[j] == "\\" else 1
            cur.append(args[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


def _format_calls():
    """Yield (path, line, name, format) for each printf-like call.

    format is the concatenated string literal of the format argument, or
    None when that argument is not a literal (a variable format, or the
    forwarding fmt parameter inside a wrapper)."""
    call_re = re.compile(r"(?<![\w.>])(" + "|".join(PRINTF_NAMES) + r")\s*\(")
    for path in _src_files():
        text = _strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in call_re.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            args = _split_args(_call_args(text, m.end() - 1))
            idx = FORMAT_ARG[m.group(1)]
            if idx >= len(args) or any(a.strip() == "..." for a in args):
                continue                      # a declaration or prototype
            arg = args[idx].strip()
            lits = _string_literals(arg)
            lit_re = r'\s*"(?:[^"\\]|\\.)*"\s*'
            if lits and re.fullmatch(rf"(?:{lit_re})+", arg):
                yield path, line, m.group(1), "".join(lits)
            elif len(lits) == 2 and re.fullmatch(rf"[^\"]*\?(?:{lit_re}):(?:{lit_re})", arg):
                for lit in lits:              # cond ? "fmt a" : "fmt b"
                    yield path, line, m.group(1), lit
            else:
                yield path, line, m.group(1), None


def test_program_mk_force_includes_ce_fmt_for_every_ce_module() -> None:
    program_mk = (SPLIT / "program.mk").read_text(encoding="utf-8")
    assert "-include src/ce_fmt.h" in program_mk
    sources = [l for l in program_mk.splitlines() if l.startswith("EXTRA_C_SOURCES")]
    assert any("src/ce_fmt.c" in l for l in sources), sources
    makefiles = sorted(SPLIT.glob("Makefile.*"))
    assert len(makefiles) >= 35
    missing = [m.name for m in makefiles
               if "include split/program.mk" not in m.read_text(encoding="utf-8")]
    assert missing == []


def test_ce_fmt_redirect_is_active_on_ez80_and_not_undone() -> None:
    header = (SRC / "ce_fmt.h").read_text(encoding="utf-8")
    assert "defined(__ez80__)" in header
    for name in ("snprintf", "sprintf", "vsnprintf", "vsprintf"):
        assert f"#define {name} ce_{name}" in header
    undef_re = re.compile(r"^\s*#\s*undef\s+v?s?n?printf\b", re.M)
    offenders = [str(p.relative_to(ROOT)) for p in _src_files()
                 if p.name != "ce_fmt.h"
                 and undef_re.search(p.read_text(encoding="utf-8", errors="replace"))]
    assert offenders == []


def test_no_unredirected_printf_calls_in_src() -> None:
    bare = re.compile(r"(?<![\w.>])(printf|vprintf|fprintf|vfprintf)\s*\(")
    hits = []
    for path in _src_files():
        text = _strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in bare.finditer(text):
            hits.append(f"{path.relative_to(ROOT)}:{text.count(chr(10), 0, m.start()) + 1}")
    assert hits == []


def test_guard_subset_regex_rejects_unsupported_forms() -> None:
    for ok in ("%d", "%-26s", "%3d", "%06lu", "%c", "%.6g", "%+.4g", "% .3g", "%.*g", "%e", "%%", "%-3s"):
        m = SUPPORTED_SPEC.match(ok)
        assert m and m.end() == len(ok), ok
    for bad in ("%08s", "%0s", "%-0s", "%00c", "%+c", "% s", "%#x", "%ls", "%08g", "%f", "%.2f", "%E",
                "%G", "%lld", "%zu", "%p", "%o", "%Lg"):
        m = SUPPORTED_SPEC.match(bad)
        assert not (m and m.end() == len(bad)), bad


def test_every_src_format_string_uses_the_ce_fmt_subset() -> None:
    bad = []
    scanned = 0
    for path, line, name, fmt in _format_calls():
        if fmt is None or "%" not in fmt:
            continue
        scanned += 1
        pos = 0
        while True:
            i = fmt.find("%", pos)
            if i < 0:
                break
            m = SUPPORTED_SPEC.match(fmt, i)
            if not m:
                spec = ANY_SPEC.match(fmt, i)
                bad.append(f"{path.relative_to(ROOT)}:{line} {name} "
                           f"{(spec.group(0) if spec else fmt[i:i + 6])!r}")
                pos = i + 1
            else:
                pos = m.end()
    assert scanned > 800, scanned          # the scanner really sees the code
    assert bad == []


def test_src_formats_are_literals_outside_the_wrappers() -> None:
    # A variable format cannot be checked statically. Only the wrappers that
    # forward their own fmt parameter (steps.c, processor.c) may pass one.
    allowed = {("steps.c", "vsnprintf"), ("processor.c", "vsnprintf")}
    dynamic = [f"{p.relative_to(ROOT)}:{line} {name}"
               for p, line, name, fmt in _format_calls()
               if fmt is None and (p.name, name) not in allowed]
    assert dynamic == []


def test_ce_fmt_unit_tests_run_in_desktop_suite() -> None:
    makefile = (ROOT / "tests" / "Makefile").read_text(encoding="utf-8")
    assert "run: test_runner fmt-test" in makefile
    assert "-DCE_FMT_REAL=float" in makefile
    assert (ROOT / "tests" / "test_ce_fmt.c").is_file()


def test_built_maps_do_not_link_the_cedev_printf() -> None:
    import pytest

    maps = sorted((ROOT / "bin").glob("*/*.map"))
    if not maps:
        pytest.skip("no CE build output (bin/*/*.map)")
    linked_libc = [m.name for m in maps if "_npf_vpprintf" in m.read_text(errors="replace")]
    without_wrapper = [m.name for m in maps if "_ce_vsnprintf" not in m.read_text(errors="replace")]
    assert linked_libc == []
    assert without_wrapper == []
