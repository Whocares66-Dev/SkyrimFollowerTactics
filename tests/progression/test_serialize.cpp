#include "progression/core/Serialize.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

using fp::Companion;

namespace
{

Companion Seasoned()
{
    const fp::Rules r;
    Companion c = fp::Enroll({"Skyrim.esm", 0x0A2C8E}, "Lydia");
    const fp::SkillUsage usage{1.0, 0.0, 1.0, 0.0};
    fp::Practise(c, fp::Skill::Destruction, 5000.0, 20, usage, r);
    fp::Practise(c, fp::Skill::OneHanded, 37.5, 20, usage, r); // progress short of a level
    c.learning.xp += 900.0;
    fp::AssignSkill(c, fp::Skill::Destruction, -1, 20, r);
    fp::AssignAttribute(c, fp::Attribute::Magicka, +1, 10);
    fp::AssignAttribute(c, fp::Attribute::Magicka, +1, 10);
    c.level = 12;
    fp::PerSkill<int> base{};
    base.fill(20);
    fp::MarkApplied(c, fp::Pending(c, base, 100));
    c.perks.push_back({{"Skyrim.esm", 0x0BABE4}, "Armsman", 1});
    c.setAside.push_back({"Skyrim.esm", 0x053128});
    fp::Teach(c, {{"Skyrim.esm", 0x012FCD}, "Flames"});
    fp::SetAsideSpell(c, {{"Skyrim.esm", 0x02B96B}, "Sparks"});
    return c;
}

} // namespace

TEST_CASE("a companion reads back exactly as written", "[serialize]")
{
    const Companion c = Seasoned();
    const auto back = fp::ReadCompanion(fp::WriteCompanion(c));
    REQUIRE(back);
    CHECK(back->key == c.key);
    CHECK(back->name == c.name);
    CHECK(back->level == c.level);
    CHECK(back->learning == c.learning);
    CHECK(back->applied == c.applied);
    CHECK(back->perks == c.perks);
    CHECK(back->setAside == c.setAside);
    CHECK(back->spells == c.spells);
    CHECK(back->spellsSetAside == c.spellsSetAside);
}

TEST_CASE("fields a newer build adds are ignored; missing ones take defaults", "[serialize]")
{
    nlohmann::json j = nlohmann::json::parse(fp::WriteCompanion(Seasoned()));
    j["somethingNew"] = {1, 2, 3};
    j.erase("learning");
    j.erase("spells");
    const auto back = fp::ReadCompanion(j.dump());
    REQUIRE(back);
    CHECK(back->learning == fp::Learning{});
    CHECK(back->spells.empty());
}

TEST_CASE("a companion that cannot be read says why", "[serialize]")
{
    std::string why;
    CHECK_FALSE(fp::ReadCompanion("not json", &why));
    CHECK(why == "not JSON");
    CHECK_FALSE(fp::ReadCompanion(R"({"schema":1})", &why));
    CHECK(why == "no key");
    CHECK_FALSE(fp::ReadCompanion(R"({"schema":99,"key":"Skyrim.esm|0A2C8E"})", &why));
    CHECK(why.find("newer") != std::string::npos);
}

TEST_CASE("settings read back", "[serialize]")
{
    fp::Settings s;
    s.notifyLevels = false;
    s.notifySkills = true;
    s.released = true;
    CHECK(fp::ReadSettings(fp::WriteSettings(s)) == s);
    CHECK_FALSE(fp::ReadSettings("[]"));
}

TEST_CASE("the co-save holds the settings and every companion", "[serialize]")
{
    fp::Settings s;
    s.autoEnroll = false;
    const std::vector<Companion> party{Seasoned(), fp::Enroll({"Dawnguard.esm", 0x002B6C}, "Serana")};
    const auto records = fp::PackCoSave(party, s);
    REQUIRE(records.size() == 3);
    const auto contents = fp::UnpackCoSave(records);
    CHECK(contents.notes.empty());
    REQUIRE(contents.settings);
    CHECK_FALSE(contents.settings->autoEnroll);
    REQUIRE(contents.companions.size() == 2);
    CHECK(contents.companions[1].name == "Serana");
}

TEST_CASE("a bad record costs only itself", "[serialize]")
{
    auto records = fp::PackCoSave(std::vector<Companion>{Seasoned(), Seasoned()}, {});
    records[1].payload = "{broken";
    records.push_back({fp::RecordTag('X', 'X', 'X', 'X'), 1, "?"});
    records.push_back({fp::kCompanionRecord, fp::kSchema + 1, fp::WriteCompanion(Seasoned())});
    records.push_back({fp::kCompanionRecord, fp::kSchema, std::string(fp::kMaxRecordBytes + 1, ' ')});
    const auto contents = fp::UnpackCoSave(records);
    CHECK(contents.companions.size() == 1);
    CHECK(contents.notes.size() == 4);
}

TEST_CASE("the same companion twice keeps the first", "[serialize]")
{
    Companion a = Seasoned();
    Companion b = Seasoned();
    b.learning.xp += 1000;
    const auto contents = fp::UnpackCoSave(fp::PackCoSave(std::vector<Companion>{a, b}, {}));
    REQUIRE(contents.companions.size() == 1);
    CHECK(contents.companions[0].learning.xp == a.learning.xp);
    CHECK(contents.notes.size() == 1);
}
