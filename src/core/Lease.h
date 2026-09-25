#pragma once
// A follower's cast or shout as a lease on one of their package records,
// from the tick's side: armed with a deadline, watched until it is picked
// up, extended once when the cast begins and once when a stream starts, and
// finished for one reason. The game side arms the record, reads the engine
// each tick and hands the facts here (game/Packages.cpp, TickPackages);
// what they mean -- whether to wait, extend or finish, and why -- is decided
// here, where every deadline and every reason is tested. The engine actions
// the step asks for -- spend the scroll, report, release -- stay the game's.
// No Skyrim.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

namespace ft
{

// The lease's own standing between ticks. The sink's flags (fired,
// stopped, begun) are not here: they are written from the engine's
// threads and are read into a LeaseSeen each tick.
struct LeaseState
{
    double armedAt{0.0};
    double until{0.0}; // the deadline
    // A concentration spell: the fire event is the stream's start, not
    // its release, and the stream runs `sustain` seconds.
    bool sustained{false};
    float sustain{0.0f};
    // The AI has been seen running our package.
    bool seenRunning{false};
    // One-shots: the stream extension and the begin-cast extension.
    bool streaming{false};
    bool extended{false};
};

// The pick-up windows, in seconds of the tactics clock.
inline constexpr double kArmWindowSeconds = 2.5;      // a hand cast
inline constexpr double kVoiceArmWindowSeconds = 3.0; // a shout or a power
inline constexpr double kBeginCastSeconds = 3.0;      // once the cast begins
inline constexpr double kStreamGraceSeconds = 1.0;    // past the stream's own length

// A lease armed now: the deadline set, the one-shots forgotten. What it
// casts and whether it streams are the request's.
[[nodiscard]] LeaseState ArmLease(double now, double window, bool sustained, float sustain) noexcept;

// What the tick reads of the engine for a busy slot.
struct LeaseSeen
{
    bool holder{true};   // the follower still resolves; false: unloaded or gone
    bool running{false}; // our package is their current one
    // The sink's flags: our spell or voice left them; a CastStop after
    // that, the stream's end; a BeginCast.
    bool fired{false};
    bool stopped{false};
    bool begun{false};
    bool targetDead{false}; // a stream's target
};

// What the voice slot is casting, for the reason's wording.
enum class LeaseKind
{
    Spell,
    Shout,
    Power
};

// What the tick does for the slot.
struct LeaseStep
{
    // Finished, for this reason; null to keep waiting.
    const char *finish{nullptr};
    // A cast finished by firing: the scroll is spent.
    bool fired{false};
    // First seen running this tick.
    bool pickedUp{false};
    // The extensions applied this tick, for the log.
    bool streamExtended{false};
    bool beginExtended{false};
};

[[nodiscard]] LeaseStep AdvanceCast(LeaseState &state, const LeaseSeen &seen, LeaseKind kind, double now) noexcept;

// Where a record goes to reach the follower: one of their alias package
// arrays, in the order the game finds them. The choice: the first the
// running package came from, where it is evaluated first; else the fullest
// with anything in it, the quest that drives them; else none.
struct StackSeen
{
    bool holdsRunning{false}; // the running package is in it
    std::size_t size{0};      // its packages
};
[[nodiscard]] std::optional<std::size_t> ChooseStack(std::span<const StackSeen> places) noexcept;

} // namespace ft
