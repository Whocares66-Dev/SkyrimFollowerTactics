// Sequences from play, replayed: the graph's events as FollowerTactics.log
// has them, pasted in unchanged, recorded through the same watches the game
// opens (core/GraphEvents.h), read by the same Hear, and stepped through
// the same machines, when the game steps them: at the request, at each
// event a watch waits on, and on the turns between. What the log does not
// say -- the reads of the actor at each step -- is reconstructed beside
// each sequence from the events around it, and says so. The assertions are
// what the log reported at the end.
//
// Since 2026-09-25 each step logs its reads ("step at T: ..."), so a later
// log is a fixture without reconstruction.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Bash.h"
#include "core/Blows.h"
#include "core/GraphEvents.h"
#include "core/Lease.h"
#include "core/PlayerCast.h"
#include "core/Strike.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ft;
using Catch::Approx;

namespace
{

constexpr std::uint32_t kPlayer = 0x00000014;
constexpr std::uint32_t kJenassa = 0x000E1BA9;
constexpr std::uint32_t kSerana = 0x02002B74;

// "HH:MM:SS.mmm" as seconds of the day.
double SecondsOf(std::string_view clock)
{
    const auto number = [&](std::size_t at, std::size_t length) {
        double value = 0.0;
        for (std::size_t i = at; i < at + length; ++i)
            value = value * 10.0 + (clock[i] - '0');
        return value;
    };
    return number(0, 2) * 3600.0 + number(3, 2) * 60.0 + number(6, 2) + number(9, 3) / 1000.0;
}

std::uint32_t HexOf(std::string_view text)
{
    std::uint32_t value = 0;
    for (const char c : text)
    {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0)
            break;
        value = value * 16 + static_cast<std::uint32_t>(digit);
    }
    return value;
}

bool Starts(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

struct Event
{
    double at{0.0};
    std::uint32_t actor{0};
    GraphTag tag{GraphTag::AttackStop};
    std::uint32_t form{0};
};

struct Line
{
    double at{0.0};
    std::string_view text; // after the module
};

// "[HH:MM:SS.mmm] [thread] [L] [module] text", in the file's order.
std::vector<Line> LinesOf(std::string_view log)
{
    std::vector<Line> lines;
    while (!log.empty())
    {
        const std::size_t end = log.find('\n');
        const std::string_view line = log.substr(0, end);
        log = end == std::string_view::npos ? std::string_view{} : log.substr(end + 1);
        if (line.size() < 15 || line[0] != '[')
            continue;
        std::size_t text = 0;
        for (int i = 0; i < 4 && text != std::string_view::npos; ++i)
            text = line.find(']', text + 1);
        if (text == std::string_view::npos)
            continue;
        std::string_view rest = line.substr(text + 1);
        rest.remove_prefix((std::min)(rest.find_first_not_of(' '), rest.size()));
        lines.push_back({SecondsOf(line.substr(1, 12)), rest});
    }
    return lines;
}

// An event: "anim XXXXXXXX: tag" for an actor's, "anim: tag" for the
// player's before 2026-09-25; a fire from its own line, which says what
// fired ("[the ]left hand fired XXXXXXXX", "[the ]voice fired: shout
// XXXXXXXX"), its bare tag skipped. What is not recorded is skipped, as the
// sink skips it.
std::optional<Event> EventOf(const Line &line)
{
    std::string_view rest = line.text;
    if (!Starts(rest, "anim"))
        return std::nullopt;
    rest.remove_prefix(4);
    std::uint32_t actor = kPlayer;
    if (Starts(rest, " "))
    {
        actor = HexOf(rest.substr(1, 8));
        rest.remove_prefix((std::min)(rest.size(), std::size_t{9}));
    }
    if (!Starts(rest, ": "))
        return std::nullopt;
    rest.remove_prefix(2);
    if (Starts(rest, "the "))
        rest.remove_prefix(4);
    Event event{line.at, actor};
    if (Starts(rest, "left hand fired ") || Starts(rest, "right hand fired "))
    {
        event.tag = Starts(rest, "left") ? GraphTag::SpellFireLeft : GraphTag::SpellFireRight;
        event.form = HexOf(rest.substr(rest.find("fired ") + 6));
        return event;
    }
    if (Starts(rest, "voice fired: shout "))
    {
        event.tag = GraphTag::SpellFireVoice;
        event.form = HexOf(rest.substr(19));
        return event;
    }
    const auto tag = GraphTagOf(rest);
    if (!tag || IsFire(*tag))
        return std::nullopt;
    event.tag = *tag;
    return event;
}

std::vector<Event> EventsOf(std::string_view log)
{
    std::vector<Event> events;
    for (const Line &line : LinesOf(log))
        if (const auto event = EventOf(line))
            events.push_back(*event);
    return events;
}

// The game's order: the events before the request go by unheard; the
// request's watch opens and it takes its first step; then each event is
// recorded and, where a watch waits on it, steps; the turns between come
// every `period` from the request. `step` answers whether the request is
// over. The watches outlive the watch the caller opens in them.
void Drive(GraphWatches &watches, const std::vector<Event> &events, double requestAt, double period, double until,
           const std::function<void()> &open, const std::function<bool(double)> &step,
           const std::function<void(const Event &)> &recorded = {})
{
    std::size_t i = 0;
    const auto record = [&](const Event &event) {
        const Recorded heard = watches.Record(event.actor, event.tag, event.form);
        if (recorded)
            recorded(event);
        return heard;
    };
    for (; i < events.size() && events[i].at < requestAt; ++i)
        record(events[i]);
    open();
    if (step(requestAt))
        return;
    // Counted, not summed: the k-th turn is exactly k periods on.
    int turns = 1;
    const auto turn = [&] { return requestAt + static_cast<double>(turns) * period; };
    for (; i < events.size(); ++i)
    {
        for (; turn() < events[i].at; ++turns)
            if (step(turn()))
                return;
        if (record(events[i]).wakes && step(events[i].at))
            return;
    }
    for (; turn() <= until; ++turns)
        if (step(turn()))
            return;
}

// When a reading is true: in [from, to).
bool Between(double t, double from, double to)
{
    return t >= from && t < to;
}

struct StrikeOutcome
{
    std::string reason;
    StrikeState state;
    std::vector<double> asked; // when the action was asked for
};

StrikeOutcome ReplayStrike(const std::vector<Event> &events, double requestAt,
                           const std::function<StrikeSeen(double)> &reads)
{
    StrikeOutcome out;
    GraphWatches watches;
    GraphWatch watch;
    out.state = RequestStrikeAt(requestAt);
    Drive(
        watches, events, requestAt, 0.5, requestAt + 6.0, [&] { watch = watches.Open(kJenassa, kBlowWakes); },
        [&](double now) {
            StrikeSeen seen = reads(now);
            Hear(seen, watch.HeardSoFar());
            const char *over = AdvanceStrike(out.state, seen, now, [&] {
                out.asked.push_back(now);
                return true;
            });
            if (over)
                out.reason = over;
            return over != nullptr;
        });
    return out;
}

} // namespace

