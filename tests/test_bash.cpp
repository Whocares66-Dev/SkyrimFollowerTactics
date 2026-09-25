// A follower's bash from the request to the bash seen. No Skyrim; the two
// actions are answered by a script, and the actor's state is supplied.

#include <catch2/catch_test_macros.hpp>

#include "core/Bash.h"

#include <string>
#include <vector>

using namespace ft;

namespace
{

std::string Over(const char *reason)
{
    return reason ? reason : "";
}

// The engine takes every action, and the test reads which were asked.
struct Takes
{
    std::vector<BashCommand> asked;
    bool raise{true};
    bool bash{true};
    std::function<bool(BashCommand)> Fn()
    {
        return [this](BashCommand c) {
            asked.push_back(c);
            return c == BashCommand::RaiseBlock ? raise : bash;
        };
    }
};

BashSeen Drawn()
{
    BashSeen seen;
    seen.weaponDrawn = true;
    return seen;
}

BashSeen Blocking()
{
    BashSeen seen = Drawn();
    seen.blocking = true;
    return seen;
}

BashSeen Bashing()
{
    BashSeen seen = Blocking();
    seen.attack = BashSeen::Attack::Bash;
    seen.attackState = 6;
    return seen;
}

BashSeen Swinging()
{
    BashSeen seen = Drawn();
    seen.attack = BashSeen::Attack::Other;
    seen.attackState = 2;
    return seen;
}

} // namespace

TEST_CASE("free and already blocking: the bash goes at once, and is made when its bashStop is heard", "[bash]")
{
    BashState run = RequestBashAt(100.0, false);
    Takes takes;
    REQUIRE_FALSE(AdvanceBash(run, Blocking(), 100.0, takes.Fn()));
    REQUIRE(takes.asked == std::vector<BashCommand>{BashCommand::Bash});
    REQUIRE(run.alreadyBlocking);
    REQUIRE_FALSE(run.raised);
    REQUIRE(run.step == BashStep::Bashing);
    REQUIRE(run.sentAt == 100.0);
    REQUIRE(run.blockUpAt == 100.0);

    REQUIRE_FALSE(AdvanceBash(run, Bashing(), 100.1, takes.Fn()));
    REQUIRE(run.sawBash);
    REQUIRE(run.bashFrom == 100.1);
    // The state left, but no bashStop yet: not over.
    REQUIRE_FALSE(AdvanceBash(run, Blocking(), 100.3, takes.Fn()));
    BashSeen stopped = Blocking();
    stopped.bashStops = 1;
    REQUIRE(Over(AdvanceBash(run, stopped, 100.6, takes.Fn())) == "bash made");
    REQUIRE(run.bashEnd == 100.6);
    REQUIRE(takes.asked.size() == 1);

    // No step inside the bash state at all: its bashStop alone makes it.
    // One from before the bash was taken does not.
    BashState quick = RequestBashAt(200.0, true);
    BashSeen before = Blocking();
    before.bashStops = 4;
    REQUIRE_FALSE(AdvanceBash(quick, before, 200.0, takes.Fn()));
    REQUIRE(quick.step == BashStep::Bashing);
    REQUIRE_FALSE(AdvanceBash(quick, before, 200.2, takes.Fn()));
    BashSeen after = Drawn();
    after.bashStops = 5;
    REQUIRE(Over(AdvanceBash(quick, after, 200.6, takes.Fn())) == "bash made");
    REQUIRE(quick.sawBash);
    REQUIRE(quick.bashFrom < 0.0);
}

TEST_CASE("the block raised is not up on that step, so a raised block waits for it to be ready", "[bash]")
{
    BashState run = RequestBashAt(100.0, false);
    Takes takes;
    REQUIRE_FALSE(AdvanceBash(run, Drawn(), 100.0, takes.Fn()));
    REQUIRE(takes.asked == std::vector<BashCommand>{BashCommand::RaiseBlock});
    REQUIRE(run.raised);
    REQUIRE(run.blockAskedAt == 100.0);
    REQUIRE(run.step == BashStep::Blocking);
    REQUIRE(run.waited);

    // Up, but its animation not yet ready: however long, no bash.
    REQUIRE_FALSE(AdvanceBash(run, Blocking(), 100.05, takes.Fn()));
    REQUIRE(run.blockUpAt == 100.05);
    REQUIRE(run.steadySince < 0.0);
    REQUIRE_FALSE(AdvanceBash(run, Blocking(), 100.9, takes.Fn()));
    REQUIRE(takes.asked.size() == 1);
    // Its blockStartOut heard: the bash, on that step.
    BashSeen ready = Blocking();
    ready.blockOuts = 1;
    REQUIRE_FALSE(AdvanceBash(run, ready, 100.95, takes.Fn()));
    REQUIRE(takes.asked.back() == BashCommand::Bash);
    REQUIRE(run.steadySince == 100.95);
    REQUIRE(run.step == BashStep::Bashing);
}

