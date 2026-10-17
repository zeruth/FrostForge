"""The anti-tamper layer's runtime and the guards it inlines into game code, for tools/translate.py.

Besides the return-address check (tools/retcheck.py, stripped by tools/corpus.py), the clean exe
carries a second protection layer: a runtime around one context (PTR_142ddf848) and a state
block (0x142dde840..0x142ddf9c0: a masked call table at 0x142dde840, its key at 0x142ddf9a8),
that decodes its pointers through FUN_1400b1420 (a stub in encrypted code: "bad instruction
data"), traps on purpose into its own exception handler (FUN_1402aa0e0: guard page, single step,
breakpoint; `ud2` stubs like FUN_14021b2d0), times itself from KUSER_SHARED_DATA and rdtsc, and
hashes code pages. The port never builds that context, so none of it can run: translated
faithfully it read the null context (FUN_140222a60), or spun forever in a one-second wait the
decompiler shows as `now < now + 1000` (FUN_14021f090 in the client loop, 2026-10-17).

It comes in two shapes:

- whole functions (`protection`): marked (they call the key stub, touch the state block, or are
  a trap), with no game text (no string, no source tag), calling only other protection; and
  the internals only protection calls. The translator stubs them, as protection, not ported.
- guards inlined into game functions (`guarded`): the function's own statements around them are
  real code (FUN_14021f090's four calls at the end, Player_C.cpp's FUN_141730c70). strip_guards
  removes each guard statement, every statement that reads what a guard computed, and the local
  stores only guards read, and leaves the rest in place.

    python tools/aegis.py [--show ADDR ...]     the sets, and a function stripped
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "build/facts/aegis.json"

KEY = "1400b1420"
BLOCK = (0x142dde840, 0x142ddf9c0)
GLOBAL = re.compile(r"\b(?:DAT|PTR|UNK|_DAT|_UNK|lRam0*|uRam0*|pcRam0*|cRam0*|iRam0*)_?(142dd[0-9a-f]{4})\b")
CALL = re.compile(r"\bFUN_([0-9a-f]{9})\s*\(")
GAME_TEXT = re.compile(r'"[^"]{3,}"|/\* src: ')
# the layer's other marks: the clock it reads straight from KUSER_SHARED_DATA, the time stamp
# counter, its deliberate traps
OTHER_MARKS = re.compile(r"Ram000000007ffe0|\brdtsc\(\)|\bswi\(3\)|\binvalidInstructionException\(\)")


def in_block(text: str) -> bool:
    return any(BLOCK[0] <= int(g, 16) < BLOCK[1] for g in GLOBAL.findall(text))


def compute(bodies: dict[str, str]) -> dict:
    """{"protection": [...], "guarded": [...]}, by hex address, from the decompilations."""
    callees = {a: set(CALL.findall(c)) - {a} for a, c in bodies.items()}
    callers: dict[str, set] = {}
    for a, cs in callees.items():
        for g in cs:
            callers.setdefault(g, set()).add(a)
    game = {a for a, c in bodies.items() if GAME_TEXT.search(c)}
    traps = {a for a, c in bodies.items()
             if "invalidInstructionException()" in c and not callees[a] and len(c) < 700}
    keyed = {a for a, c in bodies.items() if f"FUN_{KEY}(" in c}
    # (by what cannot run without the runtime, as for a guard: a getter that only decodes a masked
    # value calls the key stub too, and is game code)
    prot = {KEY} | traps
    changed = True
    while changed:
        changed = False
        for a, c in bodies.items():
            if a in prot or a in game or not callees[a] <= prot:
                continue
            if _marked(c, prot):
                prot.add(a)
                changed = True
    # internals: called only from protection, calling only protection or leaves
    leaves = {a for a, cs in callees.items() if not cs}
    changed = True
    while changed:
        changed = False
        for a, cs in callees.items():
            if a in prot or a in game or not callers.get(a):
                continue
            if callers[a] <= prot and cs <= prot | leaves:
                prot.add(a)
                changed = True
    guarded = {a for a, c in bodies.items() if a not in prot and _marked(c, prot)}
    return {"protection": sorted(prot), "guarded": sorted(guarded)}


def load(cor: dict | None = None) -> dict:
    """The sets, from the cache when it is newer than the corpus."""
    corpus = ROOT / "build/corpus.jsonl"
    if CACHE.exists() and CACHE.stat().st_mtime >= corpus.stat().st_mtime:
        return json.loads(CACHE.read_text())
    if cor is None:
        from corpus import load_corpus
        cor = load_corpus()
    facts = compute({f"{a:x}": e.get("c", "") for a, e in cor.items()})
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    CACHE.write_text(json.dumps(facts))
    return facts


# ---- the statement tree --------------------------------------------------------------------

LABEL = re.compile(r"^\s*(\w+):\s*$")
CASE = re.compile(r"^\s*(?:case\b.*|default):\s*$")
GOTO = re.compile(r"\bgoto (\w+);")
IDENT = re.compile(r"\b[A-Za-z_]\w*\b")


class Node:
    """A statement: `lines` its own text (a block's head and its closing line), `kids` the
    statements inside a block, one list per arm (`if`/`else`)."""

    def __init__(self, kind: str, lines: list[str]):
        self.kind = kind          # "stmt", "label", "block"
        self.lines = lines
        self.arms: list[list[Node]] = []
        self.heads: list[list[str]] = []   # each arm's opening lines
        self.tail: list[str] = []          # the closing line(s): `}` or `} while (...);`

    def own_text(self) -> str:
        return "\n".join(self.lines + [l for h in self.heads for l in h] + self.tail)

    def all_text(self) -> str:
        out = [self.own_text()]
        for arm in self.arms:
            out += [k.all_text() for k in arm]
        return "\n".join(out)


def _balance(s: str) -> tuple[int, int]:
    s = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', s)
    return s.count("(") - s.count(")"), s.count("{") - s.count("}")


def parse(lines: list[str], i: int = 0, stop_at_close: bool = False) -> tuple[list[Node], int]:
    """Statements from lines[i:], up to the `}` closing the enclosing block when stop_at_close.
    Returns (nodes, index of that closing line or len(lines))."""
    out: list[Node] = []
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if not s:
            i += 1
            continue
        if s.startswith("}"):
            if stop_at_close:
                return out, i
            raise ValueError(f"unbalanced close at {i}")
        if LABEL.match(line) or CASE.match(line):
            out.append(Node("label", [line]))
            i += 1
            continue
        # gather a full head or statement: until parentheses balance and it ends in `;` or `{`
        j, text = i, ""
        while j < len(lines):
            text += lines[j] + "\n"
            p, _b = _balance(text)
            t = lines[j].rstrip()
            if p == 0 and (t.endswith(";") or t.endswith("{")):
                break
            j += 1
        if j >= len(lines):
            raise ValueError(f"statement at {i} does not end")
        head = lines[i:j + 1]
        if not lines[j].rstrip().endswith("{"):
            out.append(Node("stmt", head))
            i = j + 1
            continue
        node = Node("block", [])
        node.heads.append(head)
        kids, close = parse(lines, j + 1, True)
        node.arms.append(kids)
        i = close
        while True:
            if i >= len(lines):
                raise ValueError("block does not close")
            closing = lines[i].strip()
            if re.match(r"^\}\s*else\b.*\{$", closing):
                # `} else {` / `} else if (...) {` on one line
                node.heads.append([lines[i]])
                kids, i = parse(lines, i + 1, True)
                node.arms.append(kids)
                continue
            if closing == "}" and i + 1 < len(lines) and re.match(r"^\s*else\b", lines[i + 1]):
                # Ghidra's layout: `}` then `else {` / `else if (...) {` on the next line
                k, text = i + 1, ""
                while k < len(lines):
                    text += lines[k] + "\n"
                    if _balance(text)[0] == 0 and lines[k].rstrip().endswith("{"):
                        break
                    k += 1
                node.heads.append(lines[i:k + 1])
                kids, i = parse(lines, k + 1, True)
                node.arms.append(kids)
                continue
            # the closing line, and a do-while's condition (`} while (...);`), which can wrap
            k, text = i, ""
            while k < len(lines):
                text += lines[k] + "\n"
                if _balance(text)[0] == 0:
                    break
                k += 1
            node.tail = lines[i:k + 1]
            i = k + 1
            break
        out.append(node)
    return out, i


def emit(nodes: list[Node]) -> list[str]:
    out = []
    for n in nodes:
        if n.kind != "block":
            out += n.lines
            continue
        for head, arm in zip(n.heads, n.arms):
            out += head
            out += emit(arm)
        out += n.tail
    return out


# ---- stripping -----------------------------------------------------------------------------

# What a guard cannot run without, and only that: the context the port never builds (and the
# handler pointer beside it), the clock it waits on, its traps, and a call through a pointer
# decoded from the masked table (a garbage target without the runtime). The rest of the layer runs
# as translated: the state block is static mapped data, so opaque predicates over game code
# (`(DAT_142dde909 >> 0xc) * 2 + 0x7a94add9U ^ DAT_143135170`) read what the reference reads, and
# a game value stored masked (FUN_14170ff20's _DAT_14312ce10) decodes through the same key path
# it was encoded through
LIVE = re.compile(r"\b(?:PTR|DAT|_DAT|pcRam0*|lRam0*)_?(?:142ddf848|142ddf850)\b")
# (not `swi(3)`: game asserts break with int3 too, and the layer's own int3 tests the context;
# and the TEB and PEB read straight: the debugger flag at ProcessEnvironmentBlock + 2, and the
# stack bounds at Self + 8 / + 0x10 that FUN_14021f090 scribbles random bytes between. Game code
# reaches its thread's data through the TLS pointer instead)
WAITS = re.compile(r"Ram000000007ffe0|\binvalidInstructionException\(\)|\bProcessEnvironmentBlock\b|\bSelf\b")
DECODED_CALL = re.compile(r"\(\*\(code \*\)\s*\(")
# (in a guarded function, its __fastfail is the guard's: FUN_14021f090's guard runs a stub it
# builds, which jumps on to the rest of the function and never returns, so Ghidra shows that rest
# after a `swi(0x29)` the reference never reaches. Only here: elsewhere a fastfail is the game's)
FASTFAIL = re.compile(r"\bswi\(0x29\)")
# (and, in a guarded function, a call through a pointer the masked arithmetic computes, the table
# read in an earlier statement: `(*(code *)((uVar22 >> 0x20 ^ ...) << 0x20 | uVar22 & 0xffffffff))`.
# Game code calls through a vtable or a stored pointer, `(**(code **)(*plVar + 0x10))`)
DECODED_POINTER = re.compile(r"\(\*\(code \*\)\s*\(\((?:(?!;).)*?\^(?:(?!;).)*?<< 0x20", re.S)
EXIT = re.compile(r"\b(?:break|return|goto)\b")
LOOP = re.compile(r"\s*(?:while|do|for)\b")
CONTEXT_NULL = re.compile(r"PTR_142ddf848 == \(undefined1 \*\)0x0")
CONTEXT_SET = re.compile(r"PTR_142ddf848 != \(undefined1 \*\)0x0")


def _marked(text: str, prot: set[str]) -> bool:
    # (a call to the key stub alone is no mark: a masked value's getter makes it too)
    return bool(LIVE.search(text)) or bool(WAITS.search(text)) \
        or any(c in prot and c != KEY for c in CALL.findall(text)) \
        or (bool(DECODED_CALL.search(text)) and "142dde840" in text)


def _game(text: str, prot: set[str]) -> bool:
    return bool(re.search(r'"[^"]{3,}"', text)) or any(c not in prot for c in CALL.findall(text))


def _assigned(stmt: str) -> str | None:
    """The local a statement stores to: `x = ...;`, `x[i] = ...;`, `*(T *)x = ...`."""
    # (no store of its own: `if (...) goto x;`, which Ghidra writes without braces, was read as a
    # store to `if`, so its test of a guard's result stayed: FUN_1412ea4a0's debug-port check)
    if re.match(r"\s*(?:if|while|for|switch|return|goto)\b", stmt):
        return None
    m = re.match(r"\s*(?:\*\(\w[\w\s*]*\)\s*\(?)?\(?\s*(\w+)\b[^=;]*?(?<![=!<>])=(?!=)", stmt)
    return m.group(1) if m else None


def strip_guards(text: str, prot: set[str], force: bool = False) -> tuple[str, str]:
    """(code, status): 'none', 'stripped <n>', or 'kept: <why>' (the text unchanged)."""
    # the reference's runtime always has its context, so a test of it has one answer: the null
    # path (FUN_141687ed0 frees and leaves) is not the reference's, and the other is, guards and all
    folded = CONTEXT_NULL.sub("0", CONTEXT_SET.sub("1", text))
    lines = folded.split("\n")
    try:
        open_brace = next(i for i, l in enumerate(lines) if l.strip() == "{")
    except StopIteration:
        return text, "kept: no body"
    close = max(i for i, l in enumerate(lines) if l.strip() == "}")
    decl_end = next((i for i in range(open_brace + 1, close) if not lines[i].strip()), open_brace)
    decls = lines[open_brace + 1:decl_end]
    locals_ = {m.group(1) for l in decls if (m := re.search(r"(\w+)(?:\s*\[[^\]]*\])*\s*;\s*$", l))}
    try:
        tree, _end = parse(lines[decl_end:close])
    except ValueError as e:
        return text, f"kept: {e}"

    # 1. guard statements, and blocks a guard opens with no game work inside
    tainted: set[str] = set()
    removed = 0
    mixed = []

    # A local is tainted when it holds what only the runtime can give: the context copied into it
    # (`puVar5 = PTR_142ddf848;`), or a value computed from a tainted one. Taint flows through
    # right-hand sides only, and any kept store ends it (Ghidra reuses a local for unrelated
    # values: the guard's uVar21 is the game's list cursor later in FUN_1402f4d00)
    def rhs_reads(stmt: str) -> set[str]:
        v = _assigned(stmt)
        if v is None:
            return set(IDENT.findall(stmt))
        rhs = re.split(r"(?<![=!<>])=(?!=)", stmt, maxsplit=1)[1]
        return set(IDENT.findall(rhs))

    def head_stores(text: str) -> set[str]:
        # (the comma stores in a head: `while ((uVar17 = uVar21, ...))`)
        return {m.group(1) for m in re.finditer(r"\b(\w+)\s*(?<![=!<>])=(?!=)", text)} & locals_

    def walk(nodes: list[Node], keeping: bool = True) -> list[Node]:
        nonlocal removed
        keep = []
        for n in nodes:
            if n.kind == "label":
                keep.append(n)
                continue
            own = n.own_text()
            if n.kind == "stmt":
                v = _assigned(own)
                bad = _marked(own, prot) or bool(rhs_reads(own) & tainted) or bool(FASTFAIL.search(own)) \
                    or bool(DECODED_POINTER.search(own))
                if bad and not (_game(own, prot) and not _marked(own, prot)):
                    # (what a marked right-hand side gives is the runtime's too: the context, or a
                    # protection call's result, `bVar8 = FUN_14021ac70(...)` in FUN_14021f090)
                    rhs = re.split(r"(?<![=!<>])=(?!=)", own, maxsplit=1)[-1]
                    if v in locals_ and (_marked(rhs, prot) or rhs_reads(own) & tainted):
                        tainted.add(v)
                    removed += keeping
                    continue
                if keeping:
                    tainted.discard(v)
                elif v in locals_:
                    # (inside a removed block no store happens: what it would have stored is lost)
                    tainted.add(v)
                keep.append(n)
                continue
            # (a block opens a guard by its own marks, or by testing a tainted value; a head that
            # merely names a local a guard also used is game control flow: `if ((lVar15 != 0) &&
            # (iVar8 = *(int *)(lVar15 + 0x14), ...))` in FUN_1402f4d00 reassigning the guard's iVar8)
            head = "\n".join(l for h in n.heads for l in h)
            reads = (set(IDENT.findall(head)) - head_stores(head)) & tainted
            if _marked(own, prot) or reads:
                if not _game(n.all_text(), prot):
                    # (what it stores from the runtime is tainted after it, as if it ran: in its
                    # arms, and in its head, `|| (..., puVar18 = puVar10, ...)` in FUN_140222a60)
                    tainted.update(head_stores(head))
                    for arm in n.arms:
                        walk(arm, False)
                    removed += keeping
                    continue
                # game work inside a guard's block (FUN_1412ee600 samples an integrity check that
                # calls game lookups, then punishes through a decoded call): the block stays and
                # its own guard statements go. Only a head the runtime alone can answer is refused,
                # by the check below that nothing kept reads what only a guard stored
                if _marked(head, prot):
                    mixed.append(own.strip().split("\n")[0][:80])
            if keeping:
                tainted.difference_update(head_stores(head))
            else:
                tainted.update(head_stores(head))
            exits = len(EXIT.findall(n.all_text()))
            n.arms = [walk(arm, keeping) for arm in n.arms]
            if keeping and LOOP.match(head) and len(EXIT.findall(n.all_text())) < exits:
                # a loop that lost an exit to the strip is the guard's (FUN_1412ee600's page-hash
                # walk, `while( true )` left only by `if (uVar12 <= uVar18) break;` on the
                # context's page count): gone with it, unless game work runs inside
                if _game(n.all_text(), prot):
                    mixed.append("a loop whose exit was a guard's: " + head.strip().split("\n")[0][:60])
                else:
                    # (and what it stored is lost, as for any removed block)
                    tainted.update(head_stores(n.all_text()))
                    removed += 1
                    continue
            keep.append(n)
        return keep

    # one pass in program order
    kept = walk(tree)
    if mixed:
        return text, f"kept: a guard around game code ({mixed[0]})"
    if not removed:
        return text, "none"

    # 2. stores to locals that nothing kept reads any more
    while True:
        body = "\n".join(emit(kept))
        dead = False

        def prune(nodes):
            nonlocal dead
            out = []
            for n in nodes:
                if n.kind == "stmt":
                    v = _assigned(n.own_text())
                    if v in locals_ and not _game(n.own_text(), prot) and \
                            len(re.findall(r"\b" + re.escape(v) + r"\b", body)) == \
                            len(re.findall(r"\b" + re.escape(v) + r"\b", n.own_text())):
                        dead = True
                        continue
                if n.kind == "block":
                    n.arms = [prune(a) for a in n.arms]
                out.append(n)
            return out

        kept = prune(kept)
        if not dead:
            break

    # 3. nothing kept may read a local only the removed code stored (it would read garbage)
    out = emit(kept)
    kept_text = "\n".join(out)
    whole = "\n".join(lines[decl_end:close])
    for v in sorted(locals_):
        store = re.compile(r"\b" + re.escape(v) + r"\b(?:\s*\[[^\]]*\])*(?:\.\w+)*\s*(?<![=!<>])=(?!=)|&" + re.escape(v) + r"\b")
        if re.search(r"\b" + re.escape(v) + r"\b", kept_text) and store.search(whole) and not store.search(kept_text):
            if not force:
                return text, f"kept: {v} is read but only the guard stored it"

    # 4. labels: drop those no kept goto names (a kept label still needs a statement after it)
    targets = {m.group(1) for l in out for m in GOTO.finditer(l)}
    final = []
    for k, l in enumerate(out):
        m = LABEL.match(l)
        if m and not CASE.match(l):
            if m.group(1) not in targets:
                continue
            nxt = next((x for x in out[k + 1:] if x.strip()), "}")
            if nxt.strip().startswith("}") or LABEL.match(nxt):
                l = l + " ;"
        final.append(l)
    indent = "  "
    note = f"{indent}/* anti-tamper guard removed (tools/aegis.py) */"
    new = lines[:decl_end] + ["", note] + final + lines[close:]
    from corpus import _drop_unused_locals
    return "\n".join(_drop_unused_locals(new)), f"stripped {removed}"


def main() -> None:
    sys.path.insert(0, str(Path(__file__).parent))
    from corpus import load_corpus
    cor = load_corpus()
    facts = load(cor)
    prot = set(facts["protection"])
    print(f"protection {len(facts['protection'])}, guarded {len(facts['guarded'])}")
    show = [x for x in sys.argv[sys.argv.index("--show") + 1:] if not x.startswith("--")] if "--show" in sys.argv else []
    if not show:
        status = {}
        for a in facts["guarded"]:
            st = strip_guards(cor[int(a, 16)]["c"].replace("\r\n", "\n"), prot)[1]
            status[st.split(" ")[0]] = status.get(st.split(" ")[0], 0) + 1
            if not st.startswith("stripped"):
                print(f"  {a} {st}")
        print(status)
    for a in show:
        # (--force: the stripped text even where the result would be refused, to see why)
        out, st = strip_guards(cor[int(a, 16)]["c"], prot, "--force" in sys.argv)
        print(f"== {a} {st}")
        print(out)


if __name__ == "__main__":
    main()
