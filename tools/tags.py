"""The `// ref:` tags in src/: are they sound, and how much is ported?

A tag claims a frostlite function IS a reference function. This checks every tag against the
corpus (the address must be a function there, or a code address when the tag says so), finds
duplicates (two frostlite functions claiming one reference function), and splits the tagged
functions into ported and stubbed (FROST_UNIMPLEMENTED in the body). It is the start of the
recomp measurement Frozen has for 3.3.5.

    python tools/tags.py            # summary + problems
    python tools/tags.py --list     # every tag
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

from corpus import load_corpus
from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
TAG = re.compile(r"//\s*ref:\s*(?:FUN_|0x)([0-9a-fA-F]{9,})")
# a line opening a function definition (indented ones too, for in-class members)
DEF = re.compile(r"^\s*(?!//|#|return\b|if\b|while\b|for\b|else\b|do\b)[^;]*\b([A-Za-z_][\w:]*)\s*\([^;]*$")


def scan() -> list[dict]:
    tags = []
    for p in sorted((ROOT / "src").rglob("*")):
        if p.suffix not in (".cpp", ".hpp", ".h", ".c"):
            continue
        lines = p.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            m = TAG.search(line)
            if not m:
                continue
            # the definition the tag sits above: the next line that opens a function
            name, body_start = None, None
            for j in range(i + 1, min(i + 12, len(lines))):
                if DEF.match(lines[j]):
                    # the first identifier followed by '(' is the function's own name, also
                    # when a parameter is a function pointer: `void F(void (*cb)())`
                    name = re.search(r"\b([A-Za-z_][\w:]*)\s*\(", lines[j]).group(1)
                    body_start = j
                    break
            stub = False
            if body_start is not None:
                depth, opened = 0, False
                for k in range(body_start, len(lines)):
                    depth += lines[k].count("{") - lines[k].count("}")
                    opened = opened or "{" in lines[k]
                    if "FROST_UNIMPLEMENTED" in lines[k]:
                        stub = True
                    if opened and depth <= 0:
                        break
            tags.append({"addr": int(m.group(1), 16), "file": str(p.relative_to(ROOT)), "line": i + 1,
                         "name": name, "stub": stub, "header": p.suffix in (".hpp", ".h"),
                         "label": "0x" in line.split("ref:", 1)[1][:4]})
    return tags


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    tags = scan()
    cor = load_corpus()
    b = Binary()
    problems = []
    by_addr = defaultdict(list)
    for t in tags:
        by_addr[t["addr"]].append(t)
        where = f"{t['file']}:{t['line']}"
        if t["header"] and t["name"] is None:
            # headers may carry inline / template definitions; a tag on a bare declaration
            # (no body follows) belongs on the definition in the .cpp
            problems.append(f"{where}: tag on a declaration; tag the definition in the .cpp")
        if t["name"] is None:
            problems.append(f"{where}: no function definition follows the tag")
        if t["label"]:
            if not b.is_code(t["addr"]):
                problems.append(f"{where}: 0x{t['addr']:x} is not in .text")
        elif t["addr"] not in cor:
            problems.append(f"{where}: FUN_{t['addr']:x} is not a function in the corpus")
    for a, ts in by_addr.items():
        if len(ts) > 1:
            problems.append(f"FUN_{a:x} claimed {len(ts)} times: " + ", ".join(f"{t['name']} ({t['file']})" for t in ts))

    ported = [t for t in tags if not t["stub"]]
    print(f"{len(tags)} tags: {len(ported)} ported, {len(tags) - len(ported)} stubs "
          f"(of {len(cor)} reference functions)")
    if args.list:
        for t in sorted(tags, key=lambda t: t["addr"]):
            print(f"  {t['addr']:x}  {'stub  ' if t['stub'] else 'ported'}  {t['name']}  ({t['file']}:{t['line']})")
    for p in problems:
        print("PROBLEM:", p)
    if problems:
        sys.exit(1)


if __name__ == "__main__":
    main()
