# FrostLite

**Version 0.1.0** · the FrostForge client · WoW Classic **3.4.3.54261** (Wrath Classic), Windows x64

FrostLite reimplements the World of Warcraft Classic 3.4.3.54261 client function by function from
the reference binary. Every reference function gets a counterpart, either a hand port in `src/` or a
machine translation of its decompilation in `src/gen/`. It is the client half of **FrostForge**. The
server half is [azerothcore-FrostForge](https://github.com/zeruth/azerothcore-FrostForge), an
AzerothCore fork being converted to 3.4.3 on every level. FrostLite talks only to that server and
never to Blizzard endpoints.

Read [CONTRIBUTING.md](CONTRIBUTING.md) before writing code. This is a decompilation project, so ports
keep the original names, signatures, layouts and behaviour.

## Where it stands (2026-10-19)

| | |
|---|---|
| Startup | boots to the login screen: scene, music, model sounds, keyboard, credits, cinematics, Config.wtf |
| Login click | the session object, Battle.net init and the portal lookup run, and a socket opens to 127.0.0.1; the TLS transport and the RPC layer above it are not ported yet |
| Reference functions translated (`src/gen`) | **86,228** |
| Hand ports tied to the reference (`// ref:` tags) | 91,715 of 94,806 tags tied; 91,215 ported, 169 partial |
| Ports reached through adapters | 3,296 |
| Native third-party ranges (run as the reference's own bytes) | 70 |
| Stubbed | **1** (an anti-tamper junk routine, 0x1422656e0) |
| Excluded (anti-tamper, CRT) | 1,643 |
| Static initializers run at startup | 3,679 |
| Build | clean, 0 warnings, 0 demotions |
| Fidelity, translated code | mean 0.914, size-weighted 0.865, 57,988 functions at 0.9 or better |
| Fidelity, hand ports | mean 0.844, size-weighted 0.751 |

Fidelity compares each compiled function with its reference function: the calls it makes in order,
the struct fields it touches, its constants and its instruction shape (`tools/fidelity.py`). The full
report is `build/fidelity.html` (`python tools/fidelity_report.py`), and the history is in
[docs/fidelity.md](docs/fidelity.md).

## Versioning

`MAJOR.MINOR.PATCH`, set in `CMakeLists.txt` (`project(frostlite VERSION ...)`).

- **PATCH** goes up for fixes and tooling work that leave the milestone where it is.
- **MINOR** goes up when a milestone below is reached and **seen working in a run**. A clean build
  or a fidelity score alone does not count.
- **MAJOR** goes to 1 when FrostLite logs in to FrostForge, selects a character and enters the world,
  with no stubs left and every deliberate divergence recorded in [docs/stubs.md](docs/stubs.md).

| version | milestone | state |
|---|---|---|
| 0.1.0 | every reference function translated or ported; boots clean to the login screen | **current** |
| 0.2.0 | TLS to FrostForge on 127.0.0.1 and the Battle.net RPC connection up (roadmap phase 2) | open |
| 0.3.0 | logging in with an account and password (phase 3) | open |
| 0.4.0 | realm list and realm join (phase 4) | open |
| 0.5.0 | the world connection handshake (phase 5) | open |
| 0.6.0 | character list in the glue (phase 6) | open |
| 0.7.0 | character select scene with customized models (phase 7) | open |
| 1.0.0 | in world on FrostForge | open |

## Layout

| path | what |
|---|---|
| `src/` | the client. One directory per reference subsystem (`client`, `storm`, `gx`, `ui`, `glue`, `net`, `services`, `sound`, `model2`, `world`, ...); platform code in `*/win` |
| `src/gen/` | generated, never edited by hand: `translated_*.c` (the translations), `translated.h`, `jumps.c` (jump table and static initializers), `adapters.c`, `stubs.c`, `hits.c`, `manifest.json`, `demoted.json` |
| `src/gen/RefImage.cpp` | maps the reference's data sections at their original addresses, fills its `.text` with int3, writes a jump to the translation or port at each function start, and runs the static initializers |
| `tools/` | the recomp pipeline and analysis tools (below; full table in [tools/README.md](tools/README.md)) |
| `tools/ghidra/` | headless Ghidra scripts and `ghidra.sh` |
| `frostlite-install/` | Rust installer: downloads the 3.4.3.54261 client from a TACT CDN mirror into `client/` (`plan`, `install`, `reindex`, `verify`), built on `cascette-rs` with `patches/cascette-rs-fixes.patch` |
| `vendor/` | expat, FMOD Ex 4.44.64, FreeType 2.9.1, libvpx, Lua 5.1.4, zlib, at the versions the reference links |
| `data/` | committed facts: `imports.json` (the stripped IAT, read from a live client once), `names.json` (hand-identified functions), `indirect_roots.json`, `import_protos.json` |
| `docs/` | [login-roadmap.md](docs/login-roadmap.md), [stubs.md](docs/stubs.md), [fidelity.md](docs/fidelity.md), [glue-render-plan.md](docs/glue-render-plan.md) |
| `test/` | unit tests and the oracle cases (`test/storm/printf_cases.txt`, ...) |

## Building and running

Requirements: Windows x64, Visual Studio 2022 or newer (MSVC, C++17; developed on VS 2026), CMake 3.20 or newer, Python 3
with `capstone`, `pefile` and `unicorn` for the tools, LLVM (`llvm-pdbutil`, `llvm-symbolizer`) for
fidelity and crash symbolization, Ghidra for the pipeline, and Rust for the installer.

```bash
# the client data, once
cargo run --release --manifest-path frostlite-install/Cargo.toml -- install client

# configure and build out of source (CLion's cmake-build-release works the same)
cmake -S . -B build/cmake -G "Visual Studio 18 2026" -A x64
cmake --build build/cmake --config Release --target FrostLite
```

Run `FrostLite.exe` with the working directory set to `client/_classic_`, where Config.wtf, the CASC
data and `.build.info` live. Unlike the reference, FrostLite does not move to its own directory at
startup (docs/stubs.md, "Diverged on purpose"). Keep the PDB beside the exe, because crash
symbolization depends on it.

The tools read `client/_classic_/WowClassic-clean.exe`, which is the shipped exe decrypted with its
anti-tamper junk nop'd. Never analyse the shipped `WowClassic.exe`, because it is encrypted on disk.

## The pipeline

Facts come from Python, Ghidra runs once, and translation comes last. Extractors read the PE
directly in seconds and write JSON to `build/facts/`. One Ghidra session applies the facts and
exports the decompilation. The translator then turns that decompilation into C that compiles.

```bash
bash tools/pipeline.sh                # facts -> Ghidra (apply + export) -> translate   (~20 min)
bash tools/pipeline.sh --no-ghidra    # facts and translate against the current corpus
bash tools/pipeline.sh --facts        # facts only

# translate only, as the pipeline runs it (~5 min); --fix builds, demotes what fails, repeats
python tools/translate.py --handlers --all --depth 24 --fix --jobs 4 > build/translate.log
```

1. **Facts** (`tools/*.py`, about 2 min). These recover:
   - `.pdata` pieces, vtables and source files
   - Lua bindings and `C_` registrations
   - type tags and noreturn functions
   - global pointers and jump tables
   - anti-tamper return checks (`retcheck.py`)
   - function pointers (`funcptrs.py`)
   - every prototype, read off the instructions (`protos.py`), including code reached only through
     pointers
   - preserved registers (`preserved.py`)
2. **Ghidra** (`tools/ghidra/ghidra.sh all`, about 12 min). `ApplyFacts.java` applies every fact and
   never overwrites a name set by hand. `ExportData.java` and `ExportDecompAll.java` then write
   `build/refdata.jsonl` and `build/corpus.jsonl`. Each export goes to a `.new` file and replaces
   the old one only when complete.
3. **Translate** (`tools/translate.py`). It plans from the roots (entry points, Lua handlers, vtable
   slots, static initializers) and emits `src/gen`. It builds that output and demotes any body that
   does not compile, recording the compiler's line in `demoted.json`. It stops when the build is
   clean.

Defects are fixed where they arise (a fact, ApplyFacts or the translator's reading of a construct),
never patched per function. `tools/translate_patches.json` holds what is not fixed at its source yet.

Back up the Ghidra project before a session that changes much. The facts can be re-run, but what they
replace is not kept.

## Tools at a glance

| tool | use |
|---|---|
| `corpus.py --addr <a>` / `--module <file>` | read the decompilation, anti-tamper stripped |
| `disasm.py`, `callers.py`, `calltree.py`, `xrefs.py` | disassembly, callers, callees, references |
| `tags.py` | check every `// ref:` tag against the corpus |
| `fidelity.py --summary` / `--show` / `--stubs` / `--delta` / `--record` | measure the ports; `fidelity_report.py` writes `build/fidelity.html` |
| `crashcatch.py [--second-chance] [--timeout N] [--hits out.txt]` | run FrostLite under a minimal debugger, report the first real exception with a symbolized stack. With `--hits`, dump each translated function's call count at the stop, which locates an early abort by comparing counts along a call path |
| `oracle.py`, `printf_oracle.py`, `probe_oracle.py` | run reference functions under Unicorn and compare them with the ports |
| `translate.py --faults <log>` / `--why` / `--risks` | map a run's translated faults to source, explain why a function is or is not translated |
| `junkscan.py`, `leftover.py` | anti-tamper junk still in the clean exe; what the translator left out |
| `iat_dump.py` | the stripped import table, read once from a live client (committed as `data/imports.json`) |

At run time, setting `FROST_HITS=<file>` makes FrostLite write the translated functions' call counts
every 5 seconds.

## What is left

**Toward the next milestones** ([docs/login-roadmap.md](docs/login-roadmap.md)):
- Phase 2: the SChannel TLS transport, `rpc_connection` and its manager, and the protobuf runtime
  with the Battle.net messages, all running against FrostForge's certificate on 127.0.0.1.
- Phases 3 to 7: GlueLogin and the Authentication service, the GameUtilities realm list, the realm
  join, `WowConnection` / `PacketPipe` / `WDataStore` with the modern world handshake, character
  enumeration and the CharacterSelect Lua API, and the character select scene.
- On the server side, FrostForge needs 3.4.3 DB2 loading, Battle.net accounts, the 3.4.3 character
  schema, and authserver removed.

**Fidelity:**
- 331 hand stubs still run their reference function's translation; each should become a real port.
- 730 hand ports score below 0.5; read the body before trusting the score, since both sides are
  optimized.
- Translated functions at 0.9 or better fell from 58,020 to 57,988 after the tail-call fix. This is
  not investigated yet.

**Held back on purpose** ([docs/stubs.md](docs/stubs.md)):
- Telemetry, the Battle.net Agent and the CDN downloader stay offline, because none may contact
  Blizzard.
- The final teardown after the Config.wtf save is not ported.
- `OsRegistryReadString` is only reached with `-launcherlogin`.
- The anti-tamper runtime is diverged by design: its checks pass and nothing else runs.

**Not started:** everything in world (maps, movement, spells, the world renderer past the glue
scene), and game pad support.
