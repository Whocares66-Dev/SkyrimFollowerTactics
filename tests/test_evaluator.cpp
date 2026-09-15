// These tests run with no Skyrim, no SKSE, and no CommonLibSSE.
// That is the whole point -- see docs/PLAN.md section 3.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Effects.h"
#include "core/Evaluator.h"
#include "core/Vocabulary.h"

#include <string>

using namespace ft;
using namespace ft::test;

namespace
{

// Take the health potions out of the bag.
void EmptyBag(Snapshot &s, std::uint32_t form = kHealthPotion)
{
    std::erase_if(s.potions.carried, [&](const PotionStock::Carried &c) { return c.form == form; });
}

// The policy actions, by the effect: the strongest potion or poison carried
// with it.
Action Drink(const char *effect, bool strongest = true)
{
    Action a;
    a.kind = strongest ? ActionKind::DrinkStrongest : ActionKind::DrinkWeakest;
    a.effect = effect;
    return a;
}
Action DrinkHealth()
{
    return Drink("Restore Health");
}
Action DrinkMagicka()
{
    return Drink("Restore Magicka");
}
Action DrinkStamina()
{
    return Drink("Restore Stamina");
}
Action Apply(const char *effect, bool strongest = true)
{
    Action a;
    a.kind = strongest ? ActionKind::ApplyStrongest : ActionKind::ApplyWeakest;
    a.effect = effect;
    return a;
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
        REQUIRE(d.action() == ActionKind::DrinkStrongest);
        REQUIRE(d.targetId() == s.self);
        REQUIRE(trace.at(0) == Verdict::Fired);
    }

    SECTION("will not fire without a potion in the inventory")
    {
        Snapshot s = Healthy();
        s.health = {40.0f, 100.0f};
        EmptyBag(s);

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
    cast.predicate = PredicateKind::Any;
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

    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0); // potion fires

    std::swap(rs.rules[0], rs.rules[1]); // cast is now index 0, potion index 1
    s.now += 0.1;

    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0); // the cast rule, unblocked, fires from its new slot
    REQUIRE(d.action() == ActionKind::CastSpell);
    REQUIRE(trace.at(1) == Verdict::NotReached);

    // And the potion's cooldown is on the ACTION, wherever its rule now sits.
    REQUIRE(ctx.BlockedUntil({ActionKind::DrinkStrongest, 0, s.self, "Restore Health"}) > s.now);
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
        r.predicate = PredicateKind::Any;
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

TEST_CASE("a cooldown restarted when the action is over runs from then", "[cooldown]")
{
    // A cast's lease can outlast its cooldown. Counted from the decision, the
    // rule would fire again the moment the lease ended; restarted then, it
    // waits the whole cooldown after the cast.
    constexpr std::uint32_t kHeal = 0x0002F3B8;
    Rule rule;
    rule.subject = SubjectKind::Self;
    rule.predicate = PredicateKind::Any;
    rule.actionTarget = ActionTargetKind::Self;
    rule.FirstAction().kind = ActionKind::CastSpell;
    rule.FirstAction().form = kHeal;
    RuleSet rs;
    rs.rules.push_back(rule);

    Snapshot s = Healthy();
    s.spells.known.push_back(kHeal);
    EvalContext ctx;

    const Decision d = Evaluate(rs, s, ctx);
    REQUIRE(d.Fired());
    const double cooldown = MinimumCooldown(ActionKind::CastSpell);
    const double over = s.now + cooldown + 1.0; // the lease ended a second after the cooldown would have
    RestartCooldown(ctx, d.step->action, d.step->target, over);

    s.now = over + cooldown - 0.1;
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    s.now = over + cooldown;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
}

TEST_CASE("an unsupported action never fires", "[evaluator]")
{
    // The casts are the actions a runtime can lack: without the package
    // pool a cast rule says so, before its condition or its spell is asked.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));
    rs.rules[0].FirstAction() = {};
    rs.rules[0].FirstAction().kind = ActionKind::CastSpell;
    rs.rules[0].FirstAction().form = 0x12FCD;

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;
    ctx.caps.castingAvailable = false;

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
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {10.0f, 100.0f}, 900.0f});

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

    SECTION("a predicate with no measure of its own binds the nearest match")
    {
        Rule r;
        r.subject = SubjectKind::Enemy;
        r.predicate = PredicateKind::Any; // both qualify

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
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {10.0f, 100.0f}, 900.0f});

    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.2f;
    r.actionTarget = ActionTargetKind::Enemy;
    r.FirstAction().kind = ActionKind::Attack;
    rs.rules.push_back(r);

    EvalContext ctx;
    const auto d = Evaluate(rs, s, ctx);

    REQUIRE(d.Fired());
    // The whole point of the split: the enemy is named once, in the condition,
    // and "Enemy" on the action side is that same enemy.
    REQUIRE(d.targetId() == 0x102);
}

TEST_CASE("the action target is its own choice, within what makes sense", "[binding]")
{
    // A cast goes wherever the rule aims it; a potion is only ever drunk by
    // oneself, so a potion "on the player" is as unfireable as an action
    // this runtime cannot do -- and the menu never offers it.
    RuleSet rs;
    Rule r = HealBelow(0.5f);
    r.actionTarget = ActionTargetKind::Player;
    rs.rules.push_back(r);

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};

    EvalContext ctx;
    Trace trace;
    auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::Unsupported);
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Player, ActionKind::DrinkStrongest));
    REQUIRE(IsActionValidFor(ActionTargetKind::Player, ActionKind::CastSpell));
    REQUIRE(IsActionValidFor(ActionTargetKind::Self, ActionKind::DrinkStrongest));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Self, ActionKind::Attack));
    REQUIRE(IsActionValidFor(ActionTargetKind::Attacker, ActionKind::Attack));

    constexpr std::uint32_t kSpell = 0x00012FCD; // Firebolt: a cast, aimed anywhere
    rs.rules[0].FirstAction().kind = ActionKind::CastSpell;
    rs.rules[0].FirstAction().form = kSpell;
    s.spells.known.push_back(kSpell);
    d = Evaluate(rs, s, ctx);
    REQUIRE(d.Fired());
    REQUIRE(d.targetId() == kPlayerFormID);

    // "Ally" and "Enemy" on the action side mean the one the condition
    // matched, so they need a condition about one: a hand-edited profile
    // that says otherwise is invalid, not silently aimed at no one.
    REQUIRE(IsActionTargetValidFor(SubjectKind::Ally, ActionTargetKind::Ally));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Follower, ActionTargetKind::Ally));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Enemy, ActionTargetKind::Enemy));
    REQUIRE_FALSE(IsActionTargetValidFor(SubjectKind::Self, ActionTargetKind::Ally));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Ally, ActionTargetKind::Enemy)); // whoever is being fought
    REQUIRE(IsActionTargetValidFor(SubjectKind::Self, ActionTargetKind::Attacker));
    rs.rules[0].actionTarget = ActionTargetKind::Ally;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::InvalidCondition);

    // Reconcile is what the editor does after the IF side changes: the
    // target falls back to Self and an action that makes no sense there is
    // blanked, so the THEN cell never shows a pair the menu would not offer.
    Rule mixed;
    mixed.subject = SubjectKind::Ally;
    mixed.actionTarget = ActionTargetKind::Ally;
    mixed.actions = {{ActionKind::Attack}, {ActionKind::CastSpell, kSpell}};
    mixed.subject = SubjectKind::Self;
    Reconcile(mixed);
    REQUIRE(mixed.actionTarget == ActionTargetKind::Self);
    REQUIRE(mixed.actions[0].kind == ActionKind::None);
    REQUIRE(mixed.actions[1].kind == ActionKind::CastSpell);

    // A named follower as the target: that ally, when with us.
    Rule named;
    named.subject = SubjectKind::Self;
    named.actionTarget = ActionTargetKind::Follower;
    named.actionTargetForm = 0x202;
    named.FirstAction() = {ActionKind::CastSpell, kSpell};
    bool ok = false;
    REQUIRE(ResolveActionTarget(named, s, Binding{s.self, true}, &ok) == 0);
    REQUIRE_FALSE(ok);
    s.allies.push_back({0x202, {50.0f, 100.0f}, 200.0f});
    REQUIRE(ResolveActionTarget(named, s, Binding{s.self, true}, &ok) == 0x202);
    REQUIRE(ok);
}

TEST_CASE("ally conditions bind the ally, not the follower", "[binding]")
{
    Snapshot s = Healthy();
    //                  id     health          dist   bleedout
    s.allies.push_back({0x201, {80.0f, 100.0f}, 100.0f});
    s.allies.push_back({0x202, {20.0f, 100.0f}, 500.0f});

    Rule r;
    r.subject = SubjectKind::Ally;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.5f;

    const auto b = EvaluateCondition(r, s);
    REQUIRE(b.ok);
    REQUIRE(b.id == 0x202);
}

TEST_CASE("the follower's own target is Enemy: Attacked by the follower", "[binding]")
{
    Snapshot s = Healthy();
    s.currentTarget = 0x101;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::AttackedBy;
    r.subjectForm = s.self;
    // Not sensed as an enemy yet: nothing to bind.
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.enemies.push_back({0x101, {10.0f, 100.0f}, 200.0f});
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
}

TEST_CASE("no binding means the rule is skipped, not fired at nobody", "[evaluator]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Enemy;
    r.FirstAction().kind = ActionKind::Attack;
    rs.rules.push_back(r);

    EvalContext ctx;
    Trace trace;
    const auto d = Evaluate(rs, Healthy(), ctx, &trace); // no enemies

    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);
}

// ---------------------------------------------------------------------------
// The conditions: validity (an unanswerable pair is an authoring error, not a
// false condition), the fight's edges, a status, armour, and the party.
// ---------------------------------------------------------------------------

TEST_CASE("the subject and predicate validity matrix", "[validity]")
{
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::MagickaPctBelow));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::Attacking));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::Status));

    // A lone subject is not asked whom it is attacking.
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::Attacking));

    // "Any" is offered for everyone: always true of the player and of an
    // ally, and there so a rule can aim at them under the heading a reader
    // looks for it. A named follower's Any is "they are with us".
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::Any));
    REQUIRE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::Any));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::Any));
    REQUIRE(IsPredicateValidFor(SubjectKind::Follower, PredicateKind::Any));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::Any));

    // Every actor view carries all three stats, so magicka and stamina are
    // asked of anyone health is.
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::MagickaPctBelow));
    REQUIRE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::StaminaPctBelow));

    // Above is answerable exactly where below is.
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::MagickaPctAbove));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::HealthPctAbove));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::MagickaPctAbove));
    REQUIRE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::StaminaPctAbove));
    REQUIRE(AboveOf(PredicateKind::HealthPctBelow) == PredicateKind::HealthPctAbove);
    REQUIRE(AboveOf(PredicateKind::CombatBegins) == PredicateKind::CombatBegins);
}

TEST_CASE("the edges of a fight hold for one evaluation each", "[evaluator]")
{
    Rule onBegin;
    onBegin.subject = SubjectKind::Self;
    onBegin.predicate = PredicateKind::CombatBegins;
    onBegin.actionTarget = ActionTargetKind::Self;
    onBegin.FirstAction() = DrinkStamina();

    Rule always = onBegin;
    always.predicate = PredicateKind::Any;
    always.FirstAction() = DrinkMagicka();

    Rule onEnd = onBegin;
    onEnd.predicate = PredicateKind::CombatEnds;
    onEnd.FirstAction() = DrinkMagicka();

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

    // The farewell pass: only ends holds, and its rule takes the tick
    // before the others are looked at. The standing rule is false on it
    // regardless, so it cannot re-pin what the after-fight restore just
    // put back.
    s.inCombat = false;
    s.combatEnded = true;
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 2);
    REQUIRE(trace.at(0) == Verdict::NotReached);
    REQUIRE(trace.at(1) == Verdict::NotReached);
    REQUIRE_FALSE(EvaluateCondition(always, s).ok);
    REQUIRE_FALSE(EvaluateCondition(onBegin, s).ok);

    // Only the follower's own fight has edges.
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::CombatEnds));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::CombatBegins));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::CombatEnds));
}

