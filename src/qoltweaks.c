// qol.dll (built from qoltweaks) - Diablo II (1.13-style, e.g. LoD / Project Diablo 2) quality-of-life features and fixes (single player): potion/scroll/key
// auto-pickup, hold-to-click, mercenary running fix, no XP level penalty, keep equipment on death. Loaded via d2gl.json "load_dlls_late" or PlugY DllToLoad.
//
// Hooks the game's packet layer (D2Net), feeds every packet to the platform-independent logic in core.c,
// and when the logic decides a potion should be picked up, sends the same 0x16 packet a mouse click would.
//   - send hook:    inline detour of D2Net!#10024 (SendPacket(size, arg, packet))   -> sees your actions
//   - receive hook: IAT hook of D2Client's D2Net#10008 (server->client packets)     -> sees items/belt/position
// Config: qol.ini next to the game exe (created with defaults on first run).
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include "core.h"
#include "sig.h"
#include "merc.h"

static FILE *g_log;
static int g_merc_special = 0;                                   // ini: [mercenary] whirlwind
static int g_dclone = 0, g_dc_diff[3]; static double g_dc_chance = 0, g_dc_minutes = 10;   // ini: [dclone] enabled, roll_minutes, chance_per_roll (percent), normal/nightmare/hell
static int g_dc_announce = 1;                                      // ini: [dclone] announce
static volatile int g_dc_pending;                                  // 1 = the next super unique the game creates becomes Diablo Clone (read by the stub in D2Game)
static volatile int g_dc_class;                                    // MonStats class id of "diabloclone" (0 = not found yet)
static int g_respec = 0;                                         // ini: [respec] enabled
static int g_summons = 0;                                        // ini: [summons] enabled
static int g_glow_baal = 1;                                      // ini: [glow] hell_baal_required
static int g_glow_on = 0;                                        // ini: [glow] enabled
static int g_glow_min = 0;                                       // ini: [glow] min_level (0 = no level needed)
static volatile int g_glow_state = -1;                           // States.txt id of "fullsetgeneric" (the full-set glow), -1 = not read yet
static int g_merc_drop = 0;                                      // ini: [mercenary] drop_items
static int g_merc_equip = 0;                                    // ini: [mercenary] equip
#define D2GAME_TIMESTAMP_113D 1318970639u    // D2Game.dll of stock LoD 1.13d (Oct 2011)
static int g_logging = 0;        // ini: [qol] logging=0 turns off the log file, decision logging and profiling
static volatile LONG g_lines;
#define MAX_LINES 20000

static int g_pickup_in_town;                 // [pickup] pickup_in_town: 1 = auto-pickups also work in town
static void logf_(const char *fmt, ...) {
    if (!g_log || InterlockedIncrement(&g_lines) > MAX_LINES) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); fputc('\n', g_log);
    va_end(ap);
    // No fflush here: this runs on the game thread, and disk I/O spiked to ~4 ms. log_flusher() does it off-thread.
}
static DWORD WINAPI log_flusher(LPVOID arg) {
    (void)arg;
    for (;;) { Sleep(1000); if (g_log) fflush(g_log); }
    return 0;
}
static void core_log(const char *m) { logf_("%s", m); }

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

// ---- hooks ---------------------------------------------------------------------------------------------
typedef int (__stdcall *send_t)(int, int, void *);   // D2Net#10024: ret 0xc
typedef int (__stdcall *recv_t)(void *, int);        // D2Net#10008: ret 8
static send_t o_send; static recv_t o_recv;
static volatile int g_injecting;

// Self-profiling: how much CPU time do our hooks take? Logged every 15 s (verbose>=1).
static LARGE_INTEGER g_freq;
static struct { LONGLONG calls, total, max; } g_prof[2];     // [0] recv hook, [1] send hook
static DWORD g_prof_last;
static void prof_add(int i, LARGE_INTEGER t0) {
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    LONGLONG d = t1.QuadPart - t0.QuadPart;
    g_prof[i].calls++; g_prof[i].total += d; if (d > g_prof[i].max) g_prof[i].max = d;
}
static void prof_report(void) {
    DWORD now = GetTickCount();
    if (now - g_prof_last < 15000) return;
    g_prof_last = now;
    const char *n[2] = {"recv", "send"};
    for (int i = 0; i < 2; i++) if (g_prof[i].calls)
        logf_("PROFILE %s hook: %lld calls, avg %.2f us, max %.1f us, total %.1f ms", n[i], g_prof[i].calls,
              g_prof[i].total * 1e6 / g_freq.QuadPart / g_prof[i].calls, g_prof[i].max * 1e6 / (double)g_freq.QuadPart,
              g_prof[i].total * 1e3 / (double)g_freq.QuadPart);
}
// ---- max stack sizes from the game's own Misc.txt ---------------------------------------------------------
// Storm.dll is the file layer the game itself uses; with no archive handle it searches every archive the game has
// mounted, in the game's own priority order, so patches and mods are respected. Storm exports by ordinal only, so the
// four functions are identified by ordinal AND checked against their first bytes (verified on the PD2 and LoD
// Storm.dll); anything unexpected means we skip this and rely on learning the sizes while playing.
static int storm_is(HMODULE st, unsigned ord, const unsigned char *sig, int n, FARPROC *out) {
    FARPROC f = GetProcAddress(st, (LPCSTR)(UINT_PTR)ord);
    if (!f || !readable(f, (size_t)n) || memcmp((void *)f, sig, (size_t)n)) return 0;
    *out = f; return 1;
}
// With -direct the game reads its data from the loose Data\\ folder on disk instead of the MPQs (mods use this, e.g. a
// modded LoD launched with -direct -txt). Storm's own open function handles that: its search-scope argument with bit 1
// or 2 set tries the file on disk first (relative to the game folder), then falls back to the archives. Scope 0 =
// archives only. We use the same rule as the game: look at disk only when -direct is on the command line.
static DWORD storm_scope(void) {
    const char *cl = GetCommandLineA();
    char low[1024]; int i = 0;
    if (!cl) return 0;
    for (; cl[i] && i < (int)sizeof low - 1; i++) low[i] = (cl[i] >= 'A' && cl[i] <= 'Z') ? (char)(cl[i] + 32) : cl[i];
    low[i] = 0;
    return strstr(low, "-direct") ? 3 : 0;
}

// Reads a data file through Storm and hands its text to parse(). Returns parse()'s result, -1 if the file couldn't be read,
// -2 if Storm isn't the version we know (callers then fall back to learning / doing nothing).
static int storm_read(const char *name, int (*parse)(const char *, int)) {
    HMODULE st = GetModuleHandleA("Storm.dll");
    if (!st) return -1;
    static const unsigned char s_open[] = {0x8B, 0x4C, 0x24, 0x10, 0x81, 0xEC, 0x10, 0x01};   // #268 SFileOpenFileEx
    static const unsigned char s_size[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10};               // #265 SFileGetFileSize
    static const unsigned char s_read[] = {0x8B, 0x44, 0x24, 0x14, 0x8B, 0x4C, 0x24, 0x10};   // #269 SFileReadFile
    static const unsigned char s_close[] = {0x57};                                            // #253 SFileCloseFile
    FARPROC fo, fs, fr, fc;
    if (!storm_is(st, 268, s_open, sizeof s_open, &fo) || !storm_is(st, 265, s_size, sizeof s_size, &fs) ||
        !storm_is(st, 269, s_read, sizeof s_read, &fr) || !storm_is(st, 253, s_close, sizeof s_close, &fc)) {
        logf_("Storm.dll not recognised");
        return -2;
    }
    HANDLE h = NULL;
    DWORD scope = storm_scope();
    if (!((int (__stdcall *)(HANDLE, const char *, DWORD, HANDLE *))fo)(NULL, name, scope, &h) || !h) return -1;
    DWORD size = ((DWORD (__stdcall *)(HANDLE, DWORD *))fs)(h, NULL), got = 0;
    int n = -1;
    if (size > 0 && size < (16u << 20)) {
        char *buf = HeapAlloc(GetProcessHeap(), 0, size + 1);
        if (buf) {
            if (((int (__stdcall *)(HANDLE, void *, DWORD, DWORD *, void *))fr)(h, buf, size, &got, NULL) && got == size) n = parse(buf, (int)got);
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    ((int (__stdcall *)(HANDLE))fc)(h);
    if (n > 0) logf_("%s read through Storm (%s)", name, scope ? "-direct: loose Data folder first, then archives" : "archives");
    return n;
}

// ---- Terror Zone (step 1: the hourly pick and the announcement) ---------------------------------------------------------------------------
// Stateless like D2R's: the zone is a function of the real clock slot (unix time / rotate_minutes), so every game and every player with the same Levels.txt sees the same
// zone and nothing is saved. Candidates: the non-town areas of Levels.txt that have a waypoint (Waypoint column set), names from its LevelName column. Announced in purple
// with the client's PrintGameString (D2Client 1.13d +0x75EB0, as Quartermaster does) when a game is entered and whenever the slot changes. Runs on the client game thread.
static int g_tz_hell_only = 0, g_tz_xp = 0, g_tz_gold = 0, g_tz_mf = 0;           // ini: [terrorzone] hell_only, xp_pct, gold_pct
static int tz_active(void);                                             // defined with the server-side effects
static void tz_apply_level(void);
static int g_tz_lvl_bonus = 10, g_tz_lvl_min = 85, g_tz_lvl_max = 99;      // ini: level_bonus (every difficulty), hell_min_level, hell_max_level (Hell only)
#define TZ_MAX 160
static int g_tz = 0, g_tz_minutes = 60, g_tz_n, g_tz_wp_only = 0; static char g_tz_name[TZ_MAX][40]; static int g_tz_id[TZ_MAX];
static int g_tz_grp[TZ_MAX], g_tz_ngrp, g_tz_rep[TZ_MAX];                      // areas with the same name in the same act form ONE zone (Tal Rasha's seven tombs, a dungeon's twin levels): zone = group, rep = its first area
static int g_tz_act[TZ_MAX];
static int g_tz_den[TZ_MAX][3], g_tz_umin[TZ_MAX][3], g_tz_umax[TZ_MAX][3];     // the area's MonDen / MonUMin / MonUMax per difficulty, as Levels.txt says (to recognise the record layout in memory)
static int g_tz_dens_pct = 0, g_tz_elite_pct = 0;                    // ini: density_pct, elite_pct
static int parse_tz_cb(const char *txt, int len) {
    int n = 0, col_id = -1, col_wp = -1, col_nm = -1, col_act = -1, line = 0, c_den[3] = {-1, -1, -1}, c_umin[3] = {-1, -1, -1}, c_umax[3] = {-1, -1, -1};
    const char *p = txt, *end = txt + len;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)); if (!e) e = end;
        char buf[4096]; size_t ln = (size_t)(e - p); if (ln > sizeof buf - 1) ln = sizeof buf - 1; memcpy(buf, p, ln); buf[ln] = 0; if (ln && buf[ln - 1] == '\r') buf[ln - 1] = 0;
        char *f[320]; int nf = 0; char *c = buf; f[nf++] = c;
        for (; *c && nf < 320; c++) if (*c == '\t') { *c = 0; f[nf++] = c + 1; }
        if (line == 0) { for (int i = 0; i < nf; i++) { if (!strcmp(f[i], "Id")) col_id = i; else if (!strcmp(f[i], "Waypoint")) col_wp = i; else if (!strcmp(f[i], "LevelName")) col_nm = i; else if (!strcmp(f[i], "Act")) col_act = i;
                else if (!strcmp(f[i], "MonDen")) c_den[0] = i; else if (!strcmp(f[i], "MonDen(N)")) c_den[1] = i; else if (!strcmp(f[i], "MonDen(H)")) c_den[2] = i;
                else if (!strcmp(f[i], "MonUMin")) c_umin[0] = i; else if (!strcmp(f[i], "MonUMin(N)")) c_umin[1] = i; else if (!strcmp(f[i], "MonUMin(H)")) c_umin[2] = i;
                else if (!strcmp(f[i], "MonUMax")) c_umax[0] = i; else if (!strcmp(f[i], "MonUMax(N)")) c_umax[1] = i; else if (!strcmp(f[i], "MonUMax(H)")) c_umax[2] = i; }
            if (col_id < 0 || col_wp < 0 || col_nm < 0) return 0; }
        else if (nf > col_nm && nf > col_wp && nf > col_id && f[col_nm][0] && n < TZ_MAX && (g_tz_wp_only ? (f[col_wp][0] && strcmp(f[col_wp], "255")) : (c_den[2] >= 0 && nf > c_den[2] && atoi(f[c_den[2]]) > 0))) {   // any area with monsters in Hell (or only the waypoint areas)
            int id = atoi(f[col_id]);
            if (id == 1 || id == 40 || id == 75 || id == 103 || id == 109) { line++; p = e + 1; continue; }     // the five towns
            if (id == 37 || id == 73 || id == 102 || id == 108 || id == 132) { line++; p = e + 1; continue; }     // the act boss rooms: Andariel (Catacombs 4), Duriel's Lair, Mephisto (Durance 3), Chaos Sanctum, Worldstone Chamber
            g_tz_id[n] = id; snprintf(g_tz_name[n], sizeof g_tz_name[n], "%s", f[col_nm]);
            for (char *q = g_tz_name[n] + strlen(g_tz_name[n]); q > g_tz_name[n] && q[-1] == ' '; ) *--q = 0;       // "Palace Cellar Level 1 " has a trailing space
            for (int d = 0; d < 3; d++) { g_tz_den[n][d] = c_den[d] >= 0 && nf > c_den[d] ? atoi(f[c_den[d]]) : -1; g_tz_umin[n][d] = c_umin[d] >= 0 && nf > c_umin[d] ? atoi(f[c_umin[d]]) : -1; g_tz_umax[n][d] = c_umax[d] >= 0 && nf > c_umax[d] ? atoi(f[c_umax[d]]) : -1; }
            g_tz_act[n] = col_act >= 0 && nf > col_act ? atoi(f[col_act]) : 0;
            n++; }
        line++; p = e + 1;
    }
    int ng = 0;                                                                                      // zones: same LevelName and act
    for (int i = 0; i < n; i++) { int g = -1; for (int j = 0; j < i && g < 0; j++) if (g_tz_act[j] == g_tz_act[i] && !strcmp(g_tz_name[j], g_tz_name[i])) g = g_tz_grp[j];
        if (g < 0) { g = ng; g_tz_rep[ng++] = i; } g_tz_grp[i] = g; }
    g_tz_ngrp = ng; g_tz_n = n; return n;
}
static int parse_dc_cb(const char *txt, int len) {
    static const char names[1][24] = {"diabloclone"}; int id = -1;
    int n = ap_monster_ids(txt, len, names, 1, &id);
    if (n > 0 && id > 0) g_dc_class = id;
    return n;
}
// Class ids of the mercenaries (MonStats.txt rows with AI = Hireable); the merc running fix needs them.
static volatile int g_hire_n; static int g_hire_ids[16];
static int parse_monstats_cb(const char *txt, int len) {
    int ids[16], n = ap_parse_monstats(txt, len, ids, 16);
    for (int i = 0; i < n; i++) g_hire_ids[i] = ids[i];
    g_hire_n = n;
    return n;
}
static int parse_misc_cb(const char *txt, int len) { return ap_parse_misc(txt, len); }
static int parse_itemtypes_cb(const char *txt, int len) { return merc_parse_itemtypes(txt, len); }
// States.txt: the id column (2nd) of the row named "fullsetgeneric" (the glow a full set gives)
static int parse_states_cb(const char *txt, int len) {
    static const char key[] = "fullsetgeneric\t";
    for (int i = 0; i < len; ) {
        int e = i; while (e < len && txt[e] != '\n') e++;
        if (e - i > (int)sizeof key && !memcmp(txt + i, key, sizeof key - 1)) { int id = atoi(txt + i + sizeof key - 1); if (id > 0 && id < 256) { g_glow_state = id; return 1; } }
        i = e + 1;
    }
    return 0;
}
static ap_summon_t g_sum[24]; static volatile int g_sum_n;     // the druid's summon skills (Skills.txt)
static int parse_petmon_cb(const char *txt, int len);
static int parse_skills_cb(const char *txt, int len) {
    ap_summon_t tmp[24]; int n = ap_parse_skills(txt, len, tmp, 24);
    for (int i = 0; i < n; i++) g_sum[i] = tmp[i];
    g_sum_n = n;
    return n;
}

// Called from the game thread (Storm's file functions are the game's own, so use them where the game does).
// Retries for a while in case the archives aren't all mounted yet; gives up quietly after that.
static void misc_tick(void) {
    static int tries, done_misc, done_mon, done_it, done_sk, done_pm, done_st, done_dc, done_tz; static DWORD next;
    if (!g_merc_equip) done_it = 1;
    if (!g_glow_on) done_st = 1;
    if (!g_summons) done_sk = done_pm = 1;
    if (!g_dclone) done_dc = 1;
    if (!g_tz) done_tz = 1;
    if ((done_misc && done_mon && done_it && done_sk && done_pm && done_st && done_dc && done_tz) || tries >= 30) return;
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0) return;
    next = now + 3000; tries++;
    if (!done_misc) {
        int n = storm_read("Data\\global\\excel\\Misc.txt", parse_misc_cb);
        if (n >= 0 || n == -2) done_misc = 1;
        if (n >= 0) logf_("Misc.txt: %d stack sizes set (attempt %d)", n, tries);
        else if (n == -1 && tries == 30) logf_("Misc.txt could not be read; stack sizes will be learned while playing");
    }
    if (!done_mon) {
        int n = storm_read("Data\\global\\excel\\MonStats.txt", parse_monstats_cb);
        if (n >= 0 || n == -2) done_mon = 1;
        if (n > 0) logf_("MonStats.txt: %d mercenary classes (attempt %d)", n, tries);
    }
    if (!done_tz) {
        int n = storm_read("Data\\global\\excel\\Levels.txt", parse_tz_cb);
        if (n >= 0 || n == -2) done_tz = 1;
        if (n > 0) logf_("Levels.txt: %d terror zone areas in %d zones (%s, attempt %d)", n, g_tz_ngrp, g_tz_wp_only ? "waypoint areas only" : "any area with monsters", tries);
        else if (n == 0) logf_("Levels.txt: no waypoint areas found; terror zones are off");
    }
    if (!done_dc) {
        int n = storm_read("Data\\global\\excel\\MonStats.txt", parse_dc_cb);
        if (n >= 0 || n == -2) done_dc = 1;
        if (n > 0) logf_("MonStats.txt: diabloclone is class %d (attempt %d)", g_dc_class, tries);
        else if (n == 0) logf_("MonStats.txt has no diabloclone row; the Diablo Clone event is off");
    }
    if (!done_sk) {
        int n = storm_read("Data\\global\\excel\\Skills.txt", parse_skills_cb);
        if (n >= 0 || n == -2) done_sk = 1;
        if (n > 0) logf_("Skills.txt: %d druid summon skills (attempt %d)", n, tries);
    }
    if (done_sk && !done_pm && g_sum_n > 0) {
        int n = storm_read("Data\\global\\excel\\MonStats.txt", parse_petmon_cb);
        if (n >= 0 || n == -2) done_pm = 1;
        if (n > 0) logf_("MonStats.txt: %d summon pet classes (attempt %d)", n, tries);
    }
    if (!done_st) {
        int n = storm_read("Data\\global\\excel\\States.txt", parse_states_cb);
        if (n >= 0 || n == -2) done_st = 1;
        if (n > 0) logf_("States.txt: full-set glow is state %d (attempt %d)", g_glow_state, tries);
        else if (n == 0) logf_("States.txt has no fullsetgeneric row; the glow is off");
    }
    if (!done_it) {
        int n = storm_read("Data\\global\\excel\\ItemTypes.txt", parse_itemtypes_cb);
        if (n >= 0 || n == -2) done_it = 1;
        if (n > 0) logf_("ItemTypes.txt: %d item types (attempt %d), merc rules %s", n, tries, merc_rules_ready() ? "ready" : "NOT ready (missing types)");
    }
}

static int g_arg = -1;           // 2nd argument the game itself uses for gameplay packets (learned at runtime)

