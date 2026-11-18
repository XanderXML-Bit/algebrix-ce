from __future__ import annotations

import json
import os
import re
import shutil
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Mapping, Sequence

from cemu_runner import CEmuRunner, CEmuRunnerConfig
from keypress_sequences import KeyAction, expression, pause, press
from rom_validation import RomValidationResult, validate_rom_file


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST_PATH = REPO_ROOT / "tests" / "emulator" / "manifest.json"
DEFAULT_ROM_PATH = REPO_ROOT / "rom" / "Rom.rom"
DEFAULT_SMOKE_TEMPLATE_PATH = REPO_ROOT / "tests" / "emulator" / "autotest.cemu-e2e.json"
DEFAULT_REAL_EDGE_CASES_PATH = REPO_ROOT / "tests" / "cemu" / "edge-cases" / "real-edge-cases.json"
DEFAULT_MODULE_BATTERY_PATH = REPO_ROOT / "tests" / "cemu" / "edge-cases" / "module-battery.json"
DEFAULT_DOWNLOADS_DIR = Path.home() / "Downloads" / "CalcEngine"
OFFICIAL_MODULE_LIMIT_BYTES = 51_200  # 50 KB — raised from 48 KB pending compressed rebuilds of CEIUSB/CEASLA/CELSCX/CESTPRB
DEFAULT_CEMU_TIMEOUT_MULTIPLIER_ENV = "CEMU_TIMEOUT_MULTIPLIER"
DEFAULT_CEMU_TIMEOUT_MULTIPLIER = 1.0
DEFAULT_REAL_CEMU_CI_OPT_IN_ENV = "CALCENGINE_ALLOW_REAL_CEMU_IN_CI"
DEFAULT_CEMU_STARTUP_TIMEOUT_S = 15.0
DEFAULT_CEMU_COMMAND_TIMEOUT_S = 45.0
DEFAULT_CEMU_SHUTDOWN_TIMEOUT_S = 10.0
DEFAULT_CEMU_BOOT_TIMEOUT_S = 60.0
DEFAULT_HEADLESS_CANDIDATES = tuple(CEmuRunner.default_binary_candidates(repo_root=REPO_ROOT))
DEFAULT_WSL_WINDOWS_USERS_ROOT_ENV = "WSL_WINDOWS_USERS_ROOT"
DEFAULT_BOOT_SEQUENCE: tuple[KeyAction, ...] = (
    pause(4000, note="wait for launch"),
    press("clear"),
    pause(500, note="clear splash to prompt"),
)
DEFAULT_PROMPT_CLEAR_SEQUENCE: tuple[KeyAction, ...] = (
    press("clear"),
    pause(500, note="return to prompt"),
)
DEFAULT_PROGRAM_MENU_SEQUENCE: tuple[KeyAction, ...] = (
    press("prgm"),
    pause(1200, note="open PRGM menu"),
)
DEFAULT_PROMPT_RECOVERY_CRCS: tuple[str, ...] = (
    "349F4775",
    "A32840C8",
    "7C622B05",
    "7F0F6A91",
    "DAE8FE30",
)
CRC32_HEX_PATTERN = re.compile(r"^[0-9A-F]{8}$")
STAGING_TEMP_SUFFIX = ".calcengine-stage"
TRANSFER_ARTIFACT_SUFFIXES = {".8xp", ".8xv"}


def _env_flag_enabled(value: object) -> bool:
    normalized = str(value).strip().lower()
    return normalized in {"1", "true", "yes", "on"}


def hosted_ci_blocks_real_cemu(env: Mapping[str, str] | None = None) -> bool:
    source = os.environ if env is None else env
    running_in_ci = _env_flag_enabled(source.get("CI")) or _env_flag_enabled(source.get("GITHUB_ACTIONS"))
    ci_opt_in_enabled = _env_flag_enabled(source.get(DEFAULT_REAL_CEMU_CI_OPT_IN_ENV))
    return running_in_ci and not ci_opt_in_enabled


def _hosted_ci_real_cemu_blocker() -> str:
    return (
        "Real CEmu and arTIfiCE hardware-dependent lanes are disabled on hosted CI. "
        f"Set {DEFAULT_REAL_CEMU_CI_OPT_IN_ENV}=true to opt into a provisioned runner."
    )


def _wsl_windows_users_root() -> Path:
    raw_root = str(os.environ.get(DEFAULT_WSL_WINDOWS_USERS_ROOT_ENV, "/mnt/c/Users")).strip()
    return Path(raw_root).expanduser()


def _wsl_windows_user_cemu_tool_roots() -> tuple[Path, ...]:
    username = Path.home().name.strip()
    if not username:
        return ()
    return (_wsl_windows_users_root() / username / "Tools" / "CEmu",)


def _default_autotester_candidates() -> tuple[Path | None, ...]:
    return (
        Path(os.environ["CEMU_AUTOTESTER_PATH"]).expanduser()
        if os.environ.get("CEMU_AUTOTESTER_PATH")
        else None,
        Path.home() / "Tools" / "CEmu" / "v2.0" / "autotester.exe",
        Path.home() / "Tools" / "CEmu" / "autotester.exe",
        *(
            root / "v2.0" / "autotester.exe"
            for root in _wsl_windows_user_cemu_tool_roots()
        ),
        *(root / "autotester.exe" for root in _wsl_windows_user_cemu_tool_roots()),
    )


