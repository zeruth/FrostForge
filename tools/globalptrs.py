"""Writable globals that hold a pointer the code reads or writes through: build/facts/globalptrs.json.

The data export leaves most 8-byte globals `undefined8`, and the decompiler, seeing one loaded
and dereferenced, prints `*DAT_x` or `DAT_x[2]` with no pointer type to spell it with: 13
functions were stubbed as "a global used as a pointer the data export does not type", among
them static objects called through their vtable (`(*(code *)*DAT_142e303f0)(&DAT_142e303f0)`).
ApplyFacts types each global listed here as `undefined *`, a byte pointer: the decompiler then
prints every access through it at its own width (`*(undefined4 *)(DAT_x + 4)`), which is exactly
what the instructions do, and no element width is guessed.

A global qualifies when some instruction loads it whole into a register (`mov rax, [rip + X]`)
and an instruction after it in the same block, before the register is written again, addresses
memory through that register. Only .data globals the export has not typed are changed, in
ApplyFacts.

    python tools/globalptrs.py
"""
from __future__ import annotations

from collections import Counter

import capstone
from capstone import x86_const as X

from wowbin import Binary, hexa, regs_access, write_facts

BRANCH = ("j", "call", "ret", "int3", "ud2", "hlt")


def main() -> None:
    b = Binary()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    writable = [s for s in b.sections if not s.exec and s.name in (".data", ".bss")]

    def is_data(va: int) -> bool:
        return any(s.start <= va < s.end for s in writable)

    found: Counter = Counter()
    for s, e, _r in zip(b.piece_starts, b.piece_ends, b.piece_root):
        o = b.off(s)
        insns = list(md.disasm(bytes(b.image[o:o + (e - s)]), s))
        for k, i in enumerate(insns):
            if i.mnemonic != "mov" or len(i.operands) != 2:
                continue
            dst, src = i.operands
            if not (dst.type == X.X86_OP_REG and src.type == X.X86_OP_MEM and src.size == 8
                    and src.mem.base == X.X86_REG_RIP and src.mem.index == 0):
                continue
            g = i.address + i.size + src.mem.disp
            if not is_data(g):
                continue
            reg = dst.reg
            for j in insns[k + 1:k + 13]:
                if j.mnemonic.startswith(BRANCH) and not j.mnemonic.startswith("call"):
                    break
                if any(op.type == X.X86_OP_MEM and reg in (op.mem.base, op.mem.index) for op in j.operands) \
                        and j.mnemonic != "lea":
                    found[g] += 1
                    break
                if j.mnemonic.startswith("call"):
                    # (a call through it is a deref too: `call qword ptr [rax]`, checked above)
                    break
                _reads, writes = regs_access(j)
                if reg in writes:
                    break
    path = write_facts("globalptrs", {
        "rule": "an 8-byte .data/.bss global loaded into a register that the same block then "
                "addresses memory through: typed `undefined *` by ApplyFacts where untyped",
        "globals": {hexa(g): n for g, n in sorted(found.items())}})
    print(f"{len(found)} globals dereferenced as pointers -> {path}")


if __name__ == "__main__":
    main()