static int dm_begin(const void *pkt, int size, int arg, int send_arg_known);
static void dm_step(DWORD now);
static volatile int g_sum_busy;                    // 1 while the recast is running
static void sum_on_send(const unsigned char *b, int size);
static void sum_on_recv(const uint8_t *b, int len);
static void sum_step(DWORD now);
static volatile int g_dm_active_flag;               // mirrors g_dm.active for the hooks above its definition
static volatile DWORD g_sum_hold_until;              // while set (and in the future) the player's movement requests are held: a recast is in flight
static int g_last_send_r = 1;                       // result of the game's most recent real send
// The position the packets give us is only a walk DESTINATION while she is moving (or the last stop), so items beside the path looked out of range / the wrong distance.
// Read the real tile position from the client's own player unit (D2Client 1.13d, pointer at +0x11D050; dynamic path: x word at +2, y word at +6).
static void live_player_pos(void) {
    static HMODULE cl; static int ok = -1;
    if (ok < 0) {
        cl = GetModuleHandleA("D2Client.dll"); ok = 0;
        if (cl) { IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)cl + ((IMAGE_DOS_HEADER *)cl)->e_lfanew); ok = nt->FileHeader.TimeDateStamp == 1318970890u; }
    }
    if (ok != 1) return;
    DWORD *pp = (DWORD *)((BYTE *)cl + 0x11D050);
    if (!readable(pp, 4)) return;
    DWORD u = *pp;
    if (!u || !readable((const void *)u, 0x30) || *(DWORD *)u != 0) return;               // unit type 0 = player
    DWORD path = *(DWORD *)(u + 0x2c);
    if (!path || !readable((const void *)path, 8)) return;
    int x = *(WORD *)(path + 2), y = *(WORD *)(path + 6);
    if (x > 0 && y > 0) ap_set_pos(x, y);
    // Which level is she on? path+0x1c room1 -> +0x10 room2 -> +0x58 level -> +0x1d0 level number (the usual 1.13 layout). Towns: 1, 40, 75, 103, 109.
    // If any link is unreadable we say "not town" (pickups keep working) rather than block them.
    static int last_lvl = -1;
    int lvl = 0, town = 0;
    DWORD r1 = readable((const void *)(path + 0x1c), 4) ? *(DWORD *)(path + 0x1c) : 0;
    DWORD r2 = r1 && readable((const void *)(r1 + 0x10), 4) ? *(DWORD *)(r1 + 0x10) : 0;
    DWORD lv = r2 && readable((const void *)(r2 + 0x58), 4) ? *(DWORD *)(r2 + 0x58) : 0;
    if (lv && readable((const void *)(lv + 0x1d0), 4)) lvl = (int)*(DWORD *)(lv + 0x1d0);
    if (lvl == 1 || lvl == 40 || lvl == 75 || lvl == 103 || lvl == 109) town = 1;
    ap_set_town(town && !g_pickup_in_town);
    if (lvl != last_lvl) { last_lvl = lvl; logf_("area: level %d%s", lvl, town && !g_pickup_in_town ? " (town: auto-pickups off)" : ""); }
}
// The unit id of the player THIS client controls (client player unit, D2Client +0x11D050, unit id at +0xC); 0 = not known yet. The server unit of the same
// player has the same id. In a game with other players the server thread sees every player's packets, so anything keyed on "the player" must check this.
static DWORD local_player_id(void) {
    HMODULE cl = GetModuleHandleA("D2Client.dll"); if (!cl) return 0;
    DWORD *pp = (DWORD *)((BYTE *)cl + 0x11D050);
    if (!readable(pp, 4)) return 0;
    DWORD u = *pp;
    if (!u || !readable((const void *)u, 0x30) || *(DWORD *)u != 0) return 0;
    return *(DWORD *)(u + 0xC);
}
static BYTE *stub_alloc(void); static BYTE *emit(BYTE *p, const void *src, int n); static BYTE *emit_rel(BYTE *p, BYTE op, DWORD target);
// The name the game SHOWS for an area is not always Levels.txt's LevelName (Glacial Caves Level 1 is "Glacial Trail" in game): ask the game's own GetLevelName (D2Client 1.13d +0x18250, level in ESI,
// the same function qol_ui's overlay uses) and keep the text per candidate. Falls back to the LevelName column when it cannot be read.
static char g_tz_disp[TZ_MAX][48];
static const char *tz_name(int k) {
    if (k < 0 || k >= g_tz_n) return "";
    if (!g_tz_disp[k][0]) {
        static wchar_t *(*getname)(DWORD); static int tried;
        if (!tried) { HMODULE cl = GetModuleHandleA("D2Client.dll");
            if (cl) { tried = 1; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)cl + ((IMAGE_DOS_HEADER *)cl)->e_lfanew);
                BYTE *th = nt->FileHeader.TimeDateStamp == 1318970890u ? stub_alloc() : NULL;
                if (th) { static const BYTE a[] = {0x60, 0x8B, 0x74, 0x24, 0x24}; static const BYTE b[] = {0x89, 0x44, 0x24, 0x1C, 0x61, 0xC3};   // pushad; mov esi,[esp+24h]; call fn; mov [esp+1Ch],eax; popad; ret
                    BYTE *q = emit(th, a, 5); q = emit_rel(q, 0xE8, (DWORD)((BYTE *)cl + 0x18250)); emit(q, b, 6); getname = (wchar_t *(*)(DWORD))(void *)th; } } }
        wchar_t *w = getname ? getname((DWORD)g_tz_id[k]) : NULL;
        if (w && readable(w, 8)) {
            if (w[0] == 0xFF && w[1] == L'c' && w[2]) w += 3;                                         // already coloured by us
            int n = 0; for (; n < 47 && w[n]; n++) g_tz_disp[k][n] = (char)(w[n] < 256 ? w[n] : '?');
            while (n > 0 && g_tz_disp[k][n - 1] == ' ') n--; g_tz_disp[k][n] = 0;
            if (n > 0) logf_("terror zone: area %d is shown in game as \"%s\" (Levels.txt says \"%s\")", g_tz_id[k], g_tz_disp[k], g_tz_name[k]);
        }
    }
    return g_tz_disp[k][0] ? g_tz_disp[k] : g_tz_name[k];
}
// A new zone is picked every time you enter a game (and again when rotate_minutes run out while you are still in one). Nothing is saved: closing the game forgets the zone.
static volatile LONG g_tz_lock; static long long g_tz_start, g_tz_expire; static int g_tz_cur = -1, g_tz_games, g_tz_loaded; static volatile int g_tz_counted;   // g_tz_counted: the game you are in is already counted in g_tz_games (cleared when you leave the game, and when a new zone is picked)
#define TZ_MAX_GAMES 1                                                                                // a zone is live for ONE game (not configurable)
static int tz_spent(void) { return g_tz_games > TZ_MAX_GAMES; }              // the zone has already had its max_games_per_zone games: it is over for you until the next one
static void tz_state_pick(long long now) {                                                            // a new random zone, different from the last one, starting now
    static unsigned s; if (!s) s = GetTickCount() ^ (unsigned)now ^ (GetCurrentProcessId() * 2654435761u);
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; s += (unsigned)now;
    int old = g_tz_cur >= 0 ? g_tz_grp[g_tz_cur] : -1, g = (int)(s % (unsigned)g_tz_ngrp);
    if (g == old && g_tz_ngrp > 1) g = (g + 1 + (int)((s >> 8) % (unsigned)(g_tz_ngrp - 1))) % g_tz_ngrp;
    g_tz_cur = g_tz_rep[g]; g_tz_start = now; g_tz_expire = now + (long long)g_tz_minutes * 60;
    g_tz_games = 0; g_tz_counted = 0;                                                                  // picking never counts a game itself (it can run in a hook while a game is still loading, and local_player_id() may still hold the last game's unit): tz_count_game does, once per game
    logf_("terror zone: new zone area %d, lasts %d minutes", g_tz_id[g_tz_cur], g_tz_minutes);
}
static int tz_index_for(long long unused) {                                                           // the current zone (the first area of it); picks a new one when none is saved or its time is over
    (void)unused;
    if (g_tz_ngrp <= 0) return 0;
    long long now = (long long)time(NULL);
    if (g_tz_loaded && g_tz_cur >= 0 && now < g_tz_expire && now >= g_tz_start - 300) return g_tz_cur;   // the common case: no lock, no file
    while (InterlockedCompareExchange(&g_tz_lock, 1, 0)) Sleep(0);
    g_tz_loaded = 1;
    if (g_tz_cur < 0 || now >= g_tz_expire || now < g_tz_start - 300) tz_state_pick(now);
    InterlockedExchange(&g_tz_lock, 0);
    return g_tz_cur;
}
static void tz_count_game(const char *why) {                                                          // counts the game you are in, ONCE (a second call for the same game does nothing)
    while (InterlockedCompareExchange(&g_tz_lock, 1, 0)) Sleep(0);
    if (g_tz_cur >= 0 && !g_tz_counted) { g_tz_counted = 1; g_tz_games++; logf_("terror zone: game counted (%d for zone area %d, %s)", g_tz_games, g_tz_id[g_tz_cur], why); }
    InterlockedExchange(&g_tz_lock, 0);
}
static volatile DWORD g_srv_player, g_srv_game;                                                         // (defined again below with the pet code: the server's player unit and game)
static void tz_game_entered(const char *why) {                                                      // you entered a game: always a new zone, then count the game
    if (g_tz_ngrp > 0) { while (InterlockedCompareExchange(&g_tz_lock, 1, 0)) Sleep(0); g_tz_loaded = 1; tz_state_pick((long long)time(NULL)); InterlockedExchange(&g_tz_lock, 0); }
    tz_count_game(why);
}
static void tz_tick(DWORD now) {
    tz_apply_level();                                                                                    // keeps the zone's area level in step with the clock (every frame is cheap: it only acts on a change)
    static DWORD next, announce_at, last_player; static long long last_slot = -1; static void (__stdcall *print)(const wchar_t *, int); static int tried;
    if (!g_tz || g_tz_n <= 0 || (int)(now - next) < 0) return;
    next = now + 1000;
    if (!tried) { tried = 1; HMODULE cl = GetModuleHandleA("D2Client.dll");
        if (cl) { IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)cl + ((IMAGE_DOS_HEADER *)cl)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp == 1318970890u) print = (void (__stdcall *)(const wchar_t *, int))((BYTE *)cl + 0x75EB0); else logf_("terror zone: this D2Client build is not supported; the zone is only logged"); } }
    DWORD me = local_player_id();
    if (!me) { last_player = 0; g_tz_counted = 0; return; }                                           // out of the game: the next game is a new one
    // A new game is seen two ways: you were out of any game since the last tick (above), or the server's game and player pointers changed without that (a quick quit-and-create, or the player
    // pointer never reading 0). The second is ignored for 20 s after a count so the pointers settling while a game loads cannot count the same game twice.
    static DWORD last_sg, last_sp, counted_at; DWORD sg = g_srv_game, sp = g_srv_player; const char *why = "entered a game";
    int fresh = (me != last_player);
    if (!fresh && g_tz_counted && sg && sp && last_sg && last_sp && (sg != last_sg || sp != last_sp) && (int)(now - counted_at) > 20000) { fresh = 1; why = "a different game (the server's pointers changed)"; g_tz_counted = 0; }
    if (sg && sp) { last_sg = sg; last_sp = sp; }
    if (fresh) { last_player = me; tz_game_entered(why); counted_at = now; announce_at = now + 4000; last_slot = -1; }      // entering a game: counted in the file; the zone only changes when its time is over; announce it once the screen is up
    tz_index_for(0); tz_count_game("zone began during this game"); long long slot = g_tz_start;       // a zone that begins while you are playing counts this game as its first (once)
    // the zone's start time identifies it: a new one is announced when it changes
    if (slot == last_slot || (int)(now - announce_at) < 0) return;
    if (tz_spent()) { last_slot = slot; logf_("terror zone: the zone (area %d) has had its game; nothing is announced until the next zone", g_tz_id[tz_index_for(0)]); return; }     // spent: stay silent
    if (!tz_active()) return;                                                                        // hell_only and this is not Hell: no zone (checked again at the next slot / next game)
    { int k0 = tz_index_for(slot); tz_name(k0);                                                      // the name the game shows for the area: wait up to 8 s for it, never announce the internal Levels.txt name unless it cannot be read
      if (!g_tz_disp[k0][0] && (int)(now - announce_at) < 8000) return;
      if (!g_tz_disp[k0][0]) logf_("terror zone: the game's own name for area %d could not be read; announcing the Levels.txt name \"%s\"", g_tz_id[k0], g_tz_name[k0]); }
    last_slot = slot;
    int k = tz_index_for(slot);
    logf_("terror zone: announced %s (level %d)", tz_name(k), g_tz_id[k]);
    if (print) {
        static const char *lines[] = { "Heightened terror grips %s...", "A dark presence stirs in %s...", "The shadows deepen over %s...", "Something hungers in %s...", "Dread gathers in %s..." };
        const char *fmt = lines[(unsigned)slot % (sizeof lines / sizeof *lines)];
        char nb[200]; const char *pc = strstr(fmt, "%s");
        if (pc) snprintf(nb, sizeof nb, "%.*s%s%s", (int)(pc - fmt), fmt, tz_name(k), pc + 2); else snprintf(nb, sizeof nb, "%s %s", fmt, tz_name(k));
        wchar_t m[256]; swprintf(m, 256, L"\xFF" L"c;%hs", nb); print(m, 0); }
}
// -- chat commands: .reset stats / .reset skills (also with a leading '/') --
// The command is read on the SERVER side, from the chat packet of ANY player in the game (the server runs in the process that hosts the game: a bot that
// joined a TCP/IP game has no server of its own, so a client-side flag could never be acted on, 2026-11-13). Returns 1 (stats), 2 (skills) or 0.
static int respec_parse(const unsigned char *b, int size) {                  // client->server chat line: 15 <type> <?> text 00 ...
    for (int off = 1; off <= 4 && off < size; off++) {
        if (b[off] != '.' && b[off] != '/') continue;
        char t[16]; int n = 0;
        for (int i = off + 1; i < size && n < 15 && b[i]; i++) t[n++] = (char)((b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i]);
        t[n] = 0;
        if (!strcmp(t, "reset stats")) return 1;
        if (!strcmp(t, "reset skills")) return 2;
        if (!strcmp(t, "dclone")) return 3;
        if (!strcmp(t, "dclone now")) return 4;
        return 0;
    }
    return 0;
}
static int __stdcall h_send(int size, int arg, void *pkt) {
    if (g_summons && !g_injecting && size > 0 && size <= 16 && readable(pkt, size)) {
        sum_on_send(pkt, size);
        const unsigned char *b = pkt;
        (void)b;                                                                               // the player's own requests are never held: the recast waits for her to stand still instead
    }
    if (g_merc_drop && !g_injecting && size == 9 && readable(pkt, 9) && ((const unsigned char *)pkt)[0] == 0x36 && !g_dm_active_flag
        && dm_begin(pkt, size, arg, g_arg >= 0)) return g_last_send_r;      // the hire request is held back until the merc's gear is dropped
    int r = o_send(size, arg, pkt);
    if (!g_injecting && size > 0) g_last_send_r = r;
    LARGE_INTEGER t0 = {0}; if (g_logging) QueryPerformanceCounter(&t0);
    if (!g_injecting && size > 0 && readable(pkt, size < 64 ? size : 1)) {
        const unsigned char *b = pkt;
        if (b[0] < 0x60) {                              // gameplay packet (0x6x are lobby/ping)
            if (g_arg != arg) { g_arg = arg; logf_("learned send arg=%d from packet id=%02x", arg, b[0]); }
            ap_on_send(b, size);
        }
    }
    if (g_logging) prof_add(1, t0);
    return r;
}

static int __stdcall h_recv(void *buf, int cap) {
    int r = o_recv(buf, cap);
    LARGE_INTEGER t0 = {0}; if (g_logging) QueryPerformanceCounter(&t0);
    if (r > 0) {                                         // buffer was just filled by the game: no VirtualQuery needed
        ap_on_recv(buf, r);
        if (g_summons) sum_on_recv((const uint8_t *)buf, r);
    }
    misc_tick();
    DWORD now = GetTickCount();
    tz_tick(now);
    if (g_summons) sum_step(now);
    if (g_dm_active_flag) dm_step(now);
    // The live position read walks several game structures; only do it when summons need it every packet, or when a
    // pickup scan is actually due (something lies on the ground).
    int due = !g_dm_active_flag && ap_due(now);
    if (g_summons || due) live_player_pos();
    uint8_t out[16];
    int n = due ? ap_tick(now, out) : 0;
    if (n > 0 && g_arg >= 0) {                           // only once we know how the game calls SendPacket
        g_injecting = 1;
        int sr = o_send(n, g_arg, out);
        g_injecting = 0;
        logf_("sent pickup packet (%d bytes), result=%d", n, sr);
    }
    if (g_logging) { prof_add(0, t0); prof_report(); }
    return r;
}

// ---- patching ------------------------------------------------------------------------------------------
static int inline_hook(void *target, void *hook, void **tramp_out) {
    BYTE *t = (BYTE *)target;
    if (t[0] != 0xA1) { logf_("inline hook: unexpected first byte %02x (want a1 = mov eax,[abs]); skipping", t[0]); return 0; }
    BYTE *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, t, 5);                                    // `mov eax, [abs32]` is position independent
    tr[5] = 0xE9; *(DWORD *)(tr + 6) = (DWORD)(t + 5) - (DWORD)(tr + 10);
    DWORD old;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)hook - (DWORD)(t + 5);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    *tramp_out = tr;
    return 1;
}

static int hook_import_ordinal(HMODULE mod, const char *dll, unsigned ord, void *hook, void **orig) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *oft = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *ft = (void *)(base + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (!(oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32) || (oft->u1.Ordinal & 0xFFFF) != ord) continue;
            DWORD old;
            VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
            *orig = (void *)ft->u1.Function;
            ft->u1.Function = (DWORD)hook;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
            return 1;
        }
    }
    return 0;
}

static int g_keep_equip = 0;                                    // ini: [death] keep_equipment
static int g_xp_nopenalty = 0;                                  // ini: [experience] no_level_penalty
static int g_fe_fix = 0;                                        // ini: [monsters] fire_enchanted_fix
static int g_infinite_ammo = 0;                                 // ini: [items] infinite_ammo
static int g_wide_halls = 0;                                    // ini: [maps] wide_halls
static int g_cpu_fix = 0, g_cpu_throttle = 0;                    // ini: [cpu] fix, throttle_render
static int g_merc_run = 0;                                      // ini: [mercenary] run
static int g_clk_enabled = 1, g_clk_key = VK_SPACE, g_clk_delay = 200, g_clk_interval = 100;