def _default_gui_candidates() -> tuple[Path | None, ...]:
    return (
        Path(os.environ["CEMU_GUI_PATH"]).expanduser()
        if os.environ.get("CEMU_GUI_PATH")
        else None,
        Path(os.environ["CEMU_PATH"]).expanduser()
        if os.environ.get("CEMU_PATH")
        else None,
        Path.home() / "Tools" / "CEmu" / "v2.0" / "CEmu.exe",
        Path.home() / "Tools" / "CEmu" / "CEmu.exe",
        *(root / "v2.0" / "CEmu.exe" for root in _wsl_windows_user_cemu_tool_roots()),
        *(root / "CEmu.exe" for root in _wsl_windows_user_cemu_tool_roots()),
    )


class TransferPackageResolutionError(RuntimeError):
    """Raised when the CalcEngine transfer package cannot be resolved cleanly."""


class TransferManifestSchemaError(RuntimeError):
    """Raised when the transfer manifest is malformed or internally inconsistent."""


@dataclass(frozen=True)
class TransferManifestEntry:
    name: str
    size_bytes: int
    crc32: str
    kind: str


@dataclass(frozen=True)
class TransferPackage:
    manifest_path: Path
    module_entries: tuple[TransferManifestEntry, ...]
    runtime_entries: tuple[TransferManifestEntry, ...]
    readme_entry: TransferManifestEntry | None
    resolved_paths: Mapping[str, Path]
    candidate_roots: tuple[Path, ...]
    missing_entries: tuple[str, ...]

    @property
    def module_names(self) -> tuple[str, ...]:
        return tuple(entry.name for entry in self.module_entries)

    @property
    def runtime_names(self) -> tuple[str, ...]:
        return tuple(entry.name for entry in self.runtime_entries)

    @property
    def transfer_paths(self) -> tuple[Path, ...]:
        ordered_names = [entry.name for entry in self.module_entries]
        ordered_names.extend(entry.name for entry in self.runtime_entries)
        return tuple(self.resolved_paths[name] for name in ordered_names if name in self.resolved_paths)

    @property
    def all_artifact_paths(self) -> tuple[Path, ...]:
        ordered_names = [entry.name for entry in self.module_entries]
        ordered_names.extend(entry.name for entry in self.runtime_entries)
        if self.readme_entry is not None:
            ordered_names.append(self.readme_entry.name)
        return tuple(self.resolved_paths[name] for name in ordered_names if name in self.resolved_paths)

    @property
    def module_count(self) -> int:
        return len(self.module_entries)

    @property
    def runtime_dependency_count(self) -> int:
        return len(self.runtime_entries)

    @property
    def transfer_count(self) -> int:
        return len(self.transfer_paths)

    @property
    def ready(self) -> bool:
        return not self.missing_entries

    def require_complete(self) -> None:
        if not self.ready:
            missing = ", ".join(self.missing_entries)
            raise TransferPackageResolutionError(
                f"CalcEngine transfer package is incomplete; missing: {missing}"
            )


@dataclass(frozen=True)
class ResolvedCEmuEnvironment:
    repo_root: Path
    rom_path: Path | None
    rom_validation: RomValidationResult | None
    headless_path: Path | None
    autotester_path: Path | None
    gui_path: Path | None
    transfer_package: TransferPackage | None
    blockers: tuple[str, ...]

    @property
    def ready(self) -> bool:
        return (
            self.rom_path is not None
            and self.headless_path is not None
            and self.autotester_path is not None
            and self.transfer_package is not None
            and self.transfer_package.ready
            and not self.blockers
        )


def compute_crc32_hex(path: Path) -> str:
    return f"{zlib.crc32(path.read_bytes()) & 0xFFFFFFFF:08X}"


def get_cemu_timeout_multiplier(env: Mapping[str, str] | None = None) -> float:
    source = os.environ if env is None else env
    raw_value = str(source.get(DEFAULT_CEMU_TIMEOUT_MULTIPLIER_ENV, "")).strip()
    if not raw_value:
        return DEFAULT_CEMU_TIMEOUT_MULTIPLIER
    try:
        multiplier = float(raw_value)
    except (TypeError, ValueError):
        return DEFAULT_CEMU_TIMEOUT_MULTIPLIER
    return multiplier if multiplier > 0 else DEFAULT_CEMU_TIMEOUT_MULTIPLIER


def scale_cemu_timeout_seconds(
    timeout_s: float | None,
    *,
    multiplier: float | None = None,
    minimum_s: float | None = None,
) -> float | None:
    if timeout_s is None:
        return None
    factor = get_cemu_timeout_multiplier() if multiplier is None else multiplier
    scaled_timeout = float(timeout_s) * factor
    if minimum_s is not None:
        scaled_timeout = max(scaled_timeout, minimum_s)
    return round(scaled_timeout, 3)


def _normalize_crc32(
    value: object,
    *,
    field_name: str,
    allow_empty: bool = False,
) -> str:
    normalized = str(value).strip().upper()
    if allow_empty and not normalized:
        return ""
    if not CRC32_HEX_PATTERN.fullmatch(normalized):
        raise TransferManifestSchemaError(
            f"{field_name} must be an 8-character uppercase CRC32 hex string."
        )
    return normalized


def _normalize_entry_name(
    value: object,
    *,
    field_name: str,
    expected_suffix: str,
) -> str:
    normalized = str(value).strip()
    if not normalized:
        raise TransferManifestSchemaError(f"{field_name} is required.")
    if not normalized.lower().endswith(expected_suffix):
        raise TransferManifestSchemaError(
            f"{field_name} must end with '{expected_suffix}'."
        )
    return normalized


def _normalize_manifest_count(value: object, *, field_name: str) -> int:
    try:
        normalized = int(value)
    except (TypeError, ValueError) as exc:
        raise TransferManifestSchemaError(f"{field_name} must be an integer.") from exc
    if normalized < 0:
        raise TransferManifestSchemaError(f"{field_name} must be zero or greater.")
    return normalized


