#include "progression/core/Perks.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using fp::Comparison;
using fp::Condition;
using fp::ConditionFunction;
using fp::FormKey;
using fp::PerkBlock;
using fp::PerkEffect;
using fp::Skill;

namespace
{

// A small One-Handed tree with the shapes the vanilla ones have: a ranked
// root, children that need it, a node with two parents as an OR group (with
// the trailing OR vanilla writes), and a no-effect node standing between a
// parent and a useful child.
constexpr int kOneHanded = 6;

FormKey F(std::uint32_t local)
{
    return {"Test.esp", local};
}

Condition AtLeast(int level)
{
    return {ConditionFunction::GetBaseActorValue,
            kOneHanded,
            {},
            Comparison::GreaterOrEqual,
            static_cast<float>(level),
            false,
            {}};
}

Condition Has(std::uint32_t local, bool orNext = false)
{
    return {ConditionFunction::HasPerk, -1, F(local), Comparison::Equal, 1.0f, orNext, {}};
}

fp::PerkNode Node(std::string name, std::vector<fp::PerkRank> ranks, PerkEffect effect = PerkEffect::Works,
                  float y = 0.0f)
{
    fp::PerkNode n;
    n.skill = fp::Skill::OneHanded;
    n.name = std::move(name);
    n.ranks = std::move(ranks);
    n.effect = effect;
    n.y = y;
    return n;
}

struct Tree
{
    fp::PerkGraph graph;
    int armsman, stance, savage, charge, paralyze, zoom, shot;

    Tree()
    {
        armsman = graph.Add(Node("Armsman", {{F(0x10), "", {}}, {F(0x11), "", {AtLeast(20), Has(0x10)}}}));
        stance = graph.Add(Node("Stance", {{F(0x20), "", {Has(0x10), AtLeast(20)}}}));
        savage = graph.Add(Node("Savage", {{F(0x30), "", {Has(0x20), AtLeast(50)}}}, PerkEffect::Works, 0.4f));
        charge = graph.Add(Node("Charge", {{F(0x40), "", {Has(0x20), AtLeast(50)}}}, PerkEffect::Situational, 0.2f));
        paralyze = graph.Add(Node("Paralyze", {{F(0x50), "", {AtLeast(100), Has(0x40, true), Has(0x30, true)}}}));
        zoom = graph.Add(Node("Zoom", {{F(0x60), "", {Has(0x10), AtLeast(30)}}}, PerkEffect::NoEffect));
        shot = graph.Add(Node("Shot", {{F(0x70), "", {Has(0x60), AtLeast(50)}}}));
    }
};

fp::PerSkill<int> OneHanded(int level)
{
    fp::PerSkill<int> s{};
    s.fill(15);
    s[fp::Index(fp::Skill::OneHanded)] = level;
    return s;
}

} // namespace

TEST_CASE("the first rank of a root needs only a point", "[perks]")
{
    Tree t;
    fp::Holdings h;
    const auto skills = OneHanded(15);
    CHECK(fp::Status({t.graph, h, skills, 1}, t.armsman).block == PerkBlock::None);
    CHECK(fp::Status({t.graph, h, skills, 0}, t.armsman).block == PerkBlock::NoPoints);
}

TEST_CASE("a perk that needs another says so before it says the skill", "[perks]")
{
    Tree t;
    fp::Holdings h;
    auto skills = OneHanded(15);
    auto status = fp::Status({t.graph, h, skills, 1}, t.stance);
    CHECK(status.block == PerkBlock::Requires);
    REQUIRE(status.requirements.size() == 2);
    CHECK(status.requirements[0].kind == fp::Requirement::Kind::Perk);
    CHECK(status.requirements[0].name == "Armsman");
    CHECK_FALSE(status.requirements[0].met);

    h.learned.insert(F(0x10));
    status = fp::Status({t.graph, h, skills, 1}, t.stance);
    CHECK(status.block == PerkBlock::Skill);
    CHECK(status.requirements[1].need == 20);
    CHECK(status.requirements[1].have == 15);

    skills = OneHanded(20);
    CHECK(fp::Status({t.graph, h, skills, 1}, t.stance).block == PerkBlock::None);
}

