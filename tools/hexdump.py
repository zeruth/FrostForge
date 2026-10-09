"""Hex-dump bytes of the clean exe at virtual addresses.

    python tools/hexdump.py 0x1428e70d8 16 [0x1428e7548 16 ...]
"""
import sys

from wowbin import Binary


def main() -> None:
    b = Binary()
    args = sys.argv[1:]
    for i in range(0, len(args), 2):
        va = int(args[i], 16)
        n = int(args[i + 1], 0)
        off = b.off(va)
        data = bytes(b.image[off:off + n])
        print(f"0x{va:x}: {data.hex(' ')}")


if __name__ == "__main__":
    main()
