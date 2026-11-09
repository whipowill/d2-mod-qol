// core.c - potion auto-pickup logic. See NOTES.md section 6 for the protocol notes this is built on.
//
// Packet facts (verified against real single-player captures (PD2; LoD 1.13c uses the same format)):
//   server->client 0x9c / 0x9d : item action. 0x9c bit data starts at byte 8, 0x9d at byte 13 (owner header).
//       bits (LSB first): flags:32 version:10 mode:3, then
//         mode==3 (ground): x:16 y:16
//         else            : body:4 px:4 py:4 page:3
//       then item code as 4 x 8-bit ASCII chars ("hp1 ").
//       mode: 0 stored, 1 equipped, 2 belt, 3 ground, 4 cursor, 5 dropping, 6 socketed
//       action 14 = add to belt (px = belt slot), 15 = remove from belt, 0 = item appears on ground.
//   server->client 0x15 : reassign player  [15 type id(4) x(2) y(2) ...]
//   server->client 0x0d : player stop      [0d type id(4) ? x(2) y(2) ...]
//   server->client 0x0a : remove unit      [0a type id(4)]  (type 4 = item)
//   client->server 0x01/0x03 : walk/run to location [id x(2) y(2)]
//   client->server 0x16 : pick up item     [16 04 00 00 00 id(4) 00 00 00 00]  (13 bytes)
// A potion picked up with a free belt slot goes straight into the belt (server answers with action 14).
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tables.h"

#define MAX_GROUND 64
#define MAX_BELT 16
#define MAX_KSTACK 8
#define RETRY_MS 700
#define TICK_MS 50

typedef struct { uint32_t id; int x, y; int cls, kind, tier; char code[5]; int tries; uint32_t last_try; int try_dist; int qty; } ground_t;
typedef struct { uint32_t id; int qty; int full; uint32_t hash; } kstack_t;   // a key stack in the inventory (qty -1 = unknown)
// Stack bookkeeping per kind (0 key, 1 identify tome, 2 town-portal tome) - nothing here comes from the game's data files.
static int sk_trusted[3] = {1, 1, 1};   // decoded quantities are used until one contradicts a pickup of ours (then ignored until one matches)
static int sk_cap[3];         // learned max stack size (0 = unknown). Survives new games, not game restarts.
static int sk_seen[3];        // largest quantity ever decoded for this kind (a lower bound for the max)
static struct { uint32_t sid; int qty, base, sk; uint32_t t; } sk_expect;   // quantity we expect to see next after a pickup
typedef struct { uint32_t id; int kind, tier; char code[5]; } belt_t;

static ap_config cfg;
static ap_logfn logfn;
static ground_t ground[MAX_GROUND];
static int in_town;                      // player is in a town: no pickups
static belt_t belt[MAX_BELT];
static int belt_cap = 4;              // until we see the equipped belt
static int last_qty = -1;             // quantity decoded from the most recent stackable item packet (tests)
static int belt_miss;                 // consecutive potions that should have fitted the belt but landed outside it
static uint32_t cursor_t;             // when the cursor item was last announced
static int pend_fit_cnt;              // largest belt-column fill among the columns that looked like they had room, for the last potion we sent
static int pot_pend_fitted;           // ... and whether the belt model said the potion would fit
static uint32_t belt_item_id;         // id of equipped belt (0 = none seen)
static uint32_t cursor_id;            // item currently on the cursor (0 = empty)
static uint32_t player_id;
static int px, py, have_pos;
static uint32_t last_tick, last_pick;
static uint32_t tome_id[2];            // ids of the identify / town-portal tomes in the inventory (0 = none)
static int tome_full[2];               // learned: last scroll of this kind landed outside the tome
static int tome_qty[2] = {-1, -1};     // decoded scroll count in the tome (-1 = unknown)
static uint32_t tome_hash[2];          // hash of the tome's last item packet: a change means its contents changed
static uint32_t now_ms;                // last clock value seen by ap_tick (the host polls it every frame)
static uint32_t scroll_gone_id; static int scroll_gone_kind, scroll_gone_applied; static uint32_t scroll_gone_t;   // scroll that vanished after a pickup
static kstack_t kst[MAX_KSTACK];
static uint32_t key_pend_id; static int key_pend_qty;                       // key we just tried to pick up
static uint32_t key_gone_id, key_gone_sid; static int key_gone_qty; static uint32_t key_gone_t;   // sid = stack our local guess was added to  // key that vanished after a pickup
static uint32_t pot_pend_id;           // potion we just tried to pick up (to report where it ended up)
static uint32_t scroll_pend_id; static int scroll_pend_kind;   // scroll we just tried to pick up

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void say(const char *fmt, ...) {
    if (!logfn) return;
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); logfn(b);
}

