#include "progression/core/Companion.h"

#include "core/I18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace fp
{
using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace
{

template <typename T, std::size_t N> int Sum(const std::array<T, N> &values) noexcept
{
    int total = 0;
    for (const T v : values)
        total += v;
    return total;
}

std::string Thousands(double value)
{
    std::string digits = std::to_string(static_cast<long long>(std::floor(value)));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<std::size_t>(i), ",");
    return digits;
}

} // namespace

Companion Enroll(FormKey key, std::string name)
{
    Companion c;
    c.key = std::move(key);
    c.name = std::move(name);
    return c;
}

// --- the level ---------------------------------------------------------------------

LevelProgress Progress(const Companion &c, int engineLevel, int playerLevel, const Rules &r) noexcept
{
    LevelProgress p;
    p.engine = std::max(engineLevel, 1);
    const double total = XpToReach(r, p.engine) + c.learning.xp;
    const int learned = LevelFor(r, total);
    const int cap = playerLevel + r.levelsAbovePlayer;
    p.level = std::max(p.engine, std::min(learned, cap));
    p.capped = learned > std::max(cap, p.engine);
    p.into = total - XpToReach(r, learned);
    p.toNext = LevelThreshold(r, learned);
    return p;
}

int Level(const Companion &c, int engineLevel, int playerLevel, const Rules &r) noexcept
{
    return Progress(c, engineLevel, playerLevel, r).level;
}

// --- learning by doing ------------------------------------------------------------

Practice Practise(Companion &c, Skill skill, double points, int base, const SkillUsage &usage, const Rules &r) noexcept
{
    const std::size_t i = Index(skill);
    int level = base + c.learning.skills[i];
    Practice out;
    out.reached = level;
    // The player's own UseSkill passes on nothing that is worth nothing (40488).
    if (points <= 0.0 || level >= r.skillCap)
        return out;
    double &progress = c.learning.progress[i];
    progress += SkillXp(usage, points);
    while (level < r.skillCap && progress >= SkillThreshold(r, usage, level))
    {
        progress -= SkillThreshold(r, usage, level);
        ++level;
        ++c.learning.skills[i];
        c.learning.xp += XpForSkillLevel(r, level);
        ++out.skillUps;
    }
    if (level >= r.skillCap)
        progress = 0.0;
    out.reached = level;
    return out;
}

PerSkill<int> Effective(const Companion &c, const PerSkill<int> &base) noexcept
{
    PerSkill<int> out{};
    for (std::size_t i = 0; i < kSkillCount; ++i)
        out[i] = base[i] + c.learning.skills[i];
    return out;
}

// --- points ------------------------------------------------------------------------

int HeldRanks(const PerkGraph &graph, const Holdings &holdings) noexcept
{
    int held = 0;
    for (std::size_t id = 0; id < graph.Size(); ++id)
        for (const PerkRank &rank : graph.Node(static_cast<int>(id)).ranks)
            if (holdings.innate.contains(rank.form) || holdings.learned.contains(rank.form))
                ++held;
    return held;
}

int PerkPoints(int level, int heldRanks) noexcept
{
    return std::max(level - 1 - heldRanks, 0);
}

int OwnAttributePoints(const PerAttribute<int> &base, const PerAttribute<int> &raceStart, const Rules &r) noexcept
{
    if (r.attributePerLevel <= 0)
        return 0;
    return std::max((Sum(base) - Sum(raceStart)) / r.attributePerLevel, 0);
}

int AttributePoints(const Companion &c, int level, int ownPoints) noexcept
{
    return std::max(std::max(level - 1 - ownPoints, 0) - Sum(c.learning.attributePoints), 0);
}

