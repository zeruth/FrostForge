"""Call-sequence fidelity of the `// ref:` ports, binary against binary.

For every tagged frostlite function this compares the direct calls its compiled body makes
(FrostLite.exe + FrostLite.pdb) with the direct calls the reference function makes
(WowClassic-clean.exe), in order. A frostlite callee counts as the reference callee its own
`// ref:` tag names; an untagged callee cannot be matched. The score is the longest common
subsequence over the reference sequence, so a port that skips a call, calls the wrong
function or calls them in another order loses fidelity; the report lists what is missing.

Both sides are optimized builds, so a small callee inlined on one side and not the other shows
as missing or extra: read the body before trusting a low score.

    python tools/fidelity.py                          # every port, worst first
    python tools/fidelity.py --file model2/           # ports under src/model2
    python tools/fidelity.py --addr 1411ae6a0 --show  # one port, both sequences side by side
    python tools/fidelity.py --below 0.8 --min-calls 3
"""
from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import capstone

import tags as tagscan
from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
PORT_EXE = ROOT / "build/cmake/bin/Release/FrostLite.exe"
PORT_PDB = ROOT / "build/cmake/bin/Release/FrostLite.pdb"
LLVM = Path("C:/Program Files/LLVM/bin")

PROC = re.compile(r"S_[GL]PROC32 \[size = \d+\] `(.*)`$")
ADDR = re.compile(r"addr = ([0-9A-Fa-f]{4}):(\d+), code size = (\d+)")


def pdb_procs(binary: Binary) -> list[tuple[int, int, str]]:
    """(va, size, name) of every compiled function in the PDB."""
    out = subprocess.run([str(LLVM / "llvm-pdbutil.exe"), "dump", "-symbols", str(PORT_PDB)],
                         capture_output=True, text=True, errors="replace").stdout
    procs = []
    name = None
    for line in out.splitlines():
        m = PROC.search(line)
        if m:
            name = m.group(1)
            continue
        if name is not None:
            m = ADDR.search(line)
            if m:
                section = binary.sections[int(m.group(1), 16) - 1]
                procs.append((section.start + int(m.group(2)), int(m.group(3)), name))
                name = None
    return sorted(set(procs))


def symbolize(addrs: list[int], base: int) -> dict[int, tuple[str, int]]:
    """address -> (source file, line) of its first instruction."""
    text = "\n".join(hex(a - base) for a in addrs) + "\n"
    out = subprocess.run([str(LLVM / "llvm-symbolizer.exe"), f"--obj={PORT_EXE}", "--relative-address",
                          "--no-inlines", "--functions=none"], input=text, capture_output=True, text=True,
                         errors="replace").stdout
    lines = [l for l in out.splitlines()]
    result = {}
    # one location line per address, blank-line separated
    blocks, cur = [], []
    for l in lines:
        if l.strip() == "":
            if cur:
                blocks.append(cur)
                cur = []
        else:
            cur.append(l)
    if cur:
        blocks.append(cur)
    for a, block in zip(addrs, blocks):
        loc = block[-1]
        m = re.match(r"(.*):(\d+):\d+$", loc)
        if m:
            result[a] = (m.group(1).replace("\\", "/").lower(), int(m.group(2)))
    return result


def short_name(name: str) -> str:
    name = re.sub(r"<.*>", "", name)
    return name.split("::")[-1]


def call_targets(binary: Binary, va: int, size: int, md, extent=None) -> list[int]:
    """Direct call/jmp targets in [va, va+size) that leave the function, in address order."""
    code = binary.image[binary.off(va):binary.off(va) + size]
    targets = []
    for insn in md.disasm(code, va):
        if insn.mnemonic in ("call", "jmp") and insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            target = insn.operands[0].imm
            if insn.mnemonic == "jmp" and va <= target < va + size:
                continue
            if extent is not None and extent(target):
                continue
            targets.append(target)
    return targets


def resolve_thunk(binary: Binary, md, target: int, depth: int = 0) -> int:
    """A function that is only `jmp elsewhere` is its destination."""
    if depth > 4 or binary.section_of(target) is None:
        return target
    code = binary.image[binary.off(target):binary.off(target) + 16]
    for insn in md.disasm(code, target):
        if insn.mnemonic == "jmp" and insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            return resolve_thunk(binary, md, insn.operands[0].imm, depth + 1)
        break
    return target


