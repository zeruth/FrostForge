"""Recover the stripped import table from a running client.

The clean image has its import address table zeroed: one block of 8-byte slots at the
start of .rdata (0x14260d000 at the preferred base), reached by `call [rip+slot]`, with
a single real import (MessageBoxW) left in the PE header. The protection fills the
slots at startup, so the names exist only in a live process. This attaches read-only,
reads every slot, and resolves each pointer against the export tables of the loaded
modules. Run it once against a client sitting at the login screen.

    python tools/iat_dump.py [--pid N]      -> build/facts/imports.json

Read-only: PROCESS_QUERY_INFORMATION | PROCESS_VM_READ. It does not suspend, inject
or write anything.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import re
import struct
import subprocess

import pefile

from wowbin import Binary, hexa, write_facts

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
LIST_MODULES_ALL = 0x03

k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
                                       ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]


psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODULEINFO), wt.DWORD]


def find_pid() -> int:
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    pids = [int(m.group(2)) for m in re.finditer(r'"(WowClassic[^"]*\.exe)","(\d+)"', out)]
    if len(pids) != 1:
        raise SystemExit(f"expected one WowClassic process, found {len(pids)}; pass --pid")
    return pids[0]


def read(h, addr: int, size: int) -> bytes:
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        raise OSError(ctypes.get_last_error(), f"ReadProcessMemory {addr:#x}")
    return buf.raw[: got.value]


def modules(h):
    arr = (ctypes.c_void_p * 1024)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), LIST_MODULES_ALL)
    out = []
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(h, arr[i], name, 520)
        mi = MODULEINFO()
        psapi.GetModuleInformation(h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi))
        out.append((mi.base, mi.size, name.value))
    return out


def export_map(path: str) -> dict[int, list[str]]:
    """rva -> every name exported at it. Folded identical functions share an address
    (ntdll's RtlInitializeConditionVariable and RtlInitializeSRWLock), so one address
    can carry several names; the importing DLL's forwarders pick the right one."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    m: dict[int, list[str]] = {}
    exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    for e in exp.symbols if exp else []:
        m.setdefault(e.address, []).append(e.name.decode() if e.name else f"#{e.ordinal}")
    return m


MASK = (1 << 64) - 1


class Mem:
    """Page-cached reads from the target process."""

    def __init__(self, h):
        self.h = h
        self.pages: dict[int, bytes] = {}

    def code(self, va: int, n: int = 32) -> bytes:
        out = b""
        while len(out) < n:
            page = (va + len(out)) & ~0xFFF
            if page not in self.pages:
                try:
                    self.pages[page] = read(self.h, page, 0x1000)
                except OSError:
                    self.pages[page] = b""
            data = self.pages[page]
            o = (va + len(out)) - page
            if o >= len(data):
                break
            out += data[o : o + (n - len(out))]
        return out


def trace_stub(mem: Mem, va: int, md) -> tuple[int | None, str]:
    """Follow an import stub: constant arithmetic on one register across jmp-linked
    fragments, ending in an indirect jump through it (or push reg; ret). Every
    register is tracked, since stubs use rax or r10. Returns (final value, how it ended)."""
    from capstone import x86

    regs: dict[int, int] = {}
    pc = va
    for _ in range(256):
        ins = next(md.disasm(mem.code(pc, 16), pc, 1), None)
        if ins is None:
            return None, f"undecodable at {pc:#x}"
        m, ops = ins.mnemonic, ins.operands
        r = ops[0].reg if len(ops) >= 1 and ops[0].type == x86.X86_OP_REG else None
        imm = ops[1].imm & MASK if len(ops) == 2 and ops[1].type == x86.X86_OP_IMM else None
        if m == "jmp" and ops[0].type == x86.X86_OP_IMM:
            pc = ops[0].imm & MASK
            continue
        if m == "jmp" and r is not None:
            return regs.get(r), f"jmp {ins.op_str}"
        if m == "nop":
            pc += ins.size
            continue
        if r is not None and imm is not None and m in ("mov", "movabs", "add", "sub", "xor", "and", "or"):
            if m in ("mov", "movabs"):
                regs[r] = imm
            elif r not in regs:
                return None, f"{m} before {ins.reg_name(r)} set at {pc:#x}"
            else:
                v = regs[r]
                regs[r] = {"add": (v + imm) & MASK, "sub": (v - imm) & MASK, "xor": v ^ imm,
                           "and": v & imm, "or": v | imm}[m]
            pc += ins.size
            continue
        r2 = ops[1].reg if len(ops) == 2 and ops[1].type == x86.X86_OP_REG else None
        if r is not None and r2 is not None and r2 in regs and m in ("mov", "add", "sub", "xor", "and", "or", "imul"):
            if m == "mov":
                regs[r] = regs[r2]
            elif r not in regs:
                return None, f"{m} before {ins.reg_name(r)} set at {pc:#x}"
            else:
                v, w = regs[r], regs[r2]
                regs[r] = {"add": (v + w) & MASK, "sub": (v - w) & MASK, "xor": v ^ w,
                           "and": v & w, "or": v | w, "imul": (v * w) & MASK}[m]
            pc += ins.size
            continue
        if r is not None and len(ops) == 1 and m in ("not", "neg", "bswap") and r in regs:
            v = regs[r]
            regs[r] = {"not": (~v) & MASK, "neg": (-v) & MASK,
                       "bswap": int.from_bytes(v.to_bytes(8, "little"), "big")}[m]
            pc += ins.size
            continue
        if r is not None and m in ("rol", "ror") and imm is not None and r in regs:
            v, k = regs[r], imm % 64
            regs[r] = ((v << k) | (v >> (64 - k))) & MASK if m == "rol" else ((v >> k) | (v << (64 - k))) & MASK
            pc += ins.size
            continue
        if m == "push" and r is not None:
            nxt = next(md.disasm(mem.code(pc + ins.size, 16), pc + ins.size, 1), None)
            if nxt is not None and nxt.mnemonic == "ret":
                return regs.get(r), f"push {ins.op_str}; ret"
        return None, f"unmodelled '{m} {ins.op_str}' at {pc:#x}"
    return None, "too many steps"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    args = ap.parse_args()
    pid = args.pid or find_pid()

    b = Binary()
    # the slot block: every `call [rip+X]` target in .rdata, which all sit in the IAT block
    s, d = b.bytes_of(".text")
    slots = set()
    for m in re.finditer(rb"\xff\x15(....)", d, re.S):
        t = s + m.start() + 6 + struct.unpack("<i", m.group(1))[0]
        sec = b.section_of(t)
        if sec and sec.name == ".rdata" and b.u64(t) == 0:
            slots.add(t)
    lo, hi = min(slots), max(slots) + 8
    print(f"slot block {lo:#x}..{hi:#x} ({len(slots)} called slots)")

    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise SystemExit(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
    mods = modules(h)
    main_base = next(base for base, _sz, p in mods if p.lower().endswith(".exe"))
    delta = main_base - b.base
    print(f"pid {pid}: image at {main_base:#x} (preferred {b.base:#x}), {len(mods)} modules")

    import capstone

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    mem = Mem(h)
    raw = read(h, lo + delta, hi - lo)
    exports_cache: dict[str, dict[int, str]] = {}
    imports = {}
    unresolved = 0
    endings: dict[str, int] = {}

    def module_of(v: int):
        return next(((base, p) for base, sz, p in mods if base <= v < base + sz), None)

    for off in range(0, len(raw), 8):
        v = struct.unpack_from("<Q", raw, off)[0]
        if v == 0:
            continue
        slot = lo + off
        target, how = v, "direct"
        if module_of(v) is None:  # an obfuscated stub: follow it to the real target
            target, how = trace_stub(mem, v, md)
        endings[how.split(" at ")[0]] = endings.get(how.split(" at ")[0], 0) + 1
        entry = {"value": hexa(v), "target": hexa(target) if target is not None else None,
                 "via": how, "module": None, "name": None, "called": slot in slots}
        mod = module_of(target) if target is not None else None
        if mod is not None:
            base, path = mod
            if path not in exports_cache:
                exports_cache[path] = export_map(path)
            entry["module"] = path.replace("\\", "/").rsplit("/", 1)[-1]
            names = exports_cache[path].get(target - base, [])
            entry["name"] = names[0] if names else None
            if len(names) > 1:
                entry["aliases"] = names
        if entry["name"] is None:
            unresolved += 1
        imports[hexa(slot)] = entry
    k32.CloseHandle(h)
    print("stub endings:", sorted(endings.items(), key=lambda x: -x[1])[:8])

    # Forwarded exports: a KERNEL32 import of HeapAlloc lands in ntdll!RtlAllocateHeap.
    # Slots form one group per imported DLL (groups end at a null slot), so the group's
    # majority module is the DLL the code imported from; map an off-module slot back
    # through that DLL's forwarders to the name the code actually used.
    paths = {p.replace("\\", "/").rsplit("/", 1)[-1].lower(): p for _b, _s, p in mods}
    forwarders: dict[str, dict[str, list[str]]] = {}

    def forwards_of(dll_path: str) -> dict[str, list[str]]:
        if dll_path not in forwarders:
            pe = pefile.PE(dll_path, fast_load=True)
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
            m: dict[str, list[str]] = {}
            exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
            for e in exp.symbols if exp else []:
                if e.forwarder and e.name:
                    target_name = e.forwarder.decode().rsplit(".", 1)[-1]
                    m.setdefault(target_name, []).append(e.name.decode())
            forwarders[dll_path] = m
        return forwarders[dll_path]

    groups, cur = [], []
    for off in range(0, len(raw), 8):
        if struct.unpack_from("<Q", raw, off)[0] == 0:
            if cur:
                groups.append(cur)
            cur = []
        else:
            cur.append(hexa(lo + off))
    if cur:
        groups.append(cur)
    remapped = 0
    for g in groups:
        mods_in = [imports[s]["module"] for s in g]
        dll = max(set(mods_in), key=mods_in.count)
        for s in g:
            e = imports[s]
            e["dll"] = dll
            if e["module"] == dll or e["name"] is None or dll.lower() not in paths:
                continue
            fw = forwards_of(paths[dll.lower()])
            cands = [c for alias in e.get("aliases", [e["name"]]) for c in fw.get(alias, [])]
            if not cands:
                # not forwarded under any alias: the imported name is the resolved one
                e["imported_name"] = e["name"]
            else:
                e["imported_name"] = cands[0] if len(cands) == 1 else cands
            remapped += 1
    # a real import table names each function once per DLL; flag what is still ambiguous
    seen: dict[tuple, list[str]] = {}
    for s, e in imports.items():
        n = e.get("imported_name") or e["name"]
        seen.setdefault((e["dll"], str(n)), []).append(s)
    dups = {k: v for k, v in seen.items() if len(v) > 1}
    for (dll, n), ss in dups.items():
        # identical code under several names (htons/ntohs): only the call sites can
        # tell these slots apart, so record every candidate rather than pick one
        for s in ss:
            e = imports[s]
            if not isinstance(e.get("imported_name"), list):
                e["imported_name"] = e.get("aliases", [e["name"]])
            e["ambiguous"] = True
        print(f"  ambiguous: {dll}!{imports[ss[0]]['imported_name']} in slots {ss}")
    for e in imports.values():
        e.setdefault("imported_name", e["name"])
    print(f"{len(groups)} DLL groups; {remapped} forwarded slots mapped back to their importing DLL")

    out = write_facts("imports", {"block": [hexa(lo), hexa(hi)], "imports": imports})
    by_mod = {}
    for i in imports.values():
        by_mod[i["dll"]] = by_mod.get(i["dll"], 0) + 1
    print(f"{len(imports)} filled slots, {unresolved} unresolved -> {out}")
    for mname, n in sorted(by_mod.items(), key=lambda x: -x[1])[:15]:
        print(f"  {n:4}  {mname}")


if __name__ == "__main__":
    main()
