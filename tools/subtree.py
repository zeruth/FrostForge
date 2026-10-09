"""How big is everything a function reaches: the unique functions under it (direct calls, to a
depth), their total size, the biggest ones, and the source files involved. For sizing a port
before starting it.

    python tools/subtree.py 0x140227910 [--depth 8] [--top 40] [--stop 0x...,0x...]
"""
import argparse
import collections
import json
import re

from wowbin import ROOT

CALL = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9})\b")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", type=lambda s: int(s, 16))
    ap.add_argument("--depth", type=int, default=8)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--stop", default="", help="comma-separated addresses not to descend into")
    args = ap.parse_args()

    funcs = {}
    for line in open(ROOT / "build" / "corpus.jsonl", encoding="utf-8"):
        d = json.loads(line)
        funcs[int(d["addr"], 16)] = d
    src = json.loads((ROOT / "build" / "facts" / "srcrefs.json").read_text())["functions"]
    stop = {int(s, 16) for s in args.stop.split(",") if s}

    seen = {args.addr: 0}
    frontier = [args.addr]
    for depth in range(1, args.depth + 1):
        nxt = []
        for f in frontier:
            if f in stop or f not in funcs:
                continue
            for m in CALL.finditer(funcs[f].get("c") or ""):
                callee = int(m.group(1), 16)
                if callee not in seen and callee in funcs:
                    seen[callee] = depth
                    nxt.append(callee)
        frontier = nxt

    total = sum(funcs[f]["size"] for f in seen)
    print(f"{len(seen)} functions, {total} bytes (depth {args.depth})")
    files = collections.Counter()
    for f in seen:
        for s in src.get(f"{f:#x}", []):
            files[s] += 1
    print("source files:", ", ".join(f"{k} ({v})" for k, v in files.most_common(25)))
    for f in sorted(seen, key=lambda f: -funcs[f]["size"])[: args.top]:
        where = src.get(f"{f:#x}", [""])[0]
        print(f"  {f:#x} {funcs[f]['size']:6}b d{seen[f]} {where}")


if __name__ == "__main__":
    main()
