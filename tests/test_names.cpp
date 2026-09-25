#include "core/Names.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ft;

TEST_CASE("names sort as a reader looks them up", "[names]")
{
    // Case aside: "iron" beside "Iron", not after "Zombie".
    REQUIRE(NameBefore("iron dagger", "Zombie"));
    REQUIRE(NameBefore("Apple", "banana"));
    // Equal but for case: one fixed place each, capitals first.
    REQUIRE(NameBefore("Iron", "iron"));
    REQUIRE_FALSE(NameBefore("iron", "Iron"));
    REQUIRE_FALSE(NameBefore("Iron", "Iron"));
    // A prefix first.
    REQUIRE(NameBefore("Bound", "Bound weapon"));
    // Another script after the Latin, by its UTF-8 bytes: "poison" in
    // Chinese (E6 AF 92 ...) before "soul" (E7 81 B5 ...). Escaped, so the
    // file's own encoding cannot change what is compared.
    const std::string_view poison = "\xE6\xAF\x92\xE8\x8D\xAF";
    const std::string_view soul = "\xE7\x81\xB5\xE9\xAD\x82";
    REQUIRE(NameBefore("Zombie", soul));
    REQUIRE(NameBefore(poison, soul));
    REQUIRE_FALSE(NameBefore(soul, poison));
}

TEST_CASE("sorting by name keeps one name's items in the order they came", "[names]")
{
    std::vector<std::pair<std::string, int>> items{{"poison", 1}, {"Bound", 2}, {"Charge needed", 3}, {"Bound", 4}};
    SortByName(items, [](const auto &item) -> std::string_view { return item.first; });
    REQUIRE(items[0] == std::pair<std::string, int>{"Bound", 2});
    REQUIRE(items[1] == std::pair<std::string, int>{"Bound", 4});
    REQUIRE(items[2].first == "Charge needed");
    REQUIRE(items[3].first == "poison");
}
