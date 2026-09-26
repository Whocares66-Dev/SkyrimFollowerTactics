#pragma once
// The animation graph's events that a request of ours waits on, heard once
// for every actor we act on. A request opens a watch on its actor where it
// starts listening -- a bash or a power attack at the request, the player's
// cast at the lend and again at the press, a follower's cast when its
// record is armed -- and reads what the watch has heard since: how many of
// each event, how many of them were its own fire, and how many CastStops
// came after the first. The game's one sink (game/Graph.cpp) turns the
// graph's tag into a GraphTag, reads what fired where a spell left a hand
// or the voice, records it here, and queues a step when a watch waits on
// the event.
//
// Thread-safe: the graph sends its events from the animation threads, and
// the requests open, read and close their watches on the game thread. No
// Skyrim.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

// The events a step reads, by the tag the graph sends. Whatever else the
// graph says is not recorded.
enum class GraphTag : std::uint8_t
{
    AttackStop,          // attackStop: an attack's end, and a block giving way to one
    PowerAttackStop,     // PowerAttackStop: a power attack's own end
    PowerAttackStartEnd, // PowerAttack_Start_end
    PreHitFrame,         // preHitFrame
    WeaponSwing,         // weaponSwing
    HitFrame,            // HitFrame: an attack reaching its hit
    BlockStartOut,       // blockStartOut: the block up and ready
    BlockStop,           // blockStop
    BashStop,            // bashStop: a bash's own end
    BashExit,            // bashExit
    BeginCastLeft,       // BeginCastLeft
    BeginCastRight,      // BeginCastRight
    BeginCastVoice,      // BeginCastVoice
    SpellFireLeft,       // MLh_SpellFire_Event: a spell left the left hand
    SpellFireRight,      // MRh_SpellFire_Event
    SpellFireVoice,      // Voice_SpellFire_Event: a shout or a power went off
    CastStop,            // CastStop: a cast's end, a stream's included
    ShoutStop,           // shoutStop
    EquipOut,            // Magic_Equip_Out: a magic equip animation's end
    Count
};
inline constexpr std::size_t kGraphTagCount = static_cast<std::size_t>(GraphTag::Count);

// The tag as the graph sends it, in any case; nothing for one not recorded.
[[nodiscard]] std::optional<GraphTag> GraphTagOf(std::string_view name) noexcept;
// As the graph spells it.
[[nodiscard]] std::string_view GraphTagName(GraphTag tag) noexcept;
// A spell leaving a hand or the voice: the sink reads what fired.
[[nodiscard]] bool IsFire(GraphTag tag) noexcept;

// "step at 123.456: ", the start of a step's reads as its log line says
// them (ReadsOf, in core/Bash.h, core/Strike.h, core/Lease.h): the time on
// the tactics clock, to the millisecond.
[[nodiscard]] std::string StepAt(double now);

class GraphTags
{
  public:
    constexpr GraphTags() noexcept = default;
    constexpr GraphTags(std::initializer_list<GraphTag> tags) noexcept
    {
        for (const GraphTag tag : tags)
            bits_ |= Bit(tag);
    }
    [[nodiscard]] constexpr bool Has(GraphTag tag) const noexcept
    {
        return (bits_ & Bit(tag)) != 0;
    }
    [[nodiscard]] constexpr bool Empty() const noexcept
    {
        return bits_ == 0;
    }

  private:
    static constexpr std::uint32_t Bit(GraphTag tag) noexcept
    {
        return std::uint32_t{1} << static_cast<unsigned>(tag);
    }
    std::uint32_t bits_{0};
};
static_assert(kGraphTagCount <= 32, "GraphTags is one 32-bit word");

// A fire that is the watcher's own: the event, and what fired -- the spell
// in the firing hand, the shout being shouted -- or any, where the form is
// not given.
struct OwnFire
{
    GraphTag tag{GraphTag::SpellFireLeft};
    std::optional<std::uint32_t> form;
};

// What a watch has heard since it opened.
struct Heard
{
    std::array<int, kGraphTagCount> counts{};
    int ownFires{0};
    // CastStops after its first own fire: a stream's end, where the stream
    // is its own. One before it is some other cast's.
    int stopsAfterOwnFire{0};

    [[nodiscard]] int Count(GraphTag tag) const noexcept
    {
        return counts[static_cast<std::size_t>(tag)];
    }
};

// What recording one event did.
struct Recorded
{
    bool watched{false}; // a watch on the actor heard it
    bool ownFire{false}; // it was a watch's own fire
    bool wakes{false};   // a watch waits on it: whatever is in flight is to be stepped
};

class GraphWatches;

// One open watch, closed when this goes. Moved, never copied; one moved
// from, or default-made, is no watch and has heard nothing.
class GraphWatch
{
  public:
    GraphWatch() noexcept = default;
    GraphWatch(GraphWatch &&other) noexcept;
    GraphWatch &operator=(GraphWatch &&other) noexcept;
    GraphWatch(const GraphWatch &) = delete;
    GraphWatch &operator=(const GraphWatch &) = delete;
    ~GraphWatch();

    [[nodiscard]] Heard HeardSoFar() const;
    void SetWakes(GraphTags wakes) const;
    [[nodiscard]] bool Open() const noexcept
    {
        return owner_ != nullptr;
    }
    void Close() noexcept;

  private:
    friend class GraphWatches;
    GraphWatch(GraphWatches *owner, std::uint64_t id) noexcept : owner_(owner), id_(id)
    {
    }
    GraphWatches *owner_{nullptr};
    std::uint64_t id_{0};
};

class GraphWatches
{
  public:
    GraphWatches() = default;
    GraphWatches(const GraphWatches &) = delete;
    GraphWatches &operator=(const GraphWatches &) = delete;

    // A watch on the actor's events from now: the events that step what is
    // in flight when it hears them, and the fires that are its own. It must
    // not outlive this.
    [[nodiscard]] GraphWatch Open(std::uint32_t actor, GraphTags wakes = {}, std::span<const OwnFire> fires = {});
    [[nodiscard]] GraphWatch Open(std::uint32_t actor, GraphTags wakes, std::initializer_list<OwnFire> fires)
    {
        return Open(actor, wakes, std::span<const OwnFire>(fires.begin(), fires.size()));
    }

    // One of the actor's events, with what fired for a fire (0 where
    // nothing was in the hand or no shout was being shouted, and for every
    // other event).
    Recorded Record(std::uint32_t actor, GraphTag tag, std::uint32_t form = 0);

    // Any watch open on the actor: the sink logs what its graph says.
    [[nodiscard]] bool Watching(std::uint32_t actor) const;
    // Any watch open at all, without the lock: the sink's first question,
    // asked of every event of every graph it is on.
    [[nodiscard]] bool AnyOpen() const noexcept
    {
        return open_.load(std::memory_order_relaxed) != 0;
    }
    [[nodiscard]] std::size_t OpenCount() const noexcept
    {
        return open_.load(std::memory_order_relaxed);
    }

  private:
    friend class GraphWatch;
    struct Watch
    {
        std::uint64_t id{0};
        std::uint32_t actor{0};
        GraphTags wakes;
        std::vector<OwnFire> fires;
        Heard heard;
    };
    [[nodiscard]] Heard HeardBy(std::uint64_t id) const;
    void SetWakes(std::uint64_t id, GraphTags wakes);
    void Close(std::uint64_t id) noexcept;

    mutable std::mutex mutex_;
    std::vector<Watch> watches_; // a handful: one for each request in flight
    std::atomic<std::size_t> open_{0};
    std::uint64_t next_{1}; // never 0, which is no watch, and never wraps
};

} // namespace ft
