#pragma once
// The Character tab, the combat style the actor fights by, and the
// Summons tab.

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
    // Each bar written out as the follower's are. A summoner's perks reach
    // the summon as effects running on it -- Adamant's are cloaks on the
    // summoner casting onto commanded actors near them -- so they list with
    // the summon's own (dev/MODIFIERS.md).
    ft::Breakdown healthBreakdown;
    ft::Breakdown magickaBreakdown;
    ft::Breakdown staminaBreakdown;
    float remaining{0.0f}; // seconds left on the effect that commands it; 0 when unknown
    // The time left written out: the spell's duration, the summoner's perks
    // on it, the time run. The summoner's perks shape the summon here too,
    // Dark Oath's "last twice as long" among them.
    ft::Breakdown remainingBreakdown;
    bool raised{false}; // a reanimated corpse, as opposed to a summon
    std::vector<SheetSection> sheet;
};

// Everything the follower commands, in the engine's order.
[[nodiscard]] std::vector<SummonView> ScanSummons(RE::Actor *actor);

// The Character tab: race, movement, defence and the equipped weapon. Display
// only -- none of it is a rule input. Cheap reads, done in and out of combat
// alike.
[[nodiscard]] std::vector<SheetSection> BuildCharacterSheet(RE::Actor *actor);

// The Tactics tab's Combat Style section: the numbers and flags the combat
// AI is tuned by, read off the style they are using right now -- their live
// combat controller's in a fight, their record's otherwise -- so a copy the
// panel gave them shows as what it is.
[[nodiscard]] std::vector<SheetSection> BuildCombatStyleSheet(RE::Actor *actor);

// The combat style the actor fights by: the controller's live copy when
// there is one, else the record's. Null for an actor with none.
[[nodiscard]] RE::TESCombatStyle *LiveCombatStyle(RE::Actor *actor);

// May the actor hold a one-handed weapon in each hand? The combat style's
// flag; an actor with no style may, and so may the player. The panel's cells, and a request, read
// it (core/Loadout.h WouldDualWield).
[[nodiscard]] bool DualWieldAllowed(RE::Actor *actor);
} // namespace ft::game