TEST_CASE("a new fight starts with no cooldowns", "[edge]")
{
    // The heal fires, and is on cooldown for the rest of that fight. The
    // next fight's first tick -- its begins edge -- is not the same fight,
    // and the heal must not be held back by a potion drunk in the last one:
    // the edge is what resets the cooldowns, not a gap in the tick's clock,
    // which read a long bleedout as a new fight and reset them mid-fight.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));
    EvalContext ctx;
    Trace trace;

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.combatBegan = true;
    REQUIRE(Evaluate(rs, s, ctx, &trace).Fired());

    // A tick later, still in the fight and still hurt: the cooldown holds.
    s.combatBegan = false;
    s.now += 0.5;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    // The fight ends, and a new one begins inside the cooldown: it fires.
    s.now += 0.5;
    s.combatBegan = true;
    REQUIRE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(ctx.blocked.size() == 1);
}

TEST_CASE("on its edge every Combat start rule is checked first, and their lists run in order", "[edge]")
{
    // A standing rule ABOVE two Combat start rules, the second with two
    // actions. Position does not matter on the edge: the edge rules go
    // first, and together, as one list -- stamina; magicka, health -- one
    // action per tick, and the standing rule waits for all of it.
    Rule always;
    always.subject = SubjectKind::Self;
    always.predicate = PredicateKind::Any;
    always.actionTarget = ActionTargetKind::Self;
    always.FirstAction() = DrinkHealth();

    Rule first = always;
    first.predicate = PredicateKind::CombatBegins;
    first.FirstAction() = DrinkStamina();

    Rule second = first;
    second.FirstAction() = DrinkMagicka();
    second.actions.push_back(DrinkHealth());

    RuleSet rs;
    rs.rules = {always, first, second};
    EvalContext ctx;
    Trace trace;

    Snapshot s = Healthy();
    s.combatBegan = true;
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(d.action() == ActionKind::DrinkStrongest);
    REQUIRE(d.actionForm() == kStaminaPotion);
    REQUIRE(trace.at(0) == Verdict::NotReached);
    REQUIRE(trace.at(1) == Verdict::Fired);
    REQUIRE(trace.at(2) == Verdict::Queued);
    REQUIRE(ctx.InProgress());

    // The edge has passed, and the second rule's list runs anyway: it was
    // queued on the edge. The standing rule still waits.
    s.combatBegan = false;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 2);
    REQUIRE(d.actionForm() == kMagickaPotion);
    REQUIRE(d.subjectId() == s.self);
    REQUIRE(trace.at(0) == Verdict::NotReached);

    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 2);
    REQUIRE(d.actionForm() == kHealthPotion);
    REQUIRE_FALSE(ctx.InProgress());

    // Through: the standing rule has the next tick, once its potion is
    // off cooldown, and the edge rules beneath it are not reached.
    s.now += 5.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(trace.at(1) == Verdict::NotReached);
    REQUIRE(trace.at(2) == Verdict::NotReached);
}

TEST_CASE("an edge rule's list waits rather than yields, even at its first action", "[edge]")
{
    // The edge is not coming back: a Combat end rule whose first action is
    // merely on cooldown waits for it, where a standing rule would yield to
    // the rules beneath and be lost. (The end edge, because the begins edge
    // clears the cooldowns: nothing can be on one there.)
    Rule onEnd;
    onEnd.subject = SubjectKind::Self;
    onEnd.predicate = PredicateKind::CombatEnds;
    onEnd.actionTarget = ActionTargetKind::Self;
    onEnd.FirstAction() = DrinkHealth();
    Rule always = onEnd;
    always.predicate = PredicateKind::Any;
    always.FirstAction() = DrinkMagicka();

    RuleSet rs;
    rs.rules = {onEnd, always};
    EvalContext ctx;
    Trace trace;
    Snapshot s = Healthy();
    ctx.Block({ActionKind::DrinkStrongest, 0, s.self, "Restore Health"}, s.now + 1.0);

    s.inCombat = false;
    s.combatEnded = true;
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    REQUIRE(trace.at(1) == Verdict::NotReached);
    REQUIRE(ctx.InProgress());

    s.combatEnded = false;
    s.now += 2.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.actionForm() == kHealthPotion);
    REQUIRE_FALSE(ctx.InProgress());
}

TEST_CASE("the Combat end lists run on after the fight, and nothing else does", "[edge]")
{
    Rule always;
    always.subject = SubjectKind::Self;
    always.predicate = PredicateKind::Any;
    always.actionTarget = ActionTargetKind::Self;
    always.FirstAction() = DrinkHealth();

    Rule first = always;
    first.predicate = PredicateKind::CombatEnds;
    first.FirstAction() = DrinkStamina();
    Rule second = first;
    second.FirstAction() = DrinkMagicka();

    RuleSet rs;
    rs.rules = {always, first, second};
    EvalContext ctx;
    Trace trace;

    // Mid-fight, the standing rule is in the middle of nothing; the fight
    // ends. The farewell evaluation: the first Combat end rule fires, the
    // second is queued, the standing rule above them is not reached.
    Snapshot s = Healthy();
    s.inCombat = false;
    s.combatEnded = true;
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::NotReached);
    REQUIRE(trace.at(2) == Verdict::Queued);
    REQUIRE(ctx.InProgress());

    // The tick after, out of the fight: the second list runs, and the
    // standing rule is not looked at.
    s.combatEnded = false;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 2);
    REQUIRE(trace.at(0) == Verdict::NotReached);
    REQUIRE_FALSE(ctx.InProgress());

    // Through, and still out of the fight: nothing fires, however true the
    // standing rule is.
    s.now += 5.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::NotReached);
}

TEST_CASE("a new fight drops the Combat end lists still running", "[edge]")
{
    Rule onEnd;
    onEnd.subject = SubjectKind::Self;
    onEnd.predicate = PredicateKind::CombatEnds;
    onEnd.actionTarget = ActionTargetKind::Self;
    onEnd.FirstAction() = DrinkStamina();
    onEnd.actions.push_back(DrinkMagicka());
    Rule onBegin = onEnd;
    onBegin.predicate = PredicateKind::CombatBegins;
    onBegin.actions = {DrinkHealth()};

    RuleSet rs;
    rs.rules = {onEnd, onBegin};
    EvalContext ctx;

    Snapshot s = Healthy();
    s.inCombat = false;
    s.combatEnded = true;
    REQUIRE(Evaluate(rs, s, ctx).actionForm() == kStaminaPotion);
    REQUIRE(ctx.InProgress());

    // Before the magicka potion, a new fight: the end list goes, and the
    // start list runs instead.
    s.inCombat = true;
    s.combatEnded = false;
    s.combatBegan = true;
    s.now += 0.5;
    const Decision d = Evaluate(rs, s, ctx);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(d.actionForm() == kHealthPotion);
    REQUIRE_FALSE(ctx.InProgress());
}

TEST_CASE("a rule does its actions one per tick, in order, and waits rather than yields mid-list", "[sequence]")
{
    constexpr std::uint32_t kHeal = 0x00012FCC;

    // One rule, three actions: drink, cast, hold. The whole list is what
    // the player asked for, in that order, one per tick, before anything
    // else is decided.
    Rule r = HealBelow(0.5f);
    r.actions.push_back({ActionKind::CastSpell, kHeal});
    r.actions.push_back(DrinkStamina());
    Rule other = HealBelow(0.5f);
    other.actionTarget = ActionTargetKind::Self;
    other.FirstAction() = DrinkMagicka();

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
    REQUIRE(d.step.has_value());
    REQUIRE(d.action() == ActionKind::DrinkStrongest);
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
    REQUIRE(d.step.has_value());
    REQUIRE(d.action() == ActionKind::CastSpell);
    REQUIRE(ctx.pending.Active());

    // Tick four: the hold, and the list is through.
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.step.has_value());
    REQUIRE(d.action() == ActionKind::DrinkStrongest);
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
    other.FirstAction() = DrinkMagicka();
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
    REQUIRE(d.action() == ActionKind::DrinkStrongest);
    REQUIRE(ctx.pending.Active());

    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace, &actions);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(d.action() == ActionKind::DrinkStrongest);
    REQUIRE_FALSE(ctx.pending.Active());
    // The first rule was re-read from the top on the same tick: the potion
    // is inside its settle.
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
}

TEST_CASE("a list in progress is dropped when the fight ends", "[sequence]")
{
    Rule r = HealBelow(0.5f);
    r.actions.push_back(DrinkStamina());
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    EvalContext ctx;
    REQUIRE(Evaluate(rs, s, ctx).step.has_value());
    REQUIRE(ctx.pending.Active());

    s.combatEnded = true;
    s.inCombat = false;
    REQUIRE_FALSE(Evaluate(rs, s, ctx).Fired());
    REQUIRE_FALSE(ctx.pending.Active());
}

TEST_CASE("a kind of being is asked of any subject: a member answers for its group, and the groups overlap", "[type]")
{
    // The four groups: a head is any member; a member is itself. The game
    // side sets what it reads, and a being may be of several kinds at once.
    ActorTraits nord;
    nord.SetType(TypeKind::Nord);
    REQUIRE(nord.Is(TypeKind::Nord));
    REQUIRE(nord.Is(TypeKind::Man));
    REQUIRE_FALSE(nord.Is(TypeKind::Breton));
    REQUIRE_FALSE(nord.Is(TypeKind::Elf));
    REQUIRE_FALSE(nord.Is(TypeKind::Creature));
    // A vampire Nord is a Nord still, and a Vampire, Undead and a Creature.
    ActorTraits vampire = nord;
    vampire.SetType(TypeKind::Vampire);
    vampire.SetType(TypeKind::Undead);
    REQUIRE(vampire.Is(TypeKind::Nord));
    REQUIRE(vampire.Is(TypeKind::Man));
    REQUIRE(vampire.Is(TypeKind::Vampire));
    REQUIRE(vampire.Is(TypeKind::Undead));
    REQUIRE(vampire.Is(TypeKind::Creature));
    // A ghost of a Nord is Undead alone: the game side sets no people.
    ActorTraits ghost;
    ghost.SetType(TypeKind::Undead);
    REQUIRE(ghost.Is(TypeKind::Creature));
    REQUIRE_FALSE(ghost.Is(TypeKind::Man));
    // A Falmer is an Elf and a Creature both; a hagraven a Creature and
    // nothing finer; the Elder race a Man and no particular one.
    ActorTraits falmer;
    falmer.SetType(TypeKind::Falmer);
    falmer.SetType(TypeKind::Creature);
    REQUIRE(falmer.Is(TypeKind::Elf));
    REQUIRE(falmer.Is(TypeKind::Creature));
    REQUIRE_FALSE(falmer.Is(TypeKind::HighElf));
    ActorTraits hagraven;
    hagraven.SetType(TypeKind::Creature);
    REQUIRE(hagraven.Is(TypeKind::Creature));
    REQUIRE_FALSE(hagraven.Is(TypeKind::Animal));
    ActorTraits elder;
    elder.SetType(TypeKind::Man);
    REQUIRE(elder.Is(TypeKind::Man));
    REQUIRE_FALSE(elder.Is(TypeKind::Nord));

    // The groups are what the enum says they are.
    REQUIRE(GroupOf(TypeKind::Redguard) == TypeKind::Man);
    REQUIRE(GroupOf(TypeKind::WoodElf) == TypeKind::Elf);
    REQUIRE(GroupOf(TypeKind::Orc) == TypeKind::Beast);
    REQUIRE(GroupOf(TypeKind::Werewolf) == TypeKind::Creature);
    REQUIRE(IsGroupHead(TypeKind::Creature));
    REQUIRE_FALSE(IsGroupHead(TypeKind::Troll));

    // As a condition: of an enemy, binding the one of the kind.
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::Type;
    r.typeKind = TypeKind::Undead;
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {100.0f, 100.0f}, 200.0f});
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.enemies[0].traits.SetType(TypeKind::Undead);
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    r.typeKind = TypeKind::Creature;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    r.typeKind = TypeKind::Dragon;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    // Of a group only: the follower, the player and a named follower are
    // each one being, and a rule about what they are would be true always
    // or never.
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::Type));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::Type));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Follower, PredicateKind::Type));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::Type));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::Type));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Corpse, PredicateKind::Type));
}

