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
// Once profiles are out in the world, renaming a WireName silently breaks
// every one that used it, for everyone, with no error message: add freely,
// rename never. Until then (nothing has shipped) the names are as free to
// change as the code.

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
[[nodiscard]] std::string_view WireName(Hand v) noexcept;
[[nodiscard]] std::string_view WireName(StatusKind v) noexcept;
[[nodiscard]] std::string_view WireName(TypeKind v) noexcept;
[[nodiscard]] std::string_view WireName(DamageKind v) noexcept;

// Parsing is fallible on purpose. A profile written by a NEWER version of the
// mod will name things this build has never heard of, and the right response is
// to drop that rule with a log line rather than refuse the whole file -- a
// profile from the future should degrade, not explode.
[[nodiscard]] std::optional<SubjectKind> SubjectFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<PredicateKind> PredicateFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<ActionTargetKind> ActionTargetFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<ActionKind> ActionFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<Hand> HandFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<StatusKind> StatusFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<TypeKind> TypeFromWireName(std::string_view s) noexcept;
[[nodiscard]] std::optional<DamageKind> DamageFromWireName(std::string_view s) noexcept;

// A slug: lowercase ASCII letters and digits, hyphen-separated, no leading,
// trailing or doubled hyphen. Nothing else round-trips safely through a JSON
// file, a forum post and a filesystem -- and, the point here, no translated
// string can satisfy it. A test holds this line mechanically so "never
// localise the wire names" is enforced rather than asked.
[[nodiscard]] bool IsWireName(std::string_view s) noexcept;

// --- display text, all of it translatable ----------------------------------

[[nodiscard]] std::string_view DisplayName(SubjectKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(PredicateKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(ActionTargetKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(ActionKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(Hand v) noexcept;
[[nodiscard]] std::string_view DisplayName(StatusKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(TypeKind v) noexcept;
[[nodiscard]] std::string_view DisplayName(DamageKind v) noexcept;

// One line of help, for a tooltip. Kept beside the names so a new predicate
// cannot be added without someone deciding what it means to a player.
[[nodiscard]] std::string_view Describe(PredicateKind v) noexcept;
[[nodiscard]] std::string_view Describe(ActionKind v) noexcept;

// The thing an action names, as the menu heads it once the verb is the
// heading's: the equips' kind of thing ("weapon", "spell"), the charge
// policies' gem ("strongest soul gem"). Empty for an action the menu
// shows by its display name. Lowercase; the panel capitalises where it
// stands alone. Display text, never parsed -- which is why it is here and
// not cut out of the display name.
[[nodiscard]] std::string_view Noun(ActionKind v) noexcept;

// --- argument shape --------------------------------------------------------

// What kind of number a predicate's conditionArg is, if any.
//
// The UI needs this to pick a widget -- a 0-100% slider is the wrong control for
// a distance in game units and a nonsense one for "is in combat". Keeping it
// here rather than in the UI means the same answer validates the argument when a
// hand-edited profile is loaded.
enum class ArgumentKind : std::uint8_t
{
    None,    // the predicate takes no argument
    Percent, // 0.0 to 1.0, shown as 0-100%
};

[[nodiscard]] ArgumentKind ArgumentFor(PredicateKind predicate) noexcept;

} // namespace ft
