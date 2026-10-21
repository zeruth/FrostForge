"""Launches FrostLite under a minimal debugger and reports the first real exception: its code,
address and registers, and the FrostLite return addresses found on the faulting thread's stack,
symbolized through the PDB (a scan, not an unwind: false positives are possible, the order is
the stack's).

    python tools/crashcatch.py [--exe build/cmake/bin/Release/FrostLite.exe] [--cwd client/_classic_]
                               [--timeout 120]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLIZER = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"

DEBUG_ONLY_THIS_PROCESS = 0x2
EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
CONTEXT_AMD64 = 0x100000
CONTEXT_FULL = CONTEXT_AMD64 | 0x7
IGNORED = {0x80000003, 0x4000001F, 0x406D1388, 0xE06D7363, 0x40010006, 0x4001000A}

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [("ExceptionCode", wt.DWORD), ("ExceptionFlags", wt.DWORD), ("ExceptionRecord", ctypes.c_void_p),
                ("ExceptionAddress", ctypes.c_void_p), ("NumberParameters", wt.DWORD),
                ("ExceptionInformation", ctypes.c_uint64 * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wt.DWORD)]


class CREATE_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wt.HANDLE), ("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("lpBaseOfImage", ctypes.c_void_p), ("dwDebugInfoFileOffset", wt.DWORD),
                ("nDebugInfoSize", wt.DWORD), ("lpThreadLocalBase", ctypes.c_void_p),
                ("lpStartAddress", ctypes.c_void_p), ("lpImageName", ctypes.c_void_p), ("fUnicode", wt.WORD)]


class LOAD_DLL_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wt.HANDLE), ("lpBaseOfDll", ctypes.c_void_p), ("dwDebugInfoFileOffset", wt.DWORD),
                ("nDebugInfoSize", wt.DWORD), ("lpImageName", ctypes.c_void_p), ("fUnicode", wt.WORD)]


class CREATE_THREAD_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hThread", wt.HANDLE), ("lpThreadLocalBase", ctypes.c_void_p), ("lpStartAddress", ctypes.c_void_p)]


class DEBUG_EVENT_UNION(ctypes.Union):
    _fields_ = [("Exception", EXCEPTION_DEBUG_INFO), ("CreateProcessInfo", CREATE_PROCESS_DEBUG_INFO),
                ("CreateThread", CREATE_THREAD_DEBUG_INFO),
                ("LoadDll", LOAD_DLL_DEBUG_INFO), ("raw", ctypes.c_byte * 160)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD),
                ("u", DEBUG_EVENT_UNION)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_uint64), ("High", ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64), ("P2Home", ctypes.c_uint64), ("P3Home", ctypes.c_uint64),
                ("P4Home", ctypes.c_uint64), ("P5Home", ctypes.c_uint64), ("P6Home", ctypes.c_uint64),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD), ("SegCs", wt.WORD), ("SegDs", wt.WORD),
                ("SegEs", wt.WORD), ("SegFs", wt.WORD), ("SegGs", wt.WORD), ("SegSs", wt.WORD),
                ("EFlags", wt.DWORD), ("Dr0", ctypes.c_uint64), ("Dr1", ctypes.c_uint64), ("Dr2", ctypes.c_uint64),
                ("Dr3", ctypes.c_uint64), ("Dr6", ctypes.c_uint64), ("Dr7", ctypes.c_uint64),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64), ("Rbx", ctypes.c_uint64),
                ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64), ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64),
                ("R8", ctypes.c_uint64), ("R9", ctypes.c_uint64), ("R10", ctypes.c_uint64), ("R11", ctypes.c_uint64),
                ("R12", ctypes.c_uint64), ("R13", ctypes.c_uint64), ("R14", ctypes.c_uint64), ("R15", ctypes.c_uint64),
                ("Rip", ctypes.c_uint64), ("FltSave", ctypes.c_byte * 512), ("VectorRegister", M128A * 26),
                ("VectorControl", ctypes.c_uint64), ("DebugControl", ctypes.c_uint64),
                ("LastBranchToRip", ctypes.c_uint64), ("LastBranchFromRip", ctypes.c_uint64),
                ("LastExceptionToRip", ctypes.c_uint64), ("LastExceptionFromRip", ctypes.c_uint64)]


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


def read(process, address, size):
    buffer = ctypes.create_string_buffer(size)
    done = ctypes.c_size_t()
    if not k32.ReadProcessMemory(process, ctypes.c_void_p(address), buffer, size, ctypes.byref(done)):
        return b""
    return buffer.raw[:done.value]


def symbolize(exe, rvas):
    if not rvas:
        return []
    text = "\n".join("0x%x" % r for r in rvas) + "\n"
    out = subprocess.run([SYMBOLIZER, "--obj=" + exe, "--relative-address", "--demangle", "--functions=linkage"],
                         input=text, capture_output=True, text=True).stdout
    blocks = [b for b in out.strip().split("\n\n")]
    return blocks


def fault_in(process, thread_id, exe, base, name):
    """Whether the thread's instruction is in the named function (--in-function)."""
    thread = k32.OpenThread(0x1FFFFF, False, thread_id)
    context = CONTEXT()
    context.ContextFlags = CONTEXT_FULL
    k32.GetThreadContext(thread, ctypes.byref(context))
    k32.CloseHandle(thread)
    if context.Rip < base:
        return False
    block = symbolize(exe, [context.Rip - base])
    # (every function of the inline chain: the instruction's own line can be an inlined helper's)
    return bool(block) and any(name in line for line in block[0].splitlines()[0::2])


