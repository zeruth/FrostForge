"""Small reference code Ghidra never made a function of, lifted to C from its instructions.

The decompilation takes the address of thousands of these (`&LAB_x`): member-function thunks
that adjust `this`, argument shuffles in front of a tail call, one-line accessors, the copy and
move operations a std::variant's visitor tables point at. Nothing translates them, so the image
keeps int3 at their address and a callback through one returns 0.

`lift(binary, address)` decodes every instruction reachable from `address` (through conditional
and local jumps) up to a `ret` or a tail call, and gives the C function they are, or None when it
meets anything it does not model: the stack, a vector register, an indirect jump, an instruction
outside the set below. Registers are C variables, so a branch is a `goto` to the instruction it
lands on; the flags a `test`, `cmp`, arithmetic or `lock cmpxchg` leave are kept as values for the
conditional jump or move that reads them. A lifted function takes the four integer argument
registers and returns rax; a tail call passes them on as they stand.
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
STYPE = {8: "longlong", 4: "int", 2: "short", 1: "char"}
SIZE = {"qword": 8, "dword": 4, "word": 2, "byte": 1}
# the conditions a jcc/cmovcc/setcc tests, over the kept flags: (zero-result form, compare form)
CONDITIONS = {
    "e": ("{r} == 0", "{a} == {b}"), "z": ("{r} == 0", "{a} == {b}"),
    "ne": ("{r} != 0", "{a} != {b}"), "nz": ("{r} != 0", "{a} != {b}"),
    "s": ("({s}){r} < 0", None), "ns": ("({s}){r} >= 0", None),
    "b": (None, "{a} < {b}"), "c": (None, "{a} < {b}"), "nae": (None, "{a} < {b}"),
    "ae": (None, "{a} >= {b}"), "nb": (None, "{a} >= {b}"), "nc": (None, "{a} >= {b}"),
    "be": (None, "{a} <= {b}"), "na": (None, "{a} <= {b}"),
    "a": (None, "{a} > {b}"), "nbe": (None, "{a} > {b}"),
    "l": ("({s}){r} < 0", "({s}){a} < ({s}){b}"), "nge": (None, "({s}){a} < ({s}){b}"),
    "ge": ("({s}){r} >= 0", "({s}){a} >= ({s}){b}"), "nl": (None, "({s}){a} >= ({s}){b}"),
    "le": ("({s}){r} <= 0", "({s}){a} <= ({s}){b}"), "ng": (None, "({s}){a} <= ({s}){b}"),
    "g": ("({s}){r} > 0", "({s}){a} > ({s}){b}"), "nle": (None, "({s}){a} > ({s}){b}"),
}


class Unliftable(Exception):
    pass


def lift(binary, address: int, limit: int = 48) -> str | None:
    """The code at `address` as a function in the decompilation's own C, or None."""
    try:
        body, _ = _lift(binary, address, limit)
    except Unliftable:
        return None
    return (f"/* lifted from the instructions (tools/lift.py) */\n"
            f"undefined8 FUN_{address:x}(undefined8 a0,undefined8 a1,undefined8 a2,undefined8 a3)\n\n"
            f"{{\n{body}\n}}\n")


def _decode(binary, address: int, limit: int) -> dict:
    """Every instruction reachable from `address`, by address."""
    is_start = getattr(binary, "is_func_start", None)
    found, todo = {}, [address]
    while todo:
        at = todo.pop()
        while at not in found:
            o = binary.off(at)
            ins = next(_md.disasm(bytes(binary.image[o:o + 16]), at, 1), None)
            if ins is None:
                raise Unliftable("undecodable")
            found[at] = ins
            if len(found) > limit:
                raise Unliftable("too long")
            mn = ins.mnemonic
            if mn == "int3":
                raise Unliftable("runs into padding")
            if mn == "ret":
                break
            if mn == "jmp":
                if not re.fullmatch(r"0x[0-9a-f]+", ins.op_str):
                    raise Unliftable("an indirect jump")
                target = int(ins.op_str, 16)
                if _local(binary, address, target, is_start):
                    todo.append(target)
                break
            if mn.startswith("j"):
                if not re.fullmatch(r"0x[0-9a-f]+", ins.op_str):
                    raise Unliftable("an indirect branch")
                target = int(ins.op_str, 16)
                if not _local(binary, address, target, is_start):
                    raise Unliftable("a branch out of the code")
                todo.append(target)
            at = ins.address + ins.size
    return found


