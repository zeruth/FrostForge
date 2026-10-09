"""Source-file attribution: which functions reference which source paths.

Asserts, traces and allocator tags embed __FILE__, so a function that loads a path
string through a RIP-relative lea belongs to that source file. This gives each
function a module, which is what the recomp report needs to rank and group work.

    python tools/srcrefs.py     -> build/facts/srcrefs.json
"""
from __future__ import annotations

import re
from collections import defaultdict

from wowbin import Binary, hexa, write_facts

PATH = re.compile(rb"[\x20-\x7e]{0,200}?\.(?:cpp|c|h|hpp|inl|cc)\0", re.I)


def normalize(p: str) -> str:
    """Strip the build machine's checkout prefix, keep the project-relative path."""
    p = p.replace("\\", "/")
    k = p.find("/package_cache/")
    if k >= 0:
        return "pkg/" + p[k + len("/package_cache/") :]
    k = p.find("/work-git/wow/")
    if k >= 0:
        return p[k + len("/work-git/wow/") :]
    for marker in ("/Source/", "/source/", "/Engine/", "/Contrib/", "/contrib/", "/src/"):
        k = p.find(marker)
        if k >= 0:
            return p[k + 1 :]
    return p.lstrip("./")


def main() -> None:
    b = Binary()
    strings: dict[int, str] = {}
    for name in (".rdata", ".data"):
        s, d = b.bytes_of(name)
        for m in PATH.finditer(d):
            raw = m.group()[:-1].decode(errors="replace")
            # take the path part of the match: from its last drive letter / slash run start
            k = max(raw.rfind(" "), raw.rfind("\t"))
            raw = raw[k + 1 :]
            if "/" not in raw and "\\" not in raw:
                continue
            start = s + m.end() - 1 - len(raw)
            strings[start] = raw

    by_file: dict[str, set[int]] = defaultdict(set)
    by_func: dict[int, set[str]] = defaultdict(set)
    for insn, tgt in b.riprel_refs(keep=strings.__contains__):
        p = strings.get(tgt)
        if p is None:
            continue
        f = b.func_containing(insn)
        if f is None:
            continue
        n = normalize(p)
        by_file[n].add(f)
        by_func[f].add(n)

    out = write_facts("srcrefs", {
        "files": {k: sorted(hexa(f) for f in v) for k, v in sorted(by_file.items())},
        "functions": {hexa(f): sorted(v) for f, v in sorted(by_func.items())},
    })
    single = sum(1 for v in by_func.values() if len(v) == 1)
    print(f"{len(strings)} path strings; {len(by_file)} files referenced from code; "
          f"{len(by_func)} functions attributed ({single} to exactly one file) -> {out}")
    top = sorted(by_file.items(), key=lambda kv: -len(kv[1]))[:20]
    for k, v in top:
        print(f"  {len(v):5}  {k}")


if __name__ == "__main__":
    main()
