// The panel's words in another language: a catalog's text, a line looked
// up in it, and a formatted one. No Skyrim.

#include <catch2/catch_test_macros.hpp>

#include "core/I18n.h"

#include <string>
#include <vector>

using namespace ft::i18n;

namespace
{

// Each test's catalog, put back to none after, since the one in force is
// the process's.
struct Using
{
    explicit Using(std::string_view json)
    {
        std::vector<std::string> notes;
        Use(ParseCatalog(json, notes));
    }
    ~Using()
    {
        Use({});
    }
    Using(const Using &) = delete;
    Using &operator=(const Using &) = delete;
};

} // namespace

TEST_CASE("a line with no catalog, or none in it, is the English", "[i18n]")
{
    REQUIRE(std::string(Tr("Rules")) == "Rules");
    const Using zh(R"({"Rules": "规则", "Empty": ""})");
    REQUIRE(std::string(Tr("Rules")) == "规则");
    REQUIRE(std::string(Tr(std::string_view{"Rules"})) == "规则");
    REQUIRE(std::string(Tr("Pins")) == "Pins");
    // An empty entry is an untranslated one, not a blank.
    REQUIRE(std::string(Tr("Empty")) == "Empty");
}

TEST_CASE("the English pointer is handed back where there is no entry", "[i18n]")
{
    const char *english = "Not in it";
    REQUIRE(Tr(english) == english);
}

TEST_CASE("a catalog that is not one is said, and gives nothing", "[i18n]")
{
    std::vector<std::string> notes;
    REQUIRE(ParseCatalog("{ not json", notes).empty());
    REQUIRE(notes.size() == 1);
    notes.clear();
    REQUIRE(ParseCatalog("[\"a\"]", notes).empty());
    REQUIRE(notes.size() == 1);
    notes.clear();
    const Catalog c = ParseCatalog(R"({"A": 1, "B": "b"})", notes);
    REQUIRE(c.size() == 1);
    REQUIRE(notes.size() == 1);
}

TEST_CASE("a formatted line may reorder its fields", "[i18n]")
{
    REQUIRE(TrFormat("{} of {}", 2, 5) == "2 of 5");
    const Using zh(R"({"{} of {}": "{1} 中的 {0}", "{} left": "剩余 {} {}"})");
    REQUIRE(TrFormat("{} of {}", 2, 5) == "5 中的 2");
    // A translation that does not format with the arguments is passed over.
    REQUIRE(TrFormat("{} left", 3) == "3 left");
}

TEST_CASE("the game's language names its catalog", "[i18n]")
{
    REQUIRE(LocaleFor("CHINESE") == "zh-CN");
    REQUIRE(LocaleFor("chinese") == "zh-CN");
    REQUIRE(LocaleFor("ENGLISH") == "en-US");
    REQUIRE(LocaleFor("KLINGON") == "en-US");
    REQUIRE(LocaleFor("") == "en-US");
}