TEST_CASE("a status is asked of any subject, and binds whoever is in it", "[status]")
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Status;
    r.statusKind = StatusKind::Poisoned;

    Snapshot s = Healthy();
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.traits.Set(StatusKind::Poisoned);
    REQUIRE(EvaluateCondition(r, s).ok);
    // Another status is another question.
    r.statusKind = StatusKind::Burning;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // The player's own.
    r.subject = SubjectKind::Player;
    r.statusKind = StatusKind::Staggered;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    Player(s).traits.Set(StatusKind::Staggered);
    REQUIRE(EvaluateCondition(r, s).ok);
    REQUIRE(EvaluateCondition(r, s).id == kPlayerFormID);

    // An ally in it binds that ally; the nearest when several are. (The
    // player, an ally too, is in none of these.)
    s.allies.push_back({0x201, {100.0f, 100.0f}, 500.0f});
    s.allies.push_back({0x202, {100.0f, 100.0f}, 200.0f});
    r.subject = SubjectKind::Ally;
    r.statusKind = StatusKind::Fleeing;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.allies[1].traits.Set(StatusKind::Fleeing);
    REQUIRE(EvaluateCondition(r, s).id == 0x201);
    s.allies[2].traits.Set(StatusKind::Fleeing);
    REQUIRE(EvaluateCondition(r, s).id == 0x202);

    // An enemy.
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    r.subject = SubjectKind::Enemy;
    r.statusKind = StatusKind::Casting;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.enemies[0].traits.Set(StatusKind::Casting);
    REQUIRE(EvaluateCondition(r, s).id == 0x101);

    // Answerable about everyone alive; a corpse has its own three questions.
    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
        REQUIRE(IsPredicateValidFor(static_cast<SubjectKind>(i), PredicateKind::Status) ==
                (static_cast<SubjectKind>(i) != SubjectKind::Corpse));
}

TEST_CASE("armour is asked as a percent, and every measure has a lowest and a highest", "[armor]")
{
    // The value is the damage reduction, 0 to 0.8, read as a percent like
    // health: Self: Armor > 50%.
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::ArmorPctAbove;
    r.conditionArg = 0.5f;

    Snapshot s = Healthy();
    s.traits.armor = 0.6f;
    REQUIRE(EvaluateCondition(r, s).ok);
    r.predicate = PredicateKind::ArmorPctBelow;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    r.conditionArg = 0.75f;
    REQUIRE(EvaluateCondition(r, s).ok);

    // The player, in fur.
    r.subject = SubjectKind::Player;
    r.conditionArg = 0.25f;
    Player(s).traits.armor = 0.1f;
    REQUIRE(EvaluateCondition(r, s).ok);

    // Enemies: a mage in robes, a chief in plate. The percent binds among
    // those that pass, by the measure; Lowest and Highest bind the
    // extremes of it whatever the number.
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {40.0f, 100.0f}, 300.0f});
    s.enemies[0].traits.armor = 0.05f;
    s.enemies[1].traits.armor = 0.7f;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::ArmorPctAbove;
    r.conditionArg = 0.5f;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.predicate = PredicateKind::ArmorLowest;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    r.predicate = PredicateKind::ArmorHighest;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.predicate = PredicateKind::HealthLowest;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.predicate = PredicateKind::HealthHighest;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);

    // Stamina and magicka have their extremes too, of their own measure.
    s.enemies[0].stamina = {20.0f, 100.0f};
    s.enemies[1].stamina = {80.0f, 100.0f};
    s.enemies[0].magicka = {90.0f, 100.0f};
    s.enemies[1].magicka = {10.0f, 100.0f};
    r.predicate = PredicateKind::StaminaLowest;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    r.predicate = PredicateKind::StaminaHighest;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.predicate = PredicateKind::MagickaLowest;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.predicate = PredicateKind::MagickaHighest;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);

    // No enemies: no extreme to bind.
    s.enemies.clear();
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // The extremes are of a group only; the percent is anyone's.
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::StaminaLowest));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::MagickaHighest));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::ArmorHighest));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::MagickaLowest));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::ArmorPctAbove));
    REQUIRE(ExtremesOf(PredicateKind::ArmorPctBelow).highest == PredicateKind::ArmorHighest);
    REQUIRE(ExtremesOf(PredicateKind::StaminaPctBelow).lowest == PredicateKind::StaminaLowest);
    REQUIRE(ExtremesOf(PredicateKind::MagickaPctBelow).highest == PredicateKind::MagickaHighest);
    REQUIRE(ExtremesOf(PredicateKind::CombatBegins).lowest == PredicateKind::CombatBegins);
    REQUIRE(AboveOf(PredicateKind::ArmorPctBelow) == PredicateKind::ArmorPctAbove);
    REQUIRE(IsAbove(PredicateKind::ArmorPctAbove));
    REQUIRE(ArgumentFor(PredicateKind::ArmorPctBelow) == ArgumentKind::Percent);
}

TEST_CASE("resistance is asked by kind as a percent, with a lowest and a highest", "[resistance]")
{
    // A flame atronach, immune to fire and weak to frost, and a bandit with
    // half fire resistance. The game's value is a percent; the rule's
    // number is a fraction of it, so 50 reads as 50%.
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {100.0f, 100.0f}, 300.0f});
    s.enemies[0].traits.SetResist(DamageKind::Fire, 100.0f);
    s.enemies[0].traits.SetResist(DamageKind::Frost, -33.0f);
    s.enemies[1].traits.SetResist(DamageKind::Fire, 50.0f);

    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::ResistancePctAbove;
    r.damageKind = DamageKind::Fire;
    r.conditionArg = 0.75f;
    REQUIRE(EvaluateCondition(r, s).id == 0x101); // only the immune one
    r.conditionArg = 0.25f;
    REQUIRE(EvaluateCondition(r, s).id == 0x101); // both pass; the MOST resistant binds
    r.predicate = PredicateKind::ResistancePctBelow;
    r.conditionArg = 0.75f;
    REQUIRE(EvaluateCondition(r, s).id == 0x102); // only the bandit
    r.damageKind = DamageKind::Frost;
    r.conditionArg = 0.25f;
    REQUIRE(EvaluateCondition(r, s).id == 0x101); // both pass; the weakness is the least
    r.damageKind = DamageKind::Shock;
    REQUIRE(EvaluateCondition(r, s).ok); // nobody resists shock: below 25% holds
    r.predicate = PredicateKind::ResistancePctAbove;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // The extremes, of the kind asked about.
    r.damageKind = DamageKind::Frost;
    r.predicate = PredicateKind::ResistanceLowest;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    r.predicate = PredicateKind::ResistanceHighest;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.damageKind = DamageKind::Fire;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);

    // The follower, a Nord: frost half off.
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::ResistancePctAbove;
    r.damageKind = DamageKind::Frost;
    r.conditionArg = 0.25f;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.traits.SetResist(DamageKind::Frost, 50.0f);
    REQUIRE(EvaluateCondition(r, s).ok);

    // Everyone can be asked; the extremes are a group's; every kind has a
    // name; the family is known as one.
    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
        REQUIRE(IsPredicateValidFor(static_cast<SubjectKind>(i), PredicateKind::ResistancePctBelow) ==
                (static_cast<SubjectKind>(i) != SubjectKind::Corpse));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::ResistanceLowest));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::ResistanceHighest));
    for (std::size_t i = 0; i < static_cast<std::size_t>(DamageKind::COUNT); ++i)
    {
        const auto v = static_cast<DamageKind>(i);
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(DamageFromWireName(WireName(v)) == v);
        REQUIRE(DisplayName(v).size() > 0);
    }
    REQUIRE(IsResistance(PredicateKind::ResistanceLowest));
    REQUIRE(IsResistance(PredicateKind::ResistancePctAbove));
    REQUIRE_FALSE(IsResistance(PredicateKind::HitBy));
    REQUIRE(ExtremesOf(PredicateKind::ResistancePctBelow).lowest == PredicateKind::ResistanceLowest);
    REQUIRE(AboveOf(PredicateKind::ResistancePctBelow) == PredicateKind::ResistancePctAbove);
}

TEST_CASE("attacked by is asked by kind, and the attacker can be the target", "[attacked]")
{
    // An ally under fire from an enemy: the condition binds the ally, and
    // the Attacker target aims the action at the one doing it.
    Snapshot s = Healthy();
    s.allies.push_back({0x201, {60.0f, 100.0f}, 400.0f});
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});

    Rule r;
    r.subject = SubjectKind::Ally;
    r.predicate = PredicateKind::HitBy;
    r.damageKind = DamageKind::Fire;
    r.actionTarget = ActionTargetKind::Attacker;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    s.allies[0].traits.hitBy = Bit(DamageKind::Fire) | Bit(DamageKind::Melee);
    s.allies[0].traits.attacker = 0x101;
    const Binding bound = EvaluateCondition(r, s);
    REQUIRE(bound.id == 0x201);
    bool ok = false;
    REQUIRE(ResolveActionTarget(r, s, bound, &ok) == 0x101);
    REQUIRE(ok);

    // Another kind was not what hit them.
    r.damageKind = DamageKind::Frost;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    r.damageKind = DamageKind::Melee;
    REQUIRE(EvaluateCondition(r, s).ok);

    // The follower's own attacker, and none when nothing has hit them.
    r.subject = SubjectKind::Self;
    r.damageKind = DamageKind::Melee;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    s.traits.hitBy = Bit(DamageKind::Melee);
    s.traits.attacker = 0x101;
    const Binding self = EvaluateCondition(r, s);
    REQUIRE(self.id == s.self);
    REQUIRE(ResolveActionTarget(r, s, self, &ok) == 0x101);
    s.traits.attacker = 0;
    REQUIRE(ResolveActionTarget(r, s, self, &ok) == 0);
    REQUIRE_FALSE(ok);

    // The player's.
    r.subject = SubjectKind::Player;
    r.damageKind = DamageKind::Shock;
    Player(s).traits.hitBy = Bit(DamageKind::Shock);
    Player(s).traits.attacker = 0x101;
    const Binding player = EvaluateCondition(r, s);
    REQUIRE(player.id == kPlayerFormID);
    REQUIRE(ResolveActionTarget(r, s, player, &ok) == 0x101);

    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
        REQUIRE(IsPredicateValidFor(static_cast<SubjectKind>(i), PredicateKind::HitBy) ==
                (static_cast<SubjectKind>(i) != SubjectKind::Corpse));
}

