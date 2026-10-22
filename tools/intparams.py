"""The integer parameters the decompiler typed float: build/facts/intparams.json.

A position tools/protos.py reads as an integer ('i') that the corpus's decompiled signature spells
`float` or `double`. ApplyFacts gives the position an undefined type of its width, and the
decompiler is free to infer the float again from a spill or a whole-register move; listed here,
ApplyFacts pins it to a concrete unsigned type. The UnitData reader FUN_1425019e0's flags came
out `float param_4`, tested as `(uint)param_4 & 1`, a conversion: the owner-only GUID was skipped
and the player's class read as 0 (2026-10-21).

Sticky: a position stays listed once found, since the corpus decompiled with the pin no longer
shows the float that put it here.

    python tools/intparams.py            # from build/corpus.jsonl and build/facts/protos.json
"""
import json
import re

from wowbin import write_facts
from corpus import ROOT

FACTS = ROOT / "build/facts"
INT_CAST = (r"(?:int|uint|longlong|ulonglong|byte|char|short|ushort|undefined1|undefined2|undefined4|undefined8|"
            r"bool)")


def main() -> None:
    protos = json.loads((FACTS / "protos.json").read_text())
    protos = protos.get("functions", protos)
    old = FACTS / "intparams.json"
    found: dict[str, set[int]] = {}
    if old.exists():
        for a, ks in json.loads(old.read_text())["functions"].items():
            found.setdefault(a, set()).update(ks)
    before = sum(len(v) for v in found.values())
    with open(ROOT / "build/corpus.jsonl", encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            p = protos.get("0x" + r["addr"])
            m = p and re.search(r"\b" + re.escape(r["name"]) + r"\(([^)]*)\)", r.get("c", ""))
            if not m or m.group(1).strip() in ("", "void"):
                continue
            for k, (decl, fact) in enumerate(zip(m.group(1).split(","), p["params"])):
                if fact[0] != "i":
                    continue
                if re.sub(r"\s*\w+$", "", decl.strip()) in ("float", "double"):
                    found.setdefault("0x" + r["addr"], set()).add(k + 1)
                    continue
                # a struct pointer typed `float *` and read through an integer cast,
                # `(int)param_1[0x4c]`: the field is an integer, and C converts the float it
                # reads. The PlayerData reader FUN_1424fec10 read its Customizations count so, 5
                # became 0, and every field after the skipped elements came out of step
                # (2026-10-21). As an untyped integer the decompiler spells each access with its
                # own type, and a conversion only where the code converts
                d = re.match(r"\s*(?:float|double)\s*\*\s*(param_\d+)\s*$", decl)
                if d and re.search(r"\(" + INT_CAST + r"\)\s*" + d.group(1) + r"\s*\[", r.get("c", "")):
                    found.setdefault("0x" + r["addr"], set()).add(k + 1)
    out = write_facts("intparams", {"functions": {a: sorted(v) for a, v in sorted(found.items())}})
    total = sum(len(v) for v in found.values())
    print(f"{total} integer positions in {len(found)} functions ({total - before} new) -> {out}")


if __name__ == "__main__":
    main()
