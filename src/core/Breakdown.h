// A number on a sheet, written out as the calculation that made it: the
// starting value, then each thing that added to it or multiplied it, in
// the order the engine applied them, a rule, and the total. The same shape
// serves a pool's maximum (a base and the enchantments on it), an armour
// rating (the pieces, the effects, the hidden bonus), a weapon's damage
// (the record, tempering, the skill curve, each perk entry in priority
// order) and a spell's cost.
//
// Only what applies is in it: a perk entry whose conditions fail, or one
// that multiplies by one, is not a line. The lines are ours; the total is
// the engine's. When the lines do not make the total, the renderer adds an
// Other line for the gap, so a rounding or a formula we have wrong is seen
// rather than hidden. Nothing is logged: the tooltip is the report.
//
// No RE:: types: this is the arithmetic and the wording, testable without
// the game. The panel draws it (game/UI.cpp); the sensors fill it.
#pragma once

#include <string>
#include <vector>

namespace ft
{

enum class Op
{
    Start,    // the value the calculation begins from: "Base: 408"
    Add,      // a signed amount added: "+50", "-17%"
    Multiply, // a factor: "x 1.6"
};

struct BreakdownLine
{
    Op op{Op::Add};
    std::string label;
    double amount{0.0};
    // The line opened out: the sources behind a value a perk read (the
    // gauntlets and the potion behind a Fortify dial). Rendered indented
    // beneath the line; not part of the arithmetic, which the line's own
    // amount already carries.
    std::vector<BreakdownLine> detail;
};

struct Breakdown
{
    std::vector<BreakdownLine> lines;
    double total{0.0};
    int decimals{0};  // how a Start, Add or Other amount is printed
    std::string unit; // "%", " s"; appended to every amount but a factor
    std::string totalLabel{"Total"};

    [[nodiscard]] bool empty() const noexcept
    {
        return lines.empty();
    }
};

// The amounts applied in order: Start sets, Add adds, Multiply multiplies.
// With no Start the calculation begins at zero.
[[nodiscard]] double Evaluate(const Breakdown &b);

// Add the Other line where the lines do not make the total: the gap,
// when it is half a printed unit or more. Called once the lines are
// complete and the total set.
void Close(Breakdown &b);

// One amount as printed: "408", "+50", "-17%", "x 1.6". A factor is
// printed with up to two decimals and no trailing zeros.
[[nodiscard]] std::string AmountText(const Breakdown &b, const BreakdownLine &line);
[[nodiscard]] std::string TotalText(const Breakdown &b);

// The whole as text, one line each, labels and amounts in two aligned
// columns, a rule, then the total -- for the log and the tests; the panel
// draws its own.
[[nodiscard]] std::string ToText(const Breakdown &b);

// Helpers for the builders.
BreakdownLine &Start(Breakdown &b, std::string label, double amount);
BreakdownLine &Add(Breakdown &b, std::string label, double amount);
BreakdownLine &Multiply(Breakdown &b, std::string label, double factor);

} // namespace ft
