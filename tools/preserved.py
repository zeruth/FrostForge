"""The volatile integer registers each function leaves as it found them: build/facts/preserved.json.

The reference is a whole-program (LTCG) build, so the compiler knows exactly which registers a
callee writes, and keeps a value in a volatile register across a call to one that leaves it
alone. Under Win64's __fastcall, which Ghidra assumes, rcx, rdx and r8-r11 die at every call, so
such a value comes out of the call as `extraout_R8` (FUN_1405ef2d0 hands r8 on past
FUN_1405ee730) or flows in as `in_R9D`: a register "the decompiler lost track of", and the
function stubbed (298 functions read one, 2026-10-18). tools/protos.py's keep_xmm0 is the same
fact for xmm0.

For every corpus function, the instructions reachable from its entry are decoded (following
branches, not crossing into another function's entry), and every register an instruction writes,
explicitly or implicitly (`mul` rdx, `rep movs` rcx), is noted. A call or tail jump adds the
callee's writes, to a fixed point; a switch follows its cases (tools/switches.py, or every
instruction of the function where the table could not be read); an indirect call, a tail call
through a register, a call to an import, or code that does not decode counts as writing all. What is left are the registers it preserves.
ApplyFacts.java gives each function a calling convention with those registers unaffected.

    python tools/preserved.py   -> build/facts/preserved.json
"""
from __future__ import annotations

import struct
import sys
import time
from collections import Counter

import capstone
from capstone import x86 as X

from corpus import load_corpus, load_facts
from wowbin import Binary, hexa, regs_access, write_facts

# (and xmm1-xmm5: FUN_1405f81a0 leaves xmm2 alone and its caller FUN_140622410 keeps a float there
# across the call, `fVar13 = extraout_XMM2_Da`; 40 lost-register stubs read one, 2026-10-18.
# xmm0 is tools/protos.py's keep_xmm0, beside the return value)
# (and rax, which a function returning a float or nothing need not write: FUN_1401a3c00 is
# `movss xmm0, [rcx+0xdc]; ret`, and its caller FUN_1401a3c90 loads a pointer into rax before the
# call and reads through it after, `*extraout_RAX`; FUN_1405e5490 keeps al across a float getter.
# A function whose result is in rax writes it, so only those that leave it can keep it, 2026-10-18)
VOLATILE = ("RAX", "RCX", "RDX", "R8", "R9", "R10", "R11", "XMM1", "XMM2", "XMM3", "XMM4", "XMM5")
FAMILY = {}
for full, names in {"RAX": ("rax", "eax", "ax", "al", "ah"),
                    "RCX": ("rcx", "ecx", "cx", "cl", "ch"), "RDX": ("rdx", "edx", "dx", "dl", "dh"),
                    **{f"R{n}": (f"r{n}", f"r{n}d", f"r{n}w", f"r{n}b") for n in (8, 9, 10, 11)},
                    **{f"XMM{n}": (f"xmm{n}", f"ymm{n}", f"zmm{n}") for n in range(1, 6)}}.items():
    for n in names:
        FAMILY[n] = full
ALL = frozenset(VOLATILE)
LIMIT = 20000   # instructions per function before it counts as writing everything


def pieces_by_root(b: Binary) -> dict[int, list[tuple[int, int]]]:
    out: dict[int, list[tuple[int, int]]] = {}
    for s, e, r in zip(b.piece_starts, b.piece_ends, b.piece_root):
        out.setdefault(r, []).append((s, e))
    return out


