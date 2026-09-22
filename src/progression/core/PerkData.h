#pragma once
// The perk graph as a file: what dev/research/extract_perk_trees.py writes
// from Skyrim.esm (tests/progression/data/vanilla-perks.json), and what the plugin can
// write of the trees it read from the running game, to compare the two. The
// tests and the pacing simulator read the vanilla file; the plugin builds
// its graph from the engine (progression/game/PerkTrees.cpp) and never needs this.
// No Skyrim.

#include "progression/core/Perks.h"

#include <optional>
#include <string>
#include <string_view>

namespace fp
{

// A node's verdict comes from the file when it has one, else from
// Classify() over the node's "effects". None for text that is not a graph;
// `why` says what was wrong.
[[nodiscard]] std::optional<PerkGraph> ReadPerkGraph(std::string_view text, std::string *why = nullptr);

// Every node, with its verdict and note.
[[nodiscard]] std::string WritePerkGraph(const PerkGraph &graph);

} // namespace fp
