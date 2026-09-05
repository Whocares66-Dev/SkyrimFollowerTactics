#pragma once
// A follower's magic, worded for the panel: the spells she knows, her powers,
// her shouts. Built on the game thread, copied with the view.

#include "core/Loadout.h"
#include "game/Sensors.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class SpellItem;
} // namespace RE

namespace ft::game
{

// SkyUI's Magic menu tabs, less Favorites and Active Effects.
enum class MagicCategory : std::uint8_t
{
    Alteration,
    Conjuration,
    Destruction,
    Illusion,
    Restoration,
    Shouts,
    Powers,
    COUNT
};

[[nodiscard]] const char *DisplayName(MagicCategory category);

struct MagicEntry
{
    std::uint32_t form{0};
    std::string name;
    MagicCategory category{MagicCategory::Powers};
    // The columns: the school as the game names it, the level word the
    // magic menu shows, the cost as it shows it ("13/s" for a stream).
    std::string school;
    std::string level;
    // How it is cast, delivery and casting type folded into one word: Self,
    // Touch, Spray (aimed and sustained -- Flames), Projectile (aimed and
    // fired -- Firebolt), Target, Location.
    std::string cast;
    std::string cost;
    float magnitude{0.0f}; // the costliest effect's, the column
    int levelValue{0};     // the minimum skill behind the word, for sorting
    int skill{0};          // the follower's level in the spell's school
    // The level is above the follower's skill: left to itself the combat AI
    // will not choose it (Chain Lightning, Adept, against Destruction 39).
    // The package could make them cast it regardless, but by our rule it is
    // neither cast nor pinned, and the hand cells take no click for it.
    bool aboveSkill{false};
    int castValue{0};      // the delivery behind the word, for sorting
    float costValue{0.0f}; // magicka, for sorting; 0 for powers and shouts
    // In a hand -- and which -- or, for a power or shout, selected.
    bool equipped{false};
    bool equippedLeft{false};
    bool equippedRight{false};
    bool pinnedLeft{false};
    bool pinnedRight{false};
    bool pinned{false}; // a voice pin: a power or shout held in the voice slot
    // Which hands the record allows. Most spells take either; the NPC-only
    // variants take one, and a master spell takes both at once.
    bool leftAllowed{true};
    bool rightAllowed{true};
    // The same, as the pin book words it, for the equip menu's hand lists.
    ft::Grip grip{ft::Grip::None};
    // Kept from the combat AI in a fight because it would take a hand a
    // pin holds: it scores zero whenever the combat AI asks. Still known, and a
    // cast rule can still make her cast it. Listed dimmed.
    bool setAside{false};
    // Why, when it is: one line per pin that holds a hand it could take,
    // "Firebolt is pinned", for the row's tooltip.
    std::string asideBy;
    std::string hand; // the record's word for it: Either, Left, Right, Both; Voice for a power or shout

    // The page: numbers as sections, then the effect lines and the record's
    // description, as the Inventory tab's pages are laid out.
    std::vector<SheetSection> detail;
    std::string effects;
    std::string description;
};

// Everything castable she has, sorted by name. Abilities, diseases and the
// like are left out, as the magic menu leaves them out.
[[nodiscard]] std::vector<MagicEntry> ScanMagic(RE::Actor *actor);

} // namespace ft::game