// ---- config --------------------------------------------------------------------------------------------
static void load_config(ap_config *c) {
    char path[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, path);
    lstrcatA(path, "\\qol.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(path, "w");
        if (f) {
            fputs("; qol settings (changes apply on next game launch)\r\n"
                  "[qol]\r\n"
                  "; master switch for everything: 1 = on, 0 = off\r\n"
                  "enabled=1\r\n"
                  "; 1 = write qol.log (decisions, errors), 0 = no log file at all\r\n"
                  "logging=0\r\n"
                  "\r\n"
                  "[pickup]\r\n"
                  "; potion auto-pickup: 1 = on, 0 = off\r\n"
                  "enabled=1\r\n"
                  "; pick up potions within this many tiles of the player\r\n"
                  "range=4\r\n"
                  "; how often (milliseconds) to look for items to pick up. Higher = less work per second, slower reaction (20 to 2000)\r\n"
                  "poll_ms=120\r\n"
                  "; 1 = pick up every rejuvenation potion even if your belt has no room for it (it goes to your inventory)\r\n"
                  "rejuv_always=1\r\n"
                  "; 1 = also pick up gold piles within range\r\n"
                  "gold=1\r\n"
                  "; 1 = also pick things up while standing in town (0 = no pickups in town)\r\n"
                  "pickup_in_town=0\r\n"
                  "\r\n"
                  "[death]\r\n"
                  "; 1 = keep your equipment when you die (the \"KeepEquip\" hex edit by SnakeByteStudios, applied in memory). Single player.\r\n"
                  "; Off by default.\r\n"
                  "keep_equipment=1\r\n"
                  "\r\n"
                  "[experience]\r\n"
                  "; 1 = no XP penalty for level differences, in either direction: a high-level character still gets full XP from\r\n"
                  "; low-level monsters, and a low-level character still gets full XP from high-level monsters (e.g. when partied with a\r\n"
                  "; high-level character). Single player. Off by default.\r\n"
                  "no_level_penalty=1\r\n"
                  "\r\n"
                  "[monsters]\r\n"
                  "; 1 = fix the Nightmare Fire Enchanted death explosion: the game cuts it to 2/3 in Nightmare but to 1/8 in Hell, so\r\n"
                  "; late-Nightmare monsters explode about twice as hard as Hell ones. This makes Nightmare 1/4, so it grows smoothly\r\n"
                  "; from Normal into Hell. Normal and Hell are unchanged. Stock LoD 1.13d only.\r\n"
                  "fire_enchanted_fix=1\r\n"
                  "\r\n"
                  "[items]\r\n"
                  "; 1 = quivers and throwing weapons are never used up: arrows and bolts are not subtracted when you shoot, thrown javelins, knives and axes are not subtracted when you throw, and a\r\n"
                  "; throwing weapon that wears out keeps its whole stack. Applies to every player of a game you host (also on a LAN). Stock LoD 1.13d only.\r\n"
                  "infinite_ammo=1\r\n"
                  "\r\n"
                  "[mercenary]\r\n"
                  "; 1 = mercenaries run when they follow you instead of always walking (fixes a bug in stock LoD 1.13d).\r\n"
                  "; Off by default. Only does anything on stock LoD 1.13d.\r\n"
                  "run=1\r\n"
                  "; 1 = mercenaries can equip a wider range of items: Rogue: bows, crossbows; Desert: polearms, spears (also Amazon),\r\n"
                  "; javelins; Iron Wolf: shields, 1H swords, wands, scepters, maces, orbs; Barbarian: swords, axes, maces, hammers,\r\n"
                  "; Barbarian helms; every merc: rings, amulets, boots, gloves, belts. Replaces BaseMod's [MercMod] options: set\r\n"
                  "; FullEquip=0, RogueCrossBow=0, RogueAmaBow=0 in BaseMod.ini or this stays off. Stock LoD 1.13d only.\r\n"
                  "equip=1\r\n"
                  "\r\n"
                  "[glow]\r\n"
                  "; 1 = players get the glowing aura the game shows for a full set of Tal Rasha, Immortal King and the like (cosmetic, no bonuses). Applies to every player of a game you host (also on a LAN). Stock LoD 1.13d only.\r\n"
                  "enabled=1\r\n"
                  "; only characters of this level or higher get it (included). 1 = no level needed\r\n"
                  "min_level=1\r\n"
                  "; 1 = only characters that have beaten Baal in Hell (the quest data of their save) get the glow; 0 = every character of that level.\r\n"
                  "hell_baal_required=1\r\n"
                  "\r\n"
                  "[maps]\r\n"
                  "; 1 = wider hallways in the Maggot Lair and the Arcane Sanctuary (map files from qol.mpq, which must sit next to qol.dll).\r\n"
                  "; Off by default. Takes effect for areas generated after the game starts.\r\n"
                  "wide_halls=1\r\n"
                  "\r\n"
                  "[cpu]\r\n"
                  "; 1 = stop the game using 100% of a CPU core while idle (the well-known D2 busy-loop bug): its main loops now sleep 1 ms\r\n"
                  "; instead of spinning. Stock LoD 1.13d only.\r\n"
                  "fix=1\r\n"
                  "; 1 = also skip redundant frame drawing when the game is ahead of its 25 fps logic (extra CPU saving; experimental)\r\n"
                  "throttle_render=0\r\n"
                  "\r\n"
                  "[clicker]\r\n"
                  "; hold a key to repeat left mouse clicks at the current cursor position\r\n"
                  "enabled=1\r\n"
                  "; virtual-key code of the hold key (32 = spacebar)\r\n"
                  "key=32\r\n"
                  "; key must be held this long (milliseconds) before clicking starts (so typing a space doesn't click)\r\n"
                  "hold_delay_ms=200\r\n"
                  "; pause between clicks (milliseconds); 100 = 10 clicks per second\r\n"
                  "interval_ms=100\r\n", f);
            fclose(f);
        }
    }
    int master = GetPrivateProfileIntA("qol", "enabled", 1, path);
    g_merc_run = master && GetPrivateProfileIntA("mercenary", "run", 1, path);
    g_merc_equip = master && GetPrivateProfileIntA("mercenary", "equip", 1, path);
    g_merc_drop = master && GetPrivateProfileIntA("mercenary", "drop_items", 1, path);
    g_respec = master && GetPrivateProfileIntA("respec", "enabled", 1, path);
    g_summons = master && GetPrivateProfileIntA("summons", "enabled", 1, path);
    g_tz = master && GetPrivateProfileIntA("terrorzone", "enabled", 1, path);
    g_tz_hell_only = GetPrivateProfileIntA("terrorzone", "hell_only", 0, path);
    g_tz_wp_only = GetPrivateProfileIntA("terrorzone", "waypoint_areas_only", 0, path);
    g_tz_dens_pct = GetPrivateProfileIntA("terrorzone", "density_pct", 100, path); if (g_tz_dens_pct < 0) g_tz_dens_pct = 0; if (g_tz_dens_pct > 1000) g_tz_dens_pct = 1000;
    g_tz_elite_pct = GetPrivateProfileIntA("terrorzone", "elite_pct", 100, path); if (g_tz_elite_pct < 0) g_tz_elite_pct = 0; if (g_tz_elite_pct > 1000) g_tz_elite_pct = 1000;
    g_tz_lvl_bonus = GetPrivateProfileIntA("terrorzone", "level_bonus", 10, path); if (g_tz_lvl_bonus < 0) g_tz_lvl_bonus = 0; if (g_tz_lvl_bonus > 99) g_tz_lvl_bonus = 99;
    g_tz_lvl_min = GetPrivateProfileIntA("terrorzone", "hell_min_level", 85, path); if (g_tz_lvl_min < 0) g_tz_lvl_min = 0; if (g_tz_lvl_min > 110) g_tz_lvl_min = 110;
    g_tz_lvl_max = GetPrivateProfileIntA("terrorzone", "hell_max_level", 99, path); if (g_tz_lvl_max < 1) g_tz_lvl_max = 1; if (g_tz_lvl_max > 110) g_tz_lvl_max = 110;
    g_tz_mf = GetPrivateProfileIntA("terrorzone", "magic_find_pct", 100, path); if (g_tz_mf < 0) g_tz_mf = 0; if (g_tz_mf > 1000) g_tz_mf = 1000;
    g_tz_xp = GetPrivateProfileIntA("terrorzone", "xp_pct", 100, path); if (g_tz_xp < 0) g_tz_xp = 0; if (g_tz_xp > 10000) g_tz_xp = 10000;
    g_tz_gold = GetPrivateProfileIntA("terrorzone", "gold_pct", 100, path); if (g_tz_gold < 0) g_tz_gold = 0; if (g_tz_gold > 10000) g_tz_gold = 10000;
    g_tz_minutes = GetPrivateProfileIntA("terrorzone", "rotate_minutes", 60, path); if (g_tz_minutes < 1) g_tz_minutes = 1; if (g_tz_minutes > 1440) g_tz_minutes = 1440;
    g_dclone = master && GetPrivateProfileIntA("dclone", "enabled", 1, path);
    { char b[32];
      GetPrivateProfileStringA("dclone", "roll_minutes", "10", b, sizeof b, path); g_dc_minutes = atof(b); if (g_dc_minutes < 0.1) g_dc_minutes = 0.1; if (g_dc_minutes > 600) g_dc_minutes = 600;
      GetPrivateProfileStringA("dclone", "chance_per_roll", "5", b, sizeof b, path); g_dc_chance = atof(b); if (g_dc_chance < 0) g_dc_chance = 0; if (g_dc_chance > 100) g_dc_chance = 100; }
    g_dc_announce = GetPrivateProfileIntA("dclone", "announce", 1, path);
    g_dc_diff[0] = GetPrivateProfileIntA("dclone", "normal", 1, path); g_dc_diff[1] = GetPrivateProfileIntA("dclone", "nightmare", 1, path); g_dc_diff[2] = GetPrivateProfileIntA("dclone", "hell", 1, path);
    g_glow_baal = GetPrivateProfileIntA("glow", "hell_baal_required", 1, path);
    g_glow_on = master && GetPrivateProfileIntA("glow", "enabled", 0, path);
    g_glow_min = GetPrivateProfileIntA("glow", "min_level", 1, path); if (g_glow_min < 0 || g_glow_min > 99) g_glow_min = 0;
    g_wide_halls = master && GetPrivateProfileIntA("maps", "wide_halls", 1, path);
    g_cpu_fix = master && GetPrivateProfileIntA("cpu", "fix", 1, path);
    g_cpu_throttle = g_cpu_fix && GetPrivateProfileIntA("cpu", "throttle_render", 0, path);
    g_merc_special = master && GetPrivateProfileIntA("mercenary", "whirlwind", 1, path);
    g_xp_nopenalty = master && GetPrivateProfileIntA("experience", "no_level_penalty", 1, path);
    g_keep_equip = master && GetPrivateProfileIntA("death", "keep_equipment", 1, path);
    g_fe_fix = master && GetPrivateProfileIntA("monsters", "fire_enchanted_fix", 1, path);
    g_infinite_ammo = master && GetPrivateProfileIntA("items", "infinite_ammo", 1, path);
    c->enabled = master && GetPrivateProfileIntA("pickup", "enabled", 1, path);
    c->range = GetPrivateProfileIntA("pickup", "range", 4, path);
    c->rejuv_always = GetPrivateProfileIntA("pickup", "rejuv_always", 1, path);
    c->gold = GetPrivateProfileIntA("pickup", "gold", 1, path);
    g_pickup_in_town = GetPrivateProfileIntA("pickup", "pickup_in_town", 0, path);
    c->poll_ms = GetPrivateProfileIntA("pickup", "poll_ms", 120, path);
    g_logging = GetPrivateProfileIntA("qol", "logging", 0, path);
    c->verbose = g_logging ? 1 : 0;
    g_clk_enabled = master && GetPrivateProfileIntA("clicker", "enabled", 1, path);
    g_clk_key = GetPrivateProfileIntA("clicker", "key", VK_SPACE, path);
    g_clk_delay = GetPrivateProfileIntA("clicker", "hold_delay_ms", 200, path);
    g_clk_interval = GetPrivateProfileIntA("clicker", "interval_ms", 100, path);
}

// ---- hold-to-click ---------------------------------------------------------------------------------------
// While the configured key is held (after hold_delay_ms) and the game window is focused, post
// WM_LBUTTONDOWN/UP to the game window at the current cursor position every interval_ms.

static BOOL CALLBACK find_wnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND *)lp = h;
    return FALSE;
}

// Timing notes: the loop is deadline-driven (click N is due at hold_start + delay + N*interval) so Sleep() overshoot
// doesn't accumulate into a slower click rate, and it sleeps (never spins) between events. While the key is up it
// wakes only IDLE_MS apart and makes a single cheap call; the window lookup / foreground checks only happen once
// the key has been held past the delay. The down->up gap is a fixed CLICK_GAP_MS so the game sees a real click.
#define IDLE_MS 20          // key-up poll period: hold detection resolution (delay is typically 100s of ms)
#define AWAY_MS 100         // back-off while the game window isn't focused
#define CLICK_GAP_MS 15     // time the button stays down

static DWORD WINAPI clicker(LPVOID arg) {
    (void)arg;
    const int interval = g_clk_interval > CLICK_GAP_MS + 5 ? g_clk_interval : CLICK_GAP_MS + 5;
    HWND game = NULL;
    DWORD due = 0;
    int held = 0;
    for (;;) {
        if (!(GetAsyncKeyState(g_clk_key) & 0x8000)) { held = 0; Sleep(IDLE_MS); continue; }
        DWORD now = GetTickCount();
        if (!held) { held = 1; due = now + (DWORD)g_clk_delay; }
        int wait = (int)(due - now);
        if (wait > 0) { Sleep(wait < IDLE_MS ? (DWORD)wait : IDLE_MS); continue; }   // wake early enough to notice a release
        if (!game || !IsWindow(game)) { game = NULL; EnumWindows(find_wnd, (LPARAM)&game); }
        POINT pt;
        if (!game || GetForegroundWindow() != game || !GetCursorPos(&pt) || !ScreenToClient(game, &pt)) {
            held = 0; Sleep(AWAY_MS); continue;      // unfocused: restart the hold delay once focus returns
        }
        LPARAM lp = MAKELPARAM(pt.x, pt.y);
        PostMessageA(game, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        Sleep(CLICK_GAP_MS);
        PostMessageA(game, WM_LBUTTONUP, 0, lp);     // always release, even if the key was let go meanwhile
        due += (DWORD)interval;
        now = GetTickCount();
        if ((int)(now - due) > 0) due = now;         // fell behind (e.g. a stall): resync instead of bursting
    }
    return 0;
}

// ---- in-memory D2Game.dll edits (the hex edits people make to the file, done at startup instead) ---------------------
// Each edit is found by its bytes (sig.h), must occur exactly once, and is only applied if its ini switch is on, so the
// game files stay untouched. If the file already has the edit, the log says so.
//  - no XP level penalty: D2Game scales every kill's XP by a table indexed by the level gap between character and monster
//    (both directions, 0% for big gaps). The scale ends in `cmp eax,100h ; je full_xp ; ...scale...`; `je` -> `jmp` returns full XP.
//  - keep equipment on death (SnakeByteStudios' KeepEquip): in the player-creation function `mov edx,[ebp+0C8h]` becomes
//    `jmp epilogue ; nop`, skipping the rest of that function.
static void patch_bytes(BYTE *at, const BYTE *src, int n) {
    DWORD old;
    if (!VirtualProtect(at, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(at, src, (size_t)n);
    VirtualProtect(at, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, (SIZE_T)n);
}
static DWORD WINAPI patch_thread(LPVOID arg) {
    (void)arg;
    if (!g_xp_nopenalty && !g_keep_equip && !g_fe_fix && !g_infinite_ammo) return 0;
    for (int i = 0; i < 6000; i++) {                       // D2Game.dll loads when a game is started
        HMODULE g = GetModuleHandleA("D2Game.dll");
        if (!g) { Sleep(100); continue; }
        BYTE *base = (BYTE *)g;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        int xp_done = !g_xp_nopenalty, keep_done = !g_keep_equip, fe_done = !g_fe_fix, use_done = !g_infinite_ammo, brk_done = !g_infinite_ammo;
        for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *code = base + sec->VirtualAddress; size_t len = sec->Misc.VirtualSize; int patched = 0;
            if (!xp_done) {
                long at = sig_find_xp_penalty(code, len, &patched);
                if (at >= 0) {
                    xp_done = 1;
                    if (patched) logf_("no XP level penalty: D2Game already has the edit (+%lx)", (unsigned long)(code + at - base));
                    else { static const BYTE jmp = 0xEB; patch_bytes(code + at, &jmp, 1); logf_("no XP level penalty: patched D2Game +%lx (je -> jmp)", (unsigned long)(code + at - base)); }
                }
            }
            if (!keep_done) {
                long ep = -1, at = sig_find_keep_equip(code, len, &patched, &ep);
                if (at >= 0) {
                    keep_done = 1;
                    if (patched) logf_("keep equipment: D2Game already has the edit (+%lx)", (unsigned long)(code + at - base));
                    else if (ep < 0) logf_("keep equipment: function epilogue not found; not patched");
                    else {
                        BYTE jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
                        *(DWORD *)(jmp + 1) = (DWORD)(ep - (at + 5));
                        patch_bytes(code + at, jmp, 6);
                        logf_("keep equipment: patched D2Game +%lx (jmp +%lx)", (unsigned long)(code + at - base), (unsigned long)(ep - (at + 5)));
                    }
                }
            }
            if (!use_done) {                                                    // arrows, bolts and thrown weapons are not used up: skip the quantity subtraction
                long ep = -1, at = sig_find_ammo_use(code, len, &patched, &ep);
                if (at >= 0) {
                    use_done = 1;
                    if (patched) logf_("infinite ammo: D2Game ammo use already patched (+%lx)", (unsigned long)(code + at - base));
                    else { BYTE jmp[5] = {0xE9, 0, 0, 0, 0}; *(DWORD *)(jmp + 1) = (DWORD)(ep - (at + 5)); patch_bytes(code + at, jmp, 5); logf_("infinite ammo: patched ammo use (D2Game +%lx, jmp +%lx)", (unsigned long)(code + at - base), (unsigned long)(ep - (at + 5))); }
                }
            }
            if (!brk_done) {                                                    // thrown weapons that wear out keep their stack: the unit that breaks is not taken away
                long at = sig_find_ammo_break(code, len, &patched);
                if (at >= 0) {
                    brk_done = 1;
                    if (patched) logf_("infinite ammo: D2Game throwing-weapon breakage already patched (+%lx)", (unsigned long)(code + at - base));
                    else { static const BYTE zero = 0x00, nop = 0x90; patch_bytes(code + at + 2, &zero, 1); patch_bytes(code + at + 11, &nop, 1); logf_("infinite ammo: patched throwing-weapon breakage (D2Game +%lx)", (unsigned long)(code + at - base)); }
                }
            }
            if (!fe_done) {
                long at = sig_find_fe_nightmare(code, len, &patched);
                if (at >= 0) {
                    fe_done = 1;
                    if (patched) logf_("fire enchanted fix: D2Game already has the edit (+%lx)", (unsigned long)(code + at - base));
                    else { patch_bytes(code + at, SIG_FE_NM_FIX, 20); logf_("fire enchanted fix: patched D2Game +%lx (Nightmare explosion x2/3 -> x1/4)", (unsigned long)(code + at - base)); }
                }
            }
        }
        if (!use_done) logf_("infinite ammo: the ammo-use code was not found in this D2Game (or appears more than once); not patched");
        if (!brk_done) logf_("infinite ammo: the throwing-weapon breakage code was not found in this D2Game (or appears more than once); not patched");
        if (!fe_done) logf_("fire enchanted fix: the explosion code was not found in this D2Game (or appears more than once); not patched");
        if (!xp_done) logf_("no XP level penalty: the level-difference code was not found in this D2Game (or appears more than once); not patched");
        if (!keep_done) logf_("keep equipment: the player-creation code was not found in this D2Game (or appears more than once); not patched");
        return 0;
    }
    return 0;
}

// ---- mercenary equipment rules (PD2's list, plus BaseMod's FullEquip list) -----------------------------------------------
// The game decides "may this merc use this item?" in a server function (D2Game) and a twin in the interface (D2Client). Each
// sets an armor flag and then dispatches on the merc's class to hard-coded item-type checks. We jump from the start of that
// dispatch into a small stub that asks merc_allows() (merc.c); if it says yes the code continues at the game's own "allowed"
// path, otherwise at the game's own armor test, so armor, requirements and messages stay exactly as they were.
// Coexistence with BaseMod: its [MercMod] options (FullEquip, RogueCrossBow, RogueAmaBow) install replacements of these same
// functions. We refuse to patch while any of them is on (and log why), and also if the function's entry no longer looks stock.
#define D2CLIENT_TIMESTAMP_113D 1318970890u
// What the merc holds in a hand (body location 4 = right, 5 = left): 0 = nothing, MERC_HAND_UNKNOWN if it cannot be read. D2Common #10292 = item at a body location.
static merc_item_t merc_hand(DWORD unit, DWORD loc) {
    static FARPROC at_loc;
    if (!at_loc) { HMODULE c = GetModuleHandleA("D2Common.dll"); if (c) at_loc = GetProcAddress(c, (LPCSTR)(UINT_PTR)10292); }
    if (!at_loc || !unit || !readable((const void *)unit, 0x70) || *(DWORD *)unit != 1) return MERC_HAND_UNKNOWN;
    DWORD inv = *(DWORD *)(unit + 0x60);
    if (!inv || !readable((const void *)inv, 0x10)) return MERC_HAND_UNKNOWN;
    DWORD it = ((DWORD (__stdcall *)(DWORD, DWORD))at_loc)(inv, loc);
    return it && readable((const void *)it, 0x14) && *(DWORD *)it == 4 ? (merc_item_t)it : (merc_item_t)0;
}
static int __stdcall qt_merc_allows(DWORD item, int cls, DWORD chk, DWORD two, DWORD unit) {
    merc_item_t hand = cls == 271 ? merc_hand(unit, 4) : MERC_HAND_UNKNOWN, left = cls == 271 ? merc_hand(unit, 5) : MERC_HAND_UNKNOWN;
    int r = merc_allows_hands((merc_item_t)item, cls, (merc_chk)(UINT_PTR)chk, (merc_two)(UINT_PTR)two, hand, left);
    return r;
}
static int basemod_merc_active(void) {
    if (!GetModuleHandleA("BaseMod.dll")) return 0;
    char path[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, path);
    lstrcatA(path, "\\BaseMod.ini");
    return GetPrivateProfileIntA("MercMod", "FullEquip", 1, path) || GetPrivateProfileIntA("MercMod", "RogueCrossBow", 0, path) ||
           GetPrivateProfileIntA("MercMod", "RogueAmaBow", 0, path);
}
static DWORD abs_call_target(const BYTE *insn) { return (DWORD)(insn + 5) + *(const DWORD *)(insn + 1); }
static BYTE *stub_alloc(void) {
    static BYTE *mem; static int used;
    if (!mem) { mem = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); used = 0; }
    if (!mem || used > 4096 - 128) return NULL;
    BYTE *p = mem + used; used += 128; return p;
}
static BYTE *emit(BYTE *p, const void *src, int n) { memcpy(p, src, (size_t)n); return p + n; }
static BYTE *emit_rel(BYTE *p, BYTE op, DWORD target) { *p++ = op; *(DWORD *)p = target - (DWORD)(p + 4); return p + 4; }
static BYTE *emit_push(BYTE *p, DWORD v) { *p++ = 0x68; *(DWORD *)p = v; return p + 4; }
// finishes a stub: result in eax -> allowed ? jump to `allowed` : jump to `not_allowed`
static BYTE *emit_tail(BYTE *p, DWORD allowed, DWORD not_allowed) {
    static const BYTE test_eax[] = {0x85, 0xC0};
    p = emit(p, test_eax, 2);
    *p++ = 0x0F; *p++ = 0x85; *(DWORD *)p = allowed - (DWORD)(p + 4); p += 4;      // jne allowed
    return emit_rel(p, 0xE9, not_allowed);                                          // jmp not_allowed
}

static int patch_merc_module(HMODULE mod, int server) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    const char *name = server ? "D2Game" : "D2Client";
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *code = base + sec->VirtualAddress; size_t len = sec->Misc.VirtualSize;
        merc_site_t m;
        if (!(server ? sig_find_merc_server(code, len, &m) : sig_find_merc_client(code, len, &m))) continue;
        static const BYTE entry_srv[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x10}, entry_cli[] = {0xC7, 0x07, 0x00, 0x00, 0x00, 0x00};
        BYTE *entry = code + m.site - (server ? 0xAC : 0x72);
        if (memcmp(entry, server ? entry_srv : entry_cli, 6)) { logf_("merc equipment: %s rule function entry was modified by something else; not patching", name); return 0; }
        BYTE *site = code + m.site;
        if (site[0] == 0xE9) { logf_("merc equipment: %s already patched", name); return 1; }
        DWORD chk = abs_call_target(code + m.chk_call), two = abs_call_target(code + m.two_call);
        DWORD allowed = (DWORD)(code + m.allowed), not_allowed = (DWORD)(code + m.not_allowed);
        BYTE *st = stub_alloc();
        if (!st) { logf_("merc equipment: no memory for the stub"); return 0; }
        BYTE *p = st;
        if (server) {
            static const BYTE a[] = {0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x85, 0xFF, 0x74, 0x03, 0x8B, 0x47, 0x04};   // eax = edi ? [edi+4] : -1  (merc class)
            p = emit(p, a, sizeof a);
            *p++ = 0x57;                                                                                         // push edi (merc unit)
            p = emit_push(p, two); p = emit_push(p, chk);
            *p++ = 0x50; *p++ = 0x56;                                                                            // push eax(class) ; push esi(item)
        } else {
            static const BYTE a[] = {0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0x85, 0xED, 0x74, 0x03, 0x8B, 0x4D, 0x0C};   // ecx = ebp ? [ebp+0xC] : -1
            p = emit(p, a, sizeof a);
            p = emit_rel(p, 0xE8, abs_call_target(code + m.cls_call));                                           // eax = merc class (the game's own lookup)
            *p++ = 0x55;                                                                                         // push ebp (merc unit)
            p = emit_push(p, two); p = emit_push(p, chk);
            *p++ = 0x50; *p++ = 0x53;                                                                            // push eax(class) ; push ebx(item)
        }
        p = emit_rel(p, 0xE8, (DWORD)qt_merc_allows);
        p = emit_tail(p, allowed, not_allowed);
        BYTE jmp[5]; jmp[0] = 0xE9; *(DWORD *)(jmp + 1) = (DWORD)st - (DWORD)(site + 5);
        patch_bytes(site, jmp, 5);
        logf_("merc equipment: patched %s +%lx (stub %p, item-type check %lx, two-hand check %lx)", name, (unsigned long)(site - base), (void *)st, (unsigned long)chk, (unsigned long)two);
        return 1;
    }
    logf_("merc equipment: the rule code was not found in %s (or appears more than once); not patched", name);
    return 0;
}


