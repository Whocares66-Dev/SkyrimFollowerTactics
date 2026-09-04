// These tests run with no Skyrim, no SKSE, and no CommonLibSSE.
// That is the whole point -- see docs/PLAN.md section 3.

#include <catch2/catch_test_macros.hpp>

#include "core/Evaluator.h"
#include "core/Vocabulary.h"

#include <string>

using namespace ft;

namespace
{

Snapshot Healthy()
{
    Snapshot s;
    s.self = 0xA2C94;
    s.now = 100.0;
    s.health = {100.0f, 100.0f};
    s.magicka = {100.0f, 100.0f};
    s.stamina = {100.0f, 100.0f};
    s.inCombat = true;
    s.playerHealth = {100.0f, 100.0f};
    s.potions.healthCount = 5;
    return s;
}

// The marquee rule: IF self health below <pct> THEN drink a health potion.
Rule HealBelow(float pct)
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = pct;
    r.actionTarget = ActionTargetKind::ConditionSubject;
    r.FirstAction().kind = ActionKind::DrinkHealthPotion;
    r.label = "heal";
    return r;
}

} // namespace

TEST_CASE("an empty rule set does nothing", "[evaluator]")
{
    RuleSet rs;
    EvalContext ctx;
    const auto d = Evaluate(rs, Healthy(), ctx);
    REQUIRE_FALSE(d.Fired());
}

TEST_CASE("the marquee rule: health below 50% drinks a potion", "[evaluator]")
{
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));
    EvalContext ctx;

    SECTION("does not fire at full health")
    {
        Trace trace;
        const auto d = Evaluate(rs, Healthy(), ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::ConditionFalse);
    }

    SECTION("fires below the threshold, targeting the subject of the condition")
    {
        Snapshot s = Healthy();
        s.health = {40.0f, 100.0f};

        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE(d.Fired());
        REQUIRE(d.action() == ActionKind::DrinkHealthPotion);
        REQUIRE(d.targetId() == s.self);
        REQUIRE(trace.at(0) == Verdict::Fired);
    }

    SECTION("will not fire without a potion in the inventory")
    {
        Snapshot s = Healthy();
        s.health = {40.0f, 100.0f};
        s.potions.healthCount = 0;

        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::NoResource);
    }
}

TEST_CASE("order decides: the first matching rule wins", "[evaluator]")
{
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.9f)); // broad, listed first
    rs.rules.push_back(HealBelow(0.5f)); // narrow, shadowed

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;
    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);

    REQUIRE(d.ruleIndex == 0);
    REQUIRE(trace.at(1) == Verdict::NotReached);
}

TEST_CASE("a disabled rule is skipped and the next one gets a turn", "[evaluator]")
{
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.9f));
    rs.rules.back().enabled = false;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;
    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);

    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::Disabled);
}

TEST_CASE("a cooldown belongs to the action, not to the rule's position", "[evaluator]")
{
    // Two rules on DIFFERENT conditions and different actions. The potion
    // fires; then the list is reordered. The potion's cooldown must follow the
    // potion action wherever its rule now sits, and must not land on the cast
    // rule that moved into its old slot.
    constexpr std::uint32_t kHeal = 0x0002F3B8;

    Rule potion = HealBelow(0.5f);
    Rule cast;
    cast.subject = SubjectKind::Self;
    cast.predicate = PredicateKind::InCombat;
    cast.actionTarget = ActionTargetKind::Self;
    cast.FirstAction().kind = ActionKind::CastSpell;
    cast.FirstAction().form = kHeal;
    cast.label = "cast";

    RuleSet rs;
    rs.rules.push_back(potion);
    rs.rules.push_back(cast);

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.spells.known.push_back(kHeal);

    EvalContext ctx;
    ctx.caps = Capabilities::All();

    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0); // potion fires

    std::swap(rs.rules[0], rs.rules[1]); // cast is now index 0, potion index 1
    s.now += 0.1;

    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0); // the cast rule, unblocked, fires from its new slot
    REQUIRE(d.action() == ActionKind::CastSpell);
    REQUIRE(trace.at(1) == Verdict::NotReached);

    // And the potion's cooldown is on the ACTION, wherever its rule now sits.
    REQUIRE(ctx.BlockedUntil({ActionKind::DrinkHealthPotion, 0, s.self}) > s.now);
    REQUIRE(ctx.BlockedUntil({ActionKind::CastSpell, kHeal, s.self}) > s.now);
}

TEST_CASE("a cooldown is as fine as the spell and the target", "[cooldown]")
{
    // Heal on self, heal on the player, and Oakflesh on self are three
    // different things. Only a repeat of the SAME thing on the SAME actor is
    // held back.
    constexpr std::uint32_t kHeal = 0x0002F3B8;
    constexpr std::uint32_t kOakflesh = 0x0005AD5C;

    auto castRule = [](std::uint32_t spell, ActionTargetKind target) {
        Rule r;
        r.subject = SubjectKind::Self;
        r.predicate = PredicateKind::InCombat;
        r.actionTarget = target;
        r.FirstAction().kind = ActionKind::CastSpell;
        r.FirstAction().form = spell;
        return r;
    };

    RuleSet rs;
    rs.rules.push_back(castRule(kHeal, ActionTargetKind::Self));     // 0
    rs.rules.push_back(castRule(kHeal, ActionTargetKind::Player));   // 1
    rs.rules.push_back(castRule(kOakflesh, ActionTargetKind::Self)); // 2

    Snapshot s = Healthy();
    s.spells.known.push_back(kHeal);
    s.spells.known.push_back(kOakflesh);

    EvalContext ctx;
    ctx.caps = Capabilities::All();

    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0); // heal self

    s.now += 0.5;
    Trace trace;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 1); // heal player: same spell, other target
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    s.now += 0.5;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 2); // Oakflesh: other spell, same target
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    REQUIRE(trace.at(1) == Verdict::ActionCooldown);

    s.now += MinimumCooldown(ActionKind::CastSpell);
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0); // heal self is back
}

TEST_CASE("an unsupported action never fires", "[evaluator]")
{
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;
    ctx.caps.supported.fill(false);

    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::Unsupported);
}

// ---------------------------------------------------------------------------
// The subject/predicate split, and the binding it produces.
// ---------------------------------------------------------------------------

