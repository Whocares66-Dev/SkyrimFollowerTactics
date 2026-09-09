// The profile file: what a follower's tactics look like on disk, and how
// leniently they come back. No Skyrim here; forms pass through as hex.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/Profile.h"
#include "core/Vocabulary.h"

#include <string>

using namespace ft;

namespace
{

const FormCodec kHex = FormCodec::Hex();

Rule HealBelow(float pct)
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = pct;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::DrinkStrongest;
    r.FirstAction().effect = "Restore Health";
    r.label = "emergency heal";
    return r;
}

// One rule of every shape the file has to carry: a named follower on both
// sides, a status, a damage kind, an equip with a hand, a cast with an
// argument, two actions in one rule, and a rule switched off.
Profile Everything()
{
    Profile p;
    p.followerName = "Lydia";
    p.followerForm = "0xA2C94";
    p.enabled = false;
    p.rules.rules.push_back(HealBelow(0.5f));

    {
        Rule r;
        r.subject = SubjectKind::Follower;
        r.subjectForm = 0x1234;
        r.predicate = PredicateKind::Status;
        r.statusKind = StatusKind::Burning;
        r.actionTarget = ActionTargetKind::Follower;
        r.actionTargetForm = 0x1234;
        Action cast;
        cast.kind = ActionKind::CastSpell;
        cast.form = 0xFF00ABCD; // above the 24-bit mantissa, on purpose
        cast.arg = 2.5f;
        r.actions.push_back(cast);
        r.label = "douse";
        p.rules.rules.push_back(r);
    }
    {
        Rule r;
        r.enabled = false;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::ResistancePctBelow;
        r.conditionArg = 0.25f;
        r.damageKind = DamageKind::Frost;
        r.actionTarget = ActionTargetKind::Self;
        Action weapon;
        weapon.kind = ActionKind::EquipWeapon;
        weapon.form = 0x13989;
        weapon.hand = Hand::Both;
        r.actions.push_back(weapon);
        Action arrows;
        arrows.kind = ActionKind::EquipArrows;
        arrows.form = 0x1397D;
        r.actions.push_back(arrows);
        p.rules.rules.push_back(r);
    }
    {
        Rule r;
        r.subject = SubjectKind::Player;
        r.predicate = PredicateKind::AttackedBy;
        r.damageKind = DamageKind::Any;
        r.actionTarget = ActionTargetKind::Attacker;
        r.FirstAction().kind = ActionKind::Target;
        p.rules.rules.push_back(r);
    }
    {
        // The consume and power actions, each naming a form.
        Rule r;
        r.subject = SubjectKind::Self;
        r.predicate = PredicateKind::HealthPctBelow;
        r.conditionArg = 0.5f;
        r.actionTarget = ActionTargetKind::Self;
        Action food;
        food.kind = ActionKind::EatFood;
        food.form = 0x64B33;
        r.actions.push_back(food);
        Action ingredient;
        ingredient.kind = ActionKind::EatIngredient;
        ingredient.form = 0x727DE;
        r.actions.push_back(ingredient);
        Action power;
        power.kind = ActionKind::UsePower;
        power.form = 0x88821;
        r.actions.push_back(power);
        Action shout;
        shout.kind = ActionKind::Shout;
        shout.form = 0x13E07;
        r.actions.push_back(shout);
        p.rules.rules.push_back(r);
    }
    p.pins.push_back({0x13989, Hand::Both}); // a bow
    p.pins.push_back({0x12E49, Hand::None}); // a cuirass
    p.pins.push_back({0x12FCD, Hand::Left}); // a spell in one hand
    return p;
}

void RequireSame(const Action &a, const Action &b)
{
    REQUIRE(a.kind == b.kind);
    REQUIRE(a.form == b.form);
    REQUIRE(a.hand == b.hand);
    REQUIRE(a.arg == b.arg);
}

void RequireSame(const Rule &a, const Rule &b)
{
    REQUIRE(a.enabled == b.enabled);
    REQUIRE(a.label == b.label);
    REQUIRE(a.subject == b.subject);
    REQUIRE(a.subjectForm == b.subjectForm);
    REQUIRE(a.predicate == b.predicate);
    REQUIRE(a.conditionArg == b.conditionArg);
    if (a.predicate == PredicateKind::Status)
        REQUIRE(a.statusKind == b.statusKind);
    if (IsResistance(a.predicate) || a.predicate == PredicateKind::AttackedBy)
        REQUIRE(a.damageKind == b.damageKind);
    REQUIRE(a.actionTarget == b.actionTarget);
    REQUIRE(a.actionTargetForm == b.actionTargetForm);
    REQUIRE(a.actions.size() == b.actions.size());
    for (std::size_t i = 0; i < a.actions.size(); ++i)
        RequireSame(a.actions[i], b.actions[i]);
}

