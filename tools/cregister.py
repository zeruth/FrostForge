"""C_ namespaced Lua APIs: registered in code, one call per function.

The global Lua API sits in {name, func} tables (luabind.py), but C_ namespaces are
registered by generated functions that repeat, once per API function:

    lea  r9,  <handler>          ; lua_CFunction
    lea  r8,  "GetBagName"       ; function name
    lea  rdx, "C_Container"      ; namespace
    lea  rcx, [rbp-0x10]         ; registration context
    call 0x1407d2f20             ; register(ctx, namespace, name, func)

Some namespaces (C_AddOns, C_ActionBar, ...) are instead filled one field at a time
through the Lua API, with Aegis filler (nop'd in the clean exe) between the calls:

    call 0x1407d2e10(ctx, "C_AddOns", LUA_GLOBALSINDEX)   ; get/create the namespace table
    call lua_gettop(L)
    call lua_pushcclosure(L, <handler>, 0)              ; rdx = code pointer
    call lua_setfield(L, top, "GetAddOnMetadata")       ; r8  = field name
    call lua_settop(L, -2)

This walks every function that loads a `C_*` string, tracks the last lea into rdx, r8
and r9, and records namespace.name -> handler for both forms: at a call where they hold
(namespace, name, code pointer) at once, and at a call whose r8 is a name after a call
that pushed a code pointer, inside a namespace opened by an earlier call. Call targets
of both forms are reported so a third routine shows up instead of being silently missed.

    python tools/cregister.py   -> build/facts/cregister.json
"""
from __future__ import annotations

import re
from collections import Counter, defaultdict

import capstone
from capstone import x86

from wowbin import Binary, hexa, write_facts

NS = re.compile(r"C_[A-Za-z0-9_]+\Z")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,80}\Z")


def main() -> None:
    b = Binary()
    ns_strings = {}
    for sec in (".rdata", ".data"):
        s, d = b.bytes_of(sec)
        for m in re.finditer(rb"\0(C_[A-Za-z0-9_]+)\0", d):
            ns_strings[s + m.start() + 1] = m.group(1).decode()
    reg_funcs = {b.func_containing(i) for i, t in b.riprel_refs(keep=ns_strings.__contains__)}
    reg_funcs.discard(None)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    pieces_of = defaultdict(list)
    for st, en, r in zip(b.piece_starts, b.piece_ends, b.piece_root):
        if r in reg_funcs:
            pieces_of[r].append((st, en))

    entries = []
    registrars = Counter()
    seq_calls = {"open": Counter(), "push": Counter(), "set": Counter()}
    for root, pieces in pieces_of.items():
        cur_ns = None
        pending_fn = None
        for st, en in sorted(pieces):
            regs: dict[int, int] = {}
            o = b.off(st)
            for ins in md.disasm(bytes(b.image[o : o + (en - st)]), st):
                ops = ins.operands
                if ins.mnemonic == "lea" and len(ops) == 2 and ops[1].type == x86.X86_OP_MEM \
                        and ops[1].mem.base == x86.X86_REG_RIP:
                    regs[ops[0].reg] = ins.address + ins.size + ops[1].mem.disp
                    continue
                if ins.mnemonic == "call":
                    target = ops[0].imm if ops and ops[0].type == x86.X86_OP_IMM else None
                    ns_va = regs.get(x86.X86_REG_RDX)
                    nm_va = regs.get(x86.X86_REG_R8)
                    fn = regs.get(x86.X86_REG_R9)
                    name = b.cstr(nm_va, 96) if nm_va is not None else None
                    name = name if name and IDENT.match(name) else None
                    if ns_va in ns_strings and name and fn is not None and b.is_code(fn):
                        # form 1: register(ctx, namespace, name, func) in one call
                        registrars[target] += 1
                        entries.append((ns_strings[ns_va], name, fn, root, ins.address))
                    elif ns_va in ns_strings:
                        # form 2, step 1: open the namespace table
                        cur_ns = ns_strings[ns_va]
                        pending_fn = None
                        seq_calls["open"][target] += 1
                    elif ns_va is not None and b.is_code(ns_va):
                        # form 2, step 2: push a C function
                        pending_fn = ns_va
                        seq_calls["push"][target] += 1
                    elif name and pending_fn is not None and cur_ns is not None:
                        # form 2, step 3: set it as a field of the open namespace
                        entries.append((cur_ns, name, pending_fn, root, ins.address))
                        seq_calls["set"][target] += 1
                        pending_fn = None
                    # a call clobbers the volatile argument registers
                    for r in (x86.X86_REG_RCX, x86.X86_REG_RDX, x86.X86_REG_R8, x86.X86_REG_R9):
                        regs.pop(r, None)
                    continue
                # any other write to a tracked register forgets it
                if ops and ops[0].type == x86.X86_OP_REG and ins.mnemonic not in ("cmp", "test", "push"):
                    regs.pop(ops[0].reg, None)

    functions = {}
    namespaces = defaultdict(list)
    for ns, name, fn, root, site in entries:
        functions.setdefault(hexa(fn), f"{ns}.{name}")
        namespaces[ns].append([name, hexa(fn)])
    out = write_facts("cregister", {
        "registrars": {hexa(t) if t else "indirect": n for t, n in registrars.items()},
        "sequence_calls": {k: {hexa(t) if t else "indirect": n for t, n in c.items()} for k, c in seq_calls.items()},
        "registration_functions": sorted(hexa(f) for f in pieces_of),
        "namespaces": {k: v for k, v in sorted(namespaces.items())},
        "functions": functions,
    })
    print(f"{len(entries)} registrations, {len(functions)} distinct handlers, {len(namespaces)} namespaces "
          f"(of {len(set(ns_strings.values()))} C_ strings), from {len(pieces_of)} functions -> {out}")
    print("form 1 registrar call targets:", {hexa(t) if t else "indirect": n for t, n in registrars.most_common(5)})
    for step, c in seq_calls.items():
        print(f"form 2 '{step}' call targets:", {hexa(t) if t else "indirect": n for t, n in c.most_common(4)})
    missing = sorted(set(ns_strings.values()) - set(namespaces))
    print(f"C_ strings with no registration found ({len(missing)}): {missing[:25]}")
    for ns in ("C_Container", "C_Map", "C_Spell"):
        if ns in namespaces:
            print(f"  {ns}: {len(namespaces[ns])} functions, e.g. {namespaces[ns][:3]}")


if __name__ == "__main__":
    main()
