#pragma once
// What a spell, an enchantment or a potion does, effect by effect, with the
// conditions that gate each, and the Effects tab's list of what runs on an
// actor.

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

// Who a record's conditions are asked of: the Subject and the Target the
// engine passes. For an effect that is the one it lands on and whoever
// cast it (dev/CONDITIONS.md 10). A party the page cannot name is null,
// and a condition that runs on it is listed unasked, N/A: asked of nobody
// the engine answers false, which is not "not met".
struct ConditionParties
{
    RE::TESObjectREFR *subject{nullptr};
    RE::TESObjectREFR *target{nullptr};
};

// What a spell, an enchantment or a potion does, effect by effect, as a
// perk's page lists its entries: the value each moves and by how much,
// else its kind and what it names; the engine's archetype, the duration,
// "hidden" where the game's list would not show it; each row opening on
// its conditions, with a tick where they hold for whom the effect would
// land on and the actor using it. `magnitude` says which number a row
// carries: the record's, or the caster's actual one. The record beside the
// author's prose, which says what they meant.
[[nodiscard]] SheetSection EffectsOf(RE::Actor *actor, const RE::MagicItem *magic,
                                     const std::function<float(const RE::Effect *)> &magnitude);
[[nodiscard]] SheetRow EffectEntryRow(const RE::Effect &effect, float magnitude, const ConditionParties &parties);

// One effect running on the follower, for the Effects tab: the effect as
// the game names it, its magnitude, what is left of it, and where it comes
// from -- the spell, the potion, or for an enchantment the worn item that
// carries it, "Robes of Health" rather than the enchantment record's name.
// Everything running on the follower that the game would list, sorted by
// name. Effects flagged hidden, and ones already run out, are left out.
[[nodiscard]] std::vector<EffectRow> ScanActiveEffects(RE::Actor *actor);

// The conditions of one list, a row each: the call, the comparison, and a
// tick where it holds for the parties. `on` names the entry's argument the
// tab is on, for a perk's tab other than the first (the owner): those are
// listed, not evaluated.
[[nodiscard]] std::vector<SheetRow> ConditionRows(const RE::TESCondition &condition, const ConditionParties &parties,
                                                  const char *on = nullptr);

// Whether a running effect moves this actor value: a value modifier on
// it, or a dual modifier with it as either half.
[[nodiscard]] bool ModifiesValue(const RE::ActiveEffect &ae, RE::ActorValue value);
} // namespace ft::game