TEST_CASE("a rank's need of the rank below is the chain, not a listed requirement", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.learned.insert(F(0x10));
    const auto skills = OneHanded(19);
    const auto status = fp::Status({t.graph, h, skills, 1}, t.armsman);
    CHECK(status.held == 1);
    CHECK(status.learned == 1);
    CHECK(status.block == PerkBlock::Skill);
    REQUIRE(status.requirements.size() == 1);
    CHECK(status.requirements[0].kind == fp::Requirement::Kind::Skill);
}

TEST_CASE("two parents joined by OR: either one will do", "[perks]")
{
    Tree t;
    fp::Holdings h;
    const auto skills = OneHanded(100);
    auto status = fp::Status({t.graph, h, skills, 1}, t.paralyze);
    CHECK(status.block == PerkBlock::Requires);
    const auto perks = std::count_if(status.requirements.begin(), status.requirements.end(),
                                     [](const fp::Requirement &r) { return r.kind == fp::Requirement::Kind::Perk; });
    CHECK(perks == 2);
    for (const auto &r : status.requirements)
        if (r.kind == fp::Requirement::Kind::Perk)
            CHECK(r.alternative);

    h.learned.insert(F(0x30)); // Savage only
    CHECK(fp::Status({t.graph, h, skills, 1}, t.paralyze).block == PerkBlock::None);
    CHECK(fp::ConditionsMet({t.graph, h, skills, 1}, t.graph.Node(t.paralyze).ranks[0]));
}

TEST_CASE("a perk with no effect on companions is learned like any other, to reach what needs it", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.learned.insert(F(0x10));

    // Short of its skill, it waits for the skill, as any perk does.
    CHECK(fp::Status({t.graph, h, OneHanded(25), 1}, t.zoom).block == PerkBlock::Skill);

    // With it, it can be bought; what needs it waits until it is.
    const auto skills = OneHanded(50);
    CHECK(fp::Status({t.graph, h, skills, 1}, t.zoom).block == PerkBlock::None);
    CHECK_FALSE(fp::Held({t.graph, h, skills, 1}, F(0x60)));
    CHECK(fp::Status({t.graph, h, skills, 1}, t.shot).block == PerkBlock::Requires);
    h.learned.insert(F(0x60));
    CHECK(fp::Status({t.graph, h, skills, 1}, t.shot).block == PerkBlock::None);
}

TEST_CASE("perks they came with count, and are never ours to unlearn", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.innate.insert(F(0x10));
    const auto skills = OneHanded(20);
    const auto status = fp::Status({t.graph, h, skills, 1}, t.armsman);
    CHECK(status.held == 1);
    CHECK(status.innate == 1);
    CHECK(status.learned == 0);
    CHECK_FALSE(status.canUnlearn);
    CHECK(fp::Status({t.graph, h, skills, 1}, t.stance).block == PerkBlock::None);
}

TEST_CASE("a higher rank held implies the ranks below it", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.innate.insert(F(0x11)); // the second Armsman, without the first
    const auto skills = OneHanded(15);
    const auto armsman = fp::Status({t.graph, h, skills, 1}, t.armsman);
    CHECK(armsman.held == 2);
    CHECK(armsman.innate == 2);
    CHECK(armsman.block == PerkBlock::Maxed);
    CHECK(fp::Held({t.graph, h, skills, 1}, F(0x10)));
    // Stance needs the first rank, which the second implies.
    CHECK(fp::Status({t.graph, h, OneHanded(20), 1}, t.stance).block == PerkBlock::None);
}

TEST_CASE("a perk something else of ours needs cannot be unlearned", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.learned = {F(0x10), F(0x20)};
    const auto skills = OneHanded(30);
    const auto root = fp::Status({t.graph, h, skills, 0}, t.armsman);
    CHECK_FALSE(root.canUnlearn);
    CHECK(root.dependants == std::vector<int>{t.stance});
    CHECK(fp::Status({t.graph, h, skills, 0}, t.stance).canUnlearn);
}

