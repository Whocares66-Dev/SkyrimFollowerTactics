// Custom Skills Framework's files and the order a perk tree is listed in
// (core/CustomSkills.h).
#include "core/CustomSkills.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
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

// A perk's ranks: a linked list of records the engine owns, walked here over
// a handle that is only ever compared.
namespace
{

struct Rank
{
    int id{0};
    Rank *next{nullptr};
};

std::vector<int> IdsOf(const std::vector<Rank *> &chain)
{
    std::vector<int> ids;
    for (const Rank *rank : chain)
        ids.push_back(rank->id);
    return ids;
}

Rank *NextOf(Rank *rank)
{
    return rank->next;
}

} // namespace

TEST_CASE("a rank chain is the perk and everything it chains to", "[customskills]")
{
    Rank third{3, nullptr};
    Rank second{2, &third};
    Rank first{1, &second};
    REQUIRE(IdsOf(RankChain(&first, NextOf)) == std::vector<int>{1, 2, 3});

    // The last rank of a chain, asked for on its own, is a chain of one --
    // which is what a single-rank perk is.
    REQUIRE(IdsOf(RankChain(&third, NextOf)) == std::vector<int>{3});
    REQUIRE(RankChain(static_cast<Rank *>(nullptr), NextOf).empty());
}

TEST_CASE("a rank chain longer than the bound stops at it", "[customskills]")
{
    // Twenty ranks, chained. Nothing in the game has this many; a record
    // that says so is not a rank chain, and the walk says so by stopping.
    std::vector<Rank> ranks(20);
    for (std::size_t i = 0; i < ranks.size(); ++i)
    {
        ranks[i].id = static_cast<int>(i) + 1;
        ranks[i].next = i + 1 < ranks.size() ? &ranks[i + 1] : nullptr;
    }
    const auto chain = RankChain(&ranks.front(), NextOf);
    REQUIRE(chain.size() == kMaxRanks);
    REQUIRE(chain.front()->id == 1);
    REQUIRE(chain.back()->id == static_cast<int>(kMaxRanks));
}

TEST_CASE("a rank chain that comes round gives each rank once", "[customskills]")
{
    // 1 -> 2 -> 3 -> 2. The bound alone would have listed sixteen, the same
    // three over and over, each reading as a rank of sixteen; the walk stops
    // at the rank it has already had.
    Rank third{3, nullptr};
    Rank second{2, &third};
    Rank first{1, &second};
    third.next = &second;
    REQUIRE(IdsOf(RankChain(&first, NextOf)) == std::vector<int>{1, 2, 3});

    // A perk that names itself is a chain of one, not of sixteen.
    Rank alone{9, nullptr};
    alone.next = &alone;
    REQUIRE(IdsOf(RankChain(&alone, NextOf)) == std::vector<int>{9});
}

// Loading the files: that one bad neighbour is not the end of the walk.
namespace
{

// One file's JSON, with just the skill ids that matter here.
std::string Skills(const std::vector<std::string> &ids)
{
    std::string json = R"({"skills":[)";
    for (std::size_t i = 0; i < ids.size(); ++i)
        json += (i ? "," : "") + std::string(R"({"id":")") + ids[i] + R"("})";
    return json + "]}";
}

} // namespace

TEST_CASE("a file that will not parse costs only itself", "[customskills]")
{
    namespace fs = std::filesystem;
    const std::vector<fs::path> files{"a.json", "bad.json", "c.json"};
    std::vector<std::pair<std::string, std::string>> complaints;
    const auto trees = LoadSkillTrees(
        files,
        [](const fs::path &file) {
            return file.filename() == "bad.json" ? std::string{"{ this is not JSON"} : Skills({"one"});
        },
        [](const CustomSkill &skill, const std::string &label) { return label + ":" + skill.id; },
        [&](const std::string &label, std::string_view skill, std::string_view why) {
            complaints.emplace_back(label + "/" + std::string(skill), std::string(why));
        });

    REQUIRE(trees == std::vector<std::string>{"a.json:one", "c.json:one"});
    REQUIRE(complaints.size() == 1);
    REQUIRE(complaints.front().first == "bad.json/");
    REQUIRE(complaints.front().second == "not JSON");
}

TEST_CASE("a skill that will not build costs only itself, not its file", "[customskills]")
{
    namespace fs = std::filesystem;
    const std::vector<fs::path> files{"one.json"};
    std::vector<std::pair<std::string, std::string>> complaints;
    const auto trees = LoadSkillTrees(
        files, [](const fs::path &) { return Skills({"good", "unresolved", "alsogood"}); },
        [](const CustomSkill &skill, const std::string &label) {
            if (skill.id == "unresolved")
                throw std::runtime_error("names no perk in the load order");
            return label + ":" + skill.id;
        },
        [&](const std::string &label, std::string_view skill, std::string_view why) {
            complaints.emplace_back(label + "/" + std::string(skill), std::string(why));
        });

    // The skill after the bad one still loads: the throw ends that skill.
    REQUIRE(trees == std::vector<std::string>{"one.json:good", "one.json:alsogood"});
    REQUIRE(complaints.size() == 1);
    REQUIRE(complaints.front().first == "one.json/unresolved");
    REQUIRE(complaints.front().second == "names no perk in the load order");
}

TEST_CASE("a file whose text will not even be read is left out quietly", "[customskills]")
{
    namespace fs = std::filesystem;
    // ReadText gives nothing for a file it cannot open, and nothing does not
    // parse: the file is reported and the next one still loads.
    const std::vector<fs::path> files{"gone.json", "here.json"};
    std::vector<std::string> complained;
    const auto trees = LoadSkillTrees(
        files, [](const fs::path &file) { return file.filename() == "gone.json" ? std::string{} : Skills({"one"}); },
        [](const CustomSkill &skill, const std::string &label) { return label + ":" + skill.id; },
        [&](const std::string &label, std::string_view, std::string_view) { complained.push_back(label); });

    REQUIRE(trees == std::vector<std::string>{"here.json:one"});
    REQUIRE(complained == std::vector<std::string>{"gone.json"});
}

TEST_CASE("a read that throws is caught, and the walk goes on", "[customskills]")
{
    namespace fs = std::filesystem;
    const std::vector<fs::path> files{"throws.json", "here.json"};
    std::vector<std::string> complained;
    const auto trees = LoadSkillTrees(
        files,
        [](const fs::path &file) -> std::string {
            if (file.filename() == "throws.json")
                throw std::runtime_error("out of memory reading it");
            return Skills({"one"});
        },
        [](const CustomSkill &skill, const std::string &label) { return label + ":" + skill.id; },
        [&](const std::string &label, std::string_view, std::string_view) { complained.push_back(label); });

    REQUIRE(trees == std::vector<std::string>{"here.json:one"});
    REQUIRE(complained == std::vector<std::string>{"throws.json"});
}
