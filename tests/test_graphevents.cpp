// The animation graph's events as the requests hear them (core/GraphEvents.h):
// the tags, the watches, their own fires and their wakes, under threads; and
// the proof that the three sinks this replaced read the same: each is
// transcribed below as it stood (git 5aacca6, game/Blows.cpp BashGraphSink,
// game/Packages.cpp SpellFireSink, game/PlayerCast.cpp PlayerFireSink) and
// run beside the watches over random event streams.

#include <catch2/catch_test_macros.hpp>

#include "core/Bash.h"
#include "core/Blows.h"
#include "core/GraphEvents.h"
#include "core/Lease.h"
#include "core/PlayerCast.h"
#include "core/Strike.h"

#include <array>
#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace ft;

namespace
{

constexpr std::uint32_t kActor = 0x000E1BA9;
constexpr std::uint32_t kOther = 0x000A2C94;

std::vector<GraphTag> AllTags()
{
    std::vector<GraphTag> tags;
    for (std::size_t i = 0; i < kGraphTagCount; ++i)
        tags.push_back(static_cast<GraphTag>(i));
    return tags;
}

// The old sinks compared with _stricmp.
bool NoCase(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (fold(a[i]) != fold(b[i]))
            return false;
    }
    return true;
}

// What the graph might send: every recorded tag, some in another case, and
// tags that are not recorded.
std::string RandomName(std::mt19937 &rng)
{
    static const std::vector<std::string> unrecorded{
        "FootLeft", "tailCombatIdle", "SoundPlay.NPCHumanCombatShieldBlock", "Magic_Equip_Out", "HitFrameX",
        "",         "BeginCast"};
    std::uniform_int_distribution<int> pick(0, 99);
    const int roll = pick(rng);
    if (roll < 12)
        return unrecorded[static_cast<std::size_t>(roll) % unrecorded.size()];
    std::uniform_int_distribution<std::size_t> tag(0, kGraphTagCount - 1);
    std::string name(GraphTagName(static_cast<GraphTag>(tag(rng))));
    if (roll < 20)
        for (char &c : name)
            c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    return name;
}

} // namespace

// ---- The tags

TEST_CASE("every tag reads back from its name, in any case", "[graph]")
{
    for (const GraphTag tag : AllTags())
    {
        const std::string name(GraphTagName(tag));
        REQUIRE_FALSE(name.empty());
        REQUIRE(GraphTagOf(name) == tag);
        std::string upper = name;
        for (char &c : upper)
            c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        REQUIRE(GraphTagOf(upper) == tag);
        // No two names alike but for case.
        for (const GraphTag other : AllTags())
            if (other != tag)
                REQUIRE_FALSE(NoCase(name, std::string(GraphTagName(other))));
    }
    REQUIRE(GraphTagOf("mlh_spellfire_event") == GraphTag::SpellFireLeft);
    REQUIRE(GraphTagOf("HITFRAME") == GraphTag::HitFrame);
}

TEST_CASE("what the graph says and is not recorded reads as nothing", "[graph]")
{
    for (const char *name : {"FootLeft", "", "HitFram", "HitFrameX", "Hit Frame", "BeginCast", "tailCombatIdle",
                             "Magic_Equip_Out", "MLh_SpellFire", "attackStop "})
        REQUIRE_FALSE(GraphTagOf(name).has_value());
    REQUIRE(GraphTagName(GraphTag::Count).empty());
}

TEST_CASE("the three fires are the fires", "[graph]")
{
    for (const GraphTag tag : AllTags())
    {
        const bool fire =
            tag == GraphTag::SpellFireLeft || tag == GraphTag::SpellFireRight || tag == GraphTag::SpellFireVoice;
        REQUIRE(IsFire(tag) == fire);
    }
}

TEST_CASE("a set of tags holds what it was made of and nothing else", "[graph]")
{
    constexpr GraphTags none;
    STATIC_REQUIRE(none.Empty());
    constexpr GraphTags some{GraphTag::HitFrame, GraphTag::InterruptCast};
    STATIC_REQUIRE_FALSE(some.Empty());
    for (const GraphTag tag : AllTags())
    {
        REQUIRE_FALSE(none.Has(tag));
        REQUIRE(some.Has(tag) == (tag == GraphTag::HitFrame || tag == GraphTag::InterruptCast));
    }
    // The last tag, the top of the word.
    constexpr GraphTags last{GraphTag::InterruptCast};
    STATIC_REQUIRE(last.Has(GraphTag::InterruptCast));
    STATIC_REQUIRE_FALSE(last.Has(GraphTag::AttackStop));
}

// ---- The watches

