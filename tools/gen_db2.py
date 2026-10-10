"""Generates the client tables' metas, instances and load groups from the clean exe.

Every client table is a static WowClientDB2 built by a static initializer that calls the base
constructor (FUN_1405780a0) with the table and its meta: `lea rdx, [meta]; lea rcx, [table];
call`. The meta (0xb0 bytes, layout in src/db/DBMeta.hpp) points at the field arrays, the
unique-key descriptions and two generated record orders. DBClient's six load groups
(0x142ccdc70) are null-terminated lists of tables.

The record orders (meta 0x98 over records, meta 0xa0 over record pointers) are generated code:
a field read through one of the typed accessors for each side, then a compare. The simple shape
(one field, both sides, one set-on-condition) is decoded and written as C++; anything longer is
written as a tagged FROST_UNIMPLEMENTED stub until it is ported.

Writes:
    src/db/DB2Tables.hpp   every table, declared
    src/db/DB2Tables.cpp   the metas, the tables, the record orders and the load groups

    python tools/gen_db2.py
"""
from __future__ import annotations

import bisect
import re
from pathlib import Path

import capstone

from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent
CTOR = 0x1405780A0
# The sparse tables (WowClientSparseDB2.cpp): ConversationLine, ItemSparse, SceneScriptText and
# Spell. Their initializers call this base constructor instead, then make an ObjectAlloc heap
# for their records (FUN_1412401e0: the record size, 0x200 a heap, the record name).
SPARSE_CTOR = 0x1405A2CC0
POOL_CREATE = 0x1412401E0
GROUPS = 0x142CCDC70
GROUP_COUNT = 6

# The typed field accessors the generated orders call: address -> (C++ name, C++ type).
ACCESSORS = {
    0x14022B1B0: ("DB2FieldU8", "uint8_t"),
    0x140226EB0: ("DB2FieldU16", "uint16_t"),
    0x140233370: ("DB2FieldU32", "uint32_t"),
    0x140229DE0: ("DB2FieldI32", "int32_t"),
}
CONDS = {"setl": ("<", True), "setg": (">", True), "setb": ("<", False), "seta": (">", False)}

b = Binary()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def u8(va: int) -> int:
    return b.image[va - b.base]


def i32(va: int) -> int:
    return b.i32(va)


def insns(start: int) -> list:
    i = bisect.bisect_right(b.piece_starts, start) - 1
    end = b.piece_ends[i]
    code = bytes(b.image[start - b.base:end - b.base])
    return list(md.disasm(code, start))


def rip_target(insn) -> int | None:
    for op in insn.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            return insn.address + insn.size + op.mem.disp
    return None


def sparse_pool(site: int) -> tuple[str, int]:
    """The record name and size a sparse table's initializer gives its record heap."""
    name = None
    size = None
    for insn in insns(site):
        if insn.mnemonic == "lea" and insn.op_str.startswith("r8, [rip"):
            name = b.cstr(rip_target(insn))
        elif insn.mnemonic == "mov" and (mm := re.fullmatch(r"dword ptr \[r\w+ \+ 8\], (0x[0-9a-f]+|\d+)", insn.op_str)):
            size = int(mm.group(1), 0)
        elif insn.mnemonic == "call" and insn.op_str.startswith("0x") and int(insn.op_str, 16) == POOL_CREATE:
            break
    if name is None or size is None:
        raise SystemExit(f"sparse table at {site:#x}: no record heap found")
    return name, size


SPARSE = {}


def find_tables() -> list[tuple[int, int]]:
    tables = []
    sites = [(site, False) for site, _ in b.callers(CTOR)] + [(site, True) for site, _ in b.callers(SPARSE_CTOR)]
    for site, sparse in sites:
        meta = db = None
        for back in range(1, 40):
            a = site - back
            raw = bytes(b.image[a - b.base:a - b.base + 7])
            if raw[:3] == b"\x48\x8d\x15" and meta is None:
                meta = a + 7 + int.from_bytes(raw[3:7], "little", signed=True)
            if raw[:3] == b"\x48\x8d\x0d" and db is None:
                db = a + 7 + int.from_bytes(raw[3:7], "little", signed=True)
            if meta and db:
                break
        if meta is None or db is None or not b.section_of(b.u64(meta)):
            continue
        if sparse:
            SPARSE[db] = sparse_pool(site)
        elif db in SPARSE:
            continue
        tables.append((db, meta))
    tables.sort(key=lambda t: t[0])
    return tables


