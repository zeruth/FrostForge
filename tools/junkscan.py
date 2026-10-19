"""Where the clean exe still decodes as junk, by the decompiler's own markers.

The -clean exe has the Aegis junk nop'd, but some regions still decode as nonsense:
`halt_baddata()`, "Instruction at X overlaps instruction at Y", calls to addresses
inside other code (`func_0x...`), carry/borrow intrinsics on random registers. This
counts those markers per function across the corpus, so the cleaner can be pointed at
exactly the places it missed.

    python tools/junkscan.py           -> build/facts/junk.json, summary to stdout
"""
from __future__ import annotations

import re
from collections import Counter

from corpus import load_corpus, load_facts
from wowbin import write_facts

MARKERS = {
    "halt_baddata": re.compile(r"\bhalt_baddata\(\)"),
    "overlap": re.compile(r"Instruction at \(ram,0x[0-9a-f]+\) overlaps instruction"),
    "call_into_code": re.compile(r"\bfunc_0x[0-9a-f]+\("),
    "bad_flow": re.compile(r"Control flow encountered bad instruction data"),
}


# instructions no user-mode game code executes: decoded, they mark bytes that are no code
PRIVILEGED = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "hlt", "cli", "sti", "iret", "iretd",
              "iretq"}


def fragments(cor) -> dict[int, str]:
    """Corpus entries that are no function: the bytes between two functions, decoded.

    The image pads between functions with junk (and zeros), and Ghidra made a function of some
    of it: FUN_140ba06bb is the seven bytes after a `ret`, `push rax; add al, 0xab; add ecx, ebp;
    out dx, eax`, and its decompilation runs on into the next function, 0x140ba06c0, with the
    registers it reads `in_EAX` and `unaff_EBP` (seven stubs "register the decompiler lost track
    of" and one "bad data or a trap", 2026-10-18). Such an entry is no .pdata start and lies in no
    .pdata piece, and decoded from its start it never ends: it reaches the next .pdata start with
    no `ret` or `jmp` before it, or it decodes a privileged instruction, a zero word or nothing at
    all first. A real function without unwind data (a leaf) ends before the next one."""
    import bisect

    import capstone

    from wowbin import Binary

    b = Binary()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    out = {}
    for a in cor:
        if b.is_func_start(a) or b.func_containing(a) is not None or not b.is_code(a):
            continue
        i = bisect.bisect_right(b.piece_starts, a)
        nxt = b.piece_starts[i] if i < len(b.piece_starts) else b.text.end
        va, why = a, None
        while va < nxt:
            o = b.off(va)
            if va == a and bytes(b.image[o:o + 2]) == b"\0\0":
                why = "zero bytes"
                break
            ins = next(md.disasm(bytes(b.image[o:o + 16]), va, 1), None)
            if ins is None:
                why = "bytes that do not decode"
                break
            if ins.mnemonic in PRIVILEGED:
                why = f"a privileged instruction ({ins.mnemonic})"
                break
            # (`repz ret` as well as `ret`)
            if ins.mnemonic == "int3":
                why = "int3 padding before any return"
                break
            if ins.mnemonic.split()[-1] in ("ret", "retf", "ud2", "jmp"):
                break
            va += ins.size
        else:
            why = f"runs into the function at 0x{nxt:x}"
        if why:
            out[a] = why
    return out


def main() -> None:
    from corpus import strip_retcheck

    cor = load_corpus()
    frag = fragments(cor)
    rc = set(load_facts("retcheck").get("functions", {}))
    per_func = {}
    raw_totals, left_totals = Counter(), Counter()
    raw_funcs = 0
    for a, r in cor.items():
        c = r.get("c")
        if not c:
            continue
        raw = {k: len(p.findall(c)) for k, p in MARKERS.items()}
        if any(raw.values()):
            raw_funcs += 1
            for k, v in raw.items():
                if v:
                    raw_totals[k] += 1
        # what is left once the anti-tamper checks are stripped: Ghidra's warnings live
        # in the header comment and describe the raw function, so count only the
        # markers that appear in code
        stripped, status = strip_retcheck(c)
        code = "\n".join(l for l in stripped.split("\n") if not l.lstrip().startswith(("/*", "*")))
        left = {k: len(p.findall(code)) for k, p in MARKERS.items() if k != "overlap"}
        left = {k: v for k, v in left.items() if v}
        if left:
            per_func[f"0x{a:x}"] = {"name": r["name"], **left, "retcheck": f"0x{a:x}" in rc,
                                    "strip": status}
            for k in left:
                left_totals[k] += 1
    for a, why in frag.items():
        e = per_func.setdefault(f"0x{a:x}", {"name": cor[a]["name"], "retcheck": f"0x{a:x}" in rc, "strip": None})
        e["fragment"] = why
        left_totals["fragment"] += 1
    out = write_facts("junk", {"markers": list(MARKERS) + ["fragment"], "functions": per_func})
    print(f"raw decompilations: {raw_funcs} of {len(cor)} functions show junk markers "
          + ", ".join(f"{k} {n}" for k, n in raw_totals.most_common()))
    with_rc = sum(1 for v in per_func.values() if v["retcheck"])
    print(f"after stripping the checks: {len(per_func)} functions still decode as junk -> {out}")
    for k, n in left_totals.most_common():
        print(f"  {n:6}  with {k}")
    print(f"  {with_rc} of them carry the anti-tamper check (junk inside a check the stripper left)")


if __name__ == "__main__":
    main()