// ---- Jenassa's power attacks, 2026-09-25 00:32, sword and shield, a cave bear

// Taken on the request's own step, and followed to its hit and its end.
constexpr std::string_view kTakenAtOnce = R"(
[00:32:12.764] [16540] [D] [blows]    Jenassa (000E1BA9): power attack attackPowerStartInPlace requested
[00:32:12.764] [16540] [D] [blows]    Jenassa (000E1BA9): the right attack action carrying attackPowerStartInPlace taken 0.00 s after the request
[00:32:12.780] [55824] [D] [blows]    anim 000E1BA9: PowerAttack_Start_end
[00:32:12.780] [55824] [D] [blows]    anim 000E1BA9: HeadTrackingOff
[00:32:12.780] [55824] [D] [blows]    anim 000E1BA9: InterruptCast
[00:32:12.813] [16540] [D] [blows]    anim 000E1BA9: blockStop
[00:32:13.014] [55616] [D] [blows]    anim 000E1BA9: slowdownStart
[00:32:13.097] [55824] [D] [blows]    anim 000E1BA9: 00NextClip
[00:32:13.097] [55824] [D] [blows]    anim 000E1BA9: slowdownStart
[00:32:13.163] [55616] [D] [blows]    anim 000E1BA9: preHitFrame
[00:32:13.263] [45852] [D] [blows]    anim 000E1BA9: weaponSwing
[00:32:13.363] [16540] [D] [blows]    anim 000E1BA9: HitFrame
[00:32:13.764] [36176] [D] [blows]    anim 000E1BA9: 00NextClip
[00:32:13.764] [36176] [D] [blows]    anim 000E1BA9: AttackWinStart
[00:32:13.781] [55616] [D] [blows]    anim 000E1BA9: AttackWinStart
[00:32:14.131] [36176] [D] [blows]    anim 000E1BA9: attackStop
[00:32:14.131] [36176] [D] [blows]    anim 000E1BA9: AttackWinEnd
[00:32:14.131] [36176] [D] [blows]    anim 000E1BA9: HeadTrackingOn
[00:32:14.131] [36176] [D] [blows]    anim 000E1BA9: InterruptCast
[00:32:14.131] [36176] [D] [blows]    anim 000E1BA9: tailCombatIdle
[00:32:14.132] [45852] [I] [blows]    Jenassa rule 1 "": power attack made -- power attack made, after 1.37 s (attackPowerStartInPlace, taken at 0.00 s, hit at 0.60 s, refused 0; stamina 85 -> 29; 1 -> 1 deg off the strike)
)";

TEST_CASE("replayed: a power attack taken at once is made at its hit and over at its attackStop", "[replay]")
{
    const auto events = EventsOf(kTakenAtOnce);
    REQUIRE(events.size() == 8);
    const double request = SecondsOf("00:32:12.764");
    // Reconstructed: drawn, free and in front at the request (it was
    // taken on that step, 1 degree off); the attack state from the power
    // attack's start to its attackStop.
    const auto reads = [](double t) {
        StrikeSeen seen;
        seen.weaponDrawn = true;
        seen.facing = true;
        seen.attacking = Between(t, SecondsOf("00:32:12.780"), SecondsOf("00:32:14.131"));
        return seen;
    };
    const StrikeOutcome out = ReplayStrike(events, request, reads);
    REQUIRE(out.reason == "power attack made");
    REQUIRE(out.asked.size() == 1);
    REQUIRE(out.state.sentAt - request == Approx(0.0));
    REQUIRE(out.state.hitAt - request == Approx(0.60).margin(0.01));
    REQUIRE(out.state.endAt - request == Approx(1.37).margin(0.01));
    REQUIRE_FALSE(out.state.waited);
    REQUIRE(out.state.refusals == 0);
}

// Asked for mid-swing: two hit frames of the swing already under way go by
// before the action, and do not count as its hit.
constexpr std::string_view kAfterAWait = R"(
[00:32:05.659] [55616] [D] [blows]    Jenassa (000E1BA9): power attack attackPowerStartInPlace requested
[00:32:05.675] [55616] [D] [blows]    anim 000E1BA9: weaponSwing
[00:32:05.708] [50876] [D] [blows]    anim 000E1BA9: HitFrame
[00:32:05.908] [16540] [D] [blows]    anim 000E1BA9: CastOKStart
[00:32:06.176] [36176] [D] [tactics]  Jenassa (000E1BA9) rule 1 "" [power-attack]: busy, skipped this evaluation
[00:32:06.208] [16540] [D] [blows]    anim 000E1BA9: AttackWinStart
[00:32:06.241] [45852] [D] [blows]    anim 000E1BA9: CastOKStop
[00:32:06.458] [50876] [D] [blows]    anim 000E1BA9: preHitFrame
[00:32:06.559] [50876] [D] [blows]    anim 000E1BA9: weaponSwing
[00:32:06.592] [36176] [D] [blows]    anim 000E1BA9: HitFrame
[00:32:06.792] [36176] [D] [blows]    anim 000E1BA9: CastOKStart
[00:32:07.092] [36176] [D] [blows]    anim 000E1BA9: AttackWinStartLeft
[00:32:07.492] [55824] [D] [blows]    anim 000E1BA9: AttackWinEndLeft
[00:32:07.509] [50876] [D] [blows]    anim 000E1BA9: attackStop
[00:32:07.509] [50876] [D] [blows]    anim 000E1BA9: HeadTrackingOn
[00:32:07.509] [50876] [D] [blows]    anim 000E1BA9: CastOKStop
[00:32:07.509] [50876] [D] [blows]    anim 000E1BA9: tailCombatState
[00:32:07.509] [50876] [D] [blows]    anim 000E1BA9: tailCombatIdle
[00:32:07.510] [45852] [D] [blows]    Jenassa (000E1BA9): the right attack action carrying attackPowerStartInPlace taken 1.86 s after the request
[00:32:07.526] [50876] [D] [blows]    anim 000E1BA9: PowerAttack_Start_end
[00:32:07.526] [50876] [D] [blows]    anim 000E1BA9: HeadTrackingOff
[00:32:07.526] [50876] [D] [blows]    anim 000E1BA9: CastOKStop
[00:32:07.526] [50876] [D] [blows]    anim 000E1BA9: InterruptCast
[00:32:07.776] [55824] [D] [blows]    anim 000E1BA9: slowdownStart
[00:32:07.859] [55824] [D] [blows]    anim 000E1BA9: 00NextClip
[00:32:07.859] [55824] [D] [blows]    anim 000E1BA9: slowdownStart
[00:32:07.926] [36176] [D] [blows]    anim 000E1BA9: preHitFrame
[00:32:08.026] [36176] [D] [blows]    anim 000E1BA9: weaponSwing
[00:32:08.126] [50876] [D] [blows]    anim 000E1BA9: HitFrame
[00:32:08.527] [36176] [D] [blows]    anim 000E1BA9: 00NextClip
[00:32:08.527] [36176] [D] [blows]    anim 000E1BA9: AttackWinStart
[00:32:08.543] [36176] [D] [blows]    anim 000E1BA9: AttackWinStart
[00:32:08.893] [55824] [D] [blows]    anim 000E1BA9: attackStop
[00:32:08.893] [55824] [D] [blows]    anim 000E1BA9: AttackWinEnd
[00:32:08.893] [55824] [D] [blows]    anim 000E1BA9: HeadTrackingOn
[00:32:08.893] [55824] [D] [blows]    anim 000E1BA9: tailCombatState
[00:32:08.893] [55824] [D] [blows]    anim 000E1BA9: tailCombatIdle
[00:32:08.894] [55824] [I] [blows]    Jenassa rule 1 "": power attack made -- power attack made, after 3.24 s (attackPowerStartInPlace, after a wait, taken at 1.86 s, hit at 2.47 s, refused 0; stamina 95 -> 37; 10 -> 2 deg off the strike)
)";

