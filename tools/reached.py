"""What a run reached of the translated code, ranked by calls, startup apart from the rest.

A run with FROST_HITS=<file> writes how often each translated function was entered. Ranking
that file by calls mixes two kinds of reach: the image's C++ static initializers (run once at
startup through RefImage.cpp, hundreds of them each calling the same constructors) and code
the port's frame actually calls. Only the second is worth porting next by hand, and it is
buried under the first.

This splits them by the reference's call graph: a reached function with no reached caller is a
root, called from a hand port, through a pointer, or from the initializer table. Everything the
non-initializer roots reach is runtime; what only the initializers reach is startup.

    python tools/reached.py <hits> [--limit N] [--startup] [--roots]
"""
from __future__ import annotations

import argparse
import bisect
import json
from collections import defaultdict
from pathlib import Path

import numpy as np

from wowbin import ROOT, Binary

# the table FUN_14026b874 hands _initterm (tools/translate.py, Translator.INIT_TABLE)
INIT_TABLE = (0x14260e3f8, 0x142617540)


def read_hits(path: Path) -> dict[int, int]:
    hits = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2:
            hits[int(parts[0], 16)] = int(parts[1])
    return hits


def call_edges(b: Binary, targets: set[int]) -> dict[int, set[int]]:
    """callee -> the functions whose direct call/jmp rel32 lands on it, for callees in targets."""
    start, data = b.bytes_of(".text")
    n = len(data) - 5
    raw = np.frombuffer(data, dtype=np.uint8)
    disp = np.ndarray(shape=(n,), dtype="<i4", buffer=data, offset=1, strides=(1,)).astype(np.int64)
    pos = np.arange(n, dtype=np.int64)
    dest = pos + disp + (start + 5)
    want = np.array(sorted(targets), dtype=np.int64)
    hit = np.isin(dest, want) & ((raw[:n] == 0xE8) | (raw[:n] == 0xE9))
    callers: dict[int, set[int]] = defaultdict(set)
    for p in np.nonzero(hit)[0].tolist():
        va = start + p
        f = b.func_containing(va)
        if f is not None and f != int(dest[p]):
            callers[int(dest[p])].add(f)
    return callers


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hits")
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--startup", action="store_true", help="list the startup-only functions instead")
    ap.add_argument("--roots", action="store_true", help="only the runtime roots (called from hand code or a pointer)")
    args = ap.parse_args()

    hits = read_hits(Path(args.hits))
    manifest = json.loads((ROOT / "src/gen/manifest.json").read_text(encoding="utf-8"))
    translated = {int(a, 16) for a in manifest.get("translated", {})}
    reached = {a for a in hits if a in translated}

    b = Binary()
    inits = set()
    for slot in range(INIT_TABLE[0], INIT_TABLE[1], 8):
        p = b.u64(slot)
        if p:
            inits.add(p)
    callers = call_edges(b, reached)
    callees: dict[int, set[int]] = defaultdict(set)
    for callee, cs in callers.items():
        for c in cs:
            if c in reached:
                callees[c].add(callee)

    roots = {a for a in reached if not (callers.get(a, set()) & reached)}
    runtime_roots = roots - inits
    runtime: set[int] = set()
    todo = list(runtime_roots)
    while todo:
        a = todo.pop()
        if a in runtime:
            continue
        runtime.add(a)
        todo.extend(callees.get(a, ()))
    startup = reached - runtime

    if args.startup:
        pick = startup
    elif args.roots:
        pick = runtime_roots
    else:
        pick = runtime
    try:
        from corpus import load_corpus
        cor = load_corpus()
    except Exception:
        cor = {}
    rows = sorted(((hits[a], a) for a in pick), reverse=True)
    for n, a in rows[: args.limit]:
        e = cor.get(a) or {}
        size = e.get("size", 0) if isinstance(e, dict) else 0
        mark = "root" if a in roots else ""
        print(f"FUN_{a:x} {n:8d} {size:6d} {mark}")
    print(f"{len(reached)} translated reached: {len(runtime)} runtime ({len(runtime_roots)} roots), "
          f"{len(startup)} startup only ({len(roots & inits)} initializers)")


if __name__ == "__main__":
    main()
