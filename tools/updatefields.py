"""The 3.4.3 update-field descriptors, read out of the client's own deserializers.

3.4.3 (54261) uses the modern "changes mask" update-field system (the one TrinityCore generates
UpdateFields.cpp for), not the flat u32-index descriptor table of 3.3.5: the client keeps no
descriptor name strings at all. What it does keep is one generated reader pair per data struct
(ObjectData, ItemData, UnitData, ...): ReadCreate(data, stream, ?, fieldFlags, owner) and
ReadUpdate(data, stream, ?, owner), reached from each CG*_C class's vtable slots 0x368 / 0x370
(SMSG_UPDATE_OBJECT -> 0x1418efa70 -> 0x1418ef790 / 0x1418efc60 -> 0x1418eeb80).

Rather than parse ~20k lines of decompiled C, this runs those readers under emulation (Unicorn,
via tools/oracle.py) and records what they read:

  * every CDataStore primitive (u8/u16/u32/u64, packed GUID, raw bytes) and every bit-reader
    request (0x14071f930) is hooked and served from a policy, so each run is a trace of
    (kind, width, call site, value) events in exact wire order;
  * stores into the data struct (and into heap blocks the reader allocates for dynamic arrays)
    are hooked, which ties each read to the struct offset it lands at, and its type (a movss
    store is a float);
  * create: one run per field-visibility flag shows which reads each flag gates; a probe run
    per u32/bit call site with value 1 shows which reads are counts / presence bits and what
    one element looks like;
  * update: the change masks are served all-ones (sizes 0) for the full order; then every mask
    bit is knocked out in turn and the trace diffed, which maps each mask bit to the reads it
    gates (a parent/group bit gates a superset, so each read is assigned the most specific bit).

    python tools/updatefields.py                 # all types -> docs/updatefields.json + .md
    python tools/updatefields.py --only Unit     # one struct
    python tools/updatefields.py --trace UnitData --mode update   # raw baseline trace

Names are not in the client. docs/updatefields.json carries TrinityCore's names where the
client's shape (order, width, count, gate) matches TC's wotlk_classic generator, marked
"name_source": "inferred"; offsets, widths, counts, order, flags and mask bits are all read
from the client.
"""
from __future__ import annotations

import argparse
import difflib
import json
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import capstone
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UcError
from unicorn.x86_const import (
    UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RIP,
    UC_X86_REG_RSP,
)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from oracle import HEAP, HEAP_SIZE, Oracle, OracleError  # noqa: E402
from wowbin import ROOT  # noqa: E402

DOCS = ROOT / "docs"

# ---- the client's readers --------------------------------------------------------------------
READ_U8, READ_U16, READ_U32, READ_U64 = 0x1402C9990, 0x1402C99E0, 0x1402C9A30, 0x1402C9A80
READ_BYTES = 0x1402C9F90           # (stream, &ptr, n): points at n raw bytes
READ_GUID = 0x141550B20            # packed 128-bit ObjectGuid: u8 loMask, u8 hiMask, set bytes
READ_BITS = 0x14071F930            # (bitreader*, n) -> n bits, MSB first
READ_MASK = 0x1425071F0            # (bitreader*, u32 *dst, u32 *end, nbits, clear): mask words
SMEM_ALLOC, SMEM_FREE = 0x140255140, 0x140255210
CDATASTORE_LO, CDATASTORE_HI = 0x1402C9000, 0x1402CB000

FIELD_FLAGS = {0x01: "Owner", 0x02: "PartyMember", 0x04: "UnitAll", 0x08: "Empath"}

# type id -> (class vtable, [(struct, data offset in the object, create reader, update reader,
#             changed-type-mask bit)]).  From the CG*_C vtables' slots 0x368 (values create) and
#             0x370 (values update); the type ids are the switch in 0x1418eeb80.
TYPES = {
    "Object":       (0, 0x1427F9960),
    "Item":         (1, 0x1427FC2D8),
    "Container":    (2, 0x1427FE3D8),
    "Unit":         (5, 0x1427F9E70),
    "Player":       (6, 0x1427EF7F8),
    "ActivePlayer": (7, 0x1427ED6D0),
    "GameObject":   (8, 0x1427F4748),
    "DynamicObject": (9, 0x1427FBDF0),
    "Corpse":       (10, 0x1427FB7A0),
    "AreaTrigger":  (11, 0x1427FB298),
    "SceneObject":  (12, 0x1427FD940),
    "Conversation": (13, 0x1427F04D0),
}
# the data structs, their offset in the CG object and their reader pair
STRUCTS = {
    "ObjectData":       dict(ctor=0x1424EA8E0, off=0xD8,   create=0x1424FE880, update=0x142510E80, bit=0x0001),
    "ItemData":         dict(off=0x178,  create=0x1424FD470, update=0x14250F900, bit=0x0002),
    "ContainerData":    dict(off=0x470,  create=0x1424FBF80, update=0x14250E390, bit=0x0004),
    "UnitData":         dict(ctor=0x1424EAA10, off=0xD668, create=0x1425019E0, update=0x142514530, bit=0x0020),
    "PlayerData":       dict(ctor=0x1424EA8F0, off=0xDD48, create=0x1424FEC10, update=0x142511490, bit=0x0040),
    "ActivePlayerData": dict(ctor=0x1424EA250, off=0xE600, create=0x1424F5700, update=0x1425072C0, bit=0x0080),
    "GameObjectData":   dict(ctor=0x1424EA830, off=0x1E0,  create=0x1424FC770, update=0x14250EE20, bit=0x0100),
    "DynamicObjectData": dict(off=0x188, create=0x1424FC6B0, update=0x14250ED10, bit=0x0200),
    "CorpseData":       dict(ctor=0x1424EA7D0, off=0x158,  create=0x1424FC390, update=0x14250E8A0, bit=0x0400),
    "AreaTriggerData":  dict(ctor=0x1424EA740, off=0x510,  create=0x1424FBAD0, update=0x14250DEA0, bit=0x0800),
    "SceneObjectData":  dict(off=0x140,  create=0x142500FB0, update=0x142513A00, bit=0x1000),
    "ConversationData": dict(off=0x180,  create=0x1424FC250, update=0x14250E6B0, bit=0x2000),
}
TYPE_STRUCTS = {
    "Object": ["ObjectData"],
    "Item": ["ObjectData", "ItemData"],
    "Container": ["ObjectData", "ItemData", "ContainerData"],
    "Unit": ["ObjectData", "UnitData"],
    "Player": ["ObjectData", "UnitData", "PlayerData"],
    "ActivePlayer": ["ObjectData", "UnitData", "PlayerData", "ActivePlayerData"],
    "GameObject": ["ObjectData", "GameObjectData"],
    "DynamicObject": ["ObjectData", "DynamicObjectData"],
    "Corpse": ["ObjectData", "CorpseData"],
    "AreaTrigger": ["ObjectData", "AreaTriggerData"],
    "SceneObject": ["ObjectData", "SceneObjectData"],
    "Conversation": ["ObjectData", "ConversationData"],
}

