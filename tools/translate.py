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
import gc
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

# numpy (through fidelity) starts OpenBLAS with a buffer per core, ~620 MB of commit on a
# 20-thread machine before anything runs; the translator does no linear algebra
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import callargs
import fidelity as F
import floatdata
import lift
import v16
import aegis
from corpus import load_corpus, strip_retcheck
from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "src/gen"


def write_out(path: Path, text: str, **kw) -> None:
    """write_text that waits out another process holding the file (an IDE indexer does, and a
    failed write mid-run leaves src/gen inconsistent). A file whose content would not change is
    not touched, so its timestamp stays and the build does not compile it again: a --fix round
    that demotes a few functions rebuilds only their files."""
    import time
    try:
        if path.exists() and path.read_text(encoding=kw.get("encoding", "utf-8"), errors="replace") == text:
            return
    except OSError:
        pass
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
# corpus functions to a file's address range (about a quarter of them translated): ~195 files
PER_FILE = 450
# how many 8-byte argument slots a variadic port's adapter passes on, fixed ones included (the widest
# translated call, to SStrPrintf, passes under 20)
VA_SLOTS = 24

PRIMITIVE = {
    "void", "undefined", "undefined1", "undefined2", "undefined3", "undefined4", "undefined5", "undefined6",
    "undefined7", "undefined8", "byte", "uchar", "uint1", "ushort", "word", "wchar16", "uint", "uint3", "dword",
    "ulong", "longlong", "ulonglong", "qword", "uint8", "uint4", "uint2", "int4", "int8", "int2", "sbyte",
    "float4", "float8", "float10", "pointer", "pointer64", "code", "char", "short", "int", "long", "float", "double",
    "bool", "wchar_t", "HANDLE", "size_t", "unsigned", "signed", "uint5", "uint6", "uint7", "int3", "int5",
    "int6", "int7", "int1", "__time64_t", "__int64", "uintptr_t", "va_list",
}
# the Windows names src/gen/Ghidra.h declares, which the typed import slots bring into the corpus
WINDOWS_TYPES = {
    "BOOL", "BOOLEAN", "BYTE", "WORD", "DWORD", "DWORD64", "UINT", "INT", "LONG", "ULONG", "SHORT", "USHORT",
    "CHAR", "WCHAR", "LONGLONG", "ULONGLONG", "HRESULT", "LSTATUS", "LCID", "ATOM", "SIZE_T", "ULONG_PTR",
    "DWORD_PTR", "UINT_PTR", "LONG_PTR", "INT_PTR", "LPARAM", "WPARAM", "LRESULT", "SOCKET", "u_short", "u_long",
    "u_int", "u_char", "errno_t", "rsize_t", "wint_t", "__uint64",
    "HMODULE", "HINSTANCE", "HWND", "HDC", "HKEY", "HLOCAL", "HGLOBAL", "HCURSOR", "HICON", "HMENU", "HDROP",
    "HIMC", "HBRUSH", "FARPROC", "LPVOID", "PVOID", "LPCVOID", "LPSTR", "LPCSTR", "LPWSTR", "BSTR", "LPCWSTR",
    "LPDWORD", "LPBOOL", "LPBYTE", "LPLONG", "LPCRITICAL_SECTION", "PSRWLOCK", "PCONDITION_VARIABLE",
    "LPSECURITY_ATTRIBUTES", "LPOVERLAPPED", "PCCRL_CONTEXT", "PCCERT_CONTEXT", "LPRECT", "LPPOINT",
    "LPUNKNOWN", "LPCGUID", "PRAWINPUTDEVICELIST", "PUNWIND_HISTORY_TABLE",
    "PFILETIME", "LPFILETIME", "PHKEY", "LPHANDLE", "PHANDLE", "HCERTSTORE", "PCCERT_CHAIN_CONTEXT",
    "HCRYPTHASH", "HCRYPTPROV", "HCRYPTKEY", "LPSYSTEMTIME", "PSYSTEMTIME", "fd_set",
    "_OSVERSIONINFOA", "DWORDLONG", "_FILETIME", "FILETIME", "_LARGE_INTEGER", "LARGE_INTEGER", "tagRECT",
    "_SYSTEM_INFO", "SYSTEM_INFO", "IMAGE_DOS_HEADER", "_IMAGE_DOS_HEADER", "_iobuf",
}
PRIMITIVE |= WINDOWS_TYPES
# the C library's own structs and types, which src/gen/Ghidra.h names the way Ghidra writes them
PRIMITIVE |= {"tm", "FILE", "_onexit_t", "__time32_t", "__time64_t", "intptr_t", "uintptr_t", "time_t", "size_t", "mbstate_t"}
# Ghidra.h's 16-byte value (tools/v16.py)
PRIMITIVE |= {"gen_v16"}
HARD = [
    ("register the decompiler lost track of", re.compile(r"\b(in_|unaff_|extraout_)\w+")),
    ("SIMD union", re.compile(r"\._\d+_\d+_")),
    # (`LOCK(); ... UNLOCK();` blocks are mapped onto Interlocked operations by atomics())
    # (the home area, entry + 8 .. + 0x28, is a frame of its own the function owns and
    # stack_frame() seeds with the parameters; what is refused is an address in the frame,
    # `&stack0x...`, and a slot above the home area, the caller's stack arguments)
    # (`&stack0x...`, an address in the frame, is stack_frame()'s; the slot read as a value is not)
    ("the stack frame addressed directly", re.compile(r"(?<!&)\b(stack0x\w+|\w*StackX_(?:2[89a-f]|[3-9a-f][0-9a-f]|[0-9a-f]{3,}))\b")),
    # (any thread-local is Ghidra.h's emulated block. The static guards' epoch at +0x2140 there is
    # the image template's INT_MIN, and nothing raises it: so `guard > epoch` always holds, and
    # every check goes to the port runtime's _Init_thread_header, which initializes the guard
    # when it is 0 and returns at once when it was. Slower than the port's own epoch, which
    # init_epoch_pointers() reaches where it knows the shape, and correct)
    # (`swi(3)` and `swi(0x29)`, int3 and __fastfail, are Ghidra.h's; any other interrupt is not)
    ("bad data or a trap", re.compile(r"halt_baddata|\bswi\((?!3\)|0x29\))|software_interrupt|halt_unimplemented")),
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
               0x14026bca8: "_Init_thread_header",
               # _setjmp(buf, frame), its longjmp named: C has no body for it, it is the port's
               # (Lua's protected calls, libjpeg's error exit; its every register read was
               # `unaff_RBX`..., 2026-10-16)
               0x1425bf1f0: "GEN_SETJMP",
               # the CRT's sin, cos, tan and cosf, FID-unnamed: identified by running them
               # (tools/oracle.py: each gives libm's result bit for bit at 0.5 and 1.0). Their
               # argument reduction returns a double and the quadrant in rax at once, which C
               # cannot (`extraout_RAX` after it, 2026-10-17); Lua::sin/cos/tan call the first three
               0x142417530: "sin", 0x142416680: "cos", 0x14241b9f0: "tan", 0x142411e70: "cosf",
               # and fmod (oracle), frexp and acos (what Lua::frexp and Lua::acos call; acos(0.25)
               # under the oracle is 1.3181...): their error paths return the error handler's
               # double, which the decompiler shows as `extraout_XMM0_Qa`
               0x14241a730: "fmod", 0x140282f8c: "frexp", 0x142417a9c: "acos"}
# reference runtime entry points code stores as pointers, jumped to the port's own runtime: a
# call through the pointer reaches the same routine a direct call does. curl_global_init
# (FUN_14216a080) fills curl's allocator table with these; without them every curl allocation
# landed on int3 and returned null (the Login click's _calloc_base, 2026-10-15).
# Never translated, either: the reference's malloc and realloc allocate from its CRT heap handle
# (DAT_142e1eda8), which only its CRT startup creates and the port never runs. Planned once
# translated code called them directly, the jump went to the translated body, and HeapAlloc on a
# null heap faulted in ntdll at shutdown (FUN_140286974, 2026-10-17)
CRT_ENTRY ={0x140286974: "malloc", 0x140279d94: "free", 0x140289938: "realloc",
             0x14241d3f8: "_strdup", 0x1402857f8: "calloc", 0x14241c294: "_wcsdup"}
# the UCRT's internal templates the corpus names, as the public routine each one implements:
# common_gmtime_s<__int64> (0x1424147d0) is _gmtime64_s, which cross_ssl's log timestamp
# (FUN_141f35740) calls; that function stubbed left its caller a string with no buffer, and the
# first log line crashed in memmove at startup (2026-10-15)
# the UCRT's internal templates by the public routine each instantiation is, where that routine
# takes the same arguments: FID names the template (`common_ftell<long>`), not the entry point
# around it, and a template is no symbol anything links (C2065 / LNK2019, 2026-10-16)
CRT_TEMPLATES = {"common_vsprintf<__crt_stdio_output::standard_base,char>": "__stdio_common_vsprintf",
                 "common_gmtime_s<__int64>": "_gmtime64_s", "common_gmtime_s<long>": "_gmtime32_s",
                 "common_localtime_s<__int64>": "_localtime64_s", "common_localtime_s<long>": "_localtime32_s",
                 "common_ftell<long>": "ftell", "common_ftell<__int64>": "_ftelli64",
                 "common_ftell_nolock<long>": "_ftell_nolock", "common_ftell_nolock<__int64>": "_ftelli64_nolock",
                 "common_fseek<long>": "fseek", "common_fseek<__int64>": "_fseeki64",
                 "common_lseek<long>": "_lseek", "common_lseek<__int64>": "_lseeki64",
                 "common_fgets<char>": "fgets", "common_fgets<wchar_t>": "fgetws",
                 "common_getenv<char>": "getenv", "common_getenv<wchar_t>": "_wgetenv",
                 "common_getenv_s<char>": "getenv_s", "common_getenv_s<wchar_t>": "_wgetenv_s",
                 "common_fsopen<char>": "_fsopen", "common_fsopen<wchar_t>": "_wfsopen",
                 "common_stat<_stat64>": "_stat64", "common_atof_l": "_atof_l", "common_atof_l<>": "_atof_l",
                 # (the float instantiation reads the same text: a double rounded once more)
                 "common_strtod_l<>": "_strtod_l",
                 "common_end_thread": "_endthreadex",
                 "FID_conflict:__stdio_common_vfwprintf": "__stdio_common_vfwprintf",
                 "FID_conflict___stdio_common_vfwprintf": "__stdio_common_vfwprintf",
                 # Ghidra's spelling of an inlined strncpy of a constant
                 "builtin_strncpy": "strncpy", "builtin_memcpy": "memcpy", "builtin_strcpy": "strcpy"}

# one printf-style conversion (`%%` included, so it can be told apart and skipped)
FORMAT_CONVERSION = re.compile(r"%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?"
                               r"(?:hh|h|ll|l|L|z|j|t|I64|I32|I)?[diouxXeEfgGaAcspn])")
CALL = re.compile(r"(?<![\w:])((?:[A-Za-z_]\w*::)+[~A-Za-z_]\w*|[A-Za-z_]\w*)\s*\(")
SYMBOL = re.compile(r"\b((?:PTR_)*(?:DAT|_DAT|UNK|_UNK|s|u|ws|LAB|vtable|switchD|PTR|[a-zA-Z]+Ram)_\w*?_?([0-9a-f]{9}))\b")
RAM = re.compile(r"\b([a-z]+)Ram0*(1[0-9a-f]{8})\b")
FUNREF = re.compile(r"\b(?:thunk_)?FUN_([0-9a-f]{9})\b")
IMPORT = re.compile(r"\bImport::(\w+)::(\w+)\b")
# imports Ghidra names by both of the exports it matched (the same code): one of them links.
# `htons_or_ntohs` (WS2_32) left the socket address setup on the logon path unlinked (LNK2019)
IMPORT_MERGED = {"htons_or_ntohs": "htons", "htonl_or_ntohl": "htonl",
                 # one `mov qword ptr [rcx], 0; ret` serves all three: either links
                 "InitializeConditionVariable_or_InitializeSRWLock_or_InitOnceInitialize": "InitializeSRWLock",
                 "InitializeConditionVariable_or_InitializeSRWLock": "InitializeSRWLock"}
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
            # half a million globals share a few thousand type names
            out[int(d["addr"], 16)] = (d["len"], sys.intern(d["type"]))
            if d.get("name", "").endswith("::vftable"):
                VFTABLES[d["name"]] = int(d["addr"], 16)
    return out


# the reference's vtables by Ghidra's name (`std::exception::vftable`), which a constructor or
# destructor stores by name: `*param_1 = std::exception::vftable;` is the table's address
VFTABLES: dict[str, int] = {}


GLOBAL_CAST = re.compile(r"\b_?DAT_([0-9a-f]{9})\s*(?:==|!=|=)\s*\(((?:\w+ )*\w+ ?\*+)\)")
GLOBAL_COPY = re.compile(r"\b(\w+) = (?:\([^()]*\))?_?DAT_([0-9a-f]{9});|\b_?DAT_([0-9a-f]{9}) = (?:\([^()]*\))?(\w+);")
POINTER_LOCAL = re.compile(r"^\s*((?:\w+ )*\w+ ?\*+)\s*(\w+);$", re.M)
EIGHT_BYTE = {"undefined8", "longlong", "ulonglong", "pointer", "pointer64", "double"}


def corpus_global_pointers(cor: dict) -> dict[int, str]:
    """The pointer type of each untyped global, where the whole corpus agrees on it.

    The data export leaves many 8-byte globals `undefined8`, while the decompiler, seeing them
    dereferenced, prints `*DAT_x` and `DAT_x[2]`: the translator cannot spell those without a
    pointer type, and guessing an element width is how the 2026-10-12 white screen came about.
    The function that creates such a global usually says it (`DAT_x = (undefined8 *)alloc(...)`,
    `DAT_x == (longlong *)0x0`, or a copy through a declared pointer local). Elements of 8 bytes
    (`longlong *`, `undefined8 *`, `undefined **`) access memory alike and count as one;
    a global that is cast to two widths is left alone. 82 of 220 such demotions settle."""
    seen: dict[int, set[str]] = defaultdict(set)

    def norm(t: str) -> str:
        t = re.sub(r"\s+", "", t)
        element = t[:-1]
        if element.endswith("*") or element in EIGHT_BYTE:
            return "undefined8 *"
        return re.sub(r"(\*+)$", r" \1", t)

    for e in cor.values():
        c = e.get("c", "")
        if "DAT_" not in c:
            continue
        for m in GLOBAL_CAST.finditer(c):
            seen[int(m.group(1), 16)].add(norm(m.group(2)))
        local = {d.group(2): d.group(1) for d in POINTER_LOCAL.finditer(c)}
        for m in GLOBAL_COPY.finditer(c):
            var, addr = (m.group(1), m.group(2)) if m.group(1) else (m.group(4), m.group(3))
            if var in local:
                seen[int(addr, 16)].add(norm(local[var]))
    return {a: next(iter(t)) for a, t in seen.items() if len(t) == 1}


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


# A global with a trailing address tag. The initializer may be `= x`, `{x}` or a constructor's
# `(a, b)` (CDataAllocator s_pool(0x20, 0x20); went untied until 2026-10-13); group 5 holds a
# constructor's arguments, so a prototype (`bool F(int x);`) can be told apart in port_globals.
PORT_GLOBAL = re.compile(r"^(?!(?:struct|class|union|enum|typedef|using|return)\b)([A-Za-z_][\w:<>,\t \*&]*?[\t \*&:])(\w+)(\[[^\]\n]*\])?"
                         r"(?:[\t ]*(?:=|\{)[^;\n]*|[\t ]*\(([^;\n]*)\))?;[\t ]*"
                         r"//\s*(?:ref:\s*)?(?:DAT_|0x)(14[0-9a-f]{7})\b", re.M)
# A function pointer global: `void (*s_hook)(int);  // DAT_x`, `BOOL(WINAPI* s_f)(HWND);  // DAT_x`.
PORT_FNPTR = re.compile(r"^(?!(?:typedef|using|return)\b)([A-Za-z_][\w:<>,\t \*&]*?)\(\s*(?:\w+\s*)?\*\s*(\w+)\s*\)\s*\([^;\n]*\)"
                        r"(?:[\t ]*=[^;\n]*)?;[\t ]*//\s*(?:ref:\s*)?(?:DAT_|0x)(14[0-9a-f]{7})\b", re.M)
# a parameter declaration (`int32_t x`, `const char* s`): what a prototype's parentheses hold
PARAM_DECL = re.compile(r"(?:^|,)\s*(?:const\s+)?[A-Za-z_][\w:<>]*[\s\*&]+(?:const\s+)?[A-Za-z_]\w*\s*(?=,|$)")
SCALAR = re.compile(r"^(?:static\s+|extern\s+|thread_local\s+)*(?:const\s+)?(?:u?int\d+_t|bool|float|double|char|"
                    r"int|unsigned|DWORD|size_t|uintptr_t|HANDLE|HWND|\w+\s*\*+)$")


# (Ghidra's prefix spells the type, one `p` per pointer level: `pppppplStack_350` is a
# `longlong ******` at -0x350, which a 1-4 letter prefix left out of the frame. It was the child
# pointer of a struct FUN_142578b20 builds at gen_frame + 0x18; zeroed as a separate variable, the
# frame's slot kept stale stack bytes, and the copy of that struct followed them, 2026-10-15)
STACK_DECL = re.compile(r"^(\s*)([A-Za-z_][\w ]*?)\s*(\**)\s*((?:local_|[a-z]+Stack_|[a-z]+StackX_)(res)?([0-9a-f]+))"
                        r"\s*(?:\[\s*(\w+)\s*\])?\s*;[^\n]*\n", re.M)


def stack_frame(body: str, params: list[str] | None = None) -> str:
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
    # a `StackX_` local shares its home slot with the `local_res` beside it: on the frame,
    # a write of either is seen by the other
    # (a home slot, `local_res8`, is on the frame too: there it holds its parameter)
    # `&stack0xfffffffffffffff8`: an address in the frame that no local is named at, the end of a
    # local array a loop runs to (30 functions were stubbed for it, 2026-10-16); entry-relative
    ends = {int(x, 16) - (1 << 64) if int(x, 16) >= 1 << 63 else int(x, 16)
            for x in re.findall(r"&stack0x([0-9a-f]+)\b", body)}
    if not ends and not arrays and not any(re.search(rf"&\s*{n}\b", body) for n in names) and \
            not any("StackX_" in n or "local_res" in n for n in names):
        return body
    offs = {m.group(4): (int(m.group(6), 16) if m.group(5) or "StackX_" in m.group(4) else -int(m.group(6), 16))
            for m in decls}
    lo = min([*offs.values(), *ends])
    home = any(8 <= o < 0x28 for o in offs.values())
    if home:
        # (the frame reaches down to the first home slot, which it seeds: with only `local_res10`
        # and up named, it began above entry + 8 and the seed's offset came out negative)
        lo = min(lo, 8)
    pad = (8 + lo) % 16          # the entry stack pointer is 8 past a 16-byte boundary
    top = max(decls, key=lambda m: offs[m.group(4)])
    # a local Ghidra declares a scalar and yet dereferences (`undefined8 uStack_20;` ...
    # `*uStack_20 = 0;`, a stack variable split from a pointer) is the pointer the casts
    # assigned to it agree on (`uStack_20 = (undefined8 *)...`): as declared, C2100 in 10
    # functions on 2026-10-16
    retyped = {}
    for m in decls:
        n = m.group(4)
        # (dereferenced: `*local`, `(longlong *)*local` after a cast, or `local[i]`)
        if m.group(3) or m.group(7) or not re.search(rf"(?:^|[^\w)\]\s]|\*\))\s*\*\s*{n}\b|\b{n}\s*\[", body, re.M):
            continue
        casts = set(re.findall(rf"\b{n} = \(((?:\w+ )*\w+ \*+)\)", body))
        if len(casts) == 1:
            retyped[n] = casts.pop()
        elif not casts and m.group(2).strip() in ("undefined8", "longlong", "ulonglong"):
            # (no cast says what it points at: an 8-byte slot read whole, `(longlong *)*local`)
            retyped[n] = "undefined8 *"
    # and a small byte array Ghidra also assigns whole (`auStack_10 = *(undefined8 *)...`): the
    # scalar of its size, which C can assign (C2106 in 6 functions, 2026-10-16)
    for m in decls:
        n = m.group(4)
        if m.group(7) in ("2", "4", "8") and not m.group(3) and m.group(2).strip() in ("undefined1", "undefined") \
                and re.search(rf"(?<![\w.>*]){n}\s*=(?!=)", body):
            retyped[n] = f"undefined{m.group(7)}"
    whole = {n for n, t in retyped.items() if not t.endswith("*")}
    ctype = lambda m: retyped.get(m.group(4)) or f"{m.group(2).strip()} {m.group(3)}".strip()
    size_of = lambda m: f"sizeof({ctype(m)}[{m.group(7)}])" if m.group(7) and m.group(4) not in whole \
        else f"sizeof({ctype(m)})"
    size = f"0x{pad + offs[top.group(4)] - lo:x} + {size_of(top)}"
    # The reference's locals fill its frame up to the entry stack pointer, and a struct whose
    # address escapes has fields past the last one Ghidra names: FUN_14020b2e0 hands
    # `&local_38` to FUN_141c011a0 as a hidden return pointer, the callee fills 0x20 bytes, and a
    # frame that ended at the last named local took the overrun into its /GS cookie at startup
    # (2026-10-15). So a frame of plain locals reaches the entry stack pointer.
    if offs[top.group(4)] < 0:
        size = f"({size} > 0x{pad - lo:x} ? {size} : 0x{pad - lo:x})"
    # The home area (entry + 8 .. + 0x28), when the function uses any of it, is there whole and
    # holds the register parameters, as a spill leaves them: a slot read before the function
    # writes it is the parameter the reference spilled there (a two-byte struct in rcx, read as
    # `_local_res8` and `cStackX_9`), not the caller's leftovers
    seeds = []
    if home:
        home_end = pad + 0x28 - lo
        size = f"({size} > 0x{home_end:x} ? {size} : 0x{home_end:x})"
        for k, p in enumerate((params or [])[:4]):
            name = re.search(r"(\w+)\s*$", p)
            if name and p.strip() != "...":
                seeds.append(f"{decls[0].group(1)}memcpy(gen_frame + 0x{pad + 8 * (k + 1) - lo:x}, &{name.group(1)}, "
                             f"sizeof({name.group(1)}) < 8 ? sizeof({name.group(1)}) : 8);\n")
    frame = (f"{decls[0].group(1)}__declspec(align(16)) unsigned char gen_frame[{size}];\n")
    for m in reversed(decls):
        body = body[:m.start()] + body[m.end():]
    first = re.search(r"\{\n", body).end()
    body = body[:first] + frame + "".join(seeds) + body[first:]
    for m in decls:
        n, at = m.group(4), pad + offs[m.group(4)] - lo
        if n in whole:
            # indexed as its bytes, its address the bytes', anything else the whole scalar
            body = re.sub(rf"(?<![\w.>])\b{n}\b(?=\s*\[)", f"((undefined1 *)(gen_frame + 0x{at:x}))", body)
            body = re.sub(rf"&\s*\b{n}\b", f"((undefined1 *)(gen_frame + 0x{at:x}))", body)
            body = re.sub(rf"\b{n}\b", f"(*({ctype(m)} *)(gen_frame + 0x{at:x}))", body)
        elif m.group(7):
            ptr = f"(({ctype(m)} *)(gen_frame + 0x{at:x}))"
            # `&arr` is the array's address; `&arr[i]` stays an element's address
            body = re.sub(rf"&\s*\b{n}\b(?!\s*\[)", ptr, body)
            body = re.sub(rf"\b{n}\b", ptr, body)
        else:
            body = re.sub(rf"\b{n}\b", f"(*({ctype(m)} *)(gen_frame + 0x{at:x}))", body)
    body = re.sub(r"&stack0x([0-9a-f]+)\b", lambda x: "((undefined1 *)(gen_frame + 0x{:x}))".format(
        pad + (int(x.group(1), 16) - (1 << 64) if int(x.group(1), 16) >= 1 << 63 else int(x.group(1), 16)) - lo),
        body)
    # a zero extension wider than a scalar stored at a frame slot (`_local_198 = ZEXT814(0);`,
    # 14 bytes from an 8-byte slot over the ones after it, FUN_141544600): the frame holds the
    # reference's layout, so its bytes there, zeroed, then the value's own (LNK2019 ZEXT814)
    def wide_zext(m):
        ind, at, a_, b_, v = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
        return (f"{ind}memset((gen_frame + 0x{at}), 0, {b_}); "
                f"{{ undefined{a_} gen_z = (undefined{a_})({v}); memcpy((gen_frame + 0x{at}), &gen_z, {a_}); }}")
    body = re.sub(r"^([ \t]*)\(\*\((?:\w+ )*\w+ \*\)\((?:\(undefined1 \*\))?\(?gen_frame \+ 0x([0-9a-f]+)\)\)?\) = "
                  r"ZEXT([1248])(9|1[0-5])\(([^;]*)\);", wide_zext, body, flags=re.M)
    return body


