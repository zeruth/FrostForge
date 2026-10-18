"""The volatile registers a function returns holding one of its own arguments: build/facts/leftover.json.

The reference is a whole-program (LTCG) build, so a caller may read a volatile register after a
call when the compiler knows what the callee leaves there. tools/preserved.py covers the register
the callee never writes; this covers the one it overwrites with a copy of an argument: Lua's
FUN_1402bfa60 moves its first argument into r11 and returns with it there, and its caller
FUN_1402bf300 goes on using r11 as `L`. Ghidra sees only that r11 died at the call and writes
`param_1 = extraout_R11;`, a register "the decompiler lost track of", and the caller is stubbed
(267 game functions read one, 2026-10-18). With this fact the translator assigns the argument to
that variable as the call passes it (tools/translate.py, leftovers()).

Per function, a forward pass over the instructions reachable from the entry tracks which entry
register each general register holds (`mov r11, rcx`; a push and its pop; a spill to the frame
and its reload), through calls by the callee's own summary, to a fixed point. A register holds an
argument at the return when every `ret` (or tail call) agrees on it. An indirect jump, code that
does not decode, or a frame address escaping into a call (`lea rcx, [rsp+..]`, which lets the
callee change a spilled slot) ends the tracking of what it touches, so the fact errs toward absent.

    python tools/leftover.py [--show <addr>]   -> build/facts/leftover.json
"""
from __future__ import annotations

import sys
import time
from collections import Counter

import capstone
from capstone import x86 as X

from corpus import load_corpus, load_facts
from preserved import load_switches
from wowbin import Binary, hexa, regs_access, write_facts

GPR = ("RAX", "RCX", "RDX", "RBX", "RBP", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13",
       "R14", "R15")
VOLATILE = ("RAX", "RCX", "RDX", "R8", "R9", "R10", "R11")
ARGS = ("RCX", "RDX", "R8", "R9")
FAMILY, WIDTH = {}, {}
for full, names in {"RAX": ("rax", "eax", "ax", "al", "ah"), "RCX": ("rcx", "ecx", "cx", "cl", "ch"),
                    "RDX": ("rdx", "edx", "dx", "dl", "dh"), "RBX": ("rbx", "ebx", "bx", "bl", "bh"),
                    "RBP": ("rbp", "ebp", "bp", "bpl"), "RSI": ("rsi", "esi", "si", "sil"),
                    "RDI": ("rdi", "edi", "di", "dil"), "RSP": ("rsp", "esp", "sp", "spl"),
                    **{f"R{n}": (f"r{n}", f"r{n}d", f"r{n}w", f"r{n}b") for n in range(8, 16)}}.items():
    for i, n in enumerate(names):
        FAMILY[n] = full
        WIDTH[n] = (64, 32, 16, 8, 8)[i]
LIMIT = 20000
TOP = None


def entry_state():
    # (a value is (entry register, width): `mov r9d, edx` leaves the low half of the second
    # argument, zero-extended, good for a 32-bit read and no wider)
    return {"regs": {r: (r, 64) for r in GPR}, "stack": (), "slots": {}, "delta": 0}


def join(a, b):
    if a is None:
        return b
    regs = {r: (a["regs"][r] if a["regs"][r] == b["regs"][r] else TOP) for r in GPR}
    same = a["delta"] == b["delta"]
    return {"regs": regs,
            "stack": a["stack"] if same and a["stack"] == b["stack"] else (),
            "slots": {k: v for k, v in a["slots"].items() if same and b["slots"].get(k) == v},
            "delta": a["delta"] if same else None}


def narrow(v, w):
    if v is TOP:
        return TOP
    return (v[0], min(v[1], w))


