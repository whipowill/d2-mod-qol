// Replays a captured autopot.log through the pickup core and prints what it would do.
//   gcc -O1 -Wall -Isrc -o build/replay test/replay.c src/core.c && build/replay path/to/autopot.log
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core.h"
static void logfn(const char *m) { printf("    [core] %s\n", m); }
static int hexparse(const char *s, uint8_t *o) { int n = 0; unsigned v; while (sscanf(s, "%2x", &v) == 1) { o[n++] = (uint8_t)v; s += 2; while (*s == ' ') s++; } return n; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "r"); if (!f) return 1;
    ap_config c = {1, argc > 2 ? atoi(argv[2]) : 5, argc > 3 ? atoi(argv[3]) : 1, 0, 0, 0};
    ap_init(&c, logfn);
    char line[1024]; uint32_t now = 1000; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++; now += 60;
        char *bar = strstr(line, "| "); uint8_t b[256]; int n = 0;
        if (!strncmp(line, "RECV id=", 8) && bar) { n = hexparse(bar + 2, b); ap_on_recv(b, n); }
        else if (!strncmp(line, "SEND id=", 8) && bar) { n = hexparse(bar + 2, b); ap_on_send(b, n); if (b[0] == 0x16) printf("%d: (player sent pickup of id %u)\n", ln, b[5] | b[6] << 8); }
        uint8_t out[16];
        int k = ap_tick(now, out);
        if (k) { printf("%d: >>> WOULD SEND pickup id=%u\n", ln, out[5] | out[6] << 8); }
    }
    char d[200]; ap_dump(d, sizeof d); printf("end: %s\n", d);
}
