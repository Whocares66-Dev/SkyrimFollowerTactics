// The profile file: what a follower's tactics look like on disk, and how
// leniently they come back. No Skyrim here; forms pass through as hex.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Build.h"
#include "core/Profile.h"
#include "core/Vocabulary.h"

#include <string>

using namespace ft;
using ft::test::HealBelow;

namespace
{

const FormCodec kHex = FormCodec::Hex();

// Resist Fire 25% with Fortify Health 30: one enchantment made at the
// table, as its recipe.
const std::vector<EnchantEffect> kWardens{{0x581F7, 25.0f, 0, 0}, {0x49507, 30.0f, 0, 0}};

// One rule of every shape the file has to carry: a named follower on both
// sides, a status, a damage kind, an equip with a hand, a cast with an
// argument, two actions in one rule, and a rule switched off.
Profile Everything()
{
    Profile p;
    p.followerName = "Lydia";
    p.followerForm = "0xA2C94";
    p.enabled = false;
    p.rules.rules.push_back(HealBelow(0.5f, "emergency heal"));

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
        r.predicate = PredicateKind::HitBy;
        r.damageKind = DamageKind::Any;
        r.actionTarget = ActionTargetKind::Attacker;
        r.FirstAction().kind = ActionKind::Attack;
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
        Action dual;
        dual.kind = ActionKind::CastSpell;
        dual.form = 0x12FCD;
        dual.dual = true;
        r.actions.push_back(dual);
        Action poison;
        poison.kind = ActionKind::ApplyPoison;
        poison.form = 0x73F30;
        r.actions.push_back(poison);
        Action gem;
        gem.kind = ActionKind::ChargeSoulGem;
        gem.form = 0x2E4E2;
        r.actions.push_back(gem);
        p.rules.rules.push_back(r);
    }
    {
        // A hit type, and an enemy attacking a named member of the party.
        Rule r;
        r.subject = SubjectKind::Ally;
        r.predicate = PredicateKind::HitType;
        r.damageKind = DamageKind::Ranged;
        r.actionTarget = ActionTargetKind::Self;
        r.FirstAction().kind = ActionKind::ApplyWeakest;
        r.FirstAction().effect = "Damage Health";
        p.rules.rules.push_back(r);
        Rule peel;
        peel.subject = SubjectKind::Enemy;
        peel.predicate = PredicateKind::Attacking;
        peel.subjectForm = 0x1234;
        peel.actionTarget = ActionTargetKind::Enemy;
        peel.FirstAction().kind = ActionKind::PowerAttack;
        p.rules.rules.push_back(peel);
    }
    p.pins.push_back({0x13989, ItemVariant{}, Hand::Both}); // a bow, the plain one
    ItemVariant enchanted;
    enchanted.enchantment = kWardens;
    enchanted.label = "Warden";
    p.pins.push_back({0x12E49, enchanted, Hand::None}); // a cuirass: the enchanted copy, renamed
    p.pins.push_back({0x12FCD, {}, Hand::Left});        // a spell in one hand
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    p.bans = {{0x2F3B8, {}}, {0x12E49, tempered}};
    return p;
}

// Field by field first, so a failure names the field; then the whole, so a
// field added to the struct and forgotten here still counts.
void RequireSame(const Action &a, const Action &b)
{
    REQUIRE(a.kind == b.kind);
    REQUIRE(a.form == b.form);
    REQUIRE(a.hand == b.hand);
    REQUIRE(a.arg == b.arg);
    REQUIRE(a.dual == b.dual);
    REQUIRE(a.effect == b.effect);
    REQUIRE(a == b);
}

