#include "game/Tactics.h"

#include "core/Evaluator.h"
#include "game/Actions.h"
#include "game/Sensors.h"
#include "game/UI.h"
#include "game/Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

// Last reported follower count, so a change is logged once rather than every
// tick. Without this there is no way to tell a tick that is running and finding
// nobody from a tick that is not running at all -- both are silent, and the
// difference is "your setup script did not apply" versus "the mod is broken".
// -1 so the first report always fires, including the zero case.
int g_lastFollowerCount = -1;

struct FollowerState
{
    ft::EvalContext eval;
    double lastEvaluatedAt{-1.0e9};
    double lastDiagnosticAt{-1.0e9};
};

std::unordered_map<ft::ActorId, FollowerState> g_followers;

// The last evaluation for each follower, kept for the UI.
//
// Written on the game thread by the tick, read on the render thread by the UI,
// so it is guarded. The lock is held only for the copy in or out -- never
// across rendering, and never across BuildSnapshot.
// Only the exceptions are stored, so a follower we have never seen -- or a new
// one -- defaults to enabled without needing an entry.
std::mutex g_disabledMutex;
std::unordered_set<ft::ActorId> g_disabledFollowers;

// Per-follower rules. Absent means "has not been edited", and the default set
// is handed out instead -- so a new follower costs nothing until someone
// actually changes something.
std::mutex g_rulesMutex;
std::unordered_map<ft::ActorId, ft::RuleSet> g_ruleSets;

std::mutex g_viewMutex;
std::vector<FollowerView> g_view;

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

