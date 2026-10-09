"""Who calls a function: every direct call/jmp rel32 to each address, with the caller and
its source file.

    python tools/callers.py 0x1402caf30 [0x... ...]
"""
import json
import sys

from wowbin import ROOT, Binary


def main() -> None:
    b = Binary()
    src = {}
    path = ROOT / "build" / "facts" / "srcrefs.json"
    if path.exists():
        src = json.loads(path.read_text())["functions"]
    for arg in sys.argv[1:]:
        target = int(arg, 16)
        sites = b.callers(target)
        print(f"{target:#x}: {len(sites)} sites")
        for va, kind in sites:
            f = b.func_containing(va)
            where = src.get(f"{f:#x}", [""])[0] if f else ""
            print(f"  {kind:4} {va:#x}  in {f:#x}  {where}" if f else f"  {kind:4} {va:#x}  (no function)")


if __name__ == "__main__":
    main()
