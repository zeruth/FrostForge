"""Functions carrying the anti-tamper return-address check.

The clean exe still has a protection tail appended to ~3.3k functions (1,433 Lua handlers
and 1,901 others): it compares the caller's return address with a code range held in
two globals (base 0x14261a0b0, length 0x14261a0b8) and looks it up in a table at
0x1400b9810; a failed check crashes in a deliberate loop. It is protection, not game
logic, and must not be ported. This lists each such function and the first
instruction of its check, so the decompile export and the recomp can skip it.

    python tools/retcheck.py    -> build/facts/retcheck.json
"""
from __future__ import annotations

from wowbin import Binary, hexa, write_facts

RANGE_BASE = 0x14261A0B0
RANGE_LEN = 0x14261A0B8


def main() -> None:
    b = Binary()
    # riprel_refs verifies each kept site is a real instruction boundary; on this one
    # global the raw byte scan had 101 mid-instruction matches (2026-10-08)
    unverified = len(b.riprel_refs(keep=lambda t: t in (RANGE_BASE, RANGE_LEN), verify=False))
    first: dict[int, int] = {}
    refs = b.riprel_refs(keep=lambda t: t in (RANGE_BASE, RANGE_LEN))
    for insn, _tgt in refs:
        f = b.func_containing(insn)
        if f is not None and (f not in first or insn < first[f]):
            first[f] = insn
    print(f"{unverified - len(refs)} pattern matches rejected as mid-instruction")
    out = write_facts("retcheck", {
        "range_globals": [hexa(RANGE_BASE), hexa(RANGE_LEN)],
        "functions": {hexa(f): hexa(i) for f, i in sorted(first.items())},
    })
    print(f"{len(first)} functions carry the return-address check -> {out}")


if __name__ == "__main__":
    main()