std::string ToAssign(int perkPoints, int attributePoints, double pool)
{
    std::vector<std::string> parts;
    if (pool >= 1.0)
        parts.push_back(TrFormat("{} XP to reassign", Thousands(pool)));
    if (attributePoints == 1)
        parts.emplace_back(Tr("an attribute point"));
    else if (attributePoints > 1)
        parts.push_back(TrFormat("{} attribute points", attributePoints));
    if (perkPoints == 1)
        parts.emplace_back(Tr("a perk point"));
    else if (perkPoints > 1)
        parts.push_back(TrFormat("{} perk points", perkPoints));
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
        out += (i == 0 ? "" : i + 1 == parts.size() ? Tr(" and ") : Tr(", ")) + parts[i];
    return out;
}

// --- reassigning --------------------------------------------------------------------

int SkillFloor(const Rules &r, int raceBonus) noexcept
{
    return r.skillStart + raceBonus;
}

AssignCheck CheckSkill(const Companion &c, Skill skill, int delta, const PerSkill<int> &base, int floor,
                       const PerkGraph &graph, const Holdings &holdings, const Rules &r)
{
    const std::size_t i = Index(skill);
    const int level = base[i] + c.learning.skills[i];
    if (delta > 0)
    {
        if (level >= r.skillCap)
            return {AssignBlock::AtCap, {}};
        if (c.learning.pool < XpForSkillLevel(r, level + 1))
            return {AssignBlock::NoPoints, {}};
        return {};
    }
    if (level <= floor)
        return {AssignBlock::AtFloor, {}};
    PerSkill<int> after = Effective(c, base);
    after[i] -= 1;
    for (const FormKey &form : Invalidated(graph, holdings, after))
        for (const LearnedPerk &p : c.perks)
            if (p.form == form)
                return {AssignBlock::PerkNeedsIt, p.name};
    return {};
}

void AssignSkill(Companion &c, Skill skill, int delta, int base, const Rules &r) noexcept
{
    const std::size_t i = Index(skill);
    const int level = base + c.learning.skills[i];
    if (delta > 0)
        c.learning.pool -= XpForSkillLevel(r, level + 1);
    else
        c.learning.pool += XpForSkillLevel(r, level);
    c.learning.skills[i] += delta > 0 ? 1 : -1;
    // The way to the next level starts over at the new one.
    c.learning.progress[i] = 0.0;
}

int AssignSkillAll(Companion &c, Skill skill, int direction, const PerSkill<int> &base, int floor,
                   const PerkGraph &graph, const Holdings &holdings, const Rules &r)
{
    const int step = direction < 0 ? -1 : +1;
    int moved = 0;
    // Bounded: each step moves a level, and a skill has at most the cap's.
    while (moved <= r.skillCap &&
           CheckSkill(c, skill, step, base, floor, graph, holdings, r).block == AssignBlock::None)
    {
        AssignSkill(c, skill, step, base[Index(skill)], r);
        ++moved;
    }
    return moved;
}

AssignBlock CheckAttribute(const Companion &c, Attribute attribute, int delta, int available, int base, int floor,
                           int step) noexcept
{
    if (delta > 0)
        return available >= 1 ? AssignBlock::None : AssignBlock::NoPoints;
    const std::size_t a = Index(attribute);
    const int points = c.learning.attributePoints[a];
    if (points > 0)
        return AssignBlock::None; // one of ours, back to the pool
    return base + c.learning.attributes[a] - step >= floor ? AssignBlock::None : AssignBlock::AtFloor;
}

void AssignAttribute(Companion &c, Attribute attribute, int delta, int step) noexcept
{
    const std::size_t a = Index(attribute);
    int &points = c.learning.attributePoints[a];
    int &value = c.learning.attributes[a];
    const int toward = delta > 0 ? +1 : -1;
    if (points != 0 && (points > 0) != (toward > 0))
    {
        // Back toward none: what one point was worth when it moved.
        value -= value / std::abs(points);
        points += toward;
        return;
    }
    value += toward * step;
    points += toward;
}

int AssignAttributeAll(Companion &c, Attribute attribute, int direction, int available, int base, int floor,
                       int step) noexcept
{
    const int delta = direction < 0 ? -1 : +1;
    int moved = 0;
    while (CheckAttribute(c, attribute, delta, available, base, floor, step) == AssignBlock::None)
    {
        AssignAttribute(c, attribute, delta, step);
        available -= delta;
        ++moved;
    }
    return moved;
}