DATA = 0x7FF4_0000_0000
DATA_SIZE = 0x40000
DATA_PTR = DATA + 0x1000          # the struct under test
STREAM = DATA + DATA_SIZE - 0x1000  # a fake CDataStore; only +0x18/+0x1c are read
RAW = DATA + DATA_SIZE - 0x800     # what READ_BYTES points at (zeros)
DELTA_CTX = 0x1431890F0            # FUN_1418e9d10(): the change-notification context


class Explode(Exception):
    """A value the policy served as all-ones turned out to be a count."""


@dataclass
class Event:
    idx: int
    kind: str          # u8 u16 u32 u64 guid bytes bits mask
    nbits: int
    site: int          # return address of the read (for READ_MASK words: READ_MASK's caller)
    value: int
    writes: list = field(default_factory=list)   # (rip, where, size, is_float)
    stack: tuple = ()

    def key(self):
        return (self.kind, self.nbits, self.site)


class Tracer(Oracle):
    def __init__(self) -> None:
        super().__init__()
        uc = self.uc
        uc.mem_map(DATA, DATA_SIZE)
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self._float_cache: dict[int, bool] = {}
        self._frame_cache: dict[int, object] = {}
        hooks = {READ_U8: self._u8, READ_U16: self._u16, READ_U32: self._u32, READ_U64: self._u64,
                 READ_BYTES: self._bytes, READ_GUID: self._guid, READ_BITS: self._bits,
                 SMEM_ALLOC: self._alloc, SMEM_FREE: self._free}
        self._hooks = hooks
        for a in hooks:
            uc.hook_add(UC_HOOK_CODE, self._dispatch, begin=a, end=a)
        uc.hook_add(UC_HOOK_CODE, self._mask_entry, begin=READ_MASK, end=READ_MASK)
        uc.hook_add(UC_HOOK_CODE, self._cds_guard, begin=CDATASTORE_LO, end=CDATASTORE_HI)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write, begin=DATA, end=DATA + DATA_SIZE - 1)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write, begin=HEAP + 0x800000, end=HEAP + HEAP_SIZE - 1)
        self._mask_lo, self._mask_hi = READ_MASK, READ_MASK + 0xCA
        self.ctor = None
        self._delta_snapshot = bytes(uc.mem_read(DELTA_CTX, 0x100))

    # ---- run -------------------------------------------------------------------------------
    def run(self, func: int, args: list[int], policy, max_events: int = 200_000) -> list[Event]:
        """policy(event_index, kind, nbits, site, stack) -> value"""
        uc = self.uc
        uc.mem_write(DATA, b"\0" * DATA_SIZE)
        uc.mem_write(STREAM + 0x18, struct.pack("<II", 0x7FFFFFFF, 0))
        self.heap_lo = HEAP + 0x800000
        self.heap_next = self.heap_lo
        self.allocs: list[tuple[int, int]] = []
        self.events: list[Event] = []
        self.explode = None
        # the old-value delta arena (0x1431890f0 + 0x18, `0x14251e270`) is a global: restore it
        # so every run starts identical (its blocks are allocated afresh from our heap)
        uc.mem_write(DELTA_CTX, self._delta_snapshot)
        if self.ctor:
            # the struct's own constructor: optionals start as tagged-empty pointers, dynamic
            # arrays as their empty headers, exactly as the CG object's constructor leaves them
            self.call(self.ctor, [DATA_PTR])
        self.policy = policy
        self.max_events = max_events
        self.pos = 0
        self.mask_caller = 0
        self.explode = None
        self.bitstate: dict[int, int] = {}
        try:
            self.call(func, args, max_instructions=20_000_000)
        except (OracleError, UcError) as e:
            if self.explode is not None:
                raise self.explode
            raise OracleError(f"{e} after {len(self.events)} events") from e
        if self.explode is not None:
            raise self.explode
        return self.events

    def _ret(self, uc, rax: int) -> None:
        rsp = uc.reg_read(UC_X86_REG_RSP)
        ret = struct.unpack("<Q", uc.mem_read(rsp, 8))[0]
        uc.reg_write(UC_X86_REG_RSP, rsp + 8)
        uc.reg_write(UC_X86_REG_RAX, rax & 0xFFFFFFFFFFFFFFFF)
        uc.reg_write(UC_X86_REG_RIP, ret)

    def _retaddr(self, uc) -> int:
        return struct.unpack("<Q", uc.mem_read(uc.reg_read(UC_X86_REG_RSP), 8))[0]

    def _dispatch(self, uc, address, size, _u) -> None:
        if self.explode is not None:
            uc.emu_stop()
            return
        try:
            self._hooks[address](uc)
        except Explode as e:
            self.explode = e
            uc.emu_stop()

    def _mask_entry(self, uc, address, size, _u) -> None:
        self.mask_caller = self._retaddr(uc)

    def _cds_guard(self, uc, address, size, _u) -> None:
        if address in self._hooks or not self.bin.is_func_start(address):
            return
        self.explode = OracleError(f"unhooked CDataStore function {address:#x} called from "
                                   f"{self._retaddr(uc):#x}")
        uc.emu_stop()

    def _frame_at(self, ret: int):
        """For a return address inside a small read helper (FUN_14251a050 = `return ReadU32()`,
        and its u8/u16/u64/float siblings), the helper's stack use at that call; else None."""
        if ret in self._frame_cache:
            return self._frame_cache[ret]
        import bisect
        b = self.bin
        i = bisect.bisect_right(b.piece_starts, ret) - 1
        usage = None
        if i >= 0 and ret < b.piece_ends[i] and b.piece_ends[i] - b.piece_starts[i] <= 0x60 \
                and b.piece_root[i] == b.piece_starts[i]:
            start = b.piece_starts[i]
            usage = 0
            code = bytes(b.image[b.off(start):b.off(ret)])
            for insn in self.cs.disasm(code, start):
                if insn.mnemonic == "push":
                    usage += 8
                elif insn.mnemonic == "sub" and insn.op_str.startswith("rsp, "):
                    usage += int(insn.op_str.split(", ")[1], 0)
                elif insn.mnemonic == "call" and insn.address + insn.size != ret:
                    usage = None          # not a thin wrapper
                    break
        self._frame_cache[ret] = usage
        return usage

    def _event(self, uc, kind: str, nbits: int) -> Event:
        site = self._retaddr(uc)
        usage = self._frame_at(site)
        if usage is not None:
            # a thin read helper: the field is its caller's, so is the site
            rsp = uc.reg_read(UC_X86_REG_RSP)
            caller = struct.unpack("<Q", uc.mem_read(rsp + 8 + usage, 8))[0]
            if self.bin.is_code(caller):     # (a helper reached by a tail jump keeps its own)
                site = caller
        if self._mask_lo <= site < self._mask_hi:
            kind, site = "mask", self.mask_caller
        ev = Event(len(self.events), kind, nbits, site, 0)
        if len(self.events) >= self.max_events:
            raise Explode(ev)
        ev.value = self.policy(ev) & ((1 << nbits) - 1 if nbits < 64 else (1 << 64) - 1)
        self.events.append(ev)
        return ev

    def _advance(self, n: int) -> None:
        self.pos += n
        self.uc.mem_write(STREAM + 0x1C, struct.pack("<I", self.pos & 0xFFFFFFFF))

    def _prim(self, uc, kind: str, n: int) -> None:
        ev = self._event(uc, kind, 8 * n)
        dst = uc.reg_read(UC_X86_REG_RDX)
        uc.mem_write(dst, ev.value.to_bytes(n, "little"))
        self._advance(n)
        self._ret(uc, uc.reg_read(UC_X86_REG_RCX))

    def _u8(self, uc):
        self._prim(uc, "u8", 1)

    def _u16(self, uc):
        self._prim(uc, "u16", 2)

    def _u32(self, uc):
        self._prim(uc, "u32", 4)

    def _u64(self, uc):
        self._prim(uc, "u64", 8)

    def _bytes(self, uc):
        n = uc.reg_read(UC_X86_REG_R8) & 0xFFFFFFFF
        ev = self._event(uc, "bytes", 0)
        ev.nbits = n * 8
        uc.mem_write(uc.reg_read(UC_X86_REG_RDX), struct.pack("<Q", RAW))
        self._advance(n)
        self._ret(uc, uc.reg_read(UC_X86_REG_RCX))

    def _guid(self, uc):
        ev = self._event(uc, "guid", 128)
        dst = uc.reg_read(UC_X86_REG_RDX)
        uc.mem_write(dst, ev.value.to_bytes(16, "little"))
        ev.writes.append((self._retaddr(uc), dst, 16, False))   # a host write: no hook fires
        lo, hi = ev.value & ((1 << 64) - 1), ev.value >> 64
        self._advance(2 + sum(1 for i in range(8) if (lo >> 8 * i) & 0xFF)
                      + sum(1 for i in range(8) if (hi >> 8 * i) & 0xFF))
        self._ret(uc, uc.reg_read(UC_X86_REG_RCX))

    def _bits(self, uc):
        reader = uc.reg_read(UC_X86_REG_RCX)
        n = uc.reg_read(UC_X86_REG_RDX) & 0xFFFFFFFF
        ev = self._event(uc, "bits", n)
        pos = uc.mem_read(reader + 9, 1)[0]
        for _ in range(n):
            if pos == 8:
                pos = 0
                self._advance(1)
            pos += 1
        uc.mem_write(reader + 8, bytes([0, pos]))
        self._ret(uc, ev.value)

    def _alloc(self, uc):
        n = uc.reg_read(UC_X86_REG_RCX)
        if n > 0x100000:
            raise Explode(f"SMemAlloc({n:#x})")
        self.heap_next = (self.heap_next + 15) & ~15
        p = self.heap_next
        if p + n >= HEAP + HEAP_SIZE:
            raise Explode("heap exhausted")
        self.heap_next += max(n, 16)
        uc.mem_write(p, b"\0" * n)
        self.allocs.append((p, n))
        self._ret(uc, p)

    def _free(self, uc):
        self._ret(uc, 1)

    def _is_float_store(self, rip: int) -> bool:
        r = self._float_cache.get(rip)
        if r is None:
            code = bytes(self.uc.mem_read(rip, 16))
            insn = next(self.cs.disasm(code, rip), None)
            r = bool(insn) and insn.mnemonic in ("movss", "vmovss", "movd", "vmovd") and "xmm" in insn.op_str
            self._float_cache[rip] = r
        return r

    def _on_write(self, uc, access, address, size, value, _u) -> None:
        if not self.events:
            return
        if address >= STREAM - 0x10:
            return
        rip = uc.reg_read(UC_X86_REG_RIP)
        if READ_BITS <= rip < READ_BITS + 0xC43:
            return
        self.events[-1].writes.append((rip, address, size, self._is_float_store(rip)))