// ---- class-restricted items (companion of the rule above) -------------------------------------------------------
// D2Common's requirement check refuses every class-restricted item (Amazon bows, Paladin shields, Sorceress orbs) for a merc;
// at its Barbarian test we first ask merc_class_ok(). (Rogue quivers were dropped: the merc panel could not show them.)
#define D2COMMON_TIMESTAMP_113D 1318970339u
static int __stdcall qt_merc_class_ok(int type_id, int merc_class, int item_class) {
    int ok = merc_class_ok(type_id, merc_class, item_class);
    return ok;
}

static int patch_merc_class(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *code = base + sec->VirtualAddress; size_t len = sec->Misc.VirtualSize; merc_cls_t m;
        if (!sig_find_merc_class(code, len, &m)) continue;
        static const BYTE entry[] = {0x8B, 0x44, 0x24, 0x10, 0x83, 0xEC, 0x20};
        if (memcmp(code + m.site - 0x290, entry, sizeof entry)) { logf_("merc equipment: D2Common requirement check was modified by something else; not patching"); return 0; }
        BYTE *site = code + m.site;
        if (site[0] == 0xE9) { logf_("merc equipment: D2Common class check already patched"); return 1; }
        DWORD typefn = abs_call_target(code + m.type_call);
        BYTE *st = stub_alloc(); if (!st) return 0;
        BYTE *p = st;
        static const BYTE a[] = {0x83, 0xFE, 0x01};                  // cmp esi,1   (unit type: monster = a merc)
        p = emit(p, a, 3);
        BYTE *jne_fix = p; *p++ = 0x0F; *p++ = 0x85; p += 4;         // jne ORIG (patched below)
        *p++ = 0x57; p = emit_rel(p, 0xE8, typefn);                  // push edi (item) ; call item-type lookup -> eax
        *p++ = 0x53;                                                  // push ebx   (item class)
        static const BYTE b[] = {0xFF, 0x75, 0x04};                  // push [ebp+4] (merc class)
        p = emit(p, b, 3);
        *p++ = 0x50;                                                  // push eax   (item type)
        p = emit_rel(p, 0xE8, (DWORD)qt_merc_class_ok);
        static const BYTE t[] = {0x85, 0xC0};
        p = emit(p, t, 2);
        *p++ = 0x0F; *p++ = 0x85; *(DWORD *)p = (DWORD)(code + m.allowed) - (DWORD)(p + 4); p += 4;   // jne allowed
        *(DWORD *)(jne_fix + 2) = (DWORD)p - (DWORD)(jne_fix + 6);                                     // ORIG:
        static const BYTE o[] = {0x83, 0xFB, 0x04};                  // the displaced `cmp ebx,4`
        p = emit(p, o, 3);
        *p++ = 0x0F; *p++ = 0x85; *(DWORD *)p = (DWORD)(code + m.not_barb) - (DWORD)(p + 4); p += 4;   // jne (the game's own non-Barbarian path)
        emit_rel(p, 0xE9, (DWORD)(code + m.cont));
        BYTE jmp[5]; jmp[0] = 0xE9; *(DWORD *)(jmp + 1) = (DWORD)st - (DWORD)(site + 5);
        patch_bytes(site, jmp, 5);
        logf_("merc equipment: patched D2Common class check +%lx (Amazon bows, Paladin shields, orbs)", (unsigned long)(site - base));
        return 1;
    }
    logf_("merc equipment: the class check was not found in D2Common (or appears more than once); not patched");
    return 0;
}




// ---- merc dismissal: drop what the merc wears by playing the game's own requests -----------------------------------
// Doing "take off" + "drop" inside one server request leaves the client without the cursor update (it is sent late), so
// the client crashes. Instead, when the client sends the hire request (0x36) while the merc wears items and the cursor is
// empty, we hold it back, send the game's own "take item off the merc" (0x61 <slot>) and "drop cursor item" (0x17 <id>)
// requests one by one, a few hundred ms apart (like a player's clicks), then release the hire request.
static DWORD g_dg_base;
static volatile DWORD g_merc_unit;                    // the player's mercenary unit (D2Game memory), seen via the SetMode hook
typedef DWORD (__stdcall *dg_fn1)(DWORD);
typedef DWORD (__stdcall *dg_fn2)(DWORD, DWORD);
static DWORD dg_body_item(DWORD inv, DWORD loc) {
    if (!g_dg_base) return 0;
    DWORD it = ((dg_fn2)(g_dg_base + 0xac3a))(inv, loc);          // D2Common: item worn in a body location
    return (it && readable((const void *)it, 0x14) && *(DWORD *)it == 4) ? it : 0;
}
static struct {
    volatile int active; int n, idx, phase;
    struct { DWORD loc, guid; } job[10];
    DWORD next_ms, start_ms, phase_ms; uint8_t held[32]; int held_n, held_arg;
} g_dm;
static int dm_hire_class(DWORD u) {
    if (!u || !readable((const void *)u, 0x70) || *(DWORD *)u != 1) return 0;
    DWORD cls = *(DWORD *)(u + 4);
    for (int i = 0; i < g_hire_n; i++) if ((DWORD)g_hire_ids[i] == cls) return 1;
    return 0;
}
static int dm_begin(const void *pkt, int size, int arg, int send_arg_known) {
    if (!send_arg_known || size > (int)sizeof g_dm.held) return 0;
    DWORD u = g_merc_unit;
    if (!dm_hire_class(u)) return 0;
    DWORD mode = *(DWORD *)(u + 0x10);
    if (mode == 0 || mode == 12) return 0;                // dead: the game refuses 0x61; the server-side hook below drops its gear
    if (ap_cursor_busy()) { logf_("merc drop: an item is on your cursor; the merc's items are not dropped"); return 0; }
    DWORD inv = *(DWORD *)(u + 0x60);
    if (!inv || !readable((const void *)inv, 0x10)) return 0;
    int n = 0;
    for (DWORD loc = 1; loc <= 10 && n < 10; loc++) {
        DWORD it = dg_body_item(inv, loc);
        if (it) { g_dm.job[n].loc = loc; g_dm.job[n].guid = *(DWORD *)(it + 0xc); n++; }
    }
    if (!n) return 0;
    memcpy(g_dm.held, pkt, size); g_dm.held_n = size; g_dm.held_arg = arg;
    g_dm.n = n; g_dm.idx = 0; g_dm.phase = 0;
    g_dm.start_ms = GetTickCount(); g_dm.next_ms = g_dm.start_ms + 20;
    g_dm.active = 1; g_dm_active_flag = 1;
    logf_("merc drop: hire request held; dropping %d worn item(s) first", n);
    return 1;
}
static void dm_finish(void) {
    g_injecting = 1;
    int sr = o_send(g_dm.held_n, g_dm.held_arg, g_dm.held);
    g_injecting = 0;
    logf_("merc drop: hire request released (result %d)", sr);
    g_dm.active = 0; g_dm_active_flag = 0;
}
static void dm_step(DWORD now) {
    if (now - g_dm.start_ms > 10000) { logf_("merc drop: timed out; releasing the hire request"); dm_finish(); return; }
    if ((int)(now - g_dm.next_ms) < 0) return;
    if (g_dm.idx >= g_dm.n) { dm_finish(); return; }
    uint8_t pk[8]; int len;
    uint32_t cur = ap_cursor_id();
    if (g_dm.phase == 1 && !cur) {                         // the server announces the cursor item with a NEW id; wait for it
        if (now - g_dm.phase_ms > 3000) { logf_("merc drop: no cursor item arrived for slot %u; skipping it", (unsigned)g_dm.job[g_dm.idx].loc); g_dm.idx++; g_dm.phase = 0; g_dm.next_ms = now + 30; }
        return;
    }
    if (g_dm.phase == 0 && cur) {                          // the previous item is still on the cursor: wait for the drop to land
        if (now - g_dm.phase_ms > 3000) { logf_("merc drop: the cursor is not empty; stopping"); g_dm.idx = g_dm.n; }
        return;
    }
    if (g_dm.phase == 0) { pk[0] = 0x61; pk[1] = (uint8_t)g_dm.job[g_dm.idx].loc; pk[2] = 0; len = 3; g_dm.phase_ms = now; }
    else { pk[0] = 0x17; memcpy(pk + 1, &cur, 4); len = 5; g_dm.phase_ms = now; }
    g_injecting = 1;
    o_send(len, g_arg, pk);
    g_injecting = 0;
    if (g_dm.phase == 1) g_dm.idx++;
    g_dm.phase ^= 1;
    g_dm.next_ms = now + 30;                              // the real pacing is the waits for the cursor item above
}

// ---- dead merc: its gear is dropped by the server, inside the hire ---------------------------------------------------------
// The "take item off the merc" request (0x61) is refused for a dead merc (handler 6fc8c020 asks 6fc8d410 "is it dead?"), so the
// sequence above cannot run. Instead we hook the call that frees the replaced merc (D2Game +0xa73e3, inside the hire, section 9)
// and, if that merc is dead, put a COPY of each worn item on the ground at the player's feet with the game's own code for "you
// bought an item but your inventory is full" (6fc94e9a: copy 6fd02c40 -> free spot 6fd00000 -> place 6fcff970 with no dropper).
// The copy has a new id the client has never seen, so it simply appears like any new ground item; the originals are freed with
// the old merc right after, exactly as the game always did. Nothing here runs unless the hire really replaces the merc.
// The game functions use private register conventions; these cdecl thunks load the registers: qt_x(fn, args...).
__asm__(
    ".text\n"
    "_qt_isdead:\n"                     // (fn, unit) : eax = unit -> eax != 0 when the unit is dead
    "  movl 4(%esp), %edx\n  movl 8(%esp), %eax\n  call *%edx\n  ret\n"
    "_qt_dup:\n"                        // (fn, game, item) : eax = game, stdcall(item, 1) -> eax = the copy
    "  pushl %esi\n  pushl %ebx\n  pushl %edi\n  pushl %ebp\n"
    "  movl 0x14(%esp), %edx\n  movl 0x18(%esp), %eax\n  pushl $1\n  pushl 0x20(%esp)\n  call *%edx\n"
    "  popl %ebp\n  popl %edi\n  popl %ebx\n  popl %esi\n  ret\n"
    "_qt_spot:\n"                       // (fn, room, in, out) : esi = in coords, stdcall(room, out, 1) -> eax = the room of the free spot
    "  pushl %esi\n  pushl %ebx\n  pushl %edi\n  pushl %ebp\n"
    "  movl 0x14(%esp), %eax\n  movl 0x1c(%esp), %esi\n  pushl $1\n  pushl 0x24(%esp)\n  pushl 0x20(%esp)\n  call *%eax\n"
    "  popl %ebp\n  popl %edi\n  popl %ebx\n  popl %esi\n  ret\n"
    "_qt_place:\n"                      // (fn, item, room, game, x, y) : ecx = item, ebx = room, eax = y, stdcall(game, 0, x)
    "  pushl %esi\n  pushl %ebx\n  pushl %edi\n  pushl %ebp\n"
    "  movl 0x14(%esp), %edx\n  movl 0x18(%esp), %ecx\n  movl 0x1c(%esp), %ebx\n  movl 0x28(%esp), %eax\n"
    "  pushl 0x24(%esp)\n  pushl $0\n  pushl 0x28(%esp)\n  call *%edx\n"
    "  popl %ebp\n  popl %edi\n  popl %ebx\n  popl %esi\n  ret\n");
extern DWORD __cdecl qt_isdead(DWORD fn, DWORD unit);
extern DWORD __cdecl qt_dup(DWORD fn, DWORD game, DWORD item);
extern DWORD __cdecl qt_spot(DWORD fn, DWORD room, DWORD *in, DWORD *out);
extern void __cdecl qt_place(DWORD fn, DWORD item, DWORD room, DWORD game, DWORD x, DWORD y);
typedef void (__stdcall *dg_coords_t)(DWORD, DWORD *);
static void __stdcall qt_dead_merc_drop(DWORD game, DWORD merc, DWORD player) {
    DWORD b = g_dg_base;
    if (!b || !game || !player || !dm_hire_class(merc) || !qt_isdead(b + 0x6d410, merc)) return;   // a living merc was handled before the hire
    if (!readable((const void *)player, 0x70) || *(DWORD *)player != 0) return;
    DWORD inv = *(DWORD *)(merc + 0x60);
    if (!inv || !readable((const void *)inv, 0x10)) return;
    int n = 0, lost = 0;
    for (DWORD loc = 1; loc <= 10; loc++) {
        DWORD it = dg_body_item(inv, loc);
        if (!it) continue;
        DWORD in[2] = {0, 0}, out[2] = {0, 0};
        ((dg_coords_t)(b + 0xa4a2))(player, in);                                  // D2Common: the player's position
        DWORD room = ((dg_fn1)(b + 0xa412))(player);                               // D2Common: the player's room
        DWORD spot = room ? qt_spot(b + 0xe0000, room, in, out) : 0;               // a free spot next to the player
        if (!spot) { lost++; continue; }
        DWORD cp = qt_dup(b + 0xe2c40, game, it);                                  // a fresh copy (new id; sockets included)
        if (!cp) { lost++; continue; }
        qt_place(b + 0xdf970, cp, spot, game, out[0], out[1]);                     // on the ground (mode 3) in that room
        n++;
    }
    if (n || lost) logf_("merc drop: the merc is dead; dropped %d item(s) at your feet%s", n, lost ? " (some could not be placed)" : "");
}
static int patch_merc_free_hook(HMODULE g) {
    BYTE *site = (BYTE *)g + 0xa73df;
    static const BYTE expect[5] = {0x8B, 0xD7, 0x8B, 0xCE, 0xE8};                  // mov edx,edi (old merc); mov ecx,esi (game); call free-unit
    if (!readable(site, 9) || memcmp(site, expect, 5) || abs_call_target(site + 4) != (DWORD)g + 0x6fc70) {
        logf_("merc drop: the hire's merc-free call is not as expected; dead mercs' gear is not dropped");
        return 0;
    }
    BYTE *st = stub_alloc();
    if (!st) return 0;
    BYTE *p = st;
    *p++ = 0x60;                                                                   // pushad
    *p++ = 0x53; *p++ = 0x52; *p++ = 0x51;                                         // push ebx (player); push edx (old merc); push ecx (game)
    p = emit_rel(p, 0xE8, (DWORD)qt_dead_merc_drop);                               // stdcall: pops its 3 arguments
    *p++ = 0x61;                                                                   // popad
    emit_rel(p, 0xE9, (DWORD)g + 0x6fc70);                                         // on to the game's free-unit (the return address is still on the stack)
    BYTE call[5]; call[0] = 0xE8; *(DWORD *)(call + 1) = (DWORD)st - (DWORD)(site + 4 + 5);
    patch_bytes(site + 4, call, 5);
    logf_("merc drop: dead mercs' gear will be dropped too (hooked D2Game +a73e3)");
    return 1;
}



// D2Game's server equip action picks ONE body location for the item: the item type's first one (right hand), except the Iron Wolf's shield, which
// takes the second (left hand). Quivers list the same two locations as bows and shields, so a Rogue's quiver replaced her bow. At the Iron Wolf test
// we ask merc_slot2(): Iron Wolf + shield, or Rogue + quiver => take the SECOND location.
static int __stdcall qt_merc_slot2(DWORD item, int cls, DWORD chk) { return merc_slot2((merc_item_t)item, cls, (merc_chk)(UINT_PTR)chk); }
static int patch_merc_slot(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *code = base + sec->VirtualAddress; size_t len = sec->Misc.VirtualSize; merc_slot_t m;
        if (!sig_find_merc_slot(code, len, &m)) continue;
        BYTE *site = code + m.site;
        if (site[0] == 0xE9) { logf_("merc equipment: D2Game body-location choice already patched"); return 1; }
        DWORD chk = abs_call_target(code + m.chk_call);
        BYTE *st = stub_alloc(); if (!st) return 0;
        BYTE *p = st;
        static const BYTE a[] = {0x8B, 0x44, 0x24, 0x30};            // mov eax,[esp+0x30]   (the item being equipped)
        p = emit(p, a, 4);
        *p++ = 0x68; *(DWORD *)p = chk; p += 4;                      // push <item-type check>
        static const BYTE b[] = {0xFF, 0x76, 0x04};                  // push [esi+4]         (merc class)
        p = emit(p, b, 3);
        *p++ = 0x50;                                                  // push eax             (item)
        p = emit_rel(p, 0xE8, (DWORD)qt_merc_slot2);
        static const BYTE t[] = {0x85, 0xC0};
        p = emit(p, t, 2);
        *p++ = 0x0F; *p++ = 0x85; *(DWORD *)p = (DWORD)(code + m.use2) - (DWORD)(p + 4); p += 4;       // jne second location
        emit_rel(p, 0xE9, (DWORD)(code + m.use1));                                                     // first location
        BYTE patch[9]; patch[0] = 0xE9; *(DWORD *)(patch + 1) = (DWORD)st - (DWORD)(site + 5); memset(patch + 5, 0x90, 4);
        patch_bytes(site, patch, 9);
        logf_("merc equipment: patched D2Game body-location choice +%lx (second hand slot for quivers)", (unsigned long)(site - base));
        return 1;
    }
    logf_("merc equipment: the body-location choice was not found in D2Game (or appears more than once); not patched");
    return 0;
}