// ---- item classification --------------------------------------------------------------------------
// cls 1 = potion (kind 0 health, 1 mana, 2 rejuv, 3 stamina; tier: higher is better)
// cls 2 = scroll (kind 0 identify -> tome ibk, 1 town portal -> tome tbk)
// cls 3 = key (goes into an existing key stack in the inventory)
// cls 4 = gold (added straight to your gold, never touches the cursor)
static int classify(const char *c, int *cls, int *kind, int *tier) {
    *tier = 1;
    if (c[0] == 'h' && c[1] == 'p' && c[2] >= '1' && c[2] <= '5' && !c[3]) { *cls = 1; *kind = 0; *tier = c[2] - '0'; return 1; }
    if (c[0] == 'm' && c[1] == 'p' && c[2] >= '1' && c[2] <= '5' && !c[3]) { *cls = 1; *kind = 1; *tier = c[2] - '0'; return 1; }
    if (!strcmp(c, "rvs")) { *cls = 1; *kind = 2; *tier = 1; return 1; }
    if (!strcmp(c, "rvl")) { *cls = 1; *kind = 2; *tier = 2; return 1; }
    if (!strcmp(c, "vps")) { *cls = 1; *kind = 3; return 1; }
    if (!strcmp(c, "gld")) { *cls = 4; *kind = 0; return 1; }
    if (!strcmp(c, "key")) { *cls = 3; *kind = 0; return 1; }
    if (!strcmp(c, "isc")) { *cls = 2; *kind = 0; return 1; }
    if (!strcmp(c, "tsc")) { *cls = 2; *kind = 1; return 1; }
    return 0;
}

static int belt_slots_for(const char *code) {
    for (int i = 0; BELT_TABLE[i].code; i++) if (!strcmp(BELT_TABLE[i].code, code)) return BELT_TABLE[i].slots;
    return -1;
}

// ---- bit reader ------------------------------------------------------------------------------------
typedef struct { const uint8_t *b; int nbits, pos; } br_t;
static uint32_t bits(br_t *r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, r->pos++) if (r->pos < r->nbits) v |= (uint32_t)((r->b[r->pos >> 3] >> (r->pos & 7)) & 1) << i;
    return v;
}

static int stack_kind(const char *code) { return !strcmp(code, "key") ? 0 : !strcmp(code, "ibk") ? 1 : !strcmp(code, "tbk") ? 2 : -1; }
// Max stack size for a kind: what we have learned while playing, 0 = not known yet.
static int stack_cap(int sk) { return sk_cap[sk]; }

typedef struct { int act, mode, body, px, py, page, x, y, ear, qty; uint32_t id; char code[5]; } item_t;

// Quantity of a stackable item (keys, tomes), from the extended data after the item code. Layouts seen in real packets:
//   PD2 (version 105): socketed:3 ilvl:7 quality:4 pic:1(+3) class:1(+11) <quality data> [runeword:16] [tome spell:5] qty:9 flag:1 0x1FF
//   LoD (version 101): the same but WITHOUT the flag bit:                                                  qty:9        0x1FF
//   classic d2s order (32-bit item id after the socket count, timestamp bit before the quantity): tried last, unseen so far.
// (Verified: PD2 tome 80 / 75, key stacks 50 / 2; modded LoD tomes 48, key stack 23.) A layout is only accepted if the 0x1FF
// property terminator sits exactly where it predicts; otherwise -1 and callers fall back to learning from outcomes.
static int parse_qty_layout(br_t r, uint32_t flags, const char *code, int layout) {
    bits(&r, 3);
    if (layout == 2) bits(&r, 32);
    bits(&r, 7);
    if (bits(&r, 4) != 2) return -1;                             // normal quality only
    if (bits(&r, 1)) bits(&r, 3);
    if (bits(&r, 1)) bits(&r, 11);
    if (flags & (1u << 26)) bits(&r, 16);
    if (flags & (1u << 24)) return -1;                           // personalized
    if (!strcmp(code, "ibk") || !strcmp(code, "tbk")) bits(&r, 5);
    int q;
    if (layout == 2) { bits(&r, 1); q = (int)bits(&r, 9); }
    else { q = (int)bits(&r, 9); if (layout == 0) bits(&r, 1); }
    if (flags & (1u << 11)) bits(&r, 4);                         // socketed
    if (bits(&r, 9) != 0x1FF || r.pos > r.nbits) return -1;
    return q >= 1 ? q : -1;
}
static int parse_qty(const br_t *r, uint32_t flags, const char *code) {
    if (stack_kind(code) < 0 || (flags & 0x00200000)) return -1;   // not a stackable we care about / "simple" item
    for (int layout = 0; layout < 3; layout++) {
        int q = parse_qty_layout(*r, flags, code, layout);
        if (q > 0) return q;
    }
    return -1;
}

static int parse_item(const uint8_t *p, int len, item_t *it) {
    int start = p[0] == 0x9d ? 13 : 8;
    if (len < start + 8) return 0;
    memset(it, 0, sizeof *it);
    it->act = p[1];
    it->id = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
    br_t r = {p + start, (len - start) * 8, 0};
    uint32_t flags = bits(&r, 32); bits(&r, 10);
    it->mode = (int)bits(&r, 3);
    if (flags & 0x10000) { it->ear = 1; return 1; }
    if (it->mode == 3) { it->x = (int)bits(&r, 16); it->y = (int)bits(&r, 16); }
    else { it->body = (int)bits(&r, 4); it->px = (int)bits(&r, 4); it->py = (int)bits(&r, 4); it->page = (int)bits(&r, 3); }
    for (int i = 0; i < 4; i++) { int c = (int)bits(&r, 8); it->code[i] = (c >= 32 && c < 127) ? (char)c : '?'; }
    it->code[4] = 0;
    for (int i = 3; i >= 0 && it->code[i] == ' '; i--) it->code[i] = 0;
    it->qty = parse_qty(&r, flags, it->code);
    if (stack_kind(it->code) >= 0) last_qty = it->qty;
    return 1;
}

