#pragma once
// The two log channels, and the one call that writes both.
//
//   FollowerTactics.log         prose, for a human tailing it while playing
//   FollowerTactics.events.jsonl  one JSON object per line, for querying after
//
// Neither is hand-maintained against the other: an event is written once, here,
// and comes out of both. docs/LOGGING.md is the design -- what belongs at each
// level, and the catalogue of events.
//
// Every line names its module, and the module is a FIELD rather than a string
// somebody typed at the front of the message:
//
//   [14:02:11.4] [info] [tactics]   Lydia (000A2C94) FIRED rule 0 "emergency heal"
//   [14:02:11.4] [info] [packages]  slot 3 aims at Lydia (000A2C94)
//
// so `grep '\[packages\]'` is a thing you can do. Call it as
// `ft::log::packages.info(...)`, never `logger::info("packages: ...")`.

#include "core/LogEvent.h"

#include <fmt/format.h>
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

// An actor's id and name, as an event carries them: the id is what a query
// keys on, the name is for the human reading the query's output and is never
// read back. Follows PROFILES.md's convention, deliberately.
[[nodiscard]] std::uint32_t IdOf(RE::Actor *actor) noexcept;
[[nodiscard]] std::string NameOf(RE::Actor *actor);
[[nodiscard]] std::string NameOf(const RE::TESForm *form);

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

    // An event: the JSON line on the sidecar AND the prose line on the log,
    // from this one call. `name` is a name from docs/LOGGING.md's catalogue,
    // `fields` what that row says it carries beyond the envelope, and the
    // trailing format string is how the same fact reads in prose.
    //
    //   ft::log::tactics.event(Level::Info, "rule.fired", actor,
    //       {{"ruleIndex", i}, {"ruleName", rule.label}, {"healthPct", pct}},
    //       "{} FIRED rule {} \"{}\"", Describe(actor), i, rule.label);
    template <class... Args>
    void event(Level level, std::string_view name, RE::Actor *who, Fields fields, fmt::format_string<Args...> f,
               Args &&...args) const
    {
        if (!Enabled(level))
            return;
        const auto prose = fmt::format(std::move(f), std::forward<Args>(args)...);
        Emit(level, name, who, fields, name_, prose);
    }

    // The same, for an event about no actor in particular (the packages, the
    // tick, a hook that failed to install).
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
void Emit(Level level, std::string_view event, RE::Actor *who, Fields fields, std::string_view module,
          std::string_view prose);

} // namespace ft::log
