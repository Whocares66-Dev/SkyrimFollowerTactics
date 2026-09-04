#pragma once
// Sensors: turn a live RE::Actor into an ft::Snapshot.
//
// This is the boundary. Everything above it (ft::Snapshot, ft::Evaluate) is
// RE::-free and unit tested; everything below is imperative Skyrim code that
// can only be verified by playing. Keep this file thin and obvious.

#include "core/Snapshot.h"

#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class InventoryEntryData;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// The RE:: pointers an action may need, carried alongside the Snapshot rather
// than inside it -- ft::Snapshot must never see an RE:: type, and an action
// still has to be handed the actual potion to equip.
struct PotionChoice
{
    RE::AlchemyItem *health{nullptr};
    RE::AlchemyItem *magicka{nullptr};
    RE::AlchemyItem *stamina{nullptr};
};

// One castable spell a follower knows, for the editor's menu.
//
// Name and id together because the menu shows one and stores the other: the
// name is what a player picks by and is translated, the FormID is what the
// rule keeps and what survives a language change. Same split as wire names
// versus display names, for the same reason.
struct SpellOption
{
    std::uint32_t form{0};
    std::string name;
};

// One drinkable potion a follower carries, for the editor's menu.
struct PotionOption
{
    std::uint32_t form{0};
    std::string name;
    int count{0};
};

// Every drinkable potion she carries, sorted by name. Menu content only.
[[nodiscard]] std::vector<PotionOption> ScanCarriedPotions(RE::Actor *actor);

// Every spell the follower can actually cast, sorted by name.
//
// Sorted here rather than in the UI because the order is a property of the
// list, not of how it is drawn, and doing it once per rebuild beats doing it
// every frame the menu is open.
//
// Filtered to SpellType::kSpell. Abilities, diseases and passive effects also
// live in an actor's spell list and none of them are castable, so offering
// them would be offering rules that can never work.
[[nodiscard]] std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor);

// One line of the character sheet, already worded. Worded HERE, not in the
// panel, because every value is an actor-value read and the RE:: enum naming
// it belongs with the read; the panel then has nothing to know about what a
// resistance cap is or which slot counts as armour.
// Font Awesome's infinity, for a sheet row whose value is "no end": an
// effect with no duration. The panel draws a row's icon in place of its
// value text.
inline constexpr unsigned kIconInfinity = 0xF534;

struct SheetRow
{
    std::string label;
    std::string value;
    unsigned icon{0};      // a Font Awesome codepoint drawn instead of the value, when set
    std::string modifiers; // Skills tab only: "+35% damage, -17% cost"
    std::string note;      // tooltip on the modifiers; empty for none
    // Rows revealed by expanding this one: a skill's perks. Empty means the
    // row is a plain line and cannot be opened.
    std::vector<SheetRow> detail;
    // The inventory item this row names, if any: a click on it opens the
    // item's page on the Inventory tab. 0 for a row that names nothing.
    std::uint32_t form{0};
};

struct SheetSection
{
    std::string title;
    std::vector<SheetRow> rows;
    // The heading this section sits under when several share one -- Attack
    // over a Right Hand table and a Left Hand table. Empty means the title
    // is the heading.
    std::string group;
};

// One effect running on the follower, for the Effects tab: the effect as
// the game names it, its magnitude, what is left of it, and where it comes
// from -- the spell, the potion, or for an enchantment the worn item that
// carries it, "Robes of Health" rather than the enchantment record's name.
struct EffectRow
{
    // The base effect and what applied it, together the row's identity:
    // the same effect can run twice from two sources.
    std::uint32_t form{0};
    std::uint32_t sourceForm{0};
    std::string name;
    float magnitude{0.0f};
    float duration{0.0f};      // in all; 0 for one with no duration
    float remaining{-1.0f};    // seconds left; below zero for one with no duration
    std::string remainingText; // empty for one with no duration
    std::string source;
    // False when the effect is running but changes nothing for this actor:
    // Fortify One-handed on a follower, which writes a value nothing on a
    // follower reads. Listed greyed, hovering as "Not applied".
    bool applied{true};