TEST_CASE("a group condition binds the member that best satisfies it", "[binding]")
{
    Snapshot s = Healthy();
    //                   id     health          dist   casting atkPlayer LOS
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f, false, false, true});
    s.enemies.push_back({0x102, {10.0f, 100.0f}, 900.0f, false, false, true});

    SECTION("a health predicate binds the weakest match, not the nearest")
    {
        Rule r;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::HealthPctBelow;
        r.conditionArg = 0.6f; // both qualify

        const auto b = EvaluateCondition(r, s);
        REQUIRE(b.ok);
        REQUIRE(b.id == 0x102); // 10% health, though it is further away
    }

    SECTION("an above predicate binds the healthiest match")
    {
        Rule r;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::HealthPctAbove;
        r.conditionArg = 0.05f; // both qualify

        const auto b = EvaluateCondition(r, s);
        REQUIRE(b.ok);
        REQUIRE(b.id == 0x101); // 50% health

        r.conditionArg = 0.6f; // neither
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    }

    SECTION("a distance predicate binds the nearest match")
    {
        Rule r;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::WithinDistance;
        r.conditionArg = 1000.0f; // both qualify

        const auto b = EvaluateCondition(r, s);
        REQUIRE(b.ok);
        REQUIRE(b.id == 0x101);
    }

    SECTION("no member satisfying the predicate means no match")
    {
        Rule r;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::HealthPctBelow;
        r.conditionArg = 0.05f;

        REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    }
}

TEST_CASE("the action follows the binding of the condition by default", "[binding]")
{
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f, false, false, true});
    s.enemies.push_back({0x102, {10.0f, 100.0f}, 900.0f, false, false, true});

    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.2f;
    r.actionTarget = ActionTargetKind::ConditionSubject;
    r.FirstAction().kind = ActionKind::StopCombat;
    rs.rules.push_back(r);

    EvalContext ctx;
    const auto d = Evaluate(rs, s, ctx);

    REQUIRE(d.Fired());
    // The whole point of the split: the enemy is named once, in the condition,
    // and the action lands on that same enemy.
    REQUIRE(d.targetId() == 0x102);
}

TEST_CASE("the action target can be overridden away from the subject", "[binding]")
{
    RuleSet rs;
    Rule r = HealBelow(0.5f);
    r.actionTarget = ActionTargetKind::Player;
    rs.rules.push_back(r);

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;
    const auto d = Evaluate(rs, s, ctx);

    REQUIRE(d.Fired());
    REQUIRE(d.targetId() == kPlayerFormID);
}

TEST_CASE("ally conditions bind the ally, not the follower", "[binding]")
{
    Snapshot s = Healthy();
    //                  id     health          dist   bleedout
    s.allies.push_back({0x201, {80.0f, 100.0f}, 100.0f, false});
    s.allies.push_back({0x202, {20.0f, 100.0f}, 500.0f, false});

    Rule r;
    r.subject = SubjectKind::Ally;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.5f;

    const auto b = EvaluateCondition(r, s);
    REQUIRE(b.ok);
    REQUIRE(b.id == 0x202);
}

TEST_CASE("CountAtLeast asks about the group and binds the nearest member", "[binding]")
{
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 900.0f, false, false, true});
    s.enemies.push_back({0x102, {50.0f, 100.0f}, 300.0f, false, false, true});

    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::CountAtLeast;

    SECTION("holds when the group is big enough")
    {
        r.conditionArg = 2.0f;
        const auto b = EvaluateCondition(r, s);
        REQUIRE(b.ok);
        REQUIRE(b.id == 0x102); // nearest
    }

    SECTION("fails when it is not")
    {
        r.conditionArg = 3.0f;
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    }
}

TEST_CASE("CurrentTarget needs sensed data for anything beyond existence", "[binding]")
{
    Snapshot s = Healthy();
    s.currentTarget = 0x101;

    SECTION("Always binds on existence alone")
    {
        Rule r;
        r.subject = SubjectKind::CurrentTarget;
        r.predicate = PredicateKind::Any;
        REQUIRE(EvaluateCondition(r, s).id == 0x101);
    }

    SECTION("a health predicate needs the target in the enemy list")
    {
        Rule r;
        r.subject = SubjectKind::CurrentTarget;
        r.predicate = PredicateKind::HealthPctBelow;
        r.conditionArg = 0.9f;

        REQUIRE_FALSE(EvaluateCondition(r, s).ok); // not sensed yet

        s.enemies.push_back({0x101, {10.0f, 100.0f}, 200.0f, false, false, true});
        REQUIRE(EvaluateCondition(r, s).ok);
    }
}

TEST_CASE("no binding means the rule is skipped, not fired at nobody", "[evaluator]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::ConditionSubject;
    r.FirstAction().kind = ActionKind::StopCombat;
    rs.rules.push_back(r);

    EvalContext ctx;
    Trace trace;
    const auto d = Evaluate(rs, Healthy(), ctx, &trace); // no enemies

    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);
}

// ---------------------------------------------------------------------------
// Validity: an unanswerable pair is an authoring error, not a false condition.
// ---------------------------------------------------------------------------

TEST_CASE("the subject and predicate validity matrix", "[validity]")
{
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::MagickaPctBelow));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::CountAtLeast));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::InBleedout));

    // Distance to oneself is meaningless, and a lone subject has no count.
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::WithinDistance));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::CountAtLeast));

    // The Snapshot carries no magicka or stamina for anyone but the follower.
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::MagickaPctBelow));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::StaminaPctBelow));

    // Above is answerable exactly where below is.
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::MagickaPctAbove));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::HealthPctAbove));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::MagickaPctAbove));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::StaminaPctAbove));
    REQUIRE(AboveOf(PredicateKind::HealthPctBelow) == PredicateKind::HealthPctAbove);
    REQUIRE(AboveOf(PredicateKind::InCombat) == PredicateKind::InCombat);
}

TEST_CASE("the edges of a fight hold for one evaluation each", "[evaluator]")
{
    Rule onBegin;
    onBegin.subject = SubjectKind::Self;
    onBegin.predicate = PredicateKind::CombatBegins;
    onBegin.actionTarget = ActionTargetKind::Self;
    onBegin.FirstAction().kind = ActionKind::HoldPosition;

    Rule always = onBegin;
    always.predicate = PredicateKind::Any;
    always.FirstAction().kind = ActionKind::Flee;

    Rule onEnd = onBegin;
    onEnd.predicate = PredicateKind::CombatEnds;
    onEnd.FirstAction().kind = ActionKind::StopCombat;

    RuleSet rs;
    rs.rules = {onBegin, always, onEnd};
    EvalContext ctx;
    Trace trace;

    // The first evaluation of a fight: begins holds, and wins by order.
    Snapshot s = Healthy();
    s.combatBegan = true;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);

    // The next: begins has passed, and the standing rule has its turn.
    s.combatBegan = false;
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);

    // The farewell pass: only ends holds. The standing rule is false on it,
    // so it cannot re-pin what the after-fight restore just put back.
    s.inCombat = false;
    s.combatEnded = true;
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 2);
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);
    REQUIRE(trace.at(1) == Verdict::ConditionFalse);

    // Only the follower's own fight has edges.
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::CombatEnds));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::CombatBegins));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::CombatEnds));
}

