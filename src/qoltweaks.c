// qol.dll (built from qoltweaks) - Diablo II (1.13-style, e.g. LoD / Project Diablo 2) quality-of-life features and fixes (single player): potion/scroll/key
// auto-pickup, hold-to-click, mercenary running fix, no XP level penalty, keep equipment on death. Loaded via d2gl.json "load_dlls_late" or PlugY DllToLoad.
//
// Hooks the game's packet layer (D2Net), feeds every packet to the platform-independent logic in core.c,
// and when the logic decides a potion should be picked up, sends the same 0x16 packet a mouse click would.
//   - send hook:    inline detour of D2Net!#10024 (SendPacket(size, arg, packet))   -> sees your actions
//   - receive hook: IAT hook of D2Client's D2Net#10008 (server->client packets)     -> sees items/belt/position
// Config: qol.ini next to the game exe (created with defaults on first run).
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "core.h"
#include "sig.h"
#include "merc.h"

static FILE *g_log;
static int g_merc_drop = 0;                                      // ini: [mercenary] drop_items
static int g_merc_equip = 0;                                    // ini: [mercenary] equip
#define D2GAME_TIMESTAMP_113D 1318970639u    // D2Game.dll of stock LoD 1.13d (Oct 2011)
static int g_logging = 1;        // ini: [qol] logging=0 turns off the log file, decision logging and profiling
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

// Called from the game thread (Storm's file functions are the game's own, so use them where the game does).
// Retries for a while in case the archives aren't all mounted yet; gives up quietly after that.
static void misc_tick(void) {
    static int tries, done_misc, done_mon, done_it; static DWORD next;
    if (!g_merc_equip) done_it = 1;
    if ((done_misc && done_mon && done_it) || tries >= 30) return;
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
    if (!done_it) {
        int n = storm_read("Data\\global\\excel\\ItemTypes.txt", parse_itemtypes_cb);
        if (n >= 0 || n == -2) done_it = 1;
        if (n > 0) logf_("ItemTypes.txt: %d item types (attempt %d), merc rules %s", n, tries, merc_rules_ready() ? "ready" : "NOT ready (missing types)");
    }
}

static int g_arg = -1;           // 2nd argument the game itself uses for gameplay packets (learned at runtime)

static int dm_begin(const void *pkt, int size, int arg, int send_arg_known);
static void dm_step(DWORD now);
static volatile int g_dm_active_flag;               // mirrors g_dm.active for the hooks above its definition
static int g_last_send_r = 1;                        // result of the game's most recent real send
static int __stdcall h_send(int size, int arg, void *pkt) {
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
    }
    misc_tick();
    if (g_dm_active_flag) dm_step(GetTickCount());
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
                  "logging=1\r\n"
                  "\r\n"
                  "[pickup]\r\n"
                  "; potion auto-pickup: 1 = on, 0 = off\r\n"
                  "enabled=1\r\n"
                  "; pick up potions within this many tiles of the player\r\n"
                  "range=4\r\n"
                  "; 1 = pick up every rejuvenation potion even if your belt has no room for it (it goes to your inventory)\r\n"
                  "rejuv_always=0\r\n"
                  "\r\n"
                  "[death]\r\n"
                  "; 1 = keep your equipment when you die (the \"KeepEquip\" hex edit by SnakeByteStudios, applied in memory). Single player.\r\n"
                  "; Off by default.\r\n"
                  "keep_equipment=0\r\n"
                  "\r\n"
                  "[experience]\r\n"
                  "; 1 = no XP penalty for level differences, in either direction: a high-level character still gets full XP from\r\n"
                  "; low-level monsters, and a low-level character still gets full XP from high-level monsters (e.g. when partied with a\r\n"
                  "; high-level character). Single player. Off by default.\r\n"
                  "no_level_penalty=0\r\n"
                  "\r\n"
                  "[mercenary]\r\n"
                  "; 1 = mercenaries run when they follow you instead of always walking (fixes a bug in stock LoD 1.13d).\r\n"
                  "; Off by default. Only does anything on stock LoD 1.13d.\r\n"
                  "run=0\r\n"
                  "; 1 = mercenaries can equip a wider range of items: Rogue: bows, crossbows; Desert: polearms, spears (also Amazon),\r\n"
                  "; javelins; Iron Wolf: shields, 1H swords, wands, scepters, maces, orbs; Barbarian: swords, axes, maces, hammers,\r\n"
                  "; Barbarian helms; every merc: rings, amulets, boots, gloves, belts. Replaces BaseMod's [MercMod] options: set\r\n"
                  "; FullEquip=0, RogueCrossBow=0, RogueAmaBow=0 in BaseMod.ini or this stays off. Stock LoD 1.13d only.\r\n"
                  "equip=0\r\n"
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
    g_merc_run = master && GetPrivateProfileIntA("mercenary", "run", 0, path);
    g_merc_equip = master && GetPrivateProfileIntA("mercenary", "equip", 0, path);
    g_merc_drop = master && GetPrivateProfileIntA("mercenary", "drop_items", 0, path);
    g_xp_nopenalty = master && GetPrivateProfileIntA("experience", "no_level_penalty", 0, path);
    g_keep_equip = master && GetPrivateProfileIntA("death", "keep_equipment", 0, path);
    c->enabled = master && GetPrivateProfileIntA("pickup", "enabled", 1, path);
    c->range = GetPrivateProfileIntA("pickup", "range", 4, path);
    c->rejuv_always = GetPrivateProfileIntA("pickup", "rejuv_always", 0, path);
    g_logging = GetPrivateProfileIntA("qol", "logging", 1, path);
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
    static int logged;                                     // only reached when the merc already meets the item's strength / dexterity / level
    if (g_logging && logged < 30) { logged++; logf_("merc class test: merc class %d, item type %d, item class %d -> %s", merc_class, type_id, item_class, ok ? "allowed" : "refused"); }
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
    int sr = o_send(len, g_arg, pk);
    g_injecting = 0;
    logf_("merc drop: sent %s for slot %u item %08x (result %d)", g_dm.phase == 0 ? "take-off" : "drop", (unsigned)g_dm.job[g_dm.idx].loc, (unsigned)(g_dm.phase == 0 ? g_dm.job[g_dm.idx].guid : cur), sr);
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
    if ((g_merc_drop || g_merc_equip) && unit && readable((const void *)unit, 8) && *(DWORD *)unit == 1) {          // remember the mercenary's unit
        DWORD c = *(DWORD *)(unit + 4);
        for (int i = 0; i < g_hire_n; i++) if ((DWORD)g_hire_ids[i] == c) { g_merc_unit = unit; break; }
    }
    if (g_merc_run && mode == 2 && unit && *(DWORD *)unit == 1) {                 // monster unit asked to WALK
        DWORD cls = *(DWORD *)(unit + 4);
        for (int i = 0; i < g_hire_n; i++) if ((DWORD)g_hire_ids[i] == cls) {
            mode = 15;
            LONG n = InterlockedIncrement(&g_merc_conv);
            if (n == 1 || n % 200 == 0) logf_("merc run fix: mercenary (class %u) asked to walk -> run (conversion #%ld)", (unsigned)cls, (long)n);
            break;
        }
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
