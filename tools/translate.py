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
import datetime
import gc
import functools
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
    "HKL", "HGDIOBJ", "LPWSADATA", "LPCANDIDATELIST", "LPCOMPOSITIONFORM", "LPCANDIDATEFORM",
    "PEXCEPTION_RECORD", "BCRYPT_ALG_HANDLE", "PSLIST_HEADER", "PBYTE", "PUCHAR", "LPWORD", "PUINT", "PCWSTR",
    "NTSTATUS", "wctype_t", "sockaddr", "timeval", "DATA_BLOB", "CRYPTPROTECT_PROMPTSTRUCT",
    # (SetThreadExecutionState's, winnt.h: FUN_14205dfb0, a pointer leaf, 2026-10-19)
    "EXECUTION_STATE",
}
PRIMITIVE |= WINDOWS_TYPES
# the C library's own structs and types, which src/gen/Ghidra.h names the way Ghidra writes them
PRIMITIVE |= {"tm", "FILE", "_onexit_t", "__time32_t", "__time64_t", "intptr_t", "uintptr_t", "time_t", "size_t", "mbstate_t"}
# Ghidra.h's 16-byte value (tools/v16.py), and its 32-byte storage (an AVX2 kernel's parameters)
PRIMITIVE |= {"gen_v16", "gen_v32"}
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
       "tolower", "toupper", "isdigit", "isalpha", "isspace", "atoi", "atof", "strtol", "strtoul", "free", "malloc",
       # (whose reference bodies are the CRT's templates and lambdas, not C: fflush's
       # `__crt_seh_guarded_call<int>::operator()<...>`, 2026-10-18)
       "fflush", "strtoull", "wcstol", "wcstoul", "raise"}
# and runtime routines by their exact name, the leading underscore part of it (`_close` is not
# `close`): the port's runtime has each under that name
CRT_EXACT = {"_close", "_commit", "_fstat64", "_wtol", "_strtoi64", "_fullpath", "_wfullpath"}
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
               0x14241a730: "fmod", 0x140282f8c: "frexp", 0x142417a9c: "acos",
               # FID-unnamed wrappers of the runtime's templates, each `return common_X(args, k)`
               # (their template a runtime internal, which game code reaching them left a stub,
               # 2026-10-18): common_fseek with a 32-bit offset, and common_exit with the full
               # cleanup and with none (the UCRT's _crt_exit_full_cleanup, _crt_exit_no_cleanup)
               0x140279378: "fseek", 0x14027258c: "exit", 0x140272544: "_exit",
               # and the rest game code calls, each read off its body (2026-10-18): translated,
               # they ran on the reference runtime's per-thread data and locks, which FrostLite
               # never sets up, through internals stubbed "called, not in the work set" (16).
               # rand: the 0x343fd/0x269ec3 step on __acrt_getptd's seed at +0x28
               0x142415c90: "rand",
               # localeconv: __acrt_getptd's locale info (+0x90) updated, its lconv (+0xf8)
               # returned (Lua's read_numeral reads the decimal point through it)
               0x14028ecc4: "localeconv",
               # _mktime64 and _mkgmtime64: `return common_mktime(tm, true / false)` (Lua::time)
               0x1424151f4: "_mktime64", 0x1424151ec: "_mkgmtime64",
               # wcstombs: `return _wcstombs_l_helper(dst, src, n, NULL)`
               0x142419228: "wcstombs",
               # _errno and __doserrno: __acrt_getptd_noexit's +0x20 and +0x24, or the globals
               # without one. FID calls both __doserrno (__doserrno_140283a54, _140283a34)
               0x140283a54: "_errno", 0x140283a34: "__doserrno",
               # _invalid_parameter_noinfo: `return _invalid_parameter(0, 0, 0, 0, 0)`, and the
               # noreturn form, which then calls _invoke_watson
               0x1402727ac: "_invalid_parameter_noinfo", 0x1402727cc: "_invalid_parameter_noinfo_noreturn",
               # strpbrk, the SSE one (tools/oracle.py: libc's answer in 8 cases, the empty set and
               # Lua's SPECIALS among them; not strstr). Its tail is a jump into a `pslldq xmm2`
               # ladder, which tools/protos.py read as an xmm2 input: a float third parameter, and
               # its callers (Lua's str_find_aux) passed `in_XMM2_Da`, 2026-10-18
               0x140272e20: "strpbrk",
               # the rest of the CRT's SSE string routines FID left unnamed, each identified under
               # tools/oracle.py against libc on byte and wide inputs (2026-10-20). Their bodies are
               # aligned 16-byte scans that mask the bytes before the string's start (`shl eax, cl`;
               # `and edx, eax`) and broadcast the character (`pshufd`); the decompiler drops both and
               # compares four lanes, so translated they matched bytes before the string and missed
               # ones after: curl's URL parser (strchr for the port's ':', strcspn for junk, strspn for
               # the host) rejected https://127.0.0.1:8081/... as CURLUE_BAD_PORT_NUMBER and the web
               # login never connected
               0x14026c578: "strchr", 0x14026c5f4: "strrchr", 0x14026c730: "strstr",
               0x14240fdc0: "strcspn", 0x14241c5a0: "strspn", 0x14240f9bc: "wcschr", 0x14240fa34: "wcsstr"}
# reference runtime entry points code stores as pointers, jumped to the port's own runtime: a
# call through the pointer reaches the same routine a direct call does. curl_global_init
# (FUN_14216a080) fills curl's allocator table with these; without them every curl allocation
# landed on int3 and returned null (the Login click's _calloc_base, 2026-10-15).
# Never translated, either: the reference's malloc and realloc allocate from its CRT heap handle
# (DAT_142e1eda8), which only its CRT startup creates and the port never runs. Planned once
# translated code called them directly, the jump went to the translated body, and HeapAlloc on a
# null heap faulted in ntdll on the Login click (FUN_140286974, 2026-10-17)
CRT_ENTRY = {0x140286974: "malloc", 0x140279d94: "free", 0x140289938: "realloc",
             0x14241d3f8: "_strdup", 0x1402857f8: "calloc", 0x14241c294: "_wcsdup",
             # the CRT's lazy resolver behind its time pointer (0x142cc8ac8): looks the precise
             # clock up in kernel32 on the first call and stores it. Translated it reads a FILETIME
             # through `->` (stubbed), and the stub filled no time at all (2026-10-17)
             0x140266b10: "GetSystemTimePreciseAsFileTime",
             # FID's Global::_endthreadex: its body is common_end_thread, which CRT_TEMPLATES
             # names _endthreadex, which resolved back to this function: translated it called
             # itself until the stack ran out (C4717, 2026-10-17)
             0x140272d6c: "_endthreadex",
             # _msize_base: HeapSize on the reference CRT heap handle, as malloc's (2026-10-18)
             0x140295920: "_msize"}
# (the CRT_ENTRY routines that are Windows APIs, declared as imports in translated.h)
CRT_ENTRY_IMPORTS = {"GetSystemTimePreciseAsFileTime"}
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
                 # (inlined into game code: FUN_142284280 enumerating a folder, 2026-10-18)
                 "common_find_first_wide<_wfinddata64i32_t>": "_wfindfirst64i32",
                 "common_find_next_wide<_wfinddata64i32_t>": "_wfindnext64i32",
                 "common_vsprintf<__crt_stdio_output::standard_base,wchar_t>": "__stdio_common_vswprintf",
                 "common_lseek_nolock<__int64>": "_lseeki64_nolock", "common_lseek_nolock<long>": "_lseek_nolock",
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
    out = F.pdbutil("-publics")
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
    types = F.pdbutil("-types").splitlines()
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

    syms = F.pdbutil("-symbols").splitlines()
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
# (the local itself, not a store through it: `*pfVar34 = _DAT_14297aad0;` writes the float the
# global holds through a float pointer, and read as a copy into pfVar34 it typed the global
# `float *`, C2440 in FUN_14247b930 and FUN_14249eec0, 2026-10-18)
GLOBAL_COPY = re.compile(r"(?<![\w*.>\]])(\w+) = (?:\([^()]*\))?_?DAT_([0-9a-f]{9});|"
                         r"(?<![\w*.>\]&])_?DAT_([0-9a-f]{9}) = (?:\([^()]*\))?(\w+);")
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
# (`StackY_` is entry-relative too, below the frame Ghidra sized: the stack pointer after a
# __chkstk, `puVar26 = auStackY_988;` and the locals at `puVar26 + 0x60`. Off the frame it was an
# 8-byte array of its own and those stores overran it, C4789 in FUN_1412fd5c0, 2026-10-18)
STACK_DECL = re.compile(r"^(\s*)([A-Za-z_][\w ]*?)\s*(\**)\s*((?:local_|[a-z]+Stack_|[a-z]+StackX_|[a-z]+StackY_)(res)?([0-9a-f]+))"
                        r"\s*(?:\[\s*(\w+)\s*\])?\s*;[^\n]*\n", re.M)


# the C runtime routines translated code calls with Ghidra's 64-bit integers where they take
# pointers, by the (1-based) positions of those parameters
CRT_POINTER_PARAMS = {"memmove": (1, 2), "memcpy": (1, 2), "memset": (1,), "memcmp": (1, 2), "free": (1,),
                      "_free_base": (1,), "realloc": (1,), "_realloc_base": (1,), "qsort": (1,),
                      "fwrite": (1, 4), "fread": (1, 4), "strlen": (1,), "strcmp": (1, 2), "strcpy": (1, 2),
                      # (FUN_140291ca8 hands _msize the block as an integer: C4022 at 3 sites)
                      "_msize": (1,), "strpbrk": (1, 2), "wcstombs": (1, 2),
                      "strchr": (1,), "strrchr": (1,), "strstr": (1, 2), "strspn": (1, 2), "strcspn": (1, 2),
                      "wcschr": (1,), "wcsstr": (1, 2)}
INT32_TYPES = r"(?:int|uint|undefined4|int3|uint3|undefined3|ushort|short|undefined2|byte|char|undefined1|uchar|word|dword)"


def call_args(body: str, start: int) -> tuple[list[tuple[int, int]], int]:
    """The (start, end) of each argument of the call whose `(` is at `start`, and the `)` after it."""
    depth, k, spans, s = 1, start + 1, [], start + 1
    while k < len(body) and depth:
        ch = body[k]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
            if depth == 0:
                break
        elif ch == "," and depth == 1:
            spans.append((s, k))
            s = k + 1
        elif ch == '"':
            k += 1
            while k < len(body) and body[k] != '"':
                k += 2 if body[k] == "\\" else 1
        k += 1
    spans.append((s, k))
    return spans, k


def explicit_casts(body: str, params: list[str]) -> str:
    """Conversions the reference makes and C only warns about, written out so a build log shows
    what is left. Each is the same value as before; nothing here changes what runs:
    - a 64-bit integer handed to a C runtime pointer parameter (`memmove(lVar1, ...)`, C4022 at
      1,728 sites): cast to `void *`;
    - a pointer read as 32 bits (`return (int)puVar6;`, `uVar6 = (uint)puVar7;`: the reference
      reading eax of a register that held a pointer, C4311 at 1,737 sites): through `longlong`;
    - a bool negated (`-bVar8 & 2U`, C4804): as the int C promotes it to."""
    # C4022
    out, i = [], 0
    for m in re.finditer(r"(?<![\w.>])(" + "|".join(CRT_POINTER_PARAMS) + r")\(", body):
        if m.start() < i:
            continue
        spans, close = call_args(body, m.end() - 1)
        pieces, at = [body[i:m.end()]], m.end()
        for n, (s, e) in enumerate(spans, 1):
            arg = body[s:e]
            pieces.append(body[at:s])
            pieces.append(f"(void *)({arg.strip()})" if n in CRT_POINTER_PARAMS[m.group(1)] and arg.strip() else arg)
            at = e
        out += pieces
        i = at
    out.append(body[i:])
    body = "".join(out)
    # C4311: the pointer-typed names (locals, parameters), then a 32-bit cast of one or of a cast
    # to a pointer
    # (initialized ones too, `undefined1 *gen_rsp = (undefined1 *)_AddressOfReturnAddress();`, and
    # pointers to arrays, `undefined1 (*pauVar23) [32];`: C4311 in FUN_1413444e0, FUN_1401a9730)
    pointers = set(re.findall(r"^\s*(?:const\s+)?\w+\s*\*+\s*(\w+)\s*(?:=[^;\n]*)?;", body, re.M)) \
        | set(re.findall(r"^\s*(?:const\s+)?\w+\s*\(\s*\*+\s*(\w+)\s*\)\s*\[\w+\]\s*(?:=[^;\n]*)?;", body, re.M)) \
        | {m.group(1) or m.group(2) for p in params
           if (m := re.search(r"\*\s*(\w+)\s*$|\(\s*\*+\s*(\w+)\s*\)\s*\[\w+\]\s*$", p))}
    if pointers:
        names = "|".join(map(re.escape, sorted(pointers, key=len, reverse=True)))
        body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=(?:{names})\b(?!\s*(?:\[|->|\.)))", r"\1(longlong)", body)
        # (pointer arithmetic on one: `(int)(pbVar13 + 1)`)
        body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=\((?:{names})\s*[+-])", r"\1(longlong)", body)
    # (a cast to a pointer, `(uint)(char *)x`; a pointer made from the frame, `(int)((byte *)(...))`;
    # an address, `(int)&x`; a pointer read through a pointer to it, `(uint)(*(undefined ** *)(...))`)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=\((?:const\s+)?\w+\s*\*+\s*\))", r"\1(longlong)", body)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=\(\((?:const\s+)?\w+(?:\s*\*)+\s*\)\()", r"\1(longlong)", body)
    # (a pointer read through a pointer to it, then offset: `(int)((*(undefined1 * *)(...)) + 8)`)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=\(\(\*\((?:const\s+)?\w+(?:\s*\*){{2,}}\s*\)\()", r"\1(longlong)", body)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=&)", r"\1(longlong)", body)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=REFG\((?:const\s+)?\w+\s*\*+\s*,)", r"\1(longlong)", body)
    # (and unparenthesized, `(int)*(char **)(param_2 + 0x10)`)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=\*\((?:const\s+)?\w+(?:\s*\*){{2,}}\s*\))", r"\1(longlong)", body)
    body = re.sub(rf"(\({INT32_TYPES}\))\s*(?=\(\*\((?:const\s+)?\w+(?:\s*\*){{2,}}\s*\)\()", r"\1(longlong)", body)
    # C4804
    for name in re.findall(r"^\s*bool\s+(\w+)\s*;", body, re.M):
        body = re.sub(rf"(?<![\w)\]])-\s*{name}\b", f"-(int){name}", body)
    return body


DECL_KEYWORDS = {"return", "goto", "break", "continue", "case", "default", "else", "do", "extern", "static", "typedef"}


# the runtime routines the port calls instead (named_callee's strings) that return nothing: a tail
# call to one carries no result
VOID_RUNTIME = {"free", "_free_base", "_endthreadex", "_exit", "exit", "abort", "qsort", "srand",
                "_invalid_parameter_noinfo", "_invalid_parameter_noinfo_noreturn",
                "_Init_thread_header", "_Init_thread_footer", "_Init_thread_abort", "_CxxThrowException",
                "terminate", "_lock", "_unlock"}

TAIL_CALL_STMT = re.compile(r"^([ \t]*)((?:thunk_)?FUN_[0-9a-f]{9}\([^;]*\)|[A-Za-z_]\w*(?:::~?\w+)+\([^;]*\)"
                            r"|\(\*+\(code \*+\)[^;]*\)\([^;]*\));", re.M)