TEST_CASE("a named follower is one ally asked about alone", "[follower]")
{
    Snapshot s = Healthy();
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    s.allies.push_back({0x201, {30.0f, 100.0f}, 400.0f}); // Lydia, hurt
    s.allies.push_back({0x202, {90.0f, 100.0f}, 200.0f}); // Marcurio, fine

    Rule r;
    r.subject = SubjectKind::Follower;
    r.subjectForm = 0x202;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.5f;
    // Marcurio is fine, though an ally is hurt: only the named one counts.
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    r.subjectForm = 0x201;
    REQUIRE(EvaluateCondition(r, s).id == 0x201);

    // Not with us: no match, not an error.
    r.subjectForm = 0x203;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // Everything an ally answers.
    REQUIRE(IsPredicateValidFor(SubjectKind::Follower, PredicateKind::MagickaPctBelow));
    REQUIRE(IsPredicateValidFor(SubjectKind::Follower, PredicateKind::Status));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Follower, PredicateKind::HealthLowest));
}

TEST_CASE("the enemy on a member of the party, and the one a member is on", "[party]")
{
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {100.0f, 100.0f}, 200.0f, kPlayerFormID}); // going for the player
    s.enemies.push_back({0x103, {100.0f, 100.0f}, 250.0f, 0x201});         // going for a follower
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    s.allies.push_back({0x201, {60.0f, 100.0f}, 300.0f});
    Player(s).target = 0x101;
    s.allies[1].target = 0x103;
    s.currentTarget = 0x102;

    Rule r;
    r.subject = SubjectKind::Enemy;
    // The player: member 0.
    r.predicate = PredicateKind::Attacking;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);
    r.predicate = PredicateKind::AttackedBy;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    // Another follower, by id.
    r.subjectForm = 0x201;
    r.predicate = PredicateKind::Attacking;
    REQUIRE(EvaluateCondition(r, s).id == 0x103);
    r.predicate = PredicateKind::AttackedBy;
    REQUIRE(EvaluateCondition(r, s).id == 0x103);
    // The follower themself: their own target.
    r.subjectForm = s.self;
    REQUIRE(EvaluateCondition(r, s).id == 0x102);

    // The player fighting no one: nothing is their target.
    r.subjectForm = 0;
    Player(s).target = 0;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // Only the Enemy heading asks these.
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::Attacking));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::AttackedBy));
    // Any for the player and an ally.
    REQUIRE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::Any));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::Any));
    // Under an enemy condition the action goes to that enemy, not to the
    // attacker or the follower's target; under the follower's target, to it.
    REQUIRE(IsActionTargetValidFor(SubjectKind::Enemy, ActionTargetKind::Enemy));
    REQUIRE_FALSE(IsActionTargetValidFor(SubjectKind::Enemy, ActionTargetKind::Attacker));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Self, ActionTargetKind::Enemy));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Ally, ActionTargetKind::Attacker));
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
    Player(s).health = {90.0f, 100.0f};
    REQUIRE(EvaluateCondition(r, s).ok);
}

TEST_CASE("an unanswerable pair reports InvalidCondition, not ConditionFalse", "[validity]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Attacking; // nonsense: oneself, going for a party member
    r.subjectForm = 0;
    r.FirstAction() = DrinkMagicka();
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
Rule HurtBut(float pct, const Action &action, const char *label)
{
    Rule r = HealBelow(pct);
    r.FirstAction() = action;
    r.label = label;
    return r;
}

} // namespace

TEST_CASE("one situation draws its remedies in list order, one per turn", "[cooldown]")
{
    // Three rules, one problem. The list is a preference order: the potion
    // first, and if the next turn still finds them hurt, the next remedy. Each
    // action carries its own cooldown; nothing is keyed by the condition.
    RuleSet rs;
    rs.rules.push_back(HurtBut(0.25f, DrinkHealth(), "potion"));
    rs.rules.push_back(HurtBut(0.25f, DrinkMagicka(), "heal spell"));
    rs.rules.push_back(HurtBut(0.25f, DrinkStamina(), "back off"));

    Snapshot s = Healthy();
    s.health = {20.0f, 100.0f};
    // (magicka potions are in the bag already)

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
    s.now += MinimumCooldown(ActionKind::DrinkStrongest);
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
}

TEST_CASE("an unavailable action falls through immediately, in the same tick", "[cooldown]")
{
    // "No potions? Then try the next thing." Being unable to act is NOT a
    // cooldown: nothing was done, so nothing needs time to settle, and the next
    // remedy for the same problem should be tried at once rather than after a
    // wait. This is the distinction the condition cooldown must not blur.
    RuleSet rs;
    rs.rules.push_back(HurtBut(0.5f, DrinkHealth(), "potion"));
    rs.rules.push_back(HurtBut(0.5f, DrinkMagicka(), "back off"));

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    EmptyBag(s);

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
    spare.predicate = PredicateKind::Any;
    spare.FirstAction() = DrinkHealth();
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

    s.now += MinimumCooldown(ActionKind::DrinkStrongest);
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

TEST_CASE("a different situation is still free to draw a response", "[cooldown]")
{
    // Responding to low health must not silence an unrelated concern.
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Rule swarmed;
    swarmed.subject = SubjectKind::Enemy;
    swarmed.predicate = PredicateKind::Any;
    swarmed.actionTarget = ActionTargetKind::Self;
    swarmed.FirstAction() = DrinkStamina();
    swarmed.label = "back off when an enemy is near";
    rs.rules.push_back(swarmed);

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {50.0f, 100.0f}, 400.0f});

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
    hold.predicate = PredicateKind::Any;
    hold.actionTarget = ActionTargetKind::Self;
    hold.FirstAction() = DrinkStamina();
    hold.label = "brace: hold position";
    rs.rules.push_back(hold);

    Rule disengage = hold;
    disengage.FirstAction().kind = ActionKind::DrinkPotion; // a different action from rule 0's
    disengage.FirstAction().form = kMagickaPotion;
    disengage.label = "brace: break off";
    rs.rules.push_back(disengage);

    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});

    EvalContext ctx;

    // Rule 0 wins now, and keeps winning every time it comes off cooldown.
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
    s.now += MinimumCooldown(ActionKind::DrinkStrongest) + 0.01;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    // Make rule 0's action unavailable -- the bag runs out of stamina
    // potions, as an "already in effect" check would do -- and rule 1 gets
    // its turn. Note the wait: rule 0's fire also blocked the condition they
    // share, so the settle has to elapse first. Availability decides WHO
    // acts; the cooldown decides WHEN.
    EmptyBag(s, kStaminaPotion);
    s.now += MinimumCooldown(ActionKind::DrinkStrongest) + 0.01;

    Trace trace;
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::NoResource);
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
    potion.FirstAction() = DrinkHealth();
    potion.FirstAction().form = 0;
    rs.rules.push_back(potion);

    Snapshot s = Healthy();
    s.health = {50.0f, 100.0f};
    s.spells.known.push_back(kOakflesh);

    EvalContext ctx;
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = true;

    Trace trace;
    const auto skipped = Evaluate(rs, s, ctx, &trace);
    REQUIRE(skipped.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::Busy);

    // The pool clears. The potion's fire blocked the condition both rules
    // share, so that settle has to elapse -- but the cast rule itself must
    // carry NO cooldown from having been skipped.
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = false;
    s.now += MinimumCooldown(ActionKind::DrinkStrongest) + 0.01;
    const auto fired = Evaluate(rs, s, ctx, &trace);
    REQUIRE(fired.ruleIndex == 0);
    REQUIRE(trace.at(0) == Verdict::Fired);
}

TEST_CASE("a cast they cannot afford is reported and spends no cooldown", "[resources]")
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
    potion.FirstAction() = DrinkHealth();
    potion.FirstAction().form = 0;
    rs.rules.push_back(potion);

    Snapshot s = Healthy();
    s.health = {50.0f, 100.0f};
    s.magicka = {30.0f, 100.0f};
    s.spells.known.push_back(kHeal);
    s.spells.costs.push_back({kHeal, 60.0f});

    EvalContext ctx;

    // 30 magicka against a 60-point spell: the cast rule is reported, the
    // potion rule fires instead.
    Trace trace;
    const auto d1 = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d1.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::CannotAfford);

    // Magicka back, potion settle elapsed: the cast rule fires at once. It
    // must carry no cooldown from having been unaffordable.
    s.magicka = {100.0f, 100.0f};
    s.now += MinimumCooldown(ActionKind::DrinkStrongest) + 0.01;
    const auto d2 = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d2.ruleIndex == 0);
    REQUIRE(trace.at(0) == Verdict::Fired);

    // A spell with no recorded cost is never blocked on this ground.
    Snapshot t = Healthy();
    t.health = {50.0f, 100.0f};
    t.magicka = {0.0f, 100.0f};
    t.spells.known.push_back(kHeal);
    EvalContext ctx2;
    REQUIRE(Evaluate(rs, t, ctx2).ruleIndex == 0);
}

TEST_CASE("a named potion is drunk only while carried, and cools down per potion", "[potions]")
{
    constexpr std::uint32_t kStamina = 0x0003EAE7; // not the one Healthy() carries
    constexpr std::uint32_t kResistFire = 0x0003EB3E;

    auto drink = [](std::uint32_t form) {
        Rule r;
        r.subject = SubjectKind::Self;
        r.predicate = PredicateKind::Any;
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

    // Carries neither: both report it, nothing fires.
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(trace.at(1) == Verdict::NoResource);
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::DrinkPotion)) == "none in inventory");

    // Carries both: the first fires, and its cooldown is its own -- the
    // second potion fires on the next turn.
    s.potions.carried.push_back({kStamina, 6});
    s.potions.carried.push_back({kResistFire, 1});
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
    s.now += 0.5;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
}

TEST_CASE("food and an ingredient are eaten only while carried, and by their own kind", "[potions]")
{
    constexpr std::uint32_t kBeef = 0x00064B33;
    constexpr std::uint32_t kFlower = 0x000727DE;

    auto eat = [](ActionKind kind, std::uint32_t form) {
        Rule r;
        r.subject = SubjectKind::Self;
        r.predicate = PredicateKind::Any;
        r.actionTarget = ActionTargetKind::Self;
        r.FirstAction().kind = kind;
        r.FirstAction().form = form;
        return r;
    };

    RuleSet rs;
    rs.rules.push_back(eat(ActionKind::EatFood, kBeef));
    rs.rules.push_back(eat(ActionKind::EatIngredient, kFlower));

    Snapshot s = Healthy();
    EvalContext ctx;

    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(trace.at(1) == Verdict::NoResource);
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EatFood)) == "none in inventory");
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EatIngredient)) == "none in inventory");

    // The form under the wrong kind is not carried: a hand-edited profile
    // that puts the beef under eat-ingredient names nothing.
    s.potions.carried.push_back({kBeef, 2, ConsumableKind::Ingredient});
    s.potions.carried.push_back({kFlower, 3, ConsumableKind::Food});
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(trace.at(1) == Verdict::NoResource);

    // Under their own kinds: the first fires, and the second on the next turn.
    s.potions.carried.clear();
    s.potions.carried.push_back({kBeef, 2, ConsumableKind::Food});
    s.potions.carried.push_back({kFlower, 3, ConsumableKind::Ingredient});
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
    s.now += 0.5;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    // Only Self eats.
    REQUIRE(IsActionValidFor(ActionTargetKind::Self, ActionKind::EatFood));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Player, ActionKind::EatFood));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Enemy, ActionKind::EatIngredient));
}