// ---- state tables ----------------------------------------------------------------------------------
static void ground_remove(uint32_t id) { for (int i = 0; i < MAX_GROUND; i++) if (ground[i].id == id) ground[i].id = 0; }
static void belt_remove(uint32_t id) { for (int i = 0; i < MAX_BELT; i++) if (belt[i].id == id) belt[i].id = 0; }
static void kstack_remove(uint32_t id) { for (int i = 0; i < MAX_KSTACK; i++) if (kst[i].id == id) kst[i].id = 0; }

static uint32_t fnv(const uint8_t *p, int n);
// Hash of the item's bit data only. The 8-byte header carries the action code, which differs between announcements of
// the same unchanged item (add vs update ...), so hashing the whole packet made "changed" fire spuriously.
static uint32_t fnv_item(const uint8_t *p, int len) { int st = p[0] == 0x9d ? 13 : 8; return len > st ? fnv(p + st, len - st) : 0; }
static uint32_t fnv(const uint8_t *p, int n) { uint32_t h = 2166136261u; while (n-- > 0) h = (h ^ *p++) * 16777619u; return h; }

// Decoded quantities drive decisions unless one of them contradicted a pickup of ours (sk_trusted).
static int qty_usable(int sk, int q) { return sk_trusted[sk] && q > 0; }

static void note_qty(int sk, int q, uint32_t sid) {
    if (q <= 0) return;
    if (q > sk_seen[sk]) sk_seen[sk] = q;
    if (sk_cap[sk] && q > sk_cap[sk]) sk_cap[sk] = q;            // the max can't be below what we have seen
    if (sk_expect.sid == sid && sk_expect.sk == sk && sk_expect.t && now_ms - sk_expect.t < 4000) {
        if (q == sk_expect.qty) {                                  // exactly what our pickup should have produced
            if (!sk_trusted[sk]) say("stack quantity matches our pickup again (kind %d, q=%d): trusting it", sk, q);
            sk_trusted[sk] = 1; sk_expect.t = 0;
        } else if (q < sk_expect.base || q > sk_expect.qty) {      // neither before nor after our pickup: the decode is off
            say("stack quantity %d != expected %d (kind %d): ignoring decoded quantities until one matches", q, sk_expect.qty, sk);
            sk_trusted[sk] = 0; sk_expect.t = 0;
        }                                                          // else: an announcement from before our last pickup landed
    }
}

// Does a ground key pile of gq keys fit into some inventory key stack?
static int key_fits(int gq) {
    int cap = stack_cap(0);
    for (int i = 0; i < MAX_KSTACK; i++) {
        if (!kst[i].id || kst[i].full) continue;
        if (qty_usable(0, kst[i].qty) && cap > 0 && kst[i].qty + gq > cap) continue;
        return 1;
    }
    return 0;
}
static int tome_has_room(int k) {
    if (!tome_id[k] || tome_full[k]) return 0;
    int cap = stack_cap(1 + k);
    return !(qty_usable(1 + k, tome_qty[k]) && cap > 0 && tome_qty[k] >= cap);
}
static void keys_mark_full(int gq) {
    int best = 0;
    for (int i = 0; i < MAX_KSTACK; i++) if (kst[i].id) { kst[i].full = 1; if (kst[i].qty > best) best = kst[i].qty; }
    // A single key bounced although every stack looked usable: the biggest stack is the cap.
    if (gq == 1 && qty_usable(0, best) && best >= sk_seen[0]) { sk_cap[0] = best; say("learned key stack max = %d", best); }
}
static void tome_mark_full(int k) {
    tome_full[k] = 1;
    if (qty_usable(1 + k, tome_qty[k]) && tome_qty[k] >= sk_seen[1 + k]) { sk_cap[1 + k] = tome_qty[k]; say("learned tome %d max = %d", k, tome_qty[k]); }
}

