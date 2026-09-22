#include "game/I18n.h"

#include "core/Files.h"
#include "core/I18n.h"
#include "core/LogSettings.h"
#include "game/Log.h"

#include <filesystem>

namespace ft::game
{

void LoadLanguage()
{
    std::vector<std::string> notes;
    std::string locale = ft::log::ParseIniSettings(ft::ReadText("Data/SKSE/Plugins/FollowerTactics.ini"), notes).language;
    if (locale.empty())
    {
        const auto *setting = RE::GetINISetting("sLanguage:General");
        const char *language = setting ? setting->GetString() : nullptr;
        locale = ft::i18n::LocaleFor(language ? language : "");
    }
    if (locale == "en-US")
    {
        log::ui.info("panel in English");
        return;
    }

    const std::filesystem::path file =
        std::filesystem::path{"Data/SKSE/Plugins/FollowerTactics/Translations"} / (locale + ".json");
    const std::string text = ft::ReadText(file);
    if (text.empty())
    {
        log::ui.info("no catalog for {} ({}) -- panel in English", locale, ft::FileLabel(file));
        return;
    }
    notes.clear();
    auto catalog = ft::i18n::ParseCatalog(text, notes);
    for (const std::string &note : notes)
        log::ui.warn("{}: {}", ft::FileLabel(file), note);
    log::ui.info("panel in {}: {} lines translated", locale, catalog.size());
    ft::i18n::Use(std::move(catalog));
}

} // namespace ft::game
