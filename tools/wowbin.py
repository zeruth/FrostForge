"""Shared access to the 3.4.3 client binary for the fact extractors.

Every extractor works on WowClassic-clean.exe (the Aegis junk nop'd, decrypted image;
never the shipped exe) and writes JSON facts under build/facts/, which
tools/ghidra/ApplyFacts.java applies to the Ghidra program.

The binary is loaded once into memory as a flat image laid out by RVA, so any
virtual address can be read without caring which section holds it.
"""
from __future__ import annotations

import bisect
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "client" / "_classic_" / "WowClassic-clean.exe"
FACTS = ROOT / "build" / "facts"


def regs_access(insn) -> tuple[list[int], list[int]]:
    """capstone's (registers read, registers written), with the operands it leaves out.

    capstone 5.0 reports some register operands with no access at all, and leaves them out of
    regs_access: `test dword ptr [rcx + 0x28], edx` reads only rcx by its account. Over .text that
    is ~6k `test m, r` alone, and FUN_140520fe0 (`test [rcx + 0x28], edx; setne al; ret`) was
    found taking one parameter, its decompilation reading `in_EDX` (2026-10-16). Such an operand
    is read; a first operand of an instruction that has others is written too (`vbroadcasti128
    ymm0, [m]`), unless the instruction only compares or pushes."""
    import capstone
    from capstone import x86_const as X
    try:
        reads, writes = insn.regs_access()
    except capstone.CsError:
        return [], []
    reads, writes = list(reads), list(writes)
    ops = insn.operands
    for k, o in enumerate(ops):
        if o.type != X.X86_OP_REG or o.reg in reads or o.reg in writes:
            continue
        if k == 0 and len(ops) > 1 and insn.mnemonic not in ("test", "cmp", "bt", "push") \
                and not insn.mnemonic.startswith("vpscatter"):
            writes.append(o.reg)
            if insn.mnemonic in ("bswap", "xchg"):
                reads.append(o.reg)
        else:
            reads.append(o.reg)
            if insn.mnemonic == "bswap":
                writes.append(o.reg)
    return reads, writes


@dataclass
class Section:
    name: str
    start: int  # VA
    end: int  # VA, exclusive (virtual size)
    exec: bool


