"""Generates the glue's script-API registration layer from the clean exe.

The reference registers its Lua API with generated code, in three shapes:

  Enum values   open(ctx, "Enum", GLOBALS); open(ctx, "<Enum>", top); lua_pushnumber(v);
                lua_setfield(-2, "<Field>"); lua_settop(-3)        (0x1407d2e10, 0x1402b3b50,
                0x1402b43c0), and Enum.<Enum>Meta fields through 0x1407d2f10(ctx, meta, field, int)
  functions     register(ctx, "<C_Namespace>", "<Name>", handler)   (0x1407d2f20), or
                open(ctx, ns, GLOBALS); lua_pushcclosure(handler); lua_setfield(top, "<Name>")
  tables        FrameScript_RegisterNamespace(table, count, ns)   (0x14063d2b0) and
                FrameScript_RegisterFunction(name, handler) over a table (0x14063daa0)

plus LE_ constants through FrameScript_SetGlobalNumber(name, int) (0x14063e1b0). This reads
those calls back out of the instructions and writes:

  src/glue/ScriptAPIRegistry.cpp   the registration functions, one per reference function
  src/glue/ScriptAPIHandlers.hpp   every handler, declared
  src/glue/ScriptAPIStubs.cpp      a FROST_UNIMPLEMENTED stub for each handler not ported yet
                                   (a handler is ported when a `// ref: FUN_<addr>` tag for it
                                   exists anywhere else under src/)

    python tools/gen_script_api.py --scan 0x140d95f90        # what one function registers
    python tools/gen_script_api.py --emit                    # regenerate the three files
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
from pathlib import Path

import capstone

from wowbin import Binary

ROOT = Path(__file__).resolve().parent.parent

OPEN = 0x1407D2E10
REGISTER = 0x1407D2F20
ENUM_META = 0x1407D2F10
# Enum.<Enum>.<Field> = value for enums whose values fit a byte or a short (0x140bb3ea0,
# 0x140bb3f50): the same as the meta setter, the value in r9b / r9w.
ENUM_VALUE8 = 0x140BB3EA0
ENUM_VALUE16 = 0x140BB3F50
# Enum.<Enum>.<Field> = value through a setter, by the setter: its port and how r9 is read
ENUM_SETTERS = {
    0x1407CB7E0: ("ScriptSetEnumValue", "i32"),     # also reached through the thunk ENUM_META
    ENUM_META: ("ScriptSetEnumValue", "i32"),
    0x1407CB890: ("ScriptSetEnumValueU32", "u32"),
    0x1407CB940: ("ScriptSetEnumValueU64", "u64"),
    ENUM_VALUE8: ("ScriptSetEnumValue8", "u8"),
    ENUM_VALUE16: ("ScriptSetEnumValue16", "u16"),
}
# <table>.<name>[field] = value, the field in the fifth (stack) argument
TABLE_SETTER = 0x140C09480
PUSHNUMBER = 0x1402B3B50
PUSHCCLOSURE = 0x1402B3880
PUSHUNSIGNED = 0x14065F770
CALLEE_SAVED = {"rbx", "rbp", "rdi", "rsi", "r12", "r13", "r14", "r15",
                "ebx", "ebp", "edi", "esi", "r12d", "r13d", "r14d", "r15d"}
SETFIELD = 0x1402B43C0
SET_GLOBAL_NUMBER = 0x14063E1B0
REGISTER_NAMESPACE = 0x14063D2B0
REGISTER_FUNCTION = 0x14063DAA0
REGISTER_GLOBAL = 0x1407D2F80
QUIET = {0x1402B30B0, 0x1402B45D0, 0x1402B2770, 0x142424D80}
GLOBALSINDEX = 0xFFFFD8EE

# The glue's script API (FUN_140afd040) in order: (reference function, C++ name).
GLUE_SCRIPT_API = [
    (0x1407D53F0, "ScriptAPI_C_AccountServices"),
    (0x1407D60E0, "ScriptAPI_C_AddOns"),
    (0x140BCA380, "ScriptRegister_140bca380"),
    (0x140C846D0, "ScriptAPI_C_CameraDefaults"),
    (0x140C84910, "ScriptEnum_CharacterCreateRaceMode"),
    (0x140C869E0, "ScriptGlobals_CanCreateCharacter"),
    (0x140C86C60, "ScriptEnum_CharacterServiceInfoFlag"),
    (0x140C88AC0, "ScriptAPI_C_ChatInfo"),
    (0x140C891A0, "ScriptRegister_140c891a0"),
    (0x140D959D0, "ScriptEnum_ConfigurationWarning"),
    (0x140D95F90, "ScriptEnum_ConsoleCategory"),
    (0x140D95EF0, "ScriptAPI_C_ConsoleScriptCollection"),
    (0x140F8F150, "ScriptEnum_UICursorType"),
    (0x140F922A0, "ScriptEnum_ManipulatorEventType"),
    (0x14107EA90, "ScriptAPI_C_EventUtils"),
    (0x14107EB00, "ScriptGlobals_ProcessExceptionClient"),
    (0x14107ECD0, "ScriptGlobals_CanUpgradeExpansion"),
    (0x140F2CB90, "ScriptAPI_C_GamePad"),
    (0x140F2CEA0, "ScriptAPI_C_GameRules"),
    (0x140C132D0, "ScriptAPI_C_InterfaceFileManifest"),
    (0x140C19F30, "ScriptEnum_BindingSet"),
    (0x140C1A2E0, "ScriptAPI_Kiosk"),
    (0x140C1A3B0, "ScriptRegister_140c1a3b0"),
    (0x141117760, "ScriptRegister_141117760"),
    (0x141117780, "ScriptAPI_C_LoginUI"),
    (0x1410A7E00, "ScriptAPI_C_PaidServices"),
    (0x140EC54A0, "ScriptEnum_ClientPlatformType"),
    (0x140B012B0, "ScriptAPI_C_ReportSystem"),
    (0x140AE5D90, "ScriptAPI_C_SocialContractGlue"),
    (0x140AE6420, "ScriptAPI_C_Sound"),
    (0x140E6B6E0, "ScriptEnum_VasQueueStatus"),
    (0x140E6BEE0, "ScriptAPI_C_StorePublic"),
    (0x140E6E450, "ScriptEnum_BattlepayDisplayFlag"),
    (0x140E6F870, "ScriptGlobals_StripHyperlinks"),
    (0x140E701D0, "ScriptEnum_TitleIconVersion"),
    (0x140D276B0, "ScriptAPI_C_UIColor"),
    (0x140D277D0, "ScriptGlobals_DoesAnyDisplayHaveNotch"),
    (0x140D278A0, "ScriptEnum_ModelSceneSetting"),
    (0x140D286F0, "ScriptAPI_C_Timer"),
    (0x140B5F920, "ScriptAPI_C_UserFeedback"),
    (0x140B60950, "ScriptEnum_GraphicsValidationResult"),
    (0x140B623C0, "ScriptEnum_CommunicationMode"),
    (0x14115C010, "ScriptRegister_14115c010"),
    (0x14115C0C0, "ScriptAPI_C_XMLUtil"),
    (0x140C80980, "ScriptAPI_C_CVar"),
    (0x140E6F9B0, "ScriptAPI_C_System"),
    (0x1407D3000, "ScriptEnum_AccountData"),
    (0x140C84440, "ScriptEnum_CameraModeAspectRatio"),
    (0x140C84710, "ScriptEnum_ChrRacesAllianceType"),
    (0x140C85A00, "ScriptEnum_CharCustomizationType"),
    (0x140C86FE0, "ScriptEnum_PermanentChatChannelType"),
    (0x14107EEE0, "ScriptEnum_ModelLightType"),
    (0x1410A73A0, "ScriptEnum_ModelBlendOperation"),
    (0x140EC7FC0, "ScriptEnum_CraftingOrderDuration"),
    (0x140AFFD60, "ScriptEnum_ReportType"),
    (0x140E6BF80, "ScriptEnum_BattlepayBannerType"),
    (0x140D0AD60, "ScriptEnum_NodeOpFailureReason"),
    (0x140B5DFB0, "ScriptEnum_UnitSex"),
    (0x140B605C0, "ScriptEnum_ValueAddedServiceType"),
    (0x141088C70, "ScriptEnum_SeasonID"),
]

# The glue manager's global tables (CGlueMgr init, 0x14167bac0): name, reference function, and
# its tables as (address, count, namespace or None for globals).
GLUE_TABLES = [
    (0x141DB0F60, "Sub141DB0F60", [(0x142D38C30, 8, None)]),
    (0x1424DB660, "Sub1424DB660", [(0x142D5E590, 0xD, None)]),
    (0x141D06170, "Sub141D06170", [(0x142D387E0, 0x45, None)]),
    (0x1416A70B0, "Sub1416A70B0", [(0x142D1C1B0, 0x3A, None)]),
    (0x14168ED20, "Sub14168ED20", [(0x142D1B4C0, 0x1A, None)]),
    (0x14169C190, "Sub14169C190", [(0x142D1B6A0, 0x10, "C_CharacterServices")]),
    (0x14169C220, "Sub14169C220", [(0x142D1B7A0, 10, "C_SharedCharacterServices"),
                                   (0x142D1B840, 1, "C_CharacterServicesPublic")]),
    (0x14130C2E0, "Sub14130C2E0", [(0x142CFD660, 0x11, None)]),
    (0x1419F63A0, "Sub1419F63A0", [(0x142D2D460, 0x22, None)]),
    (0x141B47390, "Sub141B47390", [(0x142D31BD0, 5, "C_AuthChallenge")]),
    (0x141B65C50, "Sub141B65C50", [(0x142D32000, 0x10, "C_WowTokenSecure"),
                                   (0x142D32100, 0xC, "C_WowTokenPublic"),
                                   (0x142D321C0, 4, "C_WowTokenGlue")]),
    (0x1416A0D20, "Sub1416A0D20", [(0x142D1B860, 0x1D, "C_Login")]),
    (0x1416A8880, "Sub1416A8880", [(0x1427E9760, 8, "C_RealmList")]),
    (0x141DB1990, "Sub141DB1990", [(0x14284BBE8, 3, "C_Widget")]),
]

# LE_ constants and the like set as global numbers (scanned).
GLUE_GLOBAL_NUMBERS = [
    (0x14191C2A0, "Sub14191C2A0"),
]


def function_extent(b: Binary, start: int) -> int:
    i = bisect.bisect_right(b.piece_starts, start) - 1
    if i >= 0 and start < b.piece_ends[i]:
        return b.piece_ends[i] - start
    return 0x4000


EVENT_TABLE = 0x141273D80          # the event signaller table (FUN_141273d80)
EVENT_PROFILE_ENTER = 0x14063EA00  # the dispatchers' profiling scope
EVENT_PROFILE_LEAVE = 0x14063EBD0
EVENT_CALL = 0x140647940           # a signaller's call of OnEvent: (L, nargs, base)
_setter_slots: dict[int, int | None] = {}
_corpus: dict[int, str] | None = None


def event_setter_slot(b: Binary, setter: int) -> int | None:
    """The table slot a setter stores its argument into: `call FUN_141273d80; mov [rax+disp], reg`."""
    if setter in _setter_slots:
        return _setter_slots[setter]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    slot = None
    seen_table = False
    for insn in md.disasm(b.image[b.off(setter):b.off(setter) + 0x40], setter):
        ops = insn.operands
        if insn.mnemonic == "call" and ops and ops[0].type == capstone.x86.X86_OP_IMM:
            seen_table = ops[0].imm == EVENT_TABLE
            if not seen_table:
                break
        elif seen_table and insn.mnemonic == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM \
                and ops[0].mem.base == capstone.x86.X86_REG_RAX and ops[1].type == capstone.x86.X86_OP_REG:
            slot = ops[0].mem.disp
            break
        elif insn.mnemonic == "ret":
            break
    _setter_slots[setter] = slot
    return slot


def corpus_code(addr: int) -> str:
    global _corpus
    if _corpus is None:
        _corpus = {}
        with (ROOT / "build" / "corpus.jsonl").open(encoding="utf-8", errors="replace") as f:
            for line in f:
                rec = json.loads(line)
                _corpus[int(rec["addr"], 16)] = rec.get("c", "")
    return _corpus.get(addr, "")


def event_signal_info(b: Binary, dispatcher: int) -> dict:
    """A dispatcher's inner signaller, the event's name, and how many arguments it pushes."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    inner = None
    for insn in md.disasm(b.image[b.off(dispatcher):b.off(dispatcher) + 0x100], dispatcher):
        ops = insn.operands
        if insn.mnemonic == "call" and ops and ops[0].type == capstone.x86.X86_OP_IMM:
            if ops[0].imm not in (EVENT_PROFILE_ENTER, EVENT_PROFILE_LEAVE):
                inner = ops[0].imm
                break
        if insn.mnemonic == "ret":
            break
    code = corpus_code(inner) if inner else ""
    name = re.search(r'= "([A-Za-z0-9_]+)";', code)
    nargs = re.search(r"FUN_140647940\(\w+,(0x[0-9a-fA-F]+|\d+),", code)
    # the event's id, as the fire function hands it to the handler's profiling scope
    hashed = re.search(r"FUN_14063e980\(\w+,(-?0x[0-9a-fA-F]+|-?\d+),", code)
    event_id = int(hashed.group(1), 0) & 0xFFFFFFFFFFFFFFFF if hashed else None
    return {
        "inner": inner,
        "name": name.group(1) if name else None,
        "nargs": int(nargs.group(1), 0) if nargs else None,
        "id": event_id,
    }


