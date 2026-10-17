"""Fidelity of the `// ref:` ports, binary against binary, and where the stubs are.

Every tagged frostlite function is compared with the reference function it claims to be, using
the compiled port (FrostLite.exe + FrostLite.pdb) and the reference (WowClassic-clean.exe).
Four signals, each scored against what the reference has, then blended:

  calls   the direct calls, in order, as a weighted longest common subsequence. A port callee
          counts as the reference function its own tag names. Compiler noise is taken out first:
          library calls (CRT, std, Blizzard's FID-named runtime) are dropped on both sides; the
          port's inlined tagged functions are put back where the PDB says they were inlined;
          untagged port helpers are flattened into their caller, because the reference usually
          inlined the code a port split out; and a reference callee that is a tiny leaf (it
          makes no calls of its own) weighs a quarter, because the port's compiler often
          inlines exactly those.
  fields  the displacements the code reads and writes through a pointer ([reg+0x1c]): struct
          layouts are ported to match, so these should agree even where codegen does not.
  consts  the strings, float/double constants and non-trivial integer immediates it uses.
  shape   mnemonic trigram overlap. Reported, not blended: on ports that match the reference in
          calls, fields and constants its median is 0.56 and it spans the whole range, so it
          measures register allocation and scheduling (two compilers), not the port.

A port whose instructions match the reference's with addresses masked is reported as `same`.

    python tools/fidelity.py                          # every scored port, worst first
    python tools/fidelity.py --file model2/           # ports under src/model2
    python tools/fidelity.py --addr 1411ae6a0 --show  # one port in full: both call sequences,
                                                      # the fields and constants it misses
    python tools/fidelity.py --below 0.6 --min-calls 3
    python tools/fidelity.py --stubs                  # live stubs first: stubs that ported
                                                      # code calls, by reference size
    python tools/fidelity.py --stubs --all            # every stub, dead ones too
    python tools/fidelity.py --summary --record       # totals, appended to docs/fidelity.md

Every run writes build/fidelity.json (one record per tagged function) for other tools.
Both sides are optimized builds: read the body before trusting a low score.
"""
from __future__ import annotations

import argparse
import bisect
import datetime
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
import numpy as np

import tags as tagscan
from corpus import load_corpus
from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
PORT_EXE = ROOT / "build/cmake/bin/Release/FrostLite.exe"
PORT_PDB = ROOT / "build/cmake/bin/Release/FrostLite.pdb"
LLVM = Path("C:/Program Files/LLVM/bin")
OUT_JSON = ROOT / "build/fidelity.json"
# links a source tag cannot express: a template instance or header code, by its PDB name
LINKS = Path(__file__).with_name("links.json")
PREV_JSON = ROOT / "build/fidelity.prev.json"
REF_CACHE = ROOT / "build/fidelity-ref-cache.json"
HISTORY = ROOT / "docs/fidelity.md"

WEIGHTS = {"calls": 0.5, "fields": 0.28, "consts": 0.22, "shape": 0.0}
LEAF_WEIGHT = 0.25          # a reference callee that calls nothing and is this small or less
LEAF_SIZE = 0x40
FLATTEN_DEPTH = 3           # how deep untagged port helpers are flattened
FLATTEN_SIZE = 0x800        # and how big one may be

# ---- the port's PDB --------------------------------------------------------------------------

REC = re.compile(r"^\s*(\d+) \| (S_\w+)")
PROC = re.compile(r"S_[GL]PROC32 \[size = \d+\] `(.*)`$")
ADDR = re.compile(r"addr = ([0-9A-Fa-f]{4}):(\d+), code size = (\d+)")
PARENT = re.compile(r"parent = (\d+)")
INLINEE = re.compile(r"inlinee = 0x([0-9A-F]+) ")
CODE = re.compile(r"code 0x([0-9A-F]+) ")
ID_REC = re.compile(r"^\s*0x([0-9A-F]+) \| (LF_M?FUNC_ID)")
ID_NAME = re.compile(r"name = (.*), type = 0x[0-9A-F]+, (?:class type = 0x([0-9A-F]+)|parent scope = )")
TYPE_REC = re.compile(r"^\s*0x([0-9A-F]+) \| LF_(?:CLASS|STRUCTURE|UNION) \[size = \d+\] `(.*)`$")


def pdbutil(*args: str) -> str:
    return subprocess.run([str(LLVM / "llvm-pdbutil.exe"), "dump", *args, str(PORT_PDB)], capture_output=True,
                          text=True, errors="replace").stdout


def strip_name(name: str) -> str:
    """A PDB name without templates or the anonymous namespace, for matching."""
    name = name.replace("`anonymous namespace'::", "")
    out, depth = [], 0
    for ch in name:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def short_name(name: str) -> str:
    return strip_name(name).split("::")[-1]


def last_two(name: str) -> str:
    return "::".join(strip_name(name).split("::")[-2:])


def load_pdb(binary: Binary):
    """The procs (va, size, name) and, per proc, its inline sites as (va, inlinee name)."""
    classes = {}
    for line in pdbutil("-types").splitlines():
        m = TYPE_REC.match(line)
        if m:
            classes[int(m.group(1), 16)] = m.group(2)
    ids = {}
    current = None
    for line in pdbutil("-ids").splitlines():
        m = ID_REC.match(line)
        if m:
            current = int(m.group(1), 16)
            continue
        if current is not None:
            m = ID_NAME.search(line)
            if m:
                name = m.group(1)
                if m.group(2):
                    owner = classes.get(int(m.group(2), 16))
                    if owner:
                        name = f"{owner}::{name}"
                ids[current] = name
            current = None

    procs = []
    inlines = defaultdict(list)
    proc_of_rec = {}
    parent_of_rec = {}
    pending = None          # (kind, record offset)
    for line in pdbutil("-symbols").splitlines():
        if line.startswith("Mod ") or line.lstrip().startswith("Mod "):
            proc_of_rec.clear()
            parent_of_rec.clear()
            pending = None
            continue
        m = REC.match(line)
        if m:
            rec, kind = int(m.group(1)), m.group(2)
            pending = None
            if kind in ("S_GPROC32", "S_LPROC32"):
                pm = PROC.search(line)
                pending = ("proc", rec, pm.group(1) if pm else "?")
            elif kind == "S_INLINESITE":
                pending = ("inline", rec, None)
            continue
        if pending is None:
            continue
        kind, rec, name = pending
        if kind == "proc":
            am = ADDR.search(line)
            if am:
                section = binary.sections[int(am.group(1), 16) - 1]
                va = section.start + int(am.group(2))
                procs.append((va, int(am.group(3)), name))
                proc_of_rec[rec] = va
                pending = None
        elif kind == "inline":
            pm = PARENT.search(line)
            im = INLINEE.search(line)
            if pm and im:
                parent_of_rec[rec] = int(pm.group(1))
                root = rec
                for _ in range(64):
                    if root in proc_of_rec:
                        break
                    root = parent_of_rec.get(root)
                    if root is None:
                        break
                pending = ("inline-code", rec, (proc_of_rec.get(root), ids.get(int(im.group(1), 16))))
        elif kind == "inline-code":
            cm = CODE.search(line)
            if cm:
                proc_va, inlinee = name
                if proc_va is not None and inlinee:
                    inlines[proc_va].append((proc_va + int(cm.group(1), 16), inlinee))
                pending = None
    return sorted(set(procs)), inlines


