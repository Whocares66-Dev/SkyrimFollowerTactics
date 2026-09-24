#include "progression/core/Companion.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using fp::Companion;
using fp::FormKey;
using fp::Skill;

namespace
{

Companion Lydia()
{
    return fp::Enroll({"Skyrim.esm", 0x0A2C8E}, "Lydia");
}

fp::PerSkill<int> Base(int value = 20)
{
    fp::PerSkill<int> b{};
    b.fill(value);
    return b;
}

// Rules whose numbers are easy to follow: a skill's next level costs its
// level in skill XP, and a use is worth its points.
fp::Rules Plain()
{
    fp::Rules r;
    r.skillUseCurve = 1.0;
    return r;
}
const fp::SkillUsage kPlainUsage{1.0, 0.0, 1.0, 0.0};

fp::PerkGraph BladeTree()
{
    // One perk that needs One-Handed 25.
    fp::PerkGraph graph;
    fp::PerkNode node;
    node.name = "Blade";
    node.tree = Skill::OneHanded;
    node.ranks = {
        {{"Test.esp", 1},
         "",
         {{fp::ConditionFunction::GetBaseActorValue, 6, {}, fp::Comparison::GreaterOrEqual, 25.0f, false, {}}}}};
    graph.Add(node);
    return graph;
}

} // namespace

TEST_CASE("enrolling starts with nothing learned", "[companion]")
{
    const Companion c = Lydia();
    CHECK(c.learning == fp::Learning{});
    CHECK(c.perks.empty());
    CHECK(c.spells.empty());
}

TEST_CASE("using a skill raises it as the player's own use does", "[companion]")
{
    const fp::Rules r = Plain();
    Companion c = Lydia();

    auto p = fp::Practise(c, Skill::OneHanded, 20.0, 20, kPlainUsage, r); // 20 of 20 to the next level
    CHECK(p.skillUps == 1);
    CHECK(p.reached == 21);
    CHECK(c.learning.skills[fp::Index(Skill::OneHanded)] == 1);
    CHECK(c.learning.xp == 21.0); // a skill-up gives its new level

    p = fp::Practise(c, Skill::OneHanded, 45.0, 20, kPlainUsage, r); // 21 and 22 fit, 2 left over
    CHECK(p.skillUps == 2);
    CHECK(p.reached == 23);
    CHECK(c.learning.progress[fp::Index(Skill::OneHanded)] == 2.0);
    CHECK(c.learning.xp == 21.0 + 22.0 + 23.0);

    // Nothing for nothing, or at the cap.
    CHECK(fp::Practise(c, Skill::OneHanded, 0.0, 20, kPlainUsage, r).skillUps == 0);
    CHECK(fp::Practise(c, Skill::Block, 500.0, 100, kPlainUsage, r).skillUps == 0);
}

TEST_CASE("the level stacks on the engine's, capped above the player, never below the engine", "[companion]")
{
    const fp::Rules r; // levels 75 + 25 x level, 5 above the player
    Companion c = Lydia();
    CHECK(fp::Level(c, 30, 40, r) == 30);

    // Onmund, held at 30: 3,000 learned on top reaches 33.
    c.learning.xp = 3000.0;
    CHECK(fp::Level(c, 30, 40, r) == 33);

    // One who levels with the player: no more than 5 above.
    c.learning.xp = 1'000'000.0;
    const auto capped = fp::Progress(c, 30, 30, r);
    CHECK(capped.level == 35);
    CHECK(capped.capped);

    // Erandur's x1.5 puts them above the player: the cap never pulls them
    // down, and with nothing learned they are not at the limit either.
    c.learning.xp = 1'000'000.0;
    CHECK(fp::Level(c, 30, 20, r) == 30);
    c.learning.xp = 0.0;
    CHECK_FALSE(fp::Progress(c, 30, 20, r).capped);
}

