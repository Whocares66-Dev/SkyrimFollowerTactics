#include "core/BagView.h"

namespace ft
{
namespace
{

template <class Pick> const BagRow *FirstRow(const BagView &view, Pick pick) noexcept
{
    for (const BagRow &row : view.rows)
        if (pick(row))
            return &row;
    return nullptr;
}

} // namespace

int BagView::Listed() const noexcept
{
    int listed = 0;
    for (const BagRow &row : rows)
        listed += row.count;
    return listed;
}

const BagRow *BagView::RowOfToken(const void *token) const noexcept
{
    if (!token)
        return nullptr;
    return FirstRow(*this, [token](const BagRow &row) { return row.token == token; });
}

std::size_t BagView::IndexOf(const BagRow *row) const noexcept
{
    return static_cast<std::size_t>(row - rows.data());
}

bool ListWorn(const BagRow &row, Hand hands) noexcept
{
    switch (hands)
    {
    case Hand::Left:
        return row.wornLeft;
    case Hand::Right:
    case Hand::Both:
        return row.wornRight;
    default:
        return row.wornLeft || row.wornRight;
    }
}

bool WornIn(const BagView &view, const BagRow &row, Hand hands) noexcept
{
    if (!view.weapon)
        return hands == Hand::Right ? false : ListWorn(row, Hand::None);
    return ListWorn(row, hands);
}

int CountVariant(const BagView &view, const std::optional<ItemVariant> &variant) noexcept
{
    if (view.total <= 0)
        return 0;
    if (!variant)
        return view.total;
    int named = 0;
    for (const BagRow &row : view.rows)
        if (SameVariant(row.variant, *variant))
            named += row.count;
    // The listless remainder is plain.
    if (variant->IsPlain())
        named += view.Listless();
    return named;
}

const BagRow *WornRow(const BagView &view, Hand hands) noexcept
{
    return FirstRow(view, [&](const BagRow &row) { return WornIn(view, row, hands); });
}

const BagRow *UnwornRow(const BagView &view) noexcept
{
    return FirstRow(view, [](const BagRow &row) { return !ListWorn(row, Hand::None); });
}

const BagRow *WornVariantRow(const BagView &view, const ItemVariant &variant, Hand hands) noexcept
{
    return FirstRow(view,
                    [&](const BagRow &row) { return WornIn(view, row, hands) && SameVariant(row.variant, variant); });
}

const BagRow *UnwornVariantRow(const BagView &view, const ItemVariant &variant) noexcept
{
    if (const BagRow *stack = FirstRow(view, [&](const BagRow &row) {
            return !ListWorn(row, Hand::None) && !row.ownRow && SameVariant(row.variant, variant);
        }))
        return stack;
    return FirstRow(view,
                    [&](const BagRow &row) { return !ListWorn(row, Hand::None) && SameVariant(row.variant, variant); });
}

const BagRow *WornStackRow(const BagView &view, Hand hands) noexcept
{
    return FirstRow(view, [&](const BagRow &row) { return WornIn(view, row, hands) && !row.ownRow; });
}

const BagRow *UnwornStackRow(const BagView &view) noexcept
{
    return FirstRow(view, [](const BagRow &row) { return !ListWorn(row, Hand::None) && !row.ownRow; });
}

std::vector<ItemVariant> RowsOf(const BagView &view)
{
    std::vector<ItemVariant> rows;
    if (view.total <= 0)
        return rows;
    int apart = 0;
    for (const BagRow &row : view.rows)
    {
        if (!row.ownRow)
            continue;
        apart += row.count;
        rows.push_back(row.variant);
    }
    if (view.total > apart)
        rows.emplace_back(); // the plain stack: the listless copies and the folded lists
    return rows;
}

std::vector<VariantInBag> CopiesForEngine(const BagView &view)
{
    std::vector<VariantInBag> copies;
    if (view.total <= 0)
        return copies;
    int apart = 0;
    for (const BagRow &row : view.rows)
        if (row.ownRow)
            apart += row.count;
    if (view.total > apart)
        copies.push_back({ItemVariant{}, true, false});
    for (const BagRow &row : view.rows)
        copies.push_back({row.variant, !row.ownRow, ListWorn(row, Hand::None)});
    return copies;
}

EquipPlan PlanEquip(const BagView &view, EquipAsk ask, const std::optional<ItemVariant> &variant, Hand hands,
                    const void *rowToken, bool formWornInHands)
{
    const bool oneHand = view.weapon && (hands == Hand::Left || hands == Hand::Right);
    const Hand other = Without(Hand::Both, hands);
    const auto planned = [&](const BagRow *row, bool unequipOther) {
        EquipPlan plan;
        plan.proceed = true;
        plan.unequipOther = unequipOther;
        if (row)
            plan.row = view.IndexOf(row);
        return plan;
    };

    switch (ask)
    {
    case EquipAsk::Row: {
        const BagRow *row = view.RowOfToken(rowToken);
        if (!row || WornIn(view, *row, hands))
            return {};
        if (ListWorn(*row, Hand::None))
        {
            if (!oneHand)
                return {};
            return planned(row, true);
        }
        return planned(row, false);
    }
    case EquipAsk::Stack: {
        if (WornStackRow(view, hands))
            return {};
        if (const BagRow *row = UnwornStackRow(view))
            return planned(row, false);
        if (view.HasListlessCopy())
            return planned(nullptr, false);
        if (!oneHand)
            return {};
        const BagRow *across = WornStackRow(view, other);
        if (!across)
            return {};
        return planned(across, true);
    }
    case EquipAsk::Variant:
    default: {
        const bool worn = variant ? WornVariantRow(view, *variant, hands) != nullptr : formWornInHands;
        if (worn)
            return {};
        const BagRow *row = variant ? UnwornVariantRow(view, *variant) : UnwornRow(view);
        bool listless = false;
        if (row && variant && variant->IsPlain() && row->ownRow && view.HasListlessCopy())
        {
            row = nullptr;
            listless = true;
        }
        if (!row && !listless && oneHand && CountVariant(view, variant) < 2)
        {
            const BagRow *across = variant ? WornVariantRow(view, *variant, other) : WornRow(view, other);
            if (across)
                return planned(across, true);
        }
        if (!row && !listless && variant && !(variant->IsPlain() && view.HasListlessCopy()))
            return {};
        return planned(row, false);
    }
    }
}

} // namespace ft