def _archive_slot_name(name: str) -> str:
    return Path(name).stem.upper()[:8]


def _validate_unique_entry_names(entries: Sequence[TransferManifestEntry]) -> None:
    seen_by_name: dict[str, str] = {}
    seen_by_slot: dict[str, str] = {}

    for entry in entries:
        normalized_name = entry.name.lower()
        if normalized_name in seen_by_name:
            raise TransferManifestSchemaError(
                f"Manifest entry '{entry.name}' duplicates '{seen_by_name[normalized_name]}' by filename."
            )
        seen_by_name[normalized_name] = entry.name

        archive_slot = _archive_slot_name(entry.name)
        previous_name = seen_by_slot.get(archive_slot)
        if previous_name is not None:
            raise TransferManifestSchemaError(
                f"Manifest entries '{previous_name}' and '{entry.name}' collide on TI-84 CE archive slot '{archive_slot}'."
            )
        seen_by_slot[archive_slot] = entry.name


def _build_manifest_entry(
    row: object,
    *,
    index: int,
    kind: str,
) -> TransferManifestEntry:
    if not isinstance(row, Mapping):
        raise TransferManifestSchemaError(
            f"{kind} entry {index} must be a JSON object."
        )

    if kind == "module":
        name = _normalize_entry_name(
            row.get("name"),
            field_name=f"modules[{index}].name",
            expected_suffix=".8xp",
        )
    else:
        name = _normalize_entry_name(
            row.get("name"),
            field_name=f"runtime_dependencies[{index}].name",
            expected_suffix=".8xv",
        )

    size_field = (
        f"modules[{index}].size_bytes"
        if kind == "module"
        else f"runtime_dependencies[{index}].size_bytes"
    )
    size_bytes = _normalize_manifest_count(row.get("size_bytes"), field_name=size_field)
    if size_bytes == 0:
        raise TransferManifestSchemaError(f"{size_field} must be greater than zero.")
    if kind == "module" and size_bytes > OFFICIAL_MODULE_LIMIT_BYTES:
        raise TransferManifestSchemaError(
            f"{size_field} exceeds the TI-84 CE per-module limit of {OFFICIAL_MODULE_LIMIT_BYTES} bytes."
        )

    crc_field = (
        f"modules[{index}].crc32"
        if kind == "module"
        else f"runtime_dependencies[{index}].crc32"
    )
    crc32 = _normalize_crc32(row.get("crc32"), field_name=crc_field)

    return TransferManifestEntry(
        name=name,
        size_bytes=size_bytes,
        crc32=crc32,
        kind=kind,
    )


def _build_readme_entry(readme_payload: object) -> TransferManifestEntry:
    if not isinstance(readme_payload, Mapping):
        raise TransferManifestSchemaError("readme must be a JSON object when provided.")

    name = _normalize_entry_name(
        readme_payload.get("name"),
        field_name="readme.name",
        expected_suffix=".md",
    )
    crc32 = _normalize_crc32(
        readme_payload.get("crc32", ""),
        field_name="readme.crc32",
        allow_empty=True,
    )
    return TransferManifestEntry(
        name=name,
        size_bytes=-1,
        crc32=crc32,
        kind="readme",
    )


def _expected_staged_entries(
    package: TransferPackage,
    *,
    include_readme: bool,
) -> tuple[TransferManifestEntry, ...]:
    entries = [*package.module_entries, *package.runtime_entries]
    if include_readme and package.readme_entry is not None:
        entries.append(package.readme_entry)
    return tuple(entries)


def validate_staged_transfer_directory(
    package: TransferPackage,
    destination_dir: Path,
    *,
    include_readme: bool = True,
    allowed_extra_names: Sequence[str] = (),
) -> dict[str, object]:
    expected_entries = _expected_staged_entries(package, include_readme=include_readme)
    expected_names = [entry.name for entry in expected_entries]
    expected_name_set = {name.lower() for name in expected_names}
    allowed_extra_name_set = {name.lower() for name in allowed_extra_names}

    unexpected_entries = sorted(
        path.name
        for path in destination_dir.iterdir()
        if path.is_file()
        and (
            path.suffix.lower() in TRANSFER_ARTIFACT_SUFFIXES
            or path.name.lower() == "readme.md"
            or path.name.endswith(STAGING_TEMP_SUFFIX)
        )
        and path.name.lower() not in expected_name_set
        and path.name.lower() not in allowed_extra_name_set
    )

    missing_entries: list[str] = []
    mismatched_entries: list[str] = []
    for entry in expected_entries:
        staged_path = destination_dir / entry.name
        if not staged_path.is_file():
            missing_entries.append(entry.name)
            continue
        if not _artifact_matches(entry, staged_path):
            mismatched_entries.append(entry.name)

    return {
        "expected_names": expected_names,
        "missing_entries": missing_entries,
        "mismatched_entries": mismatched_entries,
        "unexpected_entries": unexpected_entries,
        "success": not missing_entries and not mismatched_entries and not unexpected_entries,
    }