TEST_CASE("replayed: asked mid-swing, taken at the swing's attackStop; the swing's own hits are not its hit",
          "[replay]")
{
    const auto events = EventsOf(kAfterAWait);
    const double request = SecondsOf("00:32:05.659");
    // Reconstructed: their own swing under way at the request, to its
    // attackStop; ours from its start to its own.
    const auto reads = [](double t) {
        StrikeSeen seen;
        seen.weaponDrawn = true;
        seen.facing = true;
        seen.attacking =
            t < SecondsOf("00:32:07.509") || Between(t, SecondsOf("00:32:07.526"), SecondsOf("00:32:08.893"));
        return seen;
    };
    const StrikeOutcome out = ReplayStrike(events, request, reads);
    REQUIRE(out.reason == "power attack made");
    REQUIRE(out.state.waited);
    REQUIRE(out.asked.size() == 1);
    REQUIRE(out.state.sentAt - request == Approx(1.86).margin(0.015));
    // Two HitFrames came before the action: the hit is the third.
    REQUIRE(out.state.hitAt - request == Approx(2.47).margin(0.01));
    REQUIRE(out.state.endAt - request == Approx(3.24).margin(0.01));
}

// Asked for while their own shout was going off: in play the action was
// turned away twice (the step did not yet wait out a shout), and the
// request ran out of time with the target to one side.
constexpr std::string_view kDuringAShout = R"(
[00:32:02.123] [55616] [D] [blows]    Jenassa (000E1BA9): power attack attackPowerStartInPlace requested
[00:32:02.205] [55616] [D] [blows]    anim 000E1BA9: BeginCastVoice
[00:32:02.388] [55616] [D] [blows]    anim 000E1BA9: FootRight
[00:32:02.639] [55616] [D] [blows]    Jenassa (000E1BA9): the right attack action carrying attackPowerStartInPlace turned away 0.52 s after the request
[00:32:02.789] [45852] [D] [blows]    anim 000E1BA9: FootLeft
[00:32:03.106] [50876] [D] [blows]    anim 000E1BA9: Voice_SpellFire_Event
[00:32:03.140] [45852] [D] [blows]    Jenassa (000E1BA9): the right attack action carrying attackPowerStartInPlace turned away 1.02 s after the request
[00:32:04.073] [55616] [D] [blows]    anim 000E1BA9: shoutStop
[00:32:04.073] [55616] [D] [blows]    anim 000E1BA9: tailCombatState
[00:32:04.073] [55616] [D] [blows]    anim 000E1BA9: attackStop
[00:32:04.073] [55616] [D] [blows]    anim 000E1BA9: tailCombatLocomotion
[00:32:04.106] [45852] [D] [blows]    anim 000E1BA9: FootLeft
[00:32:04.157] [16540] [I] [blows]    Jenassa rule 1 "": power attack not made -- deadline, target never in front, after 2.04 s (attackPowerStartInPlace, after a wait, taken at -1.00 s, hit at -1.00 s, refused 2; stamina 95 -> 95; 142 -> 61 deg off the strike)
)";

TEST_CASE("replayed: during their own shout the action is not asked for; after it, as the target stands", "[replay]")
{
    const auto events = EventsOf(kDuringAShout);
    const double request = SecondsOf("00:32:02.123");
    const double shoutOver = SecondsOf("00:32:04.073");
    // Reconstructed: the shout from its BeginCastVoice to its shoutStop.
    // The target was 142 degrees round at the request and 61 at the end,
    // and in front at the two steps that asked (the old step asked only
    // in front); where it stood at the shout's end the log does not say,
    // so both ways.
    for (const bool inFrontAtTheEnd : {false, true})
    {
        const auto reads = [&](double t) {
            StrikeSeen seen;
            seen.weaponDrawn = true;
            seen.casting = Between(t, SecondsOf("00:32:02.205"), shoutOver);
            seen.facing =
                Between(t, SecondsOf("00:32:02.600"), SecondsOf("00:32:03.500")) || (inFrontAtTheEnd && t >= shoutOver);
            return seen;
        };
        const StrikeOutcome out = ReplayStrike(events, request, reads);
        // Not once during the shout.
        for (const double asked : out.asked)
            REQUIRE(asked >= shoutOver);
        if (inFrontAtTheEnd)
        {
            REQUIRE(out.asked.size() == 1);
            REQUIRE(out.state.sentAt == Approx(shoutOver));
            REQUIRE(out.state.step == StrikeStep::Striking);
        }
        else
        {
            REQUIRE(out.reason == "deadline, target never in front");
            REQUIRE(out.asked.empty());
            REQUIRE(out.state.refusals == 0);
        }
    }
}

