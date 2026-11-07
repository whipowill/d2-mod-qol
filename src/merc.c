// merc.c - see merc.h
#include "merc.h"
#include <string.h>
#include <stdlib.h>

#define MAX_TYPES 512
static char type_code[MAX_TYPES][8];
static int n_types;
static int ready;

typedef struct { const char *code; int one_hand_only; } rule_src;
typedef struct { int id; int one_hand_only; } rule_t;

// PD2's merc equipment (https://wiki.projectdiablo2.com/wiki/Mercenaries), by item-type code. one_hand_only: the game never allowed
// two-handed ones (Iron Wolf swords). The Barbarian takes two-handed axes/hammers too: he has no animation set for them, but a
// merc without a matching set falls back to his base animation (the Desert Mercenary carries polearms the same way).
static const rule_src ROGUE[]  = {{"bow",0},{"xbow",0},{"abow",0},{0,0}};
static const rule_src DESERT[] = {{"spea",0},{"pole",0},{"jave",0},{"aspe",0},{"ajav",0},{0,0}};
static const rule_src WOLF[]   = {{"shie",0},{"ashd",0},{"swor",1},{"wand",0},{"scep",0},{"mace",0},{"orb",0},{0,0}};
static const rule_src BARB[]   = {{"swor",0},{"axe",0},{"mace",0},{"hamm",0},{"phlm",0},{0,0}};
// BaseMod's FullEquip list, for every merc: ring, amulet, boots, gloves, belt (body armor and helms the game already allows).
// The Hireling panel in Inventory.txt has no ring/neck/belt/feet/gloves boxes, so those items would vanish; left empty.
static const rule_src EXTRAS[] = {{0,0}};

#define MAX_RULES 16
static rule_t r_rogue[MAX_RULES], r_desert[MAX_RULES], r_wolf[MAX_RULES], r_barb[MAX_RULES], r_extra[MAX_RULES];
static int n_rogue, n_desert, n_wolf, n_barb, n_extra;
static int id_aspe = -1, id_ajav = -1, id_abow = -1, id_ashd = -1, id_orb = -1, id_bowq = -1, id_xboq = -1, id_shie = -1, id_bow = -1, id_xbow = -1;

static int type_id(const char *code) {
    for (int i = 0; i < n_types; i++) if (!strcmp(type_code[i], code)) return i;
    return -1;
}
static int resolve(const rule_src *src, rule_t *out) {
    int n = 0;
    for (; src->code && n < MAX_RULES; src++) {
        int id = type_id(src->code);
        if (id >= 0) { out[n].id = id; out[n].one_hand_only = src->one_hand_only; n++; }   // types a mod doesn't have are skipped
    }
    return n;
}

void merc_reset(void) {
    n_types = 0; ready = 0; n_rogue = n_desert = n_wolf = n_barb = n_extra = 0;
    id_aspe = id_ajav = id_abow = id_ashd = id_orb = id_bowq = id_xboq = id_shie = id_bow = id_xbow = -1;
}
int merc_rules_ready(void) { return ready; }