# ---- policies ------------------------------------------------------------------------------

def ones(n: int) -> int:
    return (1 << n) - 1 if n < 64 else (1 << 64) - 1


class Policy:
    """Serves values: masks all-ones, counts zero; `overrides` by event index."""

    def __init__(self, sizes: set, overrides: dict | None = None, default_u32: int = 0,
                 site_values: dict | None = None):
        self.sizes = sizes                 # sites known to be counts
        self.overrides = overrides or {}
        self.site_values = site_values or {}
        self.default_u32 = default_u32

    def __call__(self, ev: Event) -> int:
        if ev.idx in self.overrides:
            return self.overrides[ev.idx]
        if ev.site in self.site_values:
            return self.site_values[ev.site]
        if ev.kind == "mask":
            return ones(ev.nbits)
        if ev.kind == "bits":
            if ev.site in self.sizes:
                return 0
            return ones(ev.nbits)
        return 0


def classify_sizes(tr: Tracer, func: int, args: list[int], sizes: set, overrides=None,
                   site_values=None) -> list[Event]:
    """Runs with masks all-ones; a bits request that blows up (a count served as all-ones) is
    marked a count and the run repeated."""
    for _ in range(400):
        try:
            return tr.run(func, args, Policy(sizes, overrides, site_values=site_values))
        except Explode:
            # the culprit: the latest all-ones bits request from an unclassified site
            cands = [e for e in tr.events if e.kind == "bits" and e.nbits > 1
                     and e.site not in sizes and e.value == ones(e.nbits)
                     and e.idx not in (overrides or {})]
            if not cands:
                raise
            sizes.add(cands[-1].site)
    raise RuntimeError("classification did not converge")


# ---- trace rendering -----------------------------------------------------------------------

def where(tr: Tracer, address: int) -> str:
    if DATA <= address < DATA + DATA_SIZE:
        return f"+{address - DATA_PTR:#x}"
    for base, n in tr.allocs:
        if base <= address < base + max(n, 16):
            return f"heap[{address - base:#x}]"
    return f"{address:#x}"


