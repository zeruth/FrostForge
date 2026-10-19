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

from corpus import load_facts
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
    # (the check's junk is short: of 1,458 spans none lies between 0xc0 and 0x3c0 bytes. What is
    # longer rests only on a linear decode out of step, and was a real branch over real code:
    # 0x14198e978, `test rax, rax; je T` over 6 KB of Lua::PickupPetAction, 2026-10-18. The
    # always-taken jumps below are proven by their flags and keep any length)
    long = {a: t for a, t in junk.items() if t - a > 0x200}
    print(f"{len(long)} jumps in the checks dropped as too long to be over junk")
    for a in long:
        del junk[a]
    opaque = opaque_jumps(b)
    print(f"{len(opaque)} always-taken jumps over junk, {len(set(opaque) - set(junk))} outside the checks")
    flowed = opaque_jumps_flow(b)
    print(f"{len(flowed)} more found following the flow, {len(set(flowed) - set(opaque) - set(junk))} new")
    for a, t in flowed.items():
        opaque.setdefault(a, t)
    junk.update(opaque)
    # (the always-taken ones are proven by their flags; only the checks' jumps rest on a decode)
    bad = skipping_reached_code(b, junk, proven=set(opaque))
    print(f"{len(bad)} jumps dropped: other code reaches the bytes they skip "
          f"({', '.join(f'{a:x}->{junk[a]:x}' for a in sorted(bad)[:8])})")
    for a in bad:
        del junk[a]
    # (and none inside a jump table: its entries decode as code too, and a `0x73` byte of
    # FUN_1417354c0's table taken for a `jae` over junk was patched to `jmp`, so the decompiler,
    # which reads the table from the program's bytes, sent case 0x11 into FUN_141eb53b0 and the
    # function was stubbed on that code's registers, 2026-10-18. tools/switches.py's tables)
    tables = [(int(lo, 16), int(hi, 16)) for lo, hi in load_facts("switches").get("tables", [])]
    in_tables = {a for a in junk if any(lo <= a < hi for lo, hi in tables)}
    print(f"{len(in_tables)} jumps dropped: inside a jump table")
    for a in in_tables:
        del junk[a]
    # (the outermost of the two sets together: one may sit inside the other's junk)
    # (a jump whose junk ends before its target spans only the junk: the code after is real)
    spans = sorted((a, JUNK_ENDS.get(a, t)) for a, t in junk.items())
    junk = {a: t for a, t in junk.items() if not any(j < a < u for j, u in spans if j != a)}
    # (and the loads a linear decode of the piece is out of step at: past junk the cleaner left,
    # every later instruction boundary is off, and the second half of Lua::SendChatMessage's 24
    # checks were rejected as mid-instruction, left in the decompilation, one in a shape the text
    # stripper does not know, and the handler stubbed, 2026-10-18. Such a load counts when it
    # decodes as the check's 7-byte `mov` there; success_branches then wants both its ways to T)
    import capstone as _cs
    _md = _cs.Cs(_cs.CS_ARCH_X86, _cs.CS_MODE_64)
    verified = {i for i, t in refs if t == RANGE_BASE}
    loose = set()
    for i, t in b.riprel_refs(keep=lambda t: t == RANGE_BASE, verify=False):
        if i in verified:
            continue
        ins = next(_md.disasm(bytes(b.image[b.off(i):b.off(i) + 16]), i), None)
        if ins is not None and ins.mnemonic == "mov" and ins.size == 7 and b.func_containing(i) is not None:
            loose.add(i)
    checks = success_branches(b, sorted(verified | loose), junk)
    print(f"{sum(1 for s in checks if s in loose)} checks past a linear decode out of step")
    refs2 = b.riprel_refs(keep=lambda t: t == RANGE2_BASE)
    checks2 = call_check_branches(b, sorted(i for i, _t in refs2))
    print(f"{len(checks2)} of {len(refs2)} checks on the second range pass straight on")
    checks.update(checks2)
    out = write_facts("retcheck", {
        "range_globals": [hexa(RANGE_BASE), hexa(RANGE_LEN)],
        "functions": {hexa(f): hexa(i) for f, i in sorted(first.items())},
        "junk_jumps": {hexa(j): hexa(t) for j, t in sorted(junk.items())},
        # (where the junk a jump skips ends before its target: ApplyFacts clears only up to there)
        "junk_ends": {hexa(j): hexa(JUNK_ENDS[j]) for j in sorted(junk) if j in JUNK_ENDS},
        "checks": {hexa(s): hexa(t) for s, t in sorted(checks.items())},
    })
    n = sum(1 for i, t in refs if t == RANGE_BASE)
    print(f"{len(first)} functions carry the return-address check, {len(junk)} jumps over junk, "
          f"{len(checks)} of {n} checks with their success branch -> {out}")


