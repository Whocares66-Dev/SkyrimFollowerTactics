// One actor's turn, over the production coordinator: the request that
// finished puts its action back on cooldown, the tick chooses a list, the
// snapshot is asked for only when there is one to evaluate, the rules
// decide, and what came of the action is noted. The game's Tick calls the
// same DecideTurn and NoteOutcome for a follower and for the player
// (game/Tactics.cpp, RunTurn); this drives them with the actor's facts and
// the dispatch's answer scripted, so the composition is tested rather than
// the stages alone.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Coordinator.h"

#include <string>
#include <vector>

using namespace ft;

namespace
{

constexpr std::uint32_t kHeal = 0x0002F3B8;

Rule CastHeal()
{
    Rule cast;
    cast.subject = SubjectKind::Self;
    cast.predicate = PredicateKind::Any;
    cast.actionTarget = ActionTargetKind::Self;
    cast.FirstAction().kind = ActionKind::CastSpell;
    cast.FirstAction().form = kHeal;
    cast.label = "cast heal";
    return cast;
}

// What the game reads of the actor, as RuntimeCapabilities and ReadTick
// build it: ours in flight holds every action.
TickFacts Facts(bool fighting, bool busy, bool idleHasRules = false, bool held = false)
{
    TickFacts facts;
    facts.now.fighting = fighting;
    facts.now.held = held;
    facts.now.idleHasRules = idleHasRules;
    facts.busy = busy;
    facts.caps.busy.fill(busy);
    return facts;
}

// One actor under the tick, with the snapshot and the dispatch scripted.
struct Turn
{
    ActorRun run;
    Snapshot base = test::Healthy();
    ActionOutcome answer{ActionOutcome::Performed};
    int snapshots{0};               // how often a snapshot was asked for
    std::vector<std::string> fired; // the rules that acted, in order

    TickResult Tick(const ActorRules &rules, const TickFacts &facts, double now)
    {
        const auto snapshot = [&](Moment) {
            ++snapshots;
            Snapshot s = base;
            s.now = now;
            s.inCombat = facts.now.fighting;
            return s;
        };
        TickResult result = DecideTurn(run, rules, facts, now, snapshot);
        if (result.Fired())
        {
            fired.push_back(result.decision.rule.label);
            NoteOutcome(run, result.decision, answer);
        }
        return result;
    }
};

ActorRules CombatOnly(Rule rule)
{
    ActorRules rules;
    rules.combat.rules.push_back(std::move(rule));
    return rules;
}

} // namespace

TEST_CASE("a requested cast stays in flight, and its cooldown runs from the end", "[coordinator]")
{
    const ActorRules rules = CombatOnly(CastHeal());
    Turn lydia;
    lydia.base.spells.known.push_back(kHeal);
    lydia.answer = ActionOutcome::Requested;

    // The fight's first tick: the cast fires and is in flight.
    TickResult turn = lydia.Tick(rules, Facts(true, false), 100.0);
    REQUIRE(turn);
    REQUIRE(turn.plan.began);
    REQUIRE(turn.Fired());
    REQUIRE(lydia.run.inFlight);
    REQUIRE(lydia.run.inFlight->action.form == kHeal);

    // Mid-cast: every action is held, so nothing fires however long the
    // cast runs -- past the decision's own cooldown too.
    const double minimum = MinimumCooldown(ActionKind::CastSpell);
    for (int step = 1; 100.0 + 0.5 * step <= 100.0 + minimum + 1.0; ++step)
    {
        const double at = 100.0 + 0.5 * step;
        turn = lydia.Tick(rules, Facts(true, true), at);
        REQUIRE_FALSE(turn.Fired());
        REQUIRE(lydia.run.inFlight);
    }

    // Over at 103.5: the cooldown starts from the end, and the next cast
    // waits the minimum from there rather than from the decision.
    const double over = 103.5;
    turn = lydia.Tick(rules, Facts(true, false), over);
    REQUIRE(turn.completed);
    REQUIRE_FALSE(lydia.run.inFlight);
    REQUIRE_FALSE(turn.Fired());
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, false), over + minimum - 0.1).Fired());
    REQUIRE(lydia.Tick(rules, Facts(true, false), over + minimum).Fired());
    REQUIRE(lydia.fired.size() == 2);
}