TEST_CASE("points are what a player at the level would have had, less what they have", "[companion]")
{
    const fp::Rules r;
    CHECK(fp::PerkPoints(10, 3) == 6);
    CHECK(fp::PerkPoints(3, 5) == 0);
    CHECK(fp::OwnAttributePoints({150, 100, 100}, {100, 100, 100}, r) == 5);
    CHECK(fp::OwnAttributePoints({95, 100, 100}, {100, 100, 100}, r) == 0);

    Companion c = Lydia();
    fp::AssignAttribute(c, fp::Attribute::Health, +1, 10);
    fp::AssignAttribute(c, fp::Attribute::Health, +1, 10);
    CHECK(fp::AttributePoints(c, 10, 5) == 2);
    CHECK(fp::CheckAttribute(c, fp::Attribute::Magicka, +1, 0, 100, 100, 10) == fp::AssignBlock::NoPoints);
    // Their own Magicka at their race's start: nothing to take back.
    CHECK(fp::CheckAttribute(c, fp::Attribute::Magicka, -1, 2, 100, 100, 10) == fp::AssignBlock::AtFloor);
    CHECK(fp::CheckAttribute(c, fp::Attribute::Magicka, -1, 2, 150, 100, 10) == fp::AssignBlock::None);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Health)] == 20);

    // A point keeps what it added when assigned, as the player's level-ups
    // do: a setting changed later moves only the points after it.
    fp::AssignAttribute(c, fp::Attribute::Health, +1, 20);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Health)] == 40);
    fp::AssignAttribute(c, fp::Attribute::Health, -1, 20);
    CHECK(c.learning.attributePoints[fp::Index(fp::Attribute::Health)] == 2);

    CHECK(fp::ToAssign(1, 2, 0.0) == "2 attribute points and a perk point");
    CHECK(fp::ToAssign(0, 0, 205.0) == "205 XP to reassign");
    CHECK(fp::ToAssign(0, 0, 0.0).empty());
}

TEST_CASE("a level taken back pays for another, down to the floor and up to the cap", "[companion]")
{
    const fp::Rules r = Plain();
    const fp::PerkGraph none;
    const fp::Holdings holdings;
    const fp::PerSkill<int> base = Base(20);
    const int floor = fp::SkillFloor(r, 0); // 15
    Companion c = Lydia();
    c.learning.skills[fp::Index(Skill::OneHanded)] = 10; // One-Handed 30
    const auto check = [&](Skill skill, int delta) {
        return fp::CheckSkill(c, skill, delta, base, floor, none, holdings, r).block;
    };

    CHECK(check(Skill::TwoHanded, +1) == fp::AssignBlock::NoPoints); // nothing in the pool yet
    REQUIRE(check(Skill::OneHanded, -1) == fp::AssignBlock::None);
    fp::AssignSkill(c, Skill::OneHanded, -1, 20, r);
    CHECK(c.learning.pool == 30.0); // level 30, returned

    REQUIRE(check(Skill::TwoHanded, +1) == fp::AssignBlock::None);
    fp::AssignSkill(c, Skill::TwoHanded, +1, 20, r); // level 21 costs 21
    CHECK(c.learning.pool == 9.0);
    CHECK(check(Skill::TwoHanded, +1) == fp::AssignBlock::NoPoints);

    // Below their own value, down to where a new character starts it.
    c.learning.skills[fp::Index(Skill::OneHanded)] = -5; // 15
    CHECK(check(Skill::OneHanded, -1) == fp::AssignBlock::AtFloor);
    fp::PerSkill<int> high = base;
    high[fp::Index(Skill::Archery)] = 100;
    c.learning.pool = 1000.0;
    CHECK(fp::CheckSkill(c, Skill::Archery, +1, high, floor, none, holdings, r).block == fp::AssignBlock::AtCap);
}

