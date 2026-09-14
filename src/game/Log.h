#pragma once
// The two log channels, and the one call that writes both.
//
//   FollowerTactics.log           prose, for a human tailing it while playing:
//                                 every line, filtered by the ini's level
//   FollowerTactics.events.jsonl  the game events, one JSON object per line,
//                                 whatever the level
//
// Neither is hand-maintained against the other: a game event is written once,
// here, and comes out of both. docs/LOGGING.md is the machinery and the
// levels; docs/EVENTS.md is which events there are and what each carries.
//
// Every line names its module, and the module is a FIELD rather than a string
// somebody typed at the front of the message:
//
//   [14:02:11.4] [info] [tactics]   Lydia (000A2C94) FIRED rule 0 "emergency heal"
//   [14:02:11.4] [info] [packages]  slot 3 aims at Lydia (000A2C94)
//
// so `grep '\[packages\]'` is a thing you can do. Call it as
// `ft::log::packages.info(...)`, never `logger::info("packages: ...")`.

#include "core/EventRing.h"
#include "core/LogEvent.h"

#include <cstdint>
#include <fmt/format.h>
#include <span>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class TESForm;
} // namespace RE

namespace ft::log
{

// Reads FollowerTactics.ini, opens both files, and installs the prose logger.
// Called first thing in SKSEPluginLoad, before anything can log.
void Init();

// Whether a line at this level would be written at all. Only worth asking
// before building something expensive purely to log it -- joining a party
// into a string, walking an inventory -- since every call below asks anyway.
[[nodiscard]] bool Enabled(Level level) noexcept;

// Whether the events file is open: `events = true` in the ini, and the file
// could be made.
[[nodiscard]] bool EventsOn() noexcept;

// The game events held in memory with a sequence number above `after`, oldest
// first, copied out under the lock: for the panel, which draws from the copy.
[[nodiscard]] std::vector<LoggedEvent> RecentEvents(std::uint64_t after);

// An actor's id and name, as an event carries them: the id is what a query
// keys on, the name is for the human reading the query's output and is never
// read back. Follows PROFILES.md's convention, deliberately.
[[nodiscard]] std::uint32_t IdOf(RE::Actor *actor) noexcept;
[[nodiscard]] std::string NameOf(RE::Actor *actor);
[[nodiscard]] std::string NameOf(const RE::TESForm *form);

// An actor as an event names one: its reference id, its base id (the leveled
// template's base where there is one) and its name, appended under the three
// keys given. The keys must be string literals: a Field keeps a view of its
// key, and one built at run time would dangle. Read at the moment of the
// event, since the actor may be unloaded by the time anything follows.
void AppendActor(std::vector<Field> &fields, std::string_view idKey, std::string_view baseKey, std::string_view nameKey,
                 std::uint32_t actorId);

// A form's id and name, likewise.
void AppendForm(std::vector<Field> &fields, std::string_view idKey, std::string_view nameKey, std::uint32_t formId);

// A module of the mod: one per source file that logs, named for the file.
// Constructible at namespace scope with no static-initialisation order
// problem, because it holds nothing but its own name.
class Module
{
  public:
    constexpr explicit Module(std::string_view name) noexcept : name_(name)
    {
    }

    template <class... Args> void debug(fmt::format_string<Args...> f, Args &&...args) const
    {
        Say(Level::Debug, std::move(f), std::forward<Args>(args)...);
    }
    template <class... Args> void info(fmt::format_string<Args...> f, Args &&...args) const
    {
        Say(Level::Info, std::move(f), std::forward<Args>(args)...);
    }
    template <class... Args> void warn(fmt::format_string<Args...> f, Args &&...args) const
    {
        Say(Level::Warn, std::move(f), std::forward<Args>(args)...);
    }
    template <class... Args> void error(fmt::format_string<Args...> f, Args &&...args) const
    {
        Say(Level::Error, std::move(f), std::forward<Args>(args)...);
    }
    // For a line whose weight is only known when it happens: a spell leaving
    // a hand is news when it is our cast, and noise when it is the follower's.
    template <class... Args> void at(Level level, fmt::format_string<Args...> f, Args &&...args) const
    {
        Say(level, std::move(f), std::forward<Args>(args)...);
    }

    // A game event: the JSON line in the events file AND the prose line in
    // the log, from this one call. `name` is a name from docs/EVENTS.md's
    // catalogue, `fields` what that row says it carries beyond the envelope,
    // and the trailing format string is how the same fact reads in prose.
    // The level filters the prose line only: turning the log down must not
    // erase the record of a fight, and the event is kept in memory whatever
    // the ini says, so it is always formatted.
    //
    //   ft::log::tactics.event(Level::Info, "rule.fired", actor,
    //       {{"ruleIndex", i}, {"ruleName", rule.label}, {"healthPct", pct}},
    //       "{} FIRED rule {} \"{}\"", Describe(actor), i, rule.label);
    template <class... Args>
    void event(Level level, std::string_view name, RE::Actor *who, std::span<const Field> fields,
               fmt::format_string<Args...> f, Args &&...args) const
    {
        const auto prose = fmt::format(std::move(f), std::forward<Args>(args)...);
        Emit(level, name, who, fields, name_, prose);
    }

    // The same, written as a braced list at the call site. The span form is
    // for a list built up first, an actor's ids appended to a rule's fields.
    template <class... Args>
    void event(Level level, std::string_view name, RE::Actor *who, Fields fields, fmt::format_string<Args...> f,
               Args &&...args) const
    {
        event(level, name, who, std::span<const Field>(fields.begin(), fields.size()), std::move(f),
              std::forward<Args>(args)...);
    }

    // The same, for an event about no follower in particular (the party
    // changing, the master switch).
    template <class... Args>
    void event(Level level, std::string_view name, Fields fields, fmt::format_string<Args...> f, Args &&...args) const
    {
        event(level, name, nullptr, fields, std::move(f), std::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr std::string_view name() const noexcept
    {
        return name_;
    }

  private:
    template <class... Args> void Say(Level level, fmt::format_string<Args...> f, Args &&...args) const
    {
        if (!Enabled(level))
            return;
        Write(level, name_, fmt::format(std::move(f), std::forward<Args>(args)...));
    }

    std::string_view name_;
};

// One per source file that logs. The name is what appears in the brackets and
// what a query greps for, so it stays lower case and stays put.
inline constexpr Module actions{"actions"};
inline constexpr Module forms{"forms"};
inline constexpr Module hits{"hits"};
inline constexpr Module packages{"packages"};
inline constexpr Module pins{"pins"};
inline constexpr Module plugin{"plugin"};
inline constexpr Module profiles{"profiles"};
inline constexpr Module sensors{"sensors"};
inline constexpr Module tactics{"tactics"};
inline constexpr Module ui{"ui"};

// Not for call sites -- Module's templates above are the API. These are out of
// line so that neither spdlog nor the sinks have to be included here.
void Write(Level level, std::string_view module, std::string_view text);
void Emit(Level level, std::string_view event, RE::Actor *who, std::span<const Field> fields, std::string_view module,
          std::string_view prose);

} // namespace ft::log
