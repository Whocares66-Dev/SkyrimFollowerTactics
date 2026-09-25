#include "core/PlayerCast.h"

#include <algorithm>

namespace ft
{
namespace
{

void Begin(CastState &run, CastStep step, double now) noexcept
{
    run.step = step;
    run.stepAt = now;
}

} // namespace

std::vector<OwnFire> CastOwnFires(bool voice, Hand hand, std::uint32_t form)
{
    // The voice's fire says nothing of what went off; the run is the only
    // voice of the player's in flight.
    if (voice)
        return {{GraphTag::SpellFireVoice, std::nullopt}};
    std::vector<OwnFire> fires;
    if (Overlap(hand, Hand::Left))
        fires.push_back({GraphTag::SpellFireLeft, form});
    if (Overlap(hand, Hand::Right))
        fires.push_back({GraphTag::SpellFireRight, form});
    return fires;
}

GraphTags CastWakes(CastStep step) noexcept
{
    return step == CastStep::Lending ? GraphTags{GraphTag::InterruptCast} : GraphTags{};
}

void Hear(CastSeen &seen, const Heard &heard) noexcept
{
    seen.interrupts = heard.Count(GraphTag::InterruptCast);
    seen.ownFires = heard.ownFires;
}

const char *ToString(CastStep step) noexcept
{
    switch (step)
    {
    case CastStep::Lending:
        return "lending";
    case CastStep::Drawing:
        return "drawing";
    case CastStep::Pressing:
        return "pressing";
    case CastStep::Charging:
        return "charging";
    case CastStep::Holding:
        return "holding";
    case CastStep::Firing:
        return "firing";
    case CastStep::Restoring:
        return "restoring";
    }
    return "?";
}

std::vector<HandRestore> PlanRestore(const HeldSlot &left, const HeldSlot &right)
{
    std::vector<HandRestore> steps;
    bool restoredTwoHander = false;
    for (const bool isLeft : {true, false})
    {
        const HeldSlot &held = isLeft ? left : right;
        if (!held.lent)
            continue;
        if (held.spell != 0)
        {
            steps.push_back({isLeft, RestoreWhat::Spell, held.spell});
            continue;
        }
        if (held.item != 0)
        {
            if (held.twoHanded && restoredTwoHander)
                continue;
            restoredTwoHander = restoredTwoHander || held.twoHanded;
            steps.push_back({isLeft, RestoreWhat::Item, held.item});
            continue;
        }
        steps.push_back({isLeft, RestoreWhat::KeepBorrowed, 0});
    }
    return steps;
}

VoiceRestore PlanVoiceRestore(bool lent, bool hadBefore, bool weLentAShout) noexcept
{
    if (!lent)
        return VoiceRestore::None;
    if (hadBefore)
        return VoiceRestore::PutBack;
    return weLentAShout ? VoiceRestore::ReleaseShout : VoiceRestore::ReleasePower;
}

const char *AdvancePlayerCast(CastState &run, const CastSeen &seen, double now,
                              const std::function<void(CastCommand)> &perform)
{
    if (!seen.player)
        return "player vanished";
    const auto late = [&](double window) { return now - run.stepAt >= window; };
    const bool fireSeen = run.pressed && seen.ownFires > run.firesAtPress;

    if (run.step == CastStep::Lending)
    {
        if (!seen.placed && !run.lendAsked)
        {
            run.lendAsked = true;
            run.interruptsAtLend = seen.interrupts;
            perform(run.voice ? CastCommand::LendVoice : CastCommand::LendHands);
            return nullptr;
        }
        if (!seen.placed)
            return late(kLendSeconds)
                       ? (run.voice ? "the voice would not take it" : "the hand would not take the spell")
                       : nullptr;
        // A lend is an equip, and the equip plays in the animation graph
        // after the spell already shows in the hand, sending an
        // InterruptCast as it starts: 5 ms after the lend where the hand held
        // a spell, 70 to 90 ms where it held bare fists, and a press in
        // between was cut short by it (2026-09-24). So hands we lent are
        // pressed once it is heard; a hand that already held the spell had
        // no equip. Not at the equip's end: nothing after the InterruptCast
        // cuts a charge short, and the engine holds the press and begins the
        // cast as the equip animation ends whichever it follows. Where it is
        // never heard, the press goes at the lend's deadline, as before.
        if (run.lendAsked && !run.voice)
        {
            if (seen.interrupts > run.interruptsAtLend)
                run.settledAt = now;
            else if (!late(kLendSeconds))
                return nullptr;
        }
        Begin(run, CastStep::Drawing, now);
    }

    if (run.step == CastStep::Drawing)
    {
        // A shout needs no hands out. A press while sheathed only draws, so
        // the draw is asked for here and the press waits for it.
        if (run.voice || seen.weapon == CastSeen::Weapon::Drawn)
            Begin(run, CastStep::Pressing, now);
        else
        {
            if (!run.drew && seen.weapon == CastSeen::Weapon::Sheathed)
            {
                run.drew = true;
                perform(CastCommand::Draw);
            }
            return late(kDrawSeconds) ? "deadline, hands never drawn" : nullptr;
        }
    }

    if (run.step == CastStep::Pressing)
    {
        // The player's own doing holds the press: a swing, a block, a cast
        // of their own in this hand, the other hand's button down. The
        // pairing that makes a dual cast wants every caster idle, the
        // other hand's and the voice's too.
        const bool free = seen.casterIdle && (!run.dual || seen.othersIdle) && !seen.attacking && !seen.blocking &&
                          (run.voice || !seen.buttonHeld);
        if (!free)
            return late(kHandsFreeSeconds) ? "deadline, hands never free" : nullptr;
        if (seen.refusal)
        {
            run.reason = std::string("the engine refuses it: ") + seen.refusal;
            return "refused";
        }
        run.pressed = true;
        run.pressedAt = now;
        run.usedBefore = run.voice && seen.onUsedList;
        run.firesAtPress = seen.ownFires;
        perform(CastCommand::Press);
        Begin(run, CastStep::Charging, now);
        return nullptr;
    }

    run.highestState = (std::max)(run.highestState, seen.casterState);

    if (run.step == CastStep::Charging)
    {
        if (run.voice)
        {
            // A power is a tap, released on the next tick. A shout is held
            // while the engine charges its words, with the holds the
            // keyboard would send, and released once the highest word
            // unlocked is charged or the hold has gone on long enough. One
            // word unlocked is a tap: nothing to charge past the first.
            const int charged = run.shout ? seen.wordsCharged : -1;
            if (run.shout && run.wordsWanted > 0 && charged < run.wordsWanted && !late(kWordsSeconds))
            {
                perform(CastCommand::HoldPress);
                return nullptr;
            }
            run.wordsHeld = charged;
            run.released = true;
            run.releasedAt = now;
            perform(CastCommand::Release);
            Begin(run, CastStep::Firing, now);
            return nullptr;
        }
        // With a spell in each hand the handler holds a single press back,
        // waiting for the other hand inside its pairing window, and replays
        // it as a plain press only once a hold outlasts the window
        // (dev/PLAYER.md). The keyboard's holds do that in play; here they
        // are sent while the caster has not begun, and stop the moment it
        // has.
        if (!run.dual && !seen.casterHasSpell && seen.caster == CastSeen::Caster::None)
            perform(CastCommand::ReplayPress);
        // A stream is not released at Ready: it is held from when it starts.
        if (run.sustained && seen.caster == CastSeen::Caster::Casting)
        {
            run.readyAt = now;
            Begin(run, CastStep::Holding, now);
            return nullptr;
        }
        if (!run.sustained && seen.caster == CastSeen::Caster::Ready)
        {
            run.readyAt = now;
            run.released = true;
            run.releasedAt = now;
            perform(CastCommand::Release);
            Begin(run, CastStep::Firing, now);
            return nullptr;
        }
        // Idle again with nothing having fired: the player's own release, or
        // something that interrupted the charge.
        if (seen.casterIdle && run.highestState > 0)
            return "the charge was cut short";
        return late(kChargeSlackSeconds + run.chargeTime) ? "deadline, never ready" : nullptr;
    }

    if (run.step == CastStep::Holding)
    {
        if (fireSeen && run.firedAt < 0.0)
            run.firedAt = now;
        if (seen.casterIdle)
        {
            run.fired = run.firedAt >= 0.0;
            return run.fired ? "stream ended early" : "the stream was cut short";
        }
        if (now - run.stepAt < run.sustain)
            return nullptr;
        run.released = true;
        run.releasedAt = now;
        perform(CastCommand::Release);
        run.fired = run.firedAt >= 0.0;
        Begin(run, CastStep::Restoring, now);
        return nullptr;
    }

    if (run.step == CastStep::Firing)
    {
        // A power fires on the release with nothing to see but the engine's
        // used-power list taking it.
        const bool powerLanded = run.voice && !run.usedBefore && seen.onUsedList;
        if (fireSeen || powerLanded)
        {
            run.fired = true;
            run.firedAt = now;
            if (run.voice)
                perform(CastCommand::MarkPowerUsed);
            Begin(run, CastStep::Restoring, now);
        }
        // The voice caster sits idle while a shout plays -- the shout is the
        // process's, not the caster's -- so only a hand's cast is given up
        // on when its caster is idle again; the voice waits for its event.
        else if (!run.voice && seen.casterIdle && late(kFireGraceSeconds))
            return "released, ended without firing";
        else
            return late(kFireSeconds) ? "released, never fired" : nullptr;
    }

    if (run.step == CastStep::Restoring)
    {
        // The hands go back once the cast's own animation is over, so the
        // swap does not cut it off.
        if (seen.casterIdle || late(kSettleSeconds))
        {
            if (!run.fired)
                return "released, never fired";
            if (run.voice)
                return run.shout ? "shout fired" : "power cast";
            return run.sustained ? "stream ended" : "spell fired";
        }
    }
    return nullptr;
}

} // namespace ft