static void remove_everywhere(uint32_t id) {
    ground_remove(id); belt_remove(id); kstack_remove(id);
    if (cursor_id == id) cursor_id = 0;
    if (key_pend_id == id) {
        // Looks merged into a stack. If the server was refusing it comes back onto the ground (see on_item).
        key_gone_id = id; key_gone_qty = key_pend_qty; key_gone_t = now_ms; key_gone_sid = 0; key_pend_id = 0;
        for (int i = 0; i < MAX_KSTACK; i++) if (kst[i].id && kst[i].qty > 0 && !kst[i].full) {
            sk_expect.base = kst[i].qty; kst[i].qty += key_gone_qty; key_gone_sid = kst[i].id;
            sk_expect.sid = kst[i].id; sk_expect.qty = kst[i].qty; sk_expect.sk = 0; sk_expect.t = now_ms;
            break;
        }
        say("key vanished after pickup (merged unless it comes back)");
    }
    if (belt_item_id == id) { belt_item_id = 0; belt_cap = 4; say("belt unequipped; capacity=4"); }
    for (int k = 0; k < 2; k++) if (tome_id[k] == id) { tome_id[k] = 0; tome_full[k] = 0; say("tome %d gone", k); }
    if (scroll_pend_id == id) {
        // Looks absorbed by the tome. If the server was really refusing (tome full) it puts the scroll back on the
        // ground under the same id; on_item() catches that and marks the tome full.
        scroll_gone_id = id; scroll_gone_kind = scroll_pend_kind; scroll_gone_t = now_ms;
        tome_full[scroll_pend_kind] = 0; scroll_pend_id = 0;
        scroll_gone_applied = 0;
        if (tome_qty[scroll_gone_kind] > 0) {
            sk_expect.base = tome_qty[scroll_gone_kind]; tome_qty[scroll_gone_kind]++; scroll_gone_applied = 1;
            sk_expect.sid = tome_id[scroll_gone_kind]; sk_expect.qty = tome_qty[scroll_gone_kind]; sk_expect.sk = 1 + scroll_gone_kind; sk_expect.t = now_ms;
        } say("scroll vanished after pickup (absorbed by tome unless it comes back)");
    }
}