TEST_CASE("a power is used like a cast: known, not running, not mid-cast, free of magicka", "[spell]")
{
    constexpr std::uint32_t kEmbraceOfShadows = 0x00088821;

    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::UsePower;
    r.FirstAction().form = kEmbraceOfShadows;
    rs.rules.push_back(r);

    Snapshot s = Healthy();
    s.magicka.current = 0.0f; // a power costs nothing, so this must not matter
    EvalContext ctx;

    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::UsePower)) == "does not know that power");

    s.spells.known.push_back(kEmbraceOfShadows);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);

    // Running -- the follower is invisible for three minutes -- it is not
    // used again, whatever the cooldown says.
    s.now += 10.0;
    s.spells.active.push_back(kEmbraceOfShadows);
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(std::string(Explain(Verdict::EffectActive, ActionKind::UsePower)) == "that power is still running");

    // A follower mid-cast on their own spell is left to finish, as for a
    // cast: the power's package would interrupt it the same way.
    s.spells.active.clear();
    s.now += 10.0;
    s.traits.status |= Bit(StatusKind::Casting);
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::Casting);

    // Aimed anywhere, like a cast: the menu sorts powers by delivery.
    REQUIRE(IsActionValidFor(ActionTargetKind::Enemy, ActionKind::UsePower));
    REQUIRE(IsCast(ActionKind::UsePower));

    // A shout is the same shape: known or not, through the pool.
    REQUIRE(IsCast(ActionKind::Shout));
    REQUIRE(IsActionValidFor(ActionTargetKind::Enemy, ActionKind::Shout));
    rs.rules[0].FirstAction().kind = ActionKind::Shout;
    rs.rules[0].FirstAction().form = 0x00013E07;
    s.traits.status = 0;
    s.now += 10.0;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::Shout)) == "does not know that shout");
    s.spells.known.push_back(0x00013E07);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);

    // Inside the voice's recovery from the last shout it waits, spending no
    // cooldown; when the recovery is over it fires.
    s.now += 10.0;
    s.voiceRecovery = 12.5f;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::Recovering);
    s.voiceRecovery = 0.0f;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);
    REQUIRE_FALSE(IsConsume(ActionKind::UsePower));
    REQUIRE(ConsumableOf(ActionKind::DrinkStrongest) == ConsumableKind::Potion);
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
// one of them above their skill. Nothing pinned.
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
    close.predicate = PredicateKind::HealthPctAbove; // the sword while the enemy is fresh
    close.conditionArg = 0.5f;
    rs.rules.push_back(close);
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, kBow, Hand::Both));

    Snapshot s = Armed();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 200.0f});
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

    // The enemy is worn down: the sword rule's condition lapses, and the
    // bow rule takes the hands. The sword's pin goes with them.
    s.enemies[0].health = {20.0f, 100.0f};
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);
    Pinned(s, d);
    REQUIRE(FindPin(s.pins, kSword) == nullptr);
    REQUIRE(FindPin(s.pins, kBow) != nullptr);

    // Fresh again: the sword rule is available again -- its pin is gone --
    // and takes the right hand back.
    s.enemies[0].health = {100.0f, 100.0f};
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

    // Not theirs.
    RuleSet rs;
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, 0xDEAD, Hand::Right));
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // Theirs, but a spell under equip-weapon: a hand-edited profile's mistake.
    rs.rules[0] = Equip(ActionKind::EquipWeapon, kFirebolt, Hand::Right);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // Above the follower's skill: the AI would never choose it, so a pin
    // would be a promise unkept. Said so, not fired -- and not cast either,
    // so that cast and equip agree.
    rs.rules[0] = Equip(ActionKind::EquipSpell, kChainLightning, Hand::Right);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::AboveSkill);
    rs.rules[0] = Equip(ActionKind::CastSpell, kChainLightning);
    s.spells.known.push_back(kChainLightning);
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::AboveSkill);

    // Two variants of the sword, the plain one and the smithed one: a rule
    // naming the smithed one has it, one naming a tempering not carried
    // has nothing, and one naming the form alone has whichever. With the
    // plain one pinned in the hand asked, the rule for the smithed one is
    // not done: they are two things.
    {
        Snapshot two = Armed();
        Holdable smithed = two.loadout[0];
        smithed.variant.emplace().tempering = 1.2f;
        two.loadout.push_back(smithed);
        RuleSet copies;
        copies.rules.push_back(Equip(ActionKind::EquipWeapon, kSword, Hand::Right));
        copies.rules[0].FirstAction().variant = smithed.variant;
        EvalContext fresh;
        REQUIRE(Evaluate(copies, two, fresh, &trace).ruleIndex == 0);
        copies.rules[0].FirstAction().variant->tempering = 1.5f;
        REQUIRE(Evaluate(copies, two, fresh, &trace).ruleIndex < 0);
        REQUIRE(trace.at(0) == Verdict::NoResource);
        copies.rules[0].FirstAction().variant = std::nullopt;
        two.now += 5.0; // past the first firing's cooldown, which keys on the form
        REQUIRE(Evaluate(copies, two, fresh, &trace).ruleIndex == 0);
        copies.rules[0].FirstAction().variant = smithed.variant;
        Holdable plainSword = two.loadout[0];
        plainSword.variant = ItemVariant{};
        AddPin(two.pins, plainSword, Hand::Right, false);
        two.now += 5.0;
        REQUIRE(Evaluate(copies, two, fresh, &trace).ruleIndex == 0);
        two.pins.clear();
        AddPin(two.pins, smithed, Hand::Right, false);
        two.now += 5.0;
        REQUIRE(Evaluate(copies, two, fresh, &trace).ruleIndex < 0);
        REQUIRE(trace.at(0) == Verdict::EffectActive);
    }

    // A spell they can use, in both hands at once.
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

TEST_CASE("the arrow policies choose by damage, pin what they chose, and follow the quiver", "[evaluator]")
{
    Snapshot s = Armed();
    // Iron arrows in the loadout already, damage 8; steel arrows beside
    // them, damage 10, and a handful of Daedric ones, damage 24.
    constexpr std::uint32_t kSteelArrows = 0x1397F;
    constexpr std::uint32_t kDaedricArrows = 0x139C0;
    for (Holdable &thing : s.loadout)
        if (thing.form == kArrows)
            thing.damage = 8.0f;
    Holdable steel = Held(kSteelArrows, Kind::Ammo, Grip::None);
    steel.damage = 10.0f;
    steel.count = 40;
    Holdable daedric = Held(kDaedricArrows, Kind::Ammo, Grip::None);
    daedric.damage = 24.0f;
    daedric.count = 5;
    s.loadout.push_back(steel);
    s.loadout.push_back(daedric);

    // Strongest: the Daedric, resolved onto the step for the game side.
    RuleSet rs;
    rs.rules.push_back(Equip(ActionKind::EquipStrongestArrows, 0));
    EvalContext ctx;
    Trace trace;
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.actionForm() == kDaedricArrows);
    // Pinned, the rule is done; the Daedric ones gone, the steel are the
    // strongest and the rule fires again for them.
    AddPin(s.pins, daedric, Hand::None, false);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    std::erase_if(s.loadout, [](const Holdable &t) { return t.form == kDaedricArrows; });
    s.pins.clear();
    s.now += 5.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.actionForm() == kSteelArrows);

    // Weakest: the iron.
    rs.rules[0] = Equip(ActionKind::EquipWeakestArrows, 0);
    s.now += 5.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.actionForm() == kArrows);

    // No arrows at all: nothing to choose, and the rule says so.
    std::erase_if(s.loadout, [](const Holdable &t) { return t.IsAmmo(); });
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::NoResource);

    // A policy names no form and takes no hand; a none it is not.
    REQUIRE_FALSE(NamesForm(ActionKind::EquipStrongestArrows));
    REQUIRE_FALSE(TakesHand(ActionKind::EquipWeakestArrows));
    REQUIRE(IsEquip(ActionKind::EquipStrongestArrows));
    REQUIRE(KindOf(ActionKind::EquipWeakestArrows) == Kind::Ammo);
}

TEST_CASE("a weapon none lets go of the hand it names, and is done when that hand holds no pin", "[evaluator]")
{
    Snapshot s = Armed();
    AddPin(s.pins, s.loadout[0], Hand::Right, false); // the sword, right
    RuleSet rs;
    rs.rules.push_back(Equip(ActionKind::EquipWeapon, 0, Hand::Left));
    EvalContext ctx;
    Trace trace;
    // The left holds no weapon pin: nothing to let go of there.
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex < 0);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    // The right does, and Both covers it too.
    rs.rules[0] = Equip(ActionKind::EquipWeapon, 0, Hand::Right);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);
    rs.rules[0] = Equip(ActionKind::EquipWeapon, 0, Hand::Both);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);
    // A none with no hand, as an old profile might carry: any weapon pin.
    rs.rules[0] = Equip(ActionKind::EquipWeapon, 0, Hand::None);
    s.now += 5.0;
    REQUIRE(Evaluate(rs, s, ctx, &trace).ruleIndex == 0);
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
    if (d.step && IsEquip(d.step->action.kind) && d.step->action.form != 0)
        Pinned(s, d);
    return d;
}

std::vector<ActionKind> Kinds(const Decision &d)
{
    std::vector<ActionKind> out;
    if (d.step)
        out.push_back(d.step->action.kind);
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
    r.actions.push_back(DrinkHealth());
    r.actions.push_back({ActionKind::CastSpell, kOakflesh});
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Armed();
    EmptyBag(s);
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

TEST_CASE("everything after a cast waits while the cast is in the air", "[sequence]")
{
    // "Combat start: cast Stoneflesh, equip Flames." The game side marks
    // every action busy while a cast of ours is in the air, so the equip
    // waits rather than putting Flames in the casting hand mid-cast.
    constexpr std::uint32_t kStoneflesh = 0x0005AD5D;
    Rule r = Equip(ActionKind::EquipSpell, kFirebolt, Hand::Right);
    r.actions.insert(r.actions.begin(), {ActionKind::CastSpell, kStoneflesh});
    RuleSet rs;
    rs.rules = {r, HealBelow(0.5f)};

    Snapshot s = Armed();
    s.health = {40.0f, 100.0f};
    s.spells.known.push_back(kStoneflesh);
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::CastSpell);

    // In the air: the equip waits, and so would any other rule.
    ctx.caps.busy.fill(true);
    REQUIRE_FALSE(Tick(rs, s, ctx, trace, actions).Fired());
    REQUIRE(trace.at(0) == Verdict::Busy);
    REQUIRE(trace.at(1) == Verdict::NotReached);

    // Landed: the equip goes.
    ctx.caps.busy.fill(false);
    const Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.action() == ActionKind::EquipSpell);
    REQUIRE(d.actionForm() == kFirebolt);
}

TEST_CASE("a list keeps the target and the actions it began with", "[sequence]")
{
    // Bound to the weakest enemy when it began, the list keeps aiming at
    // that enemy though another becomes the weakest; and it keeps its
    // actions and its name though the rule is edited and moved under it.
    constexpr std::uint32_t kA = 0x00012FCC;
    constexpr std::uint32_t kB = 0x0005AD5C;
    Rule r;
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 0.9f;
    r.actionTarget = ActionTargetKind::Enemy;
    r.actions = {{ActionKind::CastSpell, kA}, {ActionKind::CastSpell, kB}};
    r.label = "focus the weakest";
    RuleSet rs;
    rs.rules = {r};

    Snapshot s = Healthy();
    s.spells.known = {kA, kB};
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {80.0f, 100.0f}, 300.0f});
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.targetId() == 0x101);
    REQUIRE(d.subjectId() == 0x101);
    REQUIRE(d.rule.label == "focus the weakest");

    // The other enemy is now the weakest, the rule says something else
    // entirely under another name, and another rule has been put above it:
    // the list in progress is unmoved by any of it.
    s.enemies[0].health = {90.0f, 100.0f};
    s.enemies[1].health = {10.0f, 100.0f};
    rs.rules[0].actions = {DrinkStamina()};
    rs.rules[0].label = "rewritten";
    rs.rules.insert(rs.rules.begin(), HealBelow(0.9f));
    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.actionForm() == kB);
    REQUIRE(d.targetId() == 0x101);
    REQUIRE(d.subjectId() == 0x101);
    REQUIRE(d.rule.label == "focus the weakest");
    REQUIRE_FALSE(ctx.pending.Active());
}