// ---- Jenassa's power bash, 2026-09-25 00:34

constexpr std::string_view kPowerBash = R"(
[00:34:56.766] [36176] [D] [blows]    Jenassa (000E1BA9): power bash requested
[00:34:56.780] [55824] [D] [blows]    anim 000E1BA9: InterruptCast
[00:34:56.780] [55824] [D] [blows]    anim 000E1BA9: tailCombatIdle
[00:34:56.780] [55824] [D] [blows]    anim 000E1BA9: SoundPlay.NPCHumanCombatShieldBlock
[00:34:56.780] [55824] [D] [blows]    anim 000E1BA9: blockStartOut
[00:34:56.782] [50876] [D] [blows]    Jenassa (000E1BA9): the right attack action carrying bashPowerStart taken 0.02 s after the request
[00:34:56.797] [16540] [D] [blows]    anim 000E1BA9: HeadTrackingOff
[00:34:56.797] [16540] [D] [blows]    anim 000E1BA9: tailCombatIdle
[00:34:56.797] [16540] [D] [blows]    anim 000E1BA9: SoundPlay.NPCHumanCombatShieldBashPower
[00:34:56.830] [16540] [D] [blows]    anim 000E1BA9: blockStop
[00:34:56.847] [36176] [D] [blows]    anim 000E1BA9: FootScuffLeft
[00:34:56.981] [55616] [D] [blows]    anim 000E1BA9: preHitFrame
[00:34:57.080] [50876] [D] [blows]    anim 000E1BA9: HitFrame
[00:34:57.114] [36176] [D] [blows]    anim 000E1BA9: InitiateWinBegin
[00:34:57.214] [16540] [D] [blows]    anim 000E1BA9: FootScuffLeft
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: bashExit
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: FootScuffRight
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: InitiateWinEnd
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: bashStop
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: HeadTrackingOn
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: InterruptCast
[00:34:57.347] [45852] [D] [blows]    anim 000E1BA9: tailCombatIdle
[00:34:57.348] [45852] [D] [blows]    Jenassa (000E1BA9): block lowered
[00:34:57.348] [45852] [I] [blows]    Jenassa rule 1 "": power bash made -- bash made, after 0.58 s (block raised, up at 0.02 s, steady at 0.02 s, taken at 0.02 s after a wait, refused 0 block + 0 bash; bash state 0.52 s, stamina 95 -> 40)
)";

TEST_CASE("replayed: a power bash raises the block, is taken at its blockStartOut and made at its bashStop", "[replay]")
{
    const auto events = EventsOf(kPowerBash);
    const double request = SecondsOf("00:34:56.766");
    // Reconstructed: free and not blocking at the request (the request
    // raised the block); blocking from its blockStartOut to its blockStop;
    // the bash's attack state from the action to its bashStop.
    const auto reads = [](double t) {
        BashSeen seen;
        seen.weaponDrawn = true;
        seen.blocking = Between(t, SecondsOf("00:34:56.780"), SecondsOf("00:34:56.830"));
        const bool bashing = Between(t, SecondsOf("00:34:56.782"), SecondsOf("00:34:57.347"));
        seen.attack = bashing ? BashSeen::Attack::Bash : BashSeen::Attack::None;
        seen.attackState = bashing ? 6 : 0;
        return seen;
    };
    GraphWatches watches;
    GraphWatch watch;
    BashState run = RequestBashAt(request, true);
    std::vector<BashCommand> asked;
    std::string reason;
    Drive(
        watches, events, request, 0.5, request + 4.0, [&] { watch = watches.Open(kJenassa, kBlowWakes); },
        [&](double now) {
            BashSeen seen = reads(now);
            Hear(seen, watch.HeardSoFar());
            const char *over = AdvanceBash(run, seen, now, [&](BashCommand command) {
                asked.push_back(command);
                return true;
            });
            if (over)
                reason = over;
            return over != nullptr;
        });
    REQUIRE(reason == "bash made");
    REQUIRE(asked == std::vector<BashCommand>{BashCommand::RaiseBlock, BashCommand::Bash});
    REQUIRE(run.raised);
    REQUIRE(run.waited);
    REQUIRE(run.blockUpAt - request == Approx(0.02).margin(0.01));
    REQUIRE(run.sentAt - request == Approx(0.02).margin(0.01));
    REQUIRE(run.bashEnd - request == Approx(0.58).margin(0.01));
    REQUIRE(run.bashEnd - run.bashFrom == Approx(0.52).margin(0.01));
    REQUIRE(run.blockRefusals == 0);
    REQUIRE(run.bashRefusals == 0);
}

// ---- The player's Windrunner, dual cast, 2026-09-25 23:43

namespace
{

struct CastOutcome
{
    std::string reason;
    CastState state;
};

// The player's cast as the game steps it: the request's step, the equip's
// InterruptCast while it lends, and the fast tick every 50 ms. Reconstructed
// reads: the hands drawn and free; the spell in the hands from `placedAt`;
// after the press, the caster charging until `readyAt`, Ready until the
// release, casting until the fire, idle after.
CastOutcome ReplayDualCast(const std::vector<Event> &events, double requestAt, double placedAt, double readyAt,
                           double firedAt)
{
    constexpr std::uint32_t kWindrunner = 0x3A10D265;
    CastOutcome out;
    out.state.dual = true;
    out.state.requestedAt = out.state.stepAt = requestAt;
    out.state.chargeTime = 1.5f;
    GraphWatches watches;
    GraphWatch watch;
    Drive(
        watches, events, requestAt, 0.05, requestAt + 4.0,
        [&] { watch = watches.Open(kPlayer, CastWakes(out.state.step), CastOwnFires(false, Hand::Both, kWindrunner)); },
        [&](double now) {
            CastSeen seen;
            seen.placed = now >= placedAt;
            seen.weapon = CastSeen::Weapon::Drawn;
            const CastState &run = out.state;
            if (!run.pressed || now >= firedAt + 0.02)
                seen.casterIdle = true;
            else if (now < readyAt)
                seen.caster = CastSeen::Caster::Casting;
            else if (!run.released)
                seen.caster = CastSeen::Caster::Ready;
            else
                seen.caster = CastSeen::Caster::Other;
            seen.casterHasSpell = run.pressed && !seen.casterIdle;
            seen.casterState = seen.casterIdle ? 0 : seen.caster == CastSeen::Caster::Ready ? 4 : 2;
            Hear(seen, watch.HeardSoFar());
            const char *over = AdvancePlayerCast(out.state, seen, now, [](CastCommand) {});
            if (over)
            {
                out.reason = over;
                return true;
            }
            watch.SetWakes(CastWakes(out.state.step));
            return false;
        });
    return out;
}

} // namespace

