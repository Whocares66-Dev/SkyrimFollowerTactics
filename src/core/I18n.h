#pragma once
// The panel's words in the player's language (dev/I18N.md). English is the
// source and the key, as gettext has it: `Tr("Rules")` is "Rules" until a
// catalog says otherwise, so a missing file, a missing entry or an empty
// one is English and never a blank or a key. A catalog is a flat JSON
// object from the English to the translation, one per language, read
// once before the panel is drawn (game/I18n.cpp). No Skyrim.
//
// What is marked is what tools/i18n.py extracts: `Tr("...")`,
// `TrFormat("...", ...)`, and `N_("...")` for a literal kept in a table
// and translated where it is drawn (`Tr(DisplayName(kind))`). Names the
// game gives -- an item's, a spell's, a follower's -- come translated
// already and are not marked; nor is the log, which stays English for a
// bug report.

#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Marks a literal for extraction and leaves it as it is: a table's entry,
// translated where it is drawn.
#define N_(text) text

namespace ft::i18n
{

struct Hash
{
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

using Catalog = std::unordered_map<std::string, std::string, Hash, std::equal_to<>>;

// A catalog's text. An entry whose value is empty is left out, so an
// untranslated line reads in English; one whose value is not a string is
// said in `notes` and left out. Text that is not a JSON object is said
// and gives nothing.
[[nodiscard]] Catalog ParseCatalog(std::string_view json, std::vector<std::string> &notes);

// The catalog in force from now on. Before the panel is first drawn, or
// between frames: what `Tr` handed out points into the catalog it read.
void Use(Catalog catalog);

// The line in the language in force; the English where the catalog has
// none. The pointer lives as long as the catalog, or is the argument.
[[nodiscard]] const char *Tr(const char *english);
[[nodiscard]] std::string_view Tr(std::string_view english);

// Formatted after translation, with std::format's fields, so a translation
// may reorder them: "{0} of {1}" may read "{1}中的{0}". A translation that
// does not format with these arguments is passed over for the English,
// whose fields are checked when it is compiled.
template <typename... Args> [[nodiscard]] std::string TrFormat(std::format_string<Args...> english, const Args &...args)
{
    const std::string_view translated = Tr(english.get());
    if (translated.data() == english.get().data())
    {
        return std::vformat(english.get(), std::make_format_args(args...));
    }
    try
    {
        return std::vformat(translated, std::make_format_args(args...));
    }
    catch (const std::format_error &)
    {
        return std::vformat(english.get(), std::make_format_args(args...));
    }
}

// The catalog's name for the game's language (the ini's sLanguage:
// ENGLISH, CHINESE ...), any case: "zh-CN" for CHINESE, the game's one
// Chinese, which the Chinese community's translations are simplified for.
// A name not known here is "en-US".
[[nodiscard]] std::string LocaleFor(std::string_view gameLanguage);

} // namespace ft::i18n
