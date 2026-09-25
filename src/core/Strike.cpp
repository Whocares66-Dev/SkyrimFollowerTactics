#include "core/Strike.h"

namespace ft
{

void Hear(StrikeSeen &seen, const Heard &heard) noexcept
{
    seen.hitFrames = heard.Count(GraphTag::HitFrame);
    seen.powerStops = heard.Count(GraphTag::PowerAttackStop);
    seen.attackStops = heard.Count(GraphTag::AttackStop);
}

std::string ReadsOf(const StrikeSeen &seen, double now)
{
    return StepAt(now) + "holder=" + (seen.holder ? "1" : "0") + " drawn=" + (seen.weaponDrawn ? "1" : "0") +
           " attacking=" + (seen.attacking ? "1" : "0") + " casting=" + (seen.casting ? "1" : "0") +
           " facing=" + (seen.facing ? "1" : "0") + " hitFrames=" + std::to_string(seen.hitFrames) +
           " powerStops=" + std::to_string(seen.powerStops) + " attackStops=" + std::to_string(seen.attackStops);
}

StrikeState RequestStrikeAt(double now) noexcept
{
    StrikeState state;
    state.requestedAt = now;
    return state;
}

const char *AdvanceStrike(StrikeState &run, const StrikeSeen &seen, double now, const std::function<bool()> &perform)
{
    if (!seen.holder)
        return "holder vanished";

    if (run.step == StrikeStep::Ready)
    {
        const bool late = now - run.requestedAt >= kStrikeDeadlineSeconds;
        // Their own swing, shout or spell first: an action taken during one
        // is turned away -- a power attack sent as a bare event mid-swing
        // landed 3 times in 11 (2026-09-09), and one asked for while their
        // AI shouted was turned away twice and ran out of time
        // (2026-09-25). The target in front next, as the UseWeapon
        // procedure asks before it swings (47297): their combat controller
        // turns them, and an attack thrown to one side hits nothing.
        if (!seen.weaponDrawn || seen.attacking || seen.casting)
        {
            run.waited = true;
            if (!late)
                return nullptr;
            if (!seen.weaponDrawn)
                return "deadline, weapon never drawn";
            return seen.attacking ? "deadline, still mid-swing" : "deadline, still mid-cast";
        }
        if (!seen.facing)
        {
            run.waited = true;
            return late ? "deadline, target never in front" : nullptr;
        }
        run.hitFramesBefore = seen.hitFrames;
        run.powerStopsBefore = seen.powerStops;
        run.attackStopsBefore = seen.attackStops;
        if (!perform())
        {
            ++run.refusals;
            run.waited = true;
            return late ? "deadline, refused" : nullptr;
        }
        run.sentAt = now;
        run.step = StrikeStep::Striking;
        return nullptr;
    }

    // Made when the attack reached its hit: a hit frame since the action, or
    // a power attack's own PowerAttackStop. Over at its end: an end event
    // once it has hit, or the attack state gone once it was seen. An end
    // event alone is not this attack's, since one comes as a block gives way
    // to an attack. A power attack that goes into its state and out again
    // with no hit was cut short: a dual-wield one taken with one weapon did,
    // no stamina spent (2026-09-24).
    if (seen.attacking)
        run.sawAttack = true;
    if (seen.hitFrames > run.hitFramesBefore || seen.powerStops > run.powerStopsBefore)
    {
        if (run.hitAt < 0.0)
            run.hitAt = now;
    }
    const bool hit = run.hitAt >= 0.0;
    const bool endEvent = seen.powerStops > run.powerStopsBefore || seen.attackStops > run.attackStopsBefore;
    if ((hit && (endEvent || (run.sawAttack && !seen.attacking))) || (!hit && run.sawAttack && !seen.attacking))
    {
        run.endAt = now;
        return hit ? "power attack made" : "power attack cut short";
    }
    if (now - run.sentAt >= kStrikeWatchSeconds)
    {
        if (hit)
            return "watch over, hit but still swinging";
        return run.sawAttack ? "watch over, still swinging" : "taken, never swung";
    }
    return nullptr;
}

} // namespace ft
