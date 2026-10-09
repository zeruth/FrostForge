"""Counts the naming anchors a binary carries, to decide which Ghidra tooling pays
off first: RTTI class names, source-file paths (assert/trace strings), Lua API
names, DB2 table names.

    python tools/name_anchors.py <exe>
"""
import re
import sys
from collections import Counter

import pefile


def main() -> None:
    pe = pefile.PE(sys.argv[1], fast_load=True)
    rdata = b"".join(s.get_data() for s in pe.sections
                     if s.Name.rstrip(b"\0") in (b".rdata", b".data", b"_RDATA"))

    rtti = re.findall(rb"\.\?A[VU]([A-Za-z0-9_@?$]+)@@", rdata)
    print(f"RTTI type descriptors: {len(rtti)}")
    for name in sorted(set(r.decode() for r in rtti))[:15]:
        print(f"  {name}")

    paths = re.findall(rb"[A-Za-z]:\\[\w\\. -]{4,200}?\.(?:cpp|h|hpp|c|inl)", rdata, re.I)
    paths += re.findall(rb"(?:\.\./|Source[\\/])[\w\\/. -]{4,200}?\.(?:cpp|h|hpp|c|inl)", rdata, re.I)
    print(f"\nsource-file path strings: {len(paths)} ({len(set(paths))} distinct)")
    dirs = Counter()
    for p in set(paths):
        parts = re.split(rb"[\\/]", p)
        dirs[b"/".join(parts[-3:-1]).decode(errors="replace")] += 1
    for d, n in dirs.most_common(25):
        print(f"  {n:5}  {d}")

    # Lua C API names registered by the client: CamelCase identifiers that sit next to
    # each other as NUL-terminated strings.
    lua_like = re.findall(rb"\0((?:Get|Set|Is|Has|Can|Unit|C_)[A-Z_][A-Za-z0-9_.]{2,60})\0", rdata)
    print(f"\nLua-API-like names: {len(set(lua_like))}")
    print("  " + ", ".join(sorted(set(x.decode() for x in lua_like))[:20]))

    cnames = re.findall(rb"\0(C_[A-Za-z]+)\0", rdata)
    print(f"C_ namespaces: {len(set(cnames))}: " + ", ".join(sorted(set(c.decode() for c in cnames))[:30]))


if __name__ == "__main__":
    main()
