#pragma once
// Custom Skills Framework's trees -- Exit-9B's SKSE plugin, not SKSE itself --
// resolved to the game's forms for the Skills tab. The framework reads a JSON
// file per mod from Data/SKSE/Plugins/CustomSkills; these are the same files,
// read once on first use. The reading and the order are core/CustomSkills.

#include <string>
#include <vector>

namespace RE
{
class BGSPerk;
class TESGlobal;
} // namespace RE

namespace ft::game
{

struct CustomSkillPerk
{
    RE::BGSPerk *perk{nullptr};
    int rank{1};  // 1-based position in the perk's rank chain
    int ranks{1}; // length of that chain
};

struct CustomSkillTree
{
    std::string name;                   // translated where the file gives a key
    RE::TESGlobal *level{nullptr};      // none for a tree without levels
    std::vector<CustomSkillPerk> perks; // in the tree's order, each chain's ranks in turn
};

// Every tree the installed files add, by file name and then as each file
// lists them. Empty where the framework's files are not installed.
[[nodiscard]] const std::vector<CustomSkillTree> &CustomSkillTrees();

} // namespace ft::game
