"""Static guards for CE float behaviour the desktop suite cannot see.

On the real OS 5.8 ROM:

* CEdev v11.2's exp2f computes expf(x * log2(e)) instead of expf(x * ln 2), so
  exp2f(1) is 4.23. clang turns powf(2.0f, x) into exp2f(x), which made
  chi2cdf(0,5,3) show 0.269043 instead of 0.828. Desktop libm is correct.
* The CE's float routines wrap on overflow (1E38*10 gave 2.35e-38). CEBASE,
  which evaluates plain arithmetic, builds the overflow-safe helpers in
  src/ce_float_ops.h with CE_SAFE_FLOAT=1; without that flag it silently
  falls back to the native operations.
"""
from __future__ import annotations

import importlib.util
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

_spec = importlib.util.spec_from_file_location(
    "ce_float_class_guard", Path(__file__).with_name("test_ce_float_class_guard.py"))
assert _spec is not None and _spec.loader is not None
_guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_guard)

# exp2/exp2f/exp2l calls, and pow/powf with a literal base of 2 (which clang
# turns into exp2f): 2, 2., 2.0f, 2e0, 0x1p1. A base that is 2 only through
# a macro, a const or inlining is not caught here; test_built_maps_match is
# the backstop for those (it needs a CE build under bin/).
EXP2 = re.compile(r"(?<![\w])(exp2|exp2f|exp2l)\s*\(")
POW2 = re.compile(r"(?<![\w])(pow|powf|powl)\s*\(\s*"
                  r"(2(\.0*)?([eE][+-]?0+)?|0[xX]1(\.0*)?[pP]\+?1)[fFlL]?\s*,")


def _code(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace")
    return _guard._strip_comments_and_strings(text.replace("\\\n", ""))


def _hits(text: str) -> list[str]:
    return [m.group(0) for rx in (EXP2, POW2) for m in rx.finditer(text)]


def test_no_exp2_in_src():
    bad = []
    for path in sorted(SRC.rglob("*.[ch]")):
        for hit in _hits(_code(path)):
            bad.append(f"{path.relative_to(ROOT)}: {hit}")
    assert not bad, ("CEdev exp2f is wrong on the CE; write expf(x * 0.693147182f): "
                     + "; ".join(bad))


def test_guard_catches_exp2_forms():
    sample = ("a = powf(2.0f, k); b = pow( 2 , x); c = exp2f(x); d = powf(20.0f, x); e = myexp2(x);"
              " f = powf(2e0f, x); g = powf(0x1p1f, x); h = powf(2.5f, x); i = powf(0x1p10f, x);")
    assert _hits(sample) == ["exp2f(", "powf(2.0f,", "pow( 2 ,", "powf(2e0f,", "powf(0x1p1f,"]


def test_cebase_builds_safe_float_ops():
    mk = (ROOT / "split" / "Makefile.base").read_text(encoding="utf-8")
    cflags = mk[mk.index("PROGRAM_CFLAGS"):mk.index("PROGRAM_SOURCES")]
    assert "-DCE_SAFE_FLOAT=1" in cflags


def test_safe_float_default_is_off_for_ce_modules():
    header = (SRC / "ce_float_ops.h").read_text(encoding="utf-8")
    assert re.search(r"#if defined\(__ez80__\) \|\| defined\(_EZ80\) \|\| defined\(HW_BUILD_SPLIT\)\s*\n#define CE_SAFE_FLOAT 0", header)


def test_built_maps_match():
    """When the .8xp set is built: CEBASE links ce_farith, nothing links exp2f."""
    maps = sorted(ROOT.glob("bin/*/*.map"))
    if not maps:
        pytest.skip("no CE build under bin/")
    names = {m.stem: m.read_text(encoding="utf-8", errors="replace") for m in maps}
    if "CEBASE" in names:
        assert re.search(r"^_ce_farith\s+=", names["CEBASE"], re.M)
    linked = [n for n, text in names.items() if re.search(r"^_exp2f\s+=", text, re.M)]
    assert not linked, f"modules linking CEdev exp2f: {linked}"