static void on_item(const uint8_t *p, int len) {
    item_t it;
    if (!parse_item(p, len, &it) || it.ear) return;
    if (cfg.verbose >= 2) say("item act=%d id=%u mode=%d code=%s px=%d x=%d y=%d", it.act, it.id, it.mode, it.code, it.px, it.x, it.y);
    int cls = 0, kind = 0, tier = 0, pot = classify(it.code, &cls, &kind, &tier);
    int is_potion = pot && cls == 1;

    // A scroll we tried to pick up that turns up as a separate item means the tome has no room.
    if (scroll_pend_id && it.id == scroll_pend_id && it.mode != 3) {
        tome_mark_full(scroll_pend_kind); scroll_pend_id = 0;
        say("scroll landed outside the tome -> tome %d treated as full", scroll_pend_kind);
    }
    // The "landed outside" announcement can arrive AFTER the remove-unit packet that made the item vanish from the ground
    // (seen in a real PD2 log: scroll "absorbed" but actually in the inventory). Same id, any mode but ground, soon after.
    if (scroll_gone_id && it.id == scroll_gone_id && it.mode != 3 && now_ms - scroll_gone_t < 8000) {
        if (scroll_gone_applied) tome_qty[scroll_gone_kind]--;   // undo our local guess
        sk_expect.t = 0;
        tome_mark_full(scroll_gone_kind); scroll_gone_id = 0;
        say("scroll %u showed up outside the tome (mode %d) after leaving the ground -> tome %d is full", it.id, it.mode, scroll_gone_kind);
    }
    if (key_gone_id && it.id == key_gone_id && it.mode != 3 && now_ms - key_gone_t < 8000) {
        for (int i = 0; i < MAX_KSTACK; i++) if (kst[i].id && kst[i].id == key_gone_sid) kst[i].qty -= key_gone_qty;   // undo our local guess
        sk_expect.t = 0;
        keys_mark_full(key_gone_qty); key_gone_id = 0;
        say("key %u showed up outside the stack (mode %d) after leaving the ground -> key stacks are full", it.id, it.mode);
    }
    // A key we tried to pick up that turns up as its own item (cursor / elsewhere) means the stack had no room.
    if (key_pend_id && it.id == key_pend_id && it.mode != 3) {
        keys_mark_full(key_pend_qty); key_pend_id = 0;
        say("key landed outside the stack (mode %d) -> key stacks treated as full", it.mode);
    }
    // A potion we picked up that did not end up in the belt: tell us (helps learn the server's placement rule).
    if (is_potion && it.id == pot_pend_id && it.mode != 3) {
        if (it.mode == 2) { belt_miss = 0; say("potion %s id=%u landed in belt slot %d", it.code, it.id, it.px); }
        else {
            say("WARNING: picked-up potion %s id=%u went to mode %d, not the belt", it.code, it.id, it.mode);
            // We thought the belt had room, the server disagreed: the belt is smaller than we assumed (wrong or missing
            // data for this belt). Shrink to the fullest column we saw.
            // Two misses in a row before believing it, so one odd event can't silently disable pickups.
            if (pot_pend_fitted && pend_fit_cnt >= 1 && pend_fit_cnt * 4 < belt_cap && ++belt_miss >= 2) {
                belt_cap = pend_fit_cnt * 4; belt_miss = 0; say("belt capacity corrected to %d slots", belt_cap);
            }
        }
        pot_pend_id = 0;
    }

    // The item moves: drop it from wherever we had it, then re-add per its new mode.
    if (it.id) {
        ground_remove(it.id); belt_remove(it.id); if (cursor_id == it.id) cursor_id = 0;
        if (!(it.mode == 0 && it.page == 1 && !strcmp(it.code, "key"))) kstack_remove(it.id);   // an inventory key stack keeps its learned state
    }

    if (cfg.verbose && stack_kind(it.code) >= 0) {           // raw bytes, to check/repair the quantity layout against real packets
            char hx[3 * 64 + 8]; int n = len < 64 ? len : 64;
            for (int i = 0; i < n; i++) sprintf(hx + 3 * i, "%02x ", p[i]);
            say("raw %s mode=%d page=%d qty=%d len=%d: %s", it.code, it.mode, it.page, it.qty, len, hx);
    }
    switch (it.mode) {
    case 3:   // on the ground
        if (it.id && it.id == scroll_gone_id && now_ms - scroll_gone_t < 5000) {
            if (scroll_gone_applied) tome_qty[scroll_gone_kind]--;   // undo our local guess
            sk_expect.t = 0;
            tome_mark_full(scroll_gone_kind); scroll_gone_id = 0;
            say("scroll %u came back onto the ground -> tome %d is full", it.id, scroll_gone_kind);
        }
        if (it.id && it.id == key_gone_id && now_ms - key_gone_t < 5000) {
            for (int i = 0; i < MAX_KSTACK; i++) if (kst[i].id && kst[i].id == key_gone_sid) kst[i].qty -= key_gone_qty;   // undo our local guess
            sk_expect.t = 0;
            keys_mark_full(key_gone_qty); key_gone_id = 0;
            say("key %u came back onto the ground -> key stacks are full", it.id);
        }
        if (pot && it.id) {
            for (int i = 0; i < MAX_GROUND; i++) if (!ground[i].id) {
                ground[i] = (ground_t){it.id, it.x, it.y, cls, kind, tier, {0}, 0, 0, 99, it.qty > 0 ? it.qty : 1};
                strcpy(ground[i].code, it.code);
                if (cfg.verbose) say("ground potion %s id=%u at (%d,%d)", it.code, it.id, it.x, it.y);
                break;
            }
        }
        break;
    case 2:   // belt. act 15 = removed from belt (packet still reports its old slot)
        if (it.act == 15) break;
        if (is_potion && it.px < MAX_BELT) {
            if (it.px >= belt_cap) { belt_cap = (it.px / 4 + 1) * 4; say("belt has a potion in slot %d: capacity raised to %d", it.px, belt_cap); }
            belt[it.px] = (belt_t){it.id, kind, tier, {0}};
            strcpy(belt[it.px].code, it.code);
            if (cfg.verbose) say("belt slot %d = %s (id=%u)", it.px, it.code, it.id);
        }
        break;
    case 4:   // cursor
        cursor_id = it.id; cursor_t = now_ms; break;
    case 0:   // stored (inventory/stash/cube): remember tomes
        if (!strcmp(it.code, "key") && it.page == 1) {          // inventory key stack
            uint32_t h = fnv_item(p, len); int k = -1, fresh = 0;
            for (int i = 0; i < MAX_KSTACK; i++) if (kst[i].id == it.id) k = i;
            for (int i = 0; k < 0 && i < MAX_KSTACK; i++) if (!kst[i].id) { k = i; fresh = 1; }
            if (k >= 0) {
                // New stack, or its contents changed -> room may exist. "Changed" = the decoded count differs; when the
                // count isn't known, the item's bit data differs. Plain re-announcements must not clear a learned "full".
                int changed = fresh || (it.qty > 0 && kst[k].qty > 0 ? it.qty != kst[k].qty : kst[k].hash != h);
                if (changed) kst[k].full = 0;
                kst[k].id = it.id; kst[k].qty = it.qty; kst[k].hash = h;
                note_qty(0, it.qty, it.id);
                say("key stack id=%u qty=%d (max %d%s)", it.id, it.qty, stack_cap(0), sk_trusted[0] ? "" : ", quantity ignored: mismatch");
            }
        }
        for (int k = 0; k < 2; k++) {
            if (strcmp(it.code, k ? "tbk" : "ibk")) continue;
            uint32_t h = fnv_item(p, len);
            // A new tome, or one whose contents changed (scrolls added/used), may have room again. A plain
            // re-announcement of the same tome (the action byte differs between announcements) must NOT clear a
            // learned "full" - that made the mod pick up a scroll every time.
            int changed = tome_id[k] != it.id || (it.qty > 0 && tome_qty[k] > 0 ? it.qty != tome_qty[k] : tome_hash[k] != h);
            if (changed) tome_full[k] = 0;
            tome_id[k] = it.id; tome_hash[k] = h; tome_qty[k] = it.qty;
            note_qty(1 + k, it.qty, it.id);
            say("tome %s id=%u qty=%d (max %d%s)", it.code, it.id, it.qty, stack_cap(1 + k), sk_trusted[1 + k] ? "" : ", quantity ignored: mismatch");
        }
        break;
    case 1: { // equipped
        if (it.body == 8) {
            int n = belt_slots_for(it.code);
            // Unknown belt (not in tables.h, e.g. a mod's new belt): assume the biggest and correct downward the first
            // time a potion we pick up fails to land in the belt (see the WARNING branch above).
            belt_item_id = it.id; belt_cap = n > 0 ? n : MAX_BELT;
            say("belt equipped: %s -> %d slots%s", it.code, belt_cap, n > 0 ? "" : " (unknown belt: assuming the maximum, will self-correct)");
        }
        break; }
    default: break;
    }
}

// ---- public API ------------------------------------------------------------------------------------
// Forget what was learned about stack sizes (tests; a real game keeps it across new games).
void ap_reset_learned(void) { memset(sk_cap, 0, sizeof sk_cap); memset(sk_seen, 0, sizeof sk_seen); sk_trusted[0] = sk_trusted[1] = sk_trusted[2] = 1; }

