// A sheet number written out as the calculation that made it (core/Breakdown.h).
#include "core/Breakdown.h"

#include <catch2/catch_test_macros.hpp>

using namespace ft;

TEST_CASE("the lines apply in order, so a multiply after an add is not an add after a multiply", "[breakdown]")
{
    Breakdown b;
    Start(b, "Base", 100.0);
    Add(b, "Effect A", 50.0);
    Multiply(b, "Perk B", 2.0);
    REQUIRE(Evaluate(b) == 300.0);

    Breakdown c;
    Start(c, "Base", 100.0);
    Multiply(c, "Perk B", 2.0);
    Add(c, "Effect A", 50.0);
    REQUIRE(Evaluate(c) == 250.0);
}

TEST_CASE("a line that did not apply is listed but not counted", "[breakdown]")
{
    Breakdown b;
    Start(b, "Base", 10.0);
    BreakdownLine &line = Multiply(b, "Against undead", 1.5);
    line.applied = false;
    line.why = "no target";
    REQUIRE(Evaluate(b) == 10.0);
    const std::string text = ToText(b);
    REQUIRE(text.find("Against undead (no target)") != std::string::npos);
    REQUIRE(text.find("x 1.5") != std::string::npos);
}

TEST_CASE("Other carries what the lines do not explain, and only when it would show", "[breakdown]")
{
    Breakdown b;
    Start(b, "Base", 408.0);
    Add(b, "Ring", 50.0);
    b.total = 470.0;
    Close(b);
    REQUIRE(b.lines.size() == 3);
    REQUIRE(b.lines.back().label == "Other");
    REQUIRE(b.lines.back().amount == 12.0);
    REQUIRE(Evaluate(b) == 470.0);

    // A gap below half a printed unit is a rounding, not a line.
    Breakdown c;
    Start(c, "Base", 100.0);
    c.total = 100.4;
    Close(c);
    REQUIRE(c.lines.size() == 1);

    // With two decimals printed, the same gap shows.
    Breakdown d;
    d.decimals = 2;
    Start(d, "Base", 100.0);
    d.total = 100.4;
    Close(d);
    REQUIRE(d.lines.size() == 2);
}

TEST_CASE("amounts print as the reader adds them: signed adds, bare starts, factors without zeros", "[breakdown]")
{
    Breakdown b;
    b.unit = "%";
    const BreakdownLine start{Op::Start, "Base", 3.0};
    const BreakdownLine plus{Op::Add, "Ring", 50.0};
    const BreakdownLine minus{Op::Add, "Curse", -17.0};
    const BreakdownLine zero{Op::Add, "Nothing", -0.0};
    const BreakdownLine times{Op::Multiply, "Armsman", 1.6};
    const BreakdownLine twice{Op::Multiply, "Skald", 2.0};
    const BreakdownLine quarter{Op::Multiply, "Fortify", 1.25};
    REQUIRE(AmountText(b, start) == "3%");
    REQUIRE(AmountText(b, plus) == "+50%");
    REQUIRE(AmountText(b, minus) == "-17%");
    REQUIRE(AmountText(b, zero) == "0%");
    REQUIRE(AmountText(b, times) == "x 1.6");
    REQUIRE(AmountText(b, twice) == "x 2");
    REQUIRE(AmountText(b, quarter) == "x 1.25");
    b.decimals = 2;
    REQUIRE(AmountText(b, start) == "3.00%");
    REQUIRE(AmountText(b, plus) == "+50.00%");
}

TEST_CASE("the text lays the amounts in a column, a rule, then the total", "[breakdown]")
{
    Breakdown b;
    Start(b, "Base", 100.0);
    Add(b, "Effect A", 50.0);
    Multiply(b, "Perk B", 2.0);
    b.total = 300.0;
    Close(b);
    const std::string text = ToText(b);
    // The widest label, two spaces, the widest amount: 8 + 2 + 3.
    const std::string expected = "Base      100\n"
                                 "Effect A  +50\n"
                                 "Perk B    x 2\n"
                                 "-------------\n"
                                 "Total     300\n";
    REQUIRE(text == expected);
}

TEST_CASE("a line's detail is indented beneath it and stays out of the arithmetic", "[breakdown]")
{
    Breakdown b;
    Start(b, "Base", 10.0);
    BreakdownLine &dial = Multiply(b, "Fortify One-handed", 1.25);
    dial.detail.push_back({Op::Add, "Gauntlets", 15.0});
    dial.detail.push_back({Op::Add, "Potion", 10.0});
    b.total = 12.5;
    b.decimals = 1;
    Close(b);
    REQUIRE(Evaluate(b) == 12.5);
    const std::string text = ToText(b);
    REQUIRE(text.find("  Gauntlets") != std::string::npos);
    REQUIRE(text.find("Other") == std::string::npos);
}
