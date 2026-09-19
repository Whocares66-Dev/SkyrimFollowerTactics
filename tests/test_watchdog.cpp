// The pin watchdog across a fight, and its judgement of a pin and a ban
// on a tick. No Skyrim; what the game reads is supplied.

#include <catch2/catch_test_macros.hpp>

#include "core/Watchdog.h"

using namespace ft;

namespace
{

Holdable Dagger(std::uint32_t form = 0x12EB7)
{
    Holdable h;
    h.form = form;
    h.kind = Kind::Weapon;
    h.grip = Grip::Either;
    h.count = 1;
    return h;
}

Holdable Cuirass(std::uint32_t form = 0x13911)
{
    Holdable h;
    h.form = form;
    h.kind = Kind::Armor;
    h.grip = Grip::None;
    h.slots = 1u << 2;
    return h;
}

Pin PinOf(const Holdable &thing, Hand hands)
{
    return {thing, hands};
}

} // namespace

TEST_CASE("a fight remembers the book on the way in and gives it back on the way out", "[watchdog]")
{
    FightBook book;
    std::vector<Pin> pins{PinOf(Cuirass(), Hand::None)};
    REQUIRE(book.Remembered() == nullptr);

    // In: remembered, nothing to settle.
    REQUIRE_FALSE(book.Note(pins, true));
    REQUIRE(book.fighting);
    REQUIRE(book.Remembered() != nullptr);
    REQUIRE(book.Remembered()->size() == 1);
    // Between edges: nothing.
    REQUIRE_FALSE(book.Note(pins, true));

    // A rule pins a dagger for the fight.
    pins.push_back(PinOf(Dagger(), Hand::Right));
    REQUIRE(book.Remembered()->size() == 1);

    // Out: the rule's pin let go in place (nothing from before wants the
    // hand), the cuirass kept, the book what it was.
    const auto settled = book.Note(pins, false);
    REQUIRE(settled);
    REQUIRE(settled->released.size() == 1);
    REQUIRE(settled->released[0].form == Dagger().form);
    REQUIRE_FALSE(settled->released[0].takeOff);
    REQUIRE(settled->restored.empty());
    REQUIRE(pins.size() == 1);
    REQUIRE(pins[0].thing.form == Cuirass().form);
    REQUIRE_FALSE(book.fighting);
    REQUIRE(book.Remembered() == nullptr);
    REQUIRE_FALSE(book.Note(pins, false));
}

TEST_CASE("the panel's word mid-fight is the new normal; a rule's is for the fight", "[watchdog]")
{
    FightBook book;
    std::vector<Pin> pins;
    REQUIRE_FALSE(book.Note(pins, true));

    // The player pins a dagger from the panel during the fight: into the
    // book in use (the game does that) and mirrored into the remembered
    // one, so it survives the fight's end.
    pins.push_back(PinOf(Dagger(), Hand::Right));
    book.Mirror(PinRequest::Pin, Dagger(), Hand::Right, false, true);
    REQUIRE(book.Remembered()->size() == 1);
    // A rule pins a cuirass: not mirrored.
    pins.push_back(PinOf(Cuirass(), Hand::None));

    const auto settled = book.Note(pins, false);
    REQUIRE(settled);
    REQUIRE(settled->released.size() == 1);
    REQUIRE(settled->released[0].form == Cuirass().form);
    REQUIRE(pins.size() == 1);
    REQUIRE(pins[0].thing.form == Dagger().form);

    // Out of a fight the mirror does nothing.
    book.Mirror(PinRequest::Ban, Dagger(), Hand::Right, false, true);
    REQUIRE(book.before.empty());
}

TEST_CASE("a pin is dropped with no copy left, put back when off, and left while a cast has the hand", "[watchdog]")
{
    const Pin dagger = PinOf(Dagger(), Hand::Right);
    const Pin cuirass = PinOf(Cuirass(), Hand::None);
    PinSeen on;
    on.on = true;
    PinSeen off;
    PinSeen gone;
    gone.carried = false;
    gone.on = false;

    REQUIRE(JudgePin(dagger, on, false, false) == PinVerdict::Keep);
    REQUIRE(JudgePin(dagger, off, false, false) == PinVerdict::PutBack);
    REQUIRE(JudgePin(dagger, gone, false, false) == PinVerdict::Drop);
    // In a fight, one of our casts has the hand: the dagger waits, the
    // cuirass does not.
    REQUIRE(JudgePin(dagger, off, true, true) == PinVerdict::Keep);
    REQUIRE(JudgePin(cuirass, off, true, true) == PinVerdict::PutBack);
    REQUIRE(JudgePin(dagger, off, true, false) == PinVerdict::PutBack);
}

TEST_CASE("a ban takes a thing off unless a pin holds it, and lapses with its last row", "[watchdog]")
{
    Banned form;
    form.form = Dagger().form;
    Banned variant = form;
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    variant.variant = tempered;

    BanSeen on;
    on.on = true;
    BanSeen pinned = on;
    pinned.pinned = true;
    BanSeen off;
    BanSeen none;
    none.carried = false;
    none.on = false;

    REQUIRE(JudgeBan(form, on) == BanVerdict::TakeOff);
    REQUIRE(JudgeBan(form, pinned) == BanVerdict::Keep);
    REQUIRE(JudgeBan(form, off) == BanVerdict::Keep);
    // A ban on the form holds with no copy carried; a ban on a variant
    // goes with its last row.
    REQUIRE(JudgeBan(form, none) == BanVerdict::Keep);
    REQUIRE(JudgeBan(variant, none) == BanVerdict::Drop);
    REQUIRE(JudgeBan(variant, on) == BanVerdict::TakeOff);
}

TEST_CASE("a banned thing a rule pinned for the fight comes off the tick the fight ends", "[watchdog]")
{
    // The fight begins with an empty book; a rule pins the banned dagger,
    // which the ban's judgement leaves alone while the pin holds.
    FightBook book;
    std::vector<Pin> pins;
    REQUIRE_FALSE(book.Note(pins, true));
    pins.push_back(PinOf(Dagger(), Hand::Right));
    Banned ban;
    ban.form = Dagger().form;
    BanSeen seen;
    seen.on = true;
    seen.pinned = FindPin(pins, Dagger()) != nullptr;
    REQUIRE(JudgeBan(ban, seen) == BanVerdict::Keep);

    // The fight ends: the pin is let go in place, and the same pass finds
    // the dagger on and unpinned.
    const auto settled = book.Note(pins, false);
    REQUIRE(settled);
    REQUIRE_FALSE(settled->released[0].takeOff);
    seen.pinned = FindPin(pins, Dagger()) != nullptr;
    REQUIRE_FALSE(seen.pinned);
    REQUIRE(JudgeBan(ban, seen) == BanVerdict::TakeOff);
}
