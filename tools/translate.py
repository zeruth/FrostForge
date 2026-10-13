"""Reference functions translated to C from the decompilation corpus, in bulk.

The hand ports cover what has been ported by hand; this covers the rest mechanically, so the port
runs the reference's own logic where nobody has written it yet. Each translated function is the
Ghidra output of build/corpus.jsonl rewritten to compile as C (src/gen/Ghidra.h), tagged
`/* ref: FUN_<addr> */`, and run against the reference's data sections mapped at their original
addresses (src/gen/RefImage.cpp), so its globals are its addresses.

What a translated function calls:
  - a function the port already has: an adapter that calls the port function through its mangled
    name (/alternatename) with the ABI the PDB gives it (integers and pointers in 64 bits, floats
    and doubles apart), so no headers are needed;
  - an unported function within --depth: translated too;
  - anything else: a stub that logs the address once (GenUnimplemented) and returns 0.
Bodies the translator cannot take yet (registers Ghidra lost track of, SIMD unions, TLS, types
Ghidra applied that C does not know) become such stubs, and the reason is kept in the manifest.

    python tools/translate.py --handlers --depth 2      # the stubbed Lua API handlers
    python tools/translate.py --addr 14169d420 --depth 1
    python tools/translate.py --handlers --depth 2 --fix   # build, demote what fails, repeat

Outputs src/gen/translated_*.c, adapters.c, stubs.c, jumps.c, handlers.cpp, and
src/gen/manifest.json (what was translated, what was stubbed and why).
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import callargs
import fidelity as F
import floatdata
import lift
from corpus import load_corpus, strip_retcheck
from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "src/gen"


def write_out(path: Path, text: str, **kw) -> None:
    """write_text that waits out another process holding the file (an IDE indexer does, and a
    failed write mid-run leaves src/gen inconsistent)."""
    import time
    for attempt in range(40):
        try:
            path.write_text(text, **kw)
            return
        except OSError:
            if attempt == 39:
                raise
            time.sleep(0.25)
MANIFEST = GEN / "manifest.json"
REFDATA = ROOT / "build/refdata.jsonl"
DEMOTED = GEN / "demoted.json"       # functions the compile loop found the translator cannot take
PORT_CACHE = ROOT / "build/translate-port.json"  # the port's functions, from its last good link
PER_FILE = 150

PRIMITIVE = {
    "void", "undefined", "undefined1", "undefined2", "undefined3", "undefined4", "undefined5", "undefined6",
    "undefined7", "undefined8", "byte", "uchar", "uint1", "ushort", "word", "wchar16", "uint", "uint3", "dword",
    "ulong", "longlong", "ulonglong", "qword", "uint8", "uint4", "uint2", "int4", "int8", "int2", "sbyte",
    "float4", "float8", "float10", "pointer", "pointer64", "code", "char", "short", "int", "long", "float", "double",
    "bool", "wchar_t", "HANDLE", "size_t", "unsigned", "signed", "uint5", "uint6", "uint7", "int3", "int5",
    "int6", "int7", "int1", "__time64_t",
}
HARD = [
    ("register the decompiler lost track of", re.compile(r"\b(in_|unaff_|extraout_)\w+")),
    ("SIMD union", re.compile(r"\._\d+_\d+_")),
    # (`LOCK(); ... UNLOCK();` blocks are mapped onto Interlocked operations by atomics())
    ("the stack frame addressed directly", re.compile(r"\b(_local_res\w*|stack0x\w+|\w*StackX_\w+)")),
    ("thread-local storage", re.compile(r"ThreadLocalStoragePointer|_tls_index")),
    ("bad data or a trap", re.compile(r"halt_baddata|\bswi\(|software_interrupt|halt_unimplemented")),
    ("struct member access", re.compile(r"->")),
    # Ghidra's own word that it lost the flow (anti-tamper bytes decoded as code, overlapping
    # instructions): what it printed is not the function
    ("a decompilation Ghidra marked as damaged",
     re.compile(r"Control flow encountered bad instruction data|overlaps instruction at")),
]
CRT = {"memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp", "strchr", "strrchr", "strstr",
       "strcpy", "strncpy", "strcat", "abs", "labs", "fabs", "sqrt", "sqrtf", "floor", "floorf", "ceil", "ceilf",
       "sin", "cos", "tan", "sinf", "cosf", "tanf", "atan2", "atan2f", "pow", "powf", "fmod", "fmodf", "exp", "log",
       "tolower", "toupper", "isdigit", "isalpha", "isspace", "atoi", "atof", "strtol", "strtoul", "free", "malloc"}
# reference runtime routines FID left unnamed (tools/fidelity_exceptions.json)
CRT_BY_ADDR = {0x1425bf3b0: "memmove", 0x1425bfa50: "memset",
               # the thread-safe static guard's header (its footer is named in the corpus)
               0x14026bca8: "_Init_thread_header"}

CALL = re.compile(r"(?<![\w:])((?:[A-Za-z_]\w*::)+[~A-Za-z_]\w*|[A-Za-z_]\w*)\s*\(")
SYMBOL = re.compile(r"\b((?:PTR_)*(?:DAT|_DAT|UNK|_UNK|s|u|ws|LAB|vtable|switchD|PTR|[a-zA-Z]+Ram)_\w*?_?([0-9a-f]{9}))\b")
RAM = re.compile(r"\b([a-z]+)Ram0*(1[0-9a-f]{8})\b")
FUNREF = re.compile(r"\b(?:thunk_)?FUN_([0-9a-f]{9})\b")
IMPORT = re.compile(r"\bImport::(\w+)::(\w+)\b")
# the types a body names: in casts, and in declarations of its locals
TYPE_WORD = re.compile(r"\(\s*(?:const\s+)?([A-Za-z_]\w*)\s*\*+\s*\)|"
                       r"^\s+(?!(?:return|goto|break|continue|case|else|do|delete|new)\b)"
                       # the type and the name apart (a wrapped `uVar4;` is not type `uVa`, name `r4`)
                       r"(?:const\s+)?([A-Za-z_]\w*)(?:\s+\**\s*|\s*\*+\s*)[A-Za-z_]\w*\s*(?:\[[^\]]*\])?\s*;", re.M)


# ---- the port's functions, as the PDB has them ----------------------------------------------

def load_publics() -> dict[int, str]:
    """va -> mangled name of the port's functions."""
    out = subprocess.run([str(F.LLVM / "llvm-pdbutil.exe"), "dump", "-publics", str(F.PORT_PDB)],
                         capture_output=True, text=True, errors="replace").stdout
    port = Binary(F.PORT_EXE)
    names = {}
    pending = None
    for line in out.splitlines():
        m = re.search(r"S_PUB32 \[size = \d+\] `(.*)`$", line)
        if m:
            pending = m.group(1)
            continue
        if pending is not None:
            m = re.search(r"flags = (\w+).*addr = ([0-9A-Fa-f]{4}):(\d+)", line)
            if m:
                # an adapter's alias is listed at its target's address too
                if m.group(1) == "function" and not pending.startswith("gen_alias_"):
                    section = port.sections[int(m.group(2), 16) - 1]
                    names.setdefault(section.start + int(m.group(3)), pending)
                pending = None
    return names


def load_signatures() -> dict[int, tuple]:
    """va -> (return class, [argument classes], this) for the port's functions.

    A class is 'int' (integers, pointers, references, enums, bools), 'float', 'double', 'void',
    or 'complex' (an aggregate by value, or variadic): an adapter only takes the simple ones."""
    types = subprocess.run([str(F.LLVM / "llvm-pdbutil.exe"), "dump", "-types", str(F.PORT_PDB)],
                           capture_output=True, text=True, errors="replace").stdout.splitlines()
    kind_of: dict[int, str] = {}
    proc_of: dict[int, tuple] = {}
    args_of: dict[int, list] = {}
    current = None
    for line in types:
        m = re.match(r"\s*0x([0-9A-F]+) \| (LF_\w+)", line)
        if m:
            current = (int(m.group(1), 16), m.group(2))
            kind_of[current[0]] = current[1]
            if current[1] == "LF_ARGLIST":
                args_of[current[0]] = []
            continue
        if current is None:
            continue
        idx, kind = current
        if kind in ("LF_PROCEDURE", "LF_MFUNCTION"):
            m = re.search(r"return type = 0x([0-9A-F]+)(?: \(([^)]*)\))?, # args = (\d+), param list = 0x([0-9A-F]+)", line)
            if m:
                proc_of[idx] = [int(m.group(1), 16), m.group(2), int(m.group(4), 16), kind == "LF_MFUNCTION"]
        elif kind == "LF_ARGLIST":
            m = re.search(r"0x[0-9A-F]+(?: \([^)]*\))?: `(.*)`$", line)
            if m:
                args_of[idx].append(m.group(1))

    def classify_text(text: str) -> str:
        t = text.replace("const ", "").replace("volatile ", "").strip()
        if t.endswith(("*", "&")) or "(*)" in t or "*" in t:
            return "int"
        if t == "float":
            return "float"
        if t == "double":
            return "double"
        if t == "void":
            return "void"
        if t == "...":
            return "complex"
        simple = ("bool", "char", "signed char", "unsigned char", "short", "unsigned short", "int", "unsigned",
                  "long", "unsigned long", "__int64", "unsigned __int64", "wchar_t", "char16_t", "char32_t",
                  "unsigned int", "long long", "unsigned long long", "__int8", "unsigned __int8", "__int16",
                  "unsigned __int16", "__int32", "unsigned __int32", "HRESULT", "std::nullptr_t")
        if t in simple:
            return "int"
        return "enum?" + t

    def classify_index(index: int, text: str | None) -> str:
        if text is not None:
            return classify_text(text)
        kind = kind_of.get(index, "")
        if kind in ("LF_POINTER", "LF_ENUM"):
            return "int"
        if kind == "LF_MODIFIER":
            return "complex"
        return "complex"

    syms = subprocess.run([str(F.LLVM / "llvm-pdbutil.exe"), "dump", "-symbols", str(F.PORT_PDB)],
                          capture_output=True, text=True, errors="replace").stdout.splitlines()
    port = Binary(F.PORT_EXE)
    enum_names = set()
    for line in types:
        m = re.match(r"\s*0x[0-9A-F]+ \| LF_ENUM \[size = \d+\] `(.*)`", line)
        if m:
            enum_names.add(m.group(1))
    out = {}
    va = None
    for line in syms:
        m = re.search(r"addr = ([0-9A-Fa-f]{4}):(\d+), code size = \d+", line)
        if m:
            section = port.sections[int(m.group(1), 16) - 1]
            va = section.start + int(m.group(2))
            continue
        m = re.search(r"type = `0x([0-9A-F]+) ", line)
        if m and va is not None:
            proc = proc_of.get(int(m.group(1), 16))
            if proc:
                ret = classify_index(proc[0], proc[1])
                args = []
                for text in args_of.get(proc[2], []):
                    c = classify_text(text)
                    if c.startswith("enum?"):
                        c = "int" if c[5:] in enum_names else "complex"
                    args.append(c)
                if ret.startswith("enum?"):
                    ret = "int" if ret[5:] in enum_names else "complex"
                out[va] = (ret, args, proc[3])
            va = None
    return out


# ---- the reference's globals ------------------------------------------------------------------

