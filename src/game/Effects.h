#pragma once
// Magic effects as the rules read them: every effect running on an
// actor, a record's shape for core's effect rules (core/Effects.h), what a
// consumable gives, and the picks the Effect condition offers.

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"
#include "game/Spells.h"

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

// Every effect running on the actor that is live -- with a base effect,
// not inactive, not dispelled -- in one walk. What the snapshot's
// statuses, running effects and active spells read, and the sheets'
// contributions. (The Effects tab walks the list itself: it shows the
// inactive ones, greyed.)
void ForEachActiveEffect(RE::Actor *actor, const std::function<void(RE::ActiveEffect &)> &fn);

// One consumable they carry -- a potion, a food, an ingredient -- for the
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
    // Whether an "any" rule could roll this one: every poison, and anything
    // drunk or eaten that carries a buff. The same question core's
    // PotionStock::WantedByAny answers, asked here only so the menu can
    // leave "Any" out when nothing would answer it.
    bool any{false};
};

// Every consumable they carry, sorted by name. Menu content only. Poisons
// are left out: they go on a weapon, not down the throat.
[[nodiscard]] std::vector<ConsumableOption> ScanCarriedConsumables(RE::Actor *actor);

// The effect a spell, power, shout word, potion or food is named by in the
// Effect condition: its effect that lasts -- more than a second, which a
// tick half a second apart can see, or every effect of a constant one, an
// ability -- the shown one before a hidden one, then the costliest. Null
// where nothing lasts: Firebolt, Fast Healing, a Restore Health potion, a
// teleport's momentary script, a toggle's power.
[[nodiscard]] const RE::Effect *LastingEffect(const RE::MagicItem *item);

// The Effect condition's picks (core/Effects.h), from the scans the
// editor's menus already make: the potions and food carried, the spells
// known and the scrolls carried, the shouts and the powers -- a toggle's
// power by the ability its script turns on (game/Toggles.h).
[[nodiscard]] std::vector<ft::EffectPick> ScanEffectPicks(const std::vector<SpellOption> &spells,
                                                          const std::vector<ConsumableOption> &consumables);

// Which consumable kind an inventory object is, or nothing for what is
// neither eaten nor applied.
[[nodiscard]] std::optional<ft::ConsumableKind> ConsumableKindOf(RE::TESBoundObject *object);

// Every effect a consumable gives, by the name the game shows, with the
// item's magnitude and duration of each, marked harmful or not and judged a
// buff or not. EVERY one, the bane beside the boon: which of them a rule may
// choose the item by is policy, and it lives in core where it is tested
// (PotionStock::ChoosableBy). A potion's harmful side -- the Slow in Sleeping
// Tree Sap, the regeneration damage in a wine -- is no reason to drink it,
// and a poison is chosen for what it does to the enemy; core says so, not
// this. An ingredient eaten gives its FIRST effect and no other (the rest
// are for the alchemy table), so that one is the ingredient's effect.
[[nodiscard]] std::vector<ft::PotionStock::Effect> ConsumableEffects(const RE::Actor *actor, RE::MagicItem *item,
                                                                     ft::ConsumableKind kind);

// The two hidden perks that turn the Fortify skill values into anything:
// PerkSkillBoosts reads the enchantment values (OneHandedModifier and its
// kin), AlchemySkillBoosts the potion ones (OneHandedPowerModifier ...).
// The player carries both. On the records no follower does (dev/RESEARCH.md
// 6), and UESP agrees: Fortify One-handed on a follower's gauntlets does
// nothing. So the sheets multiply a Fortify value in only for an actor who
// has the perk that reads it, and say so otherwise.
[[nodiscard]] bool ReadsSkillMods(const RE::Actor *actor);
[[nodiscard]] bool ReadsSkillPowerMods(const RE::Actor *actor);

// Does this effect change anything for this actor (core/Effects.h)?
[[nodiscard]] bool EffectApplies(const RE::Actor *actor, const RE::EffectSetting *base);
} // namespace ft::game
