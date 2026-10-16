#!/bin/bash
# Builds the FrostLite fixes to Ghidra's own classes into <ghidra>/Ghidra/patch/frostlite.jar.
# Ghidra puts that directory ahead of its release jars, so the classes here replace the stock
# ones without touching the install otherwise. Delete the jar to go back to stock.
#
#   src/ghidra/program/model/pcode/HighParamID.java  a float return measured in XMM0 was
#       committed in RAX by Decompiler Parameter ID (see the patch comment)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
G=$(ls -d /c/Users/tyler/tools/ghidra_* | head -1)
OUT="$HERE/../../../build/ghidra-patch"
rm -rf "$OUT" && mkdir -p "$OUT/classes"
CP=$(find "$G/Ghidra" -name "*.jar" ! -path "*/patch/*" | xargs -n1 cygpath -w | paste -sd";")
find "$HERE/src" -name "*.java" | xargs -n1 cygpath -w > "$OUT/sources.txt"
javac --release 21 -nowarn -cp "$CP" -d "$(cygpath -w "$OUT/classes")" "@$(cygpath -w "$OUT/sources.txt")"
jar cf "$(cygpath -w "$G/Ghidra/patch/frostlite.jar")" -C "$(cygpath -w "$OUT/classes")" .
echo "wrote $G/Ghidra/patch/frostlite.jar"
