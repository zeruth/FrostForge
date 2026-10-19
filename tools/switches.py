"""MSVC x64 jump tables, read off the instructions: build/facts/switches.json.

A switch compiles to an image-relative table of 32-bit offsets, reached through the image base:

    cmp   ecx, N ; ja default
    lea   rdx, [rip - X]                       ; rdx = the image base
    movzx eax, byte ptr [rdx + rax + B]        ; (two-level: a byte index table first)
    mov   ecx, dword ptr [rdx + rax*4 + T]
    add   rcx, rdx
    jmp   rcx

Ghidra resolves most of these itself. Where it does not, the decompiler follows the jump into
nothing and gives up: FUN_1406303d0 (a 310-case two-level table) failed with "Flow exceeded
maximum allowable instructions" and was stubbed as not in the corpus (2026-10-16). ApplyFacts
writes each table found here as a jump-table override where Ghidra has no flows from the jump.

The case count comes from the bound check before it (`cmp r, N; ja`: N + 1 cases). Every
target must land inside the jump's own function (its .pdata pieces), or the table is dropped.

    python tools/switches.py   -> build/facts/switches.json
"""
from __future__ import annotations

import bisect

import capstone
from capstone import x86_const as X

from protos import corpus_sizes
from wowbin import Binary, hexa, write_facts


def wide(md, reg: int) -> str:
    """A general register's 64-bit name (`eax` -> `rax`, `r9d` -> `r9`)."""
    n = md.reg_name(reg)
    if n.startswith("r") and n[1:2].isdigit():
        return n.rstrip("dwb")
    return "r" + n[1:] if n.startswith("e") else n