def sweep(b: Binary, md, pieces) -> tuple[set[str], set[int], bool]:
    """A linear decode of a function's .pdata pieces: what they write and call. For a jump table
    tools/switches.py could not read (its bound check out of a linear walk's reach), where the
    flow cannot follow: every instruction of the function counts."""
    writes, callees = set(), set()
    for s, e in pieces:
        o = b.off(s)
        for ins in md.disasm(bytes(b.image[o:o + (e - s)]), s):
            for r in regs_access(ins)[1]:
                fam = FAMILY.get(ins.reg_name(r))
                if fam:
                    writes.add(fam)
            if ins.mnemonic in ("call", "jmp") and ins.operands:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    if ins.mnemonic == "call" or not any(ps <= op.imm < pe for ps, pe in pieces):
                        callees.add(op.imm)
                elif ins.mnemonic == "call" or op.type == X.X86_OP_MEM:
                    return set(ALL), callees, True
    return writes, callees, False


def table_jump(prev, ins) -> bool:
    """`add rcx, rsi; jmp rcx`: a jump through an image-relative table, inside the function (a
    tail call through a register loads it whole, `mov rax, [..]; pop rbx; jmp rax`)."""
    return (prev is not None and prev.mnemonic == "add" and len(prev.operands) == 2
            and prev.operands[0].type == X.X86_OP_REG and prev.operands[1].type == X.X86_OP_REG
            and ins.operands[0].type == X.X86_OP_REG and prev.operands[0].reg == ins.operands[0].reg)


def load_switches() -> dict[int, list[int]]:
    return {int(k, 16): [int(t, 16) for t in v] for k, v in load_facts("switches").get("jumps", {}).items()}