TEST_CASE("a level a bought perk needs stays, until the perks are reset", "[companion]")
{
    const fp::Rules r = Plain();
    const fp::PerkGraph graph = BladeTree();
    Companion c = Lydia();
    c.learning.skills[fp::Index(Skill::OneHanded)] = 5; // 20 + 5 = 25
    fp::Learn(c, graph.Node(0), 0);

    const auto lower = fp::CheckSkill(c, Skill::OneHanded, -1, Base(20), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK(lower.block == fp::AssignBlock::PerkNeedsIt);
    CHECK(lower.perk == "Blade");

    fp::ResetPerks(c, Skill::OneHanded, graph, fp::HoldingsOf(c, {}));
    CHECK(fp::CheckSkill(c, Skill::OneHanded, -1, Base(20), 15, graph, fp::HoldingsOf(c, {}), r).block ==
          fp::AssignBlock::None);
}

TEST_CASE("<< and >> move a skill as far as - and + would, a level at a time", "[companion]")
{
    const fp::Rules r = Plain();
    const fp::PerkGraph none;
    Companion c = Lydia();
    const auto all = [&](Skill skill, int direction, const fp::PerSkill<int> &base, const fp::PerkGraph &graph) {
        return fp::AssignSkillAll(c, skill, direction, base, 15, graph, fp::HoldingsOf(c, {}), r);
    };

    // Down to the floor, every level into the pool.
    CHECK(all(Skill::OneHanded, -1, Base(20), none) == 5);
    CHECK(c.learning.skills[fp::Index(Skill::OneHanded)] == -5);
    CHECK(c.learning.pool == 16.0 + 17 + 18 + 19 + 20);
    CHECK(all(Skill::OneHanded, -1, Base(20), none) == 0);

    // Up as far as the pool pays: a level costs what it is worth, so the 90
    // buys Archery 21 to 24 and nothing is left for 25.
    CHECK(all(Skill::Archery, +1, Base(20), none) == 4);
    CHECK(c.learning.skills[fp::Index(Skill::Archery)] == 4);
    CHECK(c.learning.pool == 0.0);
    CHECK(all(Skill::Archery, +1, Base(20), none) == 0);

    // Up to the cap, and no further, with the pool to spare.
    c.learning.pool = 1000.0;
    fp::PerSkill<int> high = Base(20);
    high[fp::Index(Skill::Block)] = 97;
    CHECK(all(Skill::Block, +1, high, none) == 3);
    CHECK(high[fp::Index(Skill::Block)] + c.learning.skills[fp::Index(Skill::Block)] == 100);
    CHECK(all(Skill::Block, +1, high, none) == 0);
    CHECK(c.learning.pool == 1000.0 - 98 - 99 - 100);

    // Down only as far as a perk bought here allows: Blade needs 25.
    const fp::PerkGraph graph = BladeTree();
    Companion d = Lydia();
    d.learning.skills[fp::Index(Skill::OneHanded)] = 10; // 30
    fp::Learn(d, graph.Node(0), 0);
    CHECK(fp::AssignSkillAll(d, Skill::OneHanded, -1, Base(20), 15, graph, fp::HoldingsOf(d, {}), r) == 5);
    CHECK(d.learning.skills[fp::Index(Skill::OneHanded)] == 5);
}

TEST_CASE("resetting a tree's perks gives back every rank held, theirs and bought, and only that tree's", "[companion]")
{
    fp::PerkGraph graph = BladeTree();
    fp::PerkNode ranked;
    ranked.name = "Armsman";
    ranked.tree = Skill::OneHanded;
    ranked.ranks = {{{"Test.esp", 2}, "", {}}, {{"Test.esp", 3}, "", {}}};
    graph.Add(ranked);
    fp::PerkNode other;
    other.name = "Overdraw";
    other.tree = Skill::Archery;
    other.ranks = {{{"Test.esp", 4}, "", {}}};
    graph.Add(other);

    Companion c = Lydia();
    fp::Learn(c, graph.Node(1), 1); // Armsman's second rank, bought
    fp::Learn(c, graph.Node(2), 0);
    // Blade and Armsman's first rank on their own record.
    const std::unordered_set<FormKey, fp::FormKeyHash> onRecord{{"Test.esp", 1}, {"Test.esp", 2}};
    const auto held = fp::HoldingsOf(c, onRecord);
    REQUIRE(fp::HeldInTree(Skill::OneHanded, graph, held));
    const auto gone = fp::ResetPerks(c, Skill::OneHanded, graph, held);
    // In the tree's order: Armsman needs nothing, Blade One-Handed 25.
    CHECK(gone == std::vector<std::string>{"Armsman", "Armsman (2)", "Blade"});
    REQUIRE(c.perks.size() == 1);
    CHECK(c.perks[0].name == "Overdraw");
    CHECK(c.setAside == std::vector<FormKey>{{"Test.esp", 2}, {"Test.esp", 1}});
    const auto after = fp::HoldingsOf(c, onRecord);
    CHECK_FALSE(fp::HeldInTree(Skill::OneHanded, graph, after));
    CHECK(fp::HeldInTree(Skill::Archery, graph, after));
    CHECK(fp::ResetPerks(c, Skill::OneHanded, graph, after).empty());
}

TEST_CASE("a custom tree's perks are a tree of their own, bought with the same points", "[companion]")
{
    // Blade in One-Handed; Dragonborn and Deep Breath, which asks for it, in
    // Stormcrown's tree.
    fp::PerkGraph graph = BladeTree();
    const fp::TreeRef dragonborn = fp::TreeRef::Custom("Dragonborn.json/Dragonborn");
    fp::PerkNode root;
    root.name = "Dragonborn";
    root.tree = dragonborn;
    root.ranks = {{{"Stormcrown.esp", 0x80F}, "", {}}};
    graph.Add(root);
    fp::PerkNode breath;
    breath.name = "Deep Breath";
    breath.tree = dragonborn;
    breath.ranks = {
        {{"Stormcrown.esp", 0x81D},
         "",
         {{fp::ConditionFunction::HasPerk, -1, {"Stormcrown.esp", 0x80F}, fp::Comparison::Equal, 1.0f, false, {}}}}};
    graph.Add(breath);

    // Neither tree lists the other's nodes; one custom tree is not another.
    CHECK(graph.Tree(Skill::OneHanded) == std::vector<int>{0});
    CHECK(graph.Tree(dragonborn) == std::vector<int>{1, 2});
    CHECK(graph.Tree(fp::TreeRef::Custom("Other.json/Dragonborn")).empty());

    // Deep Breath waits for its parent, as any tree's perk does, and costs a
    // point as any does.
    Companion c = Lydia();
    const fp::PerSkill<int> skills = Base(15);
    const auto before = fp::HoldingsOf(c, {});
    CHECK(fp::Status({graph, before, skills, 2}, 2).block == fp::PerkBlock::Requires);
    CHECK(fp::Status({graph, before, skills, 2}, 1).block == fp::PerkBlock::None);
    fp::Learn(c, graph.Node(1), 0);
    const auto after = fp::HoldingsOf(c, {});
    CHECK(fp::Status({graph, after, skills, 1}, 2).block == fp::PerkBlock::None);
    CHECK(fp::HeldRanks(graph, after) == 1);

    // Its reset gives back its own and nothing of One-Handed's.
    fp::Learn(c, graph.Node(0), 0);
    REQUIRE(fp::ResetButtonFor(dragonborn, graph, fp::HoldingsOf(c, {})).can);
    CHECK(fp::ResetPerks(c, dragonborn, graph, fp::HoldingsOf(c, {})) == std::vector<std::string>{"Dragonborn"});
    CHECK(fp::HeldInTree(Skill::OneHanded, graph, fp::HoldingsOf(c, {})));
    CHECK_FALSE(fp::HeldInTree(dragonborn, graph, fp::HoldingsOf(c, {})));
}

TEST_CASE("a skill reads with what they learned on top, within the cap", "[companion]")
{
    // The engine's 30 and five learned: 35.
    CHECK(fp::WithLearned(30.0f, 5, 100) == 35.0f);
    // Levels taken back below their own: 25, and never below 0.
    CHECK(fp::WithLearned(30.0f, -5, 100) == 25.0f);
    CHECK(fp::WithLearned(3.0f, -5, 100) == 0.0f);
    // The engine raised their own to 99 since: one of the two learned fits
    // under 100, and the other waits.
    CHECK(fp::WithLearned(99.0f, 2, 100) == 100.0f);
    // Already past the cap, by another mod: left as it is.
    CHECK(fp::WithLearned(110.0f, 2, 100) == 110.0f);
    CHECK(fp::WithLearned(30.0f, 0, 100) == 30.0f);
}

TEST_CASE("attribute points move one at a time or as far as they go, and say why they cannot", "[companion]")
{
    Companion c = Lydia();

    // No points to assign, none assigned, their own Health at their race's
    // start: nothing can move.
    auto b = fp::AttributeButtonsFor(c, fp::Attribute::Health, 0, 100, 100, 10);
    CHECK_FALSE(b.canLower);
    CHECK(b.lower == "Already at minimum Health");
    CHECK(b.lowest == "Already at minimum Health");
    CHECK_FALSE(b.canRaise);
    CHECK(b.raise == "No attribute points available");
    CHECK(b.highest == "No attribute points available");

    // Three to assign: >> takes all three, 10 each.
    b = fp::AttributeButtonsFor(c, fp::Attribute::Health, 3, 100, 100, 10);
    CHECK(b.canRaise);
    CHECK(b.raise == "Click to increase Health");
    CHECK(b.highest == "Click to increase Health to maximum");
    CHECK(fp::AssignAttributeAll(c, fp::Attribute::Health, +1, 3, 100, 100, 10) == 3);
    CHECK(c.learning.attributePoints[fp::Index(fp::Attribute::Health)] == 3);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Health)] == 30);

    // None left: + cannot; - and << can.
    b = fp::AttributeButtonsFor(c, fp::Attribute::Health, 0, 100, 100, 10);
    CHECK_FALSE(b.canRaise);
    CHECK(b.canLower);
    CHECK(b.lower == "Click to reduce Health");
    CHECK(b.lowest == "Click to reduce Health to minimum");

    // << gives every point back; Magicka's are untouched.
    fp::AssignAttribute(c, fp::Attribute::Magicka, +1, 10);
    CHECK(fp::AssignAttributeAll(c, fp::Attribute::Health, -1, 0, 100, 100, 10) == 3);
    CHECK(c.learning.attributePoints[fp::Index(fp::Attribute::Health)] == 0);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Health)] == 0);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Magicka)] == 10);
    CHECK(fp::AssignAttributeAll(c, fp::Attribute::Health, -1, 0, 100, 100, 10) == 0);
}

