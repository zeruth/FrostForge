"""The 3.4.3 decompilation corpus: every function decompiled once, read many times.

Same idea as the 3.3.5 tools/recomp/corpus.py: a headless Ghidra run costs a minute or
more, so the whole program is exported once (ghidra/ExportDecompAll.java, parallel) to
build/corpus.jsonl, and every later read is a file lookup. Names come from the Ghidra
project, which ApplyFacts has already filled (imports, Lua and C_ APIs, data/names.json),
so re-export after a round of naming.

Rendering strips the anti-tamper return-address check (tools/retcheck.py) that the clean
exe still carries in ~3.3k functions. The check sits at the function's return point and
swallows the real return: in Lua::UnitExists the only `return 1;` left is inside the
check's success path. So the stripper cuts from the statement that loads the check's
code-range global to the end of the function and puts back `return <value>;` taken
from that success path -- and only when the cut part is nothing but the check (no
calls, no gotos into it from kept code). Otherwise the function is left whole and
marked. The raw decompilation is always kept; --raw shows it.

    python tools/corpus.py --export                   # decompile everything (~74k functions)
    python tools/corpus.py --addr 141d96a50           # render to stdout
    python tools/corpus.py --module Unit_C.cpp        # functions attributed to a source file
    python tools/corpus.py --range 141780000 141790000 --out unit_c   # -> build/modules/unit_c.c
    python tools/corpus.py --status                   # what the corpus holds; strip statistics
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
FACTS = BUILD / "facts"
CORPUS = BUILD / "corpus.jsonl"
MODULES = BUILD / "modules"

RETCHECK_GLOBALS = ("DAT_14261a0b0", "DAT_14261a0b8")
_CALL = re.compile(r"\b([A-Za-z_][\w:]*)\s*\(")
_NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "swi", "do"}
_RETURN = re.compile(r"^\s*return\b\s*(.*?);\s*$")
_LABEL = re.compile(r"^\s*([A-Za-z_]\w*):\s*$")
_GOTO = re.compile(r"\bgoto\s+([A-Za-z_]\w*)\s*;")


# ---- export --------------------------------------------------------------------------------

def export() -> None:
    ghidra = glob.glob(r"C:\Users\tyler\tools\ghidra_*")
    if not ghidra:
        sys.exit("no Ghidra install under C:\\Users\\tyler\\tools")
    BUILD.mkdir(exist_ok=True)
    tmp = CORPUS.with_suffix(".jsonl.new")
    cmd = [os.path.join(ghidra[0], "support", "analyzeHeadless.bat"),
           r"C:\Users\tyler\tools\ghidra-projects", "FrostLite", "-process", "WowClassic-clean.exe",
           "-noanalysis", "-scriptPath", str(ROOT / "tools" / "ghidra"),
           "-postScript", "ExportDecompAll.java", str(tmp)]
    env = dict(os.environ, GHIDRA_HEADLESS_MAXMEM="14G")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    for line in proc.stdout:
        if "ExportDecompAll" in line or "ERROR" in line:
            print("  ", line.strip())
    proc.wait()
    if not tmp.exists():
        sys.exit("export produced nothing")
    tmp.replace(CORPUS)
    print(f"wrote {CORPUS}")


# ---- load ----------------------------------------------------------------------------------

def load_corpus() -> dict[int, dict]:
    if not CORPUS.exists():
        sys.exit(f"{CORPUS} missing: run --export first")
    out = {}
    with CORPUS.open(encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            if "c" in r:
                r["c"] = r["c"].replace("\r\n", "\n")  # Ghidra on Windows emits CRLF
            out[int(r["addr"], 16)] = r
    return out


def load_facts(name: str) -> dict:
    p = FACTS / f"{name}.json"
    return json.loads(p.read_text()) if p.exists() else {}


# ---- anti-tamper stripping -----------------------------------------------------------------

# the success test: `if (iVar8 - uVar11 < 0x41) {`, or through a temporary,
# `uVar9 = iVar8 - uVar15; if (uVar9 < 0x41) {`
# (or the table entry's base computed in place, `if (iVar2 - (iVar14 + 0xc0000000U) < 0x41) {`:
# Lua::UnitSetRole and seven other handlers kept their checks, "no success block", 2026-10-18)
_SUCCESS = re.compile(r"^\s*if \(\w+(?: - (?:\w+|\(\w+ [+-] \w+\)))? < 0x41\) \{\s*$")
# and as a jump to the code after the check
_SUCCESS_JUMP = re.compile(r"^\s*if \(\w+(?: - (?:\w+|\(\w+ [+-] \w+\)))? < 0x41\) goto (\w+);\s*$")


def _crash_path_end(lines: list[str], end_if: int) -> int:
    """The last line of the check's own crash path right after it (`code_r0x...:` then
    `do { ... } while( true );`), or the check's own end when there is none."""
    j = end_if + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    if j < len(lines) and _LABEL.match(lines[j]) and j + 1 < len(lines) and lines[j + 1].strip() == "do {":
        loop_end = _block_end(lines, j + 1)
        if loop_end is not None and "while( true );" in lines[loop_end]:
            return loop_end
    return end_if