IMAGE_LO, IMAGE_HI = 0x140000000, 0x150000000   # the reference image's address range
INIT_EPOCH = re.compile(r"\*\(int \*\)\(\*\(longlong \*\)\(\(longlong\)ThreadLocalStoragePointer \+ "
                        r"\(ulonglong\)_tls_index \* 8\) \+\s*0x2140\)")
TLS_BLOCK = r"\*\(longlong \*\)\(\(longlong\)ThreadLocalStoragePointer \+ \(ulonglong\)_tls_index \* 8\)"
EPOCH_PTR = re.compile(r"\(int \*\)\(" + TLS_BLOCK + r"\s*\+\s*0x2140\)")
TLS_BLOCK_LOAD = re.compile(r"^(\s*)(\w+) = " + TLS_BLOCK + r";$", re.M)


# (a global's pieces, `DAT_14273f660._8_8_`, are global_pieces()'s: at its address, not `&` of the
# expression a global becomes, which was C2101 in 122 functions)
SIMD_PIECE = re.compile(r"(?<![\w.])(?!(?:_?(?:PTR_)*(?:DAT|UNK|PTR|s|u|ws|[a-z]+Ram)_\w*[0-9a-f]{9}\b))"
                        r"([A-Za-z_]\w*(?:\[[^\[\]]*\])?)\._(\d+)_(1|2|4|8)_(?!\w)")


ARG_REGISTER = re.compile(r"\bin_(?:R[CD]X|E[CD]X|[CD]X|[CD]L|R[89][DWB]?|XMM[0-3]_(?:D[a-d]|Qa))(?:_\d+)?\b")
CALL_KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof"}


def _is_call_argument(text: str, start: int, end: int) -> bool:
    """Whether text[start:end] is a whole argument of a call: between `(`/`,` and `,`/`)` (a cast
    in front allowed), the `(` opening a call rather than a condition or a group."""
    before = re.search(r"(\(\s*(?:\(\w+(?: \*+)?\)\s*)?|,\s*(?:\(\w+(?: \*+)?\)\s*)?)$", text[max(0, start - 40):start])
    if not before or not re.match(r"\s*[,)]", text[end:]):
        return False
    # the call's own `(`: walk back to the unmatched one
    depth, k = 0, start - 1
    while k >= 0:
        ch = text[k]
        if ch == ")":
            depth += 1
        elif ch == "(":
            if depth == 0:
                break
            depth -= 1
        elif ch in ";{}":
            return False
        k -= 1
    if k < 0:
        return False
    callee = re.search(r"([\w\]]+|\))\s*$", text[:k])
    return bool(callee) and callee.group(1) not in CALL_KEYWORDS


def register_upper_halves(text: str) -> str:
    """Register values that are leftovers in the reference too, as zero (as upper_half_only()
    reasons for a stack slot); a register read any other way stays refused:

    - `in_register_00000014`, `extraout_var`, `extraout_XMM0_Db` used only as the high part of a
      CONCAT: the bytes of an argument register above the argument (`CONCAT44(in_register_14,
      param_2)` for a 4-byte param_2 in rdx), or of a result register above the result. The Win64
      convention leaves them undefined.
    - an argument register the function never set, `in_R9`, used only as an argument of a call:
      what the call hands on there is whatever the register held, nothing the callee was given
      (an indirect call with a stack argument past an unset position, 2026-10-16)."""
    # (and `in_stack_fffffffffffffb4c`, the upper half of an outgoing stack argument the caller
    # wrote four bytes of, `CONCAT44(in_stack_x, 0x477)` or `(ulonglong)in_stack_x << 0x20`: the
    # caller's own frame slot, never written, read only to fill the slot; FUN_140222a60)
    upper = set(re.findall(r"\b(?:in_register_[0-9a-f]+|extraout_var(?:_\d+)?|extraout_XMM\d_D[b-d](?:_\d+)?"
                           r"|in_stack_[0-9a-f]{8,16})\b", text))
    # (and a callee-saved register handed on as a call argument as it came in, `unaff_R15` in a
    # stack argument slot a callee reads past what the caller set: nothing of this function's)
    saved = set(re.findall(r"\bunaff_(?:R(?:BX|BP|SI|DI)|R1[2-5][DWB]?|E(?:BX|BP|SI|DI))\b", text))
    for name in upper | set(ARG_REGISTER.findall(text)) | saved:
        uses = list(re.finditer(rf"\b{name}\b", text))
        decl = [u for u in uses if re.match(rf"[ \t]*[\w ]+\**\s*{name};$", text[text.rfind("\n", 0, u.start()) + 1:
                                                                               text.find("\n", u.end())])]
        rest = [u for u in uses if u not in decl]
        if name in upper:
            ok = all(re.search(r"CONCAT\d+\(\s*(?:\(\w+\))?\s*$", text[max(0, u.start() - 24):u.start()])
                     or (name.startswith("in_stack_") and re.search(r"\(ulonglong\)\s*$", text[max(0, u.start() - 12):u.start()])
                         and re.match(r"\s*<<\s*0x20\b", text[u.end():u.end() + 10])) for u in rest)
        else:
            # (or only its upper bits, `(int)((ulonglong)in_R9 >> 0x20)`: the half of a home slot
            # the function wrote 4 bytes of and read 8, which Ghidra takes for the register's)
            # (or the bytes above what the call is given, as a CONCAT's high part:
            # `CONCAT31((int3)((uint)in_R8D >> 8),0x24)`, r8b set and the rest left, FUN_141860a50)
            ok = all(_is_call_argument(text, u.start(), u.end())
                     or (re.search(r"\(\(ulonglong\)\s*$", text[max(0, u.start() - 16):u.start()])
                         and re.match(r"\s*>>\s*0x[2-3][0-9a-f]\)", text[u.end():u.end() + 12]))
                     or (re.search(r"CONCAT\d+\((?:\(\w+\)|\()*$", text[max(0, u.start() - 40):u.start()])
                         and re.match(r"(?:\s*>>\s*(?:0x)?[0-9a-f]+)?\)*,", text[u.end():u.end() + 16]))
                     for u in rest)
        if rest and ok:
            for u in decl:
                line_start = text.rfind("\n", 0, u.start()) + 1
                line_end = text.find("\n", u.end())
                # (as a comment of the same length, so the offsets of the later uses hold: a line
                # of spaces read as Ghidra's blank line after the declarations, and stack_frame
                # left out the ones below it, FUN_1412366b0's on-stack TSList split into three
                # C locals that NetEventListClear walked forever, 2026-10-17)
                n = line_end - line_start
                text = text[:line_start] + ("/*" + " " * (n - 4) + "*/" if n >= 4 else " " * n) + text[line_end:]
            text = re.sub(rf"\b{name}\b", "0", text)
    return text


def simd_pieces(text: str) -> str:
    """Ghidra's view of part of a wider variable, `auVar2._8_8_` (8 bytes at offset 8 of a
    16-byte value) or `uStack_418._1_1_`: the bytes at that offset of the variable, through a
    pointer of their width. The variable is a local, a parameter or an array element, so it has an
    address (88 functions were stubbed on "SIMD union", 2026-10-16). Pieces of another width
    stay as they are, and the function stays stubbed."""
    width = {"1": "undefined1", "2": "undefined2", "4": "undefined4", "8": "undefined8"}
    # an 8-byte piece holding a pointer and dereferenced (`*auVar91._0_8_`, `auVar91._0_8_[1]`
    # after `auVar91._0_8_ = (uint *)FUN_140317f20(...)`): the piece as that pointer type
    deref =set(re.findall(r"\*\s*([A-Za-z_]\w*)\._(\d+)_8_", text)) | set(re.findall(r"\b([A-Za-z_]\w*)\._(\d+)_8_(?=\s*\[)", text))
    for name, off in deref:
        casts = set(re.findall(rf"\b{name}\._{off}_8_ = \(((?:\w+ )*\w+ \*+)\)", text))
        typ = casts.pop() if len(casts) == 1 else "undefined8 *"
        # (only where it is dereferenced or indexed, and where the pointer is assigned: the other
        # uses read it as the integer it also is, `uVar * auVar91._0_8_`)
        piece = f"(*({typ}*)((char *)&{name} + {off}))"
        # (`*x` with no space is a dereference; `a * x`, spaced, a product)
        text = re.sub(rf"(?<=\*){re.escape(name)}\._{off}_8_(?!\w)", piece, text)
        text = re.sub(rf"(?<![\w.]){re.escape(name)}\._{off}_8_(?=\s*\[)", piece, text)
        text = re.sub(rf"(?<![\w.]){re.escape(name)}\._{off}_8_(?=\s*=\s*\({re.escape(typ.rstrip(' *'))})", piece, text)
    text = SIMD_PIECE.sub(lambda m: f"(*({width[m.group(3)]} *)((char *)&{m.group(1)} + {m.group(2)}))", text)
    # a piece of 3, 5, 6 or 7 bytes (`local_1e4._0_3_ = CONCAT12(...)`, an RGB triple): no C type
    # is that wide, so its bytes are copied in place, written as a statement, read as a value
    odd = r"(?<![\w.])(?!(?:_?(?:PTR_)*(?:DAT|UNK|PTR|s|u|ws)_\w*[0-9a-f]{9}\b))([A-Za-z_]\w*(?:\[[^\[\]]*\])?)\._(\d+)_([3567])_(?!\w)"
    text = re.sub(r"^([ \t]*)" + odd + r"\s*=\s*([^;]*?);", lambda m: f"{m.group(1)}GEN_PIECE_SET(&{m.group(2)}, "
                  f"{m.group(3)}, {m.group(4)}, {m.group(5)});", text, flags=re.M | re.S)
    return re.sub(odd, lambda m: f"GEN_PIECE(&{m.group(1)}, {m.group(2)}, {m.group(3)})", text)


def init_epoch_pointers(text: str) -> str:
    """The epoch held in a variable rather than read in place (bgs_low's Initialize,
    FUN_141dea240, which creates the RPC connection manager): its address, or the TLS block
    when the epoch is the only slot read from it. Any other slot is a reference thread-local
    the port does not have, so that function stays demoted."""
    text = EPOCH_PTR.sub("GenInitThreadEpochPtr()", text)
    for m in list(TLS_BLOCK_LOAD.finditer(text)):
        var = m.group(2)
        rest = text[:m.start()] + text[m.end():]
        uses = re.findall(rf"\b{var}\b(\s*\+\s*0x[0-9a-f]+)?", rest)
        decl = len(re.findall(rf"^\s*[\w ]+\**\s*{var};$", rest, re.M))
        offsets = [u for u in uses]
        if len(offsets) - decl > 0 and all(re.sub(r"\s", "", o) == "+0x2140" for o in offsets[decl:]) \
                and offsets[:decl] == [""] * decl:
            text = text[:m.start()] + f"{m.group(1)}{var} = (longlong)GenInitThreadEpochPtr() - 0x2140;" \
                + text[m.end():]
    return text


# an inlined strlen: `n = -1; do { n = n + 1; } while (*(char *)(p + n) != '\0');`, or with a
# char pointer `while (p[n] != '\0')` (the ostream writes on the login path, FUN_14023d9b0)
STRLEN_LOOP = re.compile(r"(\w+) = -1;\s*do \{\s*\1 = \1 \+ 1;\s*\} while "
                         r"\((?:\*\(char \*\)\((?P<sum>[\w\[\]\*]+) \+ \1\)|(?P<index>\w+)\[\1\]) != '\\0'\);")


# the same loop with the index read through a copy made each pass (FUN_14148b360:
# `lVar4 = -1; do { lVar1 = lVar4 + 1; lVar4 = lVar4 + 1; } while (local_408[lVar1] != '\0');`)
STRLEN_LOOP_COPY = re.compile(r"(\w+) = -1;\s*do \{\s*(\w+) = \1 \+ 1;\s*\1 = \1 \+ 1;\s*\} while "
                              r"\((?:\*\(char \*\)\((?P<sum>[\w\[\]\*]+) \+ \2\)|(?P<index>\w+)\[\2\]) != '\\0'\);")


def strlen_matches(text: str, end: int) -> list[re.Match]:
    """Every inlined strlen loop before `end`, in text order."""
    found = list(STRLEN_LOOP.finditer(text, 0, end)) + list(STRLEN_LOOP_COPY.finditer(text, 0, end))
    return sorted(found, key=lambda m: m.start())


def strlen_loops(text: str, end: int) -> list[tuple[str, str]]:
    """The inlined strlens before `end`, each as (the length's variable, the string)."""
    return [(m.group(1), m.group("sum") or m.group("index")) for m in strlen_matches(text, end)]


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

    text = re.sub(r"\b((?:PTR_)*(?:_?DAT|_?UNK|s|u|ws|PTR)_\w*?_?[0-9a-f]{9})\._(\d+)_(\d+)_", piece, text)

    # the same notation on a local or a parameter (`local_res18[0]._1_3_`, a register's upper
    # bytes): a typed access at its address, or, for an odd width only read, a shift and a mask
    vectors = set(re.findall(r"\b(\w+) \[16\];", text))     # a real 16-byte value stays refused

    def odd_write(m):
        # an odd-width piece written (`local_res10._0_3_ = CONCAT12(...)`, the bytes of a pixel
        # built one channel at a time: the cursor scalers FUN_1414f9d90 / FUN_1414fb180, whose
        # stubs left the 48-pixel cursor empty) has no C lvalue: the whole value, the piece's bits
        # replaced
        indent, var, off, size, rhs = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
        if size in PIECE_TYPE or var.split("[")[0] in vectors or off + size > 8:
            return m.group(0)
        mask = ((1 << (8 * size)) - 1) << (8 * off)
        return f"{indent}{var} = ({var} & ~{mask:#x}ULL) | (((ulonglong)({rhs}) << {8 * off}) & {mask:#x}ULL);"

    text = re.sub(r"^([ \t]*)([A-Za-z_]\w*(?:\[[^\]\[]+\])?)\._(\d+)_(\d+)_ = ([^;\n]+);", odd_write, text, flags=re.M)

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


# a call through a code pointer, as the body spells one by now: `(**(code **)(x))(`,
# `(*(code *)x)(`, `(*(code **)x)(`, `(*(*(code **)x))(`, `(*REFG(code *, x))(`
INDIRECT_CALL = re.compile(r"\((?:\*\*\(code \*\*\)|\*\(code \*\*?\)|\*\(\*\(code \*\*\)|\*REFG\(code \*,)")
# a top-level operator that makes an expression something other than its operands' type
NOT_FLOAT_OP = re.compile(r"<|(?<!-)>|[!=]=|&&|\|\||\?|%|\^|(?<![&])&(?![&])|\|")


def indirect_float_calls(body: str, params: list[str]) -> str:
    """A call through a code pointer that passes or returns a float gets a prototype.

    `code` is unprototyped and returns rax, so a float argument went promoted to double and the
    callee - a port virtual, or a translated one behind its gen_entry wrapper, both of the
    reference's ABI - read the low half of the double from xmm; and a float result was read
    from rax. The tooltip sized itself from line widths read out of rax and set its width and
    height with doubles where floats were read (w 0, h 2.0 or -3.7e19: a screen-tall, collapsed
    tooltip, 2026-10-17). The prototype names a parameter's type up to the last floating
    point argument (`unsigned long long` for the rest of those) and leaves the remainder to `...`,
    which passes them as the unprototyped call did."""
    kinds = {}
    # (with an initializer too: a float parameter narrowed from its promoted double)
    for m in re.finditer(r"^\s*(float|double)\s+(\w+)(?:\[[^\]]*\])?\s*(?:=[^;\n]*)?;", body, re.M):
        kinds[m.group(2)] = m.group(1)
    for p in params:
        w = p.split()
        if len(w) == 2 and w[0] in ("float", "double", "float4", "float8") and re.fullmatch(r"\w+", w[1]):
            kinds[w[1]] = "double" if w[0] in ("double", "float8") else "float"
    floats = [n for n, k in kinds.items() if k == "float"]
    doubles = [n for n, k in kinds.items() if k == "double"]

    def alt(names):
        return r"\b(?:" + "|".join(sorted(names, key=len, reverse=True)) + r")\b" if names else r"(?!)"

    float_rx = re.compile(alt(floats) + r"|\*\s*\(float\s*\*\)|REFG\(float\b|CALLF\(float\b|\(float\)|GEN_AS_FLOAT\(|"
                          r"\b\d+\.\d*(?:e[+-]?\d+)?\b|\b\d+e[+-]?\d+\b")
    double_rx = re.compile(alt(doubles) + r"|\*\s*\(double\s*\*\)|REFG\(double\b|CALLF\(double\b|\(double\)|GEN_AS_DOUBLE\(")

    def top_level(expr: str) -> str:
        """The expression with everything inside parentheses blanked out."""
        out, depth = [], 0
        for ch in expr:
            if ch == "(":
                depth += 1
                out.append(ch if depth == 1 else " ")
            elif ch == ")":
                depth -= 1
                out.append(ch if depth == 0 else " ")
            else:
                out.append(ch if depth == 0 else " ")
        return "".join(out)

    def kind(expr: str) -> str | None:
        e = expr.strip()
        while e.startswith("(") and matching_paren(e, 1) == len(e):
            e = e[1:-1].strip()
        # (Ghidra's byte slices, `SUB84(dVar1,0)`, are bits, not a value of the operand's type)
        if not e or e.startswith(("&", "\"", "SUB", "CONCAT", "ZEXT", "SEXT")):
            return None
        # a cast to a non-floating type decides it
        cast = re.match(r"\(\s*([A-Za-z_][\w ]*?)\s*(\**)\s*\)", e)
        if cast and matching_paren(e, 1) == cast.end() and (cast.group(2) or cast.group(1) not in ("float", "double")):
            return None
        if NOT_FLOAT_OP.search(top_level(e)):
            return None
        if double_rx.search(e):
            return "double"
        if float_rx.search(e):
            return "float"
        return None

    out = body
    for m in reversed(list(INDIRECT_CALL.finditer(body))):
        start = m.start()
        callee_end = matching_paren(out, start + 1)
        j = callee_end
        # (Ghidra breaks a long call between the callee and its arguments: the tooltip's
        # SetWidth went on passing a double, 2026-10-17)
        while j < len(out) and out[j] in " \t\r\n":
            j += 1
        if j >= len(out) or out[j] != "(":
            continue
        args_end = matching_paren(out, j + 1)
        args = split_args(out[j + 1:args_end - 1])
        arg_kinds = [kind(a) for a in args]
        # the result: what the call is assigned to, as the statement opens
        line_start = max(out.rfind(c, 0, start) for c in ";{}\n") + 1
        prefix = out[line_start:start]
        ret = "unsigned long long"
        pm = re.fullmatch(r"\s*(\w+)\s*=\s*", prefix)
        if pm and pm.group(1) in kinds:
            ret = kinds[pm.group(1)]
        else:
            pm = re.fullmatch(r"\s*\*\s*\((float|double)\s*\*\).*=\s*", prefix)
            if pm and "==" not in prefix:
                ret = pm.group(1)
        last = max((k for k, t in enumerate(arg_kinds) if t), default=-1)
        if last < 0 and ret == "unsigned long long":
            continue
        types, new_args = [], []
        for k, a in enumerate(args):
            if k <= last:
                t = arg_kinds[k] or "unsigned long long"
                types.append(t)
                new_args.append(f"({t})({a})")
            else:
                new_args.append(a)
        if last < 0:
            proto = ""
        else:
            proto = ", ".join(types) + (", ..." if len(args) > last + 1 else "")
        callee = out[start:callee_end]
        call = f"(({ret} (*)({proto})){callee})({','.join(new_args)})"
        out = out[:start] + call + out[args_end:]
    return out


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
    """Each `FUN_x(args)` call: (match start, end of the argument list, address, arguments).
    A `thunk_FUN_x(args)` call counts as a call to x: Ghidra drops a thunk's arguments as
    readily as anyone's, and before this the lost-argument check never saw them (55 translated
    functions called their thunks short)."""
    for m in re.finditer(r"\b(?:thunk_)?FUN_([0-9a-f]{9})\(", text):
        depth, i = 1, m.end()
        quote = None
        while i < len(text) and depth:
            ch = text[i]
            if quote:
                # a parenthesis inside a string literal is text: protobuf's VerifyVersion
                # (FUN_1422046d0) appends "... (Version verification failed in \"", and counting
                # its "(" ran the argument list into the next call, which then read as a call
                # one argument short; the stub broke cross_ssl's start on the login path
                if ch == "\\":
                    i += 2
                    continue
                if ch == quote:
                    quote = None
            elif ch in "\"'":
                quote = ch
            else:
                depth += {"(": 1, ")": -1}.get(ch, 0)
            i += 1
        yield m.start(), i - 1, int(m.group(1), 16), split_args(text[m.end():i - 1])


def spelled_alike(machine: str, written: str, integers: set[str]) -> bool:
    """Whether callargs' expression and the decompiler's spelling name the same field read:
    `(uint)((*(uint *)((char *)(param_1) + 0x20)))` and `*(uint *)(param_1 + 0x20)`. callargs
    adds in bytes; Ghidra's `p + n` is in bytes only when p is an integer, so every name in the
    written form must be one the function declares with an integer type (`integers`)."""
    def norm(s: str) -> str:
        s = s.replace("(char *)", "")
        s = re.sub(r"^\(uint\)", "", s.strip())
        return re.sub(r"[()\s]", "", s)
    names = set(re.findall(r"\b(?:param_\d+|[a-z]*Var\d+|local_\w+)\b", written))
    if not names or not names <= integers:
        return False
    return norm(machine) == norm(written)


INTEGER_DECL = re.compile(r"(?:^|[(,])\s*(?:longlong|ulonglong|undefined8|int|uint|undefined4|long|ulong) (\w+)(?=[,);])",
                          re.M)


def code_arithmetic(text: str) -> str:
    """Byte arithmetic on a `code *` local. Ghidra types a block whose first slot it saw called
    as `code *` and goes on addressing it by bytes (`*(undefined4 *)(pcVar4 + 0x10)`, a heap
    object whose vtable pointer is the first word); in C `code` is a function type, and
    arithmetic on its pointer does not compile (C2296, 11 functions, 4 on the login path)."""
    for name in re.findall(r"^\s*code \*(\w+);\s*$", text, re.M):
        text = re.sub(rf"(?<![\w*.>&])\b{name}(\s*[+-]\s*(?![=+-]))", rf"(char *){name}\1", text)
    return text


def zero_of(ret: str) -> str:
    """A stub's or a fault handler's result: zero of the return type (a 16-byte vector result
    is a union C cannot cast an int to, C2440 once ApplyFacts placed them in xmm0)."""
    return "gen_v16_qq(0, 0)" if ret.strip() == "gen_v16" else f"({ret})0"


def rdtsc_upper(text: str) -> str:
    """`uVar8 = CONCAT44((int)(uVar8 >> 0x20),(int)uVar14) | uVar14 & 0xffffffff00000000;` after
    `uVar14 = rdtsc();`: Ghidra's RDTSC writes EAX and EDX without the zero extension a 32-bit
    write has in 64-bit mode (ia.sinc: `EDX = tmp(4); EAX = tmp(0);`), so `or rax, rdx` after
    `shl rdx, 0x20` keeps the old upper half of rax, which the processor clears. It is the
    counter itself; the old rax read in was `extraout_RAX` in FUN_140222a60 (2026-10-16)."""
    for v in set(re.findall(r"\b(\w+) = (?:__)?rdtsc\(\);", text)):
        text = re.sub(rf"CONCAT44\(\(int\)\((?:\(\w+\))?\w+ >> 0x20\),\(int\){v}\) \| {v} & 0xffffffff00000000", v, text)
    return text


