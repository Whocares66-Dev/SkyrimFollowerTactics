// The bag view and the decisions over it: which copy of a form is which,
// and which one an equip takes (dev/GAME_MODEL.md, part one). Each bag is
// built by hand as the game would read it off the actor's entry. The
// scenarios of 2026-09-12 are here by number where a decision over the
// view is what went wrong.

#include <catch2/catch_test_macros.hpp>

#include "core/BagView.h"

using namespace ft;

namespace
{

// Tokens: addresses that mean nothing, distinct from each other.
constexpr int kTokens[8]{};
const void *Token(int i)
{
    return &kTokens[i];
}

ItemVariant Plain()
{
    return {};
}

ItemVariant Tempered(float health = 1.2f)
{
    ItemVariant v;
    v.tempering = health;
    return v;
}

ItemVariant Named(const char *label)
{
    ItemVariant v;
    v.label = label;
    return v;
}

BagRow Row(int token, ItemVariant variant, bool ownRow, Hand worn = Hand::None, int count = 1)
{
    BagRow row;
    row.token = Token(token);
    row.variant = std::move(variant);
    row.ownRow = ownRow;
    row.wornRight = worn == Hand::Right || worn == Hand::Both;
    row.wornLeft = worn == Hand::Left || worn == Hand::Both;
    row.count = count;
    return row;
}

BagView Weapon(int total, std::vector<BagRow> rows)
{
    BagView view;
    view.total = total;
    view.weapon = true;
    view.rows = std::move(rows);
    return view;
}

BagView Armour(int total, std::vector<BagRow> rows)
{
    BagView view = Weapon(total, std::move(rows));
    view.weapon = false;
    return view;
}

// A row the engine marked Worn: for armour and a shield, the one mark.
BagRow WornMark(int token, ItemVariant variant = {})
{
    BagRow row = Row(token, std::move(variant), false);
    row.wornRight = true;
    return row;
}

} // namespace

TEST_CASE("the listless remainder is what the lists do not account for", "[bag]")
{
    BagView empty;
    REQUIRE(empty.Listed() == 0);
    REQUIRE(empty.Listless() == 0);
    REQUIRE_FALSE(empty.HasListlessCopy());

    // Scenario 4: two plain daggers handed back beside a listless one --
    // one row of three on the tab, count three.
    const BagView three = Weapon(3, {Row(0, Plain(), false), Row(1, Plain(), false)});
    REQUIRE(three.Listed() == 2);
    REQUIRE(three.Listless() == 1);
    REQUIRE(three.HasListlessCopy());
    REQUIRE(CountVariant(three, Plain()) == 3);
    REQUIRE(CountVariant(three, std::nullopt) == 3);
    REQUIRE(RowsOf(three).size() == 1);
    REQUIRE(RowsOf(three).front().IsPlain());

    // A count below the lists' sum -- a stale read -- is not a negative
    // remainder.
    const BagView stale = Weapon(1, {Row(0, Plain(), false, Hand::None, 2)});
    REQUIRE(stale.Listless() == 0);
}

TEST_CASE("a token names a row while the copy is there, and nothing after", "[bag]")
{
    const BagView view = Weapon(2, {Row(0, Plain(), false), Row(1, Tempered(), true)});
    REQUIRE(view.RowOfToken(Token(1)) == &view.rows[1]);
    REQUIRE(view.IndexOf(view.RowOfToken(Token(1))) == 1);
    REQUIRE(view.RowOfToken(Token(5)) == nullptr);
    REQUIRE(view.RowOfToken(nullptr) == nullptr);
}

