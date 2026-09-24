#include "progression/core/Perks.h"

#include "core/I18n.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fp
{
namespace
{

bool Compare(float lhs, Comparison op, float rhs) noexcept
{
    switch (op)
    {
    case Comparison::Equal:
        return lhs == rhs;
    case Comparison::NotEqual:
        return lhs != rhs;
    case Comparison::Greater:
        return lhs > rhs;
    case Comparison::GreaterOrEqual:
        return lhs >= rhs;
    case Comparison::Less:
        return lhs < rhs;
    case Comparison::LessOrEqual:
        return lhs <= rhs;
    default:
        return false;
    }
}

// Conditions evaluated against a companion's holdings. A perk that does
// nothing for a companion is held only as any other is: learned, as the
// player's would be, to reach what needs it.
class Evaluator
{
  public:
    explicit Evaluator(const PerkRules &rules) : rules_(rules)
    {
    }

    bool Direct(const FormKey &form) const
    {
        return rules_.holdings.innate.contains(form) || rules_.holdings.learned.contains(form);
    }

    // Held directly, or implied by a higher rank of the same node held
    // directly -- Marcurio's record carries Recovery's second rank without
    // its first.
    bool Held(const FormKey &form) const
    {
        if (Direct(form))
            return true;
        const auto found = rules_.graph.Find(form);
        if (!found)
            return false;
        const PerkNode &node = rules_.graph.Node(found->first);
        for (std::size_t r = static_cast<std::size_t>(found->second) + 1; r < node.ranks.size(); ++r)
            if (Direct(node.ranks[r].form))
                return true;
        return false;
    }

    bool One(const Condition &c)
    {
        switch (c.function)
        {
        case ConditionFunction::GetBaseActorValue:
        case ConditionFunction::GetActorValue: {
            const auto skill = SkillFromActorValue(c.actorValue);
            if (!skill)
                return true; // not a skill: nothing here to judge it by
            return Compare(static_cast<float>(rules_.skills[Index(*skill)]), c.comparison, c.value);
        }
        case ConditionFunction::HasPerk:
            return Compare(Held(c.perk) ? 1.0f : 0.0f, c.comparison, c.value);
        case ConditionFunction::Other:
        default:
            return true;
        }
    }

    // Skyrim's rule: a run of conditions joined by OR flags is one group,
    // and every group must pass.
    bool Met(const PerkRank &rank)
    {
        bool all = true;
        bool group = false;
        for (const Condition &c : rank.conditions)
        {
            group = group || One(c);
            if (!c.orNext)
            {
                all = all && group;
                group = false;
            }
        }
        // A trailing OR flag closes its group with the list.
        if (!rank.conditions.empty() && rank.conditions.back().orNext)
            all = all && group;
        return all;
    }

  private:
    const PerkRules &rules_;
};

} // namespace

// --- the graph -----------------------------------------------------------------

int PerkGraph::Add(PerkNode node)
{
    const int id = static_cast<int>(nodes_.size());
    node.id = id;
    for (std::size_t r = 0; r < node.ranks.size(); ++r)
        index_[node.ranks[r].form] = {id, static_cast<int>(r)};
    nodes_.push_back(std::move(node));
    return id;
}

const PerkNode &PerkGraph::Node(int id) const
{
    if (id < 0 || static_cast<std::size_t>(id) >= nodes_.size())
        throw std::out_of_range("no such perk node");
    return nodes_[static_cast<std::size_t>(id)];
}

std::optional<std::pair<int, int>> PerkGraph::Find(const FormKey &form) const
{
    const auto it = index_.find(form);
    if (it == index_.end())
        return std::nullopt;
    return it->second;
}

std::vector<int> PerkGraph::Tree(const TreeRef &tree) const
{
    std::vector<int> ids;
    for (const PerkNode &node : nodes_)
        if (node.tree == tree)
            ids.push_back(node.id);
    const auto requirement = [&tree](const PerkNode &node) {
        return node.ranks.empty() || !tree.skill ? 0 : SkillRequirement(node.ranks.front(), *tree.skill);
    };
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) {
        const PerkNode &na = nodes_[static_cast<std::size_t>(a)];
        const PerkNode &nb = nodes_[static_cast<std::size_t>(b)];
        const int ra = requirement(na);
        const int rb = requirement(nb);
        if (ra != rb)
            return ra < rb;
        return na.y < nb.y;
    });
    return ids;
}

