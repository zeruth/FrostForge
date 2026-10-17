# FrostLite tools

Recomp tooling for the WoW Classic 3.4.3.54261 client. **Every tool reads
`client/_classic_/WowClassic-clean.exe`** (decrypted, Aegis junk nop'd), never the shipped
`WowClassic.exe`, which is encrypted on disk.

## The approach: facts in Python, applied in Ghidra

Headless Ghidra takes a minute or more just to open this program, so the extractors work
straight on the PE in Python (seconds per run) and write JSON facts to `build/facts/`.
One Ghidra script, `ghidra/ApplyFacts.java`, applies them. It is re-runnable and never
replaces a name set by hand (`USER_DEFINED`). The JSON is also the input for later recomp
measurement.

```bash
bash tools/pipeline.sh                # the whole pipeline: facts, one Ghidra session, translate
bash tools/pipeline.sh --no-ghidra    # facts and translate against the corpus as it is
python tools/corpus.py --module Unit_C.cpp --out unit_c   # read a module, anti-tamper stripped
python tools/iat_dump.py              # imports (needs a running client, see below; committed)
```

### The pipeline (`tools/pipeline.sh`, 2026-10-16)

Three steps, one Ghidra session:

1. **Facts** (~2 min): every extractor below, the independent ones in parallel, then
   `funcptrs.py` and `protos.py`, which read the others' facts.
2. **Ghidra** (`ghidra/ghidra.sh all`, ~12 min): the program opens once; `ApplyFacts.java`
   applies every fact, then `ExportData.java` and `ExportDecompAll.java` write
   `build/refdata.jsonl` and `build/corpus.jsonl` (each written beside the old one and moved
   over only when complete). The Decompiler Parameter ID pass (`CommitPrototypes.java`) is gone:
   its prototypes were guesses, and `protos.py` reads them off the instructions instead.
3. **Translate** (`tools/translate.py --handlers --depth 24 --fix`, ~5 min): one load and one
   plan; a fix round re-emits only the bodies that did not compile, as stubs, and the build
   recompiles only the files that changed (`src/gen` compiles with `/MP`). Each demotion in
   `src/gen/demoted.json` carries the line the compiler pointed at.

The goal is that the corpus comes out of Ghidra right: a defect is fixed where it arises (a fact,
or the translator's reading of a construct), not patched per function. `tools/translate_patches.json`
holds what is not fixed at its source yet; a patch that no longer matches is reported and skipped.

Back up the Ghidra project before a session that changes much
(`cp -r FrostLite.rep FrostLite.rep.bak-<date>` in `C:\Users\tyler\tools\ghidra-projects`): the
facts are re-runnable, but what they replace is not kept.

| tool | what it recovers | how |
|---|---|---|
| `wowbin.py` | shared core: flat image by RVA, `.pdata` function extents, C strings, RIP-relative reference scan | |
| `pe_survey.py` | sections and entropy, imports, TLS callbacks, debug directory | |
| `name_anchors.py` | counts RTTI names, source paths, Lua names: what naming material exists | |
| `vtables.py` | ~3.5k vtables, their slots, and the functions that load them (ctors/dtors) | lea targets inside runs of code pointers in `.rdata` |
| `srcrefs.py` | per-function source file (`Source/Object/ObjectClient/Unit_C.cpp`, ...) | functions that load `__FILE__` strings |
| `luabind.py` | Lua C function tables: name -> function | 16-byte `{name, func}` runs |
| `iat_dump.py` | the stripped import table: 614 slots -> DLL!API | reads the slots from a live process, evaluates the obfuscated stubs, resolves against exports |
| `typetags.py` | which classes each function creates or destroys (9.8k functions) | the type-name string passed to `SMemAlloc`/`SMemFree` |
| `retcheck.py` | functions carrying the anti-tamper return-address check, and where it starts; the check's jumps over junk (2,885), which ApplyFacts makes unconditional and whose junk it clears, so the 155 functions Ghidra decoded with overlapping instructions decompile | loads of the check's code-range global; a forward jump inside the check whose fall-through, decoded linearly, runs across the jump's own target |
| `protos.py` | every function's parameters and result, and every indirect call's arguments (`protos.json`, `callsites.json`), in place of Ghidra's Decompiler Parameter ID | a must-define pass over rcx/rdx/r8/r9/xmm0-3 by width, the home slots and the stack parameters through rsp and the registers set from it, closed over direct and tail calls; results from the width the direct callers read (`test al, al`: a byte), or what a function only a vtable reaches leaves in rax; an indirect call's arguments are the registers written since the caller's last call, or its own untouched parameters, and the outgoing stack stores below its lowest local. Also the variadic functions (`varargs.json`: the first variadic slot's address taken, the register slots from there through r9 spilled and never read back as values), the `call __chkstk` sites ApplyFacts makes nops, and 16-byte vector arguments and results (placed in xmm by custom storage). `--show` / `--trace` say which instruction makes an input |
| `funcptrs.py` | data slots holding a function's address that code calls through, typed as a pointer to the target's signature | `call [rip+X]` / `mov reg, [rip+X]` on a slot whose value is a function start |
| `switches.py` | MSVC jump tables, written by ApplyFacts as jump-table overrides where Ghidra has no flow out of the jump | `cmp r, N; ja` bound, the image base, an optional byte index table, the 32-bit table, `add; jmp reg`; every target inside the jump's own function |
| `pieces.py` | chained `.pdata` pieces and their parents | `UNW_FLAG_CHAININFO` |
| `cregister.py` | `C_` namespaced Lua APIs: 1,237 handlers in 102 namespaces | the two registration forms in code (below) |
| `noreturn.py` | functions that never return (35) | no `ret` in any piece + tail jumps only to noreturn functions, cross-checked against call sites the compiler dead-ended |
| `corpus.py` | the whole program decompiled to `build/corpus.jsonl`; renders by address, range or source file, anti-tamper check stripped | `ghidra/ExportDecompAll.java`, parallel |
| `oracle.py` | runs reference functions under emulation (Unicorn): the exe mapped at its base, a stack, a TEB, a heap, stubs for the imports | the check for any pure function's port |
| `printf_oracle.py` | `SStrPrintf` port vs the reference's own, case by case (`test/storm/printf_cases.txt`) | 98/98 identical (2026-10-08) |
| `probe_oracle.py` | `SStrHashHT`, `OsPathNormalize`, UTF-16→UTF-8, `SStrTokenize` ports vs the reference (`test/storm/ProbeTest.cpp`) | 38/38 identical (2026-10-08); strings cross argv as hex, since argv is ANSI-codepage |
| `gen_ryu_tables.py` | the Ryu tables printf uses, read out of the binary into `src/storm/string/RyuTables.hpp` | |
| `disasm.py` | disassembly at an address or of a whole function, RIP-relative targets and strings annotated | capstone (llvm-objdump cannot read this exe) |
| `callers.py` | every direct call/jmp to a function, with the caller and its source file | E8/E9 rel32 scan, instruction-boundary checked |
| `calltree.py` | what a function calls, to a given depth, with sizes and source files | callees read from the corpus text (direct calls only) |
| `tags.py` | checks every `// ref:` tag in `src/` against the corpus; ported vs stubbed | |
| `fidelity.py` | fidelity of each `// ref:` port, compiled port against reference, blended from four scores: calls (weighted LCS of the direct calls, library calls dropped, PDB inline sites credited, untagged helpers flattened, tiny reference leaves weighted down), fields (struct displacements), consts (strings, floats, immediates) and shape (mnemonic trigrams); `same` when the instructions match with addresses masked. `--show` one port in full; `--stubs` stubs ranked live-first by reachability from the entry points and data tables, with the partial ports' unported arms; `--suggest` untagged port functions that sit where an untagged reference call does (tag candidates, by votes); `--delta` regressions and gains since the previous run; `--record` appends the totals to docs/fidelity.md. Writes build/fidelity.json | capstone over both binaries; `llvm-pdbutil` for the port's functions, inline sites and function ids; `llvm-symbolizer` to tie each tag to its body; the reference side is cached in build/fidelity-ref-cache.json (first run ~2.5 min, then ~50 s). Both sides are optimized: read the body before trusting a low score |
| `junkscan.py` | functions whose code still decodes as junk after stripping (77) | the decompiler's own markers: `halt_baddata()`, calls into the middle of code |

### The corpus (2026-10-08)

87,634 functions exported in 16 minutes, 87,624 decompiled (10 failed), ~200 MB in
`build/corpus.jsonl` (not committed; `--export` rebuilds it). Re-export after naming
work: names come from the Ghidra project at export time.

**Anti-tamper stripping.** The return-address check guards a return point, one copy per
return point, and the decompiler sinks the rest of the function into the check's success
branch (`if (<a> - <b> < 0x41) { <real code>; return ...; }`), inside a table walk inside
a range test, with a deliberate crash loop and its own crash label after it. Each check is
replaced by the contents of its success block, only when everything removed is free of
calls and nothing outside jumps into it, and the check's now-unused locals are dropped.
Result over the 3,332 functions with a check: **3,155 stripped completely**, 18 partially
(one check left visible), 155 kept whole, 4 not decompiled. Most kept ones are checks
whose own bytes still decode as junk (see below). Kept checks are marked in the render;
`--raw` shows the original.

**Junk left in the clean exe.** 1,012 raw decompilations show junk markers, almost all
inside the checks. After stripping, **77 functions** still decode as junk in code: 37 are
checks the stripper had to leave, and 40 sit outside any check, clustered at low
addresses (`0x14002bee0`...). `build/facts/junk.json` lists them: the places the Aegis
cleaner missed.
| `data/names.json` | hand-identified functions with the evidence (`SMemAlloc`, ...) | curated; applied last so it wins |

Ghidra scripts in `ghidra/`: `ApplyFacts.java` (all of the above), `ImportOptions.java`
(pre-analysis options for a fresh import), `RemovePieceFunctions.java` (removes functions
created on chained pieces), `DecompileList.java` (batch decompile, via `ghidra.sh decomp`).

## What the binary is like (2026-10-08 survey)

- x64, image base `0x140000000`; `.text` 38 MB. **74,763 functions.** `.pdata` has 148,635
  entries, but 73,872 of them are chained pieces (`UNW_FLAG_CHAININFO`): cold blocks the
  compiler moved out of line, each pointing at its parent. `wowbin.py` resolves every
  address through the chain to the root function, and every extractor attributes to roots.
  (Leaf functions without unwind info are not in `.pdata` at all.) An early run created
  1,787 functions at piece starts in Ghidra; `ghidra/RemovePieceFunctions.java` removed them.
- **No RTTI links to vtables.** The game code is built without RTTI: vtables sit back to
  back with no CompleteObjectLocator slot (only six std:: classes keep one), and the ~3.5k
  `.?AV` type descriptors do not pair with the vtables by order. Vtables are found from
  their constructors instead.
- **Type-name strings are Storm `new`/`delete` tags.** Nothing points at a type descriptor,
  but code loads its *name string* (descriptor + 16) and passes it where 3.3.5 passed
  `__FILE__`: 3,513 classes at 14,460 sites in 9,817 functions, 9,036 of which name exactly
  one class. 10,013 sites go to `SMemFree` (0x140255210), 3,672 to `SMemAlloc`
  (0x140255140). Each site says which class a function creates or destroys. (An early
  probe looked only for references to the descriptor itself and wrongly called them
  orphans.)
- **Anti-tamper tail left in 3,334 functions** (1,433 of the 3,542 Lua handlers, and 1,901
  others). The clean exe still carries a return-address check appended to them: the
  caller's return address is compared against a code range (`DAT_14261a0b0`) and a table
  (`DAT_1400b9810`), and a failed check crashes in a deliberate loop. It is protection, not
  game logic, and must not be ported; `retcheck.py` records where each one starts and
  Ghidra tags those functions `aegis:retcheck`.
- **Import table stripped.** One real import (`MessageBoxW`); the IAT block at the start of
  `.rdata` is zeroed in the clean image and filled at runtime, hence `iat_dump.py`.
- **Source paths**: ~930 `__FILE__` strings attribute 6,338 functions to ~910 files,
  including third-party code (protobuf, bgs-sdk, fmod) that the recomp should exclude.
- **Lua**: global API tables are plain `{name, func}` arrays (3,542 functions, `luabind.py`).
  `C_` namespaces are registered three ways: 11 through the same tables; most through
  generated code calling `register(ctx, "C_Container", "GetBagName", fn)` at
  `0x1407d2f20` / `0x1407eda90` (each API twice, once per Lua environment); and a few
  (`C_AddOns`, `C_ActionBar`, ...) one field at a time through `lua_pushcclosure` +
  `lua_setfield`, with Aegis filler nop'd between the calls (`cregister.py`). 113 of the
  117 `C_` strings are covered; `C_CharacterServicesPublic`, `C_SharedCharacterServices`,
  `C_LootHistory` and `C_WowTokenGlue` have no registration anywhere and may only exist as
  strings in this build.
- **No-return functions**: Ghidra's discovery pass is off (it stalled), and `noreturn.py`
  replaces it with 35 functions: CRT (`abort`, `terminate`, `_CxxThrowException`,
  `_invoke_watson`, `_endthreadex`, ...), a `longjmp` error thrower (likely Lua's), and the
  game's crash/fatal paths. Game asserts are not among them: they return.
- **RIP-relative references are verified.** `wowbin.riprel_refs(keep=...)` finds them by
  byte pattern, then keeps only sites that are real instruction boundaries in a decode of
  their piece. On string targets this changes almost nothing; on the anti-tamper global
  it removed 101 mid-instruction matches and two wrongly tagged functions.
- Storm containers (`TSGrowableArray`, `TSList`, ...) appear by name: knowledge from the
  3.3.5 Frozen port carries over.

## iat_dump.py needs a live client

It attaches **read-only** (`PROCESS_VM_READ`) to a running `WowClassic`, reads the
import slots and writes `imports.json`. It never launches the client; start it yourself
(login screen is enough) and run the script. One run is enough: the slot layout is fixed
in the binary, so the result is committed as `data/imports.json` and `ghidra.sh apply`
falls back to it.

What it found (2026-10-08): 614 slots in 18 DLL groups at `0x14260d000`, laid out like a
normal IAT (DLLs alphabetical, a null slot between groups). At runtime every slot points
at an obfuscated stub, not at the DLL: a `movabs` constant, a few `add`/`xor`/`imul`
steps on `rax` (some through `r10`), split across `jmp`-linked fragments, ending in
`jmp rax`. The tool follows each stub with a small register evaluator and resolves the
result against the loaded modules' exports. Forwarded exports (`HeapAlloc` ->
`ntdll!RtlAllocateHeap`) are mapped back to the name the code imported. Three pairs of
slots stay ambiguous because the DLL folds identical code under several names
(`htons`/`ntohs`, `htonl`/`ntohl`, `InitializeConditionVariable`/`InitializeSRWLock`);
they are labelled with every candidate (`htons_or_ntohs`) until a call site settles them.

### Static initializers are not in the corpus

The CRT runs 4,648 static initializers from the `_initterm` table (`0x14260e3f8..0x142617540`)
before `WinMain`. They are small leaf functions without `.pdata` entries, so Ghidra never made
functions of most of them and the corpus does not contain them. Find the one that sets up a
global with `wowbin.riprel_refs(keep=...)` on the global's address. Example: the SCmd lists'
`TSList` sentinels (`terminator | 1`) and their `atexit` destructors are set at `0x1401cb400`.