def dead_lost_stores(text: str) -> str:
    """`uVar8 = extraout_RAX;` whose variable is assigned again before anything reads it: a store
    of a register the decompiler lost that nothing uses, gone with its register. Followed in the
    text, which is the flow outside loops: a loop around it, a label or a goto, and it stays."""
    head, _, body = text.partition("{")
    # what each closing brace closes: the statement keyword before its opening one
    closes: dict[int, str] = {}
    stack = []
    for m in re.finditer(r"[{}]", body):
        if m.group(0) == "{":
            kw = re.search(r"\b(while|for|do|if|else|switch)\b[^;{}]*$", body[max(0, m.start() - 300):m.start()])
            stack.append(kw.group(1) if kw else "")
        elif stack:
            closes[m.start()] = stack.pop()
    gone = []
    for m in list(re.finditer(r"^[ \t]*(\w+) = ((?:extraout|in|unaff)_\w+);\n", body, re.M)):
        var = m.group(1)
        k = m.end()
        dead = False
        while True:
            n = re.search(rf"\b{var}\b|[{{}}]|\bgoto\b|^\s*\w+:", body[k:], re.M)
            if n is None:
                break
            at = k + n.start()
            tok = n.group(0)
            if tok == "}":
                if closes.get(at) in ("while", "for", "do"):
                    break
                k = at + 1
                continue
            if tok == "{":
                k = at + 1
                continue
            if tok != var:
                break
            # the variable again: dead only if this is a whole new value of it
            stmt = re.match(rf"{var}\s*=(?!=)([^;]*);", body[at:])
            dead = bool(stmt) and not re.search(rf"\b{var}\b", stmt.group(1)) and \
                body[max(0, at - 40):at].rstrip().endswith((";", "{", "}"))
            break
        if dead:
            gone.append((m.start(), m.end()))
    for s, e in reversed(gone):
        body = body[:s] + body[e:]
    return head + "{" + body


CALL_STMT = r"(?:(?:thunk_)?FUN_[0-9a-f]{9}|[A-Za-z_]\w*(?:::~?\w+)+)\([^;]*\)"


def call_results(text: str) -> str:
    """`FUN_x(...);` then `return extraout_RAX;` or `uVar = extraout_EAX;` on the next line: rax
    read right after a call is what the call left there, which a translated function returns
    (every one hands back a register's worth). Ghidra prints it so where the callee is typed void
    (FUN_14136e0f0 returns what its tail call to a free routine leaves, a path the reference's
    callers never look at; 14 functions stubbed, 2026-10-16). Integers only: a void callee's
    float is nothing C can carry."""
    pat = re.compile(rf"^([ \t]*)({CALL_STMT});[ \t]*\n[ \t]*(return |\w+ = )(\([\w ]+\**\))?(extraout_(?:RAX|EAX|AX|AL)(?:_\d+)?);",
                     re.M)

    def sub(m):
        typ = re.search(rf"^[ \t]*([\w ]+?\**)[ \t]*{m.group(5)};$", text, re.M)
        cast = m.group(4) or (f"({typ.group(1).strip()})" if typ else "")
        return f"{m.group(1)}{m.group(3)}{cast}{m.group(2)};"
    text = pat.sub(sub, text)
    # a return of what rax holds after some earlier void call, on a path that sets no result
    # (curl's global cleanup, FUN_1421813d0: `return extraout_EAX;` past a void call and a test):
    # no value the function computes, and none its callers can rely on; zero
    text = re.sub(r"\breturn (?:\([\w ]+\**\))?extraout_(?:RAX|EAX|AX|AL)(?:_\d+)?;", "return 0;", text)
    # and a variable set to it after statements of its own (`uVar3 = extraout_RAX_00;` two lines
    # past the void call that left it, in a loop that sets uVar3 again, FUN_14198a390): the same
    text = re.sub(r"^([ \t]*\w+ = )(?:\([\w ]+\**\))?extraout_(?:RAX|EAX|AX|AL)(?:_\d+)?;", r"\g<1>0;", text, flags=re.M)
    # and a float function's path that ends in a call that returns none (`FUN_142556140(...);
    # return extraout_XMM0_Da;`): xmm0 there is what the callee left, no value; zero
    text = re.sub(rf"^([ \t]*)({CALL_STMT});[ \t]*\n([ \t]*)return extraout_XMM0_(?:Da|Qa)(?:_\d+)?;",
                  r"\1\2;\n\3return 0;", text, flags=re.M)
    return text


def stack_values(text: str) -> str:
    """The frame used where the decompiler named no variable, as a value.

    `stack0xffffffffffffffd0` read and written (`*stack0xffffffffffffffd0 = 0;`, a pointer kept in
    a slot it did not split out, FUN_1420fe040), or `stack0x00000024`, the upper half of a home
    slot (`CONCAT44(stack0x00000024,param_1)`, FUN_141a4d260): the slot itself, through the frame
    address stack_frame() gives `&stack0x...` (a pointer type where it is dereferenced).

    `register0x00000020` read before any write: the stack pointer at the entry (`(longlong)
    register0x00000020 + -0x48`, a local array walked down by a pointer the decompiler merged
    with RSP, FUN_141686760): a pointer that starts at that frame address."""
    head, _, body = text.partition("{")
    for n in set(re.findall(r"(?<![&\w])(stack0x[0-9a-f]+)\b", body)):
        if re.search(rf"^[ \t]*[\w ]+\**[ \t]*{n};$", body, re.M):
            continue
        ptr = bool(re.search(rf"(?:^|[^\w)\]\s])\s*\*\s*{n}\b|\b{n}\s*\[|\b{n}\s*[!=]=\s*\(\w+ \*\)", body, re.M))
        typ = "undefined8 *" if ptr else "undefined8"
        body = re.sub(rf"(?<![&\w]){n}\b", f"(*({typ} *)&{n})", body)
    uses = list(re.finditer(r"\bregister0x00000020\b", body))
    decl = re.compile(r"^[ \t]*[\w ]+\**[ \t]*register0x00000020;\n", re.M)
    real = [u for u in uses if not decl.match(body, body.rfind("\n", 0, u.start()) + 1)]
    if real and not re.match(r"\s*=(?!=)", body[real[0].end():]) and "va_start" not in body:
        body = decl.sub("", body)
        body = re.sub(r"\(BADSPACEBASE \*\)", "(undefined1 *)", body)
        blank = re.search(r"\n[ \t]*\n", body)
        at = blank.end() if blank else body.index("\n") + 1
        # (the entry stack pointer is where the return address is: stack_frame() maps no frame
        # for `&stack0x00000000` when nothing else in the frame is named, FUN_1416e1da0, C2065)
        body = body[:at] + "  undefined1 *gen_rsp = (undefined1 *)_AddressOfReturnAddress();\n" + \
            re.sub(r"\bregister0x00000020\b", "gen_rsp", body[at:])
    return head + "{" + body


def int_variants(text: str) -> str:
    """A `double *` the code also uses as integer slots: 8-byte storage the decompiler typed by
    one of the values kept in it (a Lua value variant: a number, or a string's pointer and length,
    FUN_141cfb560). `(longlong)*param_1`, `*param_1 = (double)pdVar1` and a double local loaded
    from it then handed to memmove are integers in those slots; as doubles, C converted them
    (C2172). The pointer is read as `longlong *`; the doubles taken out of it are `longlong`;
    Ghidra's `(double)` casts on them are copies of the bits, as is a real double stored there
    (GEN_D2Q); and `-NAN` / `0.0` are -1 and 0 (2026-10-17)."""
    head, _, body = text.partition("{")
    ptrs = [p for p in re.findall(r"\bdouble \*(\w+)", head + body.split("\n\n", 1)[0])
            if (re.search(rf"\bdouble \*{p}\s*[,)]", head) or re.search(rf"^[ \t]*double \*{p};$", body, re.M))
            and re.search(rf"\((?:u)?longlong\)\s*(?:\*\s*{p}\b|{p}\s*\[)|\*\s*{p}\s*=\s*\(double\)\s*\w*[pP]\w*Var", body)]
    if not ptrs:
        return text
    loaded = set()
    for p in ptrs:
        loaded |= set(re.findall(rf"\b(dVar\d+) = (?:\*\s*{p}\b|{p}\s*\[)", body))
        loaded |= set(re.findall(rf"(?:\*\s*{p}|{p}\s*\[[^\]]*\])\s*=\s*(dVar\d+);", body))
    loaded = {v for v in loaded if re.search(rf"\((?:u)?longlong\)\s*{v}\b|\[\s*\(longlong\)\s*{v}\s*\]|memmove\([^;]*\b{v}\b|FUN_1425bf3b0\([^;]*\b{v}\b", body)}
    for v in loaded:
        body = re.sub(rf"^([ \t]*)double {v};$", rf"\1longlong {v};", body, flags=re.M)
        body = re.sub(rf"\b{v} = -NAN;", f"{v} = -1;", body)
        body = re.sub(rf"\b{v} = \(double\)", f"{v} = (longlong)", body)
        body = re.sub(rf"\b{v} != 0\.0\b", f"{v} != 0", body)
        body = re.sub(rf"\b{v} == 0\.0\b", f"{v} == 0", body)
        # (a real double put into it, a function's double result: its bits)
        body = re.sub(rf"^([ \t]*){v} = (FUN_\w+\([^;]*\));", rf"\1{v} = GEN_D2Q(\2);", body, flags=re.M)
    for p in ptrs:
        local = re.search(rf"^([ \t]*)double \*{p};$", body, re.M)
        if local:
            # a local is retyped where it is declared (an alias put before the declarations read
            # it undeclared, and renamed the declaration itself: FUN_141a45040's `local_res20`)
            body = body[:local.start()] + f"{local.group(1)}longlong *{p};" + body[local.end():]
            rest_at, name = local.start(), p
            tail = body[rest_at:]
        else:
            lines = body.split("\n")
            start = next((k for k, ln in enumerate(lines) if ln.strip() == ""), 0)
            body = "\n".join(lines[:start] + [f"  longlong *gen_{p} = (longlong *){p};"] + lines[start:])
            rest_at = body.index(f"  longlong *gen_{p} = (longlong *){p};") + len(f"  longlong *gen_{p} = (longlong *){p};")
            name = f"gen_{p}"
            tail = body[rest_at:]
            # (every use, but the pointer itself set to something else: `param_1 = ...;`)
            tail = re.sub(rf"(?<![\w*])(?<!gen_){p}\b(?!\s*=[^=])|(?<=\*){p}\b|(?<=\* ){p}\b",
                          f"gen_{p}", tail)
        tail = re.sub(rf"\(double\)\s*(?=\w*[pP]\w*Var)", "(longlong)", tail)

        # the double literals stored in or compared with its slots: the bits they are (the
        # variant's type tags 1, 2, 3 as denormals, `4.94065645841247e-324`; -1 as `-NAN`)
        def bits(lit: str) -> str:
            if lit.lstrip("-") == "NAN":
                return "-1"
            q = struct.unpack("<q", struct.pack("<d", float(lit)))[0]
            return f"{q}LL" if -(1 << 31) <= q < (1 << 31) else f"(longlong){q:#x}ULL"
        tail = re.sub(rf"((?:\*\s*{name}\b|{name}\s*\[[^\]]*\])\s*(?:=|==|!=)\s*)(-?(?:\d+\.\d+(?:e[-+]?\d+)?|NAN))\b",
                      lambda m: m.group(1) + bits(m.group(2)), tail)
        body = body[:rest_at] + tail
    return head + "{" + body


def duplicate_labels(text: str) -> str:
    """A label the decompiler defines twice (`LAB_14235c411:` in zlib's inflate, FUN_14235abd0):
    the shared epilogue at that address, copied once per value its predecessors leave in eax
    (`return 0xfffffffc;` after one, `return 0xfffffffe;` after the other). Each goto goes to
    the next copy after it, which is where the decompiler put each copy: the copies get names of
    their own (C2045, 2026-10-17)."""
    for lab in {m.group(1) for m in re.finditer(r"^((?:LAB|code_r|switchD|joined_r)\w*):\s*$", text, re.M)}:
        defs = [m.start() for m in re.finditer(rf"^{lab}:\s*$", text, re.M)]
        if len(defs) < 2:
            continue
        names = [lab] + [f"{lab}_{k}" for k in range(2, len(defs) + 1)]

        def target(pos: int) -> str:
            if lab.startswith("joined_r"):
                # (a loop's join point copied, FUN_1419c3450: each copy is the loop its gotos
                # are in, back edges included, so a goto goes to the copy nearest it)
                return min(zip(defs, names), key=lambda dn: abs(dn[0] - pos))[1]
            for d, n in zip(defs, names):
                if d > pos:
                    return n
            return names[-1]
        out, last = [], 0
        events = sorted([(d, "def", k) for k, d in enumerate(defs)] +
                        [(m.start(), "goto", m.end()) for m in re.finditer(rf"\bgoto {lab};", text)])
        for pos, kind, x in events:
            out.append(text[last:pos])
            if kind == "def":
                out.append(f"{names[x]}:")
                last = pos + len(lab) + 1
            else:
                out.append(f"goto {target(pos)};")
                last = x
        out.append(text[last:])
        text = "".join(out)
    return text


def deref_locals(text: str) -> str:
    """A local Ghidra declares an 8-byte scalar and yet dereferences or indexes (`undefined8
    local_43b0;` ... `local_43b0 = (uint *)0x0;` ... `local_43b0[0x28]`): the pointer its
    assignments agree on, or a pointer to 8 bytes when none says (C2100 / C2109 in four
    functions, 2026-10-17). stack_frame() does the same for a frame's locals."""
    # (the declarations end at the first blank line in the body: one before the signature, after
    # the decompiler's WARNING comments, hid all of them in FUN_141903630)
    end = re.compile(r"\n[ \t]*\n").search(text, max(text.find("{"), 0))
    if not end:
        return text
    decls = list(re.finditer(r"^([ \t]*)(undefined8|longlong|ulonglong) (\w+);$", text[:end.end()], re.M))
    for m in reversed(decls):
        n = m.group(3)
        rest = text[end.end():]
        # (Ghidra writes a dereference `*x`, unspaced; `* x` opening a wrapped line is a product,
        # FUN_14030b490's `* lVar10 >> 0x18`)
        if not re.search(rf"(?:^|[^\w)\]\s]|\*\))\s*\*{n}\b|\b{n}\s*\[", rest, re.M):
            continue
        casts = set(re.findall(rf"\b{n} = \(((?:\w+ )*\w+ \*+)\)", rest))
        typ = casts.pop() if len(casts) == 1 else "undefined8 *"
        text = text[:m.start()] + f"{m.group(1)}{typ}{n};" + text[m.end():]
    return text


def decompiler_temps(text: str) -> str:
    """Names the decompiler prints for storage it found no variable for.

    `unique0x10001edc = puVar5[9];`: a temporary of its own, never declared. Only ever assigned
    (FUN_1413544d0's eight are copies to stack it could not place, read by nothing) the stores
    go; read too, it is a local.

    `DAT_143217d68_1`: one byte into the global DAT_143217d68, the address plus the offset
    (C2065 in Lua::CancelRedeem, 2026-10-16)."""
    text = re.sub(r"\b(DAT|UNK)_(1[0-9a-f]{8})_(\d+)\b", lambda m: f"{m.group(1)}_{int(m.group(2), 16) + int(m.group(3)):x}", text)
    # `stack0xfffffffffffffe6c = ZEXT412(0);`: a store into the frame where no local is named,
    # as wide as its zero extension (12 bytes over a 16-byte struct's fields, FUN_141544600):
    # zeroed, then the value's own bytes, at the frame address stack_frame() gives `&stack0x`
    def zext_store(m):
        place, a_, b_, v = m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
        return (f"{m.group(1)}memset(&{place}, 0, {b_}); "
                f"{{ undefined{a_} gen_z = (undefined{a_})({v}); memcpy(&{place}, &gen_z, {a_}); }}")
    text = re.sub(r"^([ \t]*)(stack0x[0-9a-f]+) = ZEXT([1248])(\d+)\(([^;]*)\);", zext_store, text, flags=re.M)
    # `undefined1 in_register_00001204 [12];`: the lanes of an xmm argument above the float it
    # carries (register space 0x1200 on is xmm0..), which the ABI leaves undefined and a
    # `movaps` carries along whole (FUN_14240ad00 into the sincos): zero
    for reg in re.findall(r"^[ \t]*undefined1 in_register_(00001[2-9a-f][0-9a-f]{2}) \[\d+\];$", text, re.M):
        text = re.sub(rf"^([ \t]*)undefined1 in_register_{reg} \[(\d+)\];$",
                      rf"\1undefined1 gen_upper_{reg} [\2] = {{0}};", text, flags=re.M)
        text = re.sub(rf"\bin_register_{reg}\b", f"gen_upper_{reg}", text)
    # `MXCSR = x;` / `return MXCSR;`: the SSE control register, ldmxcsr / stmxcsr (the CRT's
    # _ctrlfp helpers, FUN_1402947b0 & co)
    text = re.sub(r"^([ \t]*)MXCSR = ([^;]*);", r"\1_mm_setcsr((unsigned int)(\2));", text, flags=re.M)
    text = re.sub(r"\bMXCSR\b", "_mm_getcsr()", text)
    # and their VEX forms, which Ghidra leaves as pseudo-ops (FUN_1402731f0: `vstmxcsr_avx()`)
    text = re.sub(r"\bvstmxcsr_avx\(\s*\)", "_mm_getcsr()", text)
    text = re.sub(r"\bvldmxcsr_avx\(", "_mm_setcsr((unsigned int)", text)
    # `uVar8 = _Init_thread_header(&guard);`: the port's returns nothing, and what the reference's
    # left in eax is no value its callers use
    text = re.sub(r"^([ \t]*)(\w+) = ((?:_Init_thread_header|_Init_thread_footer|FUN_14026bca8)\([^;]*\));", r"\1\3; \2 = 0;", text, flags=re.M)
    # a name with punctuation in it, `operator()<>_140285170` (the CRT's __crt_seh_guarded_call,
    # FID-named; ApplyFacts appended the address): the function at that address
    text = re.sub(r"(?<![\w:])(?:[\w:~]*::)?operator[^\s(]*\(\)[<>,\w]*_(1[0-9a-f]{8})\b", r"FUN_\1", text)
    # `(undefined1 [ 16 ])0x0`: Ghidra's spacing for a 16-byte cast, as tools/v16.py reads it
    text = re.sub(r"\(undefined1?\s*\[\s*16\s*\]\)", "(undefined1 [16])", text)
    # the callee-saved xmm registers read as they came in (`unaff_XMM6_Da` stored to the
    # frame, `unaff_XMM8_Ba` on a path that zeroes xmm8 on the other): the caller's values, which
    # nothing here means; zero (FUN_141687ed0, FUN_14030acc0)
    for n in set(re.findall(r"\bunaff_XMM\d+_[A-Za-z]+(?:_\d+)?\b", text)):
        text = re.sub(rf"^[ \t]*[\w ]+\**[ \t]*{n};\n", "", text, flags=re.M)
        # (a write to one, the restore before a ret, goes: there is nothing to restore)
        text = re.sub(rf"^[ \t]*{n}\s*=(?!=)[^;]*;\n", "", text, flags=re.M)
        text = re.sub(rf"\b{n}\b", "0", text)
    # the lanes of a float or double argument above it (`in_XMM0_Db` in the sincos, which puts
    # its argument into a vector whole): undefined in the ABI, zero
    # (and of an xmm register that carries no argument at all, `in_XMM2_Db` in a one-argument
    # function: what the caller left there)
    head = text.split("{", 1)[0]
    kinds = [k.strip() for k in re.findall(r"[(,]\s*(\w[\w ]*?)\s*\**\s*\w+(?=\s*[,)])", head)]
    for pos in range(4):
        if pos >= len(kinds) or kinds[pos] in ("float", "double"):
            for n in set(re.findall(rf"\bin_XMM{pos}_(?:D[b-d]|Qb)(?:_\d+)?\b", text)):
                if re.search(rf"^[ \t]*{n}\s*=(?!=)", text, re.M):
                    # (written too, `in_XMM2_Db = 0;` in FUN_141db2600, whose reads after it see
                    # what was written: a local, starting at the zero the caller's lanes are)
                    text = re.sub(rf"^([ \t]*[\w ]+\**[ \t]*{n});$", r"\1 = 0;", text, count=1, flags=re.M)
                    text = re.sub(rf"\b{n}\b", f"gen_{n[3:]}", text)
                    continue
                text = re.sub(rf"^[ \t]*[\w ]+\**[ \t]*{n};\n", "", text, flags=re.M)
                text = re.sub(rf"\b{n}\b", "0", text)
    text = int_variants(duplicate_labels(deref_locals(stack_values(text))))
    # a data label Ghidra named by its type (`UINT_1419c4b3c`, a byte table a switch reads): the
    # global at that address, as any DAT_
    text = re.sub(r"\b(?:U?INT|U?SHORT|U?CHAR|BYTE|U?LONG|U?LONGLONG|WORD|DWORD|QWORD|FLOAT|DOUBLE)_(1[0-9a-f]{8})\b",
                  r"DAT_\1", text)
    # a store through `code *` (`*(code *)(p + i) = (undefined1)0;`, code patched in place): a
    # byte, which C can assign; a function type it cannot (C2106)
    text = re.sub(r"^([ \t]*)\*\(code \*\)", r"\1*(undefined1 *)", text, flags=re.M)
    # `(double)&x`: an address kept in a slot the decompiler typed a double: its bits
    out, i = [], 0
    for m in re.finditer(r"\(double\)\s*&", text):
        if m.start() < i:
            continue
        end = _operand_end(text, m.end() - 1)
        out.append(text[i:m.start()] + f"gen_q2d((long long){text[m.end() - 1:end]})")
        i = end
    out.append(text[i:])
    text = "".join(out)
    # `(char *)(double)(float)param_1[8]`: a pointer read out of memory the decompiler typed a
    # float array (an 8-byte load, `mov rax, [rcx + 0x20]`): the 8 bytes at that element as the
    # pointer, not a conversion of a float (C2440 in FUN_14202e640, 2026-10-17)
    text = re.sub(r"\(((?:\w+ )*\w+ \*+)\)\(double\)\(float\)(\*?\w+(?:\[[^\[\]]+\])?)",
                  r"(\1)*(undefined8 *)&\2", text)
    # a jump table's base in code (`&LAB_1415a48c4` + index * 4, the table the switch reads):
    # its address as bytes, which C can index; as a code pointer it was C2296
    text = re.sub(r"&\s*LAB_(1[0-9a-f]{8})\b(?=\s*\+)", r"((char *)REFP(0x\1))", text)
    names = set(re.findall(r"\bunique0x[0-9a-f]+\b", text))
    for n in names:
        sets = re.findall(rf"^[ \t]*{n}\s*=(?!=)[^;]*;\n", text, re.M)
        if len(re.findall(rf"\b{n}\b", text)) == len(sets):
            text = re.sub(rf"^[ \t]*{n}\s*=(?!=)[^;]*;\n", "", text, flags=re.M)
        else:
            start = text.index("{\n") + 2
            text = text[:start] + f"  undefined8 {n};\n" + text[start:]
    return text


WIDE_VAR_DECL = re.compile(r"^([ \t]*)undefined1? (\w+) \[(32|64)\];$", re.M)
# a 32- or 64-byte pseudo-op (`vpmulld_avx2(...)`; an `_avx512vl` one works on 16 bytes, v16's)
WIDE_OP = re.compile(r"\b\w+_avx(?:2|512(?!vl\s*\()\w*)\s*\(")


