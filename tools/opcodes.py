"""The 3.4.3.54261 opcode table, read mechanically out of the client: docs/opcodes.json + .md.

The client is the source of truth for which opcodes exist and what their numbers are. Names
are proposals: a hint table (WowPacketParser's 3.4.3 opcode list, imported once with
--import-hints, see below) supplies TrinityCore-style names by number, and every name is then
checked against what the client itself says around that opcode (log strings, Lua events the
handler fires, Lua API names that reach the sender, Jam type names, source files). A name is
"evidence" when one of those client strings shares its distinctive words, "inferred" otherwise.

    python tools/opcodes.py                      # corpus -> docs/opcodes.json, docs/opcodes.md
    python tools/opcodes.py --depth 4            # walk handler / sender trees deeper for evidence
    python tools/opcodes.py --show 0x2583        # one opcode's record, as JSON
    python tools/opcodes.py --import-hints Opcodes.cs   # WPP V3_4_3_51666/Opcodes.cs -> data/opcode_hints.json

Inputs: build/corpus.jsonl (tools/corpus.py), the clean exe (tools/wowbin.py), and the facts
srcrefs, luabind, cregister. No Ghidra, no build. ~1-2 minutes, most of it loading the corpus.

How each side is found (world-handshake.ANALYSIS.md section 4 in the FrostForge server repo):

Server -> client. The JAM protocol objects are registered by static initializers that do
`lea rcx, <object>; jmp REGISTER_PROTOCOL`; every caller of REGISTER_PROTOCOL gives one object,
`{+0 name, +0x08 inRange, +0x28 parse+dispatch}`. The dispatch is a thunk into the protocol's
parse switch. Each top-level `case n` of that switch (opcode = n, or base + n when the switch
is on `param_4 - base`) builds a message (a vtable store, or a ctor+parse call taking the
stream), calls the parser, then the handler through a global (`(*DAT_x)(msg)`; null = ignored).
The connection-level messages 0x3048..0x3050 have their own switch (CONNECTION_SWITCH), called
outside the protocol list. Registrars are every function that assigns one of those globals;
the value assigned is the handler. Handlers that are thunks (`mov rcx,[rcx+8]; jmp T`) are
followed, and a virtual thunk (`mov rax,[rcx]; jmp [rax+N]`) is resolved through the vtable
the registrar stores in its `this` (marked "via_vtable": that resolution is an inference).

Client -> server. Every CMSG writer begins with WRITE_U16(stream, <opcode>) as its first call.
The writer sits in a vtable at slot +0x10 (a qword equal to its address in .rdata); the
functions that reference that vtable are its ctor (small, stores the vtable into `*param_1`)
or senders that build the packet inline. The ctor's callers are the senders. Evidence comes
from the senders and their callers up to --depth: Lua C functions (luabind / C_ APIs), strings.

Layouts: the stream primitives below are named in each parser's / writer's call sequence
("reads" / "writes"), which is a cheap first look at a body, not a full layout.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

from corpus import load_corpus, load_facts, strip_retcheck
from wowbin import ROOT, Binary

REGISTER_PROTOCOL = 0x14073D7D0   # static init: lea rcx, obj; jmp here
CONNECTION_SWITCH = 0x140726B80   # 0x3048..0x3050, called from FUN_141235030
WRITE_U16 = 0x1402CA3D0

READS = {
    0x1402C9990: "u8", 0x1402C99E0: "u16", 0x1402C9A30: "u32", 0x1402C9A80: "u64",
    0x1402C9F90: "bytes(rest|n)", 0x1402C9FE0: "bytes_copy(n)", 0x141550B20: "packed_guid",
}
WRITES = {
    0x1402CA320: "u8", 0x1402CA3D0: "u16", 0x1402CA480: "u32", 0x1402CA690: "u64",
    0x141550BF0: "packed_guid", 0x1402CA530: "f32", 0x1402CA740: "bytes(n)", 0x14074C3E0: "MovementInfo",
    0x140758AE0: "bits(n)", 0x140758C60: "bits(n)", 0x1406F5D20: "bits_flush",
}

DOCS = ROOT / "docs"
HINTS = ROOT / "data" / "opcode_hints.json"

_CALL = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9,})\s*\(|\b([A-Za-z_]\w*(?:::\w+)*)\s*\(")
_STRLIT = re.compile(r'(?<![\w])(?:u|L)?"((?:[^"\\\n]|\\.)*)"')
_SLABEL = re.compile(r"\b([su])_\w*?_(14[0-9a-f]{7})\b")
_HEX64 = re.compile(r"\b0x([0-9a-f]{11,16})\b")
_GLOBAL_CALL = re.compile(r"\(\*_?(?:DAT|PTR)_([0-9a-f]{9})\)\s*\(")
_GLOBAL_TEST = re.compile(r"\b_?(?:DAT|PTR)_([0-9a-f]{9}) != \(code \*\)0x0"
                          r"|\bpcVar\d+ = (?:\(code \*\))?_?(?:DAT|PTR)_([0-9a-f]{9});")
# a static delegate: `(**(code **)(DAT_x + 8))(&DAT_x, msg)` -- the handler is an object
# installed into DAT_x by a module's setup function
_DELEGATE = re.compile(r"\(\*\*\(code \*\*\)\(_?DAT_([0-9a-f]{9}) \+ 8\)\)\(&_?DAT_\1")
_VTSTORE = re.compile(r"=\s*(?:\([^)]*\))?\s*(?:vtable_|PTR_\w*?_|0x)(14[0-9a-f]{7})\s*;")
_ASSIGN = re.compile(r"\b_?(?:DAT|PTR)_([0-9a-f]{9}) = ([^;]+);")
_NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "do", "CONCAT11", "CONCAT31",
              "CONCAT44", "CONCAT71", "CONCAT22", "SUB81", "SUB84", "ZEXT48", "SEXT48"}


# ---- corpus helpers ------------------------------------------------------------------------------

class Corpus:
    def __init__(self) -> None:
        self.f = load_corpus()
        self.by_name: dict[str, int] = {}
        for a, r in self.f.items():
            self.by_name.setdefault(r["name"], a)
            self.by_name.setdefault(r["name"].split("::")[-1], a)
        self.callees: dict[int, list[int]] = {}
        self.callers: dict[int, set[int]] = defaultdict(set)
        for a, r in self.f.items():
            out = [int(k, 16) for k, v in (r.get("g") or {}).items() if any(x & 8 for x in v)]
            out = [t for t in out if t in self.f and t != a]
            self.callees[a] = out
            for t in out:
                self.callers[t].add(a)
        self._body: dict[int, str] = {}

    def text(self, a: int) -> str:
        if a not in self._body:
            r = self.f.get(a)
            self._body[a] = strip_retcheck(r["c"])[0] if r and r.get("c") else ""
        return self._body[a]

    def name(self, a: int) -> str:
        r = self.f.get(a)
        return r["name"] if r else f"FUN_{a:x}"

    def resolve(self, token: str) -> int | None:
        m = re.fullmatch(r"(?:FUN|thunk_FUN)_([0-9a-f]{9,})", token)
        if m:
            return int(m.group(1), 16)
        return self.by_name.get(token)

    def calls_in(self, text: str) -> list[tuple[int | None, str, int]]:
        """(callee or None, token, offset) for every call in text, in order."""
        out = []
        for m in _CALL.finditer(text):
            if m.group(1):
                out.append((int(m.group(1), 16), "FUN_" + m.group(1), m.start()))
            else:
                tok = m.group(2)
                if tok in _NOT_CALLS:
                    continue
                out.append((self.by_name.get(tok), tok, m.start()))
        return out


def fbody(text: str) -> str:
    """A decompiled function without its signature (which would read as a call)."""
    return text.split("{", 1)[1] if "{" in text else text


def call_args(text: str, at: int) -> str:
    """The argument text of the call whose name starts at `at`."""
    i = text.index("(", at)
    depth, j = 0, i
    while j < len(text):
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[i + 1 : j]
        j += 1
    return text[i + 1 :]


# ---- strings and event names ---------------------------------------------------------------------

def murmur3_x64_128_low(data: bytes, seed: int = 0) -> int:
    M = 0xFFFFFFFFFFFFFFFF
    c1, c2 = 0x87C37B91114253D5, 0x4CF5AD432745937F

    def rotl(x, r):
        return ((x << r) | (x >> (64 - r))) & M

    def fmix(k):
        k ^= k >> 33
        k = (k * 0xFF51AFD7ED558CCD) & M
        k ^= k >> 33
        k = (k * 0xC4CEB9FE1A85EC53) & M
        k ^= k >> 33
        return k

    h1 = h2 = seed
    n = len(data)
    nblocks = n // 16
    for i in range(nblocks):
        k1, k2 = struct.unpack_from("<QQ", data, i * 16)
        k1 = (k1 * c1) & M; k1 = rotl(k1, 31); k1 = (k1 * c2) & M; h1 ^= k1
        h1 = rotl(h1, 27); h1 = (h1 + h2) & M; h1 = (h1 * 5 + 0x52DCE729) & M
        k2 = (k2 * c2) & M; k2 = rotl(k2, 33); k2 = (k2 * c1) & M; h2 ^= k2
        h2 = rotl(h2, 31); h2 = (h2 + h1) & M; h2 = (h2 * 5 + 0x38495AB5) & M
    tail = data[nblocks * 16 :]
    k1 = k2 = 0
    for i in range(len(tail) - 1, 7, -1):
        k2 ^= tail[i] << ((i - 8) * 8)
    if len(tail) > 8:
        k2 = (k2 * c2) & M; k2 = rotl(k2, 33); k2 = (k2 * c1) & M; h2 ^= k2
    for i in range(min(len(tail), 8) - 1, -1, -1):
        k1 ^= tail[i] << (i * 8)
    if tail:
        k1 = (k1 * c1) & M; k1 = rotl(k1, 31); k1 = (k1 * c2) & M; h1 ^= k1
    h1 ^= n; h2 ^= n
    h1 = (h1 + h2) & M; h2 = (h2 + h1) & M
    h1 = fmix(h1); h2 = fmix(h2)
    h1 = (h1 + h2) & M
    return h1


def event_names(b: Binary) -> dict[int, str]:
    """Murmur3 id -> name for every upper-case identifier string in the image (event names)."""
    out = {}
    for sec in (".rdata", ".data"):
        try:
            _, data = b.bytes_of(sec)
        except StopIteration:
            continue
        for m in re.finditer(rb"(?<=\x00)([A-Z][A-Z0-9_]{2,80})\x00", data):
            s = m.group(1)
            out.setdefault(murmur3_x64_128_low(s), s.decode())
    return out


class Evidence:
    def __init__(self, b: Binary, cor: Corpus) -> None:
        self.b, self.cor = b, cor
        self.events = event_names(b)
        self.src = load_facts("srcrefs").get("functions", {})
        self.lua = {int(k, 16): v for k, v in load_facts("luabind").get("functions", {}).items()}
        self.lua.update({int(k, 16): v for k, v in load_facts("cregister").get("functions", {}).items()})
        self.typetags = load_facts("typetags").get("functions", {})
        self._cache: dict[int, dict] = {}

    def of(self, a: int) -> dict:
        """strings, Jam/RTTI types, event names and source files one function references."""
        if a in self._cache:
            return self._cache[a]
        text = self.cor.text(a)
        strings, files = [], list(self.src.get(f"0x{a:x}", []))
        types = [t.split("::")[-1] for t in self.typetags.get(f"0x{a:x}", []) if "<" not in t][:8]

        def add(s: str) -> None:
            if not s or len(s) < 3 or ((s[0] == "." or "?$" in s or "@@" in s) and not s.startswith((".?AU", ".?AV"))):
                return
            if s.startswith((".?AU", ".?AV")):
                t = s[4:].split("@@")[0].split("@")[0]
                if t not in types:
                    types.append(t)
            elif re.search(r"[\\/]\w+\.(cpp|h|hpp|c)$", s):
                f = s.replace("\\\\", "/").replace("\\", "/")
                f = f.split("/Source/", 1)[-1] if "/Source/" in f else f
                if f not in files:
                    files.append(f)
            elif s not in strings:
                strings.append(s)

        for m in _STRLIT.finditer(text):
            add(m.group(1))
        for kind, hexa in _SLABEL.findall(text):
            va = int(hexa, 16)
            if kind == "s":
                add(self.b.cstr(va) or "")
            else:
                try:
                    o = self.b.off(va)
                    raw = bytes(self.b.image[o : o + 512])
                    end = raw.find(b"\0\0")
                    add(raw[: end + (end & 1)].decode("utf-16-le", "replace"))
                except Exception:
                    pass
        events = []
        for h in _HEX64.findall(text):
            e = self.events.get(int(h, 16))
            if e and e not in events:
                events.append(e)
        rec = {"strings": strings, "types": types, "events": events, "files": files}
        if a in self.lua:
            rec["lua"] = [self.lua[a]]
        self._cache[a] = rec
        return rec

    def tree(self, roots: list[int], depth: int, up: bool = False, max_nodes: int = 120,
             fan_limit: int = 40) -> dict:
        """Merge `of` over a BFS from roots: callees (down) or callers (up). Functions reached
        that have more than fan_limit callers (down) / callees (up) are generic and not expanded
        or collected. Each item records the depth where it was first seen."""
        seen, frontier = set(), [(r, 0) for r in roots if r]
        merged = {k: [] for k in ("strings", "types", "events", "files", "lua")}
        order = []
        while frontier and len(order) < max_nodes:
            a, d = frontier.pop(0)
            if a in seen:
                continue
            seen.add(a)
            if d > 0:
                fan = len(self.cor.callers.get(a, ())) if not up else len(self.cor.callees.get(a, ()))
                if fan > fan_limit:
                    continue
            order.append(a)
            rec = self.of(a)
            for k in merged:
                for v in rec.get(k, []):
                    if v not in [x[0] for x in merged[k]]:
                        merged[k].append((v, d, f"{a:x}"))
            if d < depth:
                nxt = sorted(self.cor.callers.get(a, ())) if up else self.cor.callees.get(a, [])
                frontier.extend((n, d + 1) for n in nxt)
        out = {k: [{"value": v, "depth": d, "in": f} for v, d, f in vals][:25]
               for k, vals in merged.items() if vals}
        out["walked"] = len(order)
        return out


# ---- server -> client ----------------------------------------------------------------------------

def protocols(b: Binary, cor: Corpus) -> list[dict]:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out = []
    for site, _kind in b.callers(REGISTER_PROTOCOL):
        lea = site - 7
        o = b.off(lea)
        ins = next(md.disasm(bytes(b.image[o : o + 7]), lea, 1), None)
        if ins is None or ins.mnemonic != "lea" or "rip" not in ins.op_str:
            print(f"  no `lea rcx, obj` before {site:#x}", file=sys.stderr)
            continue
        obj = lea + 7 + ins.operands[1].mem.disp
        name = b.cstr(b.u64(obj)) or f"proto_{obj:x}"
        dispatch = b.u64(obj + 0x28)
        sw = dispatch if "switch(" in cor.text(dispatch) else next(
            (c for c in cor.callees.get(dispatch, []) if "switch(" in cor.text(c)), None)
        out.append({"protocol": name, "object": f"{obj:#x}", "dispatch": f"{dispatch:#x}",
                    "switch": sw, "in_range": f"{b.u64(obj + 8):#x}"})
    out.append({"protocol": "Connection", "object": None, "dispatch": None,
                "switch": CONNECTION_SWITCH, "in_range": None})
    return out


def switch_cases(text: str) -> tuple[int, list[tuple[list[int], str]]]:
    """(base, [(case values, body text)]) for the switch on the opcode parameter (param_4)."""
    lines = text.split("\n")
    best = None
    for i, ln in enumerate(lines):
        m = re.match(r"^(\s*)switch\((.*)\) \{\s*$", ln)
        if m and "param_4" in m.group(2):
            best = (i, len(m.group(1)), m.group(2))
            break
    if best is None:
        return 0, []
    i0, ind, expr = best
    bm = re.search(r"-\s*(0x[0-9a-f]+|\d+)\b", expr)
    base = int(bm.group(1), 0) if bm else 0
    lab = re.compile(r"^" + " " * ind + r"(?:case (0x[0-9a-f]+|\d+)|default):\s*$")
    end = re.compile(r"^" + " " * ind + r"\}\s*$")
    cases, pending, body = [], [], []
    for ln in lines[i0 + 1 :]:
        if end.match(ln):
            break
        m = lab.match(ln)
        if m:
            if body and pending:
                cases.append((pending, "\n".join(body)))
                pending, body = [], []
            elif body:
                body = []
            if m.group(1) is not None:
                pending.append(int(m.group(1), 0))
            else:
                pending.append(None)
            continue
        body.append(ln)
    if pending:
        cases.append((pending, "\n".join(body)))
    cases = [([v for v in vals if v is not None], bd) for vals, bd in cases]
    return base, [c for c in cases if c[0]]


def follow_label(fulltext: str, body: str) -> str:
    m = re.search(r"\bgoto (\w+);", body)
    if not m:
        return ""
    lines = fulltext.split("\n")
    for i, ln in enumerate(lines):
        if re.match(r"^\s*" + re.escape(m.group(1)) + r":\s*$", ln):
            seg = []
            for x in lines[i + 1 : i + 60]:
                seg.append(x)
                if re.match(r"^\s*(return\b|break;)", x):
                    break
            return "\n".join(seg)
    return ""


def stream_aliases(text: str) -> set[str]:
    al = {"param_5"}
    for m in re.finditer(r"\b(\w+) = (?:\([^)]*\))?param_5;", text):
        al.add(m.group(1))
    return al


def seq(cor: Corpus, text: str, table: dict[int, str], stream: set[str], limit: int = 48) -> list[str]:
    """The stream primitives (and other calls taking the stream) in call order."""
    out = []
    for a, tok, at in cor.calls_in(text):
        args = call_args(text, at)
        if a in table:
            out.append(table[a])
        elif not tok.startswith("FUN_") or a is None:
            continue
        elif any(re.search(r"\b" + s + r"\b", args) for s in stream):
            out.append(f"{tok}(stream)")
        else:
            continue
        if len(out) >= limit:
            out.append("...")
            break
    return out


def analyse_case(cor: Corpus, fulltext: str, body: str, stream: set[str]) -> dict:
    rec: dict = {}
    vt = _VTSTORE.search(body)
    if vt:
        rec["message_vtable"] = f"0x{vt.group(1)}"
    def globals_in(text: str) -> list[str]:
        out = []
        for m in list(_GLOBAL_TEST.finditer(text)) + list(_GLOBAL_CALL.finditer(text)):
            g = f"0x{m.group(1) or m.group(2)}" if m.re is _GLOBAL_TEST else f"0x{m.group(1)}"
            if g not in out:
                out.append(g)
        return out

    globals_ = globals_in(body)
    if not globals_:
        globals_ = globals_in(follow_label(fulltext, body))
    rec["handler_globals"] = globals_
    parser, pstream, direct = None, None, []
    for a, tok, at in cor.calls_in(body):
        if a in READS:
            continue
        args = call_args(body, at)
        argv = [x.strip() for x in args.split(",")]
        hit = [i for i, x in enumerate(argv) if any(re.search(r"\b" + s + r"\b", x) for s in stream)]
        if hit and parser is None:
            parser, pstream = (a if a is not None else tok), f"param_{hit[0] + 1}"
        elif a is not None and tok.startswith("FUN_"):
            direct.append(a)
    rec["parser"] = "inline" if parser is None else (f"{parser:#x}" if isinstance(parser, int) else parser)
    reads = seq(cor, body, READS, stream)
    if reads:
        rec["reads_inline"] = reads
    if isinstance(parser, int):
        rec["parser_reads"] = seq(cor, fbody(cor.text(parser)), READS, {pstream})
    rec["calls"] = [f"{a:#x}" for a in direct[:12]]
    delegates = []
    for a in direct:
        m = _DELEGATE.search(cor.text(a))
        if m and len(cor.callers.get(a, ())) <= 3:
            delegates.append({"dispatcher": f"{a:#x}", "global": f"0x{m.group(1)}"})
    if delegates and not globals_:
        rec["delegates"] = delegates
    rec["_case_funcs"] = ([parser] if isinstance(parser, int) else []) + direct
    return rec


def registrars(cor: Corpus, wanted: set[str]) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = defaultdict(list)
    want = {w[2:] for w in wanted}
    for a in cor.f:
        text = cor.text(a)
        if "DAT_14" not in text and "PTR_" not in text:
            continue
        for m in _ASSIGN.finditer(text):
            if m.group(1) not in want:
                continue
            val = m.group(2).strip()
            val = re.sub(r"^\([^)]*\)\s*", "", val)
            entry = {"registrar": f"{a:#x}", "value": val}
            if val in ("0", "0x0") or re.fullmatch(r"0x0+", val):
                entry["clears"] = True
            else:
                t = cor.resolve(val)
                if t is not None:
                    entry["handler"] = f"{t:#x}"
                vt = re.search(r"\*param_1 = (?:vtable_|0x)(14[0-9a-f]{7});", text)
                if vt:
                    entry["this_vtable"] = f"0x{vt.group(1)}"
                    derived = derived_vtables(cor, a)
                    if derived:
                        entry["derived_vtables"] = [f"{v:#x}" for v in derived]
            out[f"0x{m.group(1)}"].append(entry)
    return out


def derived_vtables(cor: Corpus, ctor: int, depth: int = 3) -> list[int]:
    """Vtables stored over `this` by ctors that call `ctor` on their own `this` first (the
    derived classes), most derived first. A virtual handler slot resolves through these: the
    registrar runs in the base ctor, but the object that receives the message is derived."""
    found = []
    frontier = [ctor]
    for _ in range(depth):
        nxt = []
        for c in frontier:
            pat = re.compile(re.escape(cor.name(c)) + r"\(param_1\);\s*\n\s*\*param_1 = (?:vtable_|0x)(14[0-9a-f]{7});")
            for caller in sorted(cor.callers.get(c, ())):
                m = pat.search(cor.text(caller))
                if m:
                    found.append(int(m.group(1), 16))
                    nxt.append(caller)
        frontier = nxt
    return found[::-1]


def follow_thunks(b: Binary, a: int, this_vtable: int | None) -> list[dict]:
    """Thunk hops from a handler: plain jmp after this-adjusting moves, or a virtual slot."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    chain, cur = [], a
    for _ in range(4):
        o = b.off(cur)
        ins = list(md.disasm(bytes(b.image[o : o + 48]), cur, 8))
        hop = None
        for k, i in enumerate(ins):
            if i.mnemonic == "jmp":
                if i.op_str.startswith("0x"):
                    hop = {"jmp": f"{int(i.op_str, 16):#x}"}
                    nxt = int(i.op_str, 16)
                else:
                    # jmp [reg + slot] where reg was loaded from the object's first qword
                    m = re.fullmatch(r"qword ptr \[(\w+) \+ (0x[0-9a-f]+)\]", i.op_str)
                    if m and any(re.fullmatch(m.group(1) + r", qword ptr \[r\w+\]", x.op_str) for x in ins[:k]):
                        slot = int(m.group(2), 16)
                        hop = {"virtual_slot": f"{slot:#x}"}
                        if this_vtable:
                            t = b.u64(this_vtable + slot)
                            hop.update({"via_vtable": f"{this_vtable:#x}", "target": f"{t:#x}"})
                            nxt = t
                        else:
                            nxt = None
                break
            if i.mnemonic not in ("mov", "lea", "movzx", "movsx", "movsxd") or not i.op_str.startswith(("rcx", "rdx", "edx", "r8", "r9", "rax", "eax")):
                break
        if hop is None:
            break
        chain.append(hop)
        if nxt is None:
            break
        cur = nxt
    return chain