_PIECES = None


def all_pieces(b: Binary) -> list[tuple[int, int, int]]:
    """(start, end, root) of every piece of code to scan: the .pdata pieces, and each function
    the corpus has that no piece covers, as one piece of its own. Those are the leaves
    tools/protos.py found reached only through a pointer, which have no unwind entry (a leaf needs
    none): 0x14165a4a0 and 0x14165a510, callbacks behind `stc; jbe` and `xor al, 0; jno` over junk,
    were never scanned, kept their junk, and were stubbed as damaged (2026-10-19)."""
    global _PIECES
    if _PIECES is not None:
        return _PIECES
    import bisect
    import json
    from corpus import CORPUS
    out = list(zip(b.piece_starts, b.piece_ends, b.piece_root))
    spans = sorted((s, e) for s, e, _r in out)
    lo = [s for s, _e in spans]
    if CORPUS.exists():
        with CORPUS.open(encoding="utf-8") as f:
            for line in f:
                head = line[:120]
                i, j = head.find('"addr":"'), head.find('"size":')
                if i < 0 or j < 0:
                    continue
                a = int(head[i + 8:head.index('"', i + 8)], 16)
                size = int("".join(c for c in head[j + 7:j + 20].split(",")[0].split("}")[0] if c.isdigit()) or 0)
                k = bisect.bisect_right(lo, a) - 1
                if size <= 0 or (k >= 0 and a < spans[k][1]) or not b.is_code(a):
                    continue
                out.append((a, a + size, a))
    _PIECES = out
    return out


def piece_root(b: Binary, a: int) -> int | None:
    """The function a piece of all_pieces holds `a` in: .pdata's, else a corpus leaf's."""
    global _EXTRA
    r = b.func_containing(a)
    if r is not None:
        return r
    if _EXTRA is None:
        import bisect
        n = len(b.piece_starts)
        extra = sorted(all_pieces(b)[n:])
        _EXTRA = ([s for s, _e, _r in extra], extra, bisect)
    lo, extra, bisect = _EXTRA
    k = bisect.bisect_right(lo, a) - 1
    return extra[k][2] if k >= 0 and a < extra[k][1] else None


_EXTRA = None


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
    for s, e, _r in all_pieces(b):
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
        # and the pair whose first jump goes to the same condition again, `jg X; xchg ch, ch;
        # jle T` with `X: jg T` inside the junk the second skips: X is reached only when the
        # condition holds, so its jump is always taken, and the second jump only when it does
        # not, so that one is too. Every path goes to T. The decompiler cannot tie the two tests
        # of one flag together and followed X's fall-through into the junk: CreateFrame's core,
        # FUN_1424db710, after its lua_pushfstring (0x1424db8d1), kept as protection junk,
        # 2026-10-19. The junk the second jump skips ends at X, which stays
        for k, (a, size, mn, ops) in enumerate(insns):
            if mn not in opposite:
                continue
            try:
                x = int(ops, 16)
            except ValueError:
                continue
            j = k + 1
            while j < len(insns) and insns[j][2] in neutral:
                j += 1
            if j >= len(insns) or insns[j][2] != opposite[mn]:
                continue
            a2, size2, _m2, ops2 = insns[j]
            try:
                t = int(ops2, 16)
            except ValueError:
                continue
            if not (a2 + size2 < x < t < e):
                continue
            at_x = next(md.disasm_lite(code[x - s:x - s + 16], x), None)
            if at_x is None or at_x[2] != mn or at_x[3] != ops2:
                continue
            out[a2] = (t, size2)
            out[x] = (t, at_x[1])
            JUNK_ENDS[a2] = x
    ranges = sorted((a + size, JUNK_ENDS.get(a, t)) for a, (t, size) in out.items())
    return {a: t for a, (t, size) in out.items()
            if not any(lo <= a < hi for lo, hi in ranges if (lo, hi) != (a + size, JUNK_ENDS.get(a, t)))}


