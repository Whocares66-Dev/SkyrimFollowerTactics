#pragma once
// What the panel's rows say about the books: which row a pin marks, why a
// row is set aside, whether a ban marks it. The panel reads its rows and
// the follower's records and asks here (game/Pins.cpp, MarkPins); the
// answers are tested against rows built by hand. No Skyrim.

#include "Loadout.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ft
{

// One row of the Inventory or Magic tab, as the marking reads it.
struct RowFacts
{
    std::uint32_t form{0};
    std::optional<ItemVariant> variant; // none for a spell's row
    bool item{true};                    // an item's row has hands; a spell's entry has the pin's own
    bool worn{false};
    bool left{false};
    bool right{false};
};

// A row is marked by a ban on its variant, or on every copy of its form.
[[nodiscard]] bool BansRow(const Bans &bans, std::uint32_t form, const std::optional<ItemVariant> &variant) noexcept;

// A pin marks the incumbent: the worn row of its variant, where the pin
// says. A variant covers several rows (the clean stack and the poisoned
// dagger), and the marker goes on the one in the hand, which is what the
// follower is holding; with none worn -- the pin waiting on the watchdog
// -- every row of the variant is marked until one is. A pin on the form,
// whichever copy (a rule's), marks the same way over all its rows. `rows`
// are the tab's item rows, for whether any of the variant is worn.
[[nodiscard]] const Pin *PinOfRow(const std::vector<Pin> &pins, const RowFacts &row,
                                  std::span<const RowFacts> rows) noexcept;

// Why a row with no pin of its own is set aside: the pins in the way
// (Shadowing), or, where the style forbids two, a one-hander pinned in
// either hand that this one-hander would make a pair with.
struct RowAside
{
    bool aside{false};
    std::vector<Pin> shadowing;
    bool cannotDualWield{false};
};
[[nodiscard]] RowAside RowAsideOf(const std::vector<Pin> &pins, const Holdable &described, bool dualWield);

} // namespace ft