TEST_CASE("a decision names whom its condition bound", "[sequence]")
{
    constexpr std::uint32_t kReanimate = 0x00065BD7; // cap 13
    constexpr ActorId kWolf = 0x201;
    constexpr ActorId kBandit = 0x1002;

    Snapshot s = Healthy();
    s.inCombat = true;
    s.health = {40.0f, 100.0f};
    s.enemies.push_back({kWolf, {30.0f, 100.0f}, 300.0f});
    s.spells.known.push_back(kReanimate);
    s.spells.caps.push_back({kReanimate, 13});

    const auto bound = [&](const Rule &r) {
        RuleSet rs;
        rs.rules = {r};
        EvalContext ctx;
        const Decision d = Evaluate(rs, s, ctx);
        REQUIRE(d.Fired());
        return d.subjectId();
    };

    SECTION("the follower")
    {
        CHECK(bound(HealBelow(0.5f)) == s.self);
    }

    SECTION("the enemy it matched, whoever the action is aimed at")
    {
        Rule r = HealBelow(0.5f);
        r.subject = SubjectKind::Enemy;
        r.actionTarget = ActionTargetKind::Self;
        CHECK(bound(r) == kWolf);
    }

    SECTION("the corpse it matched")
    {
        s.corpses.push_back({kBandit, 9, 300.0f});
        Rule r;
        r.subject = SubjectKind::Corpse;
        r.predicate = PredicateKind::LevelHighest;
        r.actionTarget = ActionTargetKind::Corpse;
        r.FirstAction().kind = ActionKind::CastSpell;
        r.FirstAction().form = kReanimate;
        CHECK(bound(r) == kBandit);
    }

    SECTION("no one, for no corpse: its binding is the follower")
    {
        Rule r = HealBelow(0.5f);
        r.subject = SubjectKind::Corpse;
        r.predicate = PredicateKind::CorpseNone;
        CHECK(bound(r) == 0);
    }
}

TEST_CASE("a rule's verdict is reported when it changes", "[verdicts]")
{
    using V = Verdict;
    using Indices = std::vector<std::size_t>;
    Trace reported;

    // The first evaluation gives each rule's word once; not reached says
    // nothing, and a fire is rule.fired's to report.
    CHECK(VerdictChanges(reported, {V::Fired, V::ConditionFalse, V::NotReached}) == Indices{1});
    CHECK(VerdictChanges(reported, {V::Fired, V::ConditionFalse, V::NotReached}).empty());

    // The rule that fired is on cooldown now, and the false one is blocked.
    CHECK(VerdictChanges(reported, {V::ActionCooldown, V::NoResource, V::NotReached}) == Indices{0, 1});

    // Not reached keeps the last word, so coming back to it is no change.
    CHECK(VerdictChanges(reported, {V::NotReached, V::NotReached, V::NotReached}).empty());
    CHECK(VerdictChanges(reported, {V::ActionCooldown, V::NoResource, V::ConditionFalse}) == Indices{2});

    // The rules changed: a list of another length starts again.
    CHECK(VerdictChanges(reported, {V::NoResource}) == Indices{0});
}

TEST_CASE("a list goes on after its condition has lapsed", "[sequence]")
{
    // Deliberate. "Health below half: drink, then cast the heal" -- the
    // potion works, health is above half by the next tick, and the heal is
    // cast anyway. The list is a commitment once begun, and it has to be:
    // "combat start" holds for one tick only, and re-reading it would
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

    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::DrinkStrongest);
    s.health = {90.0f, 100.0f};
    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::CastSpell);
}

TEST_CASE("the cooldowns a list spends are the actions' own", "[sequence]")
{
    // Drinking as the first step of a list spaces the next drink exactly as
    // a single-action rule's would, wherever it sits: the settle belongs to
    // the action, and a list does not get a second potion inside it.
    Rule r = HealBelow(0.5f);
    r.actions.push_back(DrinkStamina());
    RuleSet rs;
    rs.rules = {r, HealBelow(0.5f)};

    Snapshot s = Healthy();
    s.health = {40.0f, 100.0f};
    EvalContext ctx;
    Trace trace;
    ActionTrace actions;

    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::DrinkStrongest);
    REQUIRE(Tick(rs, s, ctx, trace, actions).action() == ActionKind::DrinkStrongest);

    // One second in: both potion rules are inside the settle.
    Decision d = Tick(rs, s, ctx, trace, actions);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    REQUIRE(trace.at(1) == Verdict::ActionCooldown);

    // Past it, the list begins again from its first action.
    s.now += MinimumCooldown(ActionKind::DrinkStrongest);
    d = Tick(rs, s, ctx, trace, actions);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.action() == ActionKind::DrinkStrongest);
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
    s.now += MinimumCooldown(ActionKind::DrinkStrongest) + 5.0;
    s.potions.running = {"Restore Health"};

    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    // Not "no potion" -- they have twelve. The reason has to be the real one or
    // the log sends you to check the inventory for nothing.
    REQUIRE(trace.at(0) == Verdict::EffectActive);

    // Dose finished, still hurt: free to drink again.
    s.potions.running.clear();
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
    REQUIRE(s.potions.running.empty());

    EvalContext ctx;

    REQUIRE(Evaluate(rs, s, ctx).Fired());

    s.now += 0.15;
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    s.now += MinimumCooldown(ActionKind::DrinkStrongest);
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
        EmptyBag(s);
        EvalContext ctx;
        Trace trace;
        Evaluate(rs, s, ctx, &trace);
        REQUIRE(trace.at(0) == Verdict::NoResource);
    }

    SECTION("full bag, dose still running")
    {
        s.potions.running = {"Restore Health"};
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

TEST_CASE("Stat::Pct does not divide by zero", "[snapshot]")
{
    REQUIRE(Stat{}.Pct() == 0.0f);
    REQUIRE(Stat{50.0f, 200.0f}.Pct() == 0.25f);
}

TEST_CASE("a summon is a condition on any actor: none or active", "[summon]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::SummonNone;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = 0x000204C3; // Conjure Flame Atronach
    rs.rules.push_back(r);

    Snapshot s = Healthy();
    s.spells.known.push_back(0x000204C3);
    EvalContext ctx;

    // Nothing commanded: none holds, so the atronach is called.
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    // One commanded: none no longer holds, active does.
    s.now += 10.0;
    s.traits.summons = 1;
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);
    rs.rules[0].predicate = PredicateKind::SummonActive;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);

    // Answerable about anyone with traits, never about a corpse.
    REQUIRE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::SummonActive));
    REQUIRE(IsPredicateValidFor(SubjectKind::Ally, PredicateKind::SummonNone));
    REQUIRE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::SummonActive));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Corpse, PredicateKind::SummonActive));
}

TEST_CASE("a corpse is bound by level, within what the rule's spell can raise", "[corpse]")
{
    constexpr std::uint32_t kReanimate = 0x00065BD7; // cap 13
    constexpr ActorId kRat = 0x1001;
    constexpr ActorId kBandit = 0x1002;
    constexpr ActorId kGiant = 0x1003;

    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Corpse;
    r.predicate = PredicateKind::LevelHighest;
    r.actionTarget = ActionTargetKind::Corpse;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = kReanimate;
    rs.rules.push_back(r);

    Snapshot s = Healthy();
    s.inCombat = true;
    s.spells.known.push_back(kReanimate);
    s.spells.caps.push_back({kReanimate, 13});
    EvalContext ctx;

    // No corpses: nothing to bind; None holds instead.
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);

    // A rat, a bandit and a giant: the giant is above the cap and is passed
    // over; the bandit outranks the rat. The cast is aimed at the bandit.
    s.corpses.push_back({kRat, 1, 100.0f});
    s.corpses.push_back({kBandit, 9, 300.0f});
    s.corpses.push_back({kGiant, 32, 200.0f});
    auto decision = Evaluate(rs, s, ctx);
    REQUIRE(decision.ruleIndex == 0);
    REQUIRE(decision.step->target == kBandit);

    // Lowest picks the rat -- that is what the player asked for.
    s.now += 10.0;
    rs.rules[0].predicate = PredicateKind::LevelLowest;
    decision = Evaluate(rs, s, ctx);
    REQUIRE(decision.ruleIndex == 0);
    REQUIRE(decision.step->target == kRat);

    // A rule with no cap on its spell sees the giant: with only the giant
    // about, a conjuration on Self fires where a capped Reanimate would not.
    s.now += 10.0;
    s.corpses.clear();
    s.corpses.push_back({kGiant, 32, 200.0f});
    rs.rules[0].predicate = PredicateKind::LevelHighest;
    rs.rules[0].actionTarget = ActionTargetKind::Self;
    rs.rules[0].FirstAction().form = 0x000204C3;
    s.spells.known.push_back(0x000204C3);
    decision = Evaluate(rs, s, ctx);
    REQUIRE(decision.ruleIndex == 0);
    REQUIRE(decision.step->target == s.self);
    s.now += 10.0;
    rs.rules[0].FirstAction().form = kReanimate;
    REQUIRE_FALSE(Evaluate(rs, s, ctx).Fired());
    s.corpses.push_back({kRat, 1, 100.0f});
    s.corpses.push_back({kBandit, 9, 300.0f});

    // None: only when nothing raisable is about. With only the giant and a
    // capped spell, none holds and the action goes on the follower.
    s.now += 10.0;
    s.corpses.clear();
    s.corpses.push_back({kGiant, 32, 200.0f});
    rs.rules[0].predicate = PredicateKind::CorpseNone;
    rs.rules[0].actionTarget = ActionTargetKind::Self;
    rs.rules[0].FirstAction().form = kReanimate;
    decision = Evaluate(rs, s, ctx);
    REQUIRE(decision.ruleIndex == 0);
    REQUIRE(decision.step->target == s.self);

    // Only a Reanimate -- a spell with a cap -- goes at a corpse: Firebolt
    // aimed at one is unsupported, as the menu never offers it.
    s.now += 10.0;
    s.corpses.push_back({kBandit, 9, 300.0f});
    rs.rules[0].predicate = PredicateKind::LevelHighest;
    rs.rules[0].actionTarget = ActionTargetKind::Corpse;
    rs.rules[0].FirstAction().form = 0x00012FCD;
    s.spells.known.push_back(0x00012FCD);
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::Unsupported);

    // The corpse's questions are its own, and only a spell goes at one.
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Corpse, PredicateKind::HealthPctBelow));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::LevelHighest));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Corpse, ActionTargetKind::Corpse));
    REQUIRE_FALSE(IsActionTargetValidFor(SubjectKind::Enemy, ActionTargetKind::Corpse));
    REQUIRE(IsActionValidFor(ActionTargetKind::Corpse, ActionKind::CastSpell));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Corpse, ActionKind::Shout));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Corpse, ActionKind::Attack));
}