int ap_parse_misc(const char *txt, int len) {
    int col_code = -1, col_max = -1, set = 0, line = 0;
    const char *end = txt + len;
    while (txt < end) {
        const char *eol = memchr(txt, '\n', (size_t)(end - txt));
        if (!eol) eol = end;
        const char *f = txt;
        char code[8] = {0};
        int idx = 0, maxstack = 0;
        for (const char *q = txt; q <= eol; q++) {
            if (q != eol && *q != '\t') continue;
            int fl = (int)(q - f);
            if (fl > 0 && f[fl - 1] == '\r') fl--;
            if (line == 0) {
                if (fl == 4 && !memcmp(f, "code", 4)) col_code = idx;
                if (fl == 8 && !memcmp(f, "maxstack", 8)) col_max = idx;
            } else if (idx == col_code) memcpy(code, f, (size_t)(fl < 7 ? fl : 7));
            else if (idx == col_max) maxstack = atoi(f);
            idx++; f = q + 1;
        }
        if (line == 0 && (col_code < 0 || col_max < 0)) return 0;   // not the table we expected
        int sk = line ? stack_kind(code) : -1;
        if (sk >= 0 && maxstack > 0) { sk_cap[sk] = maxstack; set++; say("Misc.txt: %s max stack = %d", code, maxstack); }
        line++; txt = eol + 1;
    }
    return set;
}

int ap_last_qty(void) { return last_qty; }

// MonStats.txt: the class id of a monster is its row number (header excluded). Hirelings are the rows whose AI is "Hireable".
int ap_parse_monstats(const char *txt, int len, int *ids, int max) {
    // Class id = the table's explicit "hcIdx" column when it has one; otherwise the row number, not counting "Expansion" divider rows
    // (the game gives those no id: every monster after the divider is one lower than its row number).
    int col_ai = -1, col_hc = -1, line = 0, n = 0, dividers = 0;
    const char *end = txt + len;
    while (txt < end) {
        const char *eol = memchr(txt, '\n', (size_t)(end - txt));
        if (!eol) eol = end;
        const char *f = txt;
        int idx = 0, is_hire = 0, divider = 0, hc = -1;
        for (const char *q = txt; q <= eol; q++) {
            if (q != eol && *q != '\t') continue;
            int fl = (int)(q - f);
            if (fl > 0 && f[fl - 1] == '\r') fl--;
            if (line == 0) {
                if (fl == 2 && !memcmp(f, "AI", 2)) col_ai = idx;
                if (fl == 5 && !memcmp(f, "hcIdx", 5)) col_hc = idx;
            } else {
                if (idx == 0 && fl == 9 && !memcmp(f, "Expansion", 9)) divider = 1;
                if (idx == col_ai && fl == 8 && !memcmp(f, "Hireable", 8)) is_hire = 1;
                if (idx == col_hc && fl > 0 && fl < 9) { int v = 0, ok = 1; for (int k = 0; k < fl; k++) { if (f[k] < '0' || f[k] > '9') { ok = 0; break; } v = v * 10 + (f[k] - '0'); } if (ok) hc = v; }
            }
            idx++; f = q + 1;
        }
        if (line == 0 && col_ai < 0) return 0;                       // not the table we expected
        if (line > 0 && divider) dividers++;
        else if (line > 0 && is_hire && n < max) ids[n++] = hc >= 0 ? hc : line - 1 - dividers;
        line++; txt = eol + 1;
    }
    return n;
}

