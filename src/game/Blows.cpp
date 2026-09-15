#include "game/Blows.h"

#include "game/Log.h"
#include "game/Util.h"

#include "RE/C/CombatAnimation.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace ft::game
{
namespace
{

// From the request to the block lowered. Long enough for a swing in progress
// to end and a block to come up; a request still waiting then is given up,
// and says at which step.
constexpr double kDeadlineSeconds = 2.0;
// How long a bash that was taken is watched for the bash attack state. The
// animation is under a second; one not seen by then was not made.
constexpr double kWatchSeconds = 1.5;
// How long a request that had to wait -- for the follower's own swing to
// end, or the block to come up -- holds the bash once the hands are free
// with the block up. Asked for on the tick the wait ended, the tree chose an
// ordinary attack or nothing: no bash in eight such requests, where ten of
// twelve that went straight through bashed (2026-09-15).
constexpr double kSettleSeconds = 0.25;

enum class Step : std::uint8_t
{
    Ready,    // waiting for the weapon drawn and no swing, then the block
    Blocking, // waiting for the block to be up, then the bash
    Bashing   // the bash was taken; watched until it ends
};

struct Run
{
    RE::ActorHandle actor;
    RE::ActorHandle target;
    std::uint32_t id = 0;
    bool power = false;
    Step step = Step::Ready;
    double requestedAt = 0.0;
    double blockAskedAt = -1.0;
    double blockUpAt = -1.0;
    double sentAt = -1.0;
    // Since when the hands have been free (the weapon drawn, no attack), and
    // free with the block up; -1 while they are not.
    double freeSince = -1.0;
    double steadySince = -1.0;
    // When the bash attack state was first seen, and when it was over; -1
    // for not yet. A bash is short, and one cut off shorter still.
    double bashFrom = -1.0;
    double bashEnd = -1.0;
    // The follower's stamina when the request was made: what a bash the
    // engine made was charged against.
    float staminaAtRequest = -1.0f;
    // Lowered at the end only if this request raised it: a block the
    // follower already held is their AI's to lower.
    bool raised = false;
    bool alreadyBlocking = false;
    bool sawBash = false;
    bool waited = false;   // a step could not go on at once; the bash settles first
    int blockRefusals = 0; // the left attack action was turned away
    int bashRefusals = 0;  // the bash was turned away with the block up
    // The first attack state other than a bash seen once the bash was taken,
    // -1 for none: a swing the tree chose over the bash.
    int otherAttackState = -1;
    int ruleIndex = -1;
    std::string ruleName;
    // The event of the attack data current when the bash state was first
    // seen: bashStart or bashPowerStart.
    std::string attackEvent;
};

// Game thread. The count is the pacing thread's, which only asks whether any
// is in flight.
std::vector<Run> g_runs;
std::atomic<int> g_inFlight{0};

const char *EventOf(const Run &run) noexcept
{
    return run.power ? "bashPowerStart" : "bashStart";
}

const char *KindOf(const Run &run) noexcept
{
    return run.power ? "power bash" : "bash";
}

// rule.resolved, as a cast's release reports it, with the timings this
// sequence exists to measure: whether the block was up already, when it came
// up, when the bash was taken, and how often each step was turned away.
void Report(const Run &run, RE::Actor *actor, const char *reason, double now)
{
    const auto since = [&run](double at) { return at < 0.0 ? -1.0 : at - run.requestedAt; };
    double bashSeconds = -1.0;
    if (run.bashFrom >= 0.0)
        bashSeconds = (run.bashEnd >= 0.0 ? run.bashEnd : now) - run.bashFrom;
    const float staminaNow = actor ? actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) : -1.0f;
    std::vector<log::Field> fields;
    if (!actor)
        fields.emplace_back("followerId", log::Id(run.id));
    fields.emplace_back("ruleIndex", run.ruleIndex);
    fields.emplace_back("ruleName", run.ruleName);
    fields.emplace_back("kind", KindOf(run));
    fields.emplace_back("outcome", run.sawBash ? "made" : "not-made");
    fields.emplace_back("reason", reason);
    fields.emplace_back("durationS", now - run.requestedAt);
    fields.emplace_back("alreadyBlocking", run.alreadyBlocking);
    fields.emplace_back("blockRaised", run.raised);
    fields.emplace_back("blockUpS", since(run.blockUpAt));
    fields.emplace_back("sentS", since(run.sentAt));
    fields.emplace_back("waited", run.waited);
    fields.emplace_back("handsFreeS", since(run.freeSince));
    fields.emplace_back("steadyS", since(run.steadySince));
    fields.emplace_back("blockRefusals", run.blockRefusals);
    fields.emplace_back("bashRefusals", run.bashRefusals);
    fields.emplace_back("attackStateSeen", run.otherAttackState);
    fields.emplace_back("bashS", bashSeconds);
    fields.emplace_back("attackEvent", run.attackEvent);
    fields.emplace_back("staminaAtRequest", static_cast<double>(run.staminaAtRequest));
    fields.emplace_back("staminaAtEnd", static_cast<double>(staminaNow));
    log::blows.event(log::Level::Info, "rule.resolved", actor, fields,
                     "{} rule {} \"{}\": {} {} -- {}, after {:.2f} s (block {}, up at {:.2f} s, steady at {:.2f} s, "
                     "taken at {:.2f} s{}, "
                     "refused {} block + {} bash; bash state {:.2f} s, stamina {:.0f} -> {:.0f})",
                     actor ? log::NameOf(actor) : log::Id(run.id), run.ruleIndex, run.ruleName, KindOf(run),
                     run.sawBash ? "made" : "not made", reason, now - run.requestedAt,
                     run.alreadyBlocking ? "already up"
                     : run.raised        ? "raised"
                                         : "not raised",
                     since(run.blockUpAt), since(run.steadySince), since(run.sentAt), run.waited ? " after a wait" : "",
                     run.blockRefusals, run.bashRefusals, bashSeconds, run.staminaAtRequest, staminaNow);
}

void Finish(const Run &run, RE::Actor *actor, const char *reason, double now)
{
    if (run.raised && actor)
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
// while it goes on.
const char *Advance(Run &run, RE::Actor *actor, double now)
{
    if (!actor)
        return "holder vanished";
    auto *state = actor->AsActorState();
    if (!state)
        return "no actor state";
    const auto attack = state->GetAttackState();
    const bool late = now - run.requestedAt >= kDeadlineSeconds;

    if (run.step == Step::Ready)
    {
        if (late)
        {
            if (!state->IsWeaponDrawn())
                return "deadline, weapon never drawn";
            return run.blockRefusals > 0 ? "deadline, block refused" : "deadline, still mid-swing";
        }
        if (!state->IsWeaponDrawn() || attack != RE::ATTACK_STATE_ENUM::kNone)
        {
            run.waited = true;
            run.freeSince = -1.0;
            return nullptr;
        }
        if (run.freeSince < 0.0)
            run.freeSince = now;
        if (actor->IsBlocking())
            run.alreadyBlocking = true;
        else
        {
            // The combat AI's own way up, from its Block behaviour: the left
            // attack action, which the idle tree resolves into a block for
            // what is in the hands.
            if (run.blockAskedAt < 0.0)
                run.blockAskedAt = now;
            run.raised = RE::CombatAnimation::Execute(actor, RE::CombatAnimation::ANIM::kActionLeftAttack);
            if (!run.raised)
            {
                ++run.blockRefusals;
                run.waited = true;
                return nullptr;
            }
        }
        run.step = Step::Blocking;
    }

    if (run.step == Step::Blocking)
    {
        if (late)
        {
            if (run.blockUpAt < 0.0)
                return "deadline, block never up";
            return run.bashRefusals > 0 ? "deadline, bash refused from the block" : "deadline, never steady";
        }
        // The follower's own AI may swing again, or drop the block, while
        // this waits; either starts the settle over.
        if (!actor->IsBlocking() || attack != RE::ATTACK_STATE_ENUM::kNone)
        {
            run.waited = true;
            run.steadySince = -1.0;
            return nullptr;
        }
        if (run.blockUpAt < 0.0)
            run.blockUpAt = now;
        if (run.steadySince < 0.0)
            run.steadySince = now;
        if (run.waited && now - run.steadySince < kSettleSeconds)
            return nullptr;
        // A bash is the right attack action from the block, which the tree
        // resolves into bashStart; the action is what sets the bash attack
        // state. A power bash is the same action carrying bashPowerStart.
        bool taken = false;
        if (run.power)
            taken = PerformRightAttackWith(actor, EventOf(run));
        else if (const auto target = run.target.get())
            taken = RE::CombatAnimation::Execute(actor, target.get(), RE::CombatAnimation::ANIM::kActionRightAttack);
        else
            taken = RE::CombatAnimation::Execute(actor, RE::CombatAnimation::ANIM::kActionRightAttack);
        if (!taken)
        {
            ++run.bashRefusals;
            return nullptr;
        }
        run.sentAt = now;
        run.step = Step::Bashing;
        log::blows.debug("{}: {} taken {:.2f} s after the request", Describe(actor),
                         run.power ? "the right attack action carrying bashPowerStart"
                                   : "the right attack action from the block",
                         now - run.requestedAt);
        return nullptr;
    }

    if (attack == RE::ATTACK_STATE_ENUM::kBash)
    {
        if (!run.sawBash)
        {
            run.bashFrom = now;
            if (const auto *data = AttackDataOf(actor))
                run.attackEvent = data->event.c_str();
        }
        run.sawBash = true;
    }
    else if (run.sawBash)
    {
        run.bashEnd = now;
        return "bash made";
    }
    else if (attack != RE::ATTACK_STATE_ENUM::kNone && run.otherAttackState < 0)
        run.otherAttackState = static_cast<int>(attack);
    if (now - run.sentAt >= kWatchSeconds)
    {
        if (run.sawBash)
            return "watch over, still bashing";
        return run.otherAttackState >= 0 ? "taken, an attack but no bash" : "taken, never bashed";
    }
    return nullptr;
}

} // namespace

BashRequest RequestBash(RE::Actor *actor, std::uint32_t targetId, bool power, int ruleIndex, std::string_view ruleName)
{
    if (IsMidBash(actor))
        return BashRequest::AlreadyBashing;
    Run run;
    run.actor = actor->GetHandle();
    if (auto *target = targetId != 0 ? RE::TESForm::LookupByID<RE::Actor>(targetId) : nullptr)
        run.target = target->GetHandle();
    run.staminaAtRequest = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
    run.id = actor->GetFormID();
    run.power = power;
    run.ruleIndex = ruleIndex;
    run.ruleName = ruleName;
    run.requestedAt = TacticsSeconds();
    const double now = run.requestedAt;
    g_runs.push_back(std::move(run));
    log::blows.debug("{}: {} requested", Describe(actor), power ? "power bash" : "bash");
    // The first step now rather than on the next fast tick: a follower who is
    // free and already blocking bashes on this frame.
    TickBashes(now);
    return BashRequest::Started;
}

bool IsMidBash(const RE::Actor *actor)
{
    if (!actor)
        return false;
    const std::uint32_t id = actor->GetFormID();
    return std::any_of(g_runs.begin(), g_runs.end(), [id](const Run &run) { return run.id == id; });
}

bool AnyBashInFlight() noexcept
{
    return g_inFlight.load(std::memory_order_relaxed) > 0;
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
    g_inFlight.store(static_cast<int>(g_runs.size()), std::memory_order_relaxed);
}

void EndAllBashes(const char *why)
{
    const double now = TacticsSeconds();
    for (const Run &run : g_runs)
    {
        const auto actor = run.actor.get();
        Finish(run, actor.get(), why, now);
    }
    g_runs.clear();
    g_inFlight.store(0, std::memory_order_relaxed);
}

void ResetBashes()
{
    g_runs.clear();
    g_inFlight.store(0, std::memory_order_relaxed);
}

} // namespace ft::game
