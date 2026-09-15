#include "game/Profiles.h"

#include "game/Log.h"
#include "game/Settings.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ft::game
{
namespace
{

// The co-save. SKSE keeps one block per plugin, by this id, and drops the
// block on the next save when the plugin is gone -- which is what makes
// removing the mod clean.
constexpr std::uint32_t kPluginId = 'FTAC';
// One record per follower: the key, then the JSON text, each as a length
// and the bytes. The record's version is the format's schema number, so
// a record from a newer build says so before it is parsed.
constexpr std::uint32_t kFollowerRecord = 'PROF';
// One record for the player's own choices, which belong to no follower:
// what the Settings page requires (game/Settings.h), as JSON text.
constexpr std::uint32_t kSettingsRecord = 'SETT';

// The records the loaded save holds, by key, until a follower claims
// theirs. Whatever is still here when the game saves is written back as
// it came: a dismissed follower's tactics survive any number of saves
// made while they are away. Game thread.
std::unordered_map<std::string, std::string> g_saved;

bool WriteString(const SKSE::SerializationInterface *intfc, const std::string &s)
{
    const auto length = static_cast<std::uint32_t>(s.size());
    return intfc->WriteRecordData(length) && (length == 0 || intfc->WriteRecordData(s.data(), length));
}

bool ReadString(const SKSE::SerializationInterface *intfc, std::string &s)
{
    std::uint32_t length = 0;
    if (intfc->ReadRecordData(length) != sizeof length)
        return false;
    s.resize(length);
    return length == 0 || intfc->ReadRecordData(s.data(), length) == length;
}

bool WriteFollower(const SKSE::SerializationInterface *intfc, const std::string &key, const std::string &text)
{
    return intfc->OpenRecord(kFollowerRecord, static_cast<std::uint32_t>(ft::kProfileSchema)) &&
           WriteString(intfc, key) && WriteString(intfc, text);
}

void OnSave(SKSE::SerializationInterface *intfc)
{
    if (!intfc->OpenRecord(kSettingsRecord, static_cast<std::uint32_t>(ft::kProfileSchema)) ||
        !WriteString(intfc, ft::WriteSettings(CurrentSettings())))
        log::profiles.error("could not write the settings to the save");

    std::size_t live = 0;
    std::size_t carried = 0;
    for (const Filed &filed : ProfilesToSave())
    {
        if (!WriteFollower(intfc, filed.who.key, ft::WriteProfile(filed.profile, GameFormCodec())))
        {
            log::profiles.error("{}: could not write tactics to the save", filed.who.name);
            continue;
        }
        ++live;
    }
    for (const auto &[key, text] : g_saved)
    {
        if (!WriteFollower(intfc, key, text))
        {
            log::profiles.error("{}: could not write tactics back to the save", key);
            continue;
        }
        ++carried;
    }
    log::profiles.info("saved {} follower record(s), {} carried from the loaded save", live, carried);
}

void OnLoad(SKSE::SerializationInterface *intfc)
{
    g_saved.clear();
    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    while (intfc->GetNextRecordInfo(type, version, length))
    {
        if (type == kSettingsRecord)
        {
            std::string text;
            if (!ReadString(intfc, text))
            {
                log::profiles.error("the settings record is cut short -- the defaults stand");
                continue;
            }
            if (const auto settings = ft::ReadSettings(text))
                SetSettings(*settings);
            else
                log::profiles.warn("the settings record could not be read -- the defaults stand");
            continue;
        }
        if (type != kFollowerRecord)
        {
            log::profiles.warn("co-save record {:08X} is not one this build knows -- skipped", type);
            continue;
        }
        std::string key;
        std::string text;
        if (!ReadString(intfc, key) || !ReadString(intfc, text))
        {
            log::profiles.error("a co-save record is cut short -- skipped");
            continue;
        }
        if (version > static_cast<std::uint32_t>(ft::kProfileSchema))
            log::profiles.warn("{}: saved by a newer build (schema {}) -- reading what this one understands", key,
                               version);
        g_saved[key] = std::move(text);
    }
    log::profiles.info("the save holds tactics for {} follower(s)", g_saved.size());
}

// Before a load and on a new game: nothing from the last session may
// carry over. The new save's records follow, in OnLoad, or none do.
void OnRevert(SKSE::SerializationInterface *)
{
    g_saved.clear();
    // The defaults, so a save with no settings record -- one made before
    // they existed, or by a build without them -- does not inherit the last
    // session's.
    SetSettings(ft::Settings{});
    ForgetSession();
}

} // namespace

const ft::FormCodec &GameFormCodec()
{
    static const ft::FormCodec codec = [] {
        ft::FormCodec c;
        c.encode = [](std::uint32_t id) {
            auto *form = RE::TESForm::LookupByID(id);
            const RE::TESFile *file = (form && !form->IsDynamicForm()) ? form->GetFile(0) : nullptr;
            if (!file)
                return fmt::format("0x{:X}", id);
            return fmt::format("0x{:X}~{}", form->GetLocalFormID(), file->GetFilename());
        };
        c.decode = [](std::string_view s) -> std::optional<std::uint32_t> {
            const auto tilde = s.find('~');
            const auto hex = ft::FormCodec::Hex().decode(s.substr(0, tilde));
            if (!hex)
                return std::nullopt;
            if (tilde == std::string_view::npos)
                return hex; // a bare id: a runtime form, taken as it is
            auto *data = RE::TESDataHandler::GetSingleton();
            if (!data)
                return std::nullopt;
            const RE::FormID id = data->LookupFormID(*hex, s.substr(tilde + 1));
            if (id == 0)
                return std::nullopt; // that plugin is not loaded
            return id;
        };
        return c;
    }();
    return codec;
}

Identity IdentifyFollower(RE::Actor *actor)
{
    Identity who;
    who.name = DisplayNameOf(actor);

    auto stable = [](RE::TESForm *f) { return f && !f->IsDynamicForm() && f->GetFile(0); };
    RE::TESForm *record = actor->GetActorBase();
    if (!stable(record))
        record = actor;
    if (stable(record))
    {
        who.key = fmt::format("{}-{:X}", record->GetFile(0)->GetFilename(), record->GetLocalFormID());
        who.form = GameFormCodec().encode(record->GetFormID());
    }
    else
    {
        who.key = fmt::format("dynamic-{:08X}", actor->GetFormID());
        who.form = fmt::format("0x{:X}", actor->GetFormID());
        log::profiles.warn("{} has no record in any plugin -- tactics keyed by reference id", Describe(actor));
    }
    return who;
}

void InstallSerialization()
{
    const auto *serialization = SKSE::GetSerializationInterface();
    if (!serialization)
    {
        log::profiles.error("no SKSE serialization interface -- tactics will not be saved");
        return;
    }
    serialization->SetUniqueID(kPluginId);
    serialization->SetSaveCallback(OnSave);
    serialization->SetLoadCallback(OnLoad);
    serialization->SetRevertCallback(OnRevert);
}

std::optional<ft::Profile> ClaimSaved(const Identity &who)
{
    auto node = g_saved.extract(who.key);
    if (node.empty())
        return std::nullopt;

    auto read = ft::ReadProfile(node.mapped(), GameFormCodec());
    for (const auto &warning : read.warnings)
        log::profiles.warn("{}: saved tactics: {}", who.name, warning);
    if (!read.profile)
    {
        log::profiles.warn("{}: the saved tactics could not be read -- starting with none", who.name);
        return std::nullopt;
    }
    log::profiles.info("{}: {} rule(s) and {} pin(s) from the save, {}", who.name, read.profile->rules.rules.size(),
                       read.profile->pins.size(), read.profile->enabled ? "on" : "off");
    return std::move(read.profile);
}

} // namespace ft::game