def wide_vectors(text: str, a: int) -> str:
    """32- and 64-byte values: the AVX2 and AVX-512 paths. Seven of the main loop's subsystem
    ticks (FUN_140222a60 and its siblings under FUN_1402225d0) carry an integrity hash with an
    SSE2 path and two wider ones, chosen by the CPU-feature bits at 0x142ddf858; those bits stay
    clear in the translation's image of the reference's data, so only the SSE2 path runs. The
    wide variables become plain storage (gen_v32 / gen_v64, Ghidra.h): pieces of up to 16 bytes
    are what simd_pieces() and tools/v16.py already make of any variable's, a copy of a wider
    piece or of the whole is a memcpy, and anything else on a wide value (an AVX2 pseudo-op, an
    operator on 32 bytes) is a wide path, `GenWidePath` if it is ever reached. Their registers'
    upper lanes, `in_ZMM1`, are what an SSE2 path never set: zero. The whole function was
    stubbed for its wide paths before (2026-10-16)."""
    sizes = {m.group(2): int(m.group(3)) for m in WIDE_VAR_DECL.finditer(text)}
    # (only the ones used as values: pieces, whole assignments, a register's. A 32-byte buffer
    # handed to callees by its address, `FUN_142484130(..., local_28, ...)`, is a C array as it
    # is; taken for a vector, FUN_14246c7b0's calls became wide paths, 2026-10-17)
    sizes = {n: s for n, s in sizes.items()
             if re.match(r"(?:in|unaff)_[XYZ]MM", n)
             or re.search(rf"\b{n}\._\d+_\d+_|\b{n}\s*=(?!=)|=\s*{n}\s*;|[\^|&]\s*{n}\b|\b{n}\s*[\^|&]", text)}
    if not sizes and not WIDE_OP.search(text):
        return text
    head, _, body = text.partition("{")
    # (a register's: a local, all zero)
    for n in [n for n in sizes if re.match(r"(?:in|unaff)_[XYZ]MM\d+$", n)]:
        new = "gen_" + n
        body = re.sub(rf"\b{n}\b", new, body)
        sizes[new] = sizes.pop(n)
    body = WIDE_VAR_DECL.sub(lambda m: f"{m.group(1)}gen_v{m.group(3)} {m.group(2)} = {{0}};" if m.group(2) in sizes else m.group(0), body)
    names = "|".join(map(re.escape, sizes))
    wide_name = re.compile(rf"\b(?:{names})\b") if names else None
    piece = re.compile(rf"\b({names})\._(\d+)_(\d+)_") if names else None
    trap = f"GenWidePath(0x{a:x}ULL)"
    out = []
    for s, e in v16.spans(body):
        expr = body[s:e]
        if not ((wide_name and wide_name.search(expr)) or WIDE_OP.search(expr)):
            continue
        x = expr.strip()
        if re.match(r"gen_v(?:32|64) \w+ = \{0\}$", x):
            continue
        # a wide piece copied whole: `X._16_48_ = Y._16_48_`
        m = re.fullmatch(rf"({names})\._(\d+)_(\d+)_\s*=\s*({names})\._(\d+)_(\d+)_", x) if names else None
        if m and m.group(3) == m.group(6):
            out.append((s, e, f"memcpy((char *)&{m.group(1)} + {m.group(2)}, (char *)&{m.group(4)} + {m.group(5)}, {m.group(3)})"))
            continue
        # the whole of it loaded or stored through a pointer, or copied: `X = *P;`, `*P = X;`, `X = Y;`
        m = re.fullmatch(rf"({names})\s*=\s*\*\s*(?:\([^()]*\))?\s*(\w+)", x) if names else None
        if m:
            out.append((s, e, f"memcpy(&{m.group(1)}, {m.group(2)}, {sizes[m.group(1)]})"))
            continue
        m = re.fullmatch(rf"\*\s*(?:\([^()]*\))?\s*(\w+)\s*=\s*({names})", x) if names else None
        if m:
            out.append((s, e, f"memcpy({m.group(1)}, &{m.group(2)}, {sizes[m.group(2)]})"))
            continue
        m = re.fullmatch(rf"({names})\s*=\s*({names})", x) if names else None
        if m:
            out.append((s, e, f"memcpy(&{m.group(1)}, &{m.group(2)}, {min(sizes[m.group(1)], sizes[m.group(2)])})"))
            continue
        # pieces of up to 16 bytes only, and no wide pseudo-op: v16's
        rest = piece.sub(lambda pm: "P" if int(pm.group(3)) <= 16 else pm.group(0), expr) if piece else expr
        if not (wide_name and wide_name.search(rest)) and not WIDE_OP.search(rest):
            continue
        # (a condition keeps its parentheses: the trap, then false)
        stmt = body[e:e + 1] == ";" or re.match(r"\s*;", body[e:])
        out.append((s, e, trap if stmt else f"({trap}, 0)"))
    for s, e, new in reversed(out):
        body = body[:s] + body[s:e][:len(body[s:e]) - len(body[s:e].lstrip())] + new + body[e:]
    return head + "{" + body


def va_args(text: str) -> str:
    """A variadic function's body (`fn(L, fmt, ...)`, marked by tools/protos.py) with C's
    own va_list in place of the home-slot spill. The prologue stores the register arguments
    past the fixed ones to their home slots, `local_res18 = in_R8; local_res20 = in_R9;`, so
    `&local_res18` is a pointer that walks on into the caller's stack arguments: the same
    pointer MSVC's va_start gives on x64. Translated with the slots as locals, it walked into
    the translation's own frame past the second variadic argument. Left alone when a slot or
    one of those registers is used any other way."""
    head, _, body = text.partition("{")
    m = re.search(r"\(([^()]*),\s*\.\.\.\)\s*$", head.strip())
    if not m:
        return text
    fixed = m.group(1).count(",") + 1
    last = re.search(r"(\w+)\s*$", m.group(1)).group(1)
    first = 8 * (fixed + 1)
    if first > 0x80:
        return text
    blank = re.search(r"\n[ \t]*\n", body)
    if blank is None:
        return text
    # Every name for the memory from the first variadic slot on is that memory, through gen_va
    # (which va_start points at the same place: the translation is variadic too, so MSVC spills
    # its register arguments to their home slots as the reference does). Their uses are not
    # only the spill and `&slot`: the variadic register read where it came in (`*in_RDX = 0`,
    # FUN_14025cd90's first variadic argument a pointer), a slot reused as a local
    # (`local_res20 = CONCAT35(...)`), the entry stack pointer walked along the arguments
    # (`register0x00000020`, FUN_1420d10d0 calling each pointer until a null), a stack slot past
    # four fixed parameters (`&stack0x00000038`, FUN_14253a9a0). Each named slot becomes an
    # lvalue in the va area, and the spill `slot = register` a no-op (2026-10-16)
    regs = {0x10: ("RDX", "EDX", "DX", "DL"), 0x18: ("R8", "R8D", "R8W", "R8B"), 0x20: ("R9", "R9D", "R9W", "R9B")}
    decls = {}
    for dm in re.finditer(r"^[ \t]*([\w ]+?)\s*(\**)\s*(\w+)(\s*\[\w*\])?;\n", body[:blank.end()], re.M):
        decls[dm.group(3)] = (dm.group(1).strip() + (" " + dm.group(2) if dm.group(2) else ""), bool(dm.group(4)), dm.group(0))
    names: dict[str, int] = {}
    for off in range(first, 0x28, 8):
        names[f"local_res{off:x}"] = off
        for r in regs[off]:
            names[f"in_{r}"] = off
    for dm in re.finditer(r"\b((?:in_stack_|local_res|uStackX_)([0-9a-f]+))\b", body):
        off = int(dm.group(2), 16)
        if off >= first and off >= 0x28:
            names[dm.group(1)] = off
    used = {n: o for n, o in names.items() if re.search(rf"\b{n}\b", body)}
    # the spills, wherever they are: `local_res18 = in_R8;` is the slot holding what it holds
    for slot, off in list(used.items()):
        if slot.startswith("local_res"):
            for r in regs.get(off, ()):
                body = re.sub(rf"^[ \t]*{slot} = (?:\(\w+(?: \*+)?\))?in_{r};\n", "", body, flags=re.M)
    lines = []
    for n, off in sorted(used.items(), key=lambda x: x[1]):
        d = decls.get(n)
        if d is None:
            continue
        typ, is_array, line = d
        body = body.replace(line, "", 1)
        if not re.search(rf"\b{n}\b", body):
            continue
        ptr = f"gen_va_{n}"
        # (an array slot, `undefined8 local_res18 [2]`, is its first element's address)
        lines.append(f"  {typ} *{ptr} = ({typ} *)((char *)gen_va + {off - first:#x});\n")
        body = re.sub(rf"\b{n}\b", ptr if is_array else f"{ptr}[0]", body)
    # (Ghidra's name for a whole slot it also splits into pieces, `_local_res20`: its 8 bytes)
    for off in range(first, 0x28, 8):
        body = re.sub(rf"(?<![\w.]){'_local_res'}{off:x}\b", f"(*(undefined8 *)((char *)gen_va + {off - first:#x}))", body)
    # stack addresses: `&stack0x...` at or past the first variadic slot, and the entry stack
    # pointer itself (`register0x00000020`, read, never written)
    def stack_addr(sm):
        off = int(sm.group(1), 16)
        return f"((undefined1 *)gen_va + {off - first:#x})" if off >= first else sm.group(0)
    body = re.sub(r"&stack0x0*([0-9a-f]{1,8})\b", stack_addr, body)
    if "register0x00000020" in body:
        # (a pointer walking the arguments from there, which the decompiler merged into RSP: a
        # local that starts at the entry stack pointer, FUN_1420d1040's second loop)
        body = re.sub(r"^[ \t]*[\w ]+\**\s*register0x00000020;\n", "", body, flags=re.M)
        body = re.sub(r"\(BADSPACEBASE \*\)", "(undefined1 *)", body)
        lines.append(f"  undefined1 *gen_rsp = (undefined1 *)gen_va - {first:#x};\n")
        body = re.sub(r"\bregister0x00000020\b", "gen_rsp", body)
    # (after the declarations, which end at the first line that is none: a function with no
    # locals has no blank line after them, and FUN_14253a9a0's va_start landed past its `}`)
    at = 0
    for lm in re.finditer(r"[^\n]*\n", body):
        ln = lm.group(0).strip()
        # (a declaration: a type and a name; `puVar2 = x;` is a statement, as is `return 0;`)
        if ln and (not re.match(r"^[A-Za-z_][\w ]*?[\w*]\s+\**\s*[A-Za-z_]\w*(?:\s*\[\w*\])?(?:\s*=\s*[^;]*)?;$", ln)
                   or re.match(r"^(?:return|goto|break|continue|case|else|do)\b", ln)) and not ln.startswith("//"):
            break
        at = lm.end()
    body = (body[:at] + f"  va_start(gen_va, {last});\n" + "".join(lines) + body[at:])
    return head + "{\n  va_list gen_va;" + body


def rax_upper_parts(text: str) -> str:
    """RAX bytes past what the code means, read only to be carried along.

    `extraout_var` is the upper part of a call's RAX beyond the result its committed prototype
    returns: `CONCAT44(extraout_var,uVar1)` after a 32-bit result, `CONCAT71(extraout_var,cVar2)`
    after a byte. A 32-bit write zero-extends, and a byte result's callers read only the byte,
    so it is 0 (150 functions were stubbed for it once prototypes were committed, 2026-10-15).

    `in_RAX` read at entry is the bool idiom: `return in_RAX & 0xffffffffffffff00;` is false
    and `CONCAT71((int7)(in_RAX >> 8),1)` true, with whatever RAX held above the byte. Those
    reads, and any read after an assignment (`in_RAX = FUN_x(p); if ((char)in_RAX == '\\0')`),
    make it a local that starts at 0. A plain read before any assignment would be a value
    passed in RAX, and stays refused."""
    decl_rx = r"^[ \t]*[\w ]+\**[ \t]*{}(?:[ \t]*\[\w*\])?;\n"
    # `register0x00000020 = (BADSPACEBASE *)FUN_x(...)`: a call's result Ghidra put in RSP's
    # slot of the register space once prototypes were committed. The instructions use rax
    # (cross_ssl's start, FUN_141f2ece0: `call [rax+8]` at 0x141f2ee98, then `movzx ebx, al`),
    # so when every assignment is a call result it is in_RAX, under the checks below.
    if "register0x00000020" in text:
        cast = r"\(BADSPACEBASE \*\)\s*"
        sets = re.findall(rf"\bregister0x00000020\s*=(?!=)\s*({cast}(?:[\w:]+\(|\(\*))?", text)
        if sets and all(sets):
            new = re.sub(cast, "", text)
            new = re.sub(decl_rx.format("register0x00000020"), "", new, flags=re.M)
            if "BADSPACEBASE" not in new:
                text = re.sub(r"\bregister0x00000020\b", "in_RAX", new)
    if "register0x00000020" in text:
        # written before it is first read, with a value that is not the stack pointer's: a
        # general register the decompiler merged into RSP's slot after a `mov rax, rsp`
        # prologue (FUN_142488950: `register0x00000020 = (BADSPACEBASE *)(ulonglong)*(byte
        # *)(...)`, an index, then `movmskps`; 2026-10-16). A local of its own
        body = re.sub(decl_rx.format("register0x00000020"), "", text, flags=re.M)
        uses = list(re.finditer(r"\bregister0x00000020\b", body))
        first_set = re.match(r"\s*=(?!=)([^;]*);", body[uses[0].end():]) if uses else None
        if first_set and "register0x00000020" not in first_set.group(1) and "stack0x" not in first_set.group(1):
            start = body.index("{\n") + 2
            rest = re.sub(r"\(BADSPACEBASE \*\)", "(ulonglong)", body[start:])
            text = body[:start] + "  ulonglong gen_r20 = 0;\n" + re.sub(r"\bregister0x00000020\b", "gen_r20", rest)
    if re.search(r"\bextraout_var(?:_\d+)?\b", text):
        new = re.sub(decl_rx.format(r"extraout_var(?:_\d+)?"), "", text, flags=re.M)
        text = re.sub(r"\bextraout_var(?:_\d+)?\b", "0", new)
    if re.search(r"\bin_RAX\b", text):
        # the type Ghidra declared it with: the ssl filter erase (FUN_141dfb1c0) walks a bucket
        # chain through `undefined8 *in_RAX`, which as an integer was C2100 and stubbed the
        # erase on the Login click
        declared = re.search(r"^[ \t]*([\w ]+?)[ \t]*(\*+)[ \t]*in_RAX;$", text, re.M)
        rax_type = f"{declared.group(1).strip()} {declared.group(2)}" if declared else "ulonglong"
        body = re.sub(decl_rx.format("in_RAX"), "", text, flags=re.M)
        # what follows a read of the bytes above AL: `in_RAX & 0xff..00`, `(in_RAX >> 8)`
        upper = re.compile(r"\s*(?:&\s*0xffffffffffffff00\b|>>\s*8\b)")
        assigned = False
        for m in re.finditer(r"\bin_RAX\b", body):
            if upper.match(body, m.end()):
                continue
            if re.match(r"\s*=(?!=)", body[m.end():]):
                assigned = True
            elif not assigned:
                return text
        first = body.index("{\n") + 2
        rest = body[first:]
        if rax_type != "ulonglong":
            # the bytes above AL are read as an integer
            rest = re.sub(r"(?<!\(ulonglong\))\bin_RAX(?=\s*(?:&\s*0xffffffffffffff00\b|>>\s*8\b))",
                          "(ulonglong)in_RAX", rest)
        text = body[:first] + f"  {rax_type} gen_rax = 0;\n" + re.sub(r"\bin_RAX\b", "gen_rax", rest)
    return text


def return_address_args(text: str) -> str:
    """`unaff_retaddr` handed on as a call argument: the word at [rsp] on entry, the function's
    own return address, which the Storm containers pass down as the caller tag of an allocation
    (`FUN_140325310(param_1,uVar4,s___AV__TSGrowableArray...,unaff_retaddr)`, the array resize
    FUN_1417d6790 that the static initializer FUN_1417d0a00 needs at startup; 112 functions were
    stubbed for nothing else, 2026-10-15). MSVC's `_ReturnAddress()` is that same word. Left
    alone when the value is read any other way: indexed (`unaff_retaddr[-5] != 0xe8`, code that
    inspects its call site), compared, or stored."""
    decl = re.compile(r"^[ \t]*([\w ]+?\**)[ \t]*unaff_retaddr;\n", re.M)
    m = decl.search(text)
    if not m:
        return text
    body = text[:m.start()] + text[m.end():]
    uses = list(re.finditer(r"\bunaff_retaddr\b", body))
    # a call argument, or stored as it is (`*param_1 = unaff_retaddr;`, the caller recorded with
    # an allocation or a lock: 179 functions on 2026-10-16)
    if not uses or not all((re.search(r"[(,]\s*$", body[:u.start()]) and re.match(r"\s*[,)]", body[u.end():]))
                           or (re.search(r"[^=!<>]=\s*(?:\([\w ]+\**\))?\s*$", body[:u.start()])
                               and re.match(r"\s*;", body[u.end():]))
                           for u in uses):
        return text
    return re.sub(r"\bunaff_retaddr\b", f"(({m.group(1).strip()})_ReturnAddress())", body)


# the image's float sign masks (16 bytes each, for andps / xorps)
FLOAT_ABS_MASK = "DAT_14261aa20"    # 0x7fffffff
FLOAT_SIGN_MASK = "DAT_14261aa30"   # 0x80000000


# a cast or dereference Ghidra puts in front of a term: `(uint)`, `*(uint *)`, `(float)`
_TERM_PREFIX = re.compile(r"(?:\*?\((?:unsigned |signed )?\w+(?: \*+)?\)|\*)$")


def _term_start(text: str, end: int) -> int | None:
    """Where the term that ends at `end` begins: an identifier with its indexing and calls, or a
    parenthesized group, with any casts and dereferences in front of it. None when it is not one."""
    k = end
    while k > 0 and text[k - 1] in " \t":
        k -= 1
    while True:
        if k > 0 and text[k - 1] in ")]":
            close, open_ = text[k - 1], "(" if text[k - 1] == ")" else "["
            depth, k = 1, k - 1
            while k > 0 and depth:
                k -= 1
                depth += {close: 1, open_: -1}.get(text[k], 0)
            if depth:
                return None
            # an identifier before the group makes it a call or an index
            w = re.search(r"[\w.]+$", text[:k])
            if w:
                k = w.start()
                if k > 0 and text[k - 1] in ")]":
                    continue
            break
        w = re.search(r"[\w.]+$", text[:k])
        if not w:
            return None
        k = w.start()
        break
    while True:
        p = _TERM_PREFIX.search(text[:k])
        if not p:
            return k
        k = p.start()


def float_masks(text: str) -> str:
    """Ghidra's spelling of `andps` / `xorps` with the float sign masks DAT_14261aa20 (0x7fffffff)
    and DAT_14261aa30 (0x80000000): `(float)((uint)(a - b) & DAT_14261aa20)` is fabsf(a - b),
    `(float)((uint)x ^ DAT_14261aa30)` is -x. In C the `(uint)` converts the value instead of
    reinterpreting its bits, and a bitwise operator on the float-typed mask does not compile
    (C2297, 57 functions on 2026-10-16).

    Every form is rewritten: with the result read back as a float (`(float)(... ^ mask)`) or kept
    as bits (`(uint)(x * y) ^ mask` stored through a `uint *`, shifted for the sign); the mask used
    directly or through a local holding only it (`uVar5 = DAT_14261aa30;`); the operand a float
    (`fVar3`, `param_2[1]` of a float array) or already its bits (`*(uint *)(p + 0x38)`, a
    parameter typed uint). Which of the last two an operand is, the text does not say; the
    compiler does: GEN_FABS / GEN_FNEG and their ...BITS forms (Ghidra.h) take the bits of a
    float and an integer as they are, by _Generic."""
    masks = {FLOAT_ABS_MASK: "ABS", FLOAT_SIGN_MASK: "NEG"}
    for m in re.finditer(rf"^\s*(\w+) = ({FLOAT_ABS_MASK}|{FLOAT_SIGN_MASK});$", text, re.M):
        if len(re.findall(rf"^\s*{m.group(1)} = ", text, re.M)) == 1:
            masks[m.group(1)] = masks[m.group(2)]
    op_rx = re.compile(rf"\s*([&^])\s*({'|'.join(map(re.escape, masks))})\b(?!\s*[\[(])")
    out, i = [], 0
    while True:
        m = op_rx.search(text, i)
        if not m:
            out.append(text[i:])
            return "".join(out)
        kind = masks[m.group(2)]
        start = _term_start(text, m.start()) if (m.group(1) == "&") == (kind == "ABS") else None
        # the term must be the operator's whole left operand: nothing binding tighter in front
        # (`a == b & mask` is `(a == b) & mask`)
        before = text[:start].rstrip() if start is not None else ""
        if start is None or start < i or not (
                not before or before[-1] in "(,?:|" or (before[-1] == "^" and m.group(1) == "&")
                or (before[-1] == "=" and not re.search(r"[=!<>]=$", before))
                or before.endswith(("&&", "return"))):
            out.append(text[i:m.end()])
            i = m.end()
            continue
        operand = text[start:m.start()].strip()
        # a `(uint)` in front of the operand is the reinterpretation itself, not a conversion
        bare = re.sub(r"^\(uint\)\s*", "", operand)
        wrapped = before.endswith("(float)(") and text[m.end():].lstrip().startswith(")")
        if wrapped:
            lead = text[:start].rstrip()[:-len("(float)(")]
            close = text.index(")", m.end()) + 1
            out.append(text[i:len(lead)] + f"GEN_F{kind}({bare})")
            i = close
        else:
            out.append(text[i:start] + f"GEN_F{kind}BITS({bare})")
            i = m.end()


def _operand_end(text: str, k: int) -> int:
    """Where the C operand starting at k ends: casts and unary operators in front, then a name,
    a parenthesized expression or a call, then any `[...]`, `(...)`, `.x` or `->x` after it."""
    n = len(text)

    def group(j: int, open_: str, close: str) -> int:
        depth = 0
        while j < n:
            if text[j] == open_:
                depth += 1
            elif text[j] == close:
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        return n
    while k < n and text[k] in " \t":
        k += 1
    # a cast or a parenthesized operand
    if k < n and text[k] == "(":
        j = group(k, "(", ")")
        inner = text[k + 1:j - 1].strip()
        if re.fullmatch(r"(?:const\s+)?[A-Za-z_]\w*(?:\s+\w+)*\s*\**", inner):
            return _operand_end(text, j)        # a cast: its operand follows
        k = j
    elif k < n and text[k] in "*&-~!":
        return _operand_end(text, k + 1)
    else:
        m = re.match(r"[A-Za-z_]\w*|0x[0-9a-fA-F]+|\d+(?:\.\d+)?(?:e[-+]?\d+)?", text[k:])
        if not m:
            return k
        k += m.end()
    while k < n:
        if text[k] == "[":
            k = group(k, "[", "]")
        elif text[k] == "(":
            k = group(k, "(", ")")
        elif text.startswith("->", k) or text[k] == ".":
            m = re.match(r"(?:->|\.)\s*\w+", text[k:])
            k += m.end() if m else 1
        else:
            break
    return k


def wrap_uint_casts(text: str) -> str:
    """Each `(uint)<operand>` as GEN_F32BITS(<operand>), the operand to its real end: a call
    (`(uint)REFG(float, x)`, `(uint)ABS(x)`), an element (`(uint)((float *)p)[0]`), a cast in
    turn (`(uint)(float)x`). Taking only the first token gave `GEN_F32BITS(REFG)(float, x)`."""
    out, i = [], 0
    for m in re.finditer(r"\(uint\)", text):
        if m.start() < i:
            continue
        end = _operand_end(text, m.end())
        if end <= m.end():
            continue
        out.append(text[i:m.start()] + f"GEN_F32BITS({wrap_uint_casts(text[m.end():end].strip())})")
        i = end
    out.append(text[i:])
    return "".join(out)


def float_bit_exprs(text: str) -> str:
    """`(float)((uint)fVar4 & 0x80000000 | REFG(float, 0x142632530) & 0x7fffffff)`: bitwise work
    on floats that float_masks() did not take (a copysign by literal masks; a mask already the
    global's REFG; an expression wrapped over two lines). Inside such a `(float)( ... )`, every
    `(uint)` cast and every float global is a float's bits (GEN_F32BITS: an integer stays as it
    is), and the `(float)` puts the bits back (C2296 / C2297 in four functions, 2026-10-17)."""
    out, i = [], 0
    for m in re.finditer(r"\(float\)\s*\(", text):
        if m.start() < i:
            continue
        # the parenthesized operand, to its matching `)`
        depth, k = 1, m.end()
        while k < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[k], 0)
            k += 1
        inner = text[m.end():k - 1]
        if not re.search(r"(?<![&|])[&|^](?![&|=])", inner) or "(uint)" not in inner and "REFG(float" not in inner:
            continue
        new = wrap_uint_casts(inner)
        new = re.sub(r"(?<!F32BITS\()REFG\(float, (0x[0-9a-f]+)\)", r"GEN_F32BITS(REFG(float, \1))", new)
        out.append(text[i:m.start()] + f"gen_bits2f((unsigned int)({new}))")
        i = k
    out.append(text[i:])
    text = "".join(out)
    # and a float global as an operand of a bitwise operator anywhere (`REFG(float, 0x14261aa30) ^
    # (uint)x`, the sign mask first): its bits, as no float can be one in C
    text = re.sub(r"(?<!F32BITS\()REFG\(float, (0x[0-9a-f]+)\)(?=\s*[\^|&](?![&|=]))", r"GEN_F32BITS(REFG(float, \1))", text)
    # (a binary operator, spaced as Ghidra prints them: not `&REFG(...)`, an address, nor `&&`)
    text = re.sub(r"(?<=[^&|][\^|&] )REFG\(float, (0x[0-9a-f]+)\)", r"GEN_F32BITS(REFG(float, \1))", text)
    return text


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


def written_home_slots(text: str) -> list[str]:
    """`uStackX_c`-style locals the function owns: a declared part of its own home area.

    Ghidra names the bytes above the entry stack pointer that do not start a home slot
    `xStackX_N` (N bytes above it, e.g. the upper half of `local_res8`). When the function
    declares one and writes it before any read, it is scratch the function keeps there, a
    plain local; stack_frame() puts it at its offset. A declared buffer there (`auStackX_8 [8]`,
    handed to a callee to fill) is the same. One read first would be the caller's leftovers,
    and stays refused, except as the high part of a `CONCAT` over the slot's low bytes
    (`CONCAT71(uStackX_9,local_res8)`): a narrow value moved as its whole 8-byte slot, whose
    upper bytes are leftovers in the reference too, as upper_half_only() reasons."""
    out = []
    for name in set(re.findall(r"\b[a-z]+StackX_[0-9a-f]+\b", text)):
        if re.search(rf"^\s*[\w ]+\s+{name}\s*\[\s*\w+\s*\];$", text, re.M):
            out.append(name)
            continue
        decl = re.search(rf"^\s*[\w ]+\**\s*{name};$", text, re.M)
        if decl is None:
            continue
        rest = text[decl.end():]
        uses = list(re.finditer(rf"\b{name}\b", rest))
        if uses and re.match(r"\s*=[^=]", rest[uses[0].end():]):
            out.append(name)
        elif uses and all(re.search(r"CONCAT\d+\(\s*$", rest[max(0, u.start() - 12):u.start()]) for u in uses):
            out.append(name)
    return out


