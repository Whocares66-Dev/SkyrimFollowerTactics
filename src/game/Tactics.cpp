#include "game/Tactics.h"

#include "core/Evaluator.h"
#include "core/Vocabulary.h"
#include "game/Actions.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Profiles.h"
#include "game/Sensors.h"
#include "game/Sheet.h"
#include "game/UI.h"
#include "game/Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft::game
{
namespace
{

// --- tuning ----------------------------------------------------------------

// The TURN. Every half second the list is walked and at most one rule
// fires. There is no separate "global cooldown": the turn is the spacing
// between decisions, and the only other timers are per action. (docs/PLAN.md
// 3.2 asked for 150 ms; a decision a half-second is the cadence a fight
// reads well at, and the cost is far under budget either way.)
constexpr double kTickInterval = 0.5;

// How often a follower may report *why* it did not act. Without this the log
// is a line per rule per tick per follower and unreadable; with it, the
// answer to "why didn't they drink" is always in the last few seconds of the
// file.
constexpr double kDiagnosticInterval = 2.0;

// How often to report measured tick cost.
constexpr double kCostReportInterval = 5.0;

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
    double lastDiagnosticAt{-1.0e9};
    // In combat on the last tick, for the edges: the first evaluation of a
    // fight, and the one farewell evaluation after it.
    bool fighting{false};
};

std::unordered_map<ft::ActorId, FollowerState> g_followers;

// The followers switched OFF. A follower starts on: with the default rule
// set empty, on is safe -- an empty list does nothing -- and the switch is
// for silencing a written list without losing it. Only the exceptions are
// stored, so a follower we have never seen -- or a new one -- defaults to
// enabled without needing an entry. Written by the panel and by a load,
// read by the tick: guarded.
std::mutex g_disabledMutex;
std::unordered_set<ft::ActorId> g_disabledFollowers;

// Followers currently in bleedout, so the transition is logged once rather
// than every tick.
std::unordered_set<ft::ActorId> g_bleedingOut;

// Per-follower rules. Absent means "has not been edited", and the default set
// is handed out instead -- so a new follower costs nothing until someone
// actually changes something.
std::mutex g_rulesMutex;
std::unordered_map<ft::ActorId, ft::RuleSet> g_ruleSets;

// How each follower is filed, learned the first time the tick sees them
// and kept for the session: a dismissed follower's rules are still theirs
// at the next save. Game thread only.
std::unordered_map<ft::ActorId, Identity> g_identities;

// First sight of a follower: take their record from the loaded save, if
// it holds one. Before any view of them is published, so what the panel
// shows is what was saved.
void LoadIfNew(RE::Actor *follower)
{
    const ft::ActorId id = follower->GetFormID();
    if (g_identities.contains(id))
        return;
    const Identity &who = g_identities.emplace(id, IdentifyFollower(follower)).first->second;
    auto profile = ClaimSaved(who);
    if (!profile)
        return;
    {
        std::scoped_lock lock(g_rulesMutex);
        g_ruleSets[id] = std::move(profile->rules);
    }
    {
        std::scoped_lock lock(g_disabledMutex);
        if (profile->enabled)
            g_disabledFollowers.erase(id);
        else
            g_disabledFollowers.insert(id);
    }
    AdoptPins(follower, profile->pins);
    AdoptBans(follower, profile->bans);
}

} // namespace

std::vector<Filed> ProfilesToSave()
{
    std::vector<Filed> filed;
    for (const auto &[id, who] : g_identities)
    {
        Filed f;
        f.who = who;
        f.profile.followerName = who.name;
        f.profile.followerForm = who.form;
        f.profile.enabled = IsFollowerEnabled(id);
        f.profile.rules = GetRules(id);
        f.profile.pins = PlayerPinsOf(id);
        f.profile.bans = BansOf(id);
        filed.push_back(std::move(f));
    }
    return filed;
}

