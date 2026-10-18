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


def lift(binary, address: int, limit: int = 48, arity=None) -> str | None:
    """The code at `address` as a function in the decompilation's own C, or None. `arity(target)`,
    when given, is how many arguments a tail-called function reads: the stack ones are passed on
    too (the lifted code never moves rsp, so they are where its own caller put them)."""
    try:
        body, _ = _lift(binary, address, limit, arity)
    except Unliftable:
        return None
    # the four register arguments, and the stack arguments it reads
    count = max([4] + [int(n) + 1 for n in re.findall(r"\ba(\d+)\b", body)])
    params = ",".join(f"undefined8 a{i}" for i in range(count))
    return (f"/* lifted from the instructions (tools/lift.py) */\n"
            f"undefined8 FUN_{address:x}({params})\n\n"
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
                if re.fullmatch(r"qword ptr \[(?!rip)[^\]]+\]", ins.op_str):
                    break   # a tail call through memory: a vcall thunk, blz::function's invoker
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


def _lift(binary, address: int, limit: int, arity=None):
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
    stack_args: set[int] = set()  # stack arguments read (a4 on)
    set_here: set[str] = set()    # registers the code itself has set
    # The flags live in three variables every flag-setting instruction writes (`fa`, `fb` a
    # compare's operands, `fr` a result), so a jump carries its own path's flags to the label it
    # lands on. What is known statically is only their kind and width, which every path into a
    # label must agree on. (Kept as fresh names per instruction, a label read the flags of the
    # instruction textually before it: FUN_14168eff0's `ja` after `jne` from a 64-bit compare of
    # the high halves tested the low halves' compare that path never ran, C4700, 2026-10-17)
    flags = {"kind": None}        # the kind and width of what the flag variables hold here
    incoming: dict[int, list[tuple]] = {}   # label -> the flag states jumps bring to it
    fallthrough = [True]          # whether the previous instruction can run into this one
    fcount = [0]
    callees = []

    def state() -> tuple:
        return (flags.get("kind"), flags.get("width"))

    # the label whose incoming flags the code since it still depends on, and the labels whose
    # code read flags before setting any: only those care what a jump back brings
    since_label = [None]
    label_reads: set[int] = set()

    def arrive(target: int):
        incoming.setdefault(target, []).append(state())

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
            # a stack argument (the fifth on), where the caller left it: rsp never moves here
            s = re.fullmatch(r"rsp \+ (0x[0-9a-f]+)", m.group(2))
            if s:
                disp = int(s.group(1), 16)
                if disp < 0x28 or (disp - 0x28) % 8 != 0:
                    raise Unliftable(f"stack slot {m.group(2)}")
                index = 4 + (disp - 0x28) // 8
                stack_args.add(index)
                return f"(({CTYPE[SIZE[m.group(1)]]})a{index})"
            return f"(*({CTYPE[SIZE[m.group(1)]]} *)({address_of(m.group(2))}))"
        raise Unliftable(f"operand {op}")

    def address_of(expr: str) -> str:
        m = re.fullmatch(r"rip ([+-]) (0x[0-9a-f]+)", expr)
        if m:
            target = here + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            # named as the decompilation names a global, so the translator places it (the
            # image's copy, or the port's when the port holds it)
            return f"((ulonglong)&DAT_{target:x})"
        # an index scaled with no base (`lea ecx, [rcx*4]`)
        m = re.fullmatch(r"([a-z]\w*)\*(\d)(?: ([+-]) (0x[0-9a-f]+|\d+))?", expr)
        if m and m.group(1) in R64 and R64[m.group(1)][1] == 8 and m.group(1) not in ("rsp", "rbp"):
            out = f"({read(m.group(1))} * {m.group(2)})"
            if m.group(3):
                out = f"({out} {m.group(3)} {int(m.group(4), 0):#x})"
            return out
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
        lines.append(f"  fr = (ulonglong)({CTYPE[width]})({value});")
        flags.update(kind="result", r=f"(({CTYPE[width]})fr)", width=width)
        since_label[0] = None

    def keep_compare(width: int, a: str, b: str):
        # (both operands are read before either is stored: `cmp rax, [rax]`'s b reads rax)
        lines.append(f"  {{ ulonglong ca = (ulonglong)({CTYPE[width]})({a}); ulonglong cb = (ulonglong)({CTYPE[width]})({b}); "
                     f"fa = ca; fb = cb; }}")
        flags.update(kind="compare", a=f"(({CTYPE[width]})fa)", b=f"(({CTYPE[width]})fb)", width=width)
        since_label[0] = None

    def condition(cc: str) -> str:
        if since_label[0] is not None:
            label_reads.add(since_label[0])
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

    def set_state(st: tuple):
        kind, width = st
        flags.clear()
        flags["kind"] = kind
        if kind is not None:
            flags["width"] = width
            if kind == "result":
                flags["r"] = f"(({CTYPE[width]})fr)"
            else:
                flags["a"], flags["b"] = f"(({CTYPE[width]})fa)", f"(({CTYPE[width]})fb)"

    at_label: dict[int, tuple] = {}
    for at in order:
        ins = found[at]
        if at in targets:
            lines.append(f"L_{at:x}:;")
            # the flags here are whichever path came: known only when every path agrees on their
            # kind and width; otherwise unknown, which only a condition that reads them refuses
            paths = set(incoming.get(at, [])) | ({state()} if fallthrough[0] else set())
            set_state(next(iter(paths)) if len(paths) == 1 else (None, None))
            at_label[at] = state()
            since_label[0] = at
        elif not fallthrough[0]:
            set_state((None, None))
        fallthrough[0] = True
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
        elif mn == "jmp" and ops[0].startswith("qword ptr ["):
            # a tail call through a pointer in memory (`mov rax,[rcx]; jmp [rax+8]`, the thunk
            # a member-function pointer to a virtual is: GlueLogin hands the Battle.net client
            # its listener's slots this way), the argument registers passed on as they stand
            where = address_of(ops[0][len("qword ptr ["):-1])
            args = []
            for r in ARGS:
                used.add(r)
                reads.add(r)
                args.append(reg(r))
            lines.append(f"  return (**(code **)({where}))({', '.join(args)});")
            fallthrough[0] = False
        elif mn == "jmp":
            target = int(ops[0], 16)
            fallthrough[0] = False
            if target in found:
                arrive(target)
                lines.append(f"  goto L_{target:x};")
            else:
                callees.append(target)
                args = []
                for r in ARGS:
                    used.add(r)
                    reads.add(r)
                    args.append(reg(r))
                # and the stack arguments the target reads, this function's own fifth on: a
                # `this` adjuster in front of a seven-argument function passed four, and the
                # translator stubbed it for the lost ones (FUN_141297730, 39 thunks, 2026-10-18)
                need = arity(target) if arity else None
                args += [f"a{i}" for i in range(4, need or 4)]
                lines.append(f"  return FUN_{target:x}({', '.join(args)});")
        elif mn.startswith("j"):
            arrive(int(ops[0], 16))
            lines.append(f"  if {condition(mn[1:])} goto L_{int(ops[0], 16):x};")
        elif mn == "ret" and not ops:
            fallthrough[0] = False
            if "rax" in written:
                used.add("rax")
                lines.append(f"  return {reg('rax')};")
            else:
                lines.append("  return 0;")
        elif mn in ("nop", "int3"):
            pass
        else:
            raise Unliftable(f"instruction {mn} {ins.op_str}")
    # (a jump backwards, to a label already lifted, must bring the flags that label assumed)
    for t, paths in incoming.items():
        st = at_label.get(t)
        if st is not None and t in label_reads and any(p != st for p in paths):
            raise Unliftable("a jump back brings other flags than its label assumed")
    # the registers it reads, the arguments from their own registers and the rest zero
    decls = ["  ulonglong fa = 0, fb = 0, fr = 0;"]
    for full in sorted(used):
        init = f"a{ARGS.index(full)}" if full in reads and full in ARGS else "0"
        decls.append(f"  ulonglong {reg(full)} = {init};")
    return "\n".join(decls + lines), callees