def main() -> None:
    b = Binary()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    # (adjacent pieces of one function decode as one span: the bound check and the jump can sit
    # in different chained pieces, FUN_1402bfa60's `cmp eax, 6` before the piece that saves rsi
    # and its jump after, and a jump whose bound was out of reach was dropped unseen)
    spans = []
    for s, e, r in zip(b.piece_starts, b.piece_ends, b.piece_root):
        if spans and spans[-1][2] == r and spans[-1][1] == s:
            spans[-1] = (spans[-1][0], e, r)
        else:
            spans.append((s, e, r))
    # and the leaves, which have no .pdata entry (no frame to unwind): each corpus function no
    # piece covers, up to the next known start (FUN_1406303d0 is one, its table run from a leaf)
    # (to the next .pdata piece, not the next corpus function: where Ghidra lost the switch it
    # made functions of what the jumps reach, FUN_140630682 the default case of FUN_1406303d0)
    piece_lo = b.piece_starts
    for a in sorted(corpus_sizes()):
        p = bisect.bisect_right(piece_lo, a) - 1
        if p >= 0 and a < b.piece_ends[p]:
            continue
        nxt = piece_lo[p + 1] if p + 1 < len(piece_lo) else a + 0x1000
        if b.is_code(a):
            spans.append((a, min(nxt, a + 0x4000), a))
    spans.sort()
    starts = [s for s, _e, _r in spans]
    ends = [e for _s, e, _r in spans]
    roots = [r for _s, _e, r in spans]

    def root_of(va: int) -> int | None:
        i = bisect.bisect_right(starts, va) - 1
        return roots[i] if i >= 0 and va < ends[i] else None

    out: dict[str, list[str]] = {}
    tables: list[tuple[int, int]] = []
    one = two = dropped = 0
    for s, e, r in zip(starts, ends, roots):
        o = b.off(s)
        insns = list(md.disasm(bytes(b.image[o:o + (e - s)]), s))
        for k, j in enumerate(insns):
            if j.mnemonic != "jmp" or not j.operands or j.operands[0].type != X.X86_OP_REG:
                continue
            target_reg = j.operands[0].reg
            # walk back over the table load: add target, base / mov target32, [base + idx*4 + T]
            back = insns[max(0, k - 12):k][::-1]
            base_reg = table = idx_reg = None
            for i in back:
                if (i.mnemonic == "add" and len(i.operands) == 2 and i.operands[0].type == X.X86_OP_REG
                        and i.operands[0].reg == target_reg and i.operands[1].type == X.X86_OP_REG):
                    base_reg = i.operands[1].reg
                elif (i.mnemonic in ("mov", "movsxd") and base_reg is not None and len(i.operands) == 2
                      and i.operands[1].type == X.X86_OP_MEM and i.operands[1].mem.base == base_reg
                      and i.operands[1].mem.scale == 4 and table is None):
                    table = b.base + i.operands[1].mem.disp
                    idx_reg = i.operands[1].mem.index
                    break
            if table is None:
                continue
            # the base register is the image base (`lea base, [rip - X]` landing on it)
            # (the nearest write to it, however far back: a function with several switches loads
            # the base once in its prologue and keeps it in a saved register, `lea r12, [rip - X]`
            # some hundred instructions before FUN_14024d4f0's jump, and only the last 12 were
            # looked at, which left 590 tables unread)
            # (but not a write on an exit path that ends before the jump: the restore `mov rsi,
            # [rsp + 0x18]` before a `ret` between the prologue's lea and strpbrk's third table
            # (FUN_140272e20) hid the base, the table went unread, and tools/protos.py read the
            # `pslldq xmm2` ladder it jumps into as an xmm2 parameter, 2026-10-18)
            last = None
            ended = False
            for i in insns[max(0, k - 2000):k][::-1]:
                if i.mnemonic in ("ret", "jmp") or i.mnemonic.startswith("rep") and i.mnemonic.endswith("ret"):
                    ended = True
                    continue
                if (i.operands and i.operands[0].type == X.X86_OP_REG
                        and wide(md, i.operands[0].reg) == wide(md, base_reg)
                        and i.mnemonic not in ("cmp", "test", "push")):
                    if ended and not (i.mnemonic == "lea" and i.operands[1].type == X.X86_OP_MEM
                                      and i.operands[1].mem.base == X.X86_REG_RIP):
                        continue
                    last = i
                    break
            if last is None or not (last.mnemonic == "lea" and last.operands[1].mem.base == X.X86_REG_RIP
                                    and last.address + last.size + last.operands[1].mem.disp == b.base):
                continue
            # a byte index table in front (two-level), and the bound check
            bytes_at = None
            bound = None
            for i in back:
                if (i.mnemonic == "movzx" and len(i.operands) == 2 and i.operands[1].type == X.X86_OP_MEM
                        and i.operands[1].size == 1 and i.operands[1].mem.base == base_reg
                        and wide(md, i.operands[0].reg) == wide(md, idx_reg)):
                    bytes_at = b.base + i.operands[1].mem.disp
                if i.mnemonic == "cmp" and len(i.operands) == 2 and i.operands[1].type == X.X86_OP_IMM:
                    bound = i.operands[1].imm
                    break
            if bound is None:
                # (a later table on an index already bounded: FUN_14161e960 tests `cmp r8d, 0xf;
                # ja` once and jumps through four more tables on r8, or on `movsxd rax, r8d` and
                # `mov ebp, r8d` copies of it, the nearest compare well past the twelve looked at
                # above. Ghidra lost those tables, and the decompilation 29 of its 34 calls,
                # 2026-10-18. The index back through its register copies, to a `cmp` of one of
                # them with an `ja` after it; the targets are checked below as for any table)
                chain = {wide(md, idx_reg)}
                run = insns[max(0, k - 400):k]
                for p, i in reversed(list(enumerate(run))):
                    if (i.mnemonic in ("mov", "movsxd", "movzx") and len(i.operands) == 2
                            and i.operands[0].type == X.X86_OP_REG and i.operands[1].type == X.X86_OP_REG
                            and wide(md, i.operands[0].reg) in chain):
                        chain.add(wide(md, i.operands[1].reg))
                    elif (i.mnemonic == "cmp" and len(i.operands) == 2 and i.operands[0].type == X.X86_OP_REG
                          and i.operands[1].type == X.X86_OP_IMM and wide(md, i.operands[0].reg) in chain
                          and p + 1 < len(run) and run[p + 1].mnemonic == "ja"):
                        bound = i.operands[1].imm
                        break
            if bound is None or not (0 <= bound < 4096):
                continue
            n = bound + 1
            if bytes_at is not None:
                idx = [b.image[b.off(bytes_at) + c] for c in range(n)]
            else:
                idx = list(range(n))
            targets = []
            ok = True
            for c in sorted(set(idx)):
                t = b.base + b.u32(table + 4 * c)
                if root_of(t) != r and not s <= t < e:
                    ok = False
                    break
                targets.append(t)
            if not ok:
                dropped += 1
                continue
            # in case order (the override lists one destination per distinct target)
            seen, ordered = set(), []
            for c in idx:
                t = b.base + b.u32(table + 4 * c)
                if t not in seen:
                    seen.add(t)
                    ordered.append(t)
            out[hexa(j.address)] = [hexa(t) for t in ordered]
            # (and where the tables lie: they sit in .text, and decoded as code their bytes look
            # like jumps, tools/retcheck.py's jumps over junk among them)
            tables.append((table, table + 4 * (max(idx) + 1)))
            if bytes_at is not None:
                tables.append((bytes_at, bytes_at + n))
            if bytes_at is not None:
                two += 1
            else:
                one += 1
    path = write_facts("switches", {
        "rule": "an indirect jmp through an image-relative 32-bit table (optionally indexed by a byte table) "
                "after a bound check; every target inside the jump's own function",
        "jumps": out,
        "tables": [[hexa(lo), hexa(hi)] for lo, hi in sorted(set(tables))]})
    print(f"{len(out)} jump tables ({one} one-level, {two} two-level), {dropped} dropped -> {path}")


if __name__ == "__main__":
    main()
