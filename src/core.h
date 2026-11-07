// core.h - platform-independent potion auto-pickup logic (no Windows calls; unit-testable on Linux).
#ifndef AUTOPOT_CORE_H
#define AUTOPOT_CORE_H
#include <stdint.h>

typedef struct {
    int enabled;      // master switch
    int range;        // max distance (tiles, Chebyshev) from player to ground potion
    int verbose;      // 0 = quiet, 1 = decisions, 2 = also parsed item events
    int rejuv_always; // 1 = pick up every rejuvenation potion even if the belt has no room/column for it
} ap_config;

// Logging callback supplied by the host (may be NULL).
typedef void (*ap_logfn)(const char *msg);

void ap_init(const ap_config *cfg, ap_logfn log);
void ap_reset_learned(void);   // forget learned stack sizes (tests)
// Feed the game's Misc.txt (tab separated, as loaded by the game): sets the max stack size of keys and tomes from its
// `maxstack` column. Returns how many were set. Sizes learned from refused pickups still override it.
int ap_parse_misc(const char *txt, int len);
// Feed the game's MonStats.txt: fills ids[] with the class ids of mercenaries (AI = Hireable). Returns how many (0 = unexpected table).
int ap_parse_monstats(const char *txt, int len, int *ids, int max);

// Feed every server->client packet (raw buffer as returned by D2Net receive) and every client->server packet.
void ap_on_recv(const uint8_t *p, int len);
void ap_on_send(const uint8_t *p, int len);

// Call frequently (e.g. on every receive poll). now_ms is a monotonic millisecond clock.
// Returns the length (13) of a pickup packet written to out[], or 0 if nothing should be sent.
int ap_tick(uint32_t now_ms, uint8_t out[16]);
uint32_t ap_cursor_id(void);   // id of the item on the cursor (0 = none)
int ap_cursor_busy(void);   // an item is currently on the cursor

// Debug summary into buf.
void ap_dump(char *buf, int n);
int ap_last_qty(void);   // quantity decoded from the last key/tome packet (-1 = unknown); for tests
#endif
