"""Functions carrying the anti-tamper return-address check.

The clean exe still has a protection tail appended to ~3.3k functions (1,433 Lua handlers
and 1,901 others): it compares the caller's return address with a code range held in
two globals (base 0x14261a0b0, length 0x14261a0b8) and looks it up in a table at
0x1400b9810; a failed check crashes in a deliberate loop. It is protection, not game
logic, and must not be ported. This lists each such function and the first
instruction of its check, so the decompile export and the recomp can skip it.

    python tools/retcheck.py    -> build/facts/retcheck.json
"""
from __future__ import annotations

from wowbin import Binary, hexa, write_facts

RANGE_BASE = 0x14261A0B0
RANGE_LEN = 0x14261A0B8
# a second pair (base 0x142618df8, length 0x142618e00), read by 29 functions: the same range and
# call-instruction tests, without the table walk
RANGE2_BASE = 0x142618DF8


def main() -> None:
    b = Binary()
    # riprel_refs verifies each kept site is a real instruction boundary; on this one
    # global the raw byte scan had 101 mid-instruction matches (2026-10-08)
    unverified = len(b.riprel_refs(keep=lambda t: t in (RANGE_BASE, RANGE_LEN), verify=False))
    first: dict[int, int] = {}
    refs = b.riprel_refs(keep=lambda t: t in (RANGE_BASE, RANGE_LEN))
    for insn, _tgt in refs:
        f = b.func_containing(insn)
        if f is not None and (f not in first or insn < first[f]):
            first[f] = insn
    print(f"{unverified - len(refs)} pattern matches rejected as mid-instruction")
    junk = junk_jumps(b, first)
    opaque = opaque_jumps(b)
    print(f"{len(opaque)} always-taken jumps over junk, {len(set(opaque) - set(junk))} outside the checks")
    junk.update(opaque)
    # (the outermost of the two sets together: one may sit inside the other's junk)
    spans = sorted(junk.items())
    junk = {a: t for a, t in spans if not any(j < a < u for j, u in spans if j != a)}
    checks = success_branches(b, sorted(i for i, t in refs if t == RANGE_BASE), junk)
    refs2 = b.riprel_refs(keep=lambda t: t == RANGE2_BASE)
    checks2 = call_check_branches(b, sorted(i for i, _t in refs2))
    print(f"{len(checks2)} of {len(refs2)} checks on the second range pass straight on")
    checks.update(checks2)
    out = write_facts("retcheck", {
        "range_globals": [hexa(RANGE_BASE), hexa(RANGE_LEN)],
        "functions": {hexa(f): hexa(i) for f, i in sorted(first.items())},
        "junk_jumps": {hexa(j): hexa(t) for j, t in sorted(junk.items())},
        "checks": {hexa(s): hexa(t) for s, t in sorted(checks.items())},
    })
    n = sum(1 for i, t in refs if t == RANGE_BASE)
    print(f"{len(first)} functions carry the return-address check, {len(junk)} jumps over junk, "
          f"{len(checks)} of {n} checks with their success branch -> {out}")