def symbolize(addrs: list[int], base: int) -> dict[int, tuple[str, int]]:
    """address -> (source file, line) of its first instruction."""
    text = "\n".join(hex(a - base) for a in addrs) + "\n"
    out = subprocess.run([str(LLVM / "llvm-symbolizer.exe"), f"--obj={PORT_EXE}", "--relative-address",
                          "--no-inlines", "--functions=none"], input=text, capture_output=True, text=True,
                         errors="replace").stdout
    blocks, cur = [], []
    for line in out.splitlines():
        if line.strip() == "":
            if cur:
                blocks.append(cur)
                cur = []
        else:
            cur.append(line)
    if cur:
        blocks.append(cur)
    result = {}
    for a, block in zip(addrs, blocks):
        m = re.match(r"(.*):(\d+):\d+$", block[-1])
        if m:
            result[a] = (m.group(1).replace("\\", "/").lower(), int(m.group(2)))
    return result


# ---- library calls ---------------------------------------------------------------------------

PORT_LIB = re.compile(r"^(std::|`|__|_[A-Z_a-z]|mem(cpy|set|move|cmp|chr)\b|str\w*$|wcs\w*$|"
                      r"(sqrt|sin|cos|tan|atan2?|floor|ceil|pow|exp|log|fmod|abs|fabs)f?$|rand$|atexit$|[a-z_]*(printf|scanf)[a-z_]*$|"
                      r"Concurrency::|Microsoft::)")
# Storm's operator new/delete and the blz allocators are Blizzard code, flattened like any helper:
# the reference inlines them down to SMemAlloc/SMemFree
REF_KEEP = ("FUN_", "thunk_FUN", "Lua::", "Storm::", "CDataAllocator::")

# deliberate divergences, each with its reason (tools/fidelity_exceptions.json)
_EXCEPTIONS = json.loads((Path(__file__).with_name("fidelity_exceptions.json")).read_text(encoding="utf-8"))
REF_SKIP = {int(a, 16) for a in _EXCEPTIONS["skip"]} | {int(a, 16) for a in _EXCEPTIONS.get("lib", {})}
REF_ALIAS = {int(a, 16): int(v["to"], 16) for a, v in _EXCEPTIONS["alias"].items()}

def ref_is_lib(cor: dict, a: int) -> bool:
    entry = cor.get(a)
    if entry is None:
        return False
    name = entry["name"]
    return not name.startswith(REF_KEEP) and "::" not in name


# ---- disassembly -----------------------------------------------------------------------------

JCC = re.compile(r"^(j(?!mp)\w+|cmov\w+|set\w+)$")
SSE_SCALAR = {"ss": 4, "sd": 8}


class Features:
    """What one function's instructions use, in a form both binaries can be compared on."""

    def __init__(self):
        self.calls: list[int] = []          # direct call/jmp targets leaving the function
        self.call_sites: list[int] = []     # their instruction addresses
        self.fields: Counter = Counter()
        self.consts: Counter = Counter()
        self.mnemonics: list[str] = []
        self.shape: list[str] = []          # mnemonic + operands with addresses masked

    def to_json(self) -> dict:
        return {"calls": self.calls, "fields": dict(self.fields), "consts": dict(self.consts),
                "mnemonics": self.mnemonics, "shape": self.shape}

    @staticmethod
    def from_json(d: dict) -> "Features":
        f = Features()
        f.calls = d["calls"]
        f.fields = Counter({int(k): v for k, v in d["fields"].items()})
        f.consts = Counter(d["consts"])
        f.mnemonics = d["mnemonics"]
        f.shape = d["shape"]
        return f


def float_text(raw: bytes) -> str:
    if len(raw) == 4:
        v = float(np.frombuffer(raw, dtype="<f4")[0])
    else:
        v = float(np.frombuffer(raw, dtype="<f8")[0])
    if v != v:
        return "f:nan"
    if len(raw) == 4 or abs(v) < 3.4e38:
        v = float(np.float32(v))
    return f"f:{v:.6g}"


def check_code(binary: Binary, md, ranges: list[tuple[int, int]], checks: dict[int, int],
               junk: dict[int, int]) -> set[int]:
    """The instructions of the anti-tamper checks in these ranges (tools/retcheck.py): what each
    check's start reaches and its success target does not, the range tests, the table walk and
    the crash loops. Counted as the function's, a Lua handler of one call looked like it made six
    and its decompilation was stubbed as having lost calls (61 functions, 2026-10-16)."""
    insns = {}
    for begin, end in ranges:
        code = binary.image[binary.off(begin):binary.off(begin) + (end - begin)]
        for insn in md.disasm(bytes(code), begin):
            insns[insn.address] = insn

    def walk(seeds, stop):
        seen, todo = set(), list(seeds)
        while todo:
            va = todo.pop()
            if va in seen or va in stop or va not in insns:
                continue
            seen.add(va)
            i = insns[va]
            m = i.mnemonic
            nxt = va + i.size
            if va in junk:
                todo.append(junk[va])
                continue
            if m in ("ret", "int3", "ud2", "hlt"):
                continue
            if m.startswith("j") and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM:
                todo.append(i.operands[0].imm)
                if m != "jmp":
                    todo.append(nxt)
                continue
            if m == "jmp":
                continue
            todo.append(nxt)
        return seen

    out: set[int] = set()
    for s, t in checks.items():
        if s in insns:
            out |= walk([s], {t}) - walk([t], set())
    return out


def features(binary: Binary, md, ranges: list[tuple[int, int]], inside, skip: set[int] | None = None) -> Features:
    f = Features()
    for begin, end in ranges:
        code = binary.image[binary.off(begin):binary.off(begin) + (end - begin)]
        for insn in md.disasm(bytes(code), begin):
            if skip and insn.address in skip:
                continue
            mn = insn.mnemonic
            norm = "jcc" if JCC.match(mn) and mn.startswith("j") else ("cmov" if mn.startswith("cmov") else mn)
            f.mnemonics.append(norm)
            ops = insn.operands
            text = insn.op_str
            branch = mn.startswith(("call", "j")) and ops and ops[0].type == capstone.x86.X86_OP_IMM
            # a conditional branch out of the function is a tail call too
            if branch and (mn in ("call", "jmp") or not inside(ops[0].imm)):
                target = ops[0].imm
                if not inside(target):
                    f.calls.append(target)
                    f.call_sites.append(insn.address)
                f.shape.append(f"{norm} X")
                continue
            if mn.startswith("j") or mn in ("int3", "nop") or mn.startswith("nop"):
                f.shape.append(norm)
                continue
            for op in ops:
                if op.type == capstone.x86.X86_OP_MEM:
                    base = insn.reg_name(op.mem.base) if op.mem.base else ""
                    if base == "rip":
                        target = insn.address + insn.size + op.mem.disp
                        s = binary.cstr(target, 256) if binary.section_of(target) is not None else None
                        if s is not None and len(s) >= 3 and s.isprintable() and mn == "lea":
                            f.consts["s:" + s] += 1
                        elif mn.endswith(("ss", "sd")) and mn[-2:] in SSE_SCALAR and binary.section_of(target):
                            size = SSE_SCALAR[mn[-2:]]
                            off = binary.off(target)
                            f.consts[float_text(bytes(binary.image[off:off + size]))] += 1
                    elif base not in ("rsp", "rbp", "esp") and op.mem.disp > 0 and mn != "lea":
                        f.fields[op.mem.disp] += 1
                elif op.type == capstone.x86.X86_OP_IMM:
                    v = op.imm & 0xffffffff
                    if 0x10 <= v <= 0xfffffff0 and not ("rsp" in text and mn in ("add", "sub")):
                        f.consts[f"i:{v:x}"] += 1
            masked = re.sub(r"0x[0-9a-f]{6,}", "X", text)
            f.shape.append(f"{norm} {masked}")
    return f


