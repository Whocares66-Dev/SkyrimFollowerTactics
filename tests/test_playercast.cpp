// The player's cast from the request to the hands given back. No Skyrim;
// the player's state is supplied each tick and the commands are recorded.

#include <catch2/catch_test_macros.hpp>

#include "core/PlayerCast.h"

#include <string>
#include <vector>

using namespace ft;

namespace
{

std::string Over(const char *reason)
{
    return reason ? reason : "";
}

struct Sent
{
    std::vector<CastCommand> commands;
    std::function<void(CastCommand)> Fn()
    {
        return [this](CastCommand c) { commands.push_back(c); };
    }
    [[nodiscard]] CastCommand Last() const
    {
        return commands.back();
    }
};

CastState Spell(double now = 100.0)
{
    CastState run;
    run.requestedAt = run.stepAt = now;
    return run;
}

CastState Shout(int words, double now = 100.0)
{
    CastState run = Spell(now);
    run.voice = true;
    run.shout = true;
    run.wordsWanted = words;
    return run;
}

CastState Power(double now = 100.0)
{
    CastState run = Spell(now);
    run.voice = true;
    return run;
}

// The spell in the hand, the hands out, the caster idle and free.
CastSeen Free()
{
    CastSeen seen;
    seen.placed = true;
    seen.weapon = CastSeen::Weapon::Drawn;
    seen.casterIdle = true;
    seen.caster = CastSeen::Caster::None;
    return seen;
}

CastSeen Charging(int state = 2)
{
    CastSeen seen = Free();
    seen.casterIdle = false;
    seen.casterHasSpell = true;
    seen.caster = CastSeen::Caster::Other;
    seen.casterState = state;
    return seen;
}

CastSeen Ready()
{
    CastSeen seen = Charging(3);
    seen.caster = CastSeen::Caster::Ready;
    return seen;
}

CastSeen Casting()
{
    CastSeen seen = Charging(4);
    seen.caster = CastSeen::Caster::Casting;
    return seen;
}

} // namespace

