"""Disassemble the clean exe at an address (llvm-objdump cannot read it).

    python tools/disasm.py 0x1402cb0d0            # the whole function (its .pdata extent)
    python tools/disasm.py 0x1402cb0d0 0x40       # 0x40 bytes from the address

RIP-relative operands get their target printed, with the C string there when there is one.
"""
import argparse

import capstone

from wowbin import Binary


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", type=lambda s: int(s, 16))
    ap.add_argument("length", nargs="?", type=lambda s: int(s, 16))
    args = ap.parse_args()

    b = Binary()
    start = args.addr
    if args.length is None:
        import bisect
        i = bisect.bisect_right(b.piece_starts, start) - 1
        end = b.piece_ends[i] if i >= 0 and start < b.piece_ends[i] else None
        length = (end - start) if end else 0x200
    else:
        length = args.length

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    code = b.image[b.off(start):b.off(start) + length]
    for insn in md.disasm(code, start):
        note = ""
        if "rip" in insn.op_str:
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    target = insn.address + insn.size + op.mem.disp
                    s = b.cstr(target, 80) if b.section_of(target) else None
                    note = f"   ; {target:#x}" + (f" {s!r}" if s else "")
        print(f"{insn.address:#x}  {insn.mnemonic:8} {insn.op_str}{note}")


if __name__ == "__main__":
    main()
