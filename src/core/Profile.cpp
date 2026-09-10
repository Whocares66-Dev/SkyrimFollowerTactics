#include "Profile.h"

#include "Vocabulary.h"

#include <cctype>
#include <charconv>
#include <format>
#include <nlohmann/json.hpp>
#include <string>

namespace ft
{
namespace
{

// ordered_json keeps keys in the order they were written, so a file reads
// top to bottom the way a rule does -- if, then -- and two versions of one
// file diff line by line rather than by alphabet.
using json = nlohmann::ordered_json;

// --- which fields a rule actually reads ------------------------------------
//
// The file carries what the rule uses and nothing else: a status only under
// the Status predicate, a hand only under the equips that take one. Absent
// fields read as the struct's defaults, which is what the unused ones hold.

[[nodiscard]] bool UsesStatus(PredicateKind p) noexcept
{
    return p == PredicateKind::Status;
}

[[nodiscard]] bool UsesDamage(PredicateKind p) noexcept
{
    return IsResistance(p) || p == PredicateKind::HitBy;
}

[[nodiscard]] bool UsesForm(ActionKind a) noexcept
{
    return IsCast(a) || a == ActionKind::DrinkPotion || a == ActionKind::EatFood || a == ActionKind::EatIngredient ||
           IsEquip(a);
}

[[nodiscard]] bool UsesHand(ActionKind a) noexcept
{
    return a == ActionKind::EquipWeapon || a == ActionKind::EquipSpell;
}

[[nodiscard]] bool UsesArg(ActionKind a) noexcept
{
    return a == ActionKind::CastSpell;
}

// --- writing ---------------------------------------------------------------

json WriteAction(const Action &a, const FormCodec &codec)
{
    json j;
    j["action"] = WireName(a.kind);
    if (UsesForm(a.kind) && a.form != 0)
        j["form"] = codec.encode(a.form);
    if (UsesHand(a.kind))
        j["hand"] = WireName(a.hand);
    if (UsesArg(a.kind) && a.arg != 0.0f)
        j["arg"] = a.arg;
    if (a.kind == ActionKind::CastSpell && a.dual)
        j["dual"] = true;
    if (IsPolicy(a.kind))
        j["effect"] = a.effect;
    return j;
}

json WriteRule(const Rule &r, const FormCodec &codec)
{
    json j;
    j["enabled"] = r.enabled;
    if (!r.label.empty())
        j["label"] = r.label;

    json cond;
    cond["subject"] = WireName(r.subject);
    if (r.subject == SubjectKind::Follower)
        cond["follower"] = codec.encode(r.subjectForm);
    cond["predicate"] = WireName(r.predicate);
    // The party member of Attacking / Attacked by: "player", or the
    // follower's form.
    if (r.predicate == PredicateKind::Attacking || r.predicate == PredicateKind::AttackedBy)
        cond["member"] = r.subjectForm == 0 ? std::string("player") : codec.encode(r.subjectForm);
    if (ArgumentFor(r.predicate) != ArgumentKind::None)
        cond["arg"] = r.conditionArg;
    if (UsesStatus(r.predicate))
        cond["status"] = WireName(r.statusKind);
    if (UsesDamage(r.predicate))
        cond["damage"] = WireName(r.damageKind);
    j["if"] = std::move(cond);

    json then;
    then["target"] = WireName(r.actionTarget);
    if (r.actionTarget == ActionTargetKind::Follower)
        then["follower"] = codec.encode(r.actionTargetForm);
    json actions = json::array();
    for (const Action &a : r.actions)
        actions.push_back(WriteAction(a, codec));
    then["do"] = std::move(actions);
    j["then"] = std::move(then);
    return j;
}

// --- reading ---------------------------------------------------------------

// Typed lookups that answer "absent or the wrong shape" with nullopt, so
// nothing below throws. A hand-edited file with "arg": "half" is a file with
// no arg, not a crash.
[[nodiscard]] std::optional<std::string> Str(const json &j, const char *key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

[[nodiscard]] std::optional<double> Num(const json &j, const char *key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return std::nullopt;
    return it->get<double>();
}

[[nodiscard]] std::optional<bool> Bool(const json &j, const char *key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean())
        return std::nullopt;
    return it->get<bool>();
}

[[nodiscard]] const json *Obj(const json &j, const char *key)
{
    const auto it = j.find(key);
    return (it == j.end() || !it->is_object()) ? nullptr : &*it;
}

// A form field: absent is 0, which every action reads as "none"; present
// but undecodable -- a plugin not installed -- is a failure the caller
// drops the carrier for.
struct FormField
{
    std::uint32_t id{0};
    bool ok{true};
    std::string text;
};

[[nodiscard]] FormField ReadForm(const json &j, const char *key, const FormCodec &codec)
{
    const auto text = Str(j, key);
    if (!text)
        return {};
    const auto id = codec.decode(*text);
    if (!id)
        return {0, false, *text};
    return {*id, true, *text};
}

// Reads one action, or says why it will not: the reason is the warning.
[[nodiscard]] std::optional<Action> ReadAction(const json &j, const FormCodec &codec, std::string &why)
{
    if (!j.is_object())
    {
        why = "not an object";
        return std::nullopt;
    }
    const auto name = Str(j, "action");
    if (!name)
    {
        why = "no action";
        return std::nullopt;
    }
    Action a;
    const auto kind = ActionFromWireName(*name);
    if (!kind)
    {
        why = "unknown action \"" + *name + "\"";
        return std::nullopt;
    }
    a.kind = *kind;
    if (IsPolicy(a.kind))
    {
        const auto effect = Str(j, "effect");
        if (!effect || effect->empty())
        {
            why = "no effect for \"" + *name + "\"";
            return std::nullopt;
        }
        a.effect = *effect;
    }
    const FormField form = ReadForm(j, "form", codec);
    if (!form.ok)
    {
        why = "form \"" + form.text + "\" is not in this load order";
        return std::nullopt;
    }
    a.form = form.id;
    if (const auto hand = Str(j, "hand"))
    {
        const auto h = HandFromWireName(*hand);
        if (!h)
        {
            why = "unknown hand \"" + *hand + "\"";
            return std::nullopt;
        }
        a.hand = *h;
    }
    if (const auto arg = Num(j, "arg"))
        a.arg = static_cast<float>(*arg);
    if (a.kind == ActionKind::CastSpell)
        a.dual = Bool(j, "dual").value_or(false);
    return a;
}

[[nodiscard]] std::optional<Rule> ReadRule(const json &j, const FormCodec &codec, std::vector<std::string> &warnings,
                                           const std::string &where)
{
    // The rule is dropped by returning nullopt after one of these; the
    // action-level drops below only warn and carry on.
    auto drop = [&](const std::string &why) {
        warnings.push_back(where + ": " + why + " -- rule dropped");
        return std::nullopt;
    };

    if (!j.is_object())
        return drop("not an object");

    Rule r;
    r.enabled = Bool(j, "enabled").value_or(true);
    r.label = Str(j, "label").value_or("");

    const json *cond = Obj(j, "if");
    if (!cond)
        return drop("no \"if\"");
    const auto subject = Str(*cond, "subject");
    if (!subject)
        return drop("no subject");
    if (const auto s = SubjectFromWireName(*subject))
        r.subject = *s;
    else
        return drop("unknown subject \"" + *subject + "\"");
    if (r.subject == SubjectKind::Follower)
    {
        const FormField f = ReadForm(*cond, "follower", codec);
        if (!f.ok)
            return drop("follower \"" + f.text + "\" is not in this load order");
        r.subjectForm = f.id;
    }
    const auto predicate = Str(*cond, "predicate");
    if (!predicate)
        return drop("no predicate");
    if (const auto p = PredicateFromWireName(*predicate))
        r.predicate = *p;
    else
        return drop("unknown predicate \"" + *predicate + "\"");
    if (r.predicate == PredicateKind::Attacking || r.predicate == PredicateKind::AttackedBy)
    {
        // Absent or "player" is the player; anything else a follower's form.
        const auto member = Str(*cond, "member");
        if (member && *member != "player")
        {
            const FormField f = ReadForm(*cond, "member", codec);
            if (!f.ok)
                return drop("member \"" + f.text + "\" is not in this load order");
            r.subjectForm = f.id;
        }
    }
    if (const auto arg = Num(*cond, "arg"))
        r.conditionArg = static_cast<float>(*arg);
    if (const auto status = Str(*cond, "status"))
    {
        if (const auto s = StatusFromWireName(*status))
            r.statusKind = *s;
        else
            return drop("unknown status \"" + *status + "\"");
    }
    if (const auto damage = Str(*cond, "damage"))
    {
        if (const auto d = DamageFromWireName(*damage))
            r.damageKind = *d;
        else
            return drop("unknown damage kind \"" + *damage + "\"");
    }

    const json *then = Obj(j, "then");
    if (!then)
        return drop("no \"then\"");
    const auto target = Str(*then, "target");
    if (!target)
        return drop("no target");
    if (const auto t = ActionTargetFromWireName(*target))
        r.actionTarget = *t;
    else
        return drop("unknown target \"" + *target + "\"");
    if (r.actionTarget == ActionTargetKind::Follower)
    {
        const FormField f = ReadForm(*then, "follower", codec);
        if (!f.ok)
            return drop("target follower \"" + f.text + "\" is not in this load order");
        r.actionTargetForm = f.id;
    }

    const auto actions = then->find("do");
    if (actions != then->end() && actions->is_array())
    {
        std::size_t index = 0;
        for (const json &a : *actions)
        {
            std::string why;
            if (auto action = ReadAction(a, codec, why))
                r.actions.push_back(*action);
            else
                warnings.push_back(std::format("{} action {}: {} -- action dropped", where, index, why));
            ++index;
        }
    }
    return r;
}

} // namespace

FormCodec FormCodec::Hex()
{
    FormCodec c;
    c.encode = [](std::uint32_t id) {
        char buf[16];
        const auto res = std::to_chars(buf, buf + sizeof buf, id, 16);
        std::string s(buf, res.ptr);
        for (char &ch : s)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return "0x" + s;
    };
    c.decode = [](std::string_view s) -> std::optional<std::uint32_t> {
        if (s.starts_with("0x") || s.starts_with("0X"))
            s.remove_prefix(2);
        if (s.empty())
            return std::nullopt;
        std::uint32_t id = 0;
        const auto res = std::from_chars(s.data(), s.data() + s.size(), id, 16);
        if (res.ec != std::errc{} || res.ptr != s.data() + s.size())
            return std::nullopt;
        return id;
    };
    return c;
}

std::string WriteProfile(const Profile &profile, const FormCodec &codec)
{
    json j;
    j["schema"] = kProfileSchema;
    json who;
    who["name"] = profile.followerName;
    who["form"] = profile.followerForm;
    j["follower"] = std::move(who);
    j["enabled"] = profile.enabled;
    json rules = json::array();
    for (const Rule &r : profile.rules.rules)
        rules.push_back(WriteRule(r, codec));
    j["rules"] = std::move(rules);
    json pins = json::array();
    for (const PinEntry &pin : profile.pins)
    {
        json p;
        p["form"] = codec.encode(pin.form);
        if (pin.hands != Hand::None)
            p["hand"] = WireName(pin.hands);
        pins.push_back(std::move(p));
    }
    j["pins"] = std::move(pins);
    json bans = json::array();
    for (const std::uint32_t form : profile.bans)
        bans.push_back(codec.encode(form));
    j["bans"] = std::move(bans);
    return j.dump(2) + "\n";
}

ReadResult ReadProfile(std::string_view text, const FormCodec &codec)
{
    ReadResult result;
    // No exceptions: a malformed file parses to a discarded value and is
    // reported as such, rather than thrown across the game's frame.
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded())
    {
        result.warnings.emplace_back("not valid JSON -- nothing read");
        return result;
    }
    if (!j.is_object())
    {
        result.warnings.emplace_back("not a JSON object -- nothing read");
        return result;
    }