def local_facts(b: Binary, starts: set[int]):
    """Per function: (registers its own instructions write, direct callees, writes everything?)."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    text = b.text
    switches = load_switches()
    roots = pieces_by_root(b)
    out = {}
    for f in sorted(starts):
        writes, callees, everything = set(), set(), False
        seen, todo, n = set(), [f], 0
        while todo and not everything:
            a = todo.pop()
            prev = None
            while a not in seen:
                if not (text.start <= a < text.end):
                    everything = True
                    break
                seen.add(a)
                o = b.off(a)
                ins = next(md.disasm(bytes(b.image[o:o + 16]), a), None)
                n += 1
                if ins is None or n > LIMIT:
                    everything = True
                    break
                for r in regs_access(ins)[1]:
                    fam = FAMILY.get(ins.reg_name(r))
                    if fam:
                        writes.add(fam)
                mn = ins.mnemonic
                op = ins.operands[0] if ins.operands else None
                if mn == "call":
                    if op is not None and op.type == X.X86_OP_IMM:
                        callees.add(op.imm)
                    else:
                        everything = True
                        break
                    a = ins.address + ins.size
                elif mn == "jmp":
                    if op is not None and op.type == X.X86_OP_IMM:
                        if op.imm in starts and op.imm != f:
                            callees.add(op.imm)     # a tail call
                            break
                        a = op.imm
                    elif ins.address in switches:
                        # a switch (tools/switches.py): each case (FUN_1402bfa60, Lua's mark
                        # routine, writes none of r8-r11, and its callers keep values there)
                        todo.extend(switches[ins.address])
                        break
                    elif table_jump(prev, ins) and b.func_containing(a) is not None:
                        w, c, every = sweep(b, md, roots[b.func_containing(a)])
                        writes |= w
                        callees |= c
                        everything = everything or every
                        break
                    else:
                        everything = True           # a tail call through a register or memory
                        break
                elif mn.startswith("j") or mn.startswith("loop"):
                    if op is not None and op.type == X.X86_OP_IMM:
                        todo.append(op.imm)
                    a = ins.address + ins.size
                elif mn in ("ret", "retf", "int3", "ud2", "hlt"):
                    break
                else:
                    a = ins.address + ins.size
                prev = ins
        out[f] =(frozenset(ALL if everything else writes), frozenset(callees), everything)
    return out


# (and xmm0, which a caller can keep across a call too: tools/protos.py's keep_xmm0 says which
# functions leave it, and the liveness here which callers rely on that)
BIT = {r: 1 << i for i, r in enumerate(VOLATILE + ("XMM0",))}
XMM0_NAMES = {"xmm0", "ymm0", "zmm0"}
NARROW = {"al", "ah", "ax", "cl", "ch", "cx", "dl", "dh", "dx", *(f"r{n}{s}" for n in (8, 9, 10, 11) for s in ("b", "w"))}
ARGS = BIT["RCX"] | BIT["RDX"] | BIT["R8"] | BIT["R9"] | BIT["XMM0"] | BIT["XMM1"] | BIT["XMM2"] | BIT["XMM3"]
EVERY = sum(BIT.values())
# (and the low byte of each integer register on its own: `mov dl, 1; call FUN_14179f260`, whose
# second parameter is a byte, reads nothing of rdx above dl. With a byte write no kill and the
# call reading all of rdx, the rdx a call before left looked relied on, and FUN_1418555d0's four
# `extraout_RDX` stayed refused, 2026-10-18. A byte write kills the low bit, a byte parameter or a
# byte read reads only it; for 'needed' and 'relied' both bits are the register)
LOW_SHIFT = len(BIT)
LOW = {r: BIT[r] << LOW_SHIFT for r in ("RAX", "RCX", "RDX", "R8", "R9", "R10", "R11")}
BYTE_NAMES = {"al": "RAX", "cl": "RCX", "dl": "RDX", "r8b": "R8", "r9b": "R9", "r10b": "R10", "r11b": "R11"}
WORD_NAMES = {"ax": "RAX", "cx": "RCX", "dx": "RDX", "r8w": "R8", "r9w": "R9", "r10w": "R10", "r11w": "R11"}
EVERY_LOW = sum(LOW.values())


def low_of(m: int) -> int:
    """The low-byte bits of the registers in m."""
    return sum(LOW[r] for r in LOW if m & BIT[r])


def fold(m: int) -> int:
    """The registers m holds anything of: a live low byte is the register live."""
    return (m & EVERY) | ((m >> LOW_SHIFT) & EVERY)


def arg_masks() -> tuple[dict[int, int], dict[int, int]]:
    """The argument registers each function reads (tools/protos.py's parameters, by position:
    an integer in rcx/rdx/r8/r9, a float or double in xmm0-3), and each indirect call passes
    (callsites.json): what a call reads of the caller's registers. Taken as all four, a register
    the callee never reads was live into every call, and a caller looked as if it relied on what
    the call before left there."""
    gpr, xmm = ("RCX", "RDX", "R8", "R9"), ("XMM0", "XMM1", "XMM2", "XMM3")

    def mask_of(params):
        m = 0
        for k, p in enumerate(params[:4]):
            if p[0] == "i" and len(p) > 1 and p[1] == 1:
                m |= LOW[gpr[k]]
            elif p[0] == "i":
                m |= BIT[gpr[k]]
            elif p[0] in ("f", "d", "x"):
                m |= BIT[xmm[k]]
        return m

    funcs = {int(k, 16): mask_of(v.get("params", [])) for k, v in load_facts("protos").get("functions", {}).items()}
    sites = {int(k, 16): mask_of(v) for k, v in load_facts("callsites").get("calls", {}).items()}
    return funcs, sites


def needed(b: Binary, starts: set[int], keep: dict[int, set[str]], thunks: dict[int, int] | None = None):
    """Of what each function keeps, what some caller holds a live value in across a call to it.

    The keep conventions are one per distinct set, and the program holds only so many names (see
    ApplyFacts.applyKeepConventions): the full sets made 355 kinds, 285 of them did not fit, and
    5,564 functions fell back to a narrower convention than they have, FUN_14158cf10 kept r10 and
    r11 but not the r9 its callers rely on (44 `extraout_R9` stubs, 2026-10-18). A register no
    caller keeps live across the call need not be kept by the convention at all: decompiled
    either way, the variable after the call is the same. A backward liveness over each caller's
    instructions gives the registers live after each direct call; a function whose flow could not
    be followed counts all of its callees' kept registers as live."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    switches = load_switches()
    mask = {f: sum(BIT[r] for r in regs) for f, regs in keep.items()}
    callee_reads, site_reads = arg_masks()
    need: dict[int, int] = {}
    # (and per caller, per callee: the registers the callee writes that the caller still reads
    # after the call. Only those are values the caller takes from the callee's leftovers; any
    # other register the decompiler shows read after a call is its own merge of paths, and the
    # translator may give it any value, tools/translate.py leftovers)
    relied: dict[int, dict[int, int]] = {}
    followed: list[int] = []
    text = b.text
    for f in sorted(starts):
        insns: dict[int, tuple] = {}
        todo, n, open_flow = [f], 0, False
        while todo and not open_flow:
            a = todo.pop()
            last = None
            while a not in insns:
                if not (text.start <= a < text.end):
                    open_flow = True
                    break
                o = b.off(a)
                ins = next(md.disasm(bytes(b.image[o:o + 16]), a), None)
                n += 1
                if ins is None or n > LIMIT:
                    open_flow = True
                    break
                # (the instruction before, in this straight run: a table jump's `add`)
                prev, last = last, ins
                reads, writes = regs_access(ins)
                use = dfn = 0
                for r in reads:
                    name = ins.reg_name(r)
                    fam = "XMM0" if name in XMM0_NAMES else FAMILY.get(name)
                    if name in BYTE_NAMES:
                        use |= LOW[BYTE_NAMES[name]]
                    elif fam:
                        use |= BIT[fam]
                for r in writes:
                    name = ins.reg_name(r)
                    fam = "XMM0" if name in XMM0_NAMES else FAMILY.get(name)
                    # (a byte or word write merges into the register: no kill, but of its low byte)
                    if name in BYTE_NAMES:
                        dfn |= LOW[BYTE_NAMES[name]]
                    elif name in WORD_NAMES:
                        dfn |= LOW[WORD_NAMES[name]]
                    elif fam and name not in NARROW:
                        dfn |= BIT[fam] | LOW.get(fam, 0)
                dfn &= ~use
                mn = ins.mnemonic
                # (a register zeroed with itself, `xor edx, edx`, reads nothing: taken for a read,
                # it made the register live wherever it was cleared, and 31,887 callers seemed to
                # rely on what a call left there)
                if (mn in ("xor", "sub", "pxor", "xorps", "xorpd", "vpxor", "vxorps", "vxorpd", "psubb",
                           "psubd", "psubq")
                        and len(ins.operands) >= 2 and ins.operands[0].type == X.X86_OP_REG
                        and all(o.type == X.X86_OP_REG and o.reg == ins.operands[0].reg for o in ins.operands[1:])):
                    name = ins.reg_name(ins.operands[0].reg)
                    fam = "XMM0" if name in XMM0_NAMES else FAMILY.get(name)
                    if name in BYTE_NAMES:
                        use, dfn = 0, LOW[BYTE_NAMES[name]]
                    elif name in WORD_NAMES:
                        use, dfn = 0, LOW[WORD_NAMES[name]]
                    else:
                        use, dfn = 0, ((BIT[fam] | LOW.get(fam, 0)) if fam and name not in NARROW else 0)
                op = ins.operands[0] if ins.operands else None
                nxt = ins.address + ins.size
                if mn == "call":
                    callee = op.imm if op is not None and op.type == X.X86_OP_IMM else None
                    # (through a jump thunk: the function it lands on, see jump_thunks; resolved
                    # here as well, as the map holds only the thunks local_facts saw, and it stops
                    # decoding a function at its first indirect call: FUN_1402e5190's thunks past
                    # one kept r8 looking relied, 2026-10-18)
                    if callee is not None and callee not in starts:
                        if thunks is not None and callee not in thunks:
                            thunks.update(jump_thunks(b, starts, {callee}))
                        callee = (thunks or {}).get(callee, callee)
                    kept = mask.get(callee, 0) if callee is not None else 0
                    kills = (EVERY | EVERY_LOW) & ~(kept | low_of(kept))
                    args = callee_reads.get(callee, ARGS) if callee is not None else site_reads.get(ins.address, ARGS)
                    insns[a] = (use | args, kills, [nxt], callee)
                    a = nxt
                elif mn == "jmp":
                    if op is not None and op.type == X.X86_OP_IMM:
                        if op.imm in starts and op.imm != f:
                            insns[a] = (use | callee_reads.get(op.imm, ARGS), 0, [], None)     # a tail call: nothing after
                            break
                        insns[a] = (use, dfn, [op.imm], None)
                        a = op.imm
                    elif ins.address in switches:
                        insns[a] = (use, dfn, list(switches[ins.address]), None)
                        todo.extend(switches[ins.address])
                        break
                    elif op is not None and not table_jump(prev, ins):
                        # (a tail call through a pointer, `jmp [rax+0x28]` or `jmp rax` past the
                        # epilogue: it reads the argument registers and nothing follows. Taken for
                        # flow lost, FUN_1411d6e10's and FUN_1419c5290's liveness was given up,
                        # and their `extraout_` merges stayed refused, 2026-10-18)
                        insns[a] = (use | ARGS, 0, [], None)
                        break
                    else:
                        insns[a] = (use, dfn, [], None)
                        open_flow = True
                        break
                elif mn.startswith("j") or mn.startswith("loop"):
                    succ = [nxt] + ([op.imm] if op is not None and op.type == X.X86_OP_IMM else [])
                    insns[a] = (use, dfn, succ, None)
                    todo.extend(succ[1:])
                    a = nxt
                elif mn in ("ret", "retf", "int3", "ud2", "hlt"):
                    insns[a] = (use, dfn, [], None)
                    break
                else:
                    insns[a] = (use, dfn, [nxt], None)
                    a = nxt
        if open_flow:
            for x in insns.values():
                if x[3] is not None and x[3] in mask:
                    need[x[3]] = need.get(x[3], 0) | mask[x[3]]
            continue
        followed.append(f)
        live = {a: 0 for a in insns}
        order = sorted(insns, reverse=True)
        changed = True
        while changed:
            changed = False
            for a in order:
                use, dfn, succ, _c = insns[a]
                out = 0
                for s in succ:
                    out |= live.get(s, 0)
                v = use | (out & ~dfn)
                if v != live[a]:
                    live[a] = v
                    changed = True
        for a, (_u, _d, succ, callee) in insns.items():
            if callee is not None and callee in mask:
                after = fold(live.get(succ[0], 0) if succ else 0)
                if after & mask[callee]:
                    need[callee] = need.get(callee, 0) | (after & mask[callee])
            if callee is None and succ and insns[a][1] == (EVERY | EVERY_LOW):
                # (an indirect call, under 0: what is live after one is the caller's own; the
                # translator reads it when it cannot name the call a register came out of,
                # `in_XMM0 = extraout_XMM0` after `(**(code **)param_1[1])(...)` in FUN_141fe3480)
                after = fold(live.get(succ[0], 0))
                clobbered = after & EVERY & ~BIT["RAX"]
                if clobbered:
                    relied.setdefault(f, {})[0] = relied.get(f, {}).get(0, 0) | clobbered
            if callee is not None:
                after = fold(live.get(succ[0], 0) if succ else 0)
                # (rax read after a call it writes is the call's result, not a leftover: the
                # translator's reading of `relied` stays what it was before rax was tracked)
                clobbered = after & (EVERY & ~mask.get(callee, 0)) & ~BIT["RAX"]
                if clobbered:
                    relied.setdefault(f, {})[callee] = relied.get(f, {}).get(callee, 0) | clobbered
    names = lambda m: {r for r in BIT if m & BIT[r]}
    return ({f: names(m) for f, m in need.items() if m},
            {f: {g: sorted(names(m)) for g, m in gs.items()} for f, gs in relied.items()}, followed)


