// SKSE entry point. Everything here is scaffolding for Phase 0 -- the exit
// criterion is one line in the log plus one line in the console, proving the
// build/deploy/launch loop works end to end before any real work starts.

#include "core/Evaluator.h"

namespace {

void InitLogging() {
    auto path = SKSE::log::log_directory();
    if (!path) return;
    *path /= "FollowerTactics.log"sv;

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log  = std::make_shared<spdlog::logger>("global", std::move(sink));
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
}

void OnDataLoaded() {
    // Smoke-test the core from inside the game, so a broken deploy is obvious
    // immediately rather than three hours into Phase 1.
    ft::RuleSet rs;
    ft::Rule    r;
    r.condition    = ft::ConditionKind::SelfHealthPctBelow;
    r.conditionArg = 0.5f;
    r.action       = ft::ActionKind::DrinkHealthPotion;
    r.target       = ft::TargetKind::Self;
    rs.rules.push_back(r);

    ft::Snapshot s;
    s.self                = 0x1;
    s.health              = {40.0f, 100.0f};
    s.potions.healthCount = 1;

    ft::EvalContext ctx;
    const auto      d = ft::Evaluate(rs, s, ctx);

    logger::info("core self-check: rule {} fired={}", d.ruleIndex, d.Fired());

    if (auto* console = RE::ConsoleLog::GetSingleton()) {
        console->Print("FollowerTactics loaded (core self-check: %s)",
                       d.Fired() ? "ok" : "FAILED");
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    InitLogging();
    SKSE::Init(skse);

    logger::info("FollowerTactics starting up");

    SKSE::GetMessagingInterface()->RegisterListener(
        [](SKSE::MessagingInterface::Message* message) {
            if (message->type == SKSE::MessagingInterface::kDataLoaded) OnDataLoaded();
        });

    return true;
}