void ap_init(const ap_config *c, ap_logfn l) {
    cfg = *c; logfn = l;
    memset(ground, 0, sizeof ground); memset(belt, 0, sizeof belt);
    belt_cap = 4; belt_item_id = cursor_id = player_id = 0; have_pos = 0; last_tick = last_pick = 0;
    belt_miss = 0; memset(tome_id, 0, sizeof tome_id); memset(tome_full, 0, sizeof tome_full); scroll_pend_id = pot_pend_id = scroll_gone_id = 0;
    memset(kst, 0, sizeof kst); key_pend_id = key_gone_id = 0; memset(&sk_expect, 0, sizeof sk_expect);
    tome_qty[0] = tome_qty[1] = -1; tome_hash[0] = tome_hash[1] = 0; scroll_gone_applied = 0;
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int u16(const uint8_t *p) { return p[0] | p[1] << 8; }

void ap_on_recv(const uint8_t *p, int len) {
    if (len < 1) return;
    switch (p[0]) {
    case 0x01:   // game flags (sent once when joining a game): forget everything from the previous game
        if (len == 8) { ap_config c = cfg; ap_logfn l = logfn; ap_init(&c, l); say("new game: state reset"); }
        break;
    case 0x9c: case 0x9d: on_item(p, len); break;
    case 0x15:   // reassign player
        if (len >= 10 && p[1] == 0) { player_id = u32(p + 2); px = u16(p + 6); py = u16(p + 8); have_pos = 1; }
        break;
    case 0x0d:   // player stop
        if (len >= 11 && p[1] == 0 && u32(p + 2) == player_id) { px = u16(p + 7); py = u16(p + 9); have_pos = 1; }
        break;
    case 0x0a:   // remove unit
        if (len >= 6 && p[1] == 4) remove_everywhere(u32(p + 2));
        break;
    default: break;
    }
}

void ap_on_send(const uint8_t *p, int len) {
    if (len >= 5 && (p[0] == 0x20 || p[0] == 0x26)) {
        uint32_t id = u32(p + 1);
        for (int k = 0; k < 2; k++) if (tome_id[k] && tome_id[k] == id) {
            // Using a scroll frees a slot, but the server doesn't re-announce the tome: our remembered count (possibly
            // "at max") is stale, so forget it - otherwise a tome that looked full never accepts scrolls again.
            tome_full[k] = 0; tome_qty[k] = -1; say("tome %d used: count unknown, room again", k);
        }
    }
    if (len >= 5 && (p[0] == 0x01 || p[0] == 0x03)) { px = u16(p + 1); py = u16(p + 3); have_pos = 1; }
}

// A potion is only worth picking up if a belt column already holds that kind, that column has a free row, and the
// potion is at least as good as the worst one of that kind in the column (otherwise the server would not put it in
// the belt, e.g. a mana potion with no mana column ends up in the inventory).
static int potion_fits(int kind, int tier) {
    int rows = belt_cap / 4; if (rows < 1) rows = 1;
    int fits = 0, fit_cnt = 0;
    for (int c = 0; c < 4; c++) {
        int cnt = 0, same = 0, worst = 99;
        for (int r = 0; r < rows; r++) {
            const belt_t *b = &belt[r * 4 + c];
            if (!b->id) continue;
            cnt++;
            if (b->kind == kind) { same++; if (b->tier < worst) worst = b->tier; }
        }
        if (same > 0 && cnt < rows && tier >= worst) { fits = 1; if (cnt > fit_cnt) fit_cnt = cnt; }
    }
    pend_fit_cnt = fit_cnt;
    return fits;
}

// Rejuvenation potions can be taken regardless of the belt (they go to the inventory if the belt can't hold them).
static int potion_wanted(int kind, int tier) { return (cfg.rejuv_always && kind == 2) || potion_fits(kind, tier); }
static int wanted(const ground_t *g) {
    if (g->cls == 1) return potion_wanted(g->kind, g->tier);
    if (g->cls == 3) return key_fits(g->qty);
    if (g->cls == 4) return cfg.gold;
    return tome_has_room(g->kind);
}

// Cheap test for the host: is a scan due at all? False while nothing is on the ground (the common case), so the host
// can skip reading the player's live position and the scan itself.
int ap_due(uint32_t now) {
    if (!cfg.enabled || now - last_tick < (uint32_t)(cfg.poll_ms > 0 ? (cfg.poll_ms < 20 ? 20 : cfg.poll_ms > 2000 ? 2000 : cfg.poll_ms) : TICK_MS)) return 0;
    for (int i = 0; i < MAX_GROUND; i++) if (ground[i].id) return 1;
    return 0;
}

int ap_tick(uint32_t now, uint8_t out[16]) {
    now_ms = now;
    if (!cfg.enabled || !have_pos || in_town || now - last_tick < (uint32_t)(cfg.poll_ms > 0 ? (cfg.poll_ms < 20 ? 20 : cfg.poll_ms > 2000 ? 2000 : cfg.poll_ms) : TICK_MS)) return 0;
    last_tick = now;
    // A cursor item we never saw leave would block every pickup for the rest of the game: expire it.
    if (cursor_id && now - cursor_t > 20000) { say("cursor item %u unchanged for 20 s: assuming it is gone", cursor_id); cursor_id = 0; }
    if (cursor_id) return 0;                                 // never risk putting something on the cursor
    int cooling = now - last_pick < 250;                     // gold can't land on the cursor, so it ignores the cooldown

    int best = -1, bestd = 1 << 30;
    for (int i = 0; i < MAX_GROUND; i++) {
        ground_t *g = &ground[i];
        if (!g->id) continue;
        int dx = abs(g->x - px), dy = abs(g->y - py), d = dx > dy ? dx : dy;
        if (d > cfg.range) continue;
        if (cooling && g->cls != 4) continue;
        if (!wanted(g)) continue;
        if (g->tries && d < g->try_dist - 1) g->tries = 0;   // player got closer since the last attempt: try again
        if (g->tries && now - g->last_try < RETRY_MS) continue;   // no give-up: keep retrying every RETRY_MS while in range
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return 0;
    ground_t *g = &ground[best];
    g->tries++; g->last_try = now; g->try_dist = bestd; last_pick = now;
    if (g->cls == 2) { scroll_pend_id = g->id; scroll_pend_kind = g->kind; }
    else if (g->cls == 3) { key_pend_id = g->id; key_pend_qty = g->qty; }
    else if (g->cls == 4) { /* gold: nothing to track */ }
    else { pot_pend_id = g->id; pot_pend_fitted = potion_fits(g->kind, g->tier); }
    memset(out, 0, 16);
    out[0] = 0x16; out[1] = 4;
    out[5] = (uint8_t)g->id; out[6] = (uint8_t)(g->id >> 8); out[7] = (uint8_t)(g->id >> 16); out[8] = (uint8_t)(g->id >> 24);
    say("PICKUP %s id=%u dist=%d try=%d", g->code, g->id, bestd, g->tries);
    return 13;
}

void ap_dump(char *buf, int n) {
    int used = 0; for (int i = 0; i < MAX_BELT; i++) if (belt[i].id) used++;
    int gcount = 0; for (int i = 0; i < MAX_GROUND; i++) if (ground[i].id) gcount++;
    snprintf(buf, n, "pos=(%d,%d)%s belt %d/%d cursor=%u ground_potions=%d", px, py, have_pos ? "" : "?", used, belt_cap, cursor_id, gcount);
}

int ap_cursor_busy(void) { return cursor_id != 0; }
uint32_t ap_cursor_id(void) { return cursor_id; }

void ap_set_town(int t) { in_town = t; }
void ap_set_pos(int x, int y) { px = x; py = y; have_pos = 1; }
int ap_player_pos(int *x, int *y) { if (!have_pos) return 0; if (x) *x = px; if (y) *y = py; return 1; }

int ap_parse_skills(const char *txt, int len, ap_summon_t *out, int max) {
    int c_id = -1, c_cls = -1, c_sum = -1, c_pet = -1, c_max = -1, c_p3 = -1, line = 0, n = 0;
    const char *end = txt + len;
    while (txt < end) {
        const char *eol = memchr(txt, '\n', (size_t)(end - txt));
        if (!eol) eol = end;
        const char *f = txt; int idx = 0;
        char id[16] = "", cls[16] = "", sum[40] = "", pet[16] = "", pmax[80] = "", p3[16] = "";
        for (const char *q = txt; q <= eol; q++) {
            if (q != eol && *q != '\t') continue;
            int fl = (int)(q - f);
            if (fl > 0 && f[fl - 1] == '\r') fl--;
            if (line == 0) {
                if (fl == 2 && !memcmp(f, "Id", 2)) c_id = idx;
                else if (fl == 9 && !memcmp(f, "charclass", 9)) c_cls = idx;
                else if (fl == 6 && !memcmp(f, "summon", 6)) c_sum = idx;
                else if (fl == 7 && !memcmp(f, "pettype", 7)) c_pet = idx;
                else if (fl == 6 && !memcmp(f, "petmax", 6)) c_max = idx;
                else if (fl == 6 && !memcmp(f, "Param3", 6)) c_p3 = idx;
            } else {
                char *dst = idx == c_id ? id : idx == c_cls ? cls : idx == c_sum ? sum : idx == c_pet ? pet : idx == c_max ? pmax : idx == c_p3 ? p3 : 0;
                int cap = idx == c_id ? 16 : idx == c_cls ? 16 : idx == c_sum ? 40 : idx == c_pet ? 16 : idx == c_max ? 80 : idx == c_p3 ? 16 : 0;
                if (dst) { int m = fl < cap - 1 ? fl : cap - 1; memcpy(dst, f, (size_t)m); dst[m] = 0; }
            }
            idx++; f = q + 1;
        }
        if (line == 0 && (c_id < 0 || c_cls < 0 || c_sum < 0)) return 0;                // not the table we expected
        if (line > 0 && n < max && !strcmp(cls, "dru") && sum[0] && id[0]) {
            out[n].id = atoi(id);
            snprintf(out[n].pet, sizeof out[n].pet, "%s", pet);
            snprintf(out[n].mon, sizeof out[n].mon, "%s", sum);
            int rep = 1; if (strstr(pmax, "par3") && p3[0]) { rep = atoi(p3); if (rep < 1) rep = 1; if (rep > 8) rep = 8; }
            out[n].repeat = rep; n++;
        }
        line++; txt = eol + 1;
    }
    return n;
}

int ap_monster_ids(const char *txt, int len, const char (*names)[24], int n, int *out) {
    int c_hc = -1, line = 0, found = 0, dividers = 0;
    const char *end = txt + len;
    for (int i = 0; i < n; i++) out[i] = -1;
    while (txt < end) {
        const char *eol = memchr(txt, '\n', (size_t)(end - txt));
        if (!eol) eol = end;
        const char *f = txt; int idx = 0; char id[40] = "", hc[16] = "";
        for (const char *q = txt; q <= eol; q++) {
            if (q != eol && *q != '\t') continue;
            int fl = (int)(q - f);
            if (fl > 0 && f[fl - 1] == '\r') fl--;
            if (line == 0) { if (idx == 0 && !(fl == 2 && !memcmp(f, "Id", 2))) return 0; if (fl == 5 && !memcmp(f, "hcIdx", 5)) c_hc = idx; }
            else if (idx == 0) { int m = fl < 39 ? fl : 39; memcpy(id, f, (size_t)m); id[m] = 0; }
            else if (idx == c_hc) { int m = fl < 15 ? fl : 15; memcpy(hc, f, (size_t)m); hc[m] = 0; }
            idx++; f = q + 1;
        }
        if (line > 0) {
            if (!strcmp(id, "Expansion")) dividers++;
            else for (int i = 0; i < n; i++) if (!strcmp(id, names[i]) && out[i] < 0) { out[i] = hc[0] ? atoi(hc) : line - 1 - dividers; found++; }
        }
        line++; txt = eol + 1;
    }
    return found;
}
