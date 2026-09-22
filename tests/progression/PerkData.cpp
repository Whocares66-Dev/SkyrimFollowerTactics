#include "PerkData.h"

#include <nlohmann/json.hpp>

namespace fp
{
namespace
{

using nlohmann::json;

std::optional<Comparison> ComparisonFrom(std::string_view op)
{
    if (op == "==")
        return Comparison::Equal;
    if (op == "!=")
        return Comparison::NotEqual;
    if (op == ">")
        return Comparison::Greater;
    if (op == ">=")
        return Comparison::GreaterOrEqual;
    if (op == "<")
        return Comparison::Less;
    if (op == "<=")
        return Comparison::LessOrEqual;
    return std::nullopt;
}

} // namespace

std::optional<PerkGraph> ReadPerkGraph(std::string_view text, std::string *why)
{
    const auto fail = [&](std::string reason) -> std::optional<PerkGraph> {
        if (why)
            *why = std::move(reason);
        return std::nullopt;
    };
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("nodes") || !j["nodes"].is_array())
        return fail("not a perk graph");

    PerkGraph graph;
    std::size_t index = 0;
    for (const json &n : j["nodes"])
    {
        ++index;
        const auto skill = SkillFromKey(n.value("skill", std::string{}));
        if (!skill)
            return fail("node " + std::to_string(index) + " has no skill");
        PerkNode node;
        node.skill = *skill;
        node.name = n.value("name", std::string{});
        node.x = n.value("x", 0.0f);
        node.y = n.value("y", 0.0f);
        for (const json &r : n.value("ranks", json::array()))
        {
            PerkRank rank;
            const auto form = ParseFormKey(r.value("form", std::string{}));
            if (!form)
                return fail("a rank of " + node.name + " has no form");
            rank.form = *form;
            rank.description = r.value("description", std::string{});
            for (const json &c : r.value("conditions", json::array()))
            {
                Condition cond;
                const std::string fn = c.value("fn", std::string{});
                if (fn == "GetBaseActorValue")
                    cond.function = ConditionFunction::GetBaseActorValue;
                else if (fn == "GetActorValue")
                    cond.function = ConditionFunction::GetActorValue;
                else if (fn == "HasPerk")
                    cond.function = ConditionFunction::HasPerk;
                else
                {
                    cond.function = ConditionFunction::Other;
                    cond.otherName = c.value("name", fn);
                }
                cond.actorValue = c.value("av", -1);
                if (cond.function == ConditionFunction::HasPerk)
                {
                    const auto perk = ParseFormKey(c.value("perk", std::string{}));
                    if (!perk)
                        return fail("a condition of " + node.name + " names no perk");
                    cond.perk = *perk;
                }
                const auto op = ComparisonFrom(c.value("op", std::string("==")));
                if (!op)
                    return fail("a condition of " + node.name + " has an unknown comparison");
                cond.comparison = *op;
                cond.value = c.value("value", 0.0f);
                cond.orNext = c.value("or", false);
                rank.conditions.push_back(std::move(cond));
            }
            node.ranks.push_back(std::move(rank));
        }
        if (node.ranks.empty())
            return fail(node.name + " has no ranks");

        graph.Add(std::move(node));
    }
    return graph;
}

} // namespace fp