def _block_open(body: str, close: int) -> int | None:
    """The `{` a `}` at `close` closes (strings and comments do not hold braces in Ghidra's C
    often enough to matter, but string literals are skipped)."""
    depth, i, in_str = 0, close, False
    while i >= 0:
        c = body[i]
        if c == '"' and (i == 0 or body[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str:
            if c == "}":
                depth += 1
            elif c == "{":
                depth -= 1
                if depth == 0:
                    return i
        i -= 1
    return None


def _block_close(body: str, open_: int) -> int | None:
    depth, i, in_str = 0, open_, False
    while i < len(body):
        c = body[i]
        if c == '"' and body[i - 1] != "\\":
            in_str = not in_str
        elif not in_str:
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return None


def carry_tail_calls(body: str, returns_value=lambda call: True) -> str:
    """A function Ghidra calls void because it ends in a tail call hands its callee's result back
    in rax: each call statement from which control goes straight to a `return;` or the end of the
    function returns that call's value. Straight means through nothing but the ends of if and
    else blocks (an `else` block after one is skipped, the branch not taken) and labels; the end
    of a loop or a switch goes elsewhere, and any other statement may change rax."""
    ends = body.rstrip()
    fn_end = len(ends) - 1 if ends.endswith("}") else None
    edits = []
    for m in TAIL_CALL_STMT.finditer(body):
        # (the text before, less the comments between: Ghidra's `/* WARNING: Treating indirect
        # jump as call */` above a tail jump through a pointer, FUN_14031ef60, FUN_14057d1b0)
        before = body[:m.start()].rstrip()
        # (not a call that does not return, `_endthreadex(0)` under Ghidra's warning: there is no
        # result, and `return (undefined8)` of a void one was C2069 in FUN_1415aed50; nor one to
        # a function the translation declares void)
        if re.search(r"/\* WARNING: Subroutine does not return \*/\s*$", before) or not returns_value(m.group(2)):
            continue
        while before.endswith("*/") and "/*" in before:
            before = before[:before.rfind("/*")].rstrip()
        if before and before[-1] not in ";{}:":
            continue    # (the right side of an assignment begun on the line above)
        i = m.end()
        tail = False
        while True:
            # (whitespace, comments and labels pass)
            n = re.match(r"(?:\s+|/\*.*?\*/|//[^\n]*|\b(?:LAB|code_r0x)\w*:(?!:))*", body[i:], re.S)
            i += n.end()
            if body.startswith("return", i) and re.match(r"return\s*;", body[i:]):
                tail = True
                break
            if i >= len(body) or body[i] != "}":
                break
            if fn_end is not None and i == fn_end:
                tail = True
                break
            o = _block_open(body, i)
            if o is None:
                break
            opener = body[body.rfind("\n", 0, o) + 1:o]
            if not re.match(r"\s*(?:\}\s*)?(?:else\b|if\s*\()", opener) or re.match(r"\s*do\b", opener):
                break
            i += 1
            # (the branch not taken: an `else` or `else if` block after this one)
            while True:
                e = re.match(r"\s*else\b(?:\s*if\s*\([^{]*\))?\s*\{", body[i:])
                if not e:
                    break
                c = _block_close(body, i + e.end() - 1)
                if c is None:
                    break
                i = c + 1
        if tail:
            edits.append((m.start(), m.end(), f"{m.group(1)}return (undefined8){m.group(2)};"))
    for s, e, text in reversed(edits):
        body = body[:s] + text + body[e:]
    return body


NON_SCALAR_RETURNS = {"float", "double", "longdouble", "gen_v16", "gen_v32", "tm", "FILE", "mbstate_t"}


def fall_off_return(body: str, ret: str) -> str:
    """A non-void function whose last top-level statement is no return: a path where the
    reference reaches its `ret` with whatever RAX held.

    Ghidra drops the value when nothing it tracks wrote RAX on that path: the `test; je ret`
    before a tail jump through a pointer (FUN_141f80d10, FUN_14228c9b0), a stack-cookie compare
    whose failure call was stripped (FUN_1402bd150), an anti-tamper guard removed whole
    (FUN_14029fae0). C4715/C4716 in the 2026-10-18 build, and an undefined return value as C.
    RAX's last known value is what the reference returns (gen_rax, where the translation keeps
    it), otherwise zero, as zero_locals gives the decompiler's other unwritten values."""
    lines = body.rstrip().split("\n")
    if not lines or lines[-1].strip() != "}":
        return body
    last = None
    for l in reversed(lines[:-1]):
        s = l.strip()
        if not s or s.startswith("/*") or s.startswith("*") or s.startswith("//"):
            continue
        indent = len(l) - len(l.lstrip(" "))
        if indent <= 2:
            last = l
            break
    if last is not None and re.match(r"\s{2}(?:return\b|__assume\(0\);)", last):
        return body
    if ret not in NON_SCALAR_RETURNS and "*" not in ret and re.search(r"\bulonglong gen_rax\b", body):
        value = f"({ret})gen_rax"
    elif "*" in ret:
        value = f"({ret})0"
    else:
        value = None
    tail = f"  return {value};\n" if value else f"  {{\n    {ret} gen_fall = {{0}};\n    return gen_fall;\n  }}\n"
    return "\n".join(lines[:-1]) + "\n" + tail + "}\n"


def zero_locals(body: str) -> str:
    """Ghidra's scalar locals start at zero instead of whatever the stack held.

    The reads the compiler flags as uninitialized (C4700/C4701/C4703, 173 sites in 2026-10-17's
    build) are the reference's own: a stack slot it never writes either (FUN_1401fd160 copies a
    16-byte pair whose upper half nothing stores, `movups xmm0, [rsp+0x30]`), or outgoing stack
    arguments stored on one path and read after the paths join. The reference reads garbage
    there; zero is the one value that makes a run repeatable. (A register the decompiler lost is
    another matter, refused before this as "a register the decompiler lost track of".)"""
    # (the block runs from the first declaration to the blank line after the declarations: a
    # prelude can come first, `{  longlong *gen_param_1 = (longlong *)param_1;` and a blank line,
    # which ended the block before FUN_141a45040's locals and left C4703 on them)
    decl = re.compile(r"(\s+)((?:const\s+)?\w+(?:\s+\w+)*?(?:\s*\*+\s*|\s+))(\w+);")
    lines = body.split("\n")
    out, started, done = [], False, False
    for i, l in enumerate(lines):
        if i == 0 or done:
            out.append(l)
            continue
        if not l.strip():
            done = started
            out.append(l)
            continue
        m = decl.fullmatch(l)
        if m and m.group(2).split()[0] not in DECL_KEYWORDS and m.group(3) != "gen_frame" and "GEN_HIT" not in l:
            started = True
            out.append(f"{m.group(1)}{m.group(2)}{m.group(3)} = {{0}};")
        else:
            out.append(l)
    return "\n".join(out)


NARROW_TYPES = {"char", "uchar", "byte", "bool", "undefined", "undefined1", "short", "ushort", "undefined2", "word",
                "int", "uint", "undefined4", "dword", "float", "long", "ulong", "wchar_t", "wchar16"}


def param_slots(body: str, params: list[str] | None) -> str:
    """A parameter narrower than 8 bytes whose address the body takes lives in an 8-byte slot.

    In the reference every argument has a whole 8-byte slot (its register's home, or its place
    among the stack arguments), and Ghidra uses the slot as storage: `_param_5 = CONCAT11(...)`,
    `(*(undefined8 *)&param_5) = ...` here. C's parameter is only its type's size, so the store ran
    past it into the frame (C4739/C4789 in FUN_14030d360, FUN_140311510, FUN_14120bd80 and four
    more, 2026-10-17). The slot starts as the argument; the parameter is read and written in it."""
    seeds, out = [], body
    for p in params or []:
        m = re.fullmatch(r"\s*((?:unsigned |signed |const )*\w+)\s+(param_\d+)\s*", p)
        if not m or m.group(1).split()[-1] not in NARROW_TYPES or not re.search(rf"&\s*{m.group(2)}\b", body):
            continue
        t, n = m.group(1), m.group(2)
        out = re.sub(rf"\b{n}\b", f"(*({t} *)&gen_slot_{n})", out)
        # (but va_start names the parameter itself: the variable arguments are found from its
        # address. Given the slot, FUN_141a4d260 read them from the slot's neighbours, C5082)
        out = out.replace(f"va_start(gen_va, (*({t} *)&gen_slot_{n}))", f"va_start(gen_va, {n})")
        seeds.append(f"  unsigned long long gen_slot_{n} = 0;\n  memcpy(&gen_slot_{n}, &{n}, sizeof({n}));\n")
    if not seeds:
        return body
    first = re.search(r"\{\n", out).end()
    return out[:first] + "".join(seeds) + out[first:]


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
        # no local named, and an address at or above the entry stack pointer (`&stack0x00000008`,
        # the home slot handed to a callback, FUN_1415b2250; `(longlong)&stack0x00000000`, its
        # value hashed): the real stack's, where the return address is (C2065 in 4 functions,
        # 2026-10-17). Below it is a frame no local maps, which stays unresolved.
        return re.sub(r"&stack0x(0{8}[0-9a-f]{0,8}|[0-7][0-9a-f]{0,7})\b",
                      lambda x: f"((undefined1 *)_AddressOfReturnAddress() + 0x{int(x.group(1), 16):x})", body)
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
        # (a `char` array as well: `char local_38 [8]` assigned a string's first 8 bytes, C2106)
        if m.group(7) in ("2", "4", "8") and not m.group(3) and m.group(2).strip() in ("undefined1", "undefined", "char") \
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
    # (zeroed, as zero_locals() does a scalar's: what the function reads of its frame before
    # writing it is the reference's garbage too, C4700 at 50 sites on 2026-10-17)
    frame = (f"{decls[0].group(1)}__declspec(align(16)) unsigned char gen_frame[{size}] = {{0}};\n")
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
# (any whitespace between the tokens: Ghidra wraps a long condition anywhere, and a wrapped
# `*(longlong *)\n ((longlong)ThreadLocalStoragePointer` was missed, leaving that static's guard
# against the image's TLS template, so EnterWorld's "gameTip" CVar was never looked up, 2026-10-21)
# (the TLS array or the index perhaps held in a variable first: `*(int *)(*(longlong *)((longlong)
# pvVar2 + uVar8 * 8) + 0x2140)`, FUN_141c818c0's statics, 2026-10-21; a slot of a pointer array
# at 0x2140 is the epoch and nothing else)
INIT_EPOCH = re.compile(r"\*\s*\(int \*\)\s*\(\s*\*\s*\(longlong \*\)\s*\(\s*\(longlong\)\s*(?:ThreadLocalStoragePointer|pvVar\d+)\s*\+\s*"
                        r"(?:\(ulonglong\)\s*_tls_index|uVar\d+)\s*\*\s*8\s*\)\s*\+\s*0x2140\s*\)")
TLS_BLOCK = (r"\*\s*\(longlong \*\)\s*\(\s*\(longlong\)\s*ThreadLocalStoragePointer\s*\+\s*\(ulonglong\)\s*_tls_index"
             r"\s*\*\s*8\s*\)")
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
    # (and what a call left in a register, read only for the bytes above a low part the caller
    # writes itself: `CONCAT71((int7)((ulonglong)extraout_RDX >> 8),1)`, `setg dl` before the
    # next call, whose callee spills all of rdx and reads dl. The caller wrote only the byte:
    # what lies above it is nothing the compiler kept, whatever the call before left there
    # (14138db20, 141863530, 141874be0, 141cfebf0, 14179cc90, 1412a7350, 2026-10-18). Read whole,
    # as an argument or a value, it stays refused)
    left = set(re.findall(r"\bextraout_(?:R[A-D]X|R[SD]I|RBP|R(?:8|9|1[0-5])|E[A-D]X|E[SD]I|R(?:8|9|1[0-5])D)(?:_\d+)?\b",
                          text))
    for name in upper | set(ARG_REGISTER.findall(text)) | saved | left:
        uses = list(re.finditer(rf"\b{name}\b", text))
        decl = [u for u in uses if re.match(rf"[ \t]*[\w ]+\**\s*{name};$", text[text.rfind("\n", 0, u.start()) + 1:
                                                                               text.find("\n", u.end())])]
        rest = [u for u in uses if u not in decl]
        # (a plain store of a value to it reads nothing: `in_register_00000014 = 0;`, what
        # leftovers() left of `in_register_00000014 = extraout_var;` after a call, the bytes above
        # the edx it gave a virtual call. Taken for a read, FUN_14058a4a0 and two copies of it stayed
        # refused, 2026-10-18. Such statements go with the variable)
        stores = [u for u in rest if re.fullmatch(r"[ \t]*", text[text.rfind("\n", 0, u.start()) + 1:u.start()])
                  # (zero only: the value every read becomes, so nothing it stored is lost; or
                  # `extraout_var`, the bytes of a call's result above it, which rax_upper_parts()
                  # makes that zero afterwards)
                  and re.match(r"\s*=\s*(?:\(\w+\))?\s*(?:0|extraout_var(?:_\d+)?);[ \t]*$",
                               text[u.end():text.find("\n", u.end())])]
        rest = [u for u in rest if u not in stores]
        if name in upper:
            ok = all(re.search(r"CONCAT\d+\(\s*(?:\(\w+\))?\s*$", text[max(0, u.start() - 24):u.start()])
                     # (the high bytes above any low part: `<< 0x20`, `<< 0x10`, `<< 8`, FUN_14138f9d0)
                     or (name.startswith("in_stack_") and re.search(r"\(ulonglong\)\s*$", text[max(0, u.start() - 12):u.start()])
                         and re.match(r"\s*<<\s*(?:0x[12]0|8)\b", text[u.end():u.end() + 10])) for u in rest)
        else:
            # (or only its upper bits, `(int)((ulonglong)in_R9 >> 0x20)`: the half of a home slot
            # the function wrote 4 bytes of and read 8, which Ghidra takes for the register's)
            # (or the bytes above what the call is given, as a CONCAT's high part:
            # `CONCAT31((int3)((uint)in_R8D >> 8),0x24)`, r8b set and the rest left, FUN_141860a50)
            ok = all((name not in left and _is_call_argument(text, u.start(), u.end()))
                     # (the upper bytes as a value of their own: `(undefined7)((ulonglong)extraout_RDX
                     # >> 8)`, which a CONCAT puts back above a byte further on)
                     or (name in left and re.search(r"\(undefined[67]\)\s*\(\(ulonglong\)\s*$",
                                                    text[max(0, u.start() - 28):u.start()])
                         and re.match(r"\s*>>\s*(?:8|0x8|0x10)\)", text[u.end():u.end() + 12]))
                     or (name not in left and re.search(r"\(\(ulonglong\)\s*$", text[max(0, u.start() - 16):u.start()])
                         and re.match(r"\s*>>\s*0x[2-3][0-9a-f]\)", text[u.end():u.end() + 12]))
                     or (name not in left and re.search(r"CONCAT\d+\((?:\(\w+\)|\()*$", text[max(0, u.start() - 40):u.start()])
                         and re.match(r"(?:\s*>>\s*(?:0x)?[0-9a-f]+)?\)*,", text[u.end():u.end() + 16]))
                     # (a call's leftover only above a byte or a word the caller writes itself, which
                     # does not extend: a 4-byte write zero-extends, and an upper half read whole can
                     # be a second result the whole-program build hands back in rdx)
                     or (name in left and re.search(r"CONCAT(?:71|62|31|22|11)\((?:\(\w+\)|\()*$",
                                                    text[max(0, u.start() - 40):u.start()])
                         and re.match(r"\s*>>\s*(?:8|0x8|0x10)\)*,", text[u.end():u.end() + 16]))
                     # (or with its low bytes masked off, the bytes above a byte the function writes
                     # into a callee-saved register it borrows: `uVar12 = unaff_R15 &
                     # 0xffffffffffffff00;` then the byte or'd in, FUN_141225d80; `unaff_ESI &
                     # 0xffffff00`, FUN_141ae8460. Above the byte is the caller's, 2026-10-18)
                     # (a call's leftover too: `uVar21 = extraout_RCX & 0xffffffffffffff00;` before a
                     # byte goes into it, FUN_1418555d0)
                     or ((name in saved or name in left)
                         and re.match(r"\s*&\s*0x(?:ffffffffffffff|ffffff)00\b", text[u.end():u.end() + 24]))
                     for u in rest)
        if rest and ok:
            for u in stores:
                # (blanked to the same length, so the offsets of the other uses hold)
                line_start = text.rfind("\n", 0, u.start()) + 1
                line_end = text.find("\n", u.end())
                n = line_end - line_start
                text = text[:line_start] + ("/*" + " " * (n - 4) + "*/" if n >= 4 else " " * n) + text[line_end:]
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
    # the block in a variable that is used for other things too, the index perhaps copied first
    # (`uVar8 = (ulonglong)_tls_index; lVar3 = *(longlong *)((longlong)ThreadLocalStoragePointer
    # + uVar8 * 8);`, FUN_141c818c0, whose statics' guards then read the image's TLS template and
    # stayed unbuilt: the world's script setup called a null vtable, 2026-10-21): each epoch read
    # through it, up to its next assignment, is the port's epoch
    load = re.compile(r"^\s*(\w+) = \*\(longlong \*\)\(\(longlong\)(?:ThreadLocalStoragePointer|pvVar\d+) \+ "
                      r"(?:\(ulonglong\)_tls_index|\w+) \* 8\);$", re.M)
    out, at = [], 0
    for m in load.finditer(text):
        if m.start() < at:
            continue    # (inside the previous load's span: that one's variable was rewritten there)
        var = m.group(1)
        nxt = re.compile(rf"^\s*{var} = ", re.M).search(text, m.end())
        end = nxt.start() if nxt else len(text)
        out.append(text[at:m.end()])
        out.append(re.sub(rf"\*\(int \*\)\({var} \+ 0x2140\)", "_Init_thread_epoch", text[m.end():end]))
        at = end
    out.append(text[at:])
    return "".join(out)


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
        base = sym if re.match(r"(?:s|u|ws)_", sym) else f"&{sym}"
        if size not in PIECE_TYPE:
            # an odd width (`CONCAT71(DAT_142d37968._1_7_,(byte)DAT_142d37968)`: a list head
            # pointer read in two pieces, the global typed as a byte): the 8 bytes at the
            # global's address, shifted and masked. The local rule below shifted the global's
            # declared byte instead, so the pointer kept its low byte only and the world-entry
            # teardown (FUN_141c9d620) read 0x78, 2026-10-21 (17 sites)
            if off + size > 8:
                return m.group(0)
            return (f"(((*(ulonglong *)((char *){base})) >> {8 * off}) & "
                    f"{(1 << (8 * size)) - 1:#x}ULL)")
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
INDIRECT_CALL = re.compile(r"\((?:\*\*\(code \*\*\)|\*\(code \*\*?\)|\*\(\*\(code \*\*?\)|\*REFG\(code \*,)")
# a top-level operator that makes an expression something other than its operands' type
NOT_FLOAT_OP = re.compile(r"<|(?<!-)>|[!=]=|&&|\|\||\?|%|\^|(?<![&])&(?![&])|\|")

# a frame slot Ghidra typed float, as stack_frame() spells it
FLOAT_SLOT = re.compile(r"\(\(float \*\)\(gen_frame \+ (0x[0-9a-f]+)\)\)")
# an integer stored into one: `= (float)(uint)*pbVar4;`, nothing but the cast of an integer
INT_INTO_FLOAT_SLOT = re.compile(
    r"\(\(float \*\)\(gen_frame \+ (0x[0-9a-f]+)\)\)\[0\] = \(float\)\((?:uint|int|ushort|short|byte|char|uchar)\)"
    r"([\w*()\[\] +\-]+);")


INT_CAST_TYPES = r"(?:unsigned long long|long long|ulonglong|longlong|uint|int|ushort|short|byte|char|uchar|" \
                 r"undefined[1248]?|ulong|long)"


def _cast_operand_end(s: str, i: int) -> int:
    """Where the operand of a cast starting at s[i] ends: unary operators and further casts,
    then an identifier, number or parenthesized group, then subscripts, calls and members."""
    n = len(s)
    while i < n and s[i] in " -~!*&":
        i += 1
    while i < n and s[i] == "(":
        m = re.match(r"\((?:\w+ ?)+\**\)", s[i:])
        if m and not re.match(r"\((?:\w+)\)\s*[;,)\]]", s[i:]):
            i += m.end()
            while i < n and s[i] in " -~!*&":
                i += 1
            continue
        break
    if i < n and s[i] == "(":
        depth = 0
        while i < n:
            depth += s[i] == "("
            depth -= s[i] == ")"
            i += 1
            if depth == 0:
                break
    else:
        m = re.match(r"[\w.]+", s[i:])
        i += m.end() if m else 0
    while i < n and (s[i] in "[(" or s.startswith("->", i) or s[i] == "."):
        if s.startswith("->", i):
            i += 2 + len(re.match(r"\w*", s[i + 2:]).group())
            continue
        if s[i] == ".":
            i += 1 + len(re.match(r"\w*", s[i + 1:]).group())
            continue
        close = {"[": "]", "(": ")"}[s[i]]
        depth = 0
        while i < n:
            depth += s[i] in "(["
            depth -= s[i] in ")]"
            i += 1
            if depth == 0:
                break
    return i


def reinterpreted_float_locals(body: str) -> str:
    """In a function with no integer/float conversion instruction, a float or double local the
    decompiler casts to an integer, or assigns an integer cast to a double, is one register
    holding both kinds: the integer is its bits. curl's formatter FUN_142190e40 keeps every
    argument in one union and Ghidra merged the integer and the %f paths into `double dVar13`;
    `(ulonglong)dVar13 % 10` converted the denormal that 8081's bits make to 0, every number
    printed as "0", the web login's port with them, and curl connected to port 0 (2026-10-20)."""
    kinds = {m.group(2): m.group(1) for m in re.finditer(r"^\s*(double|float) (\w+)(?: = \{0\})?;$", body, re.M)}
    if not kinds:
        return body
    names = "|".join(map(re.escape, kinds))
    # an integer view of the local: its bits
    body = re.sub(rf"\(({INT_CAST_TYPES})\)\s*\b({names})\b(?!\s*[\[(])",
                  lambda m: f"({m.group(1)})GEN_BITS({m.group(2)})", body)
    # a value cast to the local's kind in a store to it: the bits it is made of
    out, i = [], 0
    for m in re.finditer(rf"\b({names}) = \((double|float)\)", body):
        if m.start() < i or kinds[m.group(1)] != m.group(2):
            continue
        end = _cast_operand_end(body, m.end())
        operand = body[m.end():end]
        # (only an integer value: a float widened to a double is cvtss2sd, a conversion this
        # test does not look for, and stays a conversion)
        if not re.search(rf"GEN_BITS\(|\({INT_CAST_TYPES}\)", operand):
            continue
        conv = "gen_bits2d((unsigned long long)" if m.group(2) == "double" else "gen_bits2f((unsigned int)"
        out.append(body[i:m.start()] + f"{m.group(1)} = {conv}({operand}))")
        i = end
    out.append(body[i:])
    return "".join(out)


def stack_allocations(body: str) -> str:
    """The compiler's `alloca` (`sub rsp, size` after __chkstk) as the decompiler shows it: a
    frame address less a 16-aligned size (`(gen_frame + 0x30) + -(n + 0xf & ~0xf)`), the block
    below the frame. In C that address is whatever stack the next call uses: JamJSON's zlib
    reader (FUN_142582120) inflated the realm list into it and broke FUN_14235aad0's stack cookie
    (2026-10-20). Each becomes one _alloca of that size (with room either side for the pushes
    the decompiler shows below it), at the same offset from the block as from the frame."""
    if "& 0xfffffffffffffff0)" not in body or "gen_frame" not in body:
        return body
    sizes = set()

    def fix(m):
        sizes.add(m.group(2))
        return f"(gen_alloca_p + {m.group(1)})"

    # (the whole address expression: `((undefined1 *)(gen_frame + 0x0)) + -(...)`, or under a
    # (longlong) cast with an element pointer cast inside)
    out = re.sub(r"\((?:\(longlong\))?\((?:\((?:[\w ]+?\s*\*+)\))?\(gen_frame \+ (0x[0-9a-f]+)\)\)\s*\+\s*"
                 r"-\(([\w.>\[\]-]+) & 0xfffffffffffffff0\)\)", lambda m: f"({fix(m)})", body)
    out = re.sub(r"\(\((?:[\w ]+?\s*\*+)\)\(gen_frame \+ (0x[0-9a-f]+)\)\)\s*\+\s*"
                 r"-\(([\w.>\[\]-]+) & 0xfffffffffffffff0\)", fix, out)
    if len(sizes) != 1:
        return body          # (two sizes: not one block; leave it to fail visibly)
    size = sizes.pop()
    # one block, made where the size is known: just before the first use
    first = out.index("(gen_alloca_p + ")
    line = out.rfind("\n", 0, first) + 1
    indent = re.match(r"[ \t]*", out[line:]).group(0)
    out = (out[:line] + f"{indent}gen_alloca_p = (unsigned char *)_alloca(({size} & 0xfffffffffffffff0) + 0x400) + 0x200;\n"
           + out[line:])
    brace = out.index("{") + 1
    return out[:brace] + "\n  unsigned char *gen_alloca_p = 0;" + out[brace:]


def mistyped_float_slots(body: str) -> str:
    """A stack slot Ghidra typed float that the code stores integers into is an integer slot.

    The function shares one slot between its cases: one keeps a float there (read and written
    through its address), the others a byte or a varint, which the decompiler then spells as a
    conversion, `local_res18[0] = (float)(uint)*pbVar4`, and hands on as a float. As C that is a
    value conversion and a float argument, where the reference moved 32 bits through an integer
    register: protobuf's ExtensionSet::ParseFieldWithExtensionInfo (FUN_142237430) passed a
    sub-message's length to PushLimit as 2.0f, parsed nothing into the extension, and read its
    payload as the outer message's own fields - bgs EntityId's options got
    message_set_wire_format and the descriptor pool aborted the Battle.net login (2026-10-20).
    Such a slot is retyped to uint throughout the function: its address and its float uses stay
    as they were, as the 32 bits the reference has there."""
    slots = {m.group(1) for m in INT_INTO_FLOAT_SLOT.finditer(body)
             if not re.search(r"[/.]|fVar|dVar", m.group(2))}
    for off in slots:
        slot = f"((float *)(gen_frame + {off}))"
        uslot = f"((uint *)(gen_frame + {off}))"
        body = re.sub(re.escape(slot) + r"\[0\] = \(float\)\(((?:uint|int|ushort|short|byte|char|uchar))\)",
                      lambda m: f"{uslot}[0] = ({m.group(1)})", body)
        # a float stored back by its bits: the bits
        body = re.sub(re.escape(slot) + r"\[0\] = gen_bits2f\(", f"{uslot}[0] = (", body)
        # read into a float local: the float those bits are (movss), not a conversion
        body = re.sub(r"(\b[fd]Var\d+ = )" + re.escape(slot) + r"\[0\];",
                      lambda m: f"{m.group(1)}gen_bits2f({uslot}[0]);", body)
        body = body.replace(slot, uslot)
    return body


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
        # (nor is a string literal, a wide one included: "2-0.dll" in L"api-ms-win-core-synch-l1-2-0.dll"
        # read as the float 0., 2026-10-20)
        if not e or e.startswith(("&", "\"", "L\"", "u\"", "SUB", "CONCAT", "ZEXT", "SEXT")):
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
        # from the fifth on an argument is a stack slot of 8 bytes, and an unprototyped call stores
        # an int's 4 only: WSASend's lpOverlapped, a literal 0, arrived as 0x7ffc00000000 and the
        # Battle.net TLS handshake's second flight failed with WSAEFAULT (2026-10-20). CALLF's
        # arguments are widened in float_arguments; these are the calls through code pointers.
        def widened(k: int, a: str) -> str:
            if k >= 4 and not arg_kinds[k] and not a.strip().startswith("(unsigned long long)("):
                return f"(unsigned long long)({a})"
            return a
        if last < 0 and ret == "unsigned long long":
            if len(args) > 4:
                new_args = [widened(k, a) for k, a in enumerate(args)]
                if new_args != args:
                    out = out[:j + 1] + ",".join(new_args) + out[args_end - 1:]
            continue
        types, new_args = [], []
        for k, a in enumerate(args):
            if k <= last:
                t = arg_kinds[k] or "unsigned long long"
                types.append(t)
                new_args.append(f"({t})({a})")
            else:
                new_args.append(widened(k, a))
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
        # (`(ulonglong *)*stack0x...`: dereferenced right after a pointer cast, whose `)` the first
        # alternative refuses as the end of a multiplication's operand; 789 functions, C2100)
        ptr = bool(re.search(rf"(?:^|[^\w)\]\s])\s*\*\s*{n}\b|\(\w[\w ]*\*+\)\s*\*\s*{n}\b|\b{n}\s*\[|"
                             rf"\b{n}\s*[!=]=\s*\(\w+ \*\)", body, re.M))
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
    # and a whole one the function borrows (saved in its prologue, restored before it returns) as
    # its own scratch: `unaff_XMM6`..`unaff_XMM11` in FUN_1423a9a70, each written before any lane is
    # read; FUN_141633ad0's four lanes stored, then read (5 functions, 2026-10-18). A local of the
    # function's, starting at zero as every local does (zero_locals), whatever the caller left
    for n in set(re.findall(r"\bunaff_XMM\d+\b(?!_)", text)):
        text = re.sub(rf"\b{n}\b", f"gen_{n}", text)
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
    # a register xor'd with itself, `pxor xmm5, xmm5` as Ghidra folds it into an expression of the
    # lanes it never saw set: `CONCAT13(in_XMM5_Bd,...,in_XMM5_Ba) ^ CONCAT13(in_XMM5_Bd,...)`, zero
    # whatever the lanes held (FUN_1423f7ed0, 2026-10-18). Only an expression of such lanes and
    # CONCATs, which has no effect of its own; the lanes nothing else reads go with it
    lane = r"in_XMM\d+_[A-Z][a-z]"
    xor_self = re.compile(rf"((?:CONCAT\d+\(|{lane}|[,()\s])+?)\s*\^\s*\1(?![\w(])")
    if re.search(lane, text):
        text = xor_self.sub(lambda m: "0" if re.search(lane, m.group(1)) and m.group(1).count("(") == m.group(1).count(")")
                            else m.group(0), text)
        for n in set(re.findall(rf"\b{lane}\b", text)):
            if len(re.findall(rf"\b{n}\b", text)) == 1:
                text = re.sub(rf"^[ \t]*[\w ]+\**[ \t]*{n};\n", "", text, flags=re.M)
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
    # (and not a byte buffer: one indexed, `local_20[0] = 0;`, or decayed into a pointer local,
    # `local_38 = local_20;`, a string's inline storage. Taken for a vector, both statements were
    # wide paths and dropped, and the string's data pointer was left as stack garbage in
    # FUN_1414e1f80 on the Login click, 2026-10-17)
    pointers = set(re.findall(r"^\s*(?:const\s+)?\w+\s*\*+\s*(\w+)\s*;", text, re.M)) \
        | set(re.findall(r"\*\s*(\w+)\s*[,)]", text.partition("{")[0]))
    uses = WIDE_VAR_DECL.sub("", text)

    def buffer(n: str) -> bool:
        return bool(re.search(rf"(?<![\w.]){n}\s*\[", uses)) \
            or any(lhs in pointers for lhs in re.findall(rf"\b(\w+)\s*=\s*{n}\s*;", uses))

    sizes = {n: s for n, s in sizes.items()
             if re.match(r"(?:in|unaff)_[XYZ]MM", n)
             or (not buffer(n)
                 and re.search(rf"\b{n}\._\d+_\d+_|\b{n}\s*=(?!=)|=\s*{n}\s*;|[\^|&]\s*{n}\b|\b{n}\s*[\^|&]", text))}
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


def array_copies(text: str) -> str:
    """One local array assigned whole to another, `auVar2 = local_b0;` for two `uint [2]` (an
    8-byte copy, FUN_14250e390), is no C (C2106 once local_b0 is a frame slot): the bytes copied,
    as many as the destination holds."""
    # (the size from the declaration: once a frame slot, the array is a pointer expression, whose
    # sizeof is a pointer's)
    arrays = {m.group(2): f"sizeof({m.group(1).strip()}) * {m.group(3)}"
              for m in re.finditer(r"^[ \t]*((?:\w+ )+?\**)\s*(\w+)\s*\[\s*(\d+)\s*\];$", text, re.M)}
    if len(arrays) < 2:
        return text
    return re.sub(r"^([ \t]*)(\w+) = (\w+);$",
                  lambda m: f"{m.group(1)}memcpy({m.group(2)}, {m.group(3)}, {arrays[m.group(2)]});"
                  if m.group(2) in arrays and m.group(3) in arrays else m.group(0), text, flags=re.M)


def v32_params(text: str) -> str:
    """A parameter Ghidra types a pointer to 32 bytes, `undefined1 (*param_1) [32]` (declared
    gen_v32 * by signature()): `*param_1` is its array, which C decays to its first byte's address,
    so `*param_1 + param_2` steps bytes and `(*param_1)[i]` reads one. On gen_v32 neither is C
    (C2088, 11 AVX2 kernels, 2026-10-18): the bytes, through a byte pointer."""
    head, sep, body = text.partition("{")
    names = re.findall(r"undefined1?\s*\(\*(param_\d+)\)\s*\[32\]", head)
    if not names or not sep:
        return text
    def unary(m):
        # (a dereference, not `x * param_1`: what comes before the `*` is no operand's end)
        prev = body[:m.start()].rstrip()[-1:]
        return m.group(0) if prev and (prev.isalnum() or prev in "_)]") else f"((undefined1 *)({m.group(1)}))"

    for n in names:
        body = re.sub(rf"\*\s*({n})\b(?!\s*=[^=])", unary, body)
    return head + sep + body


def crt_block(a: int) -> bool:
    """In one of the image's two runs of C runtime code (vcruntime and the UCRT, linked in one
    piece each: 0x14026b400.., 0x14240ee60..)."""
    return 0x14026b400 <= a < 0x140296900 or 0x14240ee60 <= a < 0x14241f760


def balanced(text: str, open_at: int) -> int:
    """The index just past the bracket that closes the one at open_at."""
    depth, k = 0, open_at
    pairs = {"(": ")", "[": "]", "{": "}"}
    close = pairs[text[open_at]]
    while k < len(text):
        ch = text[k]
        if ch == text[open_at]:
            depth += 1
        elif ch == close:
            depth -= 1
            if depth == 0:
                return k + 1
        k += 1
    return -1


DOUBLE_ROUTINES = ("frexp", "ldexp", "fmod", "sin", "cos", "tan", "sqrt", "pow", "floor", "ceil", "exp",
                   "log", "log10", "atan", "atan2", "acos", "asin", "modf")


def late_fixes(text: str) -> str:
    """The last shapes the compiler refused in 2026-10-18's build, each a class of its own, on the
    final text (they are made of the translator's own forms: REFG, gen_frame, GEN_PIECE)."""
    # a register Ghidra has no name for (`register0x0000120c = 0;`, C2065): a local of its own
    regs = sorted(set(re.findall(r"\bregister0x[0-9a-f]+\b", text)))
    if regs:
        brace = text.find("\n{\n")
        if brace >= 0:
            decls = "".join(f"\n  unsigned long long gen_{r} = 0;" for r in regs)
            text = text[:brace + 2] + decls + text[brace + 2:]
            for r in regs:
                text = re.sub(rf"\b{r}\b", f"gen_{r}", text)
    # a field of the image's own DOS header (`((char *)REFP(0x140000000)).e_res_4_`, C2224): an
    # array field is its address, a scalar one its value
    dos = {"e_res_4_": (0x1c, None), "e_res2_10_": (0x28, None), "e_lfanew": (0x3c, "int"),
           "e_magic": (0x0, "ushort"), "e_cblp": (0x2, "ushort"), "e_oemid": (0x24, "ushort")}
    text = re.sub(r"\(\(char \*\)REFP\(0x140000000\)\)\.(\w+)",
                  lambda m: (f"((char *)REFP(0x{0x140000000 + dos[m.group(1)][0]:x}))" if dos[m.group(1)][1] is None
                             else f"(*({dos[m.group(1)][1]} *)REFP(0x{0x140000000 + dos[m.group(1)][0]:x}))")
                  if m.group(1) in dos else m.group(0), text)
    # (and the header's first word compared as a pointer: `((char *)REFP(0x140000000)) ==
    # (undefined2)0x5a4d`, the MZ check, its e_magic dropped)
    text = re.sub(r"\(\(char \*\)REFP\((0x[0-9a-f]+)\)\) ([=!]=) \(undefined2\)(0x[0-9a-f]+)",
                  r"(*(unsigned short *)REFP(\1)) \2 \3", text)
    # a zero cast to a byte array of no scalar's size (`(undefined1 [7])0x0`, C2067): stored as
    # that many zero bytes, and as a value, zero
    text = re.sub(r"\(\((?:undefined1|char) \*\)\(([^;]*?)\)\) = \((?:undefined1|char)\s*\[\s*(\d+)\s*\]\)0x0;",
                  r"memset((void *)(\1), 0, \2);", text)
    text = re.sub(r"\((?:undefined1|char)\s*\[\s*\d+\s*\]\)0x0\b", "0ULL", text)
    # a value's low bytes stored through a byte pointer or into a piece (`((undefined1 *)(gen_frame
    # + 0x21)) = SUB87(x,0)`, `GEN_PIECE(&local_8, 0, 7) = SUB87(d,0)`, C2106)
    out, i = [], 0
    for m in re.finditer(r"(\(\((?:undefined1|char) \*\)|GEN_PIECE\()", text):
        if m.start() < i:
            continue
        if m.group(1).startswith("GEN_PIECE"):
            end = balanced(text, m.end() - 1)
            if end < 0:
                continue
            target = text[m.end():end - 1]
        else:
            end = balanced(text, m.start())
            if end < 0:
                continue
            target = f"(void *){text[m.start() + len(m.group(1)):end - 1]}, 0"
        s = re.match(r"\s*=\s*SUB(\d)(\d)\(", text[end:])
        if not s:
            continue
        vstart = end + s.end() - 1
        vend = balanced(text, vstart)
        args = text[vstart + 1:vend - 1]
        if not re.search(r",\s*0\s*$", args):
            continue
        value = re.sub(r",\s*0\s*$", "", args)
        if m.group(1).startswith("GEN_PIECE"):
            call = f"gen_piece_set({target.rsplit(',', 2)[0]}, {target.rsplit(',', 2)[1].strip()}, {s.group(2)}, GEN_BITS({value}))"
        else:
            call = f"gen_piece_set({target}, {s.group(2)}, GEN_BITS({value}))"
        out.append(text[i:m.start()] + call)
        i = vend
    text = "".join(out) + text[i:]
    # a float global Ghidra typed as a pointer, read as the float it is (`fVar50 < REFG(float *,
    # 0x14297b6b0)`, `*pfVar34 = REFG(float *, ...)`, C2440)
    # (an ordering only: `==` between pointers is a pointer comparison)
    text = re.sub(r"((?<![<>=!])[<>]=?\s*|\*pfVar\w*\s*=\s*)REFG\(float \*, ", r"\1REFG(float, ", text)
    # a double global masked as the integer its bits are (`REFG(double, x) & 0xffffffff`, C2296)
    text = re.sub(r"(REFG\(double, 0x[0-9a-f]+\))(\s*[&|^]\s)", r"GEN_BITS(\1)\2", text)
    # the address of a float global's bits (C2102): the global's own address
    text = re.sub(r"&GEN_F32BITS\(REFG\(float, (0x[0-9a-f]+)\)\)", r"((uint *)REFP(\1))", text)
    # a pointer variable scaled as an integer (`puVar4 * 4`, C2296)
    for p in set(re.findall(r"^\s+(?:const\s+)?\w+\s*\*+\s*(\w+)\s*(?:=[^;]*)?;", text, re.M)):
        text = re.sub(rf"(?<![\w*(])\b{p}\s*\*\s*(0x[0-9a-f]+|\d+)\b", rf"((longlong){p} * \1)", text)
    # a double result cast straight to a pointer (`(wchar_t *)frexp(...)`, C2440): by its integer
    text = re.sub(r"\((\w+ \*+)\)(" + "|".join(DOUBLE_ROUTINES) + r")\(", r"(\1)(longlong)\2(", text)
    # a call through a pointer read as a pointer to an integer (`(*(*(undefined8 * *)p))(...)`,
    # C2064): through it as code
    out, i = [], 0
    for m in re.finditer(r"\(\*\(\*\(undefined8 \* \*\)", text):
        end = balanced(text, m.start())
        if end > 0 and text[end:end + 1] == "(":
            out.append(text[i:m.start()] + "(*(code *)(*(undefined8 * *)")
            i = m.end()
    text = "".join(out) + text[i:]
    # Ghidra's 10-byte integer around a shift pair (`((unkuint10)x << 0x30) >> 0x10`, C2065): the
    # result's 64 bits, which is the value shifted by the difference
    text = re.sub(r"\(\(unkuint10\)(\w+\([^()]*\)|\w+) << (0x[0-9a-f]+|\d+)\) >> (0x[0-9a-f]+|\d+)",
                  lambda m: f"((ulonglong)({m.group(1)}) << {int(m.group(2), 0) - int(m.group(3), 0)})", text)
    # a switch on a double (`case 0.0:`, C2052): on its bits (gen_dkey), the cases as theirs
    for m in reversed(list(re.finditer(r"switch\(\(long long\)\(", text))):
        open_ = text.find("{", m.end())
        close = balanced(text, open_) if open_ >= 0 else -1
        if close < 0:
            continue
        block = text[open_:close]
        if not re.search(r"\bcase -?\d+\.\d+(?:e[+-]?\d+)?:", block):
            continue

        def bits(c):
            v = float(c.group(1))
            return f"case 0x{(0 if v == 0 else struct.unpack('<Q', struct.pack('<d', v))[0]):x}ULL:"

        block = re.sub(r"\bcase (-?\d+(?:\.\d+)?(?:e[+-]?\d+)?):", bits, block)
        text = text[:m.start()] + "switch(gen_dkey(" + text[m.end():open_] + block + text[close:]
    return text


LEFTOVER_REG = {"RCX": "RCX", "ECX": "RCX", "CX": "RCX", "CL": "RCX", "RDX": "RDX", "EDX": "RDX", "DX": "RDX",
                "DL": "RDX", **{f"R{n}{s}": f"R{n}" for n in (8, 9, 10, 11) for s in ("", "D", "W", "B")}}
ARG_REGS = ("RCX", "RDX", "R8", "R9")


def call_arg_spans(text: str, open_at: int) -> list[tuple[int, int]] | None:
    """The spans of a call's top-level arguments, from the index just past its `(`."""
    spans, depth, start, k = [], 1, open_at, open_at
    while k < len(text):
        ch = text[k]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                if text[start:k].strip():
                    spans.append((start, k))
                return spans
        elif ch == "," and depth == 1:
            spans.append((start, k))
            start = k + 1
        elif ch == '"':
            k += 1
            while k < len(text) and text[k] != '"':
                k += 2 if text[k] == "\\" else 1
        k += 1
    return None


PHANTOM_CAST = r"(?:\(\s*[\w ]+\**\s*\)\s*)*"


def indirect_arg(text: str, start: int, end: int) -> bool:
    """Whether text[start:end] is a whole argument (under casts) of a call through a pointer,
    `(**(code **)(*p + 0x30))(p, &x, (int)uVar7)`."""
    m = re.search(r"([(,])\s*" + PHANTOM_CAST + r"$", text[max(0, start - 200):start])
    if not m or not re.match(r"\s*[,)]", text[end:]):
        return False
    k = max(0, start - 200) + m.start()
    if m.group(1) == ",":
        depth = 0
        k -= 1
        while k >= 0:
            ch = text[k]
            if ch == ")":
                depth += 1
            elif ch == "(":
                if depth == 0:
                    break
                depth -= 1
            k -= 1
        if k < 0:
            return False
    return text[:k].rstrip().endswith(")")


def phantom_register(text: str, name: str) -> bool:
    """A register read after a call that only ever becomes an argument of a call through a pointer,
    directly or through variables read only so. Such a call has no prototype the compiler could
    rely on: it sets every argument the callee takes, at every site. A register it does not set
    is no argument, whatever the decompiler guessed from it still holding something (the leftover
    of FUN_140229de0 in r8 and r9, passed on to a virtual call that takes two; 2026-10-18)."""
    body_at = text.find("{")

    def reads(v: str):
        for u in re.finditer(rf"\b{v}\b", text):
            if u.start() <= body_at:
                continue
            ls = text.rfind("\n", 0, u.start()) + 1
            if re.fullmatch(rf"[ \t]*[\w ]+\**\s*{v};", text[ls:text.find(chr(10), u.end())]):
                continue        # its declaration
            if re.match(r"\s*=(?!=)", text[u.end():]):
                continue        # an assignment to it
            yield u

    for u in reads(name):
        if indirect_arg(text, u.start(), u.end()):
            continue
        # `v = name` (a statement, or in a comma expression): v read only as such an argument
        m = re.search(r"\b(\w+)\s*=\s*" + PHANTOM_CAST + r"$", text[max(0, u.start() - 120):u.start()])
        if m and re.match(r"\s*[;,)]", text[u.end():]) and m.group(1) != name and \
                all(indirect_arg(text, w.start(), w.end()) for w in reads(m.group(1))):
            continue
        return False
    return True


def leftovers(text: str, callee_of, facts: dict[int, dict[str, list]], relied: dict[int, set[str]] | None = None) -> str:
    """`extraout_R11` read after a call to a function that returns with r11 holding one of its own
    arguments (tools/leftover.py): that argument, as the call passed it.

    Ghidra sees only that the register died at the call. The variable is assigned inside the
    call's argument list, `FUN_1402bfa60((gen_extraout_R11 = (longlong)(param_1)), lVar1)`, so it
    holds what the register held whatever else changed since; a 32-bit leftover (`mov r9d, edx`)
    keeps only the low half. A read no fact explains is left for the "lost track" refusal."""
    body_at = text.find("{")
    if body_at < 0:
        return text
    call = re.compile(r"\b(FUN_1[0-9a-f]{8}|[A-Za-z_][\w:]*)\s*\(")
    for name in sorted(set(re.findall(r"\bextraout_[A-Z0-9]+(?:_\d+)?\b", text[body_at:]))):
        m = re.fullmatch(r"extraout_([A-Z0-9]+?)(?:_\d+)?", name)
        reg = LEFTOVER_REG.get(m.group(1)) if m else None
        if reg is None:
            continue
        decl = re.search(rf"^[ \t]*([\w ]+?\**)[ \t]*\b{name};$", text, re.M)
        uses = [u for u in re.finditer(rf"\b{name}\b", text) if u.start() > body_at and
                (decl is None or not decl.start() <= u.start() < decl.end())]
        if decl is None or not uses:
            continue
        before = text[:uses[0].start()]
        calls = [c for c in call.finditer(before, body_at)
                 if c.group(1) not in ("if", "while", "for", "switch", "return", "sizeof", "CONCAT44", "CALLF")]
        if not calls:
            continue
        c = calls[-1]
        target = callee_of(c.group(1))
        fact = facts.get(target) if isinstance(target, int) else None
        if not fact or reg not in fact:
            continue
        arg, bits = fact[reg]
        k = ARG_REGS.index(arg)
        spans = call_arg_spans(text, c.end())
        if spans is None or k >= len(spans):
            continue
        lo, hi = spans[k]
        t = decl.group(1).strip()
        value = text[lo:hi].strip()
        if bits == 32:
            value = f"(uint)({value})"
        new = f"gen_{name}"
        text = text[:lo] + f"({new} = ({t})({value}))" + text[hi:]
        text = re.sub(rf"\b{name}\b", new, text)
    # and what reaches only the arguments of calls through pointers: no argument at all; or what
    # follows a direct call whose callee writes the register while this function's instructions
    # never read it after that call (`relied`, tools/preserved.py's liveness): the decompiler's
    # own merge of paths, a variable it gave the register on one of them, `uVar7 = extraout_RDX;`
    # read only where uVar7 holds something else (39 such reads, 2026-10-18). Its value is never
    # used for anything the reference computes, so any value will do
    for name in sorted(set(re.findall(r"\bextraout_\w+\b", text[body_at:]))):
        # (a whole xmm register is declared as Ghidra's 16-byte array, `undefined1 extraout_XMM0
        # [16];`, which this pass runs before tools/v16.py retypes: taken for no declaration, every
        # such merge was refused, `auVar32 = extraout_XMM0;` in FUN_140315680 with all four lanes
        # stored before any is read, and 5 functions more, 2026-10-18)
        decl = re.search(rf"^([ \t]*[\w ]+?\**[ \t]*)\b{name}(\s*\[\d+\])?;$", text, re.M)
        if not decl:
            continue
        if not phantom_register(text, name):
            if relied is None:
                continue
            m = re.fullmatch(r"extraout_([A-Z0-9]+?)(?:_[A-Z][a-z])?(?:_\d+)?", name)
            reg = LEFTOVER_REG.get(m.group(1)) if m else None
            if reg is None and m and re.fullmatch(r"XMM\d+", m.group(1)):
                reg = m.group(1)
            uses = [u for u in re.finditer(rf"\b{name}\b", text) if u.start() > decl.end()]
            if reg is None or not uses:
                continue
            calls = [c for c in re.finditer(r"\b(FUN_1[0-9a-f]{8}|[A-Za-z_][\w:]*)\s*\(", text[:uses[0].start()])
                     if c.start() > body_at and c.group(1) not in ("if", "while", "for", "switch", "return", "sizeof",
                                                                  "CONCAT44", "CALLF")]
            if not calls:
                continue
            target = callee_of(calls[-1].group(1))
            if isinstance(target, int):
                if reg in relied.get(target, ()):
                    continue
            elif reg in set().union(*relied.values()) if relied else False:
                # (a call it cannot name, through a pointer or behind a cast Ghidra put first: what
                # any call of this function leaves that is read after it, indirect ones under 0.
                # A register live after none is the decompiler's merge, FUN_141fe3480's xmm0)
                continue
        if decl.group(2):
            # (the array keeps its declaration; zero_locals() starts it at zero once retyped)
            text = text[:decl.start()] + f"{decl.group(1)}gen_{name}{decl.group(2)};" + text[decl.end():]
        else:
            text = text[:decl.start()] + f"{decl.group(1)}gen_{name} = 0;" + text[decl.end():]
        text = re.sub(rf"\b{name}\b", f"gen_{name}", text)
    return text


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
        # (a binary operator, after an operand: `(float)(&DAT_142cfc8e0)[param_1]` is an array of
        # floats read, its `&` an address; as bits, `&GEN_F32BITS(...)` was C2102 in 5 functions)
        if not re.search(r"[\w)\]]\s*[&|^](?![&|=])", inner) or "(uint)" not in inner and "REFG(float" not in inner:
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
             "realloc": 2, "calloc": 2, "_strdup": 1, "_wcsdup": 1, "_endthreadex": 1,
             "strspn": 2, "strcspn": 2, "wcschr": 2, "wcsstr": 2}


def crt_arity(body: str) -> str:
    """A runtime routine called with more arguments than it takes, down to its own: a lifted
    thunk (tools/lift.py) hands on all four argument registers (`return memmove(r_rcx, r_rdx,
    r_r8, r_r9);`, C2197 in 2 functions on 2026-10-16), and the extra ones are only the registers'
    leftovers."""
    out, i = [], 0
    for m in re.finditer(r"(?<![\w.>])(" + "|".join(CRT_ARITY) + r")\(", body):
        if m.start() < i:
            continue
        # (call_args: a literal's own parentheses and commas are not the call's; Lua's parser
        # FUN_141b8e820 calls `strchr("()[]^*/%+-#, ", c)`, and its `)` and `, ` cut the call
        # to one argument: three functions demoted, 2026-10-20)
        spans, close = call_args(body, m.end() - 1)
        n = CRT_ARITY[m.group(1)]
        if len(spans) > n:
            out.append(body[i:spans[n - 1][1]] + ")")
            i = close + 1
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
    m = re.fullmatch(r"(.+?) ([+&|^-]) (-?\w+|\(.+\))", rhs.strip())
    if m and _bare(m.group(1)) == _bare(lhs):
        # (a subtraction, `*puVar1 = *puVar1 - 1`, Ghidra's `lock dec`: the add of its negation;
        # 7 functions' reference counts were stubbed for it, 2026-10-18)
        if m.group(2) == "-":
            return "ADD", f"-({m.group(3)})"
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
        elif len(lines) == 3 and all(assigns) and re.fullmatch(r"\w+", assigns[0].group(1)) \
                and re.fullmatch(r"\w+", assigns[1].group(1)) \
                and re.fullmatch(rf"(?:\([\w ]+\)\s*)*{assigns[0].group(1)}", assigns[1].group(2).strip()) \
                and _bare(assigns[0].group(2)) == _bare(assigns[2].group(1)) \
                and _rmw(*assigns[2].groups()):
            # old = lv; x = (casts)old; lv = lv OP k  (a copy of the old value between, FUN_141c36320's
            # `uVar7 = (undefined1)(int)lVar11`): the xadd, then the copy
            old, (lhs, rhs) = assigns[0].group(1), assigns[2].groups()
            op = _rmw(lhs, rhs)
            out = [f"{old} = GEN_ATOMIC_{op[0]}({lhs}, {op[1]});", lines[1]]
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
            # old = lv; if (a == old) { lv = new; } else { x = y; }  (cmpxchg with the old value read
            # on failure: a reference count bumped only if unchanged, FUN_141c36320)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+); if \(([^;]+?) == ([^;]+?)\) \{ ([^;]+?) = ([^;]+); \} "
                                             r"else \{ ((?:[^;{}]+; )+)\}", text)
            if c:
                old, loaded, a, b, lv, new, other = c.groups()
                if _bare(loaded) == _bare(lv) and old in (a, b):
                    cmp = b if a == old else a
                    out = [f"{old} = GEN_ATOMIC_CAS({lv}, {new}, {cmp});",
                           f"if ({old} != {cmp}) {{ {other.strip()} }}"]
            # flag = a == lv; old = lv; if (flag) { lv = new; old = a; }  (the flag first, then the
            # value: Ghidra's other order of the same cmpxchg, FUN_14235a180, FUN_1422613d0)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+?) == ([^;]+?); (\w+) = ([^;]+); if \(\1\) \{ "
                                             r"([^;]+?) = ([^;]+); \4 = ([^;]+); \}", text)
            if c:
                flag, a, b, old, loaded, lv, new, after = c.groups()
                if _bare(loaded) == _bare(lv) and _bare(lv) in (_bare(a), _bare(b)):
                    cmp = b if _bare(a) == _bare(lv) else a
                    if _bare(after) == _bare(cmp):
                        out = [f"{old} = GEN_ATOMIC_CAS({lv}, {new}, {cmp});", f"{flag} = {old} == {cmp};"]
            # flag = c != lv; r = new; if (flag) { c = lv; r = lv; } lv = r;  (cmpxchg, the expected
            # value reloaded on failure, the flag kept, FUN_14235a180)
            c = out is None and re.fullmatch(r"(\w+) = ([^;]+?) != ([^;]+?); (\w+) = ([^;]+); if \(\1\) \{ "
                                             r"([^;]+?) = ([^;]+); \4 = ([^;]+); \} ([^;]+?) = \4;", text)
            if c:
                flag, a, b, r, new, cvar, got1, got2, lv = c.groups()
                if _bare(lv) in (_bare(a), _bare(b)) and _bare(got1) == _bare(lv) == _bare(got2):
                    cmp = b if _bare(a) == _bare(lv) else a
                    if _bare(cvar) == _bare(cmp):
                        out = [f"{{ long long gen_cas_ = GEN_ATOMIC_CAS({lv}, {new}, {cmp}); {flag} = gen_cas_ != {cmp}; "
                               f"if ({flag}) {{ {cvar} = gen_cas_; {r} = gen_cas_; }} else {{ {r} = {new}; }} }}"]
            # if (a == lv) { lv = new; [x = y;] }  (cmpxchg whose old value nothing keeps: clear a
            # global only if it still holds what this function put there, FUN_1405d7cd0)
            c = out is None and re.fullmatch(r"if \(([^;]+?) == ([^;]+?)\) \{ ([^;]+?) = ([^;]+); "
                                             r"((?:\w+ = [^;]+; )?)\}", text)
            if c:
                a, b, lv, new, extra = c.groups()
                if _bare(lv) in (_bare(a), _bare(b)):
                    cmp = b if _bare(a) == _bare(lv) else a
                    out = [f"if (GEN_ATOMIC_CAS({lv}, {new}, {cmp}) == {cmp}) {{ {extra.strip()} }}"
                           if extra.strip() else f"GEN_ATOMIC_CAS({lv}, {new}, {cmp});"]
            # flag = old == cmp; lv = old; if (flag) { lv = new; old = cmp; }  (the memory's value
            # read before the markers, as Ghidra hoists it, and written back as cmpxchg always
            # writes: `xor eax, eax; lock cmpxchg [0x143479470], edi` in FUN_1422613d0's second
            # spin loop, the same instruction its first loop decoded in another shape, 2026-10-18)
            c = out is None and re.fullmatch(r"(\w+) = (\w+) == ([^;]+?); ([^;]+?) = \2; if \(\1\) \{ \4 = ([^;]+); "
                                             r"\2 = \3; \}", text)
            if c:
                flag, old, cmp, lv, new = c.groups()
                out = [f"{old} = GEN_ATOMIC_CAS({lv}, {new}, {cmp});", f"{flag} = {old} == {cmp};"]
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
    sources = [p for top in ("src", "vendor") for ext in ("*.cpp", "*.c") for p in (ROOT / top).rglob(ext)]
    for p in sources:
        rel = p.relative_to(ROOT).as_posix()
        if rel.startswith("src/gen/"):
            continue
        if rel not in by_file and p.suffix == ".c":
            continue   # (C files carry exports only for internal ports; nothing to clean otherwise)
        text = p.read_text(encoding="utf-8", errors="replace")
        had = EXPORT_BEGIN in text
        if not had and rel not in by_file:
            continue
        kept = re.sub(r"\n*" + re.escape(EXPORT_BEGIN) + r".*?" + re.escape(EXPORT_END) + r"\n?", "\n",
                      text, flags=re.S).rstrip("\n") + "\n"
        if rel in by_file:
            lines = ["", EXPORT_BEGIN]
            for base, g in sorted(by_file[rel]):
                prefix = "gen_f" if g.get("fn") else "gen_p"
                linkage = "" if p.suffix == ".c" else 'extern "C" '
                lines.append(f'{linkage}void* const {prefix}_{base:x} = (void*)&{g["qual"]};')
            lines.append(EXPORT_END)
            kept += "\n".join(lines) + "\n"
        if kept != text:
            p.write_text(kept, encoding="utf-8", newline="\n")


