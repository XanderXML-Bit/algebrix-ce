from __future__ import annotations

from pathlib import Path

import pytest

from screen_capture import (
    ScreenRect,
    assert_images_match,
    assert_text_region_contains,
    compare_images,
    extract_text_region,
    load_image,
    load_text_grid,
)


def test_png_and_ppm_with_identical_pixels_match(sample_image_paths: dict[str, Path]) -> None:
    png_image = load_image(sample_image_paths["png_a"])
    ppm_image = load_image(sample_image_paths["ppm_a"])

    result = compare_images(actual=png_image, expected=ppm_image)
    assert result.matches is True
    assert result.mismatch_pixels == 0


def test_image_difference_is_reported(sample_image_paths: dict[str, Path]) -> None:
    result = assert_images_match(sample_image_paths["png_a"], sample_image_paths["png_a"])
    assert result.matches is True

    with pytest.raises(AssertionError):
        assert_images_match(sample_image_paths["png_a"], sample_image_paths["png_b"])


def test_text_region_extraction_and_pattern_assertion(sample_image_paths: dict[str, Path]) -> None:
    grid = load_text_grid(sample_image_paths["text_json"])
    region = extract_text_region(grid, ScreenRect(left=0, top=1, width=9, height=1))
    assert region.text == "RESULT: 5"

    matched_region = assert_text_region_contains(
        grid,
        ScreenRect(left=0, top=2, width=12, height=1),
        expected_text="pass",
        case_sensitive=False,
    )
    assert matched_region.text.strip() == "STATUS: PASS"