def ev_dest(tr: Tracer, ev: Event):
    """The struct offset a read lands at, and whether it is stored as a float."""
    for rip, address, size, is_float in ev.writes:
        if DATA <= address < DATA + DATA_SIZE and address >= DATA_PTR:
            return address - DATA_PTR, size, is_float, None
        for base, n in tr.allocs:
            if base <= address < base + max(n, 16):
                return address - base, size, is_float, base
    return None, None, False, None


def type_of(ev: Event, size, is_float) -> str:
    if ev.kind == "guid":
        return "ObjectGuid"
    if ev.kind == "bytes":
        return f"bytes[{ev.nbits // 8}]"
    if ev.kind in ("bits", "mask"):
        return f"bits{ev.nbits}"
    if is_float and ev.kind == "u32":
        return "float"
    return {"u8": "uint8", "u16": "uint16", "u32": "uint32", "u64": "uint64"}[ev.kind]


def heap_owner(tr: Tracer, base: int) -> str:
    """The struct offset that holds a heap block's pointer (a dynamic array's storage)."""
    mem = bytes(tr.uc.mem_read(DATA_PTR, DATA_SIZE - 0x2000))
    needle = struct.pack("<Q", base)
    i = mem.find(needle)
    if i >= 0:
        return f"+{i:#x}"
    for b2, n2 in tr.allocs:
        blk = bytes(tr.uc.mem_read(b2, n2))
        j = blk.find(needle)
        if j >= 0 and b2 != base:
            return f"{heap_owner(tr, b2)}->[{j:#x}]"
    return "?"


def dkeys(tr: Tracer, events: list[Event]) -> list[tuple]:
    """Diff keys: what was read, where from and where it landed, so repeated elements of an
    array stay distinguishable (a site alone makes a knocked-out element 0 look like element 12).
    Must be taken before the next run (heap blocks are per run)."""
    out = []
    blocks = {}
    for ev in events:
        off, size, is_float, heap = ev_dest(tr, ev)
        if heap is not None:
            blocks.setdefault(heap, len(blocks))
        out.append((ev.kind, ev.nbits, ev.site, off, None if heap is None else "h"))
    return out


def flat_rows(tr: Tracer, events: list[Event]) -> list[dict]:
    out: list[dict] = []
    owners: dict[int, str] = {}
    for ev in events:
        off, size, is_float, heap = ev_dest(tr, ev)
        if heap is not None and heap not in owners:
            owners[heap] = heap_owner(tr, heap)
        out.append(dict(i=ev.idx, kind=ev.kind, nbits=ev.nbits, type=type_of(ev, size, is_float),
                        site=f"{ev.site:#x}", off=off,
                        heap=owners.get(heap) if heap is not None else None))
    return out


def _same_shape(a: list[dict], b: list[dict]):
    """b repeats a (same sites/kinds/types/array shapes) at one constant offset delta: the
    delta, else None."""
    delta = None
    for x, y in zip(a, b):
        if x["site"] != y["site"] or x["kind"] != y["kind"] or x["type"] != y["type"] \
                or x["heap"] != y["heap"] or x.get("count", 1) != y.get("count", 1) \
                or x.get("stride") != y.get("stride"):
            return None
        if x["off"] is None or y["off"] is None:
            if x["off"] != y["off"]:
                return None
            continue
        d = y["off"] - x["off"]
        if d <= 0 or (delta is not None and d != delta):
            return None
        delta = d
    return delta if delta is not None else None


def rows(tr: Tracer, events: list[Event]) -> list[dict]:
    """Events -> rows. A run of reads that repeats (same call sites, offsets advancing by one
    constant stride) folds to one array row: `count` instances of its `members`, each member
    carrying the event indices of all its instances (`idxs`)."""
    flat = flat_rows(tr, events)
    for r in flat:
        r["idxs"] = [r["i"]]
        r["count"] = 1
    return _fold(_fold(flat, 1, 1), 2, 40)


def _fold(items: list[dict], pmin: int, pmax: int) -> list[dict]:
    """One folding pass: period 1 makes arrays of single reads, periods >= 2 make groups (a
    repeated struct) whose members may themselves be arrays."""
    out: list[dict] = []
    k = 0
    while k < len(items):
        best = None
        if items[k]["kind"] != "group":
            for p in range(pmin, pmax + 1):
                if k + 2 * p > len(items):
                    break
                first = items[k:k + p]
                if any(x["kind"] == "group" for x in first):
                    break
                stride = _same_shape(first, items[k + p:k + 2 * p])
                if stride is None:
                    continue
                n = 2
                while k + (n + 1) * p <= len(items):
                    if _same_shape(first, items[k + n * p:k + (n + 1) * p]) != stride * n:
                        break
                    n += 1
                best = (n, p, stride)
                break   # the shortest period that repeats wins
        if best is None:
            out.append(items[k])
            k += 1
            continue
        n, p, stride = best
        members = []
        for m in range(p):
            r = dict(items[k + m])
            r["idxs"] = [i for j in range(n) for i in items[k + j * p + m]["idxs"]]
            members.append(r)
        if p == 1:
            r = members[0]
            r["count"], r["stride"] = n, stride
            out.append(r)
        else:
            out.append(dict(i=members[0]["i"], kind="group", count=n, stride=stride,
                            off=members[0]["off"], heap=members[0]["heap"], members=members,
                            idxs=[i for m in members for i in m["idxs"]]))
        k += n * p
    return out


def walk_rows(rs: list[dict]):
    for r in rs:
        if r["kind"] == "group":
            yield from r["members"]
        else:
            yield r


def fmt_row(r: dict, indent: str = "") -> str:
    if r["kind"] == "group":
        head = f"{indent}{r['i']:5d} group[{r['count']}] stride {r['stride']:#x}"
        return "\n".join([head] + [fmt_row(m, indent + "      ") for m in r["members"]])
    loc = (f"{r['heap']}[{r['off']:#x}]" if r["heap"] else
           (f"+{r['off']:#x}" if r["off"] is not None else "-"))
    arr = f"[{r['count']}] stride {r['stride']:#x}" if r.get("count", 1) > 1 and "stride" in r else ""
    extra = ""
    if r.get("flags"):
        extra += " flags=" + "|".join(r["flags"])
    if r.get("gate"):
        extra += " gate=" + ",".join(f"{g['mask_event']}.{g['bit']}" for g in r["gate"] if g)
    return f"{indent}{r['i']:5d} {r['kind']:5s} {r['type']:11s} {loc:18s} {arr:16s} site {r['site']}{extra}"


# ---- create ----------------------------------------------------------------------------------

