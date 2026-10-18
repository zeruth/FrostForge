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

from wowbin import regs_access

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


def _pieces(binary) -> dict[int, list[tuple[int, int]]]:
    """Each root function's pieces, (start, end), from the binary's .pdata chains (cached)."""
    cached = getattr(binary, "_callargs_pieces", None)
    if cached is None:
        cached = {}
        for s, e, r in zip(binary.piece_starts, binary.piece_ends, binary.piece_root):
            cached.setdefault(r, []).append((s, e))
        binary._callargs_pieces = cached
    return cached


class _Function:
    def __init__(self, binary, start: int, size: int):
        # (the root piece to its .pdata end: Ghidra's size stops where its body did, which in a
        # guarded function is the guard. The tick FUN_14167c520 is 0x3098 bytes in .pdata and 1837
        # in the corpus, and its every access past that (the byte flags at 0x14311c529/52a) read
        # as none, so an untyped flag kept the 8-byte default, 2026-10-17)
        for piece, end in _pieces(binary).get(start, []):
            if piece == start and end - start > size:
                size = end - start
        o = binary.off(start)
        self.ins = list(_md.disasm(bytes(binary.image[o:o + size]), start))
        # the function's other pieces (cold paths the compiler moved out of line): a protobuf
        # message's ByteSize keeps its varint-size calls there, so without them every call site
        # was outside the function and 117 callers of FUN_14222d2d0 read as calls with their
        # argument lost
        for piece, end in _pieces(binary).get(start, []):
            if start <= piece < start + size:
                continue
            o = binary.off(piece)
            self.ins += list(_md.disasm(bytes(binary.image[o:o + end - piece]), piece))
        self.index = {i.address: k for k, i in enumerate(self.ins)}
        self.targets = set()
        self.sources: dict[int, list[int]] = {}
        for k, i in enumerate(self.ins):
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == x86.X86_OP_IMM:
                self.targets.add(i.operands[0].imm)
                self.sources.setdefault(i.operands[0].imm, []).append(k)
        self.pinned = self._pinned()
        self.rbp_delta = None   # how far below the entry rsp the prologue points rbp
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
                # a frame pointer set up beside rsp (rsp itself does not move): where it points
                if i.mnemonic == "mov" and ops[1].type == x86.X86_OP_REG and i.reg_name(ops[1].reg) == "rsp":
                    self.rbp_delta = delta
                elif i.mnemonic == "lea" and ops[1].type == x86.X86_OP_MEM and i.reg_name(ops[1].mem.base) == "rsp" \
                        and ops[1].mem.index == 0:
                    self.rbp_delta = delta - ops[1].mem.disp
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
            _, written = regs_access(i)
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
                _, written = regs_access(i)
                writes += any(FAMILY.get(i.reg_name(r)) == reg for r in written)
            if writes == 1:
                out[reg] = (k, value)
        return out

    def writes(self, k: int, reg: str) -> bool:
        i = self.ins[k]
        if i.group(capstone.CS_GRP_CALL):
            return reg not in CALLEE_SAVED
        _, written = regs_access(i)
        return any(FAMILY.get(i.reg_name(r)) == reg for r in written)

    def returned_exits(self, reg: str) -> list[int]:
        """The exits that return `reg` (`mov rax, reg`, then only the epilogue and the ret)."""
        out = []
        for k, i in enumerate(self.ins):
            ops = i.operands
            if i.mnemonic != "mov" or len(ops) != 2 or ops[0].type != x86.X86_OP_REG or ops[1].type != x86.X86_OP_REG:
                continue
            if i.reg_name(ops[0].reg) != "rax" or i.reg_name(ops[1].reg) != reg:
                continue
            for nxt in self.ins[k + 1:k + 8]:
                if nxt.group(capstone.CS_GRP_RET):
                    out.append(k)
                    break
                if nxt.mnemonic not in ("pop", "add") or nxt.group(capstone.CS_GRP_JUMP):
                    break
                if nxt.mnemonic == "add" and nxt.op_str.split(",")[0] != "rsp":
                    break
        return out

    def same_as_returned(self, k: int, reg: str) -> bool:
        """Whether `reg` just before instruction k holds what an exit returns: walking back from
        k with `reg` untouched reaches instructions every way back from a `mov rax, reg; ret`
        exit also reaches with it untouched (the tail call and the plain return of a protobuf
        serializer, which leave the output pointer where it was)."""
        shared = set()
        for p in self.path(k):
            if p in ("entry", "split"):
                break
            if self.writes(p, reg):
                break
            shared.add(p)
        if not shared:
            return False
        for exit_ in self.returned_exits(reg):
            todo = [exit_]
            seen = set()
            ok = True
            while todo and ok:
                p = todo.pop()
                if p in seen:
                    continue
                seen.add(p)
                if len(seen) > 200:
                    ok = False
                    break
                for q in self.predecessors(p):
                    if q in shared:
                        continue
                    if self.writes(q, reg) or q == 0:
                        ok = False
                        break
                    todo.append(q)
                if not self.predecessors(p):
                    ok = False
            if ok:
                return True
        return False

    def stack_slot(self, base: str, disp: int, j: int) -> str | None:
        """The local at [base + disp] in the body, by Ghidra's naming: local_N is N bytes below
        the entry stack pointer, local_resN N above it (the caller's home space)."""
        if j < self.body_start:
            return None
        if base == "rsp" and self.frame is not None:
            o = disp - self.frame
        elif base == "rbp" and self.rbp_delta is not None:
            o = disp - self.rbp_delta
        else:
            return None
        return f"local_res{o:x}" if o > 0 else f"local_{-o:x}"

    def predecessors(self, p: int) -> list[int]:
        """The instructions that can run just before instruction p."""
        out = [q for q in self.sources.get(self.ins[p].address, []) if q != p]
        prev = self.ins[p - 1] if p > 0 else None
        if (prev is not None and prev.mnemonic != "jmp" and not prev.group(capstone.CS_GRP_RET)
                and prev.address + prev.size == self.ins[p].address):
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
                if reg == "rax" and i.operands and i.operands[0].type == x86.X86_OP_IMM:
                    # what a direct call returned: the translator names it by the variable the
                    # decompilation assigns from that call (`lVar1 = FUN_x(...)`), when there is
                    # one; a tail jump through a field of it lost its argument
                    # (FUN_1414ac6f0: `jmp FUN_1414e7fa0` with rcx = [rax + 0x3a8])
                    return f"RET(0x{i.operands[0].imm & MASK[8]:x})"
                if reg not in CALLEE_SAVED:
                    return None         # a call clobbers the volatile registers
                return before(q, False, visiting, budget)
            if not i.group(capstone.CS_GRP_JUMP):
                writes_memory = any(op.type == x86.X86_OP_MEM and op.access & capstone.CS_AC_WRITE
                                    for op in i.operands)
                _, written = regs_access(i)
                if any(FAMILY.get(i.reg_name(r)) == reg for r in written):
                    return self._evaluate(q, i, reg, floor, depth, clean)
                if writes_memory:
                    # a store into this function's own frame (a spill to [rsp + d] below the entry
                    # stack pointer) cannot change a global: loads of globals stay good ("stack");
                    # any other store spoils every load (ssl's shutdown FUN_141dfb5d0 spills rbx
                    # between loading DAT_143264bc0 into rcx and the call that takes it)
                    own = self.frame is not None and all(
                        op.mem.base != 0 and i.reg_name(op.mem.base) == "rsp" and op.mem.index == 0
                        and 0 <= op.mem.disp < self.frame
                        for op in i.operands if op.type == x86.X86_OP_MEM and op.access & capstone.CS_AC_WRITE)
                    clean = "stack" if own and clean else False
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
                # a 32-bit move zero-extends: the low half of the value (protobuf's
                # `mov ecx, eax` of a uint field into the varint size call)
                return src if width == 8 else f"(uint)({src})" if width == 4 else None
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
            if clean is not True:
                return None     # only a stack store came between: good for globals alone
            slot = self.stack_slot(i.reg_name(mem.base), mem.disp, j)
            if slot is not None and size == 8:
                # a stack slot: the local Ghidra names by its offset from the entry stack pointer
                return slot
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
            slot = self.stack_slot(i.reg_name(mem.base), mem.disp, j)
            if slot is not None and width == 8:
                # the address of a local (a struct or buffer the callee fills)
                return f"&{slot}"
            base = self.value(j, FAMILY.get(i.reg_name(mem.base), ""), floor, depth + 1)
            if isinstance(base, str) and width == 8:
                # an address formed from a symbolic one (a parameter + a field offset): in bytes,
                # whatever C type the parameter has
                return f"(undefined8)((char *)({base}) + {mem.disp:#x})" if mem.disp else base
            return None if not isinstance(base, int) else (base + mem.disp) & MASK[width]
        return None


