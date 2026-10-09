# Contributing to FrostLite

FrostLite is a from-scratch reimplementation of the World of Warcraft Classic **3.4.3.54261**
client (Windows x64), ported function by function from the reference binary. It follows the
same rules as Frozen (the 3.3.5a port next door), adapted to this build.

## The reference

* The reference is `client/_classic_/WowClassic-clean.exe`: the shipped exe decrypted, with the
  Aegis protection junk nop'd. Never analyse the shipped `WowClassic.exe`; it is encrypted on
  disk. `client/` is installed by `frostlite-install` (see `tools/README.md`).
* Aegis code (the PE entry stub, the return-address checks appended to ~3.3k functions) is
  protection, not game logic. It is out of scope and is never ported.
* Read functions from the decompilation corpus (`python tools/corpus.py --addr <addr>`), not
  one headless Ghidra run at a time. `tools/calltree.py` shows what a function calls.

## Faithfulness

* Do not assume behaviour from another client version is present in 3.4.3.54261, Frozen's
  3.3.5a included. Verify it in the reference before contributing it.
* **Never borrow code from Frozen, squall or typhoon.** Assume every 3.3.5 implementation differs
  from 3.4.3's, and port each function from the 3.4.3 reference. (Checks so far bore this out:
  3.4.3's TSLink tags list ends with the low bit, and its SStrHashHT clamps to 0xFFFD.)
* Keep the original function signatures, behaviours, class names, layouts, structs and enums.
  When the original name is unknown, name by behaviour. When a 3.3.5 function plays exactly the
  same role and its name is known there, that name may be used (`CommonMain`); say so in a
  comment. Do not invent names from the 3.3.5 code for functions that merely look similar.
* The reference is already x64, so layouts carry over directly. When behaviour must differ
  anyway, document the original and the reason at the site.

## Tags

* Put `// ref: FUN_<addr>` (the reference address, lower-case hex, 9 digits) directly above every
  ported function's definition in the `.cpp`, never on a declaration.
* Code the reference only reaches through a pointer, which Ghidra never made a function (thread
  and fiber start routines, callbacks), is tagged `// ref: 0x<addr>` instead.
* A tag is a claim that the function IS that reference function. Do not tag a similar one.
* `python tools/tags.py` checks every tag against the corpus and counts ported vs stubbed.
* A reference function that is identified but not ported yet is a stub: tag it the same way and
  call `FROST_UNIMPLEMENTED()` in its body, which logs it once when reached. A run then shows
  exactly which unported function it got to.

## Commits

* Conventional Commits: `feat`, `fix` or `chore`, then the subsystem: `app`, `client`, `os`,
  `storm`, `gx`, `ui`, `net`, ... Headline at most 72 characters; detail in the body.
* One scoped change per commit.

## Code style

* `.clang-format` and `.editorconfig` (taken from Frozen).
* Fixed-width integer types (`int32_t`, `uint64_t`, ...); `intptr_t`/`uintptr_t` for pointer
  arithmetic. No `NULL`: `nullptr`, `'\0'`, `0`.
* Include local headers by their path from `src`: `#include "client/Main.hpp"`. Use `<...>` for
  system and vendor headers.
* Header guards from the path: `client/Main.hpp` -> `CLIENT_MAIN_HPP`.
* Platform code lives in its own directory (`app/win`, `os/win`), as in Frozen, so other
  platforms can be added without moving code. Windows x64 is the only target for now.
