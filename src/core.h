// core.h - platform-independent potion auto-pickup logic (no Windows calls; unit-testable on Linux).
#ifndef AUTOPOT_CORE_H
#define AUTOPOT_CORE_H
#include <stdint.h>

typedef struct {
    int enabled;      // master switch
    int range;        // max distance (tiles, Chebyshev) from player to ground potion
    int verbose;      // 0 = quiet, 1 = decisions, 2 = also parsed item events
    int rejuv_always; // 1 = pick up every rejuvenation potion even if the belt has no room/column for it
    int gold;         // 1 = also pick up gold piles on the ground
    int poll_ms;      // how often (ms) to look for items to pick up; 0 = default (120)
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
// Druid summon skills from the game's Skills.txt (charclass dru with a `summon`): id, the pet type (summons of one type replace each other) and how many casts
// bring a full set back (the `Param3` limit when the pet limit uses it, as with the wolves; otherwise 1). Returns how many, 0 if it isn't that table.
typedef struct { int id; char pet[16]; char mon[24]; int repeat; } ap_summon_t;      // mon = the pet's MonStats Id
int ap_parse_skills(const char *txt, int len, ap_summon_t *out, int max);
int ap_due(uint32_t now);   // 1 = a pickup scan is due and something is on the ground (else skip ap_tick and live position reads)
int ap_player_pos(int *x, int *y);
void ap_set_town(int in_town);                        // 1 = the player is in a town: no auto-pickups
void ap_set_pos(int x, int y);                         // the player's real current position (tiles), when the host can read it
// Class ids of monsters by their MonStats `Id` name (explicit hcIdx column when the table has one, else the row number without "Expansion" divider rows).
// out[i] = -1 when names[i] is not in the table. Returns how many were found, 0 if it isn't that table.
int ap_monster_ids(const char *txt, int len, const char (*names)[24], int n, int *out);   // last known position of the player (0 = unknown)

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
