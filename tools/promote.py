"""The translated functions a run reached, ranked for porting by hand.

    FROST_HITS=<file> FrostLite.exe        # the run writes each translated function's call count
    python tools/promote.py --hits <file> [--limit 40] [--file GlueMgr.cpp]

A function is ranked by how much of the run it carries: its call count and its size. Each row
says where its hand port belongs: the source file of the nearest hand-ported function by address
(the compiler lays out a source file's functions together), with that neighbour and the
distance. A port goes there, written from the decompilation with the disassembly as the
check, tagged `// ref: FUN_x`; the translator then adapts every translated caller to it (rerun
fidelity.py, then translate.py) and stops translating it.
"""

from __future__ import annotations

import argparse
import bisect
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hits", required=True, help="the run's FROST_HITS file")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--file", help="only functions whose suggested home contains this")
    ap.add_argument("--json", help="write the ranked list here too")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    hits = {}
    for line in Path(args.hits).read_text().splitlines():
        a, n = line.split()
        hits[int(a, 16)] = int(n)
    manifest = json.loads((ROOT / "src/gen/manifest.json").read_text(encoding="utf-8"))
    translated = {int(a, 16) for a in manifest.get("translated", {})}
    sizes, names = {}, {}
    with (ROOT / "build/corpus.jsonl").open(encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            a = int(d["addr"], 16)
            if a in hits:
                sizes[a] = d.get("size", 0)
                names[a] = d.get("name", "")
    # the hand ports, by address: where a neighbour of the reference lives in the port
    ports = []
    for r in json.loads((ROOT / "build/fidelity.json").read_text()):
        f = r["file"].replace("\\", "/")
        if not f.startswith("src/gen/"):
            ports.append((int(r["addr"], 16), f, r["name"]))
    ports.sort()
    keys = [p[0] for p in ports]

    def home(a: int):
        i = bisect.bisect_left(keys, a)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(ports):
                d = abs(ports[j][0] - a)
                if best is None or d < best[0]:
                    best = (d, ports[j])
        return best

    rows = []
    for a, n in hits.items():
        if a not in translated:
            continue
        size = sizes.get(a, 0)
        near = home(a)
        score = math.log2(n + 1) * size
        rows.append({"addr": f"{a:x}", "name": names.get(a, ""), "calls": n, "size": size, "score": round(score),
                     "home": near[1][1] if near else "", "neighbour": near[1][2] if near else "",
                     "distance": near[0] if near else None})
    rows.sort(key=lambda r: -r["score"])
    if args.file:
        rows = [r for r in rows if args.file in r["home"]]
    total = sum(r["size"] for r in rows)
    print(f"{len(rows)} translated functions reached, {total:,} bytes of reference code")
    for r in rows[:args.limit or None]:
        print(f"FUN_{r['addr']}  calls {r['calls']:>6}  size {r['size']:>5}  -> {r['home']} "
              f"(near {r['neighbour'][:40]}, {r['distance']:#x})")
    by_home = {}
    for r in rows:
        by_home.setdefault(r["home"], [0, 0])
        by_home[r["home"]][0] += 1
        by_home[r["home"]][1] += r["size"]
    print("\nby suggested home:")
    for h, (count, size) in sorted(by_home.items(), key=lambda kv: -kv[1][1])[:25]:
        print(f"  {count:>4} functions {size:>7,} bytes  {h}")
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