class Binary:
    def __init__(self, path: Path = EXE):
        self.path = path
        # (the exception directory is read from the image below, not through pefile: its parse
        # built ~320 MB of objects nothing used, held in reference cycles until a collection)
        pe = pefile.PE(str(path), fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.image = bytearray(pe.OPTIONAL_HEADER.SizeOfImage)
        self.sections: list[Section] = []
        for s in pe.sections:
            data = s.get_data()[: s.Misc_VirtualSize]
            self.image[s.VirtualAddress : s.VirtualAddress + len(data)] = data
            self.sections.append(Section(
                s.Name.rstrip(b"\0").decode(errors="replace"),
                self.base + s.VirtualAddress,
                self.base + s.VirtualAddress + s.Misc_VirtualSize,
                bool(s.Characteristics & 0x20000000),
            ))
        # .pdata: exact extents of every code piece. Half the entries (~74k of 148k) are
        # chained pieces of a larger function (UNW_FLAG_CHAININFO): the compiler splits
        # cold blocks out of line, and each piece points at its parent's entry. An
        # address resolves to the piece it lands in, then to the root function; only
        # roots are functions.
        d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
        pe.close()
        pieces = []  # (begin, end, parent begin or None)
        for i in range(d.Size // 12):
            begin, end, unwind = struct.unpack_from("<III", self.image, d.VirtualAddress + 12 * i)
            ver_flags, _prolog, count = struct.unpack_from("<BBB", self.image, unwind)
            parent = None
            if (ver_flags >> 3) & 4:  # UNW_FLAG_CHAININFO: parent entry follows the codes
                at = unwind + 4 + 2 * ((count + 1) & ~1)
                parent = self.base + struct.unpack_from("<I", self.image, at)[0]
            pieces.append((self.base + begin, self.base + end, parent))
        parent_of = {b: p for b, _e, p in pieces if p is not None}

        def root(va: int) -> int:
            seen = 0
            while va in parent_of and seen < 64:
                va = parent_of[va]
                seen += 1
            return va

        pieces.sort()
        self.piece_starts = [p[0] for p in pieces]
        self.piece_ends = [p[1] for p in pieces]
        self.piece_root = [root(p[0]) for p in pieces]
        self.func_starts = sorted({r for r in self.piece_root})
        self.chained = parent_of
        self.text = self.section(".text")

    # ---- addresses -------------------------------------------------------------
    def section(self, name: str) -> Section:
        return next(s for s in self.sections if s.name == name)

    def section_of(self, va: int) -> Section | None:
        for s in self.sections:
            if s.start <= va < s.end:
                return s
        return None

    def is_code(self, va: int) -> bool:
        return self.text.start <= va < self.text.end

    def off(self, va: int) -> int:
        return va - self.base

    # ---- reads -----------------------------------------------------------------
    def u32(self, va: int) -> int:
        return struct.unpack_from("<I", self.image, self.off(va))[0]

    def i32(self, va: int) -> int:
        return struct.unpack_from("<i", self.image, self.off(va))[0]

    def u64(self, va: int) -> int:
        return struct.unpack_from("<Q", self.image, self.off(va))[0]

    def cstr(self, va: int, limit: int = 512) -> str | None:
        o = self.off(va)
        if not 0 <= o < len(self.image):
            return None
        end = self.image.find(b"\0", o, o + limit)
        if end < 0:
            return None
        raw = bytes(self.image[o:end])
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError:
            return None

    def bytes_of(self, name: str) -> tuple[int, bytes]:
        s = self.section(name)
        return s.start, bytes(self.image[self.off(s.start) : self.off(s.end)])

    # ---- functions -------------------------------------------------------------
    def func_containing(self, va: int) -> int | None:
        """The root function an address belongs to (through chained pieces)."""
        i = bisect.bisect_right(self.piece_starts, va) - 1
        if i >= 0 and va < self.piece_ends[i]:
            return self.piece_root[i]
        return None

    def is_func_start(self, va: int) -> bool:
        i = bisect.bisect_left(self.func_starts, va)
        return i < len(self.func_starts) and self.func_starts[i] == va

    # ---- references --------------------------------------------------------------
    _RIPREL = re.compile(
        # REX.W (+R) prefix, then lea/mov/cmp-family opcode with a RIP-relative ModRM
        rb"[\x48\x4c](?:\x8d|\x8b|\x89|\x39|\x3b)([\x05\x0d\x15\x1d\x25\x2d\x35\x3d])(....)",
        re.S,
    )

    def riprel_refs(self, keep=None, verify: bool = True) -> list[tuple[int, int]]:
        """(instruction VA, target VA) for every REX.W lea/mov/cmp with a RIP-relative
        operand in .text.

        Found by a byte-pattern scan, which also matches inside other instructions. So
        pass `keep(target) -> bool` to select the targets you care about, and the
        matches that survive are verified: the site must be an instruction boundary in
        a linear decode of its .pdata piece (2026-10-08: 101 of the matches on the
        anti-tamper global were mid-instruction). Without `keep` nothing is verified,
        since that would mean decoding all of .text."""
        start, data = self.bytes_of(".text")
        out = []
        for m in self._RIPREL.finditer(data):
            insn = start + m.start()
            disp = struct.unpack("<i", m.group(2))[0]
            tgt = insn + 7 + disp  # 7 = REX + opcode + ModRM + disp32
            if keep is not None and not keep(tgt):
                continue
            if keep is not None and verify and not self.is_insn_start(insn):
                continue
            out.append((insn, tgt))
        return out

    def xrefs(self, lo: int, hi: int | None = None) -> list[tuple[int, int, str]]:
        """Every instruction in .text whose RIP-relative memory operand lands in [lo, hi)
        (hi defaults to lo + 1): (instruction VA, target VA, disassembly). Unlike
        riprel_refs this is not limited to REX.W lea/mov/cmp -- `mov dword [rip+X], imm`,
        SSE stores, 32-bit moves, calls through [rip+X] all count -- which is what finds the
        static initializers that set up globals. Each candidate is confirmed by decoding the
        instruction it belongs to at a verified instruction boundary."""
        import capstone
        import numpy as np
        from capstone import x86

        hi = lo + 1 if hi is None else hi
        start, data = self.bytes_of(".text")
        n = len(data) - 4
        disp = np.ndarray(shape=(n,), dtype="<i4", buffer=data, offset=0, strides=(1,)).astype(np.int64)
        pos = np.arange(n, dtype=np.int64)
        found: set[int] = set()
        for imm in (0, 1, 2, 4):
            # target = start + pos + 4 (disp) + imm + disp
            t = pos + disp + (start + 4 + imm)
            found.update(np.nonzero((t >= lo) & (t < hi))[0].tolist())
        del disp, pos

        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        out = []
        seen = set()
        for p in sorted(found):
            field = start + p
            # Outside every .pdata piece (a leaf function with no unwind info) any address
            # passes is_insn_start, so the first candidate that decodes proves nothing: keep
            # trying until one's RIP operand lands in the range.
            in_piece = self.func_containing(field) is not None
            for back in range(1, 12):
                insn_va = field - back
                if insn_va in seen or not self.is_insn_start(insn_va):
                    continue
                o = self.off(insn_va)
                ins = next(md.disasm(bytes(self.image[o : o + 16]), insn_va, 1), None)
                if ins is None or not (insn_va < field < insn_va + ins.size):
                    continue
                matched = False
                for op in ins.operands:
                    if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                        target = insn_va + ins.size + op.mem.disp
                        if lo <= target < hi:
                            out.append((insn_va, target, f"{ins.mnemonic} {ins.op_str}"))
                            seen.add(insn_va)
                            matched = True
                if in_piece or matched:
                    break
        return sorted(out)

    def callers(self, target: int) -> list[tuple[int, str]]:
        """Every direct `call`/`jmp` rel32 in .text that lands on target: (instruction VA,
        "call" or "jmp"), each confirmed to sit on an instruction boundary."""
        import numpy as np

        start, data = self.bytes_of(".text")
        n = len(data) - 5
        raw = np.frombuffer(data, dtype=np.uint8)
        disp = np.ndarray(shape=(n,), dtype="<i4", buffer=data, offset=1, strides=(1,)).astype(np.int64)
        pos = np.arange(n, dtype=np.int64)
        hit = (pos + disp + (start + 5)) == target
        hit &= (raw[:n] == 0xE8) | (raw[:n] == 0xE9)
        out = []
        for p in np.nonzero(hit)[0].tolist():
            va = start + p
            if self.is_insn_start(va):
                out.append((va, "call" if raw[p] == 0xE8 else "jmp"))
        return out

    _bounds: dict[int, set[int]] | None = None

    def is_insn_start(self, va: int) -> bool:
        """Whether va begins an instruction in a linear decode of its .pdata piece.
        Pieces are exact and MSVC keeps jump tables out of .text, so a linear sweep of
        one piece is reliable. Addresses outside every piece (leaf functions without
        unwind info) are accepted."""
        import capstone

        i = bisect.bisect_right(self.piece_starts, va) - 1
        if i < 0 or va >= self.piece_ends[i]:
            return True
        piece = self.piece_starts[i]
        if self._bounds is None:
            self._bounds = {}
            self._md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        if piece not in self._bounds:
            if len(self._bounds) > 20000:
                self._bounds.clear()
            o = self.off(piece)
            code = bytes(self.image[o : o + (self.piece_ends[i] - piece)])
            self._bounds[piece] = {a for a, _s, _m, _o in self._md.disasm_lite(code, piece)}
        return va in self._bounds[piece]


def write_facts(name: str, data) -> Path:
    FACTS.mkdir(parents=True, exist_ok=True)
    path = FACTS / f"{name}.json"
    path.write_text(json.dumps(data, indent=1))
    return path


def hexa(va: int) -> str:
    return f"0x{va:x}"
