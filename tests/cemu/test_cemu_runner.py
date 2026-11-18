from __future__ import annotations

import cemu_runner as cemu_runner_module
import shutil
import sys
from pathlib import Path

import pytest

from cemu_runner import (
    CEmuArchiveCapacityError,
    CEmuArchiveTransferError,
    CEmuProcessExitedError,
    CEmuRomLoadError,
    CEmuRunner,
    CEmuRunnerConfig,
)


def _build_runner(temp_rom_path: Path, script: str) -> CEmuRunner:
    config = CEmuRunnerConfig(
        executable_path=sys.executable,
        arguments=("-c", script),
        rom_path=temp_rom_path,
        startup_timeout_s=1.0,
        command_timeout_s=1.0,
        shutdown_timeout_s=0.5,
        boot_timeout_s=1.0,
    )
    return CEmuRunner(config=config)


def test_runner_reports_process_exit_before_ready(temp_rom_path: Path) -> None:
    runner = _build_runner(
        temp_rom_path,
        "import sys; print('boot failed', file=sys.stderr, flush=True); sys.exit(17)",
    )

    with pytest.raises(CEmuProcessExitedError, match="before reporting ready state"):
        runner.start()

    runner.shutdown()


def test_runner_reports_process_exit_while_waiting_for_initial_state(temp_rom_path: Path) -> None:
    runner = _build_runner(
        temp_rom_path,
        (
            "import json, sys\n"
            "print('fake-cemu ready', file=sys.stderr, flush=True)\n"
            "for raw_line in sys.stdin:\n"
            "    request = json.loads(raw_line)\n"
            "    if request['cmd'] == 'get_state':\n"
            "        print('fatal crash', file=sys.stderr, flush=True)\n"
            "        sys.exit(9)\n"
        ),
    )

    with pytest.raises(CEmuProcessExitedError, match="waiting for 'get_state'"):
        runner.start()

    runner.shutdown()


def test_runner_classifies_corrupted_rom_failures(temp_rom_path: Path) -> None:
    runner = _build_runner(
        temp_rom_path,
        (
            "import json, sys\n"
            "print('fake-cemu ready', file=sys.stderr, flush=True)\n"
            "for raw_line in sys.stdin:\n"
            "    request = json.loads(raw_line)\n"
            "    if request['cmd'] == 'get_state':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'get_state', 'rom_loaded': False, 'running': False, 'halted': False}), flush=True)\n"
            "    elif request['cmd'] == 'load_rom':\n"
            "        print(json.dumps({'ok': False, 'cmd': 'load_rom', 'error': 'ROM image is corrupt or invalid'}), flush=True)\n"
            "    elif request['cmd'] == 'exit':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'exit'}), flush=True)\n"
            "        break\n"
        ),
    )

    runner.start()
    with pytest.raises(CEmuRomLoadError, match="ROM image is corrupt or invalid"):
        runner.load_rom()
    runner.shutdown()


def test_runner_classifies_archive_capacity_failures(temp_rom_path: Path, temp_program_path: Path) -> None:
    runner = _build_runner(
        temp_rom_path,
        (
            "import json, sys\n"
            "print('fake-cemu ready', file=sys.stderr, flush=True)\n"
            "for raw_line in sys.stdin:\n"
            "    request = json.loads(raw_line)\n"
            "    if request['cmd'] == 'get_state':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'get_state', 'rom_loaded': True, 'running': True, 'halted': False}), flush=True)\n"
            "    elif request['cmd'] == 'load_app':\n"
            "        print(json.dumps({'ok': False, 'cmd': 'load_app', 'error': 'archive requires 8192 bytes, but capacity is limited to 4096 bytes'}), flush=True)\n"
            "    elif request['cmd'] == 'exit':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'exit'}), flush=True)\n"
            "        break\n"
        ),
    )

    runner.start()
    with pytest.raises(CEmuArchiveCapacityError, match="archive requires 8192 bytes"):
        runner.load_program(temp_program_path)
    runner.shutdown()


def test_runner_classifies_generic_archive_transfer_failures(temp_rom_path: Path, temp_program_path: Path) -> None:
    runner = _build_runner(
        temp_rom_path,
        (
            "import json, sys\n"
            "print('fake-cemu ready', file=sys.stderr, flush=True)\n"
            "for raw_line in sys.stdin:\n"
            "    request = json.loads(raw_line)\n"
            "    if request['cmd'] == 'get_state':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'get_state', 'rom_loaded': True, 'running': True, 'halted': False}), flush=True)\n"
            "    elif request['cmd'] == 'load_app':\n"
            "        print(json.dumps({'ok': False, 'cmd': 'load_app', 'error': 'transfer checksum mismatch for staged archive payload'}), flush=True)\n"
            "    elif request['cmd'] == 'exit':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'exit'}), flush=True)\n"
            "        break\n"
        ),
    )

    runner.start()
    with pytest.raises(CEmuArchiveTransferError, match="transfer checksum mismatch"):
        runner.load_program(temp_program_path)
    runner.shutdown()