PRIMITIVE_SIZES = {0x03: 0, 0x08: 4, 0x10: 1, 0x11: 2, 0x12: 4, 0x13: 8, 0x14: 16, 0x20: 1, 0x21: 2, 0x22: 4,
                   0x23: 8, 0x24: 16, 0x30: 1, 0x31: 2, 0x32: 4, 0x33: 8, 0x40: 4, 0x41: 8, 0x42: 10, 0x43: 16,
                   0x46: 2, 0x68: 1, 0x69: 1, 0x70: 1, 0x71: 2, 0x72: 2, 0x73: 2, 0x74: 4, 0x75: 4, 0x76: 8,
                   0x77: 8, 0x78: 16, 0x79: 16, 0x7a: 2, 0x7b: 4, 0x7c: 1}


def load_data_sizes() -> dict[str, int]:
    """Each port global's size, by its unqualified name (the smallest, when the name repeats), from
    the data symbols' types in the dumps the translator reads anyway.

    (`llvm-pdbutil pretty -globals`, which gave them before, took over 20 minutes on its own
    after each build, while the four other dumps ran in parallel in a minute: most of the
    translator's start, 2026-10-18)"""
    types = F.pdbutil("-types")
    rec: dict[int, tuple[str, str]] = {}
    for m in re.finditer(r"^\s*0x([0-9A-F]+) \| (LF_\w+) \[size = \d+\][^\n]*\n((?:(?!\s*0x[0-9A-F]+ \| LF_)[^\n]*\n)*)",
                         types, re.M):
        rec[int(m.group(1), 16)] = (m.group(2), m.group(3))
    memo: dict[int, int | None] = {}

    def size_of(ti: int, depth: int = 0) -> int | None:
        if ti < 0x1000:
            if (ti >> 8) & 0xF:
                return 8    # a pointer mode
            return PRIMITIVE_SIZES.get(ti & 0xFF)
        if ti in memo or depth > 20:
            return memo.get(ti)
        kind, body = rec.get(ti, ("", ""))
        out = None
        if kind == "LF_ARRAY":
            m = re.search(r"\bsize: (\d+)", body)
            out = int(m.group(1)) if m else None
        elif kind in ("LF_STRUCTURE", "LF_CLASS", "LF_UNION", "LF_INTERFACE"):
            ref = re.search(r"forward ref \(-> 0x([0-9A-F]+)\)", body)
            if ref:
                out = size_of(int(ref.group(1), 16), depth + 1)
            else:
                m = re.search(r"sizeof (\d+)", body)
                out = int(m.group(1)) if m else None
        elif kind == "LF_POINTER":
            out = 8
        elif kind == "LF_MODIFIER":
            m = re.search(r"referent = 0x([0-9A-F]+)", body)
            out = size_of(int(m.group(1), 16), depth + 1) if m else None
        elif kind == "LF_ENUM":
            m = re.search(r"underlying type = 0x([0-9A-F]+)", body)
            out = size_of(int(m.group(1), 16), depth + 1) if m else 4
        memo[ti] = out
        return out

    sizes: dict[str, int] = {}
    for dump in (F.pdbutil("-globals"), F.pdbutil("-symbols")):
        for m in re.finditer(r"S_[GL]DATA32 \[size = \d+\] `([^`]*)`\s*\n\s*type = 0x([0-9A-F]+)", dump):
            size = size_of(int(m.group(2), 16))
            name = re.sub(r"^.*::", "", m.group(1))
            if size and re.fullmatch(r"\w+", name):
                sizes[name] = min(size, sizes.get(name, size))
    return sizes


