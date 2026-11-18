from __future__ import annotations

import json
from pathlib import Path

import pytest

from cemu_runner import CEmuRunner
from pipeline_support import DEFAULT_BOOT_SEQUENCE, get_real_cemu_status


REAL_CEMU_READY, REAL_CEMU_SKIP_REASON = get_real_cemu_status()


def _assert_transfer_snapshot(
    snapshot_path: Path,
    expected_transfer_names: list[str],
    expected_module_names: set[str],
    expected_runtime_names: set[str],
) -> None:
    payload = json.loads(snapshot_path.read_text(encoding="utf-8"))
    ordered_transfer_names = [Path(path).name for path in payload["transfer_files"]]
    transfer_names = set(ordered_transfer_names)

    assert ordered_transfer_names == expected_transfer_names
    assert expected_module_names.issubset(transfer_names)
    assert expected_runtime_names.issubset(transfer_names)
    assert len({name for name in transfer_names if name.lower().endswith(".8xp")}) == len(expected_module_names)
    assert len({name for name in transfer_names if name.lower().endswith(".8xv")}) == len(expected_runtime_names)


def test_cemu_loads_manifest_modules_into_fake_archive(
    fake_cemu_runner: CEmuRunner,
    staged_transfer_package_dir: Path,
    resolved_transfer_package,
    tmp_path: Path,
) -> None:
    fake_cemu_runner.boot(
        program_paths=[staged_transfer_package_dir],
        ready_sequence=DEFAULT_BOOT_SEQUENCE,
    )
    snapshot_path = tmp_path / "fake-transfer-snapshot.json"
    fake_cemu_runner.snapshot(snapshot_path)

    state = fake_cemu_runner.get_state()
    assert state.payload["app_count"] == 39

    _assert_transfer_snapshot(
        snapshot_path=snapshot_path,
        expected_transfer_names=sorted(
            [*resolved_transfer_package.module_names, *resolved_transfer_package.runtime_names],
            key=str.lower,
        ),
        expected_module_names=set(resolved_transfer_package.module_names),
        expected_runtime_names=set(resolved_transfer_package.runtime_names),
    )


@pytest.mark.skipif(not REAL_CEMU_READY, reason=REAL_CEMU_SKIP_REASON)
def test_cemu_loads_all_35_modules_on_real_backend(
    real_cemu_config,
    staged_transfer_package_dir: Path,
    resolved_transfer_package,
    tmp_path: Path,
) -> None:
    assert real_cemu_config is not None

    with CEmuRunner(real_cemu_config) as runner:
        runner.boot(
            program_paths=[staged_transfer_package_dir],
            ready_sequence=DEFAULT_BOOT_SEQUENCE,
        )
        snapshot_path = tmp_path / "real-transfer-snapshot.json"
        runner.snapshot(snapshot_path)
        state = runner.get_state()

    assert state.payload["app_count"] == 39
    _assert_transfer_snapshot(
        snapshot_path=snapshot_path,
        expected_transfer_names=sorted(
            [*resolved_transfer_package.module_names, *resolved_transfer_package.runtime_names],
            key=str.lower,
        ),
        expected_module_names=set(resolved_transfer_package.module_names),
        expected_runtime_names=set(resolved_transfer_package.runtime_names),
    )