TEST_CASE("a weapon's marks are by hand; anything else has the one mark", "[bag]")
{
    // Scenario 1: the plain dagger in one hand, the tempered in the
    // other: each row ticks its own hand.
    const BagView daggers = Weapon(2, {Row(0, Plain(), false, Hand::Right), Row(1, Tempered(), true, Hand::Left)});
    REQUIRE(WornIn(daggers, daggers.rows[0], Hand::Right));
    REQUIRE_FALSE(WornIn(daggers, daggers.rows[0], Hand::Left));
    REQUIRE(WornIn(daggers, daggers.rows[1], Hand::Left));
    REQUIRE_FALSE(WornIn(daggers, daggers.rows[1], Hand::Right));
    REQUIRE(WornIn(daggers, daggers.rows[1], Hand::None));
    REQUIRE(WornVariantRow(daggers, Plain(), Hand::Right) == &daggers.rows[0]);
    REQUIRE(WornVariantRow(daggers, Plain(), Hand::Left) == nullptr);
    REQUIRE(WornVariantRow(daggers, Tempered(), Hand::Left) == &daggers.rows[1]);
    // A two-hander sits in the right hand: Both asks the right.
    REQUIRE(ListWorn(daggers.rows[0], Hand::Both));

    // Scenario 2: a worn shield carries the one Worn mark. Asked as a
    // weapon would be, the left hand found nothing to take off; by its
    // kind, the left hand or no hand is any mark, and the right is none.
    const BagView shield = Armour(1, {WornMark(0)});
    REQUIRE(ListWorn(shield.rows[0], Hand::Right));
    REQUIRE(WornIn(shield, shield.rows[0], Hand::Left));
    REQUIRE(WornIn(shield, shield.rows[0], Hand::None));
    REQUIRE_FALSE(WornIn(shield, shield.rows[0], Hand::Right));
    REQUIRE(WornRow(shield, Hand::Left) == &shield.rows[0]);
    REQUIRE(WornRow(shield, Hand::Right) == nullptr);
    REQUIRE(UnwornRow(shield) == nullptr);
}

TEST_CASE("the rows of the tab: each row of its own, and the stack once", "[bag]")
{
    // Three plain on a folded ownership list, one listless, one tempered,
    // one poisoned (plain variant, a row of its own).
    const BagView view = Weapon(
        6, {Row(0, Plain(), false, Hand::None, 3), Row(1, Tempered(), true), Row(2, Plain(), true, Hand::Right)});
    const auto rows = RowsOf(view);
    REQUIRE(rows.size() == 3);
    REQUIRE(SameVariant(rows[0], Tempered()));
    REQUIRE(rows[1].IsPlain());
    REQUIRE(rows[2].IsPlain());
    REQUIRE(CountVariant(view, Plain()) == 5); // three folded, one listless, one poisoned
    REQUIRE(CountVariant(view, Tempered()) == 1);
    REQUIRE(CountVariant(view, Named("Frost Fang")) == 0);

    // Only rows of their own, none plain: no stack row.
    const BagView apart = Weapon(1, {Row(0, Tempered(), true)});
    REQUIRE(RowsOf(apart).size() == 1);
    REQUIRE(RowsOf(BagView{}).empty());

    // As the engine's equip sees the same bag: the plain stack first, then
    // each list, the folded ones plain to the engine and the poisoned one
    // not.
    const auto copies = CopiesForEngine(view);
    REQUIRE(copies.size() == 4);
    REQUIRE(copies[0].plainToEngine);
    REQUIRE_FALSE(copies[0].worn);
    REQUIRE(copies[1].plainToEngine);
    REQUIRE_FALSE(copies[2].plainToEngine);
    REQUIRE_FALSE(copies[3].plainToEngine);
    REQUIRE(copies[3].worn);
    REQUIRE(CopiesForEngine(apart).size() == 1);
}

TEST_CASE("of the unworn, the stack before a row of its own", "[bag]")
{
    // The poisoned dagger shares the plain variant with the clean ones. A
    // pin on the plain variant means a clean one while any is there.
    const BagView view = Weapon(3, {Row(0, Plain(), true), Row(1, Plain(), false), Row(2, Tempered(), true)});
    REQUIRE(UnwornVariantRow(view, Plain()) == &view.rows[1]);
    REQUIRE(UnwornStackRow(view) == &view.rows[1]);
    // With no clean list, the poisoned one is the plain variant's.
    const BagView poisonedOnly = Weapon(1, {Row(0, Plain(), true)});
    REQUIRE(UnwornVariantRow(poisonedOnly, Plain()) == &poisonedOnly.rows[0]);
    REQUIRE(UnwornStackRow(poisonedOnly) == nullptr);
    REQUIRE(WornStackRow(view, Hand::Right) == nullptr);
}

