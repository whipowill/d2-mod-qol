# Diablo II QOL tweaks (qol.dll) - engineering notes

Reverse-engineering facts, protocol details, dead ends and techniques, moved out of CLAUDE.md so they are only read when a task needs them.
CLAUDE.md holds the project summary, the user's standing rules, the feature status table and the build/test commands (sections 1 and 2 of the old single file).
Addresses are RVAs unless a full `6fxxxxxx` virtual address is given. Items marked **[confirmed]** were verified in the real game by the user; everything else was built from disassembly / packet logs and is only as good as the test stated next to it.

---------------------------------------------------------------------------------------------------------------------------------
## 3. Module facts (stock LoD 1.13d)
| module | base | PE timestamp |
|---|---|---|
| D2Game | 0x6fc20000 | 1318970639 (0x4E9DE50F) |
| D2Client | 0x6fab0000 | 1318970890 |
| D2Common | 0x6fd50000 | 1318970339 |
| Storm | 0x6fbf0000 | |
- These are the **1.13d** files (BaseMod picks addresses by D2Client version; this D2Client matches its 0xd0040 = 1.13d case, 0xd003c = 1.13c).
- PD2's own D2Game is a different 2010 build (ts 1268106930) but contains the same merc rule function; PD2's *extra* merc rules live in the 5 MB `ProjectDiablo.dll`, which we never reversed.
- D2Common/Fog in the LoD folder are unmodified. `D2gfx.dll` there is the PlugY loader (calls PlugY.dll `_Init@4(0)` after the renderer loads, `_Release@4` on unload) - not recreated.
  `Patch_D2.mpq` is a rebuilt data package (182 files), not a code edit.
- D2Net code is byte-identical in PD2 and LoD, but export ordinals differ.

---------------------------------------------------------------------------------------------------------------------------------
## 4. Hooking and patching infrastructure (src/qoltweaks.c)
- **Send hook**: inline hook on D2Net's SendPacket(size, arg, packet) (stdcall ret 0xC). **Receive hook**: IAT hook of D2Client's import of D2Net ReceivePacket(buf, cap) (stdcall ret 8, returns length,
  -1 when empty, polled every frame - never log empty polls; per-poll disk flushes caused lag). Gameplay packets do NOT go through D2Client's IAT slot for send (only lobby/ping do), hence the inline hook.
