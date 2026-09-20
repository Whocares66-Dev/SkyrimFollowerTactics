#pragma once
// The effects a potion or a poison can be chosen by, arranged for the menu:
// the vanilla ones in groups that go together -- the three restores, the
// three fortifies, the regenerations, the resistances, the weapon skills,
// the magic skills -- and whatever else is carried (a mod's effect, a
// custom brew's) after them by name. The list itself comes from the
// bottles in the bag; this only orders it.

#include "Kinds.h"
#include "Snapshot.h"

#include <span>

#include <string>
#include <string_view>
#include <vector>

namespace ft
{

struct EffectEntry
{
    std::string name;
    // The menu group, for a divider between one and the next. The known
    // effects' groups come first; the unknown ones share the last.
    int group{0};
};

// The carried effects in menu order: the known groups in their order, each
// in its own order, then the rest alphabetically as one group. Duplicates
// are dropped.
[[nodiscard]] std::vector<EffectEntry> ArrangeEffects(ConsumableKind kind, std::vector<std::string> names);

// The word the menu shows for an effect: the three restores and the three
// damages are "Health", "Stamina", "Magicka" -- what the menu said for
// them before there were others -- and every other effect is its own
// name.
[[nodiscard]] std::string_view EffectLabel(std::string_view effect) noexcept;

// An effect no follower can use: the two about disease, which a follower
// never catches. Left out of the bottles' effects at the scan.
[[nodiscard]] bool EffectUseless(std::string_view effect) noexcept;

// An effect's record, as read, for whether it does anything for the actor
// and whether it is a buff.
struct EffectShape
{
    bool valueModifier{false}; // a Value, PeakValue or DualValue modifier archetype
    bool peakValue{false};     // the PeakValueModifier archetype in particular
    bool skillModifier{false}; // its value is a skill modifier (OneHandedModifier ... EnchantingModifier)
    bool skillPower{false};    // its value is a skill power modifier
    bool harmful{false};
    bool waterbreathing{false};
    float duration{0.0f};
};

// Does this effect change anything for this actor? A value-modifying
// effect on a skill modifier -- Fortify One-handed's OneHandedModifier,
// Fortify Destruction's DestructionModifier -- is read only by the two
// hidden perks a follower does not carry (dev/RESEARCH.md 6): the value
// moves, and nothing looks at it.
[[nodiscard]] bool EffectApplies(const EffectShape &shape, bool readsSkillMods, bool readsSkillPowerMods) noexcept;

// A buff: lasting, not harmful, a peak value modifier the actor reads, and
// not waterbreathing, which does nothing for a follower.
[[nodiscard]] bool IsBuff(const EffectShape &shape, bool readsSkillMods, bool readsSkillPowerMods) noexcept;

// One effect of a consumable as the game read it, in the item's own
// order.
struct ConsumableEffectSeen
{
    std::string name; // as the game shows it; empty for a nameless effect
    float magnitude{0.0f};
    float duration{0.0f};
    EffectShape shape;
};

// Every effect a consumable gives, by the name the game shows, marked
// harmful or not and judged a buff or not. EVERY one, the bane beside the
// boon: which of them a rule may choose the item by is policy, and lives
// in PotionStock::ChoosableBy. A nameless effect, and one no follower can
// use, are left out. An ingredient eaten gives its FIRST effect and no
// other -- the rest are for the alchemy table -- so only that one is
// looked at, and an ingredient whose first effect is left out gives
// nothing rather than falling through to the second.
[[nodiscard]] std::vector<PotionStock::Effect> ConsumableEffectsOf(std::span<const ConsumableEffectSeen> effects,
                                                                   ConsumableKind kind, bool readsSkillMods,
                                                                   bool readsSkillPowerMods);

} // namespace ft
