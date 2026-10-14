"""Virtual function tables, found without RTTI.

The game code is built without RTTI: vtables sit back to back in .rdata with no
CompleteObjectLocator slot between them (only std:: classes keep one), and the ~3.5k
`.?AV` type descriptors do not pair with them by order. So a vtable's start
is recovered from the code that takes its address: constructors and destructors do
`lea reg, [rip+vtable]` then store it into the object. Every lea target that lands
inside a run of function pointers is a vtable start; its slots run to the next start.

    python tools/vtables.py     -> build/facts/vtables.json
"""
from __future__ import annotations

import bisect
import struct
from collections import defaultdict

from wowbin import Binary, hexa, write_facts


def main() -> None:
    b = Binary()
    s, d = b.bytes_of(".rdata")
    n = len(d) // 8
    q = struct.unpack_from(f"<{n}Q", d)
    # any pointer into .text: leaf stubs (ret, xor eax,eax; ret) have no .pdata entry
    is_fp = [b.is_code(v) for v in q]

    # lea targets (8-aligned, pointing at a function pointer) and who takes them
    takers: dict[int, set[int]] = defaultdict(set)
    for insn, tgt in b.riprel_refs(keep=lambda t: s <= t < s + 8 * n and (t - s) % 8 == 0 and is_fp[(t - s) // 8]):
        if s <= tgt < s + 8 * n and (tgt - s) % 8 == 0 and is_fp[(tgt - s) // 8]:
            f = b.func_containing(insn)
            if f is not None:
                takers[tgt].add(f)
    # static objects: a global whose vptr the linker fills in (bgs::ssl's default filter
    # factory 0x142d39828 -> 0x1428783c8) has no code that takes its vtable's address, so
    # read the vptr out of the data instead. Only targets no lea-found vtable already covers,
    # so a pointer into the middle of one cannot split it.
    lea_starts = sorted(takers)

    def covered(t: int) -> bool:
        """True when t is a slot of the lea-found vtable at or before it."""
        k = bisect.bisect_right(lea_starts, t) - 1
        if k < 0:
            return False
        i = (lea_starts[k] - s) // 8
        stop = (t - s) // 8
        while i < stop and is_fp[i]:
            i += 1
        return i == stop

    holders: dict[int, set[int]] = defaultdict(set)
    for sec in (".data",):
        ds, dd = b.bytes_of(sec)
        for j in range(0, len(dd) - 7, 8):
            t = struct.unpack_from("<Q", dd, j)[0]
            if s <= t < s + 8 * n and (t - s) % 8 == 0 and is_fp[(t - s) // 8] and not covered(t):
                holders[t].add(ds + j)
    starts = sorted(set(takers) | set(holders))

    vtables = []
    for k, vt in enumerate(starts):
        i = (vt - s) // 8
        limit = (starts[k + 1] - s) // 8 if k + 1 < len(starts) else n
        slots = []
        while i < limit and is_fp[i]:
            slots.append(hexa(q[i]))
            i += 1
        entry = {"vtable": hexa(vt), "slots": slots, "referenced_by": sorted(hexa(f) for f in takers[vt])}
        if vt in holders:
            entry["held_by"] = sorted(hexa(g) for g in holders[vt])
        vtables.append(entry)

    # how often each function appears as a slot: shared slots are base-class
    # implementations or pure-virtual / empty stubs
    slot_use = defaultdict(int)
    for v in vtables:
        for f in v["slots"]:
            slot_use[f] += 1
    out = write_facts("vtables", {
        "vtables": vtables,
        "shared_slots": {f: c for f, c in sorted(slot_use.items(), key=lambda x: -x[1])[:200]},
    })
    sizes = sorted(len(v["slots"]) for v in vtables)
    print(f"{len(vtables)} vtables, {sum(sizes)} slots, median {sizes[len(sizes)//2]} max {sizes[-1]} -> {out}")
    print("most shared slot targets (stubs / base implementations):")
    for f, c in sorted(slot_use.items(), key=lambda x: -x[1])[:5]:
        print(f"  {f}  in {c} vtables")


if __name__ == "__main__":
    main()
