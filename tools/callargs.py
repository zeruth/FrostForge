"""Call arguments the decompiler dropped, read back from the instructions at the call.

Ghidra loses a call's register arguments when it cannot tie their setup to the call (`mov ecx,
0x2f` a few instructions up, `xor r8d, r8d; lea edx, [r8 + 1]`). When the value is a constant
the instructions make plain, it can be recovered: walk back from the call to the instruction that
last wrote the argument register, inside the call's straight-line run (no branch, call or jump
target in between), and evaluate it.
"""

from __future__ import annotations

import capstone
from capstone import x86

ARG_REGS = ("rcx", "rdx", "r8", "r9")
_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
_md.detail = True

FAMILY = {}
for full, names in {
    "rax": ("rax", "eax", "ax", "al", "ah"), "rcx": ("rcx", "ecx", "cx", "cl", "ch"),
    "rdx": ("rdx", "edx", "dx", "dl", "dh"), "rbx": ("rbx", "ebx", "bx", "bl", "bh"),
    "rsi": ("rsi", "esi", "si", "sil"), "rdi": ("rdi", "edi", "di", "dil"),
    "rbp": ("rbp", "ebp", "bp", "bpl"), "rsp": ("rsp", "esp", "sp", "spl"),
    **{f"r{n}": (f"r{n}", f"r{n}d", f"r{n}w", f"r{n}b") for n in range(8, 16)},
}.items():
    for name in names:
        FAMILY[name] = full
MASK = {8: (1 << 64) - 1, 4: (1 << 32) - 1}
CALLEE_SAVED = {"rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15"}


