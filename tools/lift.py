"""Straight-line reference code Ghidra never made a function of, lifted to C from its instructions.

The decompilation takes the address of thousands of these (`&LAB_x`): member-function thunks
that adjust `this`, argument shuffles in front of a tail call, one-line accessors. Nothing
translates them, so the image keeps int3 at their address and a callback through one returns 0.
`lift(binary, address)` follows the instructions to the first `jmp` (a tail call) or `ret` and
gives the C function they are, or None when it meets anything it does not model: a branch, the
stack, a vector register, an instruction outside the small set below. A lifted function takes
the four integer argument registers and returns rax; a tail call passes them on as they stand.
"""

from __future__ import annotations

import re

import capstone

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

ARGS = ("rcx", "rdx", "r8", "r9")
R64 = {}
for full, names in {
    "rax": ("rax", "eax", "ax", "al"), "rcx": ("rcx", "ecx", "cx", "cl"), "rdx": ("rdx", "edx", "dx", "dl"),
    "rbx": ("rbx", "ebx", "bx", "bl"), "rsi": ("rsi", "esi", "si", "sil"), "rdi": ("rdi", "edi", "di", "dil"),
    **{f"r{n}": (f"r{n}", f"r{n}d", f"r{n}w", f"r{n}b") for n in range(8, 16)},
}.items():
    for width, name in zip((8, 4, 2, 1), names):
        R64[name] = (full, width)
CTYPE = {8: "ulonglong", 4: "uint", 2: "ushort", 1: "byte"}
SIZE = {"qword": 8, "dword": 4, "word": 2, "byte": 1}


class Unliftable(Exception):
    pass


def lift(binary, address: int, limit: int = 16) -> str | None:
    """The code at `address` as a function in the decompilation's own C, or None."""
    try:
        body, _ = _lift(binary, address, limit)
    except Unliftable:
        return None
    return (f"/* lifted from the instructions (tools/lift.py) */\n"
            f"undefined8 FUN_{address:x}(undefined8 a0,undefined8 a1,undefined8 a2,undefined8 a3)\n\n"
            f"{{\n{body}\n}}\n")