def recover() -> None:
    """--cover: only the conventions again, from the 'needed' and 'all' sets already written (after
    ListConventions.java has listed what the program holds)."""
    facts = load_facts("preserved")
    need = {int(k, 16): set(v) for k, v in facts["needed"].items()}
    full = {int(k, 16): set(v) for k, v in facts["all"].items()}
    chosen, family = cover(need, full)
    facts["functions"] = {hexa(f): sorted(regs) for f, regs in sorted(chosen.items())}
    out = write_facts("preserved", facts)
    short = sum(1 for f, n in need.items() if not n <= chosen.get(f, set()))
    kinds = len({tuple(sorted(v)) for v in chosen.values()})
    print(f"{len(need)} functions; {kinds} conventions used, {short} covered only in part -> {out}")


def jump_thunks(b: Binary, starts: set[int], targets) -> dict[int, int]:
    """The `jmp rel32` thunks among the call targets that are no corpus function, each to the
    function it lands on (through chained thunks). Ghidra makes thunks of them, which the corpus
    leaves out, and its decompilation shows a call to one under the target's name. Taken for
    unknown code, a call to one wrote every register and read every argument: FUN_141c911a0's
    call through 0x1416762f0 (`jmp FUN_14156dc60`) made the rdx a call before it left look live
    (an `extraout_RDX` stub, and three more like it, 2026-10-18)."""
    out = {}
    for t in targets:
        a, hops = t, 0
        while a not in starts and hops < 8 and b.is_code(a) and b.image[b.off(a)] == 0xE9:
            a = a + 5 + struct.unpack_from("<i", b.image, b.off(a) + 1)[0]
            hops += 1
        if a != t and a in starts:
            out[t] = a
    return out