TEST_CASE("an apply rule needs a weapon that takes a poison, and waits on one already poisoned", "[evaluator]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 2.0f; // always
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction() = Apply("Damage Health");
    rs.rules.push_back(r);
    EvalContext ctx;

    Snapshot s = Healthy();
    s.potions.Add(0x3A5A4, 3, ConsumableKind::Poison, {"Damage Health", 15.0f, 0.0f});

    SECTION("no weapon in hand: not met, with its own verdict")
    {
        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::NothingToPoison);
    }

    SECTION("every weapon in hand poisoned: waits, as a buff rule waits on the buff")
    {
        s.rightWeapon = {true, true};
        s.leftWeapon = {true, true};
        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::EffectActive);
    }

    SECTION("a clean weapon and a poison carried: fires")
    {
        s.rightWeapon = {true, false};
        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE(d.Fired());
        REQUIRE(d.action() == ActionKind::ApplyStrongest);
    }

    SECTION("a poisoned sword right and a clean dagger left: fires, for the dagger")
    {
        s.rightWeapon = {true, true};
        s.leftWeapon = {true, false};
        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE(d.Fired());
    }

    SECTION("no poison carried: none in inventory, before the weapon is asked about")
    {
        s.rightWeapon = {true, false};
        EmptyBag(s, 0x3A5A4);
        Trace trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::NoResource);
    }
}

TEST_CASE("weapon poisoned and unpoisoned read each hand", "[evaluator]")
{
    Snapshot s = Healthy();
    Rule r;
    r.subject = SubjectKind::Self;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction() = DrinkMagicka();
    RuleSet rs;
    rs.rules.push_back(r);
    // A fresh context each time: a firing sets the action's cooldown, which
    // is not what is being asked here.
    const auto holds = [&](PredicateKind p) {
        EvalContext ctx;
        rs.rules[0].predicate = p;
        return Evaluate(rs, s, ctx).Fired();
    };

    // Nothing in hand: neither.
    REQUIRE_FALSE(holds(PredicateKind::WeaponPoisonNone));
    REQUIRE_FALSE(holds(PredicateKind::WeaponPoisonActive));
    // A clean sword: unpoisoned only.
    s.rightWeapon = {true, false};
    REQUIRE(holds(PredicateKind::WeaponPoisonNone));
    REQUIRE_FALSE(holds(PredicateKind::WeaponPoisonActive));
    // A poisoned sword right and a clean dagger left: both.
    s.rightWeapon = {true, true};
    s.leftWeapon = {true, false};
    REQUIRE(holds(PredicateKind::WeaponPoisonNone));
    REQUIRE(holds(PredicateKind::WeaponPoisonActive));
    // Both poisoned: poisoned only.
    s.leftWeapon = {true, true};
    REQUIRE_FALSE(holds(PredicateKind::WeaponPoisonNone));
    REQUIRE(holds(PredicateKind::WeaponPoisonActive));
    // A staff: nothing to say.
    s.rightWeapon = {};
    s.leftWeapon = {};
    REQUIRE_FALSE(holds(PredicateKind::WeaponPoisonNone));

    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::WeaponPoisonNone));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Player, PredicateKind::WeaponPoisonActive));
}

TEST_CASE("the gem for a charge: the largest that fits, else the smallest carried", "[evaluator]")
{
    // Petty 250, lesser 500, common 1000, as the game's settings have them.
    const std::vector<Snapshot::SoulGemView> gems{{0xA, 2, 250.0f}, {0xB, 1, 500.0f}, {0xC, 3, 1000.0f}};

    // A weapon short by 600: strongest is the lesser (the common would
    // overfill), weakest the petty.
    REQUIRE(ChooseSoulGem(gems, 600.0f, true) == 0xB);
    REQUIRE(ChooseSoulGem(gems, 600.0f, false) == 0xA);
    // Short by 1500: the common fits.
    REQUIRE(ChooseSoulGem(gems, 1500.0f, true) == 0xC);
    // Short by 100: every gem would overfill; both policies take the
    // smallest, which overfills least.
    REQUIRE(ChooseSoulGem(gems, 100.0f, true) == 0xA);
    REQUIRE(ChooseSoulGem(gems, 100.0f, false) == 0xA);
    // A gem with none left does not count.
    const std::vector<Snapshot::SoulGemView> out{{0xA, 0, 250.0f}, {0xC, 1, 1000.0f}};
    REQUIRE(ChooseSoulGem(out, 100.0f, false) == 0xC);
    REQUIRE(ChooseSoulGem({}, 100.0f, true) == 0);
}

TEST_CASE("a charge rule needs an enchanted weapon, and waits when none needs a charge", "[evaluator]")
{
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 2.0f;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::ChargeStrongestSoulGem;
    rs.rules.push_back(r);

    Snapshot s = Healthy();
    s.soulGems = {{0xA, 1, 250.0f}, {0xB, 1, 60.0f}};
    Decision last;
    const auto verdict = [&]() {
        EvalContext ctx;
        Trace trace;
        last = Evaluate(rs, s, ctx, &trace);
        return trace.at(0);
    };

    // A plain sword: nothing to charge.
    s.rightWeapon = {true, false, false, 0.0f, 0.0f, 0.0f};
    REQUIRE(verdict() == Verdict::NothingToCharge);
    // An enchanted sword with charge for many hits: waits.
    s.rightWeapon = {true, false, true, 80.0f, 100.0f, 20.0f};
    REQUIRE(verdict() == Verdict::EffectActive);
    // One that cannot pay for the next hit: fires, and the step carries the
    // gem the policy chose for what that hand is missing -- the largest
    // that fits the 90 missing, so the game side has only to spend it.
    s.rightWeapon = {true, false, true, 10.0f, 100.0f, 20.0f};
    REQUIRE(verdict() == Verdict::Fired);
    REQUIRE(last.step->action.form == 0xB);
    // The left hand's need sizes the gem when the right is fine.
    s.rightWeapon = {true, false, true, 80.0f, 100.0f, 20.0f};
    s.leftWeapon = {true, false, true, 5.0f, 500.0f, 20.0f};
    REQUIRE(verdict() == Verdict::Fired);
    REQUIRE(last.step->action.form == 0xA);
    s.leftWeapon = {};
    s.rightWeapon = {true, false, true, 10.0f, 100.0f, 20.0f};
    // No gem carried: none in inventory.
    s.soulGems.clear();
    REQUIRE(verdict() == Verdict::NoResource);

    // The condition itself, hand by hand.
    s.soulGems = {{0xA, 1, 250.0f}};
    rs.rules[0].predicate = PredicateKind::WeaponChargeNeeded;
    rs.rules[0].FirstAction() = DrinkMagicka();
    s.rightWeapon = {true, false, true, 80.0f, 100.0f, 20.0f};
    s.leftWeapon = {true, false, true, 5.0f, 100.0f, 20.0f};
    REQUIRE(verdict() == Verdict::Fired);
    s.leftWeapon = {};
    REQUIRE(verdict() == Verdict::ConditionFalse);
    REQUIRE(IsPredicateValidFor(SubjectKind::Self, PredicateKind::WeaponChargeNeeded));
    REQUIRE_FALSE(IsPredicateValidFor(SubjectKind::Enemy, PredicateKind::WeaponChargeNeeded));
}

TEST_CASE("a status no action could answer is not asked about the follower themself", "[vocabulary]")
{
    for (const auto status : {StatusKind::BleedingOut, StatusKind::Casting, StatusKind::Fleeing, StatusKind::Staggered})
    {
        REQUIRE_FALSE(IsStatusValidFor(SubjectKind::Self, status));
        REQUIRE(IsStatusValidFor(SubjectKind::Enemy, status));
    }
    REQUIRE_FALSE(IsStatusValidFor(SubjectKind::Player, StatusKind::BleedingOut));
    REQUIRE_FALSE(IsStatusValidFor(SubjectKind::Player, StatusKind::Fleeing));
    REQUIRE(IsStatusValidFor(SubjectKind::Player, StatusKind::Casting));
    REQUIRE(IsStatusValidFor(SubjectKind::Self, StatusKind::Poisoned));
    REQUIRE(IsStatusValidFor(SubjectKind::Self, StatusKind::Burning));
}

TEST_CASE("the enemy an action goes to, read from the condition", "[binding]")
{
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x102, {100.0f, 100.0f}, 150.0f});

    Rule r;
    r.actionTarget = ActionTargetKind::Enemy;
    // Under an enemy condition: the one matched.
    r.subject = SubjectKind::Enemy;
    Binding matched{0x101, true};
    bool ok = false;
    REQUIRE(ResolveActionTarget(r, s, matched, &ok) == 0x101);
    REQUIRE(ok);
    // Under the follower's own condition: whoever they are fighting.
    r.subject = SubjectKind::Self;
    s.currentTarget = 0x101;
    REQUIRE(ResolveActionTarget(r, s, Binding{s.self, true}, &ok) == 0x101);
    // Fighting no one: the nearest enemy.
    s.currentTarget = 0;
    REQUIRE(ResolveActionTarget(r, s, Binding{s.self, true}, &ok) == 0x102);
    // No enemy at all: no one.
    s.enemies.clear();
    REQUIRE(ResolveActionTarget(r, s, Binding{s.self, true}, &ok) == 0);
    REQUIRE_FALSE(ok);
}

TEST_CASE("Hit type asks what is in hand, of anyone", "[binding]")
{
    for (const auto subject : {SubjectKind::Self, SubjectKind::Player, SubjectKind::Ally, SubjectKind::Enemy})
        REQUIRE(IsPredicateValidFor(subject, PredicateKind::HitType));

    Snapshot s = Healthy();
    s.traits.Wield(DamageKind::Melee);
    s.traits.Wield(DamageKind::Fire); // a flaming sword
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f});
    s.enemies[0].traits.Wield(DamageKind::Magic);

    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HitType;
    r.damageKind = DamageKind::Melee;
    REQUIRE(EvaluateCondition(r, s).ok);
    r.damageKind = DamageKind::Fire;
    REQUIRE(EvaluateCondition(r, s).ok);
    r.damageKind = DamageKind::Magic;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // Any is not a hit type. The game side reads hands with nothing in them
    // as Melee -- fists, and a bear's claws -- so every actor hits with
    // something and the condition would be true of everyone: the plain Any
    // condition under a heading that promises a filter. It is refused, not
    // answered, so a profile carrying it reports InvalidCondition rather
    // than firing on every tick.
    r.damageKind = DamageKind::Any;
    REQUIRE_FALSE(IsDamageKindValidFor(PredicateKind::HitType, DamageKind::Any));
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    // Under Hit by it is the whole point: hit with anything at all.
    REQUIRE(IsDamageKindValidFor(PredicateKind::HitBy, DamageKind::Any));

    r.subject = SubjectKind::Enemy;
    r.damageKind = DamageKind::Magic;
    REQUIRE(EvaluateCondition(r, s).id == 0x101);
    r.damageKind = DamageKind::Melee;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);

    // Empty hands in the snapshot hit with nothing: the core compares bits,
    // and it is Sensors that puts Melee in them for an actor holding nothing.
    r.subject = SubjectKind::Player;
    r.damageKind = DamageKind::Melee;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
}