def load_data_sizes_pretty() -> dict[str, int]:
    """(the old reader, by `pretty -globals`, kept to compare against)"""
    out = F.pdbutil("pretty", "-globals")
    sizes = {}
    for m in re.finditer(r"^\s*data \[0x[0-9a-f]+, sizeof=(\d+)\] (.*?)\s*$", out, re.M):
        # (a function pointer, or an array of them, ends in its parameter list: `void (__cdecl *
        # blz::g_logSinks[16])(const char*, ...)`. Read as the last word, it had no size, so the
        # translated code's lock at +0x88 past the 0x80-byte array went to other port data, C4739
        # unseen, 2026-10-17)
        fp = re.search(r"\(\s*(?:__\w+\s*)?\*+\s*(?:\w+::)*(\w+)(?:\[\d*\])*\s*\)\s*\(", m.group(2))
        plain = re.search(r"(\w+)(?:\[\d*\])*$", m.group(2))
        name = fp.group(1) if fp else plain.group(1) if plain else None
        if name:
            size = int(m.group(1))
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
    return out | crt_exports()


@functools.lru_cache(maxsize=None)
def crt_exports() -> frozenset[str]:
    """Every name the port's C runtime DLLs export."""
    import pefile
    out = set()
    for dll in ("ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"):
        path = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / dll
        if not path.exists():
            continue
        d = pefile.PE(str(path), fast_load=True)
        d.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        out |= {s.name.decode() for s in getattr(getattr(d, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []) if s.name}
        d.close()
    return frozenset(out)


def load_data_publics() -> list[str]:
    out = F.pdbutil("-publics")
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
        junk = json.loads(jf.read_text())["functions"] if jf.exists() else {}
        self.junk_functions = {int(k, 16) for k in junk}
        # (the padding between functions Ghidra made functions of, tools/junkscan.py fragments():
        # no function, so never a root, and what reaches one is the protection's junk)
        self.fragments = {int(k, 16): v["fragment"] for k, v in junk.items() if "fragment" in v}
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
        # the image's `jmp` thunks to the CRT_ENTRY routines (`jmp _calloc_base` at 0x140282ca8,
        # padded with int3): curl_global_init stores the thunk, not the routine, so its calloc
        # still landed on int3 and returned null after CRT_ENTRY had the routine (2026-10-17)
        self.crt_thunks: dict[int, str] = {}
        tbase = 0x140001000
        o = self.ref.off(tbase)
        text = bytes(self.ref.image[o:o + 0x260b5d3])
        for m in re.finditer(rb"\xe9(....)\xcc", text, re.S):
            a = tbase + m.start()
            target = a + 5 + int.from_bytes(m.group(1), "little", signed=True)
            if target in CRT_ENTRY and a % 8 == 0:
                self.crt_thunks[a] = CRT_ENTRY[target]
        self.pointer_stub_calls(text, tbase)
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
        self._pool = None   # worker processes (start_pool), or None to do the work here
        self.mismatched: set[int] = set()   # ports whose signature is not the reference's
        self.fn_exported: dict[int, dict] = {}   # internal ports reached through gen_f_ (internal_port)
        self.internal_wider: dict[int, tuple] = {}   # internal ports left out: wider than the reference
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
        # (no exe at all, after a clean: nothing is newer than the cache, which stands)
        stale = not PORT_CACHE.exists() or (F.PORT_EXE.exists() and PORT_CACHE.stat().st_mtime < F.PORT_EXE.stat().st_mtime)
        if not stale:
            try:
                old = json.loads(PORT_CACHE.read_text(encoding="utf-8"))
                stale = not (old.get("procs") and old.get("publics") and old.get("data_publics"))
            except (OSError, ValueError):
                stale = True
        if F.PORT_EXE.exists() and stale:
            # (every dump the loaders below read, all at once: one after another they were most
            # of a run's start after each build)
            F.prefetch_pdb(("-types",), ("-ids",), ("-symbols",), ("-publics",), ("-globals",))
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

    def internal_port(self, a: int, rec: dict, sig) -> str | None:
        """A hand port with no public symbol (an anonymous namespace's function or a file
        `static`), reached through a C-linkage pointer its own file exports (write_exports:
        `gen_f_<a>`): '&' and the name its file can take the address by, or None. Free
        functions only (a member's address is no plain function pointer); a template's
        instantiation has no single name. 2357 ports were passed over, the translator
        translating each a second time and the two copies keeping their state apart (the
        initial hotfix listeners, the FFX slots, 2026-10-21)."""
        name = rec["name"]
        # (a function in a named namespace's anonymous one, casc::`anonymous namespace'::FileExists,
        # is casc::FileExists from the file's scope; a static member is a plain function too)
        plain = name.replace("`anonymous namespace'::", "")
        rel = rec["file"].replace("\\", "/")
        if (sig[2] or "<" in name or "`" in plain or "operator" in plain
                or not rel.startswith(("src/", "vendor/")) or rel.startswith("src/gen/")
                or not rel.endswith((".cpp", ".c"))
                or not re.fullmatch(r"\w+(?:::\w+)*", plain) or (rel.endswith(".c") and "::" in plain)):
            return None
        # (one that takes more arguments than the reference passes is not that function, or not
        # in that shape: ReadAddLight(L, usage, args) carried FUN_1407d22e0's tag, an "is this
        # argument a finite number" check taking (L, index), and once reachable it ran on every
        # GetCharacterInfo. These had never been reached from translated code, so they stay on
        # their translation until each is checked: build/internal-arity.txt)
        ref = self.signature(a)
        if ref is not None and len(sig[1]) > len(ref[2]):
            self.internal_wider[a] = (name, len(ref[2]), len(sig[1]))
            return None
        self.fn_exported[a] = {"name": name, "file": rel, "qual": plain, "fn": True}
        return "&" + plain

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
        if mangled is None and sig is not None:
            mangled = self.internal_port(a, rec, sig)
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
        if ref is not None and len(args) + (1 if this else 0) < len(ref[2]) and mangled.startswith(("?", "&")) \
                and not mangled.endswith("ZZ"):
            self.mismatched.add(a)
            return None
        return va, mangled, sig

    def relied_facts(self, a: int) -> dict[int, set[str]] | None:
        """For function a, per direct callee, the registers that callee writes which a's own
        instructions still read after calling it (tools/preserved.py 'relied'); None when the
        facts do not cover a (its flow was not followed, or no facts at all)."""
        if not hasattr(self, "_relied"):
            p = ROOT / "build/facts/preserved.json"
            facts = json.loads(p.read_text()) if p.exists() else {}
            self._relied = ({int(f, 16): {int(g, 16): set(r) for g, r in gs.items()}
                             for f, gs in facts["relied"].items()} if "relied" in facts else None)
            self._liveness_done = set(facts["followed"]) if "followed" in facts else None
        if self._relied is None:
            return None
        if self._liveness_done is not None and f"0x{a:x}" not in self._liveness_done:
            return None
        return self._relied.get(a, {})

    def leftover_facts(self) -> dict[int, dict[str, list]]:
        """build/facts/leftover.json: the registers each function returns holding an argument."""
        if not hasattr(self, "_leftover"):
            p = ROOT / "build/facts/leftover.json"
            self._leftover = ({int(k, 16): v for k, v in json.loads(p.read_text())["functions"].items()}
                              if p.exists() else {})
        return self._leftover

    def delay_imports(self) -> dict[int, str]:
        """The reference's delay-load import slots, by address: `OLEAUT32_DLL::SysFreeString`,
        as Ghidra names a regular import (Import::<dll>_DLL::<name>)."""
        if not hasattr(self, "_delay_imports"):
            import pefile
            pe = pefile.PE(str(self.ref.path), fast_load=True)
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"]])
            self._delay_imports = {
                i.address: f"{d.dll.decode().rsplit('.', 1)[0].upper()}_DLL::{i.name.decode()}"
                for d in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []) for i in d.imports if i.name}
            pe.close()
        return self._delay_imports

    def body(self, a: int) -> str | None:
        """The function's corrected decompilation, computed once: planning asks for it twice
        (vetting, callees) and emission once more, which was two thirds of a run (2026-10-16)."""
        cache = self.__dict__.setdefault("_bodies", {})
        if a not in cache:
            text = self._body(a)
            if text is not None and "FUN_" in text:
                # a runtime routine the port's own runtime exports, reached by its address (the
                # lifter's `CALLF(undefined8, FUN_140272964)(...)`, the reference's _stricmp): the
                # port's, as a call by its name is; three lifted callers left it a stub (2026-10-18)
                exported = self.exported_runtime()
                # (called through an unprototyped pointer: the decompiler, and the lifter, pass the
                # four argument registers whatever the routine takes, `_stricmp(r_rcx, r_rdx, r_r8,
                # r_r9)`, which its own declaration refused, C2197 in 5 functions, 2026-10-18)
                # (as `gen_rt_<name>`, declared unprototyped in translated.h and linked to the
                # routine by an alternate name: no header need declare it)
                text = FUNREF.sub(lambda m: f"gen_rt_{exported[int(m.group(1), 16)]}"
                                  if int(m.group(1), 16) in exported else m.group(0), text)
            cache[a] = text
        return cache[a]

    def exported_runtime(self) -> dict[int, str]:
        """The reference's runtime routines by address whose names the port's runtime exports."""
        if not hasattr(self, "_exported_runtime"):
            names = crt_exports()
            out = {}
            for a, e in self.cor.items():
                bare = re.sub(r"^FID_conflict[:_]", "", e["name"])
                if bare in names and "::" not in bare and self.is_lib(a) and a not in CRT_ENTRY:
                    out[a] = bare
            self._exported_runtime = out
        return self._exported_runtime

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
        # (and a call to an import's `jmp [slot]` thunk that is no corpus function: the import, as a
        # call through its slot reads. FUN_1401c9f70 calls GetAdaptersInfo through 0x14241f757,
        # stubbed "not in the corpus", 2026-10-18)
        def import_thunk(m):
            t = int(m.group(1), 16)
            if t in self.cor or not self.ref.is_code(t) or self.ref.image[self.ref.off(t)] != 0xFF \
                    or self.ref.image[self.ref.off(t) + 1] != 0x25:
                return m.group(0)
            slot = t + 6 + struct.unpack_from("<i", self.ref.image, self.ref.off(t) + 2)[0]
            imp = self.import_slots().get(slot)
            if imp is None:
                return m.group(0)
            # (the DLL named as Ghidra names a regular import, delay_imports() as well)
            return f"(*Import::{imp[0].rsplit('.', 1)[0].upper()}_DLL::{imp[1]})("
        text = re.sub(r"\bFUN_(1[0-9a-f]{8})\s*\(", import_thunk, text)
        # a local typed by an import's own function type (`DefWindowProcW *local_50;`, holding
        # `Import::USER32_DLL::DefWindowProcW`): a code pointer
        for imp in set(re.findall(r"\bImport::\w+::(\w+)", text)):
            text = re.sub(rf"^([ \t]*){imp} \*(\w+);$", r"\1code *\2;", text, flags=re.M)
        # Ghidra's NaN test, apart from the C library's NAN constant (Ghidra.h)
        text = re.sub(r"\bNAN\s*\(", "GEN_ISNAN(", text)
        # a switch on a value Ghidra typed a pointer (`switch(param_2)`, an `int *param_2` the cases
        # compare with numbers, 2 functions, C2050): the whole operand as an integer, which an
        # integer operand already is
        out, i = [], 0
        for sm in re.finditer(r"\bswitch\s*\(", text):
            if sm.start() < i:
                continue
            depth, k = 1, sm.end()
            while k < len(text) and depth:
                depth += {"(": 1, ")": -1}.get(text[k], 0)
                k += 1
            out.append(text[i:sm.end()] + f"(long long)({text[sm.end():k - 1]}))")
            i = k
        text = "".join(out) + text[i:]
        # a qualified name Ghidra wrapped after a `::` (`(*Import::KERNEL32_DLL::` and the name on the
        # next line; `Lua::C_UIWidgetManager::` ...), as the header's are joined in signature():
        # read apart, `Import` was undeclared (3 functions, 2026-10-18)
        text = re.sub(r"(?<=\w)::[ \t]*\n[ \t]*(?=[\w~<])", "::", text)
        # a delay-load import slot (`(*ImgDelayDescr_IAT_142dda4d8)(p)`, OLEAUT32's SysFreeString):
        # the import it is, as any other (and its DLL's import library linked with it); 8
        # functions were demoted as undeclared (2026-10-18)
        if "ImgDelayDescr_IAT_" in text:
            delay = self.delay_imports()
            text = re.sub(r"\bImgDelayDescr_IAT_(1[0-9a-f]{8})\b",
                          lambda m: f"Import::{delay[int(m.group(1), 16)]}" if int(m.group(1), 16) in delay
                          else m.group(0), text)
        # the function's own return address (`unaff_retaddr`, FUN_1402113a0 checking which module
        # called it): the intrinsic. Not where it feeds the anti-tamper return-address check (the
        # range at DAT_14261a0b0, tools/retcheck.py): the port's addresses fail it, and its
        # failure is a deliberate crash, so those stay protection (2026-10-18)
        # an `in_stack_` slot this function writes before it reads it (an outgoing argument it
        # fills, `in_stack_fffffffffffffa28 = local_res18;` then the call, FUN_141b138a0): its own
        # local, whatever Ghidra named it
        for name in set(re.findall(r"\bin_stack_[0-9a-f]+\b", text)):
            body_at = text.find("{")
            uses = [u for u in re.finditer(rf"\b{name}\b", text) if u.start() > body_at
                    and not re.match(rf"[ \t]*[\w ]+\**\s*{name};$",
                                     text[text.rfind("\n", 0, u.start()) + 1:text.find("\n", u.end())])]
            if uses and re.match(r"\s*=(?!=)", text[uses[0].end():]) and \
                    re.fullmatch(r"\s*", text[text.rfind("\n", 0, uses[0].start()) + 1:uses[0].start()]):
                text = re.sub(rf"\b{name}\b", "gen_" + name[3:], text)
        if "unaff_retaddr" in text and "14261a0b0" not in text and "142618df8" not in text:
            text = re.sub(r"^[ \t]*[\w ]+\*?\s*unaff_retaddr;\n", "", text, flags=re.M)
            text = re.sub(r"\bunaff_retaddr\b", "((longlong)_ReturnAddress())", text)
        if "extraout_" in text:
            text = leftovers(text, lambda n: self.named_callee(n, e["name"]), self.leftover_facts(), self.relied_facts(a))
        # a cast to a small byte array (`(undefined1 [8])CONCAT44(...)`): to the scalar of its size,
        # since C casts to no array type (C2067)
        # (`char` too: `(char  [8])(*(ulonglong *)...)`, 3 functions, 2026-10-18)
        text = re.sub(r"\((?:undefined1?|char)\s*\[\s*([1248])\s*\]\)", r"(undefined\1)", text)
        # and through a pointer to one (`*(undefined1 (*) [8])(lVar7 + 0x1c) = ...`, an 8-byte
        # store): a pointer to the scalar, which C can assign through (C2106)
        text = re.sub(r"\((?:undefined1?|char)\s*\(\*\)\s*\[\s*([1248])\s*\]\)", r"(undefined\1 *)", text)
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
        # (several brackets, not all closed: `s_<Realm_Name>_____<Realm_IP>_____<_142618260`, C2065)
        # (and one whose text holds a `>`, read as a piece: `s_fff>D__BuildServer_A_work_git_wo_
        # 14280032c._0_4_`, the first four bytes of the data there, C2065, 2026-10-18)
        text = re.sub(r"\bs_\w*[<>][\w<>]*?_(1[0-9a-f]{8})\._(\d+)_(\d+)_",
                      lambda m: f"(*(undefined{m.group(3)} *)((char *)REFP(0x{m.group(1)}) + {m.group(2)}))"
                      if m.group(3) in ("1", "2", "4", "8") else m.group(0), text)
        text = re.sub(r"\bs_<[\w<>]*?_(1[0-9a-f]{8})\b", r"((char *)REFP(0x\1))", text)
        text = re.sub(r"&PTR_s_<[\w<>]*?_(1[0-9a-f]{8})\b", r"((char **)REFP(0x\1))", text)
        text = re.sub(r"\bPTR_s_<[\w<>]*?_(1[0-9a-f]{8})\b", r"(*(char **)REFP(0x\1))", text)
        # a stack allocation (`&stack0xffffffffffffffe8 + -(uVar5 & 0xfffffffffffffff0)`, the
        # `sub rsp, rax` of an alloca): the same allocation in this frame
        text = re.sub(r"^([ \t]*\w+ = (?:\([\w ]+\*+\))?)&stack0x[0-9a-f]+ \+ -\(([^;()]*(?:\([^;()]*\)[^;()]*)*)\);$",
                      r"\1(undefined1 *)_alloca(\2);", text, flags=re.M)
        # (anywhere else in an expression, and used more than once: `(uint *)(&stack0x..fff8 +
        # -(n))` and later `(longlong)(&stack0x..fff8 + -(n)) - ...`, the same allocation, C2065
        # in FUN_141647080, 2026-10-18): allocated where first met, and its address after
        allocs: dict[str, str] = {}
        out, i = [], 0
        for m in re.finditer(r"&stack0x[0-9a-f]+ \+ -\(", text):
            end = balanced(text, m.end() - 1)
            if end < 0 or m.start() < i:
                continue
            size = text[m.end():end - 1]
            # (the parentheses a cast's, `+ -(longlong)pcVar5`: the size is the cast operand,
            # C2275 in 15 functions, 2026-10-18)
            cast = re.match(r"\s*(\w+|\([^()]*\))", text[end:])
            if re.fullmatch(r"\s*(?:const\s+)?\w+(?:\s+\w+)*\s*\**\s*", size) and cast:
                size = f"({size}){cast.group(1)}"
                end += cast.end()
            whole = text[m.start():end]
            if whole not in allocs:
                allocs[whole] = f"gen_alloca_{len(allocs)}"
                use = f"({allocs[whole]} = (undefined1 *)_alloca({size}))"
            else:
                use = allocs[whole]
            out.append(text[i:m.start()] + use)
            i = end
        if allocs:
            text = "".join(out) + text[i:]
            brace = text.find("\n{\n")
            if brace >= 0:
                text = text[:brace + 2] + "".join(f"\n  undefined1 *{v};" for v in allocs.values()) + text[brace + 2:]
        # a switch on a value Ghidra typed a pointer (`switch(plVar5)`, a jump-table index held in
        # a pointer-typed register variable): C switches on integers (C2050)
        for var in re.findall(r"\bswitch\((\w+)\)", text):
            if re.search(rf"^\s*[\w ]+\*+\s*{var};$", text, re.M):
                text = text.replace(f"switch({var})", f"switch((longlong){var})")
        text = strip_retcheck(text)[0]
        # (and the return address read outside the checks just stripped, which the rule above left
        # while they were there: Lua::C_LFGList::UpdateListing hands it to its usage error and keeps
        # its high half in a local, 2026-10-18)
        if "unaff_retaddr" in text and "14261a0b0" not in text and "142618df8" not in text:
            text = re.sub(r"^[ \t]*[\w ]+\*?\s*unaff_retaddr;\n", "", text, flags=re.M)
            text = re.sub(r"\bunaff_retaddr\b", "((longlong)_ReturnAddress())", text)
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
        text = array_copies(text)
        text = v32_params(text)
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
            # (an entry that is a `jmp rel32` thunk runs its target: the GUID handlers'
            # initializer 0x1401eef00 jumps to FUN_141551820, and being a thunk it is in no
            # corpus, so the tables stayed empty and every GUID string was "Player-", 2026-10-21)
            if p and p not in self.cor and self.ref.is_code(p) and self.ref.image[self.ref.off(p)] == 0xE9:
                p = p + 5 + self.ref.i32(p + 1)
            e = self.cor.get(p)
            if not p or e is None or "c" not in e:
                continue
            touched = [int(m.group(2), 16) for m in SYMBOL.finditer(e["c"])]
            # (only a writable one: a constant the port also defines, read from .rdata, builds
            # nothing a second time. Counting those skipped 967 of the image's initializers,
            # among them the cameras' (FUN_140208620, for reading GROUND_DROP), whose
            # unconstructed strings faulted on entering the world, 2026-10-21)
            def writable(x):
                s = self.ref.section_of(x)
                return s is None or s.name not in (".rdata", ".text")
            if any(self.held_global(x) and writable(x) for x in touched):
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
        self._arity[b] = None     # (until known: thunks that jump to each other)
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
            body = self.lifted(b).partition("{")[2]
            # (a lone jump to a function, `jmp FUN_1405dec80`, lifted as handing its four argument
            # registers on: it needs what its target needs, not four. As four, a caller passing
            # none was refused, "the decompiler lost a call's arguments", 48 functions, 2026-10-18)
            only = re.fullmatch(r"\s*ulonglong fa = 0, fb = 0, fr = 0;(?:\s*ulonglong r_\w+ = a\d;)*"
                                r"\s*return FUN_([0-9a-f]{9})\((?:r_\w+(?:, )?)*\);\s*}\s*", body)
            if only and int(only.group(1), 16) != b and self.arity(int(only.group(1), 16)) is not None:
                need = self.arity(int(only.group(1), 16))
            else:
                used = [int(n) for n in re.findall(r"\ba(\d)\b", body)]
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
            self._lifted[a] = None if inside or not self.ref.is_code(a) else lift.lift(self.ref, a, arity=self.arity)
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
            return CRT_BY_ADDR.get(target) or CRT_ENTRY.get(target) or self.crt_thunks.get(target, target)

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
        if name.removeprefix("FID_conflict_") in CRT_EXACT:
            return name.removeprefix("FID_conflict_")
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
        # zlib's entry points are the reference's own even where the port links zlib too: a
        # z_stream is one library's state, and FID names only some of its functions. The
        # Battle.net realm list blob was set up by the port's inflateInit_ and run through the
        # reference's unnamed inflate (FUN_14235abd0), which looped on the other layout and hung
        # the client at the realm list (2026-10-20)
        if len(cands) == 1 and re.fullmatch(r"(?:inflate|deflate)\w*|z(?:lib)?_\w+|crc32\w*|adler32\w*|"
                                             r"(?:un)?compress2?|zError|get_crc_table", bare):
            return by_addr(cands[0])
        if bare != name and bare in self.port_public_names:
            # FID's name for a runtime routine the port links under its own (`_rmdir`)
            return bare
        if len(cands) == 1 and name not in self.port_public_names and bare not in self.port_public_names \
                and bare not in CRT and bare not in C_LIBRARY:
            return by_addr(cands[0])
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
        # (the calling convention and pointer keywords only: `__uint64 * __fastcall_keepxmm0` lost
        # its type to the same strip, and the result was a bare `*`, 2026-10-17)
        ret = re.sub(r"\b__(?:cdecl|fastcall\w*|thiscall|stdcall|vectorcall\w*|ptr64|ptr32|unaligned|restrict|w64)\b",
                     "", m.group(1)).strip() or "void"
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
        # (and to 32, `undefined1 (*param_1) [32]`, what an AVX2 kernel loads a ymm through:
        # Ghidra.h's gen_v32 storage; read as a type word it was "a type C does not know
        # ((*param_1))", FUN_142400370 and FUN_1424009d0, 2026-10-18)
        params = [re.sub(r"^undefined1? \(\*(\w+)\) \[32\]$", r"gen_v32 *\1", p) for p in params]
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

    LITERAL_COMPARE = re.compile(r'(?:([!=]=)\s*("(?:\\.|[^"\\\n])*"))|(?:("(?:\\.|[^"\\\n])*")\s*([!=]=))')

    def literal_compares(self, a: int, body: str) -> str:
        """A pointer compared with a string literal is compared with the reference's string:
        Ghidra prints the `.rdata` address the instructions load as the text there, and C's
        literal is another address, so the test came out "different" every time
        (FUN_141c7a3b0's `local_58 != "unknown or exploit addon"`, C4130, 2026-10-17). The
        address is the one this function loads (`lea`) whose bytes are that text."""
        if not self.LITERAL_COMPARE.search(body) or a not in self.cor:
            return body
        f = self.callargs._functions.get(a) or callargs._Function(self.ref, a, self.cor[a]["size"])
        loads = []
        for i in f.ins:
            for op in i.operands:
                if i.mnemonic == "lea" and op.type == callargs.x86.X86_OP_MEM and op.mem.base == callargs.x86.X86_REG_RIP:
                    loads.append(i.address + i.size + op.mem.disp)

        def text_at(va: int) -> bytes | None:
            try:
                o = self.ref.off(va)
            except Exception:
                return None
            end = self.ref.image.find(b"\0", o, o + 0x1000)
            return bytes(self.ref.image[o:end]) if end >= 0 else None

        def address_of(literal: str) -> int | None:
            want = literal[1:-1].encode("latin-1", "backslashreplace").decode("unicode_escape").encode("latin-1")
            hits = {t for t in loads if text_at(t) == want}
            return hits.pop() if len(hits) == 1 else None

        def swap(m):
            op, lit = (m.group(1), m.group(2)) if m.group(1) else (m.group(4), m.group(3))
            at = address_of(lit)
            if at is None:
                return m.group(0)
            ref = f"((char *)REFP(0x{at:x}))"
            return f"{op} {ref}" if m.group(1) else f"{ref} {op}"

        return self.LITERAL_COMPARE.sub(swap, body)

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
        translate, stub, adapt, native = {}, {}, {}, {}
        # breadth first: a function is planned at its shortest depth. Depth first, the first
        # visit could be the deepest, stub it "beyond the translation depth" and mark it seen:
        # a new corpus changed the order, and 619 functions lost the string-buffer helpers
        # FUN_142203d20 / FUN_142203dd0 they had been translated with (2026-10-17)
        from collections import deque
        frontier = deque((a, 0) for a in roots)
        queued = set(roots)
        seen = set()
        warmed = -1
        while frontier:
            a, d = frontier.popleft()
            if d > warmed:
                # each depth's bodies at once, in the workers when there are any (start_pool)
                self.warm([a] + [x for x, dx in frontier if dx == d and x not in seen])
                warmed = d
            if a in seen:
                continue
            seen.add(a)
            # (a root reached only through a pointer, data/indirect_roots.json, that the port has:
            # the port, through its adapter, which the jump table dispatches to as to any. Taken
            # for a function to translate, the anti-tamper frame tick and key stub, ported as the
            # diverged no-ops in src/client, were stubbed as protection, 2026-10-19)
            if d == 0 and a in getattr(self, "adapt_roots", ()):
                adapt[a] = True
                continue
            # (a function that did not compile is planned as translated, and stubbed only at
            # emission: its callees stay in the work set, so the set, the files and the
            # declarations stay as they were from one --fix round to the next)
            if self.native_ok(a):
                # (a root in a native range too: the reference's own code runs, not a translation)
                native[a] = True
                continue
            reason = self.why_not(a, root=d == 0, demotion=False)
            if reason is None:
                translate[a] = d
                for b in self.callees(self.body(a), self.cor[a]["name"] if a in self.cor else ""):
                    if b in seen or b in CRT_BY_ADDR or b in CRT_ENTRY or b in self.crt_thunks:
                        continue
                    if self.native_ok(b):
                        native[b] = True
                        seen.add(b)
                        continue
                    if self.is_lib(b) and not (b in self.cor and self.stateless(b)):
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
        return {"translate": translate, "stub": stub, "adapt": adapt, "native": native}

    def runtime_only_dropped(self, roots: list[int]) -> list[int]:
        """The roots less the reference runtime's own named routines that only the runtime calls.

        A routine the port's runtime has is the port's (is_lib), and what only such routines call
        is the reference runtime's inside: `_expandlocale` under setlocale, `_write_nolock` under
        _write. As roots they were translated and called its internals, stubbed (2026-10-18).
        Non-member functions only, with at least one caller, all of them the runtime's or dropped
        already (unnamed too: the EH frame handler's helpers under __CxxFrameHandler, which read
        _GetImageBase, 19 demoted). A lambda is no runtime caller (the game has its own, under
        the same `<lambda_` names); code reached only through a stored pointer stays."""
        cands = {a for a in roots if "::" not in self.cor[a]["name"]}
        if not cands:
            return roots
        named = {self.cor[a]["name"]: a for a in cands if not self.cor[a]["name"].startswith(("FUN_", "thunk_FUN_"))}
        ref = re.compile(r"\b(" + "|".join(re.escape(n) for n in sorted(named, key=len, reverse=True)) + r")\b") \
            if named else None
        callers: dict[int, set[int]] = defaultdict(set)
        for a, e in self.cor.items():
            c = e.get("c", "")
            c = c[c.find("{"):]
            for x in set(FUNREF.findall(c)):
                b = int(x, 16)
                if b in cands and b != a:
                    callers[b].add(a)
            if ref is not None:
                for n in set(ref.findall(c)):
                    if named[n] != a:
                        callers[named[n]].add(a)

        def runtime(c: int) -> bool:
            n = self.cor[c]["name"] if c in self.cor else ""
            # (a routine sent to the port's by address is the runtime's as well: under the CRT's
            # FID-unnamed _mktime64 and wcstombs, the reference's common_mktime and
            # _wcstombs_l_helper were translated and called __acrt_getptd, stubbed, 2026-10-18)
            if c in CRT_BY_ADDR or c in CRT_ENTRY:
                return True
            # (a lambda in the runtime's own code is the runtime's: __crt_seh_guarded_call's
            # `operator()` bodies, through which _close reaches _close_nolock)
            return self.is_lib(c) and ("<lambda" not in n and "operator()" not in n or crt_block(c))

        def internal_callee(b: int) -> bool:
            # a runtime internal: in the runtime's code and not one of the port's routines
            return crt_block(b) and b not in CRT_BY_ADDR and b not in CRT_ENTRY and b not in self.crt_thunks

        # and a function nothing calls (an exception funclet, a slot in one of the runtime's
        # tables) that calls the runtime's internals by name: the runtime's too (the CRT's
        # `__finally` funclets at 0x1425d4xxx unlocking with __acrt_unlock, its locale tables'
        # free routines; 2026-10-18)
        internal = [re.compile(r["pattern"]) for r in self.exceptions.get("third_party_names", [])]
        called_internal: set[int] = set()
        for a in cands:
            # (in the runtime's own code, or among the funclets the image keeps at its end: a
            # game method nothing calls but its vtable can use __std_exception_copy and is kept)
            if callers.get(a) or not (crt_block(a) or a >= 0x1425d4000):
                continue
            c = self.cor[a].get("c", "")
            for n in set(re.findall(r"\b([A-Za-z_][\w:<>,]*)\s*\(", c[c.find("{"):])):
                if any(p.search(n) for p in internal):
                    called_internal.add(a)
                    break
            else:
                # (or unnamed ones: vcruntime's exception filter funclets, FUN_1425d48bd calling
                # FUN_1402703b0, which sets __vcrt_getptd's rethrow flag. Every callee internal,
                # so a game funclet that also frees, through the port's free, is kept)
                bs = {int(x, 16) for x in FUNREF.findall(c[c.find("{"):])} - {a}
                if bs and all(internal_callee(b) for b in bs):
                    called_internal.add(a)
        # the reference's own entry: the protection's loader calls it, not code in the corpus, so
        # it has no caller here, and the CRT startup under it (__scrt_common_main_seh, the CRT's
        # initialization, its initializer tables) was translated and called the runtime's insides
        # (__vcrt_uninitialize, __acrt_execute_initializers: stubs, 2026-10-18). FrostLite has its
        # own entry and runtime
        startup = {a for a in cands if re.sub(r"^FID_conflict[:_]", "", self.cor[a]["name"])
                   in ("WinMainCRTStartup", "wWinMainCRTStartup", "mainCRTStartup", "wmainCRTStartup")}
        seeds = called_internal | startup
        # The greatest set whose every caller is the runtime's or in the set, not the least: the
        # runtime's routines call each other (strftime's expand_time and store_winword), and grown
        # from the outside in, neither of a pair could go first (both translated, stubs under
        # them, 2026-10-18). Then only what the runtime or a seed reaches through the set: two game
        # functions calling each other, otherwise reached through a vtable, are no runtime's
        dropped: set[int] = {a for a in cands if callers.get(a)} | seeds
        changed = True
        while changed:
            changed = False
            for a in list(dropped - seeds):
                if any(not runtime(c) and c not in dropped for c in callers[a]):
                    dropped.discard(a)
                    changed = True
        reached = set(seeds) | {a for a in dropped if any(runtime(c) for c in callers.get(a, ()))}
        changed = True
        while changed:
            changed = False
            for a in dropped - reached:
                if any(c in reached for c in callers[a]):
                    reached.add(a)
                    changed = True
        dropped = reached
        print(f"--all: {len(dropped)} runtime routines only the runtime calls, left out", file=sys.stderr, flush=True)
        return [a for a in roots if a not in dropped]

    def native_ranges(self) -> list[tuple[int, int]]:
        return [(int(r["from"], 16), int(r["to"], 16)) for r in self.exceptions.get("native", [])] + \
            sorted(r for rs in self.__dict__.get("_leaf_ranges", {}).values() for r in rs)

    def text_data_ranges(self) -> list[tuple[int, int]]:
        """The data inside .text the translated code reads: switch tables the compiler put after
        a function's code (`(&REFG(uint, 0x14235c424))[mode - 0x3f34]`, zlib's inflate) and other
        constants there. RefImage fills .text with int3, so such a table read as all 0xcc: no case
        of inflate's state switch matched, it looped forever, and the client hung on the realm
        list blob (2026-10-20). An address outside every .pdata piece is data, mapped from the
        image up to the next piece; code stays int3."""
        text = self.ref.text
        starts, ends = self.ref.piece_starts, self.ref.piece_ends
        found = set()
        for path in sorted((ROOT / "src/gen").glob("translated_*.c")):
            for m in re.finditer(r"\b(?:REFG\([^,()]+(?:\([^()]*\))?[^,()]*, |REFP\(|\(uintptr_t\))0x(1[0-9a-f]{8})",
                                 path.read_text(encoding="utf-8", errors="replace")):
                t = int(m.group(1), 16)
                if text.start <= t < text.end:
                    found.add(t)
        out = []
        for t in sorted(found):
            i = bisect.bisect_right(starts, t) - 1
            if (i >= 0 and t < ends[i]) or t in self.cor:
                continue          # inside a piece, or a function (a leaf has no .pdata): code
            j = bisect.bisect_right(starts, t)
            hi = min(starts[j] if j < len(starts) else text.end, t + 0x4000)
            if out and t < out[-1][1]:
                continue
            out.append((t, hi))
        # and every switch's tables (tools/switches.py), which the compiler puts inside the
        # function's own .pdata range as often as not (inflate's at 0x14235c424)
        sw = ROOT / "build/facts/switches.json"
        if sw.exists():
            out += [(int(lo, 16), int(hi, 16)) for lo, hi in json.loads(sw.read_text())["tables"]]
        out.sort()
        print(f"{len(out)} data ranges inside .text mapped from the image", file=sys.stderr)
        return out

    def native_leaf(self, a: int) -> list[tuple[int, int]] | None:
        """The code ranges of a function that may run as the reference's own machine code instead
        of a stub: a leaf (no call, no jump out of it, no indirect jump), whose decompilation
        reads no register the decompiler lost (an argument passed outside the ABI, which a
        translated caller would not set), and which touches no global the port keeps for itself
        (the port's object, not the image's copy, is the live one). What the translator could
        not put in C (a pointer-to-array type, AVX2 the 16-byte values cannot hold: libvpx's
        kernels, the C runtime's vectorized strnlen; 22 stubs, 2026-10-18) runs that way."""
        import capstone
        from capstone import x86 as CX
        e = self.cor.get(a)
        if e is None:
            return None
        c = e.get("c", "")
        if re.search(r"\b(in_|unaff_|extraout_)\w+", c[c.find("{"):]):
            return None
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        spans, seen, todo = [], set(), [a]
        while todo:
            x = todo.pop()
            while x not in seen:
                seen.add(x)
                o = self.ref.off(x)
                ins = next(md.disasm(bytes(self.ref.image[o:o + 16]), x), None)
                if ins is None or len(seen) > 20000:
                    return None
                spans.append((x, x + ins.size))
                mn = ins.mnemonic
                op = ins.operands[0] if ins.operands else None
                if mn in ("call", "syscall", "int", "int1", "into"):
                    return None
                for o2 in ins.operands:
                    if o2.type == CX.X86_OP_MEM and o2.mem.base == CX.X86_REG_RIP:
                        t = ins.address + ins.size + o2.mem.disp
                        i = bisect.bisect_right(self.held_sorted, t) - 1
                        if i >= 0 and t < self.held_sorted[i] + max(1, self.held[self.held_sorted[i]].get("size") or 1):
                            return None
                if mn == "jmp":
                    if op is None or op.type != CX.X86_OP_IMM or (op.imm in self.cor and op.imm != a):
                        return None
                    x = op.imm
                    continue
                if mn.startswith("j") or mn.startswith("loop"):
                    todo.append(op.imm)
                elif mn in ("ret", "int3", "ud2", "hlt"):
                    break
                x += ins.size
        # (merged: one range per run of instructions)
        spans.sort()
        out = []
        for lo, hi in spans:
            if out and lo <= out[-1][1]:
                out[-1] = (out[-1][0], max(out[-1][1], hi))
            else:
                out.append((lo, hi))
        return out

    def native_ok(self, a: int) -> bool:
        """Third-party assembly the port runs as the reference's own machine code
        (tools/fidelity_exceptions.json "native"; RefImage.cpp maps those ranges' bytes): a
        function there, called from translated code, unless it reaches out of the image's code
        through an import slot, which the mapped image leaves unresolved. The codecs' SIMD kernels
        have no decompilation that is C (their blocks pass state in registers), and stubbed, the
        63 the codec C side calls returned 0 (2026-10-18).

        Its instructions are followed from the entry, not read straight through: a straight read
        ran into the data a kernel keeps between its blocks and found "calls" out of the range in
        it (FUN_140195660, FUN_14019b510). A call through an import slot is allowed, as RefImage.cpp
        fills the slots (g_genImports); any other call or jump out of the mapped code is not."""
        memo = self.__dict__.setdefault("_native", {})
        if a in memo:
            return memo[a]
        ranges = self.native_ranges()

        def inside(x):
            return any(lo <= x < hi for lo, hi in ranges)

        ok = inside(a)
        if ok:
            import capstone
            from capstone import x86 as CX
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            md.detail = True
            slots = self.import_slots()
            seen, todo = set(), [a]
            while todo and ok:
                x = todo.pop()
                while ok and x not in seen and len(seen) < 50000:
                    seen.add(x)
                    if not inside(x):
                        ok = False
                        break
                    o = self.ref.off(x)
                    ins = next(md.disasm(bytes(self.ref.image[o:o + 16]), x), None)
                    if ins is None:
                        ok = False
                        break
                    mn = ins.mnemonic
                    op = ins.operands[0] if ins.operands else None
                    if mn in ("call", "jmp") and op is not None:
                        if op.type == CX.X86_OP_MEM and op.mem.base == CX.X86_REG_RIP:
                            slot = ins.address + ins.size + op.mem.disp
                            if slot not in slots:
                                ok = False      # a pointer in the data, not an import
                            else:
                                self.__dict__.setdefault("_native_slots", set()).add(slot)
                            if mn == "jmp":
                                break
                        elif op.type == CX.X86_OP_IMM:
                            if not inside(op.imm):
                                ok = False      # out of the mapped code
                            elif mn == "jmp":
                                x = op.imm
                                continue
                        elif mn == "jmp":
                            break               # a table jump within the kernel
                    elif mn.startswith("j") or mn.startswith("loop"):
                        if op is not None and op.type == CX.X86_OP_IMM:
                            todo.append(op.imm)
                    elif mn in ("ret", "int3", "ud2", "hlt"):
                        break
                    x = ins.address + ins.size
        memo[a] = ok
        return ok

    def stateless(self, a: int, depth: int = 0) -> bool:
        """A runtime routine that reads or writes none of the runtime's own state: no store to the
        image's writable data and no load from it, calling only imports and other such routines.
        Translated, it does what it did (`__crt_time_is_leap_year`, `__acrt_GetDateFormatEx` over
        the Windows call, the UTF-8 conversions); a stub returned 0 to the translated runtime code
        game code still reaches through FID-unnamed wrappers (21 stubs, 2026-10-18). The per-thread
        data, the locks and the locale references read state only the reference's startup makes,
        and stay out."""
        memo = self.__dict__.setdefault("_stateless", {})
        if a in memo:
            return memo[a]
        if depth > 6:
            return False
        memo[a] = False     # (until shown: recursion)
        import capstone
        from capstone import x86 as CX
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        data = self.ref.section(".data")
        slots = self.import_slots()
        ok, seen, todo = True, set(), [a]
        while todo and ok:
            x = todo.pop()
            while ok and x not in seen and len(seen) < 4000:
                seen.add(x)
                o = self.ref.off(x)
                ins = next(md.disasm(bytes(self.ref.image[o:o + 16]), x), None)
                if ins is None:
                    ok = False
                    break
                mn = ins.mnemonic
                for op in ins.operands:
                    if op.type == CX.X86_OP_MEM and op.mem.base == CX.X86_REG_RIP:
                        t = ins.address + ins.size + op.mem.disp
                        if data.start <= t < data.end:
                            ok = False      # the runtime's own state
                op = ins.operands[0] if ins.operands else None
                if mn in ("call", "jmp") and op is not None:
                    if op.type == CX.X86_OP_IMM:
                        if op.imm in self.cor and op.imm != a and (mn == "call" or op.imm in self.cor):
                            if not self.stateless(op.imm, depth + 1):
                                ok = False
                            if mn == "jmp":
                                break
                        elif mn == "jmp":
                            x = op.imm
                            continue
                    elif op.type == CX.X86_OP_MEM and op.mem.base == CX.X86_REG_RIP:
                        if ins.address + ins.size + op.mem.disp not in slots:
                            ok = False
                        if mn == "jmp":
                            break
                    else:
                        ok = False          # through a register: what, unknown
                elif mn.startswith("j") or mn.startswith("loop"):
                    if op is not None and op.type == CX.X86_OP_IMM:
                        todo.append(op.imm)
                elif mn in ("ret", "int3", "ud2", "hlt"):
                    break
                x = ins.address + ins.size
        memo[a] = ok
        return ok

    def import_slots(self) -> dict[int, tuple[str, str]]:
        """The image's import slots, as a live client resolved them (build/facts/imports.json):
        slot -> (DLL, name)."""
        if not hasattr(self, "_import_slots"):
            p = ROOT / "build/facts/imports.json"
            if not p.exists():
                p = ROOT / "data/imports.json"
            rows = json.loads(p.read_text(encoding="utf-8")).get("imports", {}) if p.exists() else {}
            self._import_slots = {int(k, 16): (v["dll"], v.get("imported_name") or v["name"])
                                  for k, v in rows.items() if v.get("dll") and (v.get("imported_name") or v.get("name"))}
        return self._import_slots

    def is_lib(self, a: int) -> bool:
        """Library code the port does not translate, memoized: planning asks for every callee of
        every body, and this rebuilt the `lib` address set on each call."""
        memo = self.__dict__.setdefault("_is_lib", {})
        if a in memo:
            return memo[a]
        if not hasattr(self, "_lib_rules"):
            ex = self.exceptions
            self._lib_rules = ([(int(r["from"], 16), int(r["to"], 16)) for r in ex.get("third_party", [])],
                               [re.compile(r["pattern"]) for r in ex.get("third_party_names", [])],
                               {int(k, 16) for k in ex.get("lib", {})})
        ranges, names, lib = self._lib_rules
        e = self.cor.get(a)
        if any(lo <= a < hi for lo, hi in ranges):
            # third-party code by range (tools/fidelity_exceptions.json: the codecs' assembly, the
            # protector's loader)
            out = True
        elif e is None:
            out = False
        else:
            # and by name (the reference CRT's internals), the listed runtime routines, and the
            # CRT's public names
            name = e["name"]
            # (FID's `FID_conflict:_strtoi64` as well as `_strtoi64`)
            bare = re.sub(r"^FID_conflict[:_]", "", name)
            out = any(n.search(name) for n in names) or a in lib or bare in CRT_EXACT or (
                not name.startswith(F.REF_KEEP) and "::" not in name and name.lstrip("_") in CRT) or (
                # a routine the port's C runtime DLLs export (`fclose`, `_wgetcwd`, `abort`): the
                # port's, which a call by that name reaches (named_callee). Translated, the
                # reference's ran on its own runtime's state (lowio handles, locales, the stdio
                # streams) that FrostLite never initializes, through 104 internals stubbed
                # "called, not in the work set" (2026-10-18)
                "::" not in name and bare in crt_exports()) or (
                # vcruntime's exception handling (CatchIt<__FrameHandler3>, BuildCatchObjectInternal,
                # Is_bad_exception_allowed and unnamed helpers): only it reads the image base
                # through _GetImageBase / _GetThrowImageBase, and the port has its own (19
                # demotions, LNK2019, 2026-10-18)
                re.search(r"\b_Get(?:Throw)?ImageBase\s*\(", e.get("c", "")) is not None)
        memo[a] = out
        return out

    def dead_check_warnings(self, a: int, text: str) -> bool:
        """Ghidra's damage warnings all about a skipped check's dead body. The decompiler decodes
        every block of the function before it drops the unreachable ones, and a check's crash path
        is junk: CreateFrame's core, FUN_1424db710, warned of an overlap at 0x1424dc1ea inside its
        second check (0x1424dbf40, skipped to 0x1424dc3b7) with nothing of it in the code, and was
        kept as protection junk (2026-10-19). True when the body never hits bad data
        (`halt_baddata`) and every address the warnings name lies in a listed check's span, past
        its start's jump and before where the code resumes, in this function."""
        head, _sep, body = text.partition("{")
        if "halt_baddata" in body or "Control flow encountered bad instruction data" in body:
            return False
        named = [int(x, 16) for x in re.findall(r"overlaps instruction at \(ram,0x([0-9a-f]+)\)|Instruction at \(ram,0x([0-9a-f]+)\)",
                                                 head) for x in x if x]
        if not hasattr(self, "_check_spans"):
            p = ROOT / "build/facts/retcheck.json"
            checks = json.loads(p.read_text())["checks"] if p.exists() else {}
            self._check_spans = sorted((int(s, 16) + 7, int(t, 16)) for s, t in checks.items() if int(t, 16) > int(s, 16) + 7)
        root = self.ref.func_containing(a)
        spans = [(lo, hi) for lo, hi in self._check_spans if self.ref.func_containing(lo) == root]
        return bool(spans) and all(any(lo <= x < hi for lo, hi in spans) for x in named)

    def why_not(self, a: int, root: bool, demotion: bool = True) -> str | None:
        if demotion and f"{a:x}" in self.demoted:
            return "did not compile: " + self.demoted[f"{a:x}"]
        if a in self.protection:
            # (the report leaves out reasons starting "anti-tamper": protection, not code to port)
            return "anti-tamper runtime (tools/aegis.py): protection, not ported"
        if a in self.fragments:
            return f"anti-tamper junk between two functions, no function ({self.fragments[a]})"
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
            if hits and reason.startswith("a decompilation Ghidra marked") and self.dead_check_warnings(a, text):
                continue
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
    def converts_numbers(self, a: int) -> bool:
        """Whether the function's instructions convert between integers and floats anywhere
        (cvtsi2sd, cvttsd2si and the rest, x87 fild/fist): without one, every cast the decompiler
        shows between a float local and an integer is the same bits read as the other kind."""
        cache = self.__dict__.setdefault("_converts", {})
        if a not in cache:
            e = self.cor.get(a)
            if not e:
                cache[a] = True
                return True
            import capstone
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            md.skipdata = True
            # every piece .pdata gives the function (the compiler moves cold blocks out of line),
            # and the decompiled extent
            if not hasattr(self, "_pieces_by_root"):
                self._pieces_by_root = defaultdict(list)
                for s, t, r in zip(self.ref.piece_starts, self.ref.piece_ends, self.ref.piece_root):
                    self._pieces_by_root[r].append((s, t))
            spans = self._pieces_by_root.get(a, []) + [(a, a + e.get("size", 0))]
            # (scalar and packed, SSE and AVX: cvtsi2ss, cvttsd2si, cvtdq2ps, vcvttps2dq...; the
            # packed ones carry no "si": FUN_14124c950's `(float)(int)*(short *)` is cvtdq2ps)
            conv = re.compile(r"v?cvtt?(?:\w*si\w*|\w*dq\w*|\w*pi\w*)$")
            cache[a] = any(conv.match(i.mnemonic) or i.mnemonic.startswith(("fild", "fist"))
                           for s, t in spans
                           for i in md.disasm(bytes(self.ref.image[s - self.ref.base:t - self.ref.base]), s))
        return cache[a]

    def pointer_stub_calls(self, text: bytes, tbase: int) -> None:
        """A stub `jmp qword ptr [rip+G]` whose slot G statically holds a code address is shown
        by the decompiler as `thunk_FUN_<that address>`: the slot's value at export time. A
        slot in .data is written at run time, so the call goes wherever it points then. Curl's
        Curl_resolver_cleanup 0x14216f890 is `jmp [Curl_cfree]`, read as a thunk to the CRT's
        free: Curl_close freed a block from the Storm-backed Curl_ccalloc with the CRT's heap,
        and the web login died of heap corruption (2026-10-20). Calls to such a stub are put
        back as the call through the slot, the way the decompiler prints those."""
        stubs: dict[int, tuple[int, int]] = {}   # stub -> (slot, initial target)
        # (no padding test: 0x14216f890 directly follows the previous function's last byte;
        # what makes a match a stub is that the function naming the thunk calls it, below)
        for m in re.finditer(rb"(?=(\x48?\xff\x25)(....))", text, re.S):
            a = tbase + m.start(1)
            slot = a + len(m.group(1)) + 4 + int.from_bytes(m.group(2), "little", signed=True)
            sec = self.ref.section_of(slot)
            if not sec or sec.exec or sec.name != ".data":
                continue
            init = self.ref.u64(slot)
            if self.ref.is_code(init):
                stubs[a] = (slot, init)
        by_init: dict[int, list[int]] = defaultdict(list)
        for a, (_slot, init) in stubs.items():
            by_init[init].append(a)
        for f, e in self.cor.items():
            names = [i for i in by_init if f"thunk_FUN_{i:09x}(" in e.get("c", "")]
            if not names:
                continue
            code = bytes(self.ref.image[f - self.ref.base:f - self.ref.base + e.get("size", 0)])
            called = set()
            for m in re.finditer(rb"[\xe8\xe9](....)", code, re.S):
                called.add(f + m.end() + int.from_bytes(m.group(1), "little", signed=True))
            for i in names:
                hit = [s for s in by_init[i] if s in called]
                if len(hit) == 1:
                    e["c"] = re.sub(rf"\bthunk_FUN_{i:09x}\(", f"(*PTR_FUN_{stubs[hit[0]][0]:09x})(", e["c"])

    def addressed_literals(self, rest: str) -> str:
        """A string literal indexed by an expression holding a negative absolute address
        (`("Set-Cookie:" + i)[*p + -0x14273cbc3 + j]`) is the reference's copy of the string: the
        compiler compared two buffers by their distance, and the decompiler folded one end into
        the literal. With the port's own copy of the literal the sum is a wild pointer; the web
        login's cookie scan faulted after the form arrived (FUN_141493e70, 2026-10-20). The
        literal becomes its reference address, found in the image just below the constant."""
        def literal_bytes(s: str) -> bytes:
            return s.encode("latin-1").decode("unicode_escape").encode("latin-1")

        def fix(m):
            lit, var, idx = m.group(1) if m.group(1) is not None else m.group(3), m.group(2), m.group(4)
            c = int(re.search(r"-0x(1[0-9a-f]{8})", idx).group(1), 16)
            want = literal_bytes(lit) + b"\0"
            lo = c - 0x400 - self.ref.base
            hay = bytes(self.ref.image[lo:c + 0x40 - self.ref.base])
            hits = [lo + self.ref.base + k for k in range(len(hay)) if hay.startswith(want, k)]
            if not hits:
                return m.group(0)
            addr = min(hits, key=lambda x: abs(c - x))
            inner = f"((char *)REFP(0x{addr:x}) + {var})" if var else f"((char *)REFP(0x{addr:x}))"
            return f"{inner}[{idx}]"

        return re.sub(r'(?:\("((?:[^"\\]|\\.)*)" \+ (\w+)\)|"((?:[^"\\]|\\.)*)")'
                      r'\[([^\[\]]*-0x1[0-9a-f]{8}[^\[\]]*)\]', fix, rest)

    def rip_targets(self, a: int) -> set[int]:
        """The addresses the function's instructions reach rip-relative (lea and memory operands),
        over all its pieces."""
        cache = self.__dict__.setdefault("_rip_targets", {})
        if a not in cache:
            import capstone
            from capstone import x86 as CX
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            md.detail = True
            md.skipdata = True
            e = self.cor.get(a, {})
            if not hasattr(self, "_pieces_by_root"):
                self._pieces_by_root = defaultdict(list)
                for s, t, r in zip(self.ref.piece_starts, self.ref.piece_ends, self.ref.piece_root):
                    self._pieces_by_root[r].append((s, t))
            out = set()
            for s, t in self._pieces_by_root.get(a, []) + [(a, a + e.get("size", 0))]:
                for i in md.disasm(bytes(self.ref.image[s - self.ref.base:t - self.ref.base]), s):
                    if i.id == 0:   # (skipped data)
                        continue
                    for op in i.operands:
                        if op.type == CX.X86_OP_MEM and op.mem.base == CX.X86_REG_RIP:
                            out.add(i.address + i.size + op.mem.disp)
                        elif op.type == CX.X86_OP_MEM and 0x1000 <= op.mem.disp < len(self.ref.image):
                            # (image-base relative: `lea rsi, [0x140000000]` then
                            # `[rsi + rax + 0x273ce90]`, a table the compiler indexes by RVA)
                            out.add(self.ref.base + op.mem.disp)
            cache[a] = out
        return cache[a]

    def indexed_literals(self, a: int, rest: str) -> str:
        """A string literal indexed by a computed value is the data at its address, not a C string:
        rapidjson's writer indexes "0123456789ABCDEF" plus its escape table, which runs on through
        NULs the decompiler's literal stops at (`"0123456789ABCDEFuuuu..."[c + 0x10]`). With the
        port's copy, every character past the literal read whatever followed it: the web login's
        JSON came out escaped letter by letter and FrostForge read no account name (2026-10-20).
        It becomes the reference's copy the function itself addresses."""
        lits = re.compile(r'(?:\("((?:[^"\\]|\\.)*)" \+ (\w+)\)|"((?:[^"\\]|\\.)*)")\[([^\[\]]+)\]')
        if not lits.search(rest):
            return rest
        targets = None

        def fix(m):
            nonlocal targets
            lit, var, idx = m.group(1) if m.group(1) is not None else m.group(3), m.group(2), m.group(4)
            if re.fullmatch(r"\s*(?:0x[0-9a-f]+|\d+)\s*", idx) and var is None:
                return m.group(0)     # a constant index inside the literal is the literal
            try:
                want = lit.encode("latin-1").decode("unicode_escape").encode("latin-1") + b"\0"
            except (UnicodeDecodeError, UnicodeEncodeError):
                return m.group(0)
            if targets is None:
                targets = self.rip_targets(a)
            # (the instructions may address a table inside the literal's data, rapidjson's escape
            # table 0x10 past its hex digits: the literal starts at or a little below a target)
            hits = set()
            for t in targets:
                lo = t - 0x100 - self.ref.base
                hay = bytes(self.ref.image[max(lo, 0):t + 1 - self.ref.base + len(want)])
                k = hay.find(want)
                while k >= 0:
                    if lo + k + self.ref.base <= t:
                        hits.add(lo + k + self.ref.base)
                    k = hay.find(want, k + 1)
            hits = sorted(hits)
            if len(hits) != 1:
                return m.group(0)
            inner = f"((char *)REFP(0x{hits[0]:x}) + {var})" if var else f"((char *)REFP(0x{hits[0]:x}))"
            return f"{inner}[{idx}]"

        return lits.sub(fix, rest)

    STACK_LIST_APPEND = re.compile(
        r"(?P<tail>\w+) = \*\(longlong \*\)\((?P<bucket>\w+) \+ 8\);(?P<s1>\s*)"
        r"\*(?P<link>\w+) = (?P=tail);(?P<s2>\s*)"
        r"(?P=link)\[1\] = \*\(longlong \*\)\((?P=tail) \+ 8\);(?P<s3>\s*)"
        r"\*\([\w \*]+\)\((?P=tail) \+ 8\) = local_\w+;(?P<s4>\s*)"
        r"\*\(longlong \*\*\)\((?P=bucket) \+ 8\) = (?P=link);")

    @classmethod
    def stack_list_append(cls, rest: str) -> str:
        """TSHashTable's rehash (89 instances, FUN_1419c7450 the first found): the re-bucket loop
        takes the head node off a list on its own stack and appends it to its new bucket. The
        asm keeps that node in a register; Ghidra re-reads the stack head after the unlink, which
        wrote it through a pointer, and so appends the next node instead (the default alias
        blocking stops a stack pointer's alias at an array). Half of every rehashed table was left
        on the dead stack list, and the cache clear on entering the world never ended
        (2026-10-21). The node is its link less the bucket list's link offset (the int at the
        bucket's start). ExportDecompAll now decompiles with alias blocking off; this covers a
        corpus exported before that."""
        def fix(m):
            return (f"{m.group('tail')} = *(longlong *)({m.group('bucket')} + 8);{m.group('s1')}"
                    f"*{m.group('link')} = {m.group('tail')};{m.group('s2')}"
                    f"{m.group('link')}[1] = *(longlong *)({m.group('tail')} + 8);{m.group('s3')}"
                    f"*(longlong *)({m.group('tail')} + 8) = (longlong){m.group('link')} - "
                    f"(longlong)*(int *){m.group('bucket')};{m.group('s4')}"
                    f"*(longlong **)({m.group('bucket')} + 8) = {m.group('link')};")
        return cls.STACK_LIST_APPEND.sub(fix, rest)

    @staticmethod
    def handed_on_tail_call(head: str, rest: str) -> str:
        """An open function's one indirect call with no arguments, in a body that never writes
        its parameters, is the `jmp [ptr]` it leaves through: the registers it came in with go on
        unchanged, so its parameters are that call's arguments. The decompiler shows the jump as
        a call of nothing; curl's Curl_ssl_data_pending FUN_1421784c0 (`jmp [Curl_ssl + 0x48]`)
        then called the Schannel backend with garbage for its connection (2026-10-20)."""
        names = re.findall(r"\b(param_\d+)\b", head)
        if not names or any(re.search(rf"(?<![=!<>])\b{p}\s*=[^=]", rest) for p in names):
            return rest
        call = re.compile(r"(\(\*+\(code \*+\)(?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)|\(\*\w+\))\(\)")
        if len(call.findall(rest)) != 1:
            return rest
        return call.sub(lambda m: f"{m.group(1)}({', '.join(names)})", rest)

    def rewrite(self, a: int, text: str, uses: set) -> str:
        """The decompiled function as C: callees by address, globals at their addresses."""
        head, rest = text.split("{", 1)
        sig = self.signature(a)
        ret, _, params = sig
        own_name = self.cor[a]["name"] if a in self.cor else ""
        if a in self.open_protos:
            rest = self.handed_on_tail_call(head, rest)
        rest = self.stack_list_append(rest)
        rest = self.addressed_literals(rest)
        rest = self.indexed_literals(a, rest)
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
        body = param_slots(stack_frame(atomics("{" + rest), params), params)
        # FLT_MAX as Ghidra prints it, shortest: a double literal past FLT_MAX that the compiler
        # rounds back down and flags as an overflow (C4056/C4756 at 20 sites in 8 functions);
        # the float.h constant is the exact value
        body = re.sub(r"(?<![\w.])3\.4028235e\+38(?![\w.])", "FLT_MAX", body)
        # Ghidra's `long` is 8 bytes and it writes an 8-byte constant `1L`; MSVC's long is 4, so
        # `1L << (bVar22 & 0x3f)` shifted 32 bits and every bit past 31 of a 64-bit mask was lost
        # (C4334 at 37 sites in 31 functions, 2026-10-17)
        body = "".join(re.sub(r"(?<![\w.])((?:0x[0-9a-fA-F]+|\d+)U?)L\b", r"\1LL", piece) if k % 2 == 0 else piece
                       for k, piece in enumerate(re.split(r'("(?:\\.|[^"\\\n])*")', body)))
        # a `code *` local dereferenced as data: Ghidra merged a byte cursor with a code pointer
        # (FUN_141ce0a80's strcpy loop, `*pcVar28 = *pcVar37;`), and C reads `*pcVar28` as the
        # function itself: an assignment to it is C4550 and stores nothing. A call, `(*pcVar28)(`,
        # stays a call
        body = self.literal_compares(a, body)
        # the address of a frame local returned: what rax was left holding (FUN_1402e1000's loop
        # sentinel `lea rax, [rbp-0x40]`, FUN_1418b28d0's), a pointer into a frame that is gone.
        # No caller of either reads it (their next instructions do not touch rax); C4172, 2026-10-17
        body = re.sub(r"^([ \t]*)return\s+(?:\((?:\w+\s*)+\*+\s*\)\s*)?(?:&\(\*\([^;]*?\)\(gen_frame \+ 0x[0-9a-f]+\)\)"
                      r"|\(\((?:\w+\s*)+\*+\)\(gen_frame \+ 0x[0-9a-f]+\)\));$", r"\1return 0;", body, flags=re.M)
        for name in re.findall(r"^\s*code\s*\*\s*(\w+)\s*;", body, re.M):
            data = re.compile(rf"(?<![\w)\]])\*\s*{name}\b(?!\s*\))")
            body = "\n".join(l if re.match(rf"^\s*code\s*\*\s*{name}\s*;", l) else data.sub(f"(*(undefined1 *){name})", l)
                             for l in body.split("\n"))
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
        # (a field of a label Ghidra gave a structure no header has, `&UNK_140000080.field_0x4`, the
        # PE header's machine word read through the DOS header's e_lfanew in FUN_141236c80: the
        # address that many bytes on, and without the `&` the byte there; C2224, 2026-10-18)
        body = re.sub(r"&\s*\b(?:UNK|DAT)_(1[0-9a-f]{8})\.field_0x([0-9a-f]+)\b",
                      lambda m: f"((undefined1 *)REFP(0x{int(m.group(1), 16) + int(m.group(2), 16):x}))", body)
        body = re.sub(r"\b(?:UNK|DAT)_(1[0-9a-f]{8})\.field_0x([0-9a-f]+)\b",
                      lambda m: f"(*(undefined1 *)REFP(0x{int(m.group(1), 16) + int(m.group(2), 16):x}))", body)
        # (and `e_lfanew`, the offset of the PE header, the int at +0x3c)
        body = re.sub(r"\bIMAGE_DOS_HEADER_140000000\.e_lfanew\b", "(*(int *)REFP(0x14000003c))", body)
        # (the "MZ" test, `e_magic == (char  [2])0x5a4d`: the 2 bytes there against the number; as
        # the base address it was a pointer compared with an array cast, C2067, 2026-10-18)
        body = re.sub(r"\bIMAGE_DOS_HEADER_140000000\.e_magic\s*([!=]=)\s*\(char\s*\[2\]\)\s*(0x[0-9a-f]+)",
                      r"(*(unsigned short *)REFP(0x140000000)) \1 \2", body)
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
            # (and a call that reaches the return through the end of an if or else: both branches
            # of FUN_142237350, ExtensionSet::ParseField, end in a call, the field skipper's or
            # ParseFieldWithExtensionInfo's, and meet at one `return;`. As `return 0` every
            # extension failed to parse, protobuf's MessageOptions with it, and the first
            # descriptor with a custom option, ClientException.proto's, aborted the client at
            # startup, 2026-10-19)
            def returns_value(call: str) -> bool:
                fm = re.match(r"(?:thunk_)?FUN_([0-9a-f]{9})\(", call)
                if fm:
                    d = self.decl(int(fm.group(1), 16))
                    return d is not None and d[0].strip() != "void"
                nm = re.match(r"([A-Za-z_]\w*(?:::~?\w+)+)\(", call)
                if nm:
                    target = self.named_callee(nm.group(1), own_name)
                    if isinstance(target, int):
                        d = self.decl(target)
                        return d is not None and d[0].strip() != "void"
                    return target is None or target not in VOID_RUNTIME
                return True

            body = carry_tail_calls(body, returns_value)
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
        # an unqualified name inside a namespace, which is how the decompiler prints one of its
        # members there: `Lua::GetAuctionItemInfo` passes itself as `GetAuctionItemInfo` (a
        # handler registered with the auction cache, 7 handlers demoted as undeclared, 2026-10-17)
        if "::" in own_name:
            scope = own_name.rsplit("::", 1)[0]

            def scoped(m):
                # (not the class's own name, its constructor's, which a cast spells as a type)
                if m.group(1) == scope.rsplit("::", 1)[-1]:
                    return m.group(0)
                cands = self.by_name.get(f"{scope}::{m.group(1)}", [])
                return f"((code *)REFP(0x{cands[0]:x}))" if len(cands) == 1 else m.group(0)

            # (outside string and character literals: the usage messages name the handler too)
            parts = re.split(r"(\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*')", body)
            body = "".join(p if k % 2 else re.sub(r"(?<![\w:.>])([A-Z]\w*)\b(?!\s*[(:*])", scoped, p)
                           for k, p in enumerate(parts))
        body = re.sub(r"&?\b([A-Za-z_][\w<>,]*(?:::[\w<>,~]+)*::vftable)\b",
                      lambda m: f"((undefined8 *)REFP(0x{VFTABLES[m.group(1)]:x}))" if m.group(1) in VFTABLES
                      else m.group(0), body)
        # (and one printed bare, inside its own class's namespace: std::exception's copy
        # constructor stores `*param_1 = vftable;`, C2065, 2026-10-18. The table the function's own
        # instructions load, when they load exactly one)
        if re.search(r"(?<![\w:.>])vftable\b", body) and a in self.cor:
            from capstone import x86 as CX
            tables = set(VFTABLES.values())
            mine = {i.address + i.size + i.operands[1].mem.disp
                    for i in self.callargs._function(a, self.cor[a]["size"]).ins
                    if i.mnemonic == "lea" and len(i.operands) == 2 and i.operands[1].type == CX.X86_OP_MEM
                    and i.operands[1].mem.base == CX.X86_REG_RIP} & tables
            if len(mine) == 1:
                body = re.sub(r"&?(?<![\w:.>])vftable\b", f"((undefined8 *)REFP(0x{next(iter(mine)):x}))", body)

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
        # the carried result of a callee that returns 16 bytes (a float or vector in xmm0, where
        # this one's caller reads rax): the low qword, as `(undefined8)` cannot convert a
        # gen_v16 (C2440, 4 functions, 2026-10-17)
        body = re.sub(r"^([ \t]*)return \(undefined8\)(CALLF\(gen_v16, @FUN_[0-9a-f]{9}\)\([^;]*\));$",
                      r"\1return gen_v16_q0(\2);", body, flags=re.M)
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
            # (not where Ghidra spells it `PTR_`: it kept the pointer in the expression, and a
            # movsd only copied it. FUN_141fb2700 copies a table of handlers into an object, the
            # last through xmm0, and `puVar1 = REFG(double, ...)` was C2440, 2026-10-19)
            if gtype.endswith("*") and a in self.cor and addr not in derefed and not whole.startswith("PTR_"):
                # (a pointer by the data export that the instructions read only as a float or a
                # double, `movss xmm1, [0x14297aad0]`: the scalar. As `float *`, stored into a
                # float and compared with one, C2440 in FUN_14247b930 and FUN_14249eec0,
                # 2026-10-18)
                seen = {(n, k) for n, k in self.callargs.global_accesses(a, self.cor[a]["size"]).get(addr, set()) if n <= 8}
                if len(seen) == 1 and next(iter(seen))[1] == "x":
                    size = next(iter(seen))[0]
                    gtype = "float" if size == 4 else "double"
            if (wide or (addr not in self.refdata and addr not in derefed)) and a in self.cor:
                # Ghidra's `_DAT_x` is an access wider (or of another kind) than the item defined
                # at x, so that item's type is the wrong one: the instructions say what is read.
                # FUN_141283140 divides by `_DAT_142cfbe58`, a float load factor
                # (`divss xmm0, dword ptr [0x142cfbe58]`) over an undefined1, and read one byte of
                # it: 0, a quotient of inf and a bucket count that wrapped (2026-10-15).
                # (a vector access at x, 16 bytes, says nothing of a scalar's width: the pieces
                # rule below takes it)
                seen = {(n, k) for n, k in self.callargs.global_accesses(a, self.cor[a]["size"]).get(addr, set()) if n <= 8}
                if len(seen) == 1:
                    n, kind = next(iter(seen))
                    t = {(4, "x"): "float", (8, "x"): "double", (1, "i"): "undefined1", (2, "i"): "undefined2",
                         (4, "i"): "undefined4", (8, "i"): "undefined8"}.get((n, kind))
                    if t is not None and (n != size or (kind == "x") != (gtype in ("float", "double"))):
                        size, gtype = n, t
                elif wide and seen and all(k == "i" for _n, k in seen):
                    # (several integer widths at x: Ghidra's wider view is the widest of them.
                    # FUN_1402ef340 reads `_DAT_142e25a14` a byte at a time and writes it as a
                    # dword; at the item's one byte, `>> 0x18` was always 0, C4333, 2026-10-17)
                    n = max(n for n, _k in seen)
                    if n > size:
                        size, gtype = n, f"undefined{n}"
                elif not seen and not gtype.startswith(("float", "double")):
                    # (no access at x itself: x is inside a wider access from below, a 16-byte
                    # load, which Ghidra splits into the pieces the function names. x's piece runs
                    # to the next one named, or to the access's end, at most 8 bytes.
                    # `_DAT_14261aa18 >> 0x20` on the 4-byte item was undefined (C4293 in
                    # FUN_1418b21a0), and FUN_1420c6200's 4-byte pieces of the constant at
                    # 0x1428e4a80 took the 8-byte default (C4319), 2026-10-17)
                    allacc = self.callargs.global_accesses(a, self.cor[a]["size"])
                    ends = [y + w for y, ws in allacc.items() for w, _k in ws if y <= addr < y + w and w > 8]
                    named = sorted(int(x, 16) for x in re.findall(r"\b_?(?:DAT|UNK)_([0-9a-f]{9})\b", body))
                    nxt = next((x for x in named if x > addr), None)
                    if ends:
                        end = max(ends) if nxt is None else min(max(ends), nxt)
                        n = min(8, end - addr)
                        if n in (1, 2, 4, 8) and (wide and n != size or addr not in self.refdata):
                            size, gtype = n, f"undefined{n}"
                    elif not wide and addr not in self.refdata:
                        # (no item and no access at x: reached through a base register, a table
                        # indexed `(&DAT_x)[i]`. Ghidra's plain DAT_ over nothing defined is its
                        # `undefined`, one byte, and so is the element; an access wider would be
                        # `_DAT_`. curl's character classes at 0x1428fd410 were indexed 8 bytes
                        # apart, isalnum('h') read garbage, the URL had no scheme, and the web
                        # login failed with CURLE_URL_MALFORMAT, 2026-10-20)
                        size, gtype = 1, "undefined1"
                # (and read 8 bytes wide and dereferenced, `lVar1 = *_DAT_142d01560;`: a pointer, to
                # what its casts say or to 8-byte slots)
                if wide and size == 8 and gtype.startswith("undefined") and addr in derefed:
                    gtype = self.global_pointers.get(addr, "undefined8 *")
            # the width the decompiler built the expression around, where it gave the global one
            # (the corpus's "g", ExportDecompAll's p-code varnodes): a lane of a 16-byte constant,
            # `~uVar8 & _UNK_142725a78` with uVar8 a uint, took the 8 bytes to the access's end
            # above, and the compare of a lane against 8 bytes of the constant was C4319 (four
            # functions, 2026-10-18). Ghidra's one width is the expression's; several, the rules'.
            gw = gwidths.get(f"{addr:x}")
            if gw and len(gw) == 1 and gw[0] in (1, 2, 4, 8) and gw[0] != size \
                    and (gtype.startswith("undefined") or gtype in ("float", "double")):
                n = gw[0]
                size = n
                gtype = ("float" if n == 4 else "double") if gtype in ("float", "double") and n in (4, 8) \
                    else f"undefined{n}"
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
        gwidths = self.cor[a].get("g") or {} if a in self.cor else {}
        # (`PTR_PTR_x` too, a pointer to a pointer with no DAT_ in its name: FUN_1420fab00 indexes
        # `PTR_PTR_142d3e690[0x38]`, refused as a global used as a pointer, 2026-10-18)
        derefed = {int(x, 16) for pair in re.findall(r"\*_?(?:(?:PTR_)+(?:DAT_)?|DAT_)([0-9a-f]{9})\b|"
                                                     r"(?<!\w)_?(?:(?:PTR_)+(?:DAT_)?|DAT_)([0-9a-f]{9})\[", body)
                   for x in pair if x}
        vtable_reads = {int(x, 16) for x in re.findall(r"\(\s*\*_?DAT_([0-9a-f]{9})\s*\+\s*0x[0-9a-f]+\s*\)", body)}

        # an untyped 8-byte global the function copies to or from a pointer local
        # (`piVar10 = DAT_142de0de8;`): the local's type is the global's
        local_types = {m.group(2): re.sub(r"\s+", " ", m.group(1)).strip()
                       for m in re.finditer(r"^\s*((?:\w+ )*\w+ ?\*+)\s*(\w+);$", body, re.M)}
        borrowed = {}
        # (GLOBAL_COPY's pattern: a store through the local, `*pfVar34 = _DAT_x;`, is no copy)
        for m in GLOBAL_COPY.finditer(body):
            var, addr = (m.group(1), m.group(2)) if m.group(1) else (m.group(4), m.group(3))
            ctype = local_types.get(var)
            if ctype and ctype.endswith("*"):
                borrowed.setdefault(int(addr, 16), ctype.replace(" *", "*").replace("*", " *").strip())
        # or the global plus an offset into a pointer local, uncast (`puVar10 = DAT_1430af020 + 4;`):
        # in Ghidra's C the global is that pointer and the offset counts elements. As an integer
        # the Battle.net game-account list was walked 4 bytes in, not 4 qwords, the account's
        # name was never found, and the login stopped after GetGameAccountState (2026-10-20)
        for m in re.finditer(r"(?<![\w*.>\]])(\w+) = _?DAT_([0-9a-f]{9}) [+-] ", body):
            ctype = local_types.get(m.group(1))
            if ctype and ctype.endswith("*"):
                borrowed.setdefault(int(m.group(2), 16), ctype.replace(" *", "*").replace("*", " *").strip())
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
        # and an UNK_ label always: Ghidra names an address UNK_ because no item starts there (it
        # is inside a wider one the data export types), so `&UNK_x + n` is a byte offset whatever
        # the export says. Ed25519's field constants in FUN_1420beaa0 (`*(uint *)(&UNK_1428e1ddc +
        # lVar6)`) were read 4 and 8 times too far apart, the signature check of
        # ENTER_ENCRYPTED_MODE failed and the client dropped the world connection (2026-10-20)
        body = re.sub(r"&\s*(_?UNK_([0-9a-f]{9}))(?=\s*[+-][^+-=>])",
                      lambda m: f"(undefined1 *)&{m.group(1)}", body)
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
        # (Ghidra's prefix is the kind, not the width: `uRam0000000142ddedb0 >> 0x20` is 8 bytes,
        # and as 4 it was C4293 in FUN_1401b94f0; the decompiler's own width first)
        def ram(m):
            gw = gwidths.get(m.group(2))
            n = gw[0] if gw and len(gw) == 1 and gw[0] in (1, 2, 4, 8) else \
                {'u': 4, 'l': 8, 'c': 1, 's': 2}.get(m.group(1)[0], 8)
            return f"REFG(undefined{n}, 0x{m.group(2)})"
        body = RAM.sub(ram, body)
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
        body = mistyped_float_slots(body)
        if not self.converts_numbers(a):
            body = reinterpreted_float_locals(body)
        # a function-local static's guard is compared with the epoch signed (MSVC's `jle`); the
        # data export types the guard undefined4, which made `_Init_thread_epoch < guard` unsigned:
        # the epoch is negative (INT_MIN up), so no translated static ever ran its initializer and
        # each read as zero (Logon's platform FourCC came out empty, 790 sites, 2026-10-20)
        body = re.sub(r"(_Init_thread_epoch < )REFG\(undefined4, ", r"\1REFG(int, ", body)
        body = re.sub(r"(_Init_thread_epoch < \(?)\*\(undefined4 \*\)", r"\1*(int *)", body)
        body = self.float_arguments(body, params)
        # callers are unprototyped, so a float argument arrives promoted to double
        decls, narrow = [], []
        for k, p in enumerate(params):
            m = re.match(r"^(?:float|float4)\s+(\w+)$", p)
            s = re.fullmatch(r"\s*((?:unsigned |signed )*\w+)\s+(param_\d+)\s*", p)
            if m:
                decls.append(f"double {m.group(1)}_promoted")
                narrow.append(f"  float {m.group(1)} = (float){m.group(1)}_promoted;")
            elif k >= 4 and s and s.group(1).split()[-1] in NARROW_TYPES and s.group(1).split()[-1] != "float" \
                    and not any(x.strip() == "..." for x in params):
                # (not a variadic one: translated.h declares those with their parameters as typed)
                # a stack argument is an 8-byte slot, and every caller (unprototyped, through
                # CALLF) writes it whole: declared narrower, the definition disagreed with the
                # call, C4789 where the two met (FUN_141f24bb0 handing FUN_141f198c0's undefined2
                # param_5 0xfffd, 2026-10-17). The value is the slot's low bytes, as the
                # reference's callee reads it
                decls.append(f"unsigned long long {s.group(2)}_slot")
                narrow.append(f"  {s.group(1)} {s.group(2)} = ({s.group(1)}){s.group(2)}_slot;")
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
        body = explicit_casts(body, params)
        body = zero_locals(body)
        # a function that ends in a call that does not return (std::_Xlength_error's throw, a
        # thread's _endthreadex): the reference has int3 after it, not a return value. As C it
        # fell off the end of a non-void function (C4716 in 4 functions, 2026-10-17); a trap there
        # is what the reference does should the call ever come back (the break for a debugger,
        # then the end an unhandled int3 would be; __fastfail is what C knows does not return)
        if ret.strip() != "void":
            # (the call may span lines: a long string argument wraps; C4715 in FUN_141b3a390)
            body = re.sub(r"(/\* WARNING: Subroutine does not return \*/\s*\n(?:[^;\"\n]|\"(?:[^\"\\\n]|\\.)*\"|\n)*;\n)(\}\s*)$",
                          r"\1  __debugbreak();\n  __fastfail(7);\n  __assume(0);\n\2", body.rstrip() + "\n")
            body = fall_off_return(body, ret.strip())
            body = self.dead_frame_return(body)
        self.global_aliases.update(aliases)
        self.exported.update(exported)
        # (this function's own, for rewrite_one to hand back from a worker)
        self._last_aliases, self._last_exported = aliases, exported
        return f"{ret} FUN_{a:x}({', '.join(decls) or 'void'})\n{body.rstrip()}\n"

    def returned_param(self, b: int) -> int | None:
        """The parameter (1-based) a function returns on every path, from its decompilation
        (`return param_2;`), else None."""
        e = self.cor.get(b)
        if not e or "c" not in e:
            return None
        rets = set(re.findall(r"\breturn\s+([^;]*);", e["c"]))
        if len(rets) != 1:
            return None
        m = re.fullmatch(r"(?:\([^()]*\))?param_(\d+)", next(iter(rets)).strip())
        return int(m.group(1)) if m else None

    def dead_frame_return(self, body: str) -> str:
        """A function whose result is its callee's handing back an address in this function's
        own frame: the reference leaves that address in rax as the frame goes, a pointer to stack
        nothing owns any more. FUN_1411bba70 returns FUN_1411cda20's second argument, its own
        local, and its callers all drop it; the translation's would be gen_frame's address, not
        even the reference's, and the compiler's C4172 (2026-10-19). Diverged: zero, which no
        caller can mistake for a live object."""
        if "gen_frame" not in body:
            return body
        out = body
        for rm in re.finditer(r"^([ \t]*)return\s+(\w+)\s*;", body, re.M):
            var = rm.group(2)
            before = body[:rm.start()]
            assigns = list(re.finditer(rf"^[ \t]*{var}\s*=\s*(?:\([^()]*\))?CALLF\([^,]+,\s*FUN_([0-9a-f]{{9}})\)\(",
                                       before, re.M))
            if not assigns:
                continue
            last = assigns[-1]
            # (and nothing assigns it after that call)
            if re.search(rf"^[ \t]*{var}\s*=", before[last.end():], re.M):
                continue
            k = self.returned_param(int(last.group(1), 16))
            if k is None:
                continue
            spans, _close = call_args(before, last.end() - 1)
            if len(spans) < k or "gen_frame" not in before[spans[k - 1][0]:spans[k - 1][1]]:
                continue
            out = out.replace(rm.group(0), f"{rm.group(1)}return 0;  /* the callee's handing back of this "
                                           f"function's own frame: dead once it returns */", 1)
        # (and the call returned directly, a tail call carried: `return (undefined8)CALLF(ulonglong *,
        # FUN_1414f3330)(param_1 + 0x20, &frame..., &frame...)`, C4172 in five functions once
        # carry_tail_calls followed tail calls out of if blocks, 2026-10-19)
        for rm in reversed(list(re.finditer(r"^([ \t]*)return\s+(?:\(\w+[\w *]*\))?(CALLF\([^,]+,\s*FUN_([0-9a-f]{9})\)\()",
                                            out, re.M))):
            k = self.returned_param(int(rm.group(3), 16))
            if k is None:
                continue
            spans, close = call_args(out, rm.end() - 1)
            if len(spans) < k or "gen_frame" not in out[spans[k - 1][0]:spans[k - 1][1]]:
                continue
            end = out.index(";", close) + 1
            call = out[rm.start(2):close + 1]
            out = (out[:rm.start()] + f"{rm.group(1)}{call};\n{rm.group(1)}return 0;  /* the callee's handing back "
                   f"of this function's own frame: dead once it returns */" + out[end:])
        return stack_allocations(out)

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

    def rewrite_one(self, a: int, text: str, adapted) -> tuple[int, tuple]:
        """One planned body rewritten, with everything prepare() keeps of it: in a worker
        process or here alike, so the two paths give the same result."""
        mine: set[int] = set()
        self._last_aliases, self._last_exported = {}, {}
        try:
            body = self.rewrite(a, text, mine)
        except Untranslatable as e:
            return a, ("untranslatable", str(e))
        aliases, exported = self._last_aliases, self._last_exported
        return a, ("ok", body, mine,
                   {IMPORT_MERGED.get(m.group(2), m.group(2)) for m in IMPORT.finditer(text)},
                   {m.group(1) for m in IMPORT.finditer(text)},
                   self.port_reads(body, adapted), aliases, exported)

    def start_pool(self, jobs: int) -> None:
        """Worker processes for the per-function work (body recovery and rewriting are about
        three quarters of an --all run, and independent per function once the bodies are all
        known): each builds its own Translator, 30 s and ~1.5 GB."""
        self._pool = None
        if jobs > 1:
            import multiprocessing
            print(f"starting {jobs} workers ...", file=sys.stderr, flush=True)
            self._pool = multiprocessing.get_context("spawn").Pool(jobs, _worker_init, (self.depth,))

    def warm(self, addrs) -> None:
        """Bodies for these addresses, computed by the workers (with what recovering them
        learned: the stack parameters found, a guard left in, what tools/v16.py refused)."""
        cache = self.__dict__.setdefault("_bodies", {})
        todo = sorted({a for a in addrs if a not in cache})
        if self._pool is None or len(todo) < 200:
            return
        for batch in self._pool.imap(_worker_bodies, [todo[i:i + 400] for i in range(0, len(todo), 400)]):
            for a, body, extra, guard, v16 in batch:
                cache[a] = body
                if extra:
                    self._extra_params[a] = extra
                if guard is not None:
                    self.guard_failed[a] = guard
                if v16 is not None:
                    self.v16_failed[a] = v16

    # ---- emission -------------------------------------------------------------------------
    def prepare(self, roots: list[int]) -> None:
        """Plan once and rewrite every planned body once. The --fix rounds then only re-emit:
        a function that did not compile becomes a stub in place, the plan stays, and so do
        the files, the declarations and every other body."""
        print(f"planning from {len(roots)} roots ...", file=sys.stderr, flush=True)
        plan = self.plan(roots)
        print(f"plan: {len(plan['translate'])} to translate, {len(plan['adapt'])} adapted, "
              f"{len(plan['stub'])} stubbed", file=sys.stderr, flush=True)
        # A body's recovery reads its callees' signatures, which carry the stack parameters their
        # own recovery found (recover_arguments, _extra_params): known only once that callee's
        # body was done, so a caller done first did without them, by planning order, and in a
        # worker by which worker had done what. The callers of those callees are done again now
        # that all are known: one result whatever the order.
        if self._extra_params:
            call = re.compile(r"\bFUN_(" + "|".join(f"{b:x}" for b in self._extra_params) + r")\b")
            cache = self.__dict__.setdefault("_bodies", {})
            redo = [a for a in plan["translate"] if cache.get(a) and call.search(cache[a])]
            for a in redo:
                del cache[a]
                self.body(a)
            print(f"{len(redo)} bodies redone with their callees' recovered parameters",
                  file=sys.stderr, flush=True)
        bodies: dict[int, str] = {}
        uses: set[int] = set()
        self._imports, self._dlls, self._port_reads = {}, {}, {}
        order = sorted(plan["translate"])
        if self._pool is not None and len(order) > 200:
            # (in the workers, a chunk each: what the bodies' recovery learned is theirs too)
            state = (self._extra_params, set(plan["adapt"]))
            chunks = [order[i:i + 400] for i in range(0, len(order), 400)]
            results = self._pool.imap(_worker_rewrite,
                                      ((state, [(a, self.body(a)) for a in ch]) for ch in chunks))
        else:
            results = ([self.rewrite_one(a, self.body(a), plan["adapt"])] for a in order)
        for batch in results:
            for a, out in batch:
                if out[0] == "untranslatable":
                    del plan["translate"][a]
                    plan["stub"][a] = out[1]
                    continue
                _, body, mine, imports, dlls, reads, aliases, exported = out
                uses |= mine
                bodies[a] = body
                self._imports[a], self._dlls[a] = imports, dlls
                if reads:
                    self._port_reads[a] = reads
                self.global_aliases.update(aliases)
                self.exported.update(exported)
        # whatever is called and neither translated nor adapted is stubbed
        for b in uses:
            if b in plan["native"] or self.native_ok(b):
                plan["native"][b] = True
            elif b not in plan["translate"] and b not in plan["adapt"] and b not in plan["stub"]:
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
                "adapt": self._plan["adapt"], "stub": dict(self._plan["stub"]),
                "native": dict(self._plan.get("native", {}))}
        for a in self._plan["translate"]:
            if f"{a:x}" in demoted:
                plan["stub"][a] = "did not compile: " + demoted[f"{a:x}"]
        # a stub the reference's own machine code can stand in for (native_leaf): native instead.
        # Not protection, nor the runtime's internals, nor what nothing translated calls
        leaves = self.__dict__.setdefault("_leaf_ranges", {})
        for a, why in list(plan["stub"].items()):
            if why.startswith(("anti-tamper", "an inlined anti-tamper", "called, not in", "beyond the")):
                continue
            if a not in leaves:
                leaves[a] = self.native_leaf(a)
            if leaves[a]:
                del plan["stub"][a]
                plan["native"][a] = why
        self.__dict__["_leaf_ranges"] = {a: r for a, r in leaves.items() if r and a in plan["native"]}
        # (a native function is a pointer to the reference's code, not a C function: an address
        # taken of it is the pointer itself)
        native_ref = (re.compile(r"&\s*(FUN_(?:" + "|".join(f"{a:x}" for a in plan["native"]) + r"))\b")
                      if plan["native"] else None)
        GEN.mkdir(exist_ok=True)
        port_reads = {f"{a:x}": r for a, r in self._port_reads.items() if a in plan["translate"]}
        by_file: dict[str, list[int]] = defaultdict(list)
        for a in sorted(plan["translate"]):
            by_file[self.file_of(a)].append(a)
        lines_of = {}
        for name, funcs in sorted(by_file.items()):
            k = name[len("translated_"):-2]
            out = ["/* GENERATED by tools/translate.py from build/corpus.jsonl; do not edit by hand. */",
                   # (not disabled, since 2026-10-17: what can be a translation defect rather than
                   # Ghidra's style. 4700/4701/4703 a local read before any store (a register the
                   # decompiler lost), 4715/4716 a path without a return value, 4334 a 32-bit
                   # shift widened after it overflowed, 4307 a constant that overflowed, 4098 a void
                   # function returning a value. 4554, precedence, stays off: Ghidra prints by C's
                   # grammar, `a >> b & c`, 1,355 sites of style)
                   "#include \"gen/translated.h\"", "#pragma warning(disable: 4013 4024 4028 4047 4090 4133 4142 4189 4245 "
                   "4244 4267 4305 4306 4310 4312 4389 4554 4702 4100 4101 4102 4146 "
                   "4018 4057 4113 4152 4204 4459 4456 4457 4127 4805 4800 4996)",
                   # a call count per function (written out under FROST_HITS): which translated
                   # code a run reaches, and how often, for choosing what to port by hand
                   f"#define GEN_HITS gen_hits_{k}", f"unsigned int gen_hits_{k}[{len(funcs)}];", ""]
            for i, a in enumerate(funcs):
                start = len(out) + 1
                out.append(f"/* ref: FUN_{a:x} */")
                text = late_fixes(self._rewritten[a])
                if native_ref is not None:
                    text = native_ref.sub(r"\1", text)
                if self.decl(a)[0] == "gen_v16":
                    # a 16-byte result loaded from a global (`movaps xmm0, [rip + X]; ret`), which
                    # the decompiler reads at the global's own, smaller type: all 16 bytes (C2440
                    # in FUN_1424719f0 and three more, 2026-10-18)
                    text = re.sub(r"\breturn REFG\([\w ]+\**, (0x[0-9a-f]+)\);",
                                  r"return (*(gen_v16 *)REFP(\1));", text)
                lines = text.splitlines()
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

        imports = (set().union(*self._imports.values()) if self._imports else set()) | CRT_ENTRY_IMPORTS
        dlls = set().union(*self._dlls.values()) if self._dlls else set()
        prototypes = [prototype(a) for a in sorted(set(plan["translate"]) | set(plan["adapt"]) | set(plan["stub"]))]
        # the third-party assembly run as the reference's own code (RefImage.cpp maps it): a call
        # is a call to its address, with the parameters Ghidra gave it where it has them (an
        # unprototyped call would pass a float as a double)
        for a in sorted(plan["native"]):
            ret, params = self.decl(a)
            sig = ", ".join(params) if params and "..." not in params else ""
            prototypes.append(f"#define FUN_{a:x} (({ret} (*)({sig}))0x{a:x}ULL)")
        header = ["/* GENERATED by tools/translate.py; do not edit by hand. */",
                  "#ifndef GEN_TRANSLATED_H", "#define GEN_TRANSLATED_H", "", "#include \"gen/Ghidra.h\"", ""]
        # the import libraries of the DLLs the reference's imports come from (combase's exports
        # are in ole32.lib)
        header += [f"#pragma comment(lib, \"{'ole32' if d == 'COMBASE_DLL' else d.removesuffix('_DLL').lower()}.lib\")"
                   for d in sorted(dlls)]
        header += [f"__declspec(dllimport) unsigned long long {name}();" for name in sorted(imports)]
        # the runtime routines translated code reaches by address, the port's own (body())
        rt = sorted({n for a in plan["translate"] for n in re.findall(r"\bgen_rt_(\w+)", self._rewritten.get(a, ""))})
        for name in rt:
            header += [f"#pragma comment(linker, \"/include:{name}\")",
                       f"#pragma comment(linker, \"/alternatename:gen_rt_{name}={name}\")",
                       f"unsigned long long gen_rt_{name}();"]
        # the port's own globals, where the translated code would read the image's
        # (declared at the port object's own size from the PDB: as a `char` every access wider
        # than a byte was C4739, 115 of them in 57 functions, all inside the object, and a real
        # overrun could not be told from them; 2026-10-17)
        def storage(g: dict) -> str:
            return f"unsigned char gen_g_{{}}[{g['size']}]" if g.get("size") else "char gen_g_{}"

        for base, mangled in sorted(self.global_aliases.items()):
            g = self.held[base]
            header += [f"/* {g['name']} ({g['file']}) */",
                       f"#pragma comment(linker, \"/alternatename:gen_g_{base:x}={mangled}\")",
                       f"extern {storage(g).format(f'{base:x}')};"]
        for base, g in sorted(self.exported.items()):
            cast = f"(*(unsigned char (*)[{g['size']}])gen_p_{base:x})" if g.get("size") else f"(*(char *)gen_p_{base:x})"
            header += [f"/* {g['name']} ({g['file']}), internal: through its pointer */",
                       f"extern void *const gen_p_{base:x};",
                       f"#define gen_g_{base:x} {cast}"]
        write_exports({**self.exported,
                       **{a: g for a, g in self.fn_exported.items() if a in plan["adapt"]}})
        (ROOT / "build/internal-arity.txt").write_text(
            "".join(f"FUN_{a:x}  ref {nr}  port {np_}  {name}\n"
                    for a, (name, nr, np_) in sorted(self.internal_wider.items())), encoding="utf-8")
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
            internal = mangled.startswith("&")
            call = (f"(({fptr})gen_f_{a:x})({', '.join(pass_args)})" if internal
                    else f"(({fptr})&{alias})({', '.join(pass_args)})")
            proto = f"{gret} FUN_{a:x}({', '.join(params) or 'void'})"
            if ctor and gret.strip() != "void":
                inner = f"    {call};\n    return ({gret})a0;"
            elif ret == "void":
                # (zero of the reference's own type: a 16-byte vector result is no int, C2440 in
                # the ParticleEmitterRender and ShaderEffectLightMatrix adapters, 2026-10-17)
                inner = f"    {call};\n    return {zero_of(gret)};"
            elif ret == "int" and ("float" in gret or "double" in gret):
                inner = f"    return ({gret})(long long){call};"
            else:
                inner = f"    return ({gret}){call};"
            if internal:
                adapters += [f"/* {mangled[1:]}: internal, through the pointer its file exports */",
                             f"extern void *const gen_f_{a:x};", f"{proto} {{", inner, "}", ""]
            else:
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
                 # (4232: a runtime routine's address in the table is its import thunk's, not
                 # "the" function's; only a callable target matters here)
                 "#include <excpt.h>", "#pragma warning(disable: 4013 4024 4047 4133 4244 4305 4312 4100 4232)", "",
                 "struct _EXCEPTION_POINTERS;",
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
        # the globals the port holds, for RefImage to send the image's own pointers to them there
        # (a static table of {table, callback} pairs, 0x142d2d1d0, named Spell's image copy, never
        # built: its patch-callback vector was garbage and its free aborted, 2026-10-21)
        # (one table sorted by address: RefImage searches it)
        held = [(base, self.held[base].get("size") or 8, f"(void*)&gen_g_{base:x}, 0")
                for base in self.global_aliases]
        held += [(base, g.get("size") or 8, f"0, &gen_p_{base:x}") for base, g in self.exported.items()]
        held_rows = [f"    {{ 0x{base:x}ULL, {size}ULL, {target} }}," for base, size, target in sorted(held)]
        jumps += ["", "typedef struct { unsigned long long m_ref; unsigned long long m_size; void* m_port; "
                  "void* const* m_portPtr; } GENHELD;", "const GENHELD g_genHeld[] = {"]
        jumps += held_rows + ["    { 0, 0, 0, 0 }", "};", f"const size_t g_genHeldCount = {len(held_rows)};"]
        jumps += ["", "typedef struct { unsigned long long m_address; void* m_target; } GENJUMP;",
                  "const GENJUMP g_genJumps[] = {"]
        reachable = sorted(set(plan["translate"]) | set(plan["adapt"]))
        jumps += [f"    {{ 0x{a:x}ULL, (void*)&{entry_of.get(a, f'FUN_{a:x}')} }}," for a in reachable]
        crt = [(a, name) for a, name in sorted({**CRT_ENTRY, **self.crt_thunks}.items()) if a not in set(reachable)]
        jumps += [f"    {{ 0x{a:x}ULL, (void*)&{name} }}," for a, name in crt]
        jumps += ["    { 0, 0 }", "};", f"const size_t g_genJumpCount = {len(reachable) + len(crt)};", ""]
        # the code ranges RefImage.cpp maps as the reference's own bytes (native_ok), from, to,
        # and the data inside .text the translated code reads (switch tables)
        ranges = self.native_ranges() + self.text_data_ranges()
        jumps += ["const unsigned long long g_genNative[] = {"]
        jumps += [f"    0x{lo:x}ULL, 0x{hi:x}ULL," for lo, hi in ranges] + ["    0, 0", "};",
                                                                             f"const size_t g_genNativeCount = {len(ranges)};", ""]
        # and the import slots that code calls through, which RefImage.cpp fills: the mapped
        # image's own are empty (the protector resolves them at run time)
        slots = sorted(self.__dict__.get("_native_slots", set()))
        imports = self.import_slots()
        jumps += ["typedef struct { unsigned long long m_slot; const char* m_dll; const char* m_name; } GENIMPORT;",
                  "const GENIMPORT g_genImports[] = {"]
        jumps += [f"    {{ 0x{s:x}ULL, \"{imports[s][0]}\", \"{imports[s][1]}\" }}," for s in slots]
        jumps += ["    { 0, 0, 0 }", "};", f"const size_t g_genImportCount = {len(slots)};", ""]
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
            # run as the reference's own machine code: the third-party assembly the translated code
            # calls, and leaves C could not take (with why not)
            "native": {f"{a:x}": (why if isinstance(why, str) else "third-party assembly (native range)")
                       for a, why in sorted(plan["native"].items())},
            "stubbed": {f"{a:x}": why for a, why in sorted(plan["stub"].items())},
            "handlers": {f"{a:x}": cname for a, cname in sorted(handlers.items()) if a in plan["translate"]},
            # left out on purpose, with why (--all): the protection, the padding Ghidra made functions
            # of, the reference's C runtime (the port runs its own), and what only that runtime reaches
            "excluded": {f"{a:x}": why for a, why in sorted(getattr(self, "excluded", {}).items())
                         if a not in plan["translate"] and a not in plan["adapt"] and a not in plan["native"]
                         and a not in plan["stub"]},
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
    # The linker names one referencing function per symbol and object, and none at all for the
    # rest (LNK2001): demoting only those took a full link per two or three functions (28 rounds
    # on Ghidra's SSE pseudo-ops in the --all run, 2026-10-17, `pshufhw` never attributed). Every
    # translated function that calls an unresolved symbol is demoted in this round instead.
    missing = set(re.findall(r"error LNK20(?:01|19): unresolved external symbol (\w+)", out))
    if missing:
        call = re.compile(r"\b(" + "|".join(sorted(map(re.escape, missing))) + r")\s*\(")
        named = {(a, why) for _, a, why in errors}
        for f in sorted(GEN.glob("translated_*.c")):
            for body in re.split(r"(?=^/\* ref: FUN_[0-9a-f]{9} \*/$)", f.read_text(encoding="utf-8"), flags=re.M):
                head = re.match(r"/\* ref: FUN_([0-9a-f]{9}) \*/", body)
                if not head:
                    continue
                for sym in sorted(set(call.findall(body))):
                    if (int(head.group(1), 16), f"LNK2019 unresolved {sym}") not in named:
                        errors.append(("LINK", int(head.group(1), 16), f"LNK2019 unresolved {sym}"))
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


