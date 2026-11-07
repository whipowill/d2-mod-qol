// Byte signatures for the two D2Net functions we hook. D2Net's code is identical across PD2 and stock LoD 1.13, but
// the export ORDINALS differ between builds (PD2: send=#10024 recv=#10008; stock LoD: send=#10015 recv=#10005),
// so we find the functions by their code instead of by ordinal. Pure functions, unit-tested natively (test/sigtest.c).
#ifndef AP_SIG_H
#define AP_SIG_H
#include <string.h>

static unsigned sig_u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

// SendPacket(size, arg, packet), stdcall ret 0xc:   mov eax,[abs]; test eax,eax; jne +5; xor eax,eax; ret 0xc
static int sig_is_send(const unsigned char *p) {
    static const unsigned char tail[] = {0x85, 0xC0, 0x75, 0x05, 0x33, 0xC0, 0xC2, 0x0C, 0x00};
    return p[0] == 0xA1 && !memcmp(p + 5, tail, sizeof tail);
}

// ReceivePacket(buf, cap), stdcall ret 8 (server->client queue):
//   mov eax,[esp+4]; push eax; mov eax,[esp+0xc]; push <queue>; push <queue+4>; call ...; ret 8
// A sibling function with the same shape serves the other queue (<queue> = <queue+4>-12); ours is queue+4 - 4.
static int sig_is_recv(const unsigned char *p) {
    static const unsigned char head[] = {0x8B, 0x44, 0x24, 0x04, 0x50, 0x8B, 0x44, 0x24, 0x0C, 0x68};
    if (memcmp(p, head, sizeof head) || p[14] != 0x68 || p[19] != 0xE8 || p[24] != 0xC2 || p[25] != 0x08) return 0;
    return sig_u32(p + 10) + 4 == sig_u32(p + 15);
}

// D2Common "set unit mode" (SetMode(unit, mode), stdcall ret 8): the same 24 code bytes in PD2 and stock LoD, at different
// ordinals (LoD #10193, PD2 #11090). Only the two jump offsets are wildcards.
static int sig_is_setmode(const unsigned char *p) {
    static const unsigned char a[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6, 0x74};
    static const unsigned char b[] = {0x8B, 0x0E, 0x83, 0xF9, 0x05, 0x74};
    static const unsigned char c[] = {0x8B, 0x46, 0x10, 0x57, 0x8B, 0x7C, 0x24, 0x10};
    return !memcmp(p, a, sizeof a) && !memcmp(p + 9, b, sizeof b) && !memcmp(p + 16, c, sizeof c);
}

// The level-difference XP scale (D2Game): `cmp eax,100h ; je full ; mov ecx,100h ; jmp muldiv` followed by `mov eax,edx ; ret`.
// Making the `je` a `jmp` skips the whole scale (no XP penalty for level gaps, in either direction). Found by pattern, so it works
// on any build that has this code (stock 1.13c, and PD2's own D2Game at a different address). Returns the offset of the
// 74/EB byte if the pattern occurs exactly once in buf, else -1. *patched is set to 1 if that byte is already EB.
static long sig_find_xp_penalty(const unsigned char *buf, size_t n, int *patched) {
    long found = -1; int count = 0;
    for (size_t i = 0; i + 20 <= n; i++) {
        const unsigned char *p = buf + i;
        if (p[0] == 0x3D && p[1] == 0x00 && p[2] == 0x01 && p[3] == 0x00 && p[4] == 0x00 && (p[5] == 0x74 || p[5] == 0xEB) && p[6] == 0x0A &&
            p[7] == 0xB9 && p[8] == 0x00 && p[9] == 0x01 && p[10] == 0x00 && p[11] == 0x00 && p[12] == 0xE9 && p[17] == 0x8B && p[18] == 0xC2 && p[19] == 0xC3) {
            found = (long)(i + 5); count++; if (patched) *patched = p[5] == 0xEB;
        }
    }
    return count == 1 ? found : -1;
}