// A file with one rule, from the pieces a test wants to vary.
std::string OneRuleFile(const std::string &rule, const std::string &extraTop = "")
{
    return R"({ "schema": 1, "enabled": true, )" + extraTop + R"( "rules": [ )" + rule + " ] }";
}

const std::string kHealRule = R"({
    "label": "heal",
    "if": { "subject": "self", "predicate": "health-pct-below", "arg": 0.5 },
    "then": { "target": "self", "do": [ { "action": "drink-strongest-health-potion" } ] }
})";

} // namespace

TEST_CASE("a profile round-trips through its file", "[profile]")
{
    const Profile before = Everything();
    const auto read = ReadProfile(WriteProfile(before, kHex), kHex);

    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile.has_value());
    const Profile &after = *read.profile;
    REQUIRE(after.followerName == before.followerName);
    REQUIRE(after.followerForm == before.followerForm);
    REQUIRE(after.enabled == before.enabled);
    REQUIRE(after.rules.schemaVersion == kProfileSchema);
    REQUIRE(after.rules.rules.size() == before.rules.rules.size());
    for (std::size_t i = 0; i < before.rules.rules.size(); ++i)
        RequireSame(before.rules.rules[i], after.rules.rules[i]);
    REQUIRE(after.pins.size() == before.pins.size());
    for (std::size_t i = 0; i < before.pins.size(); ++i)
    {
        REQUIRE(after.pins[i].form == before.pins[i].form);
        REQUIRE(after.pins[i].hands == before.pins[i].hands);
    }
}

TEST_CASE("the file is the schema number, the follower, the switch and the rules", "[profile]")
{
    const auto j = nlohmann::json::parse(WriteProfile(Everything(), kHex));

    REQUIRE(j["schema"] == kProfileSchema);
    REQUIRE(j["follower"]["name"] == "Lydia");
    REQUIRE(j["follower"]["form"] == "0xA2C94");
    REQUIRE(j["enabled"] == false);
    REQUIRE(j["rules"].is_array());

    // Wire names, never display text: this is the shareable format.
    const auto &heal = j["rules"][0];
    REQUIRE(heal["if"]["subject"] == "self");
    REQUIRE(heal["if"]["predicate"] == "health-pct-below");
    REQUIRE(heal["then"]["do"][0]["action"] == "drink-strongest");
    REQUIRE(heal["then"]["do"][0]["effect"] == "Restore Health");
    REQUIRE(heal["label"] == "emergency heal");

    // A form is whatever the codec says, and a hand is a word.
    const auto &frost = j["rules"][2];
    REQUIRE(frost["enabled"] == false);
    REQUIRE(frost["if"]["damage"] == "frost");
    REQUIRE(frost["then"]["do"][0]["form"] == "0x13989");
    REQUIRE(frost["then"]["do"][0]["hand"] == "both");

    // A pin is a form and, when it has one, a hand.
    REQUIRE(j["pins"].size() == 3);
    REQUIRE(j["pins"][0]["form"] == "0x13989");
    REQUIRE(j["pins"][0]["hand"] == "both");
    REQUIRE_FALSE(j["pins"][1].contains("hand"));
    REQUIRE(j["pins"][2]["hand"] == "left");
}