class Analysis:
    def __init__(self, b: Binary, starts: set[int], preserved: dict[int, set[str]]):
        self.b, self.starts, self.preserved = b, starts, preserved
        self.switches = load_switches()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.summary: dict[int, dict[str, tuple]] = {}
        self._insns: dict[int, object] = {}
        self._busy: set[int] = set()

    def summary_of(self, f: int) -> dict:
        """A callee's summary, computed when first needed (its own callees first); a call back
        into one being computed (recursion) counts as unknown."""
        if f not in self.summary:
            if f in self._busy or f not in self.starts or len(self._busy) > 400:
                return {}
            self._busy.add(f)
            try:
                self.summary[f] = self.run(f) or {}
            finally:
                self._busy.discard(f)
        return self.summary[f]

    def insn(self, a):
        if len(self._insns) > 2_000_000:
            # (decoded instructions are a few hundred bytes each: a whole-image cache was 10 GB)
            self._insns.clear()
        if a not in self._insns:
            if not (self.b.text.start <= a < self.b.text.end):
                self._insns[a] = None
            else:
                o = self.b.off(a)
                self._insns[a] = next(self.md.disasm(bytes(self.b.image[o:o + 16]), a), None)
        return self._insns[a]

    def after_call(self, st, callee):
        """The state after a call: the callee's summary, in this caller's terms."""
        regs = dict(st["regs"])
        summ = self.summary_of(callee) if callee is not None else None
        keep = self.preserved.get(callee, set()) if callee is not None else set()
        for r in VOLATILE:
            if r in keep:
                continue
            s = summ.get(r) if summ else None
            regs[r] = TOP if s is None else narrow(st["regs"][s[0]], s[1])
        regs["RAX"] = TOP
        return {"regs": regs, "stack": st["stack"], "slots": st["slots"], "delta": st["delta"]}

    def run(self, f: int, show=False):
        """The registers holding an entry register's value at every return, or None."""
        states = {f: entry_state()}
        todo = [f]
        rets = None
        n = 0
        escaped = False
        while todo:
            a = todo.pop()
            st = states[a]
            ins = self.insn(a)
            n += 1
            if ins is None or n > LIMIT:
                return None
            nxt, out = [], st
            regs = dict(st["regs"])
            stack, slots, delta = st["stack"], dict(st["slots"]), st["delta"]
            mn = ins.mnemonic
            ops = ins.operands

            def reg_of(op):
                return FAMILY.get(ins.reg_name(op.reg)) if op.type == X.X86_OP_REG else None

            def rsp_slot(op):
                m = op.mem
                if (op.type == X.X86_OP_MEM and m.base and ins.reg_name(m.base) == "rsp" and not m.index
                        and delta is not None):
                    return delta + m.disp
                return None

            handled = False
            if mn == "mov" and len(ops) == 2:
                d, s = ops
                dr, sr = reg_of(d), reg_of(s)
                if dr and sr and dr != "RSP" and sr != "RSP":
                    w = WIDTH[ins.reg_name(d.reg)]
                    # (a 32-bit move zero-extends: the low half of the source, and a narrower
                    # one merges into the old value, which is no copy at all)
                    regs[dr] = narrow(regs.get(sr, TOP), w) if w >= 32 else TOP
                    handled = True
                elif dr and dr != "RSP" and rsp_slot(s) is not None and s.size == 8:
                    regs[dr] = slots.get(rsp_slot(s), TOP)
                    handled = True
                elif sr and rsp_slot(d) is not None:
                    slots[rsp_slot(d)] = regs.get(sr, TOP) if d.size == 8 else TOP
                    handled = True
            elif mn == "xchg" and len(ops) == 2 and reg_of(ops[0]) and reg_of(ops[1]) and ops[0].size == 8:
                x, y = reg_of(ops[0]), reg_of(ops[1])
                regs[x], regs[y] = regs.get(y, TOP), regs.get(x, TOP)
                regs.pop("RSP", None)
                handled = True
            elif mn == "push" and ops and reg_of(ops[0]) and ops[0].size == 8:
                stack = stack + (regs.get(reg_of(ops[0]), TOP),)
                delta = None if delta is None else delta - 8
                handled = True
            elif mn == "pop" and ops and reg_of(ops[0]) and reg_of(ops[0]) != "RSP":
                r = reg_of(ops[0])
                regs[r] = stack[-1] if stack else TOP
                stack = stack[:-1]
                delta = None if delta is None else delta + 8
                handled = True
            elif mn in ("sub", "add") and len(ops) == 2 and reg_of(ops[0]) == "RSP" and ops[1].type == X.X86_OP_IMM:
                if delta is not None:
                    delta += -ops[1].imm if mn == "sub" else ops[1].imm
                # (the frame's slots are addressed from the new rsp: keyed by entry offset, the
                # same slot keeps its key)
                handled = True
            elif mn == "lea" and len(ops) == 2 and ops[1].type == X.X86_OP_MEM and ins.reg_name(ops[1].mem.base) == "rsp":
                # an address in the frame: whatever it is given to may write the slots
                escaped = True
                r = reg_of(ops[0])
                if r and r != "RSP":
                    regs[r] = TOP
                handled = True
            if escaped:
                slots = {}
            if not handled:
                for r in regs_access(ins)[1]:
                    fam = FAMILY.get(ins.reg_name(r))
                    if fam == "RSP":
                        if mn not in ("call", "ret"):
                            delta, stack, slots = None, (), {}
                    elif fam:
                        regs[fam] = TOP
                # a store to the frame through anything but a plain rsp slot
                for op in ops:
                    if op.type == X.X86_OP_MEM and op.access & capstone.CS_AC_WRITE:
                        k = rsp_slot(op)
                        if k is not None:
                            slots.pop(k, None)
            out = {"regs": regs, "stack": stack, "slots": slots, "delta": delta}
            op = ops[0] if ops else None
            if mn == "call":
                callee = op.imm if op is not None and op.type == X.X86_OP_IMM else None
                out = self.after_call(out, callee)
                nxt.append(ins.address + ins.size)
            elif mn == "jmp":
                if op is not None and op.type == X.X86_OP_IMM:
                    if op.imm in self.starts and op.imm != f:
                        ret = self.after_call(out, op.imm)
                        rets = join(rets, ret)
                    else:
                        nxt.append(op.imm)
                elif ins.address in self.switches:
                    nxt.extend(self.switches[ins.address])
                else:
                    return None
            elif mn.startswith("j") or mn.startswith("loop"):
                if op is not None and op.type == X.X86_OP_IMM:
                    nxt.append(op.imm)
                nxt.append(ins.address + ins.size)
            elif mn == "ret":
                rets = join(rets, out)
            elif mn in ("int3", "ud2", "hlt"):
                pass
            else:
                nxt.append(ins.address + ins.size)
            for t in nxt:
                old = states.get(t)
                new = join(old, out)
                if old is None or new != old:
                    states[t] = new
                    todo.append(t)
        if show:
            print(hexa(f), rets and rets["regs"])
        if rets is None:
            return {}
        # (a register restored to its own entry value is kept too: a caller's value survives it)
        return {r: v for r, v in rets["regs"].items() if r in VOLATILE and r != "RAX" and v is not TOP}


