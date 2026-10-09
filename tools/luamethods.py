"""Condensed decompilations of a Lua method table's functions.

Each method generated from the reference's script-method template starts with the same
prologue (argument count check, self lookup through FrameScript_Object::GetObjectFromLua,
the usage error, a stack check, the lookup again) and ends with the pending-error check.
This prints, per method, the usage string and the lines after the prologue, with the
anti-tamper blocks left out, so a table of a hundred methods reads in one sitting.

    python tools/luamethods.py 0x142cf8610 93 > build/modules/frame_methods.txt
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable


def decompile(addr: int) -> str:
    out = ROOT / "build" / "modules" / "lm_tmp.c"
    subprocess.run([PY, str(ROOT / "tools" / "corpus.py"), "--addr", hex(addr), "--out", "lm_tmp"],
                   check=True, capture_output=True)
    return out.read_text(errors="replace")


def condense(text: str) -> list[str]:
    lines = text.splitlines()
    usage = None
    for line in lines:
        m = re.search(r'"(Usage:[^"]*)"', line)
        if m:
            usage = m.group(1)
            break
    # the body starts after the last self lookup of the prologue
    start = 0
    for i, line in enumerate(lines):
        if "FUN_140642590(" in line or "FUN_1406425f0(" in line:
            start = i
    # skip the closing braces of the prologue
    i = start + 1
    depth = 0
    while i < len(lines) and lines[i].strip() in ("}", "}}", ""):
        i += 1
    body = []
    skipping = False
    for line in lines[i:]:
        if "DAT_14261a0b0" in line or "unaff_retaddr[-" in line:
            skipping = True
        if "anti-tamper" in line:
            skipping = False
            body.append(line)
            continue
        if skipping:
            continue
        body.append(line)
    out = [f"  usage: {usage}"] if usage else []
    return out + body


def main() -> None:
    table = int(sys.argv[1], 16)
    count = int(sys.argv[2], 0)
    b = Binary()
    for i in range(count):
        name = b.cstr(b.u64(table + i * 16))
        fn = b.u64(table + i * 16 + 8)
        print(f"==== {name} @ {fn:#x}")
        for line in condense(decompile(fn)):
            print(line)
        print()


if __name__ == "__main__":
    main()