def skipping_reached_code(b: Binary, jumps: dict[int, int], proven: set[int] = frozenset()) -> set[int]:
    """The jumps over "junk" whose skipped bytes the function's flow reaches some other way: real
    code, which ApplyFacts would clear. The linear decodes that find them can run out of step
    through real code, and a real jump in there looked like one over junk: 0x14193bb05 skipped
    12 KB of Lua::CastSpellByID, eight such spans in all, and each of those handlers came out
    damaged (2026-10-18). The flow follows every jump's target, and never the fall-through of one
    of these (that is the junk), from the function's pieces."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    roots: dict[int, list[int]] = {}
    for a in jumps:
        r = piece_root(b, a)
        if r is not None:
            roots.setdefault(r, []).append(a)
    pieces: dict[int, list[tuple[int, int]]] = {}
    for s, e, r in all_pieces(b):
        if r in roots:
            pieces.setdefault(r, []).append((s, e))
    bad = set()
    for r, js in roots.items():
        ps = pieces[r]

        def inside(x):
            return any(s <= x < e for s, e in ps)

        reached: set[int] = set()
        todo, seen = [s for s, _e in ps], set()
        while todo:
            a = todo.pop()
            while inside(a) and a not in seen:
                seen.add(a)
                o = b.off(a)
                ins = next(md.disasm_lite(bytes(b.image[o:o + 16]), a), None)
                if ins is None:
                    break
                _x, size, mn, ops = ins
                reached.update(range(a, a + size))
                if mn in ("ret", "int3", "ud2", "hlt") or (mn == "jmp" and not ops.startswith("0x")):
                    break
                if mn.startswith("j") or mn.startswith("loop"):
                    try:
                        t = int(ops, 16)
                    except ValueError:
                        t = None
                    if mn == "jmp":
                        a = t
                        continue
                    if t is not None:
                        todo.append(t)
                    if a in jumps:
                        break
                a += size
        for a in js:
            o = b.off(a)
            ins = next(md.disasm_lite(bytes(b.image[o:o + 16]), a), None)
            size = ins[1] if ins else 2
            if any(x in reached for x in range(a + size, JUNK_ENDS.get(a, jumps[a]))):
                bad.add(a)
                continue
            # and junk ends inside an instruction that spans the target: decoded from the jump's
            # own fall-through, real code lands on it (0x14198e978, `test rax, rax; je T` over
            # 6 KB of Lua::PickupPetAction, its decode from the piece start out of step)
            if a in proven:
                continue    # (a short opaque jump can skip junk that happens to decode onto T)
            x, t = a + size, jumps[a]
            o = b.off(x)
            for _y, sz, _m, _o in md.disasm_lite(bytes(b.image[o:o + (t - x) + 16]), x):
                if x >= t:
                    break
                x += sz
            if x == t:
                bad.add(a)
    return bad


OPPOSITE = {}
for _x, _y in [("jo", "jno"), ("jb", "jae"), ("je", "jne"), ("jbe", "ja"), ("js", "jns"), ("jp", "jnp"),
               ("jl", "jge"), ("jle", "jg"), ("jc", "jnc"), ("jz", "jnz"), ("jnb", "jnae")]:
    OPPOSITE[_x], OPPOSITE[_y] = _y, _x


def opaque_jumps_flow(b: Binary) -> dict[int, int]:
    """opaque_jumps, decoding along the flow from each function's pieces rather than straight
    through them. A linear decode that runs into junk can stay out of step for hundreds of bytes
    of real code after it, and the always-taken jump there is never seen: Lua::CastSpellByID's
    `stc; xchg ah, ah; jb T` (0x14193d5f4), whose fall-through into junk left the function
    damaged, and ten more Lua handlers with it (2026-10-18). The flow never takes an opaque
    jump's fall-through, and a jump counts only if nothing else the flow reaches lies in the
    bytes it skips: those are junk, and ApplyFacts clears them."""
    # Each instruction carries the conditions known true on every path into it, the flags
    # untouched since they were set: `clc` makes jae/jnb/jnc so, `stc` jb/jc/jnae and jbe/jna (CF
    # is enough for those: Lua::PostAuction's `stc; jbe T`), a test, and, or or xor clears CF and
    # OF; a taken jump makes its own condition true at its target, and its fall-through the
    # opposite one. An instruction that leaves the flags alone keeps them, anything else forgets
    # them, and where paths meet only what both know is kept. A conditional jump whose condition
    # is known is always taken, and its fall-through never followed. That covers the second of
    # two opposite jumps (`jl X; ...; jge Y`, Lua::AutoStoreGuildBankItem: past the first, the
    # second's condition holds) and a jump repeated at the first one's target (`clc; jae T` with
    # another jae at T, Lua::GetMacroItemIcons), which the patterns before did not (2026-10-18).
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    neutral = {"mov", "lea", "nop", "push", "pop", "xchg", "movzx", "movsx", "movsxd", "bswap", "not"}
    shifts = ("shl", "shr", "sal", "sar", "rol", "ror")
    sets = {"clc": frozenset(("jae", "jnb", "jnc")),
            "stc": frozenset(("jb", "jc", "jnae", "jbe", "jna")),
            **{m: frozenset(("jae", "jnb", "jnc", "jno")) for m in ("test", "and", "or", "xor")}}
    pieces: dict[int, list[tuple[int, int]]] = {}
    for s, e, r in all_pieces(b):
        pieces.setdefault(r, []).append((s, e))
    decoded: dict[int, tuple] = {}

    def decode(x):
        if x not in decoded:
            o = b.off(x)
            decoded[x] = next(md.disasm_lite(bytes(b.image[o:o + 16]), x), None)
        return decoded[x]

    out: dict[int, int] = {}
    for root, ps in pieces.items():
        decoded.clear()
        hi = max(e for _s, e in ps)

        def inside(x):
            return any(s <= x < e for s, e in ps)

        state: dict[int, frozenset] = {}
        todo = [(s, frozenset()) for s, _e in ps]
        while todo:
            a, known = todo.pop()
            if not inside(a):
                continue
            old = state.get(a)
            if old is not None:
                known = old & known
                if known == old:
                    continue
            state[a] = known
            ins = decode(a)
            if ins is None:
                continue
            _x, size, mn, ops = ins
            if mn in ("ret", "int3", "ud2", "hlt") or (mn == "jmp" and not ops.startswith("0x")):
                continue
            if mn == "jmp":
                todo.append((int(ops, 16), known))
                continue
            if mn.startswith("j") or mn.startswith("loop"):
                t = int(ops, 16) if ops.startswith("0x") else None
                if t is not None:
                    todo.append((t, known | {mn}))
                if mn in known and t is not None:
                    continue        # always taken: no fall-through
                opp = OPPOSITE.get(mn)
                todo.append((a + size, (known | {opp}) if opp else known))
                continue
            if mn in sets:
                after = sets[mn]
            elif mn in neutral or mn.startswith("cmov") or (mn in shifts and ops.endswith(", 0")):
                after = known
            else:
                after = frozenset()
            todo.append((a + size, after))
        # the jumps always taken, and what the flow reaches without their fall-throughs
        found = {}
        for a, known in state.items():
            ins = decode(a)
            if ins and ins[2].startswith("j") and ins[2] != "jmp" and ins[2] in known and ins[3].startswith("0x"):
                t = int(ins[3], 16)
                if a + ins[1] < t < hi:
                    found[a] = (t, ins[1])
        reached: set[int] = set()
        for a in state:
            ins = decode(a)
            if ins:
                reached.update(range(a, a + ins[1]))
        for a, (t, size) in found.items():
            hit = next((x for x in range(a + size, t) if x in reached), None)
            if hit is None:
                out[a] = t
            elif hit > a + size:
                # real code before the target (the block the opposite jump before it goes to):
                # the junk is only what lies before that
                out[a] = t
                JUNK_ENDS[a] = hit
    return out


# the always-taken jumps whose junk ends before their target (opaque_jumps_flow): jump -> end
JUNK_ENDS: dict[int, int] = {}


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
        t = None
        # (to the next check, or past it when the table walk two checks share lies beyond it:
        # 0x1418fdf00 and 0x1418fe070 end in the same `cmp eax, 0x40`; the decode walk below still
        # has to agree)
        for limit in (min(0x2000, nxt - s), 0x2000):
            win = bytes(b.image[o:o + limit])
            k = win.find(b"\x83\xf8\x40")
            while k >= 0 and t is None:
                if win[k + 3:k + 5] == b"\x0f\x86":
                    t = s + k + 9 + struct.unpack_from("<i", win, k + 5)[0]
                elif win[k + 3] == 0x76:
                    t = s + k + 5 + struct.unpack_from("<b", win, k + 4)[0]
                else:
                    k = win.find(b"\x83\xf8\x40", k + 1)
            if t is not None:
                break
        win = bytes(b.image[o:o + 16])
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
