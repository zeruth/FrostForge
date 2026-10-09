"""Call tree from the corpus: what a function calls, to a given depth, in call order.

Reads callees out of the decompiled C (build/corpus.jsonl): every FUN_<addr> and every
named function the corpus knows, in the order they first appear. Each line shows the
size, the name, and the source file (srcrefs) when one is known, so a path such as
entry -> login screen can be read in one view before porting it top-down.

    python tools/calltree.py 1402112a0                 # depth 3
    python tools/calltree.py 1402112a0 --depth 5 --max-children 25
    python tools/calltree.py 1402112a0 --skip-crt      # hide CRT / library leaves

Indirect calls (vtables, function pointers) do not appear: the corpus text has no
target for them.
"""
from __future__ import annotations

import argparse
import re
import sys

from corpus import load_corpus, load_facts, strip_retcheck

_FUN = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9,})\s*\(")
_NAMED = re.compile(r"\b([A-Za-z_][\w]*(?:::[A-Za-z_]\w*)*)\s*\(")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--max-children", type=int, default=40)
    ap.add_argument("--skip-crt", action="store_true", help="do not expand functions Ghidra's FID named")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    cor = load_corpus()
    by_name = {}
    for a, r in cor.items():
        by_name.setdefault(r["name"].replace("::", "::"), a)
        by_name.setdefault(r["name"].split("::")[-1], a)
    src = load_facts("srcrefs").get("functions", {})

    def callees(a: int) -> list[int]:
        r = cor.get(a)
        if not r or "c" not in r:
            return []
        body, _ = strip_retcheck(r["c"])
        body = body.split("{", 1)[1] if "{" in body else body  # skip the signature
        out, seen = [], set()
        for m in re.finditer(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9,})\s*\(|\b([A-Za-z_]\w*(?:::\w+)*)\s*\(", body):
            if m.group(1):
                t = int(m.group(1), 16)
            else:
                t = by_name.get(m.group(2))
                if t is None:
                    continue
            if t != a and t not in seen and t in cor:
                seen.add(t)
                out.append(t)
        return out

    def label(a: int) -> str:
        r = cor[a]
        files = src.get(f"0x{a:x}")
        where = f"  [{', '.join(f.rsplit('/', 1)[-1] for f in files)}]" if files else ""
        return f"{a:x} {r['name']} ({r.get('size', '?')}b){where}"

    def is_lib(a: int) -> bool:
        n = cor[a]["name"]
        return not n.startswith(("FUN_", "thunk_FUN", "Lua::", "Storm::", "CDataAllocator::")) and "::" not in n

    printed = set()

    def walk(a: int, depth: int, indent: str) -> None:
        print(indent + label(a) + ("  (see above)" if a in printed and depth else ""))
        if a in printed or depth == 0 or (args.skip_crt and is_lib(a) and indent):
            return
        printed.add(a)
        kids = callees(a)
        for k in kids[: args.max_children]:
            walk(k, depth - 1, indent + "  ")
        if len(kids) > args.max_children:
            print(indent + f"  ... {len(kids) - args.max_children} more")

    walk(int(args.addr, 16), args.depth, "")


if __name__ == "__main__":
    main()