    // The page: numbers as sections, and the effect's description with the
    // magnitude and duration filled in, as the item card shows it.
    std::vector<SheetSection> detail;
    std::string description;
};

// Everything running on the follower that the game would list, sorted by
// name. Effects flagged hidden, and ones already run out, are left out.
[[nodiscard]] std::vector<EffectRow> ScanActiveEffects(RE::Actor *actor);

// Where a number on a sheet comes from. The engine keeps an actor value as
// a base plus lumps -- what perks and race add, what magic adds -- and
// names no source; but every running effect says which value it moves, by
// how much, and what applied it. So the magic lump can be told by source
// -- "Adept Robes of Destruction: +100" -- and only the perks-and-race
// lump stays a lump.
struct Contribution
{
    std::string source; // the worn item, the potion, the spell
    float amount{0.0f}; // signed: a detrimental effect takes away
};
[[nodiscard]] std::vector<Contribution> Contributions(RE::Actor *actor, RE::ActorValue value);

// The hover text for a sheet row that reads an actor value: "Base: 3",
// then each running effect by its source, then "Perks and race: +2" when
// they add anything. `unit` follows each number ("%" or "").
[[nodiscard]] std::string ValueNote(RE::Actor *actor, RE::ActorValue value, const char *unit);

// The Character tab: race, movement, defence and the equipped weapon. Display
// only -- none of it is a rule input. Cheap reads, done in and out of combat
// alike.
[[nodiscard]] std::vector<SheetSection> BuildCharacterSheet(RE::Actor *actor);

// The Skills tab: the eighteen skills grouped as the game groups them, with
// any fortify or potion modifier folded into the same line.
[[nodiscard]] std::vector<SheetSection> BuildSkillSheet(RE::Actor *actor);

// The Tactics tab's Combat Style section: the numbers and flags the combat
// AI is tuned by, read off the style she is using right now -- her live
// combat controller's in a fight, her record's otherwise -- so a copy the
// panel gave her shows as what it is.
[[nodiscard]] std::vector<SheetSection> BuildCombatStyleSheet(RE::Actor *actor);

// The damage a weapon does in her hands, as the inventory menu would show
// it: base, times tempering, times the skill curve, through her perks, times
// any Fortify effect on the skill. `entry` may be null, in which case the
// weapon is taken as untempered.
[[nodiscard]] float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry);

// The armour rating a piece gives her, the same way: base, times tempering,
// times the armour skill's curve, through her perks, times any Fortify
// effect on the skill. Clothing rates 0.
[[nodiscard]] float ArmorRating(RE::Actor *actor, RE::TESObjectARMO *armor, RE::InventoryEntryData *entry);

// Resolve a FormID from a rule back to the spell it names, or nullptr.
[[nodiscard]] RE::SpellItem *FindSpell(std::uint32_t form);

// Dump the actor's active magic effects to the log: source item, archetype,
// elapsed/duration, magnitude.
//
// This is here to answer one question empirically rather than from memory --
// does drinking a vanilla healing potion leave anything running that we could
// check? If it does, "is the effect I applied still active" is a far more
// precise availability test than a fixed cooldown, and it generalises to
// spells and food. If the list is empty, the effect is instant, the condition
// itself is the check, and MinimumCooldown stays the right mechanism.
//
// ActiveEffect carries `spell` (the AlchemyItem for a potion), `duration` and
// `elapsedSeconds`, so the check is exact once we know it is worth making.
void LogActiveEffects(RE::Actor *actor, const char *when);

// Build the snapshot for one follower. `now` is monotonic seconds since plugin
// load; cooldowns are measured against it.
//
// Phase 1 scope: this fills self/player state and the potion inventory only.
// The enemies and allies vectors are deliberately left EMPTY -- the marquee
// rule is Self + HealthPctBelow, which needs none of it, and every extra sensor
// is per-tick cost that has to be justified (docs/PLAN.md 3.3). Group subjects
// will not match until those are populated.
ft::Snapshot BuildSnapshot(RE::Actor *actor, double now, PotionChoice &choice);

} // namespace ft::game