TEST_CASE("unlearning takes the top rank, and only when it is ours", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.innate.insert(F(0x10));
    h.learned.insert(F(0x11));
    const auto skills = OneHanded(30);
    const auto status = fp::Status({t.graph, h, skills, 0}, t.armsman);
    CHECK(status.held == 2);
    CHECK(status.canUnlearn);
    CHECK(status.block == PerkBlock::Maxed);
}

TEST_CASE("a lower skill invalidates what needed it, and what needed that", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.learned = {F(0x10), F(0x11), F(0x20), F(0x30)};
    CHECK(fp::Invalidated(t.graph, h, OneHanded(50)).empty());
    CHECK(fp::Invalidated(t.graph, h, OneHanded(25)) == std::vector<FormKey>{F(0x30)});
    // At 15 the second Armsman, Stance and Savage all fail; the higher rank
    // is listed first.
    const auto gone = fp::Invalidated(t.graph, h, OneHanded(15));
    CHECK(gone == std::vector<FormKey>{F(0x11), F(0x20), F(0x30)});
}

TEST_CASE("perks that fail only once another goes are found too", "[perks]")
{
    // Y needs nothing but X; X needs One-Handed 40. At 30, X fails on its
    // own and Y only once X is gone.
    fp::PerkGraph graph;
    graph.Add(Node("X", {{F(0x80), "", {AtLeast(40)}}}));
    graph.Add(Node("Y", {{F(0x90), "", {Has(0x80)}}}));
    fp::Holdings h;
    h.learned = {F(0x80), F(0x90)};
    CHECK(fp::Invalidated(graph, h, OneHanded(40)).empty());
    CHECK(fp::Invalidated(graph, h, OneHanded(30)) == std::vector<FormKey>{F(0x80), F(0x90)});
}

TEST_CASE("setting aside a perk something bought here needs is what WouldBreak finds", "[perks]")
{
    Tree t;
    fp::Holdings h;
    h.innate = {F(0x10)};           // Armsman, their own
    h.learned = {F(0x20), F(0x30)}; // Stance and Savage, bought here
    const auto skills = OneHanded(60);
    const fp::PerkRules rules{t.graph, h, skills, 0};
    const std::vector<FormKey> armsman{F(0x10)};
    // Direct dependants: Stance needs Armsman. Savage needs Stance, which is
    // still held -- one is enough to refuse.
    CHECK(fp::WouldBreak(rules, armsman) == std::vector<int>{t.stance});
    const std::vector<FormKey> savage{F(0x30)};
    CHECK(fp::WouldBreak(rules, savage).empty());
}

TEST_CASE("a node's parents are the perks its first rank names", "[perks]")
{
    Tree t;
    CHECK(t.graph.Parents(t.armsman).empty());
    CHECK(t.graph.Parents(t.stance) == std::vector<int>{t.armsman});
    CHECK(t.graph.Parents(t.paralyze) == std::vector<int>{t.charge, t.savage});
}

TEST_CASE("a tree lists by requirement, then as the Skills menu places them", "[perks]")
{
    Tree t;
    const auto order = t.graph.Tree(fp::Skill::OneHanded);
    REQUIRE(order.size() == 7);
    CHECK(order.front() == t.armsman);
    // Charge and Savage both need 50; Charge is placed higher.
    const auto charge = std::find(order.begin(), order.end(), t.charge);
    const auto savage = std::find(order.begin(), order.end(), t.savage);
    CHECK(charge < savage);
    CHECK(order.back() == t.paralyze);
}

TEST_CASE("the requirement shown for a rank is its own skill's threshold", "[perks]")
{
    Tree t;
    CHECK(fp::SkillRequirement(t.graph.Node(t.armsman).ranks[0], fp::Skill::OneHanded) == 0);
    CHECK(fp::SkillRequirement(t.graph.Node(t.armsman).ranks[1], fp::Skill::OneHanded) == 20);
    CHECK(fp::SkillRequirement(t.graph.Node(t.paralyze).ranks[0], fp::Skill::OneHanded) == 100);
    CHECK(fp::SkillRequirement(t.graph.Node(t.paralyze).ranks[0], fp::Skill::Block) == 0);
}

