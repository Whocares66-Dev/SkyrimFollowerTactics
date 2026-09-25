// A follower's power attack from the request to the swing's end. No Skyrim;
// the action is answered by a script, and the actor's state is supplied.

#include <catch2/catch_test_macros.hpp>

#include "core/Strike.h"

#include <string>

using namespace ft;

namespace
{

std::string Over(const char *reason)
{
    return reason ? reason : "";
}

struct Takes
{
    int asked{0};
    bool take{true};
    std::function<bool()> Fn()
    {
        return [this] {
            ++asked;
            return take;
        };
    }
};

// Drawn, free, the target in front.
StrikeSeen Ready()
{
    StrikeSeen seen;
    seen.weaponDrawn = true;
    seen.facing = true;
    return seen;
}

StrikeSeen Swinging(StrikeSeen seen = Ready())
{
    seen.attacking = true;
    return seen;
}

} // namespace

TEST_CASE("free, facing: the action at once, made at its hit, over at its end", "[strike]")
{
    StrikeState run = RequestStrikeAt(100.0);
    Takes takes;
    REQUIRE_FALSE(AdvanceStrike(run, Ready(), 100.0, takes.Fn()));
    REQUIRE(takes.asked == 1);
    REQUIRE(run.step == StrikeStep::Striking);
    REQUIRE(run.sentAt == 100.0);
    REQUIRE_FALSE(run.waited);

    // Swinging, not yet at its hit.
    REQUIRE_FALSE(AdvanceStrike(run, Swinging(), 100.3, takes.Fn()));
    REQUIRE(run.sawAttack);
    StrikeSeen hit = Swinging();
    hit.hitFrames = 1;
    REQUIRE_FALSE(AdvanceStrike(run, hit, 100.6, takes.Fn()));
    REQUIRE(run.hitAt == 100.6);
    // Its own end: made.
    StrikeSeen stopped = Ready();
    stopped.hitFrames = 1;
    stopped.powerStops = 1;
    stopped.attackStops = 1;
    REQUIRE(Over(AdvanceStrike(run, stopped, 101.4, takes.Fn())) == "power attack made");
    REQUIRE(run.endAt == 101.4);
    REQUIRE(takes.asked == 1);
}

TEST_CASE("steps that miss the attack state still see it made by its events", "[strike]")
{
    // A dual-wield power attack: three hit frames, no PowerAttackStop, a
    // plain attackStop at the end; no step lands inside the attack state.
    StrikeState run = RequestStrikeAt(100.0);
    Takes takes;
    StrikeSeen before = Ready();
    before.hitFrames = 7;
    before.attackStops = 4;
    REQUIRE_FALSE(AdvanceStrike(run, before, 100.0, takes.Fn()));
    StrikeSeen hits = Ready();
    hits.hitFrames = 10;
    hits.attackStops = 4;
    REQUIRE_FALSE(AdvanceStrike(run, hits, 101.0, takes.Fn()));
    REQUIRE(run.hitAt == 101.0);
    hits.attackStops = 5;
    REQUIRE(Over(AdvanceStrike(run, hits, 102.0, takes.Fn())) == "power attack made");
}

TEST_CASE("an end event before any hit is not this attack's; in and out with no hit is cut short", "[strike]")
{
    StrikeState run = RequestStrikeAt(100.0);
    Takes takes;
    REQUIRE_FALSE(AdvanceStrike(run, Ready(), 100.0, takes.Fn()));
    // A block giving way sends an attackStop: not over.
    StrikeSeen blockEnd = Swinging();
    blockEnd.attackStops = 1;
    REQUIRE_FALSE(AdvanceStrike(run, blockEnd, 100.05, takes.Fn()));
    // Into the attack state and out again with no hit frame: cut short.
    blockEnd.attacking = false;
    REQUIRE(Over(AdvanceStrike(run, blockEnd, 100.3, takes.Fn())) == "power attack cut short");
}