def _local(binary, start: int, target: int, is_start) -> bool:
    """A jump that stays in this code rather than tail-calling another function."""
    if target == start:
        return True
    if is_start is not None and is_start(target):
        return False
    return start < target < start + 0x400


def _lift(binary, address: int, limit: int):
    found = _decode(binary, address, limit)
    order = sorted(found)
    targets = set()
    for ins in found.values():
        if ins.mnemonic.startswith("j") and re.fullmatch(r"0x[0-9a-f]+", ins.op_str):
            t = int(ins.op_str, 16)
            if t in found:
                targets.add(t)

    lines: list[str] = []
    used: set[str] = set()        # registers read
    written: set[str] = set(ARGS)  # set before they are read, in address order
    reads: set[str] = set()       # argument registers read: the arguments the code takes
    set_here: set[str] = set()    # registers the code itself has set
    flags = {"kind": None}        # what the last flag-setting instruction left
    fcount = [0]
    callees = []

    def reg(full: str) -> str:
        return f"r_{full}"

    def read(op: str) -> str:
        op = op.strip()
        if op in R64:
            full, width = R64[op]
            if full not in written:
                # a callee-saved register or leftover state: not this code's to read
                raise Unliftable(f"reads {full} before it is set")
            used.add(full)
            if full not in set_here:
                reads.add(full)
            return reg(full) if width == 8 else f"(({CTYPE[width]}){reg(full)})"
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
        if not m or m.group(1) not in R64 or R64[m.group(1)][1] != 8 or m.group(1) in ("rsp", "rbp"):
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
        op = op.strip()
        if op in R64:
            full, width = R64[op]
            if width < 4 and full not in written:
                raise Unliftable("a partial write to an unset register")
            written.add(full)
            set_here.add(full)
            used.add(full)
            if width == 8:
                expr = f"(ulonglong)({value})"
            elif width == 4:
                expr = f"(ulonglong)(uint)({value})"          # a 32-bit write clears the top half
            else:
                used.add(full)
                mask = (1 << (8 * width)) - 1
                expr = f"(({reg(full)} & ~{mask:#x}ULL) | ((ulonglong)({value}) & {mask:#x}))"
            lines.append(f"  {reg(full)} = {expr};")
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

    def keep_result(width: int, value: str):
        """Flags of a result: kept as the value, read as zero/sign tests."""
        name = f"f{fcount[0]}"
        fcount[0] += 1
        lines.append(f"  {CTYPE[width]} {name} = ({CTYPE[width]})({value});")
        flags.update(kind="result", r=name, width=width)

    def keep_compare(width: int, a: str, b: str):
        names = (f"f{fcount[0]}", f"f{fcount[0] + 1}")
        fcount[0] += 2
        lines.append(f"  {CTYPE[width]} {names[0]} = ({CTYPE[width]})({a});")
        lines.append(f"  {CTYPE[width]} {names[1]} = ({CTYPE[width]})({b});")
        flags.update(kind="compare", a=names[0], b=names[1], width=width)

    def condition(cc: str) -> str:
        if cc not in CONDITIONS or flags["kind"] is None:
            raise Unliftable(f"condition {cc}")
        zero_form, compare_form = CONDITIONS[cc]
        s = STYPE[flags["width"]]
        if flags["kind"] == "result":
            if zero_form is None:
                raise Unliftable(f"condition {cc} after a result")
            return "(" + zero_form.format(r=flags["r"], s=s) + ")"
        if compare_form is None:
            raise Unliftable(f"condition {cc} after a compare")
        return "(" + compare_form.format(a=flags["a"], b=flags["b"], s=s) + ")"

    for at in order:
        ins = found[at]
        if at in targets:
            lines.append(f"L_{at:x}:;")
        here = ins.address + ins.size
        mn, ops = ins.mnemonic, [x.strip() for x in ins.op_str.split(",")] if ins.op_str else []
        if mn in ("mov", "movabs", "movzx"):
            write(ops[0], read(ops[1]))
        elif mn in ("movsxd", "movsx"):
            src = read(ops[1])
            width = SIZE.get(ops[1].split()[0], R64.get(ops[1], (None, 4))[1])
            write(ops[0], f"(ulonglong)(longlong)({STYPE[width]})({src})")
        elif mn == "lea":
            m = re.fullmatch(r"(?:\w+ ptr )?\[(.+)\]", ops[1])
            if not m:
                raise Unliftable("lea operand")
            write(ops[0], address_of(m.group(1)))
        elif mn == "xor" and ops[0] == ops[1]:
            write(ops[0], "0")
            keep_result(width_of(ops[0]), "0")
        elif mn in ("add", "sub", "and", "or", "xor", "shr", "shl", "sar"):
            op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^", "shr": ">>", "shl": "<<", "sar": ">>"}[mn]
            width = width_of(ops[0])
            ctype = CTYPE[width] if mn != "sar" else STYPE[width]
            before = read(ops[0])
            value = f"(({ctype})({before}) {op} ({ctype})({read(ops[1])}))"
            if mn == "sub":
                keep_compare(width, before, read(ops[1]))
                write(ops[0], f"{flags['a']} - {flags['b']}")
            else:
                write(ops[0], value)
                keep_result(width, read(ops[0]))
        elif mn in ("inc", "dec"):
            width = width_of(ops[0])
            write(ops[0], f"({CTYPE[width]})({read(ops[0])}) {'+' if mn == 'inc' else '-'} 1")
            keep_result(width, read(ops[0]))
        elif mn == "test":
            width = width_of(ops[0])
            keep_result(width, f"{read(ops[0])} & {read(ops[1])}")
        elif mn == "cmp":
            width = width_of(ops[0])
            keep_compare(width, read(ops[0]), read(ops[1]))
        elif mn.startswith("cmov"):
            write(ops[0], f"({condition(mn[4:])} ? {read(ops[1])} : {read(ops[0])})")
        elif mn.startswith("set") and len(ops) == 1:
            write(ops[0], f"({condition(mn[3:])} ? 1 : 0)")
        elif mn == "lock cmpxchg":
            width = width_of(ops[0])
            if width not in (4, 8):
                raise Unliftable("cmpxchg width")
            m = re.fullmatch(r"(?:qword|dword) ptr \[(.+)\]", ops[0])
            if not m:
                raise Unliftable("cmpxchg operand")
            where = address_of(m.group(1))
            expected = read("rax" if width == 8 else "eax")
            value = read(ops[1])
            name = f"f{fcount[0]}"
            fcount[0] += 1
            if width == 8:
                lines.append(f"  ulonglong {name} = (ulonglong)_InterlockedCompareExchange64((volatile long long *)({where}), "
                             f"(long long)({value}), (long long)({expected}));")
            else:
                lines.append(f"  uint {name} = (uint)_InterlockedCompareExchange((volatile long *)({where}), "
                             f"(long)({value}), (long)({expected}));")
            keep_compare(width, expected, name)
            write("rax" if width == 8 else "eax", name)
        elif mn == "mul" and len(ops) == 1:
            width = width_of(ops[0])
            if width != 4:
                raise Unliftable("mul width")
            lines.append(f"  {{ ulonglong p = (ulonglong)(uint)({read('eax')}) * (ulonglong)(uint)({read(ops[0])});")
            lines.append(f"    {reg('rax')} = (uint)p; {reg('rdx')} = (uint)(p >> 32); }}")
            written.update(("rax", "rdx"))
            set_here.update(("rax", "rdx"))
            used.update(("rax", "rdx"))
        elif mn == "jmp":
            target = int(ops[0], 16)
            if target in found:
                lines.append(f"  goto L_{target:x};")
            else:
                callees.append(target)
                args = []
                for r in ARGS:
                    used.add(r)
                    reads.add(r)
                    args.append(reg(r))
                lines.append(f"  return FUN_{target:x}({', '.join(args)});")
        elif mn.startswith("j"):
            lines.append(f"  if {condition(mn[1:])} goto L_{int(ops[0], 16):x};")
        elif mn == "ret" and not ops:
            if "rax" in written:
                used.add("rax")
                lines.append(f"  return {reg('rax')};")
            else:
                lines.append("  return 0;")
        elif mn in ("nop", "int3"):
            pass
        else:
            raise Unliftable(f"instruction {mn} {ins.op_str}")
    # the registers it reads, the arguments from their own registers and the rest zero
    decls = []
    for full in sorted(used):
        init = f"a{ARGS.index(full)}" if full in reads and full in ARGS else "0"
        decls.append(f"  ulonglong {reg(full)} = {init};")
    return "\n".join(decls + lines), callees
