// Custom Skills Framework's files and the order a perk tree is listed in
// (core/CustomSkills.h).
#include "core/CustomSkills.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ft;

namespace
{

// Stormcrown's Dragonborn tree as it ships (SKSE/Plugins/CustomSkills/Dragonborn.json),
// its skydome and experience settings left out.
constexpr const char *kDragonborn = R"json({
  "version": 1,
  "perkPoints": "Stormcrown.esp|802",
  "skills": [
    {
      "id": "Dragonborn",
      "name": "Dragonborn",
      "description": "New perks become available after consuming dragon souls.",
      "level": null,
      "ratio": "Stormcrown.esp|801",
      "nodes": [
        { "id": "Dragonborn", "perk": "Stormcrown.esp|80F", "x": -0.03, "y": -0.4,
          "links": [ "DragonOfTheNorth", "DeepBreath", "SkyAbove" ] },
        { "id": "DragonOfTheNorth", "perk": "Stormcrown.esp|825", "x": -0.03, "y": 0.2 },
        { "id": "DeepBreath", "perk": "Stormcrown.esp|81D", "x": -1.68, "y": 0, "links": [ "AncientVoice" ] },
        { "id": "AncientVoice", "perk": "Stormcrown.esp|F1C", "x": -2.62, "y": 0.8, "links": [ "SharpTongue" ] },
        { "id": "SharpTongue", "perk": "Stormcrown.esp|F19", "x": -2.62, "y": 2.2, "links": [ "Stormcrown" ] },
        { "id": "Stormcrown", "perk": "Stormcrown.esp|81E", "x": -1.2, "y": 3 },
        { "id": "SkyAbove", "perk": "Stormcrown.esp|824", "x": 1.68, "y": 0, "links": [ "VoiceWithin" ] },
        { "id": "VoiceWithin", "perk": "Stormcrown.esp|830", "x": 2.62, "y": 0.8, "links": [ "Windcaller" ] },
        { "id": "Windcaller", "perk": "Stormcrown.esp|82D", "x": 2.65, "y": 2.2, "links": [ "WayOfTheVoice" ] },
        { "id": "WayOfTheVoice", "perk": "Stormcrown.esp|C3F", "x": 1.1, "y": 3 }
      ]
    }
  ]
})json";

std::vector<std::string> OrderedIds(const CustomSkill &skill)
{
    std::vector<std::string> ids;
    for (const std::size_t i : TreeOrder(PlacesOf(skill)))
        ids.push_back(skill.nodes[i].id);
    return ids;
}

} // namespace

TEST_CASE("a form reference is the plugin and the id within it, in hex", "[customskills]")
{
    const auto ref = ParseFormRef("Stormcrown.esp|82D");
    REQUIRE(ref);
    REQUIRE(ref->plugin == "Stormcrown.esp");
    REQUIRE(ref->id == 0x82D);

    REQUIRE(ParseFormRef("DragonCultPriesthood.esp|0661F7")->id == 0x661F7);
    REQUIRE(ParseFormRef("Camping Plus Plus.esp|0x805")->id == 0x805);

    REQUIRE_FALSE(ParseFormRef("Stormcrown.esp"));
    REQUIRE_FALSE(ParseFormRef("Stormcrown.esp|"));
    REQUIRE_FALSE(ParseFormRef("|82D"));
    REQUIRE_FALSE(ParseFormRef("Stormcrown.esp|82G"));
}

TEST_CASE("a skill file reads into its skills and their nodes", "[customskills]")
{
    std::string error;
    const auto skills = ParseCustomSkills(kDragonborn, &error);
    REQUIRE(error.empty());
    REQUIRE(skills.size() == 1);
    const CustomSkill &skill = skills.front();
    REQUIRE(skill.name == "Dragonborn");
    REQUIRE_FALSE(skill.level); // "level": null, a tree without levels
    REQUIRE(skill.nodes.size() == 10);
    REQUIRE(skill.nodes.front().perk->id == 0x80F);
    REQUIRE(skill.nodes.front().links.size() == 3);
    REQUIRE(skill.nodes[8].id == "Windcaller");
    REQUIRE(skill.nodes[8].x == 2.65);
}

TEST_CASE("text that is not the framework's JSON reads as no skills, with a reason", "[customskills]")
{
    std::string error;
    REQUIRE(ParseCustomSkills("not json at all", &error).empty());
    REQUIRE(error == "not JSON");
    REQUIRE(ParseCustomSkills(R"({"version": 1})", &error).empty());
    REQUIRE(error == "no skills array");
}

TEST_CASE("a tree lists by depth, then left to right as the menu draws it", "[customskills]")
{
    // The menu draws larger x further left: Sky Above (1.68) left of Deep
    // Breath (-1.68), Way of the Voice (1.1) left of Stormcrown (-1.2).
    const auto skills = ParseCustomSkills(kDragonborn);
    REQUIRE(OrderedIds(skills.front()) == std::vector<std::string>{
                                              "Dragonborn",
                                              "SkyAbove",
                                              "DragonOfTheNorth",
                                              "DeepBreath",
                                              "VoiceWithin",
                                              "AncientVoice",
                                              "Windcaller",
                                              "SharpTongue",
                                              "WayOfTheVoice",
                                              "Stormcrown",
                                          });
}

TEST_CASE("a perk with two parents comes after the deeper one", "[customskills]")
{
    // Root -> A -> B -> Late, and Root -> Late: Late is two links from the
    // root one way and three the other, and waits for B.
    std::vector<TreeNodePlace> nodes(4);
    nodes[0].children = {1, 3}; // Root
    nodes[1].children = {2};    // A
    nodes[2].children = {3};    // B
    nodes[3].x = 5.0;           // Late, drawn far left, which does not bring it forward
    REQUIRE(TreeOrder(nodes) == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("a tree with a loop still lists every node once", "[customskills]")
{
    // Root -> A, and A <-> B: no tree should, and a bad file must not hang.
    std::vector<TreeNodePlace> nodes(3);
    nodes[0].children = {1};
    nodes[1].children = {2};
    nodes[2].children = {1};
    const auto order = TreeOrder(nodes);
    REQUIRE(order.size() == 3);
    REQUIRE(order.front() == 0);
}
