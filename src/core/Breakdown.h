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
// the engine's. Amounts print with the decimals they have, so every step the
// formula takes is on the page, a rounding included; when the lines still
// do not make the total, an Other line carries the gap, and Other means the
// formula as written misses something. Nothing is logged: the tooltip is
// the report.
//
// No RE:: types: this is the arithmetic and the wording, testable without
// the game. The panel draws it (game/UI.cpp); the sensors fill it.
#pragma once

#include "core/I18n.h"

#include <optional>
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
    // The line's own unit, where it differs from the breakdown's: the
    // sources behind a value a perk read are in the value's units.
    std::optional<std::string> unit;
    // The amount as written, where the number alone would hide its shape:
    // "x (1.35 + 0.20)" for a factor the engine builds as a sum. The
    // arithmetic still uses `amount`.
    std::optional<std::string> amountText;
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
    int decimals{0};  // the fewest decimals an amount or the total prints with; more where it has them, two at most
    std::string unit; // "%", " s"; appended to every amount but a factor
    std::string totalLabel{i18n::Tr("Total")};

    [[nodiscard]] bool empty() const noexcept
    {
        return lines.empty();
    }
};

// The amounts applied in order: Start sets, Add adds, Multiply multiplies.
// With no Start the calculation begins at zero.
[[nodiscard]] double Evaluate(const Breakdown &b);

// Whether an amount prints as anything but zero: half of the last digit
// printed or more, the second decimal at most. Below that it is
// floating-point noise, not a line.
[[nodiscard]] bool Visible(const Breakdown &b, double amount);

// Add the Other line where the lines do not make the total: the gap, when
// it is Visible, which is something the formula as written does not take
// into account. Called once the lines are complete and the total set.
void Close(Breakdown &b);

// One amount as printed: "408", "+50", "+4.16", "-17%", "x 1.6". Every
// amount, a factor included, prints with up to two decimals and no trailing
// zeros past the breakdown's own decimals.
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