INDIRECT_ROOTS = ROOT / "data/indirect_roots.json"
HIT = re.compile(r"\[unimplemented\] reference FUN_([0-9a-f]{9}) \(translated code reached it\)")


def load_indirect_roots() -> set[int]:
    """Functions a run saw translated code reach through a pointer (a vtable slot, a callback, a
    table in data). No static rule finds them all: the pointers come from descriptor structs and
    registrations the planner cannot follow (a measured rule over the data items translated code
    names covered none of eleven such hits and added ~600 functions, 2026-10-17). A run names
    them exactly, on the int3 each one landed on, so they are roots like the hand forwarders."""
    if not INDIRECT_ROOTS.exists():
        return set()
    return {int(k, 16) for k in json.loads(INDIRECT_ROOTS.read_text(encoding="utf-8"))}


def harvest_roots(log: Path) -> None:
    roots = json.loads(INDIRECT_ROOTS.read_text(encoding="utf-8")) if INDIRECT_ROOTS.exists() else {}
    stamp = datetime.date.today().isoformat()
    added = []
    for m in HIT.finditer(log.read_text(encoding="utf-8", errors="replace")):
        key = f"0x{m.group(1)}"
        if key not in roots:
            roots[key] = f"reached through a pointer, {stamp}"
            added.append(key)
    INDIRECT_ROOTS.write_text(json.dumps(dict(sorted(roots.items())), indent=1) + "\n", encoding="utf-8")
    print(f"{len(added)} new roots ({len(roots)} in all): {' '.join(added)}")


