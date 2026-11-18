"""Run the production finite-sum helper under measured CE add semantics.

Only evaluator boundaries are doubled. The helper definition is extracted
unchanged from eval.c and compiled with UBSan. No ROM is needed for these tests.
"""
from __future__ import annotations

import ctypes
import json
import math
import re
import shutil
import struct
import subprocess
from fractions import Fraction
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def production_helper():
    source = (ROOT / "src/eval.c").read_text()
    start = source.index("static __attribute__((noinline)) ce_float_t eval_sum_compensated(")
    end = source.index("\n}\n#endif", start) + 2
    return source[start:end]


def build_sum(out, helper=None):
    out.mkdir(parents=True, exist_ok=True)
    (out / "sum_production.inc").write_text(production_helper() if helper is None else helper)
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("needs a host C compiler")
    library = out / "sum.so"
    subprocess.run([cc, "-std=c99", "-O1", "-g", "-shared", "-fPIC",
                    "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I" + str(out), "-I" + str(ROOT / "tests/harness"),
                    str(ROOT / "tests/harness/sum_compensated_harness.c"),
                    "-lm", "-o", str(library)], check=True, capture_output=True, text=True)
    lib = ctypes.CDLL(str(library))
    lib.sum_run.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                           ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_uint)]
    lib.sum_add_bits.argtypes = [ctypes.c_uint, ctypes.c_uint]
    lib.sum_add_bits.restype = ctypes.c_uint
    return lib


@pytest.fixture(scope="module")
def sum_lib(tmp_path_factory):
    return build_sum(tmp_path_factory.mktemp("sum_ce"))


def run_sum(lib, terms, cancel=0, fail=-1):
    ts = (ctypes.c_float * len(terms))(*terms)
    out = (ctypes.c_uint * 6)()
    lib.sum_run(ts, len(terms), cancel, fail, out)
    return list(out)


def assert_bound(terms, observed):
    ts = list(map(Fraction, terms))
    total, absolute = sum(ts), sum(map(abs, ts))
    value = struct.unpack("<f", struct.pack("<I", observed))[0]
    assert math.isfinite(value)
    assert abs(Fraction(value) - total) <= (4 * abs(total) + absolute) / 2**24


def contract_terms():
    return {
        "row1": list(map(float, range(1, 10001))),
        "row2": list(map(float, range(0, 10001))),
        "row3": list(map(float, range(1, 10002))),
        "row4": [1.0] * 10001,
        "row5": [float(n*n) for n in range(1, 1001)],
        "row6": [float(n**3) for n in range(1, 101)],
        "row7": [f32(0.1)] * 10000,
        "row8": [f32(1/n) for n in range(1, 10001)],
        "row9": [f32(1/f32(n*n)) for n in range(1, 10001)],
        "row10": [f32((-1)**n/n) for n in range(1, 10001)],
        "row11": [float((-1)**n*n) for n in range(1, 10001)],
        "row12": [float(n-5000) for n in range(1, 10001)],
        "row13": [float(n*(n+1)//2) for n in range(1, 101)],
        "fallback200": [16777215.0] * 200,
        "fallback10001": [16777215.0] * 10001,
        "mixed": [1e7] + [f32(0.01)] * 9999,
        "cancel": [f32((-1)**n*(1+n/10000)) for n in range(1, 10001)],
        "residual": [8388609.0, 8388608.0, 0.25, -8388608.0, -8388608.0],
        "odd_transition": [3.0, 0.25],
        "tie_even": [8388608.0, 8388609.0],
        "tie_odd": [8388609.0, 8388610.0],
        "negative_tie": [-8388608.0, -8388609.0],
        "edge_positive": [16777215.0]*128 + [126.0, 0.25],
        "edge_negative": [-16777215.0]*128 + [-126.0, -0.25],
    }


@pytest.mark.parametrize("name", list(contract_terms()))
def test_sum_accuracy(sum_lib, name):
    terms = contract_terms()[name]
    out = run_sum(sum_lib, terms)
    assert out[1] == 0, (name, out)
    assert out[2] == len(terms)
    assert out[4:] == [bits(37), 1], "restore outer variable binding"
    assert_bound(terms, out[0])
    if name.startswith("row") and int(name[3:]) not in (7, 8, 9, 10):
        assert out[0] == bits(sum(terms)), (name, out)
    if name in ("tie_even", "tie_odd", "negative_tie"):
        assert out[0] == bits(sum(terms))
    if name == "residual":
        assert out[0] == bits(1.0)


def test_ce_add_matches_798_owned_rom_samples(sum_lib):
    samples = json.loads((ROOT / "tests/harness/sum_ce_add_samples.json").read_text())
    assert len(samples) == 798
    for a, b, expected in samples:
        assert sum_lib.sum_add_bits(a, b) == expected, (a, b, expected)


def test_cadence_and_first_error(sum_lib):
    ts = [1.0] * 10001
    cancelled = run_sum(sum_lib, ts, cancel=2)
    assert cancelled[1:4] == [3, 256, 2]
    failed = run_sum(sum_lib, ts, cancel=2, fail=5)
    assert failed[1:4] == [7, 6, 1]
    assert cancelled[4:] == failed[4:] == [bits(37), 1]
    recovered = run_sum(sum_lib, [2.0, 3.0])
    assert recovered[:3] == [bits(5), 0, 2]
    capped = run_sum(sum_lib, [1.0] * 10002)
    assert capped[1:3] == [2, 0]


@pytest.mark.parametrize("terms", [[float("inf")], [float("nan")], [f32(3.4e38)] * 2])
def test_sum_overflow_first_error(sum_lib, terms):
    out = run_sum(sum_lib, terms)
    assert out[1] == 2
    assert out[4:] == [bits(37), 1]


@pytest.mark.parametrize("mutant", ["naive", "no_integer", "no_compensation", "no_guard", "no_residual"])
def test_negative_control(tmp_path, mutant):
    helper = production_helper()
    if mutant == "naive":
        helper = helper.replace("bool exact = true;", "bool exact = false;")
        helper = helper.replace("comp = CE_SUM_FADD(CE_SUM_FADD(next, -total), -y);", "comp = 0;")
    elif mutant == "no_integer":
        helper = helper.replace("bool exact = true;", "bool exact = false;")
    elif mutant == "no_compensation":
        helper = helper.replace("comp = CE_SUM_FADD(CE_SUM_FADD(next, -total), -y);", "comp = 0;")
    elif mutant == "no_guard":
        helper = re.sub(r"\(ti >= 0 \? integer <= 2147483647L - ti\s*: integer >= -2147483647L - ti\)", "true", helper)
    else:
        helper = helper.replace("comp = -(ce_float_t)residual;", "comp = 0; (void)residual;")
    assert helper != production_helper(), mutant
    lib = build_sum(tmp_path, helper)
    cases = contract_terms()
    killed = []
    for name in ["row1", "row5", "row6", "row7", "row8", "row9", "fallback200", "fallback10001", "residual"]:
        terms = cases[name]
        out = run_sum(lib, terms)
        try:
            assert out[1] == 0
            assert_bound(terms, out[0])
            if name in ("row5", "row6"):
                assert out[0] == bits(sum(terms))
            if name == "residual":
                assert out[0] == bits(1.0)
        except AssertionError:
            killed.append(name)
    assert killed, mutant
    print(mutant, "rejected by", killed)