void RequireSame(const Rule &a, const Rule &b)
{
    REQUIRE(a.enabled == b.enabled);
    REQUIRE(a.label == b.label);
    REQUIRE(a.subject == b.subject);
    REQUIRE(a.subjectForm == b.subjectForm);
    REQUIRE(a.predicate == b.predicate);
    REQUIRE(a.conditionArg == b.conditionArg);
    REQUIRE(a.statusKind == b.statusKind);
    REQUIRE(a.typeKind == b.typeKind);
    REQUIRE(a.damageKind == b.damageKind);
    REQUIRE(a.actionTarget == b.actionTarget);
    REQUIRE(a.actionTargetForm == b.actionTargetForm);
    REQUIRE(a.actions.size() == b.actions.size());
    for (std::size_t i = 0; i < a.actions.size(); ++i)
        RequireSame(a.actions[i], b.actions[i]);
    REQUIRE(a == b);
}

// A file with one rule, from the pieces a test wants to vary.
std::string OneRuleFile(const std::string &rule, const std::string &extraTop = "")
{
    return R"({ "schema": 1, "enabled": true, )" + extraTop + R"( "rules": [ )" + rule + " ] }";
}

const std::string kHealRule = R"({
    "label": "heal",
    "if": { "subject": "self", "predicate": "health-pct-below", "arg": 0.5 },
    "then": { "target": "self", "do": [ { "action": "drink-strongest", "effect": "Restore Health" } ] }
})";

} // namespace

TEST_CASE("the settings round-trip, and a document missing them keeps the defaults", "[profile]")
{
    Settings s;
    REQUIRE(s.tacticsEnabled);        // an empty rule list does nothing, so on is safe
    REQUIRE(s.requireDualWieldStyle); // vanilla's own answer: the style decides
    REQUIRE_FALSE(s.requireDualCastPerks);
    REQUIRE_FALSE(s.requirePowerBashPerk);

    s.tacticsEnabled = false;
    s.requireDualWieldStyle = false;
    s.requireDualCastPerks = true;
    s.requirePowerBashPerk = true;
    const auto back = ReadSettings(WriteSettings(s));
    REQUIRE(back);
    REQUIRE_FALSE(back->tacticsEnabled);
    REQUIRE_FALSE(back->requireDualWieldStyle);
    REQUIRE(back->requireDualCastPerks);
    REQUIRE(back->requirePowerBashPerk);

    // A key this build does not know is ignored, and one it knows but the
    // document does not carry keeps its default.
    const auto partial = ReadSettings(R"({"schema":1,"requirePowerBashPerk":true,"somethingElse":7})");
    REQUIRE(partial);
    REQUIRE(partial->tacticsEnabled);
    REQUIRE(partial->requireDualWieldStyle);
    REQUIRE_FALSE(partial->requireDualCastPerks);
    REQUIRE(partial->requirePowerBashPerk);

    // Not a document at all: nothing read, and the caller keeps what it has.
    REQUIRE_FALSE(ReadSettings("{").has_value());
    REQUIRE_FALSE(ReadSettings("[1,2,3]").has_value());
}

TEST_CASE("a profile round-trips through its file", "[profile]")
{
    const Profile before = Everything();
    const auto read = ReadProfile(WriteProfile(before, kHex), kHex);

    for (const auto &warning : read.warnings)
        UNSCOPED_INFO(warning);
    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile.has_value());
    const Profile &after = *read.profile;
    REQUIRE(after.followerName == before.followerName);
    REQUIRE(after.followerForm == before.followerForm);
    REQUIRE(after.enabled == before.enabled);
    REQUIRE(after.rules.rules.size() == before.rules.rules.size());
    for (std::size_t i = 0; i < before.rules.rules.size(); ++i)
        RequireSame(before.rules.rules[i], after.rules.rules[i]);
    REQUIRE(after.pins.size() == before.pins.size());
    for (std::size_t i = 0; i < before.pins.size(); ++i)
    {
        REQUIRE(after.pins[i].form == before.pins[i].form);
        REQUIRE(after.pins[i].variant.has_value() == before.pins[i].variant.has_value());
        REQUIRE(SameVariant(after.pins[i].variant, before.pins[i].variant));
        REQUIRE(after.pins[i].hands == before.pins[i].hands);
    }
    REQUIRE(after.bans.size() == before.bans.size());
    for (std::size_t i = 0; i < before.bans.size(); ++i)
    {
        REQUIRE(after.bans[i].form == before.bans[i].form);
        REQUIRE(after.bans[i].variant.has_value() == before.bans[i].variant.has_value());
        REQUIRE(SameVariant(after.bans[i].variant, before.bans[i].variant));
    }
}

