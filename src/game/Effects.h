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
#include <optional>
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

// Whether any effect of the item is hostile or detrimental: what tells a
// buff put on someone else from a spell cast at them.
[[nodiscard]] bool HasHarm(const RE::MagicItem &item);

// The illusion influence an effect of `spell` puts up, by the engine's
// effect types (dev/CONDITIONS.md 2): Calm, Fear and Turn Undead, Frenzy,
// and Rally in a spell with no harm in it; any effect of Skyrim.esm's Call
// to Arms. Nothing for any other.
[[nodiscard]] std::optional<ft::StatusKind> InfluenceOf(const RE::EffectSetting &base, const RE::MagicItem *spell);

// Would this effect of `spell`, cast by `caster`, take on `target` and act:
// the entry's conditions, of the target with the caster, then the engine's
// own test as an effect lands (addr::kCheckAddEffect, dev/CONDITIONS.md 2),
// asked with the resistance the engine would hand it -- the record's
// conditions, and for an influence the level its magnitude reaches with the
// caster's perks and, `dual`, a dual cast. Game thread.
[[nodiscard]] bool LandsOn(RE::Effect &effect, RE::MagicItem *spell, RE::Actor *caster, RE::Actor *target, bool dual);

// A cast of `form` by `caster`, into the snapshot as core's WouldHaveEffect
// weighs it (SpellState's casts and landings): whom it reaches, and whether
// anything of it would take on each actor the snapshot holds that it can
// reach. After the
// party, the enemies and the corpses are in the snapshot. Game thread.
void AddLandings(RE::Actor *caster, std::uint32_t form, ft::Snapshot &s);

// The Effect condition's picks (core/Effects.h): what anyone in the party
// can put up, on themselves or on someone else. The page's own from the
// scans the editor's menus already make, `spells` and `consumables`; each of
// `others`' scanned here. The potions and food carried, the spells known and
// the scrolls carried, the shouts and the powers -- one whose script puts up
// another spell by that spell (game/Toggles.h).
[[nodiscard]] std::vector<ft::EffectPick> ScanEffectPicks(const std::vector<SpellOption> &spells,
                                                          const std::vector<ConsumableOption> &consumables,
                                                          const std::vector<RE::Actor *> &others);

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
// are for the alchemy table), so that one is the ingredient's effect. One
// the engine would not land on the actor consuming it (LandsOn) is left
// out, and `anyLands` says whether any of them, listed or not, would land.
// Game thread.
[[nodiscard]] std::vector<ft::PotionStock::Effect> ConsumableEffects(RE::Actor *actor, RE::MagicItem *item,
                                                                     ft::ConsumableKind kind, bool *anyLands = nullptr);

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