class _Function:
    def __init__(self, binary, start: int, size: int):
        o = binary.off(start)
        self.ins = list(_md.disasm(bytes(binary.image[o:o + size]), start))
        self.index = {i.address: k for k, i in enumerate(self.ins)}
        self.targets = set()
        self.sources: dict[int, list[int]] = {}
        for k, i in enumerate(self.ins):
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == x86.X86_OP_IMM:
                self.targets.add(i.operands[0].imm)
                self.sources.setdefault(i.operands[0].imm, []).append(k)
        self.pinned = self._pinned()
        self.frame, self.body_start = self._frame()

    def _frame(self) -> tuple[int | None, int]:
        """How far the prologue moves rsp (its pushes and `sub rsp, N`), and where the body
        starts; None when the prologue does something else to rsp (a probe, a frame pointer
        the body addresses through)."""
        delta = 0
        for k, i in enumerate(self.ins):
            ops = i.operands
            if i.mnemonic == "push":
                delta += 8
            elif i.mnemonic == "sub" and len(ops) == 2 and ops[0].type == x86.X86_OP_REG \
                    and i.reg_name(ops[0].reg) == "rsp" and ops[1].type == x86.X86_OP_IMM:
                delta += ops[1].imm
            elif i.mnemonic == "mov" and len(ops) == 2 and ops[0].type == x86.X86_OP_MEM \
                    and i.reg_name(ops[0].mem.base) == "rsp" and ops[1].type == x86.X86_OP_REG:
                pass        # saving a register in the home space
            elif i.mnemonic in ("mov", "lea") and len(ops) == 2 and ops[0].type == x86.X86_OP_REG \
                    and i.reg_name(ops[0].reg) == "rbp":
                pass        # a frame pointer set up beside rsp; rsp itself does not move
            else:
                return delta, k
        return None, 0

    def path(self, k: int):
        """The instructions executed just before instruction k, latest first, while there is
        only one way they can have run: through a fall-through, or back through the single jump
        into a block nothing falls into. Ends with "entry" at the function's start, or "split"
        where more than one way in meets."""
        p = k
        for _ in range(96):
            here = self.ins[p].address
            jumps = [s for s in self.sources.get(here, []) if s != p]
            prev = self.ins[p - 1] if p > 0 else None
            falls = prev is not None and prev.mnemonic != "jmp" and not prev.group(capstone.CS_GRP_RET)
            if p == 0:
                yield "entry" if not jumps else "split"
                return
            ways = len(jumps) + (1 if falls else 0)
            if ways != 1:
                yield "split"
                return
            p = p - 1 if falls else jumps[0]
            yield p
        yield "split"

    def _pinned(self) -> dict[str, tuple[int, str]]:
        """Callee-saved registers that hold an entry argument for the whole function: set once,
        in the entry run (before any branch or call), from an argument register still holding
        its entry value, and written nowhere else (the epilogue's pop aside)."""
        live = {r: f"param_{n + 1}" for n, r in enumerate(ARG_REGS)}
        found = {}
        for k, i in enumerate(self.ins):
            if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or i.group(capstone.CS_GRP_RET):
                break
            if k > 0 and i.address in self.targets:
                break
            ops = i.operands
            if (i.mnemonic == "mov" and len(ops) == 2 and ops[0].type == x86.X86_OP_REG and ops[0].size == 8
                    and ops[1].type == x86.X86_OP_REG):
                dst, src = FAMILY.get(i.reg_name(ops[0].reg)), FAMILY.get(i.reg_name(ops[1].reg))
                if dst in CALLEE_SAVED and src in live:
                    found[dst] = (k, live[src])
            _, written = i.regs_access()
            for r in written:
                family = FAMILY.get(i.reg_name(r))
                live.pop(family, None)
        def restore(k: int) -> bool:
            # the epilogue putting a saved register back (mov rbx, [rsp + 0x30] before the ret)
            i = self.ins[k]
            ops = i.operands
            if i.mnemonic != "mov" or len(ops) != 2 or ops[1].type != x86.X86_OP_MEM:
                return False
            if i.reg_name(ops[1].mem.base) not in ("rsp", "rbp", "r11"):
                return False
            for nxt in self.ins[k + 1:k + 8]:
                if nxt.group(capstone.CS_GRP_RET) or nxt.mnemonic == "jmp":
                    return True
                if nxt.group(capstone.CS_GRP_CALL):
                    return False
            return False

        out = {}
        for reg, (k, value) in found.items():
            writes = 0
            for n, i in enumerate(self.ins):
                if i.mnemonic in ("pop", "push") or restore(n):
                    continue
                _, written = i.regs_access()
                writes += any(FAMILY.get(i.reg_name(r)) == reg for r in written)
            if writes == 1:
                out[reg] = (k, value)
        return out

    def predecessors(self, p: int) -> list[int]:
        """The instructions that can run just before instruction p."""
        out = [q for q in self.sources.get(self.ins[p].address, []) if q != p]
        prev = self.ins[p - 1] if p > 0 else None
        if prev is not None and prev.mnemonic != "jmp" and not prev.group(capstone.CS_GRP_RET):
            out.append(p - 1)
        return out

    def value(self, k: int, reg: str, floor: int, depth: int = 0) -> int | str | None:
        """What `reg` (a 64-bit family name) holds just before instruction k: a constant, or a C
        expression over the function's parameters and globals; None when that is not certain.
        Every way into k is followed back; where ways meet, they must all give the same value."""
        if depth > 4:
            return None
        if reg in self.pinned and self.pinned[reg][0] < k:
            return self.pinned[reg][1]
        memo: dict[tuple[int, bool], object] = {}
        UNSET = object()

        def before(p: int, clean: bool, visiting: frozenset, budget: list) -> object:
            """The value just before instruction p (None: unknown)."""
            key = (p, clean)
            if key in memo:
                return memo[key]
            if p in visiting or budget[0] <= 0:
                return None             # a loop, or too far
            budget[0] -= 1
            if p == 0 and not self.sources.get(self.ins[0].address):
                result = f"param_{ARG_REGS.index(reg) + 1}" if reg in ARG_REGS else None
                memo[key] = result
                return result
            preds = self.predecessors(p)
            if not preds:
                return None
            results = []
            for q in preds:
                r = at(q, clean, visiting | {p}, budget)
                if r is None:
                    memo[key] = None
                    return None
                results.append(r)
            result = results[0] if all(r == results[0] for r in results) else None
            memo[key] = result
            return result

        def at(q: int, clean: bool, visiting: frozenset, budget: list) -> object:
            """The value just after instruction q has run."""
            i = self.ins[q]
            if i.group(capstone.CS_GRP_CALL):
                if reg not in CALLEE_SAVED:
                    return None         # a call clobbers the volatile registers
                return before(q, False, visiting, budget)
            if not i.group(capstone.CS_GRP_JUMP):
                writes_memory = any(op.type == x86.X86_OP_MEM and op.access & capstone.CS_AC_WRITE
                                    for op in i.operands)
                _, written = i.regs_access()
                if any(FAMILY.get(i.reg_name(r)) == reg for r in written):
                    return self._evaluate(q, i, reg, floor, depth, clean)
                if writes_memory:
                    clean = False
            return before(q, clean, visiting, budget)

        return before(k, True, frozenset(), [400])

    def _evaluate(self, j: int, i, reg: str, floor: int, depth: int, clean: bool = False) -> int | str | None:
        ops = i.operands
        if not ops or ops[0].type != x86.X86_OP_REG or FAMILY.get(i.reg_name(ops[0].reg)) != reg:
            return None
        width = ops[0].size
        if width not in MASK:
            return None
        mn = i.mnemonic
        if mn == "xor" and len(ops) == 2 and ops[1].type == x86.X86_OP_REG and ops[1].reg == ops[0].reg:
            return 0
        if mn in ("mov", "movabs") and ops[1].type == x86.X86_OP_IMM:
            return ops[1].imm & MASK[width]
        if mn == "mov" and ops[1].type == x86.X86_OP_REG:
            src = self.value(j, FAMILY.get(i.reg_name(ops[1].reg), ""), floor, depth + 1)
            if isinstance(src, str):
                return src if width == 8 else None
            return None if src is None else src & MASK[width]
        if mn in ("add", "sub") and ops[1].type == x86.X86_OP_IMM:
            before = self.value(j, reg, floor, depth + 1)
            delta = ops[1].imm if mn == "add" else -ops[1].imm
            if isinstance(before, int):
                return (before + delta) & MASK[width]
            if isinstance(before, str) and width == 8:
                return f"(undefined8)((char *)({before}) + {delta:#x})"
            return None
        if mn in ("mov", "movzx") and ops[1].type == x86.X86_OP_MEM and clean:
            # a load nothing has written over since: the same read, in C
            mem = ops[1].mem
            size = ops[1].size
            if mem.index != 0 or mem.base == 0 or size not in (1, 2, 4, 8):
                return None
            ctype = {1: "byte", 2: "ushort", 4: "uint", 8: "ulonglong"}[size]
            if i.reg_name(mem.base) == "rip":
                return f"(*({ctype} *)&DAT_{(i.address + i.size + mem.disp) & MASK[8]:x})"
            if i.reg_name(mem.base) == "rsp" and self.frame is not None and size == 8 and j >= self.body_start:
                # a stack slot: the local Ghidra names by its offset from the entry stack pointer
                # (local_N below it, local_resN in the caller's home space above it)
                o = mem.disp - self.frame
                return f"local_res{o:x}" if o > 0 else f"local_{-o:x}"
            base = self.value(j, FAMILY.get(i.reg_name(mem.base), ""), floor, depth + 1)
            if isinstance(base, str):
                return f"(*({ctype} *)((char *)({base}) + {mem.disp:#x}))"
            return None
        if mn == "or" and ops[1].type == x86.X86_OP_IMM and ops[1].imm == -1:
            return MASK[width]
        if mn == "lea" and ops[1].type == x86.X86_OP_MEM:
            mem = ops[1].mem
            if mem.index != 0 or mem.base == 0:
                return None
            if i.reg_name(mem.base) == "rip":
                # an address in the image: named as the decompilation names a global, so the
                # translator places it (the port's copy when the port holds it)
                return f"&DAT_{(i.address + i.size + mem.disp) & MASK[8]:x}" if width == 8 else None
            base = self.value(j, FAMILY.get(i.reg_name(mem.base), ""), floor, depth + 1)
            if isinstance(base, str) and width == 8:
                # an address formed from a symbolic one (a parameter + a field offset): in bytes,
                # whatever C type the parameter has
                return f"(undefined8)((char *)({base}) + {mem.disp:#x})" if mem.disp else base
            return None if not isinstance(base, int) else (base + mem.disp) & MASK[width]
        return None


