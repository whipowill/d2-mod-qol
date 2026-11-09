// Unit tests for the pickup rules using synthetic packets. Build/run: ./test.sh
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core.h"
#include "merc.h"
static int fails, T = 1000, rejuv, keep_learned, bad_term, gold;
static void quiet(const char *m) { (void)m; }
typedef struct { uint8_t b[64]; int pos; } bw;
static void put(bw *w, uint32_t v, int n) { for (int i = 0; i < n; i++, w->pos++) if ((v >> i) & 1) w->b[w->pos >> 3] |= 1 << (w->pos & 7); }
// build a 0x9c item packet. mode 3: x,y ; else slot px.
static int item(uint8_t *out, int act, uint32_t id, int mode, int px, int x, int y, const char *code) {
    bw w; memset(&w, 0, sizeof w);
    put(&w, 0x00200010, 32); put(&w, 0x69, 10); put(&w, mode, 3);
    if (mode == 3) { put(&w, x, 16); put(&w, y, 16); } else { put(&w, 0, 4); put(&w, px, 4); put(&w, 0, 4); put(&w, 0, 3); }
    char c4[4] = {' ', ' ', ' ', ' '}; memcpy(c4, code, strlen(code));
    for (int i = 0; i < 4; i++) put(&w, (uint8_t)c4[i], 8);
    int nb = (w.pos + 7) / 8; memset(out, 0, 8 + nb);
    out[0] = 0x9c; out[1] = act; out[2] = 8 + nb; out[3] = 0x10; memcpy(out + 4, &id, 4); memcpy(out + 8, w.b, nb);
    return 8 + nb;
}
static void recv_item(int act, uint32_t id, int mode, int px, int x, int y, const char *code) { uint8_t b[80]; int n = item(b, act, id, mode, px, x, y, code); ap_on_recv(b, n); }
static void equip_belt(const char *code) { // body=8 : craft manually
    bw w; memset(&w, 0, sizeof w); put(&w, 0x00000010, 32); put(&w, 0x69, 10); put(&w, 1, 3); put(&w, 8, 4); put(&w, 8, 4); put(&w, 0, 4); put(&w, 0, 3);
    char c4[4] = {' ', ' ', ' ', ' '}; memcpy(c4, code, strlen(code)); for (int i = 0; i < 4; i++) put(&w, (uint8_t)c4[i], 8);
    uint8_t b[64] = {0}; int nb = (w.pos + 7) / 8; b[0] = 0x9c; b[1] = 6; b[2] = 8 + nb; uint32_t id = 900; memcpy(b + 4, &id, 4); memcpy(b + 8, w.b, nb); ap_on_recv(b, 8 + nb);
}
// extended item with a quantity (key stack), PD2 layout: ...code socketed:3 ilvl:7 quality:4 pic:1 class:1 [spell:5] qty:9 flag:1 0x1FF
static void recv_stack(int act, uint32_t id, int mode, int page, int x, int y, const char *code, int qty, uint32_t flags) {
    bw w; memset(&w, 0, sizeof w);
    put(&w, flags, 32); put(&w, 0x69, 10); put(&w, mode, 3);
    if (mode == 3) { put(&w, x, 16); put(&w, y, 16); } else { put(&w, 0, 4); put(&w, 0, 4); put(&w, 0, 4); put(&w, page, 3); }
    char c4[4] = {' ', ' ', ' ', ' '}; memcpy(c4, code, strlen(code));
    for (int i = 0; i < 4; i++) put(&w, (uint8_t)c4[i], 8);
    put(&w, 0, 3); put(&w, 30, 7); put(&w, 2, 4); put(&w, 0, 1); put(&w, 0, 1);
    if (!strcmp(code, "ibk") || !strcmp(code, "tbk")) put(&w, 0, 5);   // tome spell
    put(&w, qty, 9); put(&w, 1, 1); put(&w, bad_term ? 0x0AA : 0x1FF, 9);   // quantity, flag, empty property list
    int nb = (w.pos + 7) / 8; uint8_t b[80] = {0};
    b[0] = 0x9c; b[1] = act; b[2] = 8 + nb; b[3] = 0x10; memcpy(b + 4, &id, 4); memcpy(b + 8, w.b, nb); ap_on_recv(b, 8 + nb);
}
#define KSTACK(id, qty) recv_stack(0, id, 0, 1, 0, 0, "key", qty, 0x10)
#define KGROUND(id, dx, qty) recv_stack(0, id, 3, 0, 10000 + (dx), 10001, "key", qty, 0x10)
static void start(const char *beltcode) {
    if (!keep_learned) ap_reset_learned();
    ap_config c = {1, 4, 0, rejuv, gold}; ap_init(&c, quiet);
    uint8_t g[8] = {0x01, 0, 4, 0, 0x10, 0, 1, 0}; ap_on_recv(g, 8);                         // game flags: new game
    uint8_t p[11] = {0x15, 0, 1, 0, 0, 0, 0x10, 0x27, 0x10, 0x27, 1}; ap_on_recv(p, 11);   // player at (10000,10000)
    if (beltcode) equip_belt(beltcode);
}
static int tick(void) { uint8_t o[16]; T += 800; int n = ap_tick(T, o); return n ? (int)(o[5] | o[6] << 8) : 0; }
#define CHECK(name, cond) do { if (cond) printf("ok   %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)
#define BELT(slot, id, code) recv_item(14, id, 2, slot, 0, 0, code)
#define GROUND(id, dx, code) recv_item(0, id, 3, 0, 10000 + (dx), 10001, code)

