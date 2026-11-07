// merc.h - mercenary equipment rules (platform independent; unit-tested natively).
// The game decides "may this merc wear/wield this item?" with a hard-coded table keyed on the merc's class number. This module
// holds a replacement table: item-type CODES per merc class (PD2's rules plus BaseMod's "FullEquip" list), resolved to the
// running game's item-type numbers from its ItemTypes.txt, and evaluated with the game's own item-type check function.
#ifndef QT_MERC_H
#define QT_MERC_H
#include <stdint.h>

#ifdef _WIN32
#define MERC_CALL __stdcall
#else
#define MERC_CALL
#endif

typedef uintptr_t merc_item_t;                                        // the game's item pointer, opaque here
typedef int (MERC_CALL *merc_chk)(merc_item_t item, int type_id);     // game: is `item` of (or derived from) this item type?
typedef int (MERC_CALL *merc_two)(merc_item_t item);                  // game: is `item` two-handed?

// Feed the game's ItemTypes.txt (row number = item-type id). Returns the number of types read, 0 if it isn't that table.
int merc_parse_itemtypes(const char *txt, int len);
int merc_rules_ready(void);
void merc_reset(void);

// Is `item` an allowed non-armor item for the merc class `cls` (271 Rogue, 338 Desert, 359 Iron Wolf, 561 Barbarian)?
// Armor (body armor, helms) is NOT decided here: the game's own armor test still runs when this returns 0.
int merc_allows(merc_item_t item, int cls, merc_chk chk, merc_two two);
// Same, told what the merc holds in her right hand (0 = nothing, MERC_HAND_UNKNOWN = could not be read). A Rogue's quiver is only allowed next to the
// weapon it belongs to (arrows with a bow, bolts with a crossbow, or an empty hand): the game silently DESTROYS a mismatched quiver, so we refuse it.
#define MERC_HAND_UNKNOWN ((merc_item_t)-1)
int merc_allows_hand(merc_item_t item, int cls, merc_chk chk, merc_two two, merc_item_t right_hand);
// Same, also told what is in her LEFT hand: a bow or crossbow is refused next to a quiver of the other kind (the game would destroy the weapon).
int merc_allows_hands(merc_item_t item, int cls, merc_chk chk, merc_two two, merc_item_t right_hand, merc_item_t left_hand);
// Class-restricted items (Amazon bows/spears/javelins, Paladin shields, Sorceress orbs): the game refuses them for every merc except Barbarian
// helms on the Barbarian. type_id = the item's own item-type id, item_class = 0 Amazon .. 6 Assassin, 7 = unrestricted.
int merc_class_ok(int type_id, int merc_class, int item_class);

// Which body location does the server use? The game uses the item type's FIRST location (the weapon slot for quivers) except for
// the Iron Wolf's shield, which goes to the second. Returns 1 = use the second location: Iron Wolf + shield, Rogue + quiver.
int merc_slot2(merc_item_t item, int cls, merc_chk chk);
#endif
