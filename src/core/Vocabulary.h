#pragma once
// Two names for every value in the rule vocabulary, and what shape each
// predicate's argument takes.
//
// THE TWO NAMES ARE SEPARATE ON PURPOSE, AND THAT IS THE WHOLE POINT.
//
//   WireName    a stable ASCII identifier. This is what a profile contains on
//               disk. Never translated, never renamed.
//   DisplayName human-readable text for the UI. Localise this freely; nothing
//               ever parses it.
//
// Collapsing them into one table looks tidier and is a trap. Profiles are meant
// to be shared -- posted on a forum, copied between installs, diffed -- so the
// names in them are a public data format. If the UI showed those same strings,
// then the first person to translate the UI would have translated the file
// format along with it: a French player's profile would say "SanteEnDessousDe"
// and would not load anywhere else. Nobody would intend that, and nothing in a
// single-table design would stop it.
//
// Split, the mistake is not available. DisplayName is obviously translatable and
// obviously not a key; WireName is obviously an identifier. Only WireName has a
// parse function, so keying logic off display text does not compile.
//
// Renaming a WireName silently breaks every exported profile that used it, for
// everyone, with no error message. Add freely; rename never.

#include "Rule.h"

#include <optional>
#include <string_view>

namespace ft
{

// --- the wire format -------------------------------------------------------

[[nodiscard]] std::string_view WireName(SubjectKind v) noexcept;
[[nodiscard]] std::string_view WireName(PredicateKind v) noexcept;
[[nodiscard]] std::string_view WireName(ActionTargetKind v) noexcept;
[[nodiscard]] std::string_view WireName(ActionKind v) noexcept;

// Parsing is fallible on purpose. A profile written by a NEWER version of the
// mod will name things this build has never heard of, and the right response is
// to drop that rule with a log line rather than refuse the whole file -- a
// profile from the future should degrade, not explode.
[[nodiscard]] std::optional<SubjectKind> SubjectFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<PredicateKind> PredicateFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<ActionTargetKind> ActionTargetFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<ActionKind> ActionFromWireName(std::string_view s) noexcept;

// ASCII letters and digits only, starting with a letter. Nothing else
// round-trips safely through a JSON file, a forum post and a filesystem -- and,
// the point here, no translated string can satisfy it. A test holds this line
// mechanically so "never localise the wire names" is enforced rather than asked.
[[nodiscard]] bool IsWireName(std::string_view s) noexcept;

// --- display text, all of it translatable ----------------------------------

[[nodiscard]] std::string_view DisplayName(SubjectKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(PredicateKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(ActionTargetKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(ActionKind v) noexcept;

// One line of help, for a tooltip. Kept beside the names so a new predicate
// cannot be added without someone deciding what it means to a player.
[[nodiscard]] std::string_view Describe(PredicateKind v) noexcept;
[[nodiscard]] std::string_view Describe(ActionKind v) noexcept;

// --- argument shape --------------------------------------------------------

// What kind of number a predicate's conditionArg is, if any.
//
// The UI needs this to pick a widget -- a 0-100% slider is the wrong control for
// a distance in game units and a nonsense one for "is in combat". Keeping it
// here rather than in the UI means the same answer validates the argument when a
// hand-edited profile is loaded.
enum class ArgumentKind : std::uint8_t
{
    None,     // the predicate takes no argument
    Percent,  // 0.0 to 1.0, shown as 0-100%
    Distance, // game units
    Count,    // a whole number of actors
};

[[nodiscard]] ArgumentKind ArgumentFor(PredicateKind predicate) noexcept;

} // namespace ft