namespace
{

// The last evaluation for each follower, kept for the UI.
//
// Written on the game thread by the tick, read on the render thread by the UI,
// so it is guarded. The lock is held only for the copy in or out -- never
// across rendering, and never across BuildSnapshot.
// The last evaluation for each follower, kept for the UI.
//
// Written on the game thread by the tick, read on the render thread by the UI,
// so it is guarded. The lock is held only for the copy in or out -- never
// across rendering, and never across BuildSnapshot.
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

// What a follower starts with: nothing. Installing the mod must not change
// how anyone's followers fight until the player has written a rule: the
// Phase 1 "emergency heal" rule that used to sit here surprised a fresh
// install with a follower drinking potions on their own initiative.
const ft::RuleSet kNoRules;

// What the runtime can do this tick. The casts need the package pool
// (made in memory at load, game/Forms.h); without it they report
// Unsupported, a truthful "not available here" rather than a rule that
// silently never fires.
ft::Capabilities RuntimeCapabilities(const RE::Actor *actor)
{
    ft::Capabilities caps;
    caps.castingAvailable = PackagesAvailable();

    // Transient, unlike the line above: every slot mid-cast means a cast rule
    // is skipped for THIS evaluation only, with no cooldown spent, and the
    // next rule down gets its turn.
    caps.busy[static_cast<std::size_t>(ft::ActionKind::CastSpell)] = PackagesAvailable() && !HasFreeSlot();
    caps.busy[static_cast<std::size_t>(ft::ActionKind::UseScroll)] =
        caps.busy[static_cast<std::size_t>(ft::ActionKind::CastSpell)];
    caps.busy[static_cast<std::size_t>(ft::ActionKind::UsePower)] = PackagesAvailable() && !HasFreeVoiceSlot();
    caps.busy[static_cast<std::size_t>(ft::ActionKind::Shout)] =
        caps.busy[static_cast<std::size_t>(ft::ActionKind::UsePower)];

    // A cast of OURS still in the air -- the lease is held from the request
    // until the follower's own spell-fire event names the spell -- makes
    // every action wait, not just another cast: a pin into the casting hand
    // or a potion would cut it off. "Combat start: cast Stoneflesh, equip
    // Flames" put Flames in the hand half a second into the cast. A list in
    // progress waits on the next tick; a fresh rule yields for this one.
    // Ours only: the AI's own casting, a pinned Flames streaming all fight,
    // holds nothing up.
    if (IsMidCast(actor))
        caps.busy.fill(true);
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

// A rule's actions by wire name, "+"-joined, for the log.
std::string ActionNames(const ft::Rule &rule)
{
    std::string names;
    for (const auto &action : rule.actions)
        names += (names.empty() ? "" : "+") + std::string(ft::WireName(action.kind));
    return names.empty() ? "none" : names;
}

ft::ActionKind FirstKind(const ft::Rule &rule)
{
    return rule.actions.empty() ? ft::ActionKind::None : rule.actions.front().kind;
}

void LogDiagnostic(RE::Actor *actor, const ft::Snapshot &snap, const ft::RuleSet &rules, const ft::Trace &trace)
{
    if (!log::Enabled(log::Level::Debug))
        return;

    log::tactics.debug("{} health {:.0f}/{:.0f} ({:.0f}%) combat={} consumables={}", Describe(actor),
                       snap.health.current, snap.health.max, snap.health.Pct() * 100.0, snap.inCombat,
                       snap.potions.carried.size());

    for (std::size_t i = 0; i < trace.size(); ++i)
    {
        const auto &rule = rules.rules[i];
        log::tactics.debug("  rule {} \"{}\" [{}]: {}", i, rule.label, ActionNames(rule),
                           ft::Explain(trace[i], FirstKind(rule)));
    }
}

// Every sheet tab's content, read off the actor. Not rule inputs, so done on
// both the in-combat and idle paths: the panel does not go blank when a fight
// ends.
void FillCharacterView(RE::Actor *actor, CharacterView &v)
{
    v.id = actor->GetFormID();
    v.name = DisplayNameOf(actor);
    v.level = actor->GetLevel();
    v.health = ReadStat(actor, RE::ActorValue::kHealth);
    v.stamina = ReadStat(actor, RE::ActorValue::kStamina);
    v.magicka = ReadStat(actor, RE::ActorValue::kMagicka);
    v.carriedWeight = actor->GetWeightInContainer();
    v.healthBreakdown = ValueBreakdown(actor, RE::ActorValue::kHealth, "");
    v.staminaBreakdown = ValueBreakdown(actor, RE::ActorValue::kStamina, "");
    v.magickaBreakdown = ValueBreakdown(actor, RE::ActorValue::kMagicka, "");
    v.carryCapacity = actor->GetTotalCarryWeight();
    v.carryBreakdown = CarryWeightBreakdown(actor);
    v.sheet = BuildCharacterSheet(actor);
    v.skills = BuildSkillSheet(actor);
    v.perks = BuildPerkPages(actor);
    v.summons = ScanSummons(actor);
    v.inventory = ScanInventory(actor);
    v.magic = ScanMagic(actor);
    v.effects = ScanActiveEffects(actor);
}

// The sheet, and what the rule editor and its menus read beside it.
void FillDisplayFields(RE::Actor *actor, FollowerView &v)
{
    FillCharacterView(actor, v);

    // Scanned on the idle path too, so the spell menu is populated while rules
    // are being written -- which is the only time anyone opens it. A follower's
    // spell list changes rarely, but it does change (the console addspell that
    // set this test up is exactly such a change), so it is re-read rather than
    // cached until something invalidates it.
    v.spells = ScanCastableSpells(actor);
    v.consumables = ScanCarriedConsumables(actor);
    v.voiceRecovery = VoiceRecoveryOf(actor);
    for (auto *other : CollectManagedFollowers())
    {
        if (other && other != actor)
            v.peers.push_back({other->GetFormID(), DisplayNameOf(other)});
    }
    MarkPins(actor, v.inventory, v.magic);
    v.combatStyle = BuildCombatStyleSheet(actor);

    v.holdings.self = v.id;
    for (const auto &peer : v.peers)
        v.holdings.peers.push_back(peer.id);
    for (const auto &option : v.consumables)
        v.holdings.consumables.push_back({option.form, option.kind, option.effects});
    for (const auto &option : v.spells)
        v.holdings.castable.push_back(option.form);
    for (const auto &item : v.inventory)
    {
        const ft::Kind kind = item.category == ItemCategory::Arrows    ? ft::Kind::Ammo
                              : item.category == ItemCategory::Weapons ? ft::Kind::Weapon
                              : item.category == ItemCategory::Armor   ? ft::Kind::Armor
                                                                       : ft::Kind::Other;
        v.holdings.things.push_back({item.form, item.variant, kind});
    }
    for (const auto &entry : v.magic)
        v.holdings.things.push_back({entry.form, {}, ft::Kind::Spell});

    // A rule reads as the thing it names is called now, and keeps that
    // name while the thing is away (Action::name): the current name is
    // taken from the row for it each time the bag is read. For a copy, the
    // row is whatever holds the id now -- the engine hands an id out again
    // after its copy leaves -- and the rule says so, which is the contract.
    const auto currentName = [&](const ft::Action &action) -> std::string {
        if (ft::IsEquip(action.kind))
        {
            for (const auto &entry : v.magic)
                if (entry.form == action.form)
                    return entry.name;
            for (const auto &item : v.inventory)
                if (item.form == action.form && ft::SameVariant(item.variant, action.variant))
                    return item.name;
            return {};
        }
        if (ft::NamesConsumable(action.kind))
        {
            for (const auto &option : v.consumables)
                if (option.form == action.form && option.kind == ft::ConsumableOf(action.kind))
                    return option.name;
            return {};
        }
        for (const auto &option : v.spells)
            if (option.form == action.form)
                return option.name;
        return {};
    };
    ft::RuleSet rules = GetRules(v.id);
    bool renamed = false;
    for (ft::Rule &rule : rules.rules)
    {
        for (ft::Action &action : rule.actions)
        {
            if (!ft::NamesForm(action.kind) || action.form == 0)
                continue;
            const std::string now = currentName(action);
            if (!now.empty() && now != action.name)
            {
                action.name = now;
                renamed = true;
            }
        }
    }
    if (renamed)
        SetRules(v.id, std::move(rules));
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
    snapshot.health = ReadStat(actor, RE::ActorValue::kHealth);
    snapshot.magicka = ReadStat(actor, RE::ActorValue::kMagicka);
    snapshot.stamina = ReadStat(actor, RE::ActorValue::kStamina);

    FollowerView v;
    v.snapshot = snapshot;
    v.evaluated = false;
    v.inCombat = inCombat;
    FillDisplayFields(actor, v);
    PublishOne(std::move(v));
}

void PublishView(RE::Actor *actor, const ft::Snapshot &snapshot, const ft::Trace &trace,
                 const ft::ActionTrace &actionTrace);

void EvaluateFollower(RE::Actor *actor, double now, bool began, bool ended)
{
    const ft::ActorId id = actor->GetFormID();
    auto &state = g_followers[id];

    state.eval.caps = RuntimeCapabilities(actor);

    const auto started = std::chrono::steady_clock::now();

    ft::Snapshot snapshot = BuildSnapshot(actor, now);

    // Who is who, once per fight, so the Ally and Enemy subjects can be
    // read against the log. The fight's edge is the tick's (below); what
    // it resets in the evaluation context -- the cooldowns, a list part
    // way through -- the core resets on the same edge, where it is tested.
    if (began)
    {
        const auto names = [](const auto &views) {
            std::string out;
            for (const auto &v : views)
            {
                auto *who = RE::TESForm::LookupByID<RE::Actor>(v.id);
                out += (out.empty() ? "" : ", ") + std::string(NameOr(who, "?"));
            }
            return out.empty() ? std::string("nobody") : out;
        };
        const auto ids = [](const auto &views) {
            std::vector<std::uint32_t> out;
            out.reserve(views.size());
            for (const auto &v : views)
                out.push_back(v.id);
            return out;
        };
        log::tactics.event(log::Level::Info, "combat.entered", actor,
                           {{"allies", ids(snapshot.allies)}, {"enemies", ids(snapshot.enemies)}},
                           "{} entered combat -- tactics engaged; allies: {} -- enemies: {}", Describe(actor),
                           names(snapshot.allies), names(snapshot.enemies));
    }
    snapshot.combatBegan = began;
    snapshot.combatEnded = ended;

    // The other edge. One evaluation runs after a fight ends, for the rules
    // that ask about exactly that, and this is it -- so a query can bracket a
    // fight between the two events rather than guessing where it stopped.
    if (ended)
        log::tactics.event(log::Level::Info, "combat.left", actor, {}, "{} left combat -- the Combat end rules run",
                           Describe(actor));

    // This follower's own rules, not a shared static -- the whole point of
    // making them per-follower.
    const ft::RuleSet rules = GetRules(id);
    ft::Trace trace;
    ft::ActionTrace actionTrace;
    const ft::Decision decision = ft::Evaluate(rules, snapshot, state.eval, &trace, &actionTrace);

    // The cost measured is the snapshot and the evaluation -- the rules'
    // own -- not the panel's sheets, which PublishView builds after.
    g_cost.Add(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());

    PublishView(actor, snapshot, trace, actionTrace);

    if (decision.Fired())
    {
        // The one action of the tick; the rest of the rule's list follows,
        // one per tick.
        const auto index = static_cast<std::size_t>(decision.ruleIndex);
        const std::string label = index < rules.rules.size() ? rules.rules[index].label : "";
        {
            const auto &step = *decision.step;
            const auto result = Execute(step.action, step.target, actor);

            log::tactics.event(log::Level::Info, "rule.fired", actor,
                               {{"ruleIndex", decision.ruleIndex},
                                {"ruleName", label},
                                {"action", ft::WireName(step.action.kind)},
                                {"targetFormId", log::Id(step.target)},
                                {"outcome", ToString(result)},
                                {"healthPct", snapshot.health.Pct()}},
                               "{} FIRED rule {} \"{}\" [{}] -> {} [health {:.0f}/{:.0f} = {:.0f}%]", Describe(actor),
                               decision.ruleIndex, label, ft::WireName(step.action.kind), ToString(result),
                               snapshot.health.current, snapshot.health.max, snapshot.health.Pct() * 100.0);

            if (result != ActionResult::Performed)
            {
                // A rule that fires but does not take effect is the failure
                // worth shouting about: the engine believed it acted, and it
                // did not.
                log::tactics.event(log::Level::Warn, "rule.actionFailed", actor,
                                   {{"ruleIndex", decision.ruleIndex},
                                    {"ruleName", label},
                                    {"action", ft::WireName(step.action.kind)},
                                    {"reason", ToString(result)}},
                                   "{} action did NOT take effect: {}", Describe(actor), ToString(result));
            }
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
void PublishView(RE::Actor *actor, const ft::Snapshot &snapshot, const ft::Trace &trace,
                 const ft::ActionTrace &actionTrace)
{
    FollowerView v;
    v.snapshot = snapshot;
    v.trace = trace;
    v.actionTrace = actionTrace;
    v.evaluated = true;
    // Evaluated is no longer the same as fighting: the Combat end lists
    // run on after the fight.
    v.inCombat = snapshot.inCombat;
    FillDisplayFields(actor, v);
    PublishOne(std::move(v));
}

// --- the tick ---------------------------------------------------------------

// Is the world's clock stopped? Rules are gated on time running, not on
// any menu being closed -- those are different questions, and only the
// first is the one a tactic cares about. Firing into a frozen world is
// how a half-written rule drank potions while it was still being edited.
//
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
//
// Logged on change only. This runs every tick, and a gate that stays silent
// when it works is indistinguishable from one that is not running at all --
// which is exactly how the previous version stayed hidden for a whole test
// round. It also tells us WHICH signal caught a given menu, so this can be
// narrowed later on evidence rather than on a guess.
bool EvaluationHeld()
{
    auto *ui = RE::UI::GetSingleton();
    auto *main = RE::Main::GetSingleton();
    const bool pausedMenu = ui && ui->GameIsPaused();
    const bool frozenClock = main && main->GetRuntimeData().freezeTime;

    static int previous = -1;
    const int state = (pausedMenu ? 1 : 0) | (frozenClock ? 2 : 0);
    if (state != previous)
    {
        previous = state;
        if (state == 0)
            log::tactics.info("time is running -- evaluating");
        else
            log::tactics.info("time stopped ({}{}{}) -- evaluation held", pausedMenu ? "paused menu" : "",
                              (pausedMenu && frozenClock) ? " + " : "", frozenClock ? "frozen clock" : "");
    }
    return pausedMenu || frozenClock;
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
    RepublishOwed();

    // Not an early return on the tactics switch: the switch gates rule
    // EVALUATION, and the rest of this -- the views behind the Character,
    // Skills and Inventory tabs, the pin watchdog -- is not tactics and runs
    // whether or not they are being told what to do. The frozen clock still
    // holds everything, since nothing below can act on a stopped world.
    if (EvaluationHeld())
        return;

    // Game time, in real seconds: it does not advance while the game is
    // paused, so nothing below is aged by a menu.
    const double now = TacticsSeconds();
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
            log::tactics.event(log::Level::Info, "followers.controlled", {{"count", std::size_t{0}}},
                               "0 followers. Nobody nearby has the player-teammate flag -- "
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
            std::vector<std::uint32_t> ids;
            ids.reserve(followers.size());
            for (auto *f : followers)
                ids.push_back(f->GetFormID());
            log::tactics.event(log::Level::Info, "followers.controlled",
                               {{"count", followers.size()}, {"followers", ids}}, "{} follower(s) under control: {}",
                               followers.size(), names);
        }
    }

    // Pinned gear, independent of tactics. Cheap when nothing is pinned.
    // BEFORE the rules, and it matters: the pin pass is what remembers the
    // book when a fight begins, and a rule may pin on the very first tick
    // of one. Run after the rules, it remembered the rule's pin as the
    // player's, and restored it when the fight ended.
    // Anyone new takes their record from the save first -- BEFORE the pin
    // pass, which on a follower's first fighting tick remembers the book
    // for after the fight: pins adopted after that would be let go when it
    // ended, as if the fight had made them.
    for (auto *follower : followers)
        LoadIfNew(follower);

    KeepPins(followers);

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
        // Bleeding out, nothing can be performed: no potion, no cast, and the
        // 12:20 run fired a cast rule four times at negative health. Hold
        // evaluation until they are up again, and say so once.
        const bool down = follower->AsActorState() && follower->AsActorState()->IsBleedingOut();
        const bool wasDown = g_bleedingOut.contains(follower->GetFormID());
        if (down != wasDown)
        {
            log::tactics.event(log::Level::Info, down ? "follower.down" : "follower.up", follower, {}, "{} {}",
                               Describe(follower),
                               down ? "is bleeding out -- tactics held" : "is up -- tactics resume");
            if (down)
                g_bleedingOut.insert(follower->GetFormID());
            else
                g_bleedingOut.erase(follower->GetFormID());
        }

        // The edges of a fight, from the tick: one evaluation is the first
        // of the fight, and one more runs after it ends, for the rules that
        // ask about exactly that. The Combat end lists that evaluation
        // queues run one action per tick, so evaluation goes on out of the
        // fight while one is in progress; the core decides nothing else on
        // those ticks.
        // An edge is used up only by an evaluation. Held down or switched
        // off through it, the follower still owes the fight its first
        // evaluation, or the farewell one -- a heal-after-the-fight rule is
        // exactly what someone just up from bleeding out needs.
        auto &state = g_followers[follower->GetFormID()];
        const bool began = fighting && !state.fighting;
        const bool ended = !fighting && state.fighting;

        if (g_enabled.load() && (fighting || ended || state.eval.InProgress()) && !down &&
            IsFollowerEnabled(follower->GetFormID()))
        {
            state.fighting = fighting;
            EvaluateFollower(follower, now, began, ended);
        }
        else
            PublishIdle(follower, now, fighting);
    }

    // Armed cast requests are withdrawn from here, whether or not anyone is
    // still fighting: a request must not outlive the moment it was made for.
    TickPackages(now, followers);

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

    // Menu entries follow the views: added for a newcomer, removed for the
    // dismissed. After the erase above, so a dismissed follower is gone from
    // the views by the time this looks.
    ui::SyncFollowers();

    if (g_cost.samples > 0 && (now - g_lastCostReport) >= kCostReportInterval)
    {
        g_lastCostReport = now;
        log::tactics.event(log::Level::Info, "tactics.cost",
                           {{"evaluations", g_cost.samples}, {"avgUs", g_cost.AvgUs()}, {"maxUs", g_cost.maxUs}},
                           "{} evaluations, avg {:.0f} us, max {:.0f} us  (budget: under "
                           "500 us/frame across all followers)",
                           g_cost.samples, g_cost.AvgUs(), g_cost.maxUs);
        g_cost.Reset();
    }
}

} // namespace

ft::RuleSet GetRules(ft::ActorId id)
{
    std::scoped_lock lock(g_rulesMutex);
    const auto it = g_ruleSets.find(id);
    return it == g_ruleSets.end() ? kNoRules : it->second;
}

void PublishFollower(RE::Actor *actor)
{
    PublishIdle(actor, TacticsSeconds(), actor->IsInCombat());
}

void PublishAllFollowers()
{
    const auto followers = CollectManagedFollowers();
    for (auto *follower : followers)
        PublishFollower(follower);
    log::tactics.debug("panel opened -- {} follower view(s) refreshed", followers.size());
}

namespace
{
// The player's page. Guarded by the views' lock.
std::optional<CharacterView> g_playerView;
} // namespace

void PublishPlayer()
{
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!player)
        return;
    const auto started = std::chrono::steady_clock::now();
    CharacterView v;
    FillCharacterView(player, v);
    v.player = true;
    v.inCombat = player->IsInCombat();
    // A follower's scans, over a bag often many times the size of theirs,
    // on the frame the panel opens: measured, so a hitch there has a number.
    log::tactics.debug("player page built in {:.1f} ms: {} items, {} spells, {} effects",
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                       v.inventory.size(), v.magic.size(), v.effects.size());
    std::scoped_lock lock(g_viewMutex);
    g_playerView = std::move(v);
}

std::optional<CharacterView> ObservePlayer()
{
    std::scoped_lock lock(g_viewMutex);
    return g_playerView;
}

void SetRules(ft::ActorId id, ft::RuleSet rules)
{
    std::scoped_lock lock(g_rulesMutex);
    g_ruleSets[id] = std::move(rules);
}

void ForgetSession()
{
    g_identities.clear();
    // The fight too: a save loaded mid-fight into a quiet game would
    // otherwise see the old fight end on its first tick and run the
    // Combat end rules there.
    g_followers.clear();
    g_bleedingOut.clear();
    ForgetPins();
    {
        std::scoped_lock lock(g_rulesMutex);
        g_ruleSets.clear();
    }
    {
        std::scoped_lock lock(g_disabledMutex);
        g_disabledFollowers.clear();
    }
}

std::vector<FollowerView> ObserveFollowers()
{
    std::scoped_lock lock(g_viewMutex);
    return g_view;
}

std::optional<FollowerView> ObserveFollower(ft::ActorId id)
{
    std::scoped_lock lock(g_viewMutex);
    for (const auto &view : g_view)
        if (view.id == id)
            return view;
    return std::nullopt;
}

void Install()
{
    if (g_installed.exchange(true))
        return;

    log::tactics.event(log::Level::Info, "tactics.installed",
                       {{"tickMs", kTickInterval * 1000.0}, {"maxFollowers", kMaxManagedFollowers}},
                       "tick {:.0f} ms, rules in a fight and on its farewell, max {} followers", kTickInterval * 1000.0,
                       kMaxManagedFollowers);
    log::tactics.info("a follower starts with no rules; tactics are kept in the save (SKSE co-save)");

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
    log::tactics.event(log::Level::Info, "tactics.switched", {{"enabled", enabled}}, "{}",
                       enabled ? "enabled" : "disabled");
}

bool IsEnabled()
{
    return g_enabled.load();
}

} // namespace ft::game