TEST_CASE("a pin this build cannot place is dropped alone", "[profile]")
{
    FormCodec installed = kHex;
    installed.decode = [](std::string_view s) -> std::optional<std::uint32_t> {
        if (s == "0x13989~Skyrim.esm")
            return 0x13989;
        return std::nullopt;
    };
    const std::string file = R"({ "schema": 1, "rules": [], "pins": [
        { "form": "0x13989~Skyrim.esm", "hand": "both" },
        { "form": "0x7~Gone.esp" },
        { "form": "0x13989~Skyrim.esm", "hand": "tail" },
        { "hand": "left" },
        "0x13989~Skyrim.esm"
    ] })";
    const auto read = ReadProfile(file, installed);

    REQUIRE(read.profile->pins.size() == 1);
    REQUIRE(read.profile->pins[0].form == 0x13989);
    REQUIRE(read.profile->pins[0].hands == Hand::Both);
    REQUIRE(read.warnings.size() == 4);
    REQUIRE(read.warnings[0].find("pin 1") != std::string::npos);
    REQUIRE(read.warnings[0].find("0x7~Gone.esp") != std::string::npos);
    REQUIRE(read.warnings[1].find("tail") != std::string::npos);

    // A file from before there were pins has none, and nothing to say.
    const auto older = ReadProfile(OneRuleFile(kHealRule), kHex);
    REQUIRE(older.profile->pins.empty());
    REQUIRE(older.profile->bans.empty());
    REQUIRE(older.warnings.empty());
}

TEST_CASE("bans are forms, written and read back, a stranger dropped alone", "[profile]")
{
    Profile profile;
    profile.bans = {0x13989, 0x2F3B8};
    const auto j = nlohmann::json::parse(WriteProfile(profile, kHex));
    REQUIRE(j["bans"].size() == 2);
    REQUIRE(j["bans"][0] == "0x13989");
    REQUIRE(j["bans"][1] == "0x2F3B8");

    FormCodec installed = kHex;
    installed.decode = [](std::string_view s) -> std::optional<std::uint32_t> {
        if (s == "0x13989~Skyrim.esm")
            return 0x13989;
        return std::nullopt;
    };
    const std::string file = R"({ "schema": 1, "rules": [], "bans": [
        "0x13989~Skyrim.esm", "0x7~Gone.esp", 7, { "form": "0x13989~Skyrim.esm" }
    ] })";
    const auto read = ReadProfile(file, installed);
    REQUIRE(read.profile->bans.size() == 1);
    REQUIRE(read.profile->bans[0] == 0x13989);
    REQUIRE(read.warnings.size() == 3);
    REQUIRE(read.warnings[0].find("ban 1") != std::string::npos);
    REQUIRE(read.warnings[0].find("0x7~Gone.esp") != std::string::npos);
}

TEST_CASE("the file carries only the fields a rule reads", "[profile]")
{
    const auto j = nlohmann::json::parse(WriteProfile(Everything(), kHex));

    // Health-below: an argument, no status, no damage, no named follower.
    const auto &heal = j["rules"][0];
    REQUIRE(heal["if"].contains("arg"));
    REQUIRE_FALSE(heal["if"].contains("status"));
    REQUIRE_FALSE(heal["if"].contains("damage"));
    REQUIRE_FALSE(heal["if"].contains("follower"));
    // A potion names no form and takes no hand.
    REQUIRE_FALSE(heal["then"]["do"][0].contains("form"));
    REQUIRE_FALSE(heal["then"]["do"][0].contains("hand"));

    // Status: a status, no argument.
    const auto &douse = j["rules"][1];
    REQUIRE(douse["if"]["status"] == "burning");
    REQUIRE_FALSE(douse["if"].contains("arg"));
    REQUIRE(douse["if"]["follower"] == "0x1234");
    REQUIRE(douse["then"]["follower"] == "0x1234");
    REQUIRE(douse["then"]["do"][0]["arg"] == 2.5);

    // Arrows have no hand; a rule with no label has no label key.
    const auto &frost = j["rules"][2];
    REQUIRE_FALSE(frost["then"]["do"][1].contains("hand"));
    REQUIRE_FALSE(frost.contains("label"));
}

TEST_CASE("a key this build does not know is ignored", "[profile]")
{
    const std::string rule = R"({
        "colour": "red",
        "if": { "subject": "self", "predicate": "health-pct-below", "arg": 0.5, "mood": "grim" },
        "then": { "target": "self", "do": [ { "action": "drink-strongest-health-potion", "sip": true } ] }
    })";
    const auto read = ReadProfile(OneRuleFile(rule, R"("theme": "dark",)"), kHex);

    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile->rules.rules.size() == 1);
    RequireSame(read.profile->rules.rules[0], [] {
        Rule r = HealBelow(0.5f);
        r.label.clear();
        return r;
    }());
}