TEST_CASE("every action that names a form keeps it through the file, and no other writes one", "[profile]")
{
    // One rule per action kind, each with a form set. The ones that name a
    // form read it back; the rest never wrote it. A named poison and a
    // named soul gem were lost here until 2026-09-11: the profile's own
    // list of which actions carry a form had neither, so the rule came
    // back with form 0 and could never fire again.
    for (std::size_t i = 1; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto kind = static_cast<ActionKind>(i);
        INFO(WireName(kind));
        Rule r;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::Any;
        r.actionTarget =
            IsActionValidFor(ActionTargetKind::Self, kind) ? ActionTargetKind::Self : ActionTargetKind::Enemy;
        Action a;
        a.kind = kind;
        a.form = 0xFF00ABCD;
        if (IsPolicy(kind))
            a.effect = "Restore Health";
        r.actions.push_back(a);
        Profile p;
        p.rules.rules.push_back(r);

        const auto read = ReadProfile(WriteProfile(p, kHex), kHex);
        REQUIRE(read.warnings.empty());
        REQUIRE(read.profile->rules.rules.size() == 1);
        REQUIRE(read.profile->rules.rules[0].actions.size() == 1);
        const Action &back = read.profile->rules.rules[0].actions[0];
        REQUIRE(back.kind == kind);
        REQUIRE(back.form == (NamesForm(kind) ? 0xFF00ABCDu : 0u));
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
}

TEST_CASE("bans are a form and a variant, written and read back, a stranger dropped alone", "[profile]")
{
    Profile profile;
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    profile.bans = {{0x13989, std::nullopt}, {0x2F3B8, tempered}};
    const auto j = nlohmann::json::parse(WriteProfile(profile, kHex));
    REQUIRE(j["bans"].size() == 2);
    REQUIRE(j["bans"][0]["form"] == "0x13989");
    REQUIRE_FALSE(j["bans"][0].contains("variant")); // the form, whichever: nothing to say
    REQUIRE(j["bans"][1]["form"] == "0x2F3B8");
    REQUIRE(j["bans"][1]["variant"]["tempering"].get<double>() == Catch::Approx(1.2));

    FormCodec installed = kHex;
    installed.decode = [](std::string_view s) -> std::optional<std::uint32_t> {
        if (s == "0x13989~Skyrim.esm")
            return 0x13989;
        if (s == "0x581F7~Skyrim.esm")
            return 0x581F7;
        return std::nullopt;
    };
    // A bare form is a ban on every variant of it; a variant with an effect
    // this load order cannot name is dropped alone; a part of the wrong
    // shape reads as absent, and a variant of the wrong shape as none.
    const std::string file = R"({ "schema": 1, "rules": [], "bans": [
        { "form": "0x13989~Skyrim.esm", "variant": { "tempering": 1.2 } }, { "form": "0x7~Gone.esp" }, 7,
        "0x13989~Skyrim.esm",
        { "form": "0x13989~Skyrim.esm", "variant": { "enchant": [ { "effect": "0x9~Gone.esp", "mag": 10 } ] } },
        { "form": "0x13989~Skyrim.esm", "variant": { "enchant": [ { "effect": "0x581F7~Skyrim.esm", "mag": 25, "dur": 3 } ], "label": "Fang" } },
        { "form": "0x13989~Skyrim.esm", "variant": { "tempering": "fine", "enchant": "0x581F7~Skyrim.esm" } },
        { "form": "0x13989~Skyrim.esm" },
        { "form": "0x13989~Skyrim.esm", "variant": "fine" }
    ] })";
    const auto read = ReadProfile(file, installed);
    REQUIRE(read.profile->bans.size() == 5);
    REQUIRE(read.profile->bans[0].form == 0x13989);
    REQUIRE(read.profile->bans[0].variant->tempering == Catch::Approx(1.2f));
    REQUIRE(read.profile->bans[1].variant->enchantment.size() == 1);
    REQUIRE(read.profile->bans[1].variant->enchantment[0].effect == 0x581F7);
    REQUIRE(read.profile->bans[1].variant->enchantment[0].magnitude == Catch::Approx(25.0f));
    REQUIRE(read.profile->bans[1].variant->enchantment[0].duration == 3);
    REQUIRE(read.profile->bans[1].variant->label == "Fang");
    REQUIRE(read.profile->bans[2].variant->IsPlain()); // parts of the wrong shape read as absent
    REQUIRE_FALSE(read.profile->bans[3].variant.has_value());
    REQUIRE_FALSE(read.profile->bans[4].variant.has_value());
    REQUIRE(read.warnings.size() == 4);
    REQUIRE(read.warnings[0].find("ban 1") != std::string::npos);
    REQUIRE(read.warnings[0].find("0x7~Gone.esp") != std::string::npos);
    REQUIRE(read.warnings[3].find("0x9~Gone.esp") != std::string::npos);
}