class CallArgs:
    """Recovered constant arguments per function, cached."""

    def __init__(self, binary):
        self.binary = binary
        self._functions: dict[int, _Function] = {}

    def indirect_calls(self, function: int, size: int) -> int:
        """How many calls the function makes through a register or memory (imports included)."""
        f = self._functions.get(function)
        if f is None:
            f = self._functions[function] = _Function(self.binary, function, size)
        return sum(1 for i in f.ins if i.mnemonic == "call" and i.operands
                   and i.operands[0].type != x86.X86_OP_IMM)

    def recover(self, function: int, size: int, target: int, positions: list[int], every: list | None = None):
        """The constants every call from `function` to `target` passes in the argument
        `positions` (0-3), when all calls agree; None when any is unknown or they differ.
        With `every` (a list), each call's values are appended to it instead, unknowns as None,
        and the list is returned."""
        if any(p >= len(ARG_REGS) for p in positions):
            return None
        f = self._functions.get(function)
        if f is None:
            f = self._functions[function] = _Function(self.binary, function, size)
        found = None
        for k, i in enumerate(f.ins):
            if i.mnemonic not in ("call", "jmp") or not i.operands or i.operands[0].type != x86.X86_OP_IMM:
                continue
            if i.operands[0].imm != target:
                continue
            values = [f.value(k, ARG_REGS[p], 0) for p in positions]
            if every is not None:
                every.append(values)
                continue
            if any(v is None for v in values):
                return None
            if found is not None and values != found:
                return None
            found = values
        return every if every is not None else found