std::vector<int> PerkGraph::Parents(int id) const
{
    std::vector<int> parents;
    const PerkNode &node = Node(id);
    if (node.ranks.empty())
        return parents;
    for (const Condition &c : node.ranks.front().conditions)
    {
        if (c.function != ConditionFunction::HasPerk || c.value < 1.0f)
            continue;
        const auto found = Find(c.perk);
        if (found && found->first != id && std::find(parents.begin(), parents.end(), found->first) == parents.end())
            parents.push_back(found->first);
    }
    return parents;
}

void PerkGraph::Clear() noexcept
{
    nodes_.clear();
    index_.clear();
}

int SkillRequirement(const PerkRank &rank, Skill skill) noexcept
{
    int need = 0;
    for (const Condition &c : rank.conditions)
    {
        if (c.function != ConditionFunction::GetBaseActorValue && c.function != ConditionFunction::GetActorValue)
            continue;
        if (SkillFromActorValue(c.actorValue) != skill)
            continue;
        if (c.comparison == Comparison::GreaterOrEqual)
            need = std::max(need, static_cast<int>(c.value));
        else if (c.comparison == Comparison::Greater)
            need = std::max(need, static_cast<int>(c.value) + 1);
    }
    return need;
}

// --- the rules -------------------------------------------------------------------

bool Held(const PerkRules &rules, const FormKey &form)
{
    Evaluator evaluator(rules);
    return evaluator.Held(form);
}

bool ConditionsMet(const PerkRules &rules, const PerkRank &rank)
{
    Evaluator evaluator(rules);
    return evaluator.Met(rank);
}

PerkStatus Status(const PerkRules &rules, int nodeId)
{
    PerkStatus status;
    const PerkNode &node = rules.graph.Node(nodeId);
    Evaluator evaluator(rules);

    // Held up to the highest rank held directly: the ranks below it are
    // implied, and count as theirs unless bought here.
    int top = -1;
    for (std::size_t r = 0; r < node.ranks.size(); ++r)
        if (evaluator.Direct(node.ranks[r].form))
            top = static_cast<int>(r);
    for (int r = 0; r <= top; ++r)
    {
        if (rules.holdings.learned.contains(node.ranks[static_cast<std::size_t>(r)].form))
            ++status.learned;
        else
            ++status.innate;
    }
    status.held = top + 1;

    // Giving back: the top rank held, bought here or their own, while
    // nothing held needs it.
    if (status.held > 0)
    {
        const FormKey &highest = node.ranks[static_cast<std::size_t>(status.held - 1)].form;
        const std::array<FormKey, 1> removing{highest};
        status.dependants = WouldBreak(rules, removing);
        status.canUnlearn = status.dependants.empty();
    }

    if (status.held >= static_cast<int>(node.ranks.size()))
    {
        status.block = PerkBlock::Maxed;
        return status;
    }

    // One of their own they gave back: theirs again for a point.
    const PerkRank &next = node.ranks[static_cast<std::size_t>(status.held)];
    if (rules.holdings.setAside.contains(next.form))
    {
        status.restores = true;
        status.block = rules.points > 0 ? PerkBlock::None : PerkBlock::NoPoints;
        return status;
    }

    // The next rank's conditions, each said in words. A HasPerk on this
    // node's own lower rank is the rank chain, which "2/5" says already.
    bool skillFails = false;
    bool perkFails = false;
    bool groupMet = false;
    std::size_t groupStart = 0;
    for (std::size_t i = 0; i < next.conditions.size(); ++i)
    {
        const Condition &c = next.conditions[i];
        const bool met = evaluator.One(c);
        const bool inGroup = c.orNext || (i > 0 && next.conditions[i - 1].orNext);
        Requirement req;
        req.met = met;
        req.alternative = inGroup;
        switch (c.function)
        {
        case ConditionFunction::GetBaseActorValue:
        case ConditionFunction::GetActorValue:
            if (const auto skill = SkillFromActorValue(c.actorValue))
            {
                req.kind = Requirement::Kind::Skill;
                req.skill = *skill;
                req.need = static_cast<int>(c.value) + (c.comparison == Comparison::Greater ? 1 : 0);
                req.have = rules.skills[Index(*skill)];
            }
            else
            {
                req.kind = Requirement::Kind::Other;
                req.name = ft::i18n::TrFormat("actor value {}", c.actorValue);
            }
            break;
        case ConditionFunction::HasPerk:
            req.kind = Requirement::Kind::Perk;
            if (const auto found = rules.graph.Find(c.perk))
            {
                req.node = found->first;
                req.name = rules.graph.Node(found->first).name;
            }
            else
                req.name = ToString(c.perk);
            break;
        case ConditionFunction::Other:
        default:
            req.kind = Requirement::Kind::Other;
            req.name = c.otherName;
            break;
        }
        const bool chain = req.kind == Requirement::Kind::Perk && req.node == nodeId;
        const bool negated = req.kind == Requirement::Kind::Perk && c.value < 1.0f;
        if (!chain && !negated)
            status.requirements.push_back(req);

        groupMet = groupMet || met;
        if (!c.orNext)
        {
            if (!groupMet)
                for (std::size_t j = groupStart; j <= i; ++j)
                {
                    const ConditionFunction f = next.conditions[j].function;
                    if (f == ConditionFunction::HasPerk)
                        perkFails = true;
                    else
                        skillFails = true;
                }
            groupMet = false;
            groupStart = i + 1;
        }
    }
    if (!next.conditions.empty() && next.conditions.back().orNext && !groupMet)
        perkFails = true;

    if (perkFails)
        status.block = PerkBlock::Requires;
    else if (skillFails)
        status.block = PerkBlock::Skill;
    else if (rules.points <= 0)
        status.block = PerkBlock::NoPoints;
    else
        status.block = PerkBlock::None;
    return status;
}

