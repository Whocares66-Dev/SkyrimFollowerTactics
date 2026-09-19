// A follower's lease on a package record, from the tick's side: armed
// with a deadline, extended once when the cast begins and once when a
// stream starts, finished for one reason. No Skyrim; the game reads the
// engine and the sink's flags into a LeaseSeen each tick.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Lease.h"

#include <string>

using namespace ft;
using Catch::Approx;

namespace
{

std::string Finish(const LeaseStep &step)
{
    return step.finish ? step.finish : "";
}

LeaseSeen Running()
{
    LeaseSeen seen;
    seen.running = true;
    return seen;
}

} // namespace

TEST_CASE("a lease armed now has its window, and nothing else yet", "[lease]")
{
    const LeaseState state = ArmLease(100.0, kArmWindowSeconds, true, 4.0f, false);
    REQUIRE(state.armedAt == 100.0);
    REQUIRE(state.until == Approx(102.5));
    REQUIRE(state.sustained);
    REQUIRE(state.sustain == 4.0f);
    REQUIRE_FALSE(state.seenRunning);
    REQUIRE_FALSE(state.streaming);
    REQUIRE_FALSE(state.extended);
    REQUIRE_FALSE(state.swinging);
    REQUIRE(ArmLease(0.0, kWeaponArmWindowSeconds, false, 0.0f, true).weapon);
}

TEST_CASE("never picked up: the deadline says so, and not a moment before", "[lease]")
{
    LeaseState state = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    REQUIRE_FALSE(AdvanceCast(state, {}, LeaseKind::Spell, 100.5).finish);
    REQUIRE_FALSE(AdvanceCast(state, {}, LeaseKind::Spell, 102.4999).finish);
    REQUIRE(Finish(AdvanceCast(state, {}, LeaseKind::Spell, 102.5)) == "deadline, AI never picked it up");
}

TEST_CASE("picked up and fired: the fire releases, once, and spends the scroll", "[lease]")
{
    LeaseState state = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    LeaseStep step = AdvanceCast(state, Running(), LeaseKind::Spell, 100.4);
    REQUIRE(step.pickedUp);
    REQUIRE_FALSE(step.finish);
    REQUIRE(state.seenRunning);
    // Running again is not picked up again.
    REQUIRE_FALSE(AdvanceCast(state, Running(), LeaseKind::Spell, 100.9).pickedUp);

    LeaseSeen fired = Running();
    fired.fired = true;
    step = AdvanceCast(state, fired, LeaseKind::Spell, 101.4);
    REQUIRE(Finish(step) == "spell fired");
    REQUIRE(step.fired);

    // The voice slot's wording.
    LeaseState voice = ArmLease(100.0, kVoiceArmWindowSeconds, false, 0.0f, false);
    REQUIRE(Finish(AdvanceCast(voice, fired, LeaseKind::Shout, 101.0)) == "shout fired");
    voice = ArmLease(100.0, kVoiceArmWindowSeconds, false, 0.0f, false);
    REQUIRE(Finish(AdvanceCast(voice, fired, LeaseKind::Power, 101.0)) == "power fired");
}

TEST_CASE("the cast begun steps the deadline back once, and never forward", "[lease]")
{
    LeaseState state = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    LeaseSeen begun = Running();
    begun.begun = true;
    LeaseStep step = AdvanceCast(state, begun, LeaseKind::Spell, 102.0);
    REQUIRE(step.beginExtended);
    REQUIRE(state.until == Approx(105.0));
    // Seen again, the flag still up: no second extension.
    step = AdvanceCast(state, begun, LeaseKind::Spell, 104.0);
    REQUIRE_FALSE(step.beginExtended);
    REQUIRE(state.until == Approx(105.0));
    // Never fired: the deadline names it.
    REQUIRE_FALSE(AdvanceCast(state, begun, LeaseKind::Spell, 104.99).finish);
    REQUIRE(Finish(AdvanceCast(state, begun, LeaseKind::Spell, 105.0)) == "deadline, never cast");

    // Begun with plenty of window left: the deadline stays where it was.
    LeaseState early = ArmLease(100.0, 10.0, false, 0.0f, false);
    REQUIRE(AdvanceCast(early, begun, LeaseKind::Spell, 100.5).beginExtended);
    REQUIRE(early.until == Approx(110.0));

    // A shout's deadline wording.
    LeaseState voice = ArmLease(100.0, kVoiceArmWindowSeconds, false, 0.0f, false);
    REQUIRE(Finish(AdvanceCast(voice, Running(), LeaseKind::Shout, 103.0)) == "deadline, shout never ended");
}