TEST_CASE("a rule does its actions one per tick, in order, and waits rather than yields mid-list", "[sequence]")
{
    constexpr std::uint32_t kHeal = 0x00012FCC;

    // One rule, three actions: drink, cast, hold. The whole list is what
    // the player asked for, in that order, one per tick, before anything
    // else is decided.
    Rule r = HealBelow(0.5f);
    r.actions.push_back({ActionKind::CastSpell, kHeal});
    r.actions.push_back({ActionKind::HoldPosition});
    Rule other = HealBelow(0.5f);
    other.actionTarget = ActionTargetKind::Self;
    other.FirstAction().kind = ActionKind::Flee;

    RuleSet rs;
    rs.rules = {r, other};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.spells.known.push_back(kHeal);
    EvalContext ctx;

    Trace trace;
    ActionTrace actions;
    Decision d = Evaluate(rs, s, ctx, &trace, &actions);
    // Tick one: the potion, and only the potion; the rest waits.
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.steps.size() == 1);
    REQUIRE(d.action() == ActionKind::DrinkHealthPotion);
    REQUIRE(trace.at(0) == Verdict::Fired);
    REQUIRE(actions.at(0) == std::vector<Verdict>{Verdict::Fired, Verdict::NotReached, Verdict::NotReached});
    REQUIRE(trace.at(1) == Verdict::NotReached);
    REQUIRE(ctx.pending.Active());

    // Tick two: the cast pool is busy. The rule owns the tick and waits;
    // the second rule does not get it even though its condition holds.
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = true;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::Busy);
    REQUIRE(actions.at(0) == std::vector<Verdict>{Verdict::NotReached, Verdict::Busy, Verdict::NotReached});
    REQUIRE(trace.at(1) == Verdict::NotReached);

    // Tick three: the pool frees, the cast goes.
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = false;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.steps.size() == 1);
    REQUIRE(d.action() == ActionKind::CastSpell);
    REQUIRE(ctx.pending.Active());

    // Tick four: the hold, and the list is through.
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.steps.size() == 1);
    REQUIRE(d.action() == ActionKind::HoldPosition);
    REQUIRE(actions.at(0) == std::vector<Verdict>{Verdict::NotReached, Verdict::NotReached, Verdict::Fired});
    REQUIRE_FALSE(ctx.pending.Active());

    // Tick five: everything on cooldown. The first action is merely blocked
    // for the moment, so the rule has not begun and yields to the next one.
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    REQUIRE_FALSE(ctx.pending.Active());
}

TEST_CASE("a list whose remainder cannot be done is through, and the tick goes on", "[sequence]")
{
    constexpr std::uint32_t kHeal = 0x00012FCC;

    // Drink, then cast. The potion goes; next tick the cast is
    // unaffordable -- a cannot, not a not-yet -- so it is skipped, the list
    // is through, and the next rule gets the same tick.
    Rule r = HealBelow(0.5f);
    r.actions.push_back({ActionKind::CastSpell, kHeal});
    Rule other = HealBelow(0.5f);
    other.actionTarget = ActionTargetKind::Self;
    other.FirstAction().kind = ActionKind::Flee;
    RuleSet rs;
    rs.rules = {r, other};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.spells.known.push_back(kHeal);
    s.spells.costs.push_back({kHeal, 80.0f});
    s.magicka = {10.0f, 100.0f};
    EvalContext ctx;

    Trace trace;
    ActionTrace actions;
    Decision d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.action() == ActionKind::DrinkHealthPotion);
    REQUIRE(ctx.pending.Active());

    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(d.action() == ActionKind::Flee);
    REQUIRE_FALSE(ctx.pending.Active());
    // The first rule was re-read from the top on the same tick: the potion
    // is inside its settle.
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
}

TEST_CASE("a list in progress is dropped when the fight ends", "[sequence]")
{
    Rule r = HealBelow(0.5f);
    r.actions.push_back({ActionKind::HoldPosition});
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    EvalContext ctx;
    REQUIRE(Evaluate(rs, s, ctx).steps.size() == 1);
    REQUIRE(ctx.pending.Active());

    s.combatEnded = true;
    s.inCombat = false;
    REQUIRE_FALSE(Evaluate(rs, s, ctx).Fired());
    REQUIRE_FALSE(ctx.pending.Active());
}

TEST_CASE("above and below are the same number from either side", "[evaluator]")
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::MagickaPctAbove;
    r.conditionArg = 0.5f;

    Snapshot s = Healthy();
    s.magicka = {80.0f, 100.0f};
    REQUIRE(EvaluateCondition(r, s).ok);
    s.magicka = {20.0f, 100.0f};
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // Exactly at the line is neither above nor below.
    s.magicka = {50.0f, 100.0f};
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    r.predicate = PredicateKind::MagickaPctBelow;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    r.subject = SubjectKind::Player;
    r.predicate = PredicateKind::HealthPctAbove;
    s.playerHealth = {90.0f, 100.0f};
    REQUIRE(EvaluateCondition(r, s).ok);
}

TEST_CASE("an unanswerable pair reports InvalidCondition, not ConditionFalse", "[validity]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::WithinDistance; // nonsense
    r.conditionArg = 100.0f;
    r.FirstAction().kind = ActionKind::StopCombat;
    rs.rules.push_back(r);

    EvalContext ctx;
    Trace trace;
    const auto d = Evaluate(rs, Healthy(), ctx, &trace);

    REQUIRE_FALSE(d.Fired());
    // Distinguishing these two is the difference between "your rule is broken"
    // and "your rule is fine but the world is not in that state right now".
    REQUIRE(trace.at(0) == Verdict::InvalidCondition);
}

// ---------------------------------------------------------------------------
// Cooldowns: spacing that belongs to the situation and to the remedy, not only
// to the rule the author wrote.
// ---------------------------------------------------------------------------

namespace
{

// A rule sharing the "self health below pct" condition but applying a different
// remedy -- the shape of "drink a potion / cast a heal / eat food".
Rule HurtBut(float pct, ActionKind action, const char *label)
{
    Rule r = HealBelow(pct);
    r.FirstAction().kind = action;
    r.label = label;
    return r;
}

} // namespace

TEST_CASE("one situation draws its remedies in list order, one per turn", "[cooldown]")
{
    // Three rules, one problem. The list is a preference order: the potion
    // first, and if the next turn still finds her hurt, the next remedy. Each
    // action carries its own cooldown; nothing is keyed by the condition.
    RuleSet rs;
    rs.rules.push_back(HurtBut(0.25f, ActionKind::DrinkHealthPotion, "potion"));
    rs.rules.push_back(HurtBut(0.25f, ActionKind::DrinkMagickaPotion, "heal spell"));
    rs.rules.push_back(HurtBut(0.25f, ActionKind::Flee, "back off"));

    Snapshot s = Healthy();
    s.health = {20.0f, 100.0f};
    s.potions.magickaCount = 5;

    EvalContext ctx;

    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    // Next turn, still hurt: the potion is on ITS cooldown and says so; the
    // second remedy is free and fires.
    s.now += 0.5;
    Trace trace;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    // Both potions used: the third remedy gets its turn.
    s.now += 0.5;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 2);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    REQUIRE(trace.at(1) == Verdict::ActionCooldown);

    // Once the potion has had time to work, it is available again.
    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion);
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
}

