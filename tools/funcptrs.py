"""Data slots that hold a function's address and are called through: build/facts/funcptrs.json.

Ghidra types such a slot as a bare `pointer`, so at every call through it the decompiler
guesses the arguments from the registers it sees set, and often guesses short. curl keeps its
allocators in slots like these (Curl_ccalloc = PTR__calloc_base_142d3e8b8), and about half the
calls through that one came out as `(*PTR__calloc_base_142d3e8b8)(1)` with the size dropped:
Curl_multi_handle (0x14216d2d0) allocated with a garbage size, its init failed and its error
path destroyed lists that were never set up. ApplyFacts types each slot listed here as a pointer
to its target's own signature, as it already does for the import slots.

A slot qualifies when its static value is a function start and some instruction calls through
it (`call [rip+X]`) or loads it (`mov reg, [rip+X]`). Import slots and vtables are typed by their
own facts and left out.

    python tools/funcptrs.py
"""
from __future__ import annotations

import json
import re
import struct
from collections import defaultdict

from wowbin import FACTS, ROOT, Binary, hexa, write_facts

CORPUS = ROOT / "build" / "corpus.jsonl"

CALL_RIP = re.compile(rb"\xff\x15(....)", re.S)


def main() -> None:
    b = Binary()
    data = [s for s in b.sections if not s.exec]

    def in_data(va: int) -> bool:
        return any(s.start <= va < s.end for s in data)

    skip: list[tuple[int, int]] = []
    imports = json.loads((FACTS / "imports.json").read_text())
    lo, hi = (int(x, 16) for x in imports["block"])
    skip.append((lo, hi + 8))
    vt = json.loads((FACTS / "vtables.json").read_text())
    for v in vt["vtables"]:
        start = int(v["vtable"], 16)
        skip.append((start, start + 8 * len(v["slots"])))
    skip.sort()

    def skipped(va: int) -> bool:
        return any(a <= va < e for a, e in skip)

    # the functions Ghidra has: .pdata alone misses the leaves that need no unwind data, and
    # the CRT's allocators are among them (_calloc_base's slot holds 0x140282ca8, a leaf)
    funcs = set()
    with open(CORPUS, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'\{"addr": ?"([0-9a-f]+)"', line)
            if m:
                funcs.add(int(m.group(1), 16))

    def resolve(va: int) -> int | None:
        # through `jmp rel32` thunks, which Ghidra leaves undefined: _calloc_base's slot holds
        # 0x140282ca8, `jmp 0x1402857f8`
        for _ in range(4):
            if va in funcs:
                return va
            if not b.text.start <= va < b.text.end or b.image[b.off(va)] != 0xE9:
                return None
            va = va + 5 + b.i32(va + 1)
        return None

    def keep(va: int) -> bool:
        return va % 8 == 0 and in_data(va) and not skipped(va) and resolve(b.u64(va)) is not None

    sites: dict[int, list[str]] = defaultdict(list)
    for insn, tgt in b.riprel_refs(keep=lambda t: keep(t)):
        # only the loads (8b): a lea takes the slot's address, a store replaces its value
        if b.image[b.off(insn) + 1] == 0x8B:
            sites[tgt].append("load")
    start, text = b.bytes_of(".text")
    for m in CALL_RIP.finditer(text):
        insn = start + m.start()
        tgt = insn + 6 + struct.unpack("<i", m.group(1))[0]
        if keep(tgt) and b.is_insn_start(insn):
            sites[tgt].append("call")

    out = {hexa(slot): {"value": hexa(b.u64(slot)), "target": hexa(resolve(b.u64(slot))), "calls": kinds.count("call"), "loads": kinds.count("load")}
           for slot, kinds in sorted(sites.items())}
    path = write_facts("funcptrs", out)
    print(f"{len(out)} function-pointer slots ({sum(v['calls'] for v in out.values())} calls through them) -> {path}")


if __name__ == "__main__":
    main()
