"""Port globals that name their reference address but are not tied to it.

tools/translate.py ties a port global to the image's (translated code then reads and writes the
port's copy) only when the declaration ends in its address tag: `uint8_t g_x;  // DAT_1430af4c8`.
A global whose address sits in the comment above it instead is invisible to the translator, so
translated code keeps its own copy in the image and the two drift apart: on 2026-10-13
C_Login.IsReconnectLoginPossible read the port's g_reconnectLoginPossible while the translated
FUN_14148b330 cleared the image's DAT_1430af4c8.

This lists namespace-scope declarations whose comment block names exactly one DAT_ address and
whose line carries none, and says whether translated code touches that address (which is when it
matters).

A global kept apart on purpose says so on its line, `// untied: <why>`, and is not listed.

    python tools/untied.py [--all]     # --all: also those translated code never touches
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from translate import PARAM_DECL, PORT_FNPTR, PORT_GLOBAL, port_globals

ROOT = Path(__file__).resolve().parent.parent
DECL = re.compile(r"^(?!(?:struct|class|union|enum|typedef|using|return|namespace|template|static_assert|extern \"C\")\b)"
                  r"[A-Za-z_][\w:<>,\t \*&()]*?[\t \*&)](\w+)(?:\[[^\]\n]*\])?(?:[\t ]*(?:=|\{|\()[^;\n]*)?;\s*(//.*)?$")
ADDR = re.compile(r"\b(?:DAT_|0x)(14[0-9a-f]{7})\b")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    held = port_globals()
    gen = ""
    for p in (ROOT / "src" / "gen").glob("translated_*.c"):
        gen += p.read_text(encoding="utf-8", errors="replace")
    touched = {int(a, 16) for a in re.findall(r"(?:REFG\([^,]+, |REFP\()0x(14[0-9a-f]{7})\)", gen)}

    rows = []
    for p in sorted((ROOT / "src").rglob("*.[ch]pp")):
        if "gen" in p.relative_to(ROOT).parts:
            continue
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        for i, line in enumerate(lines):
            m = DECL.match(line)
            if m is None:
                continue
            code = line.split("//")[0]
            call = re.search(r"\b\w+\s*\(([^()]*)\)\s*(?:const\s*)?(?:override\s*)?;\s*$", code)
            if call and not re.search(r"\(\s*\w*\s*\*", code) \
                    and (not call.group(1).strip() or call.group(1).strip() == "void" or PARAM_DECL.search(call.group(1))):
                continue  # a prototype
            if PORT_GLOBAL.match(line) or PORT_FNPTR.match(line) or "// untied:" in line:
                # tied, or left apart on purpose with the reason on the line
                continue
            # the comment block straight above
            j, block = i - 1, []
            while j >= 0 and lines[j].lstrip().startswith("//"):
                block.append(lines[j])
                j -= 1
            addrs = set(ADDR.findall(" ".join(block)))
            if len(addrs) != 1:
                continue
            addr = int(addrs.pop(), 16)
            if addr in held:
                continue
            hot = addr in touched
            if hot or args.all:
                rows.append((not hot, p.relative_to(ROOT).as_posix(), i + 1, m.group(1), addr, hot))
    for _cold, path, line, name, addr, hot in sorted(rows):
        print(f"{path}:{line}  {name}  DAT_{addr:x}  {'translated code touches it' if hot else ''}")
    print(f"{len(rows)} untied globals" + ("" if args.all else " that translated code touches"))


if __name__ == "__main__":
    main()
