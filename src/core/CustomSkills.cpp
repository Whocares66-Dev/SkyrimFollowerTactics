#include "core/CustomSkills.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <numeric>
#include <unordered_map>

namespace ft
{
namespace
{

std::string StringOr(const nlohmann::json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::optional<FormRef> FormOr(const nlohmann::json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? ParseFormRef(it->get<std::string>()) : std::nullopt;
}

double NumberOr(const nlohmann::json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : 0.0;
}

} // namespace

std::optional<FormRef> ParseFormRef(std::string_view text)
{
    const auto bar = text.find('|');
    if (bar == std::string_view::npos || bar == 0 || bar + 1 >= text.size())
        return std::nullopt;
    std::string_view hex = text.substr(bar + 1);
    if (hex.starts_with("0x") || hex.starts_with("0X"))
        hex.remove_prefix(2);
    std::uint32_t id = 0;
    const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
    if (ec != std::errc{} || end != hex.data() + hex.size())
        return std::nullopt;
    return FormRef{std::string(text.substr(0, bar)), id};
}

std::vector<CustomSkill> ParseCustomSkills(std::string_view json, std::string *error)
{
    std::vector<CustomSkill> out;
    const auto root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (!root.is_object() || !root.contains("skills") || !root.at("skills").is_array())
    {
        if (error)
            *error = root.is_discarded() ? "not JSON" : "no skills array";
        return out;
    }
    for (const auto &entry : root.at("skills"))
    {
        if (!entry.is_object())
            continue;
        CustomSkill skill;
        skill.id = StringOr(entry, "id");
        skill.name = StringOr(entry, "name");
        skill.description = StringOr(entry, "description");
        skill.level = FormOr(entry, "level");
        if (const auto nodes = entry.find("nodes"); nodes != entry.end() && nodes->is_array())
        {
            for (const auto &item : *nodes)
            {
                if (!item.is_object())
                    continue;
                CustomSkillNode node;
                node.id = StringOr(item, "id");
                node.perk = FormOr(item, "perk");
                node.x = NumberOr(item, "x");
                node.y = NumberOr(item, "y");
                if (const auto links = item.find("links"); links != item.end() && links->is_array())
                {
                    for (const auto &link : *links)
                        if (link.is_string())
                            node.links.push_back(link.get<std::string>());
                }
                skill.nodes.push_back(std::move(node));
            }
        }
        out.push_back(std::move(skill));
    }
    return out;
}

std::vector<TreeNodePlace> PlacesOf(const CustomSkill &skill)
{
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < skill.nodes.size(); ++i)
        index.emplace(skill.nodes[i].id, i);
    std::vector<TreeNodePlace> places(skill.nodes.size());
    for (std::size_t i = 0; i < skill.nodes.size(); ++i)
    {
        places[i].x = skill.nodes[i].x;
        for (const std::string &link : skill.nodes[i].links)
        {
            if (const auto it = index.find(link); it != index.end() && it->second != i)
                places[i].children.push_back(it->second);
        }
    }
    return places;
}

std::vector<std::size_t> TreeOrder(const std::vector<TreeNodePlace> &nodes)
{
    const std::size_t count = nodes.size();
    std::vector<std::size_t> unplacedParents(count, 0);
    for (const TreeNodePlace &node : nodes)
    {
        for (const std::size_t child : node.children)
            if (child < count)
                ++unplacedParents[child];
    }

    // A node's depth is settled once every parent has been placed: the
    // longest chain, not the first one found.
    std::vector<int> depth(count, 0);
    std::vector<bool> placed(count, false);
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < count; ++i)
        if (unplacedParents[i] == 0)
            ready.push_back(i);
    while (!ready.empty())
    {
        const std::size_t i = ready.back();
        ready.pop_back();
        placed[i] = true;
        for (const std::size_t child : nodes[i].children)
        {
            if (child >= count)
                continue;
            depth[child] = (std::max)(depth[child], depth[i] + 1);
            if (--unplacedParents[child] == 0)
                ready.push_back(child);
        }
    }
    int deepest = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (placed[i])
            deepest = (std::max)(deepest, depth[i]);
    for (std::size_t i = 0; i < count; ++i)
        if (!placed[i])
            depth[i] = deepest + 1;

    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (depth[a] != depth[b])
            return depth[a] < depth[b];
        return nodes[a].x > nodes[b].x;
    });
    return order;
}

} // namespace ft
