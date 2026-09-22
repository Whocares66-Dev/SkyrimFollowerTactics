#pragma once
// The perk graph as a file: what dev/research/extract_perk_trees.py writes
// from Skyrim.esm (data/vanilla-perks.json), for the tests to read the
// eighteen vanilla trees without the game. The plugin builds its graph from
// the engine (progression/game/PerkTrees.cpp) and never reads a file.

#include "progression/core/Perks.h"

#include <optional>
#include <string>
#include <string_view>

namespace fp
{

// None for text that is not a graph; `why` says what was wrong.
[[nodiscard]] std::optional<PerkGraph> ReadPerkGraph(std::string_view text, std::string *why = nullptr);

} // namespace fp