TEST_CASE("an unavailable action falls through immediately, in the same tick", "[cooldown]")
{
    // "No potions? Then try the next thing." Being unable to act is NOT a
    // cooldown: nothing was done, so nothing needs time to settle, and the next
    // remedy for the same problem should be tried at once rather than after a
    // wait. This is the distinction the condition cooldown must not blur.
    RuleSet rs;
    rs.rules.push_back(HurtBut(0.5f, ActionKind::DrinkHealthPotion, "potion"));
    rs.rules.push_back(HurtBut(0.5f, ActionKind::Flee, "back off"));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.potions.healthCount = 0; // the bag is empty

    EvalContext ctx;
    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);

    REQUIRE(d.ruleIndex == 1); // same tick, no waiting
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(trace.at(1) == Verdict::Fired);
}

TEST_CASE("two rules sharing an action cannot repeat it back to back", "[cooldown]")
{
    // Different situations, same remedy: "I am hurt" and "I am carrying plenty
    // of potions" are not the same condition, so the condition cooldown does not
    // apply -- but drinking twice in consecutive ticks still must not happen.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Rule spare;
    spare.subject = SubjectKind::Self;
    spare.predicate = PredicateKind::InCombat;
    spare.FirstAction().kind = ActionKind::DrinkHealthPotion;
    spare.label = "top up while fighting";
    rs.rules.push_back(spare);

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;

    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    s.now += 0.15;
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(1) == Verdict::ActionCooldown);

    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion);
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

TEST_CASE("a different situation is still free to draw a response", "[cooldown]")
{
    // Responding to low health must not silence an unrelated concern.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Rule swarmed;
    swarmed.subject = SubjectKind::Enemy;
    swarmed.predicate = PredicateKind::CountAtLeast;
    swarmed.conditionArg = 2.0f;
    swarmed.actionTarget = ActionTargetKind::Self;
    swarmed.FirstAction().kind = ActionKind::HoldPosition;
    swarmed.label = "back off when swarmed";
    rs.rules.push_back(swarmed);

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f, false, false, true});
    s.enemies.push_back({0x102, {50.0f, 100.0f}, 400.0f, false, false, true});

    EvalContext ctx;

    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0); // drinks
    s.now += 0.15;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 1); // different problem, acts anyway
}

TEST_CASE("a rule whose action is already in effect starves the rules below it", "[cooldown]")
{
    // Documents a constraint rather than a feature, because the obvious guess
    // is wrong. Two different standing responses to one persistent situation --
    //     enemy nearby -> hold position
    //     enemy nearby -> break off
    // -- cannot both happen by tuning cooldowns. Rules are first-match-wins, so
    // while the first is available it wins every time; a zero settle makes it
    // re-fire every tick and a non-zero one only slows the monopoly.
    //
    // The fix is availability, not spacing: an action already in effect must
    // report itself unavailable, and evaluation then falls through. Phase 4's
    // state-setting actions each owe that check. Until they have it, this is
    // what happens.
    RuleSet rs;

    Rule hold;
    hold.subject = SubjectKind::Enemy;
    hold.predicate = PredicateKind::WithinDistance;
    hold.conditionArg = 1000.0f;
    hold.actionTarget = ActionTargetKind::Self;
    hold.FirstAction().kind = ActionKind::HoldPosition;
    hold.label = "brace: hold position";
    rs.rules.push_back(hold);

    Rule disengage = hold;
    disengage.FirstAction().kind = ActionKind::StopCombat;
    disengage.label = "brace: break off";
    rs.rules.push_back(disengage);

    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f, false, false, true});

    EvalContext ctx;

    // Rule 0 wins now, and keeps winning every time it comes off cooldown.
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
    s.now += MinimumCooldown(ActionKind::HoldPosition) + 0.01;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    // Make rule 0's action unavailable -- which is what an "already in effect"
    // check will do -- and rule 1 gets its turn. Note the wait: rule 0's fire
    // also blocked the condition they share, so the settle has to elapse first.
    // Availability decides WHO acts; the cooldown decides WHEN.
    ctx.caps.supported[static_cast<std::size_t>(ActionKind::HoldPosition)] = false;
    s.now += MinimumCooldown(ActionKind::HoldPosition) + 0.01;

    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::Unsupported);
}

TEST_CASE("a busy action is skipped without spending a cooldown", "[capabilities]")
{
    // Two rules on the same condition. The cast pool is exhausted for one
    // evaluation: the cast rule must be skipped WITHOUT counting as fired, the
    // potion rule must get its turn, and once the pool clears the cast rule
    // fires as if nothing had happened. A skipped tactic is not a used tactic.
    constexpr std::uint32_t kOakflesh = 0x0005AD5C;

    RuleSet rs;
    Rule cast;
    cast.subject = SubjectKind::Self;
    cast.predicate = PredicateKind::HealthPctBelow;
    cast.conditionArg = 0.9f;
    cast.actionTarget = ActionTargetKind::Self;
    cast.FirstAction().kind = ActionKind::CastSpell;
    cast.FirstAction().form = kOakflesh;
    rs.rules.push_back(cast);

    Rule potion = cast;
    potion.FirstAction().kind = ActionKind::DrinkHealthPotion;
    potion.FirstAction().form = 0;
    rs.rules.push_back(potion);

    Snapshot s = Healthy();
    s.health = {50.0f, 100.0f};
    s.spells.known.push_back(kOakflesh);

    EvalContext ctx;
    ctx.caps = Capabilities::All();
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = true;

    Trace trace;
    const auto skipped = Evaluate(rs, s, ctx, &trace);
    REQUIRE(skipped.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::Busy);

    // The pool clears. The potion's fire blocked the condition both rules
    // share, so that settle has to elapse -- but the cast rule itself must
    // carry NO cooldown from having been skipped.
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = false;
    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion) + 0.01;
    const auto fired = Evaluate(rs, s, ctx, &trace);
    REQUIRE(fired.ruleIndex == 0);
    REQUIRE(trace.at(0) == Verdict::Fired);
}

TEST_CASE("a cast she cannot afford is reported and spends no cooldown", "[resources]")
{
    constexpr std::uint32_t kHeal = 0x0002F3B8;

    RuleSet rs;
    Rule cast;
    cast.subject = SubjectKind::Self;
    cast.predicate = PredicateKind::HealthPctBelow;
    cast.conditionArg = 0.9f;
    cast.actionTarget = ActionTargetKind::Self;
    cast.FirstAction().kind = ActionKind::CastSpell;
    cast.FirstAction().form = kHeal;
    rs.rules.push_back(cast);

    Rule potion = cast;
    potion.FirstAction().kind = ActionKind::DrinkHealthPotion;
    potion.FirstAction().form = 0;
    rs.rules.push_back(potion);

    Snapshot s = Healthy();
    s.health = {50.0f, 100.0f};
    s.magicka = {30.0f, 100.0f};
    s.spells.known.push_back(kHeal);
    s.spells.costs.push_back({kHeal, 60.0f});

    EvalContext ctx;
    ctx.caps = Capabilities::All();

    // 30 magicka against a 60-point spell: the cast rule is reported, the
    // potion rule fires instead.
    Trace trace;
    const auto d1 = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d1.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::CannotAfford);

    // Magicka back, potion settle elapsed: the cast rule fires at once. It
    // must carry no cooldown from having been unaffordable.
    s.magicka = {100.0f, 100.0f};
    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion) + 0.01;
    const auto d2 = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d2.ruleIndex == 0);
    REQUIRE(trace.at(0) == Verdict::Fired);

    // A spell with no recorded cost is never blocked on this ground.
    Snapshot t = Healthy();
    t.health = {50.0f, 100.0f};
    t.magicka = {0.0f, 100.0f};
    t.spells.known.push_back(kHeal);
    EvalContext ctx2;
    ctx2.caps = Capabilities::All();
    REQUIRE(Evaluate(rs, t, ctx2).ruleIndex == 0);
}