const ft::RuleSet &SpikeRuleSetImpl()
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
std::vector<RE::Actor *> CollectManagedFollowers()
{
    std::vector<RE::Actor *> followers;

    auto *processLists = RE::ProcessLists::GetSingleton();
    if (!processLists)
        return followers;

    // High actors only: the fully simulated ones near the player.
    //
    // Note this does NOT filter on combat. Combat decides whether a follower is
    // EVALUATED, not whether they exist -- an earlier version conflated the two
    // and the panel stayed empty until a fight started, which is exactly when
    // you cannot calmly read it. Rules are authored before the fight.
    for (auto &handle : processLists->highActorHandles)
    {
        auto actor = handle.get();
        RE::Actor *raw = actor ? actor.get() : nullptr;
        if (!raw || raw->IsDead() || !raw->IsPlayerTeammate())
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

ft::Stat ReadStatFor(RE::Actor *actor, RE::ActorValue av)
{
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return {};
    return ft::Stat{owner->GetActorValue(av), owner->GetPermanentActorValue(av)};
}

// Level and carry weight, for the panel. Not rule inputs -- three cheap reads,
// done on both the in-combat and idle paths so the panel does not go blank when
// a fight ends.
void FillDisplayFields(RE::Actor *actor, FollowerView &v)
{
    v.level = actor->GetLevel();
    v.carriedWeight = actor->GetWeightInContainer();
    if (auto *owner = actor->AsActorValueOwner())
        v.carryCapacity = owner->GetActorValue(RE::ActorValue::kCarryWeight);
}

void PublishOne(FollowerView v)
{
    std::scoped_lock lock(g_viewMutex);
    for (auto &existing : g_view)
    {
        if (existing.id == v.id)
        {
            existing = std::move(v);
            return;
        }
    }
    g_view.push_back(std::move(v));
}

// Out of combat: read what is cheap and skip what is not.
//
// Three actor-value reads and a couple of flags -- no inventory scan, no
// evaluation. That keeps "tactics only run in combat" true while still letting
// the panel show who is under control and what shape they are in.
void PublishIdle(RE::Actor *actor, double now, bool inCombat)
{
    ft::Snapshot snapshot;
    snapshot.self = actor->GetFormID();
    snapshot.now = now;
    snapshot.health = ReadStatFor(actor, RE::ActorValue::kHealth);
    snapshot.magicka = ReadStatFor(actor, RE::ActorValue::kMagicka);
    snapshot.stamina = ReadStatFor(actor, RE::ActorValue::kStamina);

    FollowerView v;
    v.id = snapshot.self;
    v.name = DisplayNameOf(actor);
    v.snapshot = snapshot;
    v.lastEvaluatedAt = now;
    v.evaluated = false;
    v.inCombat = inCombat;
    v.tacticsEnabled = IsFollowerEnabled(v.id);
    FillDisplayFields(actor, v);
    PublishOne(std::move(v));
}

void PublishView(RE::Actor *actor, const ft::Snapshot &snapshot, const ft::Trace &trace, const ft::Decision &decision,
                 double now);

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

    // This follower's own rules, not a shared static -- the whole point of
    // making them per-follower.
    const ft::RuleSet rules = GetRules(id);
    ft::Trace trace;
    const ft::Decision decision = ft::Evaluate(rules, snapshot, state.eval, &trace);

    PublishView(actor, snapshot, trace, decision, now);

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

// Replace this follower's entry in the observable view. Called for every
// follower every tick, whether or not a rule fired -- the debug column is most
// useful precisely when nothing is firing.
void PublishView(RE::Actor *actor, const ft::Snapshot &snapshot, const ft::Trace &trace, const ft::Decision &decision,
                 double now)
{
    FollowerView v;
    v.id = actor->GetFormID();
    v.name = DisplayNameOf(actor);
    v.snapshot = snapshot;
    v.trace = trace;
    v.decision = decision;
    v.lastEvaluatedAt = now;
    v.evaluated = true;
    v.inCombat = true;
    v.tacticsEnabled = true;
    FillDisplayFields(actor, v);
    PublishOne(std::move(v));
}

// --- the tick ---------------------------------------------------------------

// Tick-side wrapper: same predicate, plus a line in the log when the answer
// changes.
//
// Logged on change only. This runs every tick, and a gate that stays silent
// when it works is indistinguishable from one that is not running at all --
// which is exactly how the previous version stayed hidden for a whole test
// round. It also tells us WHICH signal caught a given menu, so this can be
// narrowed later on evidence rather than on a guess.
bool EvaluationHeld()
{
    const ClockState clock = ReadClock();

    static int previous = -1;
    const int state = (clock.pausedMenu ? 1 : 0) | (clock.frozenClock ? 2 : 0);
    if (state != previous)
    {
        previous = state;
        if (state == 0)
            logger::info("tactics: time is running -- evaluating");
        else
            logger::info("tactics: time stopped ({}{}{}) -- evaluation held", clock.pausedMenu ? "paused menu" : "",
                         (clock.pausedMenu && clock.frozenClock) ? " + " : "", clock.frozenClock ? "frozen clock" : "");
    }
    return clock.stopped();
}

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

    if (EvaluationHeld())
        return;

    const double now = NowSeconds();
    if ((now - g_lastTick) < kTickInterval)
        return;
    g_lastTick = now;

    // No player means main menu or a load screen. Walking the process lists
    // then is pointless at best.
    if (!RE::PlayerCharacter::GetSingleton())
        return;

    const auto followers = CollectManagedFollowers();

    if (static_cast<int>(followers.size()) != g_lastFollowerCount)
    {
        g_lastFollowerCount = static_cast<int>(followers.size());
        if (followers.empty())
        {
            logger::info("tactics: 0 followers. Nobody nearby has the player-teammate flag -- "
                         "if you just ran a setup script, prid probably selected nothing.");
        }
        else
        {
            std::string names;
            for (auto *f : followers)
            {
                if (!names.empty())
                    names += ", ";
                names += Describe(f);
            }
            logger::info("tactics: {} follower(s) under control: {}", followers.size(), names);
        }
    }

    // Out of combat there is nothing to decide, so the expensive work -- the
    // inventory scan inside BuildSnapshot, and the evaluation itself -- is
    // skipped entirely. What remains is a few actor-value reads, so the panel
    // is not blank while you are standing there authoring rules.
    for (auto *follower : followers)
    {
        const bool fighting = follower->IsInCombat();

        // Both switches must be on. A follower turned off still appears in the
        // panel, and still reports whether they are fighting -- they are simply
        // not evaluated, which is what the empty Status column then says.
        if (fighting && IsFollowerEnabled(follower->GetFormID()))
            EvaluateFollower(follower, now);
        else
            PublishIdle(follower, now, fighting);
    }

    // Menu entries are added lazily, because followers appear long after
    // Install() has run. Cheap: it only acts on a follower it has not seen.
    ui::RegisterNewFollowers();

    // Drop anyone who is no longer a managed follower -- dismissed, dead, or out
    // of range -- so the panel reflects the present rather than a history.
    {
        std::scoped_lock lock(g_viewMutex);
        std::erase_if(g_view, [&](const FollowerView &v) {
            for (auto *f : followers)
            {
                if (f->GetFormID() == v.id)
                    return false;
            }
            return true;
        });
    }

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

// Two signals, because neither alone is enough.
//
//   numPausesGame    what UI::GameIsPaused() returns, and it is nothing more
//                    than a count of registered menus carrying kPausesGame.
//                    It covers the inventory, map, journal, settings and the
//                    console. It CANNOT see our own panel: SKSE Menu Framework
//                    draws from a D3D present hook and never registers an
//                    IMenu, so it never moves that counter no matter what
//                    FreezeTimeOnMenu says. An earlier gate checked only this
//                    and let the panel straight through -- for a whole test
//                    round, because it also went unlogged.
//
//   Main::freezeTime the clock itself, which is what the framework sets when
//                    FreezeTimeOnMenu = true.
//
// Whether a pausing menu ALSO sets freezeTime is not established, so the two
// are OR-ed rather than one being assumed to imply the other. Both are a
// pointer dereference; there is nothing to win by guessing.
//
// Asking about the clock rather than about panel-is-open also gets
// FreezeTimeOnMenu = false right for free: with the freeze off, time keeps
// running and so do rules, so the panel shows live state rather than a still
// frame. That is the whole point of that setting.
ClockState ReadClock()
{
    auto *ui = RE::UI::GetSingleton();
    auto *main = RE::Main::GetSingleton();
    return ClockState{ui && ui->GameIsPaused(), main && main->freezeTime};
}

ft::RuleSet GetRules(ft::ActorId id)
{
    std::scoped_lock lock(g_rulesMutex);
    const auto it = g_ruleSets.find(id);
    return it == g_ruleSets.end() ? DefaultRuleSet() : it->second;
}

void SetRules(ft::ActorId id, ft::RuleSet rules)
{
    std::scoped_lock lock(g_rulesMutex);
    g_ruleSets[id] = std::move(rules);
}

const ft::RuleSet &DefaultRuleSet()
{
    return SpikeRuleSetImpl();
}

std::vector<FollowerView> ObserveFollowers()
{
    std::scoped_lock lock(g_viewMutex);
    return g_view;
}

CostStats ObserveCost()
{
    return CostStats{g_cost.AvgUs(), g_cost.maxUs, g_cost.samples};
}

const ft::RuleSet &ActiveRuleSet()
{
    return DefaultRuleSet();
}

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

void SetFollowerEnabled(ft::ActorId id, bool enabled)
{
    std::scoped_lock lock(g_disabledMutex);
    if (enabled)
        g_disabledFollowers.erase(id);
    else
        g_disabledFollowers.insert(id);
}

bool IsFollowerEnabled(ft::ActorId id)
{
    std::scoped_lock lock(g_disabledMutex);
    return !g_disabledFollowers.contains(id);
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
