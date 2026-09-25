#include "game/Blows.h"

#include "core/Bash.h"
#include "core/Blows.h"
#include "core/Strike.h"
#include "game/Graph.h"
#include "game/Log.h"
#include "game/Sensors.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include "RE/C/CombatAnimation.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ft::game
{
namespace
{

// The deadlines, the steps and the waits are core's (core/Bash.h,
// AdvanceBash), with the measurements behind them.

struct Run
{
    RE::ActorHandle actor;
    RE::ActorHandle target;
    std::uint32_t id = 0;
    // The request's standing: the step, the waits, the refusals, the times.
    ft::BashState state;
    // The follower's stamina when the request was made: what a bash the
    // engine made was charged against.
    float staminaAtRequest = -1.0f;
    int ruleIndex = -1;
    std::string ruleName;
    // The event of the attack data current when the bash state was first
    // seen: bashStart or bashPowerStart.
    std::string attackEvent;
    // Their graph from the request: what the steps count, and what steps
    // them.
    ft::GraphWatch watch;
};

// A power attack's request (core/Strike.h).
struct StrikeRun
{
    RE::ActorHandle actor;
    RE::ActorHandle target;
    std::uint32_t id = 0;
    ft::StrikeState state;
    std::string event; // the attack their hands make
    float staminaAtRequest = -1.0f;
    float headingOffAtRequest = -1.0f;
    int ruleIndex = -1;
    std::string ruleName;
    ft::GraphWatch watch; // as a bash's
};

// Game thread.
std::vector<Run> g_runs;
std::vector<StrikeRun> g_strikes;

const char *EventOf(const Run &run) noexcept
{
    return run.state.power ? "bashPowerStart" : "bashStart";
}

const char *KindOf(const Run &run) noexcept
{
    return run.state.power ? "power bash" : "bash";
}

// rule.resolved, as a cast's release reports it, with the timings this
// sequence exists to measure: whether the block was up already, when it came
// up, when the bash was taken, and how often each step was turned away.
void Report(const Run &run, RE::Actor *actor, const char *reason, double now)
{
    const auto since = [&run](double at) { return at < 0.0 ? -1.0 : at - run.state.requestedAt; };
    double bashSeconds = -1.0;
    if (run.state.bashFrom >= 0.0)
        bashSeconds = (run.state.bashEnd >= 0.0 ? run.state.bashEnd : now) - run.state.bashFrom;
    const float staminaNow = actor ? actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) : -1.0f;
    std::vector<log::Field> fields;
    if (!actor)
        fields.emplace_back("followerId", log::Id(run.id));
    fields.emplace_back("ruleIndex", run.ruleIndex);
    fields.emplace_back("ruleName", run.ruleName);
    fields.emplace_back("kind", KindOf(run));
    fields.emplace_back("outcome", run.state.sawBash ? "made" : "not-made");
    fields.emplace_back("reason", reason);
    fields.emplace_back("durationS", now - run.state.requestedAt);
    fields.emplace_back("alreadyBlocking", run.state.alreadyBlocking);
    fields.emplace_back("blockRaised", run.state.raised);
    fields.emplace_back("blockUpS", since(run.state.blockUpAt));
    fields.emplace_back("sentS", since(run.state.sentAt));
    fields.emplace_back("waited", run.state.waited);
    fields.emplace_back("handsFreeS", since(run.state.freeSince));
    fields.emplace_back("steadyS", since(run.state.steadySince));
    fields.emplace_back("blockRefusals", run.state.blockRefusals);
    fields.emplace_back("bashRefusals", run.state.bashRefusals);
    fields.emplace_back("attackStateSeen", run.state.otherAttackState);
    fields.emplace_back("bashS", bashSeconds);
    fields.emplace_back("attackEvent", run.attackEvent);
    fields.emplace_back("staminaAtRequest", static_cast<double>(run.staminaAtRequest));
    fields.emplace_back("staminaAtEnd", static_cast<double>(staminaNow));
    log::blows.event(log::Level::Info, "rule.resolved", actor, fields,
                     "{} rule {} \"{}\": {} {} -- {}, after {:.2f} s (block {}, up at {:.2f} s, steady at {:.2f} s, "
                     "taken at {:.2f} s{}, "
                     "refused {} block + {} bash; bash state {:.2f} s, stamina {:.0f} -> {:.0f})",
                     actor ? log::NameOf(actor) : log::Id(run.id), run.ruleIndex, run.ruleName, KindOf(run),
                     run.state.sawBash ? "made" : "not made", reason, now - run.state.requestedAt,
                     run.state.alreadyBlocking ? "already up"
                     : run.state.raised        ? "raised"
                                               : "not raised",
                     since(run.state.blockUpAt), since(run.state.steadySince), since(run.state.sentAt),
                     run.state.waited ? " after a wait" : "", run.state.blockRefusals, run.state.bashRefusals,
                     bashSeconds, run.staminaAtRequest, staminaNow);
}

void Finish(const Run &run, RE::Actor *actor, const char *reason, double now)
{
    if (run.state.raised && actor)
    {
        const bool lowered = RE::CombatAnimation::Execute(actor, RE::CombatAnimation::ANIM::kActionLeftRelease);
        log::blows.debug("{}: block lowered{}", Describe(actor), lowered ? "" : " -- the release was turned away");
    }
    Report(run, actor, reason, now);
}

// A power bash as the combat AI's melee chooser makes one (49170 on
// 1.6.1170): a CombatAnimation of the right attack action with the attack's
// event set in its output, then processed, then destroyed. The idle tree
// offers bashPowerStart to the player alone; the chooser's action carries
// the event past it.
bool PerformRightAttackWith(RE::Actor *actor, const char *event)
{
    auto *anim = RE::CombatAnimation::Create(actor, RE::CombatAnimation::ANIM::kActionRightAttack);
    if (!anim)
        return false;
    anim->animEvent = event;
    const bool performed = anim->Execute();
    anim->~CombatAnimation();
    RE::free(anim);
    return performed;
}

// The attack the engine made current: its event says which bash it was.
const RE::BGSAttackData *AttackDataOf(RE::Actor *actor)
{
    auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    auto *high = process ? process->high : nullptr;
    return high ? high->attackData.get() : nullptr;
}

// One step, where the request can take it; the reason it is over, or null
// while it goes on. The step is core's (AdvanceBash); this reads the actor
// and performs the two actions it asks for.
const char *Advance(Run &run, RE::Actor *actor, double now)
{
    ft::BashSeen seen;
    seen.holder = actor != nullptr;
    auto *state = actor ? actor->AsActorState() : nullptr;
    if (actor && !state)
        return "no actor state";
    if (state)
    {
        const auto attack = state->GetAttackState();
        seen.weaponDrawn = state->IsWeaponDrawn();
        seen.blocking = actor->IsBlocking();
        seen.attack = attack == RE::ATTACK_STATE_ENUM::kNone   ? ft::BashSeen::Attack::None
                      : attack == RE::ATTACK_STATE_ENUM::kBash ? ft::BashSeen::Attack::Bash
                                                               : ft::BashSeen::Attack::Other;
        seen.attackState = static_cast<int>(attack);
    }
    ft::Hear(seen, run.watch.HeardSoFar());
    log::blows.debug("{}: {} {}", Describe(actor), KindOf(run), ft::ReadsOf(seen, now));
    const bool sawBashBefore = run.state.sawBash;
    const auto perform = [&](ft::BashCommand command) {
        if (command == ft::BashCommand::RaiseBlock)
        {
            // The combat AI's own way up, from its Block behaviour: the left
            // attack action, which the idle tree resolves into a block for
            // what is in the hands.
            const bool raised = RE::CombatAnimation::Execute(actor, RE::CombatAnimation::ANIM::kActionLeftAttack);
            log::blows.debug("{}: the left attack action for the block {} {:.2f} s after the request", Describe(actor),
                             raised ? "taken" : "turned away", now - run.state.requestedAt);
            return raised;
        }
        // A bash is the right attack action from the block, which the tree
        // resolves into bashStart; the action is what sets the bash attack
        // state. A power bash is the same action carrying bashPowerStart.
        bool taken = false;
        if (run.state.power)
            taken = PerformRightAttackWith(actor, EventOf(run));
        else if (const auto target = run.target.get())
            taken = RE::CombatAnimation::Execute(actor, target.get(), RE::CombatAnimation::ANIM::kActionRightAttack);
        else
            taken = RE::CombatAnimation::Execute(actor, RE::CombatAnimation::ANIM::kActionRightAttack);
        log::blows.debug("{}: {} {} {:.2f} s after the request", Describe(actor),
                         run.state.power ? "the right attack action carrying bashPowerStart"
                                         : "the right attack action from the block",
                         taken ? "taken" : "turned away", now - run.state.requestedAt);
        return taken;
    };
    const char *over = ft::AdvanceBash(run.state, seen, now, perform);
    // The attack the engine made current on the first bash seen: its event
    // says which bash it was.
    if (!sawBashBefore && run.state.sawBash)
        if (const auto *data = AttackDataOf(actor))
            run.attackEvent = data->event.c_str();
    return over;
}

// How far round the target is from where the attack strikes, in degrees:
// their heading to the target, less the attack's own angle, as the
// UseWeapon procedure measures it before it swings (47297). Zero with no
// target to face.
float OffStrike(RE::Actor *actor, RE::Actor *target, const RE::BGSAttackData *attack)
{
    if (!actor || !target)
        return 0.0f;
    float off = actor->GetHeadingAngle(target->GetPosition(), false) - (attack ? attack->data.attackAngle : 0.0f);
    while (off > 180.0f)
        off -= 360.0f;
    while (off < -180.0f)
        off += 360.0f;
    return std::abs(off);
}

// rule.resolved for a power attack, with what this sequence measures: when
// it was taken, when it hit, how often it was turned away, and what the
// follower paid and faced.
void ReportStrike(const StrikeRun &run, RE::Actor *actor, const char *reason, double now)
{
    const auto since = [&run](double at) { return at < 0.0 ? -1.0 : at - run.state.requestedAt; };
    const float staminaNow = actor ? actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) : -1.0f;
    const auto target = run.target.get();
    const float offNow =
        actor && target ? OffStrike(actor, target.get(), AttackDataFor(actor, run.event.c_str())) : -1.0f;
    const bool made = std::string_view(reason) == "power attack made";
    std::vector<log::Field> fields;
    if (!actor)
        fields.emplace_back("followerId", log::Id(run.id));
    fields.emplace_back("ruleIndex", run.ruleIndex);
    fields.emplace_back("ruleName", run.ruleName);
    fields.emplace_back("kind", "power attack");
    fields.emplace_back("outcome", made ? "made" : "not-made");
    fields.emplace_back("reason", reason);
    fields.emplace_back("durationS", now - run.state.requestedAt);
    fields.emplace_back("attackEvent", run.event);
    fields.emplace_back("waited", run.state.waited);
    fields.emplace_back("sentS", since(run.state.sentAt));
    fields.emplace_back("hitS", since(run.state.hitAt));
    fields.emplace_back("refusals", run.state.refusals);
    fields.emplace_back("staminaAtRequest", static_cast<double>(run.staminaAtRequest));
    fields.emplace_back("staminaAtEnd", static_cast<double>(staminaNow));
    fields.emplace_back("offStrikeAtRequest", static_cast<double>(run.headingOffAtRequest));
    fields.emplace_back("offStrikeAtEnd", static_cast<double>(offNow));
    log::blows.event(log::Level::Info, "rule.resolved", actor, fields,
                     "{} rule {} \"{}\": power attack {} -- {}, after {:.2f} s ({}{}, taken at {:.2f} s, hit at "
                     "{:.2f} s, refused {}; stamina {:.0f} -> {:.0f}; {:.0f} -> {:.0f} deg off the strike)",
                     actor ? log::NameOf(actor) : log::Id(run.id), run.ruleIndex, run.ruleName,
                     made ? "made" : "not made", reason, now - run.state.requestedAt, run.event,
                     run.state.waited ? ", after a wait" : "", since(run.state.sentAt), since(run.state.hitAt),
                     run.state.refusals, run.staminaAtRequest, staminaNow, run.headingOffAtRequest, offNow);
}