def main() -> None:
    if "--cover" in sys.argv:
        return recover()
    t0 = time.time()
    b = Binary()
    starts = set(load_corpus())
    local = local_facts(b, starts)
    print(f"decoded {len(local)} functions ({time.time() - t0:.0f}s)", file=sys.stderr, flush=True)
    thunks = jump_thunks(b, starts, {g for _w, cs, _e in local.values() for g in cs if g not in starts})
    print(f"{len(thunks)} jump thunks resolved to their targets", file=sys.stderr, flush=True)
    local = {f: (w, frozenset(thunks.get(g, g) for g in cs), e) for f, (w, cs, e) in local.items()}
    writes = {f: set(w) for f, (w, _c, _e) in local.items()}
    changed = True
    while changed:
        changed = False
        for f, (_w, callees, _e) in local.items():
            if writes[f] == ALL:
                continue
            for g in callees:
                # (a callee that is no corpus function, an import thunk or code Ghidra has no
                # function for: everything)
                add = writes.get(g, ALL)
                if not add <= writes[f]:
                    writes[f] |= add
                    changed = True
    keep = {f: sorted(ALL - w) for f, w in writes.items() if ALL - w}
    print(f"{len(keep)} of {len(local)} functions preserve some volatile register "
          f"({time.time() - t0:.0f}s)", file=sys.stderr, flush=True)
    # (xmm0 as tools/protos.py found it kept)
    full = {f: set(r) for f, r in keep.items()}
    for x in load_facts("protos").get("keep_xmm0", []):
        full.setdefault(int(x, 16), set()).add("XMM0")
    need, relied, followed = needed(b, starts, full, thunks)
    chosen, family = cover(need, full)
    out = write_facts("preserved", {
        "rule": "the volatile registers neither the function nor anything it calls writes ('all'); "
                "an indirect call or jump, an import, or undecodable code writes all of them. "
                "'needed': of those, the ones some caller keeps a live value in across a call. "
                "'functions': the set each function's keep convention keeps, one of a small family "
                "(between what it needs and what it keeps)",
        "functions": {hexa(f): sorted(regs) for f, regs in sorted(chosen.items())},
        "needed": {hexa(f): sorted(regs) for f, regs in sorted(need.items())},
        "all": {hexa(f): sorted(regs) for f, regs in sorted(full.items())},
        "relied": {hexa(f): {hexa(g): r for g, r in sorted(gs.items())} for f, gs in sorted(relied.items())},
        # (the functions whose instructions the liveness followed: only for those does an absent
        # entry in 'relied' mean nothing is read after a call)
        "followed": [hexa(f) for f in sorted(followed)],
    })
    short = sum(1 for f, n in need.items() if not n <= chosen.get(f, set()))
    print(f"{len(need)} functions keep a register some caller relies on; {len(family)} conventions cover "
          f"them, {short} only in part ({time.time() - t0:.0f}s) -> {out}")
    print("by register:", dict(Counter(r for regs in need.values() for r in regs)))