TEST_CASE("a rule naming a value this build does not know is dropped, and the rest kept", "[profile]")
{
    const std::string unknownPredicate = R"({
        "label": "future",
        "if": { "subject": "self", "predicate": "distance-to-player-above", "arg": 500 },
        "then": { "target": "self", "do": [ { "action": "drink-strongest-health-potion" } ] }
    })";
    const std::string unknownSubject = R"({
        "if": { "subject": "horse", "predicate": "any" },
        "then": { "target": "self", "do": [] }
    })";
    const std::string unknownStatus = R"({
        "if": { "subject": "self", "predicate": "status", "status": "hungry" },
        "then": { "target": "self", "do": [] }
    })";
    const std::string unknownTarget = R"({
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "horse", "do": [] }
    })";
    const std::string file = R"({ "schema": 1, "rules": [ )" + unknownPredicate + "," + kHealRule + "," +
                             unknownSubject + "," + unknownStatus + "," + unknownTarget + " ] }";
    const auto read = ReadProfile(file, kHex);

    REQUIRE(read.profile.has_value());
    REQUIRE(read.profile->rules.rules.size() == 1);
    REQUIRE(read.profile->rules.rules[0].label == "heal");

    REQUIRE(read.warnings.size() == 4);
    REQUIRE(read.warnings[0].find("rule 0 \"future\"") != std::string::npos);
    REQUIRE(read.warnings[0].find("distance-to-player-above") != std::string::npos);
    REQUIRE(read.warnings[1].find("horse") != std::string::npos);
    REQUIRE(read.warnings[2].find("hungry") != std::string::npos);
    REQUIRE(read.warnings[3].find("horse") != std::string::npos);
}

TEST_CASE("an unknown action is dropped alone and its rule kept", "[profile]")
{
    const std::string rule = R"({
        "if": { "subject": "self", "predicate": "health-pct-below", "arg": 0.5 },
        "then": { "target": "self", "do": [
            { "action": "sing" },
            { "action": "drink-strongest-health-potion" },
            { "action": "equip-weapon", "hand": "tail" }
        ] }
    })";
    const auto read = ReadProfile(OneRuleFile(rule), kHex);

    REQUIRE(read.profile->rules.rules.size() == 1);
    const Rule &r = read.profile->rules.rules[0];
    REQUIRE(r.actions.size() == 1);
    REQUIRE(r.actions[0].kind == ActionKind::DrinkStrongest);
    REQUIRE(r.actions[0].effect == "Restore Health");
    REQUIRE(read.warnings.size() == 2);
    REQUIRE(read.warnings[0].find("sing") != std::string::npos);
    REQUIRE(read.warnings[1].find("tail") != std::string::npos);
}

TEST_CASE("a form the codec cannot resolve drops what names it", "[profile]")
{
    // A codec that knows one plugin: anything else is "not installed".
    FormCodec installed = kHex;
    installed.decode = [](std::string_view s) -> std::optional<std::uint32_t> {
        if (s == "0xBEEF~Present.esp")
            return 0x0100BEEF;
        return std::nullopt;
    };

    const std::string rule = R"({
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "self", "do": [
            { "action": "cast-spell", "form": "0x1~Missing.esp" },
            { "action": "cast-spell", "form": "0xBEEF~Present.esp" }
        ] }
    })";
    const std::string namedFollower = R"({
        "label": "gone",
        "if": { "subject": "follower", "follower": "0x2~Missing.esp", "predicate": "any" },
        "then": { "target": "self", "do": [] }
    })";
    const std::string file = R"({ "schema": 1, "rules": [ )" + rule + "," + namedFollower + " ] }";
    const auto read = ReadProfile(file, installed);

    REQUIRE(read.profile->rules.rules.size() == 1);
    const Rule &r = read.profile->rules.rules[0];
    REQUIRE(r.actions.size() == 1);
    REQUIRE(r.actions[0].form == 0x0100BEEF);
    REQUIRE(read.warnings.size() == 2);
    REQUIRE(read.warnings[0].find("0x1~Missing.esp") != std::string::npos);
    REQUIRE(read.warnings[1].find("rule 1 \"gone\"") != std::string::npos);
}

TEST_CASE("a file from a newer build reads what this one understands", "[profile]")
{
    const auto read = ReadProfile(OneRuleFile(kHealRule, R"("schema": 7,)"), kHex);

    REQUIRE(read.profile.has_value());
    REQUIRE(read.profile->rules.rules.size() == 1);
    REQUIRE(read.profile->rules.schemaVersion == 7);
    REQUIRE(read.warnings.size() == 1);
    REQUIRE(read.warnings[0].find("newer") != std::string::npos);
}