// ---- druid summons: remember what is alive, bring it back (free) when a game starts ------------------------------------------
// Every ~2 s (on the server's thread, driven by the packet dispatcher) the mod counts the live pets in the game's monster table (the pet classes come from
// Skills.txt `summon` + MonStats.txt) and keeps that list per character in qol_summons.txt, so the file always says what you had when you left. When a Druid
// enters a game it waits until her position is known, then replays the normal select + cast requests (spaced for the cast animation) with mana topped up before each
// request (the server tick sets stat 8 to stat 9), and finally leaves the mana full and re-selects the skill. Nothing is scanned or saved until the restore has finished, so it can't overwrite itself.
#define SUM_FILE "qol_summons.txt"
static char g_char[20]; static volatile int g_char_class = -1;               // read from the player unit on the server
static int g_saved_id[24], g_saved_cnt[24], g_saved_n;                        // what this character had (skill id, live pets)
static int g_sum_mon[24]; static volatile int g_sum_mon_ready;                 // MonStats class id of each summon's pet (-1 unknown)
static volatile int g_join_seq; static int g_join_seen;                       // bumped by the server tick whenever a new game/player appears
static volatile DWORD g_srv_player, g_srv_game;                               // the player unit and game (server memory), for the pet count and her animation mode
                                         // the player unit (server memory), for reading her animation mode
static volatile int g_refill;                                                 // 0 off, 1 refill mana before each request, 2 final top-up (set by the client side)
static volatile int g_scan_on; static DWORD g_scan_enable_at;                 // snapshots may only overwrite the file once the restore is over
static int g_cur_skill[2];                                                    // the skill the game last selected: [0] right hand, [1] left hand
static int sum_index(int id) { for (int i = 0; i < g_sum_n; i++) if (g_sum[i].id == id) return i; return -1; }

static int parse_petmon_cb(const char *txt, int len) {
    char names[24][24];
    for (int i = 0; i < g_sum_n; i++) snprintf(names[i], sizeof names[i], "%s", g_sum[i].mon);
    int n = ap_monster_ids(txt, len, (const char (*)[24])names, g_sum_n, g_sum_mon);
    if (n > 0) g_sum_mon_ready = 1;
    return n;
}
static void sum_file_load(void) {
    g_saved_n = 0;
    FILE *f = fopen(SUM_FILE, "r"); if (!f) return;
    char line[256], key[24]; snprintf(key, sizeof key, "%s=", g_char);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key))) continue;
        for (char *p = line + strlen(key); *p && g_saved_n < 24;) {
            int id = atoi(p), cnt = 1; while (*p && *p != ':' && *p != ',') p++;
            if (*p == ':') { p++; cnt = atoi(p); while (*p && *p != ',') p++; }
            if (*p == ',') p++;
            if (id > 0 && cnt > 0) { g_saved_id[g_saved_n] = id; g_saved_cnt[g_saved_n++] = cnt; }
        }
        break;
    }
    fclose(f);
}
static void sum_file_save(void) {
    char keep[8][256]; int nk = 0; char line[256]; size_t kl = strlen(g_char);
    FILE *f = fopen(SUM_FILE, "r");
    if (f) { while (nk < 8 && fgets(line, sizeof line, f)) if (strncmp(line, g_char, kl) || line[kl] != '=') snprintf(keep[nk++], 256, "%s", line); fclose(f); }
    f = fopen(SUM_FILE, "w"); if (!f) return;
    for (int i = 0; i < nk; i++) fputs(keep[i], f);
    fprintf(f, "%s=", g_char);
    for (int i = 0; i < g_saved_n; i++) fprintf(f, "%s%d:%d", i ? "," : "", g_saved_id[i], g_saved_cnt[i]);
    fputs("\n", f); fclose(f);
}

