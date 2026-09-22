// The 18 vanilla trees, as dev/research/extract_perk_trees.py read them from
// Skyrim.esm, through the same code the plugin runs on the trees it reads
// from the engine.

#include "PerkData.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

using fp::FormKey;
using fp::PerkBlock;
using fp::Skill;

namespace
{

const fp::PerkGraph &Vanilla()
{
    static const fp::PerkGraph graph = [] {
        std::ifstream in(std::string(FP_TEST_DATA) + "/vanilla-perks.json");
        std::stringstream text;
        text << in.rdbuf();
        std::string why;
        auto g = fp::ReadPerkGraph(text.str(), &why);
        if (!g)
            throw std::runtime_error("vanilla-perks.json: " + why);
        return std::move(*g);
    }();
    return graph;
}

int NodeOf(std::uint32_t local)
{
    const auto found = Vanilla().Find({"Skyrim.esm", local});
    if (!found)
        throw std::runtime_error("no such vanilla perk");
    return found->first;
}

fp::PerSkill<int> Skills(Skill skill, int level)
{
    fp::PerSkill<int> s{};
    s.fill(15);
    s[fp::Index(skill)] = level;
    return s;
}

constexpr std::uint32_t kArmsman = 0x0BABE4;
constexpr std::uint32_t kEagleEye = 0x058F61;
constexpr std::uint32_t kPowerShot = 0x058F62;
constexpr std::uint32_t kOverdraw = 0x0BABED;
constexpr std::uint32_t kMagicResistance = 0x053128;
constexpr std::uint32_t kApprenticeAlteration = 0x0C44B7;
constexpr std::uint32_t kNoviceAlteration = 0x0F2CA6;
constexpr std::uint32_t kParalyzingStrike = 0x03AFA6;
constexpr std::uint32_t kSavageStrike = 0x03AF81;
constexpr std::uint32_t kCriticalCharge = 0x0CB406;
constexpr std::uint32_t kFightingStance = 0x052D50;

} // namespace

TEST_CASE("all 180 vanilla nodes and 251 ranks read", "[vanilla]")
{
    CHECK(Vanilla().Size() == 180);
    std::size_t ranks = 0;
    for (const auto &node : Vanilla().Nodes())
        ranks += node.ranks.size();
    CHECK(ranks == 251);
}

TEST_CASE("Armsman's five ranks need 0, 20, 40, 60 and 80", "[vanilla]")
{
    const auto &armsman = Vanilla().Node(NodeOf(kArmsman));
    REQUIRE(armsman.ranks.size() == 5);
    const int expected[] = {0, 20, 40, 60, 80};
    for (std::size_t r = 0; r < 5; ++r)
        CHECK(fp::SkillRequirement(armsman.ranks[r], Skill::OneHanded) == expected[r]);
}

TEST_CASE("a companion at One-Handed 30 can take Armsman to rank 2 and no further", "[vanilla]")
{
    fp::Holdings h;
    const auto skills = Skills(Skill::OneHanded, 30);
    const int armsman = NodeOf(kArmsman);
    CHECK(fp::Status({Vanilla(), h, skills, 5}, armsman).block == PerkBlock::None);
    h.learned.insert({"Skyrim.esm", kArmsman});
    CHECK(fp::Status({Vanilla(), h, skills, 5}, armsman).block == PerkBlock::None);
    h.learned.insert(Vanilla().Node(armsman).ranks[1].form);
    const auto third = fp::Status({Vanilla(), h, skills, 5}, armsman);
    CHECK(third.held == 2);
    CHECK(third.block == PerkBlock::Skill);
}

TEST_CASE("Magic Resistance needs Apprentice Alteration, whatever the menu draws", "[vanilla]")
{
    const int node = NodeOf(kMagicResistance);
    CHECK(Vanilla().Parents(node) == std::vector<int>{NodeOf(kApprenticeAlteration)});

    fp::Holdings h;
    h.learned.insert({"Skyrim.esm", kNoviceAlteration});
    const auto skills = Skills(Skill::Alteration, 40);
    CHECK(fp::Status({Vanilla(), h, skills, 5}, node).block == PerkBlock::Requires);
    h.learned.insert({"Skyrim.esm", kApprenticeAlteration});
    CHECK(fp::Status({Vanilla(), h, skills, 5}, node).block == PerkBlock::None);
}

TEST_CASE("Paralyzing Strike takes either Savage Strike or Critical Charge", "[vanilla]")
{
    const int node = NodeOf(kParalyzingStrike);
    const auto skills = Skills(Skill::OneHanded, 100);
    fp::Holdings h;
    h.learned = {{"Skyrim.esm", kArmsman}, {"Skyrim.esm", kFightingStance}};
    CHECK(fp::Status({Vanilla(), h, skills, 5}, node).block == PerkBlock::Requires);
    auto viaSavage = h;
    viaSavage.learned.insert({"Skyrim.esm", kSavageStrike});
    CHECK(fp::Status({Vanilla(), viaSavage, skills, 5}, node).block == PerkBlock::None);
    auto viaCharge = h;
    viaCharge.learned.insert({"Skyrim.esm", kCriticalCharge});
    CHECK(fp::Status({Vanilla(), viaCharge, skills, 5}, node).block == PerkBlock::None);
}

TEST_CASE("Eagle Eye is bought to reach Power Shot, as the player's is", "[vanilla]")
{
    fp::Holdings h;
    h.learned.insert({"Skyrim.esm", kOverdraw});
    const int shot = NodeOf(kPowerShot);

    const auto enough = Skills(Skill::Archery, 50);
    CHECK(fp::Status({Vanilla(), h, enough, 5}, NodeOf(kEagleEye)).block == PerkBlock::None);
    CHECK(fp::Status({Vanilla(), h, enough, 5}, shot).block == PerkBlock::Requires);
    h.learned.insert({"Skyrim.esm", kEagleEye});
    CHECK(fp::Status({Vanilla(), h, enough, 5}, shot).block == PerkBlock::None);
    CHECK(fp::Status({Vanilla(), h, Skills(Skill::Archery, 29), 5}, shot).block == PerkBlock::Skill);
}

TEST_CASE("Marcurio's own perks read as his record has them", "[vanilla]")
{
    // HirelingMarcurio's perks, as Skyrim.esm has them: Magic
    // Resistance without Apprentice Alteration, and Recovery's second rank
    // without its first.
    fp::Holdings h;
    for (const std::uint32_t local : {0x053128u, 0x0D7999u, 0x058200u, 0x0581F8u, 0x0581F9u, 0x0581F5u})
        h.innate.insert({"Skyrim.esm", local});
    auto skills = Skills(fp::Skill::Restoration, 30);
    skills[fp::Index(fp::Skill::Alteration)] = 28;
    skills[fp::Index(fp::Skill::Destruction)] = 45;

    const auto recovery = fp::Status({Vanilla(), h, skills, 3}, NodeOf(0x0581F4));
    CHECK(recovery.held == 2);
    CHECK(recovery.innate == 2);
    CHECK(recovery.block == PerkBlock::Maxed);
    // Theirs to give back: nothing else they hold needs it.
    CHECK(recovery.canUnlearn);

    const auto resistance = fp::Status({Vanilla(), h, skills, 3}, NodeOf(kMagicResistance));
    CHECK(resistance.held == 1);
    CHECK(resistance.innate == 1);
    // The next rank needs Alteration 50 and the first rank, which is held.
    CHECK(resistance.block == PerkBlock::Skill);
}
