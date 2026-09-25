#pragma once
// Where a number on a sheet comes from: the engine's base and each
// running effect by source, the perk entries on it, and the figures
// written out -- armour, weapon damage, critical chance, attack speed,
// spell cost (dev/MODIFIERS.md).

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class BGSAttackData;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// Where a number on a sheet comes from. The engine keeps an actor value as
// a base plus modifiers and names no source; but every running effect says
// which value it moves, by how much, and what applied it. So what magic
// adds can be told by source -- "Adept Robes of Destruction: +100" -- and
// what anything else wrote there (a script, another plugin) cannot.
struct Contribution
{
    std::string source; // the worn item, the potion, the spell
    std::string effect; // the magic effect's own name, "Fortify Armor Rating"
    float amount{0.0f}; // signed: a detrimental effect takes away
    // The effect's record has Recover: the value moves while it runs and
    // back when it ends. Without it a pool moves every second, a
    // regeneration or a poison, and its maximum does not.
    bool recovers{true};
};
[[nodiscard]] std::vector<Contribution> Contributions(RE::Actor *actor, RE::ActorValue value);

// The sources as lines added to a breakdown, smallest first -- the
// weaknesses, then the boons, the largest last: "Silver Ruby Ring: +50%".
void AddSourceLines(ft::Breakdown &b, std::vector<Contribution> sources);

// A value as the parts a sheet can name: the engine's base, which no effect
// moves, and each running effect on it by source, smallest first. Every
// figure that reads a value opens it out from these, so none shows a buff
// folded into its base: the value as it reads was once the base, and hid
// Mundus's Elfborn stone in the Magicka Rate (2026-09-14). Whatever else
// is in the value -- a script's ModActorValue, another plugin's write, as
// Blade and Blunt's injuries are -- has no name, and is the Other line
// wherever the parts are summed against the value.
struct ValueParts
{
    float base{0.0f};
    std::vector<Contribution> sources;
};
[[nodiscard]] ValueParts PartsOf(RE::Actor *actor, RE::ActorValue value);

// The parts as lines, each amount times `scale`: "Base" where the base is
// not zero, starting the calculation when it is the first line, then each
// source.
void AddValueLines(ft::Breakdown &b, const ValueParts &parts, float scale = 1.0f);

// A value written out: its parts, and the value as it reads now as the
// total, with Other for the rest. For a pool (Health, Magicka, Stamina) the
// total is the maximum, not what is left of it.
[[nodiscard]] ft::Breakdown ValueBreakdown(RE::Actor *actor, RE::ActorValue value, const char *unit);

// The carry weight the engine holds them to: the Carry Weight value written
// out, then each Get Max Carry Weight perk entry, and the engine's own
// figure as the total.
[[nodiscard]] ft::Breakdown CarryWeightBreakdown(RE::Actor *actor);

// The armour rating's sources: each piece worn with its rating, the
// spells and enchantments on the armour value, the engine's hidden bonus
// per piece, and the rating they make.
[[nodiscard]] ft::Breakdown ArmorBreakdown(RE::Actor *actor);

// The perk entries an actor holds on one entry point, as lines: each
// entry in the order the engine applies them (highest priority first, the
// arrays on the actor's process being kept sorted -- dev/MODIFIERS.md),
// named for its perk, its function applied as the engine applies it; an
// entry whose conditions fail against `args` listed dimmed with the
// condition that stopped it. `args` are the call's arguments after the
// perk owner, as the engine's own call takes them: the weapon and the
// target for attack damage, the spell for cost, the piece for armour.
void AddEntryPointLines(ft::Breakdown &b, RE::Actor *actor, RE::BGSEntryPoint::ENTRY_POINT point,
                        const std::vector<void *> &args);

// The cost of a spell as the caster pays it, written out: the effects'
// costs, the skill curve, each Mod Spell Cost entry, and the engine's
// figure as the total.
[[nodiscard]] ft::Breakdown SpellCostBreakdown(RE::Actor *actor, const RE::SpellItem *spell);
// The engine's hidden per-piece bonus in the rating's own units, and the
// rating with it added: what the Armor row shows, and what its sources sum
// to.
[[nodiscard]] float HiddenArmor(RE::Actor *actor);
[[nodiscard]] float EffectiveArmor(RE::Actor *actor);
// The share of a blow their armour takes, 0 to the cap, as the engine's hit
// handler combines rating and pieces: the Armor condition's measure
// (dev/CONDITIONS.md 3). The definition says what is left out and why.
[[nodiscard]] float DamageReduction(RE::Actor *actor);

