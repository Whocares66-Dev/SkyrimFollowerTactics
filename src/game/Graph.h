#pragma once
// The one sink on the animation graphs of the actors we act on. A request
// watches its actor's graph from where it starts listening
// (core/GraphEvents.h) and reads what the watch has heard at each step; an
// event a watch waits on steps whatever is in flight at once, on the game
// thread as soon as the task queue drains (StepInFlightNow), one task for
// the event, which queues nothing further (CLAUDE.md, "A task must never
// re-arm itself"). While any watch is open on an actor, a debug build logs
// every event of theirs, recorded or not.

#include "core/GraphEvents.h"

#include <span>

namespace RE
{
class Actor;
} // namespace RE

namespace ft::game
{

// A watch on the actor's graph from now: the events that step what is in
// flight, and the fires that are the request's own. The sink goes on the
// graph where it is missing: the graph is rebuilt on a cell change and a 3D
// reload, and a sink on the old one hears nothing. No watch for no actor.
// Game thread.
[[nodiscard]] ft::GraphWatch WatchGraph(RE::Actor *actor, ft::GraphTags wakes = {},
                                        std::span<const ft::OwnFire> fires = {});

} // namespace ft::game
