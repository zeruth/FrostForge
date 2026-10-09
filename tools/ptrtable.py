"""Dump a table of pointers from the clean exe, with the C string each one points at.

    python tools/ptrtable.py 0x142d3e5c0 40      # 40 pointer entries
    python tools/ptrtable.py 0x1428e98e0 4 --str # also when entries are plain strings
"""
import argparse

from wowbin import Binary


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", type=lambda s: int(s, 16))
    ap.add_argument("count", type=int)
    ap.add_argument("--stride", type=lambda s: int(s, 0), default=8)
    args = ap.parse_args()
    b = Binary()
    for i in range(args.count):
        va = args.addr + i * args.stride
        ptr = b.u64(va)
        text = None
        if b.section_of(ptr) is not None:
            text = b.cstr(ptr)
        print(f"[{i:3}] 0x{va:x}: 0x{ptr:x} {text!r}")


if __name__ == "__main__":
    main()
