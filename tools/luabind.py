"""Lua C function tables: arrays of { const char* name; lua_CFunction func; }.

Each 16-byte entry is a pointer to a NUL-terminated identifier and a pointer into
.text; a table is a run of at least two such entries, usually closed by a null pair.
The table's namespace (C_Container, or a global table) is read from the function
that registers it: it loads the table address and the namespace string together.

    python tools/luabind.py     -> build/facts/luabind.json
"""
from __future__ import annotations

import re
import struct
from collections import defaultdict

from wowbin import Binary, hexa, write_facts

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]{1,80}\Z")


def main() -> None:
    b = Binary()
    tables = []
    for sec in (".rdata", ".data"):
        s, d = b.bytes_of(sec)
        n = len(d) // 8
        q = struct.unpack_from(f"<{n}Q", d)

        def entry(i: int):
            if i + 1 >= n or not b.is_code(q[i + 1]):
                return None
            ss = b.section_of(q[i])
            if ss is None or ss.exec:
                return None
            name = b.cstr(q[i], 96)
            if name is None or not IDENT.match(name):
                return None
            return name, q[i + 1]

        i = 0
        while i < n - 1:
            e = entry(i)
            if e is None:
                i += 1
                continue
            j, entries = i, []
            while (e := entry(j)) is not None:
                entries.append(e)
                j += 2
            if len(entries) >= 2:
                tables.append({"table": s + 8 * i, "entries": entries,
                               "null_terminated": j + 1 < n and q[j] == 0 and q[j + 1] == 0})
            i = j if j > i else i + 1

    # namespace: a string loaded by a function that also loads the table address
    starts = {t["table"]: t for t in tables}
    loads_by_func = defaultdict(set)
    ns_like = set()
    rs, rd = b.bytes_of(".rdata")
    for m in re.finditer(rb"\0(C_[A-Za-z0-9]+|[A-Z][A-Za-z]+)\0", rd):
        ns_like.add(rs + m.start() + 1)
    for insn, tgt in b.riprel_refs(keep=lambda t: t in starts or t in ns_like):
        f = b.func_containing(insn)
        if f is not None:
            loads_by_func[f].add(tgt)
    for f, tgts in loads_by_func.items():
        hit = [t for t in tgts if t in starts]
        if not hit:
            continue
        names = sorted(n for n in (b.cstr(t, 64) for t in tgts) if n and re.fullmatch(r"C_[A-Za-z0-9]+|[A-Z][A-Za-z]+", n))
        for t in hit:
            starts[t].setdefault("registered_by", []).append(hexa(f))
            ns = [n for n in names if n.startswith("C_")]
            if ns:
                starts[t]["namespace"] = ns[0] if len(ns) == 1 else ns

    funcs = {}
    for t in tables:
        ns = t.get("namespace") if isinstance(t.get("namespace"), str) else None
        for name, fn in t["entries"]:
            funcs.setdefault(hexa(fn), f"{ns}.{name}" if ns else name)
    out = write_facts("luabind", {
        "tables": [{"table": hexa(t["table"]), "namespace": t.get("namespace"),
                    "registered_by": t.get("registered_by", []),
                    "null_terminated": t["null_terminated"],
                    "entries": [[nm, hexa(fn)] for nm, fn in t["entries"]]} for t in tables],
        "functions": funcs,
    })
    n_entries = sum(len(t["entries"]) for t in tables)
    with_ns = sum(1 for t in tables if isinstance(t.get("namespace"), str))
    print(f"{len(tables)} tables, {n_entries} entries, {len(funcs)} distinct functions; "
          f"{with_ns} tables with a C_ namespace -> {out}")
    for t in sorted(tables, key=lambda t: -len(t["entries"]))[:8]:
        print(f"  {hexa(t['table'])}  {len(t['entries']):4}  ns={t.get('namespace')}  first={t['entries'][0][0]}")


if __name__ == "__main__":
    main()
