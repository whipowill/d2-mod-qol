// Checks the D2Net signatures against byte dumps extracted from real D2Net.dll files (hex args on the command line):
//   sigtest send <hex> | sigtest recv <hex> | sigtest notsend <hex> | sigtest notrecv <hex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sig.h"
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    unsigned char b[64] = {0};  // (xp commands use their own, larger buffer)
    for (size_t i = 0; i < strlen(argv[2]) / 2 && i < sizeof b; i++) { unsigned v; sscanf(argv[2] + 2 * i, "%2x", &v); b[i] = v; }
    if (!strcmp(argv[1], "keep")) {            // stock KeepEquip site + the epilogue 0x2fd bytes later, as in the real D2Game
        static unsigned char big[2048];
        const char *hx = "8b95c8000000" "8b8dc4000000" "83c902" "81ca00100000" "8995c8000000" "898dc4000000" "8b8b08050000" "81f900001000";
        memset(big, 0xCC, sizeof big);
        for (int i = 0; i < 45; i++) { unsigned v; sscanf(hx + 2 * i, "%2x", &v); big[100 + i] = (unsigned char)v; }
        static const unsigned char epi[] = {0x5F,0x5E,0x8B,0xC5,0x5D,0x5B,0x83,0xC4,0x18,0xC2,0x14,0x00};
        memcpy(big + 100 + 0x2fd, epi, sizeof epi);
        int patched = -1; long ep = -1, at = sig_find_keep_equip(big, sizeof big, &patched, &ep);
        int ok = at == 100 && patched == 0 && ep == 100 + 0x2fd && (ep - (at + 5)) == 0x2f8;     // rel32 of the original hex edit
        big[100] = 0xE9; big[105] = 0x90;                                                         // already patched: must be recognised too
        at = sig_find_keep_equip(big, sizeof big, &patched, &ep);
        ok = ok && at == 100 && patched == 1;
        big[200] = 0xE9; big[205] = 0x90; memcpy(big + 206, big + 106, 39);                       // a second copy: ambiguous -> refuse
        ok = ok && sig_find_keep_equip(big, sizeof big, &patched, &ep) == -1;
        printf("keep %s\n", ok ? "ok" : "FAIL"); return !ok;
    }
    if (!strncmp(argv[1], "xp", 2)) {          // xp-orig / xp-patched / xp-none : sig_find_xp_penalty on a buffer
        unsigned char big[256] = {0}; size_t n = strlen(argv[2]) / 2 < sizeof big ? strlen(argv[2]) / 2 : sizeof big;
        for (size_t i = 0; i < n; i++) { unsigned v; sscanf(argv[2] + 2 * i, "%2x", &v); big[i] = (unsigned char)v; }
        int patched = -1; long at = sig_find_xp_penalty(big, n, &patched);
        int ok = !strcmp(argv[1], "xp-orig") ? (at >= 0 && !patched) : !strcmp(argv[1], "xp-patched") ? (at >= 0 && patched == 1) : at < 0;
        printf("%s %s (offset %ld)\n", argv[1], ok ? "ok" : "FAIL", at); return !ok;
    }
    int s = sig_is_send(b), r = sig_is_recv(b), m = sig_is_setmode(b);
    if (!strcmp(argv[1], "setmode")) { printf("setmode %s\n", m ? "ok" : "FAIL"); return !m; }
    if (!strcmp(argv[1], "notsetmode")) { printf("notsetmode %s\n", !m ? "ok" : "FAIL"); return m; }
    int ok = !strcmp(argv[1], "send") ? s && !r : !strcmp(argv[1], "recv") ? r && !s : !strcmp(argv[1], "notsend") ? !s : !r;
    printf("%s %s\n", argv[1], ok ? "ok" : "FAIL");
    return !ok;
}