TEST_CASE("their own attribute values can be taken back to the race's start and spent elsewhere", "[companion]")
{
    // Serana at 50: their class put more into their values than 49 points
    // would, so their level leaves none to assign.
    Companion c = Lydia();
    constexpr int kOwnPoints = 83;
    CHECK(fp::AttributePoints(c, 50, kOwnPoints) == 0);

    // Health 541 over a race start of 50: three points taken back return
    // three to spend, and cost 10 of Health each.
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(fp::CheckAttribute(c, fp::Attribute::Health, -1, 0, 541, 50, 10) == fp::AssignBlock::None);
        fp::AssignAttribute(c, fp::Attribute::Health, -1, 10);
    }
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Health)] == -30);
    CHECK(fp::AttributePoints(c, 50, kOwnPoints) == 3);

    // Spent on Magicka: three points of 10.
    CHECK(fp::AssignAttributeAll(c, fp::Attribute::Magicka, +1, 3, 296, 50, 10) == 3);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Magicka)] == 30);
    CHECK(fp::AttributePoints(c, 50, kOwnPoints) == 0);

    // One moved back: Magicka gives a point back, Health takes it again.
    fp::AssignAttribute(c, fp::Attribute::Magicka, -1, 10);
    fp::AssignAttribute(c, fp::Attribute::Health, +1, 10);
    CHECK(c.learning.attributes[fp::Index(fp::Attribute::Health)] == -20);
    CHECK(c.learning.attributePoints[fp::Index(fp::Attribute::Health)] == -2);
    CHECK(fp::AttributePoints(c, 50, kOwnPoints) == 0);

    // << takes Health down to the race's start, and no further: 541 - 20
    // leaves 471 above 50, 47 points of 10, the last reaching 51.
    CHECK(fp::AssignAttributeAll(c, fp::Attribute::Health, -1, 0, 541, 50, 10) == 47);
    CHECK(541 + c.learning.attributes[fp::Index(fp::Attribute::Health)] == 51);
    CHECK(fp::CheckAttribute(c, fp::Attribute::Health, -1, 0, 541, 50, 10) == fp::AssignBlock::AtFloor);
    const auto b = fp::AttributeButtonsFor(c, fp::Attribute::Health, 49, 541, 50, 10);
    CHECK_FALSE(b.canLower);
    CHECK(b.lower == "Already at minimum Health");
    CHECK(b.canRaise);
}

