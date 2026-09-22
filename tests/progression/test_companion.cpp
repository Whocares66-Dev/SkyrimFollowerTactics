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
    node.skill = Skill::OneHanded;
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

    // Nothing for nothing, at the cap, or while paused.
    CHECK(fp::Practise(c, Skill::OneHanded, 0.0, 20, kPlainUsage, r).skillUps == 0);
    CHECK(fp::Practise(c, Skill::Block, 500.0, 100, kPlainUsage, r).skillUps == 0);
    c.paused = true;
    CHECK(fp::Practise(c, Skill::OneHanded, 500.0, 20, kPlainUsage, r).skillUps == 0);
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
    CHECK(fp::CheckAttribute(c, fp::Attribute::Magicka, +1, 0) == fp::AssignBlock::NoPoints);
    CHECK(fp::CheckAttribute(c, fp::Attribute::Magicka, -1, 2) == fp::AssignBlock::NoneAssigned);
    CHECK(fp::Pending(c, Base(20), 100).attributes[fp::Index(fp::Attribute::Health)] == 20);

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

TEST_CASE("a level a bought perk needs stays, until a reset returns both", "[companion]")
{
    const fp::Rules r = Plain();
    const fp::PerkGraph graph = BladeTree();
    Companion c = Lydia();
    c.learning.skills[fp::Index(Skill::OneHanded)] = 5; // 20 + 5 = 25
    fp::Learn(c, graph.Node(0), 0);

    const auto lower = fp::CheckSkill(c, Skill::OneHanded, -1, Base(20), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK(lower.block == fp::AssignBlock::PerkNeedsIt);
    CHECK(lower.perk == "Blade");

    // Reset, as a skill made Legendary: to the floor, the levels and the
    // tree's bought perks returned.
    const auto reset = fp::ResetSkill(c, Skill::OneHanded, 20, 15, graph, r);
    CHECK(reset.returned == 16.0 + 17 + 18 + 19 + 20 + 21 + 22 + 23 + 24 + 25);
    CHECK(reset.unlearned == std::vector<std::string>{"Blade"});
    CHECK(c.perks.empty());
    CHECK(c.learning.skills[fp::Index(Skill::OneHanded)] == -5);
    CHECK(c.learning.pool == reset.returned);
}

TEST_CASE("what they have reaches the engine as a delta, once, and comes back off", "[companion]")
{
    const fp::Rules r = Plain();
    Companion c = Lydia();
    c.learning.skills[fp::Index(Skill::OneHanded)] = 3;
    fp::AssignAttribute(c, fp::Attribute::Stamina, +1, 10);
    const auto pending = fp::Pending(c, Base(20), 100);
    CHECK(pending.skills[fp::Index(Skill::OneHanded)] == 3);
    CHECK(pending.attributes[fp::Index(fp::Attribute::Stamina)] == 10);
    fp::MarkApplied(c, pending);
    CHECK(fp::Pending(c, Base(20), 100).Empty());

    fp::AssignSkill(c, Skill::OneHanded, -1, 20, r);
    CHECK(fp::Pending(c, Base(20), 100).skills[fp::Index(Skill::OneHanded)] == -1);
    fp::MarkApplied(c, fp::Pending(c, Base(20), 100));
    CHECK(fp::Withdrawal(c).skills[fp::Index(Skill::OneHanded)] == -2);

    // The engine raised their own One-Handed to 99 since: only one of the two
    // learned levels fits under 100, and the other waits.
    fp::PerSkill<int> raised = Base(20);
    raised[fp::Index(Skill::OneHanded)] = 99;
    CHECK(fp::Pending(c, raised, 100).skills[fp::Index(Skill::OneHanded)] == -1);
}

TEST_CASE("a perk held counts against the level's points, and unlearning gives it back", "[companion]")
{
    Companion c = Lydia();
    fp::PerkGraph graph;
    fp::PerkNode armsman;
    armsman.name = "Armsman";
    armsman.skill = Skill::OneHanded;
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

TEST_CASE("one of their own perks can be set aside and taken up again, for nothing", "[companion]")
{
    Companion c = Lydia();
    fp::PerkGraph graph;
    fp::PerkNode recovery;
    recovery.name = "Recovery";
    recovery.skill = Skill::Restoration;
    recovery.ranks = {{{"Skyrim.esm", 0x0581F4}, "", {}}, {{"Skyrim.esm", 0x0581F5}, "", {}}};
    graph.Add(recovery);
    // As Marcurio's record has it: the second rank without the first.
    const std::unordered_set<FormKey, fp::FormKeyHash> onRecord{{"Skyrim.esm", 0x0581F5}};
    CHECK(fp::HeldRanks(graph, fp::HoldingsOf(c, onRecord)) == 1);

    fp::SetAside(c, recovery, onRecord);
    CHECK(c.setAside == std::vector<FormKey>{{"Skyrim.esm", 0x0581F5}});
    CHECK(fp::IsSetAside(c, recovery));
    CHECK(fp::HeldRanks(graph, fp::HoldingsOf(c, onRecord)) == 0); // its point is free again

    fp::SetAside(c, recovery, onRecord); // twice is once
    CHECK(c.setAside.size() == 1);

    CHECK(fp::Restore(c, recovery));
    CHECK_FALSE(fp::IsSetAside(c, recovery));
    CHECK(fp::HoldingsOf(c, onRecord).innate.contains({"Skyrim.esm", 0x0581F5}));
    CHECK_FALSE(fp::Restore(c, recovery));
}

TEST_CASE("spells taught here can be forgotten; others are not ours", "[companion]")
{
    Companion c = Lydia();
    const fp::SpellFacts flames{{"Skyrim.esm", 0x012FCD}, "Flames", Skill::Destruction, 0, 14, true};
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
    const fp::SpellFacts sparks{{"Skyrim.esm", 0x02B96B}, "Sparks", Skill::Destruction, 0, 13, true};
    const fp::SpellFacts flames{{"Skyrim.esm", 0x012FCD}, "Flames", Skill::Destruction, 0, 14, true};

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

TEST_CASE("the test button's experience adds up, and nothing is nothing", "[companion]")
{
    Companion c = Lydia();
    for (int i = 0; i < 150; ++i)
        fp::Gift(c, 10.0);
    fp::Gift(c, -5.0);
    CHECK(c.learning.xp == 1500.0);
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
    CHECK_FALSE(b.canReset);
    CHECK_FALSE(b.canResetPerks);
    CHECK(b.resetPerks == "No perks to reset");

    // Above it, with the pool: every one can act.
    c.learning.pool = 1500.0;
    b = fp::ButtonsFor(c, Skill::OneHanded, Base(20), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK(b.canLower);
    CHECK(b.lower == "Click to reduce skill");
    CHECK(b.lowest == "Click to reduce to minimum skill");
    CHECK(b.canRaise);
    CHECK(b.raise == "Click to increase skill");
    CHECK(b.highest == "Click to increase to maximum skill");
    CHECK(b.canReset);
    CHECK(b.reset == "Reset skill and perks");

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
    CHECK(b.canResetPerks);
    CHECK(b.resetPerks == "Click to reset perks");

    // Every skill alike: Smithing moves as One-Handed does.
    b = fp::ButtonsFor(c, Skill::Smithing, Base(25), 15, graph, fp::HoldingsOf(c, {}), r);
    CHECK(b.canLower);
    CHECK(b.canRaise);
}

TEST_CASE("resetting a tree's perks returns them and leaves the skill", "[companion]")
{
    const fp::Rules r = Plain();
    Companion c = Lydia();
    const fp::PerkGraph graph = BladeTree();
    c.learning.skills[fp::Index(Skill::OneHanded)] = 12;
    fp::Learn(c, graph.Node(0), 0);
    REQUIRE(fp::BoughtInTree(c, Skill::OneHanded, graph));
    const auto unlearned = fp::ResetPerks(c, Skill::OneHanded, graph);
    CHECK(unlearned == std::vector<std::string>{"Blade"});
    CHECK(c.perks.empty());
    CHECK_FALSE(fp::BoughtInTree(c, Skill::OneHanded, graph));
    CHECK(c.learning.skills[fp::Index(Skill::OneHanded)] == 12);
}
