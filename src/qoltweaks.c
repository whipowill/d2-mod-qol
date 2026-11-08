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
#include "core.h"
#include "sig.h"
#include "merc.h"

static FILE *g_log;
static int g_merc_special = 0;                                   // ini: [mercenary] whirlwind
static int g_summons = 0;                                        // ini: [summons] enabled
static int g_merc_drop = 0;                                      // ini: [mercenary] drop_items
static int g_merc_equip = 0;                                    // ini: [mercenary] equip
#define D2GAME_TIMESTAMP_113D 1318970639u    // D2Game.dll of stock LoD 1.13d (Oct 2011)
static int g_logging = 0;        // ini: [qol] logging=0 turns off the log file, decision logging and profiling
static volatile LONG g_lines;
#define MAX_LINES 20000

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
    static int tries, done_misc, done_mon, done_it, done_sk, done_pm; static DWORD next;
    if (!g_merc_equip) done_it = 1;
    if (!g_summons) done_sk = done_pm = 1;
    if ((done_misc && done_mon && done_it && done_sk && done_pm) || tries >= 30) return;
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
    if (g_summons) sum_step(GetTickCount());
    if (g_dm_active_flag) dm_step(GetTickCount());
    live_player_pos();
    uint8_t out[16];
    int n = g_dm_active_flag ? 0 : ap_tick(GetTickCount(), out);
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
static int g_wide_halls = 0;                                    // ini: [maps] wide_halls
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
                  "; 1 = pick up every rejuvenation potion even if your belt has no room for it (it goes to your inventory)\r\n"
                  "rejuv_always=1\r\n"
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
                  "[maps]\r\n"
                  "; 1 = wider hallways in the Maggot Lair and the Arcane Sanctuary (map files from qol.mpq, which must sit next to qol.dll).\r\n"
                  "; Off by default. Takes effect for areas generated after the game starts.\r\n"
                  "wide_halls=1\r\n"
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
    g_summons = master && GetPrivateProfileIntA("summons", "enabled", 1, path);
    g_wide_halls = master && GetPrivateProfileIntA("maps", "wide_halls", 1, path);
    g_merc_special = master && GetPrivateProfileIntA("mercenary", "whirlwind", 1, path);
    g_xp_nopenalty = master && GetPrivateProfileIntA("experience", "no_level_penalty", 1, path);
    g_keep_equip = master && GetPrivateProfileIntA("death", "keep_equipment", 1, path);
    c->enabled = master && GetPrivateProfileIntA("pickup", "enabled", 1, path);
    c->range = GetPrivateProfileIntA("pickup", "range", 4, path);
    c->rejuv_always = GetPrivateProfileIntA("pickup", "rejuv_always", 1, path);
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
    if (!g_xp_nopenalty && !g_keep_equip) return 0;
    for (int i = 0; i < 6000; i++) {                       // D2Game.dll loads when a game is started
        HMODULE g = GetModuleHandleA("D2Game.dll");
        if (!g) { Sleep(100); continue; }
        BYTE *base = (BYTE *)g;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        int xp_done = !g_xp_nopenalty, keep_done = !g_keep_equip;
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
        }
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
static void __cdecl qt_srv_tick(DWORD game, DWORD player) {
    static DWORD last_player, next;
    if (player != last_player) {                                              // a new game: note who is playing
        if (!readable((const void *)player, 0x20) || *(DWORD *)player != 0) return;
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
    *p++ = 0x60; *p++ = 0x52; *p++ = 0x51;                                    // pushad; push edx (player); push ecx (game)
    p = emit_rel(p, 0xE8, (DWORD)qt_srv_tick);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08; *p++ = 0x61;                       // add esp,8; popad
    *p++ = 0xFF; *p++ = 0x24; *p++ = 0xFD; *(DWORD *)p = table; p += 4;       // jmp [edi*8 + table]  (the handler the game was about to call)
    BYTE patch[7]; patch[0] = 0xE8; *(DWORD *)(patch + 1) = (DWORD)st - (DWORD)(site + 5); patch[5] = patch[6] = 0x90;
    patch_bytes(site, patch, 7);
    logf_("summons: watching the live pets (installed in D2Game)");
    return 1;
}
static DWORD WINAPI summon_thread(LPVOID arg) {
    (void)arg;
    if (!g_summons) return 0;
    for (int i = 0; i < 6000; i++) {
        HMODULE g = GetModuleHandleA("D2Game.dll");
        if (g) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g + ((IMAGE_DOS_HEADER *)g)->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != D2GAME_TIMESTAMP_113D) logf_("summons: this D2Game build is not supported; not installed");
            else patch_summon_hook(g);
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