// -- server side: the packet dispatcher hook gives (game, player) on the server's thread --
static void sum_count_alive(DWORD game, int *alive, int on_server_thread) {                         // live pets per summon skill, from the game's monster table (128 chains at game+0x1320, next unit at +0xe4)
    for (int k = 0; k < 24; k++) alive[k] = 0;
    if (!readable((const void *)(game + 0x1320), 0x200)) return;
    for (int bkt = 0; bkt < 128; bkt++) {
        DWORD u = *(DWORD *)(game + 0x1320 + 4 * bkt);
        for (int hops = 0; u && hops < 3000; hops++, u = *(DWORD *)(u + 0xe4)) {
            if (!on_server_thread && !readable((const void *)u, 0xf0)) break;     // the game's own thread owns these chains, so it can walk them without the memory checks
            if (*(DWORD *)u != 1) continue;
            int cls = (int)*(DWORD *)(u + 4); DWORD mode = *(DWORD *)(u + 0x10);
            if (mode == 0 || mode == 12) continue;                           // dying / dead
            for (int k = 0; k < g_sum_n; k++) if (g_sum_mon[k] == cls && cls >= 0) alive[k]++;
        }
    }
}
// -- server side reset (logic and 1.13d addresses from PlugY's StatsPoints.cpp / SkillsPoints.cpp) --
typedef int (__stdcall *fn_stat3)(DWORD, DWORD, DWORD);
typedef void (__stdcall *fn_addstat)(DWORD, DWORD, int, DWORD);
typedef DWORD (__stdcall *fn_sklvl)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *fn_skcost)(DWORD, int, DWORD, DWORD);
typedef void (__stdcall *fn_setsk)(DWORD, DWORD, DWORD, DWORD, const char *, DWORD);
// Both game functions below use a private register convention in 1.13d (PlugY calls them "by EAX,ESI,EBX"; confirmed by disassembly of the crash):
static BYTE *call_charbin(void *fn, DWORD cls) {                              // D2Common +0x17B0: eax = class id, returns the charstats row
    BYTE *r; __asm__ volatile("call *%1" : "=a"(r) : "r"(fn), "0"(cls) : "ecx", "edx", "memory", "cc"); return r;
}
static void call_skclient(void *fn, DWORD client, DWORD unit, DWORD id, DWORD rem) {   // level is always 0 here
    DWORD f = (DWORD)fn, r = rem, c = client;                                             // ecx / edx are clobbered by the callee, so they carry the inputs
    __asm__ volatile("pushl %%edx\n\tpushl $0\n\tcall *%%ecx" : "+c"(f), "+d"(r), "+a"(c) : "S"(unit), "b"(id) : "memory", "cc");
}
static void respec_stats(DWORD u, HMODULE c) {
    fn_stat3 base = (fn_stat3)GetProcAddress(c, (LPCSTR)10216);               // D2GetPlayerBaseStat
    fn_addstat add = (fn_addstat)GetProcAddress(c, (LPCSTR)10645);            // D2AddPlayerStat
    BYTE *cs = call_charbin((BYTE *)c + 0x17B0, *(DWORD *)(u + 4));         // charstats row of the class
    if (!base || !add || !cs || !readable(cs, 0x50)) { logf_("respec: stats: functions not available"); return; }
    static const int stat_id[4] = {0, 2, 1, 3}, bin_off[4] = {0x30, 0x31, 0x32, 0x33};   // str, dex, ene, vit
    int total = 0;
    for (int k = 0; k < 4; k++) {
        int cur = base(u, stat_id[k], 0), rem = cur - cs[bin_off[k]];
        if (rem <= 0) continue;
        if (cur - rem < 1) rem = cur - 1;
        if (rem <= 0) continue;
        add(u, stat_id[k], -rem, 0);
        if (stat_id[k] == 3) { add(u, 7, -rem * (cs[0x46] << 6), 0); add(u, 11, -rem * (cs[0x47] << 6), 0); }   // vitality: max life + max stamina
        if (stat_id[k] == 1) add(u, 9, -rem * (cs[0x48] << 6), 0);                                              // energy: max mana
        total += rem;
    }
    if (total) add(u, 4, total, 0);
    logf_("respec: stats reset, %d points returned", total);
}
static void respec_skills(DWORD u, HMODULE c, HMODULE g) {
    fn_stat3 getstat = (fn_stat3)GetProcAddress(c, (LPCSTR)10550);           // D2GetPlayerStat
    fn_addstat add = (fn_addstat)GetProcAddress(c, (LPCSTR)10645);
    fn_sklvl sklvl = (fn_sklvl)GetProcAddress(c, (LPCSTR)10007);              // D2GetSkillLevel
    fn_skcost skcost = (fn_skcost)GetProcAddress(c, (LPCSTR)11081);           // D2GetSkillCost
    fn_setsk setsk = (fn_setsk)GetProcAddress(c, (LPCSTR)10335);              // D2SetSkillBaseLevel
    void *onclient = (BYTE *)g + 0xDD4F0;                                    // D2SetSkillBaseLevelOnClient (tells the client)
    DWORD pd = *(DWORD *)(u + 0x14), skills = *(DWORD *)(u + 0xA8);
    if (!getstat || !add || !sklvl || !skcost || !setsk || !readable((void *)pd, 0xA0) || !readable((void *)skills, 8)) { logf_("respec: skills: functions not available"); return; }
    DWORD client = *(DWORD *)(pd + 0x9C), cls = *(DWORD *)(u + 4);
    int total = 0;
    for (DWORD sk = *(DWORD *)(skills + 4); sk && readable((void *)sk, 0x40);) {
        DWORD next = *(DWORD *)(sk + 4), bin = *(DWORD *)sk;
        if (readable((void *)bin, 0x180) && *(BYTE *)(bin + 0xC) == cls && *(int *)(sk + 0x34) == -1) {
            DWORD id = *(WORD *)bin;
            DWORD keep = !getstat(u, 107, id);                                // keep the entry when gear gives +skill levels to it (stat 107 = item_singleskill)
            DWORD lvl = sklvl(u, sk, 0);
            int skpoints = *(int *)(bin + 0x170);
            if (skpoints == -1) { setsk(u, id, 0, keep, "qol", 0); total += (int)lvl; }
            else while (lvl--) { setsk(u, id, lvl, keep, "qol", 0); total += (int)skcost(u, skpoints, id, lvl); }
            call_skclient(onclient, client, u, id, keep);
        }
        sk = next;
    }
    if (total) add(u, 5, total, 0);
    logf_("respec: skills reset, %d points returned", total);
}
static void respec_run(DWORD player, int req) {
    HMODULE c = GetModuleHandleA("D2Common.dll"), g = GetModuleHandleA("D2Game.dll");
    if (!c || !g || !readable((void *)player, 0xB0)) return;
    if (req & 1) respec_stats(player, c);
    if (req & 2) respec_skills(player, c, g);
}
// The full-set glow: the game's own state "fullsetgeneric" (what a full Tal Rasha / Immortal King / Griswold / M'avina / Aldur set sets) switched on for a player of level >= min_level.
// D2Common #10706 = state test (unit, state), #10708 = state switch (unit, state, on), #11152 = marks the state for the clients' picture (unit, state, on; the game clears that mark after each update
// it sends, so it is NOT a test), #10550 = stat read (unit, stat, layer); stat 12 = level. First try (11152 alone) was cleared every frame: the log showed it being set again on every packet.
static void glow_apply(DWORD player) {
    static FARPROC get_stat, has_state, set_state, gfx_state;
    if (!get_stat) { HMODULE c = GetModuleHandleA("D2Common.dll"); if (!c) return;
        get_stat = GetProcAddress(c, (LPCSTR)(UINT_PTR)10550); has_state = GetProcAddress(c, (LPCSTR)(UINT_PTR)10706); set_state = GetProcAddress(c, (LPCSTR)(UINT_PTR)10708); gfx_state = GetProcAddress(c, (LPCSTR)(UINT_PTR)11152); }
    if (!get_stat || !has_state || !set_state || !gfx_state) return;
    int lvl = ((int (__stdcall *)(DWORD, DWORD, DWORD))get_stat)(player, 12, 0);
    static DWORD seen; if (seen != player) { seen = player; logf_("glow: first look at player %lx: level %d, needs %d, state %d, already set %d", (unsigned long)player, lvl, g_glow_min, g_glow_state, ((int (__stdcall *)(DWORD, DWORD))has_state)(player, (DWORD)g_glow_state)); }
    if (lvl < g_glow_min || lvl > 99) return;
    if (g_glow_baal) {                                                        // Hell Baal done? quest record of difficulty 2 = *(playerdata + 0x10 + 2*4); D2Common #10156 (record, quest, state) -> bool; quest 40 = Eve of Destruction, state 10 = completed (checked against saves: set once the NM/Normal Baal is beaten)
        static FARPROC quest_state; if (!quest_state) { HMODULE c = GetModuleHandleA("D2Common.dll"); if (c) quest_state = GetProcAddress(c, (LPCSTR)(UINT_PTR)10156); }
        DWORD pd = *(DWORD *)(player + 0x14), rec = 0;
        if (!quest_state || !pd || !readable((const void *)(pd + 0x18), 4) || !(rec = *(DWORD *)(pd + 0x18)) || !readable((const void *)rec, 8)) return;
        int done = ((int (__stdcall *)(DWORD, DWORD, DWORD))quest_state)(rec, 40, 10);
        static DWORD qseen; static int qlast = -1; if (qseen != player || qlast != done) { qseen = player; qlast = done; logf_("glow: Hell Baal quest %s", done ? "done" : "not done"); }
        if (!done) return;
    }
    if (((int (__stdcall *)(DWORD, DWORD))has_state)(player, (DWORD)g_glow_state)) return;
    ((void (__stdcall *)(DWORD, DWORD, DWORD))set_state)(player, (DWORD)g_glow_state, 1);
    ((void (__stdcall *)(DWORD, DWORD, DWORD))gfx_state)(player, (DWORD)g_glow_state, 1);
    logf_("glow: level %d player %lx got the full-set glow (state %d), state now reads %d", lvl, (unsigned long)player, g_glow_state, ((int (__stdcall *)(DWORD, DWORD))has_state)(player, (DWORD)g_glow_state));
}
// ---- Diablo Clone: a random event, and the chat command .dclone (for testing) -------------------------------------------------------------
// D2Game 1.13d 6fce8d70 (RVA 0xC8D70) = SpawnMonster(id in ecx, mode in edx, game, room1, x, y, 1, 0), callee pops 6 stack args (same function PlugY uses
// for the Uber minions and the world event; the entry bytes are checked before every use). The monster class is looked up by its MonStats id "diabloclone"
// (row 333 in a stock table). The game's own stats then come from that row's Normal / Nightmare / Hell columns. The player's room1 is *(*(unit+0x2c)+0x1c),
// the position two words of the path (+2 x, +6 y); the level number is room1 +0x10 -> room2 +0x58 -> level +0x1d0 (towns are skipped). Game difficulty = game+0x6D (PlugY's struct).
typedef DWORD (__fastcall *dc_spawn_t)(DWORD id, DWORD mode, DWORD game, DWORD room, DWORD x, DWORD y, DWORD one, DWORD zero);
static int dclone_spawn(DWORD game, DWORD player, const char *why) {
    HMODULE g = GetModuleHandleA("D2Game.dll"); if (!g || !readable((const void *)player, 0x30) || !readable((const void *)game, 0x80)) return 0;
    static const BYTE sig[11] = {0x83,0xEC,0x28,0x89,0x4C,0x24,0x0C,0x8B,0x4C,0x24,0x30};
    BYTE *fn = (BYTE *)g + 0xC8D70;
    if (!readable(fn, 11) || memcmp(fn, sig, 11)) { logf_("dclone: SpawnMonster is not where expected; not spawning"); return 0; }
    int diff = *(BYTE *)(game + 0x6D); if (diff < 0 || diff > 2) { logf_("dclone: odd difficulty byte %d; not spawning", diff); return 0; }
    DWORD path = *(DWORD *)(player + 0x2C); if (!readable((const void *)path, 0x20)) return 0;
    DWORD room = *(DWORD *)(path + 0x1C); if (!readable((const void *)room, 0x20)) return 0;
    DWORD r2 = *(DWORD *)(room + 0x10), lvl = r2 && readable((const void *)r2, 0x60) ? *(DWORD *)(r2 + 0x58) : 0;
    int lno = lvl && readable((const void *)lvl, 0x1D4) ? (int)*(DWORD *)(lvl + 0x1D0) : -1;
    if (lno <= 0 || lno > 136) { logf_("dclone: %s: could not read the level number (%d); not spawning, so he can never appear in a town by mistake", why, lno); return 0; }
    if (lno == 1 || lno == 40 || lno == 75 || lno == 103 || lno == 109) { logf_("dclone: %s: the player is in a town (level %d); not spawning", why, lno); return 0; }
    int x = *(WORD *)(path + 2), y = *(WORD *)(path + 6);
    static const int off[10][2] = {{6,0},{-6,0},{0,6},{0,-6},{5,5},{-5,-5},{5,-5},{-5,5},{3,0},{0,0}};
    int start = rand() % 8;
    dc_spawn_t spawn = (dc_spawn_t)(void *)fn;
    for (int k = 0; k < 10; k++) {
        int o = k < 8 ? (start + k) % 8 : k;
        DWORD u = spawn(g_dc_class, 1, game, room, (DWORD)(x + off[o][0]), (DWORD)(y + off[o][1]), 1, 0);
        if (u) { logf_("dclone: %s: spawned class %d at %d,%d (offset %d,%d) on difficulty %d, level %d, unit %lx", why, g_dc_class, x + off[o][0], y + off[o][1], off[o][0], off[o][1], diff, lno, (unsigned long)u); return 1; }
    }
    logf_("dclone: %s: SpawnMonster returned nothing for every offset (player at %d,%d, room %lx, difficulty %d)", why, x, y, (unsigned long)room, diff);
    return 0;
}
// The announcement is the game's own world-event packet (S->C 0x5A, 40 bytes: type, fct 0x12 = "Diablo Walks the Earth", id 4, param dword, word, 31 bytes), the layout PlugY
// sends for its Diablo Clone event, broadcast to every client of the game with D2Game 1.13d BroadcastFunction RVA 0xBB510 (eax = game, edi = callback, ebx = param) and
// SendPacket RVA 0xDB780 (eax = client, two stack args data and size). Both are wrapped by small stubs into fastcall form, exactly as PlugY's wrappers do.
typedef void (__fastcall *dc_bcast_t)(DWORD game, void *fn, void *param);
typedef void (__fastcall *dc_send_t)(void *client, const void *data, DWORD size);
static dc_bcast_t g_dc_bcast; static dc_send_t g_dc_send;
static void __fastcall dc_send_cb(void *client, DWORD *param) { if (g_dc_send && client) g_dc_send(client, (const void *)param[0], param[1]); }
static void dclone_announce(DWORD game) {
    if (!g_dc_bcast || !g_dc_send) { logf_("dclone: the announcement is not available (stubs not installed)"); return; }
    static BYTE pkt[40]; memset(pkt, 0, sizeof pkt); pkt[0] = 0x5A; pkt[1] = 0x12; pkt[2] = 4;
    DWORD param[2] = {(DWORD)pkt, sizeof pkt};
    g_dc_bcast(game, (void *)dc_send_cb, param);
    logf_("dclone: announcement sent to the clients of the game");
}
static void dclone_trigger(DWORD game, const char *why) {
    if (g_dc_class <= 0) { logf_("dclone: %s: class id not known yet; nothing done", why); return; }
    if (g_dc_pending) { logf_("dclone: %s: he is already on the way (the next super unique will be him)", why); return; }
    g_dc_pending = 1;
    logf_("dclone: %s: Diablo Clone will replace the next super unique the game creates", why);
    if (g_dc_announce) dclone_announce(game);
}
static void dclone_random(DWORD game, DWORD player) {                          // a clock starts when a game loads; every roll_minutes it rolls chance_per_roll percent
    static DWORD last_game, next; static int seeded;
    if (!seeded) { seeded = 1; srand(GetTickCount()); }
    DWORD now = GetTickCount();
    if (game != last_game) { last_game = game; g_dc_pending = 0; next = now + (DWORD)(g_dc_minutes * 60000.0); logf_("dclone: clock started for this game (first roll in %.2f min, %.4f%% per roll, difficulty byte %d)", g_dc_minutes, g_dc_chance, readable((const void *)game, 0x80) ? (int)*(BYTE *)(game + 0x6D) : -1); return; }
    if ((int)(now - next) < 0) return;
    next = now + (DWORD)(g_dc_minutes * 60000.0);
    if (!g_dc_chance || !readable((const void *)game, 0x80)) return;
    int diff = *(BYTE *)(game + 0x6D); if (diff < 0 || diff > 2 || !g_dc_diff[diff]) return;
    double roll = ((double)(((unsigned)rand() << 15) | ((unsigned)rand() & 0x7FFF)) / 1073741824.0) * 100.0;   // 0..100 with about 0.0000001 resolution
    if (roll >= g_dc_chance) return;
    (void)player; dclone_trigger(game, "random");
}
static void __cdecl qt_srv_tick(DWORD game, DWORD player, DWORD id, const unsigned char *buf, DWORD size) {
    static DWORD last_player, next;
    if ((g_respec || g_dclone) && id == 0x15 && size > 4 && size < 64 && readable(buf, size) && readable((const void *)player, 0xB0) && *(DWORD *)player == 0) {   // a chat line from ANY player: only a player unit resets itself
        int req = respec_parse(buf, (int)size);
        if (req == 3) { if (g_dclone) { logf_("dclone: .dclone typed in the chat"); dclone_trigger(game, "command"); } }
        else if (req == 4) { if (g_dclone) { logf_("dclone: .dclone now typed in the chat"); dclone_spawn(game, player, "command (now)"); } }
        else if (req && g_respec) {
            logf_("respec: %s requested by a player in the game", req == 1 ? "stats" : "skills");
            respec_run(player, req);
        }
    }
    if (g_dclone && g_dc_class > 0 && readable((const void *)player, 0xB0) && *(DWORD *)player == 0 && *(DWORD *)(player + 0xC) == local_player_id()) dclone_random(game, player);   // only our own character's packets drive the timer
    if (g_glow_on && g_glow_state > 0 && readable((const void *)player, 0xB0) && *(DWORD *)player == 0) glow_apply(player);   // every player's packets pass here: each one gets the glow
    if (player != last_player) {                                              // a new game: note who is playing
        if (!readable((const void *)player, 0x20) || *(DWORD *)player != 0) return;
        DWORD me = local_player_id();                                         // another player's packets (a Druid joining, say) must not replace "the player": only our own unit counts
        if (!me || *(DWORD *)(player + 0xC) != me) return;
        last_player = player; g_srv_player = player; g_srv_game = game;
        DWORD pd = *(DWORD *)(player + 0x14);
        memset(g_char, 0, sizeof g_char);
        if (pd && readable((const void *)pd, 16)) memcpy(g_char, (const void *)pd, 15);
        g_char_class = (int)*(DWORD *)(player + 4);
        g_scan_on = 0; g_scan_enable_at = 0; g_join_seq++;
        logf_("summons: character %s (class %d)", g_char, g_char_class);
        return;
    }
    if (g_refill && g_char_class == 5) {                                      // during the recast: full mana before each request, and a full globe at the end
        static FARPROC get_stat, set_stat; static int have;
        if (!get_stat) { HMODULE c = GetModuleHandleA("D2Common.dll"); if (c) { get_stat = GetProcAddress(c, (LPCSTR)(UINT_PTR)10550); set_stat = GetProcAddress(c, (LPCSTR)(UINT_PTR)10590); } }   // GetUnitStat / SetUnitStat
        if (get_stat && set_stat) {
            if (g_refill == 1) {
                have = 1;
                ((void (__stdcall *)(DWORD, DWORD, int, DWORD))set_stat)(player, 8, ((int (__stdcall *)(DWORD, DWORD, DWORD))get_stat)(player, 9, 0), 0);      // stat 8 = mana, 9 = max mana
            } else {
                if (have) {                                                  // finish with a full globe (the casts were free, so nothing is lost)
                    int maxm = ((int (__stdcall *)(DWORD, DWORD, DWORD))get_stat)(player, 9, 0);
                    ((void (__stdcall *)(DWORD, DWORD, int, DWORD))set_stat)(player, 8, maxm, 0);
                }
                have = 0; g_refill = 0;
            }
        } else g_refill = 0;
    }
    DWORD now = GetTickCount();
    if (!g_scan_on || (int)(now - next) < 0 || g_sum_n == 0 || !g_sum_mon_ready || g_char_class != 5 || !g_char[0]) return;
    next = now + 2000;
    if (!readable((const void *)(game + 0x1320), 0x200)) return;
    int alive[24]; sum_count_alive(game, alive, 1);
    int ids[24], cnt[24], n = 0;
    for (int k = 0; k < g_sum_n && n < 24; k++) if (alive[k] > 0) { ids[n] = g_sum[k].id; cnt[n++] = alive[k]; }
    int same = n == g_saved_n; for (int i = 0; same && i < n; i++) if (ids[i] != g_saved_id[i] || cnt[i] != g_saved_cnt[i]) same = 0;
    if (same) return;
    for (int i = 0; i < n; i++) { g_saved_id[i] = ids[i]; g_saved_cnt[i] = cnt[i]; }
    g_saved_n = n;
    sum_file_save();
}
static int patch_summon_hook(HMODULE g) {
    BYTE *site = (BYTE *)g + 0x674ae;
    static const BYTE call_site[3] = {0xFF, 0x14, 0xFD};
    DWORD table = (DWORD)g + 0xfa698;
    if (!readable(site, 7) || memcmp(site, call_site, 3) || *(DWORD *)(site + 3) != table) { logf_("summons: the packet dispatcher is not as expected; not installed"); return 0; }
    BYTE *st = stub_alloc();
    if (!st) return 0;
    BYTE *p = st;
    // at the call site: ecx = game, edx = player, edi = packet id, and the handler's two stack arguments (buffer, size) sit above the return address
    *p++ = 0x60;                                                              // pushad (buffer is now at [esp+0x24], size at [esp+0x28], the packet id (edi) at [esp])
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;                       // push dword [esp+0x28]   (size)
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;                       // push dword [esp+0x28]   (buffer: the first push moved esp by 4)
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;                       // push dword [esp+0x08]   (the packet id: edi as pushad saved it)
    *p++ = 0x52; *p++ = 0x51;                                                 // push edx (player); push ecx (game)
    p = emit_rel(p, 0xE8, (DWORD)qt_srv_tick);                                // qt_srv_tick(game, player, id, buffer, size), cdecl
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x14; *p++ = 0x61;                       // add esp,20; popad
    *p++ = 0xFF; *p++ = 0x24; *p++ = 0xFD; *(DWORD *)p = table; p += 4;       // jmp [edi*8 + table]  (the handler the game was about to call)
    BYTE patch[7]; patch[0] = 0xE8; *(DWORD *)(patch + 1) = (DWORD)st - (DWORD)(site + 5); patch[5] = patch[6] = 0x90;
    patch_bytes(site, patch, 7);
    logf_("summons: watching the live pets (installed in D2Game)");
    return 1;
}
// Diablo Clone hooks in D2Game 1.13d: (1) the only call to the super-unique spawn function (call at RVA 0xCFBCF -> 0xCDE20, the site PlugY patches for its event) goes through a stub that,
// while g_dc_pending is set, puts the class of "diabloclone" in ecx (the monster id register of that function) and returns 0 afterwards (no minions), like PlugY's spawnDClone;
// (2) the two wrapper stubs of the announcement (see dclone_announce).
static int dclone_install(HMODULE g) {
    BYTE *site = (BYTE *)g + 0xCFBCF, *orig = (BYTE *)g + 0xCDE20;
    if (!readable(site, 5) || site[0] != 0xE8 || (DWORD)(site + 5) + *(DWORD *)(site + 1) != (DWORD)orig) { logf_("dclone: the super unique call site is not as expected; the event is not installed"); return 0; }
    BYTE *st = stub_alloc(), *bc = stub_alloc(), *sn = stub_alloc();
    if (!st || !bc || !sn) return 0;
    BYTE *p = st;
    *p++ = 0x83; *p++ = 0x3D; *(DWORD *)p = (DWORD)&g_dc_pending; p += 4; *p++ = 0x00;          // cmp dword [pending],0
    *p++ = 0x74; BYTE *je = p++;                                                                    // je original
    *p++ = 0xC7; *p++ = 0x05; *(DWORD *)p = (DWORD)&g_dc_pending; p += 4; *(DWORD *)p = 0; p += 4;  // mov dword [pending],0
    *p++ = 0x8B; *p++ = 0x0D; *(DWORD *)p = (DWORD)&g_dc_class; p += 4;                              // mov ecx,[class]
    for (int i = 0; i < 5; i++) { *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x14; }             // push dword [esp+0x14] x5: copy the five stack arguments
    p = emit_rel(p, 0xE8, (DWORD)orig);                                                             // call original (it pops its 5 arguments)
    *p++ = 0x31; *p++ = 0xC0; *p++ = 0xC2; *p++ = 0x14; *p++ = 0x00;                                // xor eax,eax; ret 0x14
    *je = (BYTE)(p - (je + 1));
    p = emit_rel(p, 0xE9, (DWORD)orig);                                                             // original: jmp CDE20
    BYTE patch[5]; patch[0] = 0xE8; *(DWORD *)(patch + 1) = (DWORD)st - (DWORD)(site + 5);
    patch_bytes(site, patch, 5);
    BYTE *q = bc;                                                                                   // fastcall(game, fn, param) -> eax = game, edi = fn, ebx = param
    *q++ = 0x57; *q++ = 0x53; *q++ = 0x89; *q++ = 0xC8; *q++ = 0x89; *q++ = 0xD7; *q++ = 0x8B; *q++ = 0x5C; *q++ = 0x24; *q++ = 0x0C;
    q = emit_rel(q, 0xE8, (DWORD)g + 0xBB510);
    *q++ = 0x5B; *q++ = 0x5F; *q++ = 0xC2; *q++ = 0x04; *q++ = 0x00;
    q = sn;                                                                                          // fastcall(client, data, size) -> eax = client, push size, push data
    *q++ = 0x89; *q++ = 0xC8; *q++ = 0xFF; *q++ = 0x74; *q++ = 0x24; *q++ = 0x04; *q++ = 0x52;
    q = emit_rel(q, 0xE8, (DWORD)g + 0xDB780);
    *q++ = 0xC2; *q++ = 0x04; *q++ = 0x00;
    g_dc_bcast = (dc_bcast_t)(void *)bc; g_dc_send = (dc_send_t)(void *)sn;
    logf_("dclone: the super unique spawn is hooked (D2Game +cfbcf); the event is ready");
    return 1;
}
// ---- Terror Zone effects, server side (D2Game 1.13d) ---------------------------------------------------------------------------------------------
// Experience (stat 13) and gold (stat 14) are put on the player through D2Common #10590 SetUnitStat(unit, stat, value, layer) and #10645 AddPlayerStat(unit, stat, delta, layer) (stdcall; the kill-XP
// code around D2Game +8a2e4..+8a4f7 reads the base with #10216 and writes with #10590; gold pickups go through one of the two). Both are IAT-hooked in D2Game: for a player standing in the
// current terror zone, a gain is multiplied by (100 + pct) / 100. Losses (death, level-up resets) are never touched. Hell only unless hell_only=0 (game difficulty byte = game+0x6D).
static int (__stdcall *o_tz_set)(DWORD, DWORD, DWORD, DWORD), (__stdcall *o_tz_add)(DWORD, DWORD, int, DWORD), (__stdcall *o_tz_getbase)(DWORD, DWORD, DWORD);
static int unit_levelno(DWORD u) {
    if (!u || !readable((const void *)u, 0x30) || *(DWORD *)u != 0) return -1;                       // players only
    DWORD path = *(DWORD *)(u + 0x2C), room, r2, lvl;
    if (!readable((const void *)path, 0x20) || !(room = *(DWORD *)(path + 0x1C)) || !readable((const void *)room, 0x20)) return -1;
    if (!(r2 = *(DWORD *)(room + 0x10)) || !readable((const void *)r2, 0x60) || !(lvl = *(DWORD *)(r2 + 0x58)) || !readable((const void *)lvl, 0x1D4)) return -1;
    return (int)*(DWORD *)(lvl + 0x1D0);
}
static int tz_active(void) {                                                                          // false when the zone is spent, or when hell_only is set and the game is known not to be Hell
    if (tz_spent()) return 0;
    DWORD g = g_srv_game;
    if (!g_tz_hell_only || !g || !readable((const void *)g, 0x80)) return 1;
    return *(BYTE *)(g + 0x6D) == 2;
}
static int tz_here(DWORD unit) {                                                                      // is this player standing in the current terror zone?
    if (!g_tz || g_tz_n <= 0 || !tz_active()) return 0;
    int lno = unit_levelno(unit); if (lno <= 0) return 0;
    int z = tz_index_for((long long)(time(NULL) / ((long long)g_tz_minutes * 60)));
    for (int i = 0; i < g_tz_n; i++) if (g_tz_grp[i] == g_tz_grp[z] && g_tz_id[i] == lno) return 1;
    return 0;
}
static unsigned tz_mult(unsigned delta, int stat) {
    int pct = stat == 13 ? g_tz_xp : g_tz_gold; if (pct <= 0) return delta;
    unsigned long long v = (unsigned long long)delta * (100u + (unsigned)pct) / 100u;
    return v > 0x7FFFFFFFu ? 0x7FFFFFFFu : (unsigned)v;
}
static int __stdcall h_tz_set(DWORD unit, DWORD stat, DWORD value, DWORD layer) {
    if ((stat == 13 && g_tz_xp > 0) || (stat == 14 && g_tz_gold > 0)) {
        int old = o_tz_getbase ? o_tz_getbase(unit, stat, layer) : -1;
        if (old >= 0 && (int)value > old && tz_here(unit)) {
            unsigned nv = (unsigned)old + tz_mult((unsigned)((int)value - old), (int)stat);
            value = nv; }
    }
    return o_tz_set(unit, stat, value, layer);
}
static int __stdcall h_tz_add(DWORD unit, DWORD stat, int delta, DWORD layer) {
    if (delta > 0 && ((stat == 13 && g_tz_xp > 0) || (stat == 14 && g_tz_gold > 0)) && tz_here(unit)) {
        int nd = (int)tz_mult((unsigned)delta, (int)stat);
        delta = nd; }
    return o_tz_add(unit, stat, delta, layer);
}
// The zone's area level: Levels.txt MonLvlEx (the expansion monster level of the area, per difficulty) in D2Common's in-memory table. D2Common #10142 returns the level record (first word = the
// level id); MonLvlEx is the word at record +0x16 + 2 * difficulty (the same fields qol_ui's map overlay reads). The area's monsters are created at this level, and a monster's level sets
// the item level of its drops, so raising it raises both. Applied to the zone of the current clock slot (original saved and put back when the zone changes or the game is not Hell with
// hell_only), result = max(original + level_bonus, min_level) limited to max_level (item level tops out at 99). Only areas created after the change get monsters of the new level.
typedef struct { BYTE *rec; int id; WORD lvl_orig, lvl_set; int lvl; DWORD d_orig, d_set; int dens; BYTE u_orig[2], u_set[2]; BYTE *u_at[2]; int ue; } tzm_t;
static tzm_t g_tzm[12]; static int g_tzm_n, g_tzm_group = -1, g_tzm_diff = -1;               // the areas of the current zone that were changed, and what they were
static void tz_restore_member(tzm_t *m, int diff) {
    if (m->lvl && readable(m->rec, 0x1C) && *(WORD *)(m->rec + 0x16 + 2 * diff) == m->lvl_set) *(WORD *)(m->rec + 0x16 + 2 * diff) = m->lvl_orig;
    if (m->dens && readable(m->rec, 0x30) && *(DWORD *)(m->rec + 0x1C + 4 * diff) == m->d_set) *(DWORD *)(m->rec + 0x1C + 4 * diff) = m->d_orig;
    if (m->ue) for (int q = 0; q < 2; q++) if (m->u_at[q] && readable(m->u_at[q], 1) && *m->u_at[q] == m->u_set[q]) *m->u_at[q] = m->u_orig[q];
    logf_("terror zone: area %d put back (level %d%s%s)", m->id, m->lvl_orig, m->dens ? ", density" : "", m->ue ? ", elite packs" : "");
}
static int tz_apply_member(tzm_t *m, int ci, int diff, BYTE *(__stdcall *get_rec)(DWORD)) {            // candidate ci: raise its level, density, elite packs and colour its name; 1 = a record was found
    int id = g_tz_id[ci]; memset(m, 0, sizeof *m);
    BYTE *rec = get_rec((DWORD)id);
    if (!rec || !readable(rec, 0x1C) || *(WORD *)rec != (WORD)id) return 0;
    m->rec = rec; m->id = id;
    WORD *f = (WORD *)(rec + 0x16 + 2 * diff); int orig = *f; m->lvl_orig = (WORD)orig;
    if (orig > 0 && (g_tz_lvl_bonus > 0 || (diff == 2 && g_tz_lvl_min > 0))) {                         // the area level: original + bonus, and (Hell only) at least hell_min_level, never above the maximum
        int nl = orig + g_tz_lvl_bonus; if (diff == 2 && nl < g_tz_lvl_min) nl = g_tz_lvl_min; if (nl > (diff == 2 ? g_tz_lvl_max : 99)) nl = diff == 2 ? g_tz_lvl_max : 99; if (nl < orig) nl = orig;
        m->lvl_set = (WORD)nl; *f = (WORD)nl; m->lvl = 1;
        logf_("terror zone: area %d (%s) level %d -> %d", id, diff == 0 ? "Normal" : diff == 1 ? "Nightmare" : "Hell", orig, nl); }
    // density (MonDen, a DWORD per difficulty at record +0x1C) and elite packs (MonUMin / MonUMax, bytes at +0x28..+0x2D): the layout is recognised by comparing the record with what Levels.txt says
    if (readable(rec, 0x30)) {
        if (g_tz_dens_pct > 0 && g_tz_den[ci][0] >= 0 && *(DWORD *)(rec + 0x1C) == (DWORD)g_tz_den[ci][0] && *(DWORD *)(rec + 0x20) == (DWORD)g_tz_den[ci][1] && *(DWORD *)(rec + 0x24) == (DWORD)g_tz_den[ci][2]) {
            DWORD *d = (DWORD *)(rec + 0x1C + 4 * diff); m->d_orig = *d; m->d_set = (DWORD)(((unsigned long long)m->d_orig * (100u + (unsigned)g_tz_dens_pct) + 50) / 100); *d = m->d_set; m->dens = 1;
            logf_("terror zone: area %d density %lu -> %lu", id, (unsigned long)m->d_orig, (unsigned long)m->d_set); }
        else if (g_tz_dens_pct > 0) logf_("terror zone: area %d density not changed: the record at +0x1c is %lu %lu %lu, Levels.txt says %d %d %d", id, (unsigned long)*(DWORD *)(rec + 0x1C), (unsigned long)*(DWORD *)(rec + 0x20), (unsigned long)*(DWORD *)(rec + 0x24), g_tz_den[ci][0], g_tz_den[ci][1], g_tz_den[ci][2]);
        if (g_tz_elite_pct > 0) {
            const BYTE *u = rec + 0x28; int lay = 0;                                                  // layout A: min[3] then max[3]; layout B: min, max per difficulty
            int a = 1, b = 1; for (int d = 0; d < 3; d++) { if (u[d] != g_tz_umin[ci][d] || u[3 + d] != g_tz_umax[ci][d]) a = 0; if (u[2 * d] != g_tz_umin[ci][d] || u[2 * d + 1] != g_tz_umax[ci][d]) b = 0; }
            lay = a ? 1 : b ? 2 : 0;
            if (lay) { BYTE *pm = rec + 0x28 + (lay == 1 ? diff : 2 * diff), *px = rec + 0x28 + (lay == 1 ? 3 + diff : 2 * diff + 1);
                m->u_at[0] = pm; m->u_at[1] = px; m->u_orig[0] = *pm; m->u_orig[1] = *px;
                int nmx = (*px * (100 + g_tz_elite_pct) + 99) / 100; if (nmx < 1) nmx = 1; if (nmx > 255) nmx = 255;
                int nmn = (*pm * (100 + g_tz_elite_pct) + 99) / 100; if (nmn > nmx) nmn = nmx; if (nmn > 255) nmn = 255;
                m->u_set[0] = (BYTE)nmn; m->u_set[1] = (BYTE)nmx; *pm = m->u_set[0]; *px = m->u_set[1]; m->ue = 1;
                logf_("terror zone: area %d elite packs %d-%d -> %d-%d (layout %c)", id, m->u_orig[0], m->u_orig[1], nmn, nmx, lay == 1 ? 'A' : 'B'); }
            else logf_("terror zone: area %d elite packs not changed: the record at +0x28 is %d %d %d %d %d %d, Levels.txt says min %d %d %d max %d %d %d", id, u[0], u[1], u[2], u[3], u[4], u[5], g_tz_umin[ci][0], g_tz_umin[ci][1], g_tz_umin[ci][2], g_tz_umax[ci][0], g_tz_umax[ci][1], g_tz_umax[ci][2]); }
    }
    return 1;
}
static void tz_apply_level(void) {
    static BYTE *(__stdcall *get_rec)(DWORD); static DWORD last_run; DWORD tnow = GetTickCount();
    if (tnow - last_run < 250) return; last_run = tnow;                                                    // a few times a second is plenty
    if (!g_tz || g_tz_n <= 0) return;
    if (!get_rec) { HMODULE c = GetModuleHandleA("D2Common.dll"); if (c) get_rec = (BYTE *(__stdcall *)(DWORD))(void *)GetProcAddress(c, (LPCSTR)(UINT_PTR)10142); if (!get_rec) return; }
    DWORD g = g_srv_game; int diff = (g && readable((const void *)g, 0x80)) ? (int)*(BYTE *)(g + 0x6D) : -1;
    int want = -1;                                                                                         // the zone (its group number) that should be active now, or -1
    if (diff >= 0 && diff <= 2 && tz_active() && local_player_id()) want = g_tz_grp[tz_index_for((long long)(time(NULL) / ((long long)g_tz_minutes * 60)))];
    if (want == g_tzm_group && diff == g_tzm_diff) return;
    for (int i = 0; i < g_tzm_n; i++) tz_restore_member(&g_tzm[i], g_tzm_diff);                            // put the old zone back (if nobody else changed it meanwhile)
    g_tzm_n = 0; g_tzm_group = want; g_tzm_diff = diff;
    if (want >= 0) for (int ci = 0; ci < g_tz_n && g_tzm_n < 12; ci++) if (g_tz_grp[ci] == want && tz_apply_member(&g_tzm[g_tzm_n], ci, diff, get_rec)) g_tzm_n++;
}
// For other mods (qol_ui's map overlay): the Levels.txt id of the current terror zone, or 0 when there is none (feature off, not Hell with hell_only, no game yet). Plain cdecl export.
__declspec(dllexport) int qol_terror_zone(void) {                                                       // the first area of the current zone (kept for older qol_ui builds)
    if (!g_tz || g_tz_n <= 0 || !local_player_id() || !tz_active()) return 0;
    return g_tz_id[tz_index_for((long long)(time(NULL) / ((long long)g_tz_minutes * 60)))];
}
__declspec(dllexport) int qol_in_terror_zone(int level) {                                              // 1 when this area is part of the current zone (a zone can have several areas)
    if (!g_tz || g_tz_n <= 0 || !local_player_id() || !tz_active()) return 0;
    int z = tz_index_for((long long)(time(NULL) / ((long long)g_tz_minutes * 60)));
    for (int i = 0; i < g_tz_n; i++) if (g_tz_grp[i] == g_tz_grp[z] && g_tz_id[i] == level) return 1;
    return 0;
}
// Magic find: D2Game's drop code reads the player's magic find (stat 0x50 = item_magicbonus) through D2Common #10518 (unit, stat, layer; stdcall; the small helper at D2Game +0xE00F0 calls it for the
// killer and then for the killer's minion owner, +0xE0060 does the same for gold find, stat 0x4F). Hooked through D2Game's import table like the stat hooks above: when the unit is a player standing in
// the zone, the value is raised by magic_find_pct percent of itself (100 = doubled). Nothing is stored on the character, so nothing can be saved by accident. (Gold find is left alone: gold already gets xp/gold_pct.)
static int (__stdcall *o_tz_item)(DWORD, DWORD, DWORD);
static int __stdcall h_tz_item(DWORD unit, DWORD stat, DWORD layer) {
    int r = o_tz_item(unit, stat, layer);
    if (stat == 0x50 && g_tz_mf > 0 && r > 0 && tz_here(unit)) {
        long long nv = (long long)r * (100 + g_tz_mf) / 100; if (nv > 0x7FFFFFFF) nv = 0x7FFFFFFF;                  // a percentage OF the player's own magic find: 401 with magic_find_pct=100 -> 802
        static int once; if (!once) { once = 1; logf_("terror zone: magic find +%d%% is being applied to a drop in the zone (%d -> %d)", g_tz_mf, r, (int)nv); }
        r = (int)nv; }
    return r;
}
static void tz_install(HMODULE g) {
    HMODULE c = GetModuleHandleA("D2Common.dll"); if (!c || (!g_tz_xp && !g_tz_gold && !g_tz_mf)) return;
    o_tz_getbase = (int (__stdcall *)(DWORD, DWORD, DWORD))(void *)GetProcAddress(c, (LPCSTR)(UINT_PTR)10216);
    int a = hook_import_ordinal(g, "D2Common.dll", 10590, h_tz_set, (void **)&o_tz_set);
    int b = hook_import_ordinal(g, "D2Common.dll", 10645, h_tz_add, (void **)&o_tz_add);
    if (g_tz_mf > 0) logf_("terror zone: magic find hook in D2Game (#10518 item stat read: %d), +%d%%", hook_import_ordinal(g, "D2Common.dll", 10518, h_tz_item, (void **)&o_tz_item), g_tz_mf);
    logf_("terror zone: experience / gold hooks in D2Game (SetUnitStat %d, AddPlayerStat %d, base-stat reader %s): xp +%d%%, gold +%d%%, %s", a, b, o_tz_getbase ? "ok" : "MISSING", g_tz_xp, g_tz_gold, g_tz_hell_only ? "Hell only" : "all difficulties");
}
static DWORD WINAPI summon_thread(LPVOID arg) {
    (void)arg;
    if (!g_summons && !g_respec && !g_glow_on && !g_dclone && !g_tz) return 0;
    for (int i = 0; i < 6000; i++) {
        HMODULE g = GetModuleHandleA("D2Game.dll");
        if (g) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g + ((IMAGE_DOS_HEADER *)g)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != D2GAME_TIMESTAMP_113D) logf_("summons: this D2Game build is not supported; not installed");
            else { patch_summon_hook(g); if (g_dclone) dclone_install(g); if (g_tz) tz_install(g); }
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

// -- client side: bring them back --
static struct {
    int state;                                                                // 0 idle, 1 waiting for the game to be ready, 2 casting
    DWORD start, next, run_start, cast_t; int moved; int ent_id[24], ent_k[24], ent_target[24], ent_tries[24], ent_fail[24], ne, ei, phase, restore_skill, last_sel, saw_cast, before;
} g_rs;
static void sum_send(const uint8_t *pk, int len) { g_injecting = 1; o_send(len, g_arg, (void *)pk); g_injecting = 0; }
static void sum_select(int skill) { uint8_t pk[9] = {0x3c, (uint8_t)skill, (uint8_t)(skill >> 8), 0, 0, 0xff, 0xff, 0xff, 0xff}; sum_send(pk, 9); }
static void sum_finish(const char *why, DWORD now) {
    g_refill = 2;                                                             // the next request the server processes puts the mana back
    sum_select(g_rs.restore_skill ? g_rs.restore_skill : (g_rs.ne > 0 ? g_rs.ent_id[g_rs.ne - 1] : 0));    // also puts the selected skill back, and makes sure a request arrives
    g_rs.state = 0; g_sum_busy = 0; g_sum_hold_until = 0; g_scan_enable_at = now + 4000;                             // pets need a moment to appear before we start counting again
    logf_("summons: %s", why);
}
static void sum_on_send(const unsigned char *b, int size) {                   // remember which skill the game has selected, to put it back afterwards
    if (b[0] == 0x3c && size >= 9) { g_cur_skill[(b[4] & 0x80) ? 1 : 0] = b[1] | (b[2] << 8); g_rs.last_sel = 0; }   // the player changed skill: select ours again before the next cast
}
static void sum_step(DWORD now) {
    if (g_rs.state == 0 && g_join_seq != g_join_seen) {                       // a new game: is there something to bring back?
        g_join_seen = g_join_seq; g_cur_skill[0] = g_cur_skill[1] = 0;
        if (g_char_class == 5) {
            sum_file_load();
            if (g_saved_n > 0) { g_rs.state = 1; g_rs.start = now; logf_("summons: %s had %d kind(s) of summon last time; recasting once the game is ready", g_char, g_saved_n); }
            else { g_scan_on = 1; logf_("summons: nothing to bring back for %s", g_char); }
        }
    }
    if (g_rs.state == 0) { if (g_scan_enable_at && (int)(now - g_scan_enable_at) >= 0) { g_scan_enable_at = 0; g_scan_on = 1; } return; }
    if (g_arg < 0 || g_dm_active_flag) return;
    int x, y;
    if (g_rs.state == 1) {
        if (now - g_rs.start > 60000) { g_rs.state = 0; logf_("summons: gave up waiting for the game to be ready (the saved list is kept)"); return; }
        if (now - g_rs.start < 300 || !ap_player_pos(&x, &y) || g_sum_n == 0 || !g_srv_game) return;
        g_rs.ne = 0;
        for (int i = 0; i < g_saved_n; i++) {
            int k = sum_index(g_saved_id[i]); if (k < 0 || g_rs.ne >= 24) continue;
            int e = g_rs.ne++;
            g_rs.ent_id[e] = g_saved_id[i]; g_rs.ent_k[e] = k; g_rs.ent_tries[e] = 0; g_rs.ent_fail[e] = 0;
            g_rs.ent_target[e] = g_saved_cnt[i];                                     // aim for the old count; a cast that makes several pets (ravens) gets there in one go, one-per-cast pets (wolves, vines in a mod that allows several) take more
        }
        if (!g_rs.ne) { g_rs.state = 0; g_scan_on = 1; return; }
        g_refill = 1; g_sum_busy = 1;
        g_rs.restore_skill = g_cur_skill[0]; g_rs.last_sel = 0; g_rs.ei = 0; g_rs.phase = 0; g_rs.next = now; g_rs.run_start = now; g_rs.state = 2;
        logf_("summons: bringing back %d kind(s); mana is topped up before each cast and left full afterwards", g_rs.ne);
        return;
    }
    if (now - g_rs.run_start > 120000) { sum_finish("took too long; stopped", now); return; }
    if ((int)(now - g_rs.next) < 0) return;
    if (g_rs.ei >= g_rs.ne) { sum_finish("done", now); return; }
    if (!ap_player_pos(&x, &y)) return;
    int e = g_rs.ei, k = g_rs.ent_k[e], alive[24];
    DWORD pl = g_srv_player; int mode = -1;                                  // her animation mode: 1 / 5 = standing (town), 2 / 3 / 6 = walking or running, anything else = busy (casting, hit...)
    if (pl && readable((const void *)pl, 0x20)) mode = (int)*(DWORD *)(pl + 0x10);
    int moving = mode == 2 || mode == 3 || mode == 6, busy = mode >= 0 && !moving && mode != 1 && mode != 5;
    if (g_rs.phase == 0) {                                                   // cast: whatever she is doing, except in the middle of another animation
        static DWORD still_since;                                            // cast only once she has stood still for a moment; never interrupt walking / running
        if (mode >= 0 && mode != 1 && mode != 5) { still_since = 0; g_rs.next = now + 10; return; }
        if (!still_since) still_since = now;
        if (now - still_since < 150) { g_rs.next = now + 10; return; }
        (void)busy; (void)moving;
        sum_count_alive(g_srv_game, alive, 0); g_rs.before = alive[k];
        if (alive[k] >= g_rs.ent_target[e]) { g_rs.ei++; return; }           // already enough of this one
        int sk = g_rs.ent_id[e];
        if (sk != g_rs.last_sel) { sum_select(sk); g_rs.last_sel = sk; }
        uint8_t pk[5] = {0x0c, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8)}; sum_send(pk, 5);
        g_rs.ent_tries[e]++; g_rs.moved = moving;
        logf_("summons: cast skill %d (try %d, %d of %d wanted, mode %d)", sk, g_rs.ent_tries[e], alive[k], g_rs.ent_target[e], mode);
        g_rs.phase = 1; g_rs.cast_t = now; g_rs.next = now + 15;    } else {                                                                 // did the pet count go up? if not within about a second, cast again
        sum_count_alive(g_srv_game, alive, 0);
        if (alive[k] > g_rs.before) {                                        // got one
            g_sum_hold_until = 0;                                            // got one (or several): the extra select makes the server run its mana top-up right after the cast
            sum_select(g_rs.ent_id[e]); g_rs.ent_fail[e] = 0;
            if (alive[k] >= g_rs.ent_target[e]) g_rs.ei++;
            g_rs.phase = 0; g_rs.next = now;
        } else if (now - g_rs.cast_t > 1000) {                               // nothing appeared: ignored, interrupted by her running, or the limit is reached
            g_sum_hold_until = 0;
            if (!g_rs.moved) g_rs.ent_fail[e]++;                            // only casts from a standstill count against the pet
            if (g_rs.ent_fail[e] >= 3 || g_rs.ent_tries[e] >= g_rs.ent_target[e] + 8) g_rs.ei++;
            g_rs.phase = 0; g_rs.next = now;
        } else g_rs.next = now + 10;
    }
}
static void sum_on_recv(const uint8_t *b, int len) { (void)b; (void)len; }