// "KeepEquip" (SnakeByteStudios): in the player-creation function, `mov edx,[ebp+0C8h]` (6 bytes) is replaced by `jmp <epilogue> ; nop`,
// which skips the rest of the function. The 45-byte run below is that spot in stock D2Game.dll (the first 6 bytes may already be the jmp).
// *epilogue receives the offset of the function's epilogue (`pop edi ; pop esi ; mov eax,ebp ; pop ebp ; pop ebx ; add esp,18h ; ret 14h`)
// found after the site. Returns the offset of the site if it occurs exactly once, else -1. *patched = 1 if the jmp is already there.
static long sig_find_keep_equip(const unsigned char *buf, size_t n, int *patched, long *epilogue) {
    static const unsigned char tail[] = {0x8B,0x8D,0xC4,0x00,0x00,0x00, 0x83,0xC9,0x02, 0x81,0xCA,0x00,0x10,0x00,0x00, 0x89,0x95,0xC8,0x00,0x00,0x00,
                                         0x89,0x8D,0xC4,0x00,0x00,0x00, 0x8B,0x8B,0x08,0x05,0x00,0x00, 0x81,0xF9,0x00,0x00,0x10,0x00};
    static const unsigned char head[] = {0x8B,0x95,0xC8,0x00,0x00,0x00};
    static const unsigned char epi[] = {0x5F,0x5E,0x8B,0xC5,0x5D,0x5B,0x83,0xC4,0x18,0xC2,0x14,0x00};
    long found = -1; int count = 0;
    for (size_t i = 0; i + 45 <= n; i++) {
        const unsigned char *p = buf + i;
        int is_orig = !memcmp(p, head, sizeof head), is_jmp = p[0] == 0xE9 && p[5] == 0x90;
        if ((is_orig || is_jmp) && !memcmp(p + 6, tail, sizeof tail)) { found = (long)i; count++; if (patched) *patched = is_jmp; }
    }
    if (count != 1) return -1;
    if (epilogue) {
        *epilogue = -1;
        for (size_t j = (size_t)found + 45; j + sizeof epi <= n && j < (size_t)found + 0x600; j++)
            if (!memcmp(buf + j, epi, sizeof epi)) { *epilogue = (long)j; break; }
    }
    return found;
}

// ---- mercenary equipment rule: where to patch ---------------------------------------------------------------------------
// Both D2Game.dll (server) and D2Client.dll (interface) have a function that decides whether a merc may use an item: it first
// sets an "armor" flag (body armor / helm), then dispatches on the merc's class number to hard-coded item-type checks. The
// dispatch is what we replace. Found by byte patterns (-1 = any byte). Offsets are relative to the start of the buffer.
typedef struct { long site, not_allowed, allowed, chk_call, two_call, cls_call; } merc_site_t;

__attribute__((unused)) static long qt_find(const unsigned char *buf, size_t n, size_t from, size_t to, const short *pat, int len, int *count) {
    long found = -1; int c = 0;
    if (to > n) to = n;
    for (size_t i = from; i + (size_t)len <= to; i++) {
        int ok = 1;
        for (int k = 0; k < len && ok; k++) if (pat[k] >= 0 && buf[i + (size_t)k] != (unsigned char)pat[k]) ok = 0;
        if (ok) { if (found < 0) found = (long)i; c++; }
    }
    if (count) *count = c;
    return found;
}

// server (D2Game): after `push 3 / push esi / call chk` ... `mov ebx,1` the dispatch starts at `test edi,edi` (the site).
__attribute__((unused)) static int sig_find_merc_server(const unsigned char *buf, size_t n, merc_site_t *o) {
    static const short head[] = {0x6A,0x03,0x56,0x33,0xDB,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x75,0x0C, 0x6A,0x25,0x56,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x74,0x05,
                                 0xBB,0x01,0x00,0x00,0x00, 0x85,0xFF,0x75,0x1F, 0x83,0xC8,0xFF,0x2D,0x30,0x02,0x00,0x00, 0x74,-1,0x48,0x0F,0x85,-1,-1,-1,-1};
    static const short tail[] = {0x85,0xDB,0x74,-1, 0x6A,0x00,0x6A,0x00,0x6A,0x00,0x6A,0x00, 0x57,0x56,0xE8};   // test ebx,ebx / je reject / allowed: push 0 x4...
    static const short two[]  = {0x6A,0x1C,0x56,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x74,-1, 0x56,0xE8};                // axe check, then the two-handed check
    int c = 0; long h = qt_find(buf, n, 0, n, head, (int)(sizeof head / sizeof *head), &c);
    if (h < 0 || c != 1) return 0;
    long t = qt_find(buf, n, (size_t)h, (size_t)h + 0x140, tail, (int)(sizeof tail / sizeof *tail), 0);
    long w = qt_find(buf, n, (size_t)h, (size_t)h + 0x140, two, (int)(sizeof two / sizeof *two), 0);
    if (t < 0 || w < 0) return 0;
    o->site = h + 31; o->chk_call = h + 5; o->cls_call = -1;
    o->not_allowed = t; o->allowed = t + 4; o->two_call = w + 13;
    return 1;
}

