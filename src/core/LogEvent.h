#pragma once
// One structured event, as one line of JSON.
//
// The prose log is for a human tailing it while playing; this is the other
// half of the job -- "did they cast twice", a before/after across two runs, a
// file attached to a bug report that needs no tooling to read. docs/LOGGING.md
// says which events exist and what each carries.
//
// Pure, so it is testable: an event is a level, a name, and fields, and this
// turns those into the line. Everything that needs the game -- the actor's
// name, the clock, the file itself -- lives in src/game/Log.h and calls in
// here. That split is the architectural rule (CLAUDE.md), and it is what lets
// the envelope's shape be pinned by a test instead of by reading the log.

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ft::log
{

// Four levels, by what belongs in each (docs/LOGGING.md has the table):
// error, a feature is broken until something changes; warn, the mod adapted
// or skipped something on its own; info, a state change worth narrating while
// playing; debug, everything else.
enum class Level : std::uint8_t
{
    Debug,
    Info,
    Warn,
    Error
};

[[nodiscard]] const char *ToString(Level level) noexcept;

// Parses "error"/"warn"/"info"/"debug", case-insensitively. Nothing else
// parses, and the caller decides what an unreadable setting falls back to --
// core has no opinion and no way to complain.
[[nodiscard]] bool ParseLevel(std::string_view text, Level &out) noexcept;

// A form id as the log writes it: "0xFF000DE0", eight digits, upper case.
// Every id in every event goes through here, so a query keys on one spelling.
// Not noexcept: it returns a std::string, so it allocates like any other.
[[nodiscard]] std::string Id(std::uint32_t formID);

// A number as an event writes it: three decimals, so a duration reads in
// milliseconds and a fraction to a tenth of a percent. A float widened to a
// double otherwise carries its own noise into the line -- a 2.006 s hold was
// written 2.0060348510742188 -- which reads badly and diffs worse.
[[nodiscard]] double Rounded(double value) noexcept;

// An actor in a list, named as an event names any actor: the reference id,
// the base id and the name (src/game/Log.h, AppendActor), so a party reads
// the same as a rule's subject.
struct NamedActor
{
    std::uint32_t formId{0};
    std::uint32_t baseFormId{0};
    std::string name;
};

// One key and its value. The constructors are what decide how a value is
// spelled in JSON -- an id is a string, a count is a number, a list of actors
// is an array of ids -- so a call site writes the value it has and never a
// pre-formatted one.
class Field
{
  public:
    Field(std::string_view key, std::string value);
    Field(std::string_view key, const char *value);
    Field(std::string_view key, std::string_view value);
    Field(std::string_view key, bool value);

    // Every integer and every float, rather than a list of the fixed-width
    // ones: a call site passing a LONG, a DWORD or a std::size_t should not
    // have to cast, and one that had to would eventually cast wrongly.
    template <class T>
        requires std::integral<T> && (!std::same_as<T, bool>)
    Field(std::string_view key, T value) : key_(key), value_(static_cast<std::int64_t>(value))
    {
    }

    template <class T>
        requires std::floating_point<T>
    Field(std::string_view key, T value) : key_(key), value_(Rounded(static_cast<double>(value)))
    {
    }

    // A list of actors -- a party, the followers under tactics -- as an array
    // of objects, each with formId, baseFormId and name.
    Field(std::string_view key, std::vector<NamedActor> actors);

    [[nodiscard]] std::string_view key() const noexcept
    {
        return key_;
    }

    using Value = std::variant<std::int64_t, double, bool, std::string, std::vector<NamedActor>>;

    [[nodiscard]] const Value &value() const noexcept
    {
        return value_;
    }

  private:
    std::string_view key_;
    Value value_;
};

using Fields = std::initializer_list<Field>;

// The envelope, then the fields in the order they were written.
//
//   {"ts":"...","level":"info","plugin":"FollowerTactics","version":"0.1.0",
//    "event":"rule.fired","ruleIndex":0,...}
//
// No trailing newline: the sink adds it. `timestamp` is passed in rather than
// read here because a clock is not something core has -- and because a test
// that pins the envelope needs the line to be the same every run.
//
// There is deliberately no schema-version field. That belongs to the
// co-save (PROFILES.md's `schema`), which outlives the build that wrote it;
// a log line has no such lifetime.
[[nodiscard]] std::string FormatEvent(Level level, std::string_view event, std::string_view version,
                                      std::string_view timestamp, std::span<const Field> fields);

// The same, written as a braced list at the call site. The game layer takes
// the span overload above because it prepends the follower's id and name to
// what the call site wrote; a test writes the list directly.
[[nodiscard]] inline std::string FormatEvent(Level level, std::string_view event, std::string_view version,
                                             std::string_view timestamp, Fields fields)
{
    return FormatEvent(level, event, version, timestamp, std::span<const Field>(fields.begin(), fields.size()));
}

} // namespace ft::log
