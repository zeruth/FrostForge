"""Variadic functions, from the va_start spill in their decompilation.

On x64 a variadic function's prologue stores its register arguments past the fixed ones to
their home slots, so the va_list can walk on into the stack arguments: `fn(L, fmt, ...)`
decompiles with four parameters and the body `local_res18 = param_3; local_res20 = param_4;`
then `&local_res18` handed on as the va_list. Ghidra's Decompiler Parameter ID commits all
four as fixed parameters, and every caller then passes whatever its own r8/r9 held: the Lua
error routine FUN_142424d80 (`luaG_runerror`) was called as `(L, "stack overflow",
unaff_retaddr, in_R9)` and 450 callers were stubbed as reading a lost register (2026-10-15).

A function is variadic here when its trailing parameters param_K..param_4 are each
stored to their own home slot (param_K at local_res(8*K)), the address of the first slot is
taken, and the later slots are never read again. Its fixed parameters are param_1..param_(K-1).
ApplyFacts marks them varargs with that many parameters and locks the signature, so the next
commit pass (tools/ghidra/CommitPrototypes.java) leaves them alone. A function the corpus
already shows with `...` keeps its entry, so the fact survives the re-export it causes.

    python tools/varargs.py   -> build/facts/varargs.json
"""
from __future__ import annotations

import re

from corpus import load_corpus, load_facts
from wowbin import write_facts

_SPILL = re.compile(r"^\s*(local_res([0-9a-f]+)) = param_(\d+);\s*$", re.M)
_DECL = re.compile(r"^\s*[\w\s\*]+\blocal_res[0-9a-f]+(?:\s*\[[^\]]*\])?;\s*$", re.M)


def fixed_params(c: str) -> int | None:
    head, _, body = c.partition("{")
    # va_start spills every register slot past the fixed ones, through r9, so the run ends at
    # param_4; a function that takes the address of one of its parameters (the CRT's
    # _register_onexit_function hands &param_2 to a lambda) spills only that one
    n = len(re.findall(r"\bparam_\d+\b", head.split("(", 1)[-1]))
    if n != 4:
        return None
    spill = {int(k): (slot, int(off, 16)) for slot, off, k in _SPILL.findall(body)}
    if n not in spill:
        return None
    k = n
    while k - 1 in spill and k - 1 > 1:
        k -= 1
    if any(spill[j][1] != 8 * j for j in range(k, n + 1)):
        return None
    code = _DECL.sub("", body)
    if not re.search(rf"&{spill[k][0]}\b", code):
        return None
    if any(len(re.findall(rf"\b{spill[j][0]}\b", code)) != 1 for j in range(k + 1, n + 1)):
        return None
    return k - 1


def main() -> None:
    old = load_facts("varargs").get("functions", {})
    out = {}
    for a, r in load_corpus().items():
        c = r.get("c")
        if not c:
            continue
        key = f"0x{a:x}"
        fixed = fixed_params(c)
        if fixed is not None:
            out[key] = fixed
        elif key in old and "..." in c.partition("{")[0]:
            out[key] = old[key]
    path = write_facts("varargs", {"functions": dict(sorted(out.items()))})
    print(f"{len(out)} variadic functions -> {path}")


if __name__ == "__main__":
    main()
