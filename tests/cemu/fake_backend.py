from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_CEMU_PATH = REPO_ROOT / "scripts" / "cemu"
if str(SCRIPTS_CEMU_PATH) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CEMU_PATH))


from cemu_runner import CEmuRunner, CEmuRunnerConfig


FAKE_MEMORY_CRC = "7A0F9C11"
FAKE_OOM_CRC = "0BADF00D"
FAKE_RECOVERED_PROMPT_CRC = "A32840C8"


def fake_crc_for_keys(*keys: str) -> str:
    return f"{(sum(ord(ch) for ch in '-'.join(keys)) & 0xFFFFFFFF):08X}"


def build_fake_cemu_server_script(script_path: Path) -> Path:
    script_path.write_text(
        """from __future__ import annotations
import json
import sys
import time
from pathlib import Path

state = {
    "rom_loaded": False,
    "running": False,
    "halted": False,
    "total_cycles": 0,
    "pending_delay_ms": 0,
    "app_count": 0,
    "key_count": 0,
    "target_name": "",
    "target_is_asm": True,
    "rom_path": "",
    "autotester_path": "",
    "last_crc": "",
    "last_autotester_exit": 0,
    "keys": [],
    "transfer_files": [],
}

DEFAULT_ROWS = [
    "CALCENGINE READY",
    "RESULT: 5      ",
    "STATUS: PASS   ",
]

print("fake-cemu ready", file=sys.stderr, flush=True)

def emit(payload):
    print(json.dumps(payload), flush=True)

def normalize_target(paths):
    for path in paths:
        if Path(path).name.upper() == "CALCENG.8XP":
            return "CALCENG"
    if not paths:
        return ""
    return Path(paths[0]).stem.upper()[:8]

def compute_crc(*keys):
    return f"{(sum(ord(ch) for ch in '-'.join(keys)) & 0xFFFFFFFF):08X}"

def current_screen():
    keys = list(state["keys"])
    if "__HANG_SCREEN__" in keys:
        time.sleep(5)

    if "__OOM__" in keys:
        oom_index = max(index for index, value in enumerate(keys) if value == "__OOM__")
        trailing = keys[oom_index + 1 :]
        if trailing[-2:] == ["clear", "clear"] or trailing[-1:] == ["clear"]:
            return {
                "crc": "A32840C8",
                "output_excerpt": "PROMPT RECOVERED AFTER OOM",
                "text_rows": [
                    "CALCENGINE READY",
                    "PROMPT RESTORED ",
                    "STATUS: PASS   ",
                ],
            }
        return {
            "crc": "0BADF00D",
            "output_excerpt": "ERR:MEMORY",
            "text_rows": [
                "ERR:MEMORY     ",
                "CLEAR TO EXIT  ",
                "STATUS: FAIL   ",
            ],
        }

    if ("sto" in keys or "sto>" in keys) and "rcl" in keys:
        return {
            "crc": "7A0F9C11",
            "output_excerpt": "STO/RCL OK",
            "text_rows": [
                "MEMORY CHECK   ",
                "RESULT: 7      ",
                "STATUS: PASS   ",
            ],
        }

    key_stream = "-".join(keys) or "idle"
    return {
        "crc": compute_crc(*keys) if keys else compute_crc("idle"),
        "output_excerpt": key_stream,
        "text_rows": DEFAULT_ROWS,
    }

for raw_line in sys.stdin:
    if not raw_line.strip():
        continue
    request = json.loads(raw_line)
    cmd = request["cmd"]

    if cmd == "load_rom":
        state["rom_loaded"] = True
        state["running"] = True
        state["rom_path"] = request["path"]
        state["autotester_path"] = request.get("autotester_path", "")
        emit({"ok": True, "cmd": cmd, "path": state["rom_path"], "backend": "fake"})
        continue

    if cmd == "load_app":
        request_path = Path(request["path"])
        added_paths = []
        if request_path.is_dir():
            for child in sorted(request_path.iterdir(), key=lambda path: path.name.lower()):
                if child.suffix.lower() in {".8xp", ".8xv"}:
                    added_paths.append(str(child))
        elif request_path.suffix.lower() in {".8xp", ".8xv"}:
            added_paths.append(str(request_path))
        else:
            emit({"ok": False, "cmd": cmd, "error": "unsupported load_app path"})
            continue

        seen = set(state["transfer_files"])
        unique_paths = []
        for added_path in added_paths:
            if added_path in seen:
                continue
            seen.add(added_path)
            unique_paths.append(added_path)
        state["transfer_files"].extend(unique_paths)
        state["app_count"] = len(state["transfer_files"])
        state["target_name"] = normalize_target(state["transfer_files"])
        emit({
            "ok": True,
            "cmd": cmd,
            "path": request["path"],
            "added_count": len(unique_paths),
            "app_count": state["app_count"],
            "target_name": state["target_name"],
            "target_is_asm": True,
            "backend": "fake",
        })
        continue

    if cmd == "inject_key":
        state["key_count"] += 1
        key = request["key"]
        state["keys"].append(key)
        emit({
            "ok": True,
            "cmd": cmd,
            "key": key,
            "duration_ms": int(request.get("duration_ms", 0)),
            "key_count": state["key_count"],
            "backend": "fake",
        })
        continue

    if cmd == "step":
        cycles = int(request["cycles"])
        state["total_cycles"] += cycles
        state["pending_delay_ms"] += cycles // 48000
        emit({
            "ok": True,
            "cmd": cmd,
            "cycles": cycles,
            "total_cycles": state["total_cycles"],
            "pending_delay_ms": state["pending_delay_ms"],
            "backend": "fake",
        })
        continue

    if cmd == "get_state":
        emit({
            "ok": True,
            "cmd": cmd,
            "backend": "fake",
            "rom_loaded": state["rom_loaded"],
            "running": state["running"],
            "halted": state["halted"],
            "total_cycles": state["total_cycles"],
            "pending_delay_ms": state["pending_delay_ms"],
            "app_count": state["app_count"],
            "key_count": state["key_count"],
            "target_name": state["target_name"],
            "target_is_asm": state["target_is_asm"],
            "rom_path": state["rom_path"],
            "autotester_path": state["autotester_path"],
            "last_crc": state["last_crc"],
            "last_autotester_exit": state["last_autotester_exit"],
            "version": "fake-cemu",
        })
        continue

    if cmd == "get_screen":
        screen = current_screen()
        state["last_crc"] = screen["crc"]
        emit({
            "ok": True,
            "cmd": cmd,
            "format": request.get("format", "crc"),
            "crc": screen["crc"],
            "autotester_exit": 0,
            "backend": "fake",
            "output_excerpt": screen["output_excerpt"],
        })
        continue

    if cmd == "snapshot":
        output_path = Path(request["path"])
        if output_path.suffix.lower() == ".json":
            screen = current_screen()
            output_path.write_text(json.dumps({
                "screen": {
                    "text_rows": screen["text_rows"]
                },
                "crc": screen["crc"],
                "keys": state["keys"],
                "transfer_files": state["transfer_files"],
                "app_count": state["app_count"],
            }), encoding="utf-8")
            state["last_crc"] = screen["crc"]
            emit({
                "ok": True,
                "cmd": cmd,
                "type": "state",
                "path": str(output_path),
                "crc": screen["crc"],
                "autotester_exit": 0,
                "backend": "fake",
                "output_excerpt": screen["output_excerpt"],
            })
            continue
        emit({"ok": False, "cmd": cmd, "error": "unsupported snapshot path"})
        continue

    if cmd == "reset":
        state["keys"] = []
        state["key_count"] = 0
        state["pending_delay_ms"] = 0
        state["last_crc"] = ""
        emit({"ok": True, "cmd": cmd, "backend": "fake"})
        continue

    if cmd == "exit":
        emit({"ok": True, "cmd": cmd, "note": "bye"})
        break

    emit({"ok": False, "cmd": cmd, "error": f"unknown command {cmd}"})
""",
        encoding="utf-8",
    )
    return script_path


def build_fake_cemu_runner(
    tmp_path: Path,
    rom_path: Path,
    *,
    startup_timeout_s: float = 3.0,
    command_timeout_s: float = 3.0,
    shutdown_timeout_s: float = 2.0,
    boot_timeout_s: float = 5.0,
) -> CEmuRunner:
    script_path = build_fake_cemu_server_script(tmp_path / "fake_cemu_server.py")
    config = CEmuRunnerConfig(
        executable_path=sys.executable,
        arguments=(str(script_path),),
        rom_path=rom_path,
        startup_timeout_s=startup_timeout_s,
        command_timeout_s=command_timeout_s,
        shutdown_timeout_s=shutdown_timeout_s,
        boot_timeout_s=boot_timeout_s,
    )
    return CEmuRunner(config=config)