TEST_CASE("a watch hears its actor's events from when it opens, one count each", "[graph]")
{
    GraphWatches watches;
    REQUIRE_FALSE(watches.AnyOpen());
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::HitFrame).watched);

    const GraphWatch watch = watches.Open(kActor);
    REQUIRE(watch.Open());
    REQUIRE(watches.AnyOpen());
    REQUIRE(watches.OpenCount() == 1);
    REQUIRE(watches.Watching(kActor));
    REQUIRE_FALSE(watches.Watching(kOther));

    REQUIRE(watches.Record(kActor, GraphTag::HitFrame).watched);
    REQUIRE(watches.Record(kActor, GraphTag::HitFrame).watched);
    REQUIRE(watches.Record(kActor, GraphTag::AttackStop).watched);
    REQUIRE_FALSE(watches.Record(kOther, GraphTag::HitFrame).watched);
    const Heard heard = watch.HeardSoFar();
    REQUIRE(heard.Count(GraphTag::HitFrame) == 2);
    REQUIRE(heard.Count(GraphTag::AttackStop) == 1);
    int total = 0;
    for (const int count : heard.counts)
        total += count;
    REQUIRE(total == 3);
    REQUIRE(heard.ownFires == 0);
    REQUIRE(heard.stopsAfterOwnFire == 0);
}

TEST_CASE("two watches on one actor each count from their own opening", "[graph]")
{
    GraphWatches watches;
    const GraphWatch early = watches.Open(kActor);
    watches.Record(kActor, GraphTag::HitFrame);
    const GraphWatch late = watches.Open(kActor);
    watches.Record(kActor, GraphTag::HitFrame);
    REQUIRE(early.HeardSoFar().Count(GraphTag::HitFrame) == 2);
    REQUIRE(late.HeardSoFar().Count(GraphTag::HitFrame) == 1);
    REQUIRE(watches.OpenCount() == 2);
}

TEST_CASE("the tag past the last is not recorded", "[graph]")
{
    GraphWatches watches;
    const GraphWatch watch = watches.Open(kActor, {GraphTag::HitFrame});
    const Recorded recorded = watches.Record(kActor, GraphTag::Count);
    REQUIRE_FALSE(recorded.watched);
    REQUIRE_FALSE(recorded.wakes);
    REQUIRE(watch.HeardSoFar().counts == Heard{}.counts);
}

TEST_CASE("a watch closes when it goes, when moved over, and when asked, once", "[graph]")
{
    GraphWatches watches;
    {
        const GraphWatch scoped = watches.Open(kActor);
        REQUIRE(watches.OpenCount() == 1);
    }
    REQUIRE(watches.OpenCount() == 0);
    REQUIRE_FALSE(watches.AnyOpen());
    REQUIRE_FALSE(watches.Watching(kActor));

    GraphWatch first = watches.Open(kActor);
    watches.Record(kActor, GraphTag::HitFrame);
    // Moved: the new holder hears it, the old one is no watch. What a
    // moved-from watch does is the point here.
    GraphWatch moved(std::move(first));
    REQUIRE(moved.Open());
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE_FALSE(first.Open());
    REQUIRE(first.HeardSoFar().Count(GraphTag::HitFrame) == 0);
    REQUIRE(moved.HeardSoFar().Count(GraphTag::HitFrame) == 1);
    first.SetWakes({GraphTag::HitFrame}); // no watch: nothing to set
    first.Close();                        // and nothing to close
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE(watches.OpenCount() == 1);

    // Moved over: the one it held is closed.
    GraphWatch other = watches.Open(kOther);
    REQUIRE(watches.OpenCount() == 2);
    moved = std::move(other);
    REQUIRE(watches.OpenCount() == 1);
    REQUIRE_FALSE(watches.Watching(kActor));
    REQUIRE(watches.Watching(kOther));
    // Onto itself: nothing changes.
    GraphWatch &alias = moved;
    moved = std::move(alias);
    REQUIRE(moved.Open());
    REQUIRE(watches.OpenCount() == 1);

    moved.Close();
    REQUIRE_FALSE(moved.Open());
    REQUIRE(watches.OpenCount() == 0);
    moved.Close();
    REQUIRE(moved.HeardSoFar().counts == Heard{}.counts);
    REQUIRE_FALSE(watches.Record(kOther, GraphTag::HitFrame).watched);
}

TEST_CASE("a default watch is no watch", "[graph]")
{
    const GraphWatch none;
    REQUIRE_FALSE(none.Open());
    REQUIRE(none.HeardSoFar().ownFires == 0);
    none.SetWakes({GraphTag::HitFrame});
}