TEST_CASE("an action names its variant on the wire only when it has one, and its thing's last name", "[profile]")
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Self;
    Action plain;
    plain.kind = ActionKind::EquipWeapon;
    plain.form = 0x13989;
    plain.hand = Hand::Right;
    Action smithed = plain;
    smithed.variant.emplace().tempering = 1.2f;
    smithed.name = "Iron Dagger (Fine)";
    Action drink;
    drink.kind = ActionKind::DrinkPotion;
    drink.form = 0x3EADE;
    drink.variant.emplace().tempering = 5.0f; // not an equip: never written
    drink.name = "Potion of Minor Healing";
    r.actions = {plain, smithed, drink};
    Profile p;
    p.rules.rules.push_back(r);
    const auto j = nlohmann::json::parse(WriteProfile(p, kHex));
    const auto &steps = j["rules"][0]["then"]["do"];
    REQUIRE(steps.size() == 3);
    REQUIRE_FALSE(steps[0].contains("variant"));
    REQUIRE_FALSE(steps[0].contains("name")); // no name seen yet: nothing to keep
    REQUIRE(steps[1]["variant"]["tempering"].get<double>() == Catch::Approx(1.2));
    REQUIRE(steps[1]["name"] == "Iron Dagger (Fine)");
    REQUIRE_FALSE(steps[2].contains("variant"));
    REQUIRE(steps[2]["name"] == "Potion of Minor Healing");
    const auto read = ReadProfile(WriteProfile(p, kHex), kHex);
    REQUIRE(read.warnings.empty());
    const auto &back = read.profile->rules.rules[0].actions;
    REQUIRE(back.size() == 3);
    REQUIRE_FALSE(back[0].variant.has_value());
    REQUIRE(back[1].variant->tempering == Catch::Approx(1.2f));
    REQUIRE(back[1].name == "Iron Dagger (Fine)");
    REQUIRE_FALSE(back[2].variant.has_value());
    REQUIRE(back[2].name == "Potion of Minor Healing");
}