def resolve_thunk(binary: Binary, md, target: int, depth: int = 0) -> int:
    """A function that is only `jmp elsewhere` is its destination."""
    if depth > 4 or binary.section_of(target) is None:
        return target
    code = binary.image[binary.off(target):binary.off(target) + 16]
    for insn in md.disasm(bytes(code), target):
        if insn.mnemonic == "jmp" and insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            return resolve_thunk(binary, md, insn.operands[0].imm, depth + 1)
        break
    return target


# ---- scoring ---------------------------------------------------------------------------------

def weighted_lcs(ref: list, weights: list[float], port: list) -> tuple[float, list[bool]]:
    """The heaviest common subsequence: its weight and which reference entries it kept."""
    n, m = len(ref), len(port)
    if n == 0 or m == 0:
        return 0.0, [False] * n
    if n * m > 1_000_000:   # too big to align: credit by multiset instead
        have = Counter(port)
        kept = []
        for x in ref:
            kept.append(have[x] > 0)
            have[x] -= 1
        return sum(w for w, k in zip(weights, kept) if k), kept
    table = [[0.0] * (m + 1)]
    for i in range(1, n + 1):
        x, w = ref[i - 1], weights[i - 1]
        prev = table[i - 1]
        cur = [0.0] * (m + 1)
        for j in range(1, m + 1):
            if port[j - 1] == x:
                cur[j] = prev[j - 1] + w
            else:
                a, b = prev[j], cur[j - 1]
                cur[j] = a if a > b else b
        table.append(cur)
    kept = [False] * n
    i, j = n, m
    while i > 0 and j > 0:
        if ref[i - 1] == port[j - 1] and abs(table[i][j] - table[i - 1][j - 1] - weights[i - 1]) < 1e-9:
            kept[i - 1] = True
            i -= 1
            j -= 1
        elif table[i - 1][j] >= table[i][j - 1]:
            i -= 1
        else:
            j -= 1
    return table[n][m], kept


def lcs_pairs(a: list, b: list) -> list[tuple[int, int]]:
    """The matched index pairs of a longest common subsequence."""
    n, m = len(a), len(b)
    if n == 0 or m == 0 or n * m > 1_000_000:
        return []
    table = [[0] * (m + 1)]
    for i in range(1, n + 1):
        prev, cur, x = table[i - 1], [0] * (m + 1), a[i - 1]
        for j in range(1, m + 1):
            cur[j] = prev[j - 1] + 1 if b[j - 1] == x else (prev[j] if prev[j] > cur[j - 1] else cur[j - 1])
        table.append(cur)
    pairs = []
    i, j = n, m
    while i > 0 and j > 0:
        if a[i - 1] == b[j - 1]:
            pairs.append((i - 1, j - 1))
            i -= 1
            j -= 1
        elif table[i - 1][j] >= table[i][j - 1]:
            i -= 1
        else:
            j -= 1
    return pairs[::-1]


def overlap(ref: Counter, port: Counter) -> float | None:
    total = sum(ref.values())
    if total == 0:
        return None
    return sum(min(v, port.get(k, 0)) for k, v in ref.items()) / total


def trigram_score(a: list[str], b: list[str]) -> float | None:
    if len(a) < 3 or len(b) < 3:
        return None
    ta = Counter(zip(a, a[1:], a[2:]))
    tb = Counter(zip(b, b[1:], b[2:]))
    inter = sum((ta & tb).values())
    union = sum((ta | tb).values())
    return inter / union if union else None


# ---- callers in the port, for stub liveness --------------------------------------------------

def port_references(binary: Binary, starts: np.ndarray) -> dict[int, list[int]]:
    """function start -> addresses that call, jump to, take the address of, or table it."""
    text = binary.text
    data = np.frombuffer(bytes(binary.image[binary.off(text.start):binary.off(text.start) + (text.end - text.start)]),
                         dtype=np.uint8)
    refs = defaultdict(list)
    start_set = starts

    def add(sites: np.ndarray, targets: np.ndarray):
        hit = np.isin(targets, start_set)
        for s, t in zip(sites[hit].tolist(), targets[hit].tolist()):
            refs[t].append(s)

    n = len(data) - 7
    for opcode in (0xE8, 0xE9):
        pos = np.nonzero(data[:n] == opcode)[0]
        disp = (data[pos + 1].astype(np.int64) | (data[pos + 2].astype(np.int64) << 8)
                | (data[pos + 3].astype(np.int64) << 16) | (data[pos + 4].astype(np.int64) << 24))
        disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
        add(pos + text.start, pos + text.start + 5 + disp)
    # lea r64, [rip+disp32]: REX.W 8D modrm(00 reg 101)
    pos = np.nonzero((data[:n] & 0xF8 == 0x48) & (data[1:n + 1] == 0x8D) & ((data[2:n + 2] & 0xC7) == 0x05))[0]
    disp = (data[pos + 3].astype(np.int64) | (data[pos + 4].astype(np.int64) << 8)
            | (data[pos + 5].astype(np.int64) << 16) | (data[pos + 6].astype(np.int64) << 24))
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    add(pos + text.start, pos + text.start + 7 + disp)
    # pointer tables (vtables, callback arrays) in the data sections
    for section in binary.sections:
        if section is text or section.end - section.start < 8:
            continue
        raw = bytes(binary.image[binary.off(section.start):binary.off(section.start) + ((section.end - section.start) & ~7)])
        words = np.frombuffer(raw, dtype="<u8").astype(np.int64)
        idx = np.arange(len(words), dtype=np.int64) * 8 + section.start
        add(idx, words)
    return refs


# ---- bulk linking ------------------------------------------------------------------------------

def code_callers(binary: Binary, starts: list[int], owner) -> dict[int, set]:
    """function start -> the functions that call, jump to or take the address of it."""
    callers = defaultdict(set)
    for target, sites in port_references(binary, np.array(starts, dtype=np.int64)).items():
        for site in sites:
            o = owner(site)
            if o is not None and o != target:
                callers[target].add(o)
    return callers


def string_owners(binary: Binary, owner) -> dict[str, set]:
    """string -> the functions that take its address (`lea r64, [rip+disp]` onto a C string of
    six characters or more)."""
    text = binary.text
    data = np.frombuffer(bytes(binary.image[binary.off(text.start):binary.off(text.start) + (text.end - text.start)]),
                         dtype=np.uint8)
    n = len(data) - 7
    pos = np.nonzero(((data[:n] & 0xF8) == 0x48) & (data[1:n + 1] == 0x8D) & ((data[2:n + 2] & 0xC7) == 0x05))[0]
    disp = (data[pos + 3].astype(np.int64) | (data[pos + 4].astype(np.int64) << 8)
            | (data[pos + 5].astype(np.int64) << 16) | (data[pos + 6].astype(np.int64) << 24))
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    targets = pos + text.start + 7 + disp
    out = defaultdict(set)
    seen: dict[int, str | None] = {}
    for site, target in zip((pos + text.start).tolist(), targets.tolist()):
        if target not in seen:
            s = binary.cstr(target, 200) if binary.section_of(target) is not None and not binary.is_code(target) else None
            seen[target] = s if s is not None and len(s) >= 6 and s.isprintable() else None
        s = seen[target]
        if s is None:
            continue
        o = owner(site)
        if o is not None:
            out[s].add(o)
    return out