def create_args(func: int, flags: int) -> list[int]:
    # (data, stream, 0, fieldFlags, owner=null): a null owner skips every change notification
    return [DATA_PTR, STREAM, 0, flags, 0]


def analyse_create(tr: Tracer, name: str) -> dict:
    func = STRUCTS[name]["create"]
    tr.ctor = STRUCTS[name].get("ctor")
    sizes: set = set()
    full = 0xFF
    base = classify_sizes(tr, func, create_args(func, full), sizes)
    base_rows = rows(tr, base)

    # visibility: which reads each flag gates (run with the flag cleared from all-flags)
    def keys(evs):
        return dkeys(tr, evs)

    # A field is public if a run with no flags reads it; otherwise its visibility is every flag
    # that, alone, makes it appear (TC's conditions are ORs: Owner|UnitAll, Owner|Empath, ...).
    bkeys = keys(base)
    nothing = classify_sizes(tr, func, create_args(func, 0), sizes)
    absent0 = set(removed_events(bkeys, keys(nothing)))
    gated: dict[int, set] = {}
    for bit, fname in FIELD_FLAGS.items():
        evs = classify_sizes(tr, func, create_args(func, bit), sizes)
        absent = set(removed_events(bkeys, keys(evs)))
        for i in absent0 - absent:
            gated.setdefault(i, set()).add(fname)
    for i in absent0:
        gated.setdefault(i, set()).add("?")    # needs a flag combination no single flag gives

    # counts / presence bits: probe each u32/u8/bits site once with value 1 (u32) or ones
    probes = []
    seen = set()
    for ev in base:
        if ev.kind not in ("u32", "u8", "bits") or ev.site in seen:
            continue
        seen.add(ev.site)
        val = 1 if (ev.kind != "bits" or ev.site in sizes) else 0
        if ev.kind == "bits" and ev.value == 0:
            val = 1
        if ev.kind == "bits" and ev.site not in sizes and ev.value == ones(ev.nbits):
            val = 0                         # presence bit served on: probe it off
        # a u8 can carry a count in its top bits (`WriteBits(n, 6); FlushBits()` = byte >> 2)
        cands = [val] + ([0x04, 0x80] if ev.kind == "u8" else [])
        evs = None
        for val in cands:
            try:
                got = classify_sizes(tr, func, create_args(func, full), sizes,
                                     overrides={ev.idx: val})
            except (Explode, OracleError, RuntimeError):
                continue
            if keys(got) != bkeys:
                evs = got
                break
        if evs is None:
            continue
        sm = difflib.SequenceMatcher(None, bkeys, keys(evs), autojunk=False)
        added, removed = [], []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag in ("insert", "replace"):
                added.extend(evs[j1:j2])
            if tag in ("delete", "replace"):
                removed.extend(range(i1, i2))
        probes.append(dict(at=ev.idx, site=f"{ev.site:#x}", kind=ev.kind, value=val,
                           added=rows(tr, added), removed=removed))
    for r in walk_rows(base_rows):
        g = set()
        for i in r["idxs"]:
            g |= gated.get(i, set())
        if len(g) > 1:
            g.discard("?")
        r["flags"] = sorted(g)
    return dict(func=f"{func:#x}", events=len(base), events_no_flags=len(nothing),
                rows=base_rows, probes=probes, count_sites=sorted(f"{s:#x}" for s in sizes))


# ---- update ----------------------------------------------------------------------------------

