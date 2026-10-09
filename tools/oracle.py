"""Run reference functions under emulation: an oracle for porting pure functions.

Maps WowClassic-clean.exe at its preferred base in a Unicorn x86-64 emulator with a stack, a
TEB (gs), a scratch heap and stubs for the imports (the clean exe's import table is zeroed;
data/imports.json names every slot). A function is called with the Windows x64 convention and
runs until it returns. That is enough for self-contained code -- string formatting, hashing,
parsing -- and lets a port be checked byte for byte against the reference:

    from oracle import Oracle
    o = Oracle()
    o.sstr_vprintf("%5.2f|%x", [Oracle.f64(3.14159), 255])   # -> '  3.14|ff'

Anything the stubs do not cover (most of the OS) stops the run with an explanation.
"""
from __future__ import annotations

import json
import struct

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_MODE_64, Uc, UcError
from unicorn.x86_const import (
    UC_X86_REG_GS_BASE, UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RIP, UC_X86_REG_RSP, UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3,
)

from wowbin import ROOT, Binary

STACK, STACK_SIZE = 0x7FF0_0000_0000, 0x100000
TEB, TEB_SIZE = 0x7FF1_0000_0000, 0x10000
HEAP, HEAP_SIZE = 0x7FF2_0000_0000, 0x1000000
STUBS, STUBS_SIZE = 0x7FF3_0000_0000, 0x10000
RETURN = STUBS + STUBS_SIZE - 0x10  # a call returns here; the hook stops the run


class OracleError(Exception):
    pass


