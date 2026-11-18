"""Compile the real IPC implementation and sweep returned save failures."""
from pathlib import Path
import os
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which("gcc")


@pytest.mark.skipif(not CC, reason="requires native gcc")
@pytest.mark.parametrize("sanitized", [False, True])
@pytest.mark.parametrize("fixture", ["state_save_test", "state_runtime_test"])
def test_state_save_failure_sweep(tmp_path: Path, sanitized: bool, fixture: str) -> None:
    assert CC is not None
    binary = tmp_path / "state_save_test"
    flags = ["-std=c99", "-O0", "-g", "-DDESKTOP_BUILD", "-ffunction-sections",
             "-fdata-sections", "-Isrc", "-Itests/mock"]
    if sanitized:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer"]
    sources = [f"tests/{fixture}.c", "src/split/ipc.c", "tests/mock/fileioc_mock.c"]
    if fixture == "state_runtime_test":
        sources += ["src/split/module_runtime.c", "src/split/module_forward.c"]
    subprocess.run([CC, *flags, *sources, "-Wl,--gc-sections", "-lm", "-o", str(binary)],
                   cwd=ROOT, check=True, capture_output=True, text=True)
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:detect_leaks=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, env=env)
    assert "0 failed" in result.stdout, result.stdout
