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
# infinite ammo (real D2Game 1.13d bytes: ammo use 6fc38ce0.., throwing-weapon breakage 6fd02f1e..)
AMMOUSE=5185f6550f84d10000006a006a4656c744241000000000e8bc16ffff8be84d790ac74424040100000033ed53576a00556a4656e8ac16ffff8b442414508bdee88c9d0c008b44241885c07413833800750e50e8c118ffff8b809c000000eb0233c0556a466a015033db8bc6e8703f0c0056e86a1affff536a48568bf8e85716ffff3bc7743653576a4856e85516ffff8b44241885c07413833800750e50e87618ffff8b809c000000eb0233c0576a486a015033db8bc6e8253f0c008b4c24148b44241851e817a107008b44240c85c05f5b75088d45015d59c2080033c05d59c20800cccccccccccccccccccccccccccc5185db5556570f84d6000000833b000f
AMMOUSEP=5185f6550f84d10000006a006a4656c744241000000000e8bc16ffff8be8e9ba00000024040100000033ed53576a00556a4656e8ac16ffff8b442414508bdee88c9d0c008b44241885c07413833800750e50e8c118ffff8b809c000000eb0233c0556a466a015033db8bc6e8703f0c0056e86a1affff536a48568bf8e85716ffff3bc7743653576a4856e85516ffff8b44241885c07413833800750e50e87618ffff8b809c000000eb0233c0576a486a015033db8bc6e8253f0c008b4c24148b44241851e817a107008b44240c85c05f5b75088d45015d59c2080033c05d59c20800cccccccccccccccccccccccccccc5185db5556570f84d6000000833b000f
AMMOBRK=59c2040057e86c77f2ff663d01006a000f82250100006a4657e87c74f2ff8be883fd016a000f8e810000004d556a4657e87174f2ff85f67413833e00750e56e89676f2ff8b809c000000eb0233c0556a466a015033db8bc7e8459dffff57e83f
AMMOBRKP=59c2040057e86c77f2ff663d01006a000f82250100006a4657e87c74f2ff8be883fd006a000f8e8100000090556a4657e87174f2ff85f67413833e00750e56e89676f2ff8b809c000000eb0233c0556a466a015033db8bc7e8459dffff57e83f
.out/sigtest ammouse-orig $AMMOUSE
.out/sigtest ammouse-patched $AMMOUSEP
.out/sigtest ammouse-none $AMMOBRK
.out/sigtest ammobreak-orig $AMMOBRK
.out/sigtest ammobreak-patched $AMMOBRKP
.out/sigtest ammobreak-none $AMMOUSE
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
FEO=8a456d84c075118bc69983e20303c2c1f802f7d803f0eb2b3c017516b855555555f7ee2bd6d1fa8bc2c1e81f03c203f0eb113c02750d8bc69983e20703c2c1f8038bf081fe00001000   # Fire Enchanted per-difficulty scale (stock D2Game 1.13d 6fcef411)
.out/sigtest fe-orig $FEO
.out/sigtest fe-patched ${FEO/b855555555f7ee2bd6d1fa8bc2c1e81f03c203f0/8bc69983e20303c2c1f8028bf090909090909090}
.out/sigtest fe-none $RECV
.out/sigtest fe-none ${FEO}${FEO}                                                  # two matches = ambiguous = refuse
gcc -O1 -Wall -Wextra -Isrc -o .out/replay test/replay.c src/core.c
[ -n "${1:-}" ] && .out/replay "$1" 4 1 | grep -E "WOULD|PICKUP|end:|equipped"
