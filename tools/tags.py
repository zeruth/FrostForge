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
TAG = re.compile(r"(?://|/\*|^\s*\*+)\s*ref:\s*(?:FUN_|0x)([0-9a-fA-F]{9,})")
# the trees tags live in: frostlite's own, and the vendored code ported from the client
TAG_ROOTS = ("src", "vendor/lua-5.1.4/src")
# a line opening a function definition (indented ones too, for in-class members)
DEF = re.compile(r"^\s*(?!//|/\*|\*|#|return\b|if\b|while\b|for\b|else\b|do\b)[^;]*\b([A-Za-z_][\w:]*)\s*\([^;]*$")


# a whole declaration on one line: `DNLIGHT();`, `void F(int a) const override;`
DECL = re.compile(r"^\s*(?!return\b)[\w:<>,*&~\s]*\b[~\w:]+\s*\([^;{]*\)\s*(const\s*)?(noexcept\s*)?"
                  r"(override\s*|final\s*|=\s*(0|default|delete)\s*)?;\s*$")
# lines a stub carries without porting anything: braces, `(void)param;`, a constant return
_FILLER = re.compile(r"^(\{|\}|\};|\(void\)\s*[\w.\->\[\]]+;|return(\s+(nullptr|true|false|-?\d+[uU]?|-?\d*\.\d+f?|\{\}|0x[0-9a-fA-F]+))?;)$")


def IS_REAL(line: str) -> bool:
    """Whether a body line does anything a stub would not."""
    code = line.split("//", 1)[0].strip()
    return code != "" and not _FILLER.match(code)


def scan() -> list[dict]:
    tags = []
    for p in sorted(q for root in TAG_ROOTS for q in (ROOT / root).rglob("*")):
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
                if DECL.match(lines[j].split("//", 1)[0]):
                    break   # a declaration: the definition elsewhere carries the tag
                if DEF.match(lines[j]):
                    # the first identifier followed by '(' is the function's own name, also
                    # when a parameter is a function pointer: `void F(void (*cb)())`
                    # destructors keep their `~`: `CScriptStatus::~CScriptStatus(`
                    name = re.search(r"((?:\b[A-Za-z_]\w*::)*~?\b[A-Za-z_]\w*)\s*\(", lines[j]).group(1)
                    body_start = j
                    break
            stub = False
            real = False
            sites = []
            if body_start is not None:
                depth, opened = 0, False
                for k in range(body_start, len(lines)):
                    text = lines[k]
                    depth += text.count("{") - text.count("}")
                    was_open = opened
                    opened = opened or "{" in text
                    if "FROST_UNIMPLEMENTED" in text:
                        stub = True
                        note = text.split("//", 1)[1].strip() if "//" in text else ""
                        sites.append({"line": k + 1, "note": note})
                    elif was_open and IS_REAL(text):
                        real = True
                    if opened and depth <= 0:
                        break
            tags.append({"addr": int(m.group(1), 16), "file": str(p.relative_to(ROOT)), "line": i + 1,
                         "name": name, "stub": stub, "partial": stub and real, "sites": sites,
                         "header": p.suffix in (".hpp", ".h"),
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
    echoes = 0
    for t in tags:
        where = f"{t['file']}:{t['line']}"
        if t["name"] is None:
            if any(o["name"] for o in by_addr[t["addr"]] if o is not t):
                echoes += 1     # the definition carries the tag as well: harmless
            elif t["header"]:
                problems.append(f"{where}: tag on a declaration; tag the definition in the .cpp")
            else:
                problems.append(f"{where}: no function definition follows the tag")
        if t["label"]:
            if not b.is_code(t["addr"]):
                problems.append(f"{where}: 0x{t['addr']:x} is not in .text")
        elif t["addr"] not in cor:
            problems.append(f"{where}: FUN_{t['addr']:x} is not a function in the corpus")
    for a, ts in by_addr.items():
        named = [t for t in ts if t["name"]]
        if len(named) > 1:
            problems.append(f"FUN_{a:x} claimed {len(named)} times: " + ", ".join(f"{t['name']} ({t['file']})" for t in named))

    named = [t for t in tags if t["name"]]
    stubs = [t for t in named if t["stub"] and not t["partial"]]
    partial = [t for t in named if t["partial"]]
    print(f"{len(named)} tags: {len(named) - len(stubs) - len(partial)} ported, {len(partial)} partial, {len(stubs)} stubs "
          f"(of {len(cor)} reference functions); {echoes} declaration tags repeat their definition's")
    if args.list:
        for t in sorted(tags, key=lambda t: t["addr"]):
            print(f"  {t['addr']:x}  {'stub  ' if t['stub'] else 'ported'}  {t['name']}  ({t['file']}:{t['line']})")
    for p in problems:
        print("PROBLEM:", p)
    if problems:
        sys.exit(1)


if __name__ == "__main__":
    main()