# ---- worker processes (Translator.start_pool) ----------------------------------------------
_WORKER = None


def _worker_init(depth: int) -> None:
    global _WORKER
    _WORKER = Translator(depth)


def _worker_bodies(addrs: list[int]) -> list[tuple]:
    t = _WORKER
    out = []
    for a in addrs:
        body = t.body(a)
        out.append((a, body, t._extra_params.get(a), t.guard_failed.get(a), t.v16_failed.get(a)))
    return out


def _worker_rewrite(job) -> list[tuple]:
    (extra_params, adapted), items = job
    t = _WORKER
    # (every body's recovered stack parameters: a callee's signature carries them)
    t._extra_params = extra_params
    return [t.rewrite_one(a, text, adapted) for a, text in items]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--risks", action="store_true", help="list the port objects translated code reads, and stop")
    ap.add_argument("--faults", metavar="LOG", help="map a run log's translated faults to source, and stop")
    ap.add_argument("--handlers", action="store_true", help="the stubbed Lua API handlers as roots")
    ap.add_argument("--addr", nargs="*", default=[], help="more roots")
    ap.add_argument("--harvest", metavar="LOG",
                    help="add a run log's int3 hits (functions reached through pointers) to data/indirect_roots.json, and stop")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--all", action="store_true",
                    help="every function in the corpus a root (but the port's own, the protection and the runtime)")
    ap.add_argument("--fix", action="store_true", help="build, demote what fails to compile, repeat")
    ap.add_argument("--jobs", type=int, default=4,
                    help="worker processes for the per-function work (~1.5 GB each; 1: none)")
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
    if args.harvest:
        return harvest_roots(Path(args.harvest))

    handlers = {}
    if args.handlers:
        handlers = stub_handlers()
    # hand-written forwarders (`// translated: FUN_x` outside src/gen) call the translation
    forwarded = set()
    for path in (ROOT / "src").rglob("*.cpp"):
        if not path.relative_to(ROOT).as_posix().startswith("src/gen/"):
            forwarded |= {int(a, 16) for a in re.findall(r"^// translated: FUN_([0-9a-f]{9})$",
                                                         path.read_text(errors="replace"), re.M)}
    roots = sorted(set(handlers) | forwarded | load_indirect_roots()
                   | {int(a.lower().removeprefix("fun_"), 16) for a in args.addr})
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
    if args.all:
        # the whole client: every function the corpus has, where the port has no counterpart
        # (a port's own stays the port's, reached through its adapter) and it is neither the
        # anti-tamper runtime nor the C runtime's
        everything = [a for a in t.cor if a not in t.protection and a not in t.fragments and not t.is_lib(a)
                      and a not in CRT_BY_ADDR and not t.native_ok(a) and t.port_target(a) is None]
        kept = t.runtime_only_dropped(everything)
        # (and why each corpus function that is no root was left out, for the manifest: the fidelity
        # report counts these as accounted for, not as functions nothing covers, 2026-10-19)
        dropped = set(everything) - set(kept)
        t.excluded = {}
        for a in t.cor:
            if a in t.protection:
                t.excluded[a] = "the anti-tamper runtime"
            elif a in t.fragments:
                t.excluded[a] = "padding between functions, no function"
            elif t.is_lib(a) or a in CRT_BY_ADDR or a in CRT_ENTRY:
                t.excluded[a] = "the C runtime, the port's own"
            elif a in dropped:
                t.excluded[a] = "a runtime routine only the runtime reaches"
        everything = kept
        print(f"--all: {len(everything)} of {len(t.cor)} corpus functions as roots", file=sys.stderr, flush=True)
        roots = sorted(set(roots) | set(everything))
    # (a root that is a runtime entry or a thunk to one is the port's routine, never translated)
    roots = [r for r in roots if r not in CRT_ENTRY and r not in t.crt_thunks]
    # (and one reached through a pointer that the port has: adapted, see plan())
    t.adapt_roots = {r for r in load_indirect_roots() if r not in forwarded and r not in handlers
                     and t.port_target(r) is not None}
    t.start_pool(args.jobs)
    try:
        t.prepare(sorted(set(roots) | set(t.static_initializers())))
    finally:
        # (the workers hold ~1.5 GB each: gone before the build rounds)
        if t._pool is not None:
            t._pool.close()
            t._pool.join()
            t._pool = None
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