def link(ref, port, md, cor, procs, proc_at, proc_starts, port_of_ref, ref_of_port, ref_features, port_sequence,
         score_pair, ref_crt, apply_tag, flush_tags, args) -> None:
    """Propagates links out from the tied functions until nothing new verifies.

    Every linked pair proposes candidates twice: the untagged calls on each side that fall into
    the same gap of the two aligned call sequences, and the untagged callers of each side. A
    candidate is accepted only when it scores as a port of the reference function (the same four
    signals as the report, with something to compare: calls, fields or constants), its size is
    plausible, and it is the best match both ways. Accepted pairs are anchors for the next round.
    Plain functions get a source tag above their definition; template instances and header code,
    which one tag cannot name, go into tools/links.json by PDB name.
    """
    def ref_owner(site):
        i = bisect.bisect_right(ref.piece_starts, site) - 1
        return ref.piece_root[i] if i >= 0 and site < ref.piece_ends[i] else None

    def port_owner(site):
        i = bisect.bisect_right(proc_starts, site) - 1
        if i >= 0 and site < proc_starts[i] + proc_at[proc_starts[i]][0]:
            return proc_starts[i]
        return None

    print("indexing callers...", file=sys.stderr)
    ref_callers = code_callers(ref, sorted(set(ref.piece_root)), ref_owner)
    port_callers = code_callers(port, proc_starts, port_owner)

    def ref_ok(x) -> bool:
        return (x in cor and x not in port_of_ref and not ref_is_lib(cor, x) and x not in REF_SKIP
                and cor[x].get("size", 0) >= 8 and ref_crt(x) is None)

    def port_ok(va) -> bool:
        if va not in proc_at or va in ref_of_port or proc_at[va][0] < 8:
            return False
        stripped = strip_name(proc_at[va][1])
        return not PORT_LIB.match(stripped) and not stripped.startswith("FrostUnimplemented")

    # strings each side takes the address of: one used by a single function on both sides pins
    # the pair
    print("indexing strings...", file=sys.stderr)
    ref_strings = string_owners(ref, ref_owner)
    port_strings = string_owners(port, port_owner)
    string_pairs = []
    for s, owners in ref_strings.items():
        mine = port_strings.get(s)
        if mine and len(owners) == 1 and len(mine) == 1:
            string_pairs.append((next(iter(owners)), next(iter(mine))))
    print(f"{len(string_pairs)} strings unique on both sides", file=sys.stderr)

    accepted = []
    seen_pairs = set()
    for round_ in range(1, 13):
        cand = defaultdict(set)
        for x, va in string_pairs:
            if ref_ok(x) and port_ok(va):
                cand[x].add(va)
        for r, p in list(port_of_ref.items()):
            rseq = [REF_ALIAS.get(x, x) for x in ref_features(r).calls if not ref_is_lib(cor, x)]
            pseq = port_sequence(p, 0, {p}, [], [], flatten=False)
            pairs = lcs_pairs(rseq, pseq)
            bounds = [(-1, -1)] + pairs + [(len(rseq), len(pseq))]
            for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
                rgap = [x for x in rseq[i0 + 1:i1] if ref_ok(x)]
                pgap = [x[1] for x in pseq[j0 + 1:j1] if isinstance(x, tuple) and port_ok(x[1])]
                if rgap and pgap and len(rgap) * len(pgap) <= 64:
                    for x in rgap:
                        cand[x].update(pgap)
            rc = [x for x in ref_callers.get(r, ()) if ref_ok(x)]
            pc = [va for va in port_callers.get(p, ()) if port_ok(va)]
            if rc and pc and len(rc) * len(pc) <= 64:
                for x in rc:
                    cand[x].update(pc)
        scored = []
        for x, vas in cand.items():
            for va in vas:
                if (x, va) in seen_pairs:
                    continue
                seen_pairs.add((x, va))
                ratio = proc_at[va][0] / max(1, cor[x].get("size", 1))
                if not 0.25 <= ratio <= 4.0:
                    continue
                m = score_pair(x, va)
                if not any(m["parts"][k] is not None for k in ("calls", "fields", "consts")):
                    continue
                need = args.match if cor[x].get("size", 0) >= 0x40 else max(args.match, 0.9)
                if m["score"] >= need:
                    scored.append((m["score"], x, va))
        # the best match both ways
        scored.sort(reverse=True)
        best_for_port = {}
        for score, x, va in scored:
            best_for_port.setdefault(va, (score, x))
        new = 0
        for score, x, va in scored:
            if x in port_of_ref or va in ref_of_port or best_for_port[va][1] != x:
                continue
            port_of_ref[x] = va
            ref_of_port[va] = x
            accepted.append((score, x, va))
            new += 1
        print(f"round {round_}: {len(cand)} reference candidates, {len(scored)} verified, {new} linked", file=sys.stderr)
        if new == 0:
            break

    print(f"{len(accepted)} new links", file=sys.stderr)
    if args.dry_run:
        for score, x, va in sorted(accepted)[:args.limit or 40]:
            print(f"{score:4.2f}  FUN_{x:x}  {proc_at[va][1]}")
        return
    links = json.loads(LINKS.read_text(encoding="utf-8")) if LINKS.exists() else {}
    tagged = 0
    for score, x, va in accepted:
        if apply_tag(x, va):
            tagged += 1
        else:
            links[proc_at[va][1]] = f"FUN_{x:x}"
    flush_tags()
    LINKS.write_text(json.dumps(dict(sorted(links.items())), indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"{tagged} source tags written, {len(links)} links in {LINKS.name}", file=sys.stderr)
    if tagged:
        print("rebuild before measuring: tags tie through the build's line table, and the new tag lines "
              "moved the definitions under them", file=sys.stderr)


# ---- main ------------------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", help="only ports whose source path contains this")
    ap.add_argument("--addr", nargs="*", help="only these reference addresses")
    ap.add_argument("--below", type=float, default=1.01, help="only scores below this")
    ap.add_argument("--min-calls", type=int, default=0, help="only references making this many calls")
    ap.add_argument("--by", choices=["score", "calls", "fields", "consts", "shape"], default="score",
                    help="the column to sort and filter by")
    ap.add_argument("--show", action="store_true", help="print each port in full")
    ap.add_argument("--stubs", action="store_true", help="list the stubs, live ones first")
    ap.add_argument("--all", action="store_true", help="with --stubs: dead stubs too")
    ap.add_argument("--summary", action="store_true", help="only the totals")
    ap.add_argument("--suggest", action="store_true",
                    help="untagged port functions that sit where an untagged reference call does")
    ap.add_argument("--match", type=float, default=0.75,
                    help="with --suggest: the match score a pairing needs to be marked (and applied)")
    ap.add_argument("--link", action="store_true",
                    help="link untagged port functions to the reference in bulk, to a fixpoint (see link())")
    ap.add_argument("--dry-run", action="store_true", help="with --link: report, write nothing")
    ap.add_argument("--apply", action="store_true",
                    help="with --suggest: write the marked pairings' tags into the sources")
    ap.add_argument("--record", action="store_true", help="append the totals to docs/fidelity.md")
    ap.add_argument("--limit", type=int, default=0, help="print at most this many rows")
    ap.add_argument("--crt", action="store_true", help="list the reference functions identified as C runtime")
    ap.add_argument("--delta", action="store_true",
                    help="what moved since the previous run: regressions first, then gains")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ref = Binary()
    port = Binary(PORT_EXE)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    cor = load_corpus()

    procs, inline_sites = load_pdb(port)
    proc_at = {va: (size, name) for va, size, name in procs}
    proc_starts = [va for va, _, _ in procs]
    by_short = defaultdict(list)
    for va, size, name in procs:
        by_short[short_name(name)].append(va)

    # ---- tie each tag to its compiled function -------------------------------------------
    tags = [t for t in tagscan.scan() if t["name"] and not t.get("label")]
    # a hand port claims its address before the translated copy does: src/gen sorts ahead of
    # most of the tree, and a function promoted to a hand port is still translated until the
    # next translate.py run, which only drops it once this file says the port has it
    tags.sort(key=lambda t: t["file"].replace("\\", "/").startswith("src/gen/"))
    wanted = {short_name(t["name"]) for t in tags}
    candidates = sorted({va for s in wanted for va in by_short.get(s, [])})
    where = symbolize(candidates, port.base)
    port_of_ref: dict[int, int] = {}
    ref_of_port: dict[int, int] = {}
    tag_of_ref: dict[int, dict] = {}
    for t in tags:
        tfile = t["file"].replace("\\", "/").lower()
        best = None
        for va in by_short.get(short_name(t["name"]), []):
            loc = where.get(va)
            if loc is None or not loc[0].endswith(tfile) or loc[1] <= t["line"]:
                continue
            if best is None or loc[1] < where[best][1]:
                best = va
        if best is not None and t["addr"] not in port_of_ref:
            port_of_ref[t["addr"]] = best
            ref_of_port.setdefault(best, t["addr"])
            tag_of_ref[t["addr"]] = t
    # src/gen (tools/translate.py): an adapter or stub is the reference function its name gives,
    # so a call to it counts as that call rather than being flattened into what it does
    for va, size, name in procs:
        m = re.fullmatch(r"FUN_(14[0-9a-f]{7})", name)
        if m:
            ref_of_port.setdefault(va, int(m.group(1), 16))
    links = json.loads(LINKS.read_text(encoding="utf-8")) if LINKS.exists() else {}
    va_by_name = defaultdict(list)
    for va, size, name in procs:
        va_by_name[name].append(va)
    for name, addr in links.items():
        r = int(addr.removeprefix("FUN_"), 16)
        vas = va_by_name.get(name, [])
        if len(vas) == 1 and r not in port_of_ref and vas[0] not in ref_of_port:
            port_of_ref[r] = vas[0]
            ref_of_port[vas[0]] = r
            tag_of_ref[r] = {"addr": r, "file": "tools/links.json", "line": 0, "name": name, "stub": False,
                             "partial": False, "sites": []}
    untied = [t for t in tags if t["addr"] not in port_of_ref]

    # names an inline site can carry, for the tagged functions
    ref_by_inline_name: dict[str, int] = {}
    ambiguous = set()
    by_short_tagged = defaultdict(set)
    for r, va in port_of_ref.items():
        name = proc_at[va][1]
        ref_by_inline_name[last_two(name)] = r
        by_short_tagged[short_name(name)].add(r)
    for s, rs in by_short_tagged.items():
        if len(rs) == 1 and s not in ref_by_inline_name:
            ref_by_inline_name[s] = next(iter(rs))
        elif len(rs) > 1:
            ambiguous.add(s)

    def inline_ref(name: str) -> int | None:
        r = ref_by_inline_name.get(last_two(name))
        if r is None and short_name(name) not in ambiguous:
            r = ref_by_inline_name.get(short_name(name))
        return r

    unimplemented = [va for va, _, name in procs if short_name(name) == "FrostUnimplemented"]
    unimplemented_va = unimplemented[0] if unimplemented else None

    # ---- features ------------------------------------------------------------------------
    piece_index = defaultdict(list)
    for i in range(len(ref.piece_starts)):
        piece_index[ref.piece_root[i]].append((ref.piece_starts[i], ref.piece_ends[i]))

    def ref_root(t: int) -> int:
        i = bisect.bisect_right(ref.piece_starts, t) - 1
        return ref.piece_root[i] if i >= 0 and t < ref.piece_ends[i] else t

    # the anti-tamper checks, left out of the reference's features (check_code)
    rc_path = ROOT / "build/facts/retcheck.json"
    rc = json.loads(rc_path.read_text()) if rc_path.exists() else {}
    rc_checks = {int(s, 16): int(t, 16) for s, t in rc.get("checks", {}).items()}
    rc_junk = {int(j, 16): int(t, 16) for j, t in rc.get("junk_jumps", {}).items()}
    cache = {}
    ref_mtime = str(Path(ref.path).stat().st_mtime) + ":" + (str(rc_path.stat().st_mtime) if rc_path.exists() else "")
    if REF_CACHE.exists():
        loaded = json.loads(REF_CACHE.read_text())
        if loaded.get("mtime") == ref_mtime and loaded.get("version") == 6:
            cache = loaded["functions"]
    cache_dirty = False

    def ref_features(a: int) -> Features:
        nonlocal cache_dirty
        key = f"{a:x}"
        if key in cache:
            return Features.from_json(cache[key])
        pieces = sorted(piece_index.get(a, []))

        def inside(t):
            return ref_root(t) == a and any(b <= t < e for b, e in pieces)

        mine = {s: t for s, t in rc_checks.items() if any(b <= s < e for b, e in pieces)}
        skip = check_code(ref, md, pieces, mine, rc_junk) if mine else None
        f = features(ref, md, pieces, inside, skip)
        f.calls = [ref_root(resolve_thunk(ref, md, t)) for t in f.calls]
        cache[key] = f.to_json()
        cache_dirty = True
        return f

    port_cache: dict[int, Features] = {}

    def port_features(va: int) -> Features:
        if va not in port_cache:
            size = proc_at[va][0] if va in proc_at else 0
            f = features(port, md, [(va, va + size)], lambda t: va <= t < va + size)
            f.calls = [resolve_thunk(port, md, t) for t in f.calls]
            port_cache[va] = f
        return port_cache[va]

    # The C runtime routines FID left unnamed in the reference (memmove, memset,
    # _Init_thread_header, ...): a reference callee whose instructions match one of the port's
    # own PDB-named runtime functions of a similar size is that routine, and drops out like any
    # library call. The port links the same MSVC runtime, so the bodies agree closely.
    crt_bodies = []
    for va, size, name in procs:
        stripped = strip_name(name)
        # C runtime names only: lowercase C identifiers, not the port's own helpers
        if size >= 0x30 and re.fullmatch(r"_*[a-z][a-z0-9_]*", stripped) and PORT_LIB.match(stripped):
            crt_bodies.append((size, va, stripped))
    crt_bodies.sort()
    crt_sizes = [c[0] for c in crt_bodies]
    crt_known: dict[int, str | None] = {int(k, 16): v for k, v in cache.get("_crt", {}).items()}
    # (and the routines the translator calls by their C library names, identified by running
    # them, tools/oracle.py: the port's call to `cos` is then the reference's to FUN_142416680,
    # where the trigram match above misses the compiler's inline expansions; 2026-10-17)
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from translate import CRT_BY_ADDR
    crt_known.update(CRT_BY_ADDR)

    def ref_crt(a: int) -> str | None:
        """The runtime routine a reference function is, or None."""
        nonlocal cache_dirty
        if a in crt_known:
            return crt_known[a]
        result = None
        entry = cor.get(a)
        size = entry.get("size", 0) if entry else 0
        if a not in port_of_ref and 0x30 <= size <= 0x2000 and entry and entry["name"].startswith("FUN_"):
            mnemonics = ref_features(a).mnemonics
            lo = bisect.bisect_left(crt_sizes, int(size * 0.6))
            hi = bisect.bisect_right(crt_sizes, int(size * 1.6) + 1)
            best = 0.0
            for _, va, name in crt_bodies[lo:hi]:
                score = trigram_score(mnemonics, port_features(va).mnemonics) or 0.0
                if score > best:
                    best, result = score, name
            if best < 0.8:
                result = None
        crt_known[a] = result
        cache.setdefault("_crt", {})[f"{a:x}"] = result
        cache_dirty = True
        return result

    def ref_callee_weight(a: int) -> float:
        entry = cor.get(a)
        if entry is not None and entry.get("size", 0) <= LEAF_SIZE:
            if not ref_features(a).calls:
                return LEAF_WEIGHT
        return 1.0

    def port_sequence(va: int, depth: int, seen: set, stub_sites: list, merged: list, flatten: bool = True) -> list:
        """The port's calls as reference addresses (or ("port", va)), untagged helpers flattened
        into it when `flatten` (and listed in `merged`)."""
        f = port_features(va)
        events = [(site, "call", t) for site, t in zip(f.call_sites, f.calls)]
        events += [(site, "inline", name) for site, name in inline_sites.get(va, [])]
        events.sort(key=lambda e: (e[0], e[1] != "inline"))
        out = []
        for _, kind, x in events:
            if kind == "inline":
                r = inline_ref(x)
                if r is not None:
                    out.append(r)
                continue
            if x == unimplemented_va:
                stub_sites.append(va)
                continue
            if x in ref_of_port:
                out.append(ref_of_port[x])
                continue
            info = proc_at.get(x)
            if info is None or PORT_LIB.match(strip_name(info[1])):
                continue
            if flatten and depth < FLATTEN_DEPTH and info[0] <= FLATTEN_SIZE and x not in seen:
                merged.append(x)
                out.extend(port_sequence(x, depth + 1, seen | {x}, stub_sites, merged))
            else:
                out.append(("port", x))
        return out

    def merged_features(pva: int, helpers: list[int]) -> tuple[Counter, Counter]:
        """The port's fields and constants with those of the code it keeps out of line where
        the reference did not."""
        own = port_features(pva)
        fields, consts = Counter(own.fields), Counter(own.consts)
        for h in dict.fromkeys(helpers):
            if h in proc_at and proc_at[h][0] <= 2 * FLATTEN_SIZE:
                hf = port_features(h)
                fields.update(hf.fields)
                consts.update(hf.consts)
        return fields, consts

    def score_pair(r: int, pva: int) -> dict:
        """How closely the compiled port function at `pva` matches reference function `r`."""
        rf = ref_features(r)
        stub_sites: list = []
        helpers: list = []
        pseq = port_sequence(pva, 0, {pva}, stub_sites, helpers)
        rseq = [REF_ALIAS.get(x, x) for x in rf.calls
                if not ref_is_lib(cor, x) and x not in REF_SKIP and ref_crt(x) is None]
        weights = [ref_callee_weight(x) for x in rseq]
        total = sum(weights)
        # a tagged callee the reference never calls was inlined there: its own calls stand in
        # for it (one level, and only for helpers of a size a compiler inlines)
        rset = set(rseq)
        expanded = []
        for x in pseq:
            pva_x = port_of_ref.get(x) if not isinstance(x, tuple) else None
            if pva_x is not None and x not in rset and proc_at[pva_x][0] <= FLATTEN_SIZE:
                helpers.append(pva_x)
                expanded.extend(port_sequence(pva_x, 1, {pva, pva_x}, [], helpers))
            else:
                expanded.append(x)
        pseq = expanded
        got, kept = weighted_lcs(rseq, weights, pseq)
        # calls the port makes and the reference does not: code the reference inlined
        rset = set(rseq)
        for x in pseq:
            if x in rset:
                continue
            helpers.append(x[1] if isinstance(x, tuple) else port_of_ref.get(x, 0))
        fields, consts = merged_features(pva, helpers)
        pf = port_features(pva)
        parts = {
            "calls": got / total if total else None,
            "fields": overlap(rf.fields, fields),
            "consts": overlap(rf.consts, consts),
            "shape": trigram_score(rf.mnemonics, pf.mnemonics),
        }
        avail = {k: v for k, v in parts.items() if v is not None and WEIGHTS[k] > 0}
        score = (sum(WEIGHTS[k] * v for k, v in avail.items()) / sum(WEIGHTS[k] for k in avail)) if avail else 1.0
        return {"score": score, "parts": parts, "rf": rf, "pf": pf, "rseq": rseq, "pseq": pseq, "kept": kept,
                "fields": fields, "consts": consts, "stub_sites": stub_sites}

    pending = defaultdict(list)     # file -> [(line index, text)]

    def apply_tag(x: int, va: int) -> bool:
        """Queue `// ref: FUN_x` above the definition of the port function at `va`."""
        loc = symbolize([va], port.base).get(va)
        if loc is None:
            return False
        path = Path(loc[0])
        if not path.is_absolute():
            path = ROOT / path
        rel = path.resolve().relative_to(ROOT.resolve()).as_posix() if path.exists() else ""
        if not rel.startswith(tagscan.TAG_ROOTS) or path.suffix not in (".cpp", ".c"):
            return False
        lines = path.read_text(encoding="utf-8").splitlines()
        name = short_name(proc_at[va][1])
        pat = re.compile(r"(^|[\s:*&~])" + re.escape(name) + r"\s*\(")
        for i in range(min(loc[1] - 1, len(lines) - 1), max(-1, loc[1] - 60), -1):
            if pat.search(lines[i]) and not lines[i].rstrip().endswith(";"):
                # already tagged, or a note naming the address on purpose without a tag
                # (a port that is more than the reference function, say)
                if i > 0 and ("ref:" in lines[i - 1] or f"{x:x}" in lines[i - 1].lower()):
                    return False
                indent = re.match(r"\s*", lines[i]).group(0)
                text = f"{indent}/* ref: FUN_{x:x} */" if path.suffix == ".c" else f"{indent}// ref: FUN_{x:x}"
                pending[path].append((i, text))
                return True
        return False

    def flush_tags() -> int:
        written = 0
        for path, inserts in pending.items():
            lines = path.read_text(encoding="utf-8").splitlines()
            for i, text in sorted(inserts, reverse=True):
                lines.insert(i, text)
                written += 1
            path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        pending.clear()
        return written

    if args.link:
        link(ref, port, md, cor, procs, proc_at, proc_starts, port_of_ref, ref_of_port, ref_features, port_sequence,
             score_pair, ref_crt, apply_tag, flush_tags, args)
        return

    # ---- score every tied port -----------------------------------------------------------
    records = {}
    votes: Counter = Counter()
    for r, pva in sorted(port_of_ref.items()):
        t = tag_of_ref[r]
        m = score_pair(r, pva)
        rf, pf, rseq, pseq, kept = m["rf"], m["pf"], m["rseq"], m["pseq"], m["kept"]
        fields, consts, stub_sites, parts, score = m["fields"], m["consts"], m["stub_sites"], m["parts"], m["score"]
        same = rf.shape == pf.shape and len(rf.shape) > 0
        if t["stub"] and not t["partial"]:
            kind = "stub"
        elif t["stub"]:
            kind = "partial"
        else:
            kind = "ported"
        missing = [x for x, k in zip(rseq, kept) if not k]
        # tag suggestions: a gap where the reference calls something untagged and the port
        # calls an untagged function of its own, one for one
        raw = port_sequence(pva, 0, {pva}, [], [], flatten=False)
        pairs = lcs_pairs(rseq, raw)
        bounds = [(-1, -1)] + pairs + [(len(rseq), len(raw))]
        for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
            rgap = [x for x in rseq[i0 + 1:i1] if x not in port_of_ref]
            pgap = [x for x in raw[j0 + 1:j1] if isinstance(x, tuple)]
            if rgap and len(rgap) == len(pgap) and len(rgap) <= 3:
                for x, y in zip(rgap, pgap):
                    votes[(x, y[1])] += 1
        records[r] = {
            "addr": f"{r:x}", "name": proc_at[pva][1], "file": t["file"], "line": t["line"], "kind": kind,
            "score": round(score, 4), **{k: (round(v, 4) if v is not None else None) for k, v in parts.items()},
            "same": same, "reaches_stub": bool(stub_sites), "ref_calls": len(rseq), "port_calls": len(pseq),
            "ref_size": cor.get(r, {}).get("size", 0), "port_size": proc_at[pva][0],
            "missing": [f"{x:x}" for x in dict.fromkeys(missing)], "sites": t.get("sites", []),
            "_r": rseq, "_p": pseq, "_kept": kept, "_rf": rf, "_pf": pf, "_fields": fields, "_consts": consts,
        }

    if cache_dirty:
        REF_CACHE.write_text(json.dumps({"mtime": ref_mtime, "version": 6, "functions": cache}))

    # ---- stub liveness ---------------------------------------------------------------------
    stub_ports = {pva: r for r, pva in port_of_ref.items() if records[r]["kind"] == "stub"}
    refs = port_references(port, np.array(proc_starts, dtype=np.int64))

    def proc_containing(a: int) -> int | None:
        i = bisect.bisect_right(proc_starts, a) - 1
        if i >= 0 and a < proc_starts[i] + proc_at[proc_starts[i]][0]:
            return proc_starts[i]
        return None

    # what runs: everything reachable from the entry points and the data tables (vtables,
    # initializer lists, callbacks) through code that is not itself a stub
    edges = defaultdict(set)
    roots = {va for va, _, name in procs if short_name(name) in ("WinMain", "wWinMain", "mainCRTStartup",
                                                                  "wWinMainCRTStartup", "WinMainCRTStartup")}
    table_refs = Counter()
    for target, sites in refs.items():
        for site in sites:
            owner = proc_containing(site)
            if owner is None:
                roots.add(target)
                table_refs[target] += 1
            elif owner != target:
                edges[owner].add(target)
    reached = set()
    queue = list(roots)
    while queue:
        va = queue.pop()
        if va in reached:
            continue
        reached.add(va)
        if va in stub_ports:
            continue
        queue.extend(edges.get(va, ()))
    callers_of = defaultdict(set)
    for owner, targets in edges.items():
        for target in targets:
            callers_of[target].add(owner)

    for pva, r in stub_ports.items():
        callers = {c for c in callers_of.get(pva, ()) if c not in stub_ports and c in reached}
        rec = records[r]
        rec["callers"] = [f"{('FUN_%x' % ref_of_port[c]) if c in ref_of_port else 'untagged'} {proc_at[c][1]}"
                         for c in sorted(callers)]
        rec["stub_callers"] = sum(1 for c in callers_of.get(pva, ()) if c in stub_ports)
        rec["tables"] = table_refs.get(pva, 0)
        rec["live"] = pva in reached and (bool(callers) or rec["tables"] > 0 or pva in roots)

    # ---- output ----------------------------------------------------------------------------
    def name_of(x) -> str:
        if isinstance(x, tuple):
            return f"untagged {proc_at.get(x[1], (0, hex(x[1])))[1]}"
        pva = port_of_ref.get(x)
        return f"FUN_{x:x} {proc_at[pva][1] if pva else '(no port)'}"

    previous = {}
    if OUT_JSON.exists():
        previous = {d["addr"]: d for d in json.loads(OUT_JSON.read_text())}
        OUT_JSON.replace(PREV_JSON)
    elif PREV_JSON.exists():
        previous = {d["addr"]: d for d in json.loads(PREV_JSON.read_text())}
    OUT_JSON.write_text(json.dumps([{k: v for k, v in rec.items() if not k.startswith("_")}
                                    for rec in records.values()], indent=1))

    if args.delta:
        moved = []
        for rec in records.values():
            old = previous.get(rec["addr"])
            if old is None:
                moved.append((1.0, rec, "new"))
            elif old["kind"] != rec["kind"]:
                moved.append((-1.0 if rec["kind"] == "stub" else 1.0, rec, f"{old['kind']} -> {rec['kind']}"))
            elif abs(rec["score"] - old["score"]) >= 0.02:
                moved.append((rec["score"] - old["score"], rec, f"{old['score']:.2f} -> {rec['score']:.2f}"))
        gone = [a for a in previous if int(a, 16) not in records]
        moved.sort(key=lambda m: m[0])
        for d, rec, what in moved[:args.limit or None]:
            print(f"{'-' if d < 0 else '+'} {what:18} FUN_{rec['addr']}  {rec['name']}  ({rec['file']}:{rec['line']})")
        for a in gone:
            print(f"- gone               FUN_{a}  {previous[a]['name']}")
        print(f"{sum(1 for m in moved if m[0] < 0) + len(gone)} regressions, {sum(1 for m in moved if m[0] > 0)} gains",
              file=sys.stderr)

    # code tools/translate.py generated is measured apart: the headline numbers are the hand ports
    def generated(rec) -> bool:
        return rec["file"].replace("\\", "/").startswith("src/gen/")

    translated = [rec for rec in records.values() if generated(rec) and rec["kind"] != "stub"]
    scored = [rec for rec in records.values() if rec["kind"] != "stub" and not generated(rec)]
    stubs = [rec for rec in records.values() if rec["kind"] == "stub"]
    live = [rec for rec in stubs if rec.get("live")]
    mean = sum(rec["score"] for rec in scored) / len(scored) if scored else 0.0
    weighted = (sum(rec["score"] * rec["ref_size"] for rec in scored) / sum(rec["ref_size"] for rec in scored)
                if scored else 0.0)
    summary = {
        "tags": len(tags), "tied": len(port_of_ref), "untied": len(untied),
        "ported": sum(rec["kind"] == "ported" for rec in records.values()),
        "partial": sum(rec["kind"] == "partial" for rec in records.values()),
        "stubs": len(stubs), "live_stubs": len(live), "same": sum(rec["same"] for rec in scored),
        "live_virtual": sum(1 for rec in live if not rec["callers"]),
        "mean": round(mean, 4), "size_weighted": round(weighted, 4),
        "above_0.9": sum(rec["score"] >= 0.9 for rec in scored),
        "below_0.5": sum(rec["score"] < 0.5 for rec in scored),
        "translated": len(translated),
        "translated_mean": round(sum(r["score"] for r in translated) / len(translated), 4) if translated else 0.0,
        "translated_weighted": round(sum(r["score"] * r["ref_size"] for r in translated)
                                     / sum(r["ref_size"] for r in translated), 4) if translated else 0.0,
        "translated_above_0.9": sum(r["score"] >= 0.9 for r in translated),
    }

    def matches(rec) -> bool:
        if args.file and args.file.replace("\\", "/") not in rec["file"].replace("\\", "/"):
            return False
        if args.addr and rec["addr"] not in {a.lower().removeprefix("0x").removeprefix("fun_") for a in args.addr}:
            return False
        return True

    if args.crt:
        found = sorted((a, n) for a, n in crt_known.items() if n)
        for a, n in found:
            print(f"FUN_{a:x}  {n}")
        print(f"{len(found)} reference functions identified as C runtime", file=sys.stderr)
    elif args.delta:
        pass
    elif args.stubs:
        rows = [rec for rec in stubs if matches(rec) and (args.all or rec.get("live"))]
        rows.sort(key=lambda rec: (not rec.get("live"), -rec["ref_size"]))
        if args.limit:
            rows = rows[:args.limit]
        for rec in rows:
            state = ("LIVE" if rec["callers"] else "virt") if rec.get("live") else "dead"
            print(f"{state}  FUN_{rec['addr']}  {rec['name']}  ({rec['file']}:{rec['line']})  ref {rec['ref_size']} bytes, "
                  f"{len(rec['callers'])} callers, {rec['stub_callers']} stub callers, {rec['tables']} table refs")
            for c in rec["callers"][:4]:
                print(f"        called by {c}")
        partial = [rec for rec in records.values() if rec["kind"] == "partial" and matches(rec)]
        if partial:
            print(f"\npartial ports ({len(partial)}), their unported arms:")
            for rec in sorted(partial, key=lambda rec: -rec["ref_size"])[:args.limit or None]:
                print(f"  FUN_{rec['addr']}  {rec['name']}  ({rec['file']}:{rec['line']})")
                for s in rec["sites"]:
                    print(f"        :{s['line']}  {s['note'] or '(no note)'}")
        print(f"{len(rows)} stubs listed; {len(live)} live of {len(stubs)}", file=sys.stderr)
    elif args.suggest:
        # every candidate scored as if it were tagged; the best match wins, votes break ties
        candidates = defaultdict(list)
        for (x, va), n in votes.items():
            if x in cor and va in proc_at:
                candidates[x].append((va, n))
        best = {}
        for x, cands in candidates.items():
            scored_cands = []
            for va, n in cands:
                ratio = proc_at[va][0] / max(1, cor[x].get("size", 1))
                pm = score_pair(x, va)
                # a match with nothing to compare (no calls, fields or constants) proves nothing
                evidence = any(pm["parts"][k] is not None for k in ("calls", "fields", "consts"))
                scored_cands.append((pm["score"] if evidence else 0.0, n, va, ratio))
            scored_cands.sort(key=lambda c: (-c[0], -c[1]))
            best[x] = scored_cands[0] + (len(cands) - 1,)
        claimed = Counter(c[2] for c in best.values())
        rows = sorted(best.items(), key=lambda kv: (-(kv[1][0] >= args.match), -kv[1][1], -kv[1][0]))
        if args.limit:
            rows = rows[:args.limit]
        print("votes match  size  reference      port function")
        applied = 0
        for x, (match, n, va, ratio, others) in rows:
            note = f"  ({others} other candidates)" if others else ""
            if claimed[va] > 1:
                note += f"  (best for {claimed[va] - 1} other reference functions too)"
            good = match >= args.match and 0.25 <= ratio <= 4.0 and claimed[va] == 1
            print(f"{n:5}  {match:4.2f} {ratio:5.2f}  FUN_{x:x}  {proc_at[va][1]}{note}{'  <-' if good else ''}")
            if good and args.apply and apply_tag(x, va):
                applied += 1
        flush_tags()
        print(f"{len(rows)} suggestions" + (f"; {applied} tags written" if args.apply else ""), file=sys.stderr)
    elif not args.summary:
        key = args.by
        rows = [rec for rec in scored if matches(rec) and rec["ref_calls"] >= args.min_calls]
        rows = [rec for rec in rows if (rec[key] if rec[key] is not None else 1.0) < args.below]
        rows.sort(key=lambda rec: ((rec[key] if rec[key] is not None else 1.0), -rec["ref_size"]))
        if args.limit:
            rows = rows[:args.limit]

        def fmt(v):
            return "  -  " if v is None else f"{v:5.2f}"

        print(" score calls field const shape  function")
        for rec in rows:
            flag = " same" if rec["same"] else (" PARTIAL" if rec["kind"] == "partial" else "")
            if rec["reaches_stub"] and rec["kind"] != "partial":
                flag += " reaches-stub"
            print(f"{rec['score']:5.2f} {fmt(rec['calls'])} {fmt(rec['fields'])} {fmt(rec['consts'])} {fmt(rec['shape'])}  "
                  f"FUN_{rec['addr']}  {rec['name']}  ({rec['file']}:{rec['line']})  "
                  f"ref {rec['ref_calls']} calls/{rec['ref_size']}b{flag}")
            if args.show:
                print("    reference calls (- missing):")
                for x, k in zip(rec["_r"], rec["_kept"]):
                    print(f"      {' ' if k else '-'} {name_of(x)}")
                print("    port calls:")
                for x in rec["_p"]:
                    print(f"        {name_of(x)}")
                rf, fields, consts = rec["_rf"], rec["_fields"], rec["_consts"]
                lost_fields = sorted(k for k in rf.fields if k not in fields)
                extra_fields = sorted(k for k in fields if k not in rf.fields)
                if lost_fields:
                    print("    fields the reference uses and the port does not: "
                          + " ".join(f"{k:#x}" if k >= 0 else f"-{-k:#x}" for k in lost_fields))
                if extra_fields:
                    print("    fields only the port uses: "
                          + " ".join(f"{k:#x}" if k >= 0 else f"-{-k:#x}" for k in extra_fields))
                lost = [k for k in rf.consts if k not in consts]
                if lost:
                    print("    constants the port lacks: " + "  ".join(repr(k) for k in lost[:24]))
                for s in rec["sites"]:
                    print(f"    unported arm :{s['line']}  {s['note'] or '(no note)'}")
            elif rec["missing"]:
                for a in rec["missing"][:8]:
                    print(f"         missing {name_of(int(a, 16))}")
        print(f"{len(rows)} ports listed", file=sys.stderr)

    print(f"{summary['tied']} of {summary['tags']} tags tied to compiled functions; {summary['ported']} ported, "
          f"{summary['partial']} partial, {summary['stubs']} stubs ({summary['live_stubs']} live, "
          f"{summary['live_virtual']} of them only through tables); "
          f"fidelity mean {summary['mean']:.3f}, size-weighted {summary['size_weighted']:.3f}; "
          f"{summary['above_0.9']} at 0.9 or better, {summary['below_0.5']} below 0.5, {summary['same']} same",
          file=sys.stderr)
    if summary["translated"]:
        print(f"translated (src/gen, apart from the above): {summary['translated']} functions, "
              f"fidelity mean {summary['translated_mean']:.3f}, size-weighted {summary['translated_weighted']:.3f}, "
              f"{summary['translated_above_0.9']} at 0.9 or better", file=sys.stderr)
    if args.record:
        stamp = datetime.date.today().isoformat()
        commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True,
                                text=True).stdout.strip()
        if not HISTORY.exists():
            HISTORY.write_text("# Fidelity history\n\nOne row per `python tools/fidelity.py --record` "
                               "(see tools/fidelity.py for what the columns mean).\n\n"
                               "| date | commit | tied | ported | partial | stubs | live stubs | mean | size-weighted "
                               "| >= 0.9 | < 0.5 | same |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n",
                               encoding="utf-8", newline="\n")
        with HISTORY.open("a", encoding="utf-8", newline="\n") as f:
            f.write(f"| {stamp} | {commit} | {summary['tied']} | {summary['ported']} | {summary['partial']} | "
                    f"{summary['stubs']} | {summary['live_stubs']} | {summary['mean']:.3f} | "
                    f"{summary['size_weighted']:.3f} | {summary['above_0.9']} | {summary['below_0.5']} | "
                    f"{summary['same']} |\n")


if __name__ == "__main__":
    main()
