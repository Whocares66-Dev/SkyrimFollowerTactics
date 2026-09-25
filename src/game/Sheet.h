#pragma once
// The pieces every sheet is worded with -- the inventory pages, the magic
// pages, the character and skills sheets: a number as text, a labelled
// row, a form's name. Three files had a copy each until 2026-09-11.

#include "game/Sensors.h"

#include <string>

namespace RE
{
class TESForm;
class TESObjectREFR;
} // namespace RE

namespace ft::game
{

// One number, printf-formatted: "%.0f", "%+.1f".
[[nodiscard]] std::string Fmt(const char *fmt, double value);

// A row of a sheet: its label and its value, nothing else set.
[[nodiscard]] SheetRow Row(std::string label, std::string value);

// A form's name, or `fallback` for a null form or a nameless one. A
// reference -- an actor -- is named as the game shows it, a renamed
// follower by the name given: TESForm's own GetName does not answer for a
// reference, and Serana's sheet read "?" until this overload (2026-09-11).
[[nodiscard]] std::string NameOr(const RE::TESForm *form, const char *fallback);
[[nodiscard]] std::string NameOr(RE::TESObjectREFR *ref, const char *fallback);

// A form's name, or nothing.
[[nodiscard]] inline std::string NameOf(const RE::TESForm *form)
{
    return NameOr(form, "");
}

// The Equipped row of a page: a tick, and the pin glyph beside it when a
// pin holds the thing. Only on a page of something equipped; a bare "no"
// says nothing. Marked as the Equipped row (SheetRow::equipped), which
// is how MarkPins finds it to add the pin.
[[nodiscard]] SheetRow EquippedRow(bool pinned);

// A soul's level as the game names it, "Petty" to "Grand"; "Empty" for
// none.
[[nodiscard]] const char *SoulName(RE::SOUL_LEVEL level);

// The two glyphs an Equipped row is made of, Font Awesome's check and
// thumbtack: the same codepoints the panel's own Glyph table uses, so the
// row reads as the Inventory and Magic tabs' cells do.
inline constexpr unsigned kGlyphTick = 0xF00C;
inline constexpr unsigned kGlyphPin = 0xF08D;

} // namespace ft::game