TEST_CASE("a pin writes its variant as one object, with only the parts it has; the form alone writes none", "[profile]")
{
    Profile profile;
    ItemVariant enchanted;
    enchanted.enchantment = kWardens;
    profile.pins = {
        {0x12E49, std::nullopt, Hand::None}, {0x12E49, enchanted, Hand::None}, {0x12E49, ItemVariant{}, Hand::None}};
    const auto j = nlohmann::json::parse(WriteProfile(profile, kHex));
    REQUIRE(j["pins"].size() == 3);
    REQUIRE_FALSE(j["pins"][0].contains("variant"));
    // Each effect: its form, its magnitude, and a duration or area only
    // when it has one.
    const auto &v = j["pins"][1]["variant"];
    REQUIRE(v["enchant"].size() == 2);
    REQUIRE(v["enchant"][0]["effect"] == "0x581F7");
    REQUIRE(v["enchant"][0]["mag"] == 25.0);
    REQUIRE_FALSE(v["enchant"][0].contains("dur"));
    REQUIRE(v["enchant"][1]["effect"] == "0x49507");
    REQUIRE_FALSE(v.contains("tempering"));
    // The plain variant is an object with nothing in it: a row, not the form.
    REQUIRE(j["pins"][2]["variant"].is_object());
    REQUIRE(j["pins"][2]["variant"].empty());
    const auto read = ReadProfile(WriteProfile(profile, kHex), kHex);
    REQUIRE(read.profile->pins.size() == 3);
    REQUIRE_FALSE(read.profile->pins[0].variant.has_value());
    REQUIRE(read.profile->pins[1].variant.has_value());
    REQUIRE(SameVariant(*read.profile->pins[1].variant, enchanted));
    REQUIRE(read.profile->pins[2].variant.has_value());
    REQUIRE(read.profile->pins[2].variant->IsPlain());
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
        "then": { "target": "self", "do": [ { "action": "drink-strongest", "effect": "Restore Health", "sip": true } ] }
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
        "then": { "target": "self", "do": [ { "action": "drink-strongest", "effect": "Restore Health" } ] }
    })";
    const std::string unknownSubject = R"({
        "if": { "subject": "horse", "predicate": "any" },
        "then": { "target": "self", "do": [] }
    })";
    const std::string unknownStatus = R"({
        "if": { "subject": "self", "predicate": "status", "status": "hungry" },
        "then": { "target": "self", "do": [] }
    })";
    const std::string unknownType = R"({
        "if": { "subject": "enemy", "predicate": "type", "type": "sload" },
        "then": { "target": "self", "do": [] }
    })";
    const std::string unknownTarget = R"({
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "horse", "do": [] }
    })";
    const std::string file = R"({ "schema": 1, "rules": [ )" + unknownPredicate + "," + kHealRule + "," +
                             unknownSubject + "," + unknownStatus + "," + unknownType + "," + unknownTarget + " ] }";
    const auto read = ReadProfile(file, kHex);

    REQUIRE(read.profile.has_value());
    REQUIRE(read.profile->rules.rules.size() == 1);
    REQUIRE(read.profile->rules.rules[0].label == "heal");

    REQUIRE(read.warnings.size() == 5);
    REQUIRE(read.warnings[0].find("rule 0 \"future\"") != std::string::npos);
    REQUIRE(read.warnings[0].find("distance-to-player-above") != std::string::npos);
    REQUIRE(read.warnings[1].find("horse") != std::string::npos);
    REQUIRE(read.warnings[2].find("hungry") != std::string::npos);
    REQUIRE(read.warnings[3].find("sload") != std::string::npos);
    REQUIRE(read.warnings[4].find("horse") != std::string::npos);
}