TEST_CASE("a perk held counts against the level's points, and unlearning gives it back", "[companion]")
{
    Companion c = Lydia();
    fp::PerkGraph graph;
    fp::PerkNode armsman;
    armsman.name = "Armsman";
    armsman.tree = Skill::OneHanded;
    armsman.ranks = {{{"Skyrim.esm", 0x0BABE4}, "", {}}, {{"Skyrim.esm", 0x079343}, "", {}}};
    graph.Add(armsman);
    fp::Learn(c, graph.Node(0), 0);
    fp::Learn(c, graph.Node(0), 1);
    fp::Learn(c, graph.Node(0), 1); // twice is once
    CHECK(c.perks.size() == 2);
    CHECK(fp::HeldRanks(graph, fp::HoldingsOf(c, {})) == 2);

    CHECK(fp::Unlearn(c, {"Skyrim.esm", 0x079343}));
    CHECK(fp::HeldRanks(graph, fp::HoldingsOf(c, {})) == 1);
    CHECK_FALSE(fp::Unlearn(c, {"Skyrim.esm", 0x079343}));
}

TEST_CASE("a perk on their own record is never counted as bought here", "[companion]")
{
    Companion c = Lydia();
    c.perks.push_back({{"Skyrim.esm", 0x0BABE4}, "Armsman", 1});
    const auto holdings = fp::HoldingsOf(c, {{"Skyrim.esm", 0x0BABE4}});
    CHECK(holdings.innate.contains({"Skyrim.esm", 0x0BABE4}));
    CHECK_FALSE(holdings.learned.contains({"Skyrim.esm", 0x0BABE4}));
}

