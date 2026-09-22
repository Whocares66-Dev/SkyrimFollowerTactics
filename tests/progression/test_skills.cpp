#include "progression/core/Serialize.h"
#include "progression/core/Skills.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using fp::Attribute;
using fp::Skill;

TEST_CASE("a skill's actor value is the engine's, and reads back as the skill", "[skills]")
{
    // The skill page names a tree by its actor value (Tactics' SheetRow::tree
    // less one); Progression's controls take it back to a skill.
    CHECK(fp::ActorValueOf(Skill::OneHanded) == 6);
    CHECK(fp::ActorValueOf(Skill::Enchanting) == 23);
    for (const Skill s : fp::AllSkills())
        CHECK(fp::SkillFromActorValue(fp::ActorValueOf(s)) == s);

    // Anything else is no skill: Confidence below them, Health above.
    CHECK_FALSE(fp::SkillFromActorValue(5).has_value());
    CHECK_FALSE(fp::SkillFromActorValue(24).has_value());
    CHECK_FALSE(fp::SkillFromActorValue(-1).has_value());

    CHECK(fp::ActorValueOf(Attribute::Health) == 24);
    CHECK(fp::ActorValueOf(Attribute::Magicka) == 25);
    CHECK(fp::ActorValueOf(Attribute::Stamina) == 26);
}

TEST_CASE("every skill and attribute has a name to show and a key to save", "[skills]")
{
    std::set<std::string> names;
    std::set<std::string> keys;
    for (const Skill s : fp::AllSkills())
    {
        CHECK_FALSE(fp::Name(s).empty());
        names.emplace(fp::Name(s));
        keys.emplace(fp::Key(s));
        CHECK(fp::SkillFromKey(fp::Key(s)) == s);
    }
    CHECK(names.size() == fp::kSkillCount);
    CHECK(keys.size() == fp::kSkillCount);
    CHECK(std::string(fp::Name(Skill::OneHanded)) == "One-Handed");
    CHECK(std::string(fp::Key(Skill::OneHanded)) == "OneHanded");
    CHECK(std::string(fp::Name(Skill::HeavyArmor)) == "Heavy Armor");

    CHECK(std::string(fp::Name(Attribute::Magicka)) == "Magicka");
    for (const Attribute a : {Attribute::Health, Attribute::Magicka, Attribute::Stamina})
        CHECK(fp::AttributeFromKey(fp::Key(a)) == a);

    // A key no build wrote is nothing, not a guess.
    CHECK_FALSE(fp::SkillFromKey("Swimming").has_value());
    CHECK_FALSE(fp::SkillFromKey("").has_value());
    CHECK_FALSE(fp::AttributeFromKey("Luck").has_value());
}

TEST_CASE("Progression's co-save records are told apart from Tactics' by type", "[skills]")
{
    CHECK(fp::IsProgressionRecord(fp::kCompanionRecord));
    CHECK(fp::IsProgressionRecord(fp::kSettingsRecord));
    // Tactics' own settings record, whose tag Progression's gave up.
    CHECK_FALSE(fp::IsProgressionRecord(fp::RecordTag('S', 'E', 'T', 'T')));
    CHECK_FALSE(fp::IsProgressionRecord(0));
}
