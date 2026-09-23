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

[[nodiscard]] bool UsesType(PredicateKind p) noexcept
{
    return p == PredicateKind::Type;
}

[[nodiscard]] bool UsesDamage(PredicateKind p) noexcept
{
    return IsResistance(p) || p == PredicateKind::HitBy || p == PredicateKind::HitType;
}

[[nodiscard]] bool UsesArg(ActionKind a) noexcept
{
    return a == ActionKind::CastSpell;
}

// --- writing ---------------------------------------------------------------

// The variant on the wire: one object under "variant", each part in it
// only when present, so the plain variant is an empty object; no variant
// -- the form, whichever -- writes no key (dev/UNIQUE.md, "The variant").
void WriteVariant(json &j, const std::optional<ItemVariant> &variant, const FormCodec &codec)
{
    if (!variant)
        return;
    json v = json::object();
    if (!variant->enchantment.empty())
    {
        json effects = json::array();
        for (const EnchantEffect &e : variant->enchantment)
        {
            json one;
            one["effect"] = codec.encode(e.effect);
            one["mag"] = e.magnitude;
            if (e.duration != 0)
                one["dur"] = e.duration;
            if (e.area != 0)
                one["area"] = e.area;
            effects.push_back(std::move(one));
        }
        v["enchant"] = std::move(effects);
    }
    if (variant->tempering != 0.0f)
        v["tempering"] = variant->tempering;
    if (!variant->label.empty())
        v["label"] = variant->label;
    j["variant"] = std::move(v);
}

