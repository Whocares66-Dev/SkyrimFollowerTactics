#include "game/Toggles.h"

#include "core/PluginFile.h"
#include "game/Effects.h"
#include "game/Log.h"
#include "game/Sensors.h"
#include "game/Sheet.h"
#include "game/Util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ft::game
{
namespace
{

// Power or spell -> the ability it toggles. Written once at load.
std::unordered_map<RE::FormID, RE::FormID> g_links;

// One top group of a plugin and the file's masters. The other groups are
// passed by their sizes, unread: Skyrim.esm is a few hundred header reads.
struct TopGroup
{
    std::vector<std::string> masters;
    std::vector<std::uint8_t> bytes; // the group, header included
};

std::optional<TopGroup> ReadTopGroup(const std::filesystem::path &path, std::string_view type)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::array<std::uint8_t, kPluginHeaderSize> raw{};
    const auto next = [&]() -> std::optional<PluginHeader> {
        if (!in.read(reinterpret_cast<char *>(raw.data()), raw.size()))
            return std::nullopt;
        return ReadPluginHeader(raw);
    };
    const auto header = next();
    if (!header || !header->Is("TES4"))
        return std::nullopt;
    std::vector<std::uint8_t> data(header->size);
    if (!in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size())))
        return std::nullopt;
    TopGroup group{PluginMasters(data), {}};
    while (const auto h = next())
    {
        if (!h->IsGroup() || h->size < kPluginHeaderSize)
            return std::nullopt;
        const auto rest = static_cast<std::streamoff>(h->size - kPluginHeaderSize);
        if (!h->Labelled(type))
        {
            in.seekg(rest, std::ios::cur);
            continue;
        }
        group.bytes.assign(raw.begin(), raw.end());
        group.bytes.resize(h->size);
        if (!in.read(reinterpret_cast<char *>(group.bytes.data() + kPluginHeaderSize), rest))
            return std::nullopt;
        return group;
    }
    return group; // no such group: nothing in it
}

// Where the game reads the plugin from: the Data folder, which Mod
// Organizer's virtual file system answers for inside the game's process.
std::filesystem::path PathOf(const RE::TESFile &file)
{
    const std::filesystem::path dir = file.path[0] != '\0' ? std::filesystem::path(file.path) : "Data";
    return dir / file.fileName;
}

// A form ID in the file's own numbering, in the load order's: the high
// byte names a master, or the file itself one past the last.
RE::FormID InLoadOrder(std::uint32_t id, const std::vector<std::string> &masters, const RE::TESFile &file)
{
    auto *data = RE::TESDataHandler::GetSingleton();
    const std::size_t index = id >> 24;
    const std::string name = index < masters.size() ? masters[index] : std::string(file.GetFilename());
    const auto *mod = data ? data->LookupModByName(name) : nullptr;
    if (!mod)
        return 0;
    return data->LookupFormID(id & (mod->IsLight() ? 0xFFFu : 0xFFFFFFu), name);
}

} // namespace

void FindToggles()
{
    const auto started = std::chrono::steady_clock::now();
    auto *data = RE::TESDataHandler::GetSingleton();
    if (!data)
        return;
    using Type = RE::MagicSystem::SpellType;

    // A toggle's shape: a spell or a power that leaves nothing lasting, with
    // a script effect. Those script effects, each by the plugin that last
    // defines it, and who casts it.
    std::unordered_map<const RE::EffectSetting *, std::vector<const RE::SpellItem *>> users;
    // The plugins by name, so they are read, and logged, in one order every run.
    const auto byName = [](const RE::TESFile *a, const RE::TESFile *b) { return a->GetFilename() < b->GetFilename(); };
    std::map<const RE::TESFile *, std::vector<const RE::EffectSetting *>, decltype(byName)> byFile(byName);
    for (const auto *spell : data->GetFormArray<RE::SpellItem>())
    {
        if (!spell)
            continue;
        const Type type = spell->GetSpellType();
        if ((type != Type::kSpell && type != Type::kPower && type != Type::kLesserPower) || LastingEffect(spell))
            continue;
        for (const RE::Effect *effect : ResolvedEffects(*spell))
        {
            const auto *base = effect->baseEffect;
            if (!base->HasArchetype(RE::EffectSetting::Archetype::kScript))
                continue;
            auto &casters = users[base];
            if (casters.empty())
                if (const auto *file = base->GetFile())
                    byFile[file].push_back(base);
            casters.push_back(spell);
        }
    }

    std::size_t compressed = 0;
    std::size_t unreadable = 0;
    for (const auto &[file, effects] : byFile)
    {
        const auto group = ReadTopGroup(PathOf(*file), "MGEF");
        if (!group)
        {
            ++unreadable;
            log::sensors.warn("toggles: {} could not be read from {}", file->GetFilename(), PathOf(*file).string());
            continue;
        }
        for (const PluginRecord &record : RecordsIn(group->bytes))
        {
            const RE::FormID id = InLoadOrder(record.formID, group->masters, *file);
            const auto it =
                std::ranges::find_if(effects, [&](const auto *effect) { return effect->GetFormID() == id; });
            if (it == effects.end())
                continue;
            if (record.Compressed())
            {
                ++compressed;
                continue;
            }
            for (const auto &property : ScriptObjectProperties(Subrecord(record.data, "VMAD")))
            {
                const auto *ability =
                    RE::TESForm::LookupByID<RE::SpellItem>(InLoadOrder(property.formID, group->masters, *file));
                if (!ability || ability->GetSpellType() != Type::kAbility)
                    continue;
                for (const auto *spell : users[*it])
                    if (g_links.emplace(spell->GetFormID(), ability->GetFormID()).second)
                        log::sensors.info("toggle: {} ({:08X}) turns on {} ({:08X}), by {}'s {} in {}", NameOf(spell),
                                          spell->GetFormID(), NameOf(ability), ability->GetFormID(), property.script,
                                          property.property, file->GetFilename());
                break;
            }
        }
    }
    log::sensors.info("toggles: {} found in {} plugin(s) read, {:.0f} ms; {} compressed effect record(s) skipped, "
                      "{} plugin(s) unreadable",
                      g_links.size(), byFile.size() - unreadable,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                      compressed, unreadable);
}

const RE::SpellItem *ToggledAbility(const RE::SpellItem *source)
{
    const auto it = source ? g_links.find(source->GetFormID()) : g_links.end();
    return it != g_links.end() ? RE::TESForm::LookupByID<RE::SpellItem>(it->second) : nullptr;
}

} // namespace ft::game
