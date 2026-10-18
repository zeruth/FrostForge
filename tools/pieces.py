"""Chained .pdata pieces: out-of-line parts of a larger function, not functions. And the
functions Ghidra made at a label strictly inside a .pdata function (interior): the parent jumps
there, nothing calls it (no call instruction targets it), and the split cost both halves their
registers (16 lost-register stubs at 0x140a0xxxx, 2026-10-18). ApplyFacts.java removes both
kinds (DEFAULT-named only) and lets the parent re-absorb the code.

    python tools/pieces.py      -> build/facts/pieces.json
"""
from corpus import load_corpus
from wowbin import Binary, hexa, write_facts


def main() -> None:
    b = Binary()
    interior = sorted(a for a in load_corpus() if a not in b.chained
                      and (r := b.func_containing(a)) is not None and r != a)
    out = write_facts("pieces", {
        "pieces": [hexa(p) for p in sorted(b.chained)],
        "parent": {hexa(p): hexa(r) for p, r in sorted(b.chained.items())},
        "interior": [hexa(a) for a in interior],
    })
    print(f"{len(b.chained)} chained pieces, {len(interior)} interior entries, {len(b.func_starts)} root functions -> {out}")


if __name__ == "__main__":
    main()
