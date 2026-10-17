#!/bin/bash
# Headless Ghidra against the FrostLite project (WowClassic-clean.exe, never the shipped exe).
#
#   tools/ghidra/ghidra.sh apply                    apply build/facts/*.json
#   tools/ghidra/ghidra.sh decomp <out> <addr>...   decompile functions to a file
#   tools/ghidra/ghidra.sh script <Script.java> [args...]
#   tools/ghidra/ghidra.sh all                      apply, then export the data and the corpus,
#                                                   in one session (tools/pipeline.sh)
#   tools/ghidra/ghidra.sh sample <list> <out>      apply, then decompile the listed functions
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
  all)    mkdir -p "$ROOT/build/facts"
          [ -f "$ROOT/build/facts/imports.json" ] || cp "$ROOT/data/imports.json" "$ROOT/build/facts/"
          cp "$ROOT/data/names.json" "$ROOT/build/facts/names.json"
          # the program opens once (a minute or more): the facts go in, then both exports read the
          # result. Written beside the old files and moved over only when complete
          rm -f "$ROOT/build/refdata.jsonl.new" "$ROOT/build/corpus.jsonl.new"
          "$G/support/analyzeHeadless.bat" "$PROJ" FrostLite -process "$PROG" -noanalysis \
            -scriptPath "$SCRIPTS" \
            -postScript ApplyFacts.java "$(cygpath -w "$ROOT/build/facts")" \
            -postScript ExportData.java "$(cygpath -w "$ROOT/build/refdata.jsonl.new")" \
            -postScript ExportDecompAll.java "$(cygpath -w "$ROOT/build/corpus.jsonl.new")" \
            2>&1 | tee "$ROOT/build/ghidra-last.log" | grep --line-buffered -E "ApplyFacts:|ExportD|ERROR|Exception" || true
          [ -s "$ROOT/build/refdata.jsonl.new" ] && [ -s "$ROOT/build/corpus.jsonl.new" ] || { echo "export incomplete"; exit 1; }
          # (Windows will not replace a file another process has open: a tool reading the old
          # corpus left the new one beside it once, and the session still reported success)
          for f in refdata corpus; do
            n=0
            until mv -f "$ROOT/build/$f.jsonl.new" "$ROOT/build/$f.jsonl" 2>/dev/null; do
              n=$((n + 1))
              [ $n -ge 30 ] && { echo "could not replace build/$f.jsonl (open elsewhere?); the new one is build/$f.jsonl.new"; exit 1; }
              sleep 10
            done
          done
          echo "wrote build/refdata.jsonl, build/corpus.jsonl" ;;
  decomp) out=$(cygpath -w "$2"); shift 2; run -postScript DecompileList.java "$out" "$@"; echo "wrote $out" ;;
  # apply the facts, then decompile only the functions listed in a file (one hex address a
  # line) to corpus-format JSON lines: a check of the facts on a sample before `all`
  sample) mkdir -p "$ROOT/build/facts"
          [ -f "$ROOT/build/facts/imports.json" ] || cp "$ROOT/data/imports.json" "$ROOT/build/facts/"
          cp "$ROOT/data/names.json" "$ROOT/build/facts/names.json"
          list=$(cygpath -w "$2"); out=$(cygpath -w "$3")
          "$G/support/analyzeHeadless.bat" "$PROJ" FrostLite -process "$PROG" -noanalysis \
            -scriptPath "$SCRIPTS" \
            -postScript ApplyFacts.java "$(cygpath -w "$ROOT/build/facts")" \
            -postScript ExportDecompAll.java "$out" "@$list" \
            2>&1 | tee "$ROOT/build/ghidra-last.log" | grep --line-buffered -E "ApplyFacts:|ExportD|ERROR|Exception" || true
          echo "wrote $3" ;;
  script) s="$2"; shift 2; run -postScript "$s" "$@" ;;
  *) sed -n '2,8p' "$0"; exit 1 ;;
esac