def dump_stack(process, exe, base, context, limit=60):
    stack = b""
    page = context.Rsp
    while len(stack) < 0x10000:
        chunk = read(process, page, 0x1000 - (page & 0xfff))
        if not chunk:
            break
        stack += chunk
        page += len(chunk)
    size = 0x4000000
    rvas = []
    if context.Rip and base and base <= context.Rip < base + size:
        rvas.append(context.Rip - base)
    for i in range(0, len(stack) - 7, 8):
        value = int.from_bytes(stack[i:i + 8], "little")
        if base and base + 0x1000 <= value < base + size:
            rvas.append(value - base - 1)
    seen = []
    for rva in rvas:
        if rva not in seen:
            seen.append(rva)
    print("  rip %016x rsp %016x" % (context.Rip, context.Rsp))
    for rva, block in zip(seen[:limit], symbolize(exe, seen[:limit])):
        lines = block.strip().split("\n")
        print("  +%07x %s  %s" % (rva, lines[0] if lines else "?", lines[1] if len(lines) > 1 else ""))


def symbol_rva(exe, name):
    """A public symbol's RVA, from the PDB beside the exe (llvm-pdbutil) and the exe's sections."""
    import re
    import pefile
    out = subprocess.run([SYMBOLIZER.replace("llvm-symbolizer", "llvm-pdbutil"), "dump", "-publics",
                          os.path.splitext(exe)[0] + ".pdb"], capture_output=True, text=True).stdout
    m = re.search(r"`%s`\s*\n\s*flags = [^,]*, addr = (\d+):(\d+)" % re.escape(name), out)
    if not m:
        return None
    pe = pefile.PE(exe, fast_load=True)
    return pe.sections[int(m.group(1)) - 1].VirtualAddress + int(m.group(2))


