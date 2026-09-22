#include "game/Tactics.h"

#include "core/Coordinator.h"
#include "core/Evaluator.h"
#include "core/Tick.h"
#include "core/Vocabulary.h"
#include "game/Actions.h"
#include "game/Blows.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/PlayerCast.h"
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
// between decisions, and the only other timers are per action. (dev/PLAN.md
// 3.2 asked for 150 ms; a decision a half-second is the cadence a fight
// reads well at, and the cost is far under budget either way.)
constexpr double kTickInterval = 0.5;

// How often to report measured tick cost.
constexpr double kCostReportInterval = 5.0;

// --- state -----------------------------------------------------------------

std::atomic_bool g_enabled{true};
std::atomic_bool g_installed{false};

// A tick is waiting in SKSE's task queue. The pacing thread keeps time while
// the game thread is held up -- a load screen, a long frame -- and SKSE runs
// every queued task in one frame, so without this a stall would come back as
// a burst of ticks with no time between them.
std::atomic_bool g_tickQueued{false};

// Last reported follower count, so a change is logged once rather than every
// tick. Without this there is no way to tell a tick that is running and finding
// nobody from a tick that is not running at all -- both are silent, and the
// difference is "your setup script did not apply" versus "the mod is broken".
// -1 so the first report always fires, including the zero case.
int g_lastFollowerCount = -1;

struct FollowerState
{
    // The turn's own state: the fight's edges, the evaluation context and
    // the request in flight (core/Coordinator.h).
    ft::ActorRun run;
    // The verdicts last reported for each rule, and the rules they were for:
    // new rules, or a new fight, report afresh.
    ft::Trace reported;
    ft::RuleSet reportedRules;
};

std::unordered_map<ft::ActorId, FollowerState> g_followers;

// The followers switched OFF. A follower starts on: with the default rule
// set empty, on is safe -- an empty list does nothing -- and the switch is
// for silencing a written list without losing it. Only the exceptions are
// stored, so a follower we have never seen -- or a new one -- defaults to
// enabled without needing an entry. Written by the panel and by a load,
// read by the tick: guarded.
std::mutex g_disabledMutex;
// A set per list, the combat list's and the idle list's.
std::array<std::unordered_set<ft::ActorId>, 2> g_disabledFollowers;

std::unordered_set<ft::ActorId> &DisabledIn(ft::Moment moment)
{
    return g_disabledFollowers[static_cast<std::size_t>(moment)];
}

// Followers currently in bleedout, so the transition is logged once rather
// than every tick.
std::unordered_set<ft::ActorId> g_bleedingOut;

// Why the player's evaluation is held, or None while it is not: logged
// on change, for the same reason (core/PlayerCast.h, HeldReason).
ft::HeldReason g_playerHeld = ft::HeldReason::None;

// Per-follower rules, a map per moment. Absent means "has not been edited",
// and the default set is handed out instead -- so a new follower costs
// nothing until someone actually changes something.
std::mutex g_rulesMutex;
std::array<std::unordered_map<ft::ActorId, ft::RuleSet>, 2> g_ruleSets;

std::unordered_map<ft::ActorId, ft::RuleSet> &RuleSetsOf(ft::Moment moment)
{
    return g_ruleSets[static_cast<std::size_t>(moment)];
}

// How each follower is filed, learned the first time the tick sees them
// and kept for the session: a dismissed follower's rules are still theirs
// at the next save. Game thread only.
std::unordered_map<ft::ActorId, Identity> g_identities;

// An actor's two lists, read together under the one lock (core/Tick.h,
// ActorRules): the tick and the page each take one copy and decide
// everything from it.
ft::ActorRules RulesOf(ft::ActorId id)
{
    ft::ActorRules rules;
    rules.combat.moment = ft::Moment::Combat;
    rules.idle.moment = ft::Moment::Idle;
    std::scoped_lock lock(g_rulesMutex);
    for (const auto moment : {ft::Moment::Combat, ft::Moment::Idle})
    {
        const auto &sets = RuleSetsOf(moment);
        if (const auto it = sets.find(id); it != sets.end())
            (moment == ft::Moment::Idle ? rules.idle : rules.combat) = it->second;
    }
    return rules;
}

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
        RuleSetsOf(ft::Moment::Combat)[id] = std::move(profile->rules);
        RuleSetsOf(ft::Moment::Idle)[id] = std::move(profile->idleRules);
    }
    SetFollowerEnabled(id, ft::Moment::Combat, profile->enabled);
    SetFollowerEnabled(id, ft::Moment::Idle, profile->idleEnabled);
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
        f.profile.enabled = IsFollowerEnabled(id, ft::Moment::Combat);
        f.profile.idleEnabled = IsFollowerEnabled(id, ft::Moment::Idle);
        ft::ActorRules rules = RulesOf(id);
        f.profile.rules = std::move(rules.combat);
        f.profile.idleRules = std::move(rules.idle);
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
std::mutex g_viewMutex;
std::vector<SharedView> g_view;