# the range test that opens a check: `if ((DAT_14261a0b0 <= unaff_retaddr) && ...`
_CHECK_OPEN = re.compile(r"^\s*if \(\(DAT_14261a0b0 <= \w+\) &&")


def _block_end(lines: list[str], open_line: int) -> int | None:
    """Index of the line closing the brace block opened on open_line."""
    depth = 0
    opened = False
    for i in range(open_line, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        opened = opened or "{" in lines[i]
        if opened and depth == 0:
            return i
    return None


def _strip_one(lines: list[str], start: int) -> tuple[list[str] | None, str]:
    """Replace the check opening at `start` with its success block. (new lines, why)."""
    end_if = _block_end(lines, start)
    if end_if is None:
        return None, "check block does not close"
    succ = next((i for i in range(start, end_if) if _SUCCESS.match(lines[i])), None)
    if succ is None:
        # the success test as a jump to the code after the check (`if (iVar24 - (iVar33 +
        # 0xc0000000U) < 0x41) goto code_r0x000141c6db8f;`, PostAuction's and PlaceAuctionBid's
        # later return points): the check is that jump
        jump = next((m for i in range(start, end_if) if (m := _SUCCESS_JUMP.match(lines[i]))), None)
        if jump is None:
            return None, "no success block in the check"
        stop = _crash_path_end(lines, end_if)
        removed = lines[start:stop + 1]
        for l in removed:
            for m in _CALL.finditer(l):
                if m.group(1) not in _NOT_CALLS:
                    return None, f"call to {m.group(1)} in the check"
        labels_removed = {m.group(1) for l in removed if (m := _LABEL.match(l))}
        for l in lines[:start] + lines[stop + 1:]:
            m = _GOTO.search(l)
            if m and m.group(1) in labels_removed:
                return None, f"code outside the check jumps to {m.group(1)}"
        indent = re.match(r"\s*", lines[start]).group(0)
        return (lines[:start] + [f"{indent}/* anti-tamper return-address check removed (tools/corpus.py) */",
                                 f"{indent}goto {jump.group(1)};"] + lines[stop + 1:]), ""
    succ_end = _block_end(lines, succ)
    if succ_end is None or succ_end >= end_if:
        return None, "success block does not close"
    # the check's own crash path right after it: `code_r0x...:` then `do { ... } while( true );`
    stop = end_if
    j = end_if + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    if j < len(lines) and (m := _LABEL.match(lines[j])):
        if j + 1 < len(lines) and lines[j + 1].strip() == "do {":
            loop_end = _block_end(lines, j + 1)
            if loop_end is not None and "while( true );" in lines[loop_end]:
                stop = loop_end
    body = lines[succ + 1 : succ_end]
    removed = lines[start : succ + 1] + lines[succ_end : stop + 1]
    for l in removed:
        for m in _CALL.finditer(l):
            if m.group(1) not in _NOT_CALLS:
                return None, f"call to {m.group(1)} outside the success block"
    labels_removed = {m.group(1) for l in removed if (m := _LABEL.match(l))}
    for l in lines[:start] + body + lines[stop + 1 :]:
        m = _GOTO.search(l)
        if m and m.group(1) in labels_removed:
            return None, f"code outside the check jumps to {m.group(1)}"
    indent = re.match(r"\s*", lines[start]).group(0)
    inner = min((len(l) - len(l.lstrip()) for l in body if l.strip()), default=0)
    body = [indent + l[inner:] if l.strip() else l for l in body]
    note = f"{indent}/* anti-tamper return-address check removed (tools/corpus.py) */"
    return lines[:start] + [note] + body + lines[stop + 1 :], ""


_DECL = re.compile(r"^  [A-Za-z_][\w ]*?[\s*]+(\w+)(?: \[\w+\])?;\s*$")


def _drop_unused_locals(lines: list[str]) -> list[str]:
    """Drop local declarations that only the removed check used (bVar2..., unaff_retaddr,
    its stack buffers). Declarations are the indented lines between the function's
    opening brace and the first blank line, Ghidra's layout."""
    try:
        open_brace = lines.index("{")
    except ValueError:
        return lines
    end = next((i for i in range(open_brace + 1, len(lines)) if not lines[i].strip()), None)
    if end is None:
        return lines
    body = "\n".join(lines[end:])
    keep = []
    for l in lines[open_brace + 1 : end]:
        m = _DECL.match(l)
        if m and not re.search(r"\b" + re.escape(m.group(1)) + r"\b", body):
            continue
        keep.append(l)
    return lines[: open_brace + 1] + keep + lines[end:]


def strip_retcheck(c: str) -> tuple[str, str]:
    """(code, status): 'none', 'stripped', 'partial: <n> of <m>: <why>', or 'kept: <why>'.

    The check guards a return point, and there is one per return point (a function
    with an early `return 0` has two). The decompiler sinks the code after that point
    into the check's success branch,
        if ((DAT_14261a0b0 <= unaff_retaddr) && ...) {      <- range test
          ... table walk ... if (<a> - <b> < 0x41) { <the real code, ending in return> }
          do { crash } while( true );
        }
        code_r0x...:  do { crash } while( true );           <- its own crash path
    so each check is replaced by the contents of its success block, and only when
    everything removed is free of calls and nothing outside jumps into it. A check in
    another shape (the inverted `if (out of range) goto crash;` form) is left visible."""
    # (the range base as Ghidra names it where it holds no data item, `UNK_14261a0b0`: the same
    # global; unread, two checks stayed in FUN_140a00f40 and FUN_141000a60, 2026-10-19)
    c = re.sub(r"\b_?UNK_(14261a0b[08])\b", r"DAT_\1", c)
    if not any(g in c for g in RETCHECK_GLOBALS):
        return c, "none"
    lines = c.split("\n")
    total = sum(1 for l in lines if _CHECK_OPEN.match(l))
    done = 0
    why = ""
    pos = 0
    while True:
        start = next((i for i in range(pos, len(lines)) if _CHECK_OPEN.match(lines[i])), None)
        if start is None:
            break
        new, reason = _strip_one(lines, start)
        if new is None:
            why = why or reason
            pos = start + 1
            continue
        lines = new
        done += 1
        pos = start
    if done:
        lines = _drop_unused_locals(lines)
    out = "\n".join(lines)
    left = any(g in out for g in RETCHECK_GLOBALS)
    if not left:
        return out, "stripped"
    if done:
        return out, f"partial: {done} of {total + (0 if total else 1)}: {why or 'a check in another shape'}"
    return c, f"kept: {why or 'check in another shape'}"


# ---- render --------------------------------------------------------------------------------

def render(rec: dict, raw: bool, src: dict, tags: dict) -> str:
    a = int(rec["addr"], 16)
    head = f"// ===== {rec['name']} @ {a:#x} ({rec.get('size', '?')} bytes)"
    notes = []
    if hexs := src.get(f"0x{a:x}"):
        notes.append("src: " + ", ".join(hexs))
    if t := tags.get(f"0x{a:x}"):
        notes.append("types: " + ", ".join(t))
    body = rec.get("c")
    if body is None:
        return f"{head}\n// decompile failed: {rec.get('error', '')}\n"
    if not raw:
        body, status = strip_retcheck(body)
        if status.startswith("kept"):
            notes.append(f"anti-tamper check present ({status})")
    return "\n".join([head] + [f"// {n}" for n in notes]) + "\n" + body.strip("\n") + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--export", action="store_true")
    ap.add_argument("--addr", nargs="+")
    ap.add_argument("--range", nargs=2)
    ap.add_argument("--module", help="source file name or path fragment (srcrefs)")
    ap.add_argument("--out", help="write to build/modules/<out>.c instead of stdout")
    ap.add_argument("--raw", action="store_true", help="do not strip the anti-tamper check")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()

    if args.export:
        export()
        return
    corpus = load_corpus()
    src = load_facts("srcrefs").get("functions", {})
    tags = load_facts("typetags").get("functions", {})

    if args.status:
        ok = sum(1 for r in corpus.values() if "c" in r)
        print(f"{len(corpus)} functions, {ok} decompiled, {len(corpus) - ok} failed")
        rc = load_facts("retcheck").get("functions", {})
        stats: dict[str, int] = {}
        for k in rc:
            r = corpus.get(int(k, 16))
            if r is None or "c" not in r:
                stats["not in corpus / failed"] = stats.get("not in corpus / failed", 0) + 1
                continue
            st = strip_retcheck(r["c"])[1]
            st = st if st in ("stripped", "none") else st.split(":")[0] + ": " + st.rsplit(": ", 1)[-1].split(" to ")[0]
            stats[st] = stats.get(st, 0) + 1
        print(f"anti-tamper check, {len(rc)} functions:")
        for k, v in sorted(stats.items(), key=lambda x: -x[1]):
            print(f"  {v:6}  {k}")
        return

    picked: list[int] = []
    if args.addr:
        picked = [int(x, 16) for x in args.addr]
    elif args.range:
        lo, hi = (int(x, 16) for x in args.range)
        picked = sorted(a for a in corpus if lo <= a < hi)
    elif args.module:
        picked = sorted(int(a, 16) for a, files in src.items() if any(args.module in f for f in files))
    else:
        ap.print_help()
        return

    text = "\n".join(render(corpus[a], args.raw, src, tags) if a in corpus else f"// {a:#x}: not in corpus\n"
                     for a in picked)
    if args.out:
        MODULES.mkdir(parents=True, exist_ok=True)
        p = MODULES / f"{args.out}.c"
        p.write_text(text, encoding="utf-8")
        print(f"{len(picked)} functions -> {p}")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)


if __name__ == "__main__":
    main()
