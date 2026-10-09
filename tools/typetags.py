"""Type tags: which functions name which classes.

Nothing points at an RTTI type descriptor, but code loads its *name string*
(descriptor + 16) and passes it as a type tag, mostly to the allocator:
    FUN_142430eb0(&heap, 0, ".?AUAnimEventData@CGUnit_C@@", ...)
3.5k class names at ~14.6k sites. For each site this records the class, the function
containing it, and the first call after the load (the consumer of the tag: allocator,
container constructor, ...). A function that allocates exactly one class is usually
that class's factory or a method of its owner.

    python tools/typetags.py    -> build/facts/typetags.json
"""
from __future__ import annotations

import ctypes
import re
from collections import Counter, defaultdict

import capstone

from wowbin import Binary, hexa, write_facts

_undname = ctypes.WinDLL("dbghelp").UnDecorateSymbolName
_undname.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
_undname.restype = ctypes.c_uint32


def demangle(raw: str) -> str:
    """'.?AUAnimEventData@CGUnit_C@@' -> 'CGUnit_C::AnimEventData' (via the vftable symbol)."""
    if not raw.startswith((".?AV", ".?AU")):
        return raw
    buf = ctypes.create_string_buffer(2048)
    if _undname(("??_7" + raw[4:] + "6B@").encode(), buf, len(buf), 0) == 0:
        return raw
    return buf.value.decode(errors="replace").removeprefix("const ").removesuffix("::`vftable'")


def main() -> None:
    b = Binary()
    names = {}
    for sec in (".data", ".rdata"):
        s, d = b.bytes_of(sec)
        for m in re.finditer(rb"\.\?A[VU][A-Za-z0-9_@?$]+@@\0", d):
            names[s + m.start()] = m.group()[:-1].decode()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    sites = []
    for insn, tgt in b.riprel_refs(keep=names.__contains__):
        if tgt not in names:
            continue
        f = b.func_containing(insn)
        if f is None:
            continue
        # first direct call after the load, within a few instructions
        consumer = None
        o = b.off(insn)
        for ins in md.disasm(bytes(b.image[o : o + 96]), insn):
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                consumer = int(ins.op_str, 16)
                break
            if ins.mnemonic in ("ret", "jmp"):
                break
        sites.append((insn, f, tgt, consumer))

    cls_name = {t: demangle(r) for t, r in names.items()}
    by_func = defaultdict(set)
    by_class = defaultdict(set)
    consumers = Counter()
    for insn, f, t, c in sites:
        by_func[f].add(cls_name[t])
        by_class[cls_name[t]].add(f)
        if c is not None:
            consumers[c] += 1

    out = write_facts("typetags", {
        "sites": [[hexa(i), hexa(f), cls_name[t], hexa(c) if c else None] for i, f, t, c in sites],
        "functions": {hexa(f): sorted(v) for f, v in sorted(by_func.items())},
        "classes": {k: sorted(hexa(f) for f in v) for k, v in sorted(by_class.items())},
        "consumers": {hexa(c): n for c, n in consumers.most_common(50)},
    })
    single = sum(1 for v in by_func.values() if len(v) == 1)
    print(f"{len(sites)} sites, {len(by_class)} classes, {len(by_func)} functions "
          f"({single} name exactly one class) -> {out}")
    print("top tag consumers (allocators and friends):")
    for c, n in consumers.most_common(6):
        print(f"  {hexa(c)}  {n}")
    for k in ("CGUnit_C::AnimEventData", "WOWGUID", "CGPlayer_C", "CGUnit_C"):
        if k in by_class:
            print(f"  {k}: {len(by_class[k])} functions")


if __name__ == "__main__":
    main()
