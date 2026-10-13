"""Port TSList globals whose link offset disagrees with the reference's.

A Storm list keeps the offset of its link inside the element as its first word, and the
reference's static data holds it: 0x142cfcdf0 (SE3's fading instances) starts with 0x388. A port
list built without an offset (`TSList<T, TSGetLink<T>, 8> s_x;`) threads the element's link at
+0, which for any element whose link is elsewhere links the wrong field: on 2026-10-13 six of
SE3's seven instance lists did, the fading list closed into a cycle with the playing list, and
the credits screen hung in SE3UpdateFades.

For each namespace-scope TSList/TSExplicitList global that names its address (on its line or in
the comment above), this reads the reference's first word there and the port's constructor
argument. A default-built list over a nonzero reference offset is an error; an `offsetof(T, m)`
argument is shown with the reference value for a look (or checked by a static_assert beside it).

    python tools/listoffsets.py
"""
from __future__ import annotations

import re
from pathlib import Path

from wowbin import ROOT, Binary

DECL = re.compile(r"^(?:static\s+)?(?:STORM_)?TS(?:Explicit)?List<[^;]*?>\s*(\w+)\s*(?:\(([^;]*)\))?\s*;(.*)$")
ADDR = re.compile(r"\b(?:DAT_|0x)(14[0-9a-f]{7})\b")


def main() -> None:
    b = Binary()
    errors = 0
    rows = 0
    for p in sorted((ROOT / "src").rglob("*.cpp")):
        if "gen" in p.relative_to(ROOT).parts:
            continue
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        for i, line in enumerate(lines):
            m = DECL.match(line)
            if m is None:
                continue
            name, arg, tail = m.group(1), (m.group(2) or "").strip(), m.group(3)
            found = ADDR.findall(tail)
            j = i - 1
            while not found and j >= 0 and lines[j].lstrip().startswith("//"):
                found = ADDR.findall(lines[j])
                j -= 1
            where = f"{p.relative_to(ROOT).as_posix()}:{i + 1}"
            if not found:
                print(f"{where}  {name}: no reference address, not checked")
                continue
            addr = int(found[0], 16)
            ref = b.u64(addr)
            rows += 1
            if not arg:
                if ref != 0:
                    errors += 1
                    print(f"{where}  {name}: ERROR built at +0, the reference's list (0x{addr:x}) threads +0x{ref:x}")
            elif re.fullmatch(r"0x[0-9a-fA-F]+|\d+", arg):
                if int(arg, 0) != ref:
                    errors += 1
                    print(f"{where}  {name}: ERROR built at {arg}, the reference's threads +0x{ref:x}")
            else:
                print(f"{where}  {name}: {arg} -- the reference's threads +0x{ref:x}")
    print(f"{rows} lists checked, {errors} wrong")


if __name__ == "__main__":
    main()