TEST_CASE("a stream: the fire is its start, the deadline its end plus grace, the stop its release", "[lease]")
{
    LeaseState state = ArmLease(100.0, kArmWindowSeconds, true, 4.0f, false);
    LeaseSeen fired = Running();
    fired.fired = true;
    LeaseStep step = AdvanceCast(state, fired, LeaseKind::Spell, 101.0);
    REQUIRE(step.streamExtended);
    REQUIRE_FALSE(step.finish);
    REQUIRE(state.streaming);
    REQUIRE(state.until == Approx(106.0)); // now + sustain + grace, not a max
    // Once.
    REQUIRE_FALSE(AdvanceCast(state, fired, LeaseKind::Spell, 102.0).streamExtended);

    // The stop, after the fire, ends it.
    LeaseSeen stopped = fired;
    stopped.stopped = true;
    step = AdvanceCast(state, stopped, LeaseKind::Spell, 103.0);
    REQUIRE(Finish(step) == "stream ended");
    REQUIRE_FALSE(step.fired); // no scroll is spent on a stream's end

    // Never stopped: the deadline, still running.
    LeaseState running = ArmLease(100.0, kArmWindowSeconds, true, 4.0f, false);
    AdvanceCast(running, fired, LeaseKind::Spell, 101.0);
    REQUIRE(Finish(AdvanceCast(running, fired, LeaseKind::Spell, 106.0)) == "deadline, stream still running");

    // A short stream ends sooner than the pick-up window would have: the
    // extension is what was asked for, not a floor.
    LeaseState brief = ArmLease(100.0, kArmWindowSeconds, true, 0.5f, false);
    AdvanceCast(brief, fired, LeaseKind::Spell, 100.2);
    REQUIRE(brief.until == Approx(101.7));

    // The target dead ends a stream, and only a stream.
    LeaseSeen dead = Running();
    dead.targetDead = true;
    LeaseState stream = ArmLease(100.0, kArmWindowSeconds, true, 4.0f, false);
    REQUIRE(Finish(AdvanceCast(stream, dead, LeaseKind::Spell, 100.5)) == "target dead");
    LeaseState plain = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    REQUIRE_FALSE(AdvanceCast(plain, dead, LeaseKind::Spell, 100.5).finish);
}

TEST_CASE("the AI moving on ends it; the holder vanishing ends it whatever else is seen", "[lease]")
{
    LeaseState state = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    AdvanceCast(state, Running(), LeaseKind::Spell, 100.5);
    REQUIRE(Finish(AdvanceCast(state, {}, LeaseKind::Spell, 101.0)) == "package ended");

    // Not running yet is not "ended".
    LeaseState fresh = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    REQUIRE_FALSE(AdvanceCast(fresh, {}, LeaseKind::Spell, 101.0).finish);

    LeaseSeen gone;
    gone.holder = false;
    gone.fired = true;
    gone.running = true;
    REQUIRE(Finish(AdvanceCast(fresh, gone, LeaseKind::Spell, 101.0)) == "holder vanished");
    LeaseState weapon = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    REQUIRE(Finish(AdvanceWeapon(weapon, gone, 101.0)) == "holder vanished");
}

TEST_CASE("when several hold at once: the fire, then the exit, then the clock", "[lease]")
{
    LeaseState state = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    AdvanceCast(state, Running(), LeaseKind::Spell, 100.5);
    LeaseSeen all;
    all.fired = true;
    all.running = false;
    REQUIRE(Finish(AdvanceCast(state, all, LeaseKind::Spell, 110.0)) == "spell fired");
    LeaseState other = ArmLease(100.0, kArmWindowSeconds, false, 0.0f, false);
    AdvanceCast(other, Running(), LeaseKind::Spell, 100.5);
    REQUIRE(Finish(AdvanceCast(other, {}, LeaseKind::Spell, 110.0)) == "package ended");
}