std::vector<int> WouldBreak(const PerkRules &rules, std::span<const FormKey> removing)
{
    Holdings without = rules.holdings;
    for (const FormKey &form : removing)
    {
        without.innate.erase(form);
        without.learned.erase(form);
    }
    const PerkRules after{rules.graph, without, rules.skills, rules.points};
    Evaluator before(rules);
    Evaluator check(after);
    std::vector<int> broken;
    const auto consider = [&](const FormKey &form) {
        const auto found = rules.graph.Find(form);
        if (!found)
            return;
        const PerkRank &rank = rules.graph.Node(found->first).ranks[static_cast<std::size_t>(found->second)];
        // Held now whatever it asks -- their own may never have met it --
        // and broken only by what goes.
        if (before.Met(rank) && !check.Met(rank) &&
            std::find(broken.begin(), broken.end(), found->first) == broken.end())
            broken.push_back(found->first);
    };
    for (const FormKey &form : without.learned)
        consider(form);
    for (const FormKey &form : without.innate)
        consider(form);
    std::sort(broken.begin(), broken.end());
    return broken;
}

std::vector<FormKey> Invalidated(const PerkGraph &graph, const Holdings &holdings, const PerSkill<int> &skills)
{
    std::vector<std::pair<int, FormKey>> gone; // rank index, form
    Holdings working = holdings;
    for (;;)
    {
        const PerkRules rules{graph, working, skills, 0};
        Evaluator evaluator(rules);
        std::vector<std::pair<int, FormKey>> pass;
        for (const FormKey &form : working.learned)
        {
            const auto found = graph.Find(form);
            if (!found)
                continue;
            const PerkRank &rank = graph.Node(found->first).ranks[static_cast<std::size_t>(found->second)];
            if (!evaluator.Met(rank))
                pass.push_back({found->second, form});
        }
        if (pass.empty())
            break;
        // The set's order is not stable; the result must be.
        std::sort(pass.begin(), pass.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first)
                return a.first > b.first;
            return a.second < b.second;
        });
        for (const auto &[rank, form] : pass)
        {
            working.learned.erase(form);
            gone.push_back({rank, form});
        }
    }
    std::stable_sort(gone.begin(), gone.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<FormKey> forms;
    forms.reserve(gone.size());
    for (auto &[rank, form] : gone)
        forms.push_back(std::move(form));
    return forms;
}

} // namespace fp
