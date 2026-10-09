"""Compare the Gxu font module's pure text functions with the reference under emulation:
GxuDetermineControlCode (0x140689b30), GxuFontStripCodes (0x14068ba40) and the line-break rule
(0x14068f500).

    python tools/gxu_probe.py
"""
import struct
import subprocess
import sys

from oracle import Oracle
from wowbin import ROOT

EXE = ROOT / "build" / "cmake" / "bin" / "Release" / "ProbeTest.exe"

TEXTS = [
    "A", "|", "||", "|cff112233x", "|cFF00ff00Hello", "|cff1122", "|r", "|R", "|n", "|N", "\r\nx", "\n", "\rz",
    "|Hitem:123|h[Sword]|h", "|Hitem:1|h", "|Hplayer:Bob|h[Bob]|h|r", "|TInterface\\Icons\\Foo:16|t",
    "|T12345:0|t", "|Tx|t", "|A:atlas:14:14|a", "|Aatlas|a", "|A:x|a", "|Kq12|k", "|Kf3|kx", "|K|k", "|W", "|w",
    "|a", "|t", "|h", "|k", "|x", "|cnRED:hi", "\xc3\xa9", "\xe6\x97\xa5",
    "|Hchannel:|K12|k|h[x]|h",
]
FLAGS = [0, 0x100, 0x200, 0x400, 0x800, 0x1000]
MASKS = [0, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x1f00]
BREAKS = [(0, 65), (0x2d, 0x2c), (0x2d, 0x41), (0x3b, 0x41), (0x2f, 0x41), (0x41, 0x7c), (0x41, 0xffffffff),
          (0x20, 0x41), (0x41, 0x20), (0x41, 0xa0), (0x41, 0x42), (0x4e00, 0x4e01), (0x41, 0x4e00), (0x4e00, 0x41),
          (0x201c, 0x4e00), (0x4e00, 0x3002), (0x3002, 0x4e00), (0x24, 0x4e00), (0x4e00, 0x21), (0xff0c, 0x41),
          (0xac00, 0xac01), (0x41, 0x3001), (0x41, 0xfe50), (0x4e00, 0xff70)]


def ours(*args: str) -> str:
    return subprocess.run([str(EXE), *args], capture_output=True, text=True, encoding="latin-1").stdout.strip()


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    o = Oracle()
    diffs = 0
    total = 0

    def check(label: str, reference: str, mine: str) -> None:
        nonlocal diffs, total
        total += 1
        if reference != mine:
            diffs += 1
            print(f"DIFF  {label:50} reference={reference!r} frostlite={mine!r}")

    for text in TEXTS:
        raw = text.encode("latin-1")
        for flags in FLAGS:
            out = o.alloc(b"\0" * 16)
            address = o.cstr(raw)
            kind = o.call(0x140689B30, [address, out, out + 4, flags, out + 8]) & 0xFFFFFFFF
            advance, color, code = struct.unpack("<iII", bytes(o.uc.mem_read(out, 12)))
            if kind >= 0x80000000:
                kind -= 1 << 32
            reference = f"{kind} {advance} {color:08x} {code:08x}"
            check(f"ctrl {text[:24]!r} {flags:#x}", reference, ours("ctrl", raw.hex() or "-", f"{flags:x}"))
        for mask in MASKS:
            out = o.alloc(b"\0" * 64)
            o.call(0x14068BA40, [o.cstr(raw), len(raw), mask, out, 64])
            reference = bytes(o.read_cstr(out)).hex()
            check(f"strip {text[:24]!r} {mask:#x}", reference, ours("strip", raw.hex() or "-", f"{mask:x}"))
    for previous, code in BREAKS:
        reference = str(o.call(0x14068F500, [previous, code]) & 0xFF)
        check(f"break {previous:#x} {code:#x}", reference, ours("break", str(previous), str(code)))
    print(f"{total - diffs}/{total} match")


if __name__ == "__main__":
    main()