def dump_hits(process, exe, base, path):
    """What FROST_HITS would have written (RefImage.cpp's HitDumpThread), read out of the stopped
    process: a run that dies within the dump thread's first five seconds still leaves its counts.
    "address count" a line, most first."""
    tables, count = symbol_rva(exe, "gen_hit_tables"), symbol_rva(exe, "gen_hit_table_count")
    if tables is None or count is None:
        print("hits: gen_hit_tables not in the PDB")
        return
    n = int.from_bytes(read(process, base + count, 4), "little")
    raw = read(process, base + tables, n * 24)
    rows = []
    for t in range(n):
        hits, addrs, m = int.from_bytes(raw[t * 24:t * 24 + 8], "little"), \
            int.from_bytes(raw[t * 24 + 8:t * 24 + 16], "little"), int.from_bytes(raw[t * 24 + 16:t * 24 + 20], "little")
        h, a = read(process, hits, 4 * m), read(process, addrs, 8 * m)
        for i in range(min(len(h) // 4, len(a) // 8)):
            c = int.from_bytes(h[4 * i:4 * i + 4], "little")
            if c:
                rows.append((c, int.from_bytes(a[8 * i:8 * i + 8], "little")))
    rows.sort(key=lambda r: -r[0])
    with open(path, "w") as f:
        for c, a in rows:
            f.write("%x %u\n" % (a, c))
    print("hits: %d functions reached, written to %s" % (len(rows), path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "cmake", "bin", "Release", "FrostLite.exe"))
    ap.add_argument("--cwd", default=os.path.join(ROOT, "client", "_classic_"))
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--skip", type=int, default=0, help="let this many exceptions pass first")
    ap.add_argument("--in-function", default=None,
                    help="stop only at a fault whose instruction is in this function (by name); pass the rest on")
    ap.add_argument("--dump", nargs="*", default=[], help="memory to show at the stop: hexaddr[:len] or a register")
    ap.add_argument("--second-chance", action="store_true",
                    help="report only an exception nothing handled (guarded faults pass through)")
    ap.add_argument("--args", default="", help="the command line after the exe, as one string")
    ap.add_argument("--hits", default=None, help="at the stop, write the translated functions' call counts here")
    ap.add_argument("--all-threads", action="store_true",
                    help="at a timeout, also show every thread once, with its registers")
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    si = STARTUPINFO()
    si.cb = ctypes.sizeof(si)
    inherit = False
    log = None
    if args.log:
        # The client's stderr (the unimplemented log) goes to the file.
        log = open(args.log, "wb")
        import msvcrt
        handle = msvcrt.get_osfhandle(log.fileno())
        k32.SetHandleInformation(wt.HANDLE(handle), 1, 1)
        si.dwFlags = 0x100
        si.hStdError = handle
        si.hStdOutput = handle
        inherit = True
    pi = PROCESS_INFORMATION()
    command = ctypes.create_unicode_buffer('"%s"' % exe + (" " + args.args if args.args else ""))
    if not k32.CreateProcessW(None, command, None, None, inherit, DEBUG_ONLY_THIS_PROCESS, None, args.cwd,
                              ctypes.byref(si), ctypes.byref(pi)):
        print("CreateProcess failed", ctypes.get_last_error())
        return 1
    base = None
    event = DEBUG_EVENT()
    deadline = time.time() + args.timeout
    loader_break_seen = False
    threads = {pi.dwThreadId: pi.hThread}
    while time.time() < deadline:
        if not k32.WaitForDebugEvent(ctypes.byref(event), 1000):
            continue
        status = DBG_CONTINUE
        code = event.dwDebugEventCode
        if code == CREATE_PROCESS_DEBUG_EVENT:
            base = event.u.CreateProcessInfo.lpBaseOfImage
        elif code == CREATE_THREAD_DEBUG_EVENT:
            # (every thread, for a timeout's samples: a hang is often one thread waiting on a
            # lock another holds)
            threads[event.dwThreadId] = event.u.CreateThread.hThread
        elif code == EXIT_THREAD_DEBUG_EVENT:
            threads.pop(event.dwThreadId, None)
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            print("process exited, code", hex(ctypes.cast(ctypes.byref(event.u), ctypes.POINTER(wt.DWORD))[0]))
            return 0
        elif code == EXCEPTION_DEBUG_EVENT:
            record = event.u.Exception.ExceptionRecord
            ecode = record.ExceptionCode
            if ecode == 0x80000003 and loader_break_seen:
                # an int3 in the mapped reference image is the program's own (src/gen/RefImage.cpp
                # turns it into "log and return 0"): its handler must see it
                status = DBG_EXCEPTION_NOT_HANDLED
            elif ecode in IGNORED or (ecode & 0xF0000000) == 0x40000000:
                if ecode == 0x80000003:
                    loader_break_seen = True
                status = DBG_EXCEPTION_NOT_HANDLED if ecode not in (0x80000003, 0x4000001F) else DBG_CONTINUE
            elif args.second_chance and event.u.Exception.dwFirstChance:
                # let the program's own handlers (GenFault's guards) have it first
                status = DBG_EXCEPTION_NOT_HANDLED
            elif args.skip > 0:
                # an earlier fault than the one wanted: the program handles it
                args.skip -= 1
                status = DBG_EXCEPTION_NOT_HANDLED
            elif args.in_function and not fault_in(pi.hProcess, event.dwThreadId, exe, base, args.in_function):
                # a fault elsewhere: the program handles it
                status = DBG_EXCEPTION_NOT_HANDLED
            else:
                thread = k32.OpenThread(0x1FFFFF, False, event.dwThreadId)
                context = CONTEXT()
                context.ContextFlags = CONTEXT_FULL
                k32.GetThreadContext(thread, ctypes.byref(context))
                print("exception %08x at %x (first chance %d), thread %d" % (
                    ecode, record.ExceptionAddress or 0, event.u.Exception.dwFirstChance, event.dwThreadId))
                params = [hex(record.ExceptionInformation[i]) for i in range(record.NumberParameters)]
                print("parameters", params)
                for name in ("Rip", "Rsp", "Rax", "Rbx", "Rcx", "Rdx", "Rsi", "Rdi", "R8", "R9", "R10", "R11", "R12",
                             "R13", "R14", "R15"):
                    print("  %s %016x" % (name, getattr(context, name)))
                dump_stack(pi.hProcess, exe, base, context)
                if args.hits:
                    dump_hits(pi.hProcess, exe, base, args.hits)
                for spec in args.dump:
                    # address:length, or a register name for what it points at
                    where, _, length = spec.partition(":")
                    # (`*hexaddr`: what the 8 bytes at that address point at)
                    deref = where.startswith("*")
                    where = where.lstrip("*")
                    address = getattr(context, where.capitalize()) if hasattr(context, where.capitalize()) \
                        else int(where, 16)
                    if deref:
                        address = int.from_bytes(read(pi.hProcess, address, 8), "little")
                    data = read(pi.hProcess, address, int(length or "64", 0))
                    print("memory %x:" % address)
                    for k in range(0, len(data), 16):
                        print("  %x  %s" % (address + k, data[k:k + 16].hex(" ")))
                k32.TerminateProcess(pi.hProcess, 1)
                return 0
        k32.ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status)
    print("timeout: the main thread, sampled three times")
    for sample in range(3):
        k32.SuspendThread(pi.hThread)
        context = CONTEXT()
        context.ContextFlags = CONTEXT_FULL
        k32.GetThreadContext(pi.hThread, ctypes.byref(context))
        print("sample", sample)
        if sample == 0:
            # the registers, and what each points at: a wait on a CRITICAL_SECTION has it in one
            # of them, and its +0x10 is the owning thread's id
            for name in ("Rcx", "Rdx", "Rbx", "Rsi", "Rdi", "R12", "R13", "R14", "R15"):
                value = getattr(context, name)
                data = read(pi.hProcess, value, 0x20) if value > 0x10000 else None
                print("  %s %016x%s" % (name, value, "  -> " + data.hex(" ") if data else ""))
        dump_stack(pi.hProcess, exe, base, context, 25)
        k32.ResumeThread(pi.hThread)
        time.sleep(0.3)
    if args.all_threads:
        # the loaded modules, so a rip outside the exe names its DLL and offset
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        mods = (ctypes.c_void_p * 1024)()
        need = wt.DWORD()
        psapi.EnumProcessModulesEx(pi.hProcess, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
        spans = []
        for m in mods[:need.value // ctypes.sizeof(ctypes.c_void_p)]:
            name = ctypes.create_unicode_buffer(260)
            psapi.GetModuleBaseNameW(pi.hProcess, ctypes.c_void_p(m), name, 260)
            spans.append((m or 0, name.value))
        spans.sort()

        def where(va):
            owner = [s for s in spans if s[0] <= va]
            return "%s+%x" % (owner[-1][1], va - owner[-1][0]) if owner else "?"
        # every thread once, with its argument registers (a lock wait holds the lock in rcx)
        for tid, handle in sorted(threads.items()):
            k32.SuspendThread(handle)
            context = CONTEXT()
            context.ContextFlags = CONTEXT_FULL
            if k32.GetThreadContext(handle, ctypes.byref(context)):
                print("thread %d  rip %s  rcx %016x rdx %016x rbx %016x rsi %016x rdi %016x" % (
                    tid, where(context.Rip), context.Rcx, context.Rdx, context.Rbx, context.Rsi, context.Rdi))
                dump_stack(pi.hProcess, exe, base, context, 14)
            k32.ResumeThread(handle)
    k32.TerminateProcess(pi.hProcess, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