TEST_CASE("an arrow policy and a none by hand round-trip", "[profile]")
{
    Profile p;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::EquipStrongestArrows;
    Action none;
    none.kind = ActionKind::EquipWeapon;
    none.hand = Hand::Left;
    r.actions.push_back(none);
    p.rules.rules.push_back(r);
    const auto j = nlohmann::json::parse(WriteProfile(p, kHex));
    const auto &steps = j["rules"][0]["then"]["do"];
    REQUIRE(steps[0]["action"] == "equip-strongest-arrows");
    REQUIRE_FALSE(steps[0].contains("form")); // chosen at evaluation, never written
    REQUIRE(steps[1]["action"] == "equip-weapon");
    REQUIRE_FALSE(steps[1].contains("form"));
    REQUIRE(steps[1]["hand"] == "left");
    const auto read = ReadProfile(WriteProfile(p, kHex), kHex);
    REQUIRE(read.warnings.empty());
    const auto &back = read.profile->rules.rules[0].actions;
    REQUIRE(back.size() == 2);
    REQUIRE(back[0].kind == ActionKind::EquipStrongestArrows);
    REQUIRE(back[1].kind == ActionKind::EquipWeapon);
    REQUIRE(back[1].form == 0);
    REQUIRE(back[1].hand == Hand::Left);
}

TEST_CASE("a type condition writes its kind and reads it back", "[profile]")
{
    Profile p;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::Type;
    r.typeKind = TypeKind::DarkElf;
    r.actionTarget = ActionTargetKind::Enemy;
    r.FirstAction().kind = ActionKind::PowerAttack;
    p.rules.rules.push_back(r);
    const auto j = nlohmann::json::parse(WriteProfile(p, kHex));
    REQUIRE(j["rules"][0]["if"]["predicate"] == "type");
    REQUIRE(j["rules"][0]["if"]["type"] == "dark-elf");
    REQUIRE_FALSE(j["rules"][0]["if"].contains("status"));
    const auto read = ReadProfile(WriteProfile(p, kHex), kHex);
    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile->rules.rules.size() == 1);
    REQUIRE(read.profile->rules.rules[0].typeKind == TypeKind::DarkElf);
}

TEST_CASE("an unknown action is dropped alone and its rule kept", "[profile]")
{
    const std::string rule = R"({
        "if": { "subject": "self", "predicate": "health-pct-below", "arg": 0.5 },
        "then": { "target": "self", "do": [
            { "action": "sing" },
            { "action": "drink-strongest", "effect": "Restore Health" },
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
        "then": { "target": "self", "do": { "action": "drink-strongest", "effect": "Restore Health" } }
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

TEST_CASE("a policy names its effect on the wire, and one without an effect is dropped alone", "[profile]")
{
    const std::string rule = R"({
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "self", "do": [
            { "action": "apply-weakest", "effect": "Damage Stamina" },
            { "action": "drink-weakest", "effect": "Resist Fire" },
            { "action": "eat-strongest-food", "effect": "Restore Stamina" },
            { "action": "drink-strongest" }
        ] }
    })";
    const auto read = ReadProfile(OneRuleFile(rule), kHex);
    REQUIRE(read.profile->rules.rules.size() == 1);
    const Rule &r = read.profile->rules.rules[0];
    REQUIRE(r.actions.size() == 3);
    REQUIRE(r.actions[0].kind == ActionKind::ApplyWeakest);
    REQUIRE(r.actions[0].effect == "Damage Stamina");
    REQUIRE(r.actions[1].kind == ActionKind::DrinkWeakest);
    REQUIRE(r.actions[1].effect == "Resist Fire");
    REQUIRE(r.actions[2].kind == ActionKind::EatStrongestFood);
    REQUIRE(r.actions[2].effect == "Restore Stamina");
    REQUIRE(read.warnings.size() == 1);

    // And back out: the effect is written beside the action.
    const auto again = WriteProfile(*read.profile, kHex);
    const auto j = nlohmann::json::parse(again);
    REQUIRE(j["rules"][0]["then"]["do"][1]["action"] == "drink-weakest");
    REQUIRE(j["rules"][0]["then"]["do"][1]["effect"] == "Resist Fire");
}

