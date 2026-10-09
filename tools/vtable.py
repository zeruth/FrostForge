"""Print the slots of vtables (or any table of code pointers) in the clean exe.

    python tools/vtable.py 0x1428e6528 [0x... ...] [--count 12]

Stops at the first slot that is not a code pointer (or after --count slots).
"""
import argparse
import json

from wowbin import ROOT, Binary


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", nargs="+", type=lambda s: int(s, 16))
    ap.add_argument("--count", type=int, default=24)
    args = ap.parse_args()
    b = Binary()
    sizes = {}
    for line in open(ROOT / "build" / "corpus.jsonl", encoding="utf-8"):
        d = json.loads(line)
        sizes[int(d["addr"], 16)] = (d["name"], d["size"])
    for table in args.addr:
        print(f"{table:#x}:")
        for i in range(args.count):
            p = b.u64(table + 8 * i)
            if not b.is_code(p):
                break
            name, size = sizes.get(p, ("?", 0))
            print(f"  +{8 * i:#04x} {p:#x} {name} ({size}b)")


if __name__ == "__main__":
    main()
