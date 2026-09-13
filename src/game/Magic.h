#pragma once
// A follower's magic, worded for the panel: the spells they know, their powers,
// their shouts. Built on the game thread, copied with the view.

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
    // A spell of no school: a vampire's Drain Life, a race's ability cast
    // as a spell. The combat AI casts them and a rule may name them, so the
    // tab lists them; the level and the skill gate mean nothing for them.
    Other,
    Shouts,
    Powers,
    COUNT
};

[[nodiscard]] const char *DisplayName(MagicCategory category);

// The words a spell's page and lists use, shared with a scroll's, which is
// a spell in a wrapper: the school of a skill; the kind of spell from its
// costliest effect (the element where it does that kind of damage, else
// the archetype); how it is cast, delivery and casting type in one word.
[[nodiscard]] MagicCategory SchoolOf(RE::ActorValue skill);
[[nodiscard]] std::string TypeWord(const RE::EffectSetting *base);
[[nodiscard]] const char *CastWord(RE::MagicSystem::Delivery delivery, RE::MagicSystem::CastingType casting);

struct MagicEntry
{
    std::uint32_t form{0};
    std::string name;
    MagicCategory category{MagicCategory::Powers};
    // The columns: the school as the game names it, the level word the
    // magic menu shows, the cost as it shows it ("13/s" for a stream).
    std::string school;
    std::string level;
    // What kind of spell: the element where the costliest effect does that
    // kind of damage (Fire, Frost, Shock, Poison), else the effect's kind by
    // its archetype -- Summon, Reanimate, Bound Weapon, Calm, Fear, Frenzy,
    // Heal, Ward, Armor, Invisibility, Paralysis and so on. Every school
    // has one; the school lists show it beside the name.
    std::string type;
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
    // Never the player's: no combat AI chooses for them.
    bool aboveSkill{false};
    int castValue{0};      // the delivery behind the word, for sorting
    float costValue{0.0f}; // magicka, for sorting; 0 for powers and shouts
    // The cost written out -- the effects, the skill curve, each perk
    // entry -- as hover text on the cost cell; empty for a power or shout.
    ft::Breakdown costBreakdown;
    // In a hand -- and which -- or, for a power or shout, selected.
    bool equipped{false};
    bool equippedLeft{false};
    bool equippedRight{false};
    bool pinnedLeft{false};
    bool pinnedRight{false};
    bool pinned{false}; // a voice pin: a power or shout held in the voice slot
    // Never to be used: the combat AI scores it zero, the engine's equips of
    // it are refused, and it comes off if found on. Off, and kept off.
    bool banned{false};
    // Which hands the record allows. Most spells take either; the NPC-only
    // variants take one, and a master spell takes both at once.
    bool leftAllowed{true};
    bool rightAllowed{true};
    // The same, as the pin book words it, for the equip menu's hand lists.
    ft::Grip grip{ft::Grip::None};
    // Kept from the combat AI in a fight because it would take a hand a
    // pin holds: it scores zero whenever the combat AI asks. Still known, and a
    // cast rule can still make them cast it. Listed dimmed.
    bool setAside{false};
    // Why, when it is: one line per pin that holds a hand it could take,
    // "Firebolt is pinned", for the row's tooltip.
    std::string asideBy;
    std::string hand; // the record's word for it: Either, Left, Right, Both; Voice for a power or shout

    // The page: numbers as sections, then the effect lines and the record's
    // description, as the Inventory tab's pages are laid out.
    std::vector<SheetSection> detail;
    // What the spell does: the table of effects (EffectsOf), at the
    // follower's own magnitudes, each row's description in its last column.
    SheetSection effectsTable;
    std::string description;
};

// Everything castable they have, sorted by name. Abilities, diseases and the
// like are left out, as the magic menu leaves them out.
[[nodiscard]] std::vector<MagicEntry> ScanMagic(RE::Actor *actor);

} // namespace ft::game
