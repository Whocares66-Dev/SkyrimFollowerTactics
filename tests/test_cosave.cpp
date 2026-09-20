// The co-save's framing and reading policy. No Skyrim: the game side
// hands the records' bytes in as SKSE gave them.

#include <catch2/catch_test_macros.hpp>

#include "core/CoSave.h"
#include "core/Profile.h"

#include <optional>
#include <string>
#include <vector>

using namespace ft;

namespace
{

// The four little-endian bytes of a length, as the machine writes them.
std::string Length(std::uint32_t n)
{
    return {reinterpret_cast<const char *>(&n), sizeof n};
}

// As a string Catch can print: its string_view printer is not linked on
// every build of the library.
std::optional<std::string> Take(std::string_view &in)
{
    const auto s = TakeString(in);
    return s ? std::optional<std::string>(std::string(*s)) : std::nullopt;
}

std::string Text(const CoSaveContents &contents, std::string_view key)
{
    const std::string *text = contents.Follower(key);
    return text ? *text : "<none>";
}

CoSaveRecord Follower(std::string_view key, std::string_view text, std::uint32_t version = kProfileSchema)
{
    return {kFollowerRecord, version, PackFollower(key, text)};
}

} // namespace

TEST_CASE("a string is its length and its bytes, and comes back as it went", "[cosave]")
{
    std::string out;
    PutString(out, "Lydia");
    PutString(out, "");
    PutString(out, R"({"rules":[]})");
    REQUIRE(out.size() == 3 * 4 + 5 + 0 + 12);

    std::string_view in = out;
    REQUIRE(Take(in) == "Lydia");
    REQUIRE(Take(in) == "");
    REQUIRE(Take(in) == R"({"rules":[]})");
    REQUIRE(in.empty());
    // Nothing left: not even a length.
    REQUIRE_FALSE(Take(in));
}

TEST_CASE("a string cut short is refused, and the bytes are left where they were", "[cosave]")
{
    // Three bytes where a length needs four.
    std::string_view three = "abc";
    REQUIRE_FALSE(Take(three));
    REQUIRE(std::string(three) == "abc");

    // A length that runs past the end: ten bytes claimed, four present.
    // The length is not believed, and nothing is read past the record.
    const std::string claims = Length(10) + "abcd";
    std::string_view in = claims;
    REQUIRE_FALSE(Take(in));
    REQUIRE(in.size() == claims.size());

    // The largest length there is, on a record of a few bytes: refused the
    // same way, rather than sized to it.
    const std::string huge = Length(0xFFFFFFFFu) + "abcd";
    in = huge;
    REQUIRE_FALSE(Take(in));

    // Exactly the length present is fine.
    const std::string exact = Length(4) + "abcd";
    in = exact;
    REQUIRE(Take(in) == "abcd");
    REQUIRE(in.empty());
}

TEST_CASE("the loaded records: followers by key, the settings, and what was skipped", "[cosave]")
{
    std::vector<CoSaveRecord> records;
    records.push_back({kSettingsRecord, kProfileSchema, PackSettings(R"({"tacticsEnabled":false})")});
    records.push_back(Follower("Skyrim.esm-A2C94", "lydia"));
    records.push_back(Follower("Skyrim.esm-1348A", "jenassa"));

    const CoSaveContents contents = UnpackCoSave(records);
    REQUIRE(contents.settings == R"({"tacticsEnabled":false})");
    REQUIRE(contents.followers.size() == 2);
    REQUIRE(Text(contents, "Skyrim.esm-A2C94") == "lydia");
    REQUIRE(Text(contents, "Skyrim.esm-1348A") == "jenassa");
    REQUIRE(contents.notes.empty());

    // No records: nothing, and no settings to apply.
    const CoSaveContents none = UnpackCoSave({});
    REQUIRE_FALSE(none.settings);
    REQUIRE(none.followers.empty());
}

TEST_CASE("a record this build does not know is skipped and said", "[cosave]")
{
    std::vector<CoSaveRecord> records;
    records.push_back({RecordTag('X', 'Y', 'Z', 'W'), 1, "whatever"});
    records.push_back(Follower("Skyrim.esm-A2C94", "lydia"));
    const CoSaveContents contents = UnpackCoSave(records);
    REQUIRE(contents.followers.size() == 1);
    REQUIRE(contents.notes.size() == 1);
    REQUIRE(contents.notes[0].first == log::Level::Warn);
    REQUIRE(contents.notes[0].second.find("58595A57") != std::string::npos);
}

TEST_CASE("a record cut short is skipped without taking the rest down", "[cosave]")
{
    std::vector<CoSaveRecord> records;
    // A key and no text.
    std::string keyOnly;
    PutString(keyOnly, "Skyrim.esm-A2C94");
    records.push_back({kFollowerRecord, kProfileSchema, keyOnly});
    // A settings record with a length and nothing after it.
    records.push_back({kSettingsRecord, kProfileSchema, Length(3)});
    // An empty follower record.
    records.push_back({kFollowerRecord, kProfileSchema, ""});
    records.push_back(Follower("Skyrim.esm-1348A", "jenassa"));

    const CoSaveContents contents = UnpackCoSave(records);
    REQUIRE(contents.followers.size() == 1);
    REQUIRE(Text(contents, "Skyrim.esm-1348A") == "jenassa");
    REQUIRE_FALSE(contents.settings);
    REQUIRE(contents.notes.size() == 3);
    for (const auto &[level, text] : contents.notes)
        REQUIRE(level == log::Level::Error);
}