TEST_CASE("one of their own perks given back returns its point, and costs one to take up again", "[companion]")
{
    Companion c = Lydia();
    fp::PerkGraph graph;
    fp::PerkNode recovery;
    recovery.name = "Recovery";
    recovery.tree = Skill::Restoration;
    recovery.ranks = {{{"Skyrim.esm", 0x0581F4}, "", {}}, {{"Skyrim.esm", 0x0581F5}, "", {}}};
    graph.Add(recovery);
    // As Marcurio's record has it: the second rank without the first.
    const std::unordered_set<FormKey, fp::FormKeyHash> onRecord{{"Skyrim.esm", 0x0581F5}};
    CHECK(fp::HeldRanks(graph, fp::HoldingsOf(c, onRecord)) == 1);
    CHECK(fp::PerkPoints(5, fp::HeldRanks(graph, fp::HoldingsOf(c, onRecord))) == 3);

    CHECK(fp::SetAsideRank(c, {"Skyrim.esm", 0x0581F5}));
    CHECK_FALSE(fp::SetAsideRank(c, {"Skyrim.esm", 0x0581F5})); // twice is once
    const auto given = fp::HoldingsOf(c, onRecord);
    CHECK(given.setAside.contains({"Skyrim.esm", 0x0581F5}));
    CHECK_FALSE(given.innate.contains({"Skyrim.esm", 0x0581F5}));
    CHECK(fp::HeldRanks(graph, given) == 0);
    CHECK(fp::PerkPoints(5, fp::HeldRanks(graph, given)) == 4); // its point is free again

    CHECK(fp::RestoreRank(c, {"Skyrim.esm", 0x0581F5}));
    CHECK(fp::HoldingsOf(c, onRecord).innate.contains({"Skyrim.esm", 0x0581F5}));
    CHECK_FALSE(fp::RestoreRank(c, {"Skyrim.esm", 0x0581F5}));
}

TEST_CASE("spells taught here can be forgotten; others are not ours", "[companion]")
{
    Companion c = Lydia();
    const fp::SpellFacts flames{{"Skyrim.esm", 0x012FCD}, "Flames"};
    fp::Teach(c, flames);
    fp::Teach(c, flames);
    CHECK(c.spells.size() == 1);
    CHECK(fp::Taught(c, flames.spell));
    CHECK_FALSE(fp::Forget(c, {"Skyrim.esm", 0x02B96B}));
    CHECK(fp::Forget(c, flames.spell));
    CHECK_FALSE(fp::Taught(c, flames.spell));
}