TEST_CASE("a named potion is drunk only while carried, and cools down per potion", "[potions]")
{
    constexpr std::uint32_t kStamina = 0x00039BE8;
    constexpr std::uint32_t kResistFire = 0x0003EB3E;

    auto drink = [](std::uint32_t form) {
        Rule r;
        r.subject = SubjectKind::Self;
        r.predicate = PredicateKind::InCombat;
        r.actionTarget = ActionTargetKind::Self;
        r.FirstAction().kind = ActionKind::DrinkPotion;
        r.FirstAction().form = form;
        return r;
    };

    RuleSet rs;
    rs.rules.push_back(drink(kStamina));
    rs.rules.push_back(drink(kResistFire));

    Snapshot s = Healthy();
    EvalContext ctx;
    ctx.caps = Capabilities::All();

    // Carries neither: both report it, nothing fires.
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(trace.at(1) == Verdict::NoResource);
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::DrinkPotion)) == "does not carry that potion");

    // Carries both: the first fires, and its cooldown is its own -- the
    // second potion fires on the next turn.
    s.potions.carried.push_back({kStamina, 6});
    s.potions.carried.push_back({kResistFire, 1});
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
    s.now += 0.5;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
}

TEST_CASE("a verdict is worded for the action it happened to", "[vocabulary]")
{
    // The log said "previous dose still active" about an EQUIP rule, which is
    // true of nothing and sent a reader looking for a potion that was never in
    // the rule. Same verdict, different action, different sentence.
    REQUIRE(std::string(Explain(Verdict::EffectActive, ActionKind::DrinkHealthPotion)) == "previous dose still active");
    REQUIRE(std::string(Explain(Verdict::EffectActive, ActionKind::EquipSpell)) ==
            "already pinned, or nothing of that kind pinned to let go");

    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::DrinkHealthPotion)) == "no potion");
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EquipSpell)) == "does not know that spell");
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EquipWeapon)) == "does not carry that weapon");

    // Everything else is action-independent and must not drift from ToString.
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto action = static_cast<ActionKind>(i);
        REQUIRE(std::string(Explain(Verdict::ConditionFalse, action)) ==
                std::string(ToString(Verdict::ConditionFalse)));
        REQUIRE(std::string(Explain(Verdict::ActionCooldown, action)) ==
                std::string(ToString(Verdict::ActionCooldown)));
    }
}

TEST_CASE("a spell the follower does not know is not castable", "[spell]")
{
    // Distinct from EffectActive on purpose: "never learned it" and "learned
    // it, already has it up" are the same silence in game, and the status
    // column has to tell them apart or a mis-set rule looks like a bug.
    RuleSet rs;
    Rule buff;
    buff.subject = SubjectKind::Self;
    buff.predicate = PredicateKind::Any;
    buff.actionTarget = ActionTargetKind::Self;
    buff.FirstAction().kind = ActionKind::CastSpell;
    buff.FirstAction().form = 0x0005AD5C;
    rs.rules.push_back(buff);

    Snapshot s = Healthy();
    EvalContext ctx;

    Trace trace;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // A cast rule with no spell chosen is the same kind of unusable, and is
    // what a freshly added rule looks like before it is filled in.
    rs.rules[0].FirstAction().form = 0;
    s.spells.known.push_back(0x0005AD5C);
    Trace blank;
    REQUIRE(Evaluate(rs, s, ctx, &blank).ruleIndex < 0);
    REQUIRE(blank.at(0) == Verdict::NoResource);
}

namespace
{

constexpr std::uint32_t kSword = 0x00012EB7;
constexpr std::uint32_t kBow = 0x00013985;
constexpr std::uint32_t kShield = 0x00012EB6;
constexpr std::uint32_t kHelmet = 0x00012E4D;
constexpr std::uint32_t kArrows = 0x0001397D;
constexpr std::uint32_t kFirebolt = 0x00012FCD;
constexpr std::uint32_t kChainLightning = 0x00045F9D;

Holdable Held(std::uint32_t form, Kind kind, Grip grip, std::uint32_t slots = 0)
{
    Holdable h;
    h.form = form;
    h.kind = kind;
    h.grip = grip;
    h.slots = slots;
    return h;
}

// A follower with a sword, a bow, a shield, a helmet, arrows, and two spells,
// one of them above her skill. Nothing pinned.
Snapshot Armed()
{
    Snapshot s = Healthy();
    s.loadout.push_back(Held(kSword, Kind::Weapon, Grip::Either));
    s.loadout.push_back(Held(kBow, Kind::Weapon, Grip::Both));
    s.loadout.push_back(Held(kShield, Kind::Weapon, Grip::LeftOnly));
    s.loadout.push_back(Held(kHelmet, Kind::Armor, Grip::None, 0x2));
    s.loadout.push_back(Held(kArrows, Kind::Ammo, Grip::None));
    s.loadout.push_back(Held(kFirebolt, Kind::Spell, Grip::Either));
    Holdable chain = Held(kChainLightning, Kind::Spell, Grip::Either);
    chain.unusable = true;
    s.loadout.push_back(chain);
    return s;
}

Rule Equip(ActionKind action, std::uint32_t form, Hand hand = Hand::None)
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = action;
    r.FirstAction().form = form;
    r.FirstAction().hand = hand;
    return r;
}

// What the game side does when a pin rule fires: the pin book gains the
// thing, in the hands the decision names, and whatever it displaces goes.
void Pinned(Snapshot &s, const Decision &d)
{
    const Holdable *thing = FindHoldable(s.loadout, d.actionForm());
    REQUIRE(thing != nullptr);
    const Hand hands = thing->grip == Grip::None ? Hand::None : HandsFor(thing->grip, d.hand());
    [[maybe_unused]] const auto displaced = MakeRoom(s.pins, *thing, hands);
    AddPin(s.pins, *thing, hands, false);
}

} // namespace