// From spells in both hands: the equip's InterruptCast 5 ms after the lend.
constexpr std::string_view kFromSpells = R"(
[23:43:05.156] [54544] [D] [player]   Goldilocks (00000014): Windrunner requested from the both hand
[23:43:05.156] [54544] [D] [player]   Goldilocks (00000014): lending Windrunner the both hand (left held Flame Tempest, right held Flame Tempest)
[23:43:05.161] [43316] [D] [player]   anim: DisableBumper
[23:43:05.161] [43316] [D] [player]   anim: tailEquip
[23:43:05.161] [43316] [D] [player]   anim: weaponDraw
[23:43:05.161] [43316] [D] [player]   anim: InterruptCast
[23:43:05.161] [43316] [D] [player]   anim: arrowDetach
[23:43:05.173] [54544] [D] [player]   Goldilocks (00000014): pressed for Windrunner (both hand, dual)
[23:43:05.476] [54360] [D] [player]   anim: weaponDraw
[23:43:05.642] [54544] [D] [player]   anim: Magic_Equip_Out
[23:43:05.642] [54544] [D] [player]   anim: EnableBumper
[23:43:05.642] [54544] [D] [player]   anim: tailCombatIdle
[23:43:05.663] [53108] [D] [player]   anim: BeginCastLeft
[23:43:05.663] [53108] [D] [player]   anim: tailCombatIdle
[23:43:05.676] [43092] [D] [player]   anim: SprintStop
[23:43:06.193] [53108] [D] [player]   anim: MLh_PreChargeOut
[23:43:07.076] [53108] [D] [player]   anim: MLh_WinStart
[23:43:07.077] [53108] [D] [player]   anim: MLh_SpellFire_Event
[23:43:07.077] [53108] [D] [player]   anim: the left hand fired 3A10D265 "Windrunner"
[23:43:07.122] [43316] [D] [player]   Goldilocks (00000014): over while restoring -- spell fired
[23:43:07.124] [43316] [I] [player]   Goldilocks rule 0 "": cast cast -- spell fired, after 1.97 s (both hand; settled at 0.02 s, pressed at 0.02 s, ready at 1.57 s, released at 1.57 s, fired at 1.97 s; caster state reached 4; magicka 580 -> 390)
)";

// From bare hands: the InterruptCast 70 ms after the lend. A press before
// it was cut short by it (2026-09-24).
constexpr std::string_view kFromBareHands = R"(
[23:43:48.057] [54544] [D] [player]   Goldilocks (00000014): Windrunner requested from the both hand
[23:43:48.057] [54544] [D] [player]   Goldilocks (00000014): lending Windrunner the both hand (left held <unnamed>, right held <unnamed>)
[23:43:48.127] [43316] [D] [player]   anim: DisableBumper
[23:43:48.127] [43316] [D] [player]   anim: tailEquip
[23:43:48.127] [43316] [D] [player]   anim: weaponDraw
[23:43:48.127] [43316] [D] [player]   anim: InterruptCast
[23:43:48.127] [43316] [D] [player]   anim: arrowDetach
[23:43:48.138] [53108] [D] [player]   Goldilocks (00000014): pressed for Windrunner (both hand, dual)
[23:43:48.444] [43316] [D] [player]   anim: weaponDraw
[23:43:48.611] [53108] [D] [player]   anim: Magic_Equip_Out
[23:43:48.611] [53108] [D] [player]   anim: EnableBumper
[23:43:48.611] [53108] [D] [player]   anim: tailCombatIdle
[23:43:48.631] [53108] [D] [player]   anim: BeginCastLeft
[23:43:48.631] [53108] [D] [player]   anim: tailCombatIdle
[23:43:48.644] [43092] [D] [player]   anim: SprintStop
[23:43:49.161] [54360] [D] [player]   anim: MLh_PreChargeOut
[23:43:50.028] [54360] [D] [player]   anim: MLh_WinStart
[23:43:50.028] [54360] [D] [player]   anim: MLh_SpellFire_Event
[23:43:50.028] [54360] [D] [player]   anim: the left hand fired 3A10D265 "Windrunner"
[23:43:50.039] [43092] [D] [player]   Goldilocks (00000014): over while restoring -- spell fired
[23:43:50.041] [43092] [I] [player]   Goldilocks rule 0 "": cast cast -- spell fired, after 2.00 s (both hand; settled at 0.08 s, pressed at 0.08 s, ready at 1.63 s, released at 1.63 s, fired at 2.00 s; caster state reached 4; magicka 580 -> 390)
)";

TEST_CASE("replayed: the dual cast is pressed on the equip's InterruptCast, and fired on the left hand's fire",
          "[replay]")
{
    const auto spells = EventsOf(kFromSpells);
    // The raw MLh_SpellFire_Event is skipped; its fire line is the event.
    REQUIRE(spells.size() == 3);
    REQUIRE(spells[2].tag == GraphTag::SpellFireLeft);
    REQUIRE(spells[2].form == 0x3A10D265u);

    const double request = SecondsOf("23:43:05.156");
    const double interrupt = SecondsOf("23:43:05.161");
    const double fire = SecondsOf("23:43:07.077");
    const CastOutcome out = ReplayDualCast(spells, request, request + 0.004, request + 1.57, fire);
    REQUIRE(out.reason == "spell fired");
    REQUIRE(out.state.fired);
    // Settled, and pressed, on the InterruptCast's own step.
    REQUIRE(out.state.settledAt == Approx(interrupt));
    REQUIRE(out.state.pressedAt == Approx(interrupt));
    // Fired on the first fast tick after the left hand's fire.
    REQUIRE(out.state.firedAt >= fire);
    REQUIRE(out.state.firedAt - fire < 0.05 + 1e-9);
}

TEST_CASE("replayed: from bare hands the press waits the 70 ms for the equip's InterruptCast", "[replay]")
{
    const auto events = EventsOf(kFromBareHands);
    const double request = SecondsOf("23:43:48.057");
    const double interrupt = SecondsOf("23:43:48.127");
    const double fire = SecondsOf("23:43:50.028");
    // The spell shows in the hands on the next update, well before the
    // equip's animation starts.
    const CastOutcome out = ReplayDualCast(events, request, request + 0.01, request + 1.63, fire);
    REQUIRE(out.reason == "spell fired");
    // The fast ticks at +50 ms saw the spell placed and did not press.
    REQUIRE(out.state.settledAt == Approx(interrupt));
    REQUIRE(out.state.pressedAt == Approx(interrupt));
    REQUIRE(out.state.pressedAt - request == Approx(0.07).margin(0.001));
}