BUDGET = 116


def convention_set(name: str) -> set[str] | None:
    """The registers a keep convention keeps, from its name (as ApplyFacts names them)."""
    if name == "__fastcall_keepxmm0":
        return {"XMM0"}
    if not name.startswith("__fastcall_keep_"):
        return None
    out = set()
    for t in name[len("__fastcall_keep_"):].split("_"):
        out.add(t.upper())
    return out


def cover(need: dict[int, set[str]], full: dict[int, set[str]]) -> tuple[dict[int, set[str]], list[frozenset]]:
    """Each function's convention, from a family of at most BUDGET register sets.

    A convention may keep any set between what callers rely on and what the function leaves alone
    (keeping a register it never writes is true whatever is done with it), so one set serves many
    functions. The program holds only so many convention names, and every name a session adds
    keeps its slot (ApplyFacts.applyKeepConventions; 213 needed sets and their xmm0 variants did not
    fit, 2026-10-18). Greedily, the set that serves the most functions not yet served; a function
    no member serves whole gets the member within what it leaves alone that keeps the most of what
    it needs (what it misses is only killed by the call, as under __fastcall)."""
    bit = {r: 1 << i for i, r in enumerate(sorted({r for s in full.values() for r in s}))}

    def m(regs):
        return sum(bit[r] for r in regs)

    pairs = Counter((m(n), m(full.get(f, n))) for f, n in need.items())
    cands = {a for _n, a in pairs} | {n for n, _a in pairs}
    # the conventions the program already holds come free (ListConventions.java): its table keeps
    # every name once used, 126 of a signed byte's 127 slots on 2026-10-18, so what is new is only
    # as much as the slots left allow
    # (ApplyFacts frees every keep name nothing uses before assigning, so all the program's ids but
    # its other conventions' are this family's: ListConventions.java counts those)
    family: list[int] = []
    budget = BUDGET
    held = load_facts("convention_names")
    if held.get("held"):
        others = sum(1 for n in held["held"] if not n.startswith("__fastcall_keep"))
        budget = min(budget, 126 - others - 6)
    print(f"up to {budget} keep conventions", file=sys.stderr, flush=True)
    left = {(n, a): c for (n, a), c in pairs.items() if not any(n & ~s == 0 and s & ~a == 0 for s in family)}
    start = len(family)
    while left and len(family) - start < budget:
        best, gain = None, 0
        for s in cands:
            g = sum(c for (n, a), c in left.items() if n & ~s == 0 and s & ~a == 0)
            if g > gain or (g == gain and best is not None and g and s < best):
                best, gain = s, g
        if best is None or gain == 0:
            break
        family.append(best)
        left = {(n, a): c for (n, a), c in left.items() if not (n & ~best == 0 and best & ~a == 0)}
    names = {v: k for k, v in bit.items()}

    def regs(s):
        return {names[b] for b in names if s & b}

    chosen = {}
    for f, n in need.items():
        nm, am = m(n), m(full.get(f, n))
        fits = [s for s in family if s & ~am == 0]
        if not fits:
            continue
        s = max(fits, key=lambda s: (bin(s & nm).count("1"), -bin(s).count("1")))
        if s & nm:
            chosen[f] = regs(s)
    return chosen, [frozenset(regs(s)) for s in family]


if __name__ == "__main__":
    main()