AttributeButtons AttributeButtonsFor(const Companion &c, Attribute attribute, int available, int base, int floor,
                                     int step)
{
    const std::string name(Tr(Name(attribute)));
    AttributeButtons out;
    out.canLower = CheckAttribute(c, attribute, -1, available, base, floor, step) == AssignBlock::None;
    out.lower = out.canLower ? TrFormat("Click to reduce {}", name) : TrFormat("Already at minimum {}", name);
    out.lowest = out.canLower ? TrFormat("Click to reduce {} to minimum", name) : out.lower;
    out.canRaise = CheckAttribute(c, attribute, +1, available, base, floor, step) == AssignBlock::None;
    out.raise = out.canRaise ? TrFormat("Click to increase {}", name) : std::string(Tr("No attribute points available"));
    out.highest = out.canRaise ? TrFormat("Click to increase {} to maximum", name) : out.raise;
    return out;
}

std::vector<std::string> ResetPerks(Companion &c, Skill skill, const PerkGraph &graph, const Holdings &holdings)
{
    std::vector<std::string> gone;
    for (const int id : graph.Tree(skill))
    {
        const PerkNode &node = graph.Node(id);
        for (std::size_t r = 0; r < node.ranks.size(); ++r)
        {
            const FormKey &form = node.ranks[r].form;
            const std::string name = r > 0 ? node.name + " (" + std::to_string(r + 1) + ")" : node.name;
            if (Unlearn(c, form) || (holdings.innate.contains(form) && SetAsideRank(c, form)))
                gone.push_back(name);
        }
    }
    return gone;
}

bool HeldInTree(Skill skill, const PerkGraph &graph, const Holdings &holdings)
{
    for (const int id : graph.Tree(skill))
        for (const PerkRank &rank : graph.Node(id).ranks)
            if (holdings.innate.contains(rank.form) || holdings.learned.contains(rank.form))
                return true;
    return false;
}

SkillButtons ButtonsFor(const Companion &c, Skill skill, const PerSkill<int> &base, int floor, const PerkGraph &graph,
                        const Holdings &holdings, const Rules &r)
{
    SkillButtons out;
    const AssignCheck lower = CheckSkill(c, skill, -1, base, floor, graph, holdings, r);
    const AssignCheck raise = CheckSkill(c, skill, +1, base, floor, graph, holdings, r);
    out.canLower = lower.block == AssignBlock::None;
    const std::string cannotLower = lower.block == AssignBlock::PerkNeedsIt
                                        ? TrFormat("{} needs this skill level", lower.perk)
                                        : std::string(Tr("Already at minimum skill"));
    out.lower = out.canLower ? std::string(Tr("Click to reduce skill")) : cannotLower;
    out.lowest = out.canLower ? std::string(Tr("Click to reduce to minimum skill")) : cannotLower;
    out.canRaise = raise.block == AssignBlock::None;
    const std::string cannotRaise =
        raise.block == AssignBlock::AtCap ? std::string(Tr("Already at maximum skill")) : std::string(Tr("Not enough XP"));
    out.raise = out.canRaise ? std::string(Tr("Click to increase skill")) : cannotRaise;
    out.highest = out.canRaise ? std::string(Tr("Click to increase to maximum skill")) : cannotRaise;
    out.canResetPerks = HeldInTree(skill, graph, holdings);
    out.resetPerks =
        out.canResetPerks ? std::string(Tr("Click to reset perks")) : std::string(Tr("No perks to reset"));
    return out;
}

// --- the engine's side of it ----------------------------------------------------------

float WithLearned(float base, int learned, int cap) noexcept
{
    const auto top = static_cast<float>(cap);
    if (learned > 0)
        return base >= top ? base : std::min(base + static_cast<float>(learned), top);
    return std::max(base + static_cast<float>(learned), 0.0f);
}

// --- perks ---------------------------------------------------------------------------

