from __future__ import annotations

import binascii
import hashlib
import json
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class ScreenCaptureError(RuntimeError):
    """Base exception for screen capture utility failures."""


@dataclass(frozen=True)
class ScreenRect:
    left: int
    top: int
    width: int
    height: int


@dataclass(frozen=True)
class ScreenImage:
    width: int
    height: int
    pixels_rgba: bytes
    source_path: Path

    @property
    def pixel_count(self) -> int:
        return self.width * self.height

    def sha256(self) -> str:
        return hashlib.sha256(self.pixels_rgba).hexdigest()

    def crc32(self) -> str:
        return f"{binascii.crc32(self.pixels_rgba) & 0xFFFFFFFF:08X}"


@dataclass(frozen=True)
class ImageComparisonResult:
    matches: bool
    mismatch_pixels: int
    total_pixels: int
    mismatch_ratio: float
    actual_crc32: str
    expected_crc32: str


@dataclass(frozen=True)
class TextGrid:
    rows: tuple[str, ...]
    source_path: Path | None = None


@dataclass(frozen=True)
class TextRegion:
    rect: ScreenRect
    rows: tuple[str, ...]

    @property
    def text(self) -> str:
        return "\n".join(self.rows)


def _read_token(data: bytes, start: int) -> tuple[bytes, int]:
    index = start
    while index < len(data):
        value = data[index]
        if value == 35:
            while index < len(data) and data[index] not in (10, 13):
                index += 1
            continue
        if chr(value).isspace():
            index += 1
            continue
        break

    end = index
    while end < len(data) and not chr(data[end]).isspace():
        end += 1
    return data[index:end], end


def _ensure_rgba(width: int, height: int, pixels: bytes, channels: int) -> bytes:
    if channels == 4:
        return pixels
    converted = bytearray(width * height * 4)
    for index in range(width * height):
        src = index * channels
        dst = index * 4
        if channels == 3:
            converted[dst : dst + 3] = pixels[src : src + 3]
            converted[dst + 3] = 255
            continue
        if channels == 1:
            converted[dst] = pixels[src]
            converted[dst + 1] = pixels[src]
            converted[dst + 2] = pixels[src]
            converted[dst + 3] = 255
            continue
        raise ScreenCaptureError(f"Unsupported channel count: {channels}")
    return bytes(converted)


def load_image(path: str | Path) -> ScreenImage:
    artifact_path = Path(path)
    raw = artifact_path.read_bytes()

    if raw.startswith(PNG_SIGNATURE):
        width, height, pixels = _decode_png(raw)
        return ScreenImage(width=width, height=height, pixels_rgba=pixels, source_path=artifact_path)

    if raw.startswith(b"P6") or raw.startswith(b"P3"):
        width, height, pixels = _decode_ppm(raw)
        return ScreenImage(width=width, height=height, pixels_rgba=pixels, source_path=artifact_path)

    raise ScreenCaptureError(f"Unsupported screen artifact format: {artifact_path}")


def _decode_ppm(raw: bytes) -> tuple[int, int, bytes]:
    magic, index = _read_token(raw, 0)
    width_token, index = _read_token(raw, index)
    height_token, index = _read_token(raw, index)
    max_value_token, index = _read_token(raw, index)

    width = int(width_token)
    height = int(height_token)
    max_value = int(max_value_token)
    if max_value != 255:
        raise ScreenCaptureError("Only 8-bit PPM images are supported.")

    while index < len(raw) and chr(raw[index]).isspace():
        index += 1

    if magic == b"P6":
        rgb = raw[index : index + width * height * 3]
    elif magic == b"P3":
        values: list[int] = []
        current = index
        while len(values) < width * height * 3:
            token, current = _read_token(raw, current)
            if not token:
                break
            values.append(int(token))
        rgb = bytes(values)
    else:
        raise ScreenCaptureError("Unsupported PPM magic number.")

    if len(rgb) != width * height * 3:
        raise ScreenCaptureError("PPM pixel data length does not match dimensions.")
    return width, height, _ensure_rgba(width, height, rgb, channels=3)