// Per-evaluation cost, in microseconds. dev/PLAN.md 3.2 sets a budget -- total
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

// What the runtime can do this tick. The casts need the follower's own
// records (game/Packages.h); without them they report Unsupported, a
// truthful "not available here" rather than a rule that silently never
// fires.
ft::Capabilities RuntimeCapabilities(const RE::Actor *actor)
{
    ft::Capabilities caps;
    // The player's casts go through the input handler, not the cast
    // records (game/PlayerCast.h), and what their body has no route for is
    // marked off, so a rule of it says so. Ours in flight holds every
    // action, as a follower's does below.
    if (actor->IsPlayerRef())
    {
        for (std::size_t i = 0; i < caps.unsupported.size(); ++i)
            caps.unsupported[i] = !PlayerSupports(static_cast<ft::ActionKind>(i));
        if (IsPlayerMidCast())
            caps.busy.fill(true);
        return caps;
    }
    caps.castingAvailable = HasCastForms(actor);

    // A cast of OURS still in the air -- the lease is held from the request
    // until the follower's own spell-fire event names the spell -- makes
    // every action wait, not just another cast: a pin into the casting hand
    // or a potion would cut it off. "Combat start: cast Stoneflesh, equip
    // Flames" put Flames in the hand half a second into the cast. A list in
    // progress waits on the next tick; a fresh rule yields for this one.
    // Ours only: the AI's own casting, a pinned Flames streaming all fight,
    // holds nothing up. A power attack's record is held the same way, and a
    // bash in flight holds its block: a pin or a potion would cut either off.
    if (IsMidCast(actor) || IsMidBash(actor))
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
// Whose page this is: a living teammate, a person -- a horse, a dog or a
// familiar is a teammate to the engine and has nothing tactics can tell it
// to do (Sensors.h, IsPerson) -- and not one the player has dismissed.
//
// The teammate flag is the broad test on purpose: every follower framework
// sets it, so we integrate with all of them and depend on none. What it is
// not is prompt -- a framework may leave it set after a dismissal -- so the
// engine's own DismissedFollowerFaction has the last word. Currently
// following is the question, and it is not the same question as nearby:
// both answers hold for a follower waiting in another hold.
bool IsManagedFollower(RE::Actor *raw)
{
    return raw && !raw->IsDead() && raw->IsPlayerTeammate() && IsPerson(raw) && !IsDismissedFollower(raw);
}

void CollectFrom(const RE::BSTArray<RE::ActorHandle> &handles, std::vector<RE::Actor *> &into)
{
    for (auto &handle : handles)
    {
        auto actor = handle.get();
        RE::Actor *raw = actor ? actor.get() : nullptr;
        if (IsManagedFollower(raw))
            into.push_back(raw);
    }
}

std::vector<RE::Actor *> CollectManagedFollowers()
{
    std::vector<RE::Actor *> followers;

    auto *processLists = RE::ProcessLists::GetSingleton();
    if (!processLists)
        return followers;

    // High actors only: the fully simulated ones near the player. Both who
    // is evaluated and who is listed, deliberately -- being here is the only
    // test that tells a follower who is coming back from one who is not,
    // since a dismissed follower's marks are identical to a recruited one's
    // (dev/TODO.md, the marks read in Nordic Souls on 2026-09-20). A
    // follower who wanders off keeps the page they already had, marked.
    //
    // Note this does NOT filter on combat. Combat decides whether a follower is
    // EVALUATED, not whether they exist -- an earlier version conflated the two
    // and the panel stayed empty until a fight started, which is exactly when
    // you cannot calmly read it. Rules are authored before the fight.
    CollectFrom(processLists->highActorHandles, followers);
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

// Why each rule did or did not act, when that changes (core's
// VerdictChanges): the events log's answer to "why didn't they drink", which
// the panel's Status column gave for half a second. At debug in the prose
// log, where a line per change would crowd what is read while playing; the
// events file has every one.
void ReportVerdicts(RE::Actor *actor, const ft::RuleSet &rules, const ft::Trace &trace,
                    const ft::ActionTrace &actionTrace, ft::Trace &reported)
{
    for (const std::size_t i : ft::VerdictChanges(reported, trace))
    {
        const ft::Rule &rule = rules.rules[i];
        // Worded for the action the verdict is about: the first one reached.
        const ft::ActionKind kind =
            ft::ExplainedKind(rule, i < actionTrace.size() ? std::span<const ft::Verdict>(actionTrace[i])
                                                           : std::span<const ft::Verdict>{});
        const std::string_view reason = ft::Explain(trace[i], kind);
        log::tactics.event(
            log::Level::Debug, "rule.verdict", actor,
            {{"ruleIndex", i}, {"ruleName", rule.label}, {"verdict", ft::WireName(trace[i])}, {"reason", reason}},
            "{} rule {} \"{}\" [{}]: {}", Describe(actor), i, rule.label, ActionNames(rule), reason);
    }
}

// --- the pages --------------------------------------------------------------
//
// The panel draws one page of one character at a time, and that page is the
// only thing anyone can be reading, so it is the only thing built. Each
// filler below names the fields of one page; what a page does not draw is
// left as the last build of it left it. Game thread, and none of it under
// the views' lock: the scans are the slow part and the lock is taken only
// to hand the finished page over.

// What every page carries above its own content: who they are and the three
// bars with what they are made of. Actor values and their breakdowns, no
// scan of anything.
void FillVitals(RE::Actor *actor, CharacterView &v, bool inCombat)
{
    v.id = actor->GetFormID();
    v.name = DisplayNameOf(actor);
    v.inCombat = inCombat;
    v.level = actor->GetLevel();
    v.health = ReadStat(actor, RE::ActorValue::kHealth);
    v.stamina = ReadStat(actor, RE::ActorValue::kStamina);
    v.magicka = ReadStat(actor, RE::ActorValue::kMagicka);
    v.carriedWeight = actor->GetWeightInContainer();
    v.carryCapacity = actor->GetTotalCarryWeight();
    v.healthBreakdown = ValueBreakdown(actor, RE::ActorValue::kHealth, "");
    v.staminaBreakdown = ValueBreakdown(actor, RE::ActorValue::kStamina, "");
    v.magickaBreakdown = ValueBreakdown(actor, RE::ActorValue::kMagicka, "");
    v.carryBreakdown = CarryWeightBreakdown(actor);
}

// The bag and the spell list, each marked with the player's pins and bans.
// MarkPins reads only the list it is marking -- a spell entry's pin is
// answered from the pin list itself, never from the bag -- so each goes in
// with an empty stand-in for the other rather than the other being scanned
// as well.
void FillInventory(RE::Actor *actor, CharacterView &v)
{
    v.inventory = ScanInventory(actor);
    std::vector<MagicEntry> noMagic;
    MarkPins(actor, v.inventory, noMagic);
}

void FillMagic(RE::Actor *actor, CharacterView &v)
{
    v.magic = ScanMagic(actor);
    std::vector<InventoryItem> noItems;
    MarkPins(actor, noItems, v.magic);
}

// The rules' own page: the menus the editor offers and what it greys a rule
// by. The one page that needs both scans, since a rule may name anything
// they carry or know. A follower's spell list changes rarely, but it does
// change (a console addspell is exactly such a change), so it is re-read
// rather than kept until something says otherwise.
void FillTactics(RE::Actor *actor, FollowerView &v, ft::Moment moment)
{
    FillInventory(actor, v);
    FillMagic(actor, v);
    v.spells = ScanCastableSpells(actor);
    v.consumables = ScanCarriedConsumables(actor);
    v.peers.clear();
    for (auto *other : CollectManagedFollowers())
    {
        if (other && other != actor)
            v.peers.push_back({other->GetFormID(), DisplayNameOf(other)});
    }

    v.holdings = {};
    v.holdings.self = v.id;
    for (const auto &peer : v.peers)
        v.holdings.peers.push_back(peer.id);
    for (const auto &option : v.consumables)
        v.holdings.consumables.push_back({option.form, option.kind, option.effects, option.any});
    for (const auto &option : v.spells)
    {
        v.holdings.castable.push_back(option.form);
        if (option.dualCast)
            v.holdings.dualCastable.push_back(option.form);
    }
    // Asked here rather than of the snapshot: out of a fight a snapshot is
    // built only for an idle list with rules, and the editor greys a rule
    // then as much as in one. The perk alone, not what is in the hands: a follower with no
    // shield still has the perk or has not.
    v.holdings.powerBashPerk = PowerBashPerkMet(actor);
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
    // On the list itself, under its lock, before the snapshot below is
    // built: the page's copy is for the page. (Until 2026-09-19 the copy,
    // renamed, was written back whole after the build, over any edit the
    // panel made from the render thread meanwhile.)
    RefreshActionNames(v.id, moment, currentName);
    const ft::ActorRules rules = RulesOf(v.id);

    // What each action could do this moment, for the cells: the tick's own
    // judgement over a fresh snapshot and the actor's cooldowns, deciding
    // nothing. Out of a fight the snapshot is built only for an idle list
    // with rules, so for most actors this is the one place it costs
    // anything, once per page.
    {
        auto &state = g_followers[v.id];
        state.run.eval.caps = RuntimeCapabilities(actor);
        v.availability = ft::ProbeAvailability(
            rules.Of(moment), BuildSnapshot(actor, TacticsSeconds(), ft::SpellsNamedBy(rules)), state.run.eval);
    }
}

// One page of the sheet, the seven a follower and the player both have. The
// Character page reads the spell list too, but only to tell a spell in hand
// from an item when its sheet is clicked through to a page.
void FillPage(RE::Actor *actor, CharacterView &v, ui::Tab tab)
{
    switch (tab)
    {
    case ui::Tab::Character:
        v.sheet = BuildCharacterSheet(actor);
        FillMagic(actor, v);
        break;
    case ui::Tab::Inventory:
        FillInventory(actor, v);
        break;
    // One scan feeds both: the Magic tab draws the spells of it, the Shouts
    // tab the powers and shouts.
    case ui::Tab::Magic:
    case ui::Tab::Shouts:
        FillMagic(actor, v);
        break;
    case ui::Tab::Summons:
        v.summons = ScanSummons(actor);
        break;
    case ui::Tab::Effects:
        v.effects = ScanActiveEffects(actor);
        break;
    case ui::Tab::Skills:
        v.skills = BuildSkillSheet(actor);
        v.perks = BuildPerkPages(actor);
        v.trees = BuildPerkTrees(actor);
        break;
    default:
        break;
    }
}

// And the two a follower has beyond them.
void FillPage(RE::Actor *actor, FollowerView &v, ui::Tab tab)
{
    switch (tab)
    {
    case ui::Tab::CombatStyle:
        v.combatStyle = BuildCombatStyleSheet(actor);
        break;
    case ui::Tab::Tactics:
        FillTactics(actor, v, ft::Moment::Combat);
        break;
    case ui::Tab::IdleTactics:
        FillTactics(actor, v, ft::Moment::Idle);
        break;
    default:
        FillPage(actor, static_cast<CharacterView &>(v), tab);
        break;
    }
}

// The views' lock held.
std::vector<SharedView>::iterator FindView(ft::ActorId id)
{
    return std::find_if(g_view.begin(), g_view.end(), [id](const SharedView &v) { return v->id == id; });
}

void PublishOne(FollowerView v)
{
    std::scoped_lock lock(g_viewMutex);
    const auto it = FindView(v.id);
    auto published = std::make_shared<const FollowerView>(std::move(v));
    if (it != g_view.end())
        *it = std::move(published);
    else
        g_view.push_back(std::move(published));
}

// Everyone under tactics has an entry, so the panel can list them and open a
// page: their name and whether they are fighting, nothing scanned. A page is
// built when someone opens it, not because the follower exists. A view is
// shared with the render thread as published, so one that changes is a
// fresh copy put in its place, and one that has not is left alone.
void RefreshRoster(const std::vector<RE::Actor *> &followers)
{
    std::scoped_lock lock(g_viewMutex);
    for (auto *follower : followers)
    {
        const ft::ActorId id = follower->GetFormID();
        const std::string name = DisplayNameOf(follower);
        const bool fighting = follower->IsInCombat();
        const auto it = FindView(id);
        if (it != g_view.end() && (*it)->name == name && (*it)->inCombat == fighting && (*it)->nearby)
            continue;
        // Which marks they carry, as they first appear and whenever they
        // CHANGE: which a follower mod maintains is not in the records
        // (Sensors.h), and the telling comparison is one actor recruited
        // against the same actor dismissed. Logged on the change so the
        // two readings sit next to each other in the log; quiet otherwise.
        {
            static std::unordered_map<ft::ActorId, std::string> marks; // game thread, under the view lock
            std::string now = FollowerMarks(follower);
            const auto seen = marks.find(id);
            if (seen == marks.end() || seen->second != now)
            {
                log::tactics.debug("listing {} -- {}", Describe(follower), now);
                marks[id] = std::move(now);
            }
        }
        auto v = it != g_view.end() ? std::make_shared<FollowerView>(**it) : std::make_shared<FollowerView>();
        v->id = id;
        v->name = name;
        v->inCombat = fighting;
        v->nearby = true; // collected above, so with the player by definition
        if (it != g_view.end())
            *it = std::move(v);
        else
            g_view.push_back(std::move(v));
    }
}

// One list: the combat list in a fight and on its edges, the idle list out
// of one. The tick chooses (below); the context is one, so a cooldown
// spent by either holds for both.
ft::ActorTick::Now ReadTick(ft::ActorId id, const ft::ActorRules &rules, bool fighting, bool held);

// One actor's turn. The order -- a finished request's cooldown, the list
// to evaluate, the snapshot, the rules -- is core's (core/Coordinator.h,
// DecideTurn, tested); this reads the actor, builds the snapshot when one
// is asked for, performs the action decided and says what happened.
void RunTurn(RE::Actor *actor, double now, const ft::ActorRules &lists, bool held)
{
    const ft::ActorId id = actor->GetFormID();
    auto &state = g_followers[id];
    const bool player = actor->IsPlayerRef();

    ft::TickFacts facts;
    facts.now = ReadTick(id, lists, actor->IsInCombat(), held);
    facts.caps = RuntimeCapabilities(actor);
    facts.busy = player ? IsPlayerMidCast() : (IsMidCast(actor) || IsMidBash(actor));

    // The cost measured is the snapshot and the evaluation -- the rules'
    // own. The panel's pages are not in it: they are built when someone is
    // looking at one, not from here. Timed from inside the snapshot, since
    // a turn that evaluates nothing builds none.
    auto started = std::chrono::steady_clock::now();
    const auto snapshot = [&](ft::Moment) {
        started = std::chrono::steady_clock::now();
        return BuildSnapshot(actor, now, ft::SpellsNamedBy(lists));
    };

    ft::Trace trace;
    ft::ActionTrace actionTrace;
    const ft::TickResult turn = ft::DecideTurn(state.run, lists, facts, now, snapshot, &trace, &actionTrace);
    if (!turn)
        return;
    g_cost.Add(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());

    const ft::Moment moment = *turn.plan.list;
    const ft::Snapshot &snap = turn.snapshot;
    // Who is who, once per fight, so the Ally and Enemy subjects can be
    // read against the log. The fight's edge is the tick's; what it resets
    // in the evaluation context -- the cooldowns, a list part way through
    // -- the core resets on the same edge, where it is tested.
    if (turn.plan.began)
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
                           {{"allies", log::Actors(ids(snap.allies))}, {"enemies", log::Actors(ids(snap.enemies))}},
                           "{} entered combat -- tactics engaged; allies: {} -- enemies: {}", Describe(actor),
                           names(snap.allies), names(snap.enemies));
    }
    // The other edge. One evaluation runs after a fight ends, for the rules
    // that ask about exactly that, and this is it -- so a query can bracket a
    // fight between the two events rather than guessing where it stopped.
    if (turn.plan.ended)
        log::tactics.event(log::Level::Info, "combat.left", actor, {}, "{} left combat -- the Combat end rules run",
                           Describe(actor));

    // New rules, a new fight, or the other list, report every rule's
    // verdict afresh. The farewell evaluation reports nothing: every
    // standing rule turns false on it at once, which says only that the
    // fight is over.
    const ft::RuleSet &rules = lists.Of(moment);
    if (turn.plan.began || rules != state.reportedRules)
    {
        state.reported.clear();
        state.reportedRules = rules;
    }
    if (!turn.plan.ended)
        ReportVerdicts(actor, rules, trace, actionTrace, state.reported);

    if (!turn.Fired())
        return;

    // The one action of the tick; the rest of the rule's list follows,
    // one per tick. The rule as its list began: after a reorder or a
    // delete, the index names another rule.
    const ft::Decision &decision = turn.decision;
    const ft::Decision::Step &step = *decision.step;
    const std::string &label = decision.rule.label;
    // A requested cast's, power attack's or bash's outcome follows when
    // it is over, as rule.resolved, naming the rule given here.
    const ActionResult result = Execute(step.action, step.target, actor, decision.ruleIndex, label);
    ft::NoteOutcome(state.run, decision,
                    result == ActionResult::Performed   ? ft::ActionOutcome::Performed
                    : result == ActionResult::Requested ? ft::ActionOutcome::Requested
                                                        : ft::ActionOutcome::Failed);

    // Whom the condition bound and whom the action went at, by reference
    // and base, and the thing it used: the potion a policy chose, the
    // spell, the item and which copy of it.
    std::vector<log::Field> fields{{"list", moment == ft::Moment::Idle ? "idle" : "combat"},
                                   {"ruleIndex", decision.ruleIndex},
                                   {"ruleName", label},
                                   {"subjectKind", ft::WireName(decision.rule.subject)}};
    log::AppendActor(fields, "subjectFormId", "subjectBaseFormId", "subjectName", step.subject);
    fields.emplace_back("action", ft::WireName(step.action.kind));
    log::AppendActor(fields, "targetFormId", "targetBaseFormId", "targetName", step.target);
    if (step.action.form != 0)
        log::AppendForm(fields, "formId", "formName", step.action.form);
    if (ft::IsEquip(step.action.kind))
        fields.emplace_back("variant", ft::VariantText(step.action.variant));
    fields.emplace_back("outcome", ToString(result));
    fields.emplace_back("followerHealthPct", snap.health.Pct());
    log::tactics.event(log::Level::Info, "rule.fired", actor, fields,
                       "{} FIRED rule {} \"{}\" [{}] -> {} [health {:.0f}/{:.0f} = {:.0f}%]", Describe(actor),
                       decision.ruleIndex, label, ft::WireName(step.action.kind), ToString(result), snap.health.current,
                       snap.health.max, snap.health.Pct() * 100.0);

    // Requested is not a failure: a cast's own outcome follows when its
    // package is released.
    if (result != ActionResult::Performed && result != ActionResult::Requested)
    {
        // A rule that fires but does not take effect is the failure worth
        // shouting about: the engine believed it acted, and it did not.
        log::tactics.event(log::Level::Warn, "rule.actionFailed", actor,
                           {{"ruleIndex", decision.ruleIndex},
                            {"ruleName", label},
                            {"action", ft::WireName(step.action.kind)},
                            {"reason", ToString(result)}},
                           "{} action did NOT take effect: {}", Describe(actor), ToString(result));
    }
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

// What the tick reads of an actor for the core's choice of list
// (core/Tick.h): whether they are fighting, whether anything can be
// performed, and the switches.
ft::ActorTick::Now ReadTick(ft::ActorId id, const ft::ActorRules &rules, bool fighting, bool held)
{
    ft::ActorTick::Now now;
    now.fighting = fighting;
    now.held = held || !g_enabled.load();
    now.combatEnabled = IsFollowerEnabled(id, ft::Moment::Combat);
    now.idleEnabled = IsFollowerEnabled(id, ft::Moment::Idle);
    now.idleHasRules = !rules.idle.rules.empty();
    return now;
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
    // Not an early return on the tactics switch: the switch gates rule
    // EVALUATION, and the rest of this -- the roster behind the panel's list,
    // the pin watchdog -- is not tactics and runs whether or not they are
    // being told what to do. The frozen clock still holds everything, since
    // nothing below can act on a stopped world.
    if (EvaluationHeld())
        return;

    // Game time, in real seconds: it does not advance while the game is
    // paused, so nothing below is aged by a menu.
    const double now = TacticsSeconds();

    // No player means main menu or a load screen. Walking the process lists
    // then is pointless at best.
    if (!RE::PlayerCharacter::GetSingleton())
        return;

    const auto followers = CollectManagedFollowers();
    RefreshRoster(followers);

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
                               {{"count", followers.size()}, {"followers", log::Actors(ids)}},
                               "{} follower(s) under control: {}", followers.size(), names);
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
    // Before the rules too: a cast rule may fire on a follower's first tick.
    for (auto *follower : followers)
        ProvideCastForms(follower);

    KeepPins(followers);

    // Out of combat there is nothing to decide, so the expensive work -- the
    // inventory scan inside BuildSnapshot, and the evaluation itself -- is
    // skipped entirely. What remains is a few actor-value reads, so the panel
    // is not blank while you are standing there authoring rules.
    for (auto *follower : followers)
    {
        // Both switches must be on. A follower turned off still appears in the
        // panel, and still reports whether they are fighting -- they are simply
        // not evaluated.
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

        // The turn itself: a finished request's cooldown, the list this
        // tick is for and its edges, the evaluation, the action. Held down
        // through an edge, the follower still owes the fight its first
        // evaluation, or the farewell one.
        RunTurn(follower, now, RulesOf(follower->GetFormID()), down);
    }

    // The player, under rules of their own (dev/PLAYER.md). Found by hand,
    // since they carry no teammate flag, and given what a follower is
    // given less what is a follower's alone: no cast records, no pins, no
    // packages. Held while the player is somewhere an automatic cast is
    // wrong -- in dialogue, in furniture, mounted, in beast form
    // (PlayerHeld) -- and said once each way. The edges survive a hold as
    // they survive a bleedout above.
    if (auto *player = RE::PlayerCharacter::GetSingleton(); player && !player->IsDead())
    {
        LoadIfNew(player);
        const ft::HeldReason held = PlayerHeld(player);
        if (held != g_playerHeld)
        {
            const bool holding = held != ft::HeldReason::None;
            log::tactics.event(log::Level::Info, holding ? "player.held" : "player.free", player,
                               {{"reason", ft::ToString(held)}}, "{} {}", Describe(player),
                               holding ? std::string("is ") + ft::ToString(held) + " -- tactics held"
                                       : "is free -- tactics resume");
            g_playerHeld = held;
        }
        RunTurn(player, now, RulesOf(player->GetFormID()), held != ft::HeldReason::None);
    }

    // Armed cast requests are withdrawn from here, whether or not anyone is
    // still fighting: a request must not outlive the moment it was made for.
    TickPackages(now, followers);

    // Who is still theirs, and who of those is here. The walk above finds
    // high actors only, so a follower in another cell is absent from it
    // while still being a follower: away and dismissed are different things,
    // and reading the first as the second is what put "Dismissed" on the page
    // of a follower waiting in another hold. Only the genuinely gone -- no
    // longer a teammate, or dead -- are forgotten; the rest keep their page,
    // marked, so it can still be read and their rules written.
    {
        std::scoped_lock lock(g_viewMutex);
        for (auto &v : g_view)
        {
            const bool nearby = std::any_of(followers.begin(), followers.end(),
                                            [&v](const RE::Actor *f) { return f->GetFormID() == v->id; });
            if (nearby == v->nearby)
                continue;
            // Shared as published: a change is a copy, as in RefreshRoster.
            auto marked = std::make_shared<FollowerView>(*v);
            marked->nearby = nearby;
            v = std::move(marked);
        }
        // One definition of whose page this is, asked here as it was asked
        // when the roster was built: a follower dismissed while away has
        // the same answer as one dismissed in front of you.
        std::erase_if(g_view, [](const SharedView &v) {
            if (v->nearby)
                return false;
            return !IsManagedFollower(RE::TESForm::LookupByID<RE::Actor>(v->id));
        });
    }

    // Menu entries follow the views: added for a newcomer, removed for the
    // dismissed. After the erase above, so a dismissed follower is gone from
    // the views by the time this looks.
    ui::SyncFollowers();

    if (g_cost.samples > 0 && (now - g_lastCostReport) >= kCostReportInterval)
    {
        g_lastCostReport = now;
        log::tactics.info("{} evaluations, avg {:.0f} us, max {:.0f} us  (budget: under "
                          "500 us/frame across all followers)",
                          g_cost.samples, g_cost.AvgUs(), g_cost.maxUs);
        // Each step of the snapshot beside the whole, so a slow one is
        // named: a line for a debug log, not a player's.
        std::string steps;
        for (const StepCost &step : TakeSnapshotCosts())
        {
            if (step.samples == 0)
                continue;
            steps += fmt::format("{}{} avg {:.0f} us max {:.0f}", steps.empty() ? "" : ", ", step.name,
                                 step.totalUs / static_cast<double>(step.samples), step.maxUs);
        }
        log::tactics.debug("snapshot steps: {}", steps);
        g_cost.Reset();
    }
}

} // namespace