def ident(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z_]", "_", name)


def var_name(name: str) -> str:
    n = ident(name)
    return "g_" + n[0].lower() + n[1:] + "DB"


def c_string(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append(chr(92) * 2)
        elif ch == '"':
            out.append(chr(92) + '"')
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(chr(92) + "x%02x" % ord(ch))
    return '"' + "".join(out) + '"'


def int_array(va: int, count: int) -> list[int]:
    return [i32(va + 4 * k) for k in range(count)]


def decode_order(fn: int, deref: bool) -> dict | None:
    """The simple order shape: two accessor calls on the same field, one set-on-condition."""
    regs = {}
    rcx_off = 0
    calls = []
    cond = None
    for insn in insns(fn):
        m, ops = insn.mnemonic, insn.op_str
        if m == "ret":
            break
        if m == "xor" and ops in ("edx, edx", "r8d, r8d"):
            regs["edx" if ops.startswith("edx") else "r8"] = 0
        elif m == "mov" and re.fullmatch(r"rcx, r\w+", ops):
            rcx_off = 0
        elif m == "mov" and re.fullmatch(r"edx, (0x[0-9a-f]+|\d+)", ops):
            regs["edx"] = int(ops.split(", ")[1], 0)
        elif m == "lea" and (mm := re.fullmatch(r"edx, \[r8 \+ (0x[0-9a-f]+|\d+)\]", ops)):
            regs["edx"] = regs.get("r8", 0) + int(mm.group(1), 0)
        elif m == "lea" and (mm := re.fullmatch(r"r8d, \[rdx \+ (0x[0-9a-f]+|\d+)\]", ops)):
            regs["r8"] = regs.get("edx", 0) + int(mm.group(1), 0)
        elif m == "mov" and re.fullmatch(r"r8d, (0x[0-9a-f]+|\d+)", ops):
            regs["r8"] = int(ops.split(", ")[1], 0)
        elif m == "add" and (mm := re.fullmatch(r"rcx, (0x[0-9a-f]+|\d+)", ops)):
            rcx_off = int(mm.group(1), 0)
        elif m == "lea" and (mm := re.fullmatch(r"rcx, \[r\w+ \+ (0x[0-9a-f]+|\d+)\]", ops)):
            rcx_off = int(mm.group(1), 0)
        elif m == "lea" and ops.startswith("r9, [rip"):
            regs["r9"] = rip_target(insn)
        elif m == "call":
            target = int(ops, 16) if ops.startswith("0x") else None
            if target not in ACCESSORS:
                return None
            calls.append((target, regs.get("edx"), regs.get("r8"), rcx_off, regs.get("r9")))
            rcx_off = 0
        elif m in CONDS:
            cond = m
        elif m.startswith("j") or m.startswith("cmov"):
            return None
    if len(calls) != 2 or cond is None:
        return None
    (a1, f1, o1, r1, d1), (a2, f2, o2, r2, d2) = calls
    if a1 != a2 or f1 != f2 or o1 != o2 or r1 != r2 or d1 != d2 or None in (f1, o1, d1):
        return None
    return {"accessor": a1, "field": f1, "offset": o1, "rec": r1, "db": d1, "cond": cond, "deref": deref}


def main() -> None:
    tables = find_tables()
    names = {}
    for db, meta in tables:
        names[db] = var_name(b.cstr(b.u64(meta)))

    body = []
    instances = []
    orders = []
    decl = []
    order_decl = []
    unported = 0
    for db, meta in tables:
        name = b.cstr(b.u64(meta))
        n = ident(name)
        fdid = i32(meta + 0x08)
        file_fields = i32(meta + 0x0C)
        record_size = i32(meta + 0x10)
        fields = i32(meta + 0x14)
        index_field = i32(meta + 0x18)
        sparse = u8(meta + 0x1C)
        arrays = []
        for k, (off, count) in enumerate([(0x20, fields), (0x28, fields), (0x30, fields), (0x38, fields),
                                          (0x40, file_fields), (0x48, file_fields), (0x50, file_fields)]):
            va = b.u64(meta + off)
            label = ["Offsets", "Sizes", "Types", "Flags", "FileSizes", "FileTypes", "FileFlags"][k]
            if va == 0 or count <= 0:
                arrays.append("nullptr")
                continue
            values = int_array(va, count)
            body.append(f"const int32_t s_{n}{label}[] = {{ {', '.join(str(v) for v in values)} }};")
            arrays.append(f"s_{n}{label}")
        int_keys = i32(meta + 0x6C)
        str_keys = i32(meta + 0x70)
        int_keys_ref = "nullptr"
        if int_keys > 0 and b.u64(meta + 0x78):
            va = b.u64(meta + 0x78)
            rows = []
            for k in range(int_keys):
                e = va + 16 * k
                rows.append(f"{{ {i32(e)}, {u8(e + 4)}, {{}}, {i32(e + 8)}, 0 }}")
            body.append(f"const DBMETAINTKEY s_{n}IntKeys[] = {{ {', '.join(rows)} }};")
            int_keys_ref = f"s_{n}IntKeys"
        str_keys_ref = "nullptr"
        if str_keys > 0 and b.u64(meta + 0x80):
            va = b.u64(meta + 0x80)
            rows = []
            for k in range(str_keys):
                e = va + 16 * k
                ignore = b.u64(e + 8)
                text = c_string(b.cstr(ignore)) if ignore else "nullptr"
                rows.append(f"{{ {b.u32(e)}u, {u8(e + 4)}, {{}}, {text} }}")
            body.append(f"const DBMETASTRINGKEY s_{n}StringKeys[] = {{ {', '.join(rows)} }};")
            str_keys_ref = f"s_{n}StringKeys"

        order_refs = []
        for off, deref in ((0x98, False), (0xA0, True)):
            fn = b.u64(meta + off)
            if fn == 0:
                order_refs.append("nullptr")
                continue
            fname = f"{n}{'OrderPtr' if deref else 'Order'}"
            order_refs.append(fname)
            sig = (f"bool {fname}(const void* const* left, const void* const* right)" if deref
                   else f"bool {fname}(const void* left, const void* right)")
            order_decl.append(sig + ";")
            dec = decode_order(fn, deref)
            orders.append(f"// ref: FUN_{fn:x}")
            if dec is None or dec["db"] not in names:
                unported += 1
                orders.append(sig + " {")
                orders.append("    (void)left;")
                orders.append("    (void)right;")
                orders.append("    FROST_UNIMPLEMENTED();  // a multi-field record order")
                orders.append("    return false;")
                orders.append("}")
                continue
            acc, ctype = ACCESSORS[dec["accessor"]]
            op, signed = CONDS[dec["cond"]]
            cast = "int32_t" if signed else "uint32_t"
            src_l = "*left" if deref else "left"
            src_r = "*right" if deref else "right"
            args = f'{dec["field"]}, {dec["offset"]}, &{names[dec["db"]]}'
            orders.append(sig + " {")
            orders.append(f"    auto a = static_cast<const uint8_t*>({src_l}) + {dec['rec']};")
            orders.append(f"    auto b = static_cast<const uint8_t*>({src_r}) + {dec['rec']};")
            orders.append(f"    return static_cast<{cast}>({acc}(a, {args})) {op} static_cast<{cast}>({acc}(b, {args}));")
            orders.append("}")

        body.append(
            f"const DBMeta s_{n}Meta = {{ {c_string(name)}, {fdid}, {file_fields}, {record_size}, {fields}, "
            f"{index_field}, {sparse}, {{}}, {', '.join(arrays)}, {i32(meta + 0x58)}, {b.u32(meta + 0x5C)}u, "
            f"{i32(meta + 0x60)}, {b.u32(meta + 0x64)}u, {i32(meta + 0x68)}, {int_keys}, {str_keys}, 0, "
            f"{int_keys_ref}, {str_keys_ref}, {u8(meta + 0x88)}, {{}}, {i32(meta + 0x8C)}, {i32(meta + 0x90)}, 0, "
            f"{order_refs[0]}, {order_refs[1]}, {u8(meta + 0xA8)}, {u8(meta + 0xA9)}, {{}} }};")
        body.append("")
        instances.append(f"// 0x{db:x}")
        if db in SPARSE:
            record_name, record_size = SPARSE[db]
            instances.append(f"WowClientSparseDB2Table {names[db]}(&s_{n}Meta, {c_string(record_name)}, 0x{record_size:x});")
            decl.append(f"extern WowClientSparseDB2Table {names[db]};  // 0x{db:x} {name}")
        else:
            instances.append(f"WowClientDB2 {names[db]}(&s_{n}Meta);")
            decl.append(f"extern WowClientDB2 {names[db]};  // 0x{db:x} {name}")

    groups = []
    group_names = []
    for g in range(GROUP_COUNT):
        lst = b.u64(GROUPS + 16 * g)
        members = []
        k = 0
        while True:
            db = b.u64(lst + 8 * k)
            if db == 0:
                break
            members.append(f"&{names[db]}" if db in names else f"nullptr /* 0x{db:x} not a WowClientDB2 */")
            k += 1
        members.append("nullptr")
        groups.append(f"// 0x{lst:x}")
        groups.append(f"WowClientDB2_Base* s_group{g}[] = {{")
        for mbr in members:
            groups.append(f"    {mbr},")
        groups.append("};")
        group_names.append(f"s_group{g}")

    cpp = ["// Generated by tools/gen_db2.py from the clean exe. Do not edit.",
           "",
           '#include "db/DB2Tables.hpp"',
           "",
           '#include "util/Unimplemented.hpp"',
           "",
           "namespace {", ""]
    cpp += order_decl
    cpp += ["", "}", "", "namespace {", ""]
    cpp += body
    cpp += ["}", ""]
    cpp += instances
    cpp += ["", "namespace {", ""]
    cpp += orders
    cpp += ["", "}", ""]
    cpp += groups
    cpp += ["", "// DBClient's load groups (0x142ccdc70): each a table list and a spare word."]
    cpp += ["DB2LOADGROUP g_db2LoadGroups[] = {"]
    for gname in group_names:
        cpp.append(f"    {{ {gname}, nullptr }},")
    cpp += ["};", ""]

    hpp = ["// Generated by tools/gen_db2.py from the clean exe. Do not edit.",
           "",
           "#ifndef DB_DB2_TABLES_HPP",
           "#define DB_DB2_TABLES_HPP",
           "",
           '#include "db/WowClientDB2.hpp"',
           '#include "db/WowClientSparseDB2.hpp"',
           "",
           "struct DB2LOADGROUP {",
           "    WowClientDB2_Base** m_tables;",
           "    void* m_spare;",
           "};",
           "",
           f"// DBClient's {GROUP_COUNT} load groups.",
           f"extern DB2LOADGROUP g_db2LoadGroups[{GROUP_COUNT}];",
           ""]
    hpp += decl
    hpp += ["", "#endif", ""]

    text = "\n".join(cpp)
    (ROOT / "src/db/DB2Tables.cpp").write_text(text, newline="\n")
    (ROOT / "src/db/DB2Tables.hpp").write_text("\n".join(hpp), newline="\n")
    print(f"{len(tables)} tables, {len(order_decl)} record orders ({unported} not ported)")


if __name__ == "__main__":
    main()