json WriteAction(const Action &a, const FormCodec &codec)
{
    json j;
    j["action"] = WireName(a.kind);
    if (NamesForm(a.kind) && a.form != 0)
        j["form"] = codec.encode(a.form);
    if (IsEquip(a.kind))
        WriteVariant(j, a.variant, codec);
    if (NamesForm(a.kind) && a.form != 0 && !a.name.empty())
        j["name"] = a.name;
    if (TakesHand(a.kind))
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
    // Written only when it is on, as every other exception here is: a rule
    // that is not negated says nothing about negation.
    if (r.negated)
        cond["not"] = true;
    // The party member of Attacking / Attacked by: "player", or the
    // follower's form.
    if (r.predicate == PredicateKind::Attacking || r.predicate == PredicateKind::AttackedBy)
        cond["member"] = r.subjectForm == 0 ? std::string("player") : codec.encode(r.subjectForm);
    if (ArgumentFor(r.predicate) != ArgumentKind::None)
        cond["arg"] = r.conditionArg;
    if (UsesStatus(r.predicate))
        cond["status"] = WireName(r.statusKind);
    if (UsesType(r.predicate))
        cond["type"] = WireName(r.typeKind);
    if (UsesDamage(r.predicate))
        cond["damage"] = WireName(r.damageKind);
    // The effect, as a form: another load order's Oakflesh reads back as
    // its own.
    if (r.predicate == PredicateKind::EffectRunning)
        cond["effect"] = codec.encode(r.conditionForm);
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

// The variant read back: no "variant" key, or one of the wrong shape, is
// the form, whichever variant; within it, absent parts are plain and a
// part of the wrong shape reads as absent; a form whose plugin is not
// loaded is reported as such so the record can be dropped (an effect
// that is not in this game is not a variant this game can match).
struct VariantField
{
    std::optional<ItemVariant> variant;
    std::string missing; // the form text this load order cannot name, or empty
};
[[nodiscard]] VariantField ReadVariant(const json &j, const FormCodec &codec);

[[nodiscard]] const json *Obj(const json &j, const char *key);
[[nodiscard]] std::optional<bool> Bool(const json &j, const char *key);

[[nodiscard]] std::optional<double> Num(const json &j, const char *key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return std::nullopt;
    return it->get<double>();
}

VariantField ReadVariant(const json &j, const FormCodec &codec)
{
    VariantField out;
    const json *v = Obj(j, "variant");
    if (!v)
        return out;
    ItemVariant &variant = out.variant.emplace();
    // The enchantment is its effects; one whose effect this load order
    // cannot name is a whole the game cannot match, and the entry goes.
    if (const auto effects = v->find("enchant"); effects != v->end() && effects->is_array())
    {
        for (const json &one : *effects)
        {
            if (!one.is_object())
                continue;
            const auto text = Str(one, "effect");
            if (!text)
                continue;
            const auto form = codec.decode(*text);
            if (!form)
            {
                out.missing = *text;
                return out;
            }
            EnchantEffect e;
            e.effect = *form;
            e.magnitude = static_cast<float>(Num(one, "mag").value_or(0.0));
            e.duration = static_cast<std::uint32_t>(Num(one, "dur").value_or(0.0));
            e.area = static_cast<std::uint32_t>(Num(one, "area").value_or(0.0));
            variant.enchantment.push_back(e);
        }
    }
    if (const auto tempering = Num(*v, "tempering"))
        variant.tempering = static_cast<float>(*tempering);
    variant.label = Str(*v, "label").value_or("");
    return out;
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
        // An empty effect is "any", and written that way; a MISSING one is
        // a malformed record, so the key itself must be there.
        const auto effect = Str(j, "effect");
        if (!effect)
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
    if (IsEquip(a.kind))
    {
        const VariantField named = ReadVariant(j, codec);
        if (!named.missing.empty())
        {
            why = "form \"" + named.missing + "\" is not in this load order";
            return std::nullopt;
        }
        a.variant = named.variant;
    }
    if (NamesForm(a.kind) && a.form != 0)
        a.name = Str(j, "name").value_or("");
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
    // A Not on a condition that cannot take one -- Any, or a fight's edge --
    // is dropped, and the rule is kept: what it says about the condition
    // itself is still good, and the flag would only make it unanswerable.
    r.negated = Bool(*cond, "not").value_or(false) && CanNegate(r.predicate);
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
    if (r.predicate == PredicateKind::EffectRunning)
    {
        const FormField f = ReadForm(*cond, "effect", codec);
        if (!f.ok)
            return drop("effect \"" + f.text + "\" is not in this load order");
        r.conditionForm = f.id;
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
    if (const auto type = Str(*cond, "type"))
    {
        if (const auto t = TypeFromWireName(*type))
            r.typeKind = *t;
        else
            return drop("unknown type \"" + *type + "\"");
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

std::string WriteSettings(const Settings &settings)
{
    json j;
    j["schema"] = kProfileSchema;
    j["tacticsEnabled"] = settings.tacticsEnabled;
    j["requireDualWieldStyle"] = settings.requireDualWieldStyle;
    j["requireDualCastPerks"] = settings.requireDualCastPerks;
    j["requirePowerBashPerk"] = settings.requirePowerBashPerk;
    j["variedAiChoices"] = settings.variedAiChoices;
    return j.dump(2);
}

std::optional<Settings> ReadSettings(std::string_view text)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    Settings s;
    s.tacticsEnabled = Bool(j, "tacticsEnabled").value_or(s.tacticsEnabled);
    s.requireDualWieldStyle = Bool(j, "requireDualWieldStyle").value_or(s.requireDualWieldStyle);
    s.requireDualCastPerks = Bool(j, "requireDualCastPerks").value_or(s.requireDualCastPerks);
    s.requirePowerBashPerk = Bool(j, "requirePowerBashPerk").value_or(s.requirePowerBashPerk);
    s.variedAiChoices = Bool(j, "variedAiChoices").value_or(s.variedAiChoices);
    return s;
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
    j["idleEnabled"] = profile.idleEnabled;
    json rules = json::array();
    for (const Rule &r : profile.rules.rules)
        rules.push_back(WriteRule(r, codec));
    j["rules"] = std::move(rules);
    json idleRules = json::array();
    for (const Rule &r : profile.idleRules.rules)
        idleRules.push_back(WriteRule(r, codec));
    j["idleRules"] = std::move(idleRules);
    json pins = json::array();
    for (const PinEntry &pin : profile.pins)
    {
        json p;
        p["form"] = codec.encode(pin.form);
        WriteVariant(p, pin.variant, codec);
        if (pin.hands != Hand::None)
            p["hand"] = WireName(pin.hands);
        pins.push_back(std::move(p));
    }
    j["pins"] = std::move(pins);
    json bans = json::array();
    for (const Banned &ban : profile.bans)
    {
        json b;
        b["form"] = codec.encode(ban.form);
        WriteVariant(b, ban.variant, codec);
        bans.push_back(std::move(b));
    }
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
    if (const auto schema = Num(j, "schema"); schema && static_cast<int>(*schema) > kProfileSchema)
        result.warnings.push_back("schema " + std::to_string(static_cast<int>(*schema)) +
                                  " is newer than this build's " + std::to_string(kProfileSchema) +
                                  " -- reading what it understands");
    if (const json *who = Obj(j, "follower"))
    {
        p.followerName = Str(*who, "name").value_or("");
        p.followerForm = Str(*who, "form").value_or("");
    }
    p.enabled = Bool(j, "enabled").value_or(true);
    p.idleEnabled = Bool(j, "idleEnabled").value_or(true);

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

    if (const auto idle = j.find("idleRules"); idle != j.end() && idle->is_array())
    {
        std::size_t index = 0;
        for (const json &r : *idle)
        {
            if (auto rule = ReadRule(r, codec, result.warnings, "idle rule " + std::to_string(index)))
                p.idleRules.rules.push_back(std::move(*rule));
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
            const VariantField named = ReadVariant(entry, codec);
            if (!named.missing.empty())
            {
                result.warnings.push_back(where + ": form \"" + named.missing +
                                          "\" is not in this load order -- pin dropped");
                continue;
            }
            pin.variant = named.variant;
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

    // Bans: absent is none. A form and a variant each; one this load order
    // cannot name is dropped alone.
    if (const auto bans = j.find("bans"); bans != j.end() && bans->is_array())
    {
        std::size_t index = 0;
        for (const json &entry : *bans)
        {
            const std::string where = "ban " + std::to_string(index++);
            if (!entry.is_object())
            {
                result.warnings.push_back(where + ": not an object -- ban dropped");
                continue;
            }
            const auto formText = Str(entry, "form");
            if (!formText)
            {
                result.warnings.push_back(where + ": no form -- ban dropped");
                continue;
            }
            const auto form = codec.decode(*formText);
            if (!form)
            {
                result.warnings.push_back(where + ": form \"" + *formText +
                                          "\" is not in this load order -- ban dropped");
                continue;
            }
            const VariantField named = ReadVariant(entry, codec);
            if (!named.missing.empty())
            {
                result.warnings.push_back(where + ": form \"" + named.missing +
                                          "\" is not in this load order -- ban dropped");
                continue;
            }
            p.bans.push_back({*form, named.variant});
        }
    }

    result.profile = std::move(p);
    return result;
}

} // namespace ft