def server_to_client(b: Binary, cor: Corpus, ev: Evidence, depth: int) -> list[dict]:
    rows = []
    for p in protocols(b, cor):
        sw = p["switch"]
        if sw is None:
            print(f"  {p['protocol']}: no switch found", file=sys.stderr)
            continue
        text = cor.text(sw)
        base, cases = switch_cases(text)
        stream = stream_aliases(text)
        print(f"  {p['protocol']:18} switch {sw:#x} base {base:#x}: {sum(len(v) for v, _ in cases)} cases",
              file=sys.stderr)
        for vals, body in cases:
            info = analyse_case(cor, text, body, stream)
            for v in vals:
                rows.append({"opcode": base + v, "direction": "SMSG", "protocol": p["protocol"],
                             "switch": f"{sw:#x}", **info})
    wanted = {g for r in rows for g in r["handler_globals"]}
    regs = registrars(cor, wanted)
    # delegate globals: every function naming the global other than its dispatcher sets it up
    dglobals = {d["global"][2:] for r in rows for d in r.get("delegates", [])}
    dref = defaultdict(list)
    if dglobals:
        pat = re.compile(r"\bDAT_(" + "|".join(sorted(dglobals)) + r")\b")
        for a in cor.f:
            for g in set(pat.findall(cor.text(a))):
                dref[g].append(a)
    for r in rows:
        funcs = r.pop("_case_funcs")
        types = []
        for f in funcs[:4]:
            for t in ev.of(f)["types"]:
                if t not in types:
                    types.append(t)
        if types:
            r["message_types"] = types
        for d in r.get("delegates", []):
            setters = [a for a in dref.get(d["global"][2:], []) if f"{a:#x}" != d["dispatcher"]
                       and not _DELEGATE.search(cor.text(a))]
            d["setup"] = [f"{a:#x}" for a in setters[:6]]
            files = []
            for a in setters:
                for f in ev.of(a)["files"]:
                    if f not in files:
                        files.append(f)
            if files:
                d["setup_files"] = files[:4]
        handlers = []
        for g in r["handler_globals"]:
            for e in regs.get(g, []):
                if e.get("clears"):
                    continue
                h = dict(e)
                if "handler" in h:
                    vts = [int(v, 16) for v in h.get("derived_vtables", [])]
                    if "this_vtable" in h:
                        vts.append(int(h["this_vtable"], 16))
                    chain = follow_thunks(b, int(h["handler"], 16), vts[0] if vts else None)
                    if chain:
                        h["thunk"] = chain
                handlers.append(h)
        r["handlers"] = handlers
        r["unregistered_by"] = sorted({e["registrar"] for g in r["handler_globals"]
                                       for e in regs.get(g, []) if e.get("clears")})
        roots = []
        for h in handlers:
            if "handler" in h:
                roots.append(int(h["handler"], 16))
            for hop in h.get("thunk", []):
                t = hop.get("target") or hop.get("jmp")
                if t:
                    roots.append(int(t, 16))
        r["evidence"] = ev.tree(roots, depth) if roots else {}
        if isinstance(r.get("parser"), str) and r["parser"].startswith("0x"):
            pe = ev.tree([int(r["parser"], 16)], 1)
            if pe.get("types") or pe.get("strings"):
                r["parser_evidence"] = {k: pe[k] for k in ("types", "strings", "files") if k in pe}
    rows.sort(key=lambda r: r["opcode"])
    return rows