def removed_events(a: list, b: list) -> list[int]:
    """Indices of `a` missing from `b`: common suffix, then prefix, stripped first (a knockout
    only removes a span), the rest diffed. Suffix first: a removed array element is its own
    change mask (a read with no destination, identical to the next element's) followed by its
    values, so matching from the end puts the cut before the right mask."""
    s = 0
    while s < len(a) and s < len(b) and a[-1 - s] == b[-1 - s]:
        s += 1
    p = 0
    while p < len(a) - s and p < len(b) - s and a[p] == b[p]:
        p += 1
    mid_a, mid_b = a[p:len(a) - s], b[p:len(b) - s]
    if not mid_b:
        return list(range(p, len(a) - s))
    sm = difflib.SequenceMatcher(None, mid_a, mid_b, autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag in ("delete", "replace"):
            out.extend(range(p + i1, p + i2))
    return out


def update_args(func: int) -> list[int]:
    return [DATA_PTR, STREAM, 0, 0]


def analyse_update(tr: Tracer, name: str, max_knock: int = 4000) -> dict:
    func = STRUCTS[name]["update"]
    tr.ctor = STRUCTS[name].get("ctor")
    sizes: set = set()
    base = classify_sizes(tr, func, update_args(func), sizes)
    base_rows = rows(tr, base)

    def keys(evs):
        return dkeys(tr, evs)

    bkeys = keys(base)
    # every mask request in the baseline, every bit of it knocked out in turn
    masks = [e for e in base if (e.kind == "mask" or (e.kind == "bits" and e.site not in sizes))]
    gate: dict[int, tuple] = {}         # event idx -> (mask event idx, bit, removed count)
    gates_all: dict[int, list] = {}     # event idx -> every (mask event, bit, removed) removing it
    done_sites: dict[tuple, int] = {}
    knocks = 0
    mask_info = []
    prev_idx, ordinal = None, 0
    for m in masks:
        mask_info.append(dict(i=m.idx, kind=m.kind, nbits=m.nbits, site=f"{m.site:#x}"))
        # the n-th word of one run of mask words is its own key: a 48-block mask reads its words
        # from one site, and they must all be probed; a nested struct's mask repeated per array
        # element (separated by its fields) restarts at ordinal 0 and is probed twice only
        ordinal = ordinal + 1 if (prev_idx is not None and m.idx == prev_idx + 1
                                  and base[prev_idx].site == m.site) else 0
        prev_idx = m.idx
        k = (m.site, m.nbits, m.kind, ordinal)
        done_sites[k] = done_sites.get(k, 0) + 1
        if done_sites[k] > 2:           # repeated nested struct (array element): probed twice
            continue
        for bit in range(m.nbits):
            knocks += 1
            if knocks > max_knock:
                break
            try:
                evs = classify_sizes(tr, func, update_args(func), sizes,
                                     overrides={m.idx: ones(m.nbits) & ~(1 << bit)})
            except (Explode, OracleError, RuntimeError):
                continue
            removed = removed_events(bkeys, keys(evs))
            for i in removed:
                if i == m.idx:
                    continue
                gates_all.setdefault(i, []).append((m.idx, bit, len(removed)))
                cur = gate.get(i)
                # the most specific bit gates the fewest reads; on a tie the later bit wins
                # (a group bit is read before the field bits it guards)
                if cur is None or len(removed) <= cur[2]:
                    gate[i] = (m.idx, bit, len(removed))
    for r in walk_rows(base_rows):
        r["gate"] = [None if gate.get(i) is None else dict(mask_event=gate[i][0], bit=gate[i][1])
                     for i in r["idxs"]]
        first = r["idxs"][0]
        g = gate.get(first)
        r["parents"] = [dict(mask_event=a, bit=b) for a, b, n in
                        sorted(gates_all.get(first, []), key=lambda t: -t[2])
                        if g is None or (a, b) != (g[0], g[1])]
    # dynamic arrays: serve each count site 1 once, show one element's reads
    probes = []
    for s in sorted(sizes):
        first = next((e for e in base if e.site == s), None)
        if first is None:
            continue
        try:
            evs = classify_sizes(tr, func, update_args(func), set(sizes),
                                 overrides={first.idx: 1})
        except (Explode, OracleError, RuntimeError) as e:
            probes.append(dict(at=first.idx, site=f"{s:#x}", error=str(e)[:200]))
            continue
        sm = difflib.SequenceMatcher(None, bkeys, keys(evs), autojunk=False)
        added = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag in ("insert", "replace"):
                added.extend(evs[j1:j2])
        probes.append(dict(at=first.idx, site=f"{s:#x}", added=rows(tr, added)))
    return dict(func=f"{func:#x}", events=len(base), rows=base_rows, masks=mask_info,
                count_sites=sorted(f"{s:#x}" for s in sizes), probes=probes, knockouts=knocks)


# ---- docs ----------------------------------------------------------------------------------

NAMES_FILE = Path(__file__).resolve().parent / "updatefields_names.json"
TC_FLAG_NOTE = ("TrinityCore's UpdateFieldFlag: None=0, Owner=0x01, PartyMember=0x02, UnitAll=0x04, "
                "Empath=0x08")


def _names() -> dict:
    if NAMES_FILE.exists():
        return json.loads(NAMES_FILE.read_text())
    return {}


def _hx(v):
    return None if v is None else f"{v:#x}"


def _name(names: dict, struct: str, off, heap=None, default=None, site=None, group=False):
    """Names file keys: "0x40" a field at that struct offset; "struct@0x170" an array of structs
    starting there; "+0xc0[0x2]" a member of a dynamic array's element (the array's storage
    pointer lives at +0xc0); "dyn@+0xc0" the dynamic array itself; "site@0x..." a read with no
    struct offset (a size or a bit), by the address it is read at."""
    tbl = names.get(struct, {})
    if group:
        return tbl.get(f"struct@{off:#x}") if off is not None else None
    if heap:
        n = tbl.get(f"{heap}[{off:#x}]") if off is not None else None
    elif off is None:
        n = None
    else:
        n = tbl.get(f"{off:#x}")
    if n is None and site:
        n = tbl.get(f"site@{site}")
    return n if n is not None else default


def _bitmap(u: dict):
    """(event idx -> block index) for the top-level mask words, the block-mask event indices,
    and whether the struct has blocks at all."""
    masks = u["masks"]
    if not masks:
        return {}, [], False
    m0 = masks[0]
    parts = [m0["i"]]
    for m in masks[1:]:
        if m["i"] == parts[-1] + 1 and m["site"] == m0["site"] and m["kind"] == m0["kind"]:
            parts.append(m["i"])
        else:
            break
    # With every block set (the baseline), the words follow the block mask in block order.
    # (Their gates cannot say which is which: the words are identical reads from one site.)
    nblocks = sum(m["nbits"] for m in masks[:len(parts)])
    words = {}
    nxt = parts[-1] + 1
    for m in masks[len(parts):]:
        if m["i"] != nxt or m["nbits"] != 32 or len(words) >= nblocks:
            break
        words[m["i"]] = len(words)
        nxt += 1
    if len(words) != nblocks:
        words = {}
    return words, parts, bool(words)


def _abs_bit(g, words, parts, blocked):
    if not g:
        return None
    if blocked and g["mask_event"] in words:
        return 32 * words[g["mask_event"]] + g["bit"]
    if not blocked and g["mask_event"] == parts[0]:
        return g["bit"]
    if blocked and g["mask_event"] in parts:
        return f"block {32 * parts.index(g['mask_event']) + g['bit']}"
    return f"elem.{g['bit']}"     # a bit of a nested struct's (array element's) own mask


def _entry(names, struct, r, *, flags=True, bits=None, base_off=0):
    if r["kind"] == "group":
        goff = next((m["off"] for m in r["members"] if m["off"] is not None), None)
        e = dict(kind="struct_array", count=r["count"], stride=_hx(r["stride"]),
                 offset=_hx(goff), name=_name(names, struct, goff, group=True),
                 members=[_entry(names, struct, m, flags=flags, bits=bits, base_off=r["off"] or 0)
                          for m in r["members"]])
        if bits:
            e["bits"] = sorted({b for m in e["members"] for b in m.get("bits", []) if isinstance(b, int)})
        return e
    e = dict(name=_name(names, struct, r["off"], r["heap"], site=r["site"]), type=r["type"],
             offset=_hx(r["off"]))
    if r["heap"]:
        e["in"] = r["heap"]
    if r.get("count", 1) > 1:
        e["count"], e["stride"] = r["count"], _hx(r.get("stride"))
    if flags:
        e["visibility"] = r.get("flags") or []
    if bits:
        words, parts, blocked = bits
        bl = [_abs_bit(g, words, parts, blocked) for g in (r.get("gate") or [])]
        e["bits"] = bl
        e["parent_bits"] = [_abs_bit(g, words, parts, blocked) for g in (r.get("parents") or [])]
    e["site"] = r["site"]
    e["kind"] = r["kind"]
    return e


def _dyn(names, struct, probes, rows_list, bits=None):
    """Attach dynamic-array element layouts (from the count probes) to the rows they belong to."""
    out = {}
    for p in probes:
        if "added" not in p:
            continue
        added = p["added"]
        flat = list(walk_rows(added))
        elems = [a for a in added if a.get("i") != p["at"]]
        if not elems:
            continue
        owner = next((a["off"] for a in flat if a.get("i") == p["at"] and a["off"] is not None), None)
        out[p["at"]] = dict(
            size_field_offset=_hx(owner),
            elements_follow_event=elems[0]["i"] - 1,
            element=[_entry(names, struct, a, flags=False) for a in elems])
    return out


def _dyn_name(names, struct, e, d):
    """A dynamic array's size read is named after its storage ("dyn@+0xc0")."""
    ins = [el.get("in") for el in d["element"]
           if el.get("in") and el.get("kind") not in ("bits", "mask")]
    store = max(set(ins), key=ins.count) if ins else None
    if store:
        d["storage"] = store
        n = names.get(struct, {}).get(f"dyn@{store}")
        if n:
            e["name"] = n


def build_struct(name: str, raw: dict, names: dict) -> dict:
    c, u = raw["create"], raw["update"]
    bits = _bitmap(u)
    words, parts, blocked = bits
    nbits = None
    if raw["update"]["masks"]:
        if blocked:
            nb_blocks = sum(next(m["nbits"] for m in u["masks"] if m["i"] == p) for p in parts)
            nbits = f"{nb_blocks} blocks x 32"
        else:
            nbits = u["masks"][0]["nbits"]
    create = [_entry(names, name, r) for r in c["rows"]]
    cdyn = _dyn(names, name, c["probes"], c["rows"])
    idx_entry = {}
    for r, e in zip(c["rows"], create):
        idx_entry[r["i"]] = e
        for i in r.get("idxs", []):
            idx_entry.setdefault(i, e)
    for at, d in cdyn.items():
        e = idx_entry.get(at)
        after = idx_entry.get(d["elements_follow_event"])
        d["elements_follow"] = (after or {}).get("name") or (after or {}).get("offset")
        if e is not None:
            e["dynamic"] = d
            e["type"] = "uint32 (size)"
            _dyn_name(names, name, e, d)
    update = [_entry(names, name, r, flags=False, bits=bits) for r in u["rows"]]
    udyn = _dyn(names, name, u["probes"], u["rows"], bits)
    uidx = {}
    for r, e in zip(u["rows"], update):
        for i in r.get("idxs", [r["i"]]):
            uidx.setdefault(i, e)
    for at, d in udyn.items():
        e = uidx.get(at)
        if e is not None:
            e["dynamic"] = d
            e["type"] = "bits32 (size)"
            _dyn_name(names, name, e, d)
    # the mask reads themselves
    def _all(es):
        for x in es:
            yield x
            if x.get("kind") == "struct_array":
                yield from _all(x["members"])
    mask_rows = [e for e in _all(update) if e.get("kind") in ("bits", "mask") and "dynamic" not in e]
    for e in mask_rows:
        e["role"] = "change mask"
    return dict(
        object_offset=raw["off"], type_mask_bit=f"{raw['bit']:#06x}",
        create_reader=c["func"], update_reader=u["func"], constructor=_hx(raw.get("ctor")),
        create_reads_all_flags=c["events"], create_reads_no_flags=c["events_no_flags"],
        update_mask=dict(layout="blocks" if blocked else "single", bits=nbits,
                         block_mask_reads=len(parts)),
        create=create, update=update)


def write_docs(raw: dict) -> None:
    names = _names()
    structs = {n: build_struct(n, raw[n], names) for n in STRUCTS if n in raw}
    types = {t: dict(type_id=TYPES[t][0], vtable=f"{TYPES[t][1]:#x}", structs=TYPE_STRUCTS[t])
             for t in TYPES}
    doc = dict(
        _about="Extracted from WowClassic 3.4.3.54261 by tools/updatefields.py (emulating the "
               "client's own update-field readers). Offsets are within each data struct; names "
               "are TrinityCore's where the client's shape matches (name_source: inferred), "
               "null where unknown.",
        system="changes-mask (modern UpdateField / ViewerDependent system; no descriptor table)",
        name_source="inferred",
        field_flags={f"{k:#04x}": v for k, v in FIELD_FLAGS.items()},
        types=types, structs=structs)
    (DOCS / "updatefields.json").write_text(json.dumps(doc, indent=1))
    (DOCS / "updatefields.md").write_text(render_md(doc))
    print("wrote docs/updatefields.json and docs/updatefields.md")


def _fmt_type(e):
    t = e.get("type", "")
    if e.get("count", 1) > 1:
        t += f"[{e['count']}]"
    return t


def _fmt_bits(e):
    b = [x for x in e.get("bits", []) if x is not None]
    if not b:
        return ""
    if isinstance(b[0], str) and b[0].startswith("elem."):
        s = f"element mask bit {b[0][5:]}"
    elif all(isinstance(x, int) for x in b) and len(b) > 2 and b == list(range(b[0], b[0] + len(b))):
        s = f"{b[0]}..{b[-1]}"
    else:
        s = ", ".join(str(x) for x in b[:6]) + (" ..." if len(b) > 6 else "")
    pb = []
    for x in e.get("parent_bits", []):
        if x is not None and x not in pb and not (isinstance(x, str) and x.startswith("elem.")):
            pb.append(x)
    if pb:
        s += " (needs " + ", ".join(str(x) for x in pb[:4]) + ")"
    return s


def _md_rows(entries, update=False, depth=0):
    lines = []
    pad = "&nbsp;&nbsp;" * depth
    for n, e in enumerate(entries):
        if e.get("kind") == "struct_array":
            head = f"| {pad}{e.get('name') or '(struct)'} | struct[{e['count']}] stride {e['stride']} | {e['offset']} |"
            if update:
                head += f" {_fmt_bits(e)} |"
            else:
                head += " |"
            lines.append(head)
            lines.extend(_md_rows(e["members"], update, depth + 1))
            continue
        name = e.get("name") or ""
        if e.get("role") == "change mask":
            if name and not str(e.get("type", "")).endswith("bits1"):
                name = f"*(nested change mask; also sets {name})*"
            elif name:
                name = f"{name} (bit)"
            else:
                name = "*(change mask)*"
        typ = _fmt_type(e)
        off = e.get("offset") or ""
        if e.get("in"):
            off = f"{e['in']}[{off}]"
        if update:
            # the struct's own top-level mask words carry no field bit of their own
            extra = "" if (e.get("role") == "change mask" and e.get("offset") is None and depth == 0
                           and str(e.get("type")) in ("bits32", "bits16") and n < 60) else _fmt_bits(e)
        else:
            extra = ", ".join(e.get("visibility") or []) or ""
        line = f"| {pad}{name} | {typ} | {off} | {extra} |"
        lines.append(line)
        if e.get("dynamic"):
            d = e["dynamic"]
            if update:
                lines.append(f"| {pad}&nbsp;&nbsp;↳ per element (after the struct's masks): | | | |")
            else:
                where = d.get("elements_follow")
                lines.append(f"| {pad}&nbsp;&nbsp;↳ elements (read after {where or 'the previous field'}) | | | |")
            for el in d["element"]:
                if el.get("kind") == "struct_array":
                    lines.extend(_md_rows([el], update, depth + 2))
                    continue
                nm = el.get("name") or ("*(element change mask)*" if el.get("kind") in ("bits", "mask") else "")
                off = "" if el.get("offset") is None else f"+{el.get('offset')}"
                lines.append(f"| {pad}&nbsp;&nbsp;&nbsp;&nbsp;{nm} | {_fmt_type(el)} | {off} | |")
    return lines


def render_md(doc: dict) -> str:
    out = ["# Update fields — WoW Classic 3.4.3.54261", "",
           "Generated by `tools/updatefields.py` from the client's own deserializers (run under "
           "emulation); do not edit by hand. The machine-readable form is `updatefields.json`. "
           "The SMSG_UPDATE_OBJECT framing around these payloads is in "
           "[updateobject.md](updateobject.md).", "",
           "## Which system", "",
           "3.4.3 uses the modern **changes-mask** update-field system, not the 3.3.5 flat "
           "u32-index descriptor table. The client contains no descriptor tables and no field-name "
           "strings: each data struct has a generated reader pair, `ReadCreate` (full values, "
           "filtered by visibility flags) and `ReadUpdate` (a change mask, then only the changed "
           "values). Field names below are TrinityCore's (`wotlk_classic` UpdateFields.h) where the "
           "client's order, width, count and gating match; they are **[inferred]**. Offsets are "
           "byte offsets inside the client's data struct and are the stable identity of a field "
           "(the same offset is written by create and update).", "",
           "## Serialization rules (from the readers)", "",
           "**ValuesCreate** = `u32 size; u8 fieldFlags;` then each struct's create stream in "
           "type order. A create writes every field in declaration order with no masks; a field "
           "with a visibility flag is present only if `fieldFlags` has one of its flags. "
           f"Flags seen in the readers: {', '.join(f'{k} {v}' for k, v in doc['field_flags'].items())} "
           f"({TC_FLAG_NOTE}). A dynamic array writes `u32 size` at its place in the order; its "
           "elements follow later in the stream (see each table's *elements (read after …)* row). "
           "Fixed arrays are written element by element; arrays of structs element by element, "
           "each element's members in order.", "",
           "**ValuesUpdate** = `u32 size; u32 changedTypeMask;` then, for each struct whose type "
           "bit is set, a fresh bit stream: the change mask, then the changed values. For a "
           "struct with more than 32 mask bits the mask is a *block mask* (one bit per 32-bit "
           "block, `N` bits) followed by one 32-bit word for each set block; a struct with ≤ 32 "
           "bits writes a single word of exactly that many bits. Bits are MSB-first; "
           "`WriteBits(v, n)` puts bit n-1 of v first. Each value is read only if its own bit is "
           "set **and** every parent bit listed with it (usually bit 0, the struct's \"any field "
           "in group\" bit, and for arrays the array's parent bit). Dynamic arrays write "
           "`bits32 size`, then a per-element change mask (`size` bits, in 32-bit words), then "
           "the changed elements. A nested struct (array element) writes its own change mask "
           "and values. Byte-aligned values after bits mean the bit stream was flushed "
           "(TC `FlushBits()`).", "",
           "Bit numbers below are absolute bits of the struct's change mask (block `b`, bit `k` = "
           "`32*b + k`). `nested#E.k` = bit k of a nested struct's own mask (E = the event index "
           "in the raw trace, see build/updatefields.raw.json).", "",
           "## Types", "", "| type | id | structs | vtable |", "|---|---|---|---|"]
    for t, v in doc["types"].items():
        out.append(f"| {t} | {v['type_id']} | {', '.join(v['structs'])} | {v['vtable']} |")
    out += ["", "## Summary", "",
            "| struct | type-mask bit | create reader | update reader | mask | create reads (all flags / none) |",
            "|---|---|---|---|---|---|"]
    for n, s in doc["structs"].items():
        m = s["update_mask"]
        out.append(f"| {n} | {s['type_mask_bit']} | {s['create_reader']} | {s['update_reader']} | "
                   f"{m['bits']} ({m['layout']}) | {s['create_reads_all_flags']} / "
                   f"{s['create_reads_no_flags']} |")
    for n, s in doc["structs"].items():
        out += ["", f"## {n}", "",
                f"Object offset {s['object_offset']}; create `{s['create_reader']}`, update "
                f"`{s['update_reader']}`; constructor {s['constructor'] or '(inline)'}.", "",
                "### Create (in wire order)", "",
                "| field | type | offset | visibility |", "|---|---|---|---|"]
        out += _md_rows(s["create"])
        out += ["", "### Update (in wire order)", "",
                f"Change mask: {s['update_mask']['bits']} ({s['update_mask']['layout']}).", "",
                "| field | type | offset | bit |", "|---|---|---|---|"]
        out += _md_rows(s["update"], update=True)
    return "\n".join(out) + "\n"


# ---- main ----------------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="struct name (UnitData) or prefix")
    ap.add_argument("--trace", help="print the raw baseline trace of one struct")
    ap.add_argument("--mode", default="create", choices=("create", "update"))
    ap.add_argument("--flags", type=lambda s: int(s, 0), default=0xFF)
    ap.add_argument("--out", default=str(ROOT / "build" / "updatefields.raw.json"))
    ap.add_argument("--docs", action="store_true",
                    help="only regenerate docs/updatefields.{json,md} from build/updatefields.raw.json")
    args = ap.parse_args()
    if args.docs:
        write_docs(json.loads(Path(args.out).read_text()))
        return
    tr = Tracer()
    if args.trace:
        s = STRUCTS[args.trace]
        tr.ctor = s.get("ctor")
        sizes: set = set()
        if args.mode == "create":
            evs = classify_sizes(tr, s["create"], create_args(s["create"], args.flags), sizes)
        else:
            evs = classify_sizes(tr, s["update"], update_args(s["update"]), sizes)
        for r in rows(tr, evs):
            print(fmt_row(r))
        print("count sites:", ", ".join(f"{x:#x}" for x in sorted(sizes)))
        return
    out = {}
    if Path(args.out).exists():
        out = json.loads(Path(args.out).read_text())
    for name in STRUCTS:
        if args.only and not name.startswith(args.only):
            continue
        t0 = time.time()
        c = analyse_create(tr, name)
        u = analyse_update(tr, name)
        out[name] = dict(STRUCTS[name] | dict(create=c, update=u))
        out[name]["off"] = f"{STRUCTS[name]['off']:#x}"
        print(f"{name}: create {c['events']} reads ({len(c['rows'])} rows), update {u['events']} reads "
              f"({len(u['rows'])} rows, {u['knockouts']} knockouts) in {time.time() - t0:.1f}s",
              flush=True)
    Path(args.out).parent.mkdir(exist_ok=True)
    Path(args.out).write_text(json.dumps(out, indent=1))
    print("wrote", args.out)
    if not args.only:
        write_docs(out)


if __name__ == "__main__":
    main()
