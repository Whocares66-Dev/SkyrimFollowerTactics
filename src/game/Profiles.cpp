#include "game/Profiles.h"

#include "game/Util.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace ft::game
{
namespace
{

// Relative to the game's working directory, which is the game folder --
// where every SKSE plugin keeps its files. Under Mod Organizer the writes
// land in its overwrite folder, as they do for any mod that writes here.
const std::filesystem::path kFolder = "Data/SKSE/Plugins/FollowerTactics/followers";

std::filesystem::path FileFor(const Identity &who)
{
    return kFolder / (who.key + ".json");
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
        logger::warn("tactics: {} has no record in any plugin -- tactics file keyed by reference id, "
                     "which another save will not share",
                     Describe(actor));
    }
    return who;
}

std::optional<ft::Profile> LoadProfile(const Identity &who)
{
    const auto path = FileFor(who);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        logger::error("tactics: {}: cannot open {}", who.name, path.string());
        return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();

    auto read = ft::ReadProfile(text.str(), GameFormCodec());
    for (const auto &warning : read.warnings)
        logger::warn("tactics: {}: {}: {}", who.name, path.filename().string(), warning);
    if (!read.profile)
    {
        logger::error("tactics: {}: {} is not a tactics file -- starting with none; the next edit "
                      "overwrites it",
                      who.name, path.string());
        return std::nullopt;
    }
    logger::info("tactics: {}: read {} rule(s) from {}, {}", who.name, read.profile->rules.rules.size(), path.string(),
                 read.profile->enabled ? "on" : "off");
    return std::move(read.profile);
}

void SaveProfile(const Identity &who, const ft::Profile &profile)
{
    const auto path = FileFor(who);
    std::error_code ec;
    std::filesystem::create_directories(kFolder, ec);
    if (ec)
    {
        logger::error("tactics: {}: cannot create {}: {}", who.name, kFolder.string(), ec.message());
        return;
    }

    // Never into the file itself. The new text goes to a file beside it,
    // is confirmed there -- written without error, and the size on disk
    // is the size of the text -- and only then swapped into place, one
    // rename that replaces the old file in the same step (MoveFileEx with
    // REPLACE_EXISTING, atomic on NTFS). A crash at any point leaves
    // either the last good file or the new one, never half of either.
    const std::string text = ft::WriteProfile(profile, GameFormCodec());
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            logger::error("tactics: {}: cannot write {}", who.name, temp);
            return;
        }
        out << text;
        out.flush();
        if (!out.good())
        {
            logger::error("tactics: {}: writing {} failed -- the last good file is kept", who.name, temp);
            out.close();
            std::filesystem::remove(temp, ec);
            return;
        }
    }
    const auto written = std::filesystem::file_size(temp, ec);
    if (ec || written != text.size())
    {
        logger::error("tactics: {}: {} holds {} of {} bytes -- the last good file is kept", who.name, temp,
                      ec ? 0 : written, text.size());
        std::filesystem::remove(temp, ec);
        return;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        // A virtual filesystem may refuse to move over a file it serves
        // from elsewhere; copying into place is the fallback.
        std::filesystem::copy_file(temp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temp);
        if (ec)
        {
            logger::error("tactics: {}: cannot replace {}: {}", who.name, path.string(), ec.message());
            return;
        }
    }
    logger::info("tactics: {}: wrote {} rule(s) to {}", who.name, profile.rules.rules.size(), path.string());
}

} // namespace ft::game