ft::RuleSet GetRules(ft::ActorId id, ft::Moment moment)
{
    std::scoped_lock lock(g_rulesMutex);
    const auto &sets = RuleSetsOf(moment);
    const auto it = sets.find(id);
    if (it != sets.end())
        return it->second;
    ft::RuleSet none = kNoRules;
    none.moment = moment;
    return none;
}

namespace
{
// The player's page. Guarded by the views' lock.
SharedView g_playerView;

// The page as it stands, into `out`; left as it is where there is no page
// yet, which is a blank one and the right thing to build onto. Copied into a
// reference rather than handed back in an optional because a page is rebuilt
// from what it already holds: through an optional that is a second copy and
// a move for nothing. It also keeps the analyser out of MSVC's optional,
// whose constructed storage it reads as uninitialised and then reports the
// move that follows as an assignment of garbage.
void CopyPlayerPage(FollowerView &out)
{
    std::scoped_lock lock(g_viewMutex);
    if (g_playerView)
        out = *g_playerView;
}

void CopyFollowerPage(ft::ActorId id, FollowerView &out)
{
    std::scoped_lock lock(g_viewMutex);
    for (const auto &view : g_view)
    {
        if (view->id == id)
        {
            out = *view;
            return;
        }
    }
}
} // namespace

// Built when the page changes, and never on a beat. Measured in play
// 2026-09-15: the player's magic page is ~92 ms, being 190 spells each
// described with its detail sections and effect tables. On a beat that was
// 92 ms of game thread twice a second for as long as the page sat open --
// felt as a cursor that would not keep up -- and bought nothing, since the
// clock is frozen behind the panel and nothing a page reads can move.
void RefreshShownPage()
{
    const ui::ShownPage shown = ui::Shown();
    if (shown.tab == ui::Tab::None || shown.actor == 0)
        return;
    auto *actor = RE::TESForm::LookupByID<RE::Actor>(shown.actor);
    if (!actor)
        return;

    // The page as it stands, so the fields this one does not draw keep what
    // the last build of them left: each filler writes its own and no more.
    // Copied out rather than built in place, so the scans below -- the slow
    // part, and the whole reason only one page is built -- run with no lock
    // held and the panel keeps drawing through them.
    const auto started = std::chrono::steady_clock::now();
    if (actor->IsPlayerRef())
    {
        FollowerView v;
        CopyPlayerPage(v);
        v.player = true;
        FillVitals(actor, v, actor->IsInCombat());
        FillPage(actor, v, shown.tab);
        std::scoped_lock lock(g_viewMutex);
        g_playerView = std::make_shared<const FollowerView>(std::move(v));
    }
    else
    {
        FollowerView v;
        CopyFollowerPage(shown.actor, v);
        // Away: the page stands as it was when they were last nearby. The
        // scans want an actor the engine is simulating, and one in another
        // cell has no high process for the sheets to read.
        if (!v.nearby)
            return;
        FillVitals(actor, v, actor->IsInCombat());
        FillPage(actor, v, shown.tab);
        PublishOne(std::move(v));
    }
    // What a page costs, measured rather than assumed: the player's bag is
    // the largest there is, and this is the line that says so.
    log::tactics.debug("{} page built for {} in {:.1f} ms", ui::Name(shown.tab), Describe(actor),
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

SharedView ObservePlayer()
{
    std::scoped_lock lock(g_viewMutex);
    return g_playerView;
}

void SetRules(ft::ActorId id, ft::RuleSet rules)
{
    std::scoped_lock lock(g_rulesMutex);
    RuleSetsOf(rules.moment)[id] = std::move(rules);
}

void RefreshActionNames(ft::ActorId id, ft::Moment moment,
                        const std::function<std::string(const ft::Action &)> &currentName)
{
    std::scoped_lock lock(g_rulesMutex);
    auto &sets = RuleSetsOf(moment);
    const auto it = sets.find(id);
    if (it != sets.end()) [[maybe_unused]]
        const bool renamed = ft::RefreshActionNames(it->second, currentName);
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
        for (auto &sets : g_ruleSets)
            sets.clear();
    }
    {
        std::scoped_lock lock(g_disabledMutex);
        for (auto &disabled : g_disabledFollowers)
            disabled.clear();
    }
}

std::vector<SharedView> ObserveFollowers()
{
    std::scoped_lock lock(g_viewMutex);
    return g_view;
}

SharedView ObserveFollower(ft::ActorId id)
{
    std::scoped_lock lock(g_viewMutex);
    const auto it = FindView(id);
    return it != g_view.end() ? *it : nullptr;
}

namespace
{
// The fast tick: every 50 ms while a blow or a cast on the player is in
// flight, and not otherwise. A bash is steps -- the block raised, the bash
// sent once it is up -- and a power attack's record has to go back the moment
// its swing ends; at the half-second turn the follower's AI lowers the block
// between two steps, or the procedure starts a second power attack. The
// player's cast is steps too, and its release has to land the moment the
// caster is ready.
constexpr double kFastInterval = 0.05;
std::atomic_bool g_fastQueued{false};

void FastTick()
{
    if (EvaluationHeld() || !RE::PlayerCharacter::GetSingleton())
        return;
    const double now = TacticsSeconds();
    TickWeaponLeases(now);
    TickBashes(now);
    TickPlayerCasts(now);
}
} // namespace

void Install()
{
    if (g_installed.exchange(true))
        return;

    log::tactics.info("tick {:.0f} ms, rules in a fight and on its farewell; {:.0f} ms while a blow is in flight",
                      kTickInterval * 1000.0, kFastInterval * 1000.0);
    log::tactics.info("a follower starts with no rules; tactics are kept in the save (SKSE co-save)");

    // Detached on purpose: Skyrim never unloads SKSE plugins, and joining a
    // sleeping thread during process teardown is a good way to hang on exit.
    std::thread([] {
        using clock = std::chrono::steady_clock;
        const auto turn = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kTickInterval));
        auto nextTurn = clock::now() + turn;
        while (g_installed.load())
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(kFastInterval));
            auto *task = SKSE::GetTaskInterface();
            if (clock::now() >= nextTurn)
            {
                nextTurn = clock::now() + turn;
                if (g_tickQueued.exchange(true))
                    continue;
                if (task)
                    task->AddTask([] {
                        g_tickQueued.store(false);
                        Tick();
                    });
                else
                    g_tickQueued.store(false);
            }
            else if ((AnyWeaponLease() || AnyBashInFlight() || AnyPlayerCastInFlight()) && !g_fastQueued.exchange(true))
            {
                if (task)
                    task->AddTask([] {
                        g_fastQueued.store(false);
                        FastTick();
                    });
                else
                    g_fastQueued.store(false);
            }
        }
    }).detach();
}

void SetFollowerEnabled(ft::ActorId id, ft::Moment moment, bool enabled)
{
    std::scoped_lock lock(g_disabledMutex);
    if (enabled)
        DisabledIn(moment).erase(id);
    else
        DisabledIn(moment).insert(id);
}

bool IsFollowerEnabled(ft::ActorId id, ft::Moment moment)
{
    std::scoped_lock lock(g_disabledMutex);
    return !DisabledIn(moment).contains(id);
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