TEST_CASE("its own fires are the event and what fired, or the event whatever fired", "[graph]")
{
    constexpr std::uint32_t kSpell = 0x3A10D265;
    GraphWatches watches;
    const GraphWatch left = watches.Open(kActor, {}, {{GraphTag::SpellFireLeft, kSpell}});
    const GraphWatch voice = watches.Open(kActor, {}, {{GraphTag::SpellFireVoice, std::nullopt}});

    // Another spell from the left, ours from the right: neither is the
    // left watch's.
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::SpellFireLeft, 0x000CDB70).ownFire);
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::SpellFireRight, kSpell).ownFire);
    REQUIRE(left.HeardSoFar().ownFires == 0);
    REQUIRE(left.HeardSoFar().Count(GraphTag::SpellFireLeft) == 1);
    // Ours from the left.
    REQUIRE(watches.Record(kActor, GraphTag::SpellFireLeft, kSpell).ownFire);
    REQUIRE(left.HeardSoFar().ownFires == 1);
    // The voice, whatever went off, and nothing at all.
    REQUIRE(watches.Record(kActor, GraphTag::SpellFireVoice, 0x00013E07).ownFire);
    REQUIRE(watches.Record(kActor, GraphTag::SpellFireVoice, 0).ownFire);
    REQUIRE(voice.HeardSoFar().ownFires == 2);
    REQUIRE(left.HeardSoFar().ownFires == 1);
    // Another actor's is nobody's.
    REQUIRE_FALSE(watches.Record(kOther, GraphTag::SpellFireLeft, kSpell).ownFire);
    REQUIRE(left.HeardSoFar().ownFires == 1);
}

TEST_CASE("a CastStop counts as a stream's end only after the watch's own fire", "[graph]")
{
    constexpr std::uint32_t kSpell = 0x02011F2C;
    GraphWatches watches;
    const GraphWatch watch = watches.Open(kActor, {}, {{GraphTag::SpellFireLeft, kSpell}});
    watches.Record(kActor, GraphTag::CastStop);                   // some other cast's end
    watches.Record(kActor, GraphTag::SpellFireRight, 0x000CDB70); // their own spell
    watches.Record(kActor, GraphTag::CastStop);                   // its end
    REQUIRE(watch.HeardSoFar().stopsAfterOwnFire == 0);
    watches.Record(kActor, GraphTag::SpellFireLeft, kSpell); // ours
    REQUIRE(watch.HeardSoFar().stopsAfterOwnFire == 0);
    watches.Record(kActor, GraphTag::CastStop); // ours ends
    watches.Record(kActor, GraphTag::CastStop);
    const Heard heard = watch.HeardSoFar();
    REQUIRE(heard.stopsAfterOwnFire == 2);
    REQUIRE(heard.Count(GraphTag::CastStop) == 4);
}

TEST_CASE("an event wakes where a watch on its actor waits on it, and the wait can change", "[graph]")
{
    GraphWatches watches;
    const GraphWatch watch = watches.Open(kActor, {GraphTag::InterruptCast});
    const GraphWatch quiet = watches.Open(kOther);
    REQUIRE(watches.Record(kActor, GraphTag::InterruptCast).wakes);
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::HitFrame).wakes);
    // Another actor's InterruptCast wakes nothing: no watch on them waits.
    REQUIRE_FALSE(watches.Record(kOther, GraphTag::InterruptCast).wakes);
    watch.SetWakes({});
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::InterruptCast).wakes);
    watch.SetWakes({GraphTag::HitFrame});
    REQUIRE(watches.Record(kActor, GraphTag::HitFrame).wakes);
    // Still counted, woken or not.
    REQUIRE(watch.HeardSoFar().Count(GraphTag::InterruptCast) == 2);
}

TEST_CASE("the blows wake on the events their steps wait on, and those only", "[graph]")
{
    // The old sink's list, as it stood.
    const std::vector<std::string> old{
        "attackStop",  "PowerAttackStop", "blockStartOut", "blockStop", "bashStop", "bashExit", "PowerAttack_Start_end",
        "preHitFrame", "weaponSwing",     "HitFrame",      "shoutStop", "CastStop"};
    for (const GraphTag tag : AllTags())
    {
        bool listed = false;
        for (const std::string &name : old)
            listed = listed || NoCase(name, std::string(GraphTagName(tag)));
        REQUIRE(kBlowWakes.Has(tag) == listed);
    }
}

TEST_CASE("the player's cast wakes on the equip's InterruptCast while it lends, and on nothing after", "[graph]")
{
    for (const GraphTag tag : AllTags())
        REQUIRE(CastWakes(CastStep::Lending).Has(tag) == (tag == GraphTag::InterruptCast));
    for (const CastStep step : {CastStep::Drawing, CastStep::Pressing, CastStep::Charging, CastStep::Holding,
                                CastStep::Firing, CastStep::Restoring})
        REQUIRE(CastWakes(step).Empty());
}

TEST_CASE("a player's cast's own fires: the hands it takes, or the voice", "[graph]")
{
    constexpr std::uint32_t kSpell = 0x3A10D265;
    const auto has = [](const std::vector<OwnFire> &fires, GraphTag tag, std::optional<std::uint32_t> form) {
        for (const OwnFire &fire : fires)
            if (fire.tag == tag && fire.form == form)
                return true;
        return false;
    };
    const auto left = CastOwnFires(false, Hand::Left, kSpell);
    REQUIRE(left.size() == 1);
    REQUIRE(has(left, GraphTag::SpellFireLeft, kSpell));
    const auto right = CastOwnFires(false, Hand::Right, kSpell);
    REQUIRE(right.size() == 1);
    REQUIRE(has(right, GraphTag::SpellFireRight, kSpell));
    const auto both = CastOwnFires(false, Hand::Both, kSpell);
    REQUIRE(both.size() == 2);
    REQUIRE(has(both, GraphTag::SpellFireLeft, kSpell));
    REQUIRE(has(both, GraphTag::SpellFireRight, kSpell));
    const auto voice = CastOwnFires(true, Hand::None, kSpell);
    REQUIRE(voice.size() == 1);
    REQUIRE(has(voice, GraphTag::SpellFireVoice, std::nullopt));
    REQUIRE(CastOwnFires(false, Hand::None, kSpell).empty());
}