// ---- hirelings may use Whirlwind-style skills against enemies already in melee reach ---------------------------------------------
// The skill-start function shared by Whirlwind-type skills (D2Game srvstfunc 38) cancels the skill when the caster's target is already within melee reach (it is meant
// to close distance) and sends the caster back to neutral to attack normally. A hireling only picks such a skill when an enemy is next to her, so it was cancelled every
// time (0 ms in the special-attack mode). For hirelings we skip that cancel; every other unit keeps the game's behaviour.
// A hireling starts the spin next to the enemy, so the path's destination (the target, a couple of units away) would end the spin after a step or two. Push the
// destination on past the enemy, with a random sideways angle each time, so the spin carries through the pack and zig-zags. Positions are in sub-tile units (5 per tile);
// path dwords +0x10/+0x14/+0x18 hold the destination as (y << 16) | x. If the new spot is not reachable the game simply falls back to a normal attack.
static void ww_extend_destination(DWORD unit) {
    DWORD path = *(DWORD *)(unit + 0x2c);
    if (!path || !readable((const void *)path, 0x20)) return;
    int px = (int)(*(DWORD *)path >> 16), py = (int)(*(DWORD *)(path + 4) >> 16);
    DWORD t = *(DWORD *)(path + 0x10);
    int tx = (int)(t & 0xFFFF), ty = (int)(t >> 16);
    double dx = tx - px, dy = ty - py, len = sqrt(dx * dx + dy * dy);
    static DWORD seed = 12345;
    seed = seed * 1103515245u + 12345u + GetTickCount();
    if (len < 1.0) { double a = (double)((seed >> 8) & 1023) * 6.2831853 / 1024.0; dx = cos(a); dy = sin(a); len = 1.0; }
    double ang = ((double)((seed >> 16) % 61) - 30.0) * 3.14159265 / 180.0;                      // -30 .. +30 degrees
    double ux = dx / len, uy = dy / len, c = cos(ang), sn = sin(ang);
    double vx = ux * c - uy * sn, vy = ux * sn + uy * c;
    double reach = len + 3.0; if (reach < 6.0) reach = 6.0; if (reach > 9.0) reach = 9.0;        // through the enemy and only a step past it (about 1-2 tiles total), never far from the fight
    int nx = px + (int)(vx * reach), ny = py + (int)(vy * reach);
    if (nx < 1 || ny < 1 || nx > 0xFFFE || ny > 0xFFFE) return;
    DWORD v = ((DWORD)ny << 16) | (DWORD)nx;
    *(DWORD *)(path + 0x10) = v; *(DWORD *)(path + 0x14) = v; *(DWORD *)(path + 0x18) = v;
    if (readable((const void *)path, 0x68)) *(DWORD *)(path + 0x58) = 0;                          // detach the enemy: a player's Whirlwind aims at a ground spot, and the game re-aimed ours at the enemy
}
static int __cdecl qt_is_hire_unit(DWORD unit) {                    // plain check, no side effects
    if (!unit || *(DWORD *)unit != 1) return 0;
    DWORD c = *(DWORD *)(unit + 4);
    for (int i = 0; i < g_hire_n; i++) if ((DWORD)g_hire_ids[i] == c) return 1;
    return 0;
}
static int __cdecl qt_is_hireling(DWORD unit) {
    if (!unit || *(DWORD *)unit != 1) return 0;
    DWORD c = *(DWORD *)(unit + 4);
    for (int i = 0; i < g_hire_n; i++) if ((DWORD)g_hire_ids[i] == c) { ww_extend_destination(unit); return 1; }
    return 0;
}
static int patch_merc_special(HMODULE g) {
    BYTE *base = (BYTE *)g;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *code = base + sec->VirtualAddress; size_t len = sec->Misc.VirtualSize; int patched = 0;
        long at = sig_find_merc_special(code, len, &patched);
        if (at < 0) continue;
        if (patched) { logf_("merc whirlwind: already patched"); return 1; }
        BYTE *site = code + at;                                           // 85 C0 (test eax,eax)  74 1C (je continue)  57 (push edi): 5 bytes
        DWORD cont = (DWORD)(site + 4 + 0x1C), after = (DWORD)(site + 5);     // continue = the normal start; after = the abort path (the instruction after the displaced push edi)
        BYTE *st = stub_alloc(); if (!st) return 0;
        BYTE *p = st;
        static const BYTE a[] = {0x85, 0xC0};                             // test eax,eax        (eax = "target is in melee reach")
        p = emit(p, a, 2);
        *p++ = 0x0F; *p++ = 0x84; *(DWORD *)p = cont - (DWORD)(p + 4); p += 4;   // je continue   (not in reach: the game's normal path)
        static const BYTE b[] = {0x60, 0x56};                             // pushad ; push esi (the caster)
        p = emit(p, b, 2);
        p = emit_rel(p, 0xE8, (DWORD)qt_is_hireling);                     // eax = 1 for a hireling
        static const BYTE c[] = {0x83, 0xC4, 0x04, 0x89, 0x44, 0x24, 0x1C, 0x61, 0x85, 0xC0};   // add esp,4 ; mov [esp+1C],eax ; popad ; test eax,eax
        p = emit(p, c, sizeof c);
        *p++ = 0x0F; *p++ = 0x85; *(DWORD *)p = cont - (DWORD)(p + 4); p += 4;   // jne continue  (a hireling: skip the cancel)
        *p++ = 0x57;                                                      // push edi            (the displaced instruction)
        emit_rel(p, 0xE9, after);                                         // on to the game's cancel path
        BYTE jmp[5]; jmp[0] = 0xE9; *(DWORD *)(jmp + 1) = (DWORD)st - (DWORD)(site + 5);
        patch_bytes(site, jmp, 5);
        logf_("merc whirlwind: patched D2Game +%lx (hirelings may start special attacks in melee reach)", (unsigned long)(site - base));
        {   // players' path collision mask for hirelings too, so the spin passes through monsters like a player's Whirlwind
            int mp = 0; long ma = sig_find_merc_mask(code, len, &mp);
            if (ma < 0) logf_("merc whirlwind: path collision code not found; hirelings keep the normal collision mask");
            else if (!mp) {
                BYTE *ms = code + ma, *back = ms + 5; BYTE *st2 = stub_alloc();
                if (st2) {
                    BYTE *q = st2;
                    static const BYTE m1[] = {0x05, 0x01, 0x3C, 0x00, 0x00, 0x60, 0x56};                  // add eax,3c01 (displaced) ; pushad ; push esi
                    q = emit(q, m1, sizeof m1);
                    q = emit_rel(q, 0xE8, (DWORD)qt_is_hire_unit);
                    static const BYTE m2[] = {0x83, 0xC4, 0x04, 0x85, 0xC0, 0x61, 0x74, 0x05, 0xB8, 0x09, 0x1C, 0x00, 0x00};   // add esp,4 ; test eax,eax ; popad ; je +5 ; mov eax,1c09
                    q = emit(q, m2, sizeof m2);
                    emit_rel(q, 0xE9, (DWORD)back);
                    BYTE j2[5]; j2[0] = 0xE9; *(DWORD *)(j2 + 1) = (DWORD)st2 - (DWORD)back;
                    patch_bytes(ms, j2, 5);
                    logf_("merc whirlwind: patched D2Game +%lx (hirelings pass through monsters like a player's Whirlwind)", (unsigned long)(ms - base));
                }
            }
        }
        return 1;
    }
    logf_("merc whirlwind: the skill-start code was not found in D2Game (or appears more than once); not patched");
    return 0;
}
// ---- wider hallways: mount qol.mpq (the re-edited Maggot Lair / Arcane Sanctuary map pieces) -----------------------------------------------
// Storm's SFileOpenArchive (#266: name, priority, flags, &handle; ret 0x10) adds an archive to the same list the game's own MPQs live in; a higher
// priority is searched first, so these map files override the stock ones. Loose Data\ files (with -direct) still win over every archive.
static DWORD WINAPI maps_thread(LPVOID arg) {
    (void)arg;
    if (!g_wide_halls) return 0;
    char path[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, path); lstrcatA(path, "\\qol.mpq");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { logf_("wide halls: %s not found; not loaded", path); return 0; }
    for (int i = 0; i < 600; i++) {
        HMODULE st = GetModuleHandleA("Storm.dll");
        if (st) {
            static const unsigned char s_arc[] = {0x8B, 0x4C, 0x24, 0x10, 0x81, 0xEC, 0x20, 0x01};   // #266 SFileOpenArchive
            FARPROC f;
            if (!storm_is(st, 266, s_arc, sizeof s_arc, &f)) { logf_("wide halls: Storm.dll not recognised; not loaded"); return 0; }
            HANDLE h = NULL;
            int ok = ((int(__stdcall *)(const char *, DWORD, DWORD, HANDLE *))f)(path, 9000, 0, &h);
            logf_("wide halls: %s %s (Maggot Lair and Arcane Sanctuary map pieces)", path, ok ? "mounted" : "FAILED to open");
            return 0;
        }
        Sleep(100);
    }
    return 0;
}
// ---- CPU fix: D2 spins a core at 100% because its main loops never idle ---------------------------------------------------
// (known bug; fixed elsewhere by replacing D2Client.dll / D2Win.dll). Equivalent in-memory edits, found by byte pattern and gated by PE timestamp:
//  - D2Client message pump (runs every frame): `push 0 ; call Sleep` -> `push 1` (Sleep(0) returns at once when nothing else wants the core).
//  - D2Win idle loop (menus): `je +2 ; xor eax,eax ; push eax ; call Sleep` (sleep 0 or 20 ms) -> `nop*3 ; push 1 ; call Sleep`.
//  - optional (throttle_render): in D2Client's frame limiter a skip-draw flag is set to 1 (ebp) instead of 0 (ebx) when no logic tick ran.
static BYTE *exec_section(HMODULE m, size_t *len, int idx) {
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && idx-- == 0) { *len = sec->Misc.VirtualSize; return base + sec->VirtualAddress; }
    return NULL;
}
static DWORD pe_stamp(HMODULE m) { BYTE *b = (BYTE *)m; return ((IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew))->FileHeader.TimeDateStamp; }
static DWORD WINAPI cpu_thread(LPVOID arg) {
    (void)arg;
    if (!g_cpu_fix) return 0;
    HMODULE c = NULL, w = NULL;
    for (int i = 0; i < 600 && !(c && w); i++) { c = GetModuleHandleA("D2Client.dll"); w = GetModuleHandleA("D2Win.dll"); if (!(c && w)) Sleep(100); }
    if (!c || !w) { logf_("cpu fix: D2Client/D2Win not loaded; not patched"); return 0; }
    if (pe_stamp(c) != 1318970890u) { logf_("cpu fix: D2Client is not stock 1.13d; not patched"); }
    else {
        size_t n; BYTE *code = exec_section(c, &n, 0); unsigned char st = 0;
        long at = code ? sig_find_cpu_pump(code, n, &st) : -1;
        if (at < 0) logf_("cpu fix: D2Client message pump not found (or not unique); not patched");
        else if (st == 1) logf_("cpu fix: D2Client message pump already patched");
        else { static const BYTE one = 1; patch_bytes(code + at, &one, 1); logf_("cpu fix: D2Client message pump patched (+%lx, Sleep(0) -> Sleep(1))", (unsigned long)(code + at - (BYTE *)c)); }
        if (g_cpu_throttle) {
            at = code ? sig_find_cpu_throttle(code, n, &st) : -1;
            if (at < 0) logf_("cpu fix: D2Client frame limiter not found (or not unique); throttle_render not applied");
            else if (st == 0x2D) logf_("cpu fix: throttle_render already applied");
            else { static const BYTE b = 0x2D; patch_bytes(code + at, &b, 1); logf_("cpu fix: throttle_render applied (+%lx)", (unsigned long)(code + at - (BYTE *)c)); }
        }
    }
    if (pe_stamp(w) != 1318970449u) { logf_("cpu fix: D2Win is not stock 1.13d; not patched"); }
    else {
        size_t n; BYTE *code = exec_section(w, &n, 0); int patched = 0;
        long at = code ? sig_find_cpu_win(code, n, &patched) : -1;
        if (at < 0) logf_("cpu fix: D2Win idle loop not found (or not unique); not patched");
        else if (patched) logf_("cpu fix: D2Win idle loop already patched");
        else { static const BYTE p[5] = {0x90, 0x90, 0x90, 0x6A, 0x01}; patch_bytes(code + at, p, 5); logf_("cpu fix: D2Win idle loop patched (+%lx, sleep 1 ms)", (unsigned long)(code + at - (BYTE *)w)); }
    }
    return 0;
}
static DWORD WINAPI special_thread(LPVOID arg) {
    (void)arg;
    if (!g_merc_special) return 0;
    for (int i = 0; i < 6000; i++) {
        HMODULE g = GetModuleHandleA("D2Game.dll");
        if (g) { patch_merc_special(g); return 0; }
        Sleep(100);
    }
    return 0;
}