namespace ft
{

HeldReason HeldBy(const HoldFacts &facts) noexcept
{
    if (!facts.loaded)
        return HeldReason::NotLoaded;
    if (facts.inDialogue)
        return HeldReason::InDialogue;
    if (facts.controlsDisabled)
        return HeldReason::ControlsDisabled;
    if (facts.inFurniture)
        return HeldReason::InFurniture;
    if (facts.knockedDown)
        return HeldReason::KnockedDown;
    if (facts.swimming)
        return HeldReason::Swimming;
    if (facts.mounted)
        return HeldReason::Mounted;
    if (facts.inKillMove)
        return HeldReason::InKillMove;
    if (facts.beastForm)
        return HeldReason::BeastForm;
    return HeldReason::None;
}

const char *ToString(HeldReason reason) noexcept
{
    switch (reason)
    {
    case HeldReason::NotLoaded:
        return "not loaded";
    case HeldReason::InDialogue:
        return "in dialogue";
    case HeldReason::ControlsDisabled:
        return "fighting controls disabled";
    case HeldReason::InFurniture:
        return "in furniture";
    case HeldReason::KnockedDown:
        return "knocked down";
    case HeldReason::Swimming:
        return "swimming";
    case HeldReason::Mounted:
        return "mounted";
    case HeldReason::InKillMove:
        return "in a kill move";
    case HeldReason::BeastForm:
        return "in beast form";
    case HeldReason::None:
    default:
        return "";
    }
}

} // namespace ft