TEST_CASE("a follower's lease's own fires: its spell from either hand, and its shout from the voice", "[graph]")
{
    const auto spell = LeaseOwnFires(0x02011F2C, 0);
    REQUIRE(spell.size() == 2);
    REQUIRE(spell[0].tag == GraphTag::SpellFireLeft);
    REQUIRE(spell[0].form == 0x02011F2Cu);
    REQUIRE(spell[1].tag == GraphTag::SpellFireRight);
    const auto shout = LeaseOwnFires(0x00013E07, 0x00013E08);
    REQUIRE(shout.size() == 3);
    REQUIRE(shout[2].tag == GraphTag::SpellFireVoice);
    REQUIRE(shout[2].form == 0x00013E08u);
}

TEST_CASE("what each step reads of a watch", "[graph]")
{
    GraphWatches watches;
    const GraphWatch watch = watches.Open(kActor, {}, {{GraphTag::SpellFireLeft, 7u}});
    for (const GraphTag tag : {GraphTag::BlockStartOut, GraphTag::BlockStartOut, GraphTag::BashStop, GraphTag::HitFrame,
                               GraphTag::HitFrame, GraphTag::HitFrame, GraphTag::PowerAttackStop, GraphTag::AttackStop,
                               GraphTag::AttackStop, GraphTag::InterruptCast})
        watches.Record(kActor, tag);
    watches.Record(kActor, GraphTag::SpellFireLeft, 7);
    watches.Record(kActor, GraphTag::CastStop);
    const Heard heard = watch.HeardSoFar();

    BashSeen bash;
    Hear(bash, heard);
    REQUIRE(bash.blockOuts == 2);
    REQUIRE(bash.bashStops == 1);
    StrikeSeen strike;
    Hear(strike, heard);
    REQUIRE(strike.hitFrames == 3);
    REQUIRE(strike.powerStops == 1);
    REQUIRE(strike.attackStops == 2);
    CastSeen cast;
    Hear(cast, heard);
    REQUIRE(cast.interrupts == 1);
    REQUIRE(cast.ownFires == 1);

    LeaseSeen lease;
    Hear(lease, Heard{});
    REQUIRE_FALSE(lease.fired);
    REQUIRE_FALSE(lease.stopped);
    REQUIRE_FALSE(lease.begun);
    Hear(lease, heard);
    REQUIRE(lease.fired);
    REQUIRE(lease.stopped);
    REQUIRE_FALSE(lease.begun);
    for (const GraphTag begin : {GraphTag::BeginCastLeft, GraphTag::BeginCastRight, GraphTag::BeginCastVoice})
    {
        Heard one;
        one.counts[static_cast<std::size_t>(begin)] = 1;
        LeaseSeen begun;
        Hear(begun, one);
        REQUIRE(begun.begun);
    }
}

// ---- Threads

TEST_CASE("events from many threads while watches open, read and close: none lost, none torn", "[graph][threads]")
{
    GraphWatches watches;
    const GraphWatch whole = watches.Open(kActor, {GraphTag::HitFrame}, {{GraphTag::SpellFireLeft, 1u}});
    constexpr int kWriters = 6;
    constexpr int kEach = 4000;
    std::atomic<bool> stop{false};
    std::atomic<int> wakes{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w)
        writers.emplace_back([&watches, &wakes] {
            for (int i = 0; i < kEach; ++i)
            {
                if (watches.Record(kActor, GraphTag::HitFrame).wakes)
                    wakes.fetch_add(1, std::memory_order_relaxed);
                watches.Record(kActor, GraphTag::SpellFireLeft, i % 2 == 0 ? 1 : 2);
                watches.Record(kOther, GraphTag::HitFrame);
            }
        });
    // A reader opening and closing its own watches meanwhile: what one has
    // heard never goes down, and never runs past what was sent.
    bool monotonic = true;
    bool bounded = true;
    std::thread reader([&] {
        while (!stop.load())
        {
            const GraphWatch mine = watches.Open(kActor);
            int last = 0;
            for (int i = 0; i < 50; ++i)
            {
                const int now = mine.HeardSoFar().Count(GraphTag::HitFrame);
                monotonic = monotonic && now >= last;
                bounded = bounded && now <= kWriters * kEach;
                last = now;
                (void)watches.Watching(kActor);
            }
        }
    });
    for (std::thread &writer : writers)
        writer.join();
    stop.store(true);
    reader.join();

    const Heard heard = whole.HeardSoFar();
    REQUIRE(heard.Count(GraphTag::HitFrame) == kWriters * kEach);
    REQUIRE(heard.Count(GraphTag::SpellFireLeft) == kWriters * kEach);
    REQUIRE(heard.ownFires == kWriters * kEach / 2);
    REQUIRE(wakes.load() == kWriters * kEach);
    REQUIRE(monotonic);
    REQUIRE(bounded);
    REQUIRE(watches.OpenCount() == 1);
}