// One step of a power attack; the reason it is over, or null. The step is
// core's (AdvanceStrike); this reads the actor and performs the action.
const char *AdvanceStrikeRun(StrikeRun &run, RE::Actor *actor, double now)
{
    ft::StrikeSeen seen;
    seen.holder = actor != nullptr;
    auto *state = actor ? actor->AsActorState() : nullptr;
    if (actor && !state)
        return "no actor state";
    if (state)
    {
        seen.weaponDrawn = state->IsWeaponDrawn();
        seen.attacking = state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
        // The engine's own summary of their casters, a bit per source, the
        // voice's among them.
        seen.casting = actor->WhoIsCasting() != 0;
        const RE::BGSAttackData *attack = AttackDataFor(actor, run.event.c_str());
        // The attack's own strike angle; a whole quarter where the data does
        // not say, since the engine's check would pass anything it lacks.
        const float strike = attack ? attack->data.strikeAngle : 45.0f;
        const auto target = run.target.get();
        seen.facing = OffStrike(actor, target.get(), attack) <= strike;
    }
    ft::Hear(seen, run.watch.HeardSoFar());
    log::blows.debug("{}: power attack {}", Describe(actor), ft::ReadsOf(seen, now));
    const auto perform = [&] {
        const bool taken = PerformRightAttackWith(actor, run.event.c_str());
        log::blows.debug("{}: the right attack action carrying {} {} {:.2f} s after the request", Describe(actor),
                         run.event, taken ? "taken" : "turned away", now - run.state.requestedAt);
        return taken;
    };
    return ft::AdvanceStrike(run.state, seen, now, perform);
}