TEST_CASE("a resistance is asked only about a kind something resists", "[binding]")
{
    // Nothing resists a blow or an arrow but armour, which is its own
    // condition, and nothing resists "any": the menu offers neither, and a
    // hand-written profile asking anyway is refused rather than answered
    // against the 0 those slots hold.
    for (const auto which : {PredicateKind::ResistancePctBelow, PredicateKind::ResistancePctAbove,
                             PredicateKind::ResistanceLowest, PredicateKind::ResistanceHighest})
    {
        REQUIRE_FALSE(IsDamageKindValidFor(which, DamageKind::Melee));
        REQUIRE_FALSE(IsDamageKindValidFor(which, DamageKind::Ranged));
        REQUIRE_FALSE(IsDamageKindValidFor(which, DamageKind::Any));
        REQUIRE(IsDamageKindValidFor(which, DamageKind::Fire));
        REQUIRE(IsDamageKindValidFor(which, DamageKind::Magic));
    }
    // A predicate that reads no damage kind is unaffected by whichever one
    // the rule happens to carry.
    for (const auto kind : {DamageKind::Any, DamageKind::Melee, DamageKind::Fire})
        REQUIRE(IsDamageKindValidFor(PredicateKind::HealthPctBelow, kind));

    // The two the menu offers at zero, which no threshold above zero can
    // express: weak to a kind, and resistant to it at all.
    Snapshot s = Healthy();
    s.traits.SetResist(DamageKind::Fire, -50.0f);
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::ResistancePctBelow;
    r.damageKind = DamageKind::Fire;
    r.conditionArg = 0.0f;
    REQUIRE(EvaluateCondition(r, s).ok);
    s.traits.SetResist(DamageKind::Fire, 25.0f);
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    r.predicate = PredicateKind::ResistancePctAbove;
    REQUIRE(EvaluateCondition(r, s).ok);
    r.damageKind = DamageKind::Any;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
}

TEST_CASE("a policy chooses the bottle by its effect, strongest or weakest", "[evaluator]")
{
    Snapshot s = Healthy();
    // Three health potions of three strengths, one of them also a magicka
    // potion (a Well-being), and a resist-fire that no health rule sees.
    s.potions.carried.clear();
    s.potions.Add(0x101, 1, ConsumableKind::Potion, {"Restore Health", 25.0f, 0.0f});
    s.potions.Add(0x102, 1, ConsumableKind::Potion, {"Restore Health", 75.0f, 0.0f});
    s.potions.Add(0x103, 1, ConsumableKind::Potion, {"Restore Health", 50.0f, 0.0f});
    s.potions.Add(0x103, 1, ConsumableKind::Potion, {"Restore Magicka", 50.0f, 0.0f});
    s.potions.Add(0x104, 1, ConsumableKind::Potion, {"Resist Fire", 30.0f, 60.0f});

    REQUIRE(ChosenForm(Drink("Restore Health"), s) == 0x102);
    REQUIRE(ChosenForm(Drink("Restore Health", false), s) == 0x101);
    REQUIRE(ChosenForm(Drink("Restore Magicka"), s) == 0x103);
    REQUIRE(ChosenForm(Drink("Resist Fire"), s) == 0x104);
    REQUIRE(ChosenForm(Drink("Restore Stamina"), s) == 0);
    // A poison is not a potion, whatever its effect says.
    s.potions.Add(0x105, 1, ConsumableKind::Poison, {"Restore Health", 999.0f, 0.0f});
    REQUIRE(ChosenForm(Drink("Restore Health"), s) == 0x102);
    REQUIRE(ChosenForm(Apply("Restore Health"), s) == 0x105);
    // Food and a food-ingredient are chosen from their own kind.
    s.potions.Add(0x106, 1, ConsumableKind::Food, {"Restore Health", 2.0f, 0.0f});
    s.potions.Add(0x107, 1, ConsumableKind::Ingredient, {"Restore Health", 5.0f, 0.0f});
    Action food;
    food.kind = ActionKind::EatStrongestFood;
    food.effect = "Restore Health";
    REQUIRE(ChosenForm(food, s) == 0x106);
    food.kind = ActionKind::EatWeakestIngredient;
    REQUIRE(ChosenForm(food, s) == 0x107);
    REQUIRE(ChosenForm(Drink("Restore Health"), s) == 0x102);
    // A named bottle is its own form.
    Action named;
    named.kind = ActionKind::DrinkPotion;
    named.form = 0x104;
    REQUIRE(ChosenForm(named, s) == 0x104);

    // Equal magnitudes: the longer one is the stronger. No magnitude at
    // all: the duration is the strength.
    s.potions.carried.clear();
    s.potions.Add(0x201, 1, ConsumableKind::Poison, {"Lingering Damage Health", 1.0f, 10.0f});
    s.potions.Add(0x202, 1, ConsumableKind::Poison, {"Lingering Damage Health", 1.0f, 15.0f});
    s.potions.Add(0x203, 1, ConsumableKind::Poison, {"Paralysis", 0.0f, 3.0f});
    s.potions.Add(0x204, 1, ConsumableKind::Poison, {"Paralysis", 0.0f, 7.0f});
    REQUIRE(ChosenForm(Apply("Lingering Damage Health"), s) == 0x202);
    REQUIRE(ChosenForm(Apply("Lingering Damage Health", false), s) == 0x201);
    REQUIRE(ChosenForm(Apply("Paralysis"), s) == 0x204);

    // The decision carries the chosen bottle, so the game side has only
    // to use it; and a rule for an effect not carried has no resource.
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction() = Apply("Paralysis");
    rs.rules.push_back(r);
    s.rightWeapon = {true, false};
    EvalContext ctx;
    const auto d = Evaluate(rs, s, ctx);
    REQUIRE(d.Fired());
    REQUIRE(d.step->action.form == 0x204);
    rs.rules[0].FirstAction() = Apply("Fear");
    EvalContext ctx2;
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx2, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoResource);
}

TEST_CASE("a potion whose effect is still running is not drunk again", "[cooldown]")
{
    Snapshot s = Healthy();
    s.potions.Add(0x301, 2, ConsumableKind::Potion, {"Resist Fire", 30.0f, 60.0f});
    RuleSet rs;
    Rule r;
    r.subject = SubjectKind::Self;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction() = Drink("Resist Fire");
    rs.rules.push_back(r);
    EvalContext ctx;
    REQUIRE(Evaluate(rs, s, ctx).Fired());
    s.now += 10.0;
    s.potions.running = {"Resist Fire"};
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    // Another effect running is no reason to wait.
    s.potions.running = {"Fortify Health"};
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

TEST_CASE("two policies for two effects are two actions, each on its own cooldown", "[cooldown]")
{
    Snapshot s = Healthy();
    s.potions.Add(0x301, 2, ConsumableKind::Potion, {"Resist Fire", 30.0f, 60.0f});
    RuleSet rs;
    Rule fire;
    fire.subject = SubjectKind::Self;
    fire.actionTarget = ActionTargetKind::Self;
    fire.FirstAction() = Drink("Resist Fire");
    rs.rules.push_back(fire);
    rs.rules.push_back(HealBelow(0.5f));
    s.health = {40.0f, 100.0f};
    EvalContext ctx;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 0);
    s.now += 0.5;
    REQUIRE(Evaluate(rs, s, ctx).ruleIndex == 1); // the heal is not behind the resist's cooldown
}

TEST_CASE("a dual cast needs the perk the snapshot reports, and pays the dual cost", "[resources]")
{
    constexpr std::uint32_t kBolt = 0x0002DD29;

    RuleSet rs;
    Rule cast;
    cast.subject = SubjectKind::Self;
    cast.predicate = PredicateKind::HealthPctBelow;
    cast.conditionArg = 0.9f;
    cast.actionTarget = ActionTargetKind::Self;
    cast.FirstAction().kind = ActionKind::CastSpell;
    cast.FirstAction().form = kBolt;
    cast.FirstAction().dual = true;
    rs.rules.push_back(cast);

    Snapshot s = Healthy();
    s.health = {50.0f, 100.0f};
    s.magicka = {100.0f, 100.0f};
    s.spells.known.push_back(kBolt);
    // Known and affordable one-handed, but they cannot dual cast it.
    s.spells.costs.push_back({kBolt, 40.0f});

    EvalContext ctx;

    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::CannotDualCast);

    // With the perk, the dual cost is what counts: 112 against 100 is not
    // affordable, 112 against 120 is.
    s.spells.costs[0] = {kBolt, 40.0f, true, 112.0f};
    trace.clear();
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::CannotAfford);

    s.magicka = {120.0f, 120.0f};
    trace.clear();
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.Fired());
    REQUIRE(d.action() == ActionKind::CastSpell);
    REQUIRE(d.step->action.dual);
}

TEST_CASE("a power attack needs a fight, something that swings, and the stamina it costs", "[resources]")
{
    RuleSet rs;
    Rule swing;
    swing.subject = SubjectKind::Enemy;
    swing.predicate = PredicateKind::Any;
    swing.actionTarget = ActionTargetKind::Enemy;
    swing.FirstAction().kind = ActionKind::PowerAttack;
    rs.rules.push_back(swing);

    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f});
    s.enemies[0].reachDistance = 300.0f;
    s.stamina = {30.0f, 100.0f};
    EvalContext ctx;

    // Out of a fight, on the one pass a rule is looked at there: the
    // farewell, through a Combat end rule.
    Trace trace;
    {
        Rule farewell = swing;
        farewell.subject = SubjectKind::Self;
        farewell.predicate = PredicateKind::CombatEnds;
        RuleSet ending;
        ending.rules = {farewell};
        EvalContext fresh;
        Snapshot over = s;
        over.inCombat = false;
        over.combatEnded = true;
        REQUIRE_FALSE(Evaluate(ending, over, fresh, &trace).Fired());
        REQUIRE(trace.at(0) == Verdict::NotInCombat);
    }

    trace.clear();
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoMeleeWeapon); // a bow, a spell, nothing

    s.powerAttack = {true, 40.0f, 200.0f};
    trace.clear();
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoStamina); // 30 against 40

    // Paid for, but the enemy is at 300 against a 200 reach: a swing that
    // lands on nothing is not made.
    s.stamina = {50.0f, 100.0f};
    trace.clear();
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::OutOfReach);

    // The reach is held against the engine's measure, which leaves out both
    // bodies, not the centre-to-centre distance: a large body in reach.
    s.enemies[0].reachDistance = 150.0f;
    trace.clear();
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.Fired());
    REQUIRE(d.action() == ActionKind::PowerAttack);

    // One swing per firing: the next tick is on cooldown.
    s.now += 0.5;
    trace.clear();
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);

    // Done to an enemy, as Attack is: not to oneself.
    REQUIRE(IsActionValidFor(ActionTargetKind::Enemy, ActionKind::PowerAttack));
    REQUIRE(IsActionValidFor(ActionTargetKind::Attacker, ActionKind::PowerAttack));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Self, ActionKind::PowerAttack));
    REQUIRE_FALSE(IsActionValidFor(ActionTargetKind::Player, ActionKind::PowerAttack));
}

TEST_CASE("a bash and a power bash are blows of their own, priced apart", "[resources]")
{
    RuleSet rs;
    Rule bash;
    bash.subject = SubjectKind::Enemy;
    bash.predicate = PredicateKind::Any;
    bash.actionTarget = ActionTargetKind::Enemy;
    bash.FirstAction().kind = ActionKind::Bash;
    rs.rules.push_back(bash);
    Rule powerBash = bash;
    powerBash.FirstAction().kind = ActionKind::PowerBash;
    rs.rules.push_back(powerBash);

    Snapshot s = Healthy();
    s.inCombat = true;
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 100.0f});
    s.stamina = {40.0f, 100.0f};
    // A sword and nothing else: a power attack, no bash.
    s.powerAttack = {true, 25.0f, 180.0f};
    EvalContext ctx;

    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoMeleeWeapon);
    REQUIRE(trace.at(1) == Verdict::NoMeleeWeapon);

    // A shield: the bash is affordable at 35, the power bash at 55 is not.
    s.bash = {true, 35.0f, 180.0f};
    s.powerBash = {true, 55.0f, 180.0f};
    trace.clear();
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.Fired());
    REQUIRE(d.action() == ActionKind::Bash);

    rs.rules.erase(rs.rules.begin());
    trace.clear();
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NoStamina);

    REQUIRE(IsBlow(ActionKind::Bash));
    REQUIRE(IsBlow(ActionKind::PowerBash));
    REQUIRE_FALSE(IsBlow(ActionKind::Attack));
}