TEST_CASE("their own swing, shout or spell, and a target to one side, hold the action", "[strike]")
{
    StrikeState run = RequestStrikeAt(100.0);
    Takes takes;
    REQUIRE_FALSE(AdvanceStrike(run, Swinging(), 100.0, takes.Fn()));
    REQUIRE(run.waited);
    StrikeSeen shouting = Ready();
    shouting.casting = true;
    REQUIRE_FALSE(AdvanceStrike(run, shouting, 100.2, takes.Fn()));
    REQUIRE(takes.asked == 0);
    StrikeSeen aside = Ready();
    aside.facing = false;
    REQUIRE_FALSE(AdvanceStrike(run, aside, 100.5, takes.Fn()));
    REQUIRE(takes.asked == 0);
    REQUIRE_FALSE(AdvanceStrike(run, Ready(), 100.9, takes.Fn()));
    REQUIRE(takes.asked == 1);
    REQUIRE(run.step == StrikeStep::Striking);
}

TEST_CASE("refused, asked again; the deadline names what it waited on", "[strike]")
{
    StrikeState refused = RequestStrikeAt(100.0);
    Takes no;
    no.take = false;
    REQUIRE_FALSE(AdvanceStrike(refused, Ready(), 100.0, no.Fn()));
    REQUIRE_FALSE(AdvanceStrike(refused, Ready(), 100.5, no.Fn()));
    REQUIRE(refused.refusals == 2);
    REQUIRE(Over(AdvanceStrike(refused, Ready(), 102.0, no.Fn())) == "deadline, refused");

    Takes takes;
    StrikeState sheathed = RequestStrikeAt(100.0);
    StrikeSeen out;
    REQUIRE(Over(AdvanceStrike(sheathed, out, 102.0, takes.Fn())) == "deadline, weapon never drawn");
    StrikeState swinging = RequestStrikeAt(100.0);
    REQUIRE_FALSE(AdvanceStrike(swinging, Swinging(), 101.99, takes.Fn()));
    REQUIRE(Over(AdvanceStrike(swinging, Swinging(), 102.0, takes.Fn())) == "deadline, still mid-swing");
    StrikeState casting = RequestStrikeAt(100.0);
    StrikeSeen shouting = Ready();
    shouting.casting = true;
    REQUIRE(Over(AdvanceStrike(casting, shouting, 102.0, takes.Fn())) == "deadline, still mid-cast");
    StrikeState aside = RequestStrikeAt(100.0);
    StrikeSeen turned = Ready();
    turned.facing = false;
    REQUIRE(Over(AdvanceStrike(aside, turned, 102.0, takes.Fn())) == "deadline, target never in front");
    REQUIRE(takes.asked == 0);

    StrikeSeen gone;
    gone.holder = false;
    REQUIRE(Over(AdvanceStrike(aside, gone, 100.0, takes.Fn())) == "holder vanished");
}

TEST_CASE("taken, then watched: never swung, or still swinging", "[strike]")
{
    Takes takes;
    StrikeState quiet = RequestStrikeAt(100.0);
    REQUIRE_FALSE(AdvanceStrike(quiet, Ready(), 100.0, takes.Fn()));
    REQUIRE_FALSE(AdvanceStrike(quiet, Ready(), 102.99, takes.Fn()));
    REQUIRE(Over(AdvanceStrike(quiet, Ready(), 103.0, takes.Fn())) == "taken, never swung");

    StrikeState long_ = RequestStrikeAt(100.0);
    REQUIRE_FALSE(AdvanceStrike(long_, Ready(), 100.0, takes.Fn()));
    REQUIRE_FALSE(AdvanceStrike(long_, Swinging(), 101.0, takes.Fn()));
    REQUIRE(Over(AdvanceStrike(long_, Swinging(), 103.0, takes.Fn())) == "watch over, still swinging");

    // At its hit, and still in the attack state with no end event when the
    // watch is over.
    StrikeState hitLong = RequestStrikeAt(100.0);
    REQUIRE_FALSE(AdvanceStrike(hitLong, Ready(), 100.0, takes.Fn()));
    StrikeSeen hit = Swinging();
    hit.hitFrames = 1;
    REQUIRE_FALSE(AdvanceStrike(hitLong, hit, 100.6, takes.Fn()));
    REQUIRE(hitLong.hitAt == 100.6);
    REQUIRE(Over(AdvanceStrike(hitLong, hit, 103.0, takes.Fn())) == "watch over, hit but still swinging");
}