TEST_CASE("a performed action is done with; a refused one leaves nothing in flight", "[coordinator]")
{
    const ActorRules rules = CombatOnly(test::HealBelow(0.5f, "heal"));
    Turn lydia;
    lydia.base.health = {40.0f, 100.0f};

    REQUIRE(lydia.Tick(rules, Facts(true, false), 100.0).Fired());
    REQUIRE_FALSE(lydia.run.inFlight);
    // The potion's cooldown is the decision's, and runs from 100.
    const double again = 100.0 + MinimumCooldown(ActionKind::DrinkStrongest);
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, false), again - 0.1).Fired());
    REQUIRE(lydia.Tick(rules, Facts(true, false), again).Fired());

    Turn refused;
    refused.base.health = {40.0f, 100.0f};
    refused.answer = ActionOutcome::Failed;
    REQUIRE(refused.Tick(rules, Facts(true, false), 100.0).Fired());
    REQUIRE_FALSE(refused.run.inFlight);
}

TEST_CASE("no list to evaluate, no snapshot read", "[coordinator]")
{
    const ActorRules rules = CombatOnly(test::HealBelow(0.5f, "heal"));
    Turn lydia;
    lydia.base.health = {40.0f, 100.0f};

    // Out of a fight with no idle rules there is nothing to decide, so the
    // inventory is never scanned.
    REQUIRE_FALSE(lydia.Tick(rules, Facts(false, false), 100.0));
    REQUIRE(lydia.snapshots == 0);
    // Held -- bleeding out, or the player in dialogue -- the same, even in
    // a fight.
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, false, false, true), 100.5));
    REQUIRE(lydia.snapshots == 0);
    // Fighting and free: one snapshot, one evaluation.
    REQUIRE(lydia.Tick(rules, Facts(true, false), 101.0));
    REQUIRE(lydia.snapshots == 1);
}

TEST_CASE("a request completes while the actor is held, and the cooldown still restarts", "[coordinator]")
{
    const ActorRules rules = CombatOnly(CastHeal());
    Turn lydia;
    lydia.base.spells.known.push_back(kHeal);
    lydia.answer = ActionOutcome::Requested;
    REQUIRE(lydia.Tick(rules, Facts(true, false), 100.0).Fired());

    // Bleeding out as the cast ends: nothing is evaluated, but the request
    // is over and its cooldown runs from here.
    const TickResult held = lydia.Tick(rules, Facts(true, false, false, true), 102.0);
    REQUIRE(held.completed);
    REQUIRE_FALSE(held);
    REQUIRE_FALSE(lydia.run.inFlight);
    REQUIRE(lydia.snapshots == 1);
    const double minimum = MinimumCooldown(ActionKind::CastSpell);
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, false), 102.0 + minimum - 0.1).Fired());
    REQUIRE(lydia.Tick(rules, Facts(true, false), 102.0 + minimum).Fired());
}

TEST_CASE("a switched-off list hands the actor to the other one, through the whole turn", "[coordinator]")
{
    ActorRules rules = CombatOnly(test::HealBelow(0.5f, "combat heal"));
    rules.idle.moment = Moment::Idle;
    rules.idle.rules.push_back(test::HealBelow(0.5f, "idle heal"));
    Turn lydia;
    lydia.base.health = {40.0f, 100.0f};

    TickFacts off = Facts(true, false, true);
    off.now.combatEnabled = false;
    // In a fight with the combat list off: the idle list is not evaluated
    // in a fight either, so nothing runs and nothing is scanned.
    REQUIRE_FALSE(lydia.Tick(rules, off, 100.0));
    REQUIRE(lydia.snapshots == 0);

    // Out of the fight, still off: the farewell is spent on the silenced
    // list and the idle list has its turn on the same tick.
    TickFacts after = Facts(false, false, true);
    after.now.combatEnabled = false;
    const TickResult turn = lydia.Tick(rules, after, 100.5);
    REQUIRE(turn.plan.list == Moment::Idle);
    REQUIRE_FALSE(turn.plan.ended);
    REQUIRE(turn.Fired());
    REQUIRE(lydia.fired == std::vector<std::string>{"idle heal"});
}

TEST_CASE("two actors take their turns without touching each other's state", "[coordinator]")
{
    const ActorRules rules = CombatOnly(CastHeal());
    Turn lydia;
    Turn jenassa;
    for (Turn *who : {&lydia, &jenassa})
    {
        who->base.spells.known.push_back(kHeal);
        who->answer = ActionOutcome::Requested;
    }

    REQUIRE(lydia.Tick(rules, Facts(true, false), 100.0).Fired());
    // The second actor joins a second later: a first evaluation and a cast
    // of their own, while the first is still mid-cast.
    REQUIRE(jenassa.Tick(rules, Facts(true, false), 101.0).Fired());
    REQUIRE(lydia.run.inFlight);
    REQUIRE(jenassa.run.inFlight);
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, true), 101.0).Fired());

    // One finishing does not free the other.
    REQUIRE(lydia.Tick(rules, Facts(true, false), 102.0).completed);
    REQUIRE_FALSE(jenassa.Tick(rules, Facts(true, true), 102.0).completed);
    REQUIRE(jenassa.run.inFlight);
}

