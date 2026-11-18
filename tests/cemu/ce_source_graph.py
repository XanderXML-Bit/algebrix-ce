"""Source/header closure of the shipped split CE build (not legacy targets)."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def _assignment(text: str, name: str, *, required: bool = True) -> str:
    text = text.replace("\\\n", " ")
    value = None
    for match in re.finditer(rf"^{name}\s*(:=|\+=|\?=|=)\s*([^\n]*)", text, re.M):
        op, rhs = match.groups()
        rhs = rhs.split("#", 1)[0].strip()
        assert "$" not in rhs, f"unsupported expansion in {name}: {rhs}"
        if op == "+=":
            value = (value or "") + " " + rhs
        elif op != "?=" or value is None:
            value = rhs
    assert value is not None or not required, f"missing literal {name} assignment"
    return value or ""


def ce_module_sources(root: Path = ROOT) -> dict[str, set[str]]:
    """Read the active module lists, each makefile, and shared runtime sources.

    Ignore old Makefile.core/Makefile.mat etc.: make all does not invoke them.
    Fail closed on variable-based source lists rather than silently omit files.
    """
    top = (root / "Makefile").read_text(encoding="utf-8")
    modules = (_assignment(top, "SPLIT_MODULES") + " " +
               _assignment(top, "WORKER_MODULES")).split()
    shared = (root / "split/program.mk").read_text(encoding="utf-8")
    common = set(re.findall(r"src/[\w./-]+\.[ch]\b", shared))
    result = {}
    for module in modules:
        mk = (root / f"split/Makefile.{module}").read_text(encoding="utf-8")
        sources = (_assignment(mk, "PROGRAM_SOURCES") + " " +
                   _assignment(mk, "PROGRAM_RUNTIME_SOURCES", required=False)).split()
        assert sources and all(s.startswith("src/") and s.endswith(".c") for s in sources), sources
        # Also catches literal PROGRAM_RUNTIME_SOURCES and force-included headers.
        result[module] = set(sources) | common | set(re.findall(r"src/[\w./-]+\.[ch]\b", mk))
    return result


def ce_source_closure(root: Path = ROOT) -> list[str]:
    pending = set().union(*ce_module_sources(root).values())
    seen = set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        path = root / name
        assert path.is_file(), f"missing CE source: {name}"
        seen.add(name)
        text = path.read_text(encoding="utf-8")
        for inc in re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.M):
            candidates = [path.parent / inc, root / "src" / inc]
            header = next((p.resolve() for p in candidates if p.is_file()), None)
            assert header is not None, f"unresolved CE include {inc} in {name}"
            pending.add(header.relative_to(root.resolve()).as_posix())
    return sorted(seen)
