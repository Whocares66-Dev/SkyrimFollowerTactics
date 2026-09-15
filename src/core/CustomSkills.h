#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

} // namespace ft