def _lift(binary, address: int, limit: int):
    regs = {r: f"a{i}" for i, r in enumerate(ARGS)}
    lines: list[str] = []
    flags = None            # the expression the last test/cmp left, as "expr" (ZF: expr == 0)
    temp = 0

    def read(op: str) -> str:
        op = op.strip()
        if op in R64:
            full, width = R64[op]
            if full not in regs:
                raise Unliftable(f"reads {full} before it is set")
            return regs[full] if width == 8 else f"(({CTYPE[width]}){regs[full]})"
        if re.fullmatch(r"-?0x[0-9a-f]+|-?\d+", op):
            return f"(ulonglong){int(op, 0) & 0xffffffffffffffff:#x}ULL"
        m = re.fullmatch(r"(qword|dword|word|byte) ptr \[(.+)\]", op)
        if m:
            return f"(*({CTYPE[SIZE[m.group(1)]]} *)({address_of(m.group(2))}))"
        raise Unliftable(f"operand {op}")

    def address_of(expr: str) -> str:
        m = re.fullmatch(r"rip ([+-]) (0x[0-9a-f]+)", expr)
        if m:
            target = here + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            # named as the decompilation names a global, so the translator places it (the
            # image's copy, or the port's when the port holds it)
            return f"((ulonglong)&DAT_{target:x})"
        m = re.fullmatch(r"(\w+)(?: \+ ([a-z]\w*)(?:\*(\d))?)?(?: ([+-]) (0x[0-9a-f]+|\d+))?", expr)
        if not m or m.group(1) not in R64 or R64[m.group(1)][1] != 8:
            raise Unliftable(f"address {expr}")
        out = read(m.group(1))
        if m.group(2):
            if m.group(2) not in R64:
                raise Unliftable(f"index {m.group(2)}")
            out = f"({out} + {read(m.group(2))} * {m.group(3) or 1})"
        if m.group(4):
            out = f"({out} {m.group(4)} {int(m.group(5), 0):#x})"
        return out

    def write(op: str, value: str):
        nonlocal temp
        op = op.strip()
        if op in R64:
            full, width = R64[op]
            if width == 8:
                expr = value
            elif width == 4:
                expr = f"(ulonglong)(uint)({value})"          # a 32-bit write clears the top half
            else:
                if full not in regs:
                    raise Unliftable("a partial write to an unset register")
                mask = (1 << (8 * width)) - 1
                expr = f"(({regs[full]} & ~{mask:#x}ULL) | ((ulonglong)({value}) & {mask:#x}))"
            # each value is computed once, in order, as the instructions do
            name = f"t{temp}"
            temp += 1
            lines.append(f"  ulonglong {name} = {expr};")
            regs[full] = name
            return
        m = re.fullmatch(r"(qword|dword|word|byte) ptr \[(.+)\]", op)
        if m:
            lines.append(f"  *({CTYPE[SIZE[m.group(1)]]} *)({address_of(m.group(2))}) = "
                         f"({CTYPE[SIZE[m.group(1)]]})({value});")
            return
        raise Unliftable(f"destination {op}")

    def width_of(op: str) -> int:
        if op in R64:
            return R64[op][1]
        m = re.match(r"(qword|dword|word|byte) ptr ", op)
        if not m:
            raise Unliftable(f"operand {op}")
        return SIZE[m.group(1)]

    callees = []
    o = binary.off(address)
    count = 0
    for ins in _md.disasm(bytes(binary.image[o:o + 16 * limit]), address):
        count += 1
        if count > limit:
            raise Unliftable("too long")
        here = ins.address + ins.size
        mn, ops = ins.mnemonic, [x.strip() for x in ins.op_str.split(",")] if ins.op_str else []
        if mn == "mov" or mn == "movabs":
            write(ops[0], read(ops[1]))
        elif mn == "movzx":
            write(ops[0], read(ops[1]))
        elif mn == "movsxd" or mn == "movsx":
            src = read(ops[1])
            width = SIZE.get(ops[1].split()[0], R64.get(ops[1], (None, 4))[1])
            write(ops[0], f"(ulonglong)(longlong)({ {4: 'int', 2: 'short', 1: 'char'}[width]})({src})")
        elif mn == "lea":
            m = re.fullmatch(r"(?:\w+ ptr )?\[(.+)\]", ops[1])
            if not m:
                raise Unliftable("lea operand")
            write(ops[0], address_of(m.group(1)))
        elif mn == "xor" and ops[0] == ops[1]:
            write(ops[0], "0")
            flags = None
        elif mn in ("add", "sub", "and", "or", "xor", "shr", "shl", "sar"):
            op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^", "shr": ">>", "shl": "<<", "sar": ">>"}[mn]
            width = width_of(ops[0])
            ctype = CTYPE[width] if mn != "sar" else {8: "longlong", 4: "int", 2: "short", 1: "char"}[width]
            value = f"(({ctype})({read(ops[0])}) {op} ({ctype})({read(ops[1])}))"
            write(ops[0], value)
            flags = f"({CTYPE[width]})({value})"
        elif mn == "test":
            width = width_of(ops[0])
            flags = f"({CTYPE[width]})({read(ops[0])} & {read(ops[1])})"
        elif mn in ("cmove", "cmovne", "cmovz", "cmovnz"):
            if flags is None:
                raise Unliftable("cmov without a test")
            zero = mn in ("cmove", "cmovz")
            cond = f"({flags} == 0)" if zero else f"({flags} != 0)"
            write(ops[0], f"({cond} ? {read(ops[1])} : {read(ops[0])})")
        elif mn == "mul" and len(ops) == 1:
            width = width_of(ops[0])
            if width != 4:
                raise Unliftable("mul width")
            product = f"((ulonglong)(uint)({read('eax')}) * (ulonglong)(uint)({read(ops[0])}))"
            name = f"t{temp}"
            temp += 1
            lines.append(f"  ulonglong {name} = {product};")
            write("eax", f"(uint){name}")
            write("edx", f"(uint)({name} >> 32)")
        elif mn == "jmp":
            if not ops or not re.fullmatch(r"0x[0-9a-f]+", ops[0]):
                raise Unliftable("an indirect jump")
            target = int(ops[0], 16)
            callees.append(target)
            args = ", ".join(regs[r] if r in regs else "0" for r in ARGS)
            lines.append(f"  return FUN_{target:x}({args});")
            return "\n".join(lines), callees
        elif mn == "ret" and not ops:
            lines.append(f"  return {regs['rax']};" if "rax" in regs else "  return 0;")
            return "\n".join(lines), callees
        elif mn in ("nop", "int3") and mn == "nop":
            pass
        else:
            raise Unliftable(f"instruction {mn} {ins.op_str}")
    raise Unliftable("ran off the end")