TEST_CASE("the catalog reads a perk's effects", "[perks]")
{
    const FormKey modded{"Ordinator.esp", 0x1};
    CHECK(fp::Classify(modded, {{35}, false, false}).effect == PerkEffect::Works);
    CHECK(fp::Classify(modded, {{18}, false, false}).effect == PerkEffect::Situational);
    CHECK(fp::Classify(modded, {{54}, false, false}).effect == PerkEffect::Unverified);
    CHECK(fp::Classify(modded, {{20, 27}, false, false}).effect == PerkEffect::NoEffect);
    CHECK(fp::Classify(modded, {{}, true, false}).effect == PerkEffect::Works);
    CHECK(fp::Classify(modded, {{}, false, false}).effect == PerkEffect::Unverified);
    CHECK(fp::Classify(modded, {{150}, false, false}).effect == PerkEffect::Unverified);
    // Judged by what it does, whatever tree it is in.
}

TEST_CASE("a condition compares as the engine does, whichever way it is written", "[perks]")
{
    // One rank gated on One-Handed against 30 by `op`: learnable at `level`?
    const auto passes = [](Comparison op, int level) {
        fp::PerkGraph graph;
        Condition c = AtLeast(30);
        c.comparison = op;
        graph.Add(Node("Test", {{F(0x80), "", {c}}}));
        const fp::Holdings h;
        const auto skills = OneHanded(level);
        return fp::Status({graph, h, skills, 1}, 0).block == PerkBlock::None;
    };
    CHECK(passes(Comparison::Equal, 30));
    CHECK_FALSE(passes(Comparison::Equal, 31));
    CHECK(passes(Comparison::NotEqual, 31));
    CHECK_FALSE(passes(Comparison::NotEqual, 30));
    CHECK(passes(Comparison::Greater, 31));
    CHECK_FALSE(passes(Comparison::Greater, 30));
    CHECK(passes(Comparison::GreaterOrEqual, 30));
    CHECK_FALSE(passes(Comparison::GreaterOrEqual, 29));
    CHECK(passes(Comparison::Less, 29));
    CHECK_FALSE(passes(Comparison::Less, 30));
    CHECK(passes(Comparison::LessOrEqual, 30));
    CHECK_FALSE(passes(Comparison::LessOrEqual, 31));
}

TEST_CASE("a perk can ask that another is not held", "[perks]")
{
    // HasPerk(0x10) == 0: a mod's either-or pair.
    fp::PerkGraph graph;
    Condition without = Has(0x10);
    without.value = 0.0f;
    graph.Add(Node("Either", {{F(0x10), "", {}}}));
    const int other = graph.Add(Node("Or", {{F(0x90), "", {without}}}));
    fp::Holdings h;
    const auto skills = OneHanded(15);
    CHECK(fp::Status({graph, h, skills, 1}, other).block == PerkBlock::None);
    h.learned.insert(F(0x10));
    CHECK(fp::Status({graph, h, skills, 1}, other).block != PerkBlock::None);
}

TEST_CASE("what a condition asks of something that is no skill is left to the engine", "[perks]")
{
    // An actor value that is no skill (Health), and a function Progression
    // does not read (a quest stage, a faction rank): met here, as nothing
    // here can judge them; the engine's own HasPerk has the last word.
    fp::PerkGraph graph;
    Condition health = AtLeast(500);
    health.actorValue = 24;
    Condition other = AtLeast(1000);
    other.function = ConditionFunction::Other;
    graph.Add(Node("Test", {{F(0xA0), "", {health, other}}}));
    const fp::Holdings h;
    const auto skills = OneHanded(15);
    CHECK(fp::Status({graph, h, skills, 1}, 0).block == PerkBlock::None);
}

TEST_CASE("a graph read again starts empty", "[perks]")
{
    Tree t;
    REQUIRE(t.graph.Size() == 7);
    t.graph.Clear();
    CHECK(t.graph.Size() == 0);
    CHECK_FALSE(t.graph.Find(F(0x10)).has_value());
    CHECK(t.graph.Tree(Skill::OneHanded).empty());
}
