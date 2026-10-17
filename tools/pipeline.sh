#!/bin/bash
# The whole pipeline, from the binary to src/gen, in three steps:
#
#   1. facts: every extractor over WowClassic-clean.exe -> build/facts/*.json (in parallel)
#   2. Ghidra, one session: apply the facts, export the data and the corpus
#   3. translate: tools/translate.py --handlers --depth 24 --fix
#
#   bash tools/pipeline.sh              all three
#   bash tools/pipeline.sh --no-ghidra  facts and translate (the corpus as it is)
#   bash tools/pipeline.sh --facts      facts only
#
# The prototypes come from tools/protos.py (parameters and results read off the instructions)
# and are applied by ApplyFacts; the Decompiler Parameter ID pass (CommitPrototypes.java) is gone.
set -e
cd "$(dirname "$0")/.."
start=$(date +%s)
stamp() { echo "[$(( $(date +%s) - start ))s] $*"; }

stamp "facts"
# independent of one another
pids=()
for t in pieces vtables srcrefs luabind cregister typetags retcheck noreturn globalptrs switches; do
  ( python "tools/$t.py" 2>&1 | tail -1 | sed "s/^/  $t: /" ) &
  pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done
# these read the facts above (imports, vtables, noreturn, retcheck); protos.py also writes
# varargs.json, the variadic functions read off their prologues
( python tools/funcptrs.py 2>&1 | tail -1 | sed "s/^/  funcptrs: /" ) &
p1=$!
python tools/protos.py 2>&1 | tail -2 | sed "s/^/  protos: /"
wait $p1
[ "$1" = "--facts" ] && { stamp "done"; exit 0; }

if [ "$1" != "--no-ghidra" ]; then
  stamp "ghidra: apply + export"
  bash tools/ghidra/ghidra.sh all
fi

stamp "translate"
python tools/translate.py --handlers --depth 24 --fix
stamp "done"