def opaque_jumps(b: Binary) -> dict[int, int]:
    """Always-taken jumps over junk anywhere in the code, not only in the checks: `test al,
    0x63; shl al, 0; jno T` (test clears OF, a shift by 0 leaves the flags) with junk before T,
    which the decode runs across. After a call in a Lua handler (FUN_1407d0a90) the decompiler
    followed the fall-through into the junk and gave up on the function. A test, and, or or xor
    clears CF and OF, and the instructions allowed between leave the flags alone, so jae/jnb/jno
    there is always taken: made unconditional it means the same, whatever the bytes around it."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.skipdata = True
    clear = {"test", "and", "or", "xor"}
    neutral = {"mov", "lea", "nop", "push", "pop", "xchg", "movzx", "movsx", "bswap", "not"}
    shifts = ("shl", "shr", "sal", "sar", "rol", "ror")
    # the other forms (2026-10-16, two functions Ghidra decoded with overlapping instructions,
    # FUN_1417b3a80 and FUN_142261a50): the carry set or cleared outright, `clc; jae T` / `stc;
    # jb T`, and a pair of opposite jumps to one target, `jo T; mov bl, bl; jno T`, which together
    # always go to T. For the pair the first jump is the one made unconditional
    carry = {"clc": ("jae", "jnb", "jnc"), "stc": ("jb", "jc", "jnae")}
    opposite = {}
    for x, y in [("jo", "jno"), ("jb", "jae"), ("je", "jne"), ("jbe", "ja"), ("js", "jns"), ("jp", "jnp"),
                 ("jl", "jge"), ("jle", "jg"), ("jc", "jnc"), ("jz", "jnz"), ("jnb", "jnae")]:
        opposite[x], opposite[y] = y, x
    out: dict[int, tuple[int, int]] = {}
    for s, e, _r in zip(b.piece_starts, b.piece_ends, b.piece_root):
        o = b.off(s)
        code = bytes(b.image[o:o + (e - s)])
        insns = list(md.disasm_lite(code, s))
        starts = {a for a, _z, _m, _o in insns}
        for k, (a, size, mn, ops) in enumerate(insns):
            if not mn.startswith("j") or mn == "jmp":
                continue
            try:
                t = int(ops, 16)
            except ValueError:
                continue
            if not (a + size < t < e) or t in starts:
                continue
            j = k - 1
            while j >= 0 and (insns[j][2] in neutral or insns[j][2].startswith("cmov")
                              or (insns[j][2] in shifts and insns[j][3].endswith(", 0"))):
                j -= 1
            first, fsize = a, size
            if mn in ("jno", "jae", "jnb") and j >= 0 and insns[j][2] in clear:
                pass
            elif j >= 0 and insns[j][2] in carry and mn in carry[insns[j][2]]:
                pass
            elif (j >= 0 and insns[j][2] == opposite.get(mn) and insns[j][3] == ops
                  and all(insns[x][2] in neutral for x in range(j + 1, k))):
                first, fsize = insns[j][0], insns[j][1]
            else:
                continue
            clean = list(md.disasm_lite(code[t - s:t - s + 64], t))
            if len(clean) >= 4 and sum(z for _a, z, _m, _o in clean) >= 16:
                out[first] = (t, fsize)
    ranges = sorted((a + size, t) for a, (t, size) in out.items())
    return {a: t for a, (t, size) in out.items()
            if not any(lo <= a < hi for lo, hi in ranges if (lo, hi) != (a + size, t))}


def call_check_branches(b: Binary, starts: list[int]) -> dict[int, int]:
    """The second range's checks: in range, and the return address follows a call instruction,
    is all they test (`cmp dil, 0xe8; je T`, a direct call; else the indirect call forms, falling
    through to T past the last `je` to the crash). So T is where the code resumes. These left
    `_UNK_142618df8 <= unaff_retaddr` in 29 decompilations, a few stubbed for registers the
    check's crash paths read (2026-10-16)."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    out = {}
    for s in starts:
        o = b.off(s)
        insns = [i for i in md.disasm(bytes(b.image[o:o + 0x400]), s) if i.mnemonic != "nop"]
        if not insns or insns[0].mnemonic != "mov" or insns[0].size != 7:
            continue
        for k in range(len(insns) - 1):
            i, j = insns[k], insns[k + 1]
            if (i.mnemonic == "cmp" and len(i.operands) == 2 and i.operands[1].type == capstone.x86.X86_OP_IMM
                    and i.operands[1].imm & 0xff == 0xE8 and i.operands[0].size == 1 and j.mnemonic == "je"):
                t = int(j.op_str, 16)
                if b.func_containing(t) == b.func_containing(s):
                    out[s] = t
                break
    return out


