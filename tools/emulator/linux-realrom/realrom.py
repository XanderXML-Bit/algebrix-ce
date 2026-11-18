#!/usr/bin/env python3
"""Linux real-ROM driver for Algebrix CE on upstream CEmu ``cemu-headless``.

Nothing here downloads or ships a ROM. Point ``ALGEBRIX_ROM`` (or ``--rom``) at
a ROM dumped from a calculator you own. See README.md in this directory.

Subcommands
-----------
deploy  REPO OUT_DIR            boot the ROM, transfer bin/*/*.8xp (archived),
                                arTIfiCE (RAM) and the four CE libs (archived),
                                launch CALCENG through arTIfiCE and save
                                OUT_DIR/launched.ce at the ``>`` prompt.
cases   STATE OUT_DIR EXPR...   one fresh emulator per expression: type it,
                                ENTER, wait, save cNN.bmp + screen hash, and
                                record whether the calculator reset.
                                --then trace,down,... presses those keys after
                                the result and saves cNN-kK.bmp after each
                                (e.g. TRACE opens the launcher result viewer).
hops    STATE EXPR...           one emulator, expressions in sequence; logs every
                                program entry (name + stack pointer). Needs the
                                debug build from cemu-headless-debug.patch.
result  STATE PREFIX EXPR...    one emulator, expressions in sequence; after each,
                                reads the launcher's static g_response (address
                                from --map, default bin/launcher/CALCENG.map)
                                and prints the full result text when it starts
                                with PREFIX. The home screen shows one row per
                                result (UP opens the full-text viewer); this
                                reads the whole text without pressing keys.
                                Needs the debug build (peek/fill).
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent

# Program entry point (userMem) and the lowest legal stack byte on OS 5.x.
USER_MEM = 0xD1A881
STACK_LIMIT = 0xD19881
# CEmu autotester hash constants (tests/autotester/autotester.cpp hash_consts).
VRAM_START = 0xD40000
VRAM_16_SIZE = 320 * 240 * 2


def _crc32c_table() -> list[int]:
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC32C = _crc32c_table()


def autotester_crc(data: bytes) -> str:
    """The CEmu autotester ``hash`` value: CRC-32C (Castagnoli, reflected,
    init/xorout 0xFFFFFFFF; tests/autotester/crc32.hpp), NOT zlib's CRC-32.
    Over VRAM_START/VRAM_16_SIZE this is what every ``expected_crcs`` /
    ``expected_CRCs`` value in tests/cemu and tests/emulator compares against."""
    c = 0xFFFFFFFF
    t = _CRC32C
    for b in data:
        c = t[(c ^ b) & 0xFF] ^ (c >> 8)
    return f"{c ^ 0xFFFFFFFF:08X}"

# TI-84 Plus CE ALPHA layer exactly as printed on the keycaps (row order:
# math..÷ = A..M, log..2 = N..Z); src/core/launcher/main.c uses the same
# table, so text typed here is what a user typing from the keycaps gets.
ALPHA = dict(a="math", b="apps", c="prgm", d="-1", e="sin", f="cos", g="tan", h="^", i="^2", j=",", k="(", l=")", m="/", n="log", o="7", p="8", q="9", r="*", s="ln", t="4", u="5", v="6", w="-", x="sto", y="1", z="2")


def keys_for(text: str) -> list[str]:
    out: list[str] = []
    for ch in text:
        if ch.isdigit() or ch in "+-*/^(),.":
            out.append(ch)
        elif ch == ";":
            out.append("apps")  # matrix row separator (launcher: APPS)
        elif ch.isalpha():
            out += ["alpha", ALPHA[ch.lower()]]
        else:
            raise ValueError(f"no key mapping for {ch!r}")
    return out


class Emu:
    def __init__(self, *, binary: str, rom: str | None = None, image: str | None = None,
                 stderr_path: pathlib.Path | None = None):
        if (rom is None) == (image is None):
            raise ValueError("pass exactly one of rom/image")
        args = ["--image", image] if image else ["--rom", rom]
        if stderr_path is None:
            fd, name = tempfile.mkstemp(suffix=".log")
            os.close(fd)
            stderr_path = pathlib.Path(name)
        self.stderr_path = stderr_path
        self._err = open(self.stderr_path, "w")
        try:
            self.p = subprocess.Popen([binary, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=self._err, text=True, bufsize=1)
        except OSError:
            self._err.close()
            raise
        assert self.p.stdin is not None and self.p.stdout is not None
        self.stdin, self.stdout = self.p.stdin, self.p.stdout
        line = self.stdout.readline().strip()
        if "READY" not in line:
            self.quit()
            raise RuntimeError(f"cemu-headless did not start: {line!r}")

    def cmd(self, c: str) -> str:
        self.stdin.write(c + "\n")
        self.stdin.flush()
        r = self.stdout.readline().strip()
        if not r.startswith("OK"):
            raise RuntimeError(f"{c!r} -> {r!r}")
        return r

    def run(self, ms: int) -> None:
        self.cmd(f"run {ms}")

    def key(self, k: str, after_ms: int = 150) -> None:
        self.cmd(f"key {k}")
        self.run(after_ms)

    def type(self, text: str, gap_ms: int = 120) -> None:
        for k in keys_for(text):
            self.key(k, gap_ms)

    def shot(self, path: pathlib.Path) -> None:
        self.cmd(f"screenshot {path}")

    def screen_hash(self) -> str:
        return self.cmd("screen-hash").split()[-1]

    def peek(self, addr: int, n: int) -> bytes:
        """Raw memory read. Needs the debug build (cemu-headless-debug.patch)."""
        return bytes.fromhex(self.cmd(f"peek {addr:06X} {n}").split()[-1])

    def vram(self) -> bytes:
        return self.peek(VRAM_START, VRAM_16_SIZE)

    def vram_crc(self) -> str:
        """Autotester-compatible screen CRC of the current LCD buffer."""
        return autotester_crc(self.vram())

    def quit(self) -> str:
        """Stop the emulator (kill after 30 s) and return its stderr log."""
        if self.p.poll() is None:
            try:
                self.stdin.write("quit\n")
                self.stdin.flush()
            except OSError:
                pass
            try:
                self.p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait(timeout=10)
        if not self._err.closed:
            self._err.close()
        return self.stderr_path.read_text(errors="replace")

    def __enter__(self) -> "Emu":
        return self

    def __exit__(self, *exc) -> None:
        self.quit()


def reset_seen(log: str) -> bool:
    return "Reset triggered" in log or "NMI reset" in log


def _binary(args) -> str:
    b = args.cemu or os.environ.get("CEMU_HEADLESS")
    if not b:
        sys.exit("set CEMU_HEADLESS or pass --cemu")
    return b


def cmd_deploy(args) -> None:
    rom = args.rom or os.environ.get("ALGEBRIX_ROM")
    if not rom:
        sys.exit("set ALGEBRIX_ROM or pass --rom (an owned ROM; never download one)")
    repo = pathlib.Path(args.repo)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    mods = sorted(repo.glob("bin/*/*.8xp"))
    names = [m.name for m in mods]
    if not mods or len(set(names)) != len(names):
        sys.exit(f"expected unique built modules under {repo}/bin, found {len(mods)}")
    hw = repo / "deploy/hardware-ready"
    extra = [hw / n for n in ("arTIfiCE.8xp", "fileioc.8xv", "graphx.8xv", "keypadc.8xv", "libload.8xv")]
    with Emu(binary=_binary(args), rom=rom) as e:
        e.run(6000)
        for m in mods + extra:
            e.cmd(f"send-file {'ram' if m.name.startswith('arTIfiCE') else 'archive'} {m}")
        e.run(1000)
        for k, wait in (("clear", 500), ("prgm", 1200), ("enter", 1200), ("enter", 4000), ("enter", 6000)):
            e.key(k, wait)
        e.shot(out / "launch.bmp")
        e.cmd(f"save-state {out / 'launched.ce'}")
    print(json.dumps({"modules": len(mods), "state": str(out / "launched.ce")}))


def run_cases(binary: str, state: str, out: pathlib.Path, exprs: list[str],
              wait_ms: int = 6000, then: list[str] | None = None) -> list[dict]:
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for i, expr in enumerate(exprs):
        bmp = out / f"c{i:02d}.bmp"
        with Emu(binary=binary, image=state, stderr_path=out / f"c{i:02d}.stderr.log") as e:
            e.run(300)
            e.type(expr)
            e.cmd("key enter")
            e.run(wait_ms)
            e.shot(bmp)
            h = e.screen_hash()
            after = []
            for k, name in enumerate(then or [], 1):
                e.key(name, 600)
                kb = out / f"c{i:02d}-k{k}.bmp"
                e.shot(kb)
                after.append({"key": name, "screen_hash": e.screen_hash(), "bmp": str(kb)})
        log = e.quit()
        results.append({"i": i, "expr": expr, "screen_hash": h, "reset": reset_seen(log),
                        "bmp": str(bmp), "then": after})
    (out / "cases.json").write_text(json.dumps(results, indent=1))
    return results


def run_battery(binary: str, state: str, out: pathlib.Path, cases: list[dict],
                wait_ms: int = 6000, settle_ms: int = 1000) -> list[dict]:
    """One fresh emulator per battery case (from the launched state): type
    ``input_expression`` with the launcher key map, ENTER, wait, then record the
    autotester-compatible VRAM CRC twice ``settle_ms`` apart (``stable`` is
    False if the screen is still changing, e.g. a blinking cursor), a BMP for
    pixel inspection, and whether the calculator reset. Needs the debug build."""
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for case in cases:
        name = case["name"]
        bmp = out / f"{name}.bmp"
        try:
            keys_for(case["input_expression"])
        except ValueError as exc:
            # e.g. ';' (matrix row separator) has no launcher key at all.
            results.append({"name": name, "expr": case["input_expression"], "crc": None,
                            "untypeable": str(exc)})
            continue
        with Emu(binary=binary, image=state, stderr_path=out / f"{name}.stderr.log") as e:
            e.run(300)
            e.type(case["input_expression"])
            e.cmd("key enter")
            e.run(wait_ms)
            first = e.vram_crc()
            e.run(settle_ms)
            crc = e.vram_crc()
            e.shot(bmp)
        log = e.quit()
        results.append({"name": name, "expr": case["input_expression"], "crc": crc,
                        "stable": crc == first, "reset": reset_seen(log), "bmp": str(bmp)})
    (out / "battery.json").write_text(json.dumps(results, indent=1))
    return results


def cmd_battery(args) -> None:
    cases = json.loads(pathlib.Path(args.cases).read_text())["cases"]
    if args.only:
        cases = [c for c in cases if c["name"] in args.only]
    for r in run_battery(_binary(args), args.state, pathlib.Path(args.out), cases, args.wait_ms):
        print(json.dumps(r))


def cmd_cases(args) -> None:
    then = [k for k in (args.then or "").split(",") if k]
    for r in run_cases(_binary(args), args.state, pathlib.Path(args.out), args.exprs, args.wait_ms,
                       then):
        print(json.dumps(r))


HOP_RE = re.compile(r"^\[HOP\] pc=([0-9A-F]+) spl=([0-9A-F]+) op1=(\S*)")


def run_hops(binary: str, state: str, exprs: list[str], wait_ms: int = 6000) -> dict:
    with Emu(binary=binary, image=state) as e:
        e.run(300)
        e.cmd(f"bp {USER_MEM:06X}")
        for expr in exprs:
            e.type(expr)
            e.cmd("key enter")
            e.run(wait_ms)
            e.key("clear", 300)
    log = e.quit()
    hops = [{"prog": m.group(3), "spl": int(m.group(2), 16)}
            for m in map(HOP_RE.match, log.splitlines()) if m]
    return {"hops": hops, "reset": reset_seen(log),
            "min_spl": min((h["spl"] for h in hops), default=None)}


CE_IPC_MAGIC = 0xCE84
# ce_response_t: magic(2) version(1) ok(1) show_steps(1) settings last_answer,
# then result[MAX_RESULT_LEN]. The header is 30 B on the eZ80 today; the
# prefix is accepted anywhere in the first 64 B so a settings_t change does
# not silently break the reader (the struct is zeroed before each read).
RESULT_WINDOW = 64 + 512


def ipc_version(repo: pathlib.Path = HERE.parents[2]) -> int:
    """CE_IPC_VERSION from src/split/ipc.h of the checkout this file is in."""
    m = re.search(r"#define\s+CE_IPC_VERSION\s+(\d+)u?", (repo / "src/split/ipc.h").read_text())
    if not m:
        raise RuntimeError("CE_IPC_VERSION not found in src/split/ipc.h")
    return int(m.group(1))


def parse_response(raw: bytes, prefix: str, version: int) -> dict:
    """Decode a ce_response_t image read from RAM.

    fresh: the magic and version are valid (same test as ce_response_read).
    text:  the result string, only when fresh and it starts with prefix.
    """
    fresh = (len(raw) >= 4 and (raw[0] | (raw[1] << 8)) == CE_IPC_MAGIC
             and raw[2] == version)
    text = ""
    i = raw.find(prefix.encode("latin1")) if fresh else -1
    if 0 <= i < 64:
        end = raw.find(b"\0", i)
        text = raw[i:end if end >= 0 else len(raw)].decode("latin1")
    return {"fresh": fresh, "ok": fresh and raw[3] == 1, "text": text}


def map_symbol(map_path: str | os.PathLike, symbol: str) -> int:
    """Address of a symbol in a CEdev linker map (e.g. bin/launcher/CALCENG.map)."""
    for line in pathlib.Path(map_path).read_text(errors="replace").splitlines():
        parts = line.split("=")
        if len(parts) == 2 and parts[0].strip() == symbol:
            return int(parts[1].strip(), 16)
    raise KeyError(f"{symbol} not in {map_path}")


def _peek(e: "Emu", addr: int, n: int) -> bytes:
    return bytes.fromhex(e.cmd(f"peek {addr:06X} {n}").split("OK peek ", 1)[1].strip())


def run_result_text(binary: str, state: str, map_path: str | os.PathLike, prefix: str,
                    exprs: list[str], wait_ms: int = 6000) -> list[dict]:
    """Full result text of each evaluation, read from the launcher's g_response.

    The home screen shows one row per result, and the launcher keeps the whole
    CE_RESP in its static g_response (its result viewer reads it there).
    Before each expression the magic is cleared, so a stale response from an
    earlier evaluation is never reported:
    the text is returned only when g_response holds a fresh header with the
    right magic and CE_IPC_VERSION and its result starts with prefix.
    """
    addr = map_symbol(map_path, "_g_response")
    version = ipc_version()
    results = []
    with Emu(binary=binary, image=state) as e:
        e.run(300)
        for expr in exprs:
            e.cmd(f"fill {addr:06X} 2 0")
            e.type(expr)
            e.cmd("key enter")
            e.run(wait_ms)
            raw = _peek(e, addr, RESULT_WINDOW)
            results.append({"expr": expr, **parse_response(raw, prefix, version)})
            e.key("clear", 300)
        log = e.quit()
    for r in results:
        r["reset"] = reset_seen(log)
    return results


def cmd_result(args) -> None:
    for r in run_result_text(_binary(args), args.state, args.map, args.prefix, args.exprs,
                             args.wait_ms):
        print(json.dumps(r))


def cmd_hops(args) -> None:
    r = run_hops(_binary(args), args.state, args.exprs, args.wait_ms)
    for h in r["hops"]:
        print(f"{h['prog']:<8} spl={h['spl']:06X}")
    print(json.dumps({"reset": r["reset"], "hops": len(r["hops"]),
                      "min_spl": f"{r['min_spl']:06X}" if r["min_spl"] else None}))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cemu", help="path to cemu-headless (default: $CEMU_HEADLESS)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("deploy")
    d.add_argument("repo")
    d.add_argument("out")
    d.add_argument("--rom")
    c = sub.add_parser("cases")
    c.add_argument("state")
    c.add_argument("out")
    c.add_argument("exprs", nargs="+")
    c.add_argument("--wait-ms", type=int, default=6000)
    c.add_argument("--then", help="comma-separated keys to press after the result, one shot each")
    h = sub.add_parser("hops")
    h.add_argument("state")
    h.add_argument("exprs", nargs="+")
    h.add_argument("--wait-ms", type=int, default=6000)
    t = sub.add_parser("result")
    t.add_argument("state")
    t.add_argument("prefix")
    t.add_argument("exprs", nargs="+")
    t.add_argument("--wait-ms", type=int, default=6000)
    t.add_argument("--map", default="bin/launcher/CALCENG.map",
                   help="CEdev map of the deployed CALCENG (for _g_response)")
    b = sub.add_parser("battery")
    b.add_argument("state")
    b.add_argument("out")
    b.add_argument("--cases", default=str(HERE.parents[2] / "tests/cemu/edge-cases/module-battery.json"))
    b.add_argument("--only", nargs="*", help="case names to run (default: all)")
    b.add_argument("--wait-ms", type=int, default=6000)
    args = ap.parse_args()
    {"deploy": cmd_deploy, "cases": cmd_cases, "hops": cmd_hops, "result": cmd_result,
     "battery": cmd_battery}[args.cmd](args)


if __name__ == "__main__":
    main()