def data_pointer_arithmetic(body: str) -> str:
    """What C cannot do with a global's type that the instructions do all the same (2026-10-16):
    - a `code *` global offset as data (`*(int *)(REFG(code *, x) + 0x14)`: a slot ApplyFacts
      typed a function pointer that is also an object's address): through bytes
    - a pointer global shifted or masked (`REFG(undefined1 *, x) >> 0x3a`: tools/globalptrs.py
      typed it a pointer for its dereferences, and the code also tests its bits): as an integer
    - a switch on a pointer expression: on its integer value
    - a void runtime routine's "result" (`uVar7 = qsort(...)`): the call, then 0"""
    # (not one whose address is taken: `&REFG(code *, x) + i` is the slot's address, offset)
    body = re.sub(r"(?<!&)(REFG\(code \*, 0x[0-9a-f]+\))(\s*[+-]\s)", r"((undefined1 *)\1)\2", body)
    # the same of a frame slot holding one (`((code * *)(gen_frame + 0x10))[0] + 8`), and one
    # masked as bits (`(*(code * *)(gen_frame + 0x30)) & ~0xffffffffffULL`)
    slot = r"(\(\(code \* \*\)\(gen_frame \+ 0x[0-9a-f]+\)\)\[[^\[\]]+\]|\(\*\(code \* \*\)\(gen_frame \+ 0x[0-9a-f]+\)\))"
    body = re.sub(slot + r"(\s*[+-]\s|\[)", r"((undefined1 *)\1)\2", body)
    # a piece of a register variable the rewrite has zeroed (`extraout_var._4_4_` once
    # extraout_var is 0, `*(undefined4 *)((char *)&0 + 4)`): zero too
    body = re.sub(r"\(\*\(((?:\w+ )*\w+) \*\)\(\(char \*\)&0 \+ (?:0x)?[0-9a-f]+\)\)", r"((\1)0)", body)
    body = re.sub(slot + r"(\s*(?:&(?!&)|\|(?!\|)|\^|>>|<<)\s)", r"((ulonglong)\1)\2", body)
    # `&` of an address the rewrite already made a value (`&((undefined1 *)REFP(x))`, Ghidra's
    # `&DAT_x` of an array): that address
    body = re.sub(r"&\s*\(\((\w+ \*+)\)REFP\((0x[0-9a-f]+)\)\)", r"((\1)REFP(\2))", body)
    # a pointer cast of a float global (`(float *)REFG(float, x)`: the slot holds a pointer the
    # export typed by what it points at): its 8 bytes
    body = re.sub(r"(\((?:\w+ )*\w+ \*+\))\s*REFG\((?:float|double), (0x[0-9a-f]+)\)", r"\1REFG(undefined8, \2)", body)
    # pointers Ghidra holds in double-typed slots (a pointer moved through an xmm register): an
    # address stored into one (`*param_3 = (double)&x`), an element of a double array cast to a
    # pointer (`(byte *)((double *)(gen_frame + 0x1a0))[i]`), and a double * local's element
    # cast to one (`(void *)*pdVar1`): the slot's 8 bytes as the integer they are
    body = re.sub(r"^([ \t]*)\*(\w+) = \(double\)&", r"\1*(ulonglong *)\2 = (ulonglong)&", body, flags=re.M)
    body = re.sub(r"(\((?:\w+ )*\w+ \*+\))\(\(double \*\)", r"\1((ulonglong *)", body)
    for v in set(re.findall(r"^[ \t]*double \*(\w+);$", body, re.M)):
        body = re.sub(rf"(\((?:\w+ )*\w+ \*+\))\s*\*{v}\b", rf"\1*(ulonglong *){v}", body)
    # the absolute-value mask the float rules missed, applied to a float's bits:
    # `(float)((uint)(x) & REFG(float, ABS))` and `(float)(REFG(float, ABS) & (uint)x)` are |x|
    for _ in range(2):
        body = re.sub(rf"\(float\)\(\(uint\)\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))+)\) & REFG\(float, 0x{FLOAT_ABS_MASK[4:]}\)\)",
                      r"GEN_FABS(\1)", body)
        body = re.sub(rf"\(float\)\(REFG\(float, 0x{FLOAT_ABS_MASK[4:]}\) & \(uint\)(\w+)\)", r"GEN_FABS(\1)", body)
        # and the sign mask, xor'd: negation
        body = re.sub(rf"\(float\)\(REFG\(float, 0x{FLOAT_SIGN_MASK[4:]}\) \^ \(uint\)(\w+)\)", r"GEN_FNEG(\1)", body)
        body = re.sub(rf"\(float\)\(\(uint\)(\w+) \^ REFG\(float, 0x{FLOAT_SIGN_MASK[4:]}\)\)", r"GEN_FNEG(\1)", body)
    # a port global that holds a `code *`, offset as data (`(*(code * *)((char *)&gen_g_x + 0)) + 0x14`)
    body = re.sub(r"(\(\*\(code \* \*\)\(\(char \*\)&gen_g_\w+ \+ 0x[0-9a-f]+\)\))(\s*[+-]\s)",
                  r"((undefined1 *)\1)\2", body)
    body = re.sub(r"(REFG\((?:undefined1?|\w+) \*+, 0x[0-9a-f]+\))(\s*(?:>>|<<|&(?!&)|\|(?!\|)|\^)\s*)",
                  r"((ulonglong)\1)\2", body)

    def switch(m):
        depth, k = 1, m.end()
        while k < len(body) and depth:
            depth += {"(": 1, ")": -1}.get(body[k], 0)
            k += 1
        inner = body[m.end():k - 1]
        if "*)" in inner or "REFP(" in inner:
            return f"switch((longlong)({inner}))", k
        return None, k

    out, i = [], 0
    for m in re.finditer(r"\bswitch\(", body):
        if m.start() < i:
            continue
        new, k = switch(m)
        if new is not None:
            out.append(body[i:m.start()] + new)
            i = k
    out.append(body[i:])
    body = "".join(out)
    return re.sub(r"^([ \t]*)(\w+) = ((?:qsort|free|_free_base)\([^;]*\));$", r"\1\3;\n\1\2 = 0;", body, flags=re.M)


# standard C and C++ runtime names the port's runtime provides, beyond CRT (which the translator
# also rewrites): a reference function of one of these names is the library's, not the game's
C_LIBRARY = {"longjmp", "setjmp", "qsort", "bsearch", "abort", "exit", "atexit", "signal", "raise", "terminate",
             "calloc", "realloc", "fopen", "fclose", "fread", "fwrite", "fseek", "ftell", "fflush", "fgets",
             "fputs", "printf", "sprintf", "snprintf", "vsprintf", "vsnprintf", "sscanf", "strtod", "strtoll",
             "strtoull", "strncat", "strnlen", "wcslen", "wcscmp", "wcscpy", "wcsncpy", "towlower", "towupper",
             "isalnum", "isupper", "islower", "isxdigit", "isprint", "ispunct", "iscntrl", "isgraph", "time",
             "clock", "localtime", "gmtime", "mktime", "strftime", "rand", "srand", "getenv", "remove", "rename",
             "rmdir", "mkdir", "tmpfile", "setvbuf", "ungetc", "fgetc", "fputc", "getc", "putc", "modf", "frexp",
             "ldexp", "acos", "asin", "atan", "cosh", "sinh", "tanh", "log10", "ceil", "fabsf", "roundf", "round",
             "lround", "trunc", "nearbyint", "hypot", "cbrt", "expf", "logf", "acosf", "asinf", "atanf"}


# the C runtime routines a call can land on (CRT, CRT_BY_ADDR) by their parameter count
CRT_ARITY = {"memmove": 3, "memset": 3, "memcpy": 3, "memcmp": 3, "strlen": 1, "strcmp": 2, "strncmp": 3,
             "strchr": 2, "strrchr": 2, "strstr": 2, "strcpy": 2, "strncpy": 3, "strcat": 2, "free": 1,
             "malloc": 1, "_Init_thread_header": 1, "_Init_thread_footer": 1,
             # (CRT_ENTRY's: FUN_140279da8 hands realloc all four registers)
             "realloc": 2, "calloc": 2, "_strdup": 1, "_wcsdup": 1}


def crt_arity(body: str) -> str:
    """A runtime routine called with more arguments than it takes, down to its own: a lifted
    thunk (tools/lift.py) hands on all four argument registers (`return memmove(r_rcx, r_rdx,
    r_r8, r_r9);`, C2197 in 2 functions on 2026-10-16), and the extra ones are only the registers'
    leftovers."""
    out, i = [], 0
    for m in re.finditer(r"(?<![\w.>])(" + "|".join(CRT_ARITY) + r")\(", body):
        if m.start() < i:
            continue
        depth, k, cuts = 1, m.end(), []
        while k < len(body) and depth:
            ch = body[k]
            if ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            elif ch == "," and depth == 1:
                cuts.append(k)
            k += 1
        n = CRT_ARITY[m.group(1)]
        if len(cuts) >= n:
            out.append(body[i:cuts[n - 1]] + ")")
            i = k
    out.append(body[i:])
    return "".join(out)


def merged_registers(text: str) -> str:
    """A register Ghidra calls an input that the body also assigns (`in_R8 = param_1 + 8;` before
    one call, `in_R8` passed again to another whose prototype has more parameters than this
    caller sets; `(in_EAX = FUN_x(...), in_EAX != 0)`): one variable for every value the
    register holds, the first of them whatever it held at the entry. No parameter of this
    function is in that register (it would be a `param_`), so that first value is the garbage
    the reference reads too: an ordinary local, zero to start. 20 functions were stubbed for
    such variables on 2026-10-16. Inputs read and never assigned stay lost."""
    decl = re.compile(r"^([ \t]*)([\w]+(?: [\w]+)*(?: ?\*+)?) ?(in_(?:R[A-Z0-9]+|E[A-Z]+|[A-D][LX]|XMM\d+|YMM\d+)\w*)"
                      r"((?: \[\d+\])?);[ \t]*$", re.M)
    done = set()
    while True:
        # (found afresh after each change: positions taken before an edit no longer hold)
        m = next((d for d in decl.finditer(text) if d.group(3) not in done), None)
        if m is None:
            return text
        name = m.group(3)
        done.add(name)
        rest = text[:m.start()] + text[m.end():]
        if not re.search(rf"(?<![\w.]){name}(?:\._\d+_\d+_)?\s*=(?!=)|\(\*\([^()]*\)\(\(char \*\)&{name} \+", rest):
            continue
        init = " = {0}" if m.group(4) or "gen_v16" in m.group(2) else " = 0"
        new = f"gen_{name}"
        text = text[:m.start()] + f"{m.group(1)}{m.group(2)} {new}{m.group(4)}{init};" + text[m.end():]
        text = re.sub(rf"\b{name}\b", new, text)
        done.add(new)


def code_as_bytes(text: str) -> str:
    """A local Ghidra typed `code *` that is never called but read as data (`pcVar6[0x12]`,
    `*pcVar6 == (code)0x0`): type propagation carried a function-pointer type onto a byte
    pointer. `code` is a function type in C, which can be neither indexed nor held (C2109, C2066:
    9 functions on 2026-10-16), so such a local is the bytes it points at, and `(code)` as a
    value is a byte."""
    for var in re.findall(r"^[ \t]*code \*(\w+);$", text, re.M):
        v = re.escape(var)
        called = re.search(rf"\(\*\s*{v}\)\s*\(|\b{v}\s*\(", text)
        as_data = re.search(rf"\b{v}\s*\[|\*\s*{v}\b(?!\s*\))|\b{v}\s*[=!]=\s*\(code\)", text)
        if called or not as_data:
            continue
        text = re.sub(rf"^([ \t]*)code \*{v};$", rf"\1undefined1 *{var};", text, flags=re.M)
    # a local of type `code` itself (`code cVar1;`, loaded as `cVar1 = *pcVar10`): the byte it holds
    text = re.sub(r"^([ \t]*)code (\w+);$", r"\1undefined1 \2;", text, flags=re.M)
    # `(code)0x0`: no function type is a value
    return re.sub(r"\(code\)(?=\s*(?:0x[0-9a-f]+|\d+|'))", "(undefined1)", text)


def pointer_to_float(body: str, params: list[str]) -> str:
    """`(float)plVar13`: a value Ghidra typed a pointer, converted to floating point. The
    instruction is `cvtsi2ss` of the 64-bit register, a signed integer conversion; C has no
    conversion from a pointer, so it goes through longlong (C2440, 4 functions on 2026-10-16).
    The operand is a variable or parameter declared a pointer, a global read through a pointer
    type (`REFG(longlong *, x)`), or a frame slot of one (`(*(char * *)(gen_frame + 0x38))`)."""
    ptrs = set(re.findall(r"^\s*[\w ]+?\*+\s*(\w+);$", body, re.M)) | \
        {m.group(1) for p in params if (m := re.search(r"\*\s*(\w+)$", p.strip()))}

    def conv(m):
        operand = m.group(2)
        if operand in ptrs or operand.startswith(("REFG(", "(*(")):
            return f"({m.group(1)})(longlong){operand}"
        return m.group(0)

    return re.sub(r"\((float|double)\)\s*(\w+\b(?!\s*[\[(.])|REFG\(\w+ \*+, 0x[0-9a-f]+\)|\(\*\(\w+ \* \*+\)\([^()]*\)\))",
                  conv, body)


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
    return signed_wide_multiply(text)


def signed_wide_multiply(text: str) -> str:
    """`SUB168(SEXT816(a) * SEXT816(b),8)`, an `imul` keeping the high half (a signed divide by a
    constant, e.g. FUN_14147ad20's element count), as __mulh; `,0` is the plain product. The
    operands nest parentheses, so they are cut out by balance, not by a regex."""
    def balanced(s: str, i: int) -> int:
        depth = 0
        for j in range(i, len(s)):
            depth += {"(": 1, ")": -1}.get(s[j], 0)
            if depth == 0:
                return j
        return -1

    out, i = [], 0
    while True:
        k = text.find("SUB168(SEXT816(", i)
        if k < 0:
            out.append(text[i:])
            return "".join(out)
        a0 = k + len("SUB168(SEXT816")
        a1 = balanced(text, a0)
        rest = text[a1 + 1:]
        m = re.match(r"\s*\*\s*SEXT816", rest)
        if a1 < 0 or not m:
            out.append(text[i:k + 1])
            i = k + 1
            continue
        b0 = a1 + 1 + m.end()
        b1 = balanced(text, b0)
        m2 = re.match(r"\s*,\s*([08])\s*\)", text[b1 + 1:])
        if b1 < 0 or not m2:
            out.append(text[i:k + 1])
            i = k + 1
            continue
        a, b = text[a0 + 1:a1], text[b0 + 1:b1]
        if m2.group(1) == "8":
            expr = f"__mulh((longlong)({a}), (longlong)({b}))"
        else:
            expr = f"((longlong)({a}) * (longlong)({b}))"
        out.append(text[i:k] + expr)
        i = b1 + 1 + m2.end()