TEST_CASE("an equip rule pins once, reports done, and lets the rules beneath it through", "[equip]")
{
    // The shield and the helm: two complementary preparations for one
    // standing situation. Rules are first-match-wins, so the only way both
    // happen is for the shield rule to report itself done once the shield is
    // pinned, and fall through -- see the note in Rule.h.
    RuleSet rs;
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, kShield, Hand::Left));
    rs.rules.push_back(Equip(ActionKind::EquipArmor, kHelmet));

    Snapshot s = Armed();
    EvalContext ctx;

    Decision d = Evaluate(rs, s, ctx);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.action() == ActionKind::EquipWeapon);
    REQUIRE(d.actionForm() == kShield);
    REQUIRE(d.hand() == Hand::Left);
    Pinned(s, d);

    // The shield is pinned, so the helm gets its turn on the next tick.
    s.now += 0.5;
    Trace trace;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    Pinned(s, d);

    // Both pinned: both done, nothing fires, and nothing re-fires however
    // long the cooldowns have been over.
    s.now += 100.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(trace.at(1) == Verdict::EffectActive);

    // A thing the AI happens to hold is not done: only a pin is.
    s.pins.clear();
    s.spells.equipped.push_back(kFirebolt);
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
}

TEST_CASE("a satisfied equip rule holds its hand against the rules beneath it", "[equip]")
{
    // The sword up close, the bow otherwise. Written the natural way -- a
    // specific rule above a catch-all -- and without this the two would
    // trade places every tick: the sword rule pins the sword, falls through
    // as done, the bow rule takes both hands, the sword rule is undone and
    // fires again. Priority means the rule above keeps what it holds for as
    // long as its condition holds.
    RuleSet rs;
    Rule close = Equip(ActionKind::EquipWeapon, kSword, Hand::Right);
    close.subject = SubjectKind::Enemy;
    close.predicate = PredicateKind::WithinDistance;
    close.conditionArg = 300.0f;
    rs.rules.push_back(close);
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, kBow, Hand::Both));

    Snapshot s = Armed();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 200.0f, false, false, true});
    EvalContext ctx;

    Decision d = Evaluate(rs, s, ctx);
    REQUIRE(d.actionForm() == kSword);
    Pinned(s, d);

    // Sword pinned, enemy still close: the bow rule is outranked, not fired.
    s.now += 0.5;
    Trace trace;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(trace.at(1) == Verdict::Outranked);

    // The enemy backs off: the sword rule's condition lapses, and the bow
    // rule takes the hands. The sword's pin goes with them.
    s.enemies[0].distance = 900.0f;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);
    Pinned(s, d);
    REQUIRE(FindPin(s.pins, kSword) == nullptr);
    REQUIRE(FindPin(s.pins, kBow) != nullptr);

    // Close again: the sword rule is available again -- its pin is gone --
    // and takes the right hand back.
    s.enemies[0].distance = 200.0f;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.actionForm() == kSword);

    // A rule beneath that takes no hand the sword holds is not outranked:
    // the shield goes in the left.
    Pinned(s, d);
    rs.rules[1] = Equip(ActionKind::EquipWeapon, kShield, Hand::Left);
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
}

TEST_CASE("none lets go of every pin of its kind, unless a rule above holds one", "[equip]")
{
    RuleSet rs;
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, 0));

    Snapshot s = Armed();
    EvalContext ctx;

    // Nothing pinned: nothing to let go of, so it is done and falls through.
    Trace trace;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);

    // A pinned sword: the rule fires, naming nothing.
    AddPin(s.pins, *FindHoldable(s.loadout, kSword), Hand::Right, false);
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.action() == ActionKind::EquipWeapon);
    REQUIRE(d.actionForm() == 0);

    // Pinned armour is another kind, and none of this rule's business.
    s.pins.clear();
    AddPin(s.pins, *FindHoldable(s.loadout, kHelmet), Hand::None, false);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);

    // A rule above holding a weapon outranks a none beneath it, or the two
    // would pin and release the sword in turn.
    rs.rules.insert(rs.rules.begin(), Equip(ActionKind::EquipWeapon, kSword, Hand::Right));
    AddPin(s.pins, *FindHoldable(s.loadout, kSword), Hand::Right, false);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(trace.at(1) == Verdict::Outranked);
}

TEST_CASE("an equip rule needs the thing, of the kind it says, and one the AI would use", "[equip]")
{
    Snapshot s = Armed();
    EvalContext ctx;
    Trace trace;

    // Not hers.
    RuleSet rs;
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, 0xDEAD, Hand::Right));
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // Hers, but a spell under equip-weapon: a hand-edited profile's mistake.
    rs.rules[0] = Equip(ActionKind::EquipWeapon, kFirebolt, Hand::Right);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // Above her skill: the AI would never choose it, so a pin would be a
    // promise unkept. Said so, not fired.
    rs.rules[0] = Equip(ActionKind::EquipSpell, kChainLightning, Hand::Right);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::CannotHold);

    // A spell she can use, in both hands at once.
    rs.rules[0] = Equip(ActionKind::EquipSpell, kFirebolt, Hand::Both);
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.hand() == Hand::Both);
    // Pinned in one hand only, it is not yet done.
    AddPin(s.pins, *FindHoldable(s.loadout, kFirebolt), Hand::Left, false);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);
    AddPin(s.pins, *FindHoldable(s.loadout, kFirebolt), Hand::Right, false);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);

    // Arrows and armour take no hand, and are done once pinned at all.
    rs.rules[0] = Equip(ActionKind::EquipArrows, kArrows);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);
    AddPin(s.pins, *FindHoldable(s.loadout, kArrows), Hand::None, false);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
}

// ---------------------------------------------------------------------------
// A rule's list of actions: ordering and timing.
// ---------------------------------------------------------------------------

namespace
{

// A tick: half a second on, evaluate, and pin whatever equip steps fired,
// as the game side would.
Decision Tick(RuleSet &rs, Snapshot &s, EvalContext &ctx, Trace &trace, ActionTrace &actions)
{
    s.now += 0.5;
    const Decision d = Evaluate(rs, s, ctx, &trace, &actions);
    for (const auto &step : d.steps)
        if (IsEquip(step.action.kind) && step.action.form != 0)
            Pinned(s, Decision{d.ruleIndex, {step}});
    return d;
}

std::vector<ActionKind> Kinds(const Decision &d)
{
    std::vector<ActionKind> out;
    for (const auto &step : d.steps)
        out.push_back(step.action.kind);
    return out;
}

} // namespace

