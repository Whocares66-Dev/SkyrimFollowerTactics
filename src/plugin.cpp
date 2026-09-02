// SKSE entry point. Everything here is still Phase 0 scaffolding.
//
// IMPORTANT, so nobody is misled by a green line in the console: this file does
// not touch a follower. It evaluates the rule engine against *fabricated*
// snapshots to prove the engine behaves inside Skyrim exactly as it does under
// Catch2. Reading a real actor is Phase 1, and src/game/ does not exist yet.

#include "core/Evaluator.h"
#include "game/Tactics.h"

#include <string>
#include <vector>

namespace
{

void InitLogging()
{
    auto path = SKSE::log::log_directory();
    if (!path)
        return;
    *path /= "FollowerTactics.log"sv;

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
}

struct Check
{
    const char *name;
    bool passed;
    std::string detail;
};

// A follower at full health with potions in the bag. Scenarios below damage it
// or add actors as needed. Mirrors Healthy() in tests/test_evaluator.cpp on
// purpose -- the whole point is that both sides agree.
ft::Snapshot BaseSnapshot()
{
    ft::Snapshot s;
    s.self = 0xA2C94; // Lydia, for familiarity in the log
    s.now = 100.0;
    s.health = {100.0f, 100.0f};
    s.magicka = {100.0f, 100.0f};
    s.stamina = {100.0f, 100.0f};
    s.inCombat = true;
    s.playerHealth = {100.0f, 100.0f};
    s.potions.healthCount = 5;
    return s;
}

ft::Rule HealBelow(float pct)
{
    ft::Rule r;
    r.subject = ft::SubjectKind::Self;
    r.predicate = ft::PredicateKind::HealthPctBelow;
    r.conditionArg = pct;
    r.actionTarget = ft::ActionTargetKind::ConditionSubject;
    r.action = ft::ActionKind::DrinkHealthPotion;
    r.label = "heal";
    return r;
}

std::vector<Check> RunSelfCheck()
{
    std::vector<Check> checks;

    // 1. The marquee rule fires when it should, and binds to the follower.
    {
        ft::RuleSet rs;
        rs.rules.push_back(HealBelow(0.5f));
        auto s = BaseSnapshot();
        s.health = {40.0f, 100.0f};

        ft::EvalContext ctx;
        const auto d = ft::Evaluate(rs, s, ctx);

        checks.push_back({"health 40% < 50% -> drink potion",
                          d.Fired() && d.action == ft::ActionKind::DrinkHealthPotion && d.targetId == s.self,
                          fmt::format("fired={} target={:08X}", d.Fired(), d.targetId)});
    }

    // 2. ...and stays quiet when it should not. An empty result must be as
    //    reliable as a firing one, or the mod is worse than vanilla.
    {
        ft::RuleSet rs;
        rs.rules.push_back(HealBelow(0.5f));

        ft::EvalContext ctx;
        ft::Trace trace;
        const auto d = ft::Evaluate(rs, BaseSnapshot(), ctx, &trace);

        checks.push_back({"health 100% -> no rule fires", !d.Fired() && trace.at(0) == ft::Verdict::ConditionFalse,
                          fmt::format("verdict={}", ft::ToString(trace.at(0)))});
    }

    // 3. Group binding: two enemies qualify, and a health predicate must bind
    //    the weakest rather than the nearest.
    {
        auto s = BaseSnapshot();
        s.enemies.push_back({0x101, {50.0f, 100.0f}, 300.0f, false, false, true});
        s.enemies.push_back({0x102, {10.0f, 100.0f}, 900.0f, false, false, true});

        ft::RuleSet rs;
        ft::Rule r;
        r.subject = ft::SubjectKind::Enemy;
        r.predicate = ft::PredicateKind::HealthPctBelow;
        r.conditionArg = 0.6f;
        r.actionTarget = ft::ActionTargetKind::ConditionSubject;
        r.action = ft::ActionKind::StopCombat;
        rs.rules.push_back(r);

        ft::EvalContext ctx;
        const auto d = ft::Evaluate(rs, s, ctx);

        checks.push_back({"enemy < 60% health -> binds the weakest (0x102, not 0x101)",
                          d.Fired() && d.targetId == 0x102, fmt::format("target={:08X}", d.targetId)});
    }

    // 4. The action can be aimed away from the condition's subject.
    {
        ft::RuleSet rs;
        auto r = HealBelow(0.5f);
        r.actionTarget = ft::ActionTargetKind::Player;
        rs.rules.push_back(r);

        auto s = BaseSnapshot();
        s.health = {40.0f, 100.0f};

        ft::EvalContext ctx;
        const auto d = ft::Evaluate(rs, s, ctx);

        checks.push_back({"action target override -> player", d.Fired() && d.targetId == ft::kPlayerFormID,
                          fmt::format("target={:08X}", d.targetId)});
    }

    // 5. An unanswerable subject/predicate pair is reported as broken authoring,
    //    not as a condition that happens to be false.
    {
        ft::RuleSet rs;
        ft::Rule r;
        r.subject = ft::SubjectKind::Self;
        r.predicate = ft::PredicateKind::WithinDistance; // nonsense
        r.conditionArg = 100.0f;
        r.action = ft::ActionKind::StopCombat;
        rs.rules.push_back(r);

        ft::EvalContext ctx;
        ft::Trace trace;
        const auto d = ft::Evaluate(rs, BaseSnapshot(), ctx, &trace);

        checks.push_back({"Self + WithinDistance -> invalid condition",
                          !d.Fired() && trace.at(0) == ft::Verdict::InvalidCondition,
                          fmt::format("verdict={}", ft::ToString(trace.at(0)))});
    }

    return checks;
}

void OnDataLoaded()
{
    const auto checks = RunSelfCheck();

    std::size_t passed = 0;
    logger::info("---- core self-check ({} scenarios, fabricated snapshots) ----", checks.size());
    for (const auto &c : checks)
    {
        logger::info("  [{}] {}  ({})", c.passed ? "PASS" : "FAIL", c.name, c.detail);
        if (c.passed)
            ++passed;
    }
    logger::info("---- core self-check: {}/{} passed ----", passed, checks.size());

    // Phase 1: start the real thing. The self-check above proves the engine
    // computes correct decisions; this is what connects it to actual followers.
    ft::game::Install();

    const bool allPassed = passed == checks.size();
    if (auto *console = RE::ConsoleLog::GetSingleton())
    {
        console->Print("FollowerTactics loaded (self-check %zu/%zu %s) - tactics ACTIVE", passed, checks.size(),
                       allPassed ? "ok" : "FAILED - see FollowerTactics.log");
    }
}

} // namespace

SKSEPluginLoad(const SKSE::LoadInterface *skse)
{
    InitLogging();
    SKSE::Init(skse);

    logger::info("FollowerTactics starting up");

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message *message) {
        if (message->type == SKSE::MessagingInterface::kDataLoaded)
            OnDataLoaded();
    });

    return true;
}