TEST_CASE("the panel's click on a row: that row, the incumbent left alone, a copy across the hands moved", "[bag]")
{
    // Scenario 3: the poisoned row clicked readies the poisoned copy, not
    // the clean one first in the entry's order.
    const BagView bag = Weapon(2, {Row(0, Plain(), false), Row(1, Plain(), true)});
    EquipPlan plan = PlanEquip(bag, EquipAsk::Row, std::nullopt, Hand::Right, Token(1), false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.row == 1);
    REQUIRE_FALSE(plan.unequipOther);

    // The row is already in that hand: nothing.
    const BagView inHand = Weapon(1, {Row(0, Tempered(), true, Hand::Right)});
    REQUIRE_FALSE(PlanEquip(inHand, EquipAsk::Row, std::nullopt, Hand::Right, Token(0), false).proceed);

    // Scenario 5 and 11: the row clicked for the right hand while worn in
    // the left moves, whatever the variant's count: the left lets go
    // first.
    const BagView inOther = Weapon(5, {Row(0, Plain(), false, Hand::Left, 5)});
    plan = PlanEquip(inOther, EquipAsk::Row, std::nullopt, Hand::Right, Token(0), false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.unequipOther);
    REQUIRE(plan.row == 0);

    // The copy has left since the scan: the token names nothing, and
    // nothing is done.
    REQUIRE_FALSE(PlanEquip(bag, EquipAsk::Row, std::nullopt, Hand::Right, Token(7), false).proceed);

    // A worn piece of armour clicked: no hand to move it between.
    const BagView armour = Armour(1, {WornMark(0)});
    REQUIRE_FALSE(PlanEquip(armour, EquipAsk::Row, std::nullopt, Hand::None, Token(0), false).proceed);
}

