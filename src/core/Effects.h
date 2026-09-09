#pragma once
// The effects a potion or a poison can be chosen by, arranged for the menu:
// the vanilla ones in groups that go together -- the three restores, the
// three fortifies, the regenerations, the resistances, the weapon skills,
// the magic skills -- and whatever else is carried (a mod's effect, a
// custom brew's) after them by name. The list itself comes from the
// bottles in the bag; this only orders it.

#include "Kinds.h"

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

} // namespace ft