- Export **ordinals differ** between builds (LoD: send #10015, recv #10005; PD2: #10024/#10008), so both are found by byte signature (`sig_is_send/recv`, tested in test/sigtest.c, exactly one match per DLL).
  SendPacket: `mov eax,[abs]; ... 85 C0 75 05 33 C0 C2 0C 00`. Receive: `8B 44 24 04 50 8B 44 24 0C 68 <q> 68 <q+4> E8 ... C2 08`.
- `h_recv` runs on the game thread every frame: `misc_tick()` (reads game tables through Storm), pickup injection (`ap_tick`), the merc-drop state machine, and some diagnostics.
- **Generated stubs** live in `VirtualAlloc`'d RWX memory (`stub_alloc`, 128-byte slots; `emit`, `emit_rel`, `emit_push`, `emit_tail`); code patches use `patch_bytes` (VirtualProtect). Every patch finder is a masked byte
  pattern that must match **exactly once**, and the module's PE `TimeDateStamp` is checked first. Function-entry bytes are verified before any `jmp` is placed.
- **Never use `IsBadReadPtr` in hooks**: it trips stack guard pages and the game's crash handler reports GUARD_PAGE. Use the VirtualQuery-based `readable()`.
- Injected packets are marked with `g_injecting` so `h_send` ignores them. `g_arg` (2nd arg of SendPacket) is learned from the game's own gameplay packets (`learned send arg=`).
- The game's crash dialogue ("Unrecoverable internal error 6fxxxxxx") prints a return address inside a DLL: subtract the module base from section 3 and disassemble there.
- A lag report during early testing was environment (Wine, d2gl resolution), not the DLL: `PROFILE` lines in the log (every 15 s, with `logging=1`) show hook cost (~0.2 us per call).

### Storm.dll file access (reading the game's own tables)
Ordinals **268 SFileOpenFileEx, 265 SFileGetFileSize, 269 SFileReadFile, 253 SFileCloseFile**, each verified by ordinal AND first-bytes signature (identical in PD2 and LoD Storm.dll); mismatch => skip and rely on
learning. #268 takes a search-scope argument: **3** (loose `Data\` first, then MPQs) when the command line contains `-direct` (the game's own rule), else **0** (`storm_scope()`). No CreateFile-based reader.
Used for: `Misc.txt` (stack maxima), `MonStats.txt` (hireling class ids), `ItemTypes.txt` (type code -> id). Called from the game thread (`misc_tick`, retries every 3 s, 30 tries).

### Ids: use the table's own id column, and skip "Expansion" divider rows
- `MonStats.txt` has an explicit **`hcIdx`** column: `ap_parse_monstats` uses it (hireling = AI `Hireable`; 271, 338, 359, 560, 561 in the user's tables); with no such column it falls back to the row number NOT counting "Expansion" rows.
- `ItemTypes.txt` in this build has NO id column (37 columns, last is `*eol`), so `merc_parse_itemtypes` uses the row number NOT counting the "Expansion" divider (ItemTypes row 58, MonStats row 410). The game gives divider rows no id.
- Lesson: verify any row-number-as-id assumption against a value the game itself reports (here the logged item type 85 vs our 86).
- `Hireling.txt` `Class` column: Rogue Scout 271, Desert Mercenary 338, Eastern Sorceror 359, Barbarian 561.

---------------------------------------------------------------------------------------------------------------------------------
## 5. The two in-memory hex edits (`patch_thread`, `[experience]`, `[death]`)
Both reproduce edits the user made by hand to D2Game.dll (found by diffing the user's DLL variants against stock; all are D2Game ts 1318970639). Both are applied by `patch_thread` once D2Game is loaded (it loads when a game starts).

### 5a. No XP level penalty (`[experience] no_level_penalty=1`)
- DLL variants: stock = PD2 root `D2Game.dll` (md5-prefix 3d27210f); `Backups/D2Game.dll` (814057c1) = stock + Snake's KeepEquip; the live LoD `D2Game.dll` (a12f2e7f) = that + the XP edit.
- The edit: **one byte, 0x74 -> 0xEB at VA 6fca7ffa (file offset 0x87ffa)**. That is the `je` after `cmp eax,0x100` inside the level-difference XP scale function (VA **6fca7fc0**; scale tables at 6fd1a370 / 6fd1a39c;
  single caller is the kill-XP function at 6fca9370). `je full` -> `jmp full` returns full XP for every level gap, in BOTH directions: it fixes "a high level gets 0 from low monsters" and "a low level partied with a high level gets 0".
- Runtime: `sig_find_xp_penalty` searches D2Game's image for the unique 20-byte pattern `3D 00 01 00 00 (74|EB) 0A B9 00 01 00 00 E9 ?? ?? ?? ?? 8B C2 C3`
  (`cmp eax,100h; je +0A; mov ecx,100h; jmp muldiv; mov eax,edx; ret`), requires exactly one match, flips 74->EB. Reports `already patched` if it is EB. Matches stock LoD and PD2's own D2Game (at +0xdaa75).
  Log line: `no XP level penalty: patched D2Game +... (je -> jmp)`. BaseMod.dll also has a "NoPenaltyMonsters" option; ours needs no BaseMod.

### 5b. KeepEquip - keep equipped items on death (`[death] keep_equipment=1`)
- Original hex edit by **SnakeByteStudios** ("D2Game_snakeskeepequip"): in the player-creation function (site VA **6fcb382b**) the 6-byte `mov edx,[ebp+0C8h]` (`8B 95 C8 00 00 00`) is replaced by a 6-byte
  **`E9 rel32 90`** (`jmp` + nop) with rel32 = **0x2f8**, jumping to the function epilogue 0x2fd bytes later, which skips the rest of the function (the part that strips equipment).
- Runtime: `sig_find_keep_equip` looks for the unique 45-byte run starting at the site: head `8B 95 C8 00 00 00`, then tail
  `8B 8D C4 00 00 00 83 C9 02 81 CA 00 10 00 00 89 95 C8 00 00 00 89 8D C4 00 00 00 8B 8B 08 05 00 00 81 F9 00 00 10 00` (the first 6 bytes may already be the jmp = already patched).
  The epilogue is found by searching up to 0x600 bytes after the site for `5F 5E 8B C5 5D 5B 83 C4 18 C2 14 00` (`pop edi; pop esi; mov eax,ebp; pop ebp; pop ebx; add esp,18h; ret 14h`); patch = `E9 <epilogue-site-5> 90`.
  Works on stock LoD and PD2's D2Game.

---------------------------------------------------------------------------------------------------------------------------------
## 6. Pickup logic (core.c) and the protocol
### Protocol facts (verified from real captures; 1.13-style, NOT D2R)
- Item packets `0x9c` (bit data at byte 8) / `0x9d` (byte 13): `flags:32 version:10 mode:3`, then ground: `x:16 y:16`, else `body:4 px:4 py:4 page:3`; then the item code = 4 x 8-bit ASCII (no Huffman - that is D2R).
  mode: 0 stored, 1 equipped, 2 belt, 3 ground, 4 cursor, 5 dropping, 6 socketed. 0x9d additionally carries the owner (unit type + id) and handles cursor moves. Header: byte1 action, byte2 size, byte3 category, bytes4-7 guid.
- 0x9c actions seen on the wire: 0x01 ground->cursor, 0x02 drop to ground, 0x04 put in container, 0x12 item to cursor (mode 4). 0x9d: 0x06 equip, 0x08 unequip. Action 14 = add to belt (px = slot), 15 = remove from belt.
- Pickup: C->S `16 04 00 00 00 <itemId u32> 00 00 00 00`. Walk/run `01|03 x(2) y(2)`. Player position: S->C 0x15 (x@6,y@8), 0x0d (x@7,y@9), plus own walk targets.
  Remove unit S->C `0a 04 <id>`. New game: S->C `01` of length 8 (game flags, once per join) resets state; a lone `00` byte arrives constantly and must NOT reset anything.
- Belt slot = px, column = px % 4. A potion goes into the belt only when a column already holds that kind and has room; otherwise it lands in the inventory (mana potions went to the inventory in early tests
  because every column held health).
- Client->server ids: 0x13 interact, 0x17 drop cursor item (5 bytes: id + dword), 0x2f NPC init, 0x30 NPC cancel, 0x36 hire merc (9 bytes: id, npc guid dword, hireling word), 0x61 merc equipment (3 bytes: id +
  body-location word), 0x62 resurrect merc (5 bytes). Server->client seen: 0x27 NPC info (opens an NPC dialogue), 0x4e/0x4f quest-log refresh, 0x21 item/attr updates, 0x9e/0x9f/0xa0 attribute updates.

### Rules (the user's requirement)
Auto-pick potions to fill the belt, only if as good as or better than the worst potion of the same kind (health/mana/rejuv) already in the belt. Never when the belt is full or the cursor is busy (it would land on the cursor).
Only hp1-5, mp1-5, rvs, rvl, stamina (vps), plus scrolls isc/tsc (need the matching tome ibk/tbk in the inventory) and keys. `rejuv_always=1` lets rejuvs bypass the belt-fit test. The core retries a pickup 3 times, and again when the
player moves closer (the server may not accept 0x16 from far away).

### Learned, never configured
- **Belt capacity**: from the equipped belt's base code via tables.h as a *prior* (LoD's 15 belts are a subset of PD2's; unknown code assumes 16); raised when a potion appears in a slot >= cap, shrunk when an expected-to-fit potion
  lands outside the belt.
- **Stack maxima** (keys, tbk, ibk): from `Misc.txt` `maxstack` via Storm (`ap_parse_misc`): the user's loose modded values are keys 24, tomes 48; archive values 12/20 (LoD) and 50/80 (PD2). Learned values override: a refused
  pickup (item re-appears on the ground, lands on the cursor/outside, or "absorbed" then reappears within 5-8 s) marks the stack full. "Full" is cleared when the stack's decoded quantity changes (else when a hash of the item BIT
  DATA - not the 8-byte header, whose action byte differs between announcements - changes), or the tome is used (client 0x20/0x26). Real PD2 order when a scroll is refused: remove-unit FIRST, then the "now in inventory" item packet
  (`scroll_gone_id`/`key_gone_id`).
- **Quantity decode** (`parse_qty_layout`, tried in order): layout 0 PD2: `socketed:3 ilvl:7 quality:4 pic:1(+3) class:1(+11) [tome spell:5] qty:9 flag:1` then the `0x1FF` property terminator; layout 1 LoD (item version 101): same WITHOUT the flag bit;
  layout 2 classic d2s order (32-bit iid + timestamp bit). The 0x1FF terminator exactly after the quantity is the sanity check. There is no 32-bit item id in these packets (it is in the 8-byte header). Verified on real packets:
  PD2 ibk=75, tbk=80, key stack=50, key=2, cube key stack=50; LoD ibk=48, tbk=48 (full), key stack=23 of 24. A decoded quantity that contradicts our own pickup switches decoding off for that kind until one matches again.
- Tests use the exact bytes of real packets (test/rules.c).

### Clicker
Thread polls `GetAsyncKeyState` (default key 32 = spacebar); after `hold_delay_ms` (default 200) posts WM_LBUTTONDOWN/UP to the game window at the cursor every `interval_ms` (100), only while the game window is foreground.
Deadline-based timing, 100 ms back-off when unfocused. **[confirmed]**

---------------------------------------------------------------------------------------------------------------------------------
## 7. Merc running fix (`[mercenary] run`)
LoD 1.13d `D2Game` starts hireling follow/approach movement in WALK mode (mode 2) instead of RUN (monster mode 15), so a merc only runs when fighting. The mode is applied by D2Common `SetMode(unit, mode)` (stdcall ret 8;
LoD #10193, PD2 #11090; found by `sig_is_setmode`: `56 8B 74 24 08 85 F6 74 ?? ... 8B 0E 83 F9 05 74 ?? ... 8B 46 10 57 8B 7C 24 10`). The fix IAT-hooks D2Game's import of it (`h_setmode`): if the unit is a monster (type 1)
whose class is in `g_hire_ids`, mode 2 -> 15. Gated on D2Game ts 1318970639. The same hook records the player's merc unit (`g_merc_unit`) for the drop feature and the quiver rules, and is installed when `run`, `drop_items` or `equip` is on.
- Found with the F9-marked mercdebug trace: the merc entered mode 2 via the WL handler (site +855c5) ~11 times in 24 s and never mode 15. Handlers at D2Game 6fca5570 (mode 0xf), 6fca55a0 (mode 2).
- Wrong leads (do not retry): D2Common #10219 is an event/timer scheduler; `add esi,0x64` / `push 2` at 6fc38112 is unrelated (a byte-patched DLL there changed nothing; d2mods.info t=63030 describes the 1.10 version).

---------------------------------------------------------------------------------------------------------------------------------
## 8. Merc equipment rules (`[mercenary] equip`)
### Structure
- The rule is hard-coded in TWO functions (Hireling.txt WType columns are ignored by the engine): server **D2Game RVA 0x6b870** (esi=item, edi=merc unit, ebx=armor flag) and client **D2Client RVA 0x90840**
  (ebx=item, ebp=merc unit, esi=armor flag, edi=out flag). Each computes an armor flag, then dispatches on merc class (271 Rogue, 338 Desert, 359 Iron Wolf, 560/561 Barbarian) to `chk(item, typeid)` calls;
  `chk` is a D2Common thunk stdcall(item,type) (equivalence-aware: an Amazon bow is a bow); the two-handed check is stdcall(item). Located by `sig_find_merc_server/client`. The server rule is called from the 0x61 handler
  (6fc8c020) after the early checks; the later equip action is 6fc8b610.
- Our patch puts a `jmp` at the start of the class dispatch into a generated stub that calls `qt_merc_allows(item, cls, chk, two, merc unit)` -> `merc_allows_hands` (merc.c): yes => the game's own "allowed" path,
  no => the game's own armor test. Server stub: `mov eax,-1; test edi,edi; je +3; mov eax,[edi+4]; push edi; push two; push chk; push eax; push esi; call qt_merc_allows; test eax,eax; jne allowed; jmp not_allowed`.
  Client stub: same with `ecx=[ebp+0xC]; call clsfn`, items in ebx, merc unit in ebp.
- Item-type codes are resolved to ids from the running game's `ItemTypes.txt` (see "Ids" in section 4). One-hand-only entries use the game's two-handed check.
- **Rules** (src/merc.c): Rogue `bow, xbow, abow` + quivers (special pairing rule below); Desert `spea, pole, jave, aspe, ajav`; Iron Wolf `shie, ashd, swor(1H only), wand, scep, mace, orb`; Barbarian (560 and 561)
  `swor, axe, mace, hamm, phlm` (1H and 2H allowed; basis: the Desert merc wields polearms with only an `hth` COF, a missing set falls back to BaseW). Armor/helm stay with the game. `EXTRAS` is empty.
- After the type rules the game calls the **D2Common requirement/class check #11072** (thunks 6fc2ac64 server / 6fabc236 client; 6-arg, ret 0x18; wrapped by #10415 which also rejects some flags). Inside it, strength, dexterity,
  level (and a 4th flag) must all pass BEFORE the class test, so a merc below an item's level/str/dex is refused before our patch runs. For class-restricted items (item class != 7) it refuses unless the merc is 560/561 and the item
  class is 4. We patch its Barbarian test (D2Common RVA **0x45900**, displacing `83 FB 04 75 16`; function 6fd95670; `sig_find_merc_class`) so `merc_class_ok()` is asked first: Rogue + Amazon bow (abow), Desert + Amazon
  spear/javelin (aspe/ajav), Iron Wolf + Paladin shield (ashd) / Sorceress orb. Item classes: 0 Ama, 1 Sor, 2 Nec, 3 Pal, 4 Bar, 5 Dru, 6 Asn, 7 none.
  Class stub: `cmp esi,1; jne ORIG; push edi; call typefn; push ebx; push [ebp+4]; push eax; call qt_merc_class_ok; test eax,eax; jne allowed; ORIG: cmp ebx,4; jne not_barb; jmp cont`.
  Helpers: item-type id 6fd919b0 (`ret 4`, uses only eax); item class 6fd92b40 (byte at ItemTypes record +0x21, 7 when none, `ret 4`); neither clobbers ebx/esi/edi/ebp.
- A THIRD copy at D2Client 6fb39e40-6fb3a2c5 (error-message calls 0x54-0x56) is tooltip/message display code, not an equip rule; it is not patched and is not needed.
- **BaseMod conflict**: its `[MercMod]` flags (FullEquip=1 default, RogueCrossBow=0, RogueAmaBow=0) install replacement copies of these functions (entry jmp in D2Client, pointer swap in D2Game); its installer guard reads
  FullEquip, the other two flags are only read inside the replacement code (the user says they work independently of FullEquip). We read `BaseMod.ini` directly and **refuse to patch** if BaseMod.dll is loaded and any of the
  three is non-zero, or if a function's entry bytes are not stock. BaseMod's FullEquip list was: ring 10, amulet 12, boots 15, gloves 16, belt 19.
- Animations: mercs have limited animation sets; `MonStats2` `BaseW`/`RHv`/`LHv` list available weapon art. Act 5 merc token `0A` has COFs only for `hth`/`1hs`/`2hs`; others have `hth` (Rogue/Desert) or `1hs` (Iron Wolf).
  A merc without a matching set falls back to its base animation, which is why 2H axes just look like swords (the user said that is fine). There are no SQ COF files anywhere.

### Rogue Amazon bows - fixed [confirmed]
The class test was refusing because of an off-by-one item-type id (section 4, "Ids"): the game said the Amazon bow is type 85, we had 86. Found with the `merc class test:` log line in the class stub
(`merc class 271, item type 85, item class 0 -> refused`). The same off-by-one affected every type after the divider: aspe, ajav, ashd, orb, phlm, bowq-era ids. Fixed by skipping the "Expansion" row.

### Rogue quivers - the game's hand-pairing rule (found from logs)
- In the user's ItemTypes.txt bows, crossbows, Amazon bows, shields AND quivers all list BodyLoc1=rarm, BodyLoc2=larm. The server's single-slot choice (D2Game 6fc8b610: GetBodyLocs 6fc2a90a -> loc1/loc2, item currently in each
  via 6fc2ac3a, then `cmp [esi+4],0x167` Iron Wolf + shield test) puts a quiver in the right hand, replacing the bow. `patch_merc_slot` replaces that test (site `81 7E 04 67 01 00 00 75 1F` at D2Game 6fc8b69b, jmp + 4 nops)
  with a stub that calls `merc_slot2(item, merc class, chk)`; second location => jne to 6fc8b6b4, else 6fc8b6c3. The Hireling panel in Inventory.txt does have an lArm box (251..306 x 47..159).
- **Why quivers (and then weapons) vanished**: the placement routine D2Game 6fcd6350 asks D2Common #10722 (6fd8aa40 "can item go in body location?") which, for hand slots, compares the new item with the item in the OTHER hand
  via 6fd8a620 -> 6fd891f0. A quiver is compatible only with a weapon of the type named in its ItemTypes `Quiver` column (bowq -> bow, xboq -> xbow; Amazon bows count as bows through Equiv1) or with an empty hand; otherwise it
  returns 2, 6fcd6350 gives up SILENTLY, and its caller 6fc8b610 still reports success and clears the cursor => the item is destroyed, no packet is sent. The check runs for whichever item is equipped second, so a crossbow
  equipped next to Arrows is destroyed too (the user hit both). Evidence: server equip rule ran (item class 528 Bolts, merc 271), equip action returned 1, merc had only her original 3 items, no item packets.
- Fix (`merc_allows_hands` in merc.c): a Rogue quiver is allowed only when the right hand is empty or holds the matching weapon; a Rogue bow/crossbow is refused when the LEFT hand holds a quiver of the other kind (crossbow vs
  Arrows, bow/Amazon bow vs Bolts). Hands are read with D2Common #10292 (item at body location, 4 right / 5 left) via `merc_hand()`; the merc unit is passed to `qt_merc_allows` by both stubs (edi server, ebp client).
  Unreadable right hand => quivers refused; unreadable left hand => weapons allowed. A refused item stays on the cursor like any disallowed item. Quiver item classes 526 (Arrows) / 528 (Bolts).

### Dead ends (do not retry without new information)
- **Ring/amulet/boots/gloves/belt (BaseMod FullEquip list)**: `Inventory.txt` "Hireling"/"Hireling2" rows only have head, torso, rArm, lArm boxes; neck/belt/feet/gloves are -1, so such items vanish. `EXTRAS` left empty.
- A Hireling.txt edit for slots/weapons: WType columns are ignored by the engine; the slot layout is in Inventory.txt.
- Earlier wrong guesses: PD2's D2Game does NOT contain the merc-rule edits (they are in ProjectDiablo.dll); the version is 1.13d not 1.13c; class ids from MonStats row numbers (561/562) instead of hcIdx (560/561).

---------------------------------------------------------------------------------------------------------------------------------
## 9. Dropping a dismissed merc's gear (`[mercenary] drop_items`) **[confirmed]**
Goal: when a replacement merc is hired, the old merc's equipment is dropped on the ground (PD2 behaviour) instead of destroyed.

### What destroys the gear
Hire request (client 0x36, handler 6fc8c690 -> 6fc94570) -> a "replace merc" function that calls the unit-free function **6fc8fc70** (ecx=game, edx=unit) on the old merc; call site RVA **0xa73e3** (`8B D7 8B CE E8`; ebx = player there).
That frees the merc's inventory through 6fc8f770. Game exit frees units through other paths (the mercdebug trace showed a different call chain).

### What did NOT work (7 crash tests)
Hooking that call and invoking the game's own routines per worn item: take-off (**6fc8b510**: eax=bodyloc, ecx=merc, push player, push game; ret 8) then drop (**6fcd7fc0**: eax=guid, push player, push game; ret 8). The SERVER did everything
right, but the CLIENT crashed in a D2Common assertion (`Unrecoverable internal error 6fdd725f`, function D2Common RVA 0x87210, null room argument) while adding a ground item. Cause (from logged packets): inside one request the server sends
the drop packet (9c action 0x02) immediately but queues the "item to cursor" packet (9c action 0x12) and sends it AFTER the drop, already showing mode 3. In normal play the pair arrives 12 then 02 because the player's two clicks are
separate requests. The client received a ground item it never saw leave the merc and its room lookup returned NULL.

### What works (current design, `dm_*` in qoltweaks.c)
1. `h_send` sees the game's hire request (0x36, 9 bytes). If drop is enabled, the merc wears items (read from the merc unit via D2Game thunk RVA **0xac3a**, item at body location 1..10) and the cursor is empty (`ap_cursor_busy`),
   the request is held back (we return the last real send result).
2. `h_recv` drives a state machine (`dm_step`): send `61 <slot> 00` (take off merc, size 3) -> wait until the client sees the cursor item -> send `17 <cursor id>` (drop, size 5) -> wait until the cursor clears -> next slot.
   Finally the held hire is released. 30 ms floor between sends; per-step 3 s timeouts and a 10 s overall timeout release the hire anyway. Pickups (`ap_tick`) are paused while it runs.
3. **The cursor item has a NEW id** (e.g. 0x7b1, 0x805) - not the merc-side id (0x2f/0x2e/0x79). Dropping with the merc-side id made the server answer 1 (refused); the item stayed on the cursor and the next 0x61 then *equipped it back* onto
   the merc ("I'll put that to good use") so the new merc ended up with no gear. `ap_cursor_id()` (core.c) exposes the cursor item id seen in the item packets.
4. Only active for stock D2Game 1.13d. If the cursor holds an item at hire time the drop is skipped (gear destroyed as before).
- The NPC dialogue staying open after a hire is NORMAL game behaviour (it also happens with `drop_items=0`). After the held hire is released the server sends 0x27 (NPC info); the game sends no 0x30 during the hold.

### Game-side facts gathered
- **Client->server packet table: D2Game VA 0x6fd1a698**, 8-byte entries {handler, flag}, index = packet id. Dispatcher at 6fc873eb: `call [edi*8+0x6fd1a698]` with ecx=game, edx=player, args (buf, size). 0x17 drop (6fc8b0d0 -> 6fcd7fc0),
  0x36 hire (6fc8c690, size 9), 0x61 merc equipment (6fc8c020: cursor empty -> take off via 6fc8b510, cursor item -> equip via 6fc8b870), 0x62 resurrect (6fc8c5c0 -> 6fc93f70).
- The other ~150-entry pointer tables in D2Game are NOT packet tables: 0x6fd1fc20 (unit events/timers), 0x6fd2b6a4 (skills), 0x6fd2b81c.
- D2Common helpers called through D2Game thunks: first inventory item 6fc2a46c, next 6fc2a60a, item unit 6fc2a520, item at body location 6fc2ac3a(inv,loc) (= D2Common #10292), cursor item 6fc2a568(inv) (= #10867), item mode 6fc2ad8a.
- Drop routine 6fcd7fc0 requires the item be the player's cursor item (mode 4); 6fc87e30(game, guid, player) returns 0 only for that case. It finds a spot with 6fd00000, places the item with 6fcff970 and notifies with 6fcd7860.
- Struct hints: unit +0 type (0 player, 1 monster, 4 item), +4 class id, +0xc guid, +0x10 mode, +0x2c path, +0x60 inventory, item mode 3 = ground. Player dynamic path words: {xOffset|xPos, yOffset|yPos, ...}; item path: pRoom1 at +0, xPos at word 3, yPos at word 4.
- Item record byte at +0x12a (read by D2Common #10055, used by the 0x61 handler as an early exit) is the "quest" flag: 0 for quivers.
- Client item-create function D2Client 6fb6acb0 -> mode setter 6fb6a320 (case mode 3/5 calls 6fabc1ee, 6fabc2d8, then AddToRoom 6fabc32c -> D2Common #10748 at 0x87210 whose assertions fire on NULL room / position outside room).

---------------------------------------------------------------------------------------------------------------------------------
## 10. Config reference, diagnostics, techniques
### Config (src/qoltweaks.ini -> build/qol.ini; pickup and clicker default ON in the template, the merc/death/experience options default OFF)
`[qol] enabled, logging` - `[pickup] enabled, range (default 4), rejuv_always, pickup_in_town (0)` - `[mercenary] run, equip, drop_items` - `[death] keep_equipment` - `[experience] no_level_penalty` - `[clicker] enabled, key (VK, 32), hold_delay_ms (200), interval_ms (100)`.
Changes apply at next game launch. `logging=0` removes the log file, the profiling timers, and the core log callback. The ini template is CRLF. Removed options (do not bring back): `keys`, `key_stack_max`, `belt_slots`, `hooks`, `packets`,
`verbose`, `pd2_equip`.

### Diagnostics
No debugging output is compiled in beyond the normal one-line-per-event log (what was loaded/patched, errors, `summons:` lifecycle lines, `merc drop:` hold/release). The quiver-hunt and summon-prototype
diagnostics were removed once everything was confirmed; they are easy to recreate (a stub that re-pushes the stack args, calls the displaced original through a trampoline and logs eax; logging the 0x3c/0x0c requests;
logging 9c/9d item packets in h_recv).

### Techniques that paid off
- `./build.sh mercdebug` -> `.out/mercdebug/qol.dll`: IAT-traces every D2Game->D2Common call whose first four args are a hireling unit, tallied by (ordinal, D2Game call site, arg index) and written every ~2 s to `mercdebug.log`;
  F9 drops a marker line (may not register if the game window swallows F9). Rare call sites that appear in only one window identify one-off events (a hire, a death). An earlier version also hooked the unit-free function (6fc8fc70) and
  printed the caller chain, which is how dismissal vs game exit were told apart; that tracer was removed in cleanup - recreate it from section 9 if needed.
- Logging raw `9c/9d` packets in `h_recv` (and, for crashes, a ring buffer dumped from a patched assertion call) is the fastest way to see what the server did.
- To locate a server handler: find the packet table (section 9), disassemble the entry, follow calls; find callers with `E8 rel32` scans; return addresses in crash dialogues point straight at the failing assert.
- A tiny wrapper that logs a server function's arguments and return value (see `wrap_server_fn`) answered "which step silently drops the item?" in one test.
- Wrong-lead log (so we do not repeat it): merc "run" at 6fc38112; IsBadReadPtr; trusting a 152-entry pointer table to be the packet table; calling server routines from inside the hire request; assuming row numbers are ids.

---------------------------------------------------------------------------------------------------------------------------------
## 11. Druid summons (`[summons] enabled`) - built, awaiting the first in-game test
- Goal: remember the live summons when you leave a game and bring them back (free, fast) on the next load, like PD2. Druid only; Windows/1.13d D2Game only.
- What failed: calling the skill runner D2Game 6fc35800 (ecx=game, edx=caster, stack skill id, level) directly ran without error but created no pets (the summon handlers 114 raven /
  115 vines / 119 wolves,bear,totems at table 0x6fd2b818 need the target state a real cast sets up). Skill lookup facts found on the way: the unit's skills are at unit+0xA8 (+4 first skill, skill+4
  next, +0x28 base level, skill record id word at +0); D2Common #10007 = GetSkillLevel(unit, skill, includeBonuses) (base + item bonuses); #10429 is NOT skill lookup (it walks stat lists).
- What works as the design: replay the game's own requests. Select skill (right hand): `3c <id lo> <id hi> 00 00 ff ff ff ff` (byte 4 = 0x80 for the left hand); cast right skill at a location: `0c <x lo hi> <y lo hi>`
  (0x0f = held). Mana: the first approach (zeroing the cost words of the skills' Skills.txt records in memory: table at `**(D2Game+0xf8260)` + 0xb98, 0x23c bytes per record, start +0x184, min +0x186, shift +0x188, base +0x18a,
  per level +0x18c) was dropped in favour of the user's idea: while recasting, the server tick (which runs before every client request is handled) sets the player's mana (stat 8) to max mana (stat 9) with D2Common
  `GetUnitStat` #10550 (unit, stat, layer) / `SetUnitStat` #10590 (unit, stat, value, layer), and when the recast ends leaves it full (`g_refill` 1 = refill before each request, and a harmless re-select is sent right after every cast so it also happens straight after it, 2 = final top-up; the first version restored the starting value, which discarded the refill and looked like the casts cost mana). The client side always sends one
  more request at the end (re-select skill) so the restore is processed immediately. All druid summons have InTown=1, so they work in town.
- Live pets: the game's monster table is 128 chains at game+0x1320 (next unit at +0xe4); unit+0 type 1, +4 class, +0x10 mode (0 dying, 12 dead). Pet classes = MonStats classes named by Skills.txt `summon`.
  `qt_srv_tick` (hooked into the packet dispatcher, RVA 0x674ae, which gives (game, player) on the server thread) counts them every 2 s and writes `qol_summons.txt` (`NAME=skillid:count,...`) when it changes.
  Character name = unit+0x14 -> player data (name at +0), class = unit+4 (5 = Druid). Saving starts only after the restore finished (`g_scan_on`), so a fresh game can't overwrite the list.
- Restore (movement does not matter): the loop casts at once, whatever the player is doing, unless her animation mode is a busy one (not 1/5 standing, 2/3/6 walking/running); success = the live count of that pet rose (`sum_count_alive`), otherwise recast after 1 s; only casts from a standstill count toward giving up (3 strikes), casts while moving don't. An earlier version that waited for her to stand still, and one that treated "moving" as "casting", both misbehaved.
- Restore flow: `sum_step` (client, from h_recv) after a new game: wait 3 s and a known position, build jobs (wolves/fenris: min(count, Param3) casts; everything else once), zero costs, send select + cast spaced 150 ms / 950 ms,
  restore the previously selected skill (only known if the game sent a 0x3c earlier) and the costs.

## 12. Act 5 merc Whirlwind (`[mercenary] whirlwind`) - working
- Data side (user's excel files, applied last into the data repo): one Act 5 merc type (Hireling.txt Class 561 = act5hire2; 24 rows). Skills now: Might (mode 1, chance 5), Battle Orders (mode 5, 10), Whirlwind (mode 14 = SQ, 85). MonStats act5hire1/2:
  `Skill1=Whirlwind, Sk1lvl=15, Sk1mode=seq_mercwhirlwind` with a loose MonSeq.txt (stock rows + 24 rows: 8 A1 frames x3, hit events at frames 3 and 7 of each). PD2 uses a separate `MonWhirlwind` skill; not needed.
- Facts: monster token `0A` has art only for DT/DD/GH/NU/RN/WL/A1/A2; no monster has SQ art (spin is built from A1 frames via MonSeq). A merc's pSkills list only holds her MonStats skills.
- Three problems, three DLL fixes (all in `patch_merc_special`, hirelings only, everything else keeps the game's behaviour):
  1. The skill-start function (srvstfunc 38 = 6fce20b0) cancels when D2Common #11023 InRange says the target is in melee reach -> a hireling only picks it next to an enemy, so every spin lasted 0 ms.
     Fix: skip the cancel (`85 C0 74 1C 57` site, `sig_find_merc_special`). The stub calls `qt_is_hireling`, which also runs `ww_extend_destination`.
  2. The destination was the adjacent enemy, so he spun on the spot. The path record (unit+0x2c) holds the destination at +0x10/+0x14/+0x18 as (y<<16)|x in sub-tile units (5 per tile),
     the enemy link at +0x58 (the game re-aims the destination at it every tick, so it is cleared). `ww_extend_destination` puts the destination past the enemy: distance + 7 units,
     clamped 10..15 (about 2-3 tiles), at a random angle of -40..+40 degrees -> carves and zig-zags without running far off.
  3. Path collision mask: players get 0x1c09, monsters 0x3c01 (blocked by other units). Fix: `add eax,0x3c01` at +c21bd (`sig_find_merc_mask`) gives hirelings 0x1c09 so the spin passes through monsters.
- Unresolved: the client shows the spin for only ~0.1 s even though the server holds mode 14 for 1-5 s (client animation of mode 14 for a monster with no SQ art).
- Balance: free of mana, 85% chance, ~6 hits/s per sequence; user plans a damage analysis across all mercs at the end.
- Tools used to find this (no longer in the DLL): SetMode call-site and call-chain logging, path-record dumps. The mercdebug build (section 12 tools) is still available.

## 13. Housekeeping reminders (user side)
`PlugY.ini` `DllToLoad` must name `qol.dll`; the LoD game's `qol.ini` currently has `[mercenary] run=1 equip=1` (+ `drop_items`).
BaseMod.ini in that folder has `FullEquip=0, RogueCrossBow=0, RogueAmaBow=0`.

## 14. Wider hallways (`[maps] wide_halls`) - **[confirmed]** working in game (stock 1.13d accepts the v18 DS1s)
- PD2 does this with data only: `pd2maps.mpq` holds 58 `Act2\Maggot\*.ds1` and 218 `Act2\Arcane\*.ds1` pieces re-saved with DS1edit (same `.tg1`/`.dt1` tilesets, same grid 11x11 Lair / 13x13 Arcane,
  wall/orientation/floor cells moved, e.g. magEW walls rows 4-5 -> rows 4 and 6; DS1 version 18 vs stock 16). `LvlMaze.txt` rows for Lair/Arcane equal stock; no dt1/txt changes; no code involved.
- `assets/maps/` holds those 276 files (extracted from PD2's `pd2maps.mpq`); `tools/make_mpq.py` packs a folder into an uncompressed MPQ v1 (encrypted hash/block tables, `(listfile)`); build.sh writes `build/qol.mpq` (tracked in git).
- Runtime: `maps_thread` mounts `qol.mpq` (game folder = cwd) with Storm `SFileOpenArchive` #266 (`name, priority 9000, flags 0, &handle`, ret 0x10; sig `8B 4C 24 10 81 EC 20 01`). Loose `Data\` files with -direct still win. Default off.
- Open questions: does stock 1.13d accept the v18 DS1s, do the pieces depend on other PD2 data (monster/object placement), are the halls actually wider in game.
- Stock MPQ reading note: stock `(listfile)` uses PKWARE implode via compression mask 0x08, which a simple MPQ reader script will lack (add a PKWARE `explode` branch for mask 0x08 in its sector decompressor).

## 15. Later changes (session notes)
- **Summons restore** (section 11 is superseded on movement): the recast now waits until her animation mode is standing (1/5) for 150 ms, then casts; the player's walk/run requests are never held; start delay 300 ms,
  loop gaps 0-15 ms, give-up after 120 s. Holding movement during casts (even briefly) felt like lag; "cast whatever she is doing" never spawned pets while moving.
- **Pickup position**: the packet-derived position was only a walk DESTINATION while moving (or the last stop), so pickup failed when moving. `live_player_pos()` reads the real tile position from the client player unit
  (D2Client 1.13d, pointer at +0x11D050, path word x at +2, y at +6) every frame and feeds `ap_set_pos()`. The address was found from disassembly (most-referenced client global), not verified in game.
- **Whirlwind slide** tightened: destination = enemy distance + 3 units, clamped 6..9, angle +-30 degrees. Open request: let the merc start WW when minions block his path (needs the AI step that picks WW and checks range;
  not found yet: tables 0x6fd2b81c/0x6fd2b724 are skill handlers, 0x6fd2ec60.. is an event table, not the AI; plan = trace build logging WW picks / blocked states).
- `logging` now defaults to 0 (template, written default ini, and code fallback).
- **Defaults changed (supersedes the "settings default to off" rule in section 1):** every feature switch now defaults to 1 (pickup, rejuv_always, clicker, run, equip, drop_items, whirlwind, keep_equipment,
  no_level_penalty, wide_halls, summons) in the template ini, the written default ini and the code fallbacks. Only `logging` stays 0. Existing game-folder `qol.ini` files keep their own values.

## 16. CPU fix (`[cpu] fix`, `throttle_render`) - built, awaiting in-game test
- D2 1.13d spins one core at 100%: its main loops never idle. Known fixes are binary-patched `D2Client.dll`/`D2Win.dll` (blizzhackers `Diablo2-1.13-cpuFix/Diablo-II-1.13d-cpuFix`; byte-diffed against stock: 8 bytes in 3 places).
  d2mods.info t=62379 describes a different fix (NOP the game-type 6/8 jumps before `Sleep(10)` at D2Client 6fad7713); we do NOT use it.
- `cpu_thread` applies the same edits in memory (gated on D2Client ts 1318970890 / D2Win ts 1318970449; exactly-once masked patterns `sig_find_cpu_*` in sig.h, checked against both stock and patched files):
  1. D2Client message pump 6faf48c0 (called every frame from the frame function): `push 0 ; call ebx(Sleep)` at 6faf4928 -> `push 1` (file/RVA +0x44929).
  2. D2Win idle/menu loop 6f8eedaf: `je +2 ; xor eax,eax ; push eax` (sleeps 0 or 20 ms) -> `nop nop nop ; push 1` (+0xedaf). This covers the MENU, which the d2mods thread never found.
  3. optional `throttle_render=1` (default 0): D2Client frame limiter 6faf5e06 `mov [6fbd46f0],ebx(0)` -> `ebp(1)` (+0x45e07). That flag skips drawing the frame (set when no logic tick ran and the frame counter is above 0x16); only partly understood.
- **Gold pickup** (`[pickup] gold`, default 1): ground item code `gld` is classified as cls 4 in core.c and picked up with the normal 0x16 request within `range`; no belt/stack/cursor tracking since gold goes straight to the purse.
  Unit-tested with synthetic packets only (test/rules.c); a gold pile the player drops on purpose would be picked up again.
- **No pickups in town**: `live_player_pos()` also reads the level number (player path+0x1c room1 -> +0x10 room2 -> +0x58 level -> +0x1d0 levelno; the standard 1.13 layout, unverified in game) every frame and calls `ap_set_town()`
  for levels 1, 40, 75, 103, 109; `ap_tick` returns 0 while in town. An unreadable chain means "not town". With `logging=1` the log has `area: level N` lines to verify the offsets.
- Does not make the game faster (logic is locked at 25/s); lowers CPU/heat only. BaseMod's `[BypassFPS]` is a separate mechanism; interaction untested.

- **Sorceress "teleports on her own" when a Druid with summons is in the game (fixed, not yet confirmed in game).** `qt_srv_tick` runs on the server thread for EVERY player's packets, and its "new game" branch (`player != last_player`) took whichever player spoke last. With a second player (a Druid bot) the tick flipped between the two: `g_char_class` became 5, `g_join_seq` was bumped, and the client side `sum_step` replayed the Druid restore on the local Sorceress: select a summon skill (she does not have it, so it fails) then `0c x y` = cast her current right skill (Teleport) at her own position. It also saved the Druid's pets, counted over the whole game, under the Druid's name. Fix: `local_player_id()` (client player unit +0xC, same id as the server unit) and the tick ignores every player that is not the local one. Same trap for any future server-tick code: check the unit id.

---------------------------------------------------------------------------------------------------------------------------------
## 17. Chat reset commands (`[respec] enabled`) - built, awaiting the first in-game test
- `.reset stats` / `.reset skills` (or with `/`), typed in the chat box. Two standalone commands, there is no combined one.
- **Changed 2026-11-13 (works for ANY player in the game, not only the host):** the first version set a flag in the CLIENT hook (`h_send`) and the server tick acted on it, so it only worked in the process that hosts the game (single player, or the TCP/IP host). A bot that joined a TCP/IP game has no server thread in its process: its `.reset stats` was sent and recorded but never consumed (Qolbot, Sankt, two tries). Now the server reads the command itself: the dispatcher stub passes `(game, player, packet id, buffer, size)` to `qt_srv_tick` (the call site has ecx=game, edx=player, edi=packet id and the handler's two stack arguments buffer, size; stub = pushad, three pushes from the stack, push edx/ecx, call, add esp 20, popad, jmp), and for packet 0x15 from a player unit `respec_parse` reads the text and `respec_run(player, req)` resets THAT player. The packet is NOT swallowed (the line shows in chat). Only the player who typed it is reset. UNTESTED in a game (built and the rule tests pass); the log says `respec: stats requested by a player in the game` on the host, then `respec: stats reset, N points returned`.
- Client: nothing is hooked for this any more (`h_send` no longer looks at chat; `g_respec_req` is gone). We do not patch PlugY's chat call site (D2Client B1FD6) because PlugY patches the same bytes.
- Logic and 1.13d addresses are from PlugY (StatsPoints.cpp / SkillsPoints.cpp): D2Common ordinals AddPlayerStat 10645, GetPlayerBaseStat 10216, GetPlayerStat 10550, SetSkillBaseLevel 10335, GetSkillLevel 10007, GetSkillCost 11081, D2GetCharStatsBIN D2Common+0x17B0 (fastcall), D2Game+0xDD4F0 SetSkillBaseLevelOnClient (fastcall). Not re-verified by disassembly. Log lines: `respec: ...`.
- Stats: each of str/dex/ene/vit goes back to the class base from charstats, minus the max life/stamina/mana the points gave; returned to stat 4. Skills: every class skill to level 0 (keeps entries with +skill gear), points to stat 5.
