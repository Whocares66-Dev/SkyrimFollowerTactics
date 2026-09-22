#include "progression/core/Spells.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
fp::SpellFacts Fireball()
{
    return {{"Skyrim.esm", 0x1C789}, "Fireball", fp::Skill::Destruction, 50, 133, true};
}
} // namespace

TEST_CASE("a spell's level has the familiar names", "[spells]")
{
    CHECK(std::string(fp::LevelName(0)) == "Novice");
    CHECK(std::string(fp::LevelName(25)) == "Apprentice");
    CHECK(std::string(fp::LevelName(50)) == "Adept");
    CHECK(std::string(fp::LevelName(75)) == "Expert");
    CHECK(std::string(fp::LevelName(100)) == "Master");
}

TEST_CASE("teaching needs the school's skill at the spell's level", "[spells]")
{
    fp::PerSkill<int> skills{};
    skills[fp::Index(fp::Skill::Destruction)] = 34;
    const auto status = fp::CanTeach(Fireball(), false, skills, 300);
    CHECK(status.block == fp::TeachBlock::Skill);
    CHECK(status.need == 50);
    CHECK(status.have == 34);
    skills[fp::Index(fp::Skill::Destruction)] = 50;
    CHECK(fp::CanTeach(Fireball(), false, skills, 300).block == fp::TeachBlock::None);
}

TEST_CASE("a spell they could never cast is not taught", "[spells]")
{
    fp::PerSkill<int> skills{};
    skills[fp::Index(fp::Skill::Destruction)] = 60;
    const auto status = fp::CanTeach(Fireball(), false, skills, 80);
    CHECK(status.block == fp::TeachBlock::Magicka);
    CHECK(status.need == 133);
    CHECK(status.have == 80);
}

TEST_CASE("known spells, powers and abilities are not taught", "[spells]")
{
    fp::PerSkill<int> skills{};
    skills.fill(100);
    CHECK(fp::CanTeach(Fireball(), true, skills, 500).block == fp::TeachBlock::Known);
    auto power = Fireball();
    power.ordinary = false;
    CHECK(fp::CanTeach(power, false, skills, 500).block == fp::TeachBlock::NotTeachable);
    auto odd = Fireball();
    odd.school = fp::Skill::OneHanded;
    CHECK(fp::CanTeach(odd, false, skills, 500).block == fp::TeachBlock::NotTeachable);
    odd.school.reset();
    CHECK(fp::CanTeach(odd, false, skills, 500).block == fp::TeachBlock::NotTeachable);
}