# (the lines between, none of them the UNLOCK: an empty block, Ghidra's whole account of a
# `lock or [rsp], 0` fence, matched across to the next block's UNLOCK)
LOCK_BLOCK = re.compile(r"([ \t]*)LOCK\(\);\n((?:(?![ \t]*UNLOCK\(\);)[^\n]*\n)*?)[ \t]*UNLOCK\(\);")
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
            # old = lv; if (a == b) { lv = new; [x = y;] }  (no flag variable)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+); if \(([^;]+?) == ([^;]+?)\) \{ ([^;]+?) = ([^;]+); "
                                             r"((?:\w+ = [^;]+; )?)\}", text)
            if c:
                old, loaded, a, b, lv, new, extra = c.groups()
                if _bare(loaded) == _bare(lv) and old in (a, b):
                    cmp = b if a == old else a
                    out = [f"{old} = GEN_ATOMIC_CAS({lv}, {new}, {cmp});"]
                    if extra.strip():
                        out.append(f"if ({old} == {cmp}) {{ {extra.strip()} }}")
            # r = new; if (cmp != lv) { r = lv; } lv = r;  (cmpxchg on a global: r is the new
            # value when it took, else what was there)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+); if \(([^;]+?) != ([^;]+?)\) \{ \1 = ([^;]+); \} "
                                             r"([^;]+?) = \1;", text)
            if c:
                r, new, a, b, got, lv = c.groups()
                if _bare(got) == _bare(lv) and _bare(lv) in (_bare(a), _bare(b)):
                    cmp = b if _bare(a) == _bare(lv) else a
                    out = [f"{r} = GEN_ATOMIC_CAS({lv}, {new}, {cmp});", f"if ({r} == {cmp}) {{ {r} = {new}; }}"]
            # if (lv != 0) { r = lv; } lv = r;  (cmpxchg against 0: install r unless lv is set,
            # r ending up as what lv holds)
            c = out is None and re.fullmatch(r"if \(([^;]+?) != (?:\([\w ]+\**\))?0x?0?\) \{ (\w+) = \1; \} \1 = \2;", text)
            if c:
                lv, r = c.groups()
                out = [f"{{ long long gen_cas_ = GEN_ATOMIC_CAS({lv}, {r}, 0); if (gen_cas_ != 0) {{ {r} = gen_cas_; }} }}"]
            # flag = lv != 0; v = new; if (flag) { v = lv; } lv = v;  (the same, with the flag kept)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+?) != (?:\([\w ]+\**\))?0x?0?; (\w+) = ([^;]+); "
                                             r"if \(\1\) \{ \3 = \2; \} \2 = \3;", text)
            if c:
                flag, lv, v, new = c.groups()
                out = [f"{{ long long gen_cas_ = GEN_ATOMIC_CAS({lv}, {new}, 0); {flag} = gen_cas_ != 0; "
                       f"{v} = {flag} ? gen_cas_ : {new}; }}"]
            # flag = lv == 0; v = new; if (!flag) { v = lv; } lv = v;  (the same, the flag the
            # other way round: a spin lock's acquire, FUN_141687ed0)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+?) == (?:\([\w ]+\**\))?0x?0?; (\w+) = ([^;]+); "
                                             r"if \(!\1\) \{ \3 = \2; \} \2 = \3;", text)
            if c:
                flag, lv, v, new = c.groups()
                out = [f"{{ long long gen_cas_ = GEN_ATOMIC_CAS({lv}, {new}, 0); {flag} = gen_cas_ == 0; "
                       f"{v} = {flag} ? {new} : gen_cas_; }}"]
        if not lines:
            # nothing left of the locked instruction: `lock or [rsp], 0`, a full fence
            out = ["GEN_FENCE();"]
        return out

    def one(m):
        indent, raw = m.group(1), [l.strip() for l in m.group(2).splitlines() if l.strip()]
        # (a statement the decompiler wrapped onto the next line is one: `(&DAT_x)[i] =` then
        # its value, FUN_140285954's `xchg` of an encoded pointer)
        lines = []
        for l in raw:
            if lines and not lines[-1].endswith((";", "{", "}")):
                lines[-1] += " " + l
            else:
                lines.append(l)
        if not lines:
            return indent + core([])[0]
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
            args = m.group(4)
            if args is not None and (not args.strip() or PARAM_DECL.search(args) or args.strip() == "void"):
                continue  # a prototype, not a constructed global
            out[int(m.group(5), 16)] = {
                "name": m.group(2), "file": p.relative_to(ROOT).as_posix(), "decl": decl,
                "const": bool(re.match(r"(?:static\s+)?(?:const|constexpr)\b", decl)) and "*" not in decl,
                "scalar": bool(SCALAR.match(decl)) and not m.group(3),
                # (a class static's definition names its class: `int32_t ModelSceneFrame::
                # s_metaTable = -2;`, which the accessor has to name too, C2065 otherwise)
                # (only a prefix the name follows directly: `static blz::string s_x` names its
                # type's namespace, not the global's)
                "qual": scope_at(text, m.start()) + (re.search(r"((?:\w+::)+)$", decl).group(1)
                                                     if re.search(r"(?:\w+::)+$", decl) else "") + m.group(2),
                "tls": "thread_local" in decl,
            }
        for m in PORT_FNPTR.finditer(text):
            out[int(m.group(3), 16)] = {
                "name": m.group(2), "file": p.relative_to(ROOT).as_posix(), "decl": "void*",
                "const": False, "scalar": True,
                "qual": scope_at(text, m.start()) + m.group(2), "tls": False,
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


def port_imports() -> set[str]:
    """The names the port's executable imports from DLLs, as the linker resolves them: the
    import table's, and the same without a leading underscore's decoration differences."""
    import pefile
    if not F.PORT_EXE.exists():
        return set()
    pe = pefile.PE(str(F.PORT_EXE), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    out = {imp.name.decode() for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) for imp in entry.imports
           if imp.name}
    pe.close()
    # and everything its C runtime DLLs export, imported yet or not (`_rmdir`): linkable all the same
    for dll in ("ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"):
        path = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / dll
        if not path.exists():
            continue
        d = pefile.PE(str(path), fast_load=True)
        d.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        out |= {s.name.decode() for s in getattr(getattr(d, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []) if s.name}
        d.close()
    return out


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
        jf = ROOT / "build/facts/junk.json"
        self.junk_functions = ({int(k, 16) for k in json.loads(jf.read_text())["functions"]} if jf.exists() else set())
        # the second anti-tamper layer (tools/aegis.py): its runtime, stubbed as protection, and
        # the guards it inlines into game functions, stripped from their bodies
        layer = aegis.load(self.cor)
        self.protection = {int(a, 16) for a in layer["protection"]}
        self.protection_hex = set(layer["protection"])
        self.guarded = {int(a, 16) for a in layer["guarded"]}
        self.guard_failed: dict[int, str] = {}
        self.by_name: dict[str, list[int]] = defaultdict(list)
        for a, e in self.cor.items():
            self.by_name[e["name"]].append(a)
            # (and as the decompiler prints a name with a lone `:` in it: FID's
            # `FID_conflict:_rmdir` is called as `FID_conflict__rmdir(`)
            spelled = re.sub(r"(?<!:):(?!:)", "_", e["name"])
            if spelled != e["name"]:
                self.by_name[spelled].append(a)
        self.refdata = load_refdata()
        # globals the instructions use as floating point, where the export only gave a size
        for addr, kind in floatdata.load().items():
            size, gtype = self.refdata.get(addr, (0, "undefined"))
            if gtype.startswith("undefined") and size in (0, 4 if kind == "float" else 8):
                self.refdata[addr] = (4 if kind == "float" else 8, kind)
        self.global_pointers = corpus_global_pointers(self.cor)
        self.ref = Binary()
        # vtables (tools/vtables.py): code that stores one makes every slot callable, though
        # nothing names the slots as calls (bgs::ssl's factory 0x142880130: its Initialize
        # 0x141f6fcf0 was never translated, the SSL module failed to start, and GlueLogin
        # faulted on the null filter map, 2026-10-14)
        vt = ROOT / "build/facts/vtables.json"
        self.vtables: dict[int, list[int]] = {}
        if vt.exists():
            for t in json.loads(vt.read_text(encoding="utf-8"))["vtables"]:
                self.vtables[int(t["vtable"], 16)] = [int(s, 16) for s in t["slots"]]
                # a static object whose vptr the linker filled: naming the object is naming
                # its vtable (bgs::ssl's default filter factory 0x142d39828, whose CreateFilter
                # 0x141f185d0 stayed untranslated and GlueLogin memset a null filter, 2026-10-14)
                for g in t.get("held_by", ()):
                    self.vtables[int(g, 16)] = self.vtables[int(t["vtable"], 16)]
        self._lifted: dict[int, str | None] = {}
        self.v16_failed: dict[int, str] = {}   # what tools/v16.py could not take, by function
        self._arity: dict[int, int | None] = {}
        # the functions whose parameters the instructions do not settle: they leave through an
        # indirect jump and may hand registers on unseen (tools/protos.py)
        pf = ROOT / "build/facts/protos.json"
        protos = json.loads(pf.read_text())["functions"] if pf.exists() else {}
        self.open_protos = {int(k, 16) for k, v in protos.items() if v.get("open")}
        # each reference function's direct call sites, the anti-tamper checks left out
        # (tools/protos.py). From tools/fidelity.py's cache instead, the checks' own calls
        # counted, and 61 functions were stubbed as having lost calls they never made
        self.ref_call_counts = {k[2:]: v["calls"] for k, v in protos.items() if v.get("calls")}
        del protos
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
        # (and read again when what is cached is empty: the cache written while a build had the
        # PDB open held no procedure, no public and no data symbol, and two runs translated with
        # no view of the port at all, 2026-10-17. An empty read is an error, not a result)
        stale = not PORT_CACHE.exists() or PORT_CACHE.stat().st_mtime < F.PORT_EXE.stat().st_mtime
        if not stale:
            try:
                old = json.loads(PORT_CACHE.read_text(encoding="utf-8"))
                stale = not (old.get("procs") and old.get("publics") and old.get("data_publics"))
            except (OSError, ValueError):
                stale = True
        if F.PORT_EXE.exists() and stale:
            procs, _ = F.load_pdb(Binary(F.PORT_EXE))
            cache = {"procs": [[va, size, name] for va, size, name in procs],
                     "publics": {str(k): v for k, v in load_publics().items()},
                     "signatures": {str(k): v for k, v in load_signatures().items()},
                     "data_publics": load_data_publics(),
                     "data_sizes": load_data_sizes()}
            if not (cache["procs"] and cache["publics"] and cache["data_publics"]):
                sys.exit(f"the port's PDB ({F.PORT_PDB}) read as empty: {', '.join(k for k, v in cache.items() if not v)}")
            PORT_CACHE.write_text(json.dumps(cache), encoding="utf-8")
        cache = json.loads(PORT_CACHE.read_text(encoding="utf-8"))
        # the port's functions the linker can reach by name (a `static` one, zlib's bi_flush in the
        # port's own zlib, has a PDB name and no public symbol)
        self.port_public_names = set(cache["publics"].values())
        # and what it imports from DLLs (its C runtime is ucrtbase / vcruntime): together, every
        # name the port links. A reference function of any other name is only the reference's
        self.port_public_names |= port_imports()
        self.port_va_by_name = defaultdict(list)
        for va, size, name in cache["procs"]:
            self.port_va_by_name[name].append(va)
        self.publics = {int(k): v for k, v in cache["publics"].items()}
        self.signatures = {int(k): tuple(v) for k, v in cache["signatures"].items()}
        # the port's own globals: translated code must read the port's state, not the image's
        self.held = port_globals()
        self.held_sorted = sorted(self.held)
        data_publics = cache.get("data_publics", [])
        # (read again when empty: a PDB being written by a build reads as no symbols, and an empty
        # list cached then sent every port global declared in a header to "internal linkage",
        # g_languagesDB and the other DB2 tables among them, until the next link; 2026-10-17)
        if not data_publics:
            data_publics = cache["data_publics"] = load_data_publics()
            PORT_CACHE.write_text(json.dumps(cache), encoding="utf-8")
        if not cache.get("data_sizes"):
            cache["data_sizes"] = load_data_sizes()
            PORT_CACHE.write_text(json.dumps(cache), encoding="utf-8")
        for addr, g in self.held.items():
            # the compiler's own size for it: a span past it would land in other port memory
            g["size"] = cache["data_sizes"].get(g["name"])
            hits = [n for n in data_publics if re.match(rf"\?{re.escape(g['name'])}@(?:\w+@)*@[23]", n)]
            g["mangled"] = hits[0] if len(hits) == 1 else None
        self.global_aliases: dict[int, str] = {}
        self.exported: dict[int, dict] = {}
        self.demoted = json.loads(DEMOTED.read_text()) if DEMOTED.exists() else {}
        self.exceptions = json.loads((ROOT / "tools/fidelity_exceptions.json").read_text(encoding="utf-8"))

    # ---- classification -------------------------------------------------------------------
    def why_no_target(self, a: int) -> str | None:
        """Why a hand-ported function gets no adapter (the translator then translates it a second
        time): None when it has one, or when it is not hand-ported at all."""
        rec = self.tied.get(a)
        if rec is None:
            return None
        if rec["kind"] == "stub":
            return f"the port ({rec['name']}) is a stub"
        if self.port_target(a) is not None:
            return None
        vas = self.port_va_by_name.get(rec["name"], [])
        if not vas:
            return f"{rec['name']} is not in the port's PDB (inlined, or internal and folded)"
        if len(vas) > 1:
            return f"{rec['name']} has {len(vas)} overloads and the argument count does not pick one"
        va = vas[0]
        if self.publics.get(va) is None:
            return f"{rec['name']} has no public symbol (an anonymous namespace or static)"
        if self.signatures.get(va) is None:
            return f"{rec['name']} has no signature record"
        if a in self.mismatched:
            return f"{rec['name']} takes fewer arguments than the reference"
        return f"{rec['name']} returns or takes an aggregate by value"

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
        """The function's corrected decompilation, computed once: planning asks for it twice
        (vetting, callees) and emission once more, which was two thirds of a run (2026-10-16)."""
        cache = self.__dict__.setdefault("_bodies", {})
        if a not in cache:
            cache[a] = self._body(a)
        return cache[a]

    def _body(self, a: int) -> str | None:
        e = self.cor.get(a)
        if not e or "c" not in e:
            return self.lifted(a)
        text = e["c"].replace("\r\n", "\n")
        for patch in self.patches.get(f"{a:x}", []):
            if text.count(patch["from"]) != 1:
                # the corpus changed under it (often: the defect was fixed at its source)
                print(f"tools/translate_patches.json: FUN_{a:x}'s patch no longer matches once; skipped",
                      file=sys.stderr, flush=True)
                continue
            text = text.replace(patch["from"], patch["to"])
        for template, routine in CRT_TEMPLATES.items():
            # (the call's `(` may be on the next line: Ghidra wraps a long template name)
            if template in text:
                # (or with the address ApplyFacts appends to a name more than one function has)
                text = re.sub(re.escape(template) + r"(?:_1[0-9a-f]{8})?\s*\(", routine + "(", text)
        # any other template name made unique by its address (`foo<int>_1402a0b10(`): that function;
        # and one only one function has (`__crt_time_is_leap_year<__int64>(`), the same
        text = re.sub(r"\b[A-Za-z_][\w:]*<[^()\n;]*>_(1[0-9a-f]{8})(?=\s*\()", r"FUN_\1", text)
        text = re.sub(r"\b([A-Za-z_][\w:]*<[^()\n;]*>)(?=\s*\()",
                      lambda m: f"FUN_{self.by_name[m.group(1)][0]:x}" if len(self.by_name.get(m.group(1), [])) == 1
                      else m.group(0), text)
        # a call to an address Ghidra has no function at (`func_0x00014062aba0`): the function
        # there all the same, planned, lifted or stubbed like any other (LNK2019, Lua::GetFontInfo)
        text = re.sub(r"\bfunc_0x0*(14[0-9a-f]{7})\b", r"FUN_\1", text)
        # a local typed by an import's own function type (`DefWindowProcW *local_50;`, holding
        # `Import::USER32_DLL::DefWindowProcW`): a code pointer
        for imp in set(re.findall(r"\bImport::\w+::(\w+)", text)):
            text = re.sub(rf"^([ \t]*){imp} \*(\w+);$", r"\1code *\2;", text, flags=re.M)
        # Ghidra's NaN test, apart from the C library's NAN constant (Ghidra.h)
        text = re.sub(r"\bNAN\s*\(", "GEN_ISNAN(", text)
        # a cast to a small byte array (`(undefined1 [8])CONCAT44(...)`): to the scalar of its size,
        # since C casts to no array type (C2067)
        text = re.sub(r"\(undefined1?\s*\[([1248])\]\)", r"(undefined\1)", text)
        # and through a pointer to one (`*(undefined1 (*) [8])(lVar7 + 0x1c) = ...`, an 8-byte
        # store): a pointer to the scalar, which C can assign through (C2106)
        text = re.sub(r"\(undefined1?\s*\(\*\)\s*\[([1248])\]\)", r"(undefined\1 *)", text)
        # a local byte array of a scalar's size assigned whole (`undefined1 auVar4 [8];`, then
        # `auVar4 = local_198;` and `*(float *)((longlong)auVar4 + 0x24)`, a pointer held in xmm,
        # FUN_141544600): that scalar, when it is never indexed nor has its address taken (C2106)
        for m in reversed(list(re.finditer(r"^([ \t]*)undefined1 (\w+) \[([1248])\];$", text, re.M))):
            n = m.group(2)
            rest = text[m.end():]
            if re.search(rf"^\s*{n} = ", rest, re.M) and not re.search(rf"\b{n}\s*\[|&\s*{n}\b|[(,]\s*{n}\s*[,)]", rest):
                text = text[:m.start()] + f"{m.group(1)}undefined{m.group(3)} {n};" + text[m.end():]
        # Ghidra's rdtsc pseudo-op: the intrinsic; and the XCR0 `xgetbv` reads, as Ghidra names
        # the register: the intrinsic too
        text = re.sub(r"\brdtsc\(\)", "__rdtsc()", text)
        text = re.sub(r"^[ \t]*\w+ in_XCR0;\n", "", text, flags=re.M)
        text = re.sub(r"\bin_XCR0\b", "_xgetbv(0)", text)
        # a string Ghidra named with brackets (`s_<unknown>_142617978`): its address, as any `s_`;
        # and a pointer to one (`&PTR_s_<no_protocol>_1428f8060`): the slot, or what it holds
        text = re.sub(r"\bs_<[^<>\n]*>_(1[0-9a-f]{8})\b", r"((char *)REFP(0x\1))", text)
        text = re.sub(r"&PTR_s_<[^<>\n]*>_(1[0-9a-f]{8})\b", r"((char **)REFP(0x\1))", text)
        text = re.sub(r"\bPTR_s_<[^<>\n]*>_(1[0-9a-f]{8})\b", r"(*(char **)REFP(0x\1))", text)
        # a stack allocation (`&stack0xffffffffffffffe8 + -(uVar5 & 0xfffffffffffffff0)`, the
        # `sub rsp, rax` of an alloca): the same allocation in this frame
        text = re.sub(r"^([ \t]*\w+ = (?:\([\w ]+\*+\))?)&stack0x[0-9a-f]+ \+ -\(([^;()]*(?:\([^;()]*\)[^;()]*)*)\);$",
                      r"\1(undefined1 *)_alloca(\2);", text, flags=re.M)
        # a switch on a value Ghidra typed a pointer (`switch(plVar5)`, a jump-table index held in
        # a pointer-typed register variable): C switches on integers (C2050)
        for var in re.findall(r"\bswitch\((\w+)\)", text):
            if re.search(rf"^\s*[\w ]+\*+\s*{var};$", text, re.M):
                text = text.replace(f"switch({var})", f"switch((longlong){var})")
        text = strip_retcheck(text)[0]
        if a in self.guarded:
            text, status = aegis.strip_guards(text, self.protection_hex)
            if status.startswith("kept"):
                self.guard_failed[a] = status[len("kept: "):]
        text = call_results(dead_lost_stores(rdtsc_upper(decompiler_temps(wide_vectors(va_args(wide_multiply(text)), a)))))
        # a function-local static's guard: MSVC's per-thread epoch (TLS slot 0x2140) compared with
        # the guard, then _Init_thread_header / _footer. All three are the port's own runtime's,
        # so the protocol stays one runtime's.
        text = INIT_EPOCH.sub("_Init_thread_epoch", text)
        text = init_epoch_pointers(text)
        text = simd_pieces(text)
        # 16-byte values as gen_v16, their operators and the SSE pseudo-ops as Ghidra.h's helpers
        # and intrinsics (tools/v16.py). What it cannot take keeps Ghidra's text, and the reason
        wide = {m.group(1) for m in re.finditer(r"\b(_?(?:DAT|UNK)_([0-9a-f]{9}))\b", text)
                if self.refdata.get(int(m.group(2), 16), (0, ""))[0] == 16}
        # (and the callees that return 16 bytes, their results 16-byte values)
        vfuncs = set()
        vparams = {}
        for fm in set(re.findall(r"\bFUN_([0-9a-f]{9})\s*\(", text)):
            sig = self.signature(int(fm, 16)) if int(fm, 16) != a else None
            if sig is not None and sig[0] == "gen_v16":
                vfuncs.add(f"FUN_{fm}")
            if sig is not None:
                # (and those that take 16 bytes by value: passed whole, not by their low lane)
                whole = {k for k, p in enumerate(sig[2]) if p.startswith("gen_v16 ") and "*" not in p}
                if whole:
                    vparams[f"FUN_{fm}"] = whole
        try:
            text = v16.rewrite(text, wide, vfuncs, vparams)
        except v16.V16Unsupported as e:
            self.v16_failed[a] = str(e)
        text = register_upper_halves(text)
        text = merged_registers(text)
        # a lost register's declaration that nothing uses any more (its one use, the destination
        # operand of `movmskps`, gone with tools/v16.py): no register is read at all
        for dm in list(re.finditer(r"^[ \t]*[\w ]+?\*? ?((?:in|unaff|extraout)_\w+)(?: \[\d+\])?;\n", text, re.M)):
            if len(re.findall(rf"\b{dm.group(1)}\b", text)) == 1:
                text = text.replace(dm.group(0), "", 1)
        # every other thread-local: the image's TLS block, which RefImage.cpp builds per thread
        # from the image's template (55 functions were stubbed on "thread-local storage")
        text = re.sub(TLS_BLOCK, "((longlong)GenRefTlsBlock())", text)
        # Ghidra's function-pointer types (`_func_5014 *`) are code, and so are the signatures
        # ApplyFacts gives the function-pointer slots (`fp__calloc_base *`, tools/funcptrs.py):
        # those only ever appear pointed to
        text = re.sub(r"\b_func_\w+\b", "code", text)
        text = re.sub(r"\bfp_\w+\b", "code", text)
        text = code_arithmetic(text)
        text = float_masks(text)
        text = virtual_this(text)
        text = self.xmm_results(text)
        text = rax_upper_parts(text)
        text = return_address_args(text)
        text = global_pieces(text)
        text = code_as_bytes(text)
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

    @staticmethod
    def without_va_spill(body: str, names: list[str]) -> str:
        """A variadic function's body with its va_start spill taken out. Ghidra shows a variadic
        function (the log writer FUN_142292410, `fn(fmt, ...)`) with four parameters: the last
        ones are only stored to their home slots, `local_res10 = param_2; local_res18 = param_3;
        local_res20 = param_4;`, and only the first slot is ever used, as the va_list. Those are
        not parameters the callers must pass: a one-argument call to it was taken for a lost
        argument, demoting its caller (FUN_1422901d0, the vertex format cache) and every caller
        above that, until the D3D12 pipeline build dereferenced the stub's null."""
        spill = {}
        for slot, name in re.findall(r"^\s*(local_res[0-9a-f]+) = (\w+);\s*$", body, re.M):
            if name in names:
                spill[names.index(name)] = slot
        if len(names) < 2 or len(names) - 1 not in spill:
            return body
        k = len(names) - 1
        while k - 1 in spill and k - 1 > 0:
            k -= 1
        # the spilled run is names[k:], all through the last; past the first, the slots are
        # written and never read (declarations aside)
        code = re.sub(r"^\s*[\w\s\*]+\blocal_res[0-9a-f]+(?:\s*\[[^\]]*\])?;\s*$", "", body, flags=re.M)
        if any(len(re.findall(rf"\b{spill[j]}\b", code)) != 1 for j in range(k + 1, len(names))):
            return body
        for j in range(k, len(names)):
            body = re.sub(rf"^\s*{spill[j]} = {names[j]};\s*$", "", body, flags=re.M)
        return body

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
                rest = self.without_va_spill(rest, names)
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
                elif need == 0 and b in self.open_protos:
                    # Ghidra saw no parameters (a body that hands its registers straight on, as
                    # SCritSect::Initialize does to InitializeCriticalSection): the port's own
                    # signature says what it reads. Only for a function that leaves through an
                    # indirect jump (tools/protos.py's `open`): elsewhere the prototype is read off
                    # the instructions, and `xor al, al; ret`, the return-false body 2,377 vtables
                    # share, is no six-argument method because one port function is tagged with it
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
                loops = [n for n, s in strlen_loops(text, start) if s == args[-1]]
                if loops:
                    out.append((end, f",{loops[-1]}"))
            elif len(args) == need - 2:
                # both the string and its length dropped: the strlen just before the call (no
                # other call between) measured the string the call pushes
                last = strlen_matches(text, start)
                if last and not re.search(r"\b\w+\(", text[last[-1].end():start]):
                    n, s = strlen_loops(text, start)[-1]
                    out.append((end, f",{s},{n}"))
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
            need = self.site_need(b, args)
            if need is None or len(args) >= need:
                continue
            sig = self.signature(b)
            positions = list(range(len(args), need))
            values = self.callargs.recover(a, e["size"], b, positions)
            if values is None:
                values = self.recover_returned(a, e["size"], b, positions, text)
            if values is None and self.is_tail_call(text, start, end, b):
                # the call the function ends in is its one `jmp`: that site's registers, which
                # may be its own parameters passed on (a header Ghidra cut short gets them)
                values = self.callargs.recover(a, e["size"], b, positions, tail=True)
            if values is None:
                values = self.recover_by_elimination(a, e["size"], b, positions, text, len(args))
                if isinstance(values, tuple):
                    # one value list per short call, in their order
                    k = nth.get((b, len(args)), 0)
                    nth[(b, len(args))] = k + 1
                    values = values[k] if k < len(values) else None
            if values is not None:
                values = self.name_returns(text, values)
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
                        ptype = (sig[2][p].rsplit(None, 1)[0] if sig and p < len(sig[2]) and sig[2][p] != "..."
                                 else "undefined8")
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
        sites = list(call_sites(text))
        # returned on a path whose last call returns no float (FUN_141af9ba0's early exit, past
        # a lookup that returns a pointer and a store): xmm0 is what that callee left, no value
        # the function computes; zero, as for the same return right after the call
        for r in reversed(list(re.finditer(r"\breturn extraout_XMM0_(?:Da|Qa)(?:_\d+)?;", text))):
            before = [s for s in sites if s[1] < r.start()]
            if not before or self.decl(before[-1][2])[0] not in ("double", "float"):
                text = text[:r.start()] + "return 0;" + text[r.end():]
        decls = [m.span() for m in re.finditer(r"^\s*[\w ]+ extraout_XMM0_\w+;$", text, re.M)]
        uses = [u for u in re.finditer(r"\bextraout_XMM0_(Da|Db|Dc|Dd|Qa|Qb)\b", text)
                if not any(s <= u.start() < e for s, e in decls)]
        if not uses:
            # (and the declaration of a part no longer read goes with it)
            return re.sub(r"^[ \t]*[\w ]+ extraout_XMM0_\w+;\n", "", text, flags=re.M)
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

    def recover_returned(self, a: int, size: int, b: int, positions: list[int], text: str):
        """Arguments the instructions show are what the function returns on another exit: a
        protobuf serializer tail-calls the unknown-fields writer (FUN_1422323d0) with its output
        pointer, the register the plain exit returns, and Ghidra drops it from the call while
        its `return param_2;` keeps the variable. Each position either recovers as callargs
        does, or holds the returned value; that value is the one variable every other return
        statement names. 373 serializers were demoted for this; 272 recover (the rest also call
        the varint writer, thunk_FUN_14222d6e0, with arguments no register holds plainly)."""
        # a variable the call's own result is stored in is not the other exit's
        assigned = {m.group(1) for m in re.finditer(rf"\b(\w+)\s*=\s*(?:\([^;=]*?\))?\s*FUN_{b:x}\s*\(", text)}
        returned = set()
        for m in re.finditer(r"\breturn\b([^;]*);", text):
            value = m.group(1).strip()
            if f"FUN_{b:x}" in value:
                continue
            v = re.fullmatch(r"(?:\([\w ]+\*?\s*\)\s*)?([A-Za-z_]\w*)", value)
            if v is None:
                return None     # an exit returning something else: no one variable to name
            if v.group(1) not in assigned:
                returned.add(v.group(1))
        values = []
        for p in positions:
            v = self.callargs.recover(a, size, b, [p])
            if v is not None:
                values.append(v[0])
                continue
            if len(returned) != 1 or not self.callargs.returned_at_every_call(a, size, b, p):
                return None
            var = next(iter(returned))
            if not re.match(r"[A-Za-z_]\w*$", var) or var.isdigit():
                return None
            values.append(f"(undefined8){var}")
        return values

    @staticmethod
    def name_returns(text: str, values):
        """callargs' `RET(0xT)` (what the call to T returned) as the variable the decompilation
        assigns from that call. Only when the text calls T once and assigns that variable once,
        so the name cannot mean another value; otherwise the recovery is dropped (None)."""
        if isinstance(values, tuple):
            # elimination's per-call lists: a call result there cannot be told apart per call
            return None if any("RET(" in str(x) for v in values for x in v) else values
        out = []
        for v in values:
            if not isinstance(v, str) or "RET(" not in v:
                out.append(v)
                continue
            for target in set(re.findall(r"RET\(0x([0-9a-f]+)\)", v)):
                t = int(target, 16)
                if sum(1 for _, _, b, _ in call_sites(text) if b == t) != 1:
                    return None
                m = re.search(rf"\b(\w+) = (?:\([^()]*\))?(?:thunk_)?FUN_{t:09x}\(", text)
                if not m or len(re.findall(rf"(?<![\w.>]){m.group(1)} = ", text)) != 1:
                    return None
                v = v.replace(f"RET(0x{target})", m.group(1))
            out.append(v)
        return out

    def recover_by_elimination(self, a: int, size: int, b: int, positions: list[int], text: str, shown: int):
        """The calls to `b` disagree on the dropped arguments (luaL_error with a different
        message at each), but the calls the text shows in full account for some of them: take
        those out, and when what is left agrees, it is what the short calls pass."""
        every = self.callargs.recover(a, size, b, positions, every=[])
        if not every:
            return None
        sites = [(start, args) for start, _, t, args in call_sites(text) if t == b]
        short = [args for _, args in sites if len(args) == shown]
        full = [(start, args) for start, args in sites if len(args) > max(positions)]
        if len(every) != len(short) + len(full):
            return None
        integers = set(INTEGER_DECL.findall(text))

        def same(machine, written: str, at: int) -> bool:
            written = written.strip()
            if re.fullmatch(r"[a-z]*Var\d+", written) and isinstance(machine, str):
                # a local the call passes: what it was last assigned before the call
                # (protobuf's ByteSize: `uVar1 = *(uint *)(param_1 + 0x1c); ... FUN_x(uVar1)`)
                assigned = list(re.finditer(rf"(?<![\w.>]){written} = ([^;]+);", text[:at]))
                if assigned:
                    expr = re.sub(r"^\((?:ulonglong|uint|longlong|int)\)", "", assigned[-1].group(1).strip())
                    if spelled_alike(machine, expr, integers):
                        return True
                    # or a call's result: `uVar2 = FUN_14131b890(...)` against RET(0x14131b890)
                    ret = re.fullmatch(r"(?:\(uint\)\()*RET\(0x([0-9a-f]+)\)\)*", machine.replace(" ", ""))
                    if ret and re.match(rf"(?:\(\w+\s*\**\))?(?:thunk_)?FUN_{int(ret.group(1), 16):09x}\(", expr):
                        return True
            if isinstance(machine, int):
                try:
                    return int(written, 0) & 0xffffffffffffffff == machine
                except ValueError:
                    return False
            m = re.fullmatch(r"&DAT_([0-9a-f]+)", str(machine))
            if m and written.startswith('"') and written.endswith('"'):
                literal = written[1:-1].encode().decode("unicode_escape")
                return self.ref.cstr(int(m.group(1), 16)) == literal
            return str(machine) == written or spelled_alike(str(machine), written, integers)

        left = list(every)
        for at, args in full:
            for k, values in enumerate(left):
                if values is not None and all(same(v, args[p], at) for v, p in zip(values, positions)):
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

    @staticmethod
    def is_tail_call(text: str, start: int, end: int, b: int) -> bool:
        """Whether the call at text[start:end+1] is one the function ends in: `return FUN_b(..)`,
        `FUN_b(..); return;`, or `v = FUN_b(..); return v;`. The compiler makes such a call
        the function's `jmp b`, which is how callargs finds its instruction."""
        line_start = text.rfind("\n", 0, start) + 1
        before = text[line_start:start].strip()
        after = text[end + 1:]
        if re.fullmatch(r"return(?:\s*\([\w\s\*]+\))?", before) and re.match(r"\s*;", after):
            return True
        if before == "" and re.match(r"\s*;\s*return\s*;", after):
            return True
        v = re.fullmatch(r"(\w+)\s*=(?:\s*\([\w\s\*]+\))?", before)
        return bool(v and re.match(rf"\s*;\s*return\s+(?:\([\w\s\*]+\)\s*)?{v.group(1)}\s*;", after))

    def site_need(self, b: int, args: list[str]) -> int | None:
        """How many arguments a call to `b` must pass: its arity, or for a variadic function
        (`fn(L, fmt, ...)`, tools/protos.py) its fixed parameters plus what a literal format
        string in the last of them asks for. Ghidra infers a variadic call's arguments from the
        registers set before it, and drops some: 472 of 14,648 calls with a literal format
        were short once the variadic functions were marked (`"%s:%d: "` passed one value)."""
        need = self.arity(b)
        if need is not None:
            return need
        sig = self.signature(b)
        if sig is None or not sig[2] or sig[2][-1] != "...":
            return None
        # its fixed parameters only: what follows is what the call site's instructions pass,
        # which ApplyFacts writes as each variadic call's signature (tools/protos.py's
        # callsites.json). Counting the format's conversions as well stubbed calls the reference
        # makes short itself (`luaL_error(L, "Usage: %s:IsUnit(\"unit\")")`, its `%s` reading
        # whatever r8 held: Lua::IsUnit and three of curl's, 2026-10-16)
        fixed = len(sig[2]) - 1
        return fixed or None

    def lost_arguments(self, text: str) -> str | None:
        for _, _, b, args in call_sites(text):
            need = self.site_need(b, args)
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

    def named_callee(self, name: str, own_name: str) -> int | str | None:
        """What a call by `name` reaches: a reference function's address, the name of a runtime
        routine the port calls instead, or None (left as written). One resolution for planning
        and rewriting: planning saw only `FUN_` and qualified names, and the plain-named callees
        rewriting resolved (zlib's `bi_flush`, FID's `_fperrraise`) were stubbed "called, not in
        the work set" without being considered (23 functions, 2026-10-16)."""
        if name in ("if", "while", "for", "switch", "return", "sizeof", "do", "else"):
            return None
        if name.startswith("Import::"):
            return None

        def by_addr(target: int) -> int | str:
            # (and a runtime entry point called directly is the port's routine too, as a call
            # through its stored pointer is: see CRT_ENTRY)
            return CRT_BY_ADDR.get(target) or CRT_ENTRY.get(target, target)

        if "::" in name:
            cands = self.by_name.get(name, [])
            return by_addr(cands[0]) if len(cands) == 1 else None
        if name.startswith(("FUN_", "thunk_FUN_")):
            return by_addr(int(name[-9:], 16))
        # a name ApplyFacts made unique with its address (`FID_conflict_fallbackMethod_1402728fc`,
        # one of three copies): that function
        sm = re.search(r"_(1[0-9a-f]{8})$", name)
        if sm and int(sm.group(1), 16) in self.cor:
            return by_addr(int(sm.group(1), 16))
        # FID names a runtime routine it matched more than once `FID_conflict_<name>`
        plain = name.removeprefix("FID_conflict_").lstrip("_")
        if plain in CRT:
            return plain
        # a function in the caller's own namespace, which Ghidra prints unqualified
        # (`EnableChangeFaction()` from inside Lua::, the folded `return 0` at 0x14022c3f0
        # that luabind named Lua::EnableChangeFaction; LNK2019, 2026-10-16)
        scope = own_name.split("::")[:-1]
        while scope:
            cands = self.by_name.get("::".join(scope + [name]), [])
            if len(cands) == 1:
                return by_addr(cands[0])
            scope.pop()
        # a reference function called by its plain name that the port does not have (zlib's
        # `bi_flush`, FID's `__ascii_stricmp`, `common_fseek`: LNK2019, 2026-10-16): the
        # reference's own, by address. A name the port's runtime has stays the port's
        # (a name the port links, its own or its C runtime DLLs' (`longjmp`), stays its: only a
        # runtime-internal helper the DLLs do not export, `__ascii_stricmp`, `_handle_nanf`,
        # is the reference's copy)
        cands = self.by_name.get(name, [])
        bare = name.removeprefix("FID_conflict_")
        if bare != name and bare in self.port_public_names:
            # FID's name for a runtime routine the port links under its own (`_rmdir`)
            return bare
        if len(cands) == 1 and name not in self.port_public_names and bare not in self.port_public_names \
                and bare not in CRT and bare not in C_LIBRARY:
            return cands[0]
        return None

    def callees(self, text: str, own_name: str = "") -> set[int]:
        out = set()
        for m in FUNREF.finditer(text):
            out.add(int(m.group(1), 16))
        # code whose address is taken (callbacks, vtable thunks) runs when it is called back
        for m in re.finditer(r"&LAB_(1[0-9a-f]{8})\b", text):
            a = int(m.group(1), 16)
            if a in self.cor or self.lifted(a) is not None:
                out.add(a)
        # a vtable whose address is taken or stored: its slots run when the object is called
        for m in re.finditer(r"\b(?:vtable|PTR|DAT)_\w*?_?(1[0-9a-f]{8})\b|\b0x(1[0-9a-f]{8})\b", text):
            for s in self.vtables.get(int(m.group(1) or m.group(2), 16), ()):
                if s in self.cor or self.lifted(s) is not None:
                    out.add(s)
        for name in set(CALL.findall(text)):
            if name.startswith(("FUN_", "thunk_FUN_")):
                continue
            target = self.named_callee(name, own_name)
            if isinstance(target, int):
                out.add(target)
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
        # (Ghidra breaks a long qualified name after a `::`, the rest on the next line)
        head = re.sub(r"::\s+", "::", head)
        # (a pointer-to-16-bytes result, `undefined1 (*) [16] FUN_x(...)`: read as a name
        # `undefined1` called with `(*) [16] FUN_x(...)`, the header's parameters were garbage and
        # the function "a type C does not know" (strchr's SSE copy FUN_14026c5f4, 2026-10-16))
        head = re.sub(r"^undefined1? \(\*\) \[16\]\s*", "gen_v16 * ", head)
        # (an operator's name, `operator()<>_140285170`, read as `operator` called with `)<>...`)
        head = re.sub(r"(?<![\w:])(?:[\w:~]*::)?operator[^\s(]*\(\)[<>,\w]*_(1[0-9a-f]{8})\b", r"FUN_\1", head)
        # (a template's arguments can hold commas: `std::basic_iostream<char,std::char_traits<char>_>::`;
        # without them the name ended at the comma and the rest was taken for the return type)
        m = re.match(r"(?s)(.*?)\b([\w:~<>,]+)\s*\((.*)\)\s*$", head)
        if not m:
            return None
        ret = re.sub(r"\b__\w+\b", "", m.group(1)).strip() or "void"
        # (a function-pointer return type is code, as for the parameters below: `fp_FUN_x *`)
        ret = re.sub(r"\b(?:_func|fp)_\w+\b", "code", ret)
        # (16 bytes, a vector result, or a pointer to them: Ghidra.h's gen_v16, tools/v16.py)
        # (Ghidra writes a vector result `undefined1  [16]`, two spaces, when it is placed by hand)
        ret = re.sub(r"^undefined1?\s+\[16\]$", "gen_v16", ret)
        ret = re.sub(r"^undefined1?\s+\(\*\)\s+\[16\]$", "gen_v16 *", ret)
        # Ghidra's return types disagree between a function and its callers (one decompiles it
        # void, another uses rax), so every function hands back a register's worth
        if ret == "void":
            ret = "undefined8"
        # split at the top level only: a function-pointer parameter, `void (*param_1)(int,int)`,
        # carries commas of its own, and is declared `code *param_1` (C calls through it with any
        # arguments); split at every comma it was "a type C does not know ((*param_1))"
        params, depth, cur = [], 0, ""
        for ch in m.group(3):
            if ch == "," and depth == 0:
                params.append(cur)
                cur = ""
                continue
            depth += {"(": 1, ")": -1}.get(ch, 0)
            cur += ch
        params.append(cur)
        params = [re.sub(r"^.*\(\s*\*+\s*(\w+)\s*\)\s*\(.*\)$", r"code *\1", " ".join(p.split()), flags=re.S)
                  for p in params]
        # (Ghidra's function types in a header: `_func_void_void_ptr_uchar *`, `fp_FUN_x *`)
        params = [re.sub(r"\b(?:_func|fp)_\w+\b", "code", p) for p in params]
        # (a pointer to 16 bytes, `undefined1 (*param_2) [16]`, the type the decompiler infers for
        # a pointer SSE loads through: Ghidra.h's gen_v16, tools/v16.py; 20 functions stubbed)
        params = [re.sub(r"^undefined1? \(\*(\w+)\) \[16\]$", r"gen_v16 *\1", p.strip()) for p in params]
        # (and 16 bytes by value, a vector argument ApplyFacts placed in an xmm register)
        params = [re.sub(r"^undefined1? (\w+) \[16\]$", r"gen_v16 \1", p) for p in params]
        params = [p.strip() for p in params if p.strip() and p.strip() != "void"]
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
        return indirect_float_calls(body, params)

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
        # breadth first: a function is planned at its shortest depth. Depth first, the first
        # visit could be the deepest, stub it "beyond the translation depth" and mark it seen:
        # a new corpus changed the order, and 619 functions lost the string-buffer helpers
        # FUN_142203d20 / FUN_142203dd0 they had been translated with (2026-10-17)
        from collections import deque
        frontier = deque((a, 0) for a in roots)
        queued = set(roots)
        seen = set()
        while frontier:
            a, d = frontier.popleft()
            if a in seen:
                continue
            seen.add(a)
            # (a function that did not compile is planned as translated, and stubbed only at
            # emission: its callees stay in the work set, so the set, the files and the
            # declarations stay as they were from one --fix round to the next)
            reason = self.why_not(a, root=d == 0, demotion=False)
            if reason is None:
                translate[a] = d
                for b in self.callees(self.body(a), self.cor[a]["name"] if a in self.cor else ""):
                    if b in seen or b in CRT_BY_ADDR or b in CRT_ENTRY or self.is_lib(b):
                        continue
                    if self.port_target(b) is not None and b not in roots:
                        adapt[b] = True
                        seen.add(b)
                    # (a port the translated code cannot call, internal or returning an aggregate,
                    # or whose signature is not the reference's, is translated like any other:
                    # what it shares with the port is redirected to the port's own)
                    elif d + 1 <= self.depth:
                        frontier.append((b, d + 1))
                        queued.add(b)
                    elif b not in queued:
                        # (not when it is queued already, at the depth limit: it is translated)
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

    def why_not(self, a: int, root: bool, demotion: bool = True) -> str | None:
        if demotion and f"{a:x}" in self.demoted:
            return "did not compile: " + self.demoted[f"{a:x}"]
        if a in self.protection:
            # (the report leaves out reasons starting "anti-tamper": protection, not code to port)
            return "anti-tamper runtime (tools/aegis.py): protection, not ported"
        text = self.body(a)
        if text is None:
            return "not in the corpus"
        if a in self.guard_failed:
            # (a game function: listed with the defects, unlike the runtime above)
            return f"an inlined anti-tamper guard tools/aegis.py could not strip: {self.guard_failed[a]}"
        if a in self.v16_failed:
            return f"16-byte values: {self.v16_failed[a]}"
        # (the code only, not what its strings say: `"in_token"` is no register)
        code = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
        # (a field of a struct Ghidra.h lays out as Ghidra does: the DOS header type Ghidra
        # gives any pointer that reaches the image base, `param_3->e_magic[0]`, 395 uses)
        known = set(re.findall(r"\b(?:_?IMAGE_DOS_HEADER)\s*\*\s*(\w+)\b", code))
        if known:
            code = re.sub(r"\b(?:" + "|".join(map(re.escape, known)) + r")->", "known.", code)
        for reason, rx in HARD:
            hits = {m.group(0) for m in rx.finditer(code)} - set(upper_half_only(text)) - set(written_home_slots(text))
            if hits:
                if a in self.junk_functions and "/* src: " not in text and reason.startswith(
                        ("register the decompiler", "bad data", "a decompilation Ghidra marked")):
                    # (code the clean exe still holds as the anti-tamper layer's junk,
                    # tools/junkscan.py: protection, which is not ported)
                    return "anti-tamper code the clean exe leaves as junk (protection, not ported)"
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
            # (an import called through its slot too: `(*Import::KERNEL32_DLL::GetCurrentProcess)()`,
            # uncounted, made FUN_140250520's 29 calls look like 28, 2026-10-16)
            # (in a guarded function the shown calls are counted before tools/aegis.py strips the
            # guards: their calls through decoded pointers are in the instructions too. Counted
            # after, the per-frame tick FUN_14167c520 looked like it had lost one, and was stubbed
            # with the 213 game functions only it reaches, 2026-10-17)
            seen = self.cor[a].get("c", text) if a in self.guarded else text
            shown = len(re.findall(r"\(\*+\s*\([^;]*?\)\s*\(|\(\*[\w:]+\)\s*\(", seen))
            if self.callargs.indirect_calls(a, self.cor[a]["size"]) > shown:
                return "an indirect call the decompiler lost"
        # the reference's own direct call sites (tools/protos.py): a body that shows under half
        # of them is one the decompiler lost
        calls = self.ref_call_counts.get(f"{a:x}", 0)
        if calls >= 2:
            # (named callees too, `_callnewh(param_1)`: any name a reference function has)
            shown = len(FUNREF.findall(text)) + sum(1 for n in CALL.findall(text) if "::" in n or n in self.by_name)
            if shown * 2 < calls:
                return f"the decompiler lost calls ({shown} shown, the reference makes {calls})"
        # (casts anywhere; declarations only in the block that holds them, before Ghidra's first
        # blank line: a wrapped expression line, `param_4 * DAT_14261a988;`, is no declaration)
        inner = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text.split("{", 1)[1])
        cut = re.search(r"\n[ \t]*\n", inner)
        decls_part = inner[:cut.start()] if cut else inner
        words = [x for x, _y in TYPE_WORD.findall(inner) if x] + [y for _x, y in TYPE_WORD.findall(decls_part) if y]
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
        own_name = self.cor[a]["name"] if a in self.cor else ""
        # `_local_a8`: Ghidra's name for a wider store over a declared local (`local_a8` an
        # undefined4, the store 8 bytes: CONCAT44), as `_DAT_x` is for a global
        rest = re.sub(r"\b_((?:local|[a-z]*Stack)_[0-9a-f]+|param_\d+)\b",
                      lambda m: f"(*(undefined8 *)&{m.group(1)})"
                      if (re.search(rf"^\s*[\w ]+\**\s*{m.group(1)}\b", rest, re.M)
                          or re.search(rf"\b{m.group(1)}\s*[,)]", head)) else m.group(0), rest)
        # `_local_res10`: the whole 8-byte home slot at entry + 0x10, over narrower views of it
        # (`uStackX_14`, its upper half). The slot itself, declared if Ghidra did not
        for slot in sorted(set(re.findall(r"\b_(local_res[0-9a-f]+)\b", rest))):
            if not re.search(rf"^\s*[\w ]+\**\s*{slot}\b", rest, re.M):
                rest = "\n  undefined8 " + slot + ";" + rest
            rest = re.sub(rf"\b_{slot}\b", f"(*(undefined8 *)&{slot})", rest)
        body = stack_frame(atomics("{" + rest), params)
        for name in upper_half_only(body):
            body = re.sub(rf"^(\s*[\w ]+ {name});$", r"\1 = 0;", body, count=1, flags=re.M)
        # switch cases are labels Ghidra qualifies with `::`
        body = re.sub(r"\b(switchD_\w+)::(\w+)", r"\1__\2", body)
        # a function's address taken, `&FUN_x`: the reference address, which the jumps dispatch
        body = re.sub(r"&\s*\b(?:thunk_)?FUN_([0-9a-f]{9})\b(?!\s*\()", r"((code *)REFP(0x\1))", body)
        body = re.sub(r"&\s*\bLAB_([0-9a-f]{9})\b", r"((code *)REFP(0x\1))", body)
        # the image base: Ghidra types 0x140000000 as the DOS header, and `base + rva` (the jump
        # tables' targets, `lea rcx, [0x140000000]` + an RVA) comes out as its first field's
        # address plus the RVA (C2065, 6 functions on 2026-10-16)
        # (its `e_program`, the DOS stub after the header, is the bytes at +0x40)
        body = re.sub(r"&?\s*\bIMAGE_DOS_HEADER_140000000\.e_program\b", "((char *)REFP(0x140000040))", body)
        # (and `e_lfanew`, the offset of the PE header, the int at +0x3c)
        body = re.sub(r"\bIMAGE_DOS_HEADER_140000000\.e_lfanew\b", "(*(int *)REFP(0x14000003c))", body)
        body = re.sub(r"&?\s*\bIMAGE_DOS_HEADER_140000000(?:\.e_magic)?\b", "((char *)REFP(0x140000000))", body)
        # the thread's own TEB and PEB, which Ghidra names by field (`Self`, the TEB's pointer to
        # itself at gs:[0x30]; `ProcessEnvironmentBlock` at gs:[0x60]): this process has them too
        body = re.sub(r"(?<![\w.>])Self\b(?!\s*\()", "((void *)__readgsqword(0x30))", body)
        body = re.sub(r"(?<![\w.>])ProcessEnvironmentBlock\b(?!\s*\()", "((void *)__readgsqword(0x60))", body)
        # KUSER_SHARED_DATA, mapped at 0x7ffe0000 in every process (`uRam000000007ffe0004`, the
        # tick multiplier): read in place, as the reference does
        body = re.sub(r"\b([a-z]+)Ram0*(7ffe[0-9a-f]{4})\b",
                      lambda m: f"(*(volatile undefined{ {'u': 4, 'l': 8, 'c': 1, 's': 2}.get(m.group(1)[0], 8)} *)0x{m.group(2)}ULL)",
                      body)
        # (comments stripped first: `(?:/\*.*?\*/\s*)*void` backtracked exponentially over the
        # long `types:` block comment and `//` lines of FUN_1414d0dd0's header and never returned)
        bare_head = re.sub(r"//[^\n]*", "", re.sub(r"(?s)/\*.*?\*/", "", head))
        if re.match(r"\s*void\b", bare_head):
            # A function Ghidra calls void because it ends in a tail call (FUN_140d11f10 jumps
            # to GetObjectFromLua and hands its object back in rax): a call just before a return
            # leaves its result where the caller reads one, so the return carries it. Named calls
            # too: FUN_140574f50, the string buffer's grow, ends in Storm::SMemAlloc and its
            # `return 0` handed the SChannel filter a null 0xa000-byte buffer, 2026-10-14.
            # Only a call that is a statement of its own: one that continues the line above
            # (`DAT_x =\n       FUN_y(...);`) is the right side of an assignment, and turning it
            # into `return` was C2059 in 14 functions (the CVar registrations, 2026-10-16)
            def carry(m):
                before = body[:m.start()].rstrip()
                if before and before[-1] not in ";{}:":
                    return m.group(0)
                return f"{m.group(1)}return (undefined8){m.group(2)};"

            body = re.sub(r"^(\s*)((?:thunk_)?FUN_[0-9a-f]{9}\([^;]*\)|[A-Za-z_]\w*(?:::~?\w+)+\([^;]*\)"
                          r"|\(\*+\(code \*+\)[^;]*\)\([^;]*\));"
                          r"(\s*\n\s*)return\s*;",
                          carry, body, flags=re.M)
        body = re.sub(r"\breturn\s*;", "return 0;", body)

        def bare(m):
            # a named reference function used as a value (`_guard_check_icall`)
            cands = self.by_name.get(m.group(1), [])
            return f"((code *)REFP(0x{cands[0]:x}))" if len(cands) == 1 else m.group(0)

        # (`__scrt_unhandled_exception_filter` too: a name the reference's functions hold, nothing else)
        body = re.sub(r"(?<![\w.>])(_{1,2}[A-Za-z]\w+)\b(?!\s*\()", bare, body)
        # a qualified function name used as a value (`pcVar7 = Lua::EnableChangeFaction;`, a
        # callback registered by address): its reference address, as for a bare `_name`
        body = re.sub(r"(?<![\w:.>])&?([A-Za-z_]\w*(?:::~?\w+)+)\b(?!\s*[(:])",
                      lambda m: f"((code *)REFP(0x{self.by_name[m.group(1)][0]:x}))"
                      if len(self.by_name.get(m.group(1), [])) == 1 else m.group(0), body)
        body = re.sub(r"&?\b([A-Za-z_][\w<>,]*(?:::[\w<>,~]+)*::vftable)\b",
                      lambda m: f"((undefined8 *)REFP(0x{VFTABLES[m.group(1)]:x}))" if m.group(1) in VFTABLES
                      else m.group(0), body)

        def call(m):
            target = self.named_callee(m.group(1), own_name)
            if target is None:
                return m.group(0)
            if isinstance(target, str):
                return target + "("
            uses.add(target)
            return f"CALLF({self.decl(target)[0]}, @FUN_{target:x})("

        body = CALL.sub(call, body)
        body = crt_arity(body)
        # a void runtime routine whose result the reference's own wrapper returned (rax as it left
        # it): FUN_1415aec60 tail-calls the reference free, now the port's (CRT_ENTRY), and
        # `return (undefined8)free(param_1);` is C2069
        body = re.sub(r"^([ \t]*)return (?:\(\w+\))?((?:free|_free_base)\([^;]*\));$", r"\1\2;\n\1return 0;",
                      body, flags=re.M)

        def funref(m):
            # a function used as a value: its reference address, which the jump stubs dispatch
            return f"((code *)REFP(0x{m.group(1)}))"

        # (calls were marked `@FUN_x` above so this leaves them alone)
        body = re.sub(r"(?<!@)\b(?:thunk_)?FUN_([0-9a-f]{9})\b(?!\s*\()", funref, body).replace("@FUN_", "FUN_")

        def global_(m):
            whole, addr = m.group(1), int(m.group(2), 16)
            if whole.startswith(("s_", "u_", "ws_", "PTR_s_", "PTR_u_")) and not whole.startswith("PTR_"):
                return f"((char *)REFP(0x{addr:x}))"
            if whole.startswith(("LAB_", "switchD_")) and "__switchdataD_" not in whole:
                # labels stay labels (`switchD_x__switchdataD_y` is no label but the switch's
                # table at y, indexed as data: `(&switchD_1415aef8c__switchdataD_1415af3a8)[i]`,
                # 11 functions demoted as undeclared, 2026-10-16)
                return whole
            size, gtype = self.refdata.get(addr, (8, "undefined8"))
            if size == 8 and (re.fullmatch(r"(?:fp|_func)_\w+\s*\*", gtype) or (
                    whole.startswith(("PTR_FUN_", "PTR_LAB_", "PTR_thunk_"))
                    and gtype in ("pointer", "pointer64", "undefined *", "undefined8"))):
                # a slot holding code: the data export spells a pointer to a function as
                # `undefined *` (or as the signature ApplyFacts gave it, tools/funcptrs.py), and a
                # call through `undefined *` is no C. The cursor scaler table PTR_FUN_142d08190 was
                # called as `(*(&REFG(undefined *, ...))[i])(size)` and demoted (2026-10-16)
                gtype = "code *"
            elif whole.startswith(("PTR_s_", "PTR_u_")) and size == 8 and gtype in ("pointer", "pointer64", "undefined8"):
                # a pointer to a string (Ghidra names it by the string): the protobuf services'
                # names (`*PTR_s_bgs_protocol_account_v1_AccountS_142d39950`, read and measured by
                # the service lookups on the logon path) were untyped pointers and demoted
                gtype = "char *" if whole.startswith("PTR_s_") else "ushort *"
            elif gtype.startswith("undefined") and size == 8 and addr in borrowed:
                gtype = borrowed[addr]
            elif gtype.startswith("undefined") and size == 8 and addr in self.global_pointers and addr in derefed:
                # only where this function dereferences it: elsewhere Ghidra's `DAT_x + 8` is
                # integer arithmetic, which a pointer type would scale by the element
                gtype = self.global_pointers[addr]
            elif gtype.startswith("undefined") and size == 8 and addr in vtable_reads:
                # an object pointer whose first 8 bytes, its vtable, are read and offset to a
                # slot (`(**(code **)(*DAT_14348b658 + 0x198))(...)`): the width is the vtable
                # pointer's, whatever else the object holds (C2100, 13 functions on 2026-10-16)
                gtype = "longlong *"
            elif gtype in ("pointer", "pointer64") and size == 8 and addr in derefed:
                # Ghidra's bare `pointer` is `undefined *`: `*PTR_DAT_x` is one byte through it.
                # As an integer it demoted 98 functions, among them the blz::string clears on the
                # Login click (FUN_14147f360: `*PTR_DAT_142d076f0 = 0` is `mov byte ptr [rax], 0`)
                gtype = "undefined *"
            # (and a global the data export has no item for at all, whose 8 bytes above are only a
            # default: the tick's `DAT_14311c52a = '\0'`, `mov byte ptr`, cleared the port's bool
            # s_gxRestarted and the seven bytes after it each frame, 2026-10-17)
            wide = whole.startswith(("_DAT_", "_UNK_"))
            if (wide or (addr not in self.refdata and addr not in derefed)) and a in self.cor:
                # Ghidra's `_DAT_x` is an access wider (or of another kind) than the item defined
                # at x, so that item's type is the wrong one: the instructions say what is read.
                # FUN_141283140 divides by `_DAT_142cfbe58`, a float load factor
                # (`divss xmm0, dword ptr [0x142cfbe58]`) over an undefined1, and read one byte of
                # it: 0, a quotient of inf and a bucket count that wrapped (2026-10-15).
                seen = self.callargs.global_accesses(a, self.cor[a]["size"]).get(addr, set())
                if len(seen) == 1:
                    n, kind = next(iter(seen))
                    t = {(4, "x"): "float", (8, "x"): "double", (1, "i"): "undefined1", (2, "i"): "undefined2",
                         (4, "i"): "undefined4", (8, "i"): "undefined8"}.get((n, kind))
                    if t is not None and (n != size or (kind == "x") != (gtype in ("float", "double"))):
                        size, gtype = n, t
                # (and read 8 bytes wide and dereferenced, `lVar1 = *_DAT_142d01560;`: a pointer, to
                # what its casts say or to 8-byte slots)
                if wide and size == 8 and gtype.startswith("undefined") and addr in derefed:
                    gtype = self.global_pointers.get(addr, "undefined8 *")
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
        derefed = {int(x, 16) for pair in re.findall(r"\*_?(?:PTR_)*DAT_([0-9a-f]{9})\b|(?<!\w)_?(?:PTR_)*DAT_([0-9a-f]{9})\[",
                                                     body)
                   for x in pair if x}
        vtable_reads = {int(x, 16) for x in re.findall(r"\(\s*\*_?DAT_([0-9a-f]{9})\s*\+\s*0x[0-9a-f]+\s*\)", body)}

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
        # or one the decompiler casts a pointer into or compares with one: the bgs log sink
        # registry (FUN_141effc10) is `DAT_143264a00 = (undefined8 *)FUN_1402545c0(...)` and
        # `DAT_143264a00 == (undefined8 *)0x0`; demoted, it left the registry null and
        # BattleNetInitialize faulted on it (98 of 220 such demotions show a cast like this)
        for m in re.finditer(r"\b_?DAT_([0-9a-f]{9})\s*(?:==|!=|=)\s*\(((?:\w+ )*\w+ ?\*+)\)", body):
            borrowed.setdefault(int(m.group(1), 16), re.sub(r"\s*(\*+)$", r" \1", m.group(2)).strip())

        # a constant Ghidra wrote as the address of a "global" outside the image
        # (`&DAT_8000000000000020`, a blz::vector's inline capacity flag) is that constant
        def outside(m):
            v = int(m.group(1), 16)
            return m.group(0) if IMAGE_LO <= v < IMAGE_HI else f"((undefined1 *){v:#x}ULL)"

        body = re.sub(r"&\s*\b_?(?:PTR_)*(?:DAT|UNK|PTR|LAB)_([0-9a-f]+)\b", outside, body)
        # an access in the null page (`_DAT_00000008 = 0;`, Ghidra folding a null pointer's field
        # into an address): the same access, faulting where the reference faults
        body = re.sub(r"\b_?(?:DAT|UNK)_(0000[0-9a-f]{4})\b",
                      lambda m: f"(*(volatile undefined8 *){int(m.group(1), 16):#x}ULL)", body)
        body = re.sub(r"\b([a-z]+)Ram0*([0-9a-f]{1,4})\b",
                      lambda m: f"(*(volatile undefined{ {'u': 4, 'l': 8, 'c': 1, 's': 2}.get(m.group(1)[0], 8)} *)"
                                f"{int(m.group(2), 16):#x}ULL)", body)
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
        # `ram0x0001432089b0`: a global the decompiler reached as a raw address (FUN_141ac6ac0)
        body = re.sub(r"\bram0x0*(1[0-9a-f]{8})\b", lambda m: global_(re.match(SYMBOL, f"DAT_{m.group(1)}")), body)
        body = SYMBOL.sub(global_, body)
        body = RAM.sub(lambda m: f"REFG(undefined{ {'u': 4, 'l': 8, 'c': 1, 's': 2}.get(m.group(1)[0], 8)}, 0x{m.group(2)})", body)
        body = IMPORT.sub(lambda m: f"(*(code *)&{IMPORT_MERGED.get(m.group(2), m.group(2))})", body)
        # a global called through: a code pointer, whatever the export typed it
        # (and one typed a data pointer, `undefined8 *` / `undefined *` (tools/globalptrs.py)):
        # `(*REFG(undefined8 *, x))()` calls what the slot holds
        body = re.sub(r"\(\*REFG\((?:undefined\d?(?: \*)?), (0x[0-9a-f]+)\)\)\s*\(", r"(*REFG(code *, \1))(", body)
        # the same at a port global it is tied to (a function pointer the port holds)
        body = re.sub(r"\(\*\(\*\(undefined\d? \*\)(\(\(char \*\)&gen_g_[0-9a-f]{9} \+ 0x[0-9a-f]+\))\)\)\s*\(",
                      r"(*(*(code **)\1))(", body)
        # a local Ghidra typed as an 8-byte integer and called through: the slot of a blz::function
        # whose stack layout it merged with other data (FUN_1416a0db0: `(*local_180)(...)` with
        # `undefined8 local_180`, C2100 in 22 functions). The call is a code pointer; the other
        # uses keep the integer type
        int_locals = {m.group(2) for m in re.finditer(r"^\s*(undefined8|longlong|ulonglong) (\w+);$", body, re.M)}
        if int_locals:
            body = re.sub(r"\(\*(\w+)\)\s*\(",
                          lambda m: f"(*(code *){m.group(1)})(" if m.group(1) in int_locals else m.group(0), body)
        # the same local once stack_frame() has put it on the frame:
        # `(*(*(undefined8 *)(gen_frame + 0x8)))(` (FUN_1416a0db0)
        body = re.sub(r"\(\*\(\*\((?:undefined\d?|longlong|ulonglong) \*\)(\(gen_frame \+ 0x[0-9a-f]+\))\)\)\s*\(",
                      r"(*(*(code **)\1))(", body)
        # a byte Ghidra typed `code` (a buffer of them, e.g. FUN_141487ed0's `local_res10[0] =
        # (code)(DAT_1430af3f8 != 0)`): C cannot cast to a function type; it is a byte store
        body = re.sub(r"\(code\)\s*\(", "(undefined1)(", body)
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
        # a label that closes a block (Ghidra ends a loop or a switch on one) needs a statement
        # to label before C23: `LAB_x: ;` (C5299, 15 functions on 2026-10-16)
        body = re.sub(r"^([ \t]*(?:[A-Za-z_]\w*|case [^:\n]+|default)[ \t]*:)[ \t]*\n([ \t]*\})",
                      r"\1 ;\n\2", body, flags=re.M)
        body = pointer_to_float(body, params)
        body = data_pointer_arithmetic(body)
        body = float_bit_exprs(body)
        # a `code *` parameter never called but indexed or offset (`param_1[1]`, `param_1 + 0x20`):
        # the bytes it points at, at each use (its declaration is the prototype's, so stays)
        for p in params:
            # a `double *` parameter whose elements are cast to pointers (`(void *)*param_2`,
            # `(undefined8 *)param_1[0x1c]`): those are pointers in 8-byte slots, read as such
            dm = re.fullmatch(r"double \*(\w+)", p.strip())
            if dm:
                v = dm.group(1)
                body = re.sub(rf"(\((?:\w+ )*\w+ \*+\))\s*\*{v}\b", rf"\1*(ulonglong *){v}", body)
                body = re.sub(rf"(\((?:\w+ )*\w+ \*+\))\s*{v}\[", rf"\1((ulonglong *){v})[", body)
            pm = re.fullmatch(r"code \*(\w+)", p.strip())
            if pm and not re.search(rf"\(\*\s*{pm.group(1)}\)\s*\(|\b{pm.group(1)}\s*\(", body):
                v = pm.group(1)
                body = re.sub(rf"(?<![\w.>])\b{v}\b(?=\s*(?:\[|[+-]\s))", f"((undefined1 *){v})", body)
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
    def prepare(self, roots: list[int]) -> None:
        """Plan once and rewrite every planned body once. The --fix rounds then only re-emit:
        a function that did not compile becomes a stub in place, the plan stays, and so do
        the files, the declarations and every other body."""
        print(f"planning from {len(roots)} roots ...", file=sys.stderr, flush=True)
        plan = self.plan(roots)
        print(f"plan: {len(plan['translate'])} to translate, {len(plan['adapt'])} adapted, "
              f"{len(plan['stub'])} stubbed", file=sys.stderr, flush=True)
        bodies: dict[int, str] = {}
        uses: set[int] = set()
        self._imports, self._dlls, self._port_reads = {}, {}, {}
        for a in sorted(plan["translate"]):
            text = self.body(a)
            mine: set[int] = set()
            try:
                body = self.rewrite(a, text, mine)
            except Untranslatable as e:
                del plan["translate"][a]
                plan["stub"][a] = str(e)
                continue
            uses |= mine
            bodies[a] = body
            self._imports[a] = {IMPORT_MERGED.get(m.group(2), m.group(2)) for m in IMPORT.finditer(text)}
            self._dlls[a] = {m.group(1) for m in IMPORT.finditer(text)}
            if r := self.port_reads(body, plan["adapt"]):
                self._port_reads[a] = r
        # whatever is called and neither translated nor adapted is stubbed
        for b in uses:
            if b not in plan["translate"] and b not in plan["adapt"] and b not in plan["stub"]:
                plan["stub"][b] = "called, not in the work set"
        self._plan, self._rewritten = plan, bodies
        # the files: fixed address ranges, PER_FILE corpus functions to a range, so a function's
        # file depends only on its address and a change touches only its own file (the old
        # numbering by position renumbered every file after the first change)
        every = sorted(set(self.cor) | set(bodies))
        self._bounds = every[::PER_FILE]
        print(f"rewrote {len(bodies)} bodies", file=sys.stderr, flush=True)

    def file_of(self, a: int) -> str:
        return f"translated_{max(0, bisect.bisect_right(self._bounds, a) - 1):03}.c"

    def emit(self, handlers: dict[int, str], demoted: dict[str, str]) -> dict:
        plan = {"translate": {a: d for a, d in self._plan["translate"].items() if f"{a:x}" not in demoted},
                "adapt": self._plan["adapt"], "stub": dict(self._plan["stub"])}
        for a in self._plan["translate"]:
            if f"{a:x}" in demoted:
                plan["stub"][a] = "did not compile: " + demoted[f"{a:x}"]
        GEN.mkdir(exist_ok=True)
        port_reads = {f"{a:x}": r for a, r in self._port_reads.items() if a in plan["translate"]}
        by_file: dict[str, list[int]] = defaultdict(list)
        for a in sorted(plan["translate"]):
            by_file[self.file_of(a)].append(a)
        lines_of = {}
        for name, funcs in sorted(by_file.items()):
            k = name[len("translated_"):-2]
            out = ["/* GENERATED by tools/translate.py from build/corpus.jsonl; do not edit by hand. */",
                   "#include \"gen/translated.h\"", "#pragma warning(disable: 4013 4024 4028 4047 4090 4133 4142 4189 4245 "
                   "4244 4267 4305 4306 4307 4310 4312 4389 4700 4701 4702 4703 4715 4716 4100 4101 4102 4146 "
                   "4018 4057 4098 4113 4152 4204 4459 4456 4457 4127 4334 4554 4805 4800 4996)",
                   # a call count per function (written out under FROST_HITS): which translated
                   # code a run reaches, and how often, for choosing what to port by hand
                   f"#define GEN_HITS gen_hits_{k}", f"unsigned int gen_hits_{k}[{len(funcs)}];", ""]
            for i, a in enumerate(funcs):
                start = len(out) + 1
                out.append(f"/* ref: FUN_{a:x} */")
                lines = self._rewritten[a].splitlines()
                brace = lines.index("{") if "{" in lines else -1
                if brace >= 0:
                    lines.insert(brace + 1, f"  GEN_HIT({i});")
                out.extend(lines)
                lines_of[a] = (name, start, len(out))
                out.append("")
            out += [f"const unsigned long long gen_hit_addrs_{k}[] = {{"] + [f"    0x{a:x}ULL," for a in funcs] + ["};", ""]
            write_out(GEN / name, "\n".join(out) + "\n", encoding="utf-8", newline="\n")
        for old in GEN.glob("translated_*.c"):
            if old.name not in by_file:
                old.unlink()
        self._files = set(by_file)
        # declared without parameters: Ghidra's argument counts disagree between a function and
        # its callers, and an unprototyped declaration takes them all
        # (a translated variadic function is declared as it is defined: C takes no unprototyped
        # declaration of a `...` definition; its callers still call through CALLF)
        # Everything in translated.h is the plan's, not this round's: a function stubbed for not
        # compiling keeps its declaration (a variadic one is declared, and stubbed, with its
        # parameters), and the imports are every planned body's, so a demotion leaves the header
        # as it was and the build recompiles only the files that changed
        def prototype(a: int) -> str:
            ret, params = self.decl(a)
            if a in self._plan["translate"] and params and params[-1] == "...":
                return f"{ret} FUN_{a:x}({', '.join(params)});"
            return f"{ret} FUN_{a:x}();"

        imports = set().union(*self._imports.values()) if self._imports else set()
        dlls = set().union(*self._dlls.values()) if self._dlls else set()
        prototypes = [prototype(a) for a in sorted(set(plan["translate"]) | set(plan["adapt"]) | set(plan["stub"]))]
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

        # each translated file's call counts (GEN_HIT, Ghidra.h) and their addresses
        hits = ["/* GENERATED by tools/translate.py; do not edit by hand. */",
                "/* every translated file's call count table (RefImage.cpp writes them out under FROST_HITS) */",
                "typedef struct { unsigned int *m_hits; const unsigned long long *m_addrs; unsigned int m_count; } GENHITS;"]
        for name, funcs in sorted(by_file.items()):
            k = name[len("translated_"):-2]
            hits += [f"extern unsigned int gen_hits_{k}[];", f"extern const unsigned long long gen_hit_addrs_{k}[];"]
        hits += ["const GENHITS gen_hit_tables[] = {"]
        hits += [f"    {{ gen_hits_{name[11:-2]}, gen_hit_addrs_{name[11:-2]}, {len(funcs)} }},"
                 for name, funcs in sorted(by_file.items())] or ["    { 0, 0, 0 },"]
        hits += ["};", f"const unsigned int gen_hit_table_count = {len(by_file)};", ""]
        write_out(GEN / "hits.c", "\n".join(hits), encoding="utf-8", newline="\n")

        # adapters to the port's own functions
        adapters = ["/* GENERATED by tools/translate.py; do not edit by hand. */", "#include \"gen/translated.h\"",
                    "#pragma warning(disable: 4047 4024 4312 4311 4302 4244 4305 4022 4113 4133 4142 4028)", ""]
        abi = {"int": "unsigned long long", "float": "float", "double": "double", "void": "void"}
        # an adapter takes the port function's own arguments (the callers are unprototyped, so a
        # float arrives promoted to double) and passes them on with the port's ABI
        promoted = {"int": "unsigned long long", "float": "double", "double": "double"}
        void_for_value = []
        for a in sorted(plan["adapt"]):
            va, mangled, (ret, args, this) = self.port_target(a)
            gret = self.decl(a)[0]
            # (a typed result only: Ghidra's `undefinedN` return is mostly whatever rax held)
            # an MSVC constructor (`??0`) returns `this` in rax, though the PDB types it void, and
            # the reference's callers use it: `new (pool) CSimpleFontString(...)` is the line
            # GameTooltip:AddLine names next. Returning 0 there made every new tooltip line null
            # (SetName faulted on it, the tooltip stayed empty and unsized), 2026-10-17
            ctor = this and mangled.startswith("??0")
            if ret == "void" and not ctor and gret.strip() != "void" and not gret.strip().startswith("undefined"):
                # the reference returns something the port does not, and the adapter returns 0 in
                # its place (string_format, FUN_140236510, returns its string: a caller read `*0`,
                # 2026-10-17): the port's signature is the one to fix
                void_for_value.append(f"{a:x} {gret.strip()} <- {mangled}")
            classes = (["int"] if this else []) + list(args)
            alias = f"gen_alias_{a:x}"
            params = [f"{promoted[c]} a{i}" for i, c in enumerate(classes)]
            pass_args = [f"({abi[c]})a{i}" for i, c in enumerate(classes)]
            if mangled.endswith("ZZ"):
                # a variadic port (`...` mangles as a trailing ZZ): the variadic arguments go on as
                # 8-byte integer slots up to VA_SLOTS in all. In Win64 a variadic callee reads them
                # from the home area and the stack, where an integer and a double (which a call
                # through no prototype also passes in the integer register) are the same 8 bytes;
                # a caller that passed fewer leaves its own frame there, which the format never
                # reads. Passing the fixed ones alone dropped every value of every SStrPrintf and
                # string_format from translated code (17 ports, 2026-10-17)
                tail = [f"unsigned long long v{i}" for i in range(VA_SLOTS - len(classes))]
                params += tail
                pass_args += [f"v{i}" for i in range(len(tail))]
                classes = classes + ["int"] * len(tail)
            fptr = f"{abi[ret]} (*)({', '.join(abi[c] for c in classes) or 'void'})"
            call = f"(({fptr})&{alias})({', '.join(pass_args)})"
            proto = f"{gret} FUN_{a:x}({', '.join(params) or 'void'})"
            if ctor and gret.strip() != "void":
                inner = f"    {call};\n    return ({gret})a0;"
            elif ret == "void":
                inner = f"    {call};\n    return 0;"
            elif ret == "int" and ("float" in gret or "double" in gret):
                inner = f"    return ({gret})(long long){call};"
            else:
                inner = f"    return ({gret}){call};"
            adapters += [f"#pragma comment(linker, \"/alternatename:{alias}={mangled}\")",
                         f"extern char {alias};", f"{proto} {{", inner, "}", ""]
        write_out(GEN / "adapters.c", "\n".join(adapters), encoding="utf-8", newline="\n")
        if void_for_value:
            print(f"ports returning void where the reference returns a value ({len(void_for_value)}; the "
                  "adapter returns 0, fix the port's signature):", file=sys.stderr)
            for line in void_for_value:
                print(f"  {line}", file=sys.stderr)

        # stubs
        stubs = ["/* GENERATED by tools/translate.py; do not edit by hand. */", "#include \"gen/translated.h\"",
                 "#pragma warning(disable: 4100 4047 4024 4716)", ""]
        for a, why in sorted(plan["stub"].items()):
            ret, params = self.decl(a)
            # (a planned variadic function is declared with its parameters: so is its stub)
            head = (f"{ret} FUN_{a:x}({', '.join(params)})" if a in self._plan["translate"] and params
                    and params[-1] == "..." else f"{ret} FUN_{a:x}()")
            stubs += [f"/* {why.replace('*/', '* /')} */", f"{head} {{", f"    GenUnimplemented(0x{a:x}ULL);",
                      f"    return {zero_of(ret)};", "}", ""]
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
                      f"        return {zero_of(ret)};",
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
        crt = [(a, name) for a, name in sorted(CRT_ENTRY.items()) if a not in set(reachable)]
        jumps += [f"    {{ 0x{a:x}ULL, (void*)&{name} }}," for a, name in crt]
        jumps += ["    { 0, 0 }", "};", f"const size_t g_genJumpCount = {len(reachable) + len(crt)};", ""]
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


def run_shown(cmd: list[str]) -> str:
    """Run a build step with its output streamed to the console as it comes, and return it."""
    print(f"> {' '.join(cmd)}", file=sys.stderr, flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    lines = []
    for line in proc.stdout:
        lines.append(line)
        sys.stderr.write(line)
        sys.stderr.flush()
    proc.wait()
    return "".join(lines)


def build_errors(reconfigure: bool = True) -> list[tuple[str, int, str]]:
    # the generated files are globbed: reconfigure when their set changed, or the old list builds
    if reconfigure:
        print("reconfiguring build/cmake ...", file=sys.stderr, flush=True)
        subprocess.run(["cmake", str(ROOT / "build/cmake")], capture_output=True)
    out = run_shown(["cmake", "--build", str(ROOT / "build/cmake"), "--config", "Release", "--target", "gen"])
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
    # it compiles: then link, and name the translated functions whose symbols do not resolve.
    # A handler demoted this round needs its stub first (gen_script_api.py writes the stubs for
    # the handlers neither ported nor translated), or the link fails on the registry instead.
    subprocess.run([sys.executable, str(ROOT / "tools/gen_script_api.py"), "--emit"], capture_output=True)
    out = run_shown(["cmake", "--build", str(ROOT / "build/cmake"), "--config", "Release", "--target", "FrostLite"])
    for m in re.finditer(r"unresolved external symbol (\S+) referenced in function FUN_([0-9a-f]{9})", out):
        errors.append(("LINK", int(m.group(2), 16), f"LNK2019 unresolved {m.group(1)}"))
    if not errors and re.search(r"\berror [A-Z]+\d+", out):
        # the port itself does not build, so the link that would show the translated
        # functions' unresolved symbols never ran: not clean, and nothing known to demote
        other = sorted({l.strip() for l in out.splitlines() if re.search(r"\berror [A-Z]+\d+", l)})
        raise SystemExit("the port does not build (fix it, then run again):\n" + "\n".join(other[:10]))
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
    ap.add_argument("--why", action="store_true",
                    help="list the translated functions that have a hand port, and why it is not used; stop")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.risks:
        return report_risks()
    if args.why:
        t = Translator(args.depth)
        manifest = json.loads((GEN / "manifest.json").read_text(encoding="utf-8"))
        rows = Counter()
        for k in sorted(manifest.get("translated", {})):
            reason = t.why_no_target(int(k, 16))
            if reason:
                print(f"FUN_{k}  {reason}")
                rows[re.sub(r"\(.*?\) ", "", re.sub(r"^`anonymous namespace'::", "", reason).split(" ", 1)[1])] += 1
        for reason, n in rows.most_common():
            print(f"{n:5}  {reason}", file=sys.stderr)
        return
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
    # One load and one plan for the whole run. A round only re-emits: what did not compile becomes
    # a stub in place and nothing else moves, so the next build recompiles the few files that
    # changed (it was a fresh load, plan and full rebuild per round, six of them, ~17 minutes)
    import time
    t0 = time.time()
    print("loading the corpus and the port's PDB ...", file=sys.stderr, flush=True)
    t = Translator(args.depth)
    t.prepare(sorted(set(roots) | set(t.static_initializers())))
    demoted = {} if args.fix else dict(t.demoted)
    files_before = {p.name for p in GEN.glob("translated_*.c")}
    for round_ in range(1, 30 if args.fix else 2):
        manifest = t.emit(handlers, demoted)
        print(f"round {round_} ({time.time() - t0:.0f}s): translated {len(manifest['translated'])}, "
              f"adapted {len(manifest['adapted'])}, stubbed {len(manifest['stubbed'])} "
              f"({len(manifest['handlers'])} handlers)", file=sys.stderr, flush=True)
        if not args.fix:
            break
        # the generated files are globbed: reconfigure only when their set changed
        errors = build_errors(reconfigure=t._files != files_before)
        files_before = set(t._files)
        if not errors:
            DEMOTED.write_text(json.dumps(demoted, indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
            print(f"builds clean ({time.time() - t0:.0f}s)", file=sys.stderr)
            break
        by_file = defaultdict(list)
        for a, info in manifest["translated"].items():
            by_file[info["file"]].append((info["lines"][0], info["lines"][1], a))
        hit = 0
        sources: dict[str, list[str]] = {}
        for file, line, message in errors:
            # (the project path MSBuild appends says nothing)
            message = re.sub(r"\s*\[[A-Z]:\\[^\]]*\]?$", "", message)
            if file == "LINK":
                if f"{line:x}" not in demoted:
                    demoted[f"{line:x}"] = message
                    hit += 1
                continue
            for start, end, a in by_file.get(file, []):
                if start <= line <= end and a not in demoted:
                    # with the line it points at, so the cause can be read without a rebuild
                    if file not in sources:
                        sources[file] = (GEN / file).read_text(encoding="utf-8", errors="replace").splitlines()
                    src = sources[file][line - 1].strip() if line - 1 < len(sources[file]) else ""
                    demoted[a] = f"{message} | {src[:200]}"
                    hit += 1
                    break
        DEMOTED.write_text(json.dumps(demoted, indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        print(f"round {round_}: {len(errors)} errors, {hit} functions demoted", file=sys.stderr)
        if hit == 0:
            print("errors outside translated bodies:", errors[:5], file=sys.stderr)
            break


if __name__ == "__main__":
    main()