TEST_CASE("replayed: the bare-hands sequence without its InterruptCast presses only at the lend's deadline", "[replay]")
{
    auto events = EventsOf(kFromBareHands);
    std::erase_if(events, [](const Event &event) { return event.tag == GraphTag::InterruptCast; });
    const double request = SecondsOf("23:43:48.057");
    const CastOutcome out = ReplayDualCast(events, request, request + 0.01, request + 1.63 + 1.0, request + 2.9);
    REQUIRE(out.state.settledAt < 0.0);
    REQUIRE(out.state.pressedAt - request == Approx(kLendSeconds).margin(0.051));
}

// ---- Serana's Drain Life, a follower's stream, 2026-09-25 20:37

// The lease's lines as they were. Packages logged no bare events then, so
// the one event here is the fire; the test puts back the rest.
constexpr std::string_view kStream = R"(
[20:37:27.028] [30492] [I] [packages] Serana: FF3F0800 casts 02011F2C at 00053AE4 "Conqueror Sorcerer"
[20:37:28.030] [20940] [D] [packages] Serana began the cast on FF3F0800 after 1.0 s -- deadline stepped back
[20:37:28.273] [7636 ] [I] [packages] anim 02002B74: left hand fired 02011F2C "Drain Life" -- OURS
[20:37:32.084] [30492] [D] [packages] Serana stream started on FF3F0800 -- 3.0 s to run
[20:37:34.603] [30492] [I] [packages] Serana rule 2 "": spell Drain Life cast -- stream ended, after 4.0 s
)";

TEST_CASE("replayed: a follower's stream starts at its own fire, not another's, and ends at the CastStop after",
          "[replay]")
{
    auto events = EventsOf(kStream);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].actor == kSerana);
    REQUIRE(events[0].tag == GraphTag::SpellFireLeft);
    REQUIRE(events[0].form == 0x02011F2Cu);
    // Put back: the BeginCast the "began the cast" line reports; the
    // CastStop "stream ended" reports, 4.0 s after the arm on the lease's
    // clock (the game was paused 3.6 s of the 7.6 s the wall clock shows:
    // "stream started" was the first step after the fire, at 32.084).
    // Not from play: a CastStop and a fire of another spell from the other
    // hand before ours, which the watch must not take for the stream's.
    const auto at = [](const char *clock) { return SecondsOf(clock); };
    events.push_back({at("20:37:28.030"), kSerana, GraphTag::BeginCastLeft, 0});
    events.push_back({at("20:37:28.100"), kSerana, GraphTag::CastStop, 0});
    events.push_back({at("20:37:28.200"), kSerana, GraphTag::SpellFireRight, 0x0003B568});
    events.push_back({at("20:37:31.000"), kSerana, GraphTag::CastStop, 0});
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });

    const double armed = at("20:37:27.028");
    GraphWatches watches;
    GraphWatch watch;
    LeaseState lease = ArmLease(armed, kArmWindowSeconds, true, 3.0f);
    std::string reason;
    double streamingFrom = -1.0;
    double over = -1.0;
    Drive(
        watches, events, armed, 0.5, armed + 10.0,
        [&] { watch = watches.Open(kSerana, {}, LeaseOwnFires(0x02011F2C, 0)); },
        [&](double now) {
            LeaseSeen seen;
            seen.running = true;
            Hear(seen, watch.HeardSoFar());
            const LeaseStep step = AdvanceCast(lease, seen, LeaseKind::Spell, now);
            if (step.streamExtended)
                streamingFrom = now;
            if (step.finish)
            {
                reason = step.finish;
                over = now;
            }
            return step.finish != nullptr;
        });
    REQUIRE(reason == "stream ended");
    REQUIRE(lease.extended);
    // Extended on the first turn after its own fire, not after Flames.
    REQUIRE(streamingFrom > at("20:37:28.273"));
    REQUIRE(streamingFrom - at("20:37:28.273") <= 0.5);
    // Ended on the first turn after the CastStop that came after its fire;
    // the one before it was some other cast's.
    REQUIRE(over > at("20:37:31.000"));
    REQUIRE(over - armed == Approx(4.0).margin(0.5));
}

// ---- A log with step lines replays as it stands
//
// Each step's line gives what it read (ReadsOf), and each action's answer
// has a line of its own, so nothing is reconstructed: the machine is fed
// what the game fed it, and the watch, fed the log's events, is checked
// against the counts each step logged.

namespace
{

struct LoggedStep
{
    double now{0.0};
    std::vector<std::pair<std::string, std::string>> reads;