class CallArgs:
    """Recovered constant arguments per function, cached."""

    # (the decoded functions kept, most recent last: a translator run asks about one function at a
    # time, so a few hundred hold every repeat. Kept without bound, every function decoded stayed
    # as capstone instructions, and a run over the whole corpus (83k functions) ran the machine
    # out of memory in emission, 2026-10-17)
    KEEP = 512

    def __init__(self, binary):
        self.binary = binary
        from collections import OrderedDict
        self._functions: "OrderedDict[int, _Function]" = OrderedDict()
        self._thunks: dict[int, tuple[int, dict[str, int]] | None] = {}   # thunk -> (target, sets)
        self._accesses: dict[int, dict[int, set[tuple[int, str]]]] = {}

    def _function(self, function: int, size: int) -> "_Function":
        f = self._functions.get(function)
        if f is None:
            f = self._functions[function] = _Function(self.binary, function, size)
            if len(self._functions) > self.KEEP:
                self._functions.popitem(last=False)
        else:
            self._functions.move_to_end(function)
        return f

    def indirect_calls(self, function: int, size: int) -> int:
        """How many calls the function makes through a register or memory (imports included)."""
        f = self._function(function, size)
        # (a call through a register or frame slot that holds a function's address taken in this
        # function, `lea rax, [rip + x]; mov [rsp + 0x28], rax; ... call [rsp + 0x28]`, is one the
        # decompiler resolves and shows by the callee's name: FUN_1414c51e0's call to the CFG
        # no-op `_guard_check_icall` that way was counted lost, 2026-10-17)
        regs: dict[int, int] = {}
        slots: dict[int, int] = {}
        n = 0
        for i in f.ins:
            ops = i.operands
            if i.mnemonic == "call":
                if ops and ops[0].type != x86.X86_OP_IMM:
                    o = ops[0]
                    known = (o.type == x86.X86_OP_REG and o.reg in regs) or (
                        o.type == x86.X86_OP_MEM and o.mem.base == x86.X86_REG_RSP and o.mem.index == 0
                        and o.mem.disp in slots)
                    n += not known
                regs.clear()
                continue
            if i.mnemonic == "lea" and len(ops) == 2 and ops[1].mem.base == x86.X86_REG_RIP:
                regs[ops[0].reg] = i.address + i.size + ops[1].mem.disp
                continue
            if i.mnemonic == "mov" and len(ops) == 2 and ops[0].type == x86.X86_OP_MEM \
                    and ops[0].mem.base == x86.X86_REG_RSP and ops[0].mem.index == 0:
                if ops[1].type == x86.X86_OP_REG and ops[1].reg in regs:
                    slots[ops[0].mem.disp] = regs[ops[1].reg]
                else:
                    slots.pop(ops[0].mem.disp, None)
                continue
            if ops and ops[0].type == x86.X86_OP_REG:
                regs.pop(ops[0].reg, None)
            if i.group(x86.X86_GRP_JUMP) or i.mnemonic.startswith(("push", "pop", "sub", "add")) and ops \
                    and ops[0].type == x86.X86_OP_REG and ops[0].reg == x86.X86_REG_RSP:
                slots.clear()
        return n

    def global_accesses(self, function: int, size: int) -> dict[int, set[tuple[int, str]]]:
        """Each RIP-relative address the function reads or writes, with how: (bytes, "x") for a
        scalar SSE instruction (its operand size tells float from double: `cvtss2sd` reads 4
        bytes), "i" otherwise. `lea` takes an address without accessing it and is left out."""
        # (the translator asks once per global a function names, all together: the last answer)
        if function in self._accesses:
            return self._accesses[function]
        f = self._function(function, size)
        out: dict[int, set[tuple[int, str]]] = {}
        self._accesses = {function: out}
        for i in f.ins:
            if i.mnemonic == "lea":
                continue
            for op in i.operands:
                if op.type != x86.X86_OP_MEM or op.mem.base != x86.X86_REG_RIP:
                    continue
                kind = "x" if i.mnemonic.endswith(("ss", "sd")) else "i"
                out.setdefault(i.address + i.size + op.mem.disp, set()).add((op.size, kind))
        return out

    def calls(self, i, target: int) -> bool:
        """Whether instruction i calls (or tail-jumps to) `target`, directly or through a thunk
        (a function that is only `jmp target`, or one that first sets argument registers to
        constants), as Ghidra's thunk_FUN_ names show it."""
        return self.thunk_sets(i, target) is not None

    def thunk_sets(self, i, target: int) -> dict[str, int] | None:
        """For an instruction that calls `target` (directly or through a thunk): the argument
        registers the thunk sets before its jump, {} for a direct call or a plain thunk; None
        when i does not call `target`.

        A setter thunk is `mov dl, 1; jmp target` (FUN_14216a000 before FUN_14216a080): Ghidra
        calls it a thunk of target and drops what it sets, so the call reads short by those
        arguments. The constants are the thunk's own, the same at every call through it."""
        if i.mnemonic not in ("call", "jmp") or not i.operands or i.operands[0].type != x86.X86_OP_IMM:
            return None
        imm = i.operands[0].imm
        if imm == target:
            return {}
        if imm in self._thunks:
            t = self._thunks[imm]
            return t[1] if t and t[0] == target else None
        self._thunks[imm] = None
        try:
            o = self.binary.off(imm)
        except Exception:
            return None
        sets: dict[str, int] = {}
        for j in _md.disasm(bytes(self.binary.image[o:o + 32]), imm):
            if j.mnemonic == "jmp" and j.operands and j.operands[0].type == x86.X86_OP_IMM:
                self._thunks[imm] = (j.operands[0].imm, sets)
                break
            ops = j.operands
            if len(ops) != 2 or ops[0].type != x86.X86_OP_REG:
                break
            reg = FAMILY.get(j.reg_name(ops[0].reg))
            if reg not in ARG_REGS:
                break
            if j.mnemonic == "mov" and ops[1].type == x86.X86_OP_IMM:
                sets[reg] = ops[1].imm & ((1 << (8 * ops[0].size)) - 1)
            elif j.mnemonic == "xor" and ops[1].type == x86.X86_OP_REG and ops[1].reg == ops[0].reg:
                sets[reg] = 0
            else:
                break
        t = self._thunks[imm]
        return t[1] if t and t[0] == target else None

    def returned_at_every_call(self, function: int, size: int, target: int, position: int) -> bool:
        """Whether, at every call to `target`, argument `position` holds what the function
        returns on another exit (Function.same_as_returned)."""
        if position >= len(ARG_REGS):
            return False
        f = self._function(function, size)
        sites = [k for k, i in enumerate(f.ins) if self.calls(i, target)]
        return bool(sites) and all(f.same_as_returned(k, ARG_REGS[position]) for k in sites)

    def recover(self, function: int, size: int, target: int, positions: list[int], every: list | None = None,
                tail: bool = False):
        """The constants every call from `function` to `target` passes in the argument
        `positions` (0-3), when all calls agree; None when any is unknown or they differ.
        With `every` (a list), each call's values are appended to it instead, unknowns as None,
        and the list is returned. With `tail`, only the tail calls (`jmp target`) count, and
        there must be exactly one."""
        if any(p >= len(ARG_REGS) for p in positions):
            return None
        f = self._function(function, size)
        if tail and sum(1 for i in f.ins if i.mnemonic == "jmp" and self.thunk_sets(i, target) is not None) != 1:
            return None
        found = None
        for k, i in enumerate(f.ins):
            sets = self.thunk_sets(i, target)
            if sets is None or (tail and i.mnemonic != "jmp"):
                continue
            values = [sets[ARG_REGS[p]] if ARG_REGS[p] in sets else f.value(k, ARG_REGS[p], 0) for p in positions]
            if every is not None:
                every.append(values)
                continue
            if any(v is None for v in values):
                return None
            if found is not None and values != found:
                return None
            found = values
        return every if every is not None else found