TEST_CASE("the fight's edges through the whole turn, and a fresh session", "[coordinator]")
{
    Rule onBegin = test::HealBelow(2.0f, "on begin");
    onBegin.predicate = PredicateKind::CombatBegins;
    const ActorRules rules = CombatOnly(onBegin);
    Turn lydia;

    REQUIRE(lydia.Tick(rules, Facts(true, false), 100.0).Fired());
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, false), 100.5).Fired());
    // The farewell evaluation, then nothing with no idle rules.
    REQUIRE(lydia.Tick(rules, Facts(false, false), 101.0).plan.ended);
    REQUIRE_FALSE(lydia.Tick(rules, Facts(false, false), 101.5));
    // A second fight begins it again.
    REQUIRE(lydia.Tick(rules, Facts(true, false), 102.0).Fired());
    REQUIRE(lydia.fired.size() == 2);

    // A new session: the standing is fresh, and the next fight begins anew.
    lydia.run = ActorRun{};
    REQUIRE(lydia.Tick(rules, Facts(true, false), 200.0).Fired());
}

TEST_CASE("the idle list's turn says what its rules hold; a fight's does not", "[coordinator]")
{
    // What the game lets go of by (game/Pins.h, LapsePins): asked on every
    // idle turn, whether or not a rule acted, and not in a fight, where the
    // idle rules' pins stand as they are.
    ActorRules rules = CombatOnly(CastHeal());
    Rule shield;
    shield.subject = SubjectKind::Self;
    shield.predicate = PredicateKind::Any;
    shield.actionTarget = ActionTargetKind::Self;
    shield.FirstAction().kind = ActionKind::EquipWeapon;
    shield.FirstAction().form = 0x12EB6;
    shield.FirstAction().hand = Hand::Left;
    rules.idle.moment = Moment::Idle;
    rules.idle.rules.push_back(shield);

    Turn lydia;
    Holdable held;
    held.form = 0x12EB6;
    held.kind = Kind::Weapon;
    held.grip = Grip::LeftOnly;
    lydia.base.loadout.push_back(held);

    const TickResult idle = lydia.Tick(rules, Facts(false, false, true), 100.0);
    REQUIRE(idle.Fired());
    REQUIRE(idle.holds);
    REQUIRE(idle.holds->list == Moment::Idle);
    REQUIRE(idle.holds->wants.pins.size() == 1);
    REQUIRE(idle.holds->wants.pins[0].hands == Hand::Left);

    const TickResult fight = lydia.Tick(rules, Facts(true, false, true), 100.5);
    REQUIRE(fight);
    REQUIRE_FALSE(fight.holds);
}

TEST_CASE("a list with nothing to ask holds nothing, and says so; held, nothing is said", "[coordinator]")
{
    // Switched off, or without rules, a list's rules hold nothing, so its
    // pins are let go: an empty word, of the list the moment is. Held --
    // bleeding out, or the switch over all lists off -- nothing is asked,
    // and the pins stand until the actor is free.
    const ActorRules rules = CombatOnly(CastHeal());
    Turn lydia;

    // Out of a fight with no idle rules: the idle list's, empty.
    const TickResult quiet = lydia.Tick(rules, Facts(false, false), 100.0);
    REQUIRE_FALSE(quiet);
    REQUIRE(quiet.holds);
    REQUIRE(quiet.holds->list == Moment::Idle);
    REQUIRE(quiet.holds->wants.pins.empty());
    REQUIRE(quiet.holds->wants.holes.empty());

    // In a fight with the combat list switched off: the combat list's.
    TickFacts off = Facts(true, false);
    off.now.combatEnabled = false;
    const TickResult silenced = lydia.Tick(rules, off, 100.5);
    REQUIRE_FALSE(silenced);
    REQUIRE(silenced.holds);
    REQUIRE(silenced.holds->list == Moment::Combat);

    // Held, in a fight or out of one: nothing.
    REQUIRE_FALSE(lydia.Tick(rules, Facts(true, false, false, true), 101.0).holds);
    REQUIRE_FALSE(lydia.Tick(rules, Facts(false, false, true, true), 101.5).holds);
}