// ---- The old sinks, transcribed, beside the watches

namespace
{

// game/Blows.cpp's BashGraphSink: per actor, five counts by name, never
// reset; every step read the totals.
struct OldBlowCounts
{
    struct Counted
    {
        int blockOuts = 0;
        int bashStops = 0;
        int hitFrames = 0;
        int powerStops = 0;
        int attackStops = 0;
    };
    std::map<std::uint32_t, Counted> counted;

    void OnEvent(std::uint32_t actor, const std::string &tag)
    {
        Counted &c = counted[actor];
        if (NoCase(tag, "blockStartOut"))
            ++c.blockOuts;
        else if (NoCase(tag, "bashStop"))
            ++c.bashStops;
        else if (NoCase(tag, "HitFrame"))
            ++c.hitFrames;
        else if (NoCase(tag, "PowerAttackStop"))
            ++c.powerStops;
        else if (NoCase(tag, "attackStop"))
            ++c.attackStops;
    }
};

// The new sink over a name: what the game's does after the holder.
void Send(GraphWatches &watches, std::uint32_t actor, const std::string &name, std::uint32_t form = 0)
{
    if (const auto tag = GraphTagOf(name))
        watches.Record(actor, *tag, IsFire(*tag) ? form : 0);
}

bool SameBash(const BashState &a, const BashState &b)
{
    return a.power == b.power && a.step == b.step && a.requestedAt == b.requestedAt &&
           a.blockAskedAt == b.blockAskedAt && a.blockUpAt == b.blockUpAt && a.sentAt == b.sentAt &&
           a.freeSince == b.freeSince && a.steadySince == b.steadySince && a.bashFrom == b.bashFrom &&
           a.bashEnd == b.bashEnd && a.raised == b.raised && a.alreadyBlocking == b.alreadyBlocking &&
           a.sawBash == b.sawBash && a.waited == b.waited && a.blockRefusals == b.blockRefusals &&
           a.bashRefusals == b.bashRefusals && a.otherAttackState == b.otherAttackState;
}

bool SameStrike(const StrikeState &a, const StrikeState &b)
{
    return a.step == b.step && a.requestedAt == b.requestedAt && a.sentAt == b.sentAt && a.hitAt == b.hitAt &&
           a.endAt == b.endAt && a.waited == b.waited && a.sawAttack == b.sawAttack && a.refusals == b.refusals;
}

std::string Reason(const char *reason)
{
    return reason ? reason : "";
}

} // namespace

TEST_CASE("bashes decide the same on the watch's counts as on the old sink's totals", "[graph][equivalence]")
{
    int finished = 0;
    int made = 0;
    for (std::uint32_t seed = 1; seed <= 3000; ++seed)
    {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> percent(0, 99);
        OldBlowCounts old;
        GraphWatches watches;
        const auto event = [&] {
            const std::uint32_t actor = percent(rng) < 80 ? kActor : kOther;
            const std::string name = RandomName(rng);
            old.OnEvent(actor, name);
            Send(watches, actor, name);
        };
        // The actor's graph before the request: counted by the old sink,
        // not heard by a watch opened after.
        for (int i = percent(rng) % 12; i > 0; --i)
            event();
        double now = 100.0;
        const bool power = percent(rng) < 50;
        BashState oldRun = RequestBashAt(now, power);
        BashState newRun = oldRun;
        const GraphWatch watch = watches.Open(kActor, kBlowWakes);
        const char *over = nullptr;
        for (int step = 0; step < 60 && !over; ++step)
        {
            BashSeen seen;
            seen.weaponDrawn = percent(rng) < 85;
            seen.blocking = percent(rng) < 50;
            const int attack = percent(rng);
            seen.attack = attack < 60   ? BashSeen::Attack::None
                          : attack < 80 ? BashSeen::Attack::Bash
                                        : BashSeen::Attack::Other;
            seen.attackState = seen.attack == BashSeen::Attack::Bash   ? 6
                               : seen.attack == BashSeen::Attack::None ? 0
                                                                       : 3;
            const bool takes = percent(rng) < 70;
            const auto answer = [takes](BashCommand) { return takes; };

            BashSeen oldSeen = seen;
            oldSeen.blockOuts = old.counted[kActor].blockOuts;
            oldSeen.bashStops = old.counted[kActor].bashStops;
            BashSeen newSeen = seen;
            Hear(newSeen, watch.HeardSoFar());

            const char *oldOver = AdvanceBash(oldRun, oldSeen, now, answer);
            over = AdvanceBash(newRun, newSeen, now, answer);
            REQUIRE(Reason(oldOver) == Reason(over));
            REQUIRE(SameBash(oldRun, newRun));
            for (int i = percent(rng) % 4; i > 0; --i)
                event();
            now += 0.01 * (1 + percent(rng) % 40);
        }
        if (over)
        {
            ++finished;
            made += Reason(over) == "bash made" ? 1 : 0;
        }
    }
    // The streams reached every end, the one that matters most among them.
    REQUIRE(finished == 3000);
    REQUIRE(made > 100);
}

