#pragma once

#include "Files.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The skill trees Custom Skills Framework adds -- Exit-9B's SKSE plugin, not
// SKSE itself -- and the order a perk tree is listed in. The framework reads a
// JSON file per mod from Data/SKSE/Plugins/CustomSkills; this reads the same
// files into plain data, so the reading and the order can be tested. The game
// side resolves the forms the files name (game/CustomSkillsFramework).
namespace ft
{

// A form as the framework's files name one: "Stormcrown.esp|82D", the plugin
// and the form's id within it, in hex.
struct FormRef
{
    std::string plugin;
    std::uint32_t id{0};
};
[[nodiscard]] std::optional<FormRef> ParseFormRef(std::string_view text);

struct CustomSkillNode
{
    std::string id;
    std::optional<FormRef> perk; // the first rank; the rest chain from it
    double x{0.0};
    double y{0.0};
    std::vector<std::string> links; // the ids of the nodes this one leads to
};

struct CustomSkill
{
    std::string id;
    std::string name; // may be a translation key: "$CampingPlusPlus_Name"
    std::string description;
    std::optional<FormRef> level; // the global holding the skill's level; none for a tree without levels
    std::vector<CustomSkillNode> nodes;
};

// One file's skills. Empty, with `error` set, for text that is not the
// framework's JSON.
[[nodiscard]] std::vector<CustomSkill> ParseCustomSkills(std::string_view json, std::string *error = nullptr);

// A node of a perk tree as the order reads it: where the menu draws it
// across, and the nodes it leads to, by index.
struct TreeNodePlace
{
    double x{0.0};
    std::vector<std::size_t> children;
};

// A custom skill's nodes as places, the links resolved to indices. A link to
// no node in the skill is dropped.
[[nodiscard]] std::vector<TreeNodePlace> PlacesOf(const CustomSkill &skill);

// The order a tree's perks are listed in, as indices into `nodes`: by depth,
// the longest chain of links from a root, so every perk comes after all it
// depends on; within a depth left to right as the menu draws them, which is
// DESCENDING x, the menu mirroring the records' axis (Alchemy's Intensity at
// 3.64 is drawn far left and Concentration at 0.24 far right; Stormcrown's
// Sky Above at 1.68 left of Deep Breath at -1.68; 2026-09-15); then as given.
// A node on a cycle, which no tree should have, comes after the rest.
[[nodiscard]] std::vector<std::size_t> TreeOrder(const std::vector<TreeNodePlace> &nodes);

// How far a rank chain is walked. A perk names the first rank and the rest
// chain from it; a chain longer than this is not a rank chain, and one that
// loops would otherwise walk forever on the game thread.
inline constexpr std::size_t kMaxRanks = 16;

// A perk's ranks, first to last: the perk itself, then whatever `next`
// gives, stopping at the end of the chain, at the bound, or at a rank that
// has already been seen. Handles are compared and never dereferenced, so
// the walking is the engine's and the stopping is ours. A chain that comes
// round on itself gives its distinct ranks once rather than the bound's
// worth of repeats, which is what the two walks this replaces would have
// listed -- the same perk over and over, each reading as a rank of sixteen.
template <typename Handle, typename Next> [[nodiscard]] std::vector<Handle> RankChain(Handle first, Next next)
{
    std::vector<Handle> chain;
    for (Handle rank = first; rank && chain.size() < kMaxRanks; rank = next(rank))
    {
        if (std::find(chain.begin(), chain.end(), rank) != chain.end())
            break;
        chain.push_back(rank);
    }
    return chain;
}

// Every tree the framework's files add, in the order the files are given.
// The files are another mod's, so nothing in them may take the game down:
// `read` gives a file's text, `build` makes one skill's tree, and a file
// that will not parse, or a skill `build` throws over, costs that file or
// that skill alone -- the rest load. `report` is told the file's name, the
// skill's id where there is one, and what went wrong.
//
// The reads and the form lookups are the game's; which files and in what
// order is JsonFilesIn's (core/Files.h). This is the part in between: that
// one bad neighbour is not the end of the walk.
template <typename Read, typename Build, typename Report>
[[nodiscard]] auto LoadSkillTrees(const std::vector<std::filesystem::path> &files, Read read, Build build,
                                  Report report)
{
    std::vector<std::invoke_result_t<Build, const CustomSkill &, const std::string &>> out;
    for (const std::filesystem::path &file : files)
    {
        const std::string label = FileLabel(file);
        std::vector<CustomSkill> skills;
        try
        {
            std::string parseError;
            skills = ParseCustomSkills(read(file), &parseError);
            if (!parseError.empty())
            {
                report(label, std::string_view{}, std::string_view{parseError});
                continue;
            }
        }
        catch (const std::exception &e)
        {
            report(label, std::string_view{}, std::string_view{e.what()});
            continue;
        }
        for (const CustomSkill &skill : skills)
        {
            try
            {
                out.push_back(build(skill, label));
            }
            catch (const std::exception &e)
            {
                report(label, std::string_view{skill.id}, std::string_view{e.what()});
            }
        }
    }
    return out;
}

} // namespace ft
