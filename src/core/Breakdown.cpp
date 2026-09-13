#include "core/Breakdown.h"

#include <cmath>
#include <cstdio>

namespace ft
{
namespace
{
std::string Number(double value, int decimals, bool signed_)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), signed_ ? "%+.*f" : "%.*f", decimals, value);
    // "-0" reads as a mistake; a zero has no sign.
    std::string text = buffer;
    bool zero = true;
    for (const char c : text)
        if (c >= '1' && c <= '9')
            zero = false;
    if (zero)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, 0.0);
        text = buffer;
    }
    return text;
}

// A factor with up to two decimals, the trailing zeros dropped: "1.6",
// "2", "0.85", "1.25".
std::string Factor(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos)
    {
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text;
}
} // namespace

double Evaluate(const Breakdown &b)
{
    double value = 0.0;
    for (const BreakdownLine &line : b.lines)
    {
        switch (line.op)
        {
        case Op::Start:
            value = line.amount;
            break;
        case Op::Add:
            value += line.amount;
            break;
        case Op::Multiply:
            value *= line.amount;
            break;
        }
    }
    return value;
}

void Close(Breakdown &b)
{
    const double gap = b.total - Evaluate(b);
    // Half of the last printed digit: a difference the reader could not
    // see is not a discrepancy.
    const double visible = 0.5 / std::pow(10.0, b.decimals);
    if (std::abs(gap) >= visible)
        Add(b, "Other", gap);
}

std::string AmountText(const Breakdown &b, const BreakdownLine &line)
{
    switch (line.op)
    {
    case Op::Start:
        return Number(line.amount, b.decimals, false) + b.unit;
    case Op::Add:
        return Number(line.amount, b.decimals, true) + b.unit;
    case Op::Multiply:
        return "x " + Factor(line.amount);
    }
    return {};
}

std::string TotalText(const Breakdown &b)
{
    return Number(b.total, b.decimals, false) + b.unit;
}

std::string ToText(const Breakdown &b)
{
    struct Row
    {
        std::string label;
        std::string amount;
    };
    std::vector<Row> rows;
    const auto gather = [&](const auto &self, const std::vector<BreakdownLine> &lines, int depth) -> void {
        for (const BreakdownLine &line : lines)
        {
            std::string label(static_cast<std::size_t>(depth) * 2, ' ');
            label += line.label;
            rows.push_back({std::move(label), AmountText(b, line)});
            self(self, line.detail, depth + 1);
        }
    };
    gather(gather, b.lines, 0);
    const Row total{b.totalLabel, TotalText(b)};

    std::size_t labelWidth = total.label.size();
    std::size_t amountWidth = total.amount.size();
    for (const Row &row : rows)
    {
        labelWidth = (std::max)(labelWidth, row.label.size());
        amountWidth = (std::max)(amountWidth, row.amount.size());
    }
    const std::size_t width = labelWidth + 2 + amountWidth;
    std::string out;
    const auto put = [&](const Row &row) {
        out += row.label;
        out += std::string(width - row.label.size() - row.amount.size(), ' ');
        out += row.amount;
        out += '\n';
    };
    for (const Row &row : rows)
        put(row);
    out += std::string(width, '-') + '\n';
    put(total);
    return out;
}

namespace
{
BreakdownLine &Line(Breakdown &b, Op op, std::string label, double amount)
{
    BreakdownLine line;
    line.op = op;
    line.label = std::move(label);
    line.amount = amount;
    b.lines.push_back(std::move(line));
    return b.lines.back();
}
} // namespace

BreakdownLine &Start(Breakdown &b, std::string label, double amount)
{
    return Line(b, Op::Start, std::move(label), amount);
}

BreakdownLine &Add(Breakdown &b, std::string label, double amount)
{
    return Line(b, Op::Add, std::move(label), amount);
}

BreakdownLine &Multiply(Breakdown &b, std::string label, double factor)
{
    return Line(b, Op::Multiply, std::move(label), factor);
}

} // namespace ft