// Teach the core that key stacks hold 12 (a refused pickup of a single key onto a 12/12 stack), like the game would.
static void learn_key_cap(void) {
    keep_learned = 0; start("lbl"); KSTACK(900, 12); KGROUND(901, 2, 1); tick();
    { uint8_t r[6] = {0x0a, 4, 901 & 255, 901 >> 8, 0, 0}; ap_on_recv(r, 6); } KGROUND(901, 2, 1);
    keep_learned = 1;
}

// fake game functions for the merc rule tests: an "item" is its item-type id, bit 16 = two-handed
static int idx_of(const char **codes, int n, const char *c) { for (int i = 0; i < n; i++) if (!strcmp(codes[i], c)) return i; return 0; }
static int MERC_CALL fake_chk(merc_item_t item, int type) { return (int)(item & 0xFFFF) == type; }
static int MERC_CALL fake_two(merc_item_t item) { return (item & 0x10000) != 0; }

int main(void) {
    // ---- potions need a belt column that already holds that kind and has a free row ----
    start("lbl"); BELT(0, 100, "hp1"); GROUND(50, 2, "hp1");
    CHECK("health potion joins a health column with room", tick() == 50);
    start("lbl"); GROUND(51, 2, "hp1");
    CHECK("no pickup when the belt has no column of that kind", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); BELT(1, 101, "hp1"); BELT(2, 102, "hp1"); BELT(3, 103, "hp1"); GROUND(52, 2, "mp1");
    CHECK("mana potion ignored when belt only holds health (the bug seen in-game)", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); BELT(1, 101, "mp1"); GROUND(53, 2, "mp1");
    CHECK("mana potion taken when a mana column has room", tick() == 53);
    start("lbl"); BELT(0, 100, "mp3"); GROUND(54, 2, "mp1");
    CHECK("skips mp1 when the column's worst mana potion is mp3", tick() == 0);
    GROUND(55, 2, "mp3");
    CHECK("takes mp3 (equal to the worst)", tick() == 55);
    start("lbl"); BELT(0, 100, "hp2"); GROUND(56, 2, "hp5");
    CHECK("takes hp5 (better than the worst)", tick() == 56);
    start("lbl"); BELT(0, 100, "hp1"); BELT(4, 101, "hp1"); GROUND(57, 2, "hp1");
    CHECK("column full (2 rows) -> no pickup", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); BELT(4, 101, "hp1"); BELT(1, 102, "hp1"); GROUND(58, 2, "hp1");
    CHECK("another health column with room is enough", tick() == 58);
    start("lbl"); BELT(0, 100, "rvl"); GROUND(59, 2, "rvs");
    CHECK("skips rvs when the column holds rvl", tick() == 0);
    start("lbl"); BELT(0, 100, "vps"); GROUND(60, 2, "vps");
    CHECK("stamina potion taken when belt holds stamina", tick() == 60);
    start("lbl"); BELT(0, 100, "hp1"); GROUND(61, 2, "vps");
    CHECK("stamina potion ignored when belt has none", tick() == 0);
    start(NULL); BELT(0, 100, "hp1"); BELT(1, 101, "hp1"); BELT(2, 102, "hp1"); BELT(3, 103, "hp1"); GROUND(62, 2, "hp1");
    CHECK("no belt equipped -> 4 slots (1 row), full", tick() == 0);
    start("mbl"); BELT(0, 100, "hp1"); BELT(4, 101, "hp1"); GROUND(63, 2, "hp1");
    CHECK("12-slot belt: third row has room", tick() == 63);
    { uint8_t r[6] = {0x0a, 4, 101, 0, 0, 0}; ap_on_recv(r, 6); }
    // ---- scrolls need the matching tome in the inventory with room ----
    start("lbl"); GROUND(70, 2, "isc");
    CHECK("no identify-scroll pickup without a tome", tick() == 0);
    start("lbl"); recv_item(4, 200, 0, 1, 0, 0, "ibk"); GROUND(71, 2, "isc");
    CHECK("identify scroll taken when ibk tome present", tick() == 71);
    start("lbl"); recv_item(4, 200, 0, 1, 0, 0, "ibk"); GROUND(72, 2, "tsc");
    CHECK("town portal scroll ignored with only an identify tome", tick() == 0);
    start("lbl"); recv_item(4, 201, 0, 1, 0, 0, "tbk"); GROUND(73, 2, "tsc");
    CHECK("town portal scroll taken when tbk tome present", tick() == 73);
    // tome full: scroll shows up as a separate stored item -> stop trying
    recv_item(4, 73, 0, 2, 0, 0, "tsc"); GROUND(74, 2, "tsc");
    CHECK("tome treated as full after a scroll lands outside it", tick() == 0);
    { uint8_t u[13] = {0x20, 201, 0, 0, 0}; ap_on_send(u, 13); }
    CHECK("using the tome makes room again", tick() == 74);
    start("lbl"); recv_item(4, 201, 0, 1, 0, 0, "tbk"); GROUND(75, 2, "tsc");
    tick(); { uint8_t r[6] = {0x0a, 4, 75, 0, 0, 0}; ap_on_recv(r, 6); } GROUND(76, 2, "tsc");
    CHECK("scroll absorbed by tome keeps tome usable", tick() == 76);
    start("lbl"); recv_item(4, 201, 0, 1, 0, 0, "tbk"); GROUND(90, 2, "tsc");
    CHECK("tp scroll picked up first time", tick() == 90);
    { uint8_t r[6] = {0x0a, 4, 90, 0, 0, 0}; ap_on_recv(r, 6); } GROUND(90, 2, "tsc");
    CHECK("scroll that reappears on the ground -> tome full, stop picking it up", tick() == 0);
    GROUND(91, 2, "tsc");
    CHECK("... and other tp scrolls too", tick() == 0);
    recv_item(4, 201, 0, 1, 0, 0, "tbk");
    CHECK("... even after the same tome is re-announced", tick() == 0);
    { uint8_t u[13] = {0x20, 201, 0, 0, 0}; ap_on_send(u, 13); }
    { int r = tick(); CHECK("... until the tome is used", r == 90 || r == 91); }
    // ---- rejuvenation override ----
    rejuv = 0; start("lbl"); BELT(0, 100, "hp1"); GROUND(92, 2, "rvl");
    CHECK("rejuv ignored when the belt has no rejuv column (default)", tick() == 0);
    rejuv = 1; start("lbl"); BELT(0, 100, "hp1"); GROUND(93, 2, "rvl");
    CHECK("rejuv_always: rejuv taken with no rejuv column", tick() == 93);
    start("lbl"); BELT(0, 100, "hp1"); BELT(1, 101, "hp1"); BELT(2, 102, "hp1"); BELT(3, 103, "hp1"); GROUND(94, 2, "rvs");
    CHECK("rejuv_always: rejuv taken with a full belt", tick() == 94);
    start("lbl"); BELT(0, 100, "hp1"); GROUND(95, 2, "mp1");
    CHECK("rejuv_always does not affect other potions", tick() == 0);
    rejuv = 0;
    // ---- gold ----
    gold = 0; start("lbl"); GROUND(96, 2, "gld");
    CHECK("gold ignored when gold=0", tick() == 0);
    gold = 1; start("lbl"); GROUND(97, 2, "gld");
    CHECK("gold taken when gold=1", tick() == 97);
    start("lbl"); GROUND(98, 9, "gld");
    CHECK("gold out of range is left alone", tick() == 0);
    gold = 0;
    // ---- town ----
    start("lbl"); BELT(0, 100, "hp1"); GROUND(99, 2, "hp1"); ap_set_town(1);
    CHECK("no pickups in town", tick() == 0);
    ap_set_town(0);
    CHECK("pickups resume outside town", tick() == 99);
    // a re-announcement of the same unchanged tome with a different action byte must not clear "full"
    start("lbl"); recv_item(4, 201, 0, 1, 0, 0, "tbk"); GROUND(180, 2, "tsc"); tick();
    { uint8_t r[6] = {0x0a, 4, 180, 0, 0, 0}; ap_on_recv(r, 6); } GROUND(180, 2, "tsc");
    recv_item(6, 201, 0, 1, 0, 0, "tbk"); GROUND(181, 2, "tsc");
    CHECK("tp: tome re-announced with another action byte -> still full", tick() == 0);
    recv_stack(0, 201, 0, 1, 0, 0, "tbk", 20, 0x10); recv_stack(6, 201, 0, 1, 0, 0, "tbk", 20, 0x10);
    // decoded count: same count, different action byte -> still full; lower count -> room again
    start("lbl"); recv_stack(0, 201, 0, 1, 0, 0, "tbk", 20, 0x10); GROUND(182, 2, "tsc"); tick();
    { uint8_t r[6] = {0x0a, 4, 182, 0, 0, 0}; ap_on_recv(r, 6); } GROUND(182, 2, "tsc");
    recv_stack(6, 201, 0, 1, 0, 0, "tbk", 20, 0x10); GROUND(183, 2, "tsc");
    CHECK("tp: decoded count unchanged (other action byte) -> still full", tick() == 0);
    recv_stack(6, 201, 0, 1, 0, 0, "tbk", 19, 0x10);
    { int r = tick(); CHECK("tp: scroll used (count 19) -> room again", r == 182 || r == 183); }
    { const char *ms = "Id\tAI\tx\r\nskeleton\tSkeleton\t1\r\nroguehire\tHireable\t1\r\nExpansion\t\t\r\nact2hire\tHireable\t1\r\n";
      int ids[8]; int n = ap_parse_monstats(ms, (int)strlen(ms), ids, 8);
      CHECK("monstats: no id column -> row numbers, not counting the Expansion divider", n == 2 && ids[0] == 1 && ids[1] == 2);
      { const char *m2 = "Id\thcIdx\tAI\r\nskeleton\t0\tSkeleton\r\nroguehire\t1\tHireable\r\nExpansion\t\t\r\nact5hire1\t560\tHireable\r\nact5hire2\t561\tHireable\r\n";
        int i2[8]; int n2 = ap_parse_monstats(m2, (int)strlen(m2), i2, 8);
        CHECK("monstats: the explicit hcIdx column is used when present", n2 == 3 && i2[0] == 1 && i2[1] == 560 && i2[2] == 561); }
      { const char *sk = "skill\tId\tcharclass\tsummon\tpettype\tpetmax\tParam3\r\nRaven\t221\tdru\tdruidhawk\travens\tmin(lvl,par2)\t0\r\nSummon Spirit Wolf\t227\tdru\tspiritwolf\tspiritwolf\tmin(lvl,par3)\t5\r\nFire Bolt\t36\tsor\t\t\t\t\r\nRaise Skeleton\t70\tnec\tskeleton\tskel\t3\t\r\n";
        ap_summon_t ss[8]; int ns = ap_parse_skills(sk, (int)strlen(sk), ss, 8);
        CHECK("skills: only druid summons are read", ns == 2 && ss[0].id == 221 && ss[1].id == 227);
        CHECK("skills: wolves repeat by Param3, ravens once", ss[0].repeat == 1 && ss[1].repeat == 5 && !strcmp(ss[1].pet, "spiritwolf"));
        { const char *ms = "Id\thcIdx\tAI\r\nskeleton\t0\tx\r\ndruidhawk\t1\tx\r\nExpansion\t\t\r\nspiritwolf\t560\tx\r\n";
          char nm[3][24] = {"druidhawk", "spiritwolf", "nothere"}; int o[3];
          CHECK("monster ids by name use hcIdx", ap_monster_ids(ms, (int)strlen(ms), (const char (*)[24])nm, 3, o) == 2 && o[0] == 1 && o[1] == 560 && o[2] == -1);
          const char *m2 = "Id\tAI\r\nskeleton\tx\r\nExpansion\t\r\nspiritwolf\tx\r\n";
          CHECK("monster ids by name without hcIdx skip the divider", ap_monster_ids(m2, (int)strlen(m2), (const char (*)[24])nm, 3, o) == 1 && o[1] == 1); }
        CHECK("skills: another table is ignored", ap_parse_skills("a\tb\r\n1\t2\r\n", 12, ss, 8) == 0); }
      CHECK("monstats: unexpected table is ignored", ap_parse_monstats("a\tb\r\n1\t2\r\n", 12, ids, 8) == 0); }
    // ---- real PD2 packets (copied from qol.log): the quantity must decode ----
    { static const struct { const char *hex; int qty; const char *name; } real[] = {
        {"9c04191004000000100080006900029226b6060283" "1096fc0f", 75, "real PD2 packet: identify tome = 75"},
        {"9c04191005000000100080006900004227b6060283" "00a0fc0f", 80, "real PD2 packet: town portal tome = 80"},
        {"9c041810060000001000800069000" "4b25696070283" "20e37f", 50, "real PD2 packet: key stack = 50"},
        {"9c041810300000001000800069007" "2b25696078283" "20e07f", 2, "real PD2 packet: key stack = 2"},
        {"9c0b18109100000010008000690" "0b2b8569607828320e37f", 50, "real PD2 packet: cube key stack = 50"},
        {"9c0419100100000010008000650040" "9226b60602831060fc07", 48, "real modded-LoD packet: identify tome = 48"},
        {"9c0419100200000010008000650042" "4227b60602830060fc07", 48, "real modded-LoD packet: town portal tome = 48"},
        {"9c041810210000001000800065000" "4b25696078280" "70e13f", 23, "real modded-LoD packet: key stack = 23"} };
      for (unsigned i = 0; i < sizeof real / sizeof *real; i++) {
          uint8_t b[64]; int n = 0; for (const char *c = real[i].hex; c[0] && c[1]; c += 2) { unsigned v; sscanf(c, "%2x", &v); b[n++] = (uint8_t)v; }
          ap_reset_learned(); start("lbl"); ap_on_recv(b, n);
          CHECK(real[i].name, ap_last_qty() == real[i].qty);
      } }
    { const char *misc = "code\tmaxstack\r\ntbk\t80\r\nibk\t80\r\nkey\t50\r\n";
      const char *tbkhex = "9c04191005000000100080006900004227b6060283" "00a0fc0f";
      const char *keyhex = "9c041810060000001000800069000" "4b25696070283" "20e37f";
      uint8_t b[64]; int n = 0;
      ap_reset_learned(); ap_parse_misc(misc, (int)strlen(misc)); keep_learned = 1;
      start("lbl"); for (const char *c = tbkhex; c[0] && c[1]; c += 2) { unsigned v; sscanf(c, "%2x", &v); b[n++] = (uint8_t)v; }
      ap_on_recv(b, n); GROUND(220, 2, "tsc");
      CHECK("real PD2: tome at 80/80 + Misc.txt max 80 -> scroll skipped, no wasted pickup", tick() == 0);
      start("lbl"); n = 0; for (const char *c = keyhex; c[0] && c[1]; c += 2) { unsigned v; sscanf(c, "%2x", &v); b[n++] = (uint8_t)v; }
      ap_on_recv(b, n); KGROUND(221, 2, 1);
      CHECK("real PD2: key stack at 50/50 + Misc.txt max 50 -> key skipped", tick() == 0);
      keep_learned = 0; }
    // ---- max stack sizes read from the game's Misc.txt ----
    { const char *misc = "name\tversion\tcode\ttype\tstackable\tminstack\tmaxstack\tspawnstack\r\n"
                         "Key\t100\tkey\tkey\t1\t1\t12\t1\r\nTome of Town Portal\t100\ttbk\tbook\t1\t1\t20\t0\r\n"
                         "Tome of Identify\t100\tibk\tbook\t1\t1\t20\t0\r\nHealing Potion\t0\thp1\thpot\t0\t0\t0\t0\r\n";
      ap_reset_learned();
      int n = ap_parse_misc(misc, (int)strlen(misc));
      CHECK("misc.txt: reads key, tbk and ibk max stacks", n == 3);
      keep_learned = 1;
      start("lbl"); KSTACK(200, 12); KGROUND(190, 2, 1);
      CHECK("misc.txt: key stack 12/12 skipped without any prior learning", tick() == 0);
      start("lbl"); KSTACK(200, 11); KGROUND(191, 2, 1);
      CHECK("misc.txt: 11/12 takes a key", tick() == 191);
      start("lbl"); recv_stack(0, 201, 0, 1, 0, 0, "tbk", 20, 0x10); GROUND(192, 2, "tsc");
      CHECK("misc.txt: tome 20/20 skipped without any prior learning", tick() == 0);
      keep_learned = 0;
      n = ap_parse_misc("garbage\twith\tno\tcolumns\r\n1\t2\t3\t4\r\n", 36);
      CHECK("misc.txt: unexpected table is ignored", n == 0); }
    // scroll leaves the ground FIRST, then shows up in the inventory (real PD2 order) -> tome is full
    start("lbl"); recv_item(4, 201, 0, 1, 0, 0, "tbk"); GROUND(210, 2, "tsc");
    CHECK("tp: first scroll is tried", tick() == 210);
    { uint8_t r[6] = {0x0a, 4, 210, 0, 0, 0}; ap_on_recv(r, 6); }
    recv_item(4, 210, 0, 5, 0, 0, "tsc");
    GROUND(211, 2, "tsc");
    CHECK("tp: scroll appeared in the inventory after vanishing -> tome full, no more pickups", tick() == 0);
    { uint8_t r[6] = {0x0a, 4, 210, 0, 0, 0}; ap_on_recv(r, 6); } GROUND(210, 3, "tsc");
    CHECK("tp: ... and the same scroll thrown back out is not picked up again", tick() == 0);
    // ---- keys: only into an inventory stack with room (max 12 learned first) ----
    learn_key_cap();
    start("lbl"); KGROUND(120, 2, 1);
    CHECK("keys: no stack in inventory -> no pickup", tick() == 0);
    start("lbl"); KSTACK(200, 5); KGROUND(121, 2, 1);
    CHECK("keys: stack 5/12 has room -> pickup", tick() == 121);
    start("lbl"); KSTACK(200, 12); KGROUND(122, 2, 1);
    CHECK("keys: stack 12/12 is full -> no pickup", tick() == 0);
    start("lbl"); KSTACK(200, 11); KGROUND(123, 2, 2);
    CHECK("keys: 11 + a pile of 2 would overflow -> no pickup", tick() == 0);
    start("lbl"); KSTACK(200, 11); KGROUND(124, 2, 1);
    CHECK("keys: 11 + 1 fits exactly", tick() == 124);
    start("lbl"); recv_stack(0, 200, 0, 5, 0, 0, "key", 3, 0x10); KGROUND(125, 2, 1);
    CHECK("keys: a stack in the stash doesn't count", tick() == 0);
    start("lbl"); KSTACK(200, 10); KGROUND(127, 2, 1);
    CHECK("keys: pickup of 1 key", tick() == 127);
    { uint8_t r[6] = {0x0a, 4, 127, 0, 0, 0}; ap_on_recv(r, 6); } KGROUND(128, 2, 2);
    CHECK("keys: vanished pickup counts toward the stack (10+1+2 > 12)", tick() == 0);
    KGROUND(129, 2, 1);
    CHECK("keys: ... but 10+1+1 still fits", tick() == 129);
    KSTACK(200, 11);
    KGROUND(140, 2, 1);
    CHECK("keys: matching announcement (11) keeps decoded quantities trusted", tick() == 140 || tick() == 129);
    start("lbl"); KSTACK(200, 4); KGROUND(130, 2, 1);
    tick(); { uint8_t r[6] = {0x0a, 4, 130, 0, 0, 0}; ap_on_recv(r, 6); } KGROUND(130, 2, 1);
    CHECK("keys: server put it back on the ground -> stack treated as full", tick() == 0);
    KSTACK(200, 4); KGROUND(131, 2, 1);
    CHECK("keys: ... stays full while the stack's packet is unchanged", tick() == 0);
    KSTACK(200, 6);
    { int r = tick(); CHECK("keys: ... until the stack changes (keys used)", r == 130 || r == 131); }
    start("lbl"); KSTACK(200, 12); KSTACK(201, 3); KGROUND(132, 2, 1);
    CHECK("keys: a second stack with room is enough", tick() == 132);
    start("lbl"); recv_stack(0, 200, 0, 1, 0, 0, "key", 5, 0x00200010); KGROUND(133, 2, 1);
    CHECK("keys: undecodable stack (simple flag) -> attempt anyway", tick() == 133);
    recv_stack(4, 133, 4, 0, 0, 0, "key", 1, 0x10); KGROUND(134, 2, 1);
    CHECK("keys: ... landed on the cursor -> stop trying", tick() == 0);
    // wrong layout guard: a key whose property-list terminator is missing decodes to "unknown" instead of garbage
    bad_term = 1; start("lbl"); KSTACK(200, 12); bad_term = 0; KGROUND(135, 2, 1);
    CHECK("keys: layout check fails (no property terminator) -> quantity unknown, attempt anyway", tick() == 135);
    // quantity contradicting our own pickup -> decoded quantities ignored
    start("lbl"); KSTACK(200, 5); KGROUND(136, 2, 1);
    tick(); { uint8_t r[6] = {0x0a, 4, 136, 0, 0, 0}; ap_on_recv(r, 6); } KSTACK(200, 9);
    KSTACK(200, 12); KGROUND(137, 2, 1);
    CHECK("keys: mismatching quantity -> decoded quantities ignored, so a 12/12 stack is still attempted", tick() == 137);
    // ---- keys with no known stack size (nothing from the data files): learn it ----
    keep_learned = 0;
    start("lbl"); KSTACK(200, 12); KGROUND(150, 2, 1);
    CHECK("learn: unknown max -> try once", tick() == 150);
    { uint8_t r[6] = {0x0a, 4, 150, 0, 0, 0}; ap_on_recv(r, 6); } KGROUND(150, 2, 1);
    tick();
    keep_learned = 1;
    start("lbl"); KSTACK(200, 12); KGROUND(151, 2, 1);
    CHECK("learn: max 12 remembered into the next game -> 12/12 not attempted", tick() == 0);
    start("lbl"); KSTACK(200, 7); KGROUND(152, 2, 1);
    CHECK("learn: ... while 7/12 still is", tick() == 152);
    keep_learned = 0;
    // ---- tomes: quantity decode + learned max ----
    start("lbl"); recv_stack(0, 201, 0, 1, 0, 0, "tbk", 20, 0x10); GROUND(160, 2, "tsc");
    CHECK("tome: count known but max unknown -> try once", tick() == 160);
    { uint8_t r[6] = {0x0a, 4, 160, 0, 0, 0}; ap_on_recv(r, 6); } GROUND(160, 2, "tsc");
    tick(); keep_learned = 1;
    start("lbl"); recv_stack(0, 201, 0, 1, 0, 0, "tbk", 20, 0x10); GROUND(161, 2, "tsc");
    CHECK("tome: learned max 20 -> a 20/20 tome is skipped without trying", tick() == 0);
    start("lbl"); recv_stack(0, 201, 0, 1, 0, 0, "tbk", 19, 0x10); GROUND(162, 2, "tsc");
    CHECK("tome: 19/20 still takes a scroll", tick() == 162);
    keep_learned = 0;
    // ---- belts: don't depend on perfect tables ----
    start("zzz"); BELT(0, 100, "hp1"); GROUND(170, 2, "hp1");
    CHECK("belt: unknown belt code -> assume the biggest, pick up", tick() == 170);
    recv_item(0, 170, 0, 0, 0, 0, "hp1"); GROUND(171, 2, "hp1");
    CHECK("belt: one miss does not change the belt yet", tick() == 171);
    recv_item(0, 171, 0, 0, 0, 0, "hp1"); GROUND(172, 2, "hp1");
    CHECK("belt: second miss in a row -> capacity corrected, belt now full", tick() == 0);
    // a stuck cursor must not block pickups forever
    start("lbl"); BELT(0, 100, "hp1"); recv_item(1, 70, 4, 0, 0, 0, "hp1"); GROUND(173, 2, "hp1");
    CHECK("cursor: busy -> no pickup", tick() == 0);
    T += 25000;
    CHECK("cursor: stale for 20 s -> pickups resume", tick() == 173);
    start("lbl"); BELT(12, 100, "hp1"); BELT(0, 101, "hp1"); BELT(4, 102, "hp1"); GROUND(172, 2, "hp1");
    CHECK("belt: table too small (potion seen in slot 12) -> capacity raised", tick() == 172);
    // ---- mercenary equipment rules (merc.c) ----
    { const char *codes[] = {"bow","xbow","abow","bowq","xboq","spea","pole","jave","shie","ashd","swor","wand","scep","mace","orb","axe","hamm","phlm","club","ring","amul","boot","glov","belt","tors","helm","mis","aspe","ajav"};
      char txt[2048] = "ItemType\tCode\tEquiv1\r\n"; int ncodes = (int)(sizeof codes / sizeof *codes);
      for (int i = 0; i < ncodes; i++) { char row[64]; snprintf(row, sizeof row, "Name %d\t%s\t\r\n", i, codes[i]); strcat(txt, row); }
      int id[64]; for (int i = 0; i < ncodes; i++) id[i] = i;
      #define ITEM(code) ((merc_item_t)(id[idx_of(codes, ncodes, code)]))
      #define ITEM2H(code) ((merc_item_t)(id[idx_of(codes, ncodes, code)] | 0x10000))
      CHECK("merc: ItemTypes.txt parses", merc_parse_itemtypes(txt, (int)strlen(txt)) == ncodes && merc_rules_ready());
      CHECK("merc: wrong table is rejected", merc_parse_itemtypes("a\tb\r\n1\t2\r\n", 12) == 0 && !merc_rules_ready());
      { const char *ex = "ItemType\tCode\tEquiv1\r\nBow\tbow\t\r\nExpansion\t\t\r\nAmazon Bow\tabow\tbow\r\n";   // the divider row gets no id in the game
        merc_parse_itemtypes(ex, (int)strlen(ex));
        CHECK("merc: the Expansion divider row does not take an item-type id (abow is id 1, not 2)", merc_class_ok(1, 271, 0) && !merc_class_ok(2, 271, 0)); }
      merc_parse_itemtypes(txt, (int)strlen(txt));
      CHECK("rogue: bow",              merc_allows(ITEM("bow"), 271, fake_chk, fake_two));
      CHECK("rogue: crossbow",         merc_allows(ITEM("xbow"), 271, fake_chk, fake_two));
      CHECK("rogue: amazon bow",       merc_allows(ITEM("abow"), 271, fake_chk, fake_two));
      CHECK("rogue: arrows with a bow in hand",     merc_allows_hand(ITEM("bowq"), 271, fake_chk, fake_two, ITEM("bow") | 0x20000));
      CHECK("rogue: bolts with a crossbow in hand", merc_allows_hand(ITEM("xboq"), 271, fake_chk, fake_two, ITEM("xbow") | 0x20000));
      CHECK("rogue: quiver with an empty hand",     merc_allows_hand(ITEM("bowq"), 271, fake_chk, fake_two, 0) && merc_allows_hand(ITEM("xboq"), 271, fake_chk, fake_two, 0));
      CHECK("rogue: bolts next to a bow are refused (the game would destroy them)", !merc_allows_hand(ITEM("xboq"), 271, fake_chk, fake_two, ITEM("bow") | 0x20000));
      CHECK("rogue: arrows next to a crossbow are refused", !merc_allows_hand(ITEM("bowq"), 271, fake_chk, fake_two, ITEM("xbow") | 0x20000));
      CHECK("rogue: crossbow next to arrows is refused", !merc_allows_hands(ITEM("xbow"), 271, fake_chk, fake_two, 0, ITEM("bowq") | 0x20000));
      CHECK("rogue: bow next to bolts is refused",       !merc_allows_hands(ITEM("bow"), 271, fake_chk, fake_two, 0, ITEM("xboq") | 0x20000));
      CHECK("rogue: Amazon bow next to bolts is refused", !merc_allows_hands(ITEM("abow"), 271, fake_chk, fake_two, 0, ITEM("xboq") | 0x20000));
      CHECK("rogue: weapon next to a matching quiver is fine", merc_allows_hands(ITEM("bow"), 271, fake_chk, fake_two, 0, ITEM("bowq") | 0x20000) && merc_allows_hands(ITEM("xbow"), 271, fake_chk, fake_two, 0, ITEM("xboq") | 0x20000));
      CHECK("rogue: weapon with an empty or unreadable left hand is fine", merc_allows_hands(ITEM("xbow"), 271, fake_chk, fake_two, 0, 0) && merc_allows_hands(ITEM("xbow"), 271, fake_chk, fake_two, 0, MERC_HAND_UNKNOWN));
      CHECK("rogue: quiver refused when the right hand cannot be read", !merc_allows(ITEM("bowq"), 271, fake_chk, fake_two));
      CHECK("rogue: not a sword",      !merc_allows(ITEM("swor"), 271, fake_chk, fake_two));
      CHECK("desert: spear + polearm + javelin", merc_allows(ITEM("spea"), 338, fake_chk, fake_two) && merc_allows(ITEM("pole"), 338, fake_chk, fake_two) && merc_allows(ITEM("jave"), 338, fake_chk, fake_two));
      CHECK("desert: not a bow",       !merc_allows(ITEM("bow"), 338, fake_chk, fake_two));
      CHECK("iron wolf: shield, paladin shield, wand, scepter, mace, orb",
            merc_allows(ITEM("shie"), 359, fake_chk, fake_two) && merc_allows(ITEM("ashd"), 359, fake_chk, fake_two) && merc_allows(ITEM("wand"), 359, fake_chk, fake_two) &&
            merc_allows(ITEM("scep"), 359, fake_chk, fake_two) && merc_allows(ITEM("mace"), 359, fake_chk, fake_two) && merc_allows(ITEM("orb"), 359, fake_chk, fake_two));
      CHECK("iron wolf: 1H sword yes, 2H sword no", merc_allows(ITEM("swor"), 359, fake_chk, fake_two) && !merc_allows(ITEM2H("swor"), 359, fake_chk, fake_two));
      CHECK("iron wolf: no club, no hammer, no axe", !merc_allows(ITEM("club"), 359, fake_chk, fake_two) && !merc_allows(ITEM("hamm"), 359, fake_chk, fake_two) && !merc_allows(ITEM("axe"), 359, fake_chk, fake_two));
      CHECK("barbarian: any sword (1H and 2H) and the barbarian helm", merc_allows(ITEM("swor"), 561, fake_chk, fake_two) && merc_allows(ITEM2H("swor"), 561, fake_chk, fake_two) && merc_allows(ITEM("phlm"), 561, fake_chk, fake_two));
      CHECK("barbarian: 1H axe, mace, hammer",  merc_allows(ITEM("axe"), 561, fake_chk, fake_two) && merc_allows(ITEM("mace"), 561, fake_chk, fake_two) && merc_allows(ITEM("hamm"), 561, fake_chk, fake_two));
      CHECK("barbarian: 2H axe and 2H hammer too", merc_allows(ITEM2H("axe"), 561, fake_chk, fake_two) && merc_allows(ITEM2H("hamm"), 561, fake_chk, fake_two));
      CHECK("barbarian: no club, no bow, no shield", !merc_allows(ITEM("club"), 561, fake_chk, fake_two) && !merc_allows(ITEM("bow"), 561, fake_chk, fake_two) && !merc_allows(ITEM("shie"), 561, fake_chk, fake_two));
      CHECK("no extra slots: ring, amulet, boots, gloves, belt left to the game", !merc_allows(ITEM("ring"), 271, fake_chk, fake_two) && !merc_allows(ITEM("amul"), 338, fake_chk, fake_two) &&
            !merc_allows(ITEM("boot"), 359, fake_chk, fake_two) && !merc_allows(ITEM("glov"), 561, fake_chk, fake_two) && !merc_allows(ITEM("belt"), 271, fake_chk, fake_two));
      CHECK("armor is left to the game (body armor and helm return 0 here)", !merc_allows(ITEM("tors"), 271, fake_chk, fake_two) && !merc_allows(ITEM("helm"), 561, fake_chk, fake_two));
      CHECK("unknown merc class: left to the game", !merc_allows(ITEM("bow"), 999, fake_chk, fake_two) && !merc_allows(ITEM("bow"), -1, fake_chk, fake_two));
      char missing[1024] = "ItemType\tCode\r\nA\tbow\r\nB\tswor\r\n";
      merc_parse_itemtypes(missing, (int)strlen(missing));
      CHECK("types the mod lacks (abow, xbow...) are skipped, not fatal", merc_allows((merc_item_t)0, 271, fake_chk, fake_two) && !merc_allows((merc_item_t)1, 271, fake_chk, fake_two));
      merc_parse_itemtypes(txt, (int)strlen(txt));      // (the previous test loaded a smaller table)
      CHECK("class check: Amazon bow allowed for the Rogue only", merc_class_ok(id[2], 271, 0) && !merc_class_ok(id[2], 338, 0) && !merc_class_ok(id[2], 561, 0));
      CHECK("class check: Amazon spear + javelin for the Desert Mercenary only", merc_class_ok(id[27], 338, 0) && merc_class_ok(id[28], 338, 0) && !merc_class_ok(id[28], 271, 0) && !merc_class_ok(id[28], 338, 1));
      CHECK("desert: Amazon spear and javelin are in his list", merc_allows(ITEM("aspe"), 338, fake_chk, fake_two) && merc_allows(ITEM("ajav"), 338, fake_chk, fake_two) && !merc_allows(ITEM("ajav"), 271, fake_chk, fake_two));
      CHECK("class check: Amazon bow needs the Amazon class (not e.g. Sorceress)", !merc_class_ok(id[2], 271, 1));
      CHECK("class check: Paladin shield + Sorceress orb for the Iron Wolf", merc_class_ok(id[9], 359, 3) && merc_class_ok(id[14], 359, 1) && !merc_class_ok(id[9], 359, 1) && !merc_class_ok(id[9], 271, 3));
      CHECK("class check: everything else is the game's call", !merc_class_ok(id[17], 561, 4) && !merc_class_ok(id[0], 271, 7));
      CHECK("slot: Rogue quivers use the second location", merc_slot2(ITEM("bowq"), 271, fake_chk) && merc_slot2(ITEM("xboq"), 271, fake_chk) && !merc_slot2(ITEM("bow"), 271, fake_chk));
      CHECK("slot: Iron Wolf shield keeps the second location, a sword does not", merc_slot2(ITEM("shie"), 359, fake_chk) && !merc_slot2(ITEM("swor"), 359, fake_chk));
      CHECK("slot: other mercs never use the second location", !merc_slot2(ITEM("bowq"), 338, fake_chk) && !merc_slot2(ITEM("shie"), 561, fake_chk));
      merc_reset();
      CHECK("slot: before the rules are ready the game's own behaviour is kept (shield only)", !merc_slot2(ITEM("bowq"), 271, fake_chk)); }
    // ---- general ----
    start("lbl"); BELT(0, 100, "hp1"); GROUND(80, 30, "hp1");
    CHECK("ignores items out of range", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); GROUND(81, 1, "gld"); GROUND(82, 1, "r01");
    CHECK("ignores non-potions / non-scrolls", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); recv_item(1, 70, 4, 0, 0, 0, "hp1"); GROUND(83, 2, "hp1");
    CHECK("no pickup while an item is on the cursor", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); GROUND(84, 4, "hp1");
    { int a = tick(), b = tick(), d = tick(), e = tick();
      CHECK("keeps retrying while in range (no give-up)", a == 84 && b == 84 && d == 84 && e == 84); }
    { uint8_t w[5] = {0x03, 0x12, 0x27, 0x10, 0x27}; ap_on_send(w, 5); }
    CHECK("retries after player moves closer", tick() == 84);
    BELT(1, 84, "hp1");
    CHECK("no more attempts after it lands in the belt", tick() == 0);
    start("lbl"); BELT(0, 100, "hp1"); GROUND(85, 4, "hp1"); GROUND(86, 1, "hp1");
    CHECK("nearest first", tick() == 86);
    // a stray single 0x00 byte must not wipe state (it arrives constantly during play)
    start("lbl"); BELT(0, 100, "hp1"); { uint8_t z[1] = {0}; ap_on_recv(z, 1); } GROUND(87, 2, "hp1");
    CHECK("lone 0x00 byte does not reset state", tick() == 87);
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