TEST_CASE("a spell in a drawn hand: pressed on the first tick, released at ready, fired, given back", "[playercast]")
{
    CastState run = Spell();
    Sent sent;
    REQUIRE_FALSE(AdvancePlayerCast(run, Free(), 100.0, sent.Fn()));
    REQUIRE(sent.commands == std::vector<CastCommand>{CastCommand::Press});
    REQUIRE(run.step == CastStep::Charging);
    REQUIRE(run.pressed);
    REQUIRE(run.pressedAt == 100.0);
    REQUIRE_FALSE(run.lendAsked);
    REQUIRE_FALSE(run.drew);

    // Charging: the handler's pairing window is outlasted by replayed
    // holds while the caster has not begun, and by nothing once it has.
    REQUIRE_FALSE(AdvancePlayerCast(run, Free(), 100.05, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::ReplayPress);
    REQUIRE_FALSE(AdvancePlayerCast(run, Charging(), 100.1, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::ReplayPress);
    REQUIRE(run.highestState == 2);

    REQUIRE_FALSE(AdvancePlayerCast(run, Ready(), 100.5, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::Release);
    REQUIRE(run.released);
    REQUIRE(run.readyAt == 100.5);
    REQUIRE(run.step == CastStep::Firing);

    CastSeen fired = Charging(5);
    fired.fireSeen = true;
    REQUIRE_FALSE(AdvancePlayerCast(run, fired, 100.6, sent.Fn()));
    REQUIRE(run.fired);
    REQUIRE(run.firedAt == 100.6);
    REQUIRE(run.step == CastStep::Restoring);
    // Given back once the caster is idle.
    REQUIRE_FALSE(AdvancePlayerCast(run, Charging(5), 100.7, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(run, Free(), 101.0, sent.Fn())) == "spell fired");
    REQUIRE(sent.commands.size() == 3);
}

TEST_CASE("the spell is lent once and waited for; the voice the same", "[playercast]")
{
    CastState run = Spell();
    Sent sent;
    CastSeen empty = Free();
    empty.placed = false;
    REQUIRE_FALSE(AdvancePlayerCast(run, empty, 100.0, sent.Fn()));
    REQUIRE(sent.commands == std::vector<CastCommand>{CastCommand::LendHands});
    REQUIRE(run.lendAsked);
    // Not lent twice, and the deadline names the hand.
    REQUIRE_FALSE(AdvancePlayerCast(run, empty, 100.5, sent.Fn()));
    REQUIRE(sent.commands.size() == 1);
    REQUIRE(Over(AdvancePlayerCast(run, empty, 101.0, sent.Fn())) == "the hand would not take the spell");

    CastState voice = Power();
    Sent sentVoice;
    REQUIRE_FALSE(AdvancePlayerCast(voice, empty, 100.0, sentVoice.Fn()));
    REQUIRE(sentVoice.commands == std::vector<CastCommand>{CastCommand::LendVoice});
    REQUIRE(Over(AdvancePlayerCast(voice, empty, 101.0, sentVoice.Fn())) == "the voice would not take it");

    // Placed on a later tick, and the equip's InterruptCast heard: on to
    // the press, in the same tick. Placed but not yet heard, it waits: that
    // InterruptCast would cut a press short.
    CastState placed = Spell();
    Sent sentPlaced;
    REQUIRE_FALSE(AdvancePlayerCast(placed, empty, 100.0, sentPlaced.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(placed, Free(), 100.05, sentPlaced.Fn()));
    REQUIRE(sentPlaced.Last() == CastCommand::LendHands);
    REQUIRE(placed.step == CastStep::Lending);
    CastSeen settled = Free();
    settled.equipSettled = true;
    REQUIRE_FALSE(AdvancePlayerCast(placed, settled, 100.1, sentPlaced.Fn()));
    REQUIRE(sentPlaced.Last() == CastCommand::Press);
    REQUIRE(placed.settledAt == 100.1);

    // Never heard: pressed at the lend's deadline, as before the wait.
    CastState unheard = Spell();
    Sent sentUnheard;
    REQUIRE_FALSE(AdvancePlayerCast(unheard, empty, 100.0, sentUnheard.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(unheard, Free(), 100.9, sentUnheard.Fn()));
    REQUIRE(sentUnheard.Last() == CastCommand::LendHands);
    REQUIRE_FALSE(AdvancePlayerCast(unheard, Free(), 101.0, sentUnheard.Fn()));
    REQUIRE(sentUnheard.Last() == CastCommand::Press);
    REQUIRE(unheard.settledAt < 0.0);

    // The voice has no hands to settle: placed, it goes on.
    CastState power = Power();
    Sent sentPower;
    REQUIRE_FALSE(AdvancePlayerCast(power, empty, 100.0, sentPower.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(power, Free(), 100.05, sentPower.Fn()));
    REQUIRE(sentPower.Last() == CastCommand::Press);
}

TEST_CASE("sheathed hands are drawn once and waited for; a shout needs no hands", "[playercast]")
{
    CastState run = Spell();
    Sent sent;
    CastSeen sheathed = Free();
    sheathed.weapon = CastSeen::Weapon::Sheathed;
    REQUIRE_FALSE(AdvancePlayerCast(run, sheathed, 100.0, sent.Fn()));
    REQUIRE(sent.commands == std::vector<CastCommand>{CastCommand::Draw});
    REQUIRE(run.drew);
    // Drawing: not asked again, and the step's own clock.
    CastSeen drawing = Free();
    drawing.weapon = CastSeen::Weapon::Other;
    REQUIRE_FALSE(AdvancePlayerCast(run, drawing, 101.9, sent.Fn()));
    REQUIRE(sent.commands.size() == 1);
    REQUIRE(Over(AdvancePlayerCast(run, drawing, 102.0, sent.Fn())) == "deadline, hands never drawn");

    CastState shout = Shout(0);
    Sent sentShout;
    REQUIRE_FALSE(AdvancePlayerCast(shout, sheathed, 100.0, sentShout.Fn()));
    REQUIRE(sentShout.commands == std::vector<CastCommand>{CastCommand::Press});
}

TEST_CASE("the player's own doing holds the press, and the engine may refuse it", "[playercast]")
{
    CastState run = Spell();
    Sent sent;
    CastSeen busy = Free();
    busy.attacking = true;
    REQUIRE_FALSE(AdvancePlayerCast(run, busy, 100.0, sent.Fn()));
    busy.attacking = false;
    busy.blocking = true;
    REQUIRE_FALSE(AdvancePlayerCast(run, busy, 100.5, sent.Fn()));
    busy.blocking = false;
    busy.buttonHeld = true;
    REQUIRE_FALSE(AdvancePlayerCast(run, busy, 101.0, sent.Fn()));
    REQUIRE(sent.commands.empty());
    REQUIRE(Over(AdvancePlayerCast(run, busy, 102.0, sent.Fn())) == "deadline, hands never free");

    // The voice does not mind the attack buttons.
    CastState power = Power();
    Sent sentPower;
    REQUIRE_FALSE(AdvancePlayerCast(power, busy, 100.0, sentPower.Fn()));
    REQUIRE(sentPower.Last() == CastCommand::Press);

    // A dual cast waits for every caster.
    CastState dual = Spell();
    dual.dual = true;
    Sent sentDual;
    CastSeen others = Free();
    others.othersIdle = false;
    REQUIRE_FALSE(AdvancePlayerCast(dual, others, 100.0, sentDual.Fn()));
    REQUIRE(sentDual.commands.empty());
    REQUIRE_FALSE(AdvancePlayerCast(dual, Free(), 100.1, sentDual.Fn()));
    REQUIRE(sentDual.Last() == CastCommand::Press);
    // And its single press is not replayed.
    REQUIRE_FALSE(AdvancePlayerCast(dual, Free(), 100.2, sentDual.Fn()));
    REQUIRE(sentDual.commands.size() == 1);

    CastState refused = Spell();
    Sent sentRefused;
    CastSeen no = Free();
    no.refusal = "not enough magicka";
    REQUIRE(Over(AdvancePlayerCast(refused, no, 100.0, sentRefused.Fn())) == "refused");
    REQUIRE(refused.reason == "the engine refuses it: not enough magicka");
    REQUIRE(sentRefused.commands.empty());
}

TEST_CASE("a charge that never readies, or is cut short", "[playercast]")
{
    CastState run = Spell();
    run.chargeTime = 1.5f;
    Sent sent;
    REQUIRE_FALSE(AdvancePlayerCast(run, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(run, Charging(), 103.4, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(run, Charging(), 103.5, sent.Fn())) == "deadline, never ready");

    CastState cut = Spell();
    REQUIRE_FALSE(AdvancePlayerCast(cut, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(cut, Charging(), 100.2, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(cut, Free(), 100.4, sent.Fn())) == "the charge was cut short");
    // Idle before anything was seen is not cut short: the press has not
    // taken yet.
    CastState notYet = Spell();
    REQUIRE_FALSE(AdvancePlayerCast(notYet, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(notYet, Free(), 100.2, sent.Fn()));
}

TEST_CASE("a stream is held from when it starts, for its sustain, and its end is its own", "[playercast]")
{
    CastState run = Spell();
    run.sustained = true;
    run.sustain = 2.0f;
    Sent sent;
    REQUIRE_FALSE(AdvancePlayerCast(run, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(run, Casting(), 100.5, sent.Fn()));
    REQUIRE(run.step == CastStep::Holding);
    REQUIRE(run.readyAt == 100.5);
    REQUIRE_FALSE(run.released);

    CastSeen streaming = Casting();
    streaming.fireSeen = true;
    REQUIRE_FALSE(AdvancePlayerCast(run, streaming, 100.6, sent.Fn()));
    REQUIRE(run.firedAt == 100.6);
    REQUIRE_FALSE(AdvancePlayerCast(run, streaming, 102.4, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::Press);
    REQUIRE_FALSE(AdvancePlayerCast(run, streaming, 102.5, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::Release);
    REQUIRE(run.fired);
    REQUIRE(run.step == CastStep::Restoring);
    REQUIRE(Over(AdvancePlayerCast(run, Free(), 102.7, sent.Fn())) == "stream ended");

    // The stream ending on its own: early if it fired, cut short if not.
    CastState early = Spell();
    early.sustained = true;
    early.sustain = 2.0f;
    REQUIRE_FALSE(AdvancePlayerCast(early, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(early, Casting(), 100.5, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(early, streaming, 100.6, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(early, Free(), 101.0, sent.Fn())) == "stream ended early");
    REQUIRE(early.fired);
    CastState cut = Spell();
    cut.sustained = true;
    cut.sustain = 2.0f;
    REQUIRE_FALSE(AdvancePlayerCast(cut, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(cut, Casting(), 100.5, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(cut, Free(), 101.0, sent.Fn())) == "the stream was cut short");
    REQUIRE_FALSE(cut.fired);
}

TEST_CASE("a shout holds the control for its words, or taps for one; a power is a tap", "[playercast]")
{
    CastState shout = Shout(2);
    Sent sent;
    REQUIRE_FALSE(AdvancePlayerCast(shout, Free(), 100.0, sent.Fn()));
    CastSeen words = Free();
    words.wordsCharged = 0;
    REQUIRE_FALSE(AdvancePlayerCast(shout, words, 100.1, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::HoldPress);
    words.wordsCharged = 1;
    REQUIRE_FALSE(AdvancePlayerCast(shout, words, 100.6, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::HoldPress);
    words.wordsCharged = 2;
    REQUIRE_FALSE(AdvancePlayerCast(shout, words, 101.1, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::Release);
    REQUIRE(shout.wordsHeld == 2);
    REQUIRE(shout.step == CastStep::Firing);

    // The hold has a limit: released with what is charged.
    CastState slow = Shout(2);
    Sent sentSlow;
    REQUIRE_FALSE(AdvancePlayerCast(slow, Free(), 100.0, sentSlow.Fn()));
    words.wordsCharged = 1;
    REQUIRE_FALSE(AdvancePlayerCast(slow, words, 102.9, sentSlow.Fn()));
    REQUIRE(sentSlow.Last() == CastCommand::HoldPress);
    REQUIRE_FALSE(AdvancePlayerCast(slow, words, 103.0, sentSlow.Fn()));
    REQUIRE(sentSlow.Last() == CastCommand::Release);
    REQUIRE(slow.wordsHeld == 1);

    // One word: a tap.
    CastState one = Shout(0);
    Sent sentOne;
    REQUIRE_FALSE(AdvancePlayerCast(one, Free(), 100.0, sentOne.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(one, words, 100.05, sentOne.Fn()));
    REQUIRE(sentOne.Last() == CastCommand::Release);

    // The voice's fire event, and the voice caster idle meanwhile is not
    // given up on. On the fire tick the run goes straight on to the
    // restore, and with the caster idle it is over that tick.
    CastSeen idleVoice = Free();
    REQUIRE_FALSE(AdvancePlayerCast(one, idleVoice, 100.5, sentOne.Fn()));
    CastSeen voiceFired = Free();
    voiceFired.fireSeen = true;
    REQUIRE(Over(AdvancePlayerCast(one, voiceFired, 100.6, sentOne.Fn())) == "shout fired");
    REQUIRE(one.fired);
    REQUIRE(sentOne.Last() == CastCommand::MarkPowerUsed);
}

TEST_CASE("a power fires with nothing to see but the used list taking it", "[playercast]")
{
    CastState power = Power();
    Sent sent;
    REQUIRE_FALSE(AdvancePlayerCast(power, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(power.usedBefore);
    REQUIRE_FALSE(AdvancePlayerCast(power, Free(), 100.05, sent.Fn()));
    REQUIRE(sent.Last() == CastCommand::Release);
    CastSeen landed = Free();
    landed.onUsedList = true;
    REQUIRE(Over(AdvancePlayerCast(power, landed, 100.1, sent.Fn())) == "power cast");
    REQUIRE(power.fired);
    REQUIRE(sent.Last() == CastCommand::MarkPowerUsed);

    // Already on the list before the press: the list says nothing, and the
    // deadline names it.
    CastState used = Power();
    Sent sentUsed;
    REQUIRE_FALSE(AdvancePlayerCast(used, landed, 100.0, sentUsed.Fn()));
    REQUIRE(used.usedBefore);
    REQUIRE_FALSE(AdvancePlayerCast(used, landed, 100.05, sentUsed.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(used, landed, 101.0, sentUsed.Fn()));
    REQUIRE(Over(AdvancePlayerCast(used, landed, 102.05, sentUsed.Fn())) == "released, never fired");
}

TEST_CASE("released and nothing fired: ended without firing, or never fired", "[playercast]")
{
    CastState run = Spell();
    Sent sent;
    REQUIRE_FALSE(AdvancePlayerCast(run, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(run, Ready(), 100.5, sent.Fn()));
    // Idle again within the grace: not given up on yet.
    REQUIRE_FALSE(AdvancePlayerCast(run, Free(), 100.55, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(run, Free(), 100.65, sent.Fn())) == "released, ended without firing");

    CastState never = Spell();
    REQUIRE_FALSE(AdvancePlayerCast(never, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(never, Ready(), 100.5, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(never, Charging(5), 102.4, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(never, Charging(5), 102.5, sent.Fn())) == "released, never fired");

    // Restoring waits for the settle at most.
    CastState fired = Spell();
    REQUIRE_FALSE(AdvancePlayerCast(fired, Free(), 100.0, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(fired, Ready(), 100.5, sent.Fn()));
    CastSeen seen = Charging(5);
    seen.fireSeen = true;
    REQUIRE_FALSE(AdvancePlayerCast(fired, seen, 100.6, sent.Fn()));
    REQUIRE_FALSE(AdvancePlayerCast(fired, Charging(5), 102.5, sent.Fn()));
    REQUIRE(Over(AdvancePlayerCast(fired, Charging(5), 102.6, sent.Fn())) == "spell fired");

    CastSeen gone;
    gone.player = false;
    REQUIRE(Over(AdvancePlayerCast(fired, gone, 103.0, sent.Fn())) == "player vanished");
}

TEST_CASE("each step has a name for the log", "[playercast]")
{
    REQUIRE(std::string(ToString(CastStep::Lending)) == "lending");
    REQUIRE(std::string(ToString(CastStep::Drawing)) == "drawing");
    REQUIRE(std::string(ToString(CastStep::Pressing)) == "pressing");
    REQUIRE(std::string(ToString(CastStep::Charging)) == "charging");
    REQUIRE(std::string(ToString(CastStep::Holding)) == "holding");
    REQUIRE(std::string(ToString(CastStep::Firing)) == "firing");
    REQUIRE(std::string(ToString(CastStep::Restoring)) == "restoring");
}

TEST_CASE("each borrowed hand gets back what it held, the left first", "[playercast]")
{
    HeldSlot left;
    left.lent = true;
    left.spell = 0x12FCD;
    HeldSlot right;
    right.lent = true;
    right.item = 0x13989;
    const auto steps = PlanRestore(left, right);
    REQUIRE(steps.size() == 2);
    REQUIRE(steps[0].left);
    REQUIRE(steps[0].what == RestoreWhat::Spell);
    REQUIRE(steps[0].form == 0x12FCD);
    REQUIRE_FALSE(steps[1].left);
    REQUIRE(steps[1].what == RestoreWhat::Item);
    REQUIRE(steps[1].form == 0x13989);

    // A hand that was not borrowed is not touched.
    HeldSlot untouched;
    REQUIRE(PlanRestore(left, untouched).size() == 1);
    REQUIRE(PlanRestore(untouched, untouched).empty());

    // A hand that held nothing keeps what it was lent.
    HeldSlot empty;
    empty.lent = true;
    const auto kept = PlanRestore(empty, untouched);
    REQUIRE(kept.size() == 1);
    REQUIRE(kept[0].what == RestoreWhat::KeepBorrowed);
}

TEST_CASE("a two-hander goes back once; two copies of one dagger both go back", "[playercast]")
{
    // The greatsword reads from both hands: one equip puts it back.
    HeldSlot left;
    left.lent = true;
    left.item = 0x1359D;
    left.twoHanded = true;
    HeldSlot right = left;
    const auto once = PlanRestore(left, right);
    REQUIRE(once.size() == 1);
    REQUIRE(once[0].left);

    // Two copies of one dagger are two weapons: each hand gets one back.
    // Comparing the form alone dropped the second (fixed 2026-09-19).
    HeldSlot leftDagger;
    leftDagger.lent = true;
    leftDagger.item = 0x1397E;
    HeldSlot rightDagger = leftDagger;
    const auto both = PlanRestore(leftDagger, rightDagger);
    REQUIRE(both.size() == 2);
    REQUIRE(both[0].left);
    REQUIRE_FALSE(both[1].left);
    REQUIRE(both[0].form == both[1].form);

    // A two-hander borrowed from one hand alone still goes back.
    HeldSlot none;
    const auto single = PlanRestore(none, right);
    REQUIRE(single.size() == 1);
    REQUIRE_FALSE(single[0].left);
}

TEST_CASE("the voice gets back what it held, or gives up what it was lent", "[playercast]")
{
    REQUIRE(PlanVoiceRestore(false, true, true) == VoiceRestore::None);
    REQUIRE(PlanVoiceRestore(true, true, true) == VoiceRestore::PutBack);
    REQUIRE(PlanVoiceRestore(true, true, false) == VoiceRestore::PutBack);
    // Nothing there before: the borrowed shout or power comes off, where a
    // hand would have kept it.
    REQUIRE(PlanVoiceRestore(true, false, true) == VoiceRestore::ReleaseShout);
    REQUIRE(PlanVoiceRestore(true, false, false) == VoiceRestore::ReleasePower);
}

TEST_CASE("the player's tactics are held for the first reason that holds", "[playercast]")
{
    HoldFacts free;
    REQUIRE(HeldBy(free) == HeldReason::None);
    REQUIRE(std::string(ToString(HeldReason::None)).empty());

    HoldFacts loading;
    loading.loaded = false;
    loading.inDialogue = true;
    REQUIRE(HeldBy(loading) == HeldReason::NotLoaded);

    // The order is the order asked: a player knocked down on a horse reads
    // as knocked down, and one in dialogue with the controls away reads as
    // in dialogue.
    HoldFacts both;
    both.knockedDown = true;
    both.mounted = true;
    REQUIRE(HeldBy(both) == HeldReason::KnockedDown);
    HoldFacts talking;
    talking.inDialogue = true;
    talking.controlsDisabled = true;
    REQUIRE(HeldBy(talking) == HeldReason::InDialogue);

    // Each on its own, and its wording.
    const std::vector<std::pair<bool HoldFacts::*, HeldReason>> each{
        {&HoldFacts::inDialogue, HeldReason::InDialogue},
        {&HoldFacts::controlsDisabled, HeldReason::ControlsDisabled},
        {&HoldFacts::inFurniture, HeldReason::InFurniture},
        {&HoldFacts::knockedDown, HeldReason::KnockedDown},
        {&HoldFacts::swimming, HeldReason::Swimming},
        {&HoldFacts::mounted, HeldReason::Mounted},
        {&HoldFacts::inKillMove, HeldReason::InKillMove},
        {&HoldFacts::beastForm, HeldReason::BeastForm}};
    for (const auto &[flag, reason] : each)
    {
        HoldFacts facts;
        facts.*flag = true;
        REQUIRE(HeldBy(facts) == reason);
        REQUIRE_FALSE(std::string(ToString(reason)).empty());
    }
}
