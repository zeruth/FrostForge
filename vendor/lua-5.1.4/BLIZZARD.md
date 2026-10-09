# Blizzard's Lua 5.1.4, as linked into WowClassic 3.4.3.54261

The base is the pristine lua.org 5.1.4 release (commit b99addf). Every change on top of it is
ported from the reference binary and tagged `// ref: FUN_<addr>` on the function it belongs to.
This file records the layouts and the taint machinery those changes rest on.

## Layouts (x64)

Every value and every collectable object carries a *taint*: a pointer naming who produced it
(null is secure).

| struct | size | layout |
|---|---|---|
| TValue | 0x18 | value 0x00, tt 0x08, taint 0x10 |
| GC header | 0x12 | next 0x00, taint 0x08, tt 0x10, marked 0x11 |
| lua_State | 0xe0 | header, 0x18 taint log (per thread group), 0x20 status, 0x28 top, 0x30 base, 0x38 l_G, 0x40 ci, 0x48 savedpc, 0x50 stack_last, 0x58 stack, 0x60 end_ci, 0x68 base_ci, 0x70 stacksize, 0x74 size_ci, 0x78 nCcalls, 0x7a baseCcalls, 0x7c hookmask, 0x7d allowhook, 0x7e (new, byte), 0x80 basehookcount, 0x84 hookcount, 0x88 hook, 0x90 l_gt, 0xa8 env, 0xc0 openupval, 0xc8 gclist, 0xd0 errorJmp, 0xd8 errfunc |
| global_State | 0x1d8 | strt 0x00, frealloc 0x10, ud 0x18, profiling 0x20 (new), currentwhite 0x21, gcstate 0x22, sweepstrgc 0x24, rootgc 0x28, sweepgc 0x30, gray 0x38, grayagain 0x40, weak 0x48, tmudata 0x50, buff 0x58, GCthreshold 0x70, totalbytes 0x78, estimate 0x80, gcdept 0x88, gcpause 0x90, gcstepmul 0x94, panic 0x98, (new) 0xa0, (new) 0xa8, l_registry 0xb0, mainthread 0xc8, uvhead 0xd0, mt 0x108, tmname 0x150 |
| LG | 0x2b8 | lua_State, then global_State at 0xe0 |
| UpVal | 0x38 | header, v 0x18, u 0x20 (value, or l.prev 0x20 / l.next 0x28) |
| CClosure | 0x38 + n*0x18 | header, isC 0x12, nupvalues 0x13, gclist 0x18, env 0x20, profile 0x28 (new), f 0x30, upvalue 0x38 |
| LClosure | 0x38 + n*8 | header, isC 0x12, nupvalues 0x13, gclist 0x18, env 0x20, profile 0x28 (new), p 0x30, upvals 0x38 |
| Proto | 0x80 | |
| Table | 0x48 | header, flags 0x12, lsizenode 0x13, metatable 0x18, array 0x20, node 0x28, lastfree 0x30, gclist 0x38, sizearray 0x40 |

`profile` is a 0x20-byte zeroed block allocated per closure when the state was created with
script profiling on (`lua_newstate`'s third argument, global_State +0x20).

## Taint globals (0x142e207c0..)

| address | meaning |
|---|---|
| 0x142e207c0 | the current taint: what new values and objects are stamped with |
| 0x142e207c8 | taint tracking on |
| 0x142e207cc | taint propagation suspended |
| 0x142e207d0 | the taint a Lua closure without one gets |
| 0x142e207d8 | the taint new objects get while running tainted, when set |
| 0x142e207e8 | the first taint seen, latched |
| 0x142e207f0 | capture where the first taint came from (FUN_1402b2360) |

Reading a tainted value while tracking is on and not suspended records the taint in the thread's
log (FUN_1402bd260), makes it current, and latches the first one.