int merc_parse_itemtypes(const char *txt, int len) {
    merc_reset();
    int col_code = -1, line = 0, row = 0;
    const char *end = txt + len;
    while (txt < end) {
        const char *eol = memchr(txt, '\n', (size_t)(end - txt));
        if (!eol) eol = end;
        const char *f = txt; int idx = 0, expansion = 0;
        char code[8] = {0};
        for (const char *q = txt; q <= eol; q++) {
            if (q != eol && *q != '\t') continue;
            int fl = (int)(q - f);
            if (fl > 0 && f[fl - 1] == '\r') fl--;
            if (line == 0) { if (fl == 4 && !memcmp(f, "Code", 4)) col_code = idx; }
            else {
                if (idx == 0 && fl == 9 && !memcmp(f, "Expansion", 9)) expansion = 1;     // divider row: the game does not give it an id
                if (idx == col_code) {
                    int n = fl < 7 ? fl : 7;
                    memcpy(code, f, (size_t)n); code[n] = 0;
                    for (char *c = code; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
                }
            }
            idx++; f = q + 1;
        }
        if (line == 0 && col_code < 0) return 0;                  // not the table we expected
        if (line > 0 && !expansion) { if (row < MAX_TYPES) strcpy(type_code[row], code); row++; }
        line++; txt = eol + 1;
    }
    n_types = row < MAX_TYPES ? row : MAX_TYPES;
    n_rogue = resolve(ROGUE, r_rogue); n_desert = resolve(DESERT, r_desert); n_wolf = resolve(WOLF, r_wolf);
    n_barb = resolve(BARB, r_barb); n_extra = resolve(EXTRAS, r_extra);
    id_aspe = type_id("aspe"); id_ajav = type_id("ajav"); id_abow = type_id("abow"); id_ashd = type_id("ashd"); id_orb = type_id("orb"); id_bowq = type_id("bowq"); id_xboq = type_id("xboq"); id_shie = type_id("shie"); id_bow = type_id("bow"); id_xbow = type_id("xbow");
    ready = n_types > 0 && n_rogue > 0;
    return n_types;
}

static int match(const rule_t *r, int n, merc_item_t item, merc_chk chk, merc_two two) {
    for (int i = 0; i < n; i++) {
        if (!chk(item, r[i].id)) continue;
        if (r[i].one_hand_only && two(item)) continue;
        return 1;
    }
    return 0;
}

int merc_allows_hands(merc_item_t item, int cls, merc_chk chk, merc_two two, merc_item_t right_hand, merc_item_t left_hand) {
    if (!ready) return 0;
    int hit = 0;
    switch (cls) {
    case 271:
        hit = match(r_rogue, n_rogue, item, chk, two);
        if (hit && left_hand != MERC_HAND_UNKNOWN && left_hand) {                                      // a weapon next to a quiver of the other kind is destroyed by the game
            if (id_bowq >= 0 && chk(left_hand, id_bowq) && id_xbow >= 0 && chk(item, id_xbow)) hit = 0;   // crossbow next to arrows
            if (id_xboq >= 0 && chk(left_hand, id_xboq) && ((id_bow >= 0 && chk(item, id_bow)) || (id_abow >= 0 && chk(item, id_abow)))) hit = 0;   // bow (or Amazon bow) next to bolts
        }
        if (!hit && right_hand != MERC_HAND_UNKNOWN) {                                                // quivers: only with the weapon they belong to
            if (id_bowq >= 0 && chk(item, id_bowq)) hit = !right_hand || (id_bow >= 0 && chk(right_hand, id_bow)) || (id_abow >= 0 && chk(right_hand, id_abow));
            else if (id_xboq >= 0 && chk(item, id_xboq)) hit = !right_hand || (id_xbow >= 0 && chk(right_hand, id_xbow));
        }
        break;
    case 338: hit = match(r_desert, n_desert, item, chk, two); break;
    case 359: hit = match(r_wolf, n_wolf, item, chk, two); break;
    case 560:
    case 561: hit = match(r_barb, n_barb, item, chk, two); break;
    default: return 0;                                             // not one of the standard mercs: leave it to the game
    }
    return hit || match(r_extra, n_extra, item, chk, two);
}
int merc_allows_hand(merc_item_t item, int cls, merc_chk chk, merc_two two, merc_item_t right_hand) { return merc_allows_hands(item, cls, chk, two, right_hand, MERC_HAND_UNKNOWN); }
int merc_allows(merc_item_t item, int cls, merc_chk chk, merc_two two) { return merc_allows_hands(item, cls, chk, two, MERC_HAND_UNKNOWN, MERC_HAND_UNKNOWN); }

int merc_class_ok(int type_id, int merc_class, int item_class) {
    if (!ready) return 0;
    if (merc_class == 271) return item_class == 0 && id_abow >= 0 && type_id == id_abow;                  // Rogue: Amazon bows
    if (merc_class == 338)                                                                                 // Desert Mercenary: Amazon spears and javelins
        return item_class == 0 && ((id_aspe >= 0 && type_id == id_aspe) || (id_ajav >= 0 && type_id == id_ajav));
    if (merc_class == 359)                                                                                 // Iron Wolf: Paladin shields, Sorceress orbs
        return (item_class == 3 && id_ashd >= 0 && type_id == id_ashd) || (item_class == 1 && id_orb >= 0 && type_id == id_orb);
    return 0;                                                                                              // Barbarian helms: the game already allows them
}

int merc_slot2(merc_item_t item, int cls, merc_chk chk) {
    if (!ready) return cls == 359 && id_shie >= 0 && chk(item, id_shie);           // never worse than the game's own behaviour
    if (cls == 359) return id_shie >= 0 && chk(item, id_shie);
    if (cls == 271) return (id_bowq >= 0 && chk(item, id_bowq)) || (id_xboq >= 0 && chk(item, id_xboq));
    return 0;
}
