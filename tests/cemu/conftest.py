from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from cemu_runner import CEmuRunner, CEmuRunnerConfig
from fake_backend import build_fake_cemu_runner, build_fake_cemu_server_script
from pipeline_support import (
    build_runner_config,
    resolve_real_cemu_environment,
    resolve_transfer_package,
    stage_transfer_package,
)


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    )


def write_png(path: Path, width: int, height: int, rgba_pixels: bytes) -> None:
    rows = []
    stride = width * 4
    for row in range(height):
        offset = row * stride
        rows.append(b"\x00" + rgba_pixels[offset : offset + stride])
    compressed = zlib.compress(b"".join(rows))
    payload = (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + _png_chunk(b"IDAT", compressed)
        + _png_chunk(b"IEND", b"")
    )
    path.write_bytes(payload)


@pytest.fixture()
def temp_rom_path(tmp_path: Path) -> Path:
    rom_path = tmp_path / "Rom.rom"
    rom_path.write_bytes(b"FAKE-ROM")
    return rom_path


@pytest.fixture()
def temp_program_path(tmp_path: Path) -> Path:
    program_path = tmp_path / "CALCENG.8xp"
    program_path.write_bytes(b"FAKE-8XP")
    return program_path


@pytest.fixture()
def fake_cemu_server_script(tmp_path: Path) -> Path:
    return build_fake_cemu_server_script(tmp_path / "fake_cemu_server.py")


@pytest.fixture()
def fake_cemu_runner(
    tmp_path: Path,
    temp_rom_path: Path,
) -> CEmuRunner:
    runner = build_fake_cemu_runner(tmp_path, temp_rom_path)
    yield runner
    runner.shutdown()


@pytest.fixture()
def sample_image_paths(tmp_path: Path) -> dict[str, Path]:
    rgba_a = bytes(
        [
            255, 0, 0, 255,
            0, 255, 0, 255,
            0, 0, 255, 255,
            255, 255, 255, 255,
        ]
    )
    rgba_b = bytes(
        [
            255, 0, 0, 255,
            0, 255, 0, 255,
            0, 0, 0, 255,
            255, 255, 255, 255,
        ]
    )

    png_a = tmp_path / "screen-a.png"
    png_b = tmp_path / "screen-b.png"
    write_png(png_a, width=2, height=2, rgba_pixels=rgba_a)
    write_png(png_b, width=2, height=2, rgba_pixels=rgba_b)

    ppm_a = tmp_path / "screen-a.ppm"
    ppm_a.write_bytes(
        b"P6\n2 2\n255\n"
        + bytes(
            [
                255, 0, 0,
                0, 255, 0,
                0, 0, 255,
                255, 255, 255,
            ]
        )
    )

    text_json = tmp_path / "screen-text.json"
    text_json.write_text(
        json.dumps(
            {
                "screen": {
                    "text_rows": [
                        "CALCENGINE READY",
                        "RESULT: 5      ",
                        "STATUS: PASS   ",
                    ]
                }
            }
        ),
        encoding="utf-8",
    )

    return {"png_a": png_a, "png_b": png_b, "ppm_a": ppm_a, "text_json": text_json}


@pytest.fixture(scope="session")
def resolved_transfer_package():
    package = resolve_transfer_package(repo_root=REPO_ROOT)
    if not package.ready:
        pytest.skip(
            "CalcEngine transfer package is incomplete: "
            + ", ".join(package.missing_entries)
        )
    return package


@pytest.fixture()
def staged_transfer_package_dir(tmp_path: Path, resolved_transfer_package) -> Path:
    staging_dir = tmp_path / "transfer"
    stage_transfer_package(resolved_transfer_package, staging_dir)
    return staging_dir


@pytest.fixture(scope="session")
def real_cemu_environment():
    return resolve_real_cemu_environment(repo_root=REPO_ROOT)


@pytest.fixture(scope="session")
def real_cemu_config(real_cemu_environment) -> CEmuRunnerConfig | None:
    if not real_cemu_environment.ready:
        return None
    return build_runner_config(real_cemu_environment)