def load_refdata() -> dict[int, tuple[int, str]]:
    out = {}
    with REFDATA.open(encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            out[int(d["addr"], 16)] = (d["len"], d["type"])
    return out


def c_type_of(ghidra: str, size: int) -> str:
    """The C type a scalar global is declared with, from Ghidra's data type."""
    t = ghidra.strip()
    if t in PRIMITIVE and t not in ("void", "code", "pointer", "pointer64"):
        return t
    if t.endswith("*"):
        # Ghidra writes `*DAT_x` and `(*DAT_x)(...)` through the pointer's own type
        base = t[:-1].strip()
        if base == "code":
            return "code *"
        if base in PRIMITIVE and base not in ("pointer", "pointer64"):
            return f"{base} *"
        if base.endswith("*"):
            return "undefined8 *"
        return "undefined8 *"
    if t in ("pointer", "pointer64", "addr", "ImageBaseOffset32"):
        return "undefined8"
    return {1: "undefined1", 2: "undefined2", 4: "undefined4", 8: "undefined8"}.get(size, "undefined1")


def global_expr(ghidra: str, size: int, addr: int) -> str:
    """A reference global as an lvalue at its address: arrays stay arrays, so `x[i]` and `&x` work."""
    m = re.match(r"^(.*?)((?:\[\d+\])+)$", ghidra.strip())
    if m:
        element = c_type_of(m.group(1), 0)
        if element == "undefined1" and m.group(1).strip() not in PRIMITIVE:
            element = "undefined1"
        return f"(*({element} (*){m.group(2)})(uintptr_t)0x{addr:x}ULL)"
    return f"REFG({c_type_of(ghidra, size)}, 0x{addr:x})"


PORT_GLOBAL = re.compile(r"^(?!(?:struct|class|union|enum|typedef|using)\b)([A-Za-z_][\w:<>,\t \*&]*?[\t \*&:])(\w+)(\[[^\]\n]*\])?(?:[\t ]*(?:=|\{)[^;\n]*)?;[\t ]*"
                         r"//\s*(?:ref:\s*)?(?:DAT_|0x)(14[0-9a-f]{7})\b", re.M)
SCALAR = re.compile(r"^(?:static\s+|extern\s+|thread_local\s+)*(?:const\s+)?(?:u?int\d+_t|bool|float|double|char|"
                    r"int|unsigned|DWORD|size_t|uintptr_t|HANDLE|HWND|\w+\s*\*+)$")


STACK_DECL = re.compile(r"^(\s*)([A-Za-z_][\w ]*?)\s*(\**)\s*((?:local_|[a-z]{1,4}Stack_)(res)?([0-9a-f]+))"
                        r"\s*(?:\[\s*(\w+)\s*\])?\s*;[^\n]*\n", re.M)


def stack_frame(body: str) -> str:
    """Locals at their offsets in the reference's frame, when one's address escapes.

    Ghidra splits a stack struct into one local per field (`local_28`, `iStack_24`, `uStack_20`)
    and passes `&local_28` to a callee that reads the rest at +4 and +8; the compiler is free to
    place separate locals anywhere, so the callee would read garbage. A local named `local_N` sits
    N bytes below the stack pointer at entry (`local_resN` N above it); keeping them all in one
    buffer at those offsets, aligned as the reference's stack was, keeps the struct whole."""
    # the declarations are the block before Ghidra's first blank line
    end = re.search(r"\n[ \t]*\n", body)
    decls = [m for m in STACK_DECL.finditer(body, 0, end.end() if end else 0)
             if m.group(2).split()[0] not in ("return", "goto", "else", "case")]
    if not decls:
        return body
    names = [m.group(4) for m in decls]
    arrays = {m.group(4) for m in decls if m.group(7)}
    if not arrays and not any(re.search(rf"&\s*{n}\b", body) for n in names):
        return body
    offs = {m.group(4): (int(m.group(6), 16) if m.group(5) else -int(m.group(6), 16)) for m in decls}
    lo = min(offs.values())
    pad = (8 + lo) % 16          # the entry stack pointer is 8 past a 16-byte boundary
    top = max(decls, key=lambda m: offs[m.group(4)])
    ctype = lambda m: f"{m.group(2).strip()} {m.group(3)}".strip()
    size_of = lambda m: f"sizeof({ctype(m)}[{m.group(7)}])" if m.group(7) else f"sizeof({ctype(m)})"
    frame = (f"{decls[0].group(1)}__declspec(align(16)) unsigned char gen_frame"
             f"[0x{pad + offs[top.group(4)] - lo:x} + {size_of(top)}];\n")
    for m in reversed(decls):
        body = body[:m.start()] + body[m.end():]
    first = re.search(r"\{\n", body).end()
    body = body[:first] + frame + body[first:]
    for m in decls:
        n, at = m.group(4), pad + offs[m.group(4)] - lo
        if m.group(7):
            ptr = f"(({ctype(m)} *)(gen_frame + 0x{at:x}))"
            # `&arr` is the array's address; `&arr[i]` stays an element's address
            body = re.sub(rf"&\s*\b{n}\b(?!\s*\[)", ptr, body)
            body = re.sub(rf"\b{n}\b", ptr, body)
        else:
            body = re.sub(rf"\b{n}\b", f"(*({ctype(m)} *)(gen_frame + 0x{at:x}))", body)
    return body


IMAGE_LO, IMAGE_HI = 0x140000000, 0x150000000   # the reference image's address range
INIT_EPOCH = re.compile(r"\*\(int \*\)\(\*\(longlong \*\)\(\(longlong\)ThreadLocalStoragePointer \+ "
                        r"\(ulonglong\)_tls_index \* 8\) \+\s*0x2140\)")
STRLEN_LOOP = re.compile(r"(\w+) = -1;\s*do \{\s*\1 = \1 \+ 1;\s*\} while \(\*\(char \*\)\(([\w\[\]\*]+) \+ \1\) != '\\0'\);")


VCALL = re.compile(r"\(\*\*\(code \*\*\)(?:\(\*(?:\((?:\w+ )*\w+ ?\*+\))?|\*(?:\((?:\w+ )*\w+ ?\*+\))?)")


PIECE_TYPE = {1: "byte", 2: "ushort", 4: "uint", 8: "ulonglong"}


def global_pieces(text: str) -> str:
    """`s_must_be_a_finite_number_1426e2d68._12_4_`: N bytes at an offset of a global, in
    Ghidra's union notation (an inlined copy of a string constant, a field of a data block).
    For a global that is plain arithmetic on its address; a string symbol already is one."""
    def piece(m):
        sym, off, size = m.group(1), int(m.group(2)), int(m.group(3))
        if size not in PIECE_TYPE:
            return m.group(0)
        base = sym if re.match(r"(?:s|u|ws)_", sym) else f"&{sym}"
        return f"(*({PIECE_TYPE[size]} *)((char *){base} + {off}))"

    text = re.sub(r"\b((?:PTR_)*(?:_?DAT|s|u|ws|PTR)_\w*?_?[0-9a-f]{9})\._(\d+)_(\d+)_", piece, text)

    # the same notation on a local or a parameter (`local_res18[0]._1_3_`, a register's upper
    # bytes): a typed access at its address, or, for an odd width only read, a shift and a mask
    vectors = set(re.findall(r"\b(\w+) \[16\];", text))     # a real 16-byte value stays refused

    def local_piece(m):
        var, off, size = m.group(1), int(m.group(2)), int(m.group(3))
        if var.split("[")[0] in vectors or off + size > 8:
            return m.group(0)
        if size in PIECE_TYPE:
            return f"(*({PIECE_TYPE[size]} *)((char *)&{var} + {off}))"
        if re.match(r"\s*=[^=]", text[m.end():m.end() + 3]):
            return m.group(0)      # an odd-width piece written: no C lvalue for it
        return f"(((ulonglong)({var}) >> {8 * off}) & {(1 << (8 * size)) - 1:#x})"

    return re.sub(r"(?<![\w.])([A-Za-z_]\w*(?:\[[^\]\[]+\])?)\._(\d+)_(\d+)_", local_piece, text)


def virtual_this(text: str) -> str:
    """A virtual call shown with no arguments gets the object it reads the vtable of.

    `(**(code **)(*(longlong *)*puVar2 + 0x30))()`: the instructions load the object into rcx,
    read its vtable from it and call the slot (mov rcx,[rbx]; mov rax,[rcx]; call [rax+0x30]);
    Ghidra dropped rcx. A virtual call always passes that object first."""
    out, pos = [], 0
    for m in VCALL.finditer(text):
        if m.start() < pos:
            continue
        # the object: up to ` + K))()` (a slot past the first) or `)()` (slot 0), parentheses
        # balanced
        i, depth = m.end(), 0
        while i < len(text) and text[i] not in ";\n":
            ch = text[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and text.startswith(" + ", i):
                break
            i += 1
        obj = text[m.end():i].strip()
        rest = re.match(r"(?: \+ (?:0x[0-9a-f]+|\d+))?\)+\(\)", text[i:])
        if not obj or rest is None or not re.fullmatch(r"[\w\[\]\*\(\) +-]+", obj):
            continue
        end = i + rest.end()
        out.append(text[pos:end - 1] + obj + ")")
        pos = end
    return "".join(out) + text[pos:]


def split_args(args: str) -> list[str]:
    """A call's arguments, split at the top-level commas (not those in string literals)."""
    out, depth, cur = [], 0, ""
    quote = None
    i = 0
    while i < len(args):
        ch = args[i]
        if quote:
            cur += ch
            if ch == "\\" and i + 1 < len(args):
                cur += args[i + 1]
                i += 2
                continue
            if ch == quote:
                quote = None
            i += 1
            continue
        if ch in "\"'":
            quote = ch
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def matching_paren(text: str, start: int) -> int:
    """The index just past the `)` closing the `(` before `start`, string literals skipped."""
    depth, i, quote = 1, start, None
    while i < len(text) and depth:
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        i += 1
    return i


def call_sites(text: str):
    """Each `FUN_x(args)` call: (match start, end of the argument list, address, arguments)."""
    for m in re.finditer(r"\bFUN_([0-9a-f]{9})\(", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        yield m.start(), i - 1, int(m.group(1), 16), split_args(text[m.end():i - 1])


def upper_half_only(text: str) -> list[str]:
    """`in_stack_` slots read only for their upper 32 bits.

    A 32-bit argument passed on the stack is a 64-bit slot whose upper half the callee never
    reads; Ghidra carries that half through as `CONCAT44((int)(in_stack_x >> 0x20), arg)`. Such a
    slot can be anything, so zero is as faithful as the caller's leftovers."""
    out = []
    for name in set(re.findall(r"\bin_stack_\w+", text)):
        uses = [m for m in re.finditer(rf"\b{name}\b", text)]
        decl = re.search(rf"^\s*[\w ]+ {name};$", text, re.M)
        rest = [m for m in uses if not (decl and decl.start() <= m.start() < decl.end())]
        if rest and all(re.match(r"\s*>>\s*0x20\b", text[m.end():]) for m in rest):
            out.append(name)
        elif re.fullmatch(r"in_stack_ffff[0-9a-f]+", name):
            # a slot of the function's own outgoing-argument area (below its entry rsp): the
            # instructions set the bytes a narrow stack argument has (`mov byte ptr [rsp+0x20],0`)
            # and Ghidra carries the rest as this; the callee reads only what was set
            out.append(name)
    return out


def wide_multiply(text: str) -> str:
    """Ghidra's 128-bit products (`mul` with the high half kept) as 64-bit intrinsics.

    A divide by a constant or an overflow check builds a 16-byte union from one 64-bit value
    (`v._8_8_ = 0; v._0_8_ = x;`) and keeps one half of `ZEXT816(k) * v`. When the union is
    used no other way, it is that 64-bit value, and the halves are __umulh and a plain product."""
    for name in set(re.findall(r"\b(\w+)\._0_8_ = ", text)):
        v = re.escape(name)
        lo = f"{name}_lo"
        operand = rf"(?:ZEXT816\(([^()]+)\)|{v})"
        new = re.sub(rf"\b{v}\._8_8_ = 0;\n", "", text)
        new = re.sub(rf"\b{v}\._0_8_ = ", f"{lo} = ", new)

        def product(m):
            a = m.group(1) if m.group(1) is not None else lo
            b = m.group(2) if m.group(2) is not None else lo
            if m.group(3) == "8":
                return f"__umulh((ulonglong)({a}), (ulonglong)({b}))"
            return f"((ulonglong)({a}) * (ulonglong)({b}))"

        new = re.sub(rf"SUB168\({operand} \* {operand},([08])\)", product, new)
        new = re.sub(rf"(undefined1?|undefined) {v} \[16\];", f"ulonglong {lo};", new)
        if not re.search(rf"\b{v}\b(?!_lo)", new):
            text = new
    return text


LOCK_BLOCK = re.compile(r"([ \t]*)LOCK\(\);\n(.*?)\n[ \t]*UNLOCK\(\);", re.S)
RMW_OPS = {"+": "ADD", "&": "AND", "|": "OR", "^": "XOR"}


def _bare(expr: str) -> str:
    """An lvalue or operand without its casts and spaces, to compare two spellings of one place."""
    expr = re.sub(r"\((?:\w+ )*\w+ ?\**\)", "", expr)
    expr = re.sub(r"\s+", "", expr)
    expr = re.sub(r"\b(\w+)\[(\w+)\]", r"*(\1+\2)", expr)      # p[1] is *(p + 1)
    expr = re.sub(r"^\*\((\w+)\+0\)$", r"*\1", expr)
    return expr.strip("()")


def _local_set(line: str) -> bool:
    """`x = <expr>;` with a plain local on the left: an instruction of its own around the locked
    one (the address it works on, or a register copy of its result)."""
    return re.fullmatch(r"\w+ = [^;]+;", line) is not None


def _rmw(lhs: str, rhs: str) -> tuple[str, str] | None:
    """(op, operand) when `lhs = rhs` is `lv = lv OP k`."""
    m = re.fullmatch(r"(.+?) ([+&|^]) (-?\w+|\(.+\))", rhs.strip())
    if m and _bare(m.group(1)) == _bare(lhs):
        return RMW_OPS[m.group(2)], m.group(3)
    return None


def atomics(body: str) -> str:
    """Each `LOCK(); ... UNLOCK();` as the Interlocked operation the locked instruction is.

    Ghidra spells one `lock`-prefixed instruction as a few statements between the markers: a
    read-modify-write (`lock add/and/or/xor`), the same with the old value kept (`lock xadd`),
    an exchange (`xchg`), or a compare-exchange (`lock cmpxchg`), sometimes after the register
    arithmetic that forms the address. Anything else is a shape this does not know."""
    def core(lines):
        out = None
        assigns = [re.fullmatch(r"(.+?) = (.+);", l) for l in lines]
        if len(lines) == 1 and assigns[0]:
            lhs, rhs = assigns[0].groups()
            op = _rmw(lhs, rhs)
            out = [f"GEN_ATOMIC_{op[0]}({lhs}, {op[1]});" if op else f"GEN_ATOMIC_XCHG({lhs}, {rhs});"]
        elif len(lines) == 2 and all(assigns) and re.fullmatch(r"\w+", assigns[0].group(1)) \
                and _bare(assigns[0].group(2)) == _bare(assigns[1].group(1)):
            old, (lhs, rhs) = assigns[0].group(1), assigns[1].groups()
            op = _rmw(lhs, rhs)
            out = [f"{old} = GEN_ATOMIC_{op[0]}({lhs}, {op[1]});" if op else f"{old} = GEN_ATOMIC_XCHG({lhs}, {rhs});"]
        else:
            # [old = lv;] flag = a == b; if (flag) { lv = new; [x = y;] }
            text = " ".join(lines)
            c = re.fullmatch(r"(?:(\w+) = ([^;]+); )?(\w+) = ([^;]+?) == ([^;]+?); if \(\3\) \{ ([^;]+?) = ([^;]+); "
                             r"((?:\w+ = [^;]+; )?)\}", text)
            if c:
                old, loaded, flag, a, b, lv, new, extra = c.groups()
                if old is not None and _bare(loaded) == _bare(lv) and old in (a, b):
                    cmp = b if a == old else a
                    out = [f"{old} = GEN_ATOMIC_CAS({lv}, {new}, {cmp});", f"{flag} = {old} == {cmp};"]
                elif old is None and _bare(lv) in (_bare(a), _bare(b)):
                    cmp = b if _bare(a) == _bare(lv) else a
                    out = [f"{flag} = GEN_ATOMIC_CAS({lv}, {new}, {cmp}) == {cmp};"]
                if out is not None and extra.strip():
                    out.append(f"if ({flag}) {{ {extra.strip()} }}")
        return out

    def one(m):
        indent, lines = m.group(1), [l.strip() for l in m.group(2).splitlines() if l.strip()]
        # the fewest statements around the locked instruction that leave a shape it can be
        for k in range(len(lines)):
            if not all(_local_set(l) for l in lines[:k]):
                break
            for n in range(len(lines) - k):
                tail = lines[len(lines) - n:] if n else []
                if not all(_local_set(l) for l in tail):
                    break
                out = core(lines[k:len(lines) - n])
                if out is not None:
                    return "\n".join(indent + l for l in lines[:k] + out + tail)
        raise Untranslatable("an interlocked operation of a shape the translator does not map")

    return LOCK_BLOCK.sub(one, body)


def scalar_size(decl: str) -> int:
    """The bytes a scalar global's declared type occupies (a pointer or handle is 8)."""
    t = re.sub(r"\b(?:static|extern|const|volatile|thread_local|unsigned|signed)\b", "", decl).strip()
    if "*" in t:
        return 8
    return {"bool": 1, "char": 1, "int8_t": 1, "uint8_t": 1, "int16_t": 2, "uint16_t": 2,
            "int": 4, "": 4, "int32_t": 4, "uint32_t": 4, "float": 4, "DWORD": 4}.get(t, 8)


def scope_at(text: str, pos: int) -> str:
    """The named namespaces open at `pos`, as a `a::b::` prefix (an anonymous one adds nothing)."""
    code = re.sub(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'",
                  lambda m: " " * len(m.group(0)), text[:pos], flags=re.S)
    stack: list[str | None] = []
    for m in re.finditer(r"\bnamespace\s+([\w:]*)\s*\{|[{}]", code):
        tok = m.group(0)
        if tok.startswith("namespace"):
            stack.append(m.group(1) or "")
        elif tok == "{":
            stack.append(None)
        elif stack:
            stack.pop()
    return "".join(n + "::" for n in stack if n)


def port_globals() -> dict[int, dict]:
    """The reference globals the port holds, by the address comment on their declaration."""
    out = {}
    for p in (ROOT / "src").rglob("*.[ch]pp"):
        if "gen" in p.relative_to(ROOT).parts:
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for m in PORT_GLOBAL.finditer(text):
            decl = m.group(1).strip()
            out[int(m.group(4), 16)] = {
                "name": m.group(2), "file": p.relative_to(ROOT).as_posix(), "decl": decl,
                "const": bool(re.match(r"(?:static\s+)?(?:const|constexpr)\b", decl)) and "*" not in decl,
                "scalar": bool(SCALAR.match(decl)) and not m.group(3),
                "qual": scope_at(text, m.start()) + m.group(2),
                "tls": "thread_local" in decl,
            }
    return out


EXPORT_BEGIN = "// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----"
EXPORT_END = "// ---- end of generated globals ----"


def write_exports(exported: dict[int, dict]):
    """Give each internal-linkage global translated code reaches a C-linkage pointer to it.

    The global stays where it is (an anonymous-namespace or file `static` cannot be named from
    another object); a block at the end of its own file takes its address, and translated code
    reads and writes through that, so it shares the port's state instead of the image's."""
    by_file = defaultdict(list)
    for base, g in exported.items():
        by_file[g["file"]].append((base, g))
    for p in (ROOT / "src").rglob("*.cpp"):
        rel = p.relative_to(ROOT).as_posix()
        if rel.startswith("src/gen/"):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        had = EXPORT_BEGIN in text
        if not had and rel not in by_file:
            continue
        kept = re.sub(r"\n*" + re.escape(EXPORT_BEGIN) + r".*?" + re.escape(EXPORT_END) + r"\n?", "\n",
                      text, flags=re.S).rstrip("\n") + "\n"
        if rel in by_file:
            lines = ["", EXPORT_BEGIN]
            for base, g in sorted(by_file[rel]):
                lines.append(f'extern "C" void* const gen_p_{base:x} = (void*)&{g["qual"]};')
            lines.append(EXPORT_END)
            kept += "\n".join(lines) + "\n"
        if kept != text:
            p.write_text(kept, encoding="utf-8", newline="\n")


def load_data_sizes() -> dict[str, int]:
    """Each port global's size, by its unqualified name (the smallest, when the name repeats)."""
    out = subprocess.run([str(F.LLVM / "llvm-pdbutil.exe"), "pretty", "-globals", str(F.PORT_PDB)],
                         capture_output=True, text=True, errors="replace").stdout
    sizes = {}
    for m in re.finditer(r"^\s*data \[0x[0-9a-f]+, sizeof=(\d+)\] .*?(\w+)(?:\[\d*\])*\s*$", out, re.M):
        name, size = m.group(2), int(m.group(1))
        sizes[name] = min(size, sizes.get(name, size))
    return sizes


def load_data_publics() -> list[str]:
    out = subprocess.run([str(F.LLVM / "llvm-pdbutil.exe"), "dump", "-publics", str(F.PORT_PDB)],
                         capture_output=True, text=True, errors="replace").stdout
    # namespace-scope variables mangle as @3, static data members as @2
    return re.findall(r"S_PUB32 \[size = \d+\] `(\?[^`]*@[23][^`]*)`\n\s+flags = none", out)


# ---- translation --------------------------------------------------------------------------------

class Untranslatable(Exception):
    """A body the rewrite finds it cannot take: the function is stubbed with the reason."""


class Translator:
    def __init__(self, depth: int):
        self.depth = depth
        self.cor = load_corpus()
        self.by_name: dict[str, list[int]] = defaultdict(list)
        for a, e in self.cor.items():
            self.by_name[e["name"]].append(a)
        self.refdata = load_refdata()
        # globals the instructions use as floating point, where the export only gave a size
        for addr, kind in floatdata.load().items():
            size, gtype = self.refdata.get(addr, (0, "undefined"))
            if gtype.startswith("undefined") and size in (0, 4 if kind == "float" else 8):
                self.refdata[addr] = (4 if kind == "float" else 8, kind)
        self.ref = Binary()
        self._lifted: dict[int, str | None] = {}
        self._arity: dict[int, int | None] = {}
        self._extra_params: dict[int, dict[int, str]] = {}
        self.mismatched: set[int] = set()   # ports whose signature is not the reference's
        self.callargs = callargs.CallArgs(self.ref)
        self.patches = json.loads((ROOT / "tools/translate_patches.json").read_text(encoding="utf-8"))
        fid = json.loads(F.OUT_JSON.read_text())
        # hand ports only: the translator's own output is measured too, and is not a port
        self.tied = {int(r["addr"], 16): r for r in fid
                     if not r["file"].replace("\\", "/").startswith(("src/gen/", "src\\gen\\"))}
        # the port side, cached: a link that fails leaves no executable to read, and the
        # translator is what fixes it
        if F.PORT_EXE.exists() and (not PORT_CACHE.exists()
                                    or PORT_CACHE.stat().st_mtime < F.PORT_EXE.stat().st_mtime):
            procs, _ = F.load_pdb(Binary(F.PORT_EXE))
            cache = {"procs": [[va, size, name] for va, size, name in procs],
                     "publics": {str(k): v for k, v in load_publics().items()},
                     "signatures": {str(k): v for k, v in load_signatures().items()},
                     "data_publics": load_data_publics(),
                     "data_sizes": load_data_sizes()}
            PORT_CACHE.write_text(json.dumps(cache), encoding="utf-8")
        cache = json.loads(PORT_CACHE.read_text(encoding="utf-8"))
        self.port_va_by_name = defaultdict(list)
        for va, size, name in cache["procs"]:
            self.port_va_by_name[name].append(va)
        self.publics = {int(k): v for k, v in cache["publics"].items()}
        self.signatures = {int(k): tuple(v) for k, v in cache["signatures"].items()}
        # the port's own globals: translated code must read the port's state, not the image's
        self.held = port_globals()
        self.held_sorted = sorted(self.held)
        data_publics = cache.get("data_publics", [])
        if "data_sizes" not in cache:
            cache["data_sizes"] = load_data_sizes()
            PORT_CACHE.write_text(json.dumps(cache), encoding="utf-8")
        for addr, g in self.held.items():
            # the compiler's own size for it: a span past it would land in other port memory
            g["size"] = cache["data_sizes"].get(g["name"])
            hits = [n for n in data_publics if re.match(rf"\?{re.escape(g['name'])}@(?:\w+@)*@[23]", n)]
            g["mangled"] = hits[0] if len(hits) == 1 else None
        self.global_aliases: dict[int, str] = {}
        self.exported: dict[int, dict] = {}
        cache_path = ROOT / "build/fidelity-ref-cache.json"
        self.refcache = json.loads(cache_path.read_text())["functions"] if cache_path.exists() else {}
        self.demoted = json.loads(DEMOTED.read_text()) if DEMOTED.exists() else {}
        self.exceptions = json.loads((ROOT / "tools/fidelity_exceptions.json").read_text(encoding="utf-8"))

    # ---- classification -------------------------------------------------------------------
    def port_target(self, a: int):
        """(va, mangled, signature) when the port has a callable counterpart of `a`."""
        rec = self.tied.get(a)
        # a port stub returns nothing: the reference's body, translated, does better
        if rec is None or rec["kind"] == "stub":
            return None
        vas = self.port_va_by_name.get(rec["name"], [])
        if len(vas) > 1:
            # overloads: the one taking as many arguments as the reference, when only one does
            ref = self.signature(a)
            if ref is not None:
                vas = [v for v in vas if v in self.signatures
                       and len(self.signatures[v][1]) + (1 if self.signatures[v][2] else 0) == len(ref[2])]
        if len(vas) != 1:
            return None
        va = vas[0]
        mangled = self.publics.get(va)
        sig = self.signatures.get(va)
        if mangled is None or sig is None:
            return None
        ret, args, this = sig
        if this and re.search(r"@@[CKS]", mangled):
            # a static member function (MSVC's C/K/S access codes): no this, whatever the
            # signature record says
            this = False
            sig = (ret, args, this)
        if ret == "complex" or any(c == "complex" for c in args):
            return None
        # a port that takes fewer arguments than the reference (and is not variadic, `...`
        # mangling as a trailing ZZ) is not that function with that signature: the reference's
        # callers would hand it their arguments in the wrong places (ProtectedCall drops nargs)
        ref = self.signature(a)
        if ref is not None and len(args) + (1 if this else 0) < len(ref[2]) and mangled.startswith("?") \
                and not mangled.endswith("ZZ"):
            self.mismatched.add(a)
            return None
        return va, mangled, sig

    def body(self, a: int) -> str | None:
        e = self.cor.get(a)
        if not e or "c" not in e:
            return self.lifted(a)
        text = e["c"].replace("\r\n", "\n")
        for patch in self.patches.get(f"{a:x}", []):
            if text.count(patch["from"]) != 1:
                raise SystemExit(f"tools/translate_patches.json: FUN_{a:x}'s patch no longer matches once")
            text = text.replace(patch["from"], patch["to"])
        text = wide_multiply(strip_retcheck(text)[0])
        # a function-local static's guard: MSVC's per-thread epoch (TLS slot 0x2140) compared with
        # the guard, then _Init_thread_header / _footer. All three are the port's own runtime's,
        # so the protocol stays one runtime's.
        text = INIT_EPOCH.sub("_Init_thread_epoch", text)
        # Ghidra's function-pointer types (`_func_5014 *`) are code
        text = re.sub(r"\b_func_\w+\b", "code", text)
        text = virtual_this(text)
        text = self.xmm_results(text)
        text = global_pieces(text)
        return self.recover_arguments(a, self.restore_lengths(text))

    # the image's C++ static initializers: the table FUN_14026b874 hands _initterm
    # (`_initterm(&DAT_14260e3f8,&DAT_142617540)`)
    INIT_TABLE = (0x14260e3f8, 0x142617540)

    def static_initializers(self) -> list[int]:
        """The image's static initializers that may run at startup, in table order: decompiled,
        and touching no global the port holds (the port has built those itself; a translated
        initializer, redirected onto them, would build them a second time)."""
        if hasattr(self, "_inits"):
            return self._inits
        out = []
        for slot in range(self.INIT_TABLE[0], self.INIT_TABLE[1], 8):
            p = self.ref.u64(slot)
            e = self.cor.get(p)
            if not p or e is None or "c" not in e:
                continue
            touched = [int(m.group(2), 16) for m in SYMBOL.finditer(e["c"])]
            if any(self.held_global(x) for x in touched):
                continue
            out.append(p)
        self._inits = out
        return out

    def arity(self, b: int) -> int | None:
        """How many arguments `b` reads: the highest `param_N` its decompiled body uses (capped
        by the port's own count when it is adapted), or None when that is not known."""
        if b in self._arity:
            return self._arity[b]
        need = None
        e = self.cor.get(b)
        if e and "c" in e:
            head, _, rest = e["c"].partition("{")
            sig = self.signature(b)
            if "..." not in head and sig is not None:
                # the last parameter the body reads, by the header's own names (`this`, `L`,
                # `param_3` alike)
                names = []
                for p in sig[2]:
                    m = (re.search(r"\b(this|param_\d+)\b", p)
                         or re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", p))
                    names.append(m.group(1) if m else "")
                names = [n for n in names]
                need = max((k + 1 for k, n in enumerate(names) if n and re.search(rf"\b{n}\b", rest)), default=0)
        elif self.lifted(b) is not None:
            used = [int(n) for n in re.findall(r"\ba(\d)\b", self.lifted(b).partition("{")[2])]
            need = max(used, default=-1) + 1
        if need is not None and b in self.tied:
            pt = self.port_target(b)
            if pt is not None:
                _, mangled, (ret, args, this) = pt
                port_need = len(args) + (1 if this else 0)
                if mangled.endswith("ZZ"):
                    need = None
                elif need == 0:
                    # Ghidra saw no parameters (a body that hands its registers straight on, as
                    # SCritSect::Initialize does to InitializeCriticalSection): the port's own
                    # signature says what it reads
                    need = port_need
                else:
                    need = min(need, port_need)
        self._arity[b] = need
        return need

    def restore_lengths(self, text: str) -> str:
        """A length the decompiler computed and then dropped: a call one argument short whose
        last argument an inlined strlen just measured gets that length back."""
        out, last = [], 0
        for start, end, b, args in list(call_sites(text)):
            need = self.arity(b)
            if need is None or not args:
                continue
            if len(args) == need - 1:
                loops = [m for m in STRLEN_LOOP.finditer(text, 0, start) if m.group(2) == args[-1]]
                if loops:
                    out.append((end, f",{loops[-1].group(1)}"))
            elif len(args) == need - 2:
                # both the string and its length dropped: the strlen just before the call (no
                # other call between) measured the string the call pushes
                loops = list(STRLEN_LOOP.finditer(text, 0, start))
                if loops and not re.search(r"\b\w+\(", text[loops[-1].end():start]):
                    out.append((end, f",{loops[-1].group(2)},{loops[-1].group(1)}"))
        for pos, extra in reversed(out):
            text = text[:pos] + extra + text[pos:]
        return text

    def recover_arguments(self, a: int, text: str) -> str:
        """Constant arguments the decompiler dropped from a call, read back from the
        instructions (tools/callargs.py) when every call to that callee agrees on them."""
        e = self.cor.get(a)
        if not e:
            return text
        fixes = []
        nth: dict[tuple[int, int], int] = {}     # (callee, shown) -> which short call this is
        for start, end, b, args in list(call_sites(text)):
            need = self.arity(b)
            if need is None or len(args) >= need:
                continue
            sig = self.signature(b)
            positions = list(range(len(args), need))
            values = self.callargs.recover(a, e["size"], b, positions)
            if values is None:
                values = self.recover_by_elimination(a, e["size"], b, positions, text, len(args))
                if isinstance(values, tuple):
                    # one value list per short call, in their order
                    k = nth.get((b, len(args)), 0)
                    nth[(b, len(args))] = k + 1
                    values = values[k] if k < len(values) else None
            if values is not None:
                # a float travels in an xmm register: only an argument this function was itself
                # handed in the same place (a wrapper passing it on) carries it unchanged
                if sig is not None and any(p < len(sig[2]) and re.match(r"(float|double)\b", sig[2][p])
                                           and values[k] != f"param_{p + 1}"
                                           for k, p in enumerate(positions)):
                    continue
                own = len(self.signature(a)[2]) if self.signature(a) else 0
                for k, p in enumerate(positions):
                    m = re.fullmatch(r"param_(\d+)", str(values[k]))
                    if m and int(m.group(1)) > own:
                        # a parameter the header never declared (the function only hands it on)
                        ptype = sig[2][p].rsplit(None, 1)[0] if sig and p < len(sig[2]) else "undefined8"
                        self._extra_params.setdefault(a, {})[int(m.group(1))] = ptype
                spelled = [v if isinstance(v, str) else f"{v:#x}" for v in values]
                fixes.append((end, ("," if args else "") + ",".join(spelled)))
        for pos, extra in reversed(fixes):
            text = text[:pos] + extra + text[pos:]
        return text

    def xmm_results(self, text: str) -> str:
        """`extraout_XMM0_*`: part of the floating-point result a call left in xmm0, which Ghidra
        did not tie to the call (`FUN_x(L,i), (extraout_XMM0_Db >> 0x14 & 0x7ff) == 0x7ff` is an
        isfinite test on lua_tonumber's double). When the call just before each use returns a
        double or float, its result is kept and the part read from it."""
        decls = [m.span() for m in re.finditer(r"^\s*[\w ]+ extraout_XMM0_\w+;$", text, re.M)]
        uses = [u for u in re.finditer(r"\bextraout_XMM0_(Da|Db|Dc|Dd|Qa|Qb)\b", text)
                if not any(s <= u.start() < e for s, e in decls)]
        if not uses:
            return text
        sites = list(call_sites(text))
        wrap = {}
        for u in uses:
            before = [s for s in sites if s[1] < u.start()]
            if not before:
                return text
            start, end, b, _ = before[-1]
            ret = self.decl(b)[0]
            if ret not in ("double", "float"):
                return text
            wrap[start] = (end, ret)
        for start, (end, ret) in sorted(wrap.items(), reverse=True):
            member = "d" if ret == "double" else "f"
            text = text[:start] + f"(gen_xmm0.{member} = " + text[start:end + 1] + ")" + text[end + 1:]
        part = {"Da": "u32[0]", "Db": "u32[1]", "Dc": "u32[2]", "Dd": "u32[3]", "Qa": "u64[0]", "Qb": "u64[1]"}
        text = re.sub(r"\bextraout_XMM0_(Da|Db|Dc|Dd|Qa|Qb)\b", lambda m: f"gen_xmm0.{part[m.group(1)]}", text)
        text = re.sub(r"^\s*[\w ]+ gen_xmm0\.\w+(?:\[\d\])?;\n", "", text, flags=re.M)   # its declaration
        first = text.index("{\n") + 2
        return (text[:first] + "  union { double d; float f; unsigned int u32[4]; unsigned long long u64[2]; } gen_xmm0;\n"
                + text[first:])

    def recover_by_elimination(self, a: int, size: int, b: int, positions: list[int], text: str, shown: int):
        """The calls to `b` disagree on the dropped arguments (luaL_error with a different
        message at each), but the calls the text shows in full account for some of them: take
        those out, and when what is left agrees, it is what the short calls pass."""
        every = self.callargs.recover(a, size, b, positions, every=[])
        if not every:
            return None
        sites = [args for _, _, t, args in call_sites(text) if t == b]
        short = [args for args in sites if len(args) == shown]
        full = [args for args in sites if len(args) > max(positions)]
        if len(every) != len(short) + len(full):
            return None

        def same(machine, written: str) -> bool:
            written = written.strip()
            if isinstance(machine, int):
                try:
                    return int(written, 0) & 0xffffffffffffffff == machine
                except ValueError:
                    return False
            m = re.fullmatch(r"&DAT_([0-9a-f]+)", str(machine))
            if m and written.startswith('"') and written.endswith('"'):
                literal = written[1:-1].encode().decode("unicode_escape")
                return self.ref.cstr(int(m.group(1), 16)) == literal
            return str(machine) == written

        left = list(every)
        for args in full:
            for k, values in enumerate(left):
                if values is not None and all(same(v, args[p]) for v, p in zip(values, positions)):
                    del left[k]
                    break
            else:
                return None
        if len(left) != len(short) or any(x is None for v in left for x in v):
            return None
        if all(v == left[0] for v in left):
            return left[0]
        # they differ: the short calls in the text's order are the remaining instructions in
        # theirs, when every value is the same kind (all locals, all constants, all globals) so a
        # pairing could not swap, say, a count for a pointer
        def kind(v):
            if isinstance(v, int):
                return "int"
            m = re.match(r"(?:local|param|&DAT)", str(v))
            return m.group(0) if m else "expression"
        if len({tuple(kind(x) for x in v) for v in left}) != 1:
            return None
        return tuple(left)

    def lost_arguments(self, text: str) -> str | None:
        for _, _, b, args in call_sites(text):
            need = self.arity(b)
            if need is not None and len(args) < need:
                return f"the decompiler lost a call's arguments (FUN_{b:x} reads {need}, the call passes {len(args)})"
        return None

    def lifted(self, a: int) -> str | None:
        """Code with no decompiled function, lifted from its instructions (tools/lift.py): only
        an entry point of its own, not a label inside some function."""
        if a not in self._lifted:
            root = self.ref.func_containing(a)
            inside = root is not None and root != a and root in self.cor
            self._lifted[a] = None if inside or not self.ref.is_code(a) else lift.lift(self.ref, a)
        return self._lifted[a]

    def callees(self, text: str) -> set[int]:
        out = set()
        for m in FUNREF.finditer(text):
            out.add(int(m.group(1), 16))
        # code whose address is taken (callbacks, vtable thunks) runs when it is called back
        for m in re.finditer(r"&LAB_(1[0-9a-f]{8})\b", text):
            a = int(m.group(1), 16)
            if a in self.cor or self.lifted(a) is not None:
                out.add(a)
        for name in CALL.findall(text):
            if "::" in name and not name.startswith("Import::"):
                cands = self.by_name.get(name, [])
                if len(cands) == 1:
                    out.add(cands[0])
        return out

    def signature(self, a: int) -> tuple[str, str, list[str]] | None:
        """(return type, name, [parameter declarations]) from the decompiled header."""
        # the header as decompiled (the body's corrections never touch it, and reading the
        # corrected body here would recurse through the callers' arities)
        e = self.cor.get(a)
        text = e["c"].replace("\r\n", "\n") if e and "c" in e else self.lifted(a)
        if text is None:
            return None
        head = re.sub(r"(?s)/\*.*?\*/", "", text.split("{", 1)[0]).strip()
        m = re.match(r"(?s)(.*?)\b([\w:~<>]+)\s*\((.*)\)\s*$", head)
        if not m:
            return None
        ret = re.sub(r"\b__\w+\b", "", m.group(1)).strip() or "void"
        # Ghidra's return types disagree between a function and its callers (one decompiles it
        # void, another uses rax), so every function hands back a register's worth
        if ret == "void":
            ret = "undefined8"
        params = [p.strip() for p in m.group(3).split(",") if p.strip() and p.strip() != "void"]
        extra = self._extra_params.get(a, {}) if hasattr(self, "_extra_params") else {}
        if extra and len(params) < max(extra):
            # parameters found passed on at a call (recover_arguments); any between them that
            # nothing names are registers all the same
            for n in range(len(params) + 1, max(extra) + 1):
                params.append(f"{extra.get(n, 'undefined8')} param_{n}")
        return ret, m.group(2), params

    def held_global(self, addr: int):
        """(base, port global) when `addr` lies in a global the port holds, else None.

        A scalar spans 8 bytes; an aggregate runs to the next held global, at most 0x400."""
        i = bisect.bisect_right(self.held_sorted, addr) - 1
        if i < 0:
            return None
        base = self.held_sorted[i]
        g = self.held[base]
        nxt = self.held_sorted[i + 1] if i + 1 < len(self.held_sorted) else base + 0x400
        if g.get("size"):
            span = g["size"]
        elif g["scalar"]:
            span = min(scalar_size(g["decl"]), nxt - base)
        else:
            span = min(nxt - base, 0x400)
        return (base, g) if addr < base + span else None

    FLOAT_TYPE = re.compile(r"^(?:float|double|float4|float8|float10)$")

    def float_arguments(self, body: str, params: list[str]) -> str:
        """A floating point parameter handed an argument C types as an integer gets its bits.

        CALLF calls unprototyped, so an argument goes by its own type: Ghidra's `*param_3` (an
        undefined8 that holds a double) would travel in a general register while the callee
        reads xmm - garbage there (C_Timer's seconds came out out of range and the timer's result
        was never built, 2026-10-12). An argument with a floating point type in it is left
        alone; the rest are reinterpreted, a float's four bytes promoted to double."""
        floats = {m.group(2) for m in re.finditer(r"^\s*(float|double|float4|float8|float10)\s+\**(\w+)(?:\[[^\]]*\])?;",
                                                  body, re.M)}
        floats |= {p.split()[-1] for p in params if len(p.split()) == 2 and self.FLOAT_TYPE.match(p.split()[0])
                   and re.fullmatch(r"\w+", p.split()[-1])}
        floaty = re.compile(r"\b(?:" + "|".join(sorted(floats) or ["(?!)"]) + r")\b|\((?:float|double)\)|"
                            r"\*\s*\((?:float|double)\s*\*\)|REFG\((?:float|double)\b|CALLF\((?:float|double)\b|"
                            r"\b\d+\.\d*(?:e[+-]?\d+)?\b|\b\d+e[+-]?\d+\b|extraout_XMM|gen_xmm0\.[df]|"
                            r"\b(?:SQRT|ABS|FLOAT_|GEN_AS_)\w*\(")
        # a float parameter, not a pointer to one (`float *param_7`)
        def scalar_float(p: str) -> bool:
            return bool(p.split()) and "*" not in p and bool(self.FLOAT_TYPE.match(p.split()[0]))

        # last call first: a call in another's arguments is done before the call around it, whose
        # arguments are read again from the text as it stands
        calls = list(re.finditer(r"CALLF\(([^,()]+(?:\s*\*)*), FUN_([0-9a-f]{9})\)\(", body))
        for m in reversed(calls):
            start = m.end()
            i = matching_paren(body, start)
            args = split_args(body[start:i - 1])
            sig = self.signature(int(m.group(2), 16))
            changed = False
            for k, arg in enumerate(args):
                if sig is not None and k < len(sig[2]) and scalar_float(sig[2][k]):
                    if floaty.search(arg):
                        continue
                    ptype = sig[2][k].split()[0]
                    helper = "GEN_AS_FLOAT" if ptype in ("float", "float4") else "GEN_AS_DOUBLE"
                    args[k] = f"{helper}((unsigned long long)({arg}))"
                    changed = True
                elif k >= 4 and not floaty.search(arg) and not arg.startswith("(unsigned long long)("):
                    # from the fifth on an argument is a stack slot of 8 bytes, and an unprototyped
                    # call stores an int's 4 only: the callee read the rest as garbage (a CStatus *
                    # of 0x100000000 loading the cursor's textures, 2026-10-12)
                    args[k] = f"(unsigned long long)({arg})"
                    changed = True
            if changed:
                body = body[:start] + ",".join(args) + body[i - 1:]
        return body

    def decl(self, a: int) -> tuple[str, list[str] | None]:
        """(return type, parameters) as C can declare them; parameters None when Ghidra's use a
        type C does not know, so the definition takes none and reads nothing."""
        sig = self.signature(a)
        if sig is None:
            return "undefined8", None
        ret, _, params = sig

        def known(t: str) -> bool:
            words = t.replace("*", " ").split()
            return all(w in PRIMITIVE or w.startswith("undefined") or w in ("const", "unsigned", "signed")
                       for w in words)

        if not known(ret):
            ret = "undefined8"
        if ret.startswith("undefined") and a in self.tied:
            # Ghidra did not know the result is floating point (lua_tonumber): the port's own
            # signature does, and a double comes back in xmm0, not rax
            pt = self.port_target(a)
            if pt is not None and pt[2][0] in ("double", "float"):
                ret = pt[2][0]
        if not all(known(" ".join(p.split()[:-1]) + " " + ("*" * p.split()[-1].count("*"))) for p in params):
            return ret, None
        return ret, params

    # ---- the work set ---------------------------------------------------------------------
    def plan(self, roots: list[int]) -> dict:
        translate, stub, adapt = {}, {}, {}
        frontier = [(a, 0) for a in roots]
        seen = set()
        while frontier:
            a, d = frontier.pop()
            if a in seen:
                continue
            seen.add(a)
            reason = self.why_not(a, root=d == 0)
            if reason is None:
                translate[a] = d
                for b in self.callees(self.body(a)):
                    if b in seen or b in CRT_BY_ADDR or self.is_lib(b):
                        continue
                    if self.port_target(b) is not None and b not in roots:
                        adapt[b] = True
                        seen.add(b)
                    # (a port the translated code cannot call, internal or returning an aggregate,
                    # or whose signature is not the reference's, is translated like any other:
                    # what it shares with the port is redirected to the port's own)
                    elif d + 1 <= self.depth:
                        frontier.append((b, d + 1))
                    else:
                        stub[b] = "beyond the translation depth"
                        seen.add(b)
            else:
                stub[a] = reason
        return {"translate": translate, "stub": stub, "adapt": adapt}

    def is_lib(self, a: int) -> bool:
        e = self.cor.get(a)
        if e is None:
            return False
        name = e["name"]
        return a in {int(k, 16) for k in self.exceptions.get("lib", {})} or (
            not name.startswith(F.REF_KEEP) and "::" not in name and name.lstrip("_") in CRT)

    def why_not(self, a: int, root: bool) -> str | None:
        if f"{a:x}" in self.demoted:
            return "did not compile: " + self.demoted[f"{a:x}"]
        text = self.body(a)
        if text is None:
            return "not in the corpus"
        for reason, rx in HARD:
            hits = {m.group(0) for m in rx.finditer(text)} - set(upper_half_only(text))
            if hits:
                return reason
        if any(g in text for g in ("DAT_14261a0b0", "DAT_14261a0b8")):
            return "an anti-tamper check the corpus could not strip"
        sig = self.signature(a)
        if sig is None:
            return "no readable signature"
        lost = self.lost_arguments(text)
        if lost:
            return lost
        # a control-flow-guard check standing alone may be all that is left of an indirect call
        # (or a plain call to the guard routine, which guards nothing): the instructions say which
        if re.search(r"^\s*_guard_check_icall\([^;]*\);", text, re.M) and a in self.cor:
            shown = len(re.findall(r"\(\*+\s*\([^;]*?\)\s*\(|\(\*\w+\)\s*\(", text))
            if self.callargs.indirect_calls(a, self.cor[a]["size"]) > shown:
                return "an indirect call the decompiler lost"
        # the reference's own call list (tools/fidelity.py's cache, once the function has been
        # measured): a body that shows under half of it is one the decompiler lost
        ref = self.refcache.get(f"{a:x}")
        if ref is not None and len(ref["calls"]) >= 2:
            shown = len(FUNREF.findall(text)) + sum(1 for n in CALL.findall(text) if "::" in n)
            if shown * 2 < len(ref["calls"]):
                return f"the decompiler lost calls ({shown} shown, the reference makes {len(ref['calls'])})"
        words = [x or y for x, y in TYPE_WORD.findall(text.split("{", 1)[1])]
        for word in words + [w for p in sig[2] for w in p.split()[:-1]]:
            w = word.strip("*")
            if w and w not in PRIMITIVE and not w.startswith(("undefined", "unsigned", "const")):
                return f"a type C does not know ({w})"
        return None

    # ---- rewriting ------------------------------------------------------------------------
    def rewrite(self, a: int, text: str, uses: set) -> str:
        """The decompiled function as C: callees by address, globals at their addresses."""
        head, rest = text.split("{", 1)
        sig = self.signature(a)
        ret, _, params = sig
        # `_local_a8`: Ghidra's name for a wider store over a declared local (`local_a8` an
        # undefined4, the store 8 bytes: CONCAT44), as `_DAT_x` is for a global
        rest = re.sub(r"\b_((?:local|[a-z]*Stack)_[0-9a-f]+)\b",
                      lambda m: f"(*(undefined8 *)&{m.group(1)})"
                      if re.search(rf"^\s*[\w ]+\**\s*{m.group(1)}\b", rest, re.M) else m.group(0), rest)
        body = stack_frame(atomics("{" + rest))
        for name in upper_half_only(body):
            body = re.sub(rf"^(\s*[\w ]+ {name});$", r"\1 = 0;", body, count=1, flags=re.M)
        # switch cases are labels Ghidra qualifies with `::`
        body = re.sub(r"\b(switchD_\w+)::(\w+)", r"\1__\2", body)
        # a function's address taken, `&FUN_x`: the reference address, which the jumps dispatch
        body = re.sub(r"&\s*\b(?:thunk_)?FUN_([0-9a-f]{9})\b(?!\s*\()", r"((code *)REFP(0x\1))", body)
        body = re.sub(r"&\s*\bLAB_([0-9a-f]{9})\b", r"((code *)REFP(0x\1))", body)
        if re.match(r"(?s)\s*(?:/\*.*?\*/\s*)*void\b", head):
            # A function Ghidra calls void because it ends in a tail call (FUN_140d11f10 jumps
            # to GetObjectFromLua and hands its object back in rax): a call just before a return
            # leaves its result where the caller reads one, so the return carries it.
            body = re.sub(r"^(\s*)((?:thunk_)?FUN_[0-9a-f]{9}\([^;]*\)|\(\*+\(code \*+\)[^;]*\)\([^;]*\));"
                          r"(\s*\n\s*)return\s*;",
                          r"\1return (undefined8)\2;", body, flags=re.M)
        body = re.sub(r"\breturn\s*;", "return 0;", body)

        def bare(m):
            # a named reference function used as a value (`_guard_check_icall`)
            cands = self.by_name.get(m.group(1), [])
            return f"((code *)REFP(0x{cands[0]:x}))" if len(cands) == 1 else m.group(0)

        body = re.sub(r"(?<![\w.>])(_[A-Za-z]\w+)\b(?!\s*\()", bare, body)

        def call(m):
            name = m.group(1)
            if name in ("if", "while", "for", "switch", "return", "sizeof", "do", "else"):
                return m.group(0)
            if name.startswith("Import::"):
                return m.group(0)
            if "::" in name:
                cands = self.by_name.get(name, [])
                if len(cands) == 1:
                    target = cands[0]
                    if target in CRT_BY_ADDR:
                        return CRT_BY_ADDR[target] + "("
                    uses.add(target)
                    return f"CALLF({self.decl(target)[0]}, @FUN_{target:x})("
                return m.group(0)
            if name.startswith(("FUN_", "thunk_FUN_")):
                target = int(name[-9:], 16)
                if target in CRT_BY_ADDR:
                    return CRT_BY_ADDR[target] + "("
                uses.add(target)
                return f"CALLF({self.decl(target)[0]}, @FUN_{target:x})("
            # FID names a runtime routine it matched more than once `FID_conflict_<name>`
            plain = name.removeprefix("FID_conflict_").lstrip("_")
            if plain in CRT:
                return plain + "("
            return m.group(0)

        body = CALL.sub(call, body)

        def funref(m):
            # a function used as a value: its reference address, which the jump stubs dispatch
            return f"((code *)REFP(0x{m.group(1)}))"

        # (calls were marked `@FUN_x` above so this leaves them alone)
        body = re.sub(r"(?<!@)\b(?:thunk_)?FUN_([0-9a-f]{9})\b(?!\s*\()", funref, body).replace("@FUN_", "FUN_")

        def global_(m):
            whole, addr = m.group(1), int(m.group(2), 16)
            if whole.startswith(("s_", "u_", "ws_", "PTR_s_", "PTR_u_")) and not whole.startswith("PTR_"):
                return f"((char *)REFP(0x{addr:x}))"
            if whole.startswith(("LAB_", "switchD_")):
                # labels stay labels
                return whole
            size, gtype = self.refdata.get(addr, (8, "undefined8"))
            if gtype.startswith("undefined") and size == 8 and addr in borrowed:
                gtype = borrowed[addr]
            expr = global_expr(gtype, size, addr)
            held = self.held_global(addr)
            if held is None or held[1]["const"]:
                # the reference's own, or a constant the image holds with the same value
                return expr
            base, g = held
            if g["mangled"] is not None:
                aliases[base] = g["mangled"]
            elif g["tls"] or not g["file"].endswith(".cpp"):
                raise Untranslatable(f"reaches the port's {g['name']} ({g['file']}), which has internal linkage")
            else:
                # an internal-linkage global: through the pointer write_exports gives it
                exported[base] = g
            # the same lvalue, at the port's global instead of the image's
            return expr.replace(f"(uintptr_t)0x{addr:x}ULL", f"((char *)&gen_g_{base:x} + 0x{addr - base:x})") \
                       .replace(f"REFG({c_type_of(gtype, size)}, 0x{addr:x})",
                                f"(*({c_type_of(gtype, size)} *)((char *)&gen_g_{base:x} + 0x{addr - base:x}))")

        aliases, exported = {}, {}

        # an untyped 8-byte global the function copies to or from a pointer local
        # (`piVar10 = DAT_142de0de8;`): the local's type is the global's
        local_types = {m.group(2): re.sub(r"\s+", " ", m.group(1)).strip()
                       for m in re.finditer(r"^\s*((?:\w+ )*\w+ ?\*+)\s*(\w+);$", body, re.M)}
        borrowed = {}
        for m in re.finditer(r"\b(\w+) = (?:\([^()]*\))?_?DAT_([0-9a-f]{9});|\b_?DAT_([0-9a-f]{9}) = (?:\([^()]*\))?(\w+);",
                             body):
            var, addr = (m.group(1), m.group(2)) if m.group(1) else (m.group(4), m.group(3))
            ctype = local_types.get(var)
            if ctype and ctype.endswith("*"):
                borrowed.setdefault(int(addr, 16), ctype.replace(" *", "*").replace("*", " *").strip())

        # a constant Ghidra wrote as the address of a "global" outside the image
        # (`&DAT_8000000000000020`, a blz::vector's inline capacity flag) is that constant
        def outside(m):
            v = int(m.group(1), 16)
            return m.group(0) if IMAGE_LO <= v < IMAGE_HI else f"((undefined1 *){v:#x}ULL)"

        body = re.sub(r"&\s*\b_?(?:PTR_)*(?:DAT|UNK|PTR|LAB)_([0-9a-f]+)\b", outside, body)
        for m in re.finditer(r"\b_?(?:PTR_)*(?:DAT|UNK|PTR)_([0-9a-f]+)\b", body):
            if not IMAGE_LO <= int(m.group(1), 16) < IMAGE_HI:
                raise Untranslatable(f"reads a global outside the image ({m.group(0)})")
        # A global the data export does not type is `undefined` (one byte) to Ghidra, so its
        # `&DAT_x + n` is a byte offset; the 8-byte type it is declared with for loads would scale
        # it by 8 (FUN_14068c500's raster table read entry 3 at +0x60, not +0xc: a garbage cull
        # mode, the pipeline rejected and the device failed - the white screen of 2026-10-12)
        body = re.sub(r"&\s*(_?DAT_([0-9a-f]{9}))(?=\s*[+-][^+-=>])",
                      lambda m: m.group(0) if int(m.group(2), 16) in self.refdata else f"(undefined1 *)&{m.group(1)}",
                      body)
        # `p != &DAT_x` that ends a walk over the reference's own array just below x is the
        # array's bound, not the port global that happens to follow it: FUN_1413cf260 walked
        # 0x1430ad328.. until the address of g_wmoPortalInteriorFade, ran off and faulted (the
        # crash applying MSAA, 2026-10-12)
        used = {int(a, 16) for a in re.findall(r"\b_?DAT_([0-9a-f]{9})\b", body)}

        def bound(m):
            x = int(m.group(2), 16)
            held = self.held_global(x)
            if held is None or held[0] != x:
                return m.group(0)
            below = [y for y in used if x - 0x1000 <= y < x and self.held_global(y) is None]
            if not below:
                return m.group(0)
            return f"{m.group(1)}((undefined1 *)REFP(0x{x:x}))"

        body = re.sub(r"((?:[!=]=|[<>]=?)\s*)&\s*_?DAT_([0-9a-f]{9})\b(?!\s*[\[.+-])", bound, body)
        body = SYMBOL.sub(global_, body)
        body = RAM.sub(lambda m: f"REFG(undefined{ {'u': 4, 'l': 8, 'c': 1, 's': 2}.get(m.group(1)[0], 8)}, 0x{m.group(2)})", body)
        body = IMPORT.sub(lambda m: f"(*(code *)&{m.group(2)})", body)
        # a global called through: a code pointer, whatever the export typed it
        body = re.sub(r"\(\*REFG\(undefined\d?, (0x[0-9a-f]+)\)\)\s*\(", r"(*REFG(code *, \1))(", body)
        if re.search(r"\*REFG\(undefined\d?,|REFG\(undefined\d?, 0x[0-9a-f]+\)\s*\[", body):
            raise Untranslatable("a global used as a pointer the data export does not type")
        body = self.float_arguments(body, params)
        # callers are unprototyped, so a float argument arrives promoted to double
        decls, narrow = [], []
        for p in params:
            m = re.match(r"^(?:float|float4)\s+(\w+)$", p)
            if m:
                decls.append(f"double {m.group(1)}_promoted")
                narrow.append(f"  float {m.group(1)} = (float){m.group(1)}_promoted;")
            else:
                decls.append(p)
        if narrow:
            body = "{\n" + "\n".join(narrow) + body[1:]
        # only a body that is taken holds the port's globals
        self.global_aliases.update(aliases)
        self.exported.update(exported)
        return f"{ret} FUN_{a:x}({', '.join(decls) or 'void'})\n{body.rstrip()}\n"

    def port_reads(self, body: str, adapted) -> dict[str, dict]:
        """What a translated body reads out of the port's objects, by where the object came from.

        An object comes from an adapted port function (its result) or a port global (its value,
        or the global itself when it is an aggregate). Offsets are the reference's; a vtable slot
        is a call through `*obj + slot`. Tracking is by variable name, so it is a review list,
        not a proof."""
        origins = {}
        for m in re.finditer(r"\b(\w+) = (?:\([^()]*\))?CALLF\([^,]+, FUN_([0-9a-f]{9})\)\(", body):
            a = int(m.group(2), 16)
            # strings, fresh memory and hashes are bytes, not a port layout
            if a in adapted and not re.match(r"(lua_|luaL_|SMem|SStr)", self.tied[a]["name"]):
                origins[m.group(1)] = f"{self.tied[a]['name']} (FUN_{a:x}) result"
        for m in re.finditer(r"\b(\w+) = [^;]*?gen_g_([0-9a-f]{9})\b[^;]*;", body):
            g = self.held.get(int(m.group(2), 16))
            if g is not None:
                origins[m.group(1)] = f"{g['name']} ({g['file']}) value"
        out = defaultdict(lambda: {"offsets": set(), "vtable": set()})
        for var, origin in origins.items():
            for m in re.finditer(rf"\*\s*{var}\s*\+\s*(0x[0-9a-f]+|\d+)\)\)", body):
                out[origin]["vtable"].add(int(m.group(1), 0))
            for m in re.finditer(rf"\*\s*\([\w ]+\*+\s*\)\s*\(\s*{var}\s*\+\s*(0x[0-9a-f]+|\d+)\s*\)", body):
                out[origin]["offsets"].add(int(m.group(1), 0))
            if re.search(rf"\*\([\w ]+\*\)\s*{var}\b", body):
                out[origin]["offsets"].add(0)
        # an aggregate global read in place
        for m in re.finditer(r"&gen_g_([0-9a-f]{9}) \+ 0x([0-9a-f]+)\)", body):
            g = self.held.get(int(m.group(1), 16))
            if g is not None and not g["scalar"]:
                out[f"{g['name']} ({g['file']}) itself"]["offsets"].add(int(m.group(2), 16))
        return {k: {"offsets": [f"0x{o:x}" for o in sorted(v["offsets"])],
                    "vtable": [f"0x{o:x}" for o in sorted(v["vtable"])]}
                for k, v in out.items() if v["offsets"] or v["vtable"]}

    # ---- emission -------------------------------------------------------------------------
    def emit(self, roots: list[int], handlers: dict[int, str]) -> dict:
        plan = self.plan(roots)
        GEN.mkdir(exist_ok=True)
        for old in GEN.glob("translated_*.c"):
            old.unlink()
        uses: set[int] = set()
        bodies = {}
        imports, dlls = set(), set()
        for a in sorted(plan["translate"]):
            text = self.body(a)
            mine: set[int] = set()
            try:
                bodies[a] = self.rewrite(a, text, mine)
            except Untranslatable as e:
                del plan["translate"][a]
                plan["stub"][a] = str(e)
                continue
            uses |= mine
            imports.update(m.group(2) for m in IMPORT.finditer(text))
            dlls.update(m.group(1) for m in IMPORT.finditer(text))
        # whatever is called and neither translated nor adapted is stubbed
        for b in uses:
            if b not in plan["translate"] and b not in plan["adapt"] and b not in plan["stub"]:
                plan["stub"][b] = "called, not in the work set"
        # declared without parameters: Ghidra's argument counts disagree between a function and
        # its callers, and an unprototyped declaration takes them all
        prototypes = [f"{self.decl(a)[0]} FUN_{a:x}();"
                      for a in sorted(set(plan["translate"]) | set(plan["adapt"]) | set(plan["stub"]))]
        header = ["/* GENERATED by tools/translate.py; do not edit by hand. */",
                  "#ifndef GEN_TRANSLATED_H", "#define GEN_TRANSLATED_H", "", "#include \"gen/Ghidra.h\"", ""]
        # the import libraries of the DLLs the reference's imports come from (combase's exports
        # are in ole32.lib)
        header += [f"#pragma comment(lib, \"{'ole32' if d == 'COMBASE_DLL' else d.removesuffix('_DLL').lower()}.lib\")"
                   for d in sorted(dlls)]
        header += [f"__declspec(dllimport) unsigned long long {name}();" for name in sorted(imports)]
        # the port's own globals, where the translated code would read the image's
        for base, mangled in sorted(self.global_aliases.items()):
            header += [f"/* {self.held[base]['name']} ({self.held[base]['file']}) */",
                       f"#pragma comment(linker, \"/alternatename:gen_g_{base:x}={mangled}\")",
                       f"extern char gen_g_{base:x};"]
        for base, g in sorted(self.exported.items()):
            header += [f"/* {g['name']} ({g['file']}), internal: through its pointer */",
                       f"extern void *const gen_p_{base:x};",
                       f"#define gen_g_{base:x} (*(char *)gen_p_{base:x})"]
        write_exports(self.exported)
        header += prototypes + ["", "#endif", ""]
        write_out(GEN / "translated.h", "\n".join(header), encoding="utf-8", newline="\n")

        # the translated bodies, a few hundred to a file, each with its line range kept
        lines_of = {}
        chunk = sorted(bodies)
        hit_index = {a: i for i, a in enumerate(chunk)}
        for n in range(0, len(chunk), PER_FILE):
            path = GEN / f"translated_{n // PER_FILE:03}.c"
            out = ["/* GENERATED by tools/translate.py from build/corpus.jsonl; do not edit by hand. */",
                   "#include \"gen/translated.h\"", "#pragma warning(disable: 4013 4024 4028 4047 4090 4133 4142 4189 4245 "
                   "4244 4267 4305 4306 4307 4310 4312 4389 4700 4701 4702 4703 4715 4716 4100 4101 4102 4146 "
                   "4018 4057 4098 4113 4152 4204 4459 4456 4457 4127 4334 4554 4805 4800 4996)", ""]
            for a in chunk[n:n + PER_FILE]:
                start = len(out) + 1
                out.append(f"/* ref: FUN_{a:x} */")
                body = bodies[a].splitlines()
                # a call count per function (gen_hits, written out under FROST_HITS): which
                # translated code a run reaches, and how often, for choosing what to port by hand
                brace = body.index("{") if "{" in body else -1
                if brace >= 0:
                    body.insert(brace + 1, f"  GEN_HIT({hit_index[a]});")
                out.extend(body)
                lines_of[a] = (path.name, start, len(out))
                out.append("")
            write_out(path, "\n".join(out) + "\n", encoding="utf-8", newline="\n")
        hits = ["/* GENERATED by tools/translate.py; do not edit by hand. */",
                "/* each translated function's call count, by its index in gen_hit_addrs */",
                f"unsigned int gen_hits[{max(1, len(chunk))}];",
                f"const unsigned int gen_hit_count = {len(chunk)};",
                "const unsigned long long gen_hit_addrs[] = {"]
        hits += [f"    0x{a:x}ULL," for a in chunk] or ["    0"]
        hits += ["};", ""]
        write_out(GEN / "hits.c", "\n".join(hits), encoding="utf-8", newline="\n")

        # adapters to the port's own functions
        adapters = ["/* GENERATED by tools/translate.py; do not edit by hand. */", "#include \"gen/translated.h\"",
                    "#pragma warning(disable: 4047 4024 4312 4311 4302 4244 4305 4022 4113 4133 4142 4028)", ""]
        abi = {"int": "unsigned long long", "float": "float", "double": "double", "void": "void"}
        # an adapter takes the port function's own arguments (the callers are unprototyped, so a
        # float arrives promoted to double) and passes them on with the port's ABI
        promoted = {"int": "unsigned long long", "float": "double", "double": "double"}
        for a in sorted(plan["adapt"]):
            va, mangled, (ret, args, this) = self.port_target(a)
            gret = self.decl(a)[0]
            classes = (["int"] if this else []) + list(args)
            alias = f"gen_alias_{a:x}"
            params = [f"{promoted[c]} a{i}" for i, c in enumerate(classes)]
            pass_args = [f"({abi[c]})a{i}" for i, c in enumerate(classes)]
            fptr = f"{abi[ret]} (*)({', '.join(abi[c] for c in classes) or 'void'})"
            call = f"(({fptr})&{alias})({', '.join(pass_args)})"
            proto = f"{gret} FUN_{a:x}({', '.join(params) or 'void'})"
            if ret == "void":
                inner = f"    {call};\n    return 0;"
            elif ret == "int" and ("float" in gret or "double" in gret):
                inner = f"    return ({gret})(long long){call};"
            else:
                inner = f"    return ({gret}){call};"
            adapters += [f"#pragma comment(linker, \"/alternatename:{alias}={mangled}\")",
                         f"extern char {alias};", f"{proto} {{", inner, "}", ""]
        write_out(GEN / "adapters.c", "\n".join(adapters), encoding="utf-8", newline="\n")

        # stubs
        stubs = ["/* GENERATED by tools/translate.py; do not edit by hand. */", "#include \"gen/translated.h\"",
                 "#pragma warning(disable: 4100 4047 4024 4716)", ""]
        for a, why in sorted(plan["stub"].items()):
            ret = self.decl(a)[0]
            stubs += [f"/* {why} */", f"{ret} FUN_{a:x}() {{", f"    GenUnimplemented(0x{a:x}ULL);",
                      f"    return ({ret})0;", "}", ""]
        write_out(GEN / "stubs.c", "\n".join(stubs), encoding="utf-8", newline="\n")

        # the jumps the reference image dispatches through: translated and adapted functions.
        # Whatever calls a reference address (a port callback, a vtable) enters translated code
        # through a guarded entry with the function's own signature: a fault there returns 0 to
        # that caller, as a guarded handler does, rather than unwinding through the port's frames
        # (their locks and cleanup) to some outer guard.
        jumps = ["/* GENERATED by tools/translate.py; do not edit by hand. */", "#include \"gen/translated.h\"",
                 "#include <excpt.h>", "#pragma warning(disable: 4013 4024 4047 4133 4244 4305 4312 4100)", "",
                 "int GenFault(unsigned long long address, struct _EXCEPTION_POINTERS* info);", ""]
        entry_of = {}
        for a in sorted(plan["translate"]):
            ret, params = self.decl(a)
            if params is not None and any("..." in p for p in params):
                continue
            params = params or []
            names = [re.search(r"(\w+)\s*$", p).group(1) for p in params]
            entry_of[a] = f"gen_entry_{a:x}"
            jumps += [f"static {ret} gen_entry_{a:x}({', '.join(params) or 'void'}) {{",
                      "    __try {",
                      f"        return CALLF({ret}, FUN_{a:x})({', '.join(names)});",
                      f"    }} __except (GenFault(0x{a:x}ULL, GetExceptionInformation())) {{",
                      f"        return ({ret})0;",
                      "    }", "}"]
        # the image's static initializers that translated, in the table's order (RefImage.cpp
        # runs them once the image is mapped)
        inits = [p for p in self.static_initializers() if p in plan["translate"]]
        jumps += ["", "const unsigned long long g_genInits[] = {"]
        jumps += [f"    0x{p:x}ULL," for p in inits] + ["    0", "};", f"const size_t g_genInitCount = {len(inits)};"]
        jumps += ["", "typedef struct { unsigned long long m_address; void* m_target; } GENJUMP;",
                  "const GENJUMP g_genJumps[] = {"]
        reachable = sorted(set(plan["translate"]) | set(plan["adapt"]))
        jumps += [f"    {{ 0x{a:x}ULL, (void*)&{entry_of.get(a, f'FUN_{a:x}')} }}," for a in reachable]
        jumps += ["    { 0, 0 }", "};", f"const size_t g_genJumpCount = {len(reachable)};", ""]
        write_out(GEN / "jumps.c", "\n".join(jumps), encoding="utf-8", newline="\n")

        # the Lua handlers: the port registers them by their C++ names
        fwd = ["// GENERATED by tools/translate.py; do not edit by hand.", "// The Lua API handlers translated from the reference (src/gen/translated_*.c).", "",
               "#include \"glue/ScriptAPIHandlers.hpp\"", "", "#include <windows.h>", "",
               "extern \"C\" {", "int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);"]
        fwd += [f"unsigned long long FUN_{a:x}(unsigned long long param_1);" for a in sorted(handlers) if a in plan["translate"]]
        fwd += ["}", ""]
        for a, cname in sorted(handlers.items(), key=lambda kv: kv[1]):
            if a in plan["translate"]:
                # a fault in translated code (state the port does not have yet) costs the call,
                # not the client; Lua's own errors pass through the filter
                fwd += [f"// translated: FUN_{a:x}", f"int {cname}(lua_State* L) {{",
                        "    __try {",
                        f"        return static_cast<int>(FUN_{a:x}(reinterpret_cast<unsigned long long>(L)));",
                        f"    }} __except (GenFault(0x{a:x}ull, GetExceptionInformation())) {{",
                        "        return 0;", "    }", "}", ""]
        write_out(GEN / "handlers.cpp", "\n".join(fwd), encoding="utf-8", newline="\n")

        port_reads = {f"{a:x}": r for a in sorted(bodies) if (r := self.port_reads(bodies[a], plan["adapt"]))}
        manifest = {
            "translated": {f"{a:x}": {"depth": d, "file": lines_of[a][0], "lines": lines_of[a][1:]}
                           for a, d in sorted(plan["translate"].items())},
            # where translated code reads a port object at the reference's offsets: each is right
            # only if the port's layout matches there (tools/translate.py --risks)
            "port_reads": port_reads,
            "adapted": sorted(f"{a:x}" for a in plan["adapt"]),
            "stubbed": {f"{a:x}": why for a, why in sorted(plan["stub"].items())},
            "handlers": {f"{a:x}": cname for a, cname in sorted(handlers.items()) if a in plan["translate"]},
        }
        write_out(MANIFEST, json.dumps(manifest, indent=1) + "\n", encoding="utf-8", newline="\n")
        return manifest


def stub_handlers() -> dict[int, str]:
    """The Lua API handlers no hand port covers (tools/gen_script_api.py lists them all; run
    its --emit first)."""
    every = {int(a, 16): n for a, n in json.loads((ROOT / "build/script-handlers.json").read_text()).items()}
    covered = set()
    for path in (ROOT / "src").rglob("*.cpp"):
        rel = path.relative_to(ROOT).as_posix()
        if rel.startswith("src/gen/") or path.name in ("ScriptAPIRegistry.cpp", "ScriptAPIStubs.cpp"):
            continue
        covered |= {int(a, 16) for a in re.findall(r"// ref: FUN_([0-9a-fA-F]+)\n(?:static )?int Lua_",
                                                   path.read_text(errors="replace"))}
    # `xor eax, eax; ret` handlers are ported whole by the generator
    stubs = (ROOT / "src/glue/ScriptAPIStubs.cpp").read_text(encoding="utf-8")
    covered |= {int(a, 16) for a in re.findall(r"// Returns nothing\.\n// ref: FUN_([0-9a-f]+)", stubs)}
    return {a: n for a, n in every.items() if a not in covered}


def build_errors() -> list[tuple[str, int, str]]:
    # the generated files are globbed: reconfigure, or a changed count builds the old list
    subprocess.run(["cmake", str(ROOT / "build/cmake")], capture_output=True)
    out = subprocess.run(["cmake", "--build", str(ROOT / "build/cmake"), "--config", "Release", "--target", "gen"],
                         capture_output=True, text=True, errors="replace").stdout
    errors = []
    for line in out.splitlines():
        m = re.search(r"([\w.]+\.c)\((\d+),?\d*\): (?:fatal )?error (C\d+): (.*)$", line)
        if m:
            errors.append((m.group(1), int(m.group(2)), f"{m.group(3)} {m.group(4).strip()[:120]}"))
    if errors:
        return errors
    other = [l.strip() for l in out.splitlines() if re.search(r"\berror [A-Z]+\d+", l)]
    if other:
        # an error outside the translated bodies (a header, the runtime): nothing to demote,
        # and certainly not clean
        raise SystemExit("the gen library does not build:\n" + "\n".join(other[:10]))
    # it compiles: then link, and name the translated functions whose symbols do not resolve
    out = subprocess.run(["cmake", "--build", str(ROOT / "build/cmake"), "--config", "Release", "--target", "FrostLite"],
                         capture_output=True, text=True, errors="replace").stdout
    for m in re.finditer(r"unresolved external symbol (\S+) referenced in function FUN_([0-9a-f]{9})", out):
        errors.append(("LINK", int(m.group(2), 16), f"LNK2019 unresolved {m.group(1)}"))
    return errors


def report_risks() -> None:
    """The port objects translated code reads, by origin: the layouts to check first."""
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    by_origin = defaultdict(lambda: {"offsets": set(), "vtable": set(), "functions": set()})
    for a, reads in manifest.get("port_reads", {}).items():
        for origin, r in reads.items():
            by_origin[origin]["offsets"] |= set(r["offsets"])
            by_origin[origin]["vtable"] |= set(r["vtable"])
            by_origin[origin]["functions"].add(a)
    checked = json.loads((ROOT / "tools/translate_layouts.json").read_text(encoding="utf-8"))
    def note_of(origin):
        # checked only when every offset it is read at was checked
        for k, v in checked.items():
            if not k.startswith("_") and re.search(k, origin):
                if "offsets" not in v or by_origin[origin]["offsets"] <= set(v["offsets"]):
                    return v["note"]
        return None

    unchecked = {o for o in by_origin if note_of(o) is None}
    print(f"{len(manifest.get('port_reads', {}))} translated functions read port objects at the reference's "
          f"offsets, through {len(by_origin)} origins; {len(unchecked)} unchecked "
          f"(tools/translate_layouts.json records the checked ones)")
    for origin, r in sorted(by_origin.items(), key=lambda kv: (kv[0] not in unchecked, -len(kv[1]["functions"]))):
        if origin not in unchecked:
            print(f"  {len(r['functions']):4} fns  checked  {origin}: {note_of(origin)}")
            continue
        key = lambda o: int(o, 16)
        line = f"  {len(r['functions']):4} fns  {origin}: offsets {' '.join(sorted(r['offsets'], key=key)) or '-'}"
        if r["vtable"]:
            line += f"; vtable slots {' '.join(sorted(r['vtable'], key=key))}"
        print(line)


def report_faults(log: Path) -> None:
    """The faults a run's log recorded in translated code, each at its translated source line."""
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    rx = re.compile(r"\[translated\] FUN_([0-9a-f]+) faulted \((\w+) at ([0-9A-Fa-f]+), touching ([0-9a-f]+)\)")
    faults = [m.groups() for m in rx.finditer(log.read_text(encoding="utf-8", errors="replace"))]
    print(f"{len(faults)} faults in translated code")
    for handler, code, at, touching in faults:
        sym = subprocess.run([str(F.LLVM / "llvm-symbolizer.exe"), f"--obj={F.PORT_EXE}", f"0x{at}"],
                             capture_output=True, text=True, errors="replace").stdout.split("\n")
        where = sym[1].strip() if len(sym) > 1 else "?"
        func = sym[0].strip()
        src = ""
        m = re.match(r"(.*):(\d+):\d+$", where)
        if m and Path(m.group(1)).exists():
            src = Path(m.group(1)).read_text(encoding="utf-8").splitlines()[int(m.group(2)) - 1].strip()
        t = int(touching, 16)
        kind = f"a null object (+0x{t:x})" if t < 0x10000 else f"a wild pointer 0x{t:x}"
        suspects = [o for o, r in manifest.get("port_reads", {}).get(func.removeprefix("FUN_"), {}).items()
                    if f"0x{t:x}" in r["offsets"]]
        print(f"  {manifest['handlers'].get(handler, 'FUN_' + handler)}: {code} in {func} reading {kind}")
        print(f"      {Path(where).name}: {src}")
        if suspects:
            print(f"      the object is likely: {', '.join(suspects)}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--risks", action="store_true", help="list the port objects translated code reads, and stop")
    ap.add_argument("--faults", metavar="LOG", help="map a run log's translated faults to source, and stop")
    ap.add_argument("--handlers", action="store_true", help="the stubbed Lua API handlers as roots")
    ap.add_argument("--addr", nargs="*", default=[], help="more roots")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--fix", action="store_true", help="build, demote what fails to compile, repeat")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.risks:
        return report_risks()
    if args.faults:
        return report_faults(Path(args.faults))

    handlers = {}
    if args.handlers:
        handlers = stub_handlers()
    # hand-written forwarders (`// translated: FUN_x` outside src/gen) call the translation
    forwarded = set()
    for path in (ROOT / "src").rglob("*.cpp"):
        if not path.relative_to(ROOT).as_posix().startswith("src/gen/"):
            forwarded |= {int(a, 16) for a in re.findall(r"^// translated: FUN_([0-9a-f]{9})$",
                                                         path.read_text(errors="replace"), re.M)}
    roots = sorted(set(handlers) | forwarded | {int(a.lower().removeprefix("fun_"), 16) for a in args.addr})
    if args.fix and DEMOTED.exists():
        # every demotion is retried: what failed to compile under an older translator may
        # compile now (a recovered argument, a type), and the loop below re-demotes what does not
        DEMOTED.unlink()
    for round_ in range(1, 30 if args.fix else 2):
        t = Translator(args.depth)
        manifest = t.emit(sorted(set(roots) | set(t.static_initializers())), handlers)
        print(f"translated {len(manifest['translated'])}, adapted {len(manifest['adapted'])}, "
              f"stubbed {len(manifest['stubbed'])} ({len(manifest['handlers'])} handlers)", file=sys.stderr)
        if not args.fix:
            break
        errors = build_errors()
        if not errors:
            print("builds clean", file=sys.stderr)
            break
        demoted = json.loads(DEMOTED.read_text()) if DEMOTED.exists() else {}
        by_file = defaultdict(list)
        for a, info in manifest["translated"].items():
            by_file[info["file"]].append((info["lines"][0], info["lines"][1], a))
        hit = 0
        for file, line, message in errors:
            if file == "LINK":
                if f"{line:x}" not in demoted:
                    demoted[f"{line:x}"] = message
                    hit += 1
                continue
            for start, end, a in by_file.get(file, []):
                if start <= line <= end and a not in demoted:
                    demoted[a] = message
                    hit += 1
                    break
        DEMOTED.write_text(json.dumps(demoted, indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        print(f"round {round_}: {len(errors)} errors, {hit} functions demoted", file=sys.stderr)
        if hit == 0:
            print("errors outside translated bodies:", errors[:5], file=sys.stderr)
            break


if __name__ == "__main__":
    main()