def load_transfer_manifest(manifest_path: Path = DEFAULT_MANIFEST_PATH) -> tuple[
    tuple[TransferManifestEntry, ...],
    tuple[TransferManifestEntry, ...],
    TransferManifestEntry | None,
]:
    payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    modules_payload = payload.get("modules")
    if not isinstance(modules_payload, list):
        raise TransferManifestSchemaError("modules must be an array.")

    runtime_payload = payload.get("runtime_dependencies")
    if not isinstance(runtime_payload, list):
        raise TransferManifestSchemaError("runtime_dependencies must be an array.")

    module_entries = tuple(
        _build_manifest_entry(row, index=index, kind="module")
        for index, row in enumerate(modules_payload)
    )
    runtime_entries = tuple(
        _build_manifest_entry(row, index=index, kind="runtime")
        for index, row in enumerate(runtime_payload)
    )
    if "module_count" in payload:
        declared_module_count = _normalize_manifest_count(
            payload["module_count"],
            field_name="module_count",
        )
        if declared_module_count != len(module_entries):
            raise TransferManifestSchemaError(
                f"module_count declared {declared_module_count}, but manifest lists {len(module_entries)} modules."
            )
    if "runtime_dependency_count" in payload:
        declared_runtime_count = _normalize_manifest_count(
            payload["runtime_dependency_count"],
            field_name="runtime_dependency_count",
        )
        if declared_runtime_count != len(runtime_entries):
            raise TransferManifestSchemaError(
                f"runtime_dependency_count declared {declared_runtime_count}, but manifest lists {len(runtime_entries)} runtime dependencies."
            )

    readme_payload = payload.get("readme")
    readme_entry = _build_readme_entry(readme_payload) if readme_payload is not None else None
    validation_entries = [*module_entries, *runtime_entries]
    if readme_entry is not None:
        validation_entries.append(readme_entry)
    _validate_unique_entry_names(validation_entries)
    return module_entries, runtime_entries, readme_entry


def _iter_candidate_roots(
    repo_root: Path,
    transfer_dir: Path | None = None,
    downloads_dir: Path | None = None,
    additional_roots: Sequence[Path] = (),
) -> tuple[Path, ...]:
    ordered: list[Path] = []
    seen: set[str] = set()

    def add(path: Path | None) -> None:
        if path is None:
            return
        resolved = path.expanduser()
        key = str(resolved).lower()
        if key in seen:
            return
        seen.add(key)
        ordered.append(resolved)

    env_transfer_dir = os.environ.get("CALCENGINE_TRANSFER_DIR")
    add(transfer_dir)
    add(Path(env_transfer_dir) if env_transfer_dir else None)
    add(downloads_dir)
    add(DEFAULT_DOWNLOADS_DIR)
    for extra_root in additional_roots:
        add(extra_root)
    add(repo_root / "deploy" / "hardware-ready")
    add(repo_root / "bin")
    add(repo_root / "_research_tmp" / "toolchain" / "src")
    add(repo_root)

    return tuple(path for path in ordered if path.exists())


def _iter_named_candidates(root: Path, filename: str) -> Iterator[Path]:
    direct_candidate = root / filename
    if direct_candidate.is_file():
        yield direct_candidate

    if root.is_file() and root.name.lower() == filename.lower():
        yield root
        return

    if not root.is_dir():
        return

    discovered: list[Path] = []
    for path in root.rglob(filename):
        if path.is_file():
            discovered.append(path)
    for path in sorted(discovered, key=lambda candidate: (len(candidate.parts), len(str(candidate)), str(candidate).lower())):
        if path == direct_candidate:
            continue
        yield path


def _artifact_matches(entry: TransferManifestEntry, candidate: Path) -> bool:
    if not candidate.is_file():
        return False
    if entry.size_bytes >= 0 and candidate.stat().st_size != entry.size_bytes:
        return False
    if entry.crc32:
        return compute_crc32_hex(candidate) == entry.crc32
    return True


def _resolve_manifest_entry(
    entry: TransferManifestEntry,
    candidate_roots: Sequence[Path],
) -> Path | None:
    for root in candidate_roots:
        for candidate in _iter_named_candidates(root, entry.name):
            if _artifact_matches(entry, candidate):
                return candidate.resolve()
    return None


def resolve_transfer_package(
    repo_root: Path = REPO_ROOT,
    manifest_path: Path = DEFAULT_MANIFEST_PATH,
    transfer_dir: Path | None = None,
    downloads_dir: Path | None = None,
    additional_roots: Sequence[Path] = (),
) -> TransferPackage:
    module_entries, runtime_entries, readme_entry = load_transfer_manifest(manifest_path=manifest_path)
    candidate_roots = _iter_candidate_roots(
        repo_root=repo_root,
        transfer_dir=transfer_dir,
        downloads_dir=downloads_dir,
        additional_roots=additional_roots,
    )

    resolved_paths: dict[str, Path] = {}
    missing_entries: list[str] = []

    for entry in (*module_entries, *runtime_entries):
        resolved_path = _resolve_manifest_entry(entry, candidate_roots)
        if resolved_path is None:
            missing_entries.append(entry.name)
            continue
        resolved_paths[entry.name] = resolved_path

    if readme_entry is not None:
        resolved_path = _resolve_manifest_entry(readme_entry, candidate_roots)
        if resolved_path is None:
            missing_entries.append(readme_entry.name)
        else:
            resolved_paths[readme_entry.name] = resolved_path

    package = TransferPackage(
        manifest_path=manifest_path,
        module_entries=module_entries,
        runtime_entries=runtime_entries,
        readme_entry=readme_entry,
        resolved_paths=resolved_paths,
        candidate_roots=candidate_roots,
        missing_entries=tuple(missing_entries),
    )
    if package.ready:
        expected_paths = [entry.name for entry in (*package.module_entries, *package.runtime_entries)]
        resolved_transfer_names = [path.name for path in package.transfer_paths]
        if resolved_transfer_names != expected_paths:
            raise TransferPackageResolutionError(
                "Resolved transfer package order deviates from the manifest order."
            )
    return package


