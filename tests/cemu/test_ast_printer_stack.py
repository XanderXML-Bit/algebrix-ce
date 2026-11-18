"""Exercise the production printer with desktop and shipped-module flags."""
from pathlib import Path
import subprocess

import pytest

from test_module_flag_parity import ROOT, SPLIT, _host_cc, _makefile_vars


@pytest.mark.parametrize("module", [None, "calculus", "calc.int.simple"])
def test_printer_has_one_frame_at_pool_limit(tmp_path: Path, module: str | None):
    cc = _host_cc()
    if cc is None:
        pytest.skip("GNU-compatible host compiler unavailable")
    flags = [] if module is None else _makefile_vars(
        SPLIT / f"Makefile.{module}"
    )["PROGRAM_CFLAGS"].split()
    executable = tmp_path / "ast-printer-stack"
    command = [
        cc, "-std=c99", "-O0", "-g", "-finstrument-functions",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-DDESKTOP_BUILD", *flags, "-DCE_FMT_REDIRECT", "-DCE_FMT_REAL=float",
        "-DCE_FLOAT_CLASS_REDIRECT", "-include", str(ROOT / "src/ce_fmt.h"),
        "-I", str(ROOT / "src"), "-I", str(ROOT / "tests/mock"),
        str(ROOT / "tests/harness/ast_print_stack_harness.c"),
        str(ROOT / "src/memory.c"), str(ROOT / "src/ce_fmt.c"),
        "-lm", "-o", str(executable),
    ]
    subprocess.run(command, check=True, capture_output=True, text=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True,
                            text=True, timeout=120)
    assert "bounded printer:" in result.stdout
    assert result.stdout.rstrip().endswith("PASS")