TEST_CASE("one of their own spells can be set aside and taken up again, for nothing", "[companion]")
{
    Companion c = Lydia();
    const fp::SpellFacts sparks{{"Skyrim.esm", 0x02B96B}, "Sparks"};
    const fp::SpellFacts flames{{"Skyrim.esm", 0x012FCD}, "Flames"};

    CHECK(fp::SetAsideSpell(c, sparks));
    CHECK(fp::IsSpellSetAside(c, sparks.spell));
    CHECK_FALSE(fp::SetAsideSpell(c, sparks)); // twice is once
    CHECK(c.spellsSetAside.size() == 1);

    // Set aside, it is restored rather than taught again from a tome.
    fp::Teach(c, sparks);
    CHECK_FALSE(fp::Taught(c, sparks.spell));

    // Taught here, it is forgotten rather than set aside.
    fp::Teach(c, flames);
    CHECK_FALSE(fp::SetAsideSpell(c, flames));

    CHECK(fp::RestoreSpell(c, sparks.spell));
    CHECK_FALSE(fp::IsSpellSetAside(c, sparks.spell));
    CHECK_FALSE(fp::RestoreSpell(c, sparks.spell));
}

TEST_CASE("a skill's buttons say what a click does, or why it cannot", "[companion]")
{
    const fp::Rules r = Plain();
    Companion c = Lydia();
    const fp::PerkGraph graph = BladeTree();

    // At the floor, with nothing in the pool, nothing bought.
    auto b = fp::ButtonsFor(c, Skill::OneHanded, Base(15), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK_FALSE(b.canLower);
    CHECK(b.lower == "Already at minimum skill");
    CHECK(b.lowest == "Already at minimum skill");
    CHECK_FALSE(b.canRaise);
    CHECK(b.raise == "Not enough XP");
    CHECK(b.highest == "Not enough XP");
    const auto nothing = fp::ResetButtonFor(Skill::OneHanded, graph, fp::HoldingsOf(c, {}));
    CHECK_FALSE(nothing.can);
    CHECK(nothing.text == "No perks to reset");

    // Above it, with the pool: every one can act.
    c.learning.pool = 1500.0;
    b = fp::ButtonsFor(c, Skill::OneHanded, Base(20), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK(b.canLower);
    CHECK(b.lower == "Click to reduce skill");
    CHECK(b.lowest == "Click to reduce to minimum skill");
    CHECK(b.canRaise);
    CHECK(b.raise == "Click to increase skill");
    CHECK(b.highest == "Click to increase to maximum skill");

    // At the cap.
    b = fp::ButtonsFor(c, Skill::OneHanded, Base(100), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK_FALSE(b.canRaise);
    CHECK(b.raise == "Already at maximum skill");

    // A perk bought here that needs the level keeps - from taking it, and
    // there are perks to reset.
    fp::Learn(c, graph.Node(0), 0);
    b = fp::ButtonsFor(c, Skill::OneHanded, Base(25), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK_FALSE(b.canLower);
    CHECK(b.lower == "Blade needs this skill level");
    const auto reset = fp::ResetButtonFor(Skill::OneHanded, graph, fp::HoldingsOf(c, {}));
    CHECK(reset.can);
    CHECK(reset.text == "Click to reset perks");

    // Every skill alike: Smithing moves as One-Handed does.
    b = fp::ButtonsFor(c, Skill::Smithing, Base(25), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK(b.canLower);
    CHECK(b.canRaise);
}

TEST_CASE("resetting a tree's perks returns them and leaves the skill", "[companion]")
{
    Companion c = Lydia();
    const fp::PerkGraph graph = BladeTree();
    c.learning.skills[fp::Index(Skill::OneHanded)] = 12;
    fp::Learn(c, graph.Node(0), 0);
    REQUIRE(fp::HeldInTree(Skill::OneHanded, graph, fp::HoldingsOf(c, {})));
    const auto unlearned = fp::ResetPerks(c, Skill::OneHanded, graph, fp::HoldingsOf(c, {}));
    CHECK(unlearned == std::vector<std::string>{"Blade"});
    CHECK(c.perks.empty());
    CHECK_FALSE(fp::HeldInTree(Skill::OneHanded, graph, fp::HoldingsOf(c, {})));
    CHECK(c.learning.skills[fp::Index(Skill::OneHanded)] == 12);
}