def _copy_artifact_to_stage(
    source_path: Path,
    *,
    destination_dir: Path,
    destination_name: str,
    entry: TransferManifestEntry,
) -> None:
    destination_path = destination_dir / destination_name
    staging_path = destination_dir / f".{destination_name}{STAGING_TEMP_SUFFIX}"
    if staging_path.exists():
        staging_path.unlink()
    try:
        shutil.copy2(source_path, staging_path)
    except OSError as exc:
        if staging_path.exists():
            staging_path.unlink()
        raise TransferPackageResolutionError(
            f"Staged artifact '{destination_name}' could not be copied cleanly: {exc}"
        ) from exc
    if not _artifact_matches(entry, staging_path):
        if staging_path.exists():
            staging_path.unlink()
        raise TransferPackageResolutionError(
            f"Staged artifact '{destination_name}' failed size/CRC verification immediately after copy."
        )
    try:
        staging_path.replace(destination_path)
    except OSError as exc:
        if staging_path.exists():
            staging_path.unlink()
        raise TransferPackageResolutionError(
            f"Staged artifact '{destination_name}' could not be finalized cleanly: {exc}"
        ) from exc


def stage_transfer_package(
    package: TransferPackage,
    destination_dir: Path,
    include_readme: bool = True,
) -> Path:
    package.require_complete()
    destination_dir.mkdir(parents=True, exist_ok=True)
    expected_entries = _expected_staged_entries(package, include_readme=include_readme)
    source_paths = package.all_artifact_paths if include_readme else package.transfer_paths

    for existing_path in destination_dir.iterdir():
        if not existing_path.is_file():
            continue
        if (
            existing_path.suffix.lower() in TRANSFER_ARTIFACT_SUFFIXES
            or existing_path.name.lower() == "readme.md"
            or existing_path.name.endswith(STAGING_TEMP_SUFFIX)
        ):
            existing_path.unlink()

    entry_by_name = {entry.name: entry for entry in expected_entries}
    for path in source_paths:
        if not include_readme and path.name.lower() == "readme.md":
            continue
        _copy_artifact_to_stage(
            path,
            destination_dir=destination_dir,
            destination_name=path.name,
            entry=entry_by_name[path.name],
        )

    validation = validate_staged_transfer_directory(
        package,
        destination_dir,
        include_readme=include_readme,
    )
    if not validation["success"]:
        failures: list[str] = []
        if validation["missing_entries"]:
            failures.append("missing: " + ", ".join(validation["missing_entries"]))
        if validation["mismatched_entries"]:
            failures.append("mismatched: " + ", ".join(validation["mismatched_entries"]))
        if validation["unexpected_entries"]:
            failures.append("unexpected: " + ", ".join(validation["unexpected_entries"]))
        raise TransferPackageResolutionError(
            "Staged transfer directory failed validation after copy: " + "; ".join(failures)
        )
    return destination_dir


def resolve_rom_path(candidate: Path | None = None) -> Path | None:
    if candidate is not None and candidate.exists():
        return candidate.resolve()
    if DEFAULT_ROM_PATH.exists():
        return DEFAULT_ROM_PATH.resolve()
    return None


def _resolve_first_existing_path(candidates: Iterable[Path | None]) -> Path | None:
    for candidate in candidates:
        if candidate is None:
            continue
        expanded = candidate.expanduser()
        if expanded.exists():
            return expanded.resolve()
    return None


def resolve_headless_path(repo_root: Path = REPO_ROOT, candidate: Path | None = None) -> Path | None:
    explicit = candidate
    env_candidate = (
        Path(os.environ["CEMU_HEADLESS_PATH"]).expanduser()
        if os.environ.get("CEMU_HEADLESS_PATH")
        else None
    )
    dynamic_candidates = list(CEmuRunner.default_binary_candidates(repo_root=repo_root))
    return _resolve_first_existing_path([explicit, env_candidate, *dynamic_candidates, *DEFAULT_HEADLESS_CANDIDATES])


def resolve_autotester_path(candidate: Path | None = None) -> Path | None:
    explicit = candidate
    path_candidate = shutil.which("autotester.exe")
    return _resolve_first_existing_path(
        [
            explicit,
            *(_path for _path in _default_autotester_candidates() if _path is not None),
            Path(path_candidate) if path_candidate else None,
        ]
    )


def resolve_cemu_gui_path(candidate: Path | None = None) -> Path | None:
    explicit = candidate
    path_candidate = shutil.which("CEmu.exe") or shutil.which("cemu.exe")
    return _resolve_first_existing_path(
        [
            explicit,
            *(_path for _path in _default_gui_candidates() if _path is not None),
            Path(path_candidate) if path_candidate else None,
        ]
    )


