// These tests run with no Skyrim, no SKSE, and no CommonLibSSE.
// That is the whole point -- see docs/PLAN.md section 3.

#include <catch2/catch_test_macros.hpp>

#include "core/Evaluator.h"

using namespace ft;

namespace {

Snapshot Healthy() {
    Snapshot s;
    s.self    = 0xA2C94;
    s.now     = 100.0;
    s.health  = {100.0f, 100.0f};
    s.magicka = {100.0f, 100.0f};
    s.stamina = {100.0f, 100.0f};
    s.inCombat            = true;
    s.playerHealth        = {100.0f, 100.0f};
    s.potions.healthCount = 5;
    return s;
}

Rule HealBelow(float pct, double cooldown = 0.0) {
    Rule r;
    r.condition    = ConditionKind::SelfHealthPctBelow;
    r.conditionArg = pct;
    r.target       = TargetKind::Self;
    r.action       = ActionKind::DrinkHealthPotion;
    r.cooldown     = cooldown;
    r.label        = "heal";
    return r;
}

}  // namespace

TEST_CASE("an empty rule set does nothing", "[evaluator]") {
    RuleSet     rs;
    EvalContext ctx;
    const auto  d = Evaluate(rs, Healthy(), ctx);
    REQUIRE_FALSE(d.Fired());
}

TEST_CASE("the marquee rule: health below 50% drinks a potion", "[evaluator]") {
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));
    EvalContext ctx;

    SECTION("does not fire at full health") {
        Trace      trace;
        const auto d = Evaluate(rs, Healthy(), ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::ConditionFalse);
    }

    SECTION("fires below the threshold") {
        Snapshot s = Healthy();
        s.health   = {40.0f, 100.0f};

        Trace      trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE(d.Fired());
        REQUIRE(d.action == ActionKind::DrinkHealthPotion);
        REQUIRE(d.targetId == s.self);
        REQUIRE(trace.at(0) == Verdict::Fired);
    }

    SECTION("will not fire without a potion in the inventory") {
        Snapshot s            = Healthy();
        s.health              = {40.0f, 100.0f};
        s.potions.healthCount = 0;

        Trace      trace;
        const auto d = Evaluate(rs, s, ctx, &trace);
        REQUIRE_FALSE(d.Fired());
        REQUIRE(trace.at(0) == Verdict::NoResource);
    }
}

TEST_CASE("order decides: the first matching rule wins", "[evaluator]") {
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.9f));  // broad, listed first
    rs.rules.push_back(HealBelow(0.5f));  // narrow, shadowed

    Snapshot s = Healthy();
    s.health   = {40.0f, 100.0f};

    EvalContext ctx;
    Trace       trace;
    const auto  d = Evaluate(rs, s, ctx, &trace);

    REQUIRE(d.ruleIndex == 0);
    REQUIRE(trace.at(1) == Verdict::NotReached);
}

TEST_CASE("a disabled rule is skipped and the next one gets a turn", "[evaluator]") {
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.9f));
    rs.rules.back().enabled = false;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health   = {40.0f, 100.0f};

    EvalContext ctx;
    Trace       trace;
    const auto  d = Evaluate(rs, s, ctx, &trace);

    REQUIRE(d.ruleIndex == 1);
    REQUIRE(trace.at(0) == Verdict::Disabled);
}

TEST_CASE("per-rule cooldown suppresses a re-fire", "[evaluator]") {
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f, /*cooldown=*/10.0));

    Snapshot s = Healthy();
    s.health   = {40.0f, 100.0f};

    EvalContext ctx;
    ctx.globalCooldown = 0.0;

    REQUIRE(Evaluate(rs, s, ctx).Fired());

    s.now = 105.0;  // 5s later, inside the 10s cooldown
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::OnCooldown);

    s.now = 111.0;  // past it
    REQUIRE(Evaluate(rs, s, ctx).Fired());
}

TEST_CASE("the global cooldown rate-limits across different rules", "[evaluator]") {
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health   = {40.0f, 100.0f};

    EvalContext ctx;
    ctx.globalCooldown = 0.5;

    REQUIRE(Evaluate(rs, s, ctx).Fired());

    s.now = 100.1;  // still inside the global cooldown
    Trace trace;
    REQUIRE_FALSE(Evaluate(rs, s, ctx, &trace).Fired());

    // Reported as rate-limited, not as a false condition -- the debug column
    // has to point at the real reason or it sends you debugging the wrong thing.
    REQUIRE(trace.at(0) == Verdict::GlobalCooldown);
}

TEST_CASE("an unsupported action never fires", "[evaluator]") {
    RuleSet rs;
    rs.rules.push_back(HealBelow(0.5f));

    Snapshot s = Healthy();
    s.health   = {40.0f, 100.0f};

    EvalContext ctx;
    ctx.caps.supported.fill(false);

    Trace      trace;
    const auto d = Evaluate(rs, s, ctx, &trace);
    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::Unsupported);
}

TEST_CASE("target resolution", "[targets]") {
    Snapshot s = Healthy();
    s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f, false, false, true});
    s.enemies.push_back({0x102, {10.0f, 100.0f}, 900.0f, false, false, true});
    s.allies.push_back({0x201, {20.0f, 100.0f}, 100.0f, false});

    bool ok = false;
    REQUIRE(ResolveTarget(TargetKind::NearestEnemy, s, &ok) == 0x101);
    REQUIRE(ok);
    REQUIRE(ResolveTarget(TargetKind::LowestHealthEnemy, s, &ok) == 0x102);
    REQUIRE(ResolveTarget(TargetKind::LowestHealthAlly, s, &ok) == 0x201);
    REQUIRE(ResolveTarget(TargetKind::Player, s, &ok) == 0x14);

    SECTION("a target that does not exist is reported, not faked") {
        Snapshot empty = Healthy();
        ResolveTarget(TargetKind::NearestEnemy, empty, &ok);
        REQUIRE_FALSE(ok);
    }
}

TEST_CASE("no target means the rule is skipped, not fired at nobody", "[evaluator]") {
    RuleSet rs;
    Rule    r;
    r.condition = ConditionKind::Always;
    r.target    = TargetKind::NearestEnemy;
    r.action    = ActionKind::StopCombat;
    rs.rules.push_back(r);

    EvalContext ctx;
    Trace       trace;
    const auto  d = Evaluate(rs, Healthy(), ctx, &trace);  // no enemies

    REQUIRE_FALSE(d.Fired());
    REQUIRE(trace.at(0) == Verdict::NoTarget);
}

TEST_CASE("Stat::Pct does not divide by zero", "[snapshot]") {
    REQUIRE(Stat{}.Pct() == 0.0f);
    REQUIRE(Stat{50.0f, 200.0f}.Pct() == 0.25f);
}