def scan(b: Binary, start: int) -> dict:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    code = b.image[b.off(start):b.off(start) + function_extent(b, start)]
    regs: dict[str, tuple] = {}
    xmm: dict[str, float | None] = {}
    xmm1 = None
    pending_value = None
    pending_handler = None
    namespace = None
    enum = None
    steps = []
    other = []
    for insn in md.disasm(code, start):
        m = insn.mnemonic
        ops = insn.operands
        if len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG:
            reg = insn.reg_name(ops[0].reg)
            if m == "lea" and ops[1].type == capstone.x86.X86_OP_MEM and ops[1].mem.base == capstone.x86.X86_REG_RIP:
                target = insn.address + insn.size + ops[1].mem.disp
                s = b.cstr(target, 200) if b.section_of(target) and not b.is_code(target) else None
                regs[reg] = ("str", s) if s is not None else ("addr", target)
            elif m in ("mov", "movabs") and ops[1].type == capstone.x86.X86_OP_IMM:
                wide = reg.startswith("r") and not reg.endswith(("d", "w", "b"))
                regs[reg] = ("imm", ops[1].imm & (0xFFFFFFFFFFFFFFFF if wide else 0xFFFFFFFF))
                if reg in ("edx", "r8d", "r9d", "ecx", "r9b", "r9w"):
                    regs[{"edx": "rdx", "r8d": "r8", "r9d": "r9", "ecx": "rcx", "r9b": "r9", "r9w": "r9"}[reg]] = ("imm", ops[1].imm & 0xFFFFFFFF)
            elif m == "xor" and ops[1].type == capstone.x86.X86_OP_REG and ops[0].reg == ops[1].reg:
                regs[reg] = ("imm", 0)
                if reg in ("edx", "r8d", "r9d", "ecx", "r9b", "r9w"):
                    regs[{"edx": "rdx", "r8d": "r8", "r9d": "r9", "ecx": "rcx", "r9b": "r9", "r9w": "r9"}[reg]] = ("imm", 0)
            elif m == "mov" and ops[1].type == capstone.x86.X86_OP_REG:
                source = regs.get(insn.reg_name(ops[1].reg))
                if source is not None:
                    regs[reg] = source
                else:
                    regs.pop(reg, None)
        if m == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM \
                and ops[0].mem.base == capstone.x86.X86_REG_RSP and ops[0].mem.disp == 0x20:
            if ops[1].type == capstone.x86.X86_OP_REG:
                regs["stack20"] = regs.get(insn.reg_name(ops[1].reg))
            elif ops[1].type == capstone.x86.X86_OP_IMM:
                regs["stack20"] = ("imm", ops[1].imm & 0xFFFFFFFF)
        if m == "xorps" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG and ops[0].reg == ops[1].reg:
            xmm[insn.reg_name(ops[0].reg)] = 0.0
        elif m in ("movsd", "movss") and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG and ops[1].type == capstone.x86.X86_OP_MEM and ops[1].mem.base == capstone.x86.X86_REG_RIP:
            target = insn.address + insn.size + ops[1].mem.disp
            fmt, size = ("<d", 8) if m == "movsd" else ("<f", 4)
            xmm[insn.reg_name(ops[0].reg)] = struct.unpack(fmt, b.image[b.off(target):b.off(target) + size])[0]
        elif m in ("movaps", "movapd") and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG and ops[1].type == capstone.x86.X86_OP_REG:
            xmm[insn.reg_name(ops[0].reg)] = xmm.get(insn.reg_name(ops[1].reg))
        elif m == "call" and ops and ops[0].type == capstone.x86.X86_OP_IMM:
            target = ops[0].imm

            def s(reg):
                v = regs.get(reg)
                return v[1] if v and v[0] == "str" else None

            def a(reg):
                v = regs.get(reg)
                return v[1] if v and v[0] == "addr" else None

            def i(reg):
                v = regs.get(reg)
                return v[1] if v and v[0] == "imm" else None

            if target == OPEN:
                name = s("rdx")
                if name == "Enum":
                    enum = "Enum"
                elif enum == "Enum" and i("r8") != GLOBALSINDEX:
                    enum = name
                else:
                    enum = None
                    namespace = name
            elif target == PUSHNUMBER:
                pending_value = ("number", xmm.get("xmm1"))
            elif target == PUSHUNSIGNED:
                pending_value = ("u64", i("rdx") or 0)
            elif target == PUSHCCLOSURE:
                pending_handler = a("rdx")
            elif target == SETFIELD:
                field = s("r8")
                if enum not in (None, "Enum"):
                    steps.append(("enum", enum, field, pending_value))
                elif pending_handler is not None:
                    steps.append(("function", namespace, field, pending_handler))
                pending_value = None
                pending_handler = None
            elif target in ENUM_SETTERS:
                setter, width = ENUM_SETTERS[target]
                value = i("r9") or 0
                value = {"i32": value - (1 << 32) if value & 0x80000000 and value <= 0xFFFFFFFF else value,
                         "u32": value & 0xFFFFFFFF, "u64": value, "u8": value & 0xFF,
                         "u16": value & 0xFFFF}[width]
                steps.append(("set", s("rdx"), s("r8"), (setter, width, value)))
            elif target == TABLE_SETTER:
                field = regs.get("stack20")
                field = field[1] if field and field[0] == "str" else None
                value = i("r9") or 0
                if value & 0x80000000 and value <= 0xFFFFFFFF:
                    value -= 1 << 32
                steps.append(("settable", s("rdx"), s("r8"), (value, field)))
            elif target == REGISTER:
                steps.append(("function", s("rdx"), s("r8"), a("r9")))
            elif target == REGISTER_GLOBAL:
                steps.append(("function", None, s("rdx"), a("r8")))
            elif target == SET_GLOBAL_NUMBER:
                value = i("rdx")
                if value is not None and value & 0x80000000:
                    value -= 1 << 32
                steps.append(("number", None, s("rcx"), value))
            elif target in QUIET:
                pass
            elif a("rcx") is not None and event_setter_slot(b, target) is not None:
                steps.append(("event", target, event_setter_slot(b, target), a("rcx")))
            else:
                other.append(hex(target))
            # a call keeps only the callee-saved registers
            regs = {k: v for k, v in regs.items() if k in CALLEE_SAVED}
            if target not in QUIET:
                regs.pop("stack20", None)
    return {"steps": steps, "other": sorted(set(other))}