TEST_CASE("a swing of their own, or the block dropped, waits for the block to be ready again", "[bash]")
{
    BashState run = RequestBashAt(100.0, false);
    Takes takes;
    // Mid-swing at the request, the block ready from before the swing: wait,
    // hands not free.
    BashSeen swinging = Swinging();
    swinging.blockOuts = 3;
    REQUIRE_FALSE(AdvanceBash(run, swinging, 100.0, takes.Fn()));
    REQUIRE(run.waited);
    REQUIRE(run.freeSince < 0.0);
    REQUIRE(takes.asked.empty());
    // Free and blocking, but no ready event since the swing: the old one
    // does not count.
    BashSeen blocking = Blocking();
    blocking.blockOuts = 3;
    REQUIRE_FALSE(AdvanceBash(run, blocking, 100.5, takes.Fn()));
    REQUIRE(run.freeSince == 100.5);
    REQUIRE(takes.asked.empty());
    // Ready, then dropped before the step that would take it: it waits for
    // the next ready event.
    BashSeen dropped = Drawn();
    dropped.blockOuts = 4;
    REQUIRE_FALSE(AdvanceBash(run, dropped, 100.6, takes.Fn()));
    blocking.blockOuts = 4;
    REQUIRE_FALSE(AdvanceBash(run, blocking, 100.7, takes.Fn()));
    REQUIRE(takes.asked.empty());
    blocking.blockOuts = 5;
    REQUIRE_FALSE(AdvanceBash(run, blocking, 100.75, takes.Fn()));
    REQUIRE(takes.asked == std::vector<BashCommand>{BashCommand::Bash});
    REQUIRE(run.steadySince == 100.75);
}

TEST_CASE("refusals are counted and named at the deadline", "[bash]")
{
    // The block turned away, every tick, to the deadline.
    BashState run = RequestBashAt(100.0, false);
    Takes refuse;
    refuse.raise = false;
    REQUIRE_FALSE(AdvanceBash(run, Drawn(), 100.0, refuse.Fn()));
    REQUIRE_FALSE(AdvanceBash(run, Drawn(), 100.5, refuse.Fn()));
    REQUIRE(run.blockRefusals == 2);
    REQUIRE(run.step == BashStep::Ready);
    REQUIRE(Over(AdvanceBash(run, Drawn(), 102.0, refuse.Fn())) == "deadline, block refused");

    // The bash turned away from the block.
    BashState blocked = RequestBashAt(100.0, false);
    Takes noBash;
    noBash.bash = false;
    REQUIRE_FALSE(AdvanceBash(blocked, Blocking(), 100.0, noBash.Fn()));
    REQUIRE(blocked.bashRefusals == 1);
    REQUIRE(Over(AdvanceBash(blocked, Blocking(), 102.0, noBash.Fn())) == "deadline, bash refused from the block");
}

TEST_CASE("the deadline names where the request was stuck", "[bash]")
{
    Takes takes;
    BashState sheathed = RequestBashAt(100.0, false);
    REQUIRE_FALSE(AdvanceBash(sheathed, {}, 101.0, takes.Fn()));
    REQUIRE(Over(AdvanceBash(sheathed, {}, 102.0, takes.Fn())) == "deadline, weapon never drawn");

    BashState swinging = RequestBashAt(100.0, false);
    REQUIRE_FALSE(AdvanceBash(swinging, Swinging(), 101.99, takes.Fn()));
    REQUIRE(Over(AdvanceBash(swinging, Swinging(), 102.0, takes.Fn())) == "deadline, still mid-swing");

    // Raised but never up.
    BashState raised = RequestBashAt(100.0, false);
    REQUIRE_FALSE(AdvanceBash(raised, Drawn(), 100.0, takes.Fn()));
    REQUIRE(Over(AdvanceBash(raised, Drawn(), 102.0, takes.Fn())) == "deadline, block never up");

    // Up, but a swing keeps it from being ready.
    BashState unsteady = RequestBashAt(100.0, false);
    REQUIRE_FALSE(AdvanceBash(unsteady, Drawn(), 100.0, takes.Fn()));
    REQUIRE_FALSE(AdvanceBash(unsteady, Blocking(), 100.1, takes.Fn()));
    REQUIRE_FALSE(AdvanceBash(unsteady, Swinging(), 100.2, takes.Fn()));
    REQUIRE(Over(AdvanceBash(unsteady, Swinging(), 102.0, takes.Fn())) == "deadline, never steady");

    BashSeen gone;
    gone.holder = false;
    REQUIRE(Over(AdvanceBash(unsteady, gone, 100.0, takes.Fn())) == "holder vanished");
}

TEST_CASE("taken, then watched: never bashed, an attack but no bash, or still bashing", "[bash]")
{
    Takes takes;
    BashState never = RequestBashAt(100.0, true);
    REQUIRE_FALSE(AdvanceBash(never, Blocking(), 100.0, takes.Fn()));
    REQUIRE(never.power);
    REQUIRE_FALSE(AdvanceBash(never, Blocking(), 101.49, takes.Fn()));
    REQUIRE(Over(AdvanceBash(never, Blocking(), 101.5, takes.Fn())) == "taken, never bashed");

    BashState other = RequestBashAt(100.0, false);
    REQUIRE_FALSE(AdvanceBash(other, Blocking(), 100.0, takes.Fn()));
    REQUIRE_FALSE(AdvanceBash(other, Swinging(), 100.2, takes.Fn()));
    REQUIRE(other.otherAttackState == 2);
    REQUIRE(Over(AdvanceBash(other, Blocking(), 101.5, takes.Fn())) == "taken, an attack but no bash");

    BashState stuck = RequestBashAt(100.0, false);
    REQUIRE_FALSE(AdvanceBash(stuck, Blocking(), 100.0, takes.Fn()));
    REQUIRE_FALSE(AdvanceBash(stuck, Bashing(), 100.2, takes.Fn()));
    REQUIRE(Over(AdvanceBash(stuck, Bashing(), 101.5, takes.Fn())) == "watch over, still bashing");
    REQUIRE(stuck.sawBash);
}
