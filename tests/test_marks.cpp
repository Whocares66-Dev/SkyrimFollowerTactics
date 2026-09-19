// What the panel's rows say about the pin and ban books, without the panel.

#include <catch2/catch_test_macros.hpp>

#include "core/Marks.h"

#include <vector>

using namespace ft;

namespace
{

constexpr std::uint32_t kDagger = 0x12EB7;
constexpr std::uint32_t kSword = 0x13989;

Holdable OneHander(std::uint32_t form)
{
    Holdable h;
    h.form = form;
    h.kind = Kind::Weapon;
    h.grip = Grip::Either;
    h.count = 1;
    return h;
}

ItemVariant Tempered()
{
    ItemVariant v;
    v.tempering = 1.2f;
    return v;
}

RowFacts Row(std::uint32_t form, std::optional<ItemVariant> variant, bool left = false, bool right = false)
{
    RowFacts row;
    row.form = form;
    row.variant = std::move(variant);
    row.left = left;
    row.right = right;
    row.worn = left || right;
    return row;
}

} // namespace

TEST_CASE("a ban marks its variant's row, or every row of its form", "[marks]")
{
    Bans bans;
    Ban(bans, kDagger, Tempered());
    REQUIRE(BansRow(bans, kDagger, Tempered()));
    REQUIRE_FALSE(BansRow(bans, kDagger, ItemVariant{}));
    REQUIRE_FALSE(BansRow(bans, kSword, Tempered()));
    Ban(bans, kSword);
    REQUIRE(BansRow(bans, kSword, ItemVariant{}));
    REQUIRE(BansRow(bans, kSword, Tempered()));
    REQUIRE(BansRow(bans, kSword, std::nullopt));
}

TEST_CASE("a pin marks the worn row of its variant, or every row while none is worn", "[marks]")
{
    // The clean stack and the poisoned dagger share the plain variant; the
    // poisoned one is in the right hand.
    const std::vector<RowFacts> rows{Row(kDagger, ItemVariant{}), Row(kDagger, ItemVariant{}, false, true),
                                     Row(kDagger, Tempered())};
    std::vector<Pin> pins{{OneHander(kDagger), Hand::Right}};
    pins[0].thing.variant = ItemVariant{};

    // Only the incumbent gets the pin.
    REQUIRE(PinOfRow(pins, rows[0], rows) == nullptr);
    REQUIRE(PinOfRow(pins, rows[1], rows) == &pins[0]);
    REQUIRE(PinOfRow(pins, rows[2], rows) == nullptr);

    // Nothing of the variant worn: every row of it is marked, the pin
    // waiting on the watchdog.
    const std::vector<RowFacts> none{Row(kDagger, ItemVariant{}), Row(kDagger, ItemVariant{}),
                                     Row(kDagger, Tempered())};
    REQUIRE(PinOfRow(pins, none[0], none) == &pins[0]);
    REQUIRE(PinOfRow(pins, none[1], none) == &pins[0]);
    REQUIRE(PinOfRow(pins, none[2], none) == nullptr);

    // A pin on the form, whichever copy, marks the tempered row too, and
    // only the worn one when one is worn where it says.
    std::vector<Pin> form{{OneHander(kDagger), Hand::Right}};
    REQUIRE(PinOfRow(form, none[2], none) == &form[0]);
    REQUIRE(PinOfRow(form, rows[2], rows) == nullptr);
    REQUIRE(PinOfRow(form, rows[1], rows) == &form[0]);

    // Worn in the other hand is not the incumbent of a right-hand pin.
    const std::vector<RowFacts> leftWorn{Row(kDagger, ItemVariant{}, true, false)};
    REQUIRE(PinOfRow(pins, leftWorn[0], leftWorn) == &pins[0]); // none worn right: marked while waiting
    // A no-hand pin asks whether the row is worn at all.
    std::vector<Pin> armour{{OneHander(kDagger), Hand::None}};
    REQUIRE(PinOfRow(armour, leftWorn[0], leftWorn) == &armour[0]);

    // A spell's entry has the pin's own hands: marked by any pin on it.
    RowFacts spell;
    spell.form = kSword;
    spell.item = false;
    std::vector<Pin> spellPin{{OneHander(kSword), Hand::Left}};
    REQUIRE(PinOfRow(spellPin, spell, rows) == &spellPin[0]);
}

TEST_CASE("a row with no pin of its own is set aside by the pins in the way, or by the style", "[marks]")
{
    std::vector<Pin> pins{{OneHander(kDagger), Hand::Both}};
    const Holdable sword = OneHander(kSword);
    // Both hands pinned: a one-hander has nowhere to go.
    RowAside aside = RowAsideOf(pins, sword, true);
    REQUIRE(aside.aside);
    REQUIRE(aside.shadowing.size() == 1);
    REQUIRE_FALSE(aside.cannotDualWield);

    // One hand pinned, dual wielding allowed: the other hand is free.
    std::vector<Pin> one{{OneHander(kDagger), Hand::Right}};
    aside = RowAsideOf(one, sword, true);
    REQUIRE_FALSE(aside.aside);
    // The style forbids two: the sword would pair with the pinned dagger.
    aside = RowAsideOf(one, sword, false);
    REQUIRE(aside.aside);
    REQUIRE(aside.cannotDualWield);
    REQUIRE(aside.shadowing.empty());
    // The pinned thing itself is not set aside by its own pin.
    REQUIRE_FALSE(RowAsideOf(one, OneHander(kDagger), false).aside);
}

TEST_CASE("a follower's cell walks round: equip, pin, ban, unban; the player's only equips and unequips", "[marks]")
{
    EquipCell cell;
    REQUIRE(NextWearRequest(cell, false) == WearRequest::Equip);
    cell.on = true;
    REQUIRE(NextWearRequest(cell, false) == WearRequest::Pin);
    cell.pinned = true;
    REQUIRE(NextWearRequest(cell, false) == WearRequest::Ban);
    cell.banned = true;
    cell.pinned = false;
    cell.on = false;
    REQUIRE(NextWearRequest(cell, false) == WearRequest::Unban);

    // A pin whose thing the AI swapped out is still a pin: the click bans.
    EquipCell swapped;
    swapped.pinned = true;
    REQUIRE(NextWearRequest(swapped, false) == WearRequest::Ban);
    // A banned thing found on: the click unbans.
    EquipCell bannedOn;
    bannedOn.banned = true;
    bannedOn.on = true;
    REQUIRE(NextWearRequest(bannedOn, false) == WearRequest::Unban);

    // The player: on or off, whatever the book says.
    REQUIRE(NextWearRequest(EquipCell{}, true) == WearRequest::Equip);
    REQUIRE(NextWearRequest(bannedOn, true) == WearRequest::Unequip);
    REQUIRE(NextWearRequest(swapped, true) == WearRequest::Equip);
}

TEST_CASE("a sorted column: pinned, equipped, unequipped, banned, then what cannot be held", "[marks]")
{
    EquipCell pinned;
    pinned.pinned = true;
    pinned.on = true;
    EquipCell on;
    on.on = true;
    const EquipCell off;
    EquipCell banned;
    banned.banned = true;
    EquipCell dim;
    dim.disabled = true;
    dim.pinned = true;
    EquipCell slashed;
    slashed.allowed = false;
    REQUIRE(CellRank(pinned) == 0);
    REQUIRE(CellRank(on) == 1);
    REQUIRE(CellRank(off) == 2);
    REQUIRE(CellRank(banned) == 3);
    REQUIRE(CellRank(dim) == 4);
    REQUIRE(CellRank(slashed) == 4);
}
