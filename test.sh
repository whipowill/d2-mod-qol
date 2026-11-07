#!/usr/bin/env bash
# Unit tests (synthetic packets) + replay of a real captured log. Native Linux build of the core only.
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p .out
gcc -O1 -Wall -Wextra -Isrc -o .out/rules test/rules.c src/core.c src/merc.c && .out/rules
# D2Net hook signatures vs. real bytes (identical in PD2 and stock LoD; only the export ordinals differ)
gcc -O1 -Wall -Wextra -Isrc -o .out/sigtest test/sigtest.c
SEND=a144b2bf6f85c0750533c0c20c00
RECV=8b442404508b44240c68fcb3bf6f6800b4bf6fe8a8f8ffffc208
OTHERQ=8b442404508b44240c68f4b3bf6f6800b4bf6fe8c8f8ffffc208
LODWRONG=8b4424048b0d1cb4bf6f5051e8
.out/sigtest send $SEND
.out/sigtest recv $RECV
.out/sigtest notrecv $OTHERQ      # the other queue's sibling function must not match
.out/sigtest notsend $RECV
.out/sigtest notsend $LODWRONG    # LoD's #10024 (what the old ordinal hook hit)
.out/sigtest notrecv $LODWRONG
SETMODE=568b74240885f6745e8b0e83f90574578b4610578b7c2410    # D2Common SetMode: LoD #10193, PD2 #11090 (same bytes)
.out/sigtest setmode $SETMODE
.out/sigtest notsetmode $RECV
.out/sigtest notsetmode $SEND
XPO=05b90a0000008b048d9ca3d16f3d00010000740ab900010000e9aa95f7ff8bc2c3cccccccc    # the XP level-difference scale: stock bytes
XPP=05b90a0000008b048d9ca3d16f3d00010000eb0ab900010000e9aa95f7ff8bc2c3cccccccc    # ... with the je turned into jmp
.out/sigtest keep x
.out/sigtest xp-orig $XPO
.out/sigtest xp-patched $XPP
.out/sigtest xp-none $RECV
.out/sigtest xp-none ${XPO}${XPO}                                                  # two matches = ambiguous = refuse
SPO=6a005356e83f87f4ff85c0741c57e8c11c0000538b5c24148bc6e8f550f5ff5b5f5e5d83c438c20800        # the Whirlwind-style skill start (stock D2Game 1.13d bytes around the melee-range check)
.out/sigtest special-orig $SPO
.out/sigtest special-patched ${SPO/85c0741c57/e9aabbccdd}
.out/sigtest special-none $RECV
.out/sigtest special-none ${SPO}${SPO}                                              # two matches = ambiguous = refuse
MKO=8bc7f7d81bc02508e0ffff68010c000005013c00005389445c   # the path collision-mask choice in the same skill start (stock bytes)
.out/sigtest mask-orig $MKO
.out/sigtest mask-patched ${MKO/05013c0000/e9aabbccdd}
.out/sigtest mask-none $RECV
.out/sigtest mask-none ${MKO}${MKO}
gcc -O1 -Wall -Wextra -Isrc -o .out/replay test/replay.c src/core.c
[ -n "${1:-}" ] && .out/replay "$1" 4 1 | grep -E "WOULD|PICKUP|end:|equipped"
