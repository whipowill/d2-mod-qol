#!/usr/bin/env bash
# Cross-compile the 32-bit Windows DLL from Linux using zig (pip: ziglang, installed into .tools/).
# One-time setup:  pip install --target .tools ziglang
set -euo pipefail
cd "$(dirname "$0")"
export PYTHONPATH="$PWD/.tools"
mkdir -p build .out
ZIG="python3 -m ziglang cc -target x86-windows-gnu -O2 -Wall"
$ZIG -shared -o .out/qol.dll src/qoltweaks.c src/core.c src/merc.c -luser32
$ZIG -o .out/loadtest.exe test/loadtest.c
cp .out/qol.dll build/qol.dll   # build/ holds only the deliverables; intermediates live in .out/
cp src/qoltweaks.ini build/qol.ini
python3 tools/make_mpq.py assets/maps build/qol.mpq   # the re-edited map pieces (Maggot Lair / Arcane Sanctuary)
if [ "${1:-}" = "mercdebug" ]; then      # temporary diagnostic build for the mercenary run bug (never goes in build/)
  mkdir -p .out/mercdebug
  $ZIG -shared -DMERCDEBUG -o .out/mercdebug/qol.dll src/qoltweaks.c src/core.c src/merc.c -luser32
  echo "built diagnostic: .out/mercdebug/qol.dll"
fi
echo "built: build/qol.dll build/qol.ini build/qol.mpq"
