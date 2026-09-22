#include "core/I18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace ft::i18n
{
namespace
{

// Replaced before the panel is first drawn and read by it after; the
// catalog is never changed in place.
Catalog g_catalog;

} // namespace

Catalog ParseCatalog(std::string_view json, std::vector<std::string> &notes)
{
    Catalog catalog;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (!parsed.is_object())
    {
        notes.emplace_back(parsed.is_discarded() ? "not JSON" : "not a JSON object");
        return catalog;
    }
    for (const auto &[english, translated] : parsed.items())
    {
        if (!translated.is_string())
        {
            notes.push_back("\"" + english + "\" is not a string");
            continue;
        }
        if (auto text = translated.get<std::string>(); !text.empty())
            catalog.emplace(english, std::move(text));
    }
    return catalog;
}

void Use(Catalog catalog)
{
    g_catalog = std::move(catalog);
}

const char *Tr(const char *english)
{
    const auto found = g_catalog.find(std::string_view{english});
    return found == g_catalog.end() ? english : found->second.c_str();
}

std::string_view Tr(std::string_view english)
{
    const auto found = g_catalog.find(english);
    return found == g_catalog.end() ? english : std::string_view{found->second};
}

std::string LocaleFor(std::string_view gameLanguage)
{
    // The languages the game ships, by the name its ini gives them.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kLocales{{
        {"ENGLISH", "en-US"},
        {"CHINESE", "zh-CN"},
        {"CZECH", "cs-CZ"},
        {"FRENCH", "fr-FR"},
        {"GERMAN", "de-DE"},
        {"ITALIAN", "it-IT"},
        {"JAPANESE", "ja-JP"},
        {"POLISH", "pl-PL"},
        {"RUSSIAN", "ru-RU"},
        {"SPANISH", "es-ES"},
    }};
    std::string upper(gameLanguage);
    std::ranges::transform(upper, upper.begin(), [](char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; });
    for (const auto &[name, locale] : kLocales)
    {
        if (name == upper)
            return std::string(locale);
    }
    return "en-US";
}

} // namespace ft::i18n