def read_table(b: Binary, table: int, count: int) -> list[tuple[str, int]]:
    out = []
    for k in range(count):
        name = b.cstr(b.u64(table + k * 16))
        fn = b.u64(table + k * 16 + 8)
        out.append((name, fn))
    return out


def ident(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", text)


def handler_name(namespace: str | None, name: str) -> str:
    return "Lua_" + (ident(namespace) + "_" if namespace else "") + ident(name)


def ported_handlers() -> set[int]:
    """Handlers with a ref tag outside the generated files."""
    generated = {"ScriptAPIRegistry.cpp", "ScriptAPIStubs.cpp", "ScriptAPIHandlers.hpp"}
    tags = set()
    for path in (ROOT / "src").rglob("*.cpp"):
        if path.name in generated:
            continue
        # only a Lua handler counts: the same address can be a different function folded with it
        # (identical code folding), which does not make the handler exist
        # (or translated from the reference by tools/translate.py: src/gen/handlers.cpp)
        for m in re.finditer(r"// (?:ref|translated): FUN_([0-9a-fA-F]+)\n(?:static )?int Lua_",
                             path.read_text(errors="replace")):
            tags.add(int(m.group(1), 16))
    return tags


def cpp_double(v) -> str:
    if v is None:
        return "0.0"
    return repr(float(v))


def cpp_number(v) -> str:
    if v is None:
        return "0"
    if float(v).is_integer():
        return str(int(v))
    return repr(float(v))


def emit() -> None:
    b = Binary()
    handlers: dict[int, str] = {}          # address -> C++ name
    names_used: dict[str, int] = {}

    def handler(namespace, name, addr) -> str:
        if addr in handlers:
            return handlers[addr]
        cname = handler_name(namespace, name)
        if cname in names_used and names_used[cname] != addr:
            cname = f"{cname}_{addr:x}"
        names_used[cname] = addr
        handlers[addr] = cname
        return cname

    registry = []
    signals: dict[str, tuple[int, int, str]] = {}   # C++ name -> (dispatcher, signaller, event)
    setters: dict[int, int] = {}                     # slot setter -> its slot
    for addr, name in GLUE_SCRIPT_API:
        info = scan(b, addr)
        body = []
        order = []
        for kind, space, field, value in info["steps"]:
            if kind == "function":
                order.append(("function", (space, field, handler(space, field, value))))
            elif kind == "number":
                order.append(("number", (field, value)))
            elif kind == "event":
                order.append(("event", (space, field, value)))
            else:
                order.append((kind, (space, field, value)))
        for kind, item in order:
            if kind == "enum":
                # what the reference inlines for a value it writes itself (FUN_1407cb7e0's body)
                space, field, pushed = item
                how, value = pushed if pushed else ("number", None)
                body.append("    ScriptOpenTable(L, \"Enum\", LUA_GLOBALSINDEX);")
                body.append(f"    ScriptOpenTable(L, \"{space}\", lua_gettop(L));")
                if how == "u64":
                    body.append(f"    ScriptPushEnumUInt64(L, 0x{value:x}ull);")
                else:
                    body.append(f"    ScriptPushEnumNumber(L, {cpp_double(value)});")
                body.append(f"    lua_setfield(L, -2, \"{field}\");")
                body.append("    lua_settop(L, -3);")
            elif kind == "set":
                space, field, (setter, width, value) = item
                literal = f"0x{value:x}ull" if width == "u64" else str(value)
                body.append(f"    {setter}(L, \"{space}\", \"{field}\", {literal});")
            elif kind == "settable":
                table, space, (value, field) = item
                body.append(f"    ScriptSetTableValue(L, \"{table}\", \"{space}\", {value}, \"{field}\");")
            elif kind == "function":
                space, field, cname = item
                if space is None:
                    body.append(f"    FrameScript_RegisterFunction(\"{field}\", {cname});")
                else:
                    body.append(f"    ScriptRegisterFunction(L, \"{space}\", \"{field}\", {cname});")
            elif kind == "number":
                field, value = item
                body.append(f"    FrameScript_SetGlobalNumber(\"{field}\", {value});")
            elif kind == "event":
                setter, slot, dispatcher = item
                sig = event_signal_info(b, dispatcher)
                if sig["name"] is None or sig["nargs"] is None:
                    body.append(f"    // not generated: FUN_{setter:x} installs FUN_{dispatcher:x}, not decoded")
                elif sig["nargs"] != 0:
                    body.append(f"    // not generated: FUN_{setter:x} installs the {sig['name']} signaller "
                                f"(FUN_{dispatcher:x}, {sig['nargs']} arguments)")
                else:
                    cname = f"EventSignal_{sig['name']}"
                    if cname in signals and signals[cname][0] != dispatcher:
                        cname = f"{cname}_{dispatcher:x}"
                    signals[cname] = (dispatcher, sig["inner"], sig["name"], sig["id"])
                    setters[setter] = slot
                    body.append(f"    EventCallbacks_Set{slot:X}({cname});")
        registry.append((addr, name, "lua_State* L", body, info["other"]))

    for addr, name, tables in GLUE_TABLES:
        body = []
        for table, count, space in tables:
            entries = read_table(b, table, count)
            var = f"s_table_{table:x}"
            body.append(f"    static const SCRIPTFUNCTION {var}[] = {{")
            for field, fn in entries:
                body.append(f"        {{ \"{field}\", {handler(space, field, fn)} }},")
            body.append("    };")
            if space is None:
                body.append(f"    for (const SCRIPTFUNCTION& entry : {var}) {{")
                body.append("        FrameScript_RegisterFunction(entry.m_name, entry.m_function);")
                body.append("    }")
            else:
                body.append(f"    FrameScript_RegisterNamespace({var}, {count}, \"{space}\");")
        registry.append((addr, name, "", body, []))

    for addr, name in GLUE_GLOBAL_NUMBERS:
        info = scan(b, addr)
        body = []
        for kind, space, field, value in info["steps"]:
            if kind == "number":
                body.append(f"    FrameScript_SetGlobalNumber(\"{field}\", {value});")
        registry.append((addr, name, "", body, info["other"]))

    header = "// GENERATED by tools/gen_script_api.py from the clean exe; do not edit by hand.\n"

    lines = [header,
             "// The glue's script-API registrations: Enum tables, namespace functions, global tables",
             "// and constants, one function per reference function (see tools/gen_script_api.py).",
             "",
             "#include \"glue/ScriptAPIRegistry.hpp\"",
             "",
             "#include \"framescript/EventCallbacks.hpp\"",
             "#include \"framescript/FrameScript.hpp\"",
             "#include \"framescript/ScriptAPI.hpp\"",
             "#include \"glue/EventSignals.hpp\"",
             "#include \"glue/ScriptAPIHandlers.hpp\"",
             "",
             "extern \"C\" {",
             "#include \"lua.h\"",
             "}",
             ""]
    for addr, name, params, body, other in registry:
        lines.append(f"// ref: FUN_{addr:x}")
        lines.append(f"void {name}({params}) {{")
        if params:
            lines.append("    (void)L;")
        lines.extend(body)
        if other:
            lines.append(f"    // not generated: calls {', '.join(other)} (callback setters and the like)")
        lines.append("}")
        lines.append("")
    (ROOT / "src/glue/ScriptAPIRegistry.cpp").write_text("\n".join(lines), newline="\n")

    reg_header = [header,
                  "#ifndef GLUE_SCRIPT_API_REGISTRY_HPP",
                  "#define GLUE_SCRIPT_API_REGISTRY_HPP",
                  "",
                  "struct lua_State;",
                  ""]
    for addr, name, params, body, other in registry:
        reg_header.append(f"void {name}({params});")
    reg_header += ["", "#endif", ""]
    (ROOT / "src/glue/ScriptAPIRegistry.hpp").write_text("\n".join(reg_header), newline="\n")

    decl = [header,
            "#ifndef GLUE_SCRIPT_API_HANDLERS_HPP",
            "#define GLUE_SCRIPT_API_HANDLERS_HPP",
            "",
            "struct lua_State;",
            ""]
    for addr, cname in sorted(handlers.items(), key=lambda kv: kv[1]):
        decl.append(f"int {cname}(lua_State* L);  // FUN_{addr:x}")
    decl += ["", "#endif", ""]
    (ROOT / "src/glue/ScriptAPIHandlers.hpp").write_text("\n".join(decl), newline="\n")

    sig_header = [header,
                  "#ifndef GLUE_EVENT_SIGNALS_HPP",
                  "#define GLUE_EVENT_SIGNALS_HPP",
                  "",
                  "// The script events' signallers the glue's registrations install (see",
                  "// framescript/EventCallbacks.hpp): each sends its event to the listeners.",
                  "",
                  "#include <cstdint>",
                  "",
                  "#include \"framescript/EventCallbacks.hpp\"",
                  "#include \"framescript/FrameScriptEvents.hpp\"",
                  ""]
    for setter, slot in sorted(setters.items(), key=lambda kv: kv[1]):
        sig_header.append(f"void EventCallbacks_Set{slot:X}(EVENTSIGNAL signal);  // FUN_{setter:x}")
    sig_header.append("")
    sig_source = [header,
                  "#include \"glue/EventSignals.hpp\"",
                  "",
                  "#include \"framescript/FrameScriptEvents.hpp\"",
                  ""]
    for cname, (dispatcher, inner, event, event_id) in sorted(signals.items()):
        fire = "EventFire_" + cname[len("EventSignal_"):]
        sig_header.append(f"void {cname}(uint64_t id, void* args);")
        sig_header.append(f"void {fire}(FrameScriptEvents::EventListenerList* list, void* args);")
        sig_source += [f"// ref: FUN_{inner:x}",
                       f"void {fire}(FrameScriptEvents::EventListenerList* list, void* args) {{",
                       "    (void)args;",
                       f"    FrameScriptEvents::FireList(list, \"{event}\", {len(event)}, 0x{event_id:x}ull,",
                       "        [](lua_State*) { return 0; });",
                       "}",
                       "",
                       f"// ref: FUN_{dispatcher:x}",
                       f"void {cname}(uint64_t id, void* args) {{",
                       f"    FrameScriptEvents::Dispatch(id, args, {fire});",
                       "}",
                       ""]
    for setter, slot in sorted(setters.items(), key=lambda kv: kv[1]):
        sig_source += [f"// ref: FUN_{setter:x}",
                       f"void EventCallbacks_Set{slot:X}(EVENTSIGNAL signal) {{",
                       f"    EventCallbacks_Table()[0x{slot:x} / sizeof(EVENTSIGNAL)] = signal;",
                       "}",
                       ""]
    sig_header += ["", "#endif", ""]
    (ROOT / "src/glue/EventSignals.hpp").write_text("\n".join(sig_header), newline="\n")
    (ROOT / "src/glue/EventSignals.cpp").write_text("\n".join(sig_source), newline="\n")

    ported = ported_handlers()
    # every handler, for tools/translate.py: its roots are the ones no hand port covers
    (ROOT / "build/script-handlers.json").write_text(
        json.dumps({f"{a:x}": n for a, n in sorted(handlers.items())}, indent=0) + "\n", newline="\n")
    stubs = [header,
             "// Handlers not ported yet. Each one is replaced by a port elsewhere (with its ref tag),",
             "// after which regeneration drops the stub.",
             "",
             "#include \"glue/ScriptAPIHandlers.hpp\"",
             "",
             "#include \"util/Unimplemented.hpp\"",
             ""]
    count = 0
    trivial = 0
    for addr, cname in sorted(handlers.items(), key=lambda kv: kv[1]):
        if addr in ported:
            continue
        # `xor eax, eax; ret`: the handler returns no values, which is its whole port
        if b.image[b.off(addr):b.off(addr) + 3] == b"\x33\xc0\xc3":
            trivial += 1
            stubs += [f"// Returns nothing.",
                      f"// ref: FUN_{addr:x}",
                      f"int {cname}(lua_State* L) {{",
                      "    (void)L;",
                      "    return 0;",
                      "}",
                      ""]
            continue
        count += 1
        stubs += [f"// ref: FUN_{addr:x}",
                  f"int {cname}(lua_State* L) {{",
                  "    (void)L;",
                  "    FROST_UNIMPLEMENTED();",
                  "    return 0;",
                  "}",
                  ""]
    (ROOT / "src/glue/ScriptAPIStubs.cpp").write_text("\n".join(stubs), newline="\n")
    print(f"{len(registry)} registration functions, {len(handlers)} handlers, {count} stubs, {trivial} trivial")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", nargs="*", type=lambda s: int(s, 16))
    ap.add_argument("--emit", action="store_true")
    args = ap.parse_args()
    if args.scan:
        b = Binary()
        print(json.dumps({hex(a): scan(b, a) for a in args.scan}, indent=1))
    if args.emit:
        emit()


if __name__ == "__main__":
    main()
