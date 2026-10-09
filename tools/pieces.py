"""Chained .pdata pieces: out-of-line parts of a larger function, not functions.

    python tools/pieces.py      -> build/facts/pieces.json   (for RemovePieceFunctions.java)
"""
from wowbin import Binary, hexa, write_facts


def main() -> None:
    b = Binary()
    out = write_facts("pieces", {
        "pieces": [hexa(p) for p in sorted(b.chained)],
        "parent": {hexa(p): hexa(r) for p, r in sorted(b.chained.items())},
    })
    print(f"{len(b.chained)} chained pieces, {len(b.func_starts)} root functions -> {out}")


if __name__ == "__main__":
    main()
