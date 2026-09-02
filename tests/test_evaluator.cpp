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
    r.action = ActionKind::DrinkHealthPotion;
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
        REQUIRE(d.action == ActionKind::DrinkHealthPotion);
        REQUIRE(d.targetId == s.self);
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
    cast.action = ActionKind::CastSpell;
    cast.actionForm = kHeal;
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
    REQUIRE(d.action == ActionKind::CastSpell);
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
        r.action = ActionKind::CastSpell;
        r.actionForm = spell;
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
    r.action = ActionKind::StopCombat;
    rs.rules.push_back(r);

    EvalContext ctx;
    const auto d = Evaluate(rs, s, ctx);

    REQUIRE(d.Fired());
    // The whole point of the split: the enemy is named once, in the condition,
    // and the action lands on that same enemy.
    REQUIRE(d.targetId == 0x102);
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
    REQUIRE(d.targetId == kPlayerFormID);
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
    r.action = ActionKind::StopCombat;
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
}

TEST_CASE("an unanswerable pair reports InvalidCondition, not ConditionFalse", "[validity]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::WithinDistance; // nonsense
    r.conditionArg = 100.0f;
    r.action = ActionKind::StopCombat;
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
    r.action = action;
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
    spare.action = ActionKind::DrinkHealthPotion;
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
    swarmed.action = ActionKind::HoldPosition;
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
    hold.action = ActionKind::HoldPosition;
    hold.label = "brace: hold position";
    rs.rules.push_back(hold);

    Rule disengage = hold;
    disengage.action = ActionKind::StopCombat;
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
    cast.action = ActionKind::CastSpell;
    cast.actionForm = kOakflesh;
    rs.rules.push_back(cast);

    Rule potion = cast;
    potion.action = ActionKind::DrinkHealthPotion;
    potion.actionForm = 0;
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
    cast.action = ActionKind::CastSpell;
    cast.actionForm = kHeal;
    rs.rules.push_back(cast);

    Rule potion = cast;
    potion.action = ActionKind::DrinkHealthPotion;
    potion.actionForm = 0;
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
        r.action = ActionKind::DrinkPotion;
        r.actionForm = form;
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
            "that spell is already in hand or still running");

    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::DrinkHealthPotion)) == "no potion");
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EquipSpell)) == "does not know that spell");

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

TEST_CASE("a sustained buff is not re-equipped while it is still up", "[spell]")
{
    // The case a cooldown cannot solve. Oakflesh runs for sixty seconds, and
    // no settle worth choosing is that long -- pick two seconds and the rule
    // re-casts thirty times, pick sixty and every other spell in the game gets
    // the wrong number. Only the effect list answers it.
    constexpr std::uint32_t kOakflesh = 0x0005AD5C;

    RuleSet rs;
    Rule buff;
    buff.subject = SubjectKind::Self;
    buff.predicate = PredicateKind::InCombat;
    buff.actionTarget = ActionTargetKind::Self;
    buff.action = ActionKind::EquipSpell;
    buff.actionForm = kOakflesh;
    buff.label = "armour up";
    rs.rules.push_back(buff);

    Snapshot s = Healthy();
    s.inCombat = true;
    s.spells.known.push_back(kOakflesh);

    EvalContext ctx;

    // Nothing up yet, so it casts.
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    // The effect is now running. Well past the settle, it still must not fire.
    s.spells.active.push_back(kOakflesh);
    s.now += MinimumCooldown(ActionKind::EquipSpell) * 10.0;

    Trace trace;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);

    // It lapses, and the rule takes it again.
    s.spells.active.clear();
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
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
    buff.action = ActionKind::EquipSpell;
    buff.actionForm = 0x0005AD5C;
    rs.rules.push_back(buff);

    Snapshot s = Healthy();
    EvalContext ctx;

    Trace trace;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // An EquipSpell rule with no spell chosen is the same kind of unusable, and
    // is what a freshly added rule looks like before it is filled in.
    rs.rules[0].actionForm = 0;
    s.spells.known.push_back(0x0005AD5C);
    Trace blank;
    REQUIRE(Evaluate(rs, s, ctx, &blank).ruleIndex < 0);
    REQUIRE(blank.at(0) == Verdict::NoResource);
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
    REQUIRE_FALSE(PredicateFromWireName("health-pct-above").has_value());
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