def resolve_real_cemu_environment(
    repo_root: Path = REPO_ROOT,
    manifest_path: Path = DEFAULT_MANIFEST_PATH,
    transfer_dir: Path | None = None,
    downloads_dir: Path | None = None,
    headless_path: Path | None = None,
    autotester_path: Path | None = None,
    gui_path: Path | None = None,
    rom_path: Path | None = None,
) -> ResolvedCEmuEnvironment:
    blockers: list[str] = []

    if hosted_ci_blocks_real_cemu():
        blockers.append(_hosted_ci_real_cemu_blocker())

    resolved_rom = resolve_rom_path(rom_path)
    rom_validation: RomValidationResult | None = None
    if resolved_rom is None:
        blockers.append(f"ROM file is missing at {DEFAULT_ROM_PATH}")
    else:
        rom_validation = validate_rom_file(resolved_rom)
        blockers.extend(rom_validation.errors)

    resolved_headless = resolve_headless_path(repo_root=repo_root, candidate=headless_path)
    if resolved_headless is None:
        blockers.append("cemu-headless executable is unavailable")

    resolved_autotester = resolve_autotester_path(candidate=autotester_path)
    if resolved_autotester is None:
        blockers.append("autotester.exe is unavailable")

    resolved_gui = resolve_cemu_gui_path(candidate=gui_path)

    transfer_package: TransferPackage | None = None
    try:
        transfer_package = resolve_transfer_package(
            repo_root=repo_root,
            manifest_path=manifest_path,
            transfer_dir=transfer_dir,
            downloads_dir=downloads_dir,
        )
        if not transfer_package.ready:
            blockers.append(
                "CalcEngine transfer package is incomplete: "
                + ", ".join(transfer_package.missing_entries)
            )
    except (FileNotFoundError, KeyError, TypeError, ValueError, TransferPackageResolutionError) as exc:
        blockers.append(str(exc))

    return ResolvedCEmuEnvironment(
        repo_root=repo_root,
        rom_path=resolved_rom,
        rom_validation=rom_validation,
        headless_path=resolved_headless,
        autotester_path=resolved_autotester,
        gui_path=resolved_gui,
        transfer_package=transfer_package,
        blockers=tuple(blockers),
    )


def get_real_cemu_status(
    repo_root: Path = REPO_ROOT,
    manifest_path: Path = DEFAULT_MANIFEST_PATH,
) -> tuple[bool, str]:
    environment = resolve_real_cemu_environment(repo_root=repo_root, manifest_path=manifest_path)
    if environment.ready:
        return True, "real CEmu environment is ready"
    if environment.blockers:
        return False, "; ".join(environment.blockers)
    return False, "real CEmu environment is unavailable"


def build_runner_config(environment: ResolvedCEmuEnvironment) -> CEmuRunnerConfig:
    if not environment.ready:
        raise TransferPackageResolutionError(
            "Cannot build a real CEmu runner config until ROM, cemu-headless, autotester, "
            "and the full transfer package are available."
        )
    assert environment.headless_path is not None
    assert environment.rom_path is not None
    assert environment.autotester_path is not None
    return CEmuRunnerConfig(
        executable_path=environment.headless_path,
        rom_path=environment.rom_path,
        autotester_path=environment.autotester_path,
        startup_timeout_s=scale_cemu_timeout_seconds(DEFAULT_CEMU_STARTUP_TIMEOUT_S) or DEFAULT_CEMU_STARTUP_TIMEOUT_S,
        command_timeout_s=scale_cemu_timeout_seconds(DEFAULT_CEMU_COMMAND_TIMEOUT_S) or DEFAULT_CEMU_COMMAND_TIMEOUT_S,
        shutdown_timeout_s=scale_cemu_timeout_seconds(DEFAULT_CEMU_SHUTDOWN_TIMEOUT_S) or DEFAULT_CEMU_SHUTDOWN_TIMEOUT_S,
        boot_timeout_s=scale_cemu_timeout_seconds(DEFAULT_CEMU_BOOT_TIMEOUT_S) or DEFAULT_CEMU_BOOT_TIMEOUT_S,
    )


def load_real_smoke_expectations(
    template_path: Path = DEFAULT_SMOKE_TEMPLATE_PATH,
) -> dict[str, dict[str, object]]:
    template = json.loads(template_path.read_text(encoding="utf-8"))
    expectations: dict[str, dict[str, object]] = {}
    for hash_name, payload in template["hashes"].items():
        expected_crcs = tuple(str(value).upper() for value in payload["expected_CRCs"])
        if hash_name == "boot_home":
            expected_crcs = tuple(dict.fromkeys((*expected_crcs, "B76BC405")))
        if hash_name == "prompt_after_addition":
            expected_crcs = tuple(dict.fromkeys((*expected_crcs, "A32840C8")))
        expectations[hash_name] = {
            "description": str(payload["description"]),
            "expected_crcs": expected_crcs,
        }
    expectations["program_menu_open"] = {
        "description": "TI-OS PRGM menu opens after CalcEngine returns to the prompt",
        "expected_crcs": ("967993A8",),
    }
    expectations["prompt_after_multiplication"] = {
        "description": "CalcEngine returns to one of the locked prompt-recovery baselines after dismissing the multiplication result",
        "expected_crcs": DEFAULT_PROMPT_RECOVERY_CRCS,
    }
    expectations["negative_result"] = {
        "description": "CalcEngine returns the correct real-ROM screen for (-)5+10",
        "expected_crcs": ("B9B5D5E0",),
    }
    expectations["decimal_result"] = {
        "description": "CalcEngine returns the correct real-ROM screen for 3.14*2 on the sequential smoke path",
        "expected_crcs": ("3FDCFCFD",),
    }
    expectations["parenthesized_result"] = {
        "description": "CalcEngine preserves grouped order of operations for (4+5)*3",
        "expected_crcs": ("3956B827",),
    }
    expectations["power_result"] = {
        "description": "CalcEngine returns the correct real-ROM screen for 2^8 on the sequential smoke path",
        "expected_crcs": ("9DF8F505",),
    }
    expectations["divide_by_zero_error"] = {
        "description": "CalcEngine surfaces the real-ROM divide-by-zero error screen for 1/0 on the sequential smoke path",
        "expected_crcs": ("32BFD188",),
    }
    expectations["prompt_after_divide_by_zero"] = {
        "description": "Dismissing the divide-by-zero screen returns to one of the locked prompt-recovery baselines",
        "expected_crcs": DEFAULT_PROMPT_RECOVERY_CRCS,
    }
    expectations["prompt_after_menu_close"] = {
        "description": "Closing the TI-OS PRGM menu returns to one of the locked prompt-recovery baselines",
        "expected_crcs": (*DEFAULT_PROMPT_RECOVERY_CRCS, "99A97135"),
    }
    expectations["graphing_mode_result"] = {
        "description": "CalcEngine dispatches a graphing request through the archived CEGRPH module and renders the locked graphing-mode baseline.",
        "expected_crcs": ("B9B5D5E0",),
    }
    expectations["prompt_after_graphing_mode"] = {
        "description": "Dismissing the graphing route returns to one of the locked prompt-recovery baselines.",
        "expected_crcs": (*DEFAULT_PROMPT_RECOVERY_CRCS, "00E4C4C2"),
    }
    expectations["memory_store_variable"] = {
        "description": "Storing 5 into X through the TI store operator remains on the locked real-ROM baseline.",
        "expected_crcs": ("B9B5D5E0",),
    }
    expectations["memory_recall_variable"] = {
        "description": "Recalling X through x+2 returns the locked memory-recall baseline.",
        "expected_crcs": ("98CCBE4C",),
    }
    expectations["memory_clear_variable"] = {
        "description": "Clearing X by storing 0 remains on the locked real-ROM baseline.",
        "expected_crcs": ("D289E0D7",),
    }
    expectations["memory_recall_after_clear"] = {
        "description": "Recalling X after clearing it returns the locked zeroed-memory baseline.",
        "expected_crcs": ("99D28FB9",),
    }
    expectations["overflow_error"] = {
        "description": "A large exponential input remains on the locked overflow-path baseline.",
        "expected_crcs": ("387E9F08",),
    }
    expectations["syntax_error_screen"] = {
        "description": "An incomplete expression still surfaces the locked syntax-error screen before recovery.",
        "expected_crcs": ("B9B5D5E0",),
    }
    expectations["syntax_error_prompt_restore"] = {
        "description": "Clearing a syntax error returns to one of the locked prompt-recovery baselines.",
        "expected_crcs": DEFAULT_PROMPT_RECOVERY_CRCS,
    }
    expectations["archive_program_execution"] = {
        "description": "CalcEngine loads and runs an archived module request and renders the locked archive-execution baseline.",
        "expected_crcs": ("96FD4303",),
    }
    return expectations


