"""Compare FrostLite's SStrPrintf with the reference's, case by case.

Runs every case in test/storm/printf_cases.txt through the reference's 0x1402610c0 under
emulation (tools/oracle.py) and through build/.../PrintfTest.exe, and prints each case whose
output differs.

    python tools/printf_oracle.py
"""
import math
import subprocess
import sys

from oracle import Oracle
from wowbin import ROOT

CASES = ROOT / "test" / "storm" / "printf_cases.txt"
EXE = ROOT / "build" / "cmake" / "bin" / "Release" / "PrintfTest.exe"


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    cases = []
    for line in CASES.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        cases.append((fields[0], [(f[0], f[2:]) for f in fields[1:]]))

    ours = subprocess.run([str(EXE), str(CASES)], capture_output=True, text=True,
                          encoding="latin-1").stdout.split("\n")

    o = Oracle()
    diffs = 0
    for n, (fmt, args) in enumerate(cases):
        slots = []
        for t, v in args:
            if t == "i":
                slots.append(int(v) & 0xFFFFFFFFFFFFFFFF if int(v) >= 0 else (int(v) + (1 << 64)))
            elif t == "I":
                slots.append(int(v) & 0xFFFFFFFFFFFFFFFF)
            elif t in ("u", "c"):
                slots.append(int(v))
            elif t == "n":
                slots.append(0)
            elif t == "s":
                slots.append(o.cstr(v))
            elif t == "d":
                value = {"inf": math.inf, "-inf": -math.inf, "nan": math.nan}.get(v)
                slots.append(Oracle.f64(float(v) if value is None else value))
        reference = o.sstr_vprintf(fmt, slots)
        mine = ours[n] if n < len(ours) else "<missing>"
        if reference != mine:
            diffs += 1
            print(f"DIFF  {fmt!r:22} reference={reference!r}  frostlite={mine!r}")
    print(f"{len(cases)} cases, {diffs} differ from the reference")


if __name__ == "__main__":
    main()