TEST_CASE("a list is done in its order, one action a tick", "[sequence]")
{
    // The outfit for a fight: sword, then cuirass, then arrows. Three ticks,
    // in that order, and never two on one tick.
    Rule outfit = Equip(ActionKind::EquipWeapon, kSword, Hand::Right);
    outfit.actions.push_back({ActionKind::EquipArmor, kHelmet});
    outfit.actions.push_back({ActionKind::EquipArrows, kArrows});
    RuleSet rs;
    rs.rules = {outfit, Equip(ActionKind::EquipWeapon, kShield, Hand::Left)};

    Snapshot s = Armed();
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(Kinds(d) == std::vector<ActionKind>{ActionKind::EquipWeapon});
    REQUIRE(d.actionForm() == kSword);
    REQUIRE(FindPin(s.pins, kSword) != nullptr);
    REQUIRE(FindPin(s.pins, kHelmet) == nullptr);

    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.actionForm() == kHelmet);
    REQUIRE(FindPin(s.pins, kArrows) == nullptr);
    // Meanwhile the shield rule beneath is not reached, though it could
    // fire: the list owns the ticks.
    REQUIRE(trace.at(1) == Verdict::NotReached);

    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.actionForm() == kArrows);
    REQUIRE_FALSE(ctx.pending.Active());

    // Through: all three pinned, the rule reports itself done, and the
    // shield rule beneath gets the tick.
    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(d.actionForm() == kShield);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(actions.at(0) == std::vector<Verdict>{Verdict::EffectActive, Verdict::EffectActive, Verdict::EffectActive});
}

TEST_CASE("what cannot be done is passed over, and the rest of the list keeps its order", "[sequence]")
{
    // Sword already pinned, no potion carried: the cuirass goes on the first
    // tick and the cast on the second. Two ticks for four actions.
    constexpr std::uint32_t kOakflesh = 0x0005AD5C;
    Rule r = Equip(ActionKind::EquipWeapon, kSword, Hand::Right);
    r.actions.push_back({ActionKind::EquipArmor, kHelmet});
    r.actions.push_back({ActionKind::DrinkHealthPotion});
    r.actions.push_back({ActionKind::CastSpell, kOakflesh});
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Armed();
    s.potions.healthCount = 0;
    s.spells.known.push_back(kOakflesh);
    AddPin(s.pins, *FindHoldable(s.loadout, kSword), Hand::Right, false);
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.actionForm() == kHelmet);
    REQUIRE(actions.at(0) ==
            std::vector<Verdict>{Verdict::EffectActive, Verdict::Fired, Verdict::NotReached, Verdict::NotReached});

    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(Kinds(d) == std::vector<ActionKind>{ActionKind::CastSpell});
    REQUIRE(actions.at(0) ==
            std::vector<Verdict>{Verdict::NotReached, Verdict::NotReached, Verdict::NoResource, Verdict::Fired});
    REQUIRE_FALSE(ctx.pending.Active());
}

TEST_CASE("two casts in a list wait for each other", "[sequence]")
{
    // Cast A, cast B. B is asked for while A is still in the air -- the
    // pool is busy -- and waits for it; it is not skipped, or a list of
    // casts could never be written.
    constexpr std::uint32_t kA = 0x00012FCC;
    constexpr std::uint32_t kB = 0x0005AD5C;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Self;
    r.actions = {{ActionKind::CastSpell, kA}, {ActionKind::CastSpell, kB}};
    RuleSet rs;
    rs.rules = {r, HealBelow(0.5f)};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f}; // the potion rule beneath could fire throughout
    s.spells.known = {kA, kB};
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    REQUIRE(Tick(rs, s, ctx, trace, actions).actionForm() == kA);

    // A in the air: B waits, and so does the potion rule.
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = true;
    REQUIRE_FALSE(Tick(rs, s, ctx, trace, actions).Fired());
    REQUIRE(trace.at(0) == Verdict::Busy);
    REQUIRE(trace.at(1) == Verdict::NotReached);

    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = false;
    REQUIRE(Tick(rs, s, ctx, trace, actions).actionForm() == kB);
    REQUIRE_FALSE(ctx.pending.Active());

    // Through, and A is still inside its cooldown: the rule has not begun
    // again, so it yields and the potion is finally drunk.
    const Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
}

TEST_CASE("a list keeps the target and the actions it began with", "[sequence]")
{
    // Bound to the weakest enemy when it began, the list keeps aiming at
    // that enemy though another becomes the weakest; and it keeps its
    // actions though the rule is edited under it.
    constexpr std::uint32_t kA = 0x00012FCC;
    constexpr std::uint32_t kB = 0x0005AD5C;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.9f;
    r.actions = {{ActionKind::CastSpell, kA}, {ActionKind::CastSpell, kB}};
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Healthy();
    s.spells.known = {kA, kB};
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f, false, false, true});
    s.enemies.push_back({0x102, {80.0f, 100.0f}, 300.0f, false, false, true});
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.targetId() == 0x101);

    // The other enemy is now the weakest, and the rule now says something
    // else entirely: the list in progress is unmoved by either.
    s.enemies[0].health = {90.0f, 100.0f};
    s.enemies[1].health = {10.0f, 100.0f};
    rs.rules[0].actions = {{ActionKind::HoldPosition}};
    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.actionForm() == kB);
    REQUIRE(d.targetId() == 0x101);
    REQUIRE_FALSE(ctx.pending.Active());
}

TEST_CASE("a list goes on after its condition has lapsed", "[sequence]")
{
    // Deliberate. "Health below half: drink, then cast the heal" -- the
    // potion works, health is above half by the next tick, and the heal is
    // cast anyway. The list is a commitment once begun, and it has to be:
    // "combat begins" holds for one tick only, and re-reading it would
    // strand every list written on it after its first action.
    constexpr std::uint32_t kHeal = 0x00012FCC;
    Rule r = HealBelow(0.5f);
    r.actions.push_back({ActionKind::CastSpell, kHeal});
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.spells.known.push_back(kHeal);
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::DrinkHealthPotion);
    s.health = {90.0f, 100.0f};
    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::CastSpell);
}

TEST_CASE("the cooldowns a list spends are the actions' own", "[sequence]")
{
    // Drinking as the first step of a list spaces the next drink exactly as
    // a single-action rule's would, wherever it sits: the settle belongs to
    // the action, and a list does not get a second potion inside it.
    Rule r = HealBelow(0.5f);
    r.actions.push_back({ActionKind::HoldPosition});
    RuleSet rs;
    rs.rules = {r, HealBelow(0.5f)};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::DrinkHealthPotion);
    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::HoldPosition);

    // One second in: both potion rules are inside the settle.
    Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    REQUIRE(trace.at(1) == Verdict::ActionCooldown);

    // Past it, the list begins again from its first action.
    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion);
    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.action() == ActionKind::DrinkHealthPotion);
}

TEST_CASE("a lingering dose blocks past the minimum cooldown", "[cooldown]")
{
    // The two mechanisms compose as a max, not an either/or:
    //   * the settle time is a floor, and it is all you get on a vanilla game
    //     where Restore Health is instant and leaves nothing to inspect;
    //   * an effect still running extends the block for as long as it runs,
    //     which is what potion-overhaul mods need and what no fixed number
    //     could guess.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;

    REQUIRE(Evaluate(rs, s, ctx).Fired());

    // Well past the settle time, but the dose is still working.
    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion) + 5.0;
    s.potions.healthEffectActive = true;

    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    // Not "no potion" -- she has twelve. The reason has to be the real one or
    // the debug column sends you to check the inventory for nothing.
    REQUIRE(trace.at(0) == Verdict::EffectActive);

    // Dose finished, still hurt: free to drink again.
    s.potions.healthEffectActive = false;
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

