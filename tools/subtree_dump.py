"""Dump the decompilation of every function a root reaches (within an address range) into one
file, in call order (breadth first), skipping ones already tagged in src/.

    python tools/subtree_dump.py 0x14213fc30 build/read/casc_all.c [--lo 0x142100000 --hi 0x142170000]
"""
import argparse
import collections
import json
import pathlib
import re

from wowbin import ROOT

CALL = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9})\b")
TAG = re.compile(r"ref: (?:FUN_)?(?:0x)?([0-9a-f]{9})")


def code_of(d: dict) -> str:
    return d.get("c", "").replace(chr(13), "")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", type=lambda s: int(s, 16))
    ap.add_argument("out")
    ap.add_argument("--lo", type=lambda s: int(s, 16), default=0)
    ap.add_argument("--hi", type=lambda s: int(s, 16), default=1 << 64)
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--all", action="store_true", help="include already-tagged functions")
    args = ap.parse_args()

    funcs = {}
    for line in open(ROOT / "build" / "corpus.jsonl", encoding="utf-8"):
        d = json.loads(line)
        funcs[int(d["addr"], 16)] = d
    tagged = set()
    for path in (ROOT / "src").rglob("*.[ch]pp"):
        for m in TAG.finditer(path.read_text(encoding="utf-8", errors="replace")):
            tagged.add(int(m.group(1), 16))

    seen = {args.addr}
    order = []
    queue = collections.deque([(args.addr, 0)])
    while queue:
        addr, depth = queue.popleft()
        d = funcs.get(addr)
        if not d:
            continue
        order.append(addr)
        if depth >= args.depth:
            continue
        for m in CALL.finditer(code_of(d)):
            callee = int(m.group(1), 16)
            if callee in seen or not (args.lo <= callee < args.hi):
                continue
            if callee in tagged and not args.all:
                continue
            seen.add(callee)
            queue.append((callee, depth + 1))

    with open(args.out, "w", encoding="utf-8") as out:
        for addr in order:
            d = funcs[addr]
            out.write(f"// ===== {d.get('name', hex(addr))} @ 0x{addr:x} ({d.get('size', 0)} bytes)\n")
            out.write(code_of(d) + "\n\n")
    print(f"{len(order)} functions -> {args.out}")


if __name__ == "__main__":
    main()
