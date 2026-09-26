// The animation graph's events as the requests hear them (core/GraphEvents.h):
// the tags, the watches, their own fires and their wakes, and each step's
// reading of its watch, under threads too.

#include <catch2/catch_test_macros.hpp>

#include "core/Bash.h"
#include "core/Blows.h"
#include "core/GraphEvents.h"
#include "core/Lease.h"
#include "core/PlayerCast.h"
#include "core/Strike.h"

#include <atomic>
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

// The graph's names are compared without case.
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
                             "Magic_Equip_OutMoving", "InterruptCast", "MLh_SpellFire", "attackStop "})
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
    constexpr GraphTags some{GraphTag::HitFrame, GraphTag::EquipOut};
    STATIC_REQUIRE_FALSE(some.Empty());
    for (const GraphTag tag : AllTags())
    {
        REQUIRE_FALSE(none.Has(tag));
        REQUIRE(some.Has(tag) == (tag == GraphTag::HitFrame || tag == GraphTag::EquipOut));
    }
    // The last tag, the top of the word.
    constexpr GraphTags last{GraphTag::EquipOut};
    STATIC_REQUIRE(last.Has(GraphTag::EquipOut));
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
    const GraphWatch watch = watches.Open(kActor, {GraphTag::EquipOut});
    const GraphWatch quiet = watches.Open(kOther);
    REQUIRE(watches.Record(kActor, GraphTag::EquipOut).wakes);
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::HitFrame).wakes);
    // Another actor's equip's end wakes nothing: no watch on them waits.
    REQUIRE_FALSE(watches.Record(kOther, GraphTag::EquipOut).wakes);
    watch.SetWakes({});
    REQUIRE_FALSE(watches.Record(kActor, GraphTag::EquipOut).wakes);
    watch.SetWakes({GraphTag::HitFrame});
    REQUIRE(watches.Record(kActor, GraphTag::HitFrame).wakes);
    // Still counted, woken or not.
    REQUIRE(watch.HeardSoFar().Count(GraphTag::EquipOut) == 2);
}

TEST_CASE("the blows wake on the events their steps wait on, and those only", "[graph]")
{
    // As read from the follower's graph in play (2026-09-24).
    const std::vector<std::string> waitedOn{
        "attackStop",  "PowerAttackStop", "blockStartOut", "blockStop", "bashStop", "bashExit", "PowerAttack_Start_end",
        "preHitFrame", "weaponSwing",     "HitFrame",      "shoutStop", "CastStop"};
    for (const GraphTag tag : AllTags())
    {
        bool listed = false;
        for (const std::string &name : waitedOn)
            listed = listed || NoCase(name, std::string(GraphTagName(tag)));
        REQUIRE(kBlowWakes.Has(tag) == listed);
    }
}

TEST_CASE("the player's cast wakes on the equip's end while it lends, and on nothing after", "[graph]")
{
    for (const GraphTag tag : AllTags())
        REQUIRE(CastWakes(CastStep::Lending).Has(tag) == (tag == GraphTag::EquipOut));
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
                               GraphTag::AttackStop, GraphTag::EquipOut})
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
    REQUIRE(cast.equipOuts == 1);
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