class Oracle:
    def __init__(self) -> None:
        self.bin = Binary()
        b = self.bin
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_64)
        size = (len(b.image) + 0xFFF) & ~0xFFF
        uc.mem_map(b.base, size)
        uc.mem_write(b.base, bytes(b.image))
        for base, length in ((STACK, STACK_SIZE), (TEB, TEB_SIZE), (HEAP, HEAP_SIZE), (STUBS, STUBS_SIZE)):
            uc.mem_map(base, length)
        # TEB: self pointer at gs:[0x30], TLS array at gs:[0x58] pointing at zeroed slots
        uc.mem_write(TEB + 0x30, struct.pack("<Q", TEB))
        uc.mem_write(TEB + 0x58, struct.pack("<Q", TEB + 0x1000))
        for i in range(64):
            uc.mem_write(TEB + 0x1000 + 8 * i, struct.pack("<Q", TEB + 0x2000 + 0x400 * (i % 8)))
        uc.reg_write(UC_X86_REG_GS_BASE, TEB)
        self.heap_next = HEAP
        self.last_error = 0

        # imports: each slot points at its own stub address; the hook dispatches by name
        imports = json.loads((ROOT / "data" / "imports.json").read_text())["imports"]
        self.stub_names: dict[int, str] = {}
        for i, (slot, info) in enumerate(sorted(imports.items())):
            stub = STUBS + 0x10 * i
            uc.mem_write(int(slot, 16), struct.pack("<Q", stub))
            uc.mem_write(stub, b"\xC3")  # ret, after the hook has set rax
            name = info["imported_name"]
            self.stub_names[stub] = name if isinstance(name, str) else "_or_".join(name)
        uc.mem_write(RETURN, b"\xF4")  # hlt

        uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUBS, end=STUBS + STUBS_SIZE)
        uc.hook_add(UC_HOOK_MEM_INVALID, self._on_bad_memory)

    # ---- helpers --------------------------------------------------------------------------
    @staticmethod
    def f64(value: float) -> int:
        return struct.unpack("<Q", struct.pack("<d", value))[0]

    def alloc(self, data: bytes, align: int = 16) -> int:
        self.heap_next = (self.heap_next + align - 1) & ~(align - 1)
        address = self.heap_next
        self.uc.mem_write(address, data)
        self.heap_next += len(data) + 1
        return address

    def cstr(self, s: str | bytes) -> int:
        return self.alloc((s.encode() if isinstance(s, str) else s) + b"\0")

    def read_cstr(self, address: int, limit: int = 0x10000) -> bytes:
        data = bytes(self.uc.mem_read(address, limit))
        return data.split(b"\0", 1)[0]

    # ---- hooks ---------------------------------------------------------------------------
    def _on_stub(self, uc, address, size, _user) -> None:
        if address == RETURN:
            uc.emu_stop()
            return
        name = self.stub_names.get(address)
        if name is None:
            return
        result = self.import_call(name)
        if result is None:
            uc.emu_stop()
            raise OracleError(f"reached unsupported import {name}")
        uc.reg_write(UC_X86_REG_RAX, result)

    def import_call(self, name: str) -> int | None:
        """Minimal behaviour for the imports self-contained code touches. Extend as needed."""
        uc = self.uc
        if name == "GetLastError":
            return self.last_error
        if name == "SetLastError":
            self.last_error = uc.reg_read(UC_X86_REG_RCX) & 0xFFFFFFFF
            return 0
        if name in ("FlsGetValue", "TlsGetValue"):
            return TEB + 0x3000
        if name in ("FlsSetValue", "TlsSetValue"):
            return 1
        if name in ("EnterCriticalSection", "LeaveCriticalSection", "InitializeCriticalSection",
                    "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive"):
            return 0
        return None

    def _on_bad_memory(self, uc, access, address, size, value, _user) -> bool:
        rip = uc.reg_read(UC_X86_REG_RIP)
        raise OracleError(f"bad memory access at {address:#x} (rip {rip:#x})")

    # ---- calls -----------------------------------------------------------------------------
    def call(self, function: int, args: list[int], floats: dict[int, float] | None = None,
             max_instructions: int = 50_000_000, ret: str = "rax") -> int:
        """Runs `function` with integer args (rcx, rdx, r8, r9, then the stack) and returns rax,
        or the low 64 bits of xmm0 with ret="xmm0"."""
        uc = self.uc
        rsp = STACK + STACK_SIZE - 0x1000
        stack_args = args[4:]
        rsp -= 8 * len(stack_args)
        rsp &= ~0xF
        for i, a in enumerate(stack_args):
            uc.mem_write(rsp + 0x20 + 8 * i, struct.pack("<Q", a & 0xFFFFFFFFFFFFFFFF))
        rsp -= 8
        uc.mem_write(rsp, struct.pack("<Q", RETURN))
        uc.reg_write(UC_X86_REG_RSP, rsp)
        for reg, a in zip((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), args):
            uc.reg_write(reg, a & 0xFFFFFFFFFFFFFFFF)
        for index, value in (floats or {}).items():
            reg = (UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3)[index]
            uc.reg_write(reg, self.f64(value))
        try:
            uc.emu_start(function, RETURN, count=max_instructions)
        except UcError as e:
            rip = uc.reg_read(UC_X86_REG_RIP)
            raise OracleError(f"{e} at rip {rip:#x}") from e
        if ret == "xmm0":
            return uc.reg_read(UC_X86_REG_XMM0) & 0xFFFFFFFFFFFFFFFF
        return uc.reg_read(UC_X86_REG_RAX)

    # ---- reference functions -----------------------------------------------------------------
    def sstr_vprintf(self, fmt: str | bytes, args: list[int], size: int = 512) -> str:
        """0x1402610c0(dest, maxchars, format, va_list): arguments are 8-byte slots, doubles as
        their bit patterns (Oracle.f64), strings as addresses (Oracle.cstr)."""
        dest = self.alloc(b"\0" * size)
        va = self.alloc(b"".join(struct.pack("<Q", a & 0xFFFFFFFFFFFFFFFF) for a in args) + b"\0" * 64)
        self.call(0x1402610C0, [dest, size, self.cstr(fmt), va])
        return self.read_cstr(dest).decode("latin-1")


if __name__ == "__main__":
    o = Oracle()
    print(repr(o.sstr_vprintf("%d|%5.2f|%s|%x", [-42, Oracle.f64(3.14159), o.cstr("hi"), 255])))