TEST_CASE("power attacks decide the same on the watch's counts as on the old sink's totals", "[graph][equivalence]")
{
    int made = 0;
    int cutShort = 0;
    for (std::uint32_t seed = 1; seed <= 3000; ++seed)
    {
        std::mt19937 rng(seed * 7919u);
        std::uniform_int_distribution<int> percent(0, 99);
        OldBlowCounts old;
        GraphWatches watches;
        const auto event = [&] {
            const std::uint32_t actor = percent(rng) < 80 ? kActor : kOther;
            const std::string name = RandomName(rng);
            old.OnEvent(actor, name);
            Send(watches, actor, name);
        };
        for (int i = percent(rng) % 12; i > 0; --i)
            event();
        double now = 100.0;
        StrikeState oldRun = RequestStrikeAt(now);
        StrikeState newRun = oldRun;
        const GraphWatch watch = watches.Open(kActor, kBlowWakes);
        const char *over = nullptr;
        for (int step = 0; step < 60 && !over; ++step)
        {
            StrikeSeen seen;
            seen.weaponDrawn = percent(rng) < 90;
            seen.attacking = percent(rng) < 40;
            seen.casting = percent(rng) < 10;
            seen.facing = percent(rng) < 80;
            const bool takes = percent(rng) < 75;
            const auto answer = [takes] { return takes; };

            StrikeSeen oldSeen = seen;
            oldSeen.hitFrames = old.counted[kActor].hitFrames;
            oldSeen.powerStops = old.counted[kActor].powerStops;
            oldSeen.attackStops = old.counted[kActor].attackStops;
            StrikeSeen newSeen = seen;
            Hear(newSeen, watch.HeardSoFar());

            const char *oldOver = AdvanceStrike(oldRun, oldSeen, now, answer);
            over = AdvanceStrike(newRun, newSeen, now, answer);
            REQUIRE(Reason(oldOver) == Reason(over));
            REQUIRE(SameStrike(oldRun, newRun));
            for (int i = percent(rng) % 4; i > 0; --i)
                event();
            now += 0.01 * (1 + percent(rng) % 40);
        }
        REQUIRE(over);
        made += Reason(over) == "power attack made" ? 1 : 0;
        cutShort += Reason(over) == "power attack cut short" ? 1 : 0;
    }
    REQUIRE(made > 100);
    REQUIRE(cutShort > 100);
}

namespace
{

// game/Packages.cpp's SpellFireSink and the slot's flags: cleared at the
// arm and the release, set by the holder's events while the slot holds.
struct OldSlot
{
    std::uint32_t holder = 0;
    std::uint32_t spellId = 0;
    std::uint32_t shoutId = 0;
    bool fired = false;
    bool stopped = false;
    bool begun = false;

    void Arm(std::uint32_t actor, std::uint32_t spell, std::uint32_t shout)
    {
        fired = stopped = begun = false;
        spellId = spell;
        shoutId = shout;
        holder = actor;
    }
    void Release()
    {
        holder = spellId = shoutId = 0;
        fired = stopped = begun = false;
    }
    // One event of `who`'s graph, with the spell selected in the hand that
    // fired and the shout the process is shouting, as the sink read them.
    void OnEvent(std::uint32_t who, const std::string &tag, std::uint32_t inHand, std::uint32_t shouting)
    {
        const bool right = NoCase(tag, "MRh_SpellFire_Event");
        const bool left = NoCase(tag, "MLh_SpellFire_Event");
        const bool voice = NoCase(tag, "Voice_SpellFire_Event");
        const bool begin =
            NoCase(tag, "BeginCastVoice") || NoCase(tag, "BeginCastRight") || NoCase(tag, "BeginCastLeft");
        const bool stop = NoCase(tag, "CastStop");
        if (holder != who)
            return;
        if (stop && fired)
            stopped = true;
        if (begin)
            begun = true;
        if (voice && shoutId != 0)
        {
            if (shouting == shoutId)
                fired = true;
            return;
        }
        if (!right && !left)
            return;
        if (inHand == spellId)
            fired = true;
    }
};

} // namespace

