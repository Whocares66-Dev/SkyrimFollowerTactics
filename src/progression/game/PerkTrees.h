#pragma once
// The perk trees as the running game has them: every skill's tree, read off
// the engine at data load into the core's graph (progression/core/Perks.h), with each
// rank's conditions and a verdict on each node from what its effects hook
// into. Built once on the game thread and never changed after, so the panel
// may read it from the render thread.

#include "progression/core/Perks.h"

#include <filesystem>
#include <optional>

namespace fp::game
{

void BuildPerkGraph();

[[nodiscard]] const PerkGraph &Graph();

[[nodiscard]] RE::BGSPerk *PerkOf(const FormKey &form);

// The graph's node that `perk`, a rank's runtime id, belongs to; none for a
// perk in no tree read. Built with the graph, so any thread may ask.
[[nodiscard]] std::optional<int> NodeOfPerk(RE::FormID perk);

} // namespace fp::game