    [[nodiscard]] const std::string *Read(std::string_view key) const
    {
        const auto it = std::find_if(reads.begin(), reads.end(), [key](const auto &read) { return read.first == key; });
        return it == reads.end() ? nullptr : &it->second;
    }
    [[nodiscard]] int Int(std::string_view key) const
    {
        const std::string *value = Read(key);
        return value ? std::stoi(*value) : -1;
    }
    [[nodiscard]] bool Flag(std::string_view key) const
    {
        return Int(key) == 1;
    }
};

// "<kind> step at T: key=value ...".
std::optional<LoggedStep> StepOf(const Line &line, std::string_view kind)
{
    const std::string marker = std::string(kind) + " step at ";
    const std::size_t at = line.text.find(marker);
    if (at == std::string_view::npos)
        return std::nullopt;
    std::string_view rest = line.text.substr(at + marker.size());
    const std::size_t colon = rest.find(": ");
    if (colon == std::string_view::npos)
        return std::nullopt;
    LoggedStep step;
    step.now = std::stod(std::string(rest.substr(0, colon)));
    rest.remove_prefix(colon + 2);
    while (!rest.empty())
    {
        const std::size_t space = rest.find(' ');
        const std::string_view pair = rest.substr(0, space);
        if (const std::size_t eq = pair.find('='); eq != std::string_view::npos)
            step.reads.emplace_back(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    }
    return step;
}

// "... attack action ... taken ..." or "... turned away ...".
std::optional<bool> AnswerOf(const Line &line)
{
    if (line.text.find("attack action") == std::string_view::npos)
        return std::nullopt;
    if (line.text.find(" turned away ") != std::string_view::npos)
        return false;
    if (line.text.find(" taken ") != std::string_view::npos)
        return true;
    return std::nullopt;
}

struct LoggedReplay
{
    std::string reason;
    int steps{0};
    int asked{0};
    bool countsAgree{true}; // the watch, replayed, heard what each step logged
    bool answered{true};    // every action asked for has its answer in the log
};

using Advance = std::function<const char *(const LoggedStep &, const std::function<bool()> &)>;
using Agree = std::function<bool(const LoggedStep &, const Heard &)>;

// The watch opens with the first step, as the request's does just before
// its first step.
LoggedReplay ReplayLogged(std::string_view log, std::string_view kind,
                          const std::function<GraphWatch(GraphWatches &)> &open, const Advance &advance,
                          const Agree &agree)
{
    const auto lines = LinesOf(log);
    GraphWatches watches;
    GraphWatch watch;
    LoggedReplay out;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (const auto event = EventOf(lines[i]))
        {
            watches.Record(event->actor, event->tag, event->form);
            continue;
        }
        const auto step = StepOf(lines[i], kind);
        if (!step)
            continue;
        if (!watch.Open())
            watch = open(watches);
        ++out.steps;
        out.countsAgree = out.countsAgree && agree(*step, watch.HeardSoFar());
        std::size_t next = i + 1;
        const auto perform = [&] {
            ++out.asked;
            for (; next < lines.size() && !StepOf(lines[next], kind); ++next)
                if (const auto answer = AnswerOf(lines[next]))
                {
                    ++next;
                    return *answer;
                }
            out.answered = false;
            return true;
        };
        if (const char *over = advance(*step, perform))
        {
            out.reason = over;
            break;
        }
    }
    return out;
}

LoggedReplay ReplayLoggedStrike(std::string_view log)
{
    StrikeState run;
    bool started = false;
    return ReplayLogged(
        log, "power attack", [](GraphWatches &watches) { return watches.Open(kJenassa, kBlowWakes); },
        [&](const LoggedStep &step, const std::function<bool()> &perform) {
            if (!started)
                run = RequestStrikeAt(step.now);
            started = true;
            StrikeSeen seen;
            seen.holder = step.Flag("holder");
            seen.weaponDrawn = step.Flag("drawn");
            seen.attacking = step.Flag("attacking");
            seen.casting = step.Flag("casting");
            seen.facing = step.Flag("facing");
            seen.hitFrames = step.Int("hitFrames");
            seen.powerStops = step.Int("powerStops");
            seen.attackStops = step.Int("attackStops");
            return AdvanceStrike(run, seen, step.now, perform);
        },
        [](const LoggedStep &step, const Heard &heard) {
            StrikeSeen seen;
            Hear(seen, heard);
            return seen.hitFrames == step.Int("hitFrames") && seen.powerStops == step.Int("powerStops") &&
                   seen.attackStops == step.Int("attackStops");
        });
}

LoggedReplay ReplayLoggedBash(std::string_view log, std::string_view kind, bool power)
{
    BashState run;
    bool started = false;
    return ReplayLogged(
        log, kind, [](GraphWatches &watches) { return watches.Open(kJenassa, kBlowWakes); },
        [&](const LoggedStep &step, const std::function<bool()> &perform) {
            if (!started)
                run = RequestBashAt(step.now, power);
            started = true;
            BashSeen seen;
            seen.holder = step.Flag("holder");
            seen.weaponDrawn = step.Flag("drawn");
            seen.blocking = step.Flag("blocking");
            const std::string *attack = step.Read("attack");
            seen.attack = !attack || *attack == "none" ? BashSeen::Attack::None
                          : *attack == "bash"          ? BashSeen::Attack::Bash
                                                       : BashSeen::Attack::Other;
            seen.attackState = step.Int("state");
            seen.blockOuts = step.Int("blockOuts");
            seen.bashStops = step.Int("bashStops");
            return AdvanceBash(run, seen, step.now, [&](BashCommand) { return perform(); });
        },
        [](const LoggedStep &step, const Heard &heard) {
            BashSeen seen;
            Hear(seen, heard);
            return seen.blockOuts == step.Int("blockOuts") && seen.bashStops == step.Int("bashStops");
        });
}

// "HH:MM:SS.mmm".
std::string ClockOf(double seconds)
{
    const long long ms = std::llround(seconds * 1000.0);
    char text[16]{};
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld.%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60,
                  ms % 1000);
    return text;
}

// The log a follower's request writes, line for line as the game writes
// it: its events as the sink logs them, its steps as the step logs them
// (ReadsOf), each action's answer.
struct LogWriter
{
    std::string text;
    void Say(double at, std::string_view module, const std::string &line)
    {
        text += "[" + ClockOf(at) + "] [1] [D] [" + std::string(module) + "]    " + line + "\n";
    }
    void Heard(const Event &event)
    {
        const std::string head = "anim 000E1BA9: ";
        if (!IsFire(event.tag))
            return Say(event.at, "graph", head + std::string(GraphTagName(event.tag)));
        char form[9]{};
        std::snprintf(form, sizeof(form), "%08X", event.form);
        Say(event.at, "graph", head + std::string(GraphTagName(event.tag)));
        Say(event.at, "graph",
            head + (event.tag == GraphTag::SpellFireVoice   ? std::string("the voice fired: shout ") + form
                    : event.tag == GraphTag::SpellFireRight ? std::string("the right hand fired ") + form
                                                            : std::string("the left hand fired ") + form));
    }
};

} // namespace

TEST_CASE("a step's line says its time and reads, and is read back", "[replay]")
{
    StrikeSeen strike;
    strike.weaponDrawn = true;
    strike.facing = true;
    strike.hitFrames = 2;
    strike.attackStops = 1;
    REQUIRE(ReadsOf(strike, 1234.5) ==
            "step at 1234.500: holder=1 drawn=1 attacking=0 casting=0 facing=1 hitFrames=2 powerStops=0 attackStops=1");
    BashSeen bash;
    bash.attack = BashSeen::Attack::Bash;
    bash.attackState = 6;
    bash.blockOuts = 1;
    REQUIRE(ReadsOf(bash, 0.0004) ==
            "step at 0.000: holder=1 drawn=0 blocking=0 attack=bash state=6 blockOuts=1 bashStops=0");
    bash.attack = BashSeen::Attack::Other;
    REQUIRE(ReadsOf(bash, 1.0).find(" attack=other ") != std::string::npos);
    LeaseSeen lease;
    lease.running = true;
    lease.fired = true;
    REQUIRE(ReadsOf(lease, 99.9996) == "step at 100.000: holder=1 running=1 fired=1 stopped=0 begun=0 targetDead=0");

    // The lines are views of the text, which has to outlive them.
    const std::string text =
        "[00:32:12.764] [16540] [D] [blows]    Jenassa (000E1BA9): power attack " + ReadsOf(strike, 1234.5) + "\n";
    const auto lines = LinesOf(text);
    REQUIRE(lines.size() == 1);
    const auto step = StepOf(lines[0], "power attack");
    REQUIRE(step);
    REQUIRE(step->now == Approx(1234.5));
    REQUIRE(step->Flag("drawn"));
    REQUIRE_FALSE(step->Flag("attacking"));
    REQUIRE(step->Int("hitFrames") == 2);
    REQUIRE(step->Int("attackStops") == 1);
    REQUIRE(step->Int("missing") == -1);
    REQUIRE_FALSE(StepOf(lines[0], "power bash"));
}