def test_path_for_host_process_converts_wsl_mount_paths_for_windows_runner(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(cemu_runner_module, "_running_under_wsl", lambda: True)

    fake_exe = tmp_path / "fake-cemu.exe"
    rom_path = Path("/mnt/c/Users/user/AppData/Local/Temp/cemu-runner-path-interop/Rom.rom")
    autotester_path = Path("/mnt/c/Users/user/AppData/Local/Temp/cemu-runner-path-interop/autotester.exe")
    program_path = Path("/mnt/c/Users/user/AppData/Local/Temp/cemu-runner-path-interop/CALCENG.8xp")

    assert cemu_runner_module._path_for_host_process(rom_path, fake_exe).startswith("C:\\")
    assert "\\mnt\\" not in cemu_runner_module._path_for_host_process(rom_path, fake_exe)
    assert cemu_runner_module._path_for_host_process(autotester_path, fake_exe).startswith("C:\\")
    assert cemu_runner_module._path_for_host_process(program_path, fake_exe).startswith("C:\\")
    assert "\\mnt\\" not in cemu_runner_module._path_for_host_process(program_path, fake_exe)


def test_host_accessible_temp_roots_prefers_windows_mount_points(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(cemu_runner_module, "_running_under_wsl", lambda: True)

    roots = cemu_runner_module._host_accessible_temp_roots(
        Path("/mnt/c/Users/user/AppData/Local/Programs/cemu-headless.exe")
    )

    assert roots == (
        Path("/mnt/c/Users/user/AppData/Local/Temp"),
        Path("/mnt/c/Temp"),
        Path("/mnt/c/Users/user/AppData/Local/Programs"),
    )


def test_runner_creates_host_accessible_temp_dir_for_windows_binary(
    temp_rom_path: Path,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    windows_exe = Path("/mnt/c/Users/user/AppData/Local/Temp/fake-cemu.exe")
    runner = CEmuRunner(
        config=CEmuRunnerConfig(
            executable_path=windows_exe,
            rom_path=temp_rom_path,
        )
    )
    host_temp_root = tmp_path / "host-temp"
    monkeypatch.setattr(
        cemu_runner_module,
        "_host_accessible_temp_roots",
        lambda _executable_path: (host_temp_root,),
    )

    stage_dir = runner.create_host_accessible_temp_dir(prefix="calcengine-artifice-py-")
    try:
        assert stage_dir.is_dir()
        assert stage_dir.parent == host_temp_root
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)


def test_snapshot_copies_host_written_artifact_back_to_linux_path(
    temp_rom_path: Path,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake_exe = tmp_path / "fake-cemu.exe"
    fake_exe.write_text("", encoding="utf-8")
    host_temp_root = tmp_path / "host-temp"
    requested_snapshot_path = tmp_path / "linux-output" / "snapshot.state.json"

    monkeypatch.setattr(cemu_runner_module, "_running_under_wsl", lambda: True)
    monkeypatch.setattr(
        cemu_runner_module,
        "_host_accessible_temp_roots",
        lambda _executable_path: (host_temp_root,),
    )

    def _spawn_fake_process(command: list[str], **kwargs: object):
        script = (
            "import json, sys\n"
            "from pathlib import Path\n"
            "print('fake-cemu ready', file=sys.stderr, flush=True)\n"
            "for raw_line in sys.stdin:\n"
            "    request = json.loads(raw_line)\n"
            "    if request['cmd'] == 'get_state':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'get_state', 'rom_loaded': True, 'running': True, 'halted': False}), flush=True)\n"
            "    elif request['cmd'] == 'snapshot':\n"
            "        output_path = Path(request['path'])\n"
            "        output_path.parent.mkdir(parents=True, exist_ok=True)\n"
            "        output_path.write_text('{\"backend\":\"host-temp\"}', encoding='utf-8')\n"
            "        print(json.dumps({'ok': True, 'cmd': 'snapshot', 'type': 'state', 'path': str(output_path), 'crc': 'ABCD1234'}), flush=True)\n"
            "    elif request['cmd'] == 'exit':\n"
            "        print(json.dumps({'ok': True, 'cmd': 'exit'}), flush=True)\n"
            "        break\n"
        )
        return __import__("subprocess").Popen(
            [sys.executable, "-c", script],
            stdin=kwargs["stdin"],
            stdout=kwargs["stdout"],
            stderr=kwargs["stderr"],
            text=kwargs["text"],
            encoding=kwargs["encoding"],
            errors=kwargs["errors"],
            bufsize=kwargs["bufsize"],
            cwd=kwargs["cwd"],
            env=kwargs["env"],
            creationflags=kwargs["creationflags"],
        )

    runner = CEmuRunner(
        config=CEmuRunnerConfig(
            executable_path=fake_exe,
            rom_path=temp_rom_path,
            startup_timeout_s=1.0,
            command_timeout_s=1.0,
            shutdown_timeout_s=0.5,
            boot_timeout_s=1.0,
        ),
        process_factory=_spawn_fake_process,
    )

    runner.start()
    try:
        result = runner.snapshot(requested_snapshot_path)
    finally:
        runner.shutdown()

    assert requested_snapshot_path.exists()
    assert requested_snapshot_path.read_text(encoding="utf-8") == '{"backend":"host-temp"}'
    assert result.payload["path"] == str(requested_snapshot_path)
    assert host_temp_root.exists()
    assert list(host_temp_root.iterdir()) == []