void TickStrikes(double now)
{
    for (auto it = g_strikes.begin(); it != g_strikes.end();)
    {
        const auto actor = it->actor.get();
        if (const char *over = AdvanceStrikeRun(*it, actor.get(), now))
        {
            ReportStrike(*it, actor.get(), over, now);
            it = g_strikes.erase(it);
        }
        else
            ++it;
    }
}

void TickBashes(double now)
{
    for (auto it = g_runs.begin(); it != g_runs.end();)
    {
        const auto actor = it->actor.get();
        if (const char *over = Advance(*it, actor.get(), now))
        {
            Finish(*it, actor.get(), over, now);
            it = g_runs.erase(it);
        }
        else
            ++it;
    }
}

} // namespace

BlowRequest RequestBash(RE::Actor *actor, std::uint32_t targetId, bool power, int ruleIndex, std::string_view ruleName)
{
    if (IsMidBlow(actor))
        return BlowRequest::AlreadyInFlight;
    Run run;
    run.actor = actor->GetHandle();
    if (auto *target = targetId != 0 ? RE::TESForm::LookupByID<RE::Actor>(targetId) : nullptr)
        run.target = target->GetHandle();
    run.staminaAtRequest = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
    run.id = actor->GetFormID();
    run.ruleIndex = ruleIndex;
    run.ruleName = ruleName;
    const double now = TacticsSeconds();
    run.state = ft::RequestBashAt(now, power);
    run.watch = WatchGraph(actor, ft::kBlowWakes);
    g_runs.push_back(std::move(run));
    log::blows.debug("{}: {} requested", Describe(actor), power ? "power bash" : "bash");
    // The first step now rather than on the next event: a follower who is
    // free and already blocking bashes on this frame.
    TickBlows(now);
    return BlowRequest::Started;
}

