// The pin watchdog across a fight, and its judgement of a pin and a ban
// on a tick. No Skyrim; what the game reads is supplied.

#include <catch2/catch_test_macros.hpp>

#include "core/Watchdog.h"

using namespace ft;

namespace
{

Holdable Dagger(std::uint32_t form = 0x1397E)
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

TEST_CASE("a pin made out of a fight is not the fight's, and survives one", "[watchdog]")
{
    // An idle rule equips while nothing is happening. A rule's pin is the
    // FIGHT'S, and this one was made in no fight: there is no edge out of
    // idle for it to be settled on, and a follower who was told to hold
    // something while walking about must not be undressed by the end of
    // the next skirmish. The book only ever lets go of what it did not
    // have on the way in.
    FightBook book;
    std::vector<Pin> pins{PinOf(Dagger(), Hand::Right)};

    // A fight comes and goes over it, and it is still there.
    REQUIRE_FALSE(book.Note(pins, true));
    const auto settled = book.Note(pins, false);
    REQUIRE(settled);
    REQUIRE(settled->released.empty());
    REQUIRE(settled->restored.empty());
    REQUIRE(pins.size() == 1);
    REQUIRE(pins[0].thing.form == Dagger().form);

    // And a second, so nothing is lost by the round trip through the
    // remembered book either.
    REQUIRE_FALSE(book.Note(pins, true));
    const auto again = book.Note(pins, false);
    REQUIRE(again);
    REQUIRE(again->released.empty());
    REQUIRE(pins.size() == 1);
    REQUIRE(pins[0].thing.form == Dagger().form);
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

    REQUIRE(JudgeBan(on) == BanVerdict::TakeOff);
    REQUIRE(JudgeBan(pinned) == BanVerdict::Keep);
    REQUIRE(JudgeBan(off) == BanVerdict::Keep);
    // A mark holds while there is something for it to hold about, and
    // goes with the last of it. The same rule a pin follows, deliberately:
    // the two are one instruction about one thing, and a player who sets
    // both expects them to last as long as each other. A ban on the form
    // used to outlive every copy, on the reading that it had something to
    // say the day another arrived; it is the reader that now answers what
    // `carried` means of a form ban and of a variant ban, so the verdict
    // has no need of the ban itself.
    REQUIRE(JudgeBan(none) == BanVerdict::Drop);
    REQUIRE(JudgeBan(on) == BanVerdict::TakeOff);
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
    REQUIRE(JudgeBan(seen) == BanVerdict::Keep);

    // The fight ends: the pin is let go in place, and the same pass finds
    // the dagger on and unpinned.
    const auto settled = book.Note(pins, false);
    REQUIRE(settled);
    REQUIRE_FALSE(settled->released[0].takeOff);
    seen.pinned = FindPin(pins, Dagger()) != nullptr;
    REQUIRE_FALSE(seen.pinned);
    REQUIRE(JudgeBan(seen) == BanVerdict::TakeOff);
}

TEST_CASE("a pass puts the pins back first, then takes the banned things off", "[watchdog]")
{
    // A pin whose thing is off, one still on, one whose copies are gone;
    // and two bans, one on a thing found on and one on a thing that is not.
    std::vector<Pin> pins{PinOf(Dagger(), Hand::Right), PinOf(Cuirass(), Hand::None),
                          PinOf(Dagger(0x13989), Hand::Left)};
    PinSeen off;
    PinSeen on;
    on.on = true;
    PinSeen gone;
    gone.carried = false;
    const std::vector<PinSeen> pinsSeen{off, on, gone};

    Bans bans;
    Ban(bans, 0x1397E);
    Ban(bans, 0x13911);
    BanSeen worn;
    worn.on = true;
    BanSeen away;
    const std::vector<BanSeen> bansSeen{worn, away};

    const WatchPlan plan = PlanWatch(pins, pinsSeen, bans, bansSeen, false, false, {});
    REQUIRE(plan.dropPins == std::vector<std::size_t>{2});
    REQUIRE(plan.dropBans.empty());
    REQUIRE(plan.steps.size() == 2);
    // The pin's put-back comes before the ban's take-off.
    REQUIRE(plan.steps[0].act == WatchAct::PutBack);
    REQUIRE(plan.steps[0].index == 0);
    REQUIRE(plan.steps[1].act == WatchAct::TakeOff);
    REQUIRE(plan.steps[1].index == 0);
    REQUIRE_FALSE(plan.steps[1].afterFight);
}

TEST_CASE("one of our casts holds a hand: nothing is taken off, and a hand pin waits", "[watchdog]")
{
    std::vector<Pin> pins{PinOf(Dagger(), Hand::Right), PinOf(Cuirass(), Hand::None)};
    PinSeen off;
    const std::vector<PinSeen> pinsSeen{off, off};
    Bans bans;
    Ban(bans, 0x1397E);
    BanSeen worn;
    worn.on = true;
    const std::vector<BanSeen> bansSeen{worn};

    const WatchPlan plan = PlanWatch(pins, pinsSeen, bans, bansSeen, true, true, {});
    // The armour goes back; the hand pin waits for the cast; no ban is
    // enforced at all while a cast is in the air.
    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.steps[0].act == WatchAct::PutBack);
    REQUIRE(plan.steps[0].index == 1);
    REQUIRE(plan.dropBans.empty());
}

TEST_CASE("a banned thing whose rule pin went with the fight is taken off, and says so", "[watchdog]")
{
    const std::vector<Pin> pins;
    const std::vector<PinSeen> pinsSeen;
    Bans bans;
    Ban(bans, 0x1397E);
    BanSeen worn;
    worn.on = true;
    const std::vector<BanSeen> bansSeen{worn};
    const std::vector<std::uint32_t> lapsed{0x1397E};

    const WatchPlan plan = PlanWatch(pins, pinsSeen, bans, bansSeen, false, false, lapsed);
    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.steps[0].act == WatchAct::TakeOff);
    REQUIRE(plan.steps[0].afterFight);
    // Another form let go by the fight does not mark this one.
    const std::vector<std::uint32_t> other{0x13989};
    REQUIRE_FALSE(PlanWatch(pins, pinsSeen, bans, bansSeen, false, false, other).steps[0].afterFight);
}

TEST_CASE("a ban on a variant with no row left is dropped, and enforces nothing", "[watchdog]")
{
    const std::vector<Pin> pins;
    const std::vector<PinSeen> pinsSeen;
    Bans bans;
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    Ban(bans, 0x1397E, tempered);
    BanSeen none;
    none.carried = false;
    const std::vector<BanSeen> bansSeen{none};

    const WatchPlan plan = PlanWatch(pins, pinsSeen, bans, bansSeen, false, false, {});
    REQUIRE(plan.dropBans == std::vector<std::size_t>{0});
    REQUIRE(plan.steps.empty());
}