TEST_CASE("a follower's lease reads the same off its watch as off the old slot's flags", "[graph][equivalence]")
{
    // Few forms, so matches and near misses are common; 0 is nothing in
    // the hand, or no shout.
    const std::vector<std::uint32_t> forms{0, 0x100, 0x200, 0x300};
    int firedReads = 0;
    int stoppedReads = 0;
    int begunReads = 0;
    for (std::uint32_t seed = 1; seed <= 2000; ++seed)
    {
        std::mt19937 rng(seed * 104729u);
        std::uniform_int_distribution<int> percent(0, 99);
        std::uniform_int_distribution<std::size_t> form(0, forms.size() - 1);
        GraphWatches watches;
        // A spell slot and a shout slot, each on either of two followers.
        OldSlot oldSlots[2];
        GraphWatch newSlots[2];
        for (int op = 0; op < 200; ++op)
        {
            const int roll = percent(rng);
            const std::size_t s = static_cast<std::size_t>(percent(rng) % 2);
            if (roll < 6)
            {
                const std::uint32_t actor = percent(rng) < 70 ? kActor : kOther;
                const std::uint32_t spell = forms[form(rng)];
                const std::uint32_t shout = s == 1 ? forms[form(rng)] : 0;
                oldSlots[s].Arm(actor, spell, shout);
                newSlots[s] = watches.Open(actor, {}, LeaseOwnFires(spell, shout));
            }
            else if (roll < 9)
            {
                oldSlots[s].Release();
                newSlots[s].Close();
            }
            else if (roll < 30)
            {
                for (std::size_t i = 0; i < 2; ++i)
                {
                    LeaseSeen seen;
                    Hear(seen, newSlots[i].HeardSoFar());
                    REQUIRE(seen.fired == oldSlots[i].fired);
                    REQUIRE(seen.stopped == oldSlots[i].stopped);
                    REQUIRE(seen.begun == oldSlots[i].begun);
                    firedReads += seen.fired ? 1 : 0;
                    stoppedReads += seen.stopped ? 1 : 0;
                    begunReads += seen.begun ? 1 : 0;
                }
            }
            else
            {
                const std::uint32_t actor = percent(rng) < 80 ? kActor : kOther;
                // Fires and cast events most of all.
                static const std::vector<std::string> likely{"MLh_SpellFire_Event",
                                                             "MRh_SpellFire_Event",
                                                             "Voice_SpellFire_Event",
                                                             "CastStop",
                                                             "BeginCastLeft",
                                                             "BeginCastVoice",
                                                             "castSTOP"};
                const std::string name = percent(rng) < 70
                                             ? likely[static_cast<std::size_t>(percent(rng)) % likely.size()]
                                             : RandomName(rng);
                const std::uint32_t inHand = forms[form(rng)];
                const std::uint32_t shouting = forms[form(rng)];
                for (OldSlot &slot : oldSlots)
                    slot.OnEvent(actor, name, inHand, shouting);
                // The new sink reads the hand for a hand's fire and the
                // shout for the voice's.
                if (const auto tag = GraphTagOf(name))
                    watches.Record(actor, *tag,
                                   *tag == GraphTag::SpellFireVoice ? shouting
                                   : IsFire(*tag)                   ? inHand
                                                                    : 0);
            }
        }
    }
    REQUIRE(firedReads > 1000);
    REQUIRE(stoppedReads > 1000);
    REQUIRE(begunReads > 1000);
}

namespace
{

// game/PlayerCast.cpp's PlayerFireSink: the last spell each hand fired and
// the voice's fire, cleared at the press; the equip's InterruptCast,
// cleared at the lend.
struct OldPlayer
{
    std::uint32_t firedLeft = 0;
    std::uint32_t firedRight = 0;
    bool firedVoice = false;
    bool equipSettled = false;

    void OnEvent(const std::string &tag, std::uint32_t inHand)
    {
        const bool right = NoCase(tag, "MRh_SpellFire_Event");
        const bool left = NoCase(tag, "MLh_SpellFire_Event");
        if (NoCase(tag, "Voice_SpellFire_Event"))
        {
            firedVoice = true;
            return;
        }
        if (NoCase(tag, "InterruptCast"))
            equipSettled = true;
        if (!right && !left)
            return;
        (right ? firedRight : firedLeft) = inHand;
    }
    bool FireSeen(bool voice, Hand hand, std::uint32_t form) const
    {
        if (voice)
            return firedVoice;
        return (Overlap(hand, Hand::Left) && firedLeft == form) || (Overlap(hand, Hand::Right) && firedRight == form);
    }
};

} // namespace