TEST_CASE("an 'any' rule survives the save: an empty effect, and the two roll actions", "[profile]")
{
    // An EMPTY effect is "any" -- roll the effect, then take the strongest
    // or weakest of it -- and is distinct from a MISSING one, which is a
    // malformed record and is dropped (the test above). The two "any"
    // actions name neither an effect nor a form: they roll the thing.
    const std::string rule = R"({
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "self", "do": [
            { "action": "apply-strongest", "effect": "" },
            { "action": "drink-weakest", "effect": "" },
            { "action": "apply-any" },
            { "action": "drink-any" }
        ] }
    })";
    const auto read = ReadProfile(OneRuleFile(rule), kHex);
    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile->rules.rules.size() == 1);
    const Rule &r = read.profile->rules.rules[0];
    REQUIRE(r.actions.size() == 4);
    REQUIRE(r.actions[0].kind == ActionKind::ApplyStrongest);
    REQUIRE(r.actions[0].effect.empty());
    REQUIRE(r.actions[1].kind == ActionKind::DrinkWeakest);
    REQUIRE(r.actions[1].effect.empty());
    REQUIRE(r.actions[2].kind == ActionKind::ApplyAny);
    REQUIRE(r.actions[3].kind == ActionKind::DrinkAny);

    // Back out unchanged: the empty effect is written, so the next read
    // tells "any" from a record that lost its key.
    const auto j = nlohmann::json::parse(WriteProfile(*read.profile, kHex));
    const auto &out = j["rules"][0]["then"]["do"];
    REQUIRE(out[0]["action"] == "apply-strongest");
    REQUIRE(out[0]["effect"] == "");
    REQUIRE(out[1]["effect"] == "");
    REQUIRE(out[2]["action"] == "apply-any");
    REQUIRE_FALSE(out[2].contains("effect"));
    REQUIRE_FALSE(out[2].contains("form"));
    REQUIRE(out[3]["action"] == "drink-any");
    REQUIRE_FALSE(out[3].contains("form"));
}

TEST_CASE("a rule missing a part it cannot do without is dropped, and says which part", "[profile]")
{
    // Each shape is one rule in one file, so the warning is the whole
    // story of that file. The heal rule follows it and must survive.
    struct Shape
    {
        const char *rule;
        const char *reason;
    };
    const Shape shapes[] = {
        {R"({ "then": { "target": "self", "do": [] } })", "no \"if\""},
        {R"({ "if": { "predicate": "any" }, "then": { "target": "self", "do": [] } })", "no subject"},
        {R"({ "if": { "subject": "self" }, "then": { "target": "self", "do": [] } })", "no predicate"},
        {R"({ "if": { "subject": "self", "predicate": "attacked-by", "damage": "psychic" },
              "then": { "target": "self", "do": [] } })",
         "unknown damage kind \"psychic\""},
        {R"({ "if": { "subject": "self", "predicate": "any" } })", "no \"then\""},
        {R"({ "if": { "subject": "self", "predicate": "any" }, "then": { "do": [] } })", "no target"},
        {R"({ "if": { "subject": "self", "predicate": "any" },
              "then": { "target": "follower", "follower": "0x9~Gone.esp", "do": [] } })",
         "target follower \"0x9~Gone.esp\" is not in this load order"},
        {R"({ "if": { "subject": "enemy", "predicate": "attacking", "member": "0x9~Gone.esp" },
              "then": { "target": "self", "do": [] } })",
         "member \"0x9~Gone.esp\" is not in this load order"},
    };
    FormCodec strict = kHex;
    strict.decode = [](std::string_view) -> std::optional<std::uint32_t> { return std::nullopt; };

    for (const Shape &shape : shapes)
    {
        INFO(shape.rule);
        const std::string file = R"({ "schema": 1, "rules": [ )" + std::string(shape.rule) + "," + kHealRule + " ] }";
        const auto read = ReadProfile(file, strict);
        REQUIRE(read.profile.has_value());
        REQUIRE(read.profile->rules.rules.size() == 1);
        REQUIRE(read.profile->rules.rules[0].label == "heal");
        REQUIRE(read.warnings.size() == 1);
        REQUIRE(read.warnings[0].find("rule 0") != std::string::npos);
        REQUIRE(read.warnings[0].find(shape.reason) != std::string::npos);
    }

    // An action that is not an object, or names none, goes alone.
    const std::string oddActions = R"({ "schema": 1, "rules": [ {
        "label": "odd",
        "if": { "subject": "self", "predicate": "any" },
        "then": { "target": "self", "do": [ 7, { "effect": "Restore Health" },
                                            { "action": "drink-strongest", "effect": "Restore Health" } ] }
    } ] })";
    const auto read = ReadProfile(oddActions, kHex);
    REQUIRE(read.profile->rules.rules.size() == 1);
    REQUIRE(read.profile->rules.rules[0].actions.size() == 1);
    REQUIRE(read.warnings.size() == 2);
    REQUIRE(read.warnings[0].find("action 0: not an object") != std::string::npos);
    REQUIRE(read.warnings[1].find("action 1: no action") != std::string::npos);
}