def _paced_keys(events: Sequence[KeyAction], key_delay_ms: int) -> list[KeyAction]:
    paced: list[KeyAction] = []
    for event in events:
        paced.append(event)
        if event.kind == "key":
            paced.append(pause(key_delay_ms, note="settle after key"))
    return paced


def build_paced_expression_sequence(
    expression_text: str,
    *,
    key_delay_ms: int = 250,
    enter_settle_delay_ms: int = 200,
    result_delay_ms: int = 2000,
) -> list[KeyAction]:
    sequence = _paced_keys(expression(expression_text), key_delay_ms=key_delay_ms)
    sequence.append(pause(enter_settle_delay_ms, note="settle before enter"))
    sequence.append(press("enter"))
    sequence.append(pause(result_delay_ms, note="wait for result"))
    return sequence


def build_paced_token_sequence(
    token_values: Sequence[str],
    *,
    key_delay_ms: int = 250,
    auto_enter: bool = True,
    enter_settle_delay_ms: int = 200,
    result_delay_ms: int = 2000,
) -> list[KeyAction]:
    sequence: list[KeyAction] = []
    for token in token_values:
        sequence.append(press(token))
        sequence.append(pause(key_delay_ms, note="settle after key"))
    if auto_enter:
        sequence.append(pause(enter_settle_delay_ms, note="settle before enter"))
        sequence.append(press("enter"))
        sequence.append(pause(result_delay_ms, note="wait for result"))
    return sequence


def build_program_menu_sequence(key_delay_ms: int = 250, menu_delay_ms: int = 1200) -> list[KeyAction]:
    return [press("prgm"), pause(max(key_delay_ms, menu_delay_ms), note="open PRGM menu")]


def capture_checkpoint(
    runner: CEmuRunner,
    name: str,
    expected_crcs: Sequence[str],
    description: str,
) -> dict[str, object]:
    response = runner.capture_screen()
    observed_crc = str(response.payload["crc"]).upper()
    normalized_expected = tuple(str(value).upper() for value in expected_crcs)
    return {
        "name": name,
        "description": description,
        "expected_crcs": list(normalized_expected),
        "observed_crc": observed_crc,
        "passed": observed_crc in normalized_expected,
        "payload": dict(response.payload),
    }