// client (D2Client): same idea; the dispatch starts at `test ebp,ebp` (the site) and the class comes from a call.
__attribute__((unused)) static int sig_find_merc_client(const unsigned char *buf, size_t n, merc_site_t *o) {
    static const short head[] = {0x6A,0x03,0x53,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x75,0x0C, 0x6A,0x25,0x53,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x74,0x07,
                                 0xBE,0x01,0x00,0x00,0x00, 0x89,0x37, 0x85,0xED,0x75,0x05,0x83,0xC9,0xFF,0xEB,0x03,0x8B,0x4D,0x0C, 0xE8,-1,-1,-1,-1,
                                 0x3D,0x67,0x01,0x00,0x00};
    static const short notal[] = {0x85,0xF6,0x74,-1, 0x6A,0x00,0x6A,0x00,0x6A,0x00,0x6A,0x00, 0x55,0x53,0xE8};   // test esi,esi / je / push 0 x4 ...
    static const short allow[] = {0xBE,0x01,0x00,0x00,0x00, 0x89,0x37, 0xEB,-1, 0xCC};                           // mov esi,1 / mov [edi],esi / jmp
    static const short two[]   = {0x6A,0x1C,0x53,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x74,-1, 0x53,0xE8};
    int c = 0; long h = qt_find(buf, n, 0, n, head, (int)(sizeof head / sizeof *head), &c);
    if (h < 0 || c != 1) return 0;
    long a = qt_find(buf, n, (size_t)h, (size_t)h + 0x140, notal, (int)(sizeof notal / sizeof *notal), 0);
    long b = qt_find(buf, n, (size_t)h, (size_t)h + 0x140, allow, (int)(sizeof allow / sizeof *allow), 0);
    long w = qt_find(buf, n, (size_t)h, (size_t)h + 0x140, two, (int)(sizeof two / sizeof *two), 0);
    if (a < 0 || b < 0 || w < 0) return 0;
    o->site = h + 31; o->chk_call = h + 3; o->cls_call = h + 43;
    o->not_allowed = a; o->allowed = b; o->two_call = w + 13;
    return 1;
}

// D2Common's requirement check (#11072, called by both the server and the client rule): for a class-restricted item it refuses
// unless the unit is a Barbarian merc with a Barbarian item. Site = `cmp ebx,4 ; jne` right after the unit-type test.
typedef struct { long site, type_call, allowed, not_barb, cont; } merc_cls_t;
__attribute__((unused)) static int sig_find_merc_class(const unsigned char *buf, size_t n, merc_cls_t *o) {
    static const short pat[] = {0x57,0xE8,-1,-1,-1,-1, 0x83,0xF8,0x12, 0x75,-1, 0x6A,0x00,0x6A,0x46,0x57,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x7E,-1,
                                0x57,0xE8,-1,-1,-1,-1, 0x8B,0xD8, 0x83,0xFB,0x07, 0x74,-1, 0x85,0xED, 0x74,-1, 0x8B,0x75,0x00, 0x83,0xFE,0x01, 0x75,-1,
                                0x83,0xFB,0x04, 0x75,-1, 0x8B,0x45,0x04, 0xE8,-1,-1,-1,-1, 0x3D,0x30,0x02,0x00,0x00, 0x7C,-1, 0x3D,0x31,0x02,0x00,0x00,
                                0x7E,-1, 0x85,0xF6, 0x75,-1, 0x3B,0x5D,0x04, 0x75,-1, 0x5E,0x5B,0x5F,0xB8,0x01,0x00,0x00,0x00};
    int c = 0; long h = qt_find(buf, n, 0, n, pat, (int)(sizeof pat / sizeof *pat), &c);
    if (h < 0 || c != 1) return 0;
    o->type_call = h + 1; o->site = h + 50; o->cont = h + 55; o->not_barb = h + 77; o->allowed = h + 86;
    return 1;
}

