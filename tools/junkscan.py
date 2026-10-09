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


def main() -> None:
    from corpus import strip_retcheck

    cor = load_corpus()
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
    out = write_facts("junk", {"markers": list(MARKERS), "functions": per_func})
    print(f"raw decompilations: {raw_funcs} of {len(cor)} functions show junk markers "
          + ", ".join(f"{k} {n}" for k, n in raw_totals.most_common()))
    with_rc = sum(1 for v in per_func.values() if v["retcheck"])
    print(f"after stripping the checks: {len(per_func)} functions still decode as junk -> {out}")
    for k, n in left_totals.most_common():
        print(f"  {n:6}  with {k}")
    print(f"  {with_rc} of them carry the anti-tamper check (junk inside a check the stripper left)")


if __name__ == "__main__":
    main()
