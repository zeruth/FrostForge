"""The stubs (demoted or never translated) a root reaches through translated code, nearest first:
what a run from that root will hit as "[unimplemented]" or as a wrong value. Read it before a run.

    python tools/pathstubs.py <root hex> [<root hex> ...] [--depth N]
"""
import collections
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CALL = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9})\b")


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    depth = int(sys.argv[sys.argv.index("--depth") + 1]) if "--depth" in sys.argv else 8
    if "--depth" in sys.argv:
        args.remove(sys.argv[sys.argv.index("--depth") + 1])
    m = json.loads((ROOT / "src/gen/manifest.json").read_text(encoding="utf-8"))
    translated = set(m["translated"])
    stubbed = m["stubbed"]
    calls, names = {}, {}
    with open(ROOT / "build/corpus.jsonl", encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            calls[r["addr"]] = list(dict.fromkeys(CALL.findall(r.get("c", ""))))
            names[r["addr"]] = r["name"]
    seen = {}
    queue = collections.deque((a.lower().removeprefix("0x"), 0, None) for a in args)
    found = []
    while queue:
        a, d, parent = queue.popleft()
        if a in seen:
            continue
        seen[a] = parent
        if a in stubbed:
            found.append((d, a, stubbed[a], parent))
            continue
        if a not in translated or d >= depth:
            continue
        for c in calls.get(a, []):
            if c not in seen:
                queue.append((c, d + 1, a))
    print(len(seen), "functions walked;", len(found), "stubs on the way")
    for d, a, why, parent in found[:80]:
        print(f"{d:2} FUN_{a} {names.get(a, '')[:40]:40} <- FUN_{parent}  {why[:90]}")


if __name__ == "__main__":
    main()
