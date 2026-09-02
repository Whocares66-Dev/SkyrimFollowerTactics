#include "game/Tactics.h"

#include "core/Evaluator.h"
#include "game/Actions.h"
#include "game/Sensors.h"
#include "game/Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace ft::game
{
namespace
{

// --- tuning ----------------------------------------------------------------

// docs/PLAN.md 3.2 wants 150 ms.
constexpr double kTickInterval = 0.15;

// How often a follower may report *why* it did not act. Without this the log is
// seven lines per second per follower and unreadable; with it, the answer to
// "why didn't she drink" is always in the last few seconds of the file.
constexpr double kDiagnosticInterval = 2.0;

// A gap this long since we last evaluated a follower means the previous fight
// ended and this is a new one, so per-rule cooldowns are reset. A cooldown
// exists to stop a rule thrashing *within* a fight; carrying it into the next
// fight would silently suppress that fight's first heal.
constexpr double kNewFightGap = 5.0;

// How often to report measured tick cost.
constexpr double kCostReportInterval = 5.0;

constexpr std::size_t kMaxManagedFollowers = 8;

// --- state -----------------------------------------------------------------

std::atomic_bool g_enabled{true};
std::atomic_bool g_installed{false};

double g_lastTick = -1.0e9;

struct FollowerState
{
    ft::EvalContext eval;
    double lastEvaluatedAt{-1.0e9};
    double lastDiagnosticAt{-1.0e9};
};

std::unordered_map<ft::ActorId, FollowerState> g_followers;

// Per-evaluation cost, in microseconds. docs/PLAN.md 3.2 sets a budget -- total
// tick cost across 8 followers under 0.5 ms/frame amortised -- and insists it be
// measured rather than assumed. This is that measurement. It is also how we will
// know whether the inventory scan needs caching, before building a cache for it.
struct TickCost
{
    double totalUs{0.0};
    double maxUs{0.0};
    std::uint64_t samples{0};

    void Add(double us)
    {
        totalUs += us;
        maxUs = std::max(maxUs, us);
        ++samples;
    }
    [[nodiscard]] double AvgUs() const
    {
        return samples ? totalUs / static_cast<double>(samples) : 0.0;
    }
    void Reset()
    {
        totalUs = 0.0;
        maxUs = 0.0;
        samples = 0;
    }
};

TickCost g_cost;
double g_lastCostReport = -1.0e9;

// --- the hardcoded Phase 1 rule --------------------------------------------

const ft::RuleSet &SpikeRuleSet()
{
    static const ft::RuleSet rules = [] {
        ft::RuleSet rs;
        rs.name = "phase1-spike";

        ft::Rule heal;
        heal.label = "emergency heal";
        heal.subject = ft::SubjectKind::Self;
        heal.predicate = ft::PredicateKind::HealthPctBelow;
        heal.conditionArg = 0.5f;
        heal.actionTarget = ft::ActionTargetKind::ConditionSubject;
        heal.action = ft::ActionKind::DrinkHealthPotion;
        heal.cooldown = 10.0;
        rs.rules.push_back(heal);

        return rs;
    }();
    return rules;
}

// Only the actions Phase 1 actually implements are advertised as supported. The
// engine then reports Verdict::Unsupported for anything else instead of firing
// a rule that Actions::Execute would silently drop.
ft::Capabilities SpikeCapabilities()
{
    ft::Capabilities caps; // all false
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkHealthPotion)] = true;
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkMagickaPotion)] = true;
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkStaminaPotion)] = true;
    return caps;
}

// --- registry ---------------------------------------------------------------

// Who is under tactics control, right now.
//
// IsPlayerTeammate() reads the kPlayerTeammate bool bit (1 << 26), which every
// follower framework -- NFF, AFT, EFF -- sets, so this integrates with all of
// them for free and depends on none.
//
// IsInCombat() is the combat gate, and it is deliberately the ONLY one: tactics
// are a combat system, so out of combat this loop finds nobody and no snapshot
// is ever built.
//
// An earlier version cached combat state from TESCombatEvent to skip this loop.
// That was a mistake worth recording: the loop is a handle deref and two flag
// reads per nearby actor, while the cache needed a mutex and a reconciliation
// pass to survive missed events -- loading a save mid-fight fires no combat
// event, so the cache would say "nobody is fighting" forever. A lot of
// machinery, and a correctness hazard, to avoid a few microseconds. The
// expensive part of a tick is BuildSnapshot's inventory scan, and this gates
// that already.
std::vector<RE::Actor *> CollectFightingFollowers()
{
    std::vector<RE::Actor *> followers;

    auto *processLists = RE::ProcessLists::GetSingleton();
    if (!processLists)
        return followers;

    // High actors only: the fully simulated ones near the player. A follower
    // who is not high-process is not fighting anything.
    for (auto &handle : processLists->highActorHandles)
    {
        auto actor = handle.get();
        RE::Actor *raw = actor ? actor.get() : nullptr;
        if (!raw || raw->IsDead() || !raw->IsPlayerTeammate() || !raw->IsInCombat())
            continue;

        followers.push_back(raw);
        if (followers.size() >= kMaxManagedFollowers)
            break;
    }

    return followers;
}