BlowRequest RequestStrike(RE::Actor *actor, std::uint32_t targetId, const char *event, int ruleIndex,
                          std::string_view ruleName)
{
    if (!event)
        return BlowRequest::AlreadyInFlight;
    if (IsMidBlow(actor))
        return BlowRequest::AlreadyInFlight;
    StrikeRun run;
    run.actor = actor->GetHandle();
    auto *target = targetId != 0 ? RE::TESForm::LookupByID<RE::Actor>(targetId) : nullptr;
    if (target)
        run.target = target->GetHandle();
    run.id = actor->GetFormID();
    run.event = event;
    run.staminaAtRequest = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
    run.headingOffAtRequest = target ? OffStrike(actor, target, AttackDataFor(actor, event)) : -1.0f;
    run.ruleIndex = ruleIndex;
    run.ruleName = ruleName;
    const double now = TacticsSeconds();
    run.state = ft::RequestStrikeAt(now);
    run.watch = WatchGraph(actor, ft::kBlowWakes);
    g_strikes.push_back(std::move(run));
    log::blows.debug("{}: power attack {} requested", Describe(actor), event);
    // The first step now: a follower who is free and facing swings on this
    // frame.
    TickBlows(now);
    return BlowRequest::Started;
}

bool IsMidBlow(const RE::Actor *actor)
{
    if (!actor)
        return false;
    const std::uint32_t id = actor->GetFormID();
    return std::any_of(g_runs.begin(), g_runs.end(), [id](const Run &run) { return run.id == id; }) ||
           std::any_of(g_strikes.begin(), g_strikes.end(), [id](const StrikeRun &run) { return run.id == id; });
}

void TickBlows(double now)
{
    TickBashes(now);
    TickStrikes(now);
}

void EndAllBlows(const char *why)
{
    const double now = TacticsSeconds();
    for (const Run &run : g_runs)
    {
        const auto actor = run.actor.get();
        Finish(run, actor.get(), why, now);
    }
    for (const StrikeRun &run : g_strikes)
    {
        const auto actor = run.actor.get();
        ReportStrike(run, actor.get(), why, now);
    }
    g_runs.clear();
    g_strikes.clear();
}

void ResetBlows()
{
    g_runs.clear();
    g_strikes.clear();
}

} // namespace ft::game