def run_smoke_checklist(
    runner: CEmuRunner,
    expectations: Mapping[str, Mapping[str, object]] | None = None,
) -> list[dict[str, object]]:
    smoke_expectations = dict(load_real_smoke_expectations())
    if expectations is not None:
        smoke_expectations.update(expectations)

    checkpoints: list[dict[str, object]] = []

    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="boot_home",
            expected_crcs=tuple(smoke_expectations["boot_home"]["expected_crcs"]),
            description=str(smoke_expectations["boot_home"]["description"]),
        )
    )

    runner.send_sequence(build_paced_expression_sequence("2+3"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="addition_result",
            expected_crcs=tuple(smoke_expectations["addition_result"]["expected_crcs"]),
            description=str(smoke_expectations["addition_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="prompt_after_addition",
            expected_crcs=tuple(smoke_expectations["prompt_after_addition"]["expected_crcs"]),
            description=str(smoke_expectations["prompt_after_addition"]["description"]),
        )
    )

    runner.send_sequence(build_paced_expression_sequence("6*7"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="multiplication_result",
            expected_crcs=tuple(smoke_expectations["multiplication_result"]["expected_crcs"]),
            description=str(smoke_expectations["multiplication_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="prompt_after_multiplication",
            expected_crcs=tuple(smoke_expectations["prompt_after_multiplication"]["expected_crcs"]),
            description=str(smoke_expectations["prompt_after_multiplication"]["description"]),
        )
    )

    runner.send_sequence(build_paced_expression_sequence("(-)5+10"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="negative_result",
            expected_crcs=tuple(smoke_expectations["negative_result"]["expected_crcs"]),
            description=str(smoke_expectations["negative_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("3.14*2"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="decimal_result",
            expected_crcs=tuple(smoke_expectations["decimal_result"]["expected_crcs"]),
            description=str(smoke_expectations["decimal_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("(4+5)*3"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="parenthesized_result",
            expected_crcs=tuple(smoke_expectations["parenthesized_result"]["expected_crcs"]),
            description=str(smoke_expectations["parenthesized_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("2^8"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="power_result",
            expected_crcs=tuple(smoke_expectations["power_result"]["expected_crcs"]),
            description=str(smoke_expectations["power_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("1/0", result_delay_ms=3000))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="divide_by_zero_error",
            expected_crcs=tuple(smoke_expectations["divide_by_zero_error"]["expected_crcs"]),
            description=str(smoke_expectations["divide_by_zero_error"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="prompt_after_divide_by_zero",
            expected_crcs=tuple(smoke_expectations["prompt_after_divide_by_zero"]["expected_crcs"]),
            description=str(smoke_expectations["prompt_after_divide_by_zero"]["description"]),
        )
    )

    runner.send_sequence(build_paced_expression_sequence("graph(1,x)", result_delay_ms=3000))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="graphing_mode_result",
            expected_crcs=tuple(smoke_expectations["graphing_mode_result"]["expected_crcs"]),
            description=str(smoke_expectations["graphing_mode_result"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="prompt_after_graphing_mode",
            expected_crcs=tuple(smoke_expectations["prompt_after_graphing_mode"]["expected_crcs"]),
            description=str(smoke_expectations["prompt_after_graphing_mode"]["description"]),
        )
    )

    runner.send_sequence(build_program_menu_sequence())
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="program_menu_open",
            expected_crcs=tuple(smoke_expectations["program_menu_open"]["expected_crcs"]),
            description=str(smoke_expectations["program_menu_open"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="prompt_after_menu_close",
            expected_crcs=tuple(smoke_expectations["prompt_after_menu_close"]["expected_crcs"]),
            description=str(smoke_expectations["prompt_after_menu_close"]["description"]),
        )
    )

    runner.send_sequence(build_paced_token_sequence(("5", "2nd", "enter", "alpha", "sto")))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="memory_store_variable",
            expected_crcs=tuple(smoke_expectations["memory_store_variable"]["expected_crcs"]),
            description=str(smoke_expectations["memory_store_variable"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("x+2"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="memory_recall_variable",
            expected_crcs=tuple(smoke_expectations["memory_recall_variable"]["expected_crcs"]),
            description=str(smoke_expectations["memory_recall_variable"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_token_sequence(("0", "2nd", "enter", "alpha", "sto")))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="memory_clear_variable",
            expected_crcs=tuple(smoke_expectations["memory_clear_variable"]["expected_crcs"]),
            description=str(smoke_expectations["memory_clear_variable"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("x"))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="memory_recall_after_clear",
            expected_crcs=tuple(smoke_expectations["memory_recall_after_clear"]["expected_crcs"]),
            description=str(smoke_expectations["memory_recall_after_clear"]["description"]),
        )
    )

    runner.send_sequence(DEFAULT_PROMPT_CLEAR_SEQUENCE)
    runner.send_sequence(build_paced_expression_sequence("99999999999999^9", result_delay_ms=3500))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="overflow_error",
            expected_crcs=tuple(smoke_expectations["overflow_error"]["expected_crcs"]),
            description=str(smoke_expectations["overflow_error"]["description"]),
        )
    )

    runner.send_sequence((
        press("clear"),
        pause(600, note="dismiss the overflow path"),
    ))
    runner.send_sequence(build_paced_expression_sequence("2+", result_delay_ms=2500))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="syntax_error_screen",
            expected_crcs=tuple(smoke_expectations["syntax_error_screen"]["expected_crcs"]),
            description=str(smoke_expectations["syntax_error_screen"]["description"]),
        )
    )

    runner.send_sequence((
        press("clear"),
        pause(600, note="dismiss the syntax-error screen"),
        press("clear"),
        pause(800, note="stabilize the restored prompt"),
    ))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="syntax_error_prompt_restore",
            expected_crcs=tuple(smoke_expectations["syntax_error_prompt_restore"]["expected_crcs"]),
            description=str(smoke_expectations["syntax_error_prompt_restore"]["description"]),
        )
    )

    runner.send_sequence(build_paced_expression_sequence("hex(255)", result_delay_ms=3000))
    checkpoints.append(
        capture_checkpoint(
            runner=runner,
            name="archive_program_execution",
            expected_crcs=tuple(smoke_expectations["archive_program_execution"]["expected_crcs"]),
            description=str(smoke_expectations["archive_program_execution"]["description"]),
        )
    )

    return checkpoints


def summarize_checkpoints(checkpoints: Sequence[Mapping[str, object]]) -> dict[str, object]:
    passed = [checkpoint for checkpoint in checkpoints if bool(checkpoint.get("passed"))]
    failed = [checkpoint for checkpoint in checkpoints if not bool(checkpoint.get("passed"))]
    return {
        "checkpoint_count": len(checkpoints),
        "passed_count": len(passed),
        "failed_count": len(failed),
        "failed_names": [str(checkpoint["name"]) for checkpoint in failed],
        "success": not failed,
    }