// --- per-follower evaluation -------------------------------------------------

void LogDiagnostic(RE::Actor *actor, const ft::Snapshot &snap, const ft::RuleSet &rules, const ft::Trace &trace)
{
    logger::info("{} health {:.0f}/{:.0f} ({:.0f}%) combat={} potions={}", Describe(actor), snap.health.current,
                 snap.health.max, snap.health.Pct() * 100.0, snap.inCombat, snap.potions.healthCount);

    for (std::size_t i = 0; i < trace.size(); ++i)
    {
        const auto &rule = rules.rules[i];
        logger::info("    rule {} \"{}\": {}", i, rule.label, ft::ToString(trace[i]));
    }
}

void EvaluateFollower(RE::Actor *actor, double now)
{
    const ft::ActorId id = actor->GetFormID();
    auto &state = g_followers[id];

    // A long gap since the last evaluation means a different fight.
    if ((now - state.lastEvaluatedAt) > kNewFightGap)
    {
        state.eval = {};
        logger::info("{} entered combat -- tactics engaged", Describe(actor));
    }
    state.lastEvaluatedAt = now;
    state.eval.caps = SpikeCapabilities();

    const auto started = std::chrono::steady_clock::now();

    PotionChoice choice;
    const ft::Snapshot snapshot = BuildSnapshot(actor, now, choice);

    const auto &rules = SpikeRuleSet();
    ft::Trace trace;
    const ft::Decision decision = ft::Evaluate(rules, snapshot, state.eval, &trace);

    g_cost.Add(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());

    if (decision.Fired())
    {
        const auto result = Execute(decision, actor, choice);

        logger::info("{} FIRED rule {} \"{}\" -> {} [health {:.0f}/{:.0f} = {:.0f}%]", Describe(actor),
                     decision.ruleIndex, rules.rules[decision.ruleIndex].label, ToString(result),
                     snapshot.health.current, snapshot.health.max, snapshot.health.Pct() * 100.0);

        // Empirical check for whether a drunk potion leaves a lingering effect
        // we could test against, rather than relying on a fixed settle time.
        LogActiveEffects(actor, "just after firing");

        if (result != ActionResult::Performed)
        {
            // A rule that fires but does not take effect is the failure worth
            // shouting about: the engine believed it acted, and it did not.
            logger::warn("{} action did NOT take effect: {}", Describe(actor), ToString(result));
        }
        return;
    }

    if ((now - state.lastDiagnosticAt) >= kDiagnosticInterval)
    {
        state.lastDiagnosticAt = now;
        LogDiagnostic(actor, snapshot, rules, trace);
    }
}

// --- the tick ---------------------------------------------------------------

// Pacing lives on a separate thread; the work itself runs on the game thread via
// the task interface.
//
// It MUST be this way round. SKSE processes its task queue like this
// (skse64/Hooks_Threads.cpp):
//
//     void BSTaskPool::ProcessTasks() {
//         ...
//         while (!IsTaskQueueEmpty()) { cmd->Run(); cmd->Dispose(); }
//     }
//
// It drains until the queue is EMPTY. So a task that calls AddTask from inside
// its own Run() refills the queue faster than it drains, ProcessTasks never
// returns, and the main thread spins forever -- the game hangs on the first
// frame. A self-re-arming task is not a periodic scheduler in SKSE; it is a
// deadlock. This cost one hung startup to learn.
void Tick()
{
    if (!g_enabled.load())
        return;

    const double now = NowSeconds();
    if ((now - g_lastTick) < kTickInterval)
        return;
    g_lastTick = now;

    // No player means main menu or a load screen. Walking the process lists
    // then is pointless at best.
    if (!RE::PlayerCharacter::GetSingleton())
        return;

    for (auto *follower : CollectFightingFollowers())
        EvaluateFollower(follower, now);

    if (g_cost.samples > 0 && (now - g_lastCostReport) >= kCostReportInterval)
    {
        g_lastCostReport = now;
        logger::info("tactics: {} evaluations, avg {:.0f} us, max {:.0f} us  (budget: under "
                     "500 us/frame across all followers)",
                     g_cost.samples, g_cost.AvgUs(), g_cost.maxUs);
        g_cost.Reset();
    }
}

} // namespace

void Install()
{
    if (g_installed.exchange(true))
        return;

    logger::info("tactics: tick {:.0f} ms, combat only, max {} followers", kTickInterval * 1000.0,
                 kMaxManagedFollowers);
    logger::info("tactics: Phase 1 rule set is hardcoded -- self health below 50% -> drink the "
                 "best health potion, at most once per 10 s");

    // Detached on purpose: Skyrim never unloads SKSE plugins, and joining a
    // sleeping thread during process teardown is a good way to hang on exit.
    std::thread([] {
        while (g_installed.load())
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(kTickInterval));
            if (auto *task = SKSE::GetTaskInterface())
                task->AddTask([] { Tick(); });
        }
    }).detach();
}

void SetEnabled(bool enabled)
{
    g_enabled.store(enabled);
    logger::info("tactics: {}", enabled ? "enabled" : "disabled");
}

bool IsEnabled()
{
    return g_enabled.load();
}

} // namespace ft::game
