#!/bin/bash
# Headless Ghidra against the FrostLite project (WowClassic-clean.exe, never the shipped exe).
#
#   tools/ghidra/ghidra.sh apply                    apply build/facts/*.json
#   tools/ghidra/ghidra.sh decomp <out> <addr>...   decompile functions to a file
#   tools/ghidra/ghidra.sh script <Script.java> [args...]
#
# Each run takes a minute or more to open the program: batch addresses.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
G=$(ls -d /c/Users/tyler/tools/ghidra_* | head -1)
PROJ='C:\Users\tyler\tools\ghidra-projects'
PROG=WowClassic-clean.exe
# one directory only: cmd.exe splits a ";"-joined script path into separate arguments
SCRIPTS="$(cygpath -w "$HERE")"
export GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-14G}
mkdir -p "$ROOT/build"

run() {
  "$G/support/analyzeHeadless.bat" "$PROJ" FrostLite -process "$PROG" -noanalysis \
    -scriptPath "$SCRIPTS" "$@" > "$ROOT/build/ghidra-last.log" 2>&1 || true
  grep -E "ApplyFacts:|ERROR|Exception|wrote" "$ROOT/build/ghidra-last.log" | head -20
}

case "$1" in
  apply)  mkdir -p "$ROOT/build/facts"
          # imports.json needs a live client to regenerate, so a committed copy backs it
          [ -f "$ROOT/build/facts/imports.json" ] || cp "$ROOT/data/imports.json" "$ROOT/build/facts/"
          cp "$ROOT/data/names.json" "$ROOT/build/facts/names.json"   # hand-identified, always current
          run -postScript ApplyFacts.java "$(cygpath -w "$ROOT/build/facts")" ;;
  decomp) out=$(cygpath -w "$2"); shift 2; run -postScript DecompileList.java "$out" "$@"; echo "wrote $out" ;;
  script) s="$2"; shift 2; run -postScript "$s" "$@" ;;
  *) sed -n '2,8p' "$0"; exit 1 ;;
esac
