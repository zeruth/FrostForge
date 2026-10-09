"""Every instruction that references an address range, with the function holding it.

    python tools/xrefs.py 0x142cf57a8 [0x142cf57d8]   # [lo, hi) -- hi defaults to lo + 1
"""
import argparse

from wowbin import Binary


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("lo", type=lambda s: int(s, 16))
    ap.add_argument("hi", nargs="?", type=lambda s: int(s, 16))
    args = ap.parse_args()
    b = Binary()
    hi = args.hi if args.hi is not None else args.lo + 1
    for site, target, kind in b.xrefs(args.lo, hi):
        func = b.func_containing(site)
        print(f"{site:#x} -> {target:#x} {kind:6} in {func:#x}" if func else f"{site:#x} -> {target:#x} {kind}")


if __name__ == "__main__":
    main()
