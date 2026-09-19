#pragma once
// One form's copies in a bag, as a plain description, and the decisions
// over it (dev/GAME_MODEL.md, part one). The game side reads the actor's
// entry for the form into a view -- each extra list a row, the engine's
// own verdicts copied in -- and every question that used to be asked of
// the lists is asked here instead, where it is tested against a bag built
// by hand. The game keeps only what reads a list and what calls the
// engine. No Skyrim.
//
// Why: on 2026-09-12 eleven bugs were found in play, and each was either a
// fact about the engine not yet measured or a decision made from facts
// already readable -- which list to hand the engine, whether a click moves
// a copy between hands, a holdable's count, whether a row stands apart --
// made beside an RE:: call, where nothing ran it but the game.

#include "Loadout.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace ft
{

// One extra list of the entry: a copy, or a stack of copies, with the
// facts the decisions read.
struct BagRow
{
    // The parts of the list only the player's own hand changes.
    ItemVariant variant;
    // A row of its own on the Inventory tab rather than one of the plain
    // stack: the engine's own answer, IsInventoryStackable (dev/UNIQUE.md).
    // Tempering, a charge, a poison, a name, an enchantment and a soul keep
    // a copy apart; ownership, an id, worn marks and the count do not. A
    // poisoned copy of the plain variant is a row of its own.
    bool ownRow{false};
    // The list's marks as they are: Worn, and WornLeft. What they mean is
    // the object's kind's to say (WornIn).
    bool wornRight{false};
    bool wornLeft{false};
    // Copies on the list.
    int count{1};
    // The list itself, opaque: the game maps a row back to it, and the
    // panel's click names a row by it. Never dereferenced here.
    const void *token{nullptr};
};

struct BagView
{
    // Every copy carried, listed or not.
    int total{0};
    // A weapon's marks are by hand: Worn is the right, WornLeft the left. A
    // shield, a torch and armour carry the one Worn mark whichever way
    // they are held, so for anything but a weapon the left hand or no hand
    // is any mark, and the right hand is none.
    bool weapon{false};
    // In the entry's order, which is the order the engine's own equip
    // walks.
    std::vector<BagRow> rows;

    // Copies on some list.
    [[nodiscard]] int Listed() const noexcept;
    // Copies on no list at all, which only a null list can reach.
    [[nodiscard]] int Listless() const noexcept
    {
        return total > Listed() ? total - Listed() : 0;
    }
    [[nodiscard]] bool HasListlessCopy() const noexcept
    {
        return Listless() > 0;
    }
    // The row with this token, or null: a token from an earlier scan made
    // safe to use, since the copy may have left and the address be
    // another's or nobody's.
    [[nodiscard]] const BagRow *RowOfToken(const void *token) const noexcept;
    // The row's place, for the game to find its list.
    [[nodiscard]] std::size_t IndexOf(const BagRow *row) const noexcept;
};

// Is the copy on this row worn in those hands, by the marks alone: the
// right is Worn, the left WornLeft, Both the right (a two-hander sits
// there), None either.
[[nodiscard]] bool ListWorn(const BagRow &row, Hand hands) noexcept;
// The same asked of a copy of the view's object: by its kind.
[[nodiscard]] bool WornIn(const BagView &view, const BagRow &row, Hand hands) noexcept;

// How many copies of the variant the bag holds: its rows summed, the
// listless remainder counting as plain; of the form, with no variant.
[[nodiscard]] int CountVariant(const BagView &view, const std::optional<ItemVariant> &variant) noexcept;

// A row worn in those hands (None: worn at all), by kind; or one worn
// nowhere, by the marks; null for none.
[[nodiscard]] const BagRow *WornRow(const BagView &view, Hand hands) noexcept;
[[nodiscard]] const BagRow *UnwornRow(const BagView &view) noexcept;
// A row of the variant, worn in those hands or not worn at all. Of the
// unworn, one of the plain stack before a row of its own: the poisoned
// dagger is the plain variant, but a click on the plain stack or a pin on
// it means a clean one while any is there.
[[nodiscard]] const BagRow *WornVariantRow(const BagView &view, const ItemVariant &variant, Hand hands) noexcept;
[[nodiscard]] const BagRow *UnwornVariantRow(const BagView &view, const ItemVariant &variant) noexcept;
// A row of the plain stack -- not a row of its own -- worn in those hands
// or not worn at all.
[[nodiscard]] const BagRow *WornStackRow(const BagView &view, Hand hands) noexcept;
[[nodiscard]] const BagRow *UnwornStackRow(const BagView &view) noexcept;
// The form's rows as the Inventory tab splits them: one per row of its
// own, in the entry's order, each the row's copies; then the plain stack
// once, when any copy is in it -- the folded lists and the listless
// remainder together. What the tab draws and what the whole-form
// questions take (RowsOf, the variants alone).
struct DisplayRow
{
    std::optional<std::size_t> row; // the view's row, or none for the plain stack
    int count{0};
    ItemVariant variant;
};
[[nodiscard]] std::vector<DisplayRow> DisplayRows(const BagView &view);
[[nodiscard]] std::vector<ItemVariant> RowsOf(const BagView &view);
// The bag as the engine's own equip sees it, for EnginePick: the plain
// stack first when any copy is in it, then each list in order.
[[nodiscard]] std::vector<VariantInBag> CopiesForEngine(const BagView &view);

// ---- Which copy an equip takes.
//
// The caller's kind of question. The panel is exact: the row clicked --
// its list, or the plain stack's own copies -- and a copy of that row
// already in the hand is the incumbent, nothing done; one in the other
// hand comes across. A rule or the watchdog means the variant, and has
// the leeway: a copy of it already in the hand is the incumbent; else one
// of the stack before a row of its own, a listless copy by null, and the
// only copy in the other hand brought across. The engine leaves a worn
// list where it is, whichever hand is asked (measured 2026-09-12), so a
// copy that comes across is taken off there first.
enum class EquipAsk : std::uint8_t
{
    Row,    // the row clicked, by its token
    Stack,  // the plain stack
    Variant // the variant named, or the form with none
};

struct EquipPlan
{
    // Nothing to do: the incumbent is in the hand, or there is no copy to
    // give.
    bool proceed{false};
    // The copy is worn in the other hand: take it off there first.
    bool unequipOther{false};
    // The row to hand the engine, or none: with `proceed`, a null list --
    // a listless copy, or the form left to the engine's own choice.
    std::optional<std::size_t> row;
};

// `hands` is the hand asked for. `formWornInHands` is the engine's own
// answer to whether the form is in those hands, read off the actor rather
// than the lists, and is consulted only when no variant is named: the
// question is then of the form, whichever copy.
[[nodiscard]] EquipPlan PlanEquip(const BagView &view, EquipAsk ask, const std::optional<ItemVariant> &variant,
                                  Hand hands, const void *rowToken, bool formWornInHands);

} // namespace ft
