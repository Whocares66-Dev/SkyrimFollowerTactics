#include "progression/game/PerkTrees.h"

#include "progression/game/Forms.h"
#include "progression/game/Log.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace fp::game
{
namespace
{

PerkGraph g_graph;
// A rank's runtime id to its node (NodeOfPerk).
std::unordered_map<RE::FormID, int> g_nodeOf;

std::optional<Comparison> ComparisonOf(RE::CONDITION_ITEM_DATA::OpCode op)
{
    using Op = RE::CONDITION_ITEM_DATA::OpCode;
    switch (op)
    {
    case Op::kEqualTo:
        return Comparison::Equal;
    case Op::kNotEqualTo:
        return Comparison::NotEqual;
    case Op::kGreaterThan:
        return Comparison::Greater;
    case Op::kGreaterThanOrEqualTo:
        return Comparison::GreaterOrEqual;
    case Op::kLessThan:
        return Comparison::Less;
    case Op::kLessThanOrEqualTo:
        return Comparison::LessOrEqual;
    default:
        return std::nullopt;
    }
}

// One condition item as the core models it. The actor value functions take
// the value's number in their first parameter; HasPerk takes the perk.
Condition Convert(const RE::TESConditionItem &item)
{
    using F = RE::FUNCTION_DATA::FunctionID;
    Condition c;
    const auto &data = item.data;
    const F function = data.functionData.function.get();
    const auto param = data.functionData.params[0];
    switch (function)
    {
    case F::kGetBaseActorValue:
        c.function = ConditionFunction::GetBaseActorValue;
        c.actorValue = static_cast<int>(reinterpret_cast<std::intptr_t>(param));
        break;
    case F::kGetActorValue:
        c.function = ConditionFunction::GetActorValue;
        c.actorValue = static_cast<int>(reinterpret_cast<std::intptr_t>(param));
        break;
    case F::kHasPerk:
        if (auto key = KeyOf(static_cast<RE::TESForm *>(param)))
        {
            c.function = ConditionFunction::HasPerk;
            c.perk = std::move(*key);
        }
        else
        {
            c.function = ConditionFunction::Other;
            c.otherName = "HasPerk (a perk with no plugin)";
        }
        break;
    default:
        c.function = ConditionFunction::Other;
        c.otherName = fmt::format("condition function {}", static_cast<int>(function));
        break;
    }
    c.comparison = ComparisonOf(data.flags.opCode).value_or(Comparison::Equal);
    c.value =
        data.flags.global ? (data.comparisonValue.g ? data.comparisonValue.g->value : 0.0f) : data.comparisonValue.f;
    c.orNext = data.flags.isOR;
    return c;
}

PerkRank RankOf(RE::BGSPerk *perk, const FormKey &key)
{
    PerkRank rank;
    rank.form = key;
    RE::BSString text;
    perk->GetDescription(text, perk); // the perk as its own parent, as Companions' Path asks
    rank.description = text.c_str() ? text.c_str() : "";
    for (auto *item = perk->perkConditions.head; item; item = item->next)
        rank.conditions.push_back(Convert(*item));
    return rank;
}

EffectSummary EffectsOf(RE::BGSPerk *perk)
{
    EffectSummary summary;
    for (auto *entry : perk->perkEntries)
    {
        if (!entry)
            continue;
        switch (entry->GetType())
        {
        case RE::PERK_ENTRY_TYPE::kEntryPoint:
            summary.entryPoints.push_back(
                static_cast<int>(static_cast<RE::BGSEntryPointPerkEntry *>(entry)->entryData.entryPoint.get()));
            break;
        case RE::PERK_ENTRY_TYPE::kAbility:
            summary.ability = true;
            break;
        case RE::PERK_ENTRY_TYPE::kQuest:
        default:
            summary.quest = true;
            break;
        }
    }
    return summary;
}

void ReadTree(Skill skill, RE::BGSSkillPerkTreeNode *root, std::unordered_set<RE::BGSPerk *> &seen)
{
    std::queue<RE::BGSSkillPerkTreeNode *> todo;
    std::unordered_set<RE::BGSSkillPerkTreeNode *> visited;
    todo.push(root);
    while (!todo.empty())
    {
        auto *node = todo.front();
        todo.pop();
        if (!node || !visited.insert(node).second)
            continue;
        for (auto *child : node->children)
            todo.push(child);

        RE::BGSPerk *first = node->perk;
        if (!first || !seen.insert(first).second)
            continue;
        const auto firstKey = KeyOf(first);
        if (!firstKey)
            continue;

        PerkNode out;
        out.skill = skill;
        out.name = NameOf(first);
        out.x = node->horizontalPosition;
        out.y = node->verticalPosition;
        EffectSummary effects;
        std::unordered_set<RE::BGSPerk *> chain;
        for (RE::BGSPerk *rank = first; rank && chain.insert(rank).second; rank = rank->nextPerk)
        {
            const auto key = KeyOf(rank);
            if (!key)
                break;
            out.ranks.push_back(RankOf(rank, *key));
            const EffectSummary more = EffectsOf(rank);
            effects.entryPoints.insert(effects.entryPoints.end(), more.entryPoints.begin(), more.entryPoints.end());
            effects.ability = effects.ability || more.ability;
            effects.quest = effects.quest || more.quest;
            if (rank != first)
                seen.insert(rank);
        }
        Verdict verdict = Classify(*firstKey, effects);
        out.effect = verdict.effect;
        out.note = std::move(verdict.note);
        g_graph.Add(std::move(out));
    }
}

} // namespace

void BuildPerkGraph()
{
    g_graph.Clear();
    std::unordered_set<RE::BGSPerk *> seen;
    for (const Skill skill : AllSkills())
    {
        auto *info = RE::ActorValueList::GetActorValueInfo(static_cast<RE::ActorValue>(ActorValueOf(skill)));
        if (!info || !info->perkTree)
        {
            log::perks.warn("{} has no perk tree", Name(skill));
            continue;
        }
        const std::size_t before = g_graph.Size();
        ReadTree(skill, info->perkTree, seen);
        log::perks.debug("{}: {} perks", Name(skill), g_graph.Size() - before);
    }
    std::size_t ranks = 0;
    std::size_t works = 0;
    for (const auto &node : g_graph.Nodes())
    {
        ranks += node.ranks.size();
        works += node.effect == PerkEffect::Works ? 1 : 0;
    }
    log::perks.info("read {} perks ({} ranks) from the skill trees; {} work for companions", g_graph.Size(), ranks,
                    works);
    // Each rank's runtime id to its node, for Tactics' tree.
    for (const auto &node : g_graph.Nodes())
        for (const PerkRank &rank : node.ranks)
            if (const RE::BGSPerk *perk = PerkOf(rank.form))
                g_nodeOf.emplace(perk->GetFormID(), node.id);
}

const PerkGraph &Graph()
{
    return g_graph;
}

std::optional<int> NodeOfPerk(RE::FormID perk)
{
    const auto it = g_nodeOf.find(perk);
    return it == g_nodeOf.end() ? std::nullopt : std::optional<int>(it->second);
}

RE::BGSPerk *PerkOf(const FormKey &form)
{
    return Lookup<RE::BGSPerk>(form);
}

} // namespace fp::game
