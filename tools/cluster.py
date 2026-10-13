"""The reference functions around an address and what the port does with each.

    python tools/cluster.py 14065fd40 [--span 0x1000] [--globals]

Lists the corpus functions within the span: size, whether the port has a hand port (and where),
translates it, adapts to it or leaves it a stub, and the run's call count when --hits is given.
With --globals, the data globals the cluster's decompilations touch, and who else touches them:
a hand port of a module has to take over the state the rest of the image shares.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr")
    ap.add_argument("--span", default="0x1000")
    ap.add_argument("--hits")
    ap.add_argument("--globals", action="store_true")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    center = int(args.addr.removeprefix("FUN_"), 16)
    span = int(args.span, 0)
    manifest = json.loads((ROOT / "src/gen/manifest.json").read_text(encoding="utf-8"))
    translated = set(manifest.get("translated", {}))
    stubbed = manifest.get("stubbed", {})
    ports = {}
    for r in json.loads((ROOT / "build/fidelity.json").read_text()):
        f = r["file"].replace("\\", "/")
        if not f.startswith("src/gen/"):
            ports[r["addr"]] = f"{f}:{r['line']} {r['name']}"
    hits = {}
    if args.hits:
        for line in Path(args.hits).read_text().splitlines():
            a, n = line.split()
            hits[a] = int(n)
    corpus = {}
    with (ROOT / "build/corpus.jsonl").open(encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            a = int(d["addr"], 16)
            if abs(a - center) <= span:
                corpus[a] = d
    touched = {}
    for a in sorted(corpus):
        d = corpus[a]
        key = f"{a:x}"
        if key in ports:
            state = "PORT " + ports[key]
        elif key in translated:
            state = "translated"
        elif key in stubbed:
            state = "stub: " + stubbed[key][:60]
        else:
            state = "-"
        mark = ">" if a == center else " "
        print(f"{mark} FUN_{key} {d.get('size', 0):>5} {hits.get(key, ''):>6}  {state}")
        if args.globals:
            for g in set(re.findall(r"DAT_(14[0-9a-f]{7})", d.get("c", ""))):
                touched.setdefault(g, set()).add(key)
    if args.globals and touched:
        print("\nglobals the cluster touches, and every function that touches them:")
        users = {g: set() for g in touched}
        with (ROOT / "build/corpus.jsonl").open(encoding="utf-8") as f:
            for line in f:
                d = json.loads(line)
                c = d.get("c", "")
                for g in users:
                    if f"DAT_{g}" in c:
                        users[g].add(d["addr"])
        for g in sorted(touched):
            outside = sorted(users[g] - {f"{a:x}" for a in corpus})
            print(f"  DAT_{g}: {len(users[g])} functions" + (f"; outside the span: {', '.join(outside[:8])}" if outside else ""))


if __name__ == "__main__":
    main()
