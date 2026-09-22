#include "progression/core/Serialize.h"

#include <nlohmann/json.hpp>

namespace fp
{
namespace
{

using nlohmann::json;

json SkillMap(const PerSkill<int> &values)
{
    json out = json::object();
    for (const Skill s : AllSkills())
        if (values[Index(s)] != 0)
            out[std::string(Key(s))] = values[Index(s)];
    return out;
}

PerSkill<int> ReadSkillMap(const json &in)
{
    PerSkill<int> out{};
    if (!in.is_object())
        return out;
    for (const auto &[key, value] : in.items())
        if (const auto s = SkillFromKey(key); s && value.is_number_integer())
            out[Index(*s)] = value.get<int>();
    return out;
}

json ProgressMap(const PerSkill<double> &values)
{
    json out = json::object();
    for (const Skill s : AllSkills())
        if (values[Index(s)] != 0.0)
            out[std::string(Key(s))] = values[Index(s)];
    return out;
}

PerSkill<double> ReadProgressMap(const json &in)
{
    PerSkill<double> out{};
    if (!in.is_object())
        return out;
    for (const auto &[key, value] : in.items())
        if (const auto s = SkillFromKey(key); s && value.is_number())
            out[Index(*s)] = value.get<double>();
    return out;
}

json AttributeMap(const PerAttribute<int> &values)
{
    json out = json::object();
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        if (values[i] != 0)
            out[std::string(Key(static_cast<Attribute>(i)))] = values[i];
    return out;
}

PerAttribute<int> ReadAttributeMap(const json &in)
{
    PerAttribute<int> out{};
    if (!in.is_object())
        return out;
    for (const auto &[key, value] : in.items())
        if (const auto a = AttributeFromKey(key); a && value.is_number_integer())
            out[Index(*a)] = value.get<int>();
    return out;
}

template <typename T> T Get(const json &in, const char *key, T fallback)
{
    const auto it = in.find(key);
    if (it == in.end() || it->is_null())
        return fallback;
    try
    {
        return it->get<T>();
    }
    catch (const json::exception &)
    {
        return fallback;
    }
}

std::optional<FormKey> ReadKey(const json &in, const char *key)
{
    const auto it = in.find(key);
    if (it == in.end() || !it->is_string())
        return std::nullopt;
    return ParseFormKey(it->get<std::string>());
}

} // namespace

std::string WriteCompanion(const Companion &c)
{
    json j;
    j["schema"] = kSchema;
    j["key"] = ToString(c.key);
    j["name"] = c.name;
    j["paused"] = c.paused;
    j["level"] = c.level;
    j["learning"] = {{"skills", SkillMap(c.learning.skills)},
                     {"progress", ProgressMap(c.learning.progress)},
                     {"attributePoints", AttributeMap(c.learning.attributePoints)},
                     {"attributes", AttributeMap(c.learning.attributes)},
                     {"xp", c.learning.xp},
                     {"pool", c.learning.pool}};
    j["applied"] = {{"skills", SkillMap(c.applied.skills)}, {"attributes", AttributeMap(c.applied.attributes)}};

    json perks = json::array();
    for (const LearnedPerk &p : c.perks)
        perks.push_back({{"form", ToString(p.form)}, {"name", p.name}, {"rank", p.rank}});
    j["perks"] = std::move(perks);

    json setAside = json::array();
    for (const FormKey &form : c.setAside)
        setAside.push_back(ToString(form));
    j["setAside"] = std::move(setAside);

    json spells = json::array();
    for (const TaughtSpell &s : c.spells)
        spells.push_back({{"spell", ToString(s.spell)}, {"name", s.name}});
    j["spells"] = std::move(spells);

    json spellsSetAside = json::array();
    for (const SpellAside &s : c.spellsSetAside)
        spellsSetAside.push_back({{"spell", ToString(s.spell)}, {"name", s.name}});
    j["spellsSetAside"] = std::move(spellsSetAside);
    return j.dump();
}

std::optional<Companion> ReadCompanion(std::string_view text, std::string *why)
{
    const auto fail = [&](std::string reason) -> std::optional<Companion> {
        if (why)
            *why = std::move(reason);
        return std::nullopt;
    };

    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return fail("not JSON");
    if (Get<std::uint32_t>(j, "schema", 0) > kSchema)
        return fail("written by a newer build (schema " + std::to_string(Get<std::uint32_t>(j, "schema", 0)) + ")");
    const auto key = ReadKey(j, "key");
    if (!key)
        return fail("no key");

    Companion c;
    c.key = *key;
    c.name = Get<std::string>(j, "name", "");
    c.paused = Get<bool>(j, "paused", false);
    c.level = Get<int>(j, "level", 0);
    if (const auto it = j.find("learning"); it != j.end() && it->is_object())
    {
        c.learning.skills = ReadSkillMap(it->value("skills", json::object()));
        c.learning.progress = ReadProgressMap(it->value("progress", json::object()));
        c.learning.attributePoints = ReadAttributeMap(it->value("attributePoints", json::object()));
        c.learning.attributes = ReadAttributeMap(it->value("attributes", json::object()));
        c.learning.xp = Get<double>(*it, "xp", 0.0);
        c.learning.pool = Get<double>(*it, "pool", 0.0);
    }
    if (const auto it = j.find("applied"); it != j.end() && it->is_object())
    {
        c.applied.skills = ReadSkillMap(it->value("skills", json::object()));
        c.applied.attributes = ReadAttributeMap(it->value("attributes", json::object()));
    }

    if (const auto it = j.find("perks"); it != j.end() && it->is_array())
        for (const json &p : *it)
        {
            if (!p.is_object())
                continue;
            const auto form = ReadKey(p, "form");
            if (!form)
                continue;
            c.perks.push_back({*form, Get<std::string>(p, "name", ""), std::max(Get<int>(p, "rank", 1), 1)});
        }

    if (const auto it = j.find("setAside"); it != j.end() && it->is_array())
        for (const json &f : *it)
            if (f.is_string())
                if (const auto form = ParseFormKey(f.get<std::string>()))
                    c.setAside.push_back(*form);

    if (const auto it = j.find("spells"); it != j.end() && it->is_array())
        for (const json &s : *it)
        {
            if (!s.is_object())
                continue;
            const auto spell = ReadKey(s, "spell");
            if (!spell)
                continue;
            c.spells.push_back({*spell, Get<std::string>(s, "name", "")});
        }

    if (const auto it = j.find("spellsSetAside"); it != j.end() && it->is_array())
        for (const json &s : *it)
        {
            if (!s.is_object())
                continue;
            if (const auto spell = ReadKey(s, "spell"))
                c.spellsSetAside.push_back({*spell, Get<std::string>(s, "name", "")});
        }
    return c;
}

std::string WriteSettings(const Settings &s)
{
    json j{{"schema", kSchema},
           {"autoEnroll", s.autoEnroll},
           {"notifyLevels", s.notifyLevels},
           {"notifySkills", s.notifySkills},
           {"showNoEffectPerks", s.showNoEffectPerks},
           {"released", s.released}};
    return j.dump();
}

std::optional<Settings> ReadSettings(std::string_view text)
{
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    Settings s;
    s.autoEnroll = Get<bool>(j, "autoEnroll", s.autoEnroll);
    s.notifyLevels = Get<bool>(j, "notifyLevels", s.notifyLevels);
    s.notifySkills = Get<bool>(j, "notifySkills", s.notifySkills);
    s.showNoEffectPerks = Get<bool>(j, "showNoEffectPerks", s.showNoEffectPerks);
    s.released = Get<bool>(j, "released", s.released);
    return s;
}

std::vector<CoSaveRecord> PackCoSave(std::span<const Companion> companions, const Settings &settings)
{
    std::vector<CoSaveRecord> records;
    records.push_back({kSettingsRecord, kSchema, WriteSettings(settings)});
    for (const Companion &c : companions)
        records.push_back({kCompanionRecord, kSchema, WriteCompanion(c)});
    return records;
}

CoSaveContents UnpackCoSave(std::span<const CoSaveRecord> records)
{
    CoSaveContents out;
    for (const CoSaveRecord &r : records)
    {
        if (r.payload.size() > kMaxRecordBytes)
        {
            out.notes.push_back("a record of " + std::to_string(r.payload.size()) +
                                " bytes is larger than any of ours; skipped");
            continue;
        }
        if (r.version > kSchema)
        {
            out.notes.push_back("a record of schema " + std::to_string(r.version) + " is from a newer build; skipped");
            continue;
        }
        if (r.type == kSettingsRecord)
        {
            out.settings = ReadSettings(r.payload);
            if (!out.settings)
                out.notes.push_back("the settings could not be read; defaults stand");
        }
        else if (r.type == kCompanionRecord)
        {
            std::string why;
            if (auto c = ReadCompanion(r.payload, &why))
            {
                const bool duplicate = std::any_of(out.companions.begin(), out.companions.end(),
                                                   [&](const Companion &other) { return other.key == c->key; });
                if (duplicate)
                    out.notes.push_back(ToString(c->key) + " appears twice; the first record is kept");
                else
                    out.companions.push_back(std::move(*c));
            }
            else
                out.notes.push_back("a companion could not be read: " + why);
        }
        else
            out.notes.push_back("a record of unknown type " + Hex(r.type, 8) + "; skipped");
    }
    return out;
}

} // namespace fp