// D2Game server equip action: picks ONE body location for the item; for the Iron Wolf's shield it uses the second location.
// Site = `cmp [esi+4],167h ; jne` (the Iron Wolf test).
typedef struct { long site, chk_call, use2, use1; } merc_slot_t;
__attribute__((unused)) static int sig_find_merc_slot(const unsigned char *buf, size_t n, merc_slot_t *o) {
    static const short pat[] = {0x81,0x7E,0x04,0x67,0x01,0x00,0x00, 0x75,0x1F, 0x8B,0x44,0x24,0x30, 0x6A,0x02, 0x50, 0xE8,-1,-1,-1,-1, 0x85,0xC0,
                                0x74,0x0F, 0x0F,0xB6,0x4C,0x24,0x24, 0x8B,0x7C,0x24,0x14, 0x89,0x4C,0x24,0x14, 0xEB,0x09,
                                0x0F,0xB6,0x54,0x24,0x13, 0x89,0x54,0x24,0x14, 0x85,0xFF, 0x0F,0x84};
    int c = 0; long h = qt_find(buf, n, 0, n, pat, (int)(sizeof pat / sizeof *pat), &c);
    if (h < 0 || c != 1) return 0;
    o->site = h; o->chk_call = h + 16; o->use2 = h + 25; o->use1 = h + 40;
    return 1;
}

// D2Game skill-start function shared by Whirlwind-type skills (srvstfunc 38): if the caster's target is already within melee reach (`#11023 InRange`, result in eax)
// the skill is cancelled and the caster goes back to neutral (it then attacks normally). The site is `test eax,eax ; je continue ; push edi` (85 C0 74 1C 57), which
// we replace with a jump. Returns the offset of the `test eax,eax` if the pattern occurs exactly once (patched = 1 if it is already our `jmp`), else -1.
__attribute__((unused)) static long sig_find_merc_special(const unsigned char *buf, size_t n, int *patched) {
    static const short orig[]   = {0x6A,0x00,0x53,0x56,0xE8,-1,-1,-1,-1, 0x85,0xC0,0x74,0x1C,0x57, 0xE8,-1,-1,-1,-1, 0x53,0x8B,0x5C,0x24,0x14,0x8B,0xC6,0xE8,-1,-1,-1,-1, 0x5B,0x5F,0x5E,0x5D,0x83,0xC4,0x38,0xC2,0x08,0x00};
    static const short jumped[] = {0x6A,0x00,0x53,0x56,0xE8,-1,-1,-1,-1, 0xE9,-1,-1,-1,-1, 0xE8,-1,-1,-1,-1, 0x53,0x8B,0x5C,0x24,0x14,0x8B,0xC6,0xE8,-1,-1,-1,-1, 0x5B,0x5F,0x5E,0x5D,0x83,0xC4,0x38,0xC2,0x08,0x00};
    int c1 = 0, c2 = 0;
    long a = qt_find(buf, n, 0, n, orig, (int)(sizeof orig / sizeof *orig), &c1), b = qt_find(buf, n, 0, n, jumped, (int)(sizeof jumped / sizeof *jumped), &c2);
    if (c1 + c2 != 1) return -1;
    if (patched) *patched = c2 == 1;
    return (c1 ? a : b) + 9;
}
// The collision mask the Whirlwind-style skill start gives the mover's path: players get 0x1c09, everything else 0x3c01 (which also blocks on other units). Returns the offset of the
// `add eax,0x3c01` (5 bytes) or its already-patched jmp form; -1 unless exactly one match.
__attribute__((unused)) static long sig_find_merc_mask(const unsigned char *buf, size_t n, int *patched) {
    static const short orig[]   = {0x8B,0xC7,0xF7,0xD8,0x1B,0xC0,0x25,0x08,0xE0,0xFF,0xFF,0x68,0x01,0x0C,0x00,0x00, 0x05,0x01,0x3C,0x00,0x00, 0x53};
    static const short jumped[] = {0x8B,0xC7,0xF7,0xD8,0x1B,0xC0,0x25,0x08,0xE0,0xFF,0xFF,0x68,0x01,0x0C,0x00,0x00, 0xE9,-1,-1,-1,-1, 0x53};
    int c1 = 0, c2 = 0;
    long a = qt_find(buf, n, 0, n, orig, (int)(sizeof orig / sizeof *orig), &c1), b = qt_find(buf, n, 0, n, jumped, (int)(sizeof jumped / sizeof *jumped), &c2);
    if (c1 + c2 != 1) return -1;
    if (patched) *patched = c2 == 1;
    return (c1 ? a : b) + 16;
}
#endif