TEST_CASE("an instant effect leaves the settle time in charge", "[cooldown]")
{
    // Vanilla Restore Health is instant: duration 0, nothing in the active
    // effect list. The flag stays false and the floor does the work, which is
    // why the marquee rule behaves correctly on an unmodded game.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    REQUIRE_FALSE(s.potions.healthEffectActive);

    EvalContext ctx;

    REQUIRE(Evaluate(rs, s, ctx).Fired());

    s.now += 0.15;
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    s.now += MinimumCooldown(ActionKind::DrinkHealthPotion);
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

TEST_CASE("having no potion and having one still working are different", "[cooldown]")
{
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));
    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    SECTION("empty bag")
    {
        s.potions.healthCount = 0;
        EvalContext ctx;
        Trace trace;
        Evaluate(rs, s, ctx, &trace);
        REQUIRE(trace.at(0) == Verdict::NoResource);
    }

    SECTION("full bag, dose still running")
    {
        s.potions.healthEffectActive = true;
        EvalContext ctx;
        Trace trace;
        Evaluate(rs, s, ctx, &trace);
        REQUIRE(trace.at(0) == Verdict::EffectActive);
    }
}

TEST_CASE("nothing is on cooldown at the start of the game", "[cooldown]")
{
    // Regression: the cooldown state stores a time UNTIL WHICH a thing is
    // blocked, so zero-initialised means "free". Storing a last-fired time
    // instead would need a sentinel in the distant past -- the game clock also
    // starts at zero, and every action would look freshly used for the first
    // few seconds of play.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.now = 0.5; // half a second after the plugin loaded

    EvalContext ctx;
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

// ---------------------------------------------------------------------------
// The vocabulary: a stable wire id and a translatable display name, kept apart.
// ---------------------------------------------------------------------------

namespace
{
// Catch2 in this configuration has no StringMaker for std::string_view, so
// comparing one inside REQUIRE fails to link. Compare owned strings instead --
// it is also what the failure output wants to print.
std::string Str(std::string_view v)
{
    return std::string(v);
}
} // namespace

TEST_CASE("every wire name round-trips", "[vocabulary]")
{
    // A name that does not parse back is a rule that cannot be loaded from the
    // profile it was just saved to. Walk every enumerator rather than spot
    // checking, so adding one without a name fails here rather than in
    // somebody's save file.
    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
    {
        const auto v = static_cast<SubjectKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(SubjectFromWireName(WireName(v)) == v);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        const auto v = static_cast<PredicateKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(PredicateFromWireName(WireName(v)) == v);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionTargetKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionTargetKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(ActionTargetFromWireName(WireName(v)) == v);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(ActionFromWireName(WireName(v)) == v);
    }
    for (const Hand v : {Hand::None, Hand::Left, Hand::Right, Hand::Both})
    {
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(HandFromWireName(WireName(v)) == v);
        REQUIRE(DisplayName(v).size() > 0);
    }
}

TEST_CASE("every wire name is a slug, and no display name is", "[vocabulary]")
{
    // THE GUARD. Display text and the file format must stay separable, or the
    // first person to translate the UI translates the data format with it and
    // profiles stop loading across languages. Asserting the shapes are disjoint
    // means the two cannot be quietly merged later: a translated string carries
    // capitals, spaces or accents and cannot satisfy IsWireName.
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionKind>(i);
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(Str(WireName(v)) != Str(DisplayName(v)));
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        REQUIRE(IsWireName(WireName(static_cast<PredicateKind>(i))));
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
    {
        REQUIRE(IsWireName(WireName(static_cast<SubjectKind>(i))));
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionTargetKind::COUNT); ++i)
    {
        REQUIRE(IsWireName(WireName(static_cast<ActionTargetKind>(i))));
    }
}

TEST_CASE("the slug format rejects anything a translator would produce", "[vocabulary]")
{
    REQUIRE(IsWireName("drink-strongest-health-potion"));
    REQUIRE(IsWireName("self"));
    REQUIRE(IsWireName("count-at-least"));

    REQUIRE_FALSE(IsWireName("Drink Strongest Healing Potion")); // display text
    REQUIRE_FALSE(IsWireName("SanteEnDessousDe"));               // a translation
    REQUIRE_FALSE(IsWireName("sante-en-dessous-de-Ã©"));         // non-ASCII
    REQUIRE_FALSE(IsWireName("DrinkHealthPotion"));              // capitals
    REQUIRE_FALSE(IsWireName("-leading"));
    REQUIRE_FALSE(IsWireName("trailing-"));
    REQUIRE_FALSE(IsWireName("double--hyphen"));
    REQUIRE_FALSE(IsWireName(""));
}

TEST_CASE("an unknown wire name is rejected, not guessed at", "[vocabulary]")
{
    // A profile written by a newer build will name things this one has never
    // heard of. Returning nullopt lets the loader drop that rule with a log
    // line instead of refusing the whole file.
    REQUIRE_FALSE(PredicateFromWireName("armour-rating-below").has_value());
    REQUIRE_FALSE(ActionFromWireName("cast-healing-spell").has_value());
    REQUIRE_FALSE(SubjectFromWireName("").has_value());

    // And display text is not a key. This is the property that keeps the file
    // format independent of the player's language.
    REQUIRE_FALSE(ActionFromWireName(DisplayName(ActionKind::DrinkHealthPotion)).has_value());
}

TEST_CASE("the argument shape tells the UI which widget to draw", "[vocabulary]")
{
    REQUIRE(ArgumentFor(PredicateKind::HealthPctBelow) == ArgumentKind::Percent);
    REQUIRE(ArgumentFor(PredicateKind::WithinDistance) == ArgumentKind::Distance);
    REQUIRE(ArgumentFor(PredicateKind::CountAtLeast) == ArgumentKind::Count);

    // A predicate that takes no argument must not be given a slider that
    // silently writes a meaningless number into the profile.
    REQUIRE(ArgumentFor(PredicateKind::Any) == ArgumentKind::None);
    REQUIRE(ArgumentFor(PredicateKind::InCombat) == ArgumentKind::None);
    REQUIRE(ArgumentFor(PredicateKind::InBleedout) == ArgumentKind::None);
}

TEST_CASE("every value has display text and help text", "[vocabulary]")
{
    // Cheap way to make adding a vocabulary entry force a decision about what
    // it means to a player, rather than shipping a blank dropdown or tooltip.
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        const auto v = static_cast<PredicateKind>(i);
        REQUIRE(DisplayName(v).size() > 0);
        REQUIRE(Describe(v).size() > 0);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionKind>(i);
        REQUIRE(DisplayName(v).size() > 0);
        REQUIRE(Describe(v).size() > 0);
    }
}

TEST_CASE("Stat::Pct does not divide by zero", "[snapshot]")
{
    REQUIRE(Stat{}.Pct() == 0.0f);
    REQUIRE(Stat{50.0f, 200.0f}.Pct() == 0.25f);
}
