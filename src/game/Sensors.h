#pragma once
// Sensors: turn a live RE::Actor into an ft::Snapshot.
//
// This is the boundary. Everything above it (ft::Snapshot, ft::Evaluate) is
// RE::-free and unit tested; everything below is imperative Skyrim code that
// can only be verified by playing. Keep this file thin and obvious.

#include "core/Snapshot.h"

#include <functional>
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

// The weapon in a hand, if it takes a poison (anything but a staff). Null
// for no weapon there, or a staff.
[[nodiscard]] RE::TESObjectWEAP *PoisonableWeaponIn(RE::Actor *actor, bool left);

// The weapon a poison would go on: the right hand's if it takes one and is
// clean, else the left's on the same terms, as the inventory menu goes to
// the right hand alone. Null when neither qualifies.
[[nodiscard]] RE::TESObjectWEAP *WeaponToPoison(RE::Actor *actor);

// Does that weapon, as the actor carries it, already have a poison on it?
[[nodiscard]] bool WeaponPoisoned(RE::Actor *actor, RE::TESObjectWEAP *weapon);

// A weapon's enchantment charge as the actor carries it: what is left, the
// full amount, and what one hit draws in the actor's hands. Not enchanted
// reads as all zero.
struct WeaponCharge
{
    bool enchanted{false};
    float charge{0.0f};
    float maxCharge{0.0f};
    float costPerHit{0.0f};
};
[[nodiscard]] WeaponCharge ChargeOf(RE::Actor *actor, RE::TESObjectWEAP *weapon);

// The weapon in a hand, enchanted or not; null for no weapon there.
[[nodiscard]] RE::TESObjectWEAP *WeaponIn(RE::Actor *actor, bool left);

// What a soul of that level puts into a charge: the five iSoulLevelValue
// game settings, which the engine's own recharge reads.
[[nodiscard]] float SoulCharge(RE::SOUL_LEVEL level);

// The filled soul gems carried, as the snapshot lists them. A reusable one
// (Azura's Star, the ReusableSoulGem keyword) counts: spending it empties
// it, as the engine's own recharge does, rather than removing it.
[[nodiscard]] std::vector<ft::Snapshot::SoulGemView> ScanSoulGems(RE::Actor *actor);

// A stat as the bars and the rules read it: the current value, and the
// maximum with every modifier in -- the permanent (perks, race) and the
// temporary (a Fortify enchantment or potion).
[[nodiscard]] ft::Stat ReadStat(RE::Actor *actor, RE::ActorValue av);

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
    // Delivery Self: cast on oneself and on no one else. The menu offers it
    // under Self only, and everything aimed under everyone but Self -- save
    // a Location spell (a conjuration), which goes at whoever's feet the
    // rule names, the follower's included, and is offered under everyone.
    bool selfOnly{false};
    bool location{false};
    // Carries a Reanimate-archetype effect: the only kind aimed at a corpse.
    // The archetype is the record property the engine raises a corpse by,
    // so a mod's reanimate spell is found by it whatever it is called.
    bool reanimate{false};
    // Which menu lists it: Cast spell, Use power, or Shout. One list
    // because all three are found by the same walk of what she knows.
    enum class Kind : std::uint8_t
    {
        Spell,
        Power,
        Shout
    };
    Kind kind{Kind::Spell};
};

// One consumable she carries -- a potion, a food, an ingredient -- for the
// editor's Consume menu.
struct ConsumableOption
{
    std::uint32_t form{0};
    std::string name;
    int count{0};
    ft::ConsumableKind kind{ft::ConsumableKind::Potion};
    // The effects a policy could choose this by: a potion's boons, a
    // poison's banes, by name. What the Strongest and Weakest menus list.
    std::vector<std::string> effects;
};

// Every consumable she carries, sorted by name. Menu content only. Poisons
// are left out: they go on a weapon, not down the throat.
[[nodiscard]] std::vector<ConsumableOption> ScanCarriedConsumables(RE::Actor *actor);

// Every spell the follower can actually cast, sorted by name.
//
// Sorted here rather than in the UI because the order is a property of the
// list, not of how it is drawn, and doing it once per rebuild beats doing it
// every frame the menu is open.
//
// Filtered to SpellType::kSpell and the two power types, plus the shouts on
// the base record. Abilities, diseases and passive effects also live in an
// actor's spell list and none of them are castable, so offering them would
// be offering rules that can never work.
[[nodiscard]] std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor);

