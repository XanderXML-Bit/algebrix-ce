"""Static guard: no raw isnan/isinf/isfinite in the CE (HW build) sources.

CEdev v11.2's libc _isinff tests the wrong byte whenever the mantissa is
non-zero, so it reports finite values such as 16777215, 1048575, 65791 and
0.99999237 as infinite. On the real OS 5.8 ROM a plain "16777215" displayed
"= inf". Desktop glibc is correct, so desktop tests
cannot catch a regression, which is why this check is static.

Every file under src/ is compiled into a .8xp module, so every file must use
ce_isnan/ce_isinf/ce_isfinite from src/ce_float_class.h.
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
HELPER = SRC / "ce_float_class.h"
CALCENGINE_H = SRC / "calcengine.h"

# Raw C99 classification macros and the CEdev entry points behind them, as
# bare identifiers: this also catches (isinf)(x), `#define CLS isinf` and
# function pointers such as `= _isinff`.
RAW = re.compile(
    r"(?<![\w])(isnan|isinf|isfinite|isnormal|fpclassify|"
    r"_isnanf|_isinff|_isfinitef|_isnormalf|__builtin_isnan|__builtin_isinf|"
    r"__builtin_isfinite|__builtin_isinf_sign|__builtin_fpclassify)(?![\w])"
)


def _strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literals, keeping length and newlines."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _raw_hits(text: str) -> list[tuple[int, str]]:
    """Sorted (line, name) of every raw classifier identifier in C source.

    As the preprocessor does, C line splices (backslash, optional trailing
    blanks, newline; GCC accepts the blanks) are removed first, so that
    `is\\<newline>inf(v)` and a comment delimiter split by a splice are seen
    the way the compiler sees them. Comments and literals are then blanked.
    Hits are reported at the original line where the identifier starts."""
    assert "\x00" not in text, "NUL byte in C source"
    marked = re.sub(r"\\[ \t]*\n", "\x00", text)   # one marker per splice
    joined: list[str] = []
    lines: list[int] = []                       # original line of joined[i]
    line = 1
    for ch in marked:
        if ch == "\x00":
            line += 1
            continue
        joined.append(ch)
        lines.append(line)
        if ch == "\n":
            line += 1
    code = _strip_comments_and_strings("".join(joined))   # keeps length
    return sorted({(lines[m.start()], m.group(1)) for m in RAW.finditer(code)})


def _sources() -> list[Path]:
    return sorted(p for p in SRC.rglob("*") if p.suffix in (".c", ".h") and p.is_file())


def test_no_raw_float_classification_macros_in_src() -> None:
    hits: list[str] = []
    for path in _sources():
        if path == HELPER:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_no, name in _raw_hits(text):
            hits.append(f"{path.relative_to(ROOT)}:{line_no}: {name}")
    assert hits == [], (
        "use ce_isnan/ce_isinf/ce_isfinite (src/ce_float_class.h); CEdev v11.2 "
        "isinf() is wrong for many finite values:\n" + "\n".join(hits)
    )


def test_helpers_are_bit_tests_and_reach_every_file() -> None:
    helper = HELPER.read_text(encoding="utf-8")
    code = _strip_comments_and_strings(helper)
    # The CE branch must not fall back to the libc macros.
    ce_branch = code.split("#if defined(__ez80__)", 1)[1].split("#else", 1)[0]
    for name in ("ce_isnan", "ce_isinf", "ce_isfinite"):
        assert re.search(rf"#define {name}\(x\)\s+{name}_f\(\(float\)\(x\)\)", ce_branch), name
    assert not RAW.search(ce_branch)
    # The helper bodies use the bit pattern only, never a float compare.
    for fn in ("ce_isnan_f", "ce_isinf_f", "ce_isfinite_f"):
        body = re.search(rf"static inline int {fn}\(float x\)\s*\{{(.*?)\n\}}", code, re.S)
        assert body, fn
        assert "u.f = x;" in body.group(1)
        assert not RAW.search(body.group(1))
    # calcengine.h pulls the helper in right after <math.h>, so every file
    # that can see the raw macros can also see ce_is*.
    ce_h = CALCENGINE_H.read_text(encoding="utf-8")
    assert ce_h.index('#include "ce_float_class.h"') > ce_h.index("#include <math.h>")


def test_guard_catches_raw_calls() -> None:
    sample = (
        "/* isinf(x) in a comment is fine */\n"
        'const char *s = "isnan(";\n'
        "if (ce_isinf(v)) {}\n"
        "if (isinf (v)) {}\n"
        "y = my_isnan(v) + !isfinite(v);\n"
        "y = (isnan)(v);\n"
        "#define CLS isinf\n"
        "int (*fp)(float) = _isinff;\n"
        'const char *t = "a\\\nb"; z = isfinite(w);\n'
    )
    sample += (
        "if (isinf\n(v)) {}\n"          # lines 11-12
        "#define BAD isnan\n"            # line 13
        "if (is\\\ninf(v)) {}\n"       # lines 14-15: a line splice
        "y = isnan(v);\n"                # line 16: mapping after a splice
        "/\\\n* isinf in a spliced comment */\n"   # lines 17-18: not code
        "x = 1; /* a */ y = is\\ \nfinite(v);\n"   # lines 19-20: GCC splice with a blank
        "/* safe *\\\n/ z = isinf(v);\n"            # lines 21-22: spliced */ ends the comment
        's = "q\\\\\n"isnan(v)";\n'                # lines 23-24: spliced \" keeps the string open
    )
    code = _strip_comments_and_strings(sample)
    assert code.count("\n") == sample.count("\n")
    assert _raw_hits(sample) == [
        (4, "isinf"), (5, "isfinite"), (6, "isnan"), (7, "isinf"), (8, "_isinff"),
        (10, "isfinite"), (11, "isinf"), (13, "isnan"), (14, "isinf"), (16, "isnan"),
        (19, "isfinite"), (22, "isinf"),
    ]
