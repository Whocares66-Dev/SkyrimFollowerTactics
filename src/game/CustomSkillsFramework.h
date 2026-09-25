#pragma once
// Custom Skills Framework's trees -- Exit-9B's SKSE plugin, not SKSE itself --
// resolved to the game's forms for the Skills tab. The framework reads a JSON
// file per mod from Data/SKSE/Plugins/CustomSkills; these are the same files,
// read once on first use. The reading and the order are core/CustomSkills.

#include <cstddef>
#include <string>
#include <vector>

namespace RE
{
class BGSPerk;
class TESGlobal;
} // namespace RE

namespace ft::game
{

// A node of a tree whose perk is in the load order: the perk and the ranks
// chained from it, where the file places it, and the nodes it leads to.
struct CustomTreeNode
{
    std::vector<RE::BGSPerk *> ranks; // first to last; never empty
    double x{0.0};
    double y{0.0};
    std::vector<std::size_t> children; // indices into the tree's nodes
};

struct CustomSkillTree
{
    std::string name;                  // translated where the file gives a key
    RE::TESGlobal *level{nullptr};     // none for a tree without levels
    std::vector<CustomTreeNode> nodes; // in the tree's order (ft::TreeOrder)
};

// Every tree the installed files add, by file name and then as each file
// lists them. Empty where the framework's files are not installed.
[[nodiscard]] const std::vector<CustomSkillTree> &CustomSkillTrees();

} // namespace ft::game