def _decode_png(raw: bytes) -> tuple[int, int, bytes]:
    if not raw.startswith(PNG_SIGNATURE):
        raise ScreenCaptureError("Invalid PNG signature.")

    offset = len(PNG_SIGNATURE)
    width = 0
    height = 0
    bit_depth = 0
    color_type = 0
    interlace_method = 0
    compressed = bytearray()

    while offset < len(raw):
        length = struct.unpack(">I", raw[offset : offset + 4])[0]
        chunk_type = raw[offset + 4 : offset + 8]
        chunk_data = raw[offset + 8 : offset + 8 + length]
        offset += 12 + length

        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _compression, _filter_method, interlace_method = struct.unpack(
                ">IIBBBBB",
                chunk_data,
            )
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if bit_depth != 8:
        raise ScreenCaptureError("Only 8-bit PNG images are supported.")
    if interlace_method != 0:
        raise ScreenCaptureError("Interlaced PNG images are not supported.")
    if color_type not in (0, 2, 6):
        raise ScreenCaptureError(f"Unsupported PNG color type: {color_type}")

    channels = {0: 1, 2: 3, 6: 4}[color_type]
    stride = width * channels
    decompressed = zlib.decompress(bytes(compressed))
    expected_length = height * (stride + 1)
    if len(decompressed) != expected_length:
        raise ScreenCaptureError("PNG decompressed payload length does not match IHDR dimensions.")

    rows = bytearray(height * stride)
    previous = bytearray(stride)
    source_index = 0

    for row_index in range(height):
        filter_type = decompressed[source_index]
        source_index += 1
        current = bytearray(decompressed[source_index : source_index + stride])
        source_index += stride

        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(channels, stride):
                current[i] = (current[i] + current[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                current[i] = (current[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = current[i - channels] if i >= channels else 0
                up = previous[i]
                current[i] = (current[i] + ((left + up) // 2)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = current[i - channels] if i >= channels else 0
                up = previous[i]
                up_left = previous[i - channels] if i >= channels else 0
                current[i] = (current[i] + _paeth_predictor(left, up, up_left)) & 0xFF
        else:
            raise ScreenCaptureError(f"Unsupported PNG filter type: {filter_type}")

        destination = row_index * stride
        rows[destination : destination + stride] = current
        previous = current

    return width, height, _ensure_rgba(width, height, bytes(rows), channels=channels)


def _paeth_predictor(left: int, up: int, up_left: int) -> int:
    prediction = left + up - up_left
    distance_left = abs(prediction - left)
    distance_up = abs(prediction - up)
    distance_up_left = abs(prediction - up_left)
    if distance_left <= distance_up and distance_left <= distance_up_left:
        return left
    if distance_up <= distance_up_left:
        return up
    return up_left


def compare_images(
    actual: ScreenImage,
    expected: ScreenImage,
    tolerance_per_channel: int = 0,
    allowed_mismatch_pixels: int = 0,
) -> ImageComparisonResult:
    if actual.width != expected.width or actual.height != expected.height:
        raise ScreenCaptureError(
            f"Image dimensions differ: actual {actual.width}x{actual.height}, "
            f"expected {expected.width}x{expected.height}"
        )

    mismatch_pixels = 0
    for offset in range(0, len(actual.pixels_rgba), 4):
        pixel_mismatch = False
        for channel in range(4):
            if abs(actual.pixels_rgba[offset + channel] - expected.pixels_rgba[offset + channel]) > tolerance_per_channel:
                pixel_mismatch = True
                break
        if pixel_mismatch:
            mismatch_pixels += 1

    total_pixels = actual.pixel_count
    return ImageComparisonResult(
        matches=mismatch_pixels <= allowed_mismatch_pixels,
        mismatch_pixels=mismatch_pixels,
        total_pixels=total_pixels,
        mismatch_ratio=(mismatch_pixels / total_pixels) if total_pixels else 0.0,
        actual_crc32=actual.crc32(),
        expected_crc32=expected.crc32(),
    )


def assert_images_match(
    actual_path: str | Path,
    expected_path: str | Path,
    tolerance_per_channel: int = 0,
    allowed_mismatch_pixels: int = 0,
) -> ImageComparisonResult:
    actual = load_image(actual_path)
    expected = load_image(expected_path)
    result = compare_images(
        actual=actual,
        expected=expected,
        tolerance_per_channel=tolerance_per_channel,
        allowed_mismatch_pixels=allowed_mismatch_pixels,
    )
    if not result.matches:
        raise AssertionError(
            "Image comparison failed: "
            f"{result.mismatch_pixels}/{result.total_pixels} pixels differ "
            f"(actual CRC32 {result.actual_crc32}, expected CRC32 {result.expected_crc32})."
        )
    return result


def load_text_grid(path: str | Path) -> TextGrid:
    artifact_path = Path(path)
    if artifact_path.suffix.lower() == ".txt":
        return TextGrid(rows=tuple(artifact_path.read_text(encoding="utf-8").splitlines()), source_path=artifact_path)

    if artifact_path.suffix.lower() == ".json":
        payload = json.loads(artifact_path.read_text(encoding="utf-8"))
        rows = _extract_text_rows(payload)
        return TextGrid(rows=tuple(rows), source_path=artifact_path)

    raise ScreenCaptureError(f"Unsupported text grid format: {artifact_path}")


def _extract_text_rows(payload: Mapping[str, object]) -> Iterable[str]:
    if "text_rows" in payload and isinstance(payload["text_rows"], list):
        return [str(row) for row in payload["text_rows"]]
    if "ascii_text" in payload and isinstance(payload["ascii_text"], str):
        return payload["ascii_text"].splitlines()
    if "screen" in payload and isinstance(payload["screen"], Mapping):
        return _extract_text_rows(payload["screen"])
    raise ScreenCaptureError("JSON text artifact does not contain 'text_rows' or 'ascii_text'.")


def extract_text_region(grid: TextGrid, rect: ScreenRect) -> TextRegion:
    if rect.left < 0 or rect.top < 0 or rect.width <= 0 or rect.height <= 0:
        raise ScreenCaptureError("Text region bounds must be positive and non-negative.")

    rows: list[str] = []
    for row_index in range(rect.top, rect.top + rect.height):
        source_row = grid.rows[row_index] if row_index < len(grid.rows) else ""
        rows.append(source_row[rect.left : rect.left + rect.width])
    return TextRegion(rect=rect, rows=tuple(rows))


def assert_text_region_contains(
    grid: TextGrid,
    rect: ScreenRect,
    expected_text: str,
    case_sensitive: bool = False,
) -> TextRegion:
    region = extract_text_region(grid, rect)
    haystack = region.text if case_sensitive else region.text.lower()
    needle = expected_text if case_sensitive else expected_text.lower()
    if needle not in haystack:
        raise AssertionError(
            f"Expected text '{expected_text}' was not found in region {rect}: {region.text!r}"
        )
    return region