// Castable means SpellType::kSpell; a power is kPower or kLesserPower -- or
// a power a shout slot is leasing, which reads as Voice for the lease
// (Packages.cpp). An actor's spell list also carries abilities, diseases and
// passive racial effects, none of which a follower can choose to cast, so a
// rule naming one could never fire.
[[nodiscard]] bool IsCastable(const RE::SpellItem *spell);
[[nodiscard]] bool IsPower(const RE::SpellItem *spell);

// Walk every spell an actor has, from the three places the game keeps them.
// Missing any loses spells that are plainly there:
//   TESNPC::GetSpellList()  what the character was authored with -- Marcurio's
//                           destruction spells come from here.
//   TESRace::actorEffects   the race's: the passive resistances, and the
//                           racial power (Voice of the Emperor on an
//                           Imperial), which is why the Powers chip is not
//                           empty for a vanilla follower.
//   addedSpells             everything granted at runtime, which is what the
//                           console's addspell writes to.
// The same spell can appear in more than one; callers dedupe by form.
void ForEachSpell(RE::Actor *actor, const std::function<void(RE::SpellItem *)> &fn);

// One line of the character sheet, already worded. Worded HERE, not in the
// panel, because every value is an actor-value read and the RE:: enum naming
// it belongs with the read; the panel then has nothing to know about what a
// resistance cap is or which slot counts as armour.
// Font Awesome's infinity, for a sheet row whose value is "no end": an
// effect with no duration. The panel draws a row's icon in place of its
// value text.
inline constexpr unsigned kIconInfinity = 0xF534;

// The two glyphs an Equipped row is made of, Font Awesome's check and
// thumbtack: the same codepoints the panel's own Glyph table uses, so the
// row reads as the Inventory and Magic tabs' cells do.
inline constexpr unsigned kGlyphTick = 0xF00C;
inline constexpr unsigned kGlyphPin = 0xF08D;

struct SheetRow
{
    std::string label;
    std::string value;
    unsigned icon{0};      // a Font Awesome codepoint drawn instead of the value, when set
    unsigned icon2{0};     // a second glyph after the first: the pin beside the tick
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

// What a follower commands right now: a summon or a raised corpse, for the
// Summons tab. Its numbers come from its own actor, its page from the same
// sheet builder as the follower's, so the two tabs read alike.
struct SummonView
{
    ft::ActorId id{0};
    std::uint32_t baseId{0};
    std::string name;
    std::uint16_t level{0};
    ft::Stat health{};
    ft::Stat magicka{};
    ft::Stat stamina{};
    float remaining{0.0f}; // seconds left on the effect that commands it; 0 when unknown
    bool raised{false};    // a reanimated corpse, as opposed to a summon
    std::vector<SheetSection> sheet;
};

// Everything the follower commands, in the engine's order.
[[nodiscard]] std::vector<SummonView> ScanSummons(RE::Actor *actor);

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

// The armour rating's sources: each piece worn with its rating, and the
// spells and enchantments on the armour value, smallest first.
[[nodiscard]] std::string ArmorNote(RE::Actor *actor);
// The engine's hidden per-piece bonus in the rating's own units, and the
// rating with it added: what the Armor row shows, and what its sources sum
// to.
[[nodiscard]] float HiddenArmor(RE::Actor *actor);
[[nodiscard]] float EffectiveArmor(RE::Actor *actor);

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

// The combat style the actor fights by: the controller's live copy when
// there is one, else the record's. Null for an actor with none.
[[nodiscard]] RE::TESCombatStyle *LiveCombatStyle(RE::Actor *actor);

// May the actor hold a one-handed weapon in each hand? The combat style's
// flag; an actor with no style may. The panel's cells, and a request, read
// it (core/Loadout.h WouldDualWield).
[[nodiscard]] bool DualWieldAllowed(RE::Actor *actor);

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
// load; cooldowns are measured against it. Self and player state, the
// potions and the loadout, and the party and the enemies by definition:
// allies are the player and the other teammates, enemies whoever is in
// combat and hostile to the player (docs/CONDITIONS.md 6).
ft::Snapshot BuildSnapshot(RE::Actor *actor, double now);

} // namespace ft::game