// The damage a weapon does in their hands, as the inventory menu would show
// it: base, times tempering, times the skill curve, through their perks
// (which is where a Fortify effect on the skill counts, read by the hidden
// skill-boost perk for whoever holds it), times the attack damage
// multiplier, plus flat melee damage. `entry` may be null, in which case
// the weapon is taken as untempered. `out`, when given, is the figure
// written out. `target` is whom the perks are asked against; null, the
// actor stands in, as the inventory menu has it.
[[nodiscard]] float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry,
                                 ft::Breakdown *out = nullptr, RE::Actor *target = nullptr);

// The weapon's skill, their level in it, and the factor it gives their
// damage: the engine's min + (max - min) * skill / 100, from the player's
// pair of settings or everyone else's.
struct WeaponSkillCurve
{
    RE::ActorValue skill{RE::ActorValue::kOneHanded};
    float level{0.0f};
    float factor{1.0f};
};
[[nodiscard]] WeaponSkillCurve SkillCurveOf(RE::Actor *actor, RE::TESObjectWEAP *weapon);

// The armour rating a piece gives them, the same way: base, times tempering,
// times the armour skill's curve, through their perks. Clothing rates 0.
[[nodiscard]] float ArmorRating(RE::Actor *actor, RE::TESObjectARMO *armor, RE::InventoryEntryData *entry,
                                ft::Breakdown *out = nullptr);

// Their chance of a critical hit with a weapon, in percent: the critical
// chance value, through the perks on the critical hit chance entry point.
[[nodiscard]] float CritChance(RE::Actor *actor, RE::TESObjectWEAP *weapon, ft::Breakdown *out = nullptr);

// A weapon's attack speed in their hands: the record's speed, the two-handed
// setting for a greatsword or battleaxe, and the multiplier of the hand it
// swings in, the left where `left`.
[[nodiscard]] float WeaponSpeed(RE::Actor *actor, RE::TESObjectWEAP *weapon, bool left, ft::Breakdown *out = nullptr);

// A shout word's recovery as it applies to them: the word's own time,
// times their shout recovery multiplier.
[[nodiscard]] float WordRecovery(RE::Actor *actor, float recovery, ft::Breakdown *out = nullptr);

// The worn item carrying this enchantment, by the name the game shows for
// it, or empty if none is worn.
struct WornSource
{
    std::uint32_t form{0};
    std::string name;
};

// The worn item an enchantment's effect comes from, by name -- the name
// given at the table, else the record's. `from` is the item the active
// effect itself names (ActiveEffect::source), and is asked for first: two
// pieces enchanted alike at a table share one enchantment form, and
// searching by the form named the ring for the necklace's effect too
// (2026-09-11, a Silver Ruby Ring listed twice). Without it, the first
// worn item carrying the form.
[[nodiscard]] WornSource WornSourceOf(RE::Actor *actor, const RE::MagicItem *magic, const RE::TESBoundObject *from);

// The value's display name, where the game has one; else the Creation
// Kit's, read as words: Ward Power, Damage Resist. Most values the game
// never shows have no display name (Spellbreaker's ward read as "? +100",
// 2026-09-11).
[[nodiscard]] std::string ValueName(RE::ActorValue value);

// A value a line reads, opened out beneath it. In the value's own units,
// not the breakdown's: a multiplier beneath a recovery time read "+0.2 s".
// `scale` turns the value's units into the line's where they differ: a
// Magicka Rate Mult of 200 is the factor x 2, its terms 1 and 0.5, and
// `reading` is then in the line's units. Empty for a base alone, which is
// the line's own figure again.
[[nodiscard]] std::vector<ft::BreakdownLine> ValueLines(RE::Actor *actor, RE::ActorValue value, float reading,
                                                        float scale = 1.0f);

// A skill's level: the engine's base, what a companion Progression levels
// has learned on top of it (the value view adds that to the base the
// engine reads), and each effect on the skill by its source. Empty where
// the base is the whole of it, which the level already says. The player's
// base is taken as it reads: EngineBase asks Character's own slot, which
// is not the player's class.
[[nodiscard]] ft::Breakdown SkillBreakdown(RE::Actor *actor, RE::ActorValue skill);

// The two floats of a two-value function record, read where the engine's
// handlers read them (dev/MODIFIERS.md): the first is an actor value's
// index for the actor-value functions, the second the multiplier.
struct TwoValueData
{
    void *vtable;
    float data[2];
};
} // namespace ft::game
