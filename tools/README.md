# FrostLite tools

The recomp pipeline (facts, Ghidra, the corpus, `translate.py`, fidelity) is **Rhipper**, its own
repository since 2026-10-23 (`C:\Users\tyler\Documents\code\Rhipper`, history included). FrostLite
is a Rhipper project: `rhipper.json` at the repository root names the reference binary
(`client/_classic_/WowClassic-clean.exe`, decrypted, Aegis junk nop'd; never the shipped
`WowClassic.exe`), the build, the Ghidra project and the hooks below. Run Rhipper's tools from the
repository root:

```bash
python ../../Documents/code/Rhipper/translate.py --handlers --all --depth 24 --fix --jobs 4
bash ../../Documents/code/Rhipper/pipeline.sh            # facts, one Ghidra session, translate
python ../../Documents/code/Rhipper/fidelity.py --summary
python ../../Documents/code/Rhipper/crashcatch.py --second-chance --timeout 900
```

What stays here is what only this binary needs:

| file | what |
|---|---|
| `aegis.py` | the anti-tamper layer: its runtime, and the guards it inlines into game functions (Rhipper's `protection` plugin) |
| `gen_script_api.py` | the Lua API handler registry and stubs (Rhipper's `pre_link` hook; `--handlers` roots) |
| `luabind.py` | Lua C function tables (a `facts_extra` extractor in `pipeline.sh`) |
| `gen_db2.py`, `gen_cvars.py`, `gen_event_ids.py`, `gen_frame_methods.py`, `gen_locale_tables.py`, `gen_ryu_tables.py`, `gen_vertex_formats.py`, `gen_char_hangul.py`, `gen_char_space.py` | sources generated from the binary's tables |
| `opcodes.py`, `updatefields.py` (+ `updatefields_names.json`) | the network opcodes and update fields |
| `luamethods.py`, `gxu_probe.py`, `printf_oracle.py`, `probe_oracle.py` | probes of WoW functions, on Rhipper's oracle |
| `fidelity_exceptions.json`, `links.json`, `translate_patches.json`, `translate_layouts.json` | this project's lists, which Rhipper's tools read from here |
| `_rhipper.py` | puts Rhipper on the import path for the tools above (`import _rhipper` first) |