def main() -> None:
    t0 = time.time()
    b = Binary()
    cor = load_corpus()
    starts = set(cor)
    # (everything a function keeps, not only what its callers rely on)
    facts = load_facts("preserved")
    pres = {int(k, 16): set(v) for k, v in facts.get("all", facts.get("functions", {})).items()}
    an = Analysis(b, starts, pres)
    sys.setrecursionlimit(20000)
    if "--show" in sys.argv:
        for x in sys.argv[sys.argv.index("--show") + 1:]:
            an.summary[int(x, 16)] = an.run(int(x, 16), show=True)
        return
    # only what a decompilation reads a leftover of: the callees of every function that has an
    # `extraout_` variable (by address, and by name)
    import re
    by_name = {}
    for a, e in cor.items():
        by_name.setdefault(e["name"], a)
    want = set()
    for a, e in cor.items():
        c = e.get("c", "")
        if "extraout_" not in c:
            continue
        c = c[c.find("{"):]
        want |= {int(x, 16) for x in re.findall(r"\bFUN_([0-9a-f]{9})\b", c)}
        want |= {by_name[n] for n in re.findall(r"\b([A-Za-z_][\w:]*)\s*\(", c) if n in by_name}
    want &= starts
    print(f"{len(want)} callees to summarize", file=sys.stderr, flush=True)
    for k, f in enumerate(sorted(want)):
        an.summary_of(f)
        if k % 2000 == 0:
            print(f"  {k} ({len(an.summary)} summaries, {time.time() - t0:.0f}s)", file=sys.stderr, flush=True)
    left = {f: {r: [v[0], v[1]] for r, v in s.items() if v[0] in ARGS and v[0] != r}
            for f, s in an.summary.items()}
    left = {f: s for f, s in left.items() if s}
    out = write_facts("leftover", {
        "rule": "volatile registers that hold an entry argument register's value at every return "
                "(register -> [argument register, bits])",
        "functions": {hexa(f): s for f, s in sorted(left.items())},
    })
    print(f"{len(left)} functions return an argument in another register -> {out}")
    print("by register:", Counter(f"{r}={v[0]}" for s in left.values() for r, v in s.items()).most_common(20))


if __name__ == "__main__":
    main()