def success_branches(b: Binary, starts: list[int], junk: dict[int, int]) -> dict[int, int]:
    """Each check's start (its load of the range base) -> where the code goes on when the return
    address passes. Every check ends its table walk in `sub eax, edx; cmp eax, 0x40; jbe T`, and
    every other way out of it is a crash loop, so T is where the function's own code resumes.
    ApplyFacts makes the start a jump to T: the check is unreachable and the decompiler drops it.
    Stripped from the decompiled text instead (corpus.strip_retcheck), only the shapes its pattern
    knew came out, and 143 functions were stubbed for the rest (2026-10-16).

    T is found by bytes (83 f8 40, then jbe rel32 or rel8) between this check and the next, and
    confirmed by decoding forward from the start through the junk jumps; the two agreed on 5,579
    of 5,581 checks both found. A check where they disagree, or whose start is not the 7-byte
    `mov rdx, [rip + base]` a jump fits over, is left to the text stripper."""
    import struct

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out = {}
    for n, s in enumerate(starts):
        o = b.off(s)
        nxt = starts[n + 1] if n + 1 < len(starts) else s + 0x2000
        win = bytes(b.image[o:o + min(0x2000, nxt - s)])
        t = None
        k = win.find(b"\x83\xf8\x40")
        while k >= 0 and t is None:
            if win[k + 3:k + 5] == b"\x0f\x86":
                t = s + k + 9 + struct.unpack_from("<i", win, k + 5)[0]
            elif win[k + 3] == 0x76:
                t = s + k + 5 + struct.unpack_from("<b", win, k + 4)[0]
            else:
                k = win.find(b"\x83\xf8\x40", k + 1)
        if t is None or b.func_containing(t) != b.func_containing(s):
            continue
        first = next(md.disasm(win[:16], s), None)
        if first is None or first.mnemonic != "mov" or first.size != 7:
            continue
        # the decode walk: through the junk jumps to the first `cmp r, imm; jbe`
        va, prev, walked = s, None, None
        for _ in range(6000):
            if va in junk:
                va = junk[va]
                continue
            o = b.off(va)
            ins = next(md.disasm(bytes(b.image[o:o + 16]), va), None)
            if ins is None or ins.mnemonic in ("ret", "int3"):
                break
            if (ins.mnemonic == "jbe" and prev is not None and prev.mnemonic == "cmp"
                    and prev.operands[1].type == capstone.x86.X86_OP_IMM):
                walked = int(ins.op_str, 16)
                break
            prev = ins
            va += ins.size
        if walked is not None and walked != t:
            continue
        out[s] = t
    return out


def junk_jumps(b: Binary, funcs: dict[int, int]) -> dict[int, int]:
    """The check's conditional jumps whose fall-through is junk: `test ecx, ecx; jne T` with
    bytes after it that are no code, which a linear decode reads straight across T (the last junk
    instruction spans the jump's own target). Ghidra follows the fall-through into them, and the
    decompilation of 155 such functions, Lua handlers most, came out with overlapping
    instructions and was stubbed (2026-10-16). ApplyFacts makes each of these jumps
    unconditional (a flow override) and clears the junk, so the real code at T is what is
    decompiled. Only where the decode from T is clean and the jump is forward, inside the check."""
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    pieces: dict[int, list[tuple[int, int]]] = {}
    for s, e, r in zip(b.piece_starts, b.piece_ends, b.piece_root):
        pieces.setdefault(r, []).append((s, e))
    out = {}
    for f, check in funcs.items():
        for s, e in pieces.get(f, []):
            if e <= check:
                # (the game code before the check is left as it is: a desynced decode there
                # would make real code look like junk)
                continue
            o = b.off(s)
            code = bytes(b.image[o:o + (e - s)])
            insns = list(md.disasm_lite(code, s))
            starts = {a for a, _sz, _m, _o in insns}
            for a, size, mnem, ops in insns:
                if not mnem.startswith("j") or mnem == "jmp":
                    continue
                try:
                    t = int(ops, 16)
                except ValueError:
                    continue
                if a < check or not (a + size < t < e) or t in starts:
                    continue
                # the target sits inside an instruction of the linear decode: the bytes between
                # are not code. The decode from the target itself must be sound for a while
                clean = list(md.disasm_lite(code[t - s:t - s + 64], t))
                if len(clean) >= 4 and sum(sz for _a, sz, _m, _o in clean) >= 16:
                    out[a] = (t, size)
    # a "jump" decoded inside the bytes another one skips is junk itself (`jno` at 0x1407e27d6,
    # within 0x1407e27c3's jump over 0x1407e27c9..0x1407e2849): applied, it decoded the junk again
    # and the overlap came back (2026-10-16). Only the outermost jumps are kept
    ranges = sorted((a + size, t) for a, (t, size) in out.items())
    return {a: t for a, (t, size) in out.items()
            if not any(lo <= a < hi for lo, hi in ranges if (lo, hi) != (a + size, t))}


if __name__ == "__main__":
    main()
