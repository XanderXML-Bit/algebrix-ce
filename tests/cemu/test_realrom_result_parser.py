"""realrom.parse_response: g_response images are trusted only with a valid IPC header."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/emulator/linux-realrom"))

import realrom  # noqa: E402

VERSION = realrom.ipc_version(ROOT)
TEXT = b"Best: integration by parts\n1) integration by parts (score 80): matched by CEIPRT"


def _image(magic: int = realrom.CE_IPC_MAGIC, version: int = VERSION, ok: int = 1,
           body: bytes = TEXT) -> bytes:
    header = bytes([magic & 0xFF, magic >> 8, version, ok, 0]) + bytes(25)  # 30 B on the eZ80
    return (header + body + b"\0").ljust(realrom.RESULT_WINDOW, b"\0")


def test_valid_response_text() -> None:
    r = realrom.parse_response(_image(), "Best: ", VERSION)
    assert r == {"fresh": True, "ok": True, "text": TEXT.decode()}


def test_cleared_magic_is_stale() -> None:
    r = realrom.parse_response(_image(magic=0), "Best: ", VERSION)
    assert r == {"fresh": False, "ok": False, "text": ""}


def test_wrong_ipc_version_is_rejected() -> None:
    r = realrom.parse_response(_image(version=99), "Best: ", VERSION)
    assert r == {"fresh": False, "ok": False, "text": ""}


def test_error_response_and_missing_prefix() -> None:
    r = realrom.parse_response(_image(ok=0, body=b"Strategy report lost its progress"), "Best: ", VERSION)
    assert r == {"fresh": True, "ok": False, "text": ""}


def test_prefix_outside_header_window_ignored() -> None:
    r = realrom.parse_response(_image(body=bytes(80) + TEXT), "Best: ", VERSION)
    assert r["fresh"] and r["text"] == ""