Holdings HoldingsOf(const Companion &c, std::unordered_set<FormKey, FormKeyHash> onRecord)
{
    Holdings h;
    h.innate = std::move(onRecord);
    for (const FormKey &form : c.setAside)
        if (h.innate.erase(form) != 0)
            h.setAside.insert(form);
    for (const LearnedPerk &p : c.perks)
    {
        // A perk on their own record is theirs, whatever the ledger says: an
        // external grant of the same form must never read as refundable.
        if (!h.innate.contains(p.form))
            h.learned.insert(p.form);
    }
    return h;
}

bool Bought(const Companion &c, const FormKey &form) noexcept
{
    return std::any_of(c.perks.begin(), c.perks.end(), [&](const LearnedPerk &p) { return p.form == form; });
}

void Learn(Companion &c, const PerkNode &node, int rankIndex)
{
    if (rankIndex < 0 || static_cast<std::size_t>(rankIndex) >= node.ranks.size())
        return;
    const FormKey &form = node.ranks[static_cast<std::size_t>(rankIndex)].form;
    if (Bought(c, form))
        return;
    c.perks.push_back({form, node.name, rankIndex + 1});
}

bool Unlearn(Companion &c, const FormKey &form)
{
    const auto it = std::find_if(c.perks.begin(), c.perks.end(), [&](const LearnedPerk &p) { return p.form == form; });
    if (it == c.perks.end())
        return false;
    c.perks.erase(it);
    return true;
}

bool SetAsideRank(Companion &c, const FormKey &form)
{
    if (std::find(c.setAside.begin(), c.setAside.end(), form) != c.setAside.end())
        return false;
    c.setAside.push_back(form);
    return true;
}

bool RestoreRank(Companion &c, const FormKey &form)
{
    return std::erase(c.setAside, form) != 0;
}

bool Taught(const Companion &c, const FormKey &spell) noexcept
{
    return std::any_of(c.spells.begin(), c.spells.end(), [&](const TaughtSpell &s) { return s.spell == spell; });
}

void Teach(Companion &c, const SpellFacts &spell)
{
    if (Taught(c, spell.spell) || IsSpellSetAside(c, spell.spell))
        return;
    c.spells.push_back({spell.spell, spell.name});
}

bool Forget(Companion &c, const FormKey &spell)
{
    const auto it =
        std::find_if(c.spells.begin(), c.spells.end(), [&](const TaughtSpell &s) { return s.spell == spell; });
    if (it == c.spells.end())
        return false;
    c.spells.erase(it);
    return true;
}

bool SetAsideSpell(Companion &c, const SpellFacts &spell)
{
    if (spell.spell.Empty() || Taught(c, spell.spell) || IsSpellSetAside(c, spell.spell))
        return false;
    c.spellsSetAside.push_back({spell.spell, spell.name});
    return true;
}

bool RestoreSpell(Companion &c, const FormKey &spell)
{
    const auto it = std::find_if(c.spellsSetAside.begin(), c.spellsSetAside.end(),
                                 [&](const SpellAside &s) { return s.spell == spell; });
    if (it == c.spellsSetAside.end())
        return false;
    c.spellsSetAside.erase(it);
    return true;
}

bool IsSpellSetAside(const Companion &c, const FormKey &spell) noexcept
{
    return std::any_of(c.spellsSetAside.begin(), c.spellsSetAside.end(),
                       [&](const SpellAside &s) { return s.spell == spell; });
}

TomeRead ReadTome(Companion &c, const SpellFacts &spell, bool known)
{
    if (known || spell.spell.Empty())
        return TomeRead::Known;
    if (RestoreSpell(c, spell.spell))
        return TomeRead::Restored;
    Teach(c, spell);
    return TomeRead::Taught;
}

SpellForgotten ForgetSpell(Companion &c, const SpellFacts &spell, bool known)
{
    if (!known || spell.spell.Empty())
        return SpellForgotten::NotKnown;
    if (Forget(c, spell.spell))
        return SpellForgotten::Forgotten;
    return SetAsideSpell(c, spell) ? SpellForgotten::SetAside : SpellForgotten::NotKnown;
}

} // namespace fp