TEST_CASE("the panel's click on the stack: a clean copy, a listless one by null, the only one across", "[bag]")
{
    // Scenario 10: the plain stack clicked while the poisoned one holds
    // the hand: a clean one goes in, the poisoned row being no part of
    // the stack.
    const BagView bag = Weapon(2, {Row(0, Plain(), true, Hand::Right), Row(1, Plain(), false)});
    EquipPlan plan = PlanEquip(bag, EquipAsk::Stack, std::nullopt, Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.row == 1);

    // Only a listless copy beside the poisoned one: the null list names it.
    const BagView listless = Weapon(2, {Row(0, Plain(), true, Hand::Right)});
    plan = PlanEquip(listless, EquipAsk::Stack, std::nullopt, Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE_FALSE(plan.row);

    // A stack copy already in the hand: the incumbent.
    const BagView worn = Weapon(2, {Row(0, Plain(), false, Hand::Right), Row(1, Plain(), false)});
    REQUIRE_FALSE(PlanEquip(worn, EquipAsk::Stack, std::nullopt, Hand::Right, nullptr, false).proceed);

    // The stack's only copy is in the other hand: it comes across.
    const BagView across = Weapon(1, {Row(0, Plain(), false, Hand::Left)});
    plan = PlanEquip(across, EquipAsk::Stack, std::nullopt, Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.unequipOther);
    REQUIRE(plan.row == 0);
    // But not for a thing with no hand to come across from, nor when
    // nothing of the stack is anywhere.
    REQUIRE_FALSE(
        PlanEquip(Armour(1, {WornMark(0)}), EquipAsk::Stack, std::nullopt, Hand::None, nullptr, false).proceed);
    REQUIRE_FALSE(
        PlanEquip(Weapon(1, {Row(0, Tempered(), true)}), EquipAsk::Stack, std::nullopt, Hand::Right, nullptr, false)
            .proceed);
}

TEST_CASE("a rule's or the watchdog's equip of a variant: the incumbent, the leeway, the copy across", "[bag]")
{
    // Scenario 7: the poisoned dagger in the hand, pinned there as the
    // plain variant: it is the incumbent and stays.
    const BagView poisonedInHand = Weapon(2, {Row(0, Plain(), true, Hand::Right), Row(1, Plain(), false)});
    REQUIRE_FALSE(PlanEquip(poisonedInHand, EquipAsk::Variant, Plain(), Hand::Right, nullptr, false).proceed);

    // A clean copy of the plain variant before the poisoned one.
    const BagView both = Weapon(2, {Row(0, Plain(), true), Row(1, Plain(), false)});
    EquipPlan plan = PlanEquip(both, EquipAsk::Variant, Plain(), Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.row == 1);

    // Only the poisoned list and a listless copy: the listless one, by
    // null, before the poisoned row.
    const BagView listless = Weapon(2, {Row(0, Plain(), true)});
    plan = PlanEquip(listless, EquipAsk::Variant, Plain(), Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE_FALSE(plan.row);

    // The tempered variant named, with none unworn and none across: nothing.
    const BagView none = Weapon(2, {Row(0, Plain(), false), Row(1, Plain(), false)});
    REQUIRE_FALSE(PlanEquip(none, EquipAsk::Variant, Tempered(), Hand::Right, nullptr, false).proceed);

    // Scenario 9: the pinned dagger the engine moved to the other hand
    // comes back across -- the only copy of the variant.
    const BagView moved = Weapon(1, {Row(0, Tempered(), true, Hand::Left)});
    plan = PlanEquip(moved, EquipAsk::Variant, Tempered(), Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.unequipOther);
    REQUIRE(plan.row == 0);
    // Scenario 6, the bag's part: with a second copy of the variant the
    // one in the other hand stays; and with a clean one to hand, it is
    // taken rather than the worn one.
    const BagView two = Weapon(2, {Row(0, Plain(), false, Hand::Left), Row(1, Plain(), false)});
    plan = PlanEquip(two, EquipAsk::Variant, Plain(), Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE_FALSE(plan.unequipOther);
    REQUIRE(plan.row == 1);
    // Two copies, both worn: the one in the other hand is not brought
    // across, and there is nothing else: nothing.
    const BagView twoWorn = Weapon(2, {Row(0, Tempered(), true, Hand::Left, 2)});
    REQUIRE_FALSE(PlanEquip(twoWorn, EquipAsk::Variant, Tempered(), Hand::Right, nullptr, false).proceed);
}

TEST_CASE("an equip of the form, whichever copy, is the engine's to resolve", "[bag]")
{
    // No variant named: the engine's own reading of the hand is the
    // incumbent test.
    const BagView bag = Weapon(2, {Row(0, Plain(), false), Row(1, Tempered(), true)});
    REQUIRE_FALSE(PlanEquip(bag, EquipAsk::Variant, std::nullopt, Hand::Right, nullptr, true).proceed);
    EquipPlan plan = PlanEquip(bag, EquipAsk::Variant, std::nullopt, Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.row == 0);

    // Nothing unworn and only a listless copy: the null list, and the
    // engine chooses.
    const BagView listless = Weapon(1, {});
    plan = PlanEquip(listless, EquipAsk::Variant, std::nullopt, Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE_FALSE(plan.row);

    // The only copy in the other hand: across.
    const BagView across = Weapon(1, {Row(0, Plain(), false, Hand::Left)});
    plan = PlanEquip(across, EquipAsk::Variant, std::nullopt, Hand::Right, nullptr, false);
    REQUIRE(plan.proceed);
    REQUIRE(plan.unequipOther);
    REQUIRE(plan.row == 0);

    // A shield for the left hand, worn already by its one mark: the
    // engine's answer says so, and the marks are not asked by hand.
    const BagView shield = Armour(1, {WornMark(0)});
    REQUIRE_FALSE(PlanEquip(shield, EquipAsk::Variant, std::nullopt, Hand::Left, nullptr, true).proceed);
}
