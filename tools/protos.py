"""Every function's parameters, read off its instructions: build/facts/protos.json.

Ghidra's Decompiler Parameter ID (what tools/ghidra/CommitPrototypes.java ran) guesses a
function's inputs from its decompilation and misses those it cannot see used: the cursor scaler
dispatch FUN_1414fc8b0 reads dx and r8w only to build a packed argument on the stack, and was
committed as `(uint)`; its body then decompiled as reading registers "the decompiler lost track
of", the translator stubbed it, and the cursor vanished (2026-10-16). It also over-counts: a
function committed with four parameters makes every caller pass whatever r8/r9 held.

This reads the inputs from the code itself, for the Win64 ABI (positions 1-4 in rcx/rdx/r8/r9
or xmm0-3, the rest on the stack from [entry rsp + 0x28]):

1. Local, one decode per function (in parallel): a forward must-define pass over the eight
   argument registers, as the width written on every path to each point (a 32-bit write defines
   the whole register; a byte or word write only its low part). A read wider than that is an
   input. Reads above the home area of the entry stack, through rsp or a frame pointer set from
   it, are stack parameters. Each direct call or tail call records which argument registers may
   still be unwritten there.
2. Interprocedural: a fixpoint over those summaries. A callee's inputs are its caller's too,
   where the caller may not have written them: an argument passed on untouched (a thunk, a
   `this` handed down) is an input of every function on the way.

A function whose code leaves through an indirect jump that is not a switch (a vtable tail call)
may pass registers on unseen: it is marked `open`, and its count is only a lower bound.

    python tools/protos.py [--jobs N] [--show 0x1414fc8b0 ...]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from multiprocessing import Pool

import capstone
from capstone import x86_const as X

from wowbin import FACTS, ROOT, Binary, hexa, regs_access, write_facts

CORPUS = ROOT / "build" / "corpus.jsonl"

# the eight argument registers: 0-3 rcx rdx r8 r9, 4-7 xmm0-3
FAMILY: dict[int, tuple[int, int]] = {}
for fam, names in enumerate([("RCX", "ECX", "CX", "CL"), ("RDX", "EDX", "DX", "DL"),
                             ("R8", "R8D", "R8W", "R8B"), ("R9", "R9D", "R9W", "R9B")]):
    for name, width in zip(names, (8, 4, 2, 1)):
        FAMILY[getattr(X, f"X86_REG_{name}")] = (fam, width)
FAMILY[X.X86_REG_CH] = (0, 2)
FAMILY[X.X86_REG_DH] = (1, 2)
for i in range(4):
    for kind in ("XMM", "YMM", "ZMM"):
        FAMILY[getattr(X, f"X86_REG_{kind}{i}")] = (4 + i, 16)

# what a call leaves behind: every volatile register is written (garbage, but written)
VOLATILE_DEF = (8, 8, 8, 8, 16, 16, 16, 16)
# the argument positions by register (defs and fresh index them so)
POS_NAMES = ("RCX", "RDX", "R8", "R9", "XMM0", "XMM1", "XMM2", "XMM3")
# every general register by its 64-bit name, for the frame registers
GPR64: dict[int, int] = {}
for _q, _d, _w, _b in [("RAX", "EAX", "AX", "AL"), ("RBX", "EBX", "BX", "BL"), ("RCX", "ECX", "CX", "CL"),
                       ("RDX", "EDX", "DX", "DL"), ("RSI", "ESI", "SI", "SIL"), ("RDI", "EDI", "DI", "DIL"),
                       ("RBP", "EBP", "BP", "BPL"), ("RSP", "ESP", "SP", "SPL")] + \
        [(f"R{i}", f"R{i}D", f"R{i}W", f"R{i}B") for i in range(8, 16)]:
    for _n in (_q, _d, _w, _b):
        GPR64[getattr(X, f"X86_REG_{_n}")] = getattr(X, f"X86_REG_{_q}")
VOLATILE_GPR = [getattr(X, f"X86_REG_{n}") for n in ("RAX", "RCX", "RDX", "R8", "R9", "R10", "R11")]
# a call's unread result, kept in the frame map under keys no register has: the callee's address
PEND_RAX, PEND_XMM = -1000, -1001
# the calls whose xmm0 is still unwritten on every path here (merged by agreement, as PEND_RAX):
# what a ret hands on as this function's float (Local.xpasses)
PEND_XMM_ALL = -1007
# the width of rax this function has written since its last call (merged by the widest)
RAXW = -1002
# for indirect calls: each argument register's width written since the last call, and the
# outgoing stack arguments stored since then (count past the four)
FRESH = [-2000 - fam for fam in range(8)]
OUTSTK = -3000
MAX_KEYS = {RAXW, OUTSTK, *FRESH}
# an argument register written on some path since the entry (a call kills the volatile ones):
# united at a join. `defs` keeps the least written of the paths, which a parameter wants; a
# parameter "handed on" to an indirect call wants it untouched on every path. On a loop's first
# pass r8 still held FUN_1402f25e0's own param_3, after that FUN_140229de0's leftover, and the
# virtual call was given four arguments it does not take: `extraout_R8` (2026-10-18)
DIRTY = [-5000 - fam for fam in range(8)]
# what rax holds on every path, for a function no caller's read types: the narrowest width the
# function wrote itself (CALLW: a callee's result), absent where it can be garbage (unwritten
# since the entry); and the callees whose untouched result can reach here (0: an indirect one)
RAXMIN, RAXCALLS = -1004, -1005
CALLW = 100
# the same for xmm0: the scalar width this function last wrote it with (16: written whole, kind
# unknown), absent where it can be garbage. Calls leave it garbage: only a function's own writes
# say it returns a float
XMIN = -1006
# what xmm0 can hold at this point, every path having set it one way or another: the widths
# this function wrote it with and the callees whose float it still is (union over paths, present
# only where every path set it). A ret where it is absent returns no float. By the last write
# alone (XMIN), CRT sinh's two paths, one writing xmm0 whole and one leaving its error handler's
# double, merged to garbage, and the double result was lost (2026-10-16)
XANY = -1009
# xmm0 last written by an instruction that puts lanes together (`unpcklps xmm0, xmm1`: the sin in
# lane 0 and the cos in lane 1 of a sincos, FUN_14240ad20): a vector result, 16 bytes, whose
# upper lanes callers read (`extraout_XMM0_Db` against a float return)
VECTOR = 17
LANE_BUILD = {"unpcklps", "unpckhps", "unpcklpd", "unpckhpd", "movlhps", "movhlps", "shufps", "shufpd",
              "insertps", "blendps", "blendpd"}
RAX_WIDTH = {X.X86_REG_RAX: 8, X.X86_REG_EAX: 4, X.X86_REG_AX: 2, X.X86_REG_AL: 1, X.X86_REG_AH: 2}
XMM0_REGS = {X.X86_REG_XMM0, X.X86_REG_YMM0, X.X86_REG_ZMM0}
XMM_ALL = {getattr(X, f"X86_REG_{k}{i}") for k in ("XMM", "YMM", "ZMM") for i in range(16)}
# xmm0-5 do not survive a call; xmm6-15 do
VOLATILE_XMM = [getattr(X, f"X86_REG_{k}{i}") for k in ("XMM", "YMM", "ZMM") for i in range(6)]
# a copy of an xmm argument, frame key ALIAS - register: the argument position it holds
ALIAS = -4000
WHOLE_MOVES = {"movaps", "movups", "movapd", "movupd", "movdqa", "movdqu"}
# packed arithmetic: every lane is a value (not the bitwise ops, which work a scalar's sign too)
PACKED_ARITH = {"addps", "subps", "mulps", "divps", "minps", "maxps", "sqrtps", "rcpps", "rsqrtps", "cmpps",
                "haddps", "hsubps", "dpps", "unpckhps", "movhlps", "addsubps", "roundps",
                "cvtps2dq", "cvttps2dq"}
_MD_LITE = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def is_noop(code: bytes, a: int) -> bool:
    """Whether the code at a does nothing: stores of registers into its own home slots (a varargs
    spill), then a plain ret. Only `ret`; one-instruction `ret` functions too."""
    for _addr, _size, mn, ops_ in _MD_LITE.disasm_lite(code, a):
        if mn == "ret":
            return not ops_
        if not (mn == "mov" and re.fullmatch(r"(?:qword|dword|word|byte) ptr \[rsp \+ (?:8|0x10|0x18|0x20)\], \w+", ops_)):
            return False
    return False


def packed(m: str) -> bool:
    """Packed float arithmetic, every lane a value (and the compares capstone spells by their
    predicate, `cmpnleps`: FUN_14240b780 tests four lanes of its argument at once)."""
    m = m.lstrip("v")
    return m in PACKED_ARITH or (m.startswith("cmp") and m.endswith("ps"))


LANEWISE = WHOLE_MOVES | PACKED_ARITH |{"andps", "andnps", "orps", "xorps", "andpd", "andnpd", "orpd", "xorpd",
                                         "pand", "pandn", "por", "pxor"}
NONE_DEF = (0,) * 8
ZERO_IDIOMS = {"xor", "sub", "pxor", "xorps", "xorpd", "vxorps", "vxorpd", "vpxor", "psubd", "psubq"}
BRANCH_END = {"ret", "retf", "int3", "ud2", "hlt"}
# SSE instructions that write their destination's low lanes and keep the rest: capstone counts
# the destination as read (the merge), but a value there is not used. `movss xmm2, xmm0` read as
# an input made a double third parameter of FUN_14240ad20 and, through the fixpoint, of every
# function on the way to it
MERGE_ONLY = {"movss", "movsd", "cvtsi2ss", "cvtsi2sd", "cvtss2sd", "cvtsd2ss", "sqrtss", "sqrtsd", "rcpss",
              "rsqrtss", "roundss", "roundsd", "movlps", "movlpd", "movhps", "movhpd", "pinsrb", "pinsrw",
              "pinsrd", "pinsrq", "insertps", "movq2dq", "cvtsi2ssl", "cvtsi2sdl", "cvtsi2ssq", "cvtsi2sdq"}


# scalar floating-point arithmetic and moves (`addss`, `movsd xmm0, ...`), for the kind a function
# computes in: not the string `movsd`, which has no xmm operand
SCALAR_OPS = ("add", "sub", "mul", "div", "sqrt", "min", "max", "mov", "cvt", "comi", "ucomi", "rcp", "rsqrt")
WHOLE_LOADS = {"movups", "movaps", "movupd", "movapd", "movdqu", "movdqa", "lddqu", "movdqu8", "movdqu16",
               "movdqu32", "movdqu64", "movdqa32", "movdqa64"}


def xmm_width(mnem: str) -> int:
    """The scalar width an SSE instruction reads of an xmm register: what a float or double
    parameter looks like from inside."""
    # (the conversions by their source: `cvtsd2ss` ends in ss and reads a double)
    if mnem.lstrip("v") in ("cvtsd2ss", "cvtsd2si", "cvttsd2si"):
        return 8
    if mnem.endswith("ss") or mnem in ("cvtss2sd", "cvtss2si", "cvttss2si", "comiss", "ucomiss", "movd"):
        return 4
    if mnem.endswith("sd") or mnem in ("cvtsd2ss", "cvtsd2si", "cvttsd2si", "comisd", "ucomisd", "movq"):
        return 8
    # the bitwise ops on a float's or a double's sign and magnitude (`andpd xmm0, mask`, the
    # fabs of a double parameter FUN_142030b00 had typed a float, its upper half `in_XMM0_Db`)
    # (not the `ps` forms: compilers use `andps` on doubles too, one byte shorter)
    if mnem.lstrip("v") in ("andpd", "andnpd", "orpd", "xorpd"):
        return 8
    return 16


class Local:
    """One function's summary: inputs read here, and the calls whose callees' inputs may be."""
    __slots__ = ("inputs", "kinds", "stack", "calls", "open", "error", "why", "rets", "passes", "written",
                 "garbage", "ret_calls", "xwidths", "xgarbage", "ss", "sd", "call_sites", "tail_sites", "exits", "ipasses", "spill_width", "icalls", "local_floor",
                 "xpasses", "spilled", "slot_reads", "va_lea", "chk_calls", "vector_in", "xret_calls",
                 "xany_widths", "xany_garbage", "feeds", "call_targets", "writes_xmm0", "dfresh")

    def __init__(self):
        # indirect calls: (address, widths written since the last call x8, defined widths x8,
        # outgoing stack arguments)
        # (keyed by address: an instruction is visited again as its state settles, and the last
        # visit's is the settled one)
        self.icalls: dict[int, tuple] = {}
        self.local_floor = 1 << 30     # the lowest rsp offset >= 0x20 read back or taken: a local
        # what rax holds at every ret: the narrowest width written on all paths (CALLW: a callee's
        # result), whether some path leaves it garbage, and the callees whose result it can be
        self.written = 1 << 30
        self.garbage = False
        self.ret_calls: set[int] = set()
        self.xwidths: set[int] = set()
        self.xgarbage = False
        self.ss = self.sd = 0          # scalar SSE instructions on floats / doubles
        self.call_sites: set[int] = set()   # direct calls and tail calls, by address
        self.tail_sites: dict[int, int] = {}  # each tail jump to a function start -> that start
        self.exits: set[int] = set()   # jumps out to code no function starts at
        self.ipasses: set[int] = set()   # indirect call sites whose result this function returns
        self.spill_width: dict[int, int] = {}   # argument register -> the width it was spilled with
        self.why: dict = {}            # (--show) the instruction behind each input
        # what this function reads of each direct callee's result: callee -> [rax width, xmm0 width]
        self.rets: dict[int, list[int]] = {}
        # the callees whose result this one returns as its own (a tail call, or `call g; ret`):
        # in rax (passes) and in xmm0 (xpasses), apart. Through one set, a function returning a
        # double of its own on one path and a callee's pointer on another (`p = f(); if (!p) return
        # 1.0;`, FUN_1425aac10) handed its readers' double reads to that callee, and the pointer
        # FUN_14051f170 returns was typed a double (`extraout_RAX`, 2026-10-16)
        self.passes: set[int] = set()
        self.xpasses: set[int] = set()
        self.xret_calls: set[int] = set()   # the callees whose float can be in xmm0 at a ret (XANY)
        self.xany_widths: set[int] = set()
        self.xany_garbage = False
        self.feeds: list = []   # (callee, the calls whose xmm0 it is handed as its first argument)
        self.call_targets: dict[int, int] = {}   # direct call site -> callee
        # direct call site -> the argument positions written since the last call (a bit each):
        # what a caller hands a callee that leaves through a pointer and reads nothing itself
        self.dfresh: dict[int, int] = {}
        self.writes_xmm0 = False   # any instruction of its own writes xmm0
        # the variadic prologue, read off the instructions (in place of tools/varargs.py's reading
        # of the decompiled text): the argument positions spilled to their own home slots, those
        # read back from the slot as values, and the entry offsets whose address is taken
        self.spilled: set[int] = set()
        self.slot_reads: set[int] = set()
        self.va_lea: set[int] = set()
        # the `call __chkstk` sites (ApplyFacts makes them nops in Ghidra's copy)
        self.chk_calls: set[int] = set()
        # the xmm argument positions some packed (whole-vector) arithmetic reads: a 16-byte value
        # in the register, not a float (the SVML-style routines FUN_14240ae40 takes four lanes)
        self.vector_in: set[int] = set()
        self.inputs = [0] * 8          # widest input read per argument register (0 = none)
        self.kinds = [""] * 8          # the first reading instruction's class: "f" float, "d" double
        self.stack = 0                 # stack parameters read (count past the four)
        self.calls: list[tuple[int, tuple[int, ...]]] = []   # (callee, defined widths there)
        self.open = False
        self.error = ""

    def scalar_kind(self) -> int:
        """4 when all the scalar floating point here is on floats, 8 on doubles, else 0."""
        return 4 if self.ss and not self.sd else 8 if self.sd and not self.ss else 0


class Analyzer:
    def __init__(self, b: Binary, starts: set[int], noreturn: set[int], sizes: dict[int, int]):
        self.b = b
        self.starts = starts
        self.noreturn = noreturn
        self.sizes = sizes
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        # the anti-tamper check's jumps over junk (tools/retcheck.py), and the junk's extents
        rc = FACTS / "retcheck.json"
        self.junk_jumps = ({int(j, 16): int(t, 16) for j, t in json.loads(rc.read_text()).get("junk_jumps", {}).items()}
                           if rc.exists() else {})
        # the variadic functions (the previous run's, variadic_fixed): a direct call to one gets a call-site
        # signature like an indirect call, since the decompiler guesses its arguments from the
        # registers set before it and drops some (`FUN_1402b39c0(L, "%s - %s", s)`: 48 stubs,
        # 2026-10-16)
        va = FACTS / "varargs.json"
        self.variadic = {int(k, 16) for k in json.loads(va.read_text())["functions"]} if va.exists() else set()
        # the volatile registers each function leaves alone (tools/preserved.py 'all', which depends
        # on the instructions only): a direct call does not write them, and a read of one after it
        # is still the caller's own argument. Taken for written, FUN_1412942f0's rdx and r8, read
        # after a call that keeps both, were no parameters, and the decompilation read `in_RDX`
        # and `in_R8` (2026-10-18)
        pv = FACTS / "preserved.json"
        self.kept = ({int(k, 16): set(v) for k, v in json.loads(pv.read_text()).get("all", {}).items()}
                     if pv.exists() else {})
        # each check's start -> where the code resumes once it passes: ApplyFacts makes the start
        # a jump there, so the check's crash paths (reading whatever junk registers hold, which
        # made callers look like they read a result: the thunk call in GetSoundScaledVolume) are
        # no code of the function's here either
        self.checks = ({int(s, 16): int(t, 16) for s, t in json.loads(rc.read_text()).get("checks", {}).items()}
                       if rc.exists() else {})
        # the jump tables (tools/switches.py): each indirect jump's cases, exactly
        sw = FACTS / "switches.json"
        self.switches = ({int(j, 16): [int(t, 16) for t in ts] for j, ts in json.loads(sw.read_text())["jumps"].items()}
                         if sw.exists() else {})
        self._guessed_switch: set[int] = set()
        # each import slot -> the API's parameters, [kind, size] ("i" or "f"), by its name
        # (data/imports.json, data/import_protos.json); a folded slot (htons_or_ntohs) by its first
        api_protos = json.loads((ROOT / "data" / "import_protos.json").read_text())["protos"]
        self.import_params: dict[int, list] = {}
        for slot, info in json.loads((ROOT / "data" / "imports.json").read_text())["imports"].items():
            n = info.get("imported_name")
            n = n if n is None or isinstance(n, str) else n[0]
            if n in api_protos:
                self.import_params[int(slot, 16)] = api_protos[n]["params"]
        # __security_check_cookie: `cmp rcx, [rip + __security_cookie]; jne; rol rcx, 0x10`
        self.transparent = set()
        for s in starts:
            if not b.is_code(s):
                continue
            o = b.off(s)
            code = bytes(b.image[o:o + 16])
            if code[:3] == b"\x48\x3b\x0d" and code[7] == 0x75 and code[9:13] == b"\x48\xc1\xc1\x10":
                self.transparent.add(s)
            elif is_noop(code + bytes(b.image[o + 16:o + 64]), s):
                # (and a function that does nothing: spills and a ret, the release build's log)
                self.transparent.add(s)
        self._junk = sorted((j, t) for j, t in self.junk_jumps.items())
        self._junk_starts = [j for j, _t in self._junk]
        # the pieces of each root
        self.pieces: dict[int, list[tuple[int, int]]] = {}
        for s, e, r in zip(b.piece_starts, b.piece_ends, b.piece_root):
            self.pieces.setdefault(r, []).append((s, e))

    def in_junk(self, va: int) -> bool:
        """Whether va lies between a junk jump and its target (the jump itself excluded)."""
        import bisect
        i = bisect.bisect_right(self._junk_starts, va) - 1
        return i >= 0 and self._junk[i][0] < va < self._junk[i][1]

    def decode(self, a: int):
        """{address: insn} for the function at a: a linear sweep of its .pdata pieces, or, for a
        leaf without unwind info, a recursive descent from the entry inside its corpus extent."""
        out = {}
        pieces = self.pieces.get(a)
        if pieces:
            for s, e in pieces:
                o = self.b.off(s)
                for insn in self.md.disasm(bytes(self.b.image[o:o + (e - s)]), s):
                    out[insn.address] = insn
            return out, True
        size = self.sizes.get(a, 0x400)
        end = a + max(size, 1)
        todo = [a]
        while todo:
            va = todo.pop()
            while va not in out and a <= va < end + 0x40:
                o = self.b.off(va)
                insn = next(self.md.disasm(bytes(self.b.image[o:o + 16]), va, 1), None)
                if insn is None:
                    break
                out[va] = insn
                m = insn.mnemonic
                if m in BRANCH_END:
                    break
                if m == "jmp" and va in self.switches:
                    # a jump table (tools/switches.py) in a leaf with no unwind info: its cases are
                    # this function's code too. Missed, the parameters only the cases read were
                    # lost (FUN_1402eadc0 tests dl in four of its cases: `in_DL`, 2026-10-17)
                    todo.extend(t for t in self.switches[va] if a <= t < end + 0x40)
                    break
                if insn.group(X.X86_GRP_JUMP):
                    op = insn.operands[0] if insn.operands else None
                    if op is not None and op.type == X.X86_OP_IMM:
                        t = op.imm
                        if t not in self.starts or t == a:
                            todo.append(t)
                    if m == "jmp":
                        break
                va = insn.address + insn.size
        return out, False

    def analyze(self, a: int) -> Local:
        res = Local()
        try:
            insns, swept = self.decode(a)
        except Exception as e:  # noqa: BLE001
            res.error = f"decode: {e}"
            return res
        if a not in insns:
            res.error = "entry does not decode"
            return res
        order = sorted(insns)
        index = {va: i for i, va in enumerate(order)}
        # successors of each instruction
        succ: dict[int, list[int]] = {}
        switch_from: list[int] = []
        self._known_switch: set[int] = set()
        for i, va in enumerate(order):
            insn = insns[va]
            m = insn.mnemonic
            nxt = order[i + 1] if i + 1 < len(order) and order[i + 1] == va + insn.size else None
            s: list[int] = []
            if va in self.checks and self.checks[va] in insns:
                s.append(self.checks[va])
            elif m in BRANCH_END:
                pass
            elif insn.group(X.X86_GRP_JUMP):
                op = insn.operands[0] if insn.operands else None
                if op is not None and op.type == X.X86_OP_IMM:
                    t = op.imm
                    if t in insns and not (t in self.starts and t != a):
                        s.append(t)
                    elif t not in insns and t not in self.starts:
                        # out of the function to code no function starts at: a leaf's (main
                        # makes it one and decodes this function again)
                        res.exits.add(t)
                    # else: a tail call (handled in the pass)
                elif m == "jmp" and va in self.switches:
                    # a jump table read off the instructions (tools/switches.py): its cases
                    s.extend(t for t in self.switches[va] if t in insns)
                    self._known_switch.add(va)
                elif m == "jmp":
                    switch_from.append(va)
                # (a jump of the anti-tamper check over junk has no fall-through: retcheck.py)
                if m != "jmp" and nxt is not None and va not in self.junk_jumps:
                    s.append(nxt)
            elif insn.group(X.X86_GRP_CALL):
                op = insn.operands[0] if insn.operands else None
                if not (op is not None and op.type == X.X86_OP_IMM and op.imm in self.noreturn) and nxt is not None:
                    s.append(nxt)
            elif nxt is not None:
                s.append(nxt)
            succ[va] = s

        # a switch: the indirect jump's targets are the instructions nothing else reaches
        reached = {a}
        stack = [a]
        while stack:
            for t in succ[stack.pop()]:
                if t not in reached:
                    reached.add(t)
                    stack.append(t)
        # (the junk the check jumps over is no code, and no switch case)
        # (nor the body of a skipped check, what its start fell through to)
        check_code: set[int] = set()
        for s in self.checks.keys() & insns.keys():
            if succ[s] != [self.checks[s]]:
                continue
            stack = [s + insns[s].size]
            while stack:
                va = stack.pop()
                if va in insns and va not in reached and va not in check_code:
                    check_code.add(va)
                    stack.extend(succ[va])
        unreached = [va for va in order if va not in reached and not self.in_junk(va) and va not in check_code]
        # (the indirect jumps that are switches; any other is a tail call through a pointer)
        self._switch_jumps = (set(switch_from) if swept and unreached else set()) | self._known_switch
        self._guessed_switch = set(switch_from) - self._known_switch
        if switch_from:
            if swept and unreached:
                # a switch's cases are the unreached code's entry points: what no other
                # unreached instruction leads to (a block inside a case is entered from the case,
                # with the state the case gives it, not the jump's)
                inner = {t for va in unreached for t in succ[va]}
                cases = [va for va in unreached if va not in inner] or unreached
                for va in switch_from:
                    succ[va] = cases
            else:
                res.open = True

        # __chkstk: `mov eax, <frame>; call __chkstk; sub rsp, rax`. It probes the stack and keeps
        # every register but rax, r10 and r11, so the arguments are still the caller's after it
        self._chk_calls, self._chk_sub = set(), {}
        for i, va in enumerate(order[1:-1], 1):
            insn = insns[va]
            if insn.mnemonic != "call":
                continue
            nxt = insns[order[i + 1]]
            if nxt.mnemonic == "sub" and nxt.op_str == "rsp, rax":
                self._chk_calls.add(va)
                res.chk_calls.add(va)
                prev = insns[order[i - 1]]
                if prev.mnemonic == "mov" and prev.op_str.startswith("eax, ") and prev.operands[1].type == X.X86_OP_IMM:
                    self._chk_sub[nxt.address] = prev.operands[1].imm

        # forward pass: state per instruction = (defined widths x8, frame), the frame being the
        # registers that hold a stack address, as (register, offset from the entry rsp) pairs:
        # rsp itself, and what was set from it (`mov rax, rsp` then `lea rbp, [rax - 0x5f]`, the
        # large-frame prologue, reaches the stack parameters through rbp)
        # (xmm0 at the entry is the first argument, if a float: what a `return x;` path returns)
        state: dict[int, tuple] = {a: (NONE_DEF, tuple(sorted(((X.X86_REG_RSP, 0), (XANY, frozenset((1,)))))))}
        work = [a]
        visits = 0
        while work:
            va = work.pop()
            visits += 1
            if visits > 400000:
                res.error = "did not settle"
                break
            defs, frame = state[va]
            insn = insns[va]
            if not (va in self.checks and succ[va] == [self.checks[va]]):
                # (a skipped check's start is a plain jump: its load does nothing)
                defs, frame = self.step(a, insn, defs, dict(frame), res)
                if any(d >= 8 for d in defs):
                    fd = dict(frame)
                    for fam, d in enumerate(defs):
                        if d >= 8:
                            fd[DIRTY[fam]] = 1
                    frame = tuple(sorted(fd.items()))
            if va in self._guessed_switch and dict(frame).get(X.X86_REG_RSP) == 0:
                # an indirect jump with the stack back at the entry is a tail call (after the
                # epilogue), not a switch: its "cases" got the entry's frame, and their locals
                # were read as stack parameters (FUN_14225b780 took eleven, 2026-10-17)
                continue
            for t in succ[va]:
                old = state.get(t)
                if old is None:
                    state[t] = (defs, frame)
                    work.append(t)
                    continue
                merged = tuple(min(x, y) for x, y in zip(old[0], defs))
                # an offset that disagrees between paths is unknown from there on; the width of
                # rax written since the last call is the widest of the paths'
                mframe = [p for p in old[1] if p in frame and p[0] not in MAX_KEYS and p[0] not in DIRTY]
                od, nd = dict(old[1]), dict(frame)
                mframe += [(k, 1) for k in DIRTY if k in od or k in nd]
                for k in MAX_KEYS:
                    if k == RAXW:
                        w = max(od.get(k, 0), nd.get(k, 0))
                    elif k == OUTSTK:
                        # the outgoing slots written on both paths
                        w = od.get(k, 0) & nd.get(k, 0)
                    else:
                        # an argument register is one written on every path into the call: by
                        # the widest path's, r8 written only on the way past an early test of
                        # FUN_1405a4ae0's was passed to its vtable call, the decompilation
                        # reading `in_R8` (2026-10-16)
                        # (written on both, as wide as the wider: `this` set by `mov rcx, rbx`
                        # on one path and `mov ecx, ...` on another is still the pointer)
                        # (but a path that wrote only a byte or a word of it bounds the argument:
                        # the compiler writes the whole of a parameter on every path, and `mov r8b,
                        # 1` on one path with `xor r8d, r8d` on the other is a bool, the xor only
                        # its zeroing idiom. By the wider, FUN_1419c5290's virtual call took an int
                        # whose upper bytes were what FUN_14022b1b0 left in r8, `extraout_R8`,
                        # 2026-10-18. A 4-byte write zero-extends, so 4 and 8 stay the wider)
                        w = (min(od[k], nd[k]) if min(od[k], nd[k]) < 4 else max(od[k], nd[k])) \
                            if k in od and k in nd else 0
                    if w:
                        mframe.append((k, w))
                # rax as defined on both paths: the narrower, and either path's callee results
                mframe = [p for p in mframe if p[0] not in (RAXMIN, RAXCALLS, XMIN, XANY)]
                if XANY in od and XANY in nd:
                    mframe.append((XANY, od[XANY] | nd[XANY]))
                if RAXMIN in od and RAXMIN in nd:
                    mframe.append((RAXMIN, min(od[RAXMIN], nd[RAXMIN])))
                if XMIN in od and XMIN in nd:
                    # (a scalar width over a whole-register write is the more specific; a float on
                    # one path and a double on the other is unknown)
                    x, y = od[XMIN], nd[XMIN]
                    mframe.append((XMIN, x if x == y else y if x == 16 else x if y == 16 else 16))
                if RAXCALLS in od or RAXCALLS in nd:
                    mframe.append((RAXCALLS, od.get(RAXCALLS, frozenset()) | nd.get(RAXCALLS, frozenset())))
                # the calls whose float result may still be in xmm0: either path's (`if (x) f =
                # a->g(); else f = a->h();` then `movaps xmm6, xmm0`). rax's stays the calls both
                # paths agree on: united, a join's reads of rax typed 6,827 byte results as qwords
                mframe = [p for p in mframe if p[0] != PEND_XMM]
                if PEND_XMM in od or PEND_XMM in nd:
                    mframe.append((PEND_XMM, od.get(PEND_XMM, frozenset()) | nd.get(PEND_XMM, frozenset())))
                mframe = tuple(sorted(mframe))
                if (merged, mframe) != old:
                    state[t] = (merged, mframe)
                    work.append(t)
        return res

    def thunk_target(self, t: int) -> int:
        """Where a lone `jmp rel32` at t goes, when that is a function and t is none: t otherwise."""
        try:
            o = self.b.off(t)
            if self.b.image[o] == 0xE9:
                to = t + 5 + int.from_bytes(bytes(self.b.image[o + 1:o + 5]), "little", signed=True)
                if to in self.starts:
                    return to
        except (IndexError, ValueError):
            pass
        return t

    def step(self, a: int, insn, defs: tuple, frame: dict, res: Local):
        m = insn.mnemonic
        if m == "nop":
            # `nop word ptr [rax + rax]` (alignment, and the junk the clean exe blanked) reads
            # nothing, though its operand names rax: taken for a read, every call before such
            # padding looked like its result was used (thunk_FUN_1412c9f10's, a float)
            return defs, tuple(sorted(frame.items()))
        if "xmm" in insn.op_str and m.lstrip("v").startswith(SCALAR_OPS):
            if m.endswith("ss"):
                res.ss += 1
            elif m.endswith("sd"):
                res.sd += 1
        defs = list(defs)
        ops = insn.operands
        reads, writes = regs_access(insn)
        # (`xor ecx, ecx`, and the AVX form `vxorps xmm0, xmm0, xmm0`)
        zero = (m in ZERO_IDIOMS and len(ops) >= 2 and all(o.type == X.X86_OP_REG for o in ops)
                and len({o.reg for o in ops[1:]}) == 1 and (len(ops) == 3 or ops[0].reg == ops[1].reg))
        # `or r, -1` and `and r, 0` give the same value whatever r held: writes, not reads
        if (m in ("or", "and") and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_IMM
                and ((m == "or" and ops[1].imm in (-1, 0xffffffff, 0xffffffffffffffff)) or (m == "and" and ops[1].imm == 0))):
            zero = True
        # `sbb r, r` is 0 or -1 from the carry flag alone
        if m == "sbb" and len(ops) == 2 and ops[0].type == ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
            zero = True

        def use(fam: int, width: int) -> None:
            res.why.setdefault(fam, f"{insn.address:x} {m} {insn.op_str}")
            if fam >= 4 and res.kinds[fam] in ("", "x"):
                # a whole-register move (`movaps xmm6, xmm2`) says nothing of float or double: a
                # later scalar read does
                res.kinds[fam] = {4: "f", 8: "d"}.get(width, "x")
            res.inputs[fam] = max(res.inputs[fam], width)

        # the home slots (entry + 8 .. + 0x20): a still-incoming argument register stored into its
        # own slot is a spill, not a use, and reading the slot back is the use. A function that
        # only spills (a variadic stub, FUN_141de9b60) reads nothing; a register saved there
        # (MSVC parks rbx and rsi in home slots) and restored is no argument at all
        spill = None
        if m in ("mov", "movsd", "movss", "movq", "movaps", "movups") and len(ops) == 2 \
                and ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG and ops[0].mem.index == 0 \
                and ops[0].mem.base in frame:
            at = frame[ops[0].mem.base] + ops[0].mem.disp
            f = FAMILY.get(ops[1].reg)
            if at in (8, 16, 24, 32) and f is not None and f[0] % 4 == at // 8 - 1 and defs[f[0]] < 8:
                spill = (at, f[0], ops[1].reg)

        for op in ops:
            if op.type != X.X86_OP_MEM or op.mem.base not in frame:
                continue
            at = frame[op.mem.base] + op.mem.disp
            if X.X86_REG_RSP in frame:
                out = at - frame[X.X86_REG_RSP]
                if out >= 0x20 and (m == "lea" or op.access & capstone.CS_AC_READ or op.mem.index != 0):
                    # a slot the function reads back or takes the address of is one of its locals:
                    # the outgoing arguments end below the lowest such slot
                    res.local_floor = min(res.local_floor, out)
                elif 0x20 <= out < 0xa0 and at < 0 and op.access & capstone.CS_AC_WRITE:
                    # a store into the outgoing area, above the callee's home slots. Only below
                    # the entry stack pointer, inside this function's own frame: before the
                    # prologue's `sub rsp`, `mov [rsp + 0x20], r9` is the spill to its own home
                    # slot, and taken for a fifth argument it put `unaff_R15` (what the pushes
                    # left there) into the comparator calls of FUN_1403544f0 (2026-10-16)
                    frame[OUTSTK] = frame.get(OUTSTK, 0) | (1 << ((out - 0x20) // 8))
            if m == "lea" and at >= 0x10 and at % 8 == 0:
                # an argument slot's address (a va_start, or a parameter passed on by reference)
                res.va_lea.add(at)
            if m == "lea" and 8 <= at < 0x28 and -(at & ~7) in frame:
                # the address of a home slot holding its spilled argument (`lea rdx, [rsp + 0x28]`,
                # a va_list, or the slot handed to a callee): whoever reads through it reads the
                # argument, so the register is an input (`local_res20 = in_R9`, then `&local_res20`)
                fam = frame[-(at & ~7)]
                use(fam, 16 if fam >= 4 else res.spill_width.get(fam, 8))
            if m == "lea" or op.mem.index != 0:
                continue
            if op.access & capstone.CS_AC_WRITE and 8 <= at < 0x28:
                # a store into a home slot: it holds the spilled argument, or no longer does.
                # Partly overwritten it still holds the rest (`mov [rsp + 0x10], rdx`, then `mov
                # [rsp + 0x10], ecx` and the slot's address handed on: the callee reads rdx's upper
                # half, a key's second field; taken for gone, FUN_141b38b20 was committed with one
                # parameter and decompiled `in_RDX`, 2026-10-16)
                held = frame.get(-(at & ~7))
                if held is None or op.size >= res.spill_width.get(held, 8):
                    frame.pop(-(at & ~7), None)
                if spill is not None and spill[0] == at:
                    frame[-at] = spill[1]
                    res.spill_width[spill[1]] = max(res.spill_width.get(spill[1], 0), op.size)
                    res.spilled.add(spill[1] % 4)
            # (capstone gives no access at all for some, `test dword ptr [rsp + 0x80], ecx`: a read;
            # missed, the stack parameters FUN_14067a7a0 tests were not its own, `in_stack_00000048`)
            if op.access and not (op.access & capstone.CS_AC_READ):
                continue
            if 8 <= at < 0x28 and -(at & ~7) in frame:
                fam = frame[-(at & ~7)]
                # (read through the frame, not through a pointer to the slots themselves: that is
                # a va_list, `lea rax, [rsp + 0x38]; mov rax, [rax]`, FUN_14025cd90's first
                # variadic argument)
                if not (8 <= frame[op.mem.base] < 0x28):
                    res.slot_reads.add(fam % 4)
                # (as far into the slot as the read reaches: `movzx eax, word ptr [entry + 0xa]`
                # reads bytes 2-3 of the spilled ecx, so the parameter is 4 bytes, not the 2 read;
                # FUN_1414fa300 & co printed `in_register_0000000a` for the rest)
                # (an xmm argument spilled and read back by an integer load, `movsd [rsp + 8], xmm0`
                # then `mov rax, [rsp + 8]`, is as wide as the load: a double's bits, which as the
                # `x` kind's float left the upper half `in_XMM0_Db`, FUN_140267870 & co)
                if fam >= 4:
                    xw = xmm_width(m) if any(o.type == X.X86_OP_REG and o.reg in XMM_ALL for o in ops) else op.size
                    use(fam, xw)
                else:
                    use(fam, max(1, min(8, (at & 7) + op.size)))
            elif at >= 0x28:
                # stack parameters: reads above the home area (`lea` reads nothing: a pointer one
                # past a frame's end lands there too)
                n = (at - 0x28) // 8 + 1
                if n <= 32:
                    if n > res.stack:
                        res.why.setdefault(f"stack{n}", f"{insn.address:x} {m} {insn.op_str} (entry+{at:#x})")
                    res.stack = max(res.stack, n)

        # (unless the source is the same register: `cvtsd2ss xmm0, xmm0` after a call reads the
        # double the callee returned, and skipped as a merge, FUN_14028958c's double was typed by
        # the eax its last callee left, its caller decompiling `extraout_XMM0_Qa`)
        merge_dest = (ops[0].reg if m in MERGE_ONLY and len(ops) == 2 and ops[0].type == X.X86_OP_REG
                      and not (ops[1].type == X.X86_OP_REG and ops[1].reg == ops[0].reg) else None)

        # a direct call's result: how much of rax / xmm0 the caller reads before writing them
        # (`test al, al` after the call: the callee returns a byte)
        # (the calls whose result can be in the register here: a set, so that a read after two
        # paths meet counts for each, `if (x) f = a->g(); else f = a->h();` then a use of xmm0)
        pr, px = frame.get(PEND_RAX), frame.get(PEND_XMM)
        if (pr is not None or px is not None) and not zero:
            for r in reads:
                if r == merge_dest:
                    continue
                if pr is not None and r in RAX_WIDTH:
                    for c in pr:
                        got = res.rets.setdefault(c, [0, 0, 0, 0, 0])
                        got[0] = max(got[0], RAX_WIDTH[r])
                        got[2] = 1      # this caller reads rax (callers are counted below)
                elif px is not None and r in XMM0_REGS and xmm_width(m) in (4, 8):
                    # only a scalar read says float: a whole-register move (`movaps xmm6, xmm0`)
                    # is no evidence (FUN_14057b640, 3,425 callers reading rax, was typed float
                    # by one such move, 2026-10-16)
                    for c in px:
                        got = res.rets.setdefault(c, [0, 0, 0, 0, 0])
                        got[1] = max(got[1], xmm_width(m))
                        got[3] = 1
                elif px is not None and r in XMM0_REGS:
                    # a whole-register move of it: kept, kind unknown (what the callee writes says)
                    for c in px:
                        res.rets.setdefault(c, [0, 0, 0, 0, 0])[4] = 1
        if m == "ret":
            # what rax holds here on every path: `xor al, al; ret` returns a byte, and a path that
            # never wrote it (or left a void callee's leftovers) returns nothing
            if RAXMIN in frame:
                res.written = min(res.written, frame[RAXMIN])
            else:
                res.garbage = True
            res.ret_calls.update(frame.get(RAXCALLS, ()))
            # (an indirect call whose result can reach here is returned on as this function's)
            res.ipasses.update(-c for c in frame.get(RAXCALLS, ()) if c < 0)
            # and xmm0, as this function wrote it (16: a whole-register write, kind unknown), or
            # the float a callee left there (XANY)
            # (XMIN for whether this function writes a float itself at every ret; XANY for what a
            # float its callers read can be: alone, a void virtual ending in a call to a float
            # function was a float, 578 of them)
            if XMIN in frame:
                res.xwidths.add(frame[XMIN])
            else:
                res.xgarbage = True
            if XANY in frame:
                for v in frame[XANY]:
                    if v > 0xffff:
                        res.xret_calls.add(v)
                    else:
                        res.xany_widths.add(v)
            else:
                res.xany_garbage = True
        if m == "ret" and pr is not None:
            for c in pr:
                if c < 0:
                    # an indirect call's result returned as this function's own (`call rax; add
                    # rsp; ret`): that call site returns what this function does (left untyped,
                    # Ghidra named it `extraout_RAX` and merged it into a local)
                    res.ipasses.add(-c)
                else:
                    # the callee's result, still unread, is this function's own (`call g; add
                    # rsp; ret`). Only while rax is: once rax is written the function returns its
                    # own value, and an xmm0 nothing touched is no sign of a float passed on
                    # (ssl::Initialize writes eax after testing cross_ssl's start, and was counted
                    # as returning its result)
                    res.passes.add(c)
        if m == "ret" and frame.get(PEND_XMM_ALL):
            # and the callee whose xmm0 is still as it left it on every path here: its float is
            # this function's (only for this function's xmm0 readers: see Local.xpasses). Not
            # PEND_XMM, any path's call: a float function with a path past a call to SMemFree
            # made the free routine a double (7,207 byte results lost, 2026-10-16)
            res.xpasses.update(c for c in frame[PEND_XMM_ALL] if c > 0)
        if not zero:
            for r in reads:
                f = FAMILY.get(r)
                if r in XMM_ALL and ALIAS - r in frame and r != merge_dest:
                    # a copy of an xmm argument (`movaps xmm6, xmm1`, then `movsd [rsp + 0x70],
                    # xmm6`): the copy's reads say the argument's kind (left `x`, FUN_142580970's
                    # double was a float and its upper half `in_XMM1_Db`), and packed arithmetic
                    # on it says the argument is four lanes
                    fam = frame[ALIAS - r]
                    w = xmm_width(m)
                    if w in (4, 8) and res.kinds[fam] in ("", "x"):
                        res.kinds[fam] = {4: "f", 8: "d"}[w]
                    if packed(m):
                        res.vector_in.add(fam - 4)
                        res.kinds[fam] = "v"
                    continue
                if f is None or r == merge_dest or (spill is not None and r == spill[2]):
                    continue
                fam, width = f
                if fam >= 4:
                    width = xmm_width(m)
                    need = min(width, 8)
                    if defs[fam] < 16 and packed(m):
                        res.vector_in.add(fam - 4)
                        res.kinds[fam] = "v"
                else:
                    need = width
                if defs[fam] < need:
                    use(fam, width)
                    # (an argument read as itself is no variadic one: FUN_141f2cd30 spills xmm3
                    # and takes its slot's address, and compares the float in xmm3 too)
                    res.slot_reads.add(fam % 4)

        # control transfers
        # a call or tail jump through an import slot reads the API's arguments (tools/import_protos.py):
        # as an indirect call it read none, and a wrapper that only hands its registers on came out
        # taking nothing (OsMutexInit, FUN_140262450: `call [InitializeSRWLock]`, so its caller
        # FUN_1401ca520 dropped the lock and the client crashed at launch, 2026-10-17)
        if (insn.group(X.X86_GRP_CALL) or insn.group(X.X86_GRP_JUMP)) and len(ops) == 1 \
                and ops[0].type == X.X86_OP_MEM and ops[0].mem.base == X.X86_REG_RIP:
            api = self.import_params.get(insn.address + insn.size + ops[0].mem.disp)
            if api is not None:
                for pos, (kind, size) in enumerate(api[:4]):
                    fam = pos + 4 if kind == "f" else pos
                    width = min(size, 8)
                    if defs[fam] < width:
                        use(fam, width)
                if m == "jmp" and frame.get(X.X86_REG_RSP) == 0 and len(api) > 4:
                    # a tail jump at the entry's depth hands on the stack arguments too
                    res.stack = max(res.stack, len(api) - 4)
        if insn.group(X.X86_GRP_CALL):
            if ops and ops[0].type == X.X86_OP_IMM and ops[0].imm in self.transparent:
                # __security_check_cookie: called at a function's end with the result already in
                # rax or xmm0, and it keeps both (and every register but rcx). Taken for a call,
                # the float FUN_1402897d8 returns was its "result" (2026-10-16). The decompiler
                # injects it away (cspec callfixup), so it is no call site either
                return tuple(defs), tuple(sorted(frame.items()))
            if insn.address in self._chk_calls:
                for r in (X.X86_REG_RAX, X.X86_REG_R10, X.X86_REG_R11, PEND_RAX, RAXW, RAXMIN, RAXCALLS):
                    frame.pop(r, None)
                # (__chkstk keeps the xmm registers)
                return tuple(defs), tuple(sorted(frame.items()))
            op = ops[0] if ops else None
            if op is not None and op.type == X.X86_OP_IMM:
                # (and above them, the integer positions still untouched since the entry on every
                # path: the caller's own register handed on, an argument where it is a parameter)
                res.dfresh[insn.address] = sum(1 << k for k in range(8) if frame.get(FRESH[k], 0)) | sum(
                    1 << (8 + k) for k in range(4) if defs[k] == 0 and not frame.get(DIRTY[k]))
            if not (op is not None and op.type == X.X86_OP_IMM) or op.imm in self.variadic:
                # an indirect call (or a variadic one): what its arguments can be (written since
                # the last call, or still the function's own incoming registers), for a call-site
                # signature
                fresh = [frame.get(k, 0) for k in FRESH]
                # the register the target is read through is no argument (`mov r9, [rbx]; call
                # [r9 + 0x38]`: the vtable, which Ghidra passed on as a fourth one)
                for r in ((op.mem.base, op.mem.index) if op is not None and op.type == X.X86_OP_MEM else
                          (op.reg,) if op is not None and op.type == X.X86_OP_REG else ()):
                    if r in FAMILY and FAMILY[r][0] < 4:
                        fresh[FAMILY[r][0]] = 0
                # an argument position below one the call is given is an argument as well; one
                # still untouched since the entry on every path is this function's own parameter
                # handed on. IBattleNet's fatal-error forwarder FUN_14147f7f0 is `lea rdx, [ctx];
                # call rax` with the error in rcx as it came in: read as taking nothing, its caller
                # passed nothing, and GlueLogin's handler faulted on a null error (2026-10-20)
                # Only rcx: a register written for the function's own use reads as "given" too
                # (the CRT's atexit walk FUN_140284c30 decodes pointers with a cookie in r8 before
                # `call [guard_dispatch]`, and rdx came out a parameter), and the forwarder of a
                # `this` or a first argument is the case that matters
                top = max((pos for pos in range(4) if fresh[pos] or fresh[4 + pos]), default=-1)
                for pos in range(min(top, 1)):
                    if not fresh[pos] and not fresh[4 + pos] and defs[pos] == 0 and defs[4 + pos] == 0 \
                            and not frame.get(DIRTY[pos]) and not frame.get(DIRTY[4 + pos]):
                        use(pos, 8)
                # (an argument register written on any path in counts as written: see DIRTY)
                seen = tuple(max(d, 8) if frame.get(DIRTY[fam]) else d for fam, d in enumerate(defs))
                res.icalls[insn.address] = (tuple(fresh), seen, frame.get(OUTSTK, 0),
                                            op.imm if op is not None and op.type == X.X86_OP_IMM else 0)
            if op is not None and op.type == X.X86_OP_IMM and frame.get(PEND_XMM):
                # xmm0 still as an earlier call left it, handed to this callee as its first
                # argument (`call [vtbl + 0x430]` then `call FUN_141525b20` reading xmm0): a read of
                # that call's float, if the callee takes one there (resolved after the fixpoint).
                # Not counted, the vtable call was void and the next call read `extraout_XMM0_Da`
                res.feeds.append((op.imm, frame[PEND_XMM]))
            for r in VOLATILE_GPR + [RAXW, OUTSTK, *FRESH] + [ALIAS - x for x in VOLATILE_XMM]:
                frame.pop(r, None)
            frame[RAXMIN] = CALLW
            frame.pop(XMIN, None)
            # (a direct callee's float, or of an indirect one nothing known: as a whole write)
            frame[XANY] = frozenset((op.imm if op is not None and op.type == X.X86_OP_IMM else 16,))
            if op is not None and op.type == X.X86_OP_IMM:
                res.calls.append((op.imm, tuple(defs)))
                res.call_sites.add(insn.address)
                res.call_targets[insn.address] = op.imm
                frame[PEND_RAX] = frame[PEND_XMM] = frame[PEND_XMM_ALL] = frozenset((op.imm,))
                frame[RAXCALLS] = frozenset((op.imm,))
            else:
                # an indirect call's result, by the call's own address (negated: no function's):
                # what the caller reads of it types the call-site signature's return, where
                # `undefined8` left a float read as `extraout_XMM0_Da` (46 stubs, 2026-10-16)
                frame[PEND_RAX] = frame[PEND_XMM] = frame[PEND_XMM_ALL] = frozenset((-insn.address,))
                frame[RAXCALLS] = frozenset((-insn.address,))   # (an indirect one: by its site, negated)
            if op is not None and op.type == X.X86_OP_IMM and op.imm in self.kept:
                kept = self.kept[op.imm]
                return (tuple(defs[i] if POS_NAMES[i] in kept else VOLATILE_DEF[i] for i in range(8)),
                        tuple(sorted(frame.items())))
            return VOLATILE_DEF, tuple(sorted(frame.items()))
        # (a conditional jump to another function is a tail call on its path: `test rcx, rcx; jne
        # FUN_1417e4050` hands that callee its registers, and its `dl` input was never ours)
        if insn.group(X.X86_GRP_JUMP):
            op = ops[0] if ops else None
            if op is not None and op.type == X.X86_OP_IMM:
                t = op.imm
                via_thunk = False
                if t not in self.starts:
                    # (through a lone `jmp` the linker left between them, no function of its own:
                    # FUN_1412942f0's `jne 0x141356c10`, a `jmp FUN_1411e7c70` that takes rdx and r8,
                    # which it hands on unseen, `in_RDX` and `in_R8`, 2026-10-18)
                    t = self.thunk_target(t)
                    via_thunk = t != op.imm
                if t in self.starts and t != a:
                    # a tail call at the entry's stack depth hands on the stack parameters too
                    res.calls.append((t, tuple(defs) + ((1,) if frame.get(X.X86_REG_RSP) == 0 else (0,))))
                    # (tail calls by target: the decompiler shows four `jmp g` from the ends of
                    # one function as one call)
                    res.call_sites.add(-t)
                    # (not a site to make a call of when it goes through a thunk: the jump is to
                    # the thunk, no function, and as a call ApplyFacts made it `func_0x14065f73d()`
                    # with no arguments, where Ghidra had followed it to FUN_1402b3b30(param_1);
                    # 48 functions refused, 2026-10-18)
                    if not via_thunk:
                        res.tail_sites[insn.address] = t
                    # and its result is this function's
                    res.passes.add(t)
                    res.xpasses.add(t)
                    if frame.get(PEND_XMM):
                        # (an earlier call's xmm0 handed on to it: as for a call, FUN_14184faa0's
                        # vtable float jumped on to FUN_141525b20)
                        res.feeds.append((t, frame[PEND_XMM]))
                    res.xret_calls.add(t)
                    res.written = min(res.written, CALLW)
                    res.ret_calls.add(t)
            elif op is not None and op.type != X.X86_OP_IMM and (
                    insn.address not in self._switch_jumps
                    or (insn.address in self._guessed_switch and frame.get(X.X86_REG_RSP) == 0)):
                # an indirect tail call (a vtable's): whatever it returns
                res.written = min(res.written, CALLW)
                res.ret_calls.add(0)
                # and a call site like an indirect call's: the decompiler treats the jump as a
                # call and guesses its arguments as it does any other's. HMAC's update wrapper
                # FUN_1420bd740 measures a string into r8 and `jmp [table + rax*8]`s on; shown as
                # a call of nothing, the length was lost and SHA-256 hashed ~2^64 bytes after the
                # web login (2026-10-20)
                if frame.get(X.X86_REG_RSP) == 0:
                    fresh = [frame.get(k, 0) for k in FRESH]
                    for r in ((op.mem.base, op.mem.index) if op.type == X.X86_OP_MEM else (op.reg,)):
                        if r in FAMILY and FAMILY[r][0] < 4:
                            fresh[FAMILY[r][0]] = 0
                    seen = tuple(max(d, 8) if frame.get(DIRTY[fam]) else d for fam, d in enumerate(defs))
                    res.icalls[insn.address] = (tuple(fresh), seen, frame.get(OUTSTK, 0), 0)

        for r in writes:
            if r in XMM_ALL:
                # what an xmm argument still as it came in flows into whole, lane for lane (a
                # copy, a mask, packed arithmetic: `movaps xmm8, xmm0; andps xmm8, xmm7; mulps
                # xmm5, xmm8`) carries it on (the reads above); any other write ends that
                src = None
                if not zero and (m.lstrip("v") in LANEWISE or packed(m)):
                    for s in reads:
                        if s == r and m.lstrip("v") in WHOLE_MOVES:
                            continue
                        if s in FAMILY and FAMILY[s][0] >= 4 and defs[FAMILY[s][0]] < 16:
                            src = FAMILY[s][0]
                        elif s in XMM_ALL and ALIAS - s in frame:
                            src = frame[ALIAS - s]
                frame.pop(ALIAS - r, None)
                if src is not None:
                    frame[ALIAS - r] = src
            if r in RAX_WIDTH:
                frame.pop(PEND_RAX, None)
                # the last write's width; a byte or word written over a zeroed eax (`xor eax, eax;
                # ... setne al`) is still the int the zeroing made. Encoded width * 2, +1 if zeroed
                w = RAX_WIDTH[r] if r != X.X86_REG_AH else 2
                prev = frame.get(RAXW, 0)
                if m in ("mov", "lea") and len(ops) == 2 and (
                        (ops[1].type == X.X86_OP_REG and ops[1].reg == X.X86_REG_RSP)
                        or (ops[1].type == X.X86_OP_MEM and ops[1].mem.base == X.X86_REG_RSP and m == "lea")):
                    # `mov rax, rsp`, the large-frame prologue: a frame pointer, no result (taken
                    # for one, a static initializer was typed returning its stack pointer and Ghidra
                    # printed `register0x00000020 = (BADSPACEBASE *)...`, 106 stubs, 2026-10-16)
                    frame.pop(RAXW, None)
                elif w >= 4:
                    frame[RAXW] = w * 2 + (1 if zero or (m == "mov" and len(ops) == 2 and ops[1].type == X.X86_OP_IMM
                                                          and ops[1].imm == 0) else 0)
                elif prev & 1:
                    pass
                else:
                    frame[RAXW] = w * 2
                # (a callee's result rewritten is this function's own value now)
                frame.pop(RAXCALLS, None)
                if RAXW in frame:
                    frame[RAXMIN] = frame[RAXW] >> 1
                else:
                    frame.pop(RAXMIN, None)
            elif r in XMM0_REGS:
                res.writes_xmm0 = True
                frame.pop(PEND_XMM, None)
                frame.pop(PEND_XMM_ALL, None)
                frame[XMIN] = (VECTOR if m.lstrip("v") in LANE_BUILD else
                               {"cvtss2sd": 8, "cvtsd2ss": 4}.get(m.lstrip("v"), xmm_width(m))
                               if r == X.X86_REG_XMM0 else 16)
                frame[XANY] = frozenset((frame[XMIN],))
            f = FAMILY.get(r)
            if f is None:
                continue
            fam, width = f
            # (an xmm argument's kind is in the writing instruction: movss a float, movsd a double)
            # (as wide as written: a 32-bit write is a 4-byte argument; called with 8, Ghidra fills
            # the upper half with a register it lost track of, `CONCAT44(in_register_14, x)`)
            fw = xmm_width(m) if fam >= 4 else width
            # the last write's width (`lea rcx, [rax + rax*4]` for an index, then `mov ecx, [x]`
            # for the argument: 4); a byte or word written over a wider value keeps the wider
            frame[FRESH[fam]] = fw if fw >= 4 else max(frame.get(FRESH[fam], 0), fw)
            if fam >= 4 and m.lstrip("v") in WHOLE_LOADS and len(ops) >= 2 and ops[-1].type == X.X86_OP_MEM:
                # sixteen bytes loaded whole: a struct copied through the register (`movups xmm2,
                # [rsp + 0x20]` then a store), no argument. A float from memory is a movss, and
                # Win64 passes no vector by value. Taken for one, 63 call sites through a vtable
                # passed `in_XMM2_Da` (2026-10-16)
                frame.pop(FRESH[fam], None)
            if fam >= 4:
                defs[fam] = 16
            elif width >= 4:
                defs[fam] = 8
            elif r in (X.X86_REG_CH, X.X86_REG_DH):
                pass
            else:
                defs[fam] = max(defs[fam], width)

        # the stack pointer, and the registers that hold an address in the frame
        rsp = frame.get(X.X86_REG_RSP)
        dest = GPR64.get(ops[0].reg) if ops and ops[0].type == X.X86_OP_REG else None
        if m == "push":
            if rsp is not None:
                frame[X.X86_REG_RSP] = rsp - 8
        elif m == "pop":
            if rsp is not None:
                frame[X.X86_REG_RSP] = rsp + 8
            if dest is not None:
                frame.pop(dest, None)
        elif dest is not None:
            src = ops[1] if len(ops) > 1 else None
            new = None
            if m in ("sub", "add") and dest in frame and src is not None:
                if src.type == X.X86_OP_IMM:
                    new = frame[dest] - src.imm if m == "sub" else frame[dest] + src.imm
                elif dest == X.X86_REG_RSP and insn.address in self._chk_sub:
                    new = frame[dest] - self._chk_sub[insn.address]
            elif m == "mov" and src is not None and src.type == X.X86_OP_REG and ops[0].size == 8:
                new = frame.get(GPR64.get(src.reg))
            elif m == "lea" and src is not None and src.type == X.X86_OP_MEM and src.mem.index == 0 \
                    and ops[0].size == 8 and src.mem.base in frame:
                new = frame[src.mem.base] + src.mem.disp
            if new is None:
                frame.pop(dest, None)
            else:
                frame[dest] = new
        # anything else that writes a frame register makes it something else
        for r in writes:
            p = GPR64.get(r)
            if p is not None and p != dest and p != X.X86_REG_RSP and p in frame:
                frame.pop(p)
        return tuple(defs), tuple(sorted(frame.items()))


_A: Analyzer | None = None
# ncalls_of's entry for a function not analysed: (direct call sites, tail sites, exits, indirect
# passes, xmm0 passes, (spilled, slot reads, slot addresses, stack params), chkstk calls, vector
# inputs)
NC0 = (0, [], [], [], [], ([], [], [], 0), [], [], [], ([], True), [], [], True, [])


def variadic_fixed(va_info) -> int | None:
    """The fixed parameter count of a variadic function, from its prologue: the address of the
    first variadic slot taken (MSVC's va_start), with every register slot from there through r9
    spilled to its home and none of them read back as a value; or, past four fixed parameters,
    the address of a stack slot above every stack parameter read. A parameter passed on by
    reference has its slot's address taken too, but is read as itself (`local_res20 == 0`,
    FUN_1417eec30), which tools/varargs.py could not see from the text and marked variadic."""
    spilled, slot_reads, va_lea, stack = va_info
    spilled, slot_reads = set(spilled), set(slot_reads)
    for at in sorted(va_lea):
        k = at // 8                      # the slot's position, 1-based
        if k < 2:
            continue
        if k <= 4:
            pos = set(range(k - 1, 4))
            if stack:
                # (a stack parameter read by name is no variadic function's: past its fixed
                # parameters it reads only through the va_list. The DB2 callbacks at
                # 0x140476740.. spill and take their parameters' slots and read a fifth)
                continue
            # (two or more register slots spilled through r9 is the va_start prologue itself, even
            # where the first variadic argument is then read from its register, FUN_14025cd90's
            # `*(int *)rdx = 0`; one, the last, may be a parameter passed on by reference, and is
            # variadic only if nothing reads it as itself, FUN_141f2cd30's float in xmm3)
            if pos <= spilled and not pos & slot_reads:
                return k - 1
            if pos <= spilled and len(pos) >= 2:
                # (read as values too: variadic only if a caller passes fewer, main checks that)
                return -(k - 1)
        elif stack <= k - 5 and k <= 9:
            # (at most eight fixed: an offset far above the entry is a frame the tracking lost
            # through a large frame's `lea rbp, [rsp - 0x1000]`, and took FUN_1413ca460 for a
            # function with 249 fixed parameters)
            return k - 1
    return None


def _init(starts, noreturn, sizes):
    global _A
    _A = Analyzer(Binary(), starts, noreturn, sizes)


def _work(chunk: list[int]):
    out = []
    for a in chunk:
        r = _A.analyze(a)
        out.append((a, r.inputs, r.kinds, r.stack, r.calls, r.open, r.error, r.rets, sorted(r.passes),
                    (r.written, r.garbage, sorted(r.ret_calls), sorted(r.xwidths),
                     r.xgarbage and not r.inputs[4], r.scalar_kind()), r.icalls,
                    r.local_floor, (len(r.call_sites), sorted(r.tail_sites.items()), sorted(r.exits), sorted(r.ipasses),
                                    sorted(r.xpasses), (sorted(r.spilled), sorted(r.slot_reads), sorted(r.va_lea), r.stack),
                                    sorted(r.chk_calls), sorted(r.vector_in), sorted(r.xret_calls),
                                    (sorted(r.xany_widths), r.xany_garbage), r.feeds, sorted(r.call_targets.items()),
                                    r.writes_xmm0 or bool(r.icalls) or 0 in r.ret_calls or r.open,
                                    sorted(r.dfresh.items()))))
        if SHOW:
            print(f"  {a:x} why {r.why}")
    return out


SHOW = False


def solve(local: dict[int, tuple]) -> tuple[dict, dict, dict]:
    """The interprocedural fixpoint: inputs[f] |= inputs[g] where f calls g with that register
    possibly unwritten, with g's float or double kind; and stack[f] |= stack[g] where f tail-calls
    g at its entry's stack depth. Widths and counts only grow, so it settles.
    Returns (inputs, kinds, stack) per function."""
    inputs = {a: list(v[0]) for a, v in local.items()}
    kinds = {a: list(v[1]) for a, v in local.items()}
    stack = {a: v[2] for a, v in local.items()}
    callers: dict[int, list[int]] = {}
    for a, v in local.items():
        for g, _defs in v[3]:
            callers.setdefault(g, []).append(a)
    work = list(local)
    on = set(work)
    while work:
        f = work.pop()
        on.discard(f)
        changed = False
        mine, mykinds = inputs[f], kinds[f]
        for g, defs in local[f][3]:
            gi = inputs.get(g)
            if gi is None:
                continue
            for fam in range(8):
                w = gi[fam]
                # a Win64 position is one register or the other: an xmm read at a position the
                # callee takes as an integer is its own scratch (memcpy's vector moves), not an
                # argument it passes on
                if fam >= 4 and gi[fam - 4]:
                    continue
                if w and defs[fam] < min(w, 8) and mine[fam] < w:
                    mine[fam] = w
                    if fam >= 4 and mykinds[fam] in ("", "x"):
                        mykinds[fam] = kinds[g][fam]
                    changed = True
            if len(defs) > 8 and defs[8] and stack[g] > stack[f]:
                stack[f] = stack[g]
                changed = True
        if changed:
            for c in callers.get(f, ()):
                if c not in on:
                    on.add(c)
                    work.append(c)
    return inputs, kinds, stack


def params_of(inputs: list[int], kinds: list[str], stack: int) -> list[list]:
    """[kind, size] per position: "i" integer, "f" float, "d" double, "s" a stack slot, and
    "-" for a position below the last used that nothing reads (a register is still there)."""
    out = []
    for pos in range(4):
        gi, xi = inputs[pos], inputs[4 + pos]
        if xi and not gi:
            k = kinds[4 + pos] or "x"
            if k == "x" and xi == 8:
                # read as 8 bytes (`movq rax, xmm0`, its bits taken whole): no float, which left
                # the upper half `in_XMM0_Db` (FUN_141493530)
                k = "d"
            out.append([k, 4 if k in ("f", "x") else 16 if k == "v" else 8])
        elif gi:
            out.append(["i", gi])
        else:
            out.append(["-", 8])
    out += [["s", 8]] * stack
    while out and out[-1][0] == "-":
        out.pop()
    return out


def corpus_sizes() -> dict[int, int]:
    sizes = {}
    if CORPUS.exists():
        with CORPUS.open(encoding="utf-8") as f:
            for line in f:
                head = line[:120]
                i = head.find('"addr":"')
                j = head.find('"size":')
                if i < 0 or j < 0:
                    continue
                a = int(head[i + 8:head.index('"', i + 8)], 16)
                k = j + 7
                e = k
                while e < len(head) and head[e].isdigit():
                    e += 1
                sizes[a] = int(head[k:e] or 0)
    return sizes


def code_pointers(b: Binary) -> set[int]:
    """Every code address an absolute pointer outside .text holds, by the relocation table."""
    import pefile
    from wowbin import EXE
    pe = pefile.PE(str(EXE), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    out = set()
    for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for e in blk.entries:
            if e.type != 10:    # IMAGE_REL_BASED_DIR64
                continue
            va = base + e.rva
            if b.is_code(va):
                continue        # (an operand inside an instruction is no table entry)
            t = b.u64(va)
            if b.is_code(t):
                out.add(t)
    pe.close()
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=max(1, min(12, (os.cpu_count() or 4) - 2)))
    ap.add_argument("--show", nargs="*", default=[])
    ap.add_argument("--trace", nargs="+", metavar="ADDR FAMILY",
                    help="pairs: follow an input (0-3 rcx..r9, 4-7 xmm0-3) down the callees that pass it")
    ap.add_argument("--why-ret", nargs="+", default=[], metavar="ADDR",
                    help="which callers' reads (and results passed on) typed a function's return")
    ap.add_argument("--mismatch", metavar="FILE",
                    help="callers (hex, one per line): each callee whose result they read in a kind "
                         "or width its committed return does not give")
    args = ap.parse_args()
    t0 = time.time()
    b = Binary()
    sizes = corpus_sizes()
    starts = set(b.func_starts) | set(sizes)
    nr = json.loads((FACTS / "noreturn.json").read_text())["functions"]
    noreturn = {int(k, 16) for k in nr}
    funcs = sorted(starts)
    if args.show:
        global SHOW
        SHOW = True
        args.jobs = 1
        funcs = [int(x, 16) for x in args.show]
    # (the code bytes, for the thunks below, and the pieces, for the leaves: the rest of the image
    # is not needed again)
    text_start, text = b.bytes_of(".text")
    piece_spans = sorted(zip(b.piece_starts, b.piece_ends))
    # (and every code address the image points to, for the leaves reached only so, below)
    pointed = code_pointers(b) | {t for _i, t in b.riprel_refs(keep=b.is_code)}
    del b
    chunks = [funcs[i:i + 400] for i in range(0, len(funcs), 400)]
    local = {}
    reads_of: dict[int, dict] = {}     # caller -> {callee: [rax width, xmm0 width]}
    passes_of: dict[int, list] = {}    # f -> the callees whose result f returns
    written_of: dict[int, int] = {}    # f -> the widest rax it writes itself before a ret
    icalls_of: dict[int, dict] = {}    # f -> {indirect call address: (fresh, defs, outgoing slots)}
    floor_of: dict[int, int] = {}      # f -> where its locals start above the outgoing area
    ncalls_of: dict[int, int] = {}     # f -> its direct call sites (tail calls too), checks skipped
    if args.jobs > 1 and len(chunks) > 1:
        with Pool(args.jobs, initializer=_init, initargs=(starts, noreturn, sizes)) as pool:
            for n, part in enumerate(pool.imap_unordered(_work, chunks), 1):
                for a, inputs, kinds, stack, calls, open_, err, got, passes, wr, ic, lf, nc in part:
                    local[a] = (inputs, kinds, stack, calls, open_, err)
                    reads_of[a], passes_of[a], written_of[a], icalls_of[a], floor_of[a], ncalls_of[a] = got, passes, wr, ic, lf, nc
                if n % 25 == 0:
                    print(f"  {n * 400}/{len(funcs)} decoded, {time.time() - t0:.0f}s", file=sys.stderr, flush=True)
    else:
        _init(starts, noreturn, sizes)
        for chunk in chunks:
            for a, inputs, kinds, stack, calls, open_, err, got, passes, wr, ic, lf, nc in _work(chunk):
                local[a] = (inputs, kinds, stack, calls, open_, err)
                reads_of[a], passes_of[a], written_of[a], icalls_of[a], floor_of[a], ncalls_of[a] = got, passes, wr, ic, lf, nc
    print(f"decoded {len(local)} functions in {time.time() - t0:.0f}s", file=sys.stderr, flush=True)

    # leaves nothing made a function: a direct call target with no .pdata entry (a leaf needs no
    # unwind info) that Ghidra did not create either, so it has no decompilation and no facts
    # (`movzx eax, byte ptr [rip + x]; ret` at 0x140309cb0: its callers read a result typed by
    # nothing). Decoded here, and created by ApplyFacts. Not inside a piece or a known function,
    # and not a thunk (those resolve to their target below)
    import bisect
    piece_lo = [s for s, _e in piece_spans]
    known = sorted(sizes.items())
    known_lo = [s for s, _z in known]

    def inside_known(g: int) -> bool:
        i = bisect.bisect_right(piece_lo, g) - 1
        if i >= 0 and g < piece_spans[i][1]:
            return True
        j = bisect.bisect_right(known_lo, g) - 1
        return j >= 0 and g < known[j][0] + max(known[j][1], 1)

    leaves = set()
    jumpers = set()   # functions that jump out to a leaf: decoded again once it is one
    for f, v in list(local.items()):
        exits = ncalls_of.get(f, NC0)[2]
        for g in [g for g, _d in v[3]] + list(exits):
            o = g - text_start
            if g in local or not (0 <= o < len(text) - 2) or text[o] == 0xE9 or text[o:o + 2] == b"\xff\x25":
                continue
            if not inside_known(g):
                leaves.add(g)
                if g in exits:
                    jumpers.add(f)
    # and the code nothing calls directly but something points to: a function's address stored in
    # data (the relocation table lists every absolute pointer in the image: vtable slots, callback
    # tables) or taken in code (`lea rdx, [rip + fn]`, a callback handed to a registration). A leaf
    # reached only so has no .pdata entry and no caller, so nothing made it a function: 3,370 of
    # them in game code had no decompilation and no translation, the hand-ported hotfix listeners,
    # loading callbacks and DB2 sort helpers among them, still stubs with nothing behind them
    # (2026-10-19). Not in a native or third-party range (run as the reference's own code), not the
    # exception funclets at the image's end, and decoding as code (a thunk resolves as below)
    excluded_ranges = [(int(r["from"], 16), int(r["to"], 16)) for key in ("native", "third_party")
                       for r in json.loads((ROOT / "tools/fidelity_exceptions.json").read_text(encoding="utf-8")).get(key, [])]
    added = 0
    pointed_leaves = set()
    for g in pointed:
        o = g - text_start
        if g in local or g in leaves or not (0 <= o < len(text) - 2) or text[o] == 0xE9 or text[o:o + 2] == b"\xff\x25":
            continue
        if g >= 0x1425d4000 or any(lo <= g < hi for lo, hi in excluded_ranges) or inside_known(g):
            continue
        if text[o] == 0xCC or text[o:o + 2] == b"\x00\x00":
            continue
        leaves.add(g)
        pointed_leaves.add(g)
        added += 1
    print(f"{added} leaves reached only through a pointer", file=sys.stderr, flush=True)
    if leaves and not args.show:
        # (to a fixed point: a leaf's own calls and tail jumps reach code nothing else does. The
        # hotfix listener behind 0x14022992a's `jmp 0x141359fd0` was a stub with nothing behind it,
        # 2026-10-19)
        todo = sorted(leaves | jumpers)
        while todo:
            _init(starts | leaves, noreturn, sizes)
            more, entries, rejump = set(), set(), set()
            for a, inputs, kinds, stack, calls, open_, err, got, passes, wr, ic, lf, nc in _work(todo):
                local[a] = (inputs, kinds, stack, calls, open_, err)
                reads_of[a], passes_of[a], written_of[a], icalls_of[a], floor_of[a], ncalls_of[a] = got, passes, wr, ic, lf, nc
                for g in [g for g, _d in calls] + list(nc[2]):
                    o = g - text_start
                    if g in local or g in leaves or not (0 <= o < len(text) - 2) or text[o] == 0xE9 \
                            or text[o:o + 2] == b"\xff\x25" or text[o] == 0xCC or inside_known(g):
                        continue
                    more.add(g)
                    if a in pointed_leaves:
                        # (an entry as surely as the pointed one that reaches it: ApplyFacts made
                        # 0x140229920 first, its body took the code it jumps to, and the
                        # containing check then skipped 0x141359fd0. A junk call's target is not)
                        entries.add(g)
                    if g in nc[2]:
                        jumpers.add(a)
                        rejump.add(a)
            leaves |= more
            pointed_leaves |= entries
            # (and a function that jumps to a leaf found only now, decoded again with it a start:
            # decoded before, the jump was a branch inside it, not a tail call, and 0x1418d7b10's
            # `jne 0x1418820a0` passed on no xmm1, so its float parameter was lost and the function
            # stubbed, 2026-10-19)
            todo = sorted(more | rejump)
        print(f"decoded {len(leaves)} leaves no function covered", file=sys.stderr, flush=True)

    if args.show:
        # the callees' summaries are needed for the fixpoint: --show is local only
        for a in funcs:
            inputs, kinds, stack, calls, open_, err = local[a]
            print(f"{hexa(a)} local {params_of(inputs, kinds, stack)} open={open_} {err} "
                  f"calls={[(hexa(g), d) for g, d in calls][:8]}")
        return

    # the variadic functions, from their prologues (variadic_fixed). The local pass read the
    # previous run's set, for the call sites to them: where the set changed, their callers are
    # decoded again with the new one
    old_va = {int(k, 16): v for k, v in json.loads((FACTS / "varargs.json").read_text())["functions"].items()} \
        if (FACTS / "varargs.json").exists() else {}
    new_va = {}
    # (what each callee's direct callers leave unset among the argument registers)
    unset_by_some: dict[int, set[int]] = {}
    for f, v in local.items():
        for g, d in v[3]:
            unset_by_some.setdefault(g, set()).update(p for p in range(4) if d[p] == 0 and d[4 + p] == 0)
    for a in local:
        k = variadic_fixed(ncalls_of.get(a, NC0)[5])
        if k is not None and k < 0:
            # its variadic registers read as values as well as spilled (FUN_14025cd90 reads the
            # first variadic argument from rdx): variadic when some caller passes fewer than all,
            # not a function with every parameter's address taken (the 1,612 generated functions
            # that spill and take their parameters' slots, all set by every caller)
            k = -k
            if not unset_by_some.get(a, set()) & set(range(k, 4)):
                k = None
        if k is not None:
            new_va[a] = k
    changed_va = {a for a in old_va.keys() | new_va.keys() if old_va.get(a) != new_va.get(a)}
    write_facts("varargs", {
        "rule": "variadic: the first variadic slot's address taken (va_start), every register slot from "
                "there through r9 spilled to its home and none read back as a value; or, past four fixed "
                "parameters, a stack slot's address above every stack parameter read (tools/protos.py)",
        "functions": {hexa(a): k for a, k in sorted(new_va.items())}})
    print(f"variadic: {len(new_va)} ({len(new_va.keys() - old_va.keys())} new, "
          f"{len(old_va.keys() - new_va.keys())} no longer, "
          f"{sum(1 for a in new_va.keys() & old_va.keys() if new_va[a] != old_va[a])} fixed count changed)",
          file=sys.stderr, flush=True)
    if changed_va:
        again = {f for f, v in local.items() if any(g in changed_va for g, _d in v[3])}
        _init(starts | leaves, noreturn, sizes)
        for a, inputs, kinds, stack, calls, open_, err, got, passes, wr, ic, lf, nc in _work(sorted(again)):
            local[a] = (inputs, kinds, stack, calls, open_, err)
            reads_of[a], passes_of[a], written_of[a], icalls_of[a], floor_of[a], ncalls_of[a] = got, passes, wr, ic, lf, nc
        print(f"decoded again {len(again)} callers of the functions whose variadic status changed",
              file=sys.stderr, flush=True)

    # a variadic function's prologue spills its variadic registers to their home slots (the
    # va_list walks on from there): those are no inputs of its own, and its callers pass what they
    # pass. Only the fixed parameters (variadic_fixed above) count, or they reach every caller
    va = json.loads((FACTS / "varargs.json").read_text())["functions"] if (FACTS / "varargs.json").exists() else {}
    for k, fixed in va.items():
        v = local.get(int(k, 16))
        if v is None:
            continue
        inputs = list(v[0])
        for pos in range(fixed, 4):
            inputs[pos] = inputs[4 + pos] = 0
        local[int(k, 16)] = (inputs, v[1], 0, v[3], v[4], v[5])
    # a call to a thunk (one `jmp rel32`, no function of its own) hands its target the caller's
    # registers: for the inputs, the call is the target's. Through the thunk unseen, a function
    # passing rdx and r8 on to `thunk_FUN_14176d990` did not take them (`in_RDX`, `in_R8B`)
    thunk_to: dict[int, int] = {}
    for g in {g for v in local.values() for g, _d in v[3]} - set(local):
        o = g - text_start
        if 0 <= o < len(text) - 5 and text[o] == 0xE9:
            t = g + 5 + int.from_bytes(text[o + 1:o + 5], "little", signed=True)
            if t in local:
                thunk_to[g] = t
    solved, solved_kinds, solved_stack = solve({a: (v[0], v[1], v[2], [(thunk_to.get(g, g), d) for g, d in v[3]])
                                                for a, v in local.items()})
    if args.trace:
        # which callee chain hands a function a register input it does not read itself
        _init(starts, noreturn, sizes)
        for i in range(0, len(args.trace) - 1, 2):
            a, fam = int(args.trace[i], 16), int(args.trace[i + 1])
            seen = []
            while a not in seen:
                seen.append(a)
                inputs, kinds, stack, calls, open_, err = local[a]
                if inputs[fam]:
                    print(f"{' -> '.join(hexa(x) for x in seen)}: read at {_A.analyze(a).why.get(fam)}")
                    break
                nxt = [g for g, d in calls if g in solved and solved[g][fam] and d[fam] < min(solved[g][fam], 8)
                       and not (fam >= 4 and solved[g][fam - 4])]
                if not nxt:
                    print(f"{' -> '.join(hexa(x) for x in seen)}: no read found")
                    break
                a = nxt[0]
        return
    # thunks: a direct call target that is one `jmp rel32` and no function of its own (no .pdata
    # entry, nothing to unwind) returns what its target does. Left out, its callers' reads never
    # reached the target: GetSoundScaledVolume keeps the float FUN_1412c9f10 returns through
    # thunk_FUN_1412c9f10 (0x14130c1f0), and the target, untyped, left `extraout_XMM0_Da`
    for g in {g for v in local.values() for g, _d in v[3]} - set(local):
        o = g - text_start
        if 0 <= o < len(text) - 5 and text[o] == 0xE9:
            t = g + 5 + int.from_bytes(text[o + 1:o + 5], "little", signed=True)
            if t in local:
                passes_of[g] = [t]
                written_of[g] = (CALLW, False, [t], [], True, 0)
    # functions that do nothing: home-slot spills and a ret (the release build's log calls,
    # FUN_140255c60, a varargs `void Log(fmt, ...) {}`), for ApplyFacts to give a no-op call
    # fixup. A call to one leaves every register as it was, and its callers read rax across it
    # (`if (Log(...) != 0)` is the result of the call before): as a call, the decompiler took rax
    # for clobbered and printed `extraout_EAX` (2026-10-17)
    noops = [a for a in local if 0 <= a - text_start < len(text) and is_noop(text[a - text_start:a - text_start + 64], a)]
    noop_set = set(noops)
    # the functions xmm0 comes out of as it went in: none of their own instructions writes it,
    # they call nothing indirectly, and every callee is one of them. The whole-program build
    # keeps a value in xmm0 across a call to one (FUN_140315680 across FUN_140317e20), which Win64
    # as Ghidra knows it calls clobbered: `auVar29 = extraout_XMM0_00`. ApplyFacts gives these a
    # calling convention with xmm0 unaffected (2026-10-17)
    keep_xmm0 = {a for a in local if not ncalls_of.get(a, NC0)[12]}
    changed = True
    while changed:
        changed = False
        for a in list(keep_xmm0):
            if any(thunk_to.get(g, g) not in keep_xmm0 and thunk_to.get(g, g) not in noop_set
                   for g, _d in local[a][3]):
                keep_xmm0.discard(a)
                changed = True
    print(f"{len(keep_xmm0)} functions keep xmm0", file=sys.stderr, flush=True)
    del text
    # results: the widest read of each function's rax / xmm0 over its direct call sites, and a
    # function that returns a callee's result passes its own readers' widths on to that callee
    # per callee: [widest rax read, widest scalar xmm0 read, callers reading rax, callers reading xmm0]
    # (an earlier call's xmm0 handed on as a callee's float first argument: a read of it)
    for caller, nc in ncalls_of.items():
        for g, pend in nc[10]:
            g = thunk_to.get(g, g)
            if g in solved and solved[g][4] and not solved[g][0] and solved_kinds[g][4] in ("f", "d"):
                w = 4 if solved_kinds[g][4] == "f" else 8
                for c in pend:
                    got = reads_of.setdefault(caller, {}).setdefault(c, [0, 0, 0, 0, 0])
                    got[1] = max(got[1], w)
                    got[3] = 1
    ret = {}
    for caller, got in reads_of.items():
        for g, (wi, wx, ni, nx, kx) in got.items():
            cur = ret.setdefault(g, [0, 0, 0, 0, 0])
            cur[0], cur[1] = max(cur[0], wi), max(cur[1], wx)
            cur[2] += ni
            cur[3] += nx
            cur[4] += kx
    # (a pass in rax hands on the rax reads, one in xmm0 the xmm0 reads: Local.xpasses)
    xpasses_of = {f: nc[4] for f, nc in ncalls_of.items() if nc[4]}
    for g, p in passes_of.items():
        if g not in local:
            xpasses_of[g] = p     # (a thunk: both)
    changed = True
    while changed:
        changed = False
        for f, passes in passes_of.items():
            if f not in ret:
                continue
            for g in passes:
                cur = ret.setdefault(g, [0, 0, 0, 0, 0])
                new = [max(cur[0], ret[f][0]), cur[1], max(cur[2], ret[f][2]), cur[3], cur[4]]
                if new != cur:
                    ret[g] = new
                    changed = True
        for f, passes in xpasses_of.items():
            if f not in ret:
                continue
            for g in passes:
                cur = ret.setdefault(g, [0, 0, 0, 0, 0])
                new = [cur[0], max(cur[1], ret[f][1]), cur[2], max(cur[3], ret[f][3]), max(cur[4], ret[f][4])]
                if new != cur:
                    ret[g] = new
                    changed = True
    if args.why_ret:
        # where a function's result width came from: the callers reading it, and the callers
        # that return its result on to theirs
        for x in args.why_ret:
            g = int(x, 16)
            print(f"{hexa(g)} ret={ret.get(g)} written={written_of.get(g)} passes={[hexa(p) for p in passes_of.get(g, ())]}")
            for caller, got in reads_of.items():
                if g in got and any(got[g][2:]):
                    print(f"   read by {hexa(caller)}: {got[g]}")
            for f, passes in passes_of.items():
                if g in passes:
                    print(f"   returned on by {hexa(f)}: ret={ret.get(f)}")
        return

    # the functions some direct call or tail call reaches
    called = {g for v in local.values() for g, _d in v[3]}
    # (and what a thunk jumps to is called by the thunk's callers)
    called |= {p[0] for g, p in passes_of.items() if g not in local}
    # the callee side, for a function no caller's read types (a vtable's, or one whose callers
    # drop the result): it returns a value only if rax holds one at every ret, written by itself
    # or the result of a callee that returns one, and as wide as the narrowest of those. Typed by
    # its widest write on any path instead, void virtuals that happened to touch rax returned
    # what their last callee left there, and a comparator returning `xor al, al` on one path and
    # using eax as a loop index on another an int (`extraout_RAX`, `in_EAX & 0xffffff00`: ~300
    # stubs, 2026-10-16)
    # (only results in rax: a float result leaves rax garbage, and a static initializer storing
    # one, `_DAT_143217570 = FUN_140254020(...)`, was typed returning the callee's eax)
    value: dict[int, int] = {}
    floats = set()
    # (and nothing from a function that writes rax on no path and calls nothing: the varargs log
    # stub FUN_140255c60 leaves its caller's rax alone, so the "reads" of its callers are of what
    # they had there before; as a value, the void virtuals ending in a log call were typed
    # returning one and decompiled `return extraout_AL`, 12 stubs, 2026-10-16)
    nothing = {f for f, (wmin, garbage, calls, _xw, _xg, _k) in written_of.items()
               if garbage and wmin == 1 << 30 and not calls}
    for a, r in ret.items():
        if r[1] and r[3] > r[2]:
            floats.add(a)
            continue
        if a in called and r[0] and a not in nothing:
            value[a] = r[0]

    def float_site(f: int, c: int) -> bool:
        # an indirect call whose result the caller reads only from xmm0: its rax is garbage
        # (FUN_14176d990 stores the float a vtable call returns and was typed returning rax)
        got = reads_of.get(f, {}).get(c)
        return bool(got) and bool(got[3] or got[4]) and not got[2]
    written: dict[int, int] = {}
    changed = True
    while changed:
        changed = False
        for f, (wmin, garbage, calls, _xw, _xg, _k) in written_of.items():
            # (nor one its callers read a float from: typed by the rax it also writes, the static
            # initializer storing FUN_140254020's float took that for a byte result of its own)
            if f in value or f in floats or garbage or wmin == 1 << 30:
                continue
            if not all(c <= 0 or c in value for c in calls):
                continue
            if any(c < 0 and float_site(f, c) for c in calls):
                continue
            ws = [value[c] for c in calls if c > 0] + ([8] if any(c <= 0 for c in calls) else [])
            if wmin < CALLW:
                ws.append(wmin)
            if not ws:
                continue
            value[f] = written[f] = min(ws)
            changed = True
    # xmm0 written as one scalar kind at every ret (a `xorps xmm0, xmm0` return of 0.0 beside
    # `movss` ones is still the float)
    # and through tail calls (a path ending in `jmp g` returns g's float: FUN_1412c9f10 returns
    # 0.0 itself or FUN_1412c9780's), to a fixpoint
    xret: dict[int, int] = {}
    changed = True
    while changed:
        changed = False
        for f, (_w, _g, calls, xw, xg, kind) in written_of.items():
            if f in xret:
                continue
            nc = ncalls_of.get(f, NC0)
            rf = ret.get(f)
            if rf and (rf[3] or rf[4]):
                # its callers read xmm0: what can be there at a ret, its own writes or the floats
                # of the callees that left it (CRT sinh returns its own or its error handler's)
                xw, xg = nc[9][0], nc[9][1]
                xcalls = nc[8]
                # (1: xmm0 as it came in, the first argument: its kind, if it is a float)
                if 1 in xw:
                    k0 = {"f": 4, "d": 8}.get(solved_kinds.get(f, [""] * 8)[4]) if solved.get(f, [0] * 8)[4] else None
                    xw = [w for w in xw if w != 1] + ([k0] if k0 else [])
            else:
                # nothing says it returns a float: only if it writes one itself at every ret, or
                # tail-calls a function that does (the callees whose xmm0 reaches a ret, not
                # those whose rax does: FUN_1402897d8 writes its float after its last call)
                xcalls = [t for _at, t in nc[1]]
            if os.environ.get("PROTOS_DEBUG_X") == hexa(f):
                print(f"xret {hexa(f)}: r={rf} xw={xw} xg={xg} xcalls={[hexa(c) for c in xcalls]} "
                      f"kind={kind} known={[xret.get(c) for c in xcalls]}", file=sys.stderr)
            if xg:
                continue
            scalar = (set(xw) | {xret[c] for c in xcalls if c in xret}) - {16}
            if not scalar and 16 in xw and rf and (rf[3] or rf[4]):
                # written whole from another register only (`movaps xmm0, xmm12`), what it holds
                # copied from a callee's result earlier: that callee, one whose rax reaches the
                # ret as well (FUN_142040120's sqrt)
                scalar = {xret[c] for c in calls if c in xret} - {16}
            if not scalar and 16 in xw and kind:
                # written whole only (`movaps xmm0, xmm6; ret`): the kind of the scalar
                # arithmetic it does, when that is all one kind (12 stubs read such a result
                # as `extraout_XMM0_Da`, 2026-10-16)
                scalar = {kind}
            if len(scalar) == 1:
                xret[f] = scalar.pop()
                changed = True

    out = {}
    tail_jumps: dict[str, str] = {}
    errors = 0
    for a, (inputs, kinds, stack, calls, open_, err) in sorted(local.items()):
        if err:
            errors += 1
            continue
        e = {"params": params_of(solved[a], solved_kinds[a], solved_stack[a])}
        nc, tails = ncalls_of.get(a, NC0)[:2]
        # (less the calls to functions that do nothing, which the decompiler no longer shows)
        nc -= sum(1 for _at, t in ncalls_of.get(a, NC0)[11] if t in noop_set)
        if nc:
            # its direct call sites, the anti-tamper checks left out: what the translator checks a
            # decompilation's calls against
            e["calls"] = nc
        for at, t in tails:
            tail_jumps[hexa(at)] = hexa(t)
        r = ret.get(a)
        wmin, wgarbage, wcalls, _xw, _xg, _k = written_of.get(a, (1 << 30, False, [], [], True, 0))
        if r and wgarbage and wmin == 1 << 30 and not wcalls:
            # rax is written on no path to any ret, and no callee's result reaches one: nothing is
            # returned, whatever the callers seem to read (a varargs spill stub, FUN_140255c60, read
            # as `return in_EAX`). Their "reads" are what they left in rax before the call: the
            # whole-program build lets a caller count on a callee that leaves rax alone
            r = [0, r[1], 0, r[3], r[4]]
        # (only where no caller reads rax, and either rax holds no value of its own or callers do
        # read xmm0: a function returning an int in eax can leave lanes built up in xmm0 behind)
        if xret.get(a) == VECTOR and not (r and r[2]) and (not written.get(a) or (r and (r[3] or r[4]))):
            # its result put together lane by lane (a sincos): the vector, whatever lane a caller
            # reads first
            e["ret"] = ["v", 16]
        elif r and r[1] and r[3] > r[2]:
            # more callers read xmm0 as a scalar than read rax: a floating-point result. Typed an
            # integer by what the function writes to rax, its callers' xmm0 reads came out as
            # `extraout_XMM0_Da` (378 stubs, 2026-10-16); by the widest read instead of the most
            # callers, a pointer-returning function 3,425 callers use was typed float
            e["ret"] = [{4: "f", 8: "d"}.get(r[1], "x"), min(r[1], 8)]
        elif r and r[4] > r[2] and not _k and _xw and set(_xw) <= {16} and not _xg:
            # xmm0 written whole at every ret, by a load and nothing that says what it holds
            # (`movups xmm0, [rdx + rax + 0x60]; ret`, and `movaps xmm0, [default]; ret` on the
            # other path, FUN_142471d10), no scalar arithmetic saying it is a float, and more
            # callers keep xmm0 whole than read rax: the 16 bytes it returns. Typed by the pointer
            # rax still held, its callers read `extraout_XMM0_Da` to `_Dd` (2026-10-18). (Callers
            # reading it as a scalar are no evidence: a float function ends `movaps xmm0, xmm6`)
            e["ret"] = ["v", 16]
        elif a in called and r and r[0]:
            # as wide as its direct callers read it. A narrower write than that (a byte-returning
            # body handed on to callers that read rax whole) is `CONCAT71(in_register_00000001,
            # x)` in the callee, leftovers the translator zeroes; typed by the write instead, every
            # wider read was `extraout_var` in a caller (432 stubs)
            e["ret"] = ["i", r[0]]
        elif xret.get(a) and r and r[4] and not r[2]:
            # its callers keep xmm0 whole (`movaps xmm6, xmm0`, used later as a float) and none
            # reads rax, and it writes xmm0 as a scalar at every ret: the float it returns
            # (FUN_140ae9880's `(double)extraout_XMM0_Da`, 38 stubs, 2026-10-16)
            e["ret"] = ["v", 16] if xret[a] == VECTOR else [{4: "f", 8: "d"}[xret[a]], xret[a]]
        elif (written.get(a) and wmin == CALLW and xret.get(a) in (4, 8) and not (r and r[2])
              and xret[a] in _xw and a not in called):
            # (only for what no direct call reaches, and an xmm0 it wrote itself: a callee's
            # float handed on is no evidence, and a direct caller's read after two paths join is
            # not counted, so FUN_14124a320's int, read so by FUN_14124b080, would be a float)
            # rax holds only what callees left there, and xmm0 a scalar this function wrote
            # itself at every ret: the float (`movsd xmm0, [rsp + 0x40]; ret` after a call that
            # returns a pointer, FUN_14028958c, was typed by the call's eax)
            e["ret"] = [{4: "f", 8: "d"}[xret[a]], xret[a]]
        elif (r and (r[3] or r[4]) and not r[2] and wmin == 1 << 30 and not wcalls
              and e["params"] and e["params"][0][0] in ("f", "d")):
            # rax written on no path and no callee's result in it, its callers reading xmm0 and none
            # rax, and its first argument a float in xmm0: the float it hands back, written on one
            # path and passed through on the other (FUN_1401a4880 negates it when positive and
            # returns it as it came otherwise; untyped, its caller read `extraout_XMM0_Da`)
            e["ret"] = list(e["params"][0])
        elif written.get(a):
            # only a vtable or a pointer reaches it: what it leaves in rax itself (`xor al, al;
            # ret` returns a byte, `xor eax, eax; ... setne al` an int)
            e["ret"] = ["i", written[a]]
        elif xret.get(a) and not (r and r[2]):
            # nothing typed it and rax is garbage at some ret, but xmm0 holds a scalar at each:
            # a float virtual (`movss xmm0, [rcx + 8]; ret`)
            e["ret"] = ["v", 16] if xret[a] == VECTOR else [{4: "f", 8: "d"}[xret[a]], xret[a]]
        # xmm0 as a parameter of no known kind that is also the result's register, of a known
        # kind (`andps xmm0, mask; ret`, a fabs, its result read as a double): the result's kind
        # (as a float it left the upper half `in_XMM0_Db`, FUN_142030b00)
        if e["params"] and e["params"][0][0] == "x" and e.get("ret", ["i"])[0] in ("f", "d"):
            e["params"][0] = list(e["ret"])
        if open_:
            e["open"] = True
        out[hexa(a)] = e
    # an open function (it leaves through a pointer, `mov rax, [Curl_ssl]; jmp [rax + 0x48]`)
    # reads no argument itself, so its count from the instructions is only what it touches: 0
    # for a wrapper that hands everything on. Its callers say what it takes: the integer
    # positions every direct call to it writes first, at least. Read as taking nothing, curl's
    # Curl_ssl_data_pending FUN_1421784c0 was called with no connection and faulted, and the
    # web login stalled after its form (2026-10-20)
    by_target: dict[int, list[int]] = {}
    for f, nc in ncalls_of.items():
        targets = dict(nc[11])
        own = len(out.get(hexa(f), {}).get("params", ()))
        for site, bits in (nc[13] if len(nc) > 13 else ()):
            t = targets.get(site)
            if t is not None and out.get(hexa(t), {}).get("open"):
                passed = bits & 0xff | sum(1 << k for k in range(min(own, 4)) if bits >> (8 + k) & 1)
                by_target.setdefault(t, []).append(passed)
    raised = 0
    for t, all_bits in by_target.items():
        e = out[hexa(t)]
        n = min(next((k for k in range(4) if not bits >> k & 1), 4) for bits in all_bits)
        if n > len(e["params"]) and not any(p[0] in ("f", "d", "x") for p in e["params"]):
            e["params"] = e["params"] + [["i", 8]] * (n - len(e["params"]))
            raised += 1
    print(f"{raised} open functions take what their callers pass", file=sys.stderr)
    # a function whose result is only ever what the functions it tail-jumps to return, writing
    # neither rax nor xmm0 itself (a CPU dispatch, `cmp [level], 2; jge FUN_14240e0c0; jmp
    # FUN_14240d750`): what they return, when they agree. Typed by "what it leaves in rax", the
    # dispatcher of two 16-byte vector routines returned an int, and its caller, which reads all
    # four lanes, `extraout_XMM0_Da` to `_Dd` (FUN_1415b98c0, 2026-10-18). To a fixed point: a
    # dispatcher may jump to another
    changed = True
    while changed:
        changed = False
        for a in local:
            wmin, wgarbage, wcalls, xw, _xg, _k = written_of.get(a, (1 << 30, False, [], [], True, 0))
            e = out.get(hexa(a))
            if e is None or wmin != CALLW or wgarbage or not wcalls or xw or 0 in wcalls:
                continue
            rets = {tuple(out[hexa(t)]["ret"]) if hexa(t) in out and "ret" in out[hexa(t)] else None for t in wcalls}
            # (an xmm result only: an integer one is typed by its callers' reads, as above)
            if len(rets) == 1 and None not in rets and next(iter(rets))[0] in ("f", "d", "v"):
                new = list(next(iter(rets)))
                if e.get("ret") != new:
                    e["ret"] = new
                    changed = True
    if args.mismatch:
        import collections
        kinds = collections.Counter()
        for line in open(args.mismatch):
            f = int(line.strip(), 16)
            for g, (wi, wx, ni, nx, kx) in reads_of.get(f, {}).items():
                if g < 0:
                    continue    # (an indirect call's result: callsites.json's rets)
                e = out.get(hexa(g))
                r = e.get("ret") if e else None
                why = None
                if e is None:
                    why = "callee not analysed"
                elif (nx or kx) and not (r and r[0] in "fdx"):
                    why = f"reads xmm0 ({'scalar' if nx else 'whole'}), ret {r}"
                elif ni and not r:
                    why = f"reads rax {wi}, ret void"
                elif ni and r and r[0] in "fdx":
                    why = f"reads rax {wi}, ret float"
                elif ni and r and r[1] < wi:
                    why = f"reads rax {wi}, ret {r[1]}"
                if why:
                    kinds[why.split(",")[0] + "," + why.split(",")[1][:9] if "," in why else why] += 1
                    print(f"{hexa(f)} -> {hexa(g)}: {why}  ret={ret.get(g)} wr={written_of.get(g)}")
                elif os.environ.get("PROTOS_ALL_READS"):
                    print(f"{hexa(f)} -> {hexa(g)}: ok reads={(wi, wx, ni, nx, kx)} ret {r}")
        for k, n in kinds.most_common():
            print(n, k, file=sys.stderr)
        return
    path = write_facts("protos", {
        "rule": "Win64 inputs read before written on some path from the entry (must-define over "
                "rcx/rdx/r8/r9/xmm0-3 by width), stack reads above [entry rsp + 0x28], closed over "
                "direct calls and tail calls",
        # call targets no function covered, for ApplyFacts to create
        "leaves": [hexa(g) for g in sorted(leaves)],
        # those of them a pointer reaches (a relocation or a `lea`): an entry point however Ghidra
        # drew the function bodies around it, so ApplyFacts creates one inside another's body too
        "pointed": [hexa(g) for g in sorted(pointed_leaves)],
        # each jump from one function to another's start (a tail call), for ApplyFacts to mark
        "tail_jumps": tail_jumps,
        # each `call __chkstk` of a large frame (`mov eax, <size>; call __chkstk; sub rsp, rax`),
        # for ApplyFacts to make a nop in Ghidra's copy: the decompiler took the call's rax for
        # killed, lost the stack pointer at the `sub` and printed `register0x00000020 =
        # (BADSPACEBASE *)...`, `unaff_XMM6_Dc` for the xmm saves and the return address stored
        # over a local (`uStack_20 = 0x1413f2bcb`), 2026-10-16. The probe touches no memory the
        # function reads, and keeps every register but r10 and r11
        "chkstk_calls": sorted(hexa(c) for nc in ncalls_of.values() for c in nc[6]),
        # functions that do nothing at all (spills and a ret), for a no-op call fixup
        "noops": [hexa(a) for a in sorted(noops)],
        # functions that leave xmm0 as it was, for a calling convention that says so
        "keep_xmm0": [hexa(a) for a in sorted(keep_xmm0)],
        "functions": out})
    print(f"{len(out)} functions, {errors} not analysed, "
          f"{sum(1 for e in out.values() if e.get('open'))} open -> {path} ({time.time() - t0:.0f}s)",
          file=sys.stderr)

    # indirect calls: the decompiler guesses each one's arguments from the registers it sees set
    # and drops what it cannot tie to the call (cross_ssl's start lost the logger in r8 at its
    # factory Initialize call, 0x141f2ee98). A position is an argument when the caller wrote it
    # since its last call, or when it is still one of the caller's own parameters untouched (a
    # `this` handed on); the stores into the outgoing area are the stack arguments. Passing one
    # too many costs nothing on x64; dropping one is a wrong call
    sites, site_rets = {}, {}
    for f, ic in icalls_of.items():
        own = solved.get(f)
        if own is None or local[f][5]:
            continue
        for at, (fresh, defs, outstk, callee) in ic.items():
            args = []
            for pos in range(4):
                gw, xw = fresh[pos], fresh[4 + pos]
                if gw and not xw:
                    args.append(["i", gw])
                elif xw and not gw:
                    args.append([{4: "f", 8: "d"}.get(xw, "x"), 4 if xw != 8 else 8])
                elif gw and xw:
                    # both written since the last call: the later write is not known here; an
                    # integer is the safe reading (a float argument in rcx would be lost the same)
                    args.append(["i", gw])
                elif defs[pos] < 8 and own[pos]:
                    # the caller's own parameter handed on, as wide as the caller has it
                    args.append(["i", min(8, own[pos])])
                elif defs[4 + pos] < 8 and own[4 + pos] and not own[pos]:
                    args.append([solved_kinds[f][4 + pos] or "x", 8 if solved_kinds[f][4 + pos] == "d" else 4])
                else:
                    args.append(["-", 8])
            # the stack arguments: the run of outgoing slots written since the last call, from
            # the first, below the function's lowest local (a slot read back or whose address is
            # taken is a local, and locals sit right above the outgoing area)
            limit = max(0, (floor_of.get(f, 1 << 30) - 0x20) // 8)
            n = 0
            while n < limit and outstk >> n & 1:
                n += 1
            if n:
                args += [["-", 8]] * (4 - len(args)) + [["s", 8]] * n
            while args and args[-1][0] == "-":
                args.pop()
            if callee:
                # a variadic call: at least the callee's fixed parameters, whatever was seen set
                fixed = va.get(hexa(callee), 0)
                args += [["i", 8]] * (fixed - len(args))
                args = [["i", 8] if k < fixed and x[0] == "-" else x for k, x in enumerate(args)]
            sites[hexa(at)] = args
            # what the caller reads of the result: [rax width, scalar xmm0 width, reads rax,
            # reads xmm0 as a scalar, keeps xmm0 whole]
            got = reads_of.get(f, {}).get(-at)
            if got:
                wi, wx, ni, nx, kx = got
                if (nx or kx) and not ni:
                    site_rets[hexa(at)] = [{4: "f", 8: "d"}.get(wx, "f"), wx if wx in (4, 8) else 4]
                elif ni:
                    site_rets[hexa(at)] = ["i", wi]
            if hexa(at) not in site_rets and at in ncalls_of.get(f, NC0)[3]:
                # returned on as this function's result: as the function returns it
                own_ret = out.get(hexa(f), {}).get("ret")
                if own_ret:
                    site_rets[hexa(at)] = own_ret
    # the sites that are jumps (`jmp [rax + 0x48]`, ff /4), not calls (ff /2): the decompiler
    # applies a call-site signature only to a call, so ApplyFacts makes each a call that returns
    code_start, code = Binary().bytes_of(".text")

    def is_jump(at: int) -> bool:
        o = at - code_start
        k = o + (1 if 0x40 <= code[o] <= 0x4f else 0)
        return code[k] == 0xFF and (code[k + 1] >> 3) & 7 == 4
    jumps = sorted(k for k in sites if is_jump(int(k, 16)))
    path = write_facts("callsites", {
        "rule": "indirect calls: argument positions written since the caller's previous call, or "
                "still its own untouched parameters, and the outgoing stack stores since then",
        "calls": sites,
        "jumps": jumps,
        # the result each indirect call's caller reads (float, double or an integer width)
        "rets": site_rets})
    print(f"{len(sites)} indirect call sites -> {path}", file=sys.stderr)


if __name__ == "__main__":
    main()
