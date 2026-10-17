#!/bin/bash
# Rebuild every fact file from WowClassic-clean.exe, then apply them to the Ghidra project.
#
#   tools/facts.sh            extract + apply
#   tools/facts.sh --no-apply extract only (e.g. while Ghidra has the project open)
#
# The extractors are tools/pipeline.sh's first step (one list, run in parallel); the whole
# pipeline, apply and both exports in one Ghidra session, is `bash tools/pipeline.sh`.
# imports.json is not rebuilt here: it needs a live client (tools/iat_dump.py), and the
# committed data/imports.json stands in for it.
set -e
cd "$(dirname "$0")"
bash pipeline.sh --facts
if [ "$1" != "--no-apply" ]; then
  bash ghidra/ghidra.sh apply
fi