    Profile p;
    if (const auto schema = Num(j, "schema"))
    {
        p.rules.schemaVersion = static_cast<int>(*schema);
        if (p.rules.schemaVersion > kProfileSchema)
            result.warnings.push_back("schema " + std::to_string(p.rules.schemaVersion) +
                                      " is newer than this build's " + std::to_string(kProfileSchema) +
                                      " -- reading what it understands");
    }
    if (const json *who = Obj(j, "follower"))
    {
        p.followerName = Str(*who, "name").value_or("");
        p.followerForm = Str(*who, "form").value_or("");
    }
    p.enabled = Bool(j, "enabled").value_or(true);

    const auto rules = j.find("rules");
    if (rules == j.end() || !rules->is_array())
    {
        result.warnings.emplace_back("no \"rules\" list -- reading none");
    }
    else
    {
        std::size_t index = 0;
        for (const json &r : *rules)
        {
            std::string where = "rule " + std::to_string(index);
            if (r.is_object())
            {
                if (const auto label = Str(r, "label"); label && !label->empty())
                    where += " \"" + *label + "\"";
            }
            if (auto rule = ReadRule(r, codec, result.warnings, where))
                p.rules.rules.push_back(std::move(*rule));
            ++index;
        }
    }

    // Pins: absent is none, as a file from before there were any. One that
    // names a form not in this load order, or a hand unknown here, is
    // dropped alone.
    if (const auto pins = j.find("pins"); pins != j.end() && pins->is_array())
    {
        std::size_t index = 0;
        for (const json &entry : *pins)
        {
            const std::string where = "pin " + std::to_string(index++);
            if (!entry.is_object())
            {
                result.warnings.push_back(where + ": not an object -- pin dropped");
                continue;
            }
            const auto formText = Str(entry, "form");
            if (!formText)
            {
                result.warnings.push_back(where + ": no form -- pin dropped");
                continue;
            }
            const auto form = codec.decode(*formText);
            if (!form)
            {
                result.warnings.push_back(where + ": form \"" + *formText +
                                          "\" is not in this load order -- pin dropped");
                continue;
            }
            PinEntry pin;
            pin.form = *form;
            if (const auto hand = Str(entry, "hand"))
            {
                const auto h = HandFromWireName(*hand);
                if (!h)
                {
                    result.warnings.push_back(where + ": unknown hand \"" + *hand + "\" -- pin dropped");
                    continue;
                }
                pin.hands = *h;
            }
            p.pins.push_back(pin);
        }
    }

    // Bans: absent is none. A form each; one this load order cannot name
    // is dropped alone.
    if (const auto bans = j.find("bans"); bans != j.end() && bans->is_array())
    {
        std::size_t index = 0;
        for (const json &entry : *bans)
        {
            const std::string where = "ban " + std::to_string(index++);
            if (!entry.is_string())
            {
                result.warnings.push_back(where + ": not a form -- ban dropped");
                continue;
            }
            const auto form = codec.decode(entry.get<std::string>());
            if (!form)
            {
                result.warnings.push_back(where + ": form \"" + entry.get<std::string>() +
                                          "\" is not in this load order -- ban dropped");
                continue;
            }
            p.bans.push_back(*form);
        }
    }

    result.profile = std::move(p);
    return result;
}

} // namespace ft
