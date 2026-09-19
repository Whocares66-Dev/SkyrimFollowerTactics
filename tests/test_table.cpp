// A list's filter and its order, without the panel.

#include <catch2/catch_test_macros.hpp>

#include "core/Table.h"

#include <string>
#include <vector>

using namespace ft;

namespace
{

struct Row
{
    std::string name;
    double weight{0.0};
    std::string type;
};

enum class Column
{
    Name,
    Weight,
    Type
};

int ByColumn(const Row &a, const Row &b, Column column)
{
    switch (column)
    {
    case Column::Weight:
        return Compare(a.weight, b.weight);
    case Column::Type:
        return a.type.compare(b.type);
    default:
        return a.name.compare(b.name);
    }
}

std::vector<std::string> Names(const std::vector<const Row *> &rows)
{
    std::vector<std::string> out;
    for (const Row *row : rows)
        out.push_back(row->name);
    return out;
}

} // namespace

TEST_CASE("the filter's text is found in any cell, whatever the case; none is found everywhere", "[table]")
{
    REQUIRE(ContainsNoCase("Iron Dagger", "dagger"));
    REQUIRE(ContainsNoCase("Iron Dagger", "IRON"));
    REQUIRE(ContainsNoCase("Iron Dagger", ""));
    REQUIRE_FALSE(ContainsNoCase("Iron Dagger", "steel"));
    REQUIRE_FALSE(ContainsNoCase("", "a"));

    const std::vector<std::string> cells{"Steel Sword", "One-handed", "9.0", "45"};
    REQUIRE(AnyContains(cells, "one-HANDED"));
    REQUIRE(AnyContains(cells, "9.0"));
    REQUIRE(AnyContains(cells, ""));
    REQUIRE_FALSE(AnyContains(cells, "dagger"));
    // No cells hold nothing, not even the empty filter.
    REQUIRE_FALSE(AnyContains({}, "a"));
    REQUIRE_FALSE(AnyContains({}, ""));
}

TEST_CASE("rows sort by the column asked, ties by name ascending either way, equal rows in the order they came",
          "[table]")
{
    const std::vector<Row> rows{{"Dagger", 2.0, "One-handed"},
                                {"Bow", 5.0, "Bow"},
                                {"Axe", 5.0, "One-handed"},
                                {"Sword", 5.0, "One-handed"},
                                {"Arrow", 0.0, "Ammo"}};
    const auto all = [&] {
        std::vector<const Row *> out;
        for (const Row &row : rows)
            out.push_back(&row);
        return out;
    };

    auto byWeight = all();
    SortRows(byWeight, ByColumn, Column::Weight, true);
    REQUIRE(Names(byWeight) == std::vector<std::string>{"Arrow", "Dagger", "Axe", "Bow", "Sword"});
    // Descending: the weights reverse, the ties among the fives do not.
    auto heaviest = all();
    SortRows(heaviest, ByColumn, Column::Weight, false);
    REQUIRE(Names(heaviest) == std::vector<std::string>{"Axe", "Bow", "Sword", "Dagger", "Arrow"});

    auto byType = all();
    SortRows(byType, ByColumn, Column::Type, true);
    REQUIRE(Names(byType) == std::vector<std::string>{"Arrow", "Bow", "Axe", "Dagger", "Sword"});

    auto byName = all();
    SortRows(byName, ByColumn, Column::Name, false);
    REQUIRE(Names(byName) == std::vector<std::string>{"Sword", "Dagger", "Bow", "Axe", "Arrow"});

    // Two rows equal in every way keep the order they came in, both ways.
    const std::vector<Row> twins{{"Ring", 0.25, "Jewelry"}, {"Ring", 0.25, "Jewelry"}};
    std::vector<const Row *> same{&twins[0], &twins[1]};
    SortRows(same, ByColumn, Column::Weight, true);
    REQUIRE(same == std::vector<const Row *>{&twins[0], &twins[1]});
    SortRows(same, ByColumn, Column::Weight, false);
    REQUIRE(same == std::vector<const Row *>{&twins[0], &twins[1]});

    REQUIRE(Compare(1.0, 2.0) == -1);
    REQUIRE(Compare(2.0, 1.0) == 1);
    REQUIRE(Compare(2.0, 2.0) == 0);
}
