#pragma once
// A follower's bash, from the request to the bash seen: wait for the weapon
// drawn and no swing, raise the block by the combat AI's own action, wait
// for it to be up and steady, take the bash by the right attack action
// from the block, watch for the bash attack state. The game side reads the
// actor each fast tick and performs the two actions (game/Blows.cpp); the
// steps, the waits, the refusals and the reason it is over are decided
// here, where every deadline and every reason is tested. No Skyrim.
//
// The two actions answer at once -- the engine takes them or turns them
// away -- so the step performs them through a callback and reads the
// answer in the same tick, as the follower's own AI would. The test's
// callback is a script of answers.

#include <cstdint>
#include <functional>

namespace ft
{

// From the request to the block lowered. Long enough for a swing in
// progress to end and a block to come up; a request still waiting then is
// given up, and says at which step.
inline constexpr double kBashDeadlineSeconds = 2.0;
// How long a bash that was taken is watched for the bash attack state.
// The animation is under a second; one not seen by then was not made.
inline constexpr double kBashWatchSeconds = 1.5;
// How long a request that had to wait -- for the follower's own swing to
// end, or the block to come up -- holds the bash once the hands are free
// with the block up. Asked for on the tick the wait ended, the tree chose
// an ordinary attack or nothing: no bash in eight such requests, where
// ten of twelve that went straight through bashed (2026-09-15).
inline constexpr double kBashSettleSeconds = 0.25;

enum class BashStep : std::uint8_t
{
    Ready,    // waiting for the weapon drawn and no swing, then the block
    Blocking, // waiting for the block to be up, then the bash
    Bashing   // the bash was taken; watched until it ends
};

struct BashState
{
    bool power{false};
    BashStep step{BashStep::Ready};
    double requestedAt{0.0};
    double blockAskedAt{-1.0};
    double blockUpAt{-1.0};
    double sentAt{-1.0};
    // Since when the hands have been free (the weapon drawn, no attack),
    // and free with the block up; -1 while they are not.
    double freeSince{-1.0};
    double steadySince{-1.0};
    // When the bash attack state was first seen, and when it was over; -1
    // for not yet. A bash is short, and one cut off shorter still.
    double bashFrom{-1.0};
    double bashEnd{-1.0};
    // Lowered at the end only if this request raised it: a block the
    // follower already held is their AI's to lower.
    bool raised{false};
    bool alreadyBlocking{false};
    bool sawBash{false};
    bool waited{false};   // a step could not go on at once; the bash settles first
    int blockRefusals{0}; // the left attack action was turned away
    int bashRefusals{0};  // the bash was turned away with the block up
    // The first attack state other than a bash seen once the bash was
    // taken, -1 for none: a swing the tree chose over the bash.
    int otherAttackState{-1};
};

[[nodiscard]] BashState RequestBashAt(double now, bool power) noexcept;

// What the fast tick reads of the follower.
struct BashSeen
{
    bool holder{true}; // the follower still resolves
    bool weaponDrawn{false};
    bool blocking{false};
    // The attack state: none, the bash, or another, with the engine's
    // number for the report.
    enum class Attack : std::uint8_t
    {
        None,
        Bash,
        Other
    };
    Attack attack{Attack::None};
    int attackState{0};
};

// The two actions the step may take, each answered at once.
enum class BashCommand : std::uint8_t
{
    RaiseBlock, // the left attack action, which the tree resolves into a block
    Bash        // the right attack action from the block, bashStart or bashPowerStart
};

// One step, where the request can take it; the reason it is over, or null
// while it goes on. `perform` takes an action and answers whether the
// engine took it.
[[nodiscard]] const char *AdvanceBash(BashState &state, const BashSeen &seen, double now,
                                      const std::function<bool(BashCommand)> &perform);

} // namespace ft