def ref_calls(ref: Binary, md, addr: int) -> list[int]:
    """The reference function's callees, its chained pieces included, in address order."""
    index = getattr(ref, "_pieces_by_root", None)
    if index is None:
        index = defaultdict(list)
        for i in range(len(ref.piece_starts)):
            index[ref.piece_root[i]].append((ref.piece_starts[i], ref.piece_ends[i]))
        ref._pieces_by_root = index
    pieces = index.get(addr, [])
    if not pieces:
        return []

    def inside(t):
        i = bisect.bisect_right(ref.piece_starts, t) - 1
        return i >= 0 and t < ref.piece_ends[i] and ref.piece_root[i] == addr

    out = []
    for begin, end in sorted(pieces):
        for t in call_targets(ref, begin, end - begin, md, inside):
            t = resolve_thunk(ref, md, t)
            i = bisect.bisect_right(ref.piece_starts, t) - 1
            out.append(ref.piece_root[i] if i >= 0 and t < ref.piece_ends[i] else t)
    return out


def lcs(a: list, b: list) -> int:
    if not a or not b:
        return 0
    prev = [0] * (len(b) + 1)
    for x in a:
        cur = [0]
        for j, y in enumerate(b):
            cur.append(prev[j] + 1 if x == y else max(prev[j + 1], cur[j]))
        prev = cur
    return prev[-1]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", help="only ports whose source path contains this")
    ap.add_argument("--addr", nargs="*", help="only these reference addresses")
    ap.add_argument("--below", type=float, default=1.01, help="only scores below this")
    ap.add_argument("--min-calls", type=int, default=1, help="only references making this many calls")
    ap.add_argument("--show", action="store_true", help="print both sequences")
    args = ap.parse_args()

    ref = Binary()
    port = Binary(PORT_EXE)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    procs = pdb_procs(port)
    proc_at = {va: (size, name) for va, size, name in procs}
    by_short = defaultdict(list)
    for va, size, name in procs:
        by_short[short_name(name)].append(va)

    tags = [t for t in tagscan.scan() if t["name"] and not t.get("label")]
    wanted = {short_name(t["name"]) for t in tags}
    candidates = sorted({va for s in wanted for va in by_short.get(s, [])})
    where = symbolize(candidates, port.base)

    # tag -> compiled function: same file, same short name, first body line after the tag
    port_of_ref: dict[int, int] = {}
    ref_of_port: dict[int, int] = {}
    tag_of_ref: dict[int, dict] = {}
    for t in tags:
        tfile = t["file"].replace("\\", "/").lower()
        best = None
        for va in by_short.get(short_name(t["name"]), []):
            loc = where.get(va)
            if loc is None or not loc[0].endswith(tfile):
                continue
            if loc[1] <= t["line"]:
                continue
            if best is None or loc[1] < where[best][1]:
                best = va
        if best is not None and (t["addr"] not in port_of_ref):
            port_of_ref[t["addr"]] = best
            ref_of_port.setdefault(best, t["addr"])
            tag_of_ref[t["addr"]] = t
    name_of_ref = {a: proc_at[v][1] for a, v in port_of_ref.items()}

    def port_name(r):
        return name_of_ref.get(r, f"FUN_{r:x} (unlinked)")

    rows = []
    for r, pva in port_of_ref.items():
        t = tag_of_ref[r]
        if args.file and args.file.replace("\\", "/") not in t["file"].replace("\\", "/"):
            continue
        if args.addr and f"{r:x}" not in {a.lower().removeprefix("0x").removeprefix("fun_") for a in args.addr}:
            continue
        if t["stub"]:
            continue
        rseq = ref_calls(ref, md, r)
        if len(rseq) < args.min_calls:
            continue
        size = proc_at[pva][0]
        praw = [resolve_thunk(port, md, x) for x in call_targets(port, pva, size, md)]
        pseq = [ref_of_port.get(x, ("port", x)) for x in praw]
        common = lcs(rseq, pseq)
        score = common / len(rseq) if rseq else 1.0
        if score >= args.below:
            continue
        pset = set(pseq)
        missing = [x for x in rseq if x not in pset]
        rows.append((score, r, t, rseq, pseq, praw, missing))

    rows.sort(key=lambda x: (x[0], -len(x[3])))
    for score, r, t, rseq, pseq, praw, missing in rows:
        print(f"{score:5.2f}  FUN_{r:x}  {port_name(r)}  ({t['file']}:{t['line']})  ref {len(rseq)} calls, "
              f"port {len(pseq)}")
        if missing and not args.show:
            seen = set()
            for x in missing:
                if x in seen:
                    continue
                seen.add(x)
                print(f"         missing FUN_{x:x}  {port_name(x)}")
        if args.show:
            print("   reference:")
            for x in rseq:
                print(f"      FUN_{x:x}  {port_name(x)}")
            print("   port:")
            for x, raw in zip(pseq, praw):
                label = f"FUN_{x:x}" if isinstance(x, int) else "untagged"
                print(f"      {label:16} {proc_at.get(raw, (0, hex(raw)))[1]}")
    print(f"{len(rows)} ports listed; {len(port_of_ref)} tags tied to compiled functions", file=sys.stderr)


if __name__ == "__main__":
    main()
