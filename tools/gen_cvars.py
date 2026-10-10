"""Turns the CVar registrations in a reference function into C++.

The reference registers console variables through FUN_140320f70(name, help, flags, default,
callback, category, isReset, userData, a9), which is CVarRegister's plain-callback overload.
This reads every such call out of the function's decompilation (build/corpus.jsonl), resolves
the string arguments from the clean exe, and prints the matching CVarRegister calls. A result
stored in a global becomes `s_<name>CVar = ...`; a callback becomes `<Name>Callback`, and the
list of callbacks to port is printed after the calls.

    python tools/gen_cvars.py 14024c220
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import corpus as corpusmod  # noqa: E402
from wowbin import Binary  # noqa: E402

CALL = re.compile(r"(?:(\w+)\s*=\s*)?FUN_140320f70\(", re.S)


def split_args(text: str, start: int) -> tuple[list[str], int]:
    depth, i, args, cur = 1, start, [], ""
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            cur += c
            if c == "\\":
                cur += text[i + 1]
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            cur += c
        elif c == "(":
            depth += 1
            cur += c
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args, i
            cur += c
        elif c == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    raise ValueError("unbalanced call")


def cxx_string(arg: str, binary: Binary) -> str:
    arg = " ".join(arg.split())
    if arg == "0":
        return "nullptr"
    if arg.startswith('"'):
        # adjacent literals the decompiler split over lines
        return re.sub(r'"\s+"', "", arg).replace("\\'", "'")
    m = re.fullmatch(r"&?(?:DAT|s_\w*?)_?([0-9a-f]{9})", arg)
    if m:
        value = binary.cstr(int(m.group(1), 16))
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return f"/* {arg} */ nullptr"


def callback_name(cvar: str) -> str:
    return cvar[0].upper() + cvar[1:] + "Callback"


def main() -> None:
    corpus = corpusmod.load_corpus()
    binary = Binary()
    for a in sys.argv[1:]:
        rec = corpus[int(a, 16)]
        text = rec["c"]
        print(f"// ---- FUN_{a}")
        callbacks = []
        named = {}
        for m in CALL.finditer(text):
            args, _ = split_args(text, m.end())
            name = cxx_string(args[0], binary).strip('"')
            store = f"s_{name[0].lower() + name[1:]}CVar = " if m.group(1) else ""
            callback = "nullptr"
            if args[4] != "0":
                # a callback several variables share keeps the first one's name
                if args[4] not in named:
                    named[args[4]] = callback_name(name)
                    callbacks.append((named[args[4]], args[4]))
                callback = named[args[4]]
            flags = args[2]
            print(f'    {store}CVarRegister("{name}", {cxx_string(args[1], binary)}, {flags}, '
                  f"{cxx_string(args[3], binary)}, {callback}, {args[5]}, {('true' if args[6] != '0' else 'false')}, "
                  f"{('nullptr' if args[7] == '0' else f'reinterpret_cast<void*>({args[7]})')}, "
                  f"{('true' if args[8] != '0' else 'false')});")
        for callback, ref in callbacks:
            print(f"// callback {callback}: {ref.lstrip('&')}")


if __name__ == "__main__":
    main()