TEST_CASE("replayed from its own lines: the power attacks' logs, written as the game writes them", "[replay]")
{
    struct Case
    {
        std::string_view log;
        double request;
        std::function<StrikeSeen(double)> reads;
        const char *reason;
    };
    const auto takenAtOnce = [](double t) {
        StrikeSeen seen;
        seen.weaponDrawn = true;
        seen.facing = true;
        seen.attacking = Between(t, SecondsOf("00:32:12.780"), SecondsOf("00:32:14.131"));
        return seen;
    };
    const auto afterAWait = [](double t) {
        StrikeSeen seen;
        seen.weaponDrawn = true;
        seen.facing = true;
        seen.attacking =
            t < SecondsOf("00:32:07.509") || Between(t, SecondsOf("00:32:07.526"), SecondsOf("00:32:08.893"));
        return seen;
    };
    for (const Case &c : {Case{kTakenAtOnce, SecondsOf("00:32:12.764"), takenAtOnce, "power attack made"},
                          Case{kAfterAWait, SecondsOf("00:32:05.659"), afterAWait, "power attack made"}})
    {
        // The game's lines for the reconstructed run.
        LogWriter writer;
        GraphWatches watches;
        GraphWatch watch;
        StrikeState run = RequestStrikeAt(c.request);
        std::string direct;
        int steps = 0;
        Drive(
            watches, EventsOf(c.log), c.request, 0.5, c.request + 6.0,
            [&] { watch = watches.Open(kJenassa, kBlowWakes); },
            [&](double now) {
                StrikeSeen seen = c.reads(now);
                Hear(seen, watch.HeardSoFar());
                ++steps;
                writer.Say(now, "blows", "Jenassa (000E1BA9): power attack " + ReadsOf(seen, now));
                const char *over = AdvanceStrike(run, seen, now, [&] {
                    writer.Say(now, "blows",
                               "Jenassa (000E1BA9): the right attack action carrying attackPowerStartInPlace taken "
                               "0.00 s after the request");
                    return true;
                });
                if (over)
                    direct = over;
                return over != nullptr;
            },
            [&](const Event &event) { writer.Heard(event); });
        REQUIRE(direct == c.reason);

        const LoggedReplay replayed = ReplayLoggedStrike(writer.text);
        REQUIRE(replayed.reason == direct);
        REQUIRE(replayed.steps == steps);
        REQUIRE(replayed.asked == 1);
        REQUIRE(replayed.answered);
        REQUIRE(replayed.countsAgree);
    }
}

TEST_CASE("replayed from its own lines: the power bash", "[replay]")
{
    const double request = SecondsOf("00:34:56.766");
    const auto reads = [](double t) {
        BashSeen seen;
        seen.weaponDrawn = true;
        seen.blocking = Between(t, SecondsOf("00:34:56.780"), SecondsOf("00:34:56.830"));
        const bool bashing = Between(t, SecondsOf("00:34:56.782"), SecondsOf("00:34:57.347"));
        seen.attack = bashing ? BashSeen::Attack::Bash : BashSeen::Attack::None;
        seen.attackState = bashing ? 6 : 0;
        return seen;
    };
    LogWriter writer;
    GraphWatches watches;
    GraphWatch watch;
    BashState run = RequestBashAt(request, true);
    Drive(
        watches, EventsOf(kPowerBash), request, 0.5, request + 4.0, [&] { watch = watches.Open(kJenassa, kBlowWakes); },
        [&](double now) {
            BashSeen seen = reads(now);
            Hear(seen, watch.HeardSoFar());
            writer.Say(now, "blows", "Jenassa (000E1BA9): power bash " + ReadsOf(seen, now));
            return AdvanceBash(run, seen, now, [&](BashCommand command) {
                       writer.Say(now, "blows",
                                  command == BashCommand::RaiseBlock
                                      ? "Jenassa (000E1BA9): the left attack action for the block taken 0.00 s after "
                                        "the request"
                                      : "Jenassa (000E1BA9): the right attack action carrying bashPowerStart taken "
                                        "0.01 s after the request");
                       return true;
                   }) != nullptr;
        },
        [&](const Event &event) { writer.Heard(event); });

    const LoggedReplay replayed = ReplayLoggedBash(writer.text, "power bash", true);
    REQUIRE(replayed.reason == "bash made");
    REQUIRE(replayed.asked == 2);
    REQUIRE(replayed.answered);
    REQUIRE(replayed.countsAgree);
}

TEST_CASE("a logged step whose counts the log's events do not bear out is caught", "[replay]")
{
    // A HitFrame the step counted and the log never showed: a sink that
    // counted what it should not have, or a log missing an event.
    const std::string log =
        "[00:00:01.000] [1] [D] [blows]    Jenassa (000E1BA9): power attack step at 1.000: holder=1 drawn=1 "
        "attacking=0 casting=0 facing=1 hitFrames=0 powerStops=0 attackStops=0\n"
        "[00:00:01.000] [1] [D] [blows]    Jenassa (000E1BA9): the right attack action carrying "
        "attackPowerStartInPlace turned away 0.00 s after the request\n"
        "[00:00:01.200] [1] [D] [blows]    Jenassa (000E1BA9): power attack step at 1.200: holder=1 drawn=1 "
        "attacking=0 casting=0 facing=1 hitFrames=1 powerStops=0 attackStops=0\n";
    const LoggedReplay replayed = ReplayLoggedStrike(log);
    REQUIRE_FALSE(replayed.countsAgree);
    // The first action was turned away, as its line says; the second has no
    // line.
    REQUIRE(replayed.asked == 2);
    REQUIRE_FALSE(replayed.answered);
    REQUIRE(replayed.reason.empty());
}