TEST_CASE("the damage kind of a hit condition round-trips", "[profile]")
{
    // Hit type carries a damage kind exactly as Hit by does. It was left out
    // of the writer when the condition was added, so every Hit type rule came
    // back as the field's default, Fire.
    Profile p;
    Rule hitType;
    hitType.subject = SubjectKind::Enemy;
    hitType.predicate = PredicateKind::HitType;
    hitType.damageKind = DamageKind::Ranged;
    hitType.actionTarget = ActionTargetKind::Enemy;
    hitType.FirstAction().kind = ActionKind::Attack;
    Rule hitBy = hitType;
    hitBy.predicate = PredicateKind::HitBy;
    hitBy.damageKind = DamageKind::Frost;
    p.rules.rules.push_back(hitType);
    p.rules.rules.push_back(hitBy);

    const std::string text = WriteProfile(p, kHex);
    const auto j = nlohmann::json::parse(text);
    REQUIRE(j["rules"][0]["if"]["damage"] == "ranged");
    REQUIRE(j["rules"][1]["if"]["damage"] == "frost");

    const auto read = ReadProfile(text, kHex);
    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile->rules.rules[0].damageKind == DamageKind::Ranged);
    REQUIRE(read.profile->rules.rules[1].damageKind == DamageKind::Frost);
}

TEST_CASE("the party member of attacking and attacked by round-trips", "[profile]")
{
    // The player is written by name, a follower by form; absent is the
    // player, as a file from before there was a member.
    Profile p;
    Rule onPlayer;
    onPlayer.subject = SubjectKind::Enemy;
    onPlayer.predicate = PredicateKind::Attacking;
    onPlayer.subjectForm = 0;
    onPlayer.actionTarget = ActionTargetKind::Enemy;
    onPlayer.FirstAction().kind = ActionKind::Attack;
    Rule onFollower = onPlayer;
    onFollower.predicate = PredicateKind::AttackedBy;
    onFollower.subjectForm = 0x1234;
    p.rules.rules.push_back(onPlayer);
    p.rules.rules.push_back(onFollower);

    const std::string text = WriteProfile(p, kHex);
    const auto j = nlohmann::json::parse(text);
    REQUIRE(j["rules"][0]["if"]["member"] == "player");
    REQUIRE(j["rules"][1]["if"]["member"] == "0x1234");

    const auto read = ReadProfile(text, kHex);
    REQUIRE(read.warnings.empty());
    REQUIRE(read.profile->rules.rules[0].subjectForm == 0);
    REQUIRE(read.profile->rules.rules[1].subjectForm == 0x1234);

    const std::string older = R"({ "schema": 1, "rules": [ {
        "if": { "subject": "enemy", "predicate": "attacking" },
        "then": { "target": "enemy", "do": [ { "action": "attack" } ] }
    } ] })";
    const auto old = ReadProfile(older, kHex);
    REQUIRE(old.warnings.empty());
    REQUIRE(old.profile->rules.rules[0].subjectForm == 0);
}
