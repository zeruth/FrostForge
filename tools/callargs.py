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
        for i in self.ins:
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == x86.X86_OP_IMM:
                self.targets.add(i.operands[0].imm)
        self.pinned = self._pinned()

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
        out = {}
        for reg, (k, value) in found.items():
            writes = 0
            for i in self.ins:
                if i.mnemonic in ("pop", "push"):
                    continue
                _, written = i.regs_access()
                writes += any(FAMILY.get(i.reg_name(r)) == reg for r in written)
            if writes == 1:
                out[reg] = (k, value)
        return out

    def value(self, k: int, reg: str, floor: int, depth: int = 0) -> int | None:
        """The constant in `reg` (a 64-bit family name) just before instruction k, or None."""
        if depth > 4:
            return None
        if reg in self.pinned and self.pinned[reg][0] < k:
            return self.pinned[reg][1]
        for j in range(k - 1, floor - 1, -1):
            i = self.ins[j]
            if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or i.group(capstone.CS_GRP_RET):
                return None
            # a jump into the run between here and the call: another path reaches the call
            if any(i.address < t <= self.ins[k].address for t in self.targets):
                return None
            _, written = i.regs_access()
            names = {i.reg_name(r) for r in written}
            if not any(FAMILY.get(n) == reg for n in names):
                continue
            return self._evaluate(j, i, reg, floor, depth)
        if floor == 0 and reg in ARG_REGS:
            # back to the entry untouched: the argument the function itself was given
            return f"param_{ARG_REGS.index(reg) + 1}"
        return None

    def _evaluate(self, j: int, i, reg: str, floor: int, depth: int) -> int | None:
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
            return None if not isinstance(base, int) else (base + mem.disp) & MASK[width]
        return None


class CallArgs:
    """Recovered constant arguments per function, cached."""

    def __init__(self, binary):
        self.binary = binary
        self._functions: dict[int, _Function] = {}

    def recover(self, function: int, size: int, target: int, positions: list[int]) -> list[int] | None:
        """The constants every call from `function` to `target` passes in the argument
        `positions` (0-3), when all calls agree; None when any is unknown or they differ."""
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
            if any(v is None for v in values):
                return None
            if found is not None and values != found:
                return None
            found = values
        return found