TEST_CASE("a power attack: the swing seen steps the deadline back, and its end makes it", "[lease]")
{
    LeaseState state = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    // Their own swing at arm time, or any attack before the package is
    // theirs, is not ours.
    LeaseSeen swinging;
    swinging.attacking = true;
    swinging.ourPowerSwing = true;
    LeaseStep step = AdvanceWeapon(state, swinging, 100.2);
    REQUIRE_FALSE(step.swingSeen);
    REQUIRE(step.turnToward);

    LeaseSeen picked = Running();
    step = AdvanceWeapon(state, picked, 100.5);
    REQUIRE(step.pickedUp);
    REQUIRE(step.turnToward);

    // An ordinary attack is not the power attack.
    LeaseSeen plainSwing = Running();
    plainSwing.attacking = true;
    REQUIRE_FALSE(AdvanceWeapon(state, plainSwing, 100.8).swingSeen);

    LeaseSeen ours = Running();
    ours.attacking = true;
    ours.ourPowerSwing = true;
    step = AdvanceWeapon(state, ours, 102.9);
    REQUIRE(step.swingSeen);
    REQUIRE_FALSE(step.turnToward);
    REQUIRE(state.until == Approx(105.9));
    // Once; and with the deadline further out already, it stays.
    REQUIRE_FALSE(AdvanceWeapon(state, ours, 103.0).swingSeen);

    // The swing over: made.
    REQUIRE(Finish(AdvanceWeapon(state, Running(), 104.0)) == "power attack made");
}

TEST_CASE("a power attack that never comes, or is cut off", "[lease]")
{
    LeaseState state = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    REQUIRE(Finish(AdvanceWeapon(state, {}, 103.0)) == "deadline, AI never picked it up");

    LeaseState picked = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    AdvanceWeapon(picked, Running(), 100.5);
    REQUIRE_FALSE(AdvanceWeapon(picked, Running(), 102.99).finish);
    REQUIRE(Finish(AdvanceWeapon(picked, Running(), 103.0)) == "deadline, no power attack");

    LeaseState ended = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    AdvanceWeapon(ended, Running(), 100.5);
    REQUIRE(Finish(AdvanceWeapon(ended, {}, 101.0)) == "package ended");

    LeaseSeen ours = Running();
    ours.attacking = true;
    ours.ourPowerSwing = true;
    LeaseState mid = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    AdvanceWeapon(mid, Running(), 100.5);
    AdvanceWeapon(mid, ours, 101.0);
    LeaseSeen exited;
    exited.attacking = true;
    REQUIRE(Finish(AdvanceWeapon(mid, exited, 101.5)) == "package ended mid-swing");

    LeaseState stuck = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    AdvanceWeapon(stuck, Running(), 100.5);
    AdvanceWeapon(stuck, ours, 101.0);
    REQUIRE(Finish(AdvanceWeapon(stuck, ours, 104.0)) == "deadline, still swinging");

    // In an override list combat owns the facing.
    LeaseState listed = ArmLease(100.0, kWeaponArmWindowSeconds, false, 0.0f, true);
    LeaseSeen inList = Running();
    inList.inOverrideList = true;
    REQUIRE_FALSE(AdvanceWeapon(listed, inList, 100.5).turnToward);
}

TEST_CASE("a record goes to the running package's array, else an override list holding it, else the fullest", "[lease]")
{
    // Two alias arrays, the second holding the running package; two
    // override lists, the first holding it.
    std::vector<StackSeen> places{{false, false, 3}, {false, true, 1}, {true, true, 0}, {true, false, 0}};
    REQUIRE(ChooseStack(places, true) == 1);
    REQUIRE(ChooseStack(places, false) == 1);
    // Nothing running in an array: the override list, when allowed.
    places[1].holdsRunning = false;
    REQUIRE(ChooseStack(places, true) == 2);
    // Not allowed: the fullest array.
    REQUIRE(ChooseStack(places, false) == 0);
    // Nothing holds it: the fullest; empty arrays are nobody's.
    places[2].holdsRunning = false;
    REQUIRE(ChooseStack(places, true) == 0);
    const std::vector<StackSeen> empty{{false, false, 0}, {true, false, 0}};
    REQUIRE_FALSE(ChooseStack(empty, true));
    REQUIRE_FALSE(ChooseStack({}, true));
}
