"""Moves hand ports out of their file's anonymous namespace, so the translator can call them.

A hand port in an anonymous namespace has no public symbol: the translator cannot bind its
translated callers to it (an adapter needs the port's mangled name), so it translates the
function a second time, and the two copies share only what both tag. `translate.py --why`
lists them ("has no public symbol"). This tool closes the anonymous namespace before each such
definition (with the comment block above it) and reopens it after, leaving the code as it was.

    python tools/expose.py                  # every hand port --why names, dry run
    python tools/expose.py --apply          # rewrite the sources
    python tools/expose.py --addr FUN_x...  # only these

Skipped, with the reason printed: class members (the class itself is internal), static
functions, functions declared ahead of their definition inside the namespace, and definitions
not directly in a file-level anonymous namespace. A name another file also defines shows up as
LNK2005 at the next build: rename one of them, or leave it internal.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import tags as tagscan  # noqa: E402

DEF = re.compile(r"^[A-Za-z_][\w:<>,\s\*&]*?\b([A-Za-z_]\w*)\s*\(")


def blank_code(text: str) -> str:
    """The text with comments, strings and characters blanked (same length, newlines kept)."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", blank, text, flags=re.S)


def scopes_at(code: str, pos: int) -> list[str | None]:
    """The open scopes at `pos`: a namespace's name ("" anonymous), None for other braces."""
    stack: list[str | None] = []
    for m in re.finditer(r"\bnamespace\s+([\w:]*)\s*\{|[{}]", code[:pos]):
        tok = m.group(0)
        if tok.startswith("namespace"):
            stack.append(m.group(1) or "")
        elif tok == "{":
            stack.append(None)
        elif stack:
            stack.pop()
    return stack


def body_end(code: str, start: int) -> int | None:
    """The offset just past the brace that closes the first block opened at or after `start`."""
    depth = 0
    i = code.find("{", start)
    if i < 0:
        return None
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return j + 1
    return None


def plan(path: Path, entries: list[dict]) -> tuple[str, list[str]]:
    text = path.read_text(encoding="utf-8")
    code = blank_code(text)
    lines = text.split("\n")
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line) + 1)
    notes = []
    cuts = []
    for e in entries:
        name = e["name"]
        if "::" in name:
            notes.append(f"skip {name}: a class member (its class is internal)")
            continue
        tag_line = e["line"] - 1
        # the definition: the first line after the tag that opens a function named `name`
        def_line = None
        for j in range(tag_line + 1, min(tag_line + 12, len(lines))):
            if re.search(rf"\b{re.escape(name)}\s*\(", lines[j]):
                def_line = j
                break
        if def_line is None:
            notes.append(f"skip {name}: no definition under its tag")
            continue
        if re.match(r"\s*static\b", lines[def_line]):
            notes.append(f"skip {name}: static")
            continue
        stack = scopes_at(code, offsets[def_line])
        if stack != [""]:
            notes.append(f"skip {name}: not directly in a file-level anonymous namespace ({stack})")
            continue
        # a declaration ahead of the definition, inside the namespace, would stay internal
        ahead = code[:offsets[tag_line]]
        if re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*;", ahead):
            notes.append(f"skip {name}: declared ahead of its definition")
            continue
        # the comment block above the tag moves with it
        first = tag_line
        while first > 0 and lines[first - 1].lstrip().startswith("//"):
            first -= 1
        end = body_end(code, offsets[def_line])
        if end is None:
            notes.append(f"skip {name}: no body")
            continue
        cuts.append((offsets[first], end, name))
    # apply from the bottom up
    for start, end, name in sorted(cuts, reverse=True):
        text = text[:start] + "}  // namespace\n\n" + text[start:end] + "\n\nnamespace {" + text[end:]
        notes.append(f"exposed {name}")
    # an anonymous namespace left empty by two neighbours: drop it
    text = re.sub(r"\nnamespace \{\s*\}  // namespace\n", "\n", text)
    return text, notes


def why_targets() -> list[int]:
    import translate as T
    t = T.Translator(16)
    import json
    manifest = json.loads((T.GEN / "manifest.json").read_text(encoding="utf-8"))
    out = []
    for k in manifest.get("translated", {}):
        reason = t.why_no_target(int(k, 16))
        if reason and "no public symbol" in reason:
            out.append(int(k, 16))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", nargs="*", default=[])
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    wanted = {int(a.lower().removeprefix("fun_"), 16) for a in args.addr} or set(why_targets())
    by_file = defaultdict(list)
    for t in tagscan.scan():
        if t["addr"] in wanted and t["name"] and not t["file"].replace("\\", "/").startswith("src/gen/"):
            by_file[ROOT / t["file"]].append(t)
    for path, entries in sorted(by_file.items()):
        text, notes = plan(path, entries)
        for n in notes:
            print(f"{path.relative_to(ROOT).as_posix()}: {n}")
        if args.apply and any(n.startswith("exposed") for n in notes):
            path.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