# ---- client -> server ----------------------------------------------------------------------------

def client_to_server(b: Binary, cor: Corpus, ev: Evidence, depth: int) -> list[dict]:
    first = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{9,})\s*\(|\b([A-Za-z_]\w*(?:::\w+)*)\s*\(")
    writers = {}
    for a in cor.f:
        text = cor.text(a)
        if "FUN_1402ca3d0(" not in text:
            continue
        body = text.split("{", 1)[1] if "{" in text else text
        calls = cor.calls_in(body)
        if not calls or calls[0][0] != WRITE_U16:
            continue
        args = call_args(body, calls[0][2]).split(",")
        if len(args) != 2:
            continue
        try:
            op = int(args[1].strip(), 0)
        except ValueError:
            continue
        writers[a] = (op, args[0].strip())
    # vtable slots: qwords in the data sections equal to a writer's address
    slots = defaultdict(list)
    import numpy as np
    for sec in b.sections:
        if sec.exec:
            continue
        start, data = sec.start, bytes(b.image[b.off(sec.start) : b.off(sec.end)])
        n = len(data) // 8
        arr = np.frombuffer(data[: n * 8], dtype="<u8")
        for idx in np.nonzero(np.isin(arr, np.array(list(writers), dtype=np.uint64)))[0].tolist():
            slots[int(arr[idx])].append(start + idx * 8)
    vt_of = {}
    for w, places in slots.items():
        vt_of[w] = [p - 0x10 for p in places]
    allvt = {v for vs in vt_of.values() for v in vs}
    # functions referencing each vtable (text: vtable_<hex> or 0x<hex>)
    refs = defaultdict(list)
    pat = re.compile(r"\b(?:vtable_|PTR_\w*?_|0x)(14[0-9a-f]{7})\b")
    for a in cor.f:
        text = cor.text(a)
        for h in set(pat.findall(text)):
            v = int(h, 16)
            if v in allvt:
                refs[v].append(a)
    rows = []
    for w, (op, stream) in writers.items():
        wtext = cor.text(w)
        vts = vt_of.get(w, [])
        ctors, senders = [], set()
        for v in vts:
            for f in refs.get(v, []):
                if f == w:
                    continue
                size = cor.f[f].get("size", 0)
                small = size <= 0x60 and re.search(r"\*param_1 = (?:\([^)]*\))?\s*(?:vtable_|0x)", cor.text(f))
                if small:
                    ctors.append(f)
                    senders.update(cor.callers.get(f, ()))
                else:
                    senders.add(f)
        senders.discard(w)
        rec = {
            "opcode": op, "direction": "CMSG", "writer": f"{w:#x}",
            "vtables": [f"{v:#x}" for v in vts],
            "ctors": [f"{c:#x}" for c in sorted(set(ctors))],
            "senders": [f"{s:#x}" for s in sorted(senders)][:40],
            "sender_count": len(senders),
            "writes": seq(cor, fbody(wtext), WRITES, {stream})[1:],  # [0] is the opcode itself
            "empty_body": len(cor.calls_in(fbody(wtext))) == 1,
        }
        roots = sorted(senders) or sorted(ctors)
        rec["evidence"] = ev.tree(roots, depth, up=True) if roots else {}
        rows.append(rec)
    rows.sort(key=lambda r: (r["opcode"], r["writer"]))
    return rows


# ---- names ---------------------------------------------------------------------------------------

STOP = {"smsg", "cmsg", "msg", "message", "update", "updates", "result", "results", "response",
        "request", "requests", "info", "data", "list", "get", "set", "send", "query", "status",
        "the", "a", "of", "to", "on", "for", "and", "or", "by", "handle", "client", "server",
        "packet", "jam", "cli", "svr", "notify", "notification", "is", "has", "with", "from",
        "in", "at", "do", "c", "s", "id", "all", "new", "add", "remove", "change", "changed",
        "start", "stop", "open", "close", "use", "player", "unit", "state", "show",
        "event", "frame", "ui", "lua", "script", "cpp", "source", "game", "world", "wow", "classic",
        "build", "server", "src", "error", "failed", "success", "type", "value", "count", "flag",
        "flags", "guid", "u", "d", "x", "s", "lu", "llu", "i", "n", "one", "two", "it"}
SYN = {"char": "character", "chars": "character", "characters": "character", "enum": "enum",
       "msgs": "message", "lfg": "lfg", "pvp": "pvp", "bg": "battleground", "battlefield": "battlefield",
       "inv": "inventory", "inventories": "inventory", "auctions": "auction", "achievements": "achievement",
       "spells": "spell", "items": "item", "quests": "quest", "mails": "mail", "talents": "talent",
       "friends": "friend", "contacts": "contact", "guilds": "guild", "creatures": "creature",
       "gameobject": "gameobject", "go": "gameobject", "petition": "petition", "trainer": "trainer",
       "vendor": "vendor", "loot": "loot", "logout": "logout", "login": "login", "logon": "login",
       "tutorials": "tutorial", "tickets": "ticket", "gm": "gm", "arena": "arena", "glyphs": "glyph",
       "cooldowns": "cooldown", "auras": "aura", "titles": "title", "channels": "channel",
       "battlegrounds": "battleground", "calendar": "calendar", "rolls": "roll", "chats": "chat"}


def words(s: str) -> list[str]:
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", s)
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", s)
    out = []
    for w in re.split(r"[^A-Za-z0-9]+", s):
        w = w.lower()
        if not w or w.isdigit():
            continue
        w = SYN.get(w, w)
        if len(w) > 4 and w.endswith("s") and not w.endswith("ss"):
            w = SYN.get(w[:-1], w[:-1])
        out.append(stem(w))
    return out


def stem(w: str) -> str:
    """A crude suffix strip so DECLINED meets Decline and RENAMED meets Rename."""
    for suf in ("ing", "ed", "e"):
        if len(w) > len(suf) + 3 and w.endswith(suf):
            return w[: -len(suf)]
    return w


# subsystem nouns: one of these alone says which system, not which message
WEAK = {stem(w) for w in (
    "item", "spell", "guild", "quest", "pet", "party", "group", "aura", "character", "account",
    "battle", "world", "move", "movement", "map", "zone", "area", "time", "name", "player", "unit",
    "creature", "target", "inventory", "bag", "money", "level", "talent", "combat", "log", "cast",
    "trade", "social", "chat", "battlenet", "bnet", "lfg", "raid", "instance", "loot", "mail",
    "auction", "vehicle", "spline", "garrison", "pvp", "arena", "ticket", "gm", "voice", "club",
    "community", "calendar", "achievement", "criteria", "currency", "token", "store", "shop")}


def distinctive(name: str) -> list[str]:
    stop = {stem(w) for w in STOP}
    return [w for w in dict.fromkeys(words(name)) if w not in stop and len(w) > 1]


def evidence_items(rec: dict) -> list[tuple[str, str, int]]:
    """(kind, text, depth) for every client string that can support a name."""
    out = []
    e = rec.get("evidence", {})
    for k in ("lua", "events", "strings", "types", "files"):
        for it in e.get(k, []):
            out.append((k, it["value"], it["depth"]))
    for k in ("types", "strings", "files"):
        for it in rec.get("parser_evidence", {}).get(k, []):
            out.append(("parser_" + k, it["value"], it["depth"]))
    for t in rec.get("message_types", []):
        out.append(("message_type", t, 0))
    for d in rec.get("delegates", []):
        for f in d.get("setup_files", []):
            out.append(("delegate_files", f, 1))
    return out


def judge(name: str, rec: dict) -> tuple[str, list[str]]:
    """evidence when one client string shares two of the name's distinctive words, or all of
    them, or one that is not a mere subsystem noun (WEAK) within two calls of the message; or
    when the strings within one call of the message cover all of them between them (AccountData.cpp
    + "Received Server Save Times" for ACCOUNT_DATA_TIMES). A name made only of stop words
    (UPDATE_OBJECT) needs all of its words in one string within one call."""
    want = distinctive(name)
    only_stop = not want
    if only_stop:
        want = [w for w in dict.fromkeys(words(name)) if w not in ("smsg", "cmsg")]
    support, covered = [], {}
    for kind, text, depth in evidence_items(rec):
        if kind.endswith("files"):
            text = text.rsplit("/", 1)[-1]
        have = set(words(text))
        hit = [w for w in want if w in have]
        strong = [w for w in hit if w not in WEAK]
        if only_stop:
            ok = depth <= 1 and len(hit) == len(want)
        else:
            ok = len(hit) >= 2 or (depth <= 2 and (strong or (hit and len(hit) == len(want))))
            if depth <= 1:
                for w in hit:
                    covered.setdefault(w, f"{kind}@{depth}: {text[:60]}")
        if ok:
            support.append(f"{kind}@{depth}: {text[:90]} [{'+'.join(hit)}]")
    if not support and not only_stop and len(want) >= 2 and len(covered) == len(want):
        support = [f"{v} [{w}]" for w, v in covered.items()]
    support.sort(key=lambda s: (-s.count("+"), s))
    return ("evidence" if support else "inferred"), support[:4]


def import_hints(path: Path) -> None:
    text = path.read_text(encoding="utf-8-sig")
    out = {"source": "WowPacketParser WowPacketParser/Enums/Version/V3_4_3_51666/Opcodes.cs "
                     "(github.com/TrinityCore/WowPacketParser, last change 803417245c69, 2023-11-20); "
                     "built for 3.4.3.51666, used here only as naming hints for 54261",
           "CMSG": {}, "SMSG": {}}
    section = None
    for ln in text.splitlines():
        if "ClientOpcodes" in ln and "new()" in ln:
            section = "CMSG"
        elif "ServerOpcodes" in ln and "new()" in ln:
            section = "SMSG"
        elif "MiscOpcodes" in ln:
            section = None
        m = re.search(r"\{\s*Opcode\.(\w+),\s*(0x[0-9A-Fa-f]+)\s*\}", ln)
        if m and section:
            out[section][f"0x{int(m.group(2), 16):04X}"] = m.group(1)
    HINTS.write_text(json.dumps(out, indent=1))
    print(f"{HINTS}: {len(out['CMSG'])} CMSG, {len(out['SMSG'])} SMSG hints")


def propose(rows: list[dict], hints: dict) -> None:
    for r in rows:
        d = r["direction"]
        key = f"0x{r['opcode']:04X}"
        hint = hints.get(d, {}).get(key)
        if hint:
            r["name"], r["name_source"] = hint, "hint (WPP 3.4.3 number)"
        else:
            # a "Handle Xxx" log string or nothing
            name = None
            for it in r.get("evidence", {}).get("strings", []):
                m = re.match(r"Handle ([A-Za-z ]+)$", it["value"])
                if m and it["depth"] <= 1:
                    name = f"{d}_" + "_".join(w.upper() for w in m.group(1).split())
                    break
            if name:
                r["name"], r["name_source"] = name, "client log string"
            else:
                tag = r.get("protocol", "") if d == "SMSG" else ""
                r["name"] = f"{d}_UNKNOWN_{key[2:]}" + (f"_{tag.upper()}" if tag else "")
                r["name_source"] = "none"
        conf, support = judge(r["name"], r)
        if r["name_source"] == "none":
            conf = "inferred"
        r["confidence"], r["support"] = conf, support


# ---- output --------------------------------------------------------------------------------------

def body(seq: list | None, n: int = 10) -> str:
    if not seq:
        return ""
    return " ".join(seq[:n]) + (" ..." if len(seq) > n else "")


def short(rec: dict) -> str:
    e = rec.get("evidence", {})
    bits = []
    if rec.get("message_types"):
        bits.append("msg: " + ", ".join(rec["message_types"][:2]))
    for k, lab in (("lua", "lua"), ("events", "event"), ("strings", "str"), ("types", "type"), ("files", "src")):
        vals = [it["value"] for it in e.get(k, []) if it["depth"] <= 2][:2]
        if k == "files":
            vals = [v.rsplit("/", 1)[-1] for v in vals]
        if vals:
            bits.append(f"{lab}: " + ", ".join(v.replace("|", "/").replace("\n", " ")[:50] for v in vals))
    return "; ".join(bits)[:220]


# The path from the character list into the world, in the order a server meets it. The notes are
# what was read off the decompilation by hand (2026-10-20); the numbers and confidence always come
# from the generated records.
LOGIN_PATH = [
    ("handshake", "SMSG_AUTH_CHALLENGE"), ("handshake", "CMSG_AUTH_SESSION"),
    ("handshake", "SMSG_ENTER_ENCRYPTED_MODE"), ("handshake", "CMSG_ENTER_ENCRYPTED_MODE_ACK"),
    ("handshake", "SMSG_AUTH_RESPONSE"),
    ("glue", "SMSG_FEATURE_SYSTEM_STATUS_GLUE_SCREEN"), ("glue", "SMSG_SET_TIME_ZONE_INFORMATION"),
    ("glue", "SMSG_AVAILABLE_HOTFIXES"), ("glue", "SMSG_ACCOUNT_DATA_TIMES"), ("glue", "SMSG_TUTORIAL_FLAGS"),
    ("list", "CMSG_ENUM_CHARACTERS"), ("list", "SMSG_ENUM_CHARACTERS_RESULT"),
    ("create", "CMSG_GENERATE_RANDOM_CHARACTER_NAME"), ("create", "SMSG_GENERATE_RANDOM_CHARACTER_NAME_RESULT"),
    ("create", "CMSG_CREATE_CHARACTER"), ("create", "SMSG_CREATE_CHAR"),
    ("delete", "CMSG_CHAR_DELETE"), ("delete", "SMSG_DELETE_CHAR"),
    ("login", "CMSG_PLAYER_LOGIN"), ("login", "SMSG_CHARACTER_LOGIN_FAILED"),
    ("login", "SMSG_LOGIN_VERIFY_WORLD"), ("login", "SMSG_NEW_WORLD"), ("login", "CMSG_WORLD_PORT_RESPONSE"),
    ("login", "CMSG_LOADING_SCREEN_NOTIFY"),
    ("login", "SMSG_ACCOUNT_DATA_TIMES"), ("login", "CMSG_REQUEST_ACCOUNT_DATA"), ("login", "SMSG_UPDATE_ACCOUNT_DATA"),
    ("login", "SMSG_FEATURE_SYSTEM_STATUS"), ("login", "SMSG_MOTD"), ("login", "SMSG_LOGIN_SET_TIME_SPEED"),
    ("login", "SMSG_BIND_POINT_UPDATE"), ("login", "SMSG_INITIAL_SETUP"), ("login", "SMSG_WORLD_SERVER_INFO"),
    ("login", "SMSG_SET_PROFICIENCY"), ("login", "SMSG_UPDATE_TALENT_DATA"), ("login", "SMSG_SEND_KNOWN_SPELLS"),
    ("login", "SMSG_SEND_UNLEARN_SPELLS"), ("login", "SMSG_SEND_SPELL_HISTORY"), ("login", "SMSG_SEND_SPELL_CHARGES"),
    ("login", "SMSG_ACTIVE_GLYPHS"), ("login", "SMSG_UPDATE_ACTION_BUTTONS"), ("login", "SMSG_INITIALIZE_FACTIONS"),
    ("login", "SMSG_SETUP_CURRENCY"), ("login", "SMSG_LOAD_EQUIPMENT_SET"), ("login", "SMSG_ALL_ACHIEVEMENT_DATA"),
    ("login", "SMSG_ALL_ACCOUNT_CRITERIA"), ("login", "SMSG_ACCOUNT_MOUNT_UPDATE"), ("login", "SMSG_ACCOUNT_TOY_UPDATE"),
    ("login", "SMSG_CONTACT_LIST"), ("login", "SMSG_INIT_WORLD_STATES"), ("login", "SMSG_PHASE_SHIFT_CHANGE"),
    ("login", "SMSG_UPDATE_OBJECT"), ("login", "SMSG_MOVE_SET_ACTIVE_MOVER"), ("login", "CMSG_SET_ACTIVE_MOVER"),
    ("login", "SMSG_TIME_SYNC_REQUEST"), ("login", "CMSG_TIME_SYNC_RESPONSE"), ("login", "CMSG_QUEUED_MESSAGES_END"),
    ("keepalive", "CMSG_PING"), ("keepalive", "SMSG_PONG"), ("keepalive", "CMSG_KEEP_ALIVE"),
    ("logout", "CMSG_LOGOUT_REQUEST"), ("logout", "SMSG_LOGOUT_RESPONSE"), ("logout", "SMSG_LOGOUT_COMPLETE"),
]
LOGIN_NOTES = {
    "SMSG_AUTH_CHALLENGE": "connection switch 0x140726b80, raw body; handler FUN_141232760 (handshake ANALYSIS 3.1)",
    "CMSG_AUTH_SESSION": "u64, u32 x3, 16 + 24 bytes, bits, u32 len, bytes (handshake ANALYSIS 3.2)",
    "SMSG_ENTER_ENCRYPTED_MODE": "64-byte signature + bit Enabled; 'Protocol error: mode change out of order'",
    "SMSG_AUTH_RESPONSE": "'Handle Auth Response: %u'; must follow ENTER_ENCRYPTED_MODE (handshake ANALYSIS 3.5)",
    "CMSG_ENUM_CHARACTERS": "empty; sent by FUN_1416a3260 right after AUTH_RESPONSE result 0 ('Glue Authed To WoW')",
    "SMSG_ENUM_CHARACTERS_RESULT": "parser FUN_1406faac0; 'Handle Char Enum'; layout in handshake ANALYSIS 5.2",
    "CMSG_GENERATE_RANDOM_CHARACTER_NAME": "u8, u8 (Race, Sex per TC); Lua C_CharacterCreation.RequestRandomName",
    "SMSG_GENERATE_RANDOM_CHARACTER_NAME_RESULT": "u8 (bits: Success, name length) then name bytes; fires RANDOM_CHARACTER_NAME_RESULT",
    "CMSG_CREATE_CHARACTER": "bit-packed header (name length, flag bits) then fields, writer FUN_140754ce0; "
                             "sender FUN_1416a2d80 under C_CharacterCreation.CreateCharacter",
    "SMSG_CREATE_CHAR": "u8 Code, PackedGuid Guid (parser FUN_1406bb280); 'Handle Character Create: %u'",
    "CMSG_CHAR_DELETE": "PackedGuid; Lua DeleteCharacter(index)",
    "SMSG_DELETE_CHAR": "u8 Code (the handler thunk passes body byte 0); 'Handle Character Delete: %u'",
    "CMSG_PLAYER_LOGIN": "PackedGuid Guid, f32 FarClip; sender logs 'Character Login SEND'",
    "SMSG_CHARACTER_LOGIN_FAILED": "raw body; handler logs 'Handle Character Login Failed: %u'",
    "SMSG_LOGIN_VERIFY_WORLD": "u32 MapID, f32 X, Y, Z, Facing, u32 Reason (24 bytes; handler FUN_140233a20 reads all six "
                               "and does nothing when MapID is the current map). Client strings only say LoadNewWorld",
    "SMSG_NEW_WORLD": "raw body; 'Client : [Airlock] NewWorldHandler MapID:%d', then the client sends WORLD_PORT_RESPONSE",
    "CMSG_WORLD_PORT_RESPONSE": "empty; 'New World Handler : Send World Port Response'",
    "CMSG_LOADING_SCREEN_NOTIFY": "u32 MapID, bit Showing (flushed); LoadingScreen.cpp",
    "SMSG_ACCOUNT_DATA_TIMES": "PackedGuid PlayerGuid, u64 ServerTime, u64 AccountTimes[15]; handler 'Received Server Save "
                               "Times' (AccountData.cpp), may answer with REQUEST_ACCOUNT_DATA ('Requesting Data Download')",
    "CMSG_REQUEST_ACCOUNT_DATA": "PackedGuid PlayerGuid, bits DataType (flushed); sent from the ACCOUNT_DATA_TIMES handler FUN_14156efd0",
    "SMSG_TUTORIAL_FLAGS": "32 raw bytes (8 x u32) copied by handler FUN_14196f920; no client string names it",
    "SMSG_LOGIN_SET_TIME_SPEED": "u32 ServerTime, u32 GameTime (packed dates), f32 NewSpeed are what the handler reads "
                                 "('Gamespeed set from %.03f to %.03f'); any later fields are not read there",
    "SMSG_BIND_POINT_UPDATE": "f32 X, Y, Z, u32 MapID, u32 AreaID (20 bytes; handler FUN_141735130)",
    "SMSG_INITIAL_SETUP": "u8, u8 (TC: ServerExpansionLevel, ServerExpansionTier); handler FUN_141735910",
    "SMSG_SEND_KNOWN_SPELLS": "bit InitialLogin (flushed), u32 KnownCount, u32 FavoriteCount, u32[KnownCount], u32[FavoriteCount]",
    "SMSG_UPDATE_ACTION_BUTTONS": "u64 ActionButtons[180], u8 Reason (handler FUN_141714600; Reason 0 copies all 180)",
    "SMSG_TIME_SYNC_REQUEST": "u32 SequenceIndex; handler FUN_141741aa0 answers with TIME_SYNC_RESPONSE",
    "CMSG_TIME_SYNC_RESPONSE": "u32 SequenceIndex, u32 ClientTime; sent by the TIME_SYNC_REQUEST handler",
    "SMSG_UPDATE_OBJECT": "u32 NumObjUpdates, u16 MapID, bit HasRemovedObjects [u16 DestroyCount, u32 Count, "
                          "PackedGuid[Count]], u32 DataSize, bytes Data (parser FUN_140706c80); handler FUN_1418efa20 "
                          "('mismatched map ID ... in object update message')",
    "SMSG_MOVE_SET_ACTIVE_MOVER": "PackedGuid MoverGUID",
    "CMSG_SET_ACTIVE_MOVER": "PackedGuid",
    "CMSG_QUEUED_MESSAGES_END": "u32 (TC: Timestamp)",
    "CMSG_LOGOUT_REQUEST": "1 bit (TC: IdleLogout), flushed; Lua Logout / Quit",
    "CMSG_PING": "u32, u32 (TC: Serial, Latency)",
    "CMSG_KEEP_ALIVE": "empty; sent while a loading screen is up ('Loading Screen Ping SEND')",
    "SMSG_INIT_WORLD_STATES": "u32 x4 header then pairs (TC: MapID, AreaID, SubareaID, Count, {ID, Value}); JamClientWorldStateInfo",
}


def write_md(smsg: list[dict], cmsg: list[dict], protos: list[dict], hints: dict, path: Path) -> None:
    def cnt(rows, c):
        return sum(1 for r in rows if r["confidence"] == c)

    L = ["# Opcode table, WoW Classic 3.4.3.54261 (client as source of truth)", "",
         "Generated by `python tools/opcodes.py` from `build/corpus.jsonl` and the clean exe; do not edit",
         "by hand. Numbers and existence come only from the client. Names are proposals: by number from",
         f"WowPacketParser's 3.4.3.51666 table ({len(hints.get('SMSG', {}))} SMSG / {len(hints.get('CMSG', {}))} CMSG hints),",
         "else from a client `Handle ...` log string, else `*_UNKNOWN_*`.",
         "**confidence** is `evidence` when a client string (log text, Lua event fired by the handler,",
         "Lua API reaching the sender, Jam type name, source file) shares the name's distinctive words;",
         "`inferred` otherwise. A hint name marked `inferred` is a number match only.", "",
         f"- S→C: {len(smsg)} opcodes ({cnt(smsg, 'evidence')} evidence, {cnt(smsg, 'inferred')} inferred); "
         f"{sum(1 for r in smsg if r['handler_globals'] and not r['handlers'])} dispatch through a handler global that "
         "nothing in the client ever sets (parsed, then dropped); "
         f"{sum(1 for r in smsg if not r['handler_globals'] and not r.get('delegates'))} have neither a handler global "
         "nor a recognised delegate (read the case).",
         f"- C→S: {len(cmsg)} writers ({cnt(cmsg, 'evidence')} evidence, {cnt(cmsg, 'inferred')} inferred).", "",
         "Full records (parser, reads, handler globals, registrars, thunks, evidence with depth) are in",
         "`docs/opcodes.json`; `python tools/opcodes.py --show 0x2583` prints one.", "",
         "Reading the `reads`/`writes` columns: the stream primitives in call order (`bytes(rest|n)` with",
         "nothing else means the message arrives as a raw blob and its handler reads the fields), other",
         "`FUN_x(stream)` entries are nested parsers/writers. Bit fields are not decoded.", ""]
    by = {r["name"]: r for r in smsg + cmsg}
    L += ["## Character list to in-world", "",
          "In the order a server meets them. `layout` is what was read off the parser/handler/writer "
          "(field names follow TrinityCore where the client gives none); `(see json)` means not read here.", "",
          "| step | dir | opcode | name | confidence | layout / notes |", "|---|---|---|---|---|---|"]
    for step, name in LOGIN_PATH:
        r = by.get(name)
        if r is None:
            L.append(f"| {step} | | *absent* | {name} | | not found in the client |")
            continue
        seqs = r.get("parser_reads") or r.get("reads_inline") or r.get("writes") or []
        note = LOGIN_NOTES.get(name) or ("empty body" if r.get("empty_body") else " ".join(seqs[:14]) + " (see json)")
        L.append(f"| {step} | {r['direction'][0]}{'→C' if r['direction'] == 'SMSG' else '→S'} | "
                 f"{r['opcode']:#06x} | {name} | {r['confidence']} | {note} |")
    L += ["", "## Protocols (S→C dispatch)", "", "| protocol | object | switch | opcodes |", "|---|---|---|---|"]
    for p in protos:
        n = [r["opcode"] for r in smsg if r["protocol"] == p["protocol"]]
        rng = f"{min(n):#06x}..{max(n):#06x} ({len(n)})" if n else "-"
        sw = f"{p['switch']:#x}" if p["switch"] else "-"
        L.append(f"| {p['protocol']} | {p['object'] or '-'} | {sw} | {rng} |")
    L += ["", "## Server → client", "",
          "| opcode | proposed name | confidence | protocol | parser | reads | handler (registrar) | client evidence |",
          "|---|---|---|---|---|---|---|---|"]
    for r in smsg:
        hs = []
        for h in r["handlers"][:2]:
            t = h.get("handler", h["value"])
            tgt = [x.get("target") for x in h.get("thunk", []) if x.get("target")]
            hs.append(f"{t}{' → ' + tgt[-1] if tgt else ''} ({h['registrar']})")
        for d in r.get("delegates", [])[:1]:
            hs.append(f"delegate {d['global']} ({', '.join(d.get('setup', [])[:1]) or '?'})")
        hcol = "; ".join(hs) or ("*none*" if not r["handler_globals"] else f"*{r['handler_globals'][0]} never set*")
        L.append(f"| {r['opcode']:#06x} | {r['name']} | {r['confidence']} | {r['protocol']} | {r['parser']} | "
                 f"{body(r.get('parser_reads') or r.get('reads_inline'))} | {hcol} | {short(r)} |")
    L += ["", "## Client → server", "",
          "| opcode | proposed name | confidence | writer | writes | vtable | ctor | senders | client evidence |",
          "|---|---|---|---|---|---|---|---|---|"]
    for r in cmsg:
        L.append(f"| {r['opcode']:#06x} | {r['name']} | {r['confidence']} | {r['writer']} | "
                 f"{'empty' if r.get('empty_body') else body(r['writes'])} | {', '.join(r['vtables'][:1]) or '-'} | {', '.join(r['ctors'][:1]) or '-'} | "
                 f"{r['sender_count']} | {short(r)} |")
    path.write_text("\n".join(L) + "\n", encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--depth", type=int, default=3, help="evidence walk depth (default 3)")
    ap.add_argument("--show", type=lambda s: int(s, 16), help="print one opcode's record from docs/opcodes.json")
    ap.add_argument("--import-hints", type=Path, help="WPP Opcodes.cs -> data/opcode_hints.json")
    ap.add_argument("--json", type=Path, default=DOCS / "opcodes.json")
    ap.add_argument("--md", type=Path, default=DOCS / "opcodes.md")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    if args.import_hints:
        import_hints(args.import_hints)
        return
    if args.show is not None:
        d = json.loads(args.json.read_text(encoding="utf-8"))
        for r in d["smsg"] + d["cmsg"]:
            if r["opcode"] == args.show:
                print(json.dumps(r, indent=1))
        return
    hints = json.loads(HINTS.read_text()) if HINTS.exists() else {}
    print("loading the image and the corpus...", file=sys.stderr)
    b = Binary()
    cor = Corpus()
    ev = Evidence(b, cor)
    print(f"  {len(ev.events)} upper-case identifiers hashed for event ids", file=sys.stderr)
    protos = protocols(b, cor)
    smsg = server_to_client(b, cor, ev, args.depth)
    cmsg = client_to_server(b, cor, ev, args.depth)
    propose(smsg, hints)
    propose(cmsg, hints)
    for r in smsg + cmsg:
        r["opcode_hex"] = f"0x{r['opcode']:04X}"
    present = {"SMSG": {r["opcode_hex"] for r in smsg}, "CMSG": {r["opcode_hex"] for r in cmsg}}
    missing = {d: {k: v for k, v in hints.get(d, {}).items() if k not in present[d]} for d in ("SMSG", "CMSG")}
    out = {
        "build": "3.4.3.54261",
        "generator": "tools/opcodes.py",
        "hints": hints.get("source"),
        "counts": {"smsg": len(smsg), "cmsg": len(cmsg),
                   "smsg_evidence": sum(r["confidence"] == "evidence" for r in smsg),
                   "cmsg_evidence": sum(r["confidence"] == "evidence" for r in cmsg)},
        "protocols": [{**p, "switch": f"{p['switch']:#x}" if p["switch"] else None} for p in protos],
        "smsg": smsg, "cmsg": cmsg,
        "hints_not_in_client": missing,
    }
    args.json.write_text(json.dumps(out, indent=1), encoding="utf-8")
    write_md(smsg, cmsg, protos, hints, args.md)
    c = out["counts"]
    print(f"S->C {c['smsg']} ({c['smsg_evidence']} evidence)  C->S {c['cmsg']} ({c['cmsg_evidence']} evidence)  "
          f"hints absent from client: {len(missing['SMSG'])} SMSG, {len(missing['CMSG'])} CMSG")
    print(f"wrote {args.json} and {args.md}")


if __name__ == "__main__":
    main()