TEST_CASE("the player's cast settles and fires on the watch where the old flags did", "[graph][equivalence]")
{
    // The one reading that differs: the old flags kept the LAST spell each
    // hand fired, so ours followed by another from the same hand before the
    // step read it said not fired. The watch counts ours whatever came
    // after. Counted here to show it is that case and no other.
    int settledSame = 0;
    int firedSame = 0;
    int ourFireOverwritten = 0;
    constexpr std::uint32_t kSpell = 0x3A10D265;
    constexpr std::uint32_t kOtherSpell = 0x000CDB70;
    for (std::uint32_t seed = 1; seed <= 4000; ++seed)
    {
        std::mt19937 rng(seed * 15485863u);
        std::uniform_int_distribution<int> percent(0, 99);
        const bool voice = percent(rng) < 25;
        const Hand hand =
            voice ? Hand::None
                  : std::array{Hand::Left, Hand::Right, Hand::Both}[static_cast<std::size_t>(percent(rng) % 3)];

        GraphWatches watches;
        OldPlayer old;
        CastState run;
        run.voice = voice;
        run.dual = hand == Hand::Both;
        run.requestedAt = run.stepAt = 100.0;
        const GraphWatch watch = watches.Open(kActor, CastWakes(run.step), CastOwnFires(voice, hand, kSpell));
        // Whether our fire, since the press, was followed by another spell
        // from the same hand: the case the flags got wrong.
        bool overwritten = false;
        const auto event = [&](bool afterPress) {
            static const std::vector<std::string> likely{"InterruptCast", "MLh_SpellFire_Event", "MRh_SpellFire_Event",
                                                         "Voice_SpellFire_Event", "interruptcast"};
            const std::string name =
                percent(rng) < 75 ? likely[static_cast<std::size_t>(percent(rng)) % likely.size()] : RandomName(rng);
            const std::uint32_t inHand = percent(rng) < 50 ? kSpell : kOtherSpell;
            if (afterPress && !voice)
            {
                const bool left = NoCase(name, "MLh_SpellFire_Event");
                const bool right = NoCase(name, "MRh_SpellFire_Event");
                const std::uint32_t before = left ? old.firedLeft : old.firedRight;
                if ((left && Overlap(hand, Hand::Left)) || (right && Overlap(hand, Hand::Right)))
                    if (before == kSpell && inHand != kSpell)
                        overwritten = true;
            }
            old.OnEvent(name, inHand);
            Send(watches, kActor, name, inHand);
        };
        const auto perform = [&](CastCommand command) {
            if (command == CastCommand::LendHands)
                old.equipSettled = false; // Lend cleared it before equipping
            if (command == CastCommand::Press)
            {
                old.firedLeft = old.firedRight = 0; // ClearFireFlags, before the press
                old.firedVoice = false;
            }
        };
        for (int i = percent(rng) % 5; i > 0; --i)
            event(false);

        // Lending: asked for, then waited on with the spell in the hand.
        CastSeen seen;
        seen.placed = false;
        seen.weapon = CastSeen::Weapon::Drawn;
        seen.casterIdle = true;
        Hear(seen, watch.HeardSoFar());
        REQUIRE_FALSE(AdvancePlayerCast(run, seen, 100.0, perform));
        REQUIRE(run.lendAsked);
        double now = 100.0;
        seen.placed = true;
        while (run.step == CastStep::Lending)
        {
            for (int i = percent(rng) % 3; i > 0; --i)
                event(false);
            now += 0.05;
            const bool oldSettled = old.equipSettled;
            Hear(seen, watch.HeardSoFar());
            const bool late = now - run.stepAt >= kLendSeconds;
            REQUIRE_FALSE(AdvancePlayerCast(run, seen, now, perform));
            if (voice)
                continue;
            // Settled where the flag said so; otherwise waited, or went at
            // the lend's deadline unsettled.
            REQUIRE((run.settledAt == now) == oldSettled);
            if (!oldSettled)
                REQUIRE((run.step == CastStep::Lending) == !late);
            ++settledSame;
        }
        // Drawn, free: pressed. Then Ready, released.
        while (run.step != CastStep::Charging)
        {
            now += 0.05;
            Hear(seen, watch.HeardSoFar());
            REQUIRE_FALSE(AdvancePlayerCast(run, seen, now, perform));
        }
        REQUIRE(run.pressed);
        // Fires after the press, and the step that reads them.
        seen.casterIdle = false;
        seen.caster = CastSeen::Caster::Ready;
        seen.casterState = 4;
        now += 0.05;
        Hear(seen, watch.HeardSoFar());
        REQUIRE_FALSE(AdvancePlayerCast(run, seen, now, perform));
        REQUIRE(run.step == CastStep::Firing);
        for (int i = percent(rng) % 5; i > 0; --i)
            event(true);
        now += 0.05;
        Hear(seen, watch.HeardSoFar());
        const bool oldFired = old.FireSeen(voice, hand, kSpell);
        REQUIRE_FALSE(AdvancePlayerCast(run, seen, now, perform));
        const bool newFired = run.fired;
        if (newFired == oldFired)
            ++firedSame;
        else
        {
            REQUIRE(newFired);
            REQUIRE_FALSE(oldFired);
            REQUIRE(overwritten);
            ++ourFireOverwritten;
        }
    }
    REQUIRE(settledSame > 2000);
    REQUIRE(firedSame > 3000);
    REQUIRE(ourFireOverwritten > 0);
}