TEST_CASE("what is not a profile reads as nothing, not as a crash", "[profile]")
{
    REQUIRE_FALSE(ReadProfile("", kHex).profile.has_value());
    REQUIRE_FALSE(ReadProfile("{ not json", kHex).profile.has_value());
    REQUIRE_FALSE(ReadProfile("[1, 2, 3]", kHex).profile.has_value());
    REQUIRE_FALSE(ReadProfile("\"a string\"", kHex).profile.has_value());
    REQUIRE(ReadProfile("{ not json", kHex).warnings.size() == 1);

    // An object with no rules is a profile with none, and says so.
    const auto empty = ReadProfile("{}", kHex);
    REQUIRE(empty.profile.has_value());
    REQUIRE(empty.profile->rules.rules.empty());
    REQUIRE(empty.profile->enabled);
    REQUIRE(empty.warnings.size() == 1);
}

TEST_CASE("a field of the wrong shape reads as absent", "[profile]")
{
    const std::string rule = R"({
        "enabled": "yes",
        "label": 12,
        "if": { "subject": "self", "predicate": "health-pct-below", "arg": "half" },
        "then": { "target": "self", "do": { "action": "drink-strongest-health-potion" } }
    })";
    const auto read = ReadProfile(OneRuleFile(rule, R"("enabled": "on",)"), kHex);

    REQUIRE(read.profile->enabled);
    REQUIRE(read.profile->rules.rules.size() == 1);
    const Rule &r = read.profile->rules.rules[0];
    REQUIRE(r.enabled);
    REQUIRE(r.label.empty());
    REQUIRE(r.conditionArg == 0.0f);
    REQUIRE(r.actions.empty()); // "do" was not a list
    REQUIRE(read.warnings.empty());

    // A rule that is not an object is dropped with its index.
    const auto odd = ReadProfile(R"({ "rules": [ 42 ] })", kHex);
    REQUIRE(odd.profile->rules.rules.empty());
    REQUIRE(odd.warnings.size() == 1);
    REQUIRE(odd.warnings[0].find("rule 0") != std::string::npos);
}

TEST_CASE("the hex codec passes ids through unchanged", "[profile]")
{
    REQUIRE(kHex.encode(0xA2C94) == "0xA2C94");
    REQUIRE(kHex.encode(0) == "0x0");
    REQUIRE(kHex.decode("0xA2C94") == 0xA2C94u);
    REQUIRE(kHex.decode("a2c94") == 0xA2C94u);
    REQUIRE(kHex.decode("0xFF00ABCD") == 0xFF00ABCDu);
    REQUIRE_FALSE(kHex.decode("").has_value());
    REQUIRE_FALSE(kHex.decode("0x").has_value());
    REQUIRE_FALSE(kHex.decode("Lydia").has_value());
    REQUIRE_FALSE(kHex.decode("0xA2C94~Skyrim.esm").has_value());
}

TEST_CASE("a policy names its effect on the wire, and the old fixed names still read", "[profile]")
{
    // The twelve policies of before 2026-09-08 read as today's four with
    // the vanilla effect's name; today's names carry the effect beside them,
    // and one without an effect is dropped alone.
    const std::string rule = R"({
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "self", "do": [
            { "action": "apply-weakest-stamina-poison" },
            { "action": "drink-weakest", "effect": "Resist Fire" },
            { "action": "drink-strongest" }
        ] }
    })";
    const auto read = ReadProfile(OneRuleFile(rule), kHex);
    REQUIRE(read.profile->rules.rules.size() == 1);
    const Rule &r = read.profile->rules.rules[0];
    REQUIRE(r.actions.size() == 2);
    REQUIRE(r.actions[0].kind == ActionKind::ApplyWeakest);
    REQUIRE(r.actions[0].effect == "Damage Stamina");
    REQUIRE(r.actions[1].kind == ActionKind::DrinkWeakest);
    REQUIRE(r.actions[1].effect == "Resist Fire");
    REQUIRE(read.warnings.size() == 1);

    // And back out: the effect is written beside the action.
    const auto again = WriteProfile(*read.profile, kHex);
    const auto j = nlohmann::json::parse(again);
    REQUIRE(j["rules"][0]["then"]["do"][1]["action"] == "drink-weakest");
    REQUIRE(j["rules"][0]["then"]["do"][1]["effect"] == "Resist Fire");
}
