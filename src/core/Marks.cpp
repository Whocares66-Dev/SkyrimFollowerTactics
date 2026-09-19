#include "core/Marks.h"

#include <algorithm>

namespace ft
{
namespace
{

bool WornWhere(const RowFacts &row, Hand hands) noexcept
{
    if (!row.item)
        return true; // a spell's entry: the pin's own hands say it all
    return hands == Hand::None ? row.worn
                               : (Overlap(hands, Hand::Left) && row.left) || (Overlap(hands, Hand::Right) && row.right);
}

} // namespace

bool BansRow(const Bans &bans, std::uint32_t form, const std::optional<ItemVariant> &variant) noexcept
{
    return std::any_of(bans.begin(), bans.end(),
                       [&](const Banned &b) { return b.form == form && SameVariant(b.variant, variant); });
}

const Pin *PinOfRow(const std::vector<Pin> &pins, const RowFacts &row, std::span<const RowFacts> rows) noexcept
{
    for (const Pin &pin : pins)
    {
        if (pin.thing.form != row.form || !SameVariant(pin.thing.variant, row.variant))
            continue;
        if (WornWhere(row, pin.hands))
            return &pin;
        // Not the incumbent: marked only while no row of the variant is.
        const bool anyWorn = std::any_of(rows.begin(), rows.end(), [&](const RowFacts &other) {
            return other.form == row.form && SameVariant(pin.thing.variant, other.variant) &&
                   WornWhere(other, pin.hands);
        });
        if (!anyWorn)
            return &pin;
    }
    return nullptr;
}

RowAside RowAsideOf(const std::vector<Pin> &pins, const Holdable &described, bool dualWield)
{
    RowAside out;
    out.aside = SetAside(pins, described);
    if (out.aside)
    {
        out.shadowing = Shadowing(pins, described);
        return out;
    }
    if (dualWield)
        return out;
    for (const Pin &pin : pins)
    {
        if ((pin.hands == Hand::Left || pin.hands == Hand::Right) && !SameThing(pin.thing, described) &&
            WouldDualWield(described, &pin.thing))
        {
            out.aside = true;
            out.cannotDualWield = true;
            break;
        }
    }
    return out;
}

} // namespace ft
