#!/bin/bash
# Rebuild every fact file from WowClassic-clean.exe, then apply them to the Ghidra project.
#
#   tools/facts.sh            extract + apply
#   tools/facts.sh --no-apply extract only (e.g. while Ghidra has the project open)
#
# imports.json is not rebuilt here: it needs a live client (tools/iat_dump.py), and the
# committed data/imports.json stands in for it.
set -e
cd "$(dirname "$0")"
for t in pieces vtables srcrefs luabind cregister typetags retcheck noreturn; do
  printf '%-10s ' "$t"
  python "$t.py" | tail -1
done
if [ "$1" != "--no-apply" ]; then
  bash ghidra/ghidra.sh apply
fi