TEST_CASE("a record from a newer build is read, with a warning naming it", "[cosave]")
{
    std::vector<CoSaveRecord> records;
    records.push_back(Follower("Skyrim.esm-A2C94", "lydia", kProfileSchema + 1));
    const CoSaveContents contents = UnpackCoSave(records);
    REQUIRE(Text(contents, "Skyrim.esm-A2C94") == "lydia");
    REQUIRE(contents.notes.size() == 1);
    REQUIRE(contents.notes[0].first == log::Level::Warn);
    REQUIRE(contents.notes[0].second.starts_with("Skyrim.esm-A2C94:"));
}

TEST_CASE("two records under one key: the later wins, as the later save's write would", "[cosave]")
{
    std::vector<CoSaveRecord> records;
    records.push_back(Follower("Skyrim.esm-A2C94", "first"));
    records.push_back(Follower("Skyrim.esm-A2C94", "second"));
    records.push_back({kSettingsRecord, kProfileSchema, PackSettings("one")});
    records.push_back({kSettingsRecord, kProfileSchema, PackSettings("two")});
    const CoSaveContents contents = UnpackCoSave(records);
    REQUIRE(contents.followers.size() == 1);
    REQUIRE(Text(contents, "Skyrim.esm-A2C94") == "second");
    REQUIRE(contents.settings == "two");
}

TEST_CASE("a record's tag reads as the four characters, first most significant", "[cosave]")
{
    REQUIRE(RecordTag('P', 'R', 'O', 'F') == 0x50524F46u);
    REQUIRE(kFollowerRecord == 0x50524F46u);
    REQUIRE(kSettingsRecord == 0x53455454u);
}

TEST_CASE("a follower's key: the base record's plugin and id, else the reference's, else its runtime id", "[cosave]")
{
    REQUIRE(ChooseKeyRecord(true, true) == KeyedBy::Base);
    REQUIRE(ChooseKeyRecord(true, false) == KeyedBy::Base);
    REQUIRE(ChooseKeyRecord(false, true) == KeyedBy::Reference);
    REQUIRE(ChooseKeyRecord(false, false) == KeyedBy::Dynamic);
    // The wire form, which every save's tactics are filed under.
    REQUIRE(FollowerKey("Skyrim.esm", 0xA2C94) == "Skyrim.esm-A2C94");
    REQUIRE(FollowerKey("Dawnguard.esm", 0x2B74) == "Dawnguard.esm-2B74");
    REQUIRE(FollowerKey("Some Mod.esp", 0) == "Some Mod.esp-0");
    REQUIRE(DynamicKey(0xFF000DE0) == "dynamic-FF000DE0");
    REQUIRE(DynamicKey(0x14) == "dynamic-00000014");
}

TEST_CASE("a follower claims their record once; the rest are carried into the next save", "[cosave]")
{
    std::vector<CoSaveRecord> records;
    records.push_back(Follower("Skyrim.esm-A2C94", "lydia"));
    records.push_back(Follower("Skyrim.esm-1348A", "jenassa"));
    records.push_back(Follower("Dawnguard.esm-2B6C", "serana"));

    SavedProfiles saved;
    saved.Load(UnpackCoSave(records).followers);
    REQUIRE(saved.Carried() == 3);

    // Lydia is seen by the tick and claims that record; a second claim
    // finds nothing, as a second reference of one base does.
    REQUIRE(saved.Claim("Skyrim.esm-A2C94") == "lydia");
    REQUIRE_FALSE(saved.Claim("Skyrim.esm-A2C94"));
    REQUIRE_FALSE(saved.Claim("Skyrim.esm-NOBODY"));
    REQUIRE(saved.Carried() == 2);

    // The save: what the live followers have now, then what is still
    // carried -- the two who are away keep their tactics.
    const std::vector<SavedProfiles::Record> live{{"Skyrim.esm-A2C94", "lydia, edited"}};
    const auto records2 = saved.ToWrite(live);
    REQUIRE(records2.size() == 3);
    REQUIRE(records2[0].key == "Skyrim.esm-A2C94");
    REQUIRE(records2[0].text == "lydia, edited");
    REQUIRE(records2[1].key == "Skyrim.esm-1348A");
    REQUIRE(records2[2].text == "serana");

    // A load, or a new game, forgets the lot.
    saved.Forget();
    REQUIRE(saved.Carried() == 0);
    REQUIRE_FALSE(saved.Claim("Skyrim.esm-1348A"));
    REQUIRE(saved.ToWrite(live).size() == 1);
}

TEST_CASE("a save that holds nothing of ours writes only the live followers", "[cosave]")
{
    SavedProfiles saved;
    saved.Load(UnpackCoSave({}).followers);
    REQUIRE(saved.Carried() == 0);
    const std::vector<SavedProfiles::Record> live{{"Skyrim.esm-A2C94", "lydia"}};
    REQUIRE(saved.ToWrite(live).size() == 1);
    // And a record that could not be read is not carried either: the
    // reading dropped it before it got here.
    std::vector<CoSaveRecord> bad;
    bad.push_back({kFollowerRecord, kProfileSchema, ""});
    saved.Load(UnpackCoSave(bad).followers);
    REQUIRE(saved.Carried() == 0);
}
