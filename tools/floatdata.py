"""The reference globals its code reads and writes as floating point, from the instructions.

The data export types most globals by size alone (`undefined4`), and the decompilation does not
say a global is a float when it is only ever used through SSE instructions, so a translated
`DAT_a * DAT_b` became an integer multiply where the reference does `mulss`. Every scalar SSE
instruction with a RIP-relative memory operand names a float (`ss`, `ps`, `comiss`) or a double
(`sd`, `pd`, `comisd`) at that address; this collects them, once, into build/refdata-float.json.

    python tools/floatdata.py      (tools/translate.py runs it when the cache is missing)
"""

from __future__ import annotations

import json
from pathlib import Path

import capstone

from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "build" / "refdata-float.json"

# Only what does floating-point arithmetic, compares, converts or moves one scalar says a float.
# The packed moves (movaps, movups) are MSVC's 16-byte copy of anything, and the bitwise ops
# (andps, xorps, ...) work on bits: a global pair of function pointers copied with movups was
# typed float, and `REFG(float, 0x142d2b310) = <a function>` did not compile (C2440, 2026-10-16)
FLOAT = {"movss", "addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss", "comiss", "ucomiss",
         "cvtss2sd", "cvtss2si", "cvttss2si", "addps", "mulps", "subps", "divps", "minps", "maxps",
         "vmovss", "vaddss", "vsubss", "vmulss", "vdivss", "vcomiss", "vucomiss", "vcvtss2sd",
         "vminss", "vmaxss", "vsqrtss", "vfmadd213ss", "vfmadd231ss", "vfmadd132ss"}
DOUBLE = {"movsd", "addsd", "subsd", "mulsd", "divsd", "minsd", "maxsd", "sqrtsd", "comisd", "ucomisd",
          "cvtsd2ss", "cvtsd2si", "cvttsd2si", "addpd", "mulpd", "subpd",
          "vmovsd", "vaddsd", "vsubsd", "vmulsd", "vdivsd", "vcomisd", "vucomisd", "vcvtsd2ss"}


def scan() -> dict[str, str]:
    b = Binary()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    out: dict[int, str] = {}

    def instructions():
        # function by function (the unwind table's pieces), so data between functions never
        # puts the decoder out of step
        for start, end in zip(b.piece_starts, b.piece_ends):
            o = b.off(start)
            yield from md.disasm_lite(bytes(b.image[o:o + (end - start)]), start)

    for ins in instructions():
        address, size, mnemonic, ops = ins
        if "rip" not in ops:
            continue
        kind = "float" if mnemonic in FLOAT else "double" if mnemonic in DOUBLE else None
        if kind is None:
            continue
        # the memory operand: `[rip + 0x...]` (or `- 0x...`)
        at = ops.find("[rip ")
        expr = ops[at + 5:ops.find("]", at)]
        sign, disp = expr.split()
        target = address + size + int(disp, 16) * (1 if sign == "+" else -1)
        # a packed access starts a vector of them; only the first element's type is certain
        if out.get(target, kind) != kind:
            out[target] = "mixed"
        else:
            out[target] = kind
    return {f"{a:x}": k for a, k in out.items() if k != "mixed"}


def load() -> dict[int, str]:
    if not CACHE.exists():
        CACHE.write_text(json.dumps(scan()), encoding="utf-8")
    return {int(a, 16): k for a, k in json.loads(CACHE.read_text(encoding="utf-8")).items()}


if __name__ == "__main__":
    found = scan()
    CACHE.write_text(json.dumps(found), encoding="utf-8")
    kinds = {}
    for k in found.values():
        kinds[k] = kinds.get(k, 0) + 1
    print(f"{len(found)} globals typed by their instructions: {kinds}")