static DWORD WINAPI equip_thread(LPVOID arg) {
    (void)arg;
    if (!g_merc_equip) return 0;
    int done_srv = 0, done_cli = 0, done_com = 0, warned = 0;
    for (int i = 0; i < 6000 && !(done_srv && done_cli && done_com); i++) {        // D2Client loads first, D2Game when a game starts
        if (!merc_rules_ready()) { Sleep(100); continue; }
        if (basemod_merc_active()) {
            if (!warned) logf_("merc equipment: BaseMod's [MercMod] is on (FullEquip / RogueCrossBow / RogueAmaBow); set them to 0 in BaseMod.ini to use [mercenary] equip. Not patching.");
            return 0;
        }
        HMODULE c = GetModuleHandleA("D2Client.dll"), g = GetModuleHandleA("D2Game.dll"), com = GetModuleHandleA("D2Common.dll");
        if (com && !done_com) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)com + ((IMAGE_DOS_HEADER *)com)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != D2COMMON_TIMESTAMP_113D) logf_("merc equipment: this D2Common build is not supported; class-restricted items not patched");
            else patch_merc_class(com);
            done_com = 1;
        }
        if (c && !done_cli) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)c + ((IMAGE_DOS_HEADER *)c)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != D2CLIENT_TIMESTAMP_113D) { logf_("merc equipment: this D2Client build is not supported; not patching"); done_cli = done_srv = 1; }
            else { patch_merc_module(c, 0); done_cli = 1; }
        }
        if (g && !done_srv) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g + ((IMAGE_DOS_HEADER *)g)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != D2GAME_TIMESTAMP_113D) { logf_("merc equipment: this D2Game build is not supported; not patching"); done_srv = 1; }
            else { patch_merc_module(g, 1); patch_merc_slot(g); done_srv = 1; }
        }
        Sleep(100);
    }
    return 0;
}

// ---- mercenary running fix (stock LoD 1.13c) -------------------------------------------------------------
// In that D2Game build the hireling AI always starts its follow/approach movement in WALK mode (mode 2), so a merc never
// runs unless it is fighting. D2Common's SetMode(unit, mode) applies the mode; this wrapper turns "walk" into "run"
// (monster mode 15) for hirelings, which then move at the Run speed of MonStats.txt. Only installed for the D2Game build
// with the bug (timestamp below, found by looking at your log: the merc only ever entered mode 2); PD2's own D2Game is a
// different (older) build that does not have it.
#define BUGGY_D2GAME_TIMESTAMP D2GAME_TIMESTAMP_113D
typedef int (__stdcall *setmode_t)(DWORD, int);
static setmode_t o_setmode;
static volatile LONG g_merc_conv;                              // walk->run conversions done (logged so the log shows the fix firing)
static int __stdcall h_setmode(DWORD unit, int mode) {
    // This runs for EVERY monster mode change (thousands per second in a fight): classify with plain reads only, and do the heavier work for the merc alone.
    DWORD c = 0; int hire = 0;
    if (unit && *(DWORD *)unit == 1) { c = *(DWORD *)(unit + 4); for (int i = 0; i < g_hire_n; i++) if ((DWORD)g_hire_ids[i] == c) { hire = 1; break; } }
    if (!hire) return o_setmode(unit, mode);
    if (g_merc_drop || g_merc_equip) g_merc_unit = unit;                                                // remember the mercenary's unit
    if (g_merc_run && mode == 2) {                                                                      // the merc asked to WALK: run instead
        mode = 15;
        LONG n = InterlockedIncrement(&g_merc_conv);
        if (n == 1) logf_("merc run fix: mercenary (class %u) asked to walk -> run (conversion #%ld)", (unsigned)c, (long)n);
    }
    return o_setmode(unit, mode);
}
static DWORD WINAPI merc_thread(LPVOID arg) {
    (void)arg;
    if (!g_merc_run && !g_merc_drop && !g_merc_equip) { logf_("merc run fix: off in the ini"); return 0; }
    for (int i = 0; i < 6000; i++) {                       // D2Game.dll loads when a game is started
        HMODULE g = GetModuleHandleA("D2Game.dll"), c = GetModuleHandleA("D2Common.dll");
        if (!g || !c) { Sleep(100); continue; }
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g + ((IMAGE_DOS_HEADER *)g)->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != BUGGY_D2GAME_TIMESTAMP) { logf_("merc run fix: this D2Game build is not affected, not installed"); return 0; }
        g_dg_base = (DWORD)g;
        if (g_merc_drop) patch_merc_free_hook(g);
        unsigned ord = 0;
        for (unsigned o = 10000; o < 11200 && !ord; o++) {
            BYTE *f = (BYTE *)GetProcAddress(c, (LPCSTR)(UINT_PTR)o);
            if (f && readable(f, 24) && sig_is_setmode(f)) ord = o;
        }
        if (!ord) { logf_("merc run fix: D2Common SetMode not found"); return 0; }
        logf_("merc run fix: hook D2Game IAT -> D2Common#%u SetMode: %d", ord, hook_import_ordinal(g, "D2Common.dll", ord, h_setmode, (void **)&o_setmode));
        return 0;
    }
    return 0;
}

#ifdef MERCDEBUG
// ---- TEMPORARY diagnostic (build with -DMERCDEBUG; not part of the shipped DLL) -----------------------------------
// Traces calls D2Game.dll makes into D2Common.dll. Each imported function gets a tiny stub that records the call and
// then jumps to the real function with the stack untouched. For every call we look at the first 4 arguments: if one is a
// pointer to a monster unit whose class is a hireling (class ids from MonStats.txt rows with AI=Hireable: 271, 338,
// 359, 561, 562), the call is tallied as (D2Common ordinal, call site in D2Game, argument index). Every ~2 s the tally
// of that window is written to mercdebug.log (then reset): a timeline of everything the game does *to the mercenary*.
// Press F9 to drop a marker line (start of each test phase).
static FILE *md_log;
static DWORD md_base, md_t0, md_last, md_key; static int md_mark, md_f9;
static struct { unsigned ord, ret, arg, cls, next, n; } md_tab[4096];
static struct { DWORD p; int v; } md_cache[8192];
static unsigned md_total, md_hits;
static int md_is_hire(DWORD p) {
    if (p < 0x10000 || (p & 3) || p > 0x7ff00000) return 0;
    unsigned h = (p >> 2) & 8191;
    if (md_cache[h].p == p) return md_cache[h].v;
    int v = 0;
    if (readable((const void *)p, 8)) {                        // VirtualQuery: never touches the page (IsBadReadPtr trips stack guard pages)
        DWORD type = *(DWORD *)p, cls = *(DWORD *)(p + 4);
        if (type == 1 && (cls == 271 || cls == 338 || cls == 359 || cls == 560 || cls == 561)) v = (int)cls;
    }
    md_cache[h].p = p; md_cache[h].v = v;
    return v;
}
static void md_dump(DWORD now) {
    int any = 0;
    for (int i = 0; i < 4096; i++) if (md_tab[i].n) {
        fprintf(md_log, "t=%lu mark=%d ord=%u site=+%05x arg%u class=%u next=%u n=%u\n", (unsigned long)((now - md_t0) / 1000), md_mark, md_tab[i].ord,
                (unsigned)(md_tab[i].ret - md_base), md_tab[i].arg, md_tab[i].cls, md_tab[i].next, md_tab[i].n);
        md_tab[i].n = 0; any = 1;
    }
    fprintf(md_log, "---- window end (t=%lu mark=%d calls=%u merc-calls=%u%s)\n", (unsigned long)((now - md_t0) / 1000), md_mark, md_total, md_hits, any ? "" : " none");
    fflush(md_log);
}
static void __cdecl md_trace(unsigned ord, unsigned ret, const DWORD *args) {
    md_total++;
    for (unsigned a = 0; a < 4; a++) {
        int cls = md_is_hire(args[a]);
        if (!cls) continue;
        md_hits++;
        unsigned nx = a < 3 ? args[a + 1] : 0;                 // the argument after the merc: a mode number if this sets the unit's mode
        if (nx > 0xFFFF) nx = 0xFFFF;                         // (pointers / big values collapse into one bucket)
        unsigned h = ((ord * 2654435761u) ^ (ret * 40503u) ^ (a * 977u) ^ (nx * 131u)) & 4095;
        for (int k = 0; k < 4096; k++, h = (h + 1) & 4095) {
            if (!md_tab[h].n) { md_tab[h].ord = ord; md_tab[h].ret = ret; md_tab[h].arg = a; md_tab[h].cls = (unsigned)cls; md_tab[h].next = nx; md_tab[h].n = 1; break; }
            if (md_tab[h].ord == ord && md_tab[h].ret == ret && md_tab[h].arg == a && md_tab[h].next == nx) { md_tab[h].n++; break; }
        }
    }
    DWORD now = GetTickCount();
    if (!md_t0) md_t0 = now;
    if (now - md_key >= 40) {                                // key state is a syscall: poll it at 25 Hz, not per traced call
        md_key = now;
        int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !md_f9) { md_mark++; fprintf(md_log, "==== MARK %d at t=%lu\n", md_mark, (unsigned long)((now - md_t0) / 1000)); fflush(md_log); }
        md_f9 = f9;
    }
    if (now - md_last > 2000) { md_last = now; md_dump(now); }
}
static int md_hook_all(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    BYTE *mem = VirtualAlloc(NULL, 1 << 17, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int used = 0, count = 0;
    if (!mem) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), "D2Common.dll")) continue;
        IMAGE_THUNK_DATA *oft = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *ft = (void *)(base + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (!(oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32) || used + 64 > (1 << 17)) continue;
            unsigned ord = oft->u1.Ordinal & 0xFFFF;
            BYTE *st = mem + used; used += 64;
            int p = 0;
            st[p++] = 0x60; st[p++] = 0x9C;                                   // pushad; pushfd
            st[p++] = 0x8D; st[p++] = 0x44; st[p++] = 0x24; st[p++] = 0x28;   // lea eax,[esp+0x28]  (address of the call's arguments)
            st[p++] = 0x50;                                                   // push eax
            st[p++] = 0x8B; st[p++] = 0x44; st[p++] = 0x24; st[p++] = 0x28;   // mov eax,[esp+0x28]  (return address)
            st[p++] = 0x50;                                                   // push eax
            st[p++] = 0x68; *(DWORD *)(st + p) = ord; p += 4;                 // push ordinal
            st[p++] = 0xE8; *(DWORD *)(st + p) = (DWORD)md_trace - (DWORD)(st + p + 4); p += 4;   // call md_trace
            st[p++] = 0x83; st[p++] = 0xC4; st[p++] = 0x0C;                   // add esp,12
            st[p++] = 0x9D; st[p++] = 0x61;                                   // popfd; popad
            st[p++] = 0xFF; st[p++] = 0x25; *(DWORD *)(st + p) = (DWORD)(st + p + 4); p += 4;     // jmp [cell]
            *(DWORD *)(st + p) = ft->u1.Function;                             // cell: the real function
            DWORD old; VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
            ft->u1.Function = (DWORD)st;
            VirtualProtect(&ft->u1.Function, 4, old, &old);
            count++;
        }
    }
    return count;
}

// Trace of the unit-free function (D2Game RVA 0x6fc70: ecx = game, edx = unit): logs a mercenary being freed with the
// D2Game return addresses found on the stack, so the "dismissed" case can be told apart from the game shutting down.
static void __cdecl md_freeunit(DWORD game, DWORD unit, const DWORD *sp) {
    int cls = md_is_hire(unit);
    if (!cls) return;
    fprintf(md_log, "FREE merc unit=%08x class=%d game=%08x t=%lu mark=%d stack:", (unsigned)unit, cls, (unsigned)game, (unsigned long)((GetTickCount() - md_t0) / 1000), md_mark);
    for (int i = 0; i < 48; i++) {
        if (!readable(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v > md_base && v < md_base + 0x100000) fprintf(md_log, " +%05x", (unsigned)(v - md_base));
    }
    fprintf(md_log, "\n"); fflush(md_log);
}
static void md_hook_free(HMODULE g) {
    BYTE *fn = (BYTE *)g + 0x6fc70;
    static const BYTE orig[5] = {0x55, 0x56, 0x57, 0x8B, 0xF9};
    if (memcmp(fn, orig, 5)) { fprintf(md_log, "free-unit entry bytes differ; not traced\n"); fflush(md_log); return; }
    BYTE *st = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int p = 0;
    st[p++] = 0x60; st[p++] = 0x9C;                                       // pushad; pushfd
    st[p++] = 0x8D; st[p++] = 0x44; st[p++] = 0x24; st[p++] = 0x24;       // lea eax,[esp+0x24]  (return address of the original call)
    st[p++] = 0x50;                                                       // push sp
    st[p++] = 0x52;                                                       // push edx (unit)
    st[p++] = 0x51;                                                       // push ecx (game)
    st[p++] = 0xE8; *(DWORD *)(st + p) = (DWORD)md_freeunit - (DWORD)(st + p + 4); p += 4;
    st[p++] = 0x83; st[p++] = 0xC4; st[p++] = 0x0C;                       // add esp,12
    st[p++] = 0x9D; st[p++] = 0x61;                                       // popfd; popad
    memcpy(st + p, orig, 5); p += 5;                                      // the displaced instructions
    st[p++] = 0xE9; *(DWORD *)(st + p) = (DWORD)(fn + 5) - (DWORD)(st + p + 4); p += 4;
    DWORD old; VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9; *(DWORD *)(fn + 1) = (DWORD)st - (DWORD)(fn + 5);
    VirtualProtect(fn, 5, old, &old);
    fprintf(md_log, "traced the unit-free function\n"); fflush(md_log);
}
static DWORD WINAPI md_thread(LPVOID arg) {
    (void)arg;
    md_log = fopen("mercdebug.log", "w");
    if (!md_log) return 0;
    for (int i = 0; i < 6000; i++) {                       // D2Game.dll loads when a game is started
        HMODULE g = GetModuleHandleA("D2Game.dll");
        if (g) {
            md_base = (DWORD)g;
            int n = md_hook_all(g);
            fprintf(md_log, "traced %d D2Common imports of D2Game (base %p)\n", n, (void *)g); fflush(md_log);
            md_hook_free(g);
            return 0;
        }
        Sleep(100);
    }
    return 0;
}
#endif


static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    ap_config cfg;
    load_config(&cfg);
#ifdef MERCDEBUG
    g_logging = 1; cfg.verbose = 1;
#endif
    if (g_logging) {
        g_log = fopen("qol.log", "w");
        if (g_log) { setvbuf(g_log, NULL, _IOFBF, 1 << 16); CreateThread(NULL, 0, log_flusher, NULL, 0, NULL); }
    }
#ifdef MERCDEBUG
    CreateThread(NULL, 0, md_thread, NULL, 0, NULL);
#endif
    CreateThread(NULL, 0, merc_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, equip_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, summon_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, special_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, maps_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, cpu_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, patch_thread, NULL, 0, NULL);
    QueryPerformanceFrequency(&g_freq);
    logf_("qoltweaks loaded: enabled=%d range=%d rejuv_always=%d", cfg.enabled, cfg.range, cfg.rejuv_always);
    logf_("clicker: enabled=%d key=%d hold_delay=%dms interval=%dms", g_clk_enabled, g_clk_key, g_clk_delay, g_clk_interval);
    if (g_clk_enabled) CreateThread(NULL, 0, clicker, NULL, 0, NULL);
    ap_init(&cfg, g_logging ? core_log : NULL);

    HMODULE client = NULL;
    for (int i = 0; i < 600 && !(client = GetModuleHandleA("D2Client.dll")); i++) Sleep(50);
    HMODULE net = GetModuleHandleA("D2Net.dll");
    if (!client || !net) { logf_("D2Client/D2Net not loaded; giving up"); return 0; }
    // D2Net export ordinals differ between builds (PD2 vs stock LoD), so find the functions by code signature.
    unsigned send_ord = 0, recv_ord = 0;
    void *send = NULL;
    for (unsigned o = 10000; o < 10100; o++) {
        BYTE *f = (BYTE *)GetProcAddress(net, (LPCSTR)(UINT_PTR)o);
        if (!f || !readable(f, 26)) continue;
        if (!send_ord && sig_is_send(f)) { send_ord = o; send = f; }
        if (!recv_ord && sig_is_recv(f)) recv_ord = o;
    }
    logf_("D2Net: send=#%u recv=#%u", send_ord, recv_ord);
    if (!send || !recv_ord) { logf_("D2Net functions not recognised (unsupported game version?); giving up"); return 0; }
    logf_("hook send (inline D2Net#%u @%p): %d", send_ord, send, inline_hook(send, h_send, (void **)&o_send));
    logf_("hook recv (IAT D2Client->D2Net#%u): %d", recv_ord, hook_import_ordinal(client, "D2Net.dll", recv_ord, h_recv, (void **)&o_recv));
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res) {
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
