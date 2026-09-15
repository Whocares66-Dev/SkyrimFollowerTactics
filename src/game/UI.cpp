// The in-game panel, drawn with ImGui via SKSE Menu Framework: the rule
// editor and the character sheet -- inventory, magic, effects, summons,
// character, skills -- of a follower and of the player. Why a rule did or
// did not act is not a column here: a verdict lasted one tick and blanked
// when the fight ended, so it goes to the events log (docs/EVENTS.md).
//
// Everything here runs on the render thread. It never touches an RE::Actor and
// never reaches into live engine state -- ObserveFollowers() hands back a copy.
// Reading a follower's inventory from the render thread would be a good way to
// crash the game.

#include "game/UI.h"

#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Vocabulary.h"
#include "game/Log.h"
#include "game/Pins.h"
#include "game/Sheet.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include <SKSEMenuFramework.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

namespace Im = ImGuiMCP;

// --- set aside -------------------------------------------------------------

bool TakesSpell(ft::ActionKind action);
void SlashCell();

// A rule set aside for what it names keeps its text, its place and its
// switch state, greyed, and its cells still open so the player can name
// something else. The reasons, on the switch and on the cell concerned.
constexpr const char *kNotAvailable = "Item or ability not available";
constexpr const char *kFollowerAway = "Follower not available";
bool ConditionAvailable(const ft::Rule &rule, const FollowerView &view);
bool TargetAvailable(const ft::Rule &rule, const FollowerView &view);

// The one grey for everything set aside: a shadowed row, a banned row, an
// off rule. The theme's disabled text colour, so it follows the theme.
Im::ImU32 DimColor()
{
    return Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f);
}

// A region that is off: greyed in the same grey as a shadowed row, and
// taking no input. ImGui's own disabled look only fades the text, which
// reads brighter than the rows'; this paints it the rows' grey.
void BeginDimmed(bool dim)
{
    Im::PushStyleVar(Im::ImGuiStyleVar_DisabledAlpha, 1.0f);
    Im::PushStyleColor(Im::ImGuiCol_Text, dim ? DimColor() : Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
    Im::BeginDisabled(dim);
}

void EndDimmed()
{
    Im::EndDisabled();
    Im::PopStyleColor(1);
    Im::PopStyleVar(1);
}

// The same grey over what follows, to the end of the scope, and nothing
// else: unlike BeginDimmed's region, what is drawn stays live. For a row
// set aside, for a cell greyed with its row but still answering, for a
// perk whose conditions fail. One guard, so every greyed-but-live thing
// on the panel is greyed by the same line.
class DimText
{
  public:
    explicit DimText(bool dim) : dim_(dim)
    {
        if (dim_)
            Im::PushStyleColor(Im::ImGuiCol_Text, DimColor());
    }
    ~DimText()
    {
        if (dim_)
            Im::PopStyleColor(1);
    }
    DimText(const DimText &) = delete;
    DimText &operator=(const DimText &) = delete;

  private:
    bool dim_;
};

// The colour of what needs seeing to: a bag past its capacity.
constexpr Im::ImVec4 kAlarm{0.95f, 0.45f, 0.40f, 1.0f};

// --- rule text -------------------------------------------------------------

// --- drawing ---------------------------------------------------------------

// Width of the widest of a set of labels. Measured rather than hardcoded so the
// columns survive translation, where "Magicka" might be "Zauberkraft".
// Spacing between the three order buttons. Named because it is used twice --
// once to lay them out and once to centre them -- and the two silently
// disagreeing (2px of layout, ItemSpacing of maths) is what pushed the group
// off centre and clipped the delete button.
constexpr float kOrderGap = 2.0f;

// Horizontal breathing room inside every table cell.
//
// Zero here made a full-width button reach the cell border, which is what the
// If/Then cells want -- but it also stripped the left margin off the header
// row and the number column, which is not what those want. So the padding
// stays, and the two button cells opt out of it themselves.
constexpr float kCellPadX = 6.0f;
// The gap between an item's name and its first mark, and between marks:
// tighter than a cell's padding, since the marks belong to the name.
constexpr float kBadgeGap = 3.0f;
// Vertical padding of the inventory and magic tables' cells.
constexpr float kCellPadY = 4.0f;
// A drawer's or a sheet's inner tables sit this far inside their cell.
constexpr float kTablePad = 2.0f * kCellPadX + 8.0f;

float WidestLabel(std::initializer_list<const char *> labels)
{
    float widest = 0.0f;
    for (const char *label : labels)
        widest = (std::max)(widest, Im::CalcTextSize(label, nullptr, false, -1.0f).x);
    return widest;
}

float TextWidth(const std::string &text)
{
    return Im::CalcTextSize(text.c_str(), nullptr, false, -1.0f).x;
}

// Place text so its RIGHT edge lands on rightX. Right-aligning the labels is
// what makes a label column read as a column: left-aligned, the gap between
// each label and the thing it names is a different width on every row.
void TextRightAlignedAt(float rightX, const std::string &text)
{
    Im::SameLine((std::max)(0.0f, rightX - TextWidth(text)), -1.0f);
    Im::AlignTextToFramePadding();
    Im::Text("%s", text.c_str());
}

// One row: a labelled bar on the left, a labelled stat on the right.
//
// Both halves are drawn on the SAME ImGui line, and that is the point. Drawn as
// two independent groups they drift apart vertically, because a progress bar is
// frame-height and a line of text is not -- which is exactly what went wrong
// before. Sharing a line makes them share a baseline by construction.
struct RowGeometry
{
    float barLabelRight{0.0f};
    float barLeft{0.0f};
    float barWidth{240.0f};
    float statLabelRight{0.0f};
    float valueLeft{0.0f};
};

void BreakdownTooltip(const ft::Breakdown &b); // below, with the sheets

void DrawStatRow(const RowGeometry &g, const char *barLabel, const ft::Stat &stat, Im::ImVec4 barColour,
                 const char *statLabel, const std::function<void()> &drawValue, const ft::Breakdown &breakdown = {})
{
    Im::SetCursorPosX((std::max)(0.0f, g.barLabelRight - Im::CalcTextSize(barLabel).x));
    Im::AlignTextToFramePadding();
    Im::Text("%s", barLabel);

    Im::SameLine(g.barLeft, -1.0f);
    Im::PushStyleColor(Im::ImGuiCol_PlotHistogram, barColour);
    // The numbers centred over the bar, drawn by hand: ImGui's own overlay
    // sits just past the filled part, so it moved with the fill and the
    // three bars' numbers did not line up.
    Im::ProgressBar(stat.Pct(), Im::ImVec2(g.barWidth, 0.0f), "");
    Im::PopStyleColor(1);
    // Where the maximum comes from, on the bar, beside the number.
    if (!breakdown.empty() && Im::IsItemHovered(0))
        BreakdownTooltip(breakdown);
    const std::string overlay =
        std::to_string(static_cast<int>(stat.current)) + " / " + std::to_string(static_cast<int>(stat.max));
    if (auto *draw = Im::GetWindowDrawList())
    {
        const Im::ImVec2 lo = Im::GetItemRectMin();
        const Im::ImVec2 hi = Im::GetItemRectMax();
        const Im::ImVec2 size = Im::CalcTextSize(overlay.c_str());
        Im::ImDrawListManager::AddText(draw,
                                       {lo.x + (hi.x - lo.x - size.x) * 0.5f, lo.y + (hi.y - lo.y - size.y) * 0.5f},
                                       Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), overlay.c_str());
    }

    TextRightAlignedAt(g.statLabelRight, statLabel);

    Im::SameLine(g.valueLeft, -1.0f);
    Im::AlignTextToFramePadding();
    drawValue();
}

// Preset values offered for a predicate's argument.
//
// Dragon Age offered choices, not a number box -- "has low armour rating", not
// a slider -- and that is the right call for a tactics editor: the point is to
// express intent quickly, and nobody needs 37%. It also keeps the whole
// condition authorable in one cascade with no separate value column.
// How a predicate's value is written, wherever it appears.
//
// One function, so the menu and the row cannot disagree: the preset a player
// picks reads back exactly as the rule then reads. The comparison lives with
// the VALUE rather than in the predicate's name so it is printed once --
// "health < 50%", not "health below < 50%".
std::string ArgumentText(ft::PredicateKind predicate, float value)
{
    switch (ft::ArgumentFor(predicate))
    {
    case ft::ArgumentKind::Percent:
        return (ft::IsAbove(predicate) ? "> " : "< ") + std::to_string(std::lround(value * 100.0f)) + "%";
    case ft::ArgumentKind::None:
    default:
        return {};
    }
}

// The values offered for a predicate's argument.
//
// Dragon Age offered choices, not a number box -- "has low armour rating", not
// a slider -- and that is the right call for a tactics editor: the point is to
// express intent quickly, and nobody needs 37%. It also keeps the whole
// condition authorable in one cascade with no separate value column.
//
// Values only. Their text comes from ArgumentText so there is no second place
// for a label to drift out of step with what the rule actually says.
std::vector<float> PresetsFor(ft::PredicateKind predicate)
{
    switch (ft::ArgumentFor(predicate))
    {
    case ft::ArgumentKind::Percent:
        // A resistance is the one number here that commonly goes negative:
        // a weakness, from a curse, a race or a spell. So it gets 0 as well,
        // where "< 0%" is "weak to this" and "> 0%" "resists it at all" --
        // the questions worth asking about a weakness, and neither of them
        // expressible with the thresholds alone. Health, magicka, stamina
        // and armour never go below zero, so 0 would be a dead entry there.
        if (ft::IsResistance(predicate))
            return {0.0f, 0.25f, 0.50f, 0.75f};
        return {0.25f, 0.50f, 0.75f};
    case ft::ArgumentKind::None:
    default:
        return {};
    }
}

// The whole condition on one line: "Enemy health < 25%".
// Who a condition is about, by name where it names someone: the player, or
// one particular follower.
std::string SubjectText(const ft::Rule &r, const FollowerView &view)
{
    if (r.subject == ft::SubjectKind::Follower)
    {
        for (const auto &peer : view.peers)
            if (peer.id == r.subjectForm)
                return peer.name;
        return "Follower (away)";
    }
    return std::string(ft::DisplayName(r.subject));
}

std::string ConditionText(const ft::Rule &r, const FollowerView &view)
{
    // Who, a colon, then what: "Self: Attacked by Fire". The colon keeps
    // the two halves from having to agree grammatically.
    std::string text = SubjectText(r, view);
    text += ": ";
    // A status reads as the status: "Self Poisoned", not "Self Status";
    // a kind of being as the kind: "Enemy: Undead", "Enemy: Nord".
    if (r.predicate == ft::PredicateKind::Status)
    {
        text += ft::DisplayName(r.statusKind);
        return text;
    }
    if (r.predicate == ft::PredicateKind::Type)
    {
        text += ft::DisplayName(r.typeKind);
        return text;
    }
    // A resistance reads as "Resistance Fire", then lowest, highest or the
    // number; an attack as "Attacked by Fire".
    if (ft::IsResistance(r.predicate))
    {
        text += "Resistance ";
        text += ft::DisplayName(r.damageKind);
        if (r.predicate == ft::PredicateKind::ResistanceLowest)
            return text + " lowest";
        if (r.predicate == ft::PredicateKind::ResistanceHighest)
            return text + " highest";
    }
    else
    {
        text += ft::DisplayName(r.predicate);
        if (r.predicate == ft::PredicateKind::HitBy || r.predicate == ft::PredicateKind::HitType)
        {
            text += ' ';
            text += ft::DisplayName(r.damageKind);
        }
        // The party member: "Enemy: Attacking Self", "Enemy: Attacked by
        // Player", "... Attacking Lydia".
        if (r.predicate == ft::PredicateKind::Attacking || r.predicate == ft::PredicateKind::AttackedBy)
        {
            text += ' ';
            if (r.subjectForm == 0)
                text += ft::DisplayName(ft::SubjectKind::Player);
            else if (r.subjectForm == view.id)
                text += "Self";
            else
            {
                std::string name = "a follower (away)";
                for (const auto &peer : view.peers)
                    if (peer.id == r.subjectForm)
                        name = peer.name;
                text += name;
            }
        }
    }

    if (const std::string arg = ArgumentText(r.predicate, r.conditionArg); !arg.empty())
        text += ' ' + arg;
    return text;
}

// The Dragon Age cascade: subject, then condition, then value.
//
// This needs BeginMenu inside BeginPopup, NOT BeginCombo. Combos are a flat
// list and do not nest -- an earlier version of this editor used three
// side-by-side combos for exactly that reason, which works but reads nothing
// like the game it is copying.
//
// The validity matrix does real work here: a subject only offers predicates it
// can actually answer, so an impossible pair is not merely discouraged, it is
// unreachable. Verdict::InvalidCondition then only happens to a hand-edited
// profile.
// A button filling its table cell, with no border of its own, whose popup opens
// ANCHORED BENEATH IT rather than at the mouse.
//
// Both details matter. The cell already has a border, so a bordered button
// inside it is a box within a box. And an unanchored popup appears detached
// from the thing it belongs to -- it reads as a floating menu rather than as
// this row's condition being edited.
// Every icon on a row -- tick, double tick, plus, cross, the order carets,
// the back arrow -- is a Font Awesome glyph from the solid face the
// framework ships and loads (the inventory's category row proved it
// renders). Strokes through the draw list were tried first and read jagged
// beside ImGui's own checkbox tick; a font glyph is hinted and anti-aliased
// for free, and one em size keeps every icon on a row the same size.
enum class Glyph
{
    Cross,
    Plus,
    Tick,
    Pin,
    Ban,
    CaretRight,
    Up,
    Down,
    Back
};

unsigned Codepoint(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::Cross:
        return 0xF00D; // xmark
    case Glyph::Plus:
        return 0xF067; // plus
    case Glyph::Tick:
        return 0xF00C; // check
    case Glyph::Pin:
        return 0xF08D; // thumbtack
    case Glyph::Ban:
        return 0xF05E; // ban: a circle with a bar
    case Glyph::CaretRight:
        return 0xF0DA; // caret-right
    case Glyph::Up:
        return 0xF062; // arrow-up: a move, not a sort direction
    case Glyph::Down:
        return 0xF063; // arrow-down
    case Glyph::Back:
    default:
        return 0xF060; // arrow-left
    }
}

// A codepoint as UTF-8. Every Font Awesome icon sits in the U+F000 block, so
// the three-byte form is the only case.
std::string Utf8(unsigned codepoint)
{
    return {static_cast<char>(0xE0 | (codepoint >> 12)), static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)),
            static_cast<char>(0x80 | (codepoint & 0x3F))};
}

// A codepoint centred in the box [lo, hi], through the draw list, for a cell
// that is not a button. `scale` is the glyph's size as a fraction of the
// font's: a font glyph fills its whole em where a letter uses two thirds of
// it, so at full size a tall glyph like the pin touches the edges of a text
// row. The pin draws at kPinScale for a margin; the tick is a short glyph
// and fits at 1, as do buttons, whose frame is taller than the glyph.
constexpr float kPinScale = 0.75f;

// Where a glyph's ink lies within its em, at that scale, relative to the
// pen: the font's own glyph rectangle. A glyph is centred in its em by its
// advance and line height, and the ink of a crown sits high in the em where
// a bolt fills it; a mark laid beside text is centred on its ink instead,
// or the crown rides above the line the bolt sits on. Zero when the font
// has no such glyph.
struct InkBox
{
    float x0{0.0f}, y0{0.0f}, x1{0.0f}, y1{0.0f};
    [[nodiscard]] bool Empty() const noexcept
    {
        return x1 <= x0 || y1 <= y0;
    }
};

InkBox InkOf(unsigned codepoint, float scale)
{
    FontAwesome::PushSolid();
    Im::ImFont *font = Im::GetFont();
    const float s = font && font->FontSize > 0.0f ? Im::GetFontSize() * scale / font->FontSize : 0.0f;
    const Im::ImFontGlyph *glyph =
        font && codepoint <= 0xFFFF ? Im::ImFontManger::FindGlyph(font, static_cast<Im::ImWchar>(codepoint)) : nullptr;
    FontAwesome::Pop();
    if (!glyph || s <= 0.0f)
        return {};
    return {glyph->X0 * s, glyph->Y0 * s, glyph->X1 * s, glyph->Y1 * s};
}

// `onInk` centres the glyph's ink in the box rather than its em: for a mark
// beside text. The buttons and cells keep the em, where every glyph of a
// kind lands the same and a tick and a cross line up.
void DrawCodepoint(Im::ImDrawList *draw, unsigned codepoint, Im::ImVec2 lo, Im::ImVec2 hi, Im::ImU32 ink,
                   float scale = 1.0f, bool onInk = false)
{
    const std::string text = Utf8(codepoint);
    FontAwesome::PushSolid();
    const Im::ImFont *font = Im::GetFont();
    const float fontSize = Im::GetFontSize() * scale;
    Im::ImVec2 extent = Im::CalcTextSize(text.c_str(), nullptr, false, -1.0f);
    FontAwesome::Pop();
    if (!font)
        return;
    extent.x *= scale;
    extent.y *= scale;
    Im::ImVec2 at{(lo.x + hi.x - extent.x) * 0.5f, (lo.y + hi.y - extent.y) * 0.5f};
    if (const InkBox box = onInk ? InkOf(codepoint, scale) : InkBox{}; !box.Empty())
        at = {(lo.x + hi.x) * 0.5f - (box.x0 + box.x1) * 0.5f, (lo.y + hi.y) * 0.5f - (box.y0 + box.y1) * 0.5f};
    Im::ImDrawListManager::AddText(draw, font, fontSize, at, ink, text.c_str());
}

// The width a codepoint's ink takes at that scale: what a mark laid beside
// text needs, where a box the font's em wide would leave a margin either
// side of a narrow glyph like the bolt. The advance, for a glyph the font
// does not have.
float CodepointWidth(unsigned codepoint, float scale)
{
    if (const InkBox box = InkOf(codepoint, scale); !box.Empty())
        return box.x1 - box.x0;
    const std::string text = Utf8(codepoint);
    FontAwesome::PushSolid();
    const float width = Im::CalcTextSize(text.c_str(), nullptr, false, -1.0f).x;
    FontAwesome::Pop();
    return width * scale;
}

void DrawGlyph(Im::ImDrawList *draw, Glyph glyph, Im::ImVec2 lo, Im::ImVec2 hi, Im::ImU32 ink, float scale = 1.0f)
{
    DrawCodepoint(draw, Codepoint(glyph), lo, hi, ink, scale);
}

// A square button with the glyph painted over it, centred on the button's
// own rect the way the On cell's tick is. Not as the button's label: ImGui
// centres a label only when it fits inside the frame padding, and the icon
// font's glyph is taller than the text font's line, so the plus sat up and
// to the left. The text colour carries the disabled dimming.
// `painted` false leaves the square empty: a switch that is off shows no
// tick, as the rule rows' On cells do, rather than a ghost of one.
bool GlyphButton(const std::string &id, float size, Glyph glyph, bool painted = true)
{
    const bool clicked = Im::Button(("##" + id).c_str(), Im::ImVec2(size, size));
    if (auto *draw = Im::GetWindowDrawList(); draw && painted)
        DrawGlyph(draw, glyph, Im::GetItemRectMin(), Im::GetItemRectMax(), Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
    return clicked;
}

bool CellClicked(const char *id, float height = 0.0f);
void CentredHeading(const char *title);

bool DeleteButton(const std::string &id, float size)
{
    return GlyphButton(id, size, Glyph::Cross);
}

// Chrome for the cascade popups, shared by the condition and action menus.
//
// A thin border on every level. The root popup takes PopupBorderSize, but a
// menu opened from inside a menu is flagged as a CHILD window by ImGui
// (BeginMenuEx adds ImGuiWindowFlags_ChildWindow when its parent is itself a
// child menu) and takes ChildBorderSize instead -- which is why thinning only
// the popup border left the deeper levels at the theme's 3 px. Both are
// pushed, to the same value.
//
// ImGui places a submenu overlapping its parent, and the amount is
// ItemInnerSpacing.x -- not ItemSpacing, whatever the comment in imgui.cpp
// says it is (FindBestWindowPosForPopup reads ItemInnerSpacing). Setting it
// to the border width makes the child's left border land exactly on the
// parent's right border, so the two boxes share one line instead of
// colliding a few pixels apart.
//
// Pushed BEFORE BeginPopup: a popup takes its padding and border when the
// window is created, not while it is being filled. Popped on every path out,
// the closed one included.
void PushPopupChrome()
{
    constexpr float border = 1.0f;
    const auto *style = Im::GetStyle();
    const float innerY = style->ItemInnerSpacing.y;
    Im::PushStyleVar(Im::ImGuiStyleVar_PopupBorderSize, border);
    Im::PushStyleVar(Im::ImGuiStyleVar_ChildBorderSize, border);
    Im::PushStyleVar(Im::ImGuiStyleVar_ItemInnerSpacing, Im::ImVec2(border, innerY));
    Im::PushStyleVar(Im::ImGuiStyleVar_ItemSpacing, Im::ImVec2(6.0f, 4.0f));
}

constexpr int kPopupChromeVars = 4;

void CellButtonOpensPopup(const char *id, const std::string &label)
{
    // The highlight is the TABLE's, not the button's.
    //
    // A button can only ever paint its own rectangle, so making that rectangle
    // agree with the cell means matching the cell's padding by hand -- and it
    // is off by a pixel or two forever, because the two are computed by
    // different code from different values. TableSetBgColor fills the cell
    // rect that ImGui itself derived, so it lines up by construction rather
    // than by arithmetic.
    //
    // The button is therefore painted with nothing at all: it survives purely
    // as the hit target and the popup anchor.
    Im::SetCursorPosX(Im::GetCursorPosX() - kCellPadX);
    const float width = Im::GetContentRegionAvail().x + kCellPadX;

    const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
    Im::PushStyleColor(Im::ImGuiCol_Button, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonHovered, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonActive, invisible);
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    // Left-aligned, because this is a sentence, not a caption. Centred, the
    // text drifted around as the rule was edited and the column stopped
    // reading as a list.
    Im::PushStyleVar(Im::ImGuiStyleVar_ButtonTextAlign, Im::ImVec2(0.0f, 0.5f));

    const bool clicked = Im::Button((label + "##" + id).c_str(), Im::ImVec2(width, 0.0f));
    const bool hovered = Im::IsItemHovered(0);

    Im::PopStyleVar(2);
    Im::PopStyleColor(3);

    // Taken from the button just drawn, before anything moves the cursor.
    const Im::ImVec2 below{Im::GetItemRectMin().x, Im::GetItemRectMax().y};

    if (hovered)
        Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f), -1);

    if (clicked)
        Im::OpenPopup(id, 0);

    Im::SetNextWindowPos(below, Im::ImGuiCond_Always, Im::ImVec2(0.0f, 0.0f));
}

// One row of a cascade, drawn with the icon font's glyphs.
//
// ImGui draws a submenu's arrow and a selected item's check in its own
// strokes, and those are the two marks on this panel that were not from the
// icon font. There is no flag to turn them off, so: a submenu row is opened
// with its label pushed transparent -- ImGui still measures it and still
// draws its arrow, invisibly -- and the label and a caret are drawn over it
// through the parent's draw list; a leaf row is opened unselected, so ImGui
// draws no check, and a check is drawn where it would have been. The rect
// comes from the cursor and the window, not from the item: once a submenu
// opens, "the last item" is the submenu window.
float CascadeIconRight()
{
    const auto *style = Im::GetStyle();
    return Im::GetWindowPos().x + Im::GetWindowWidth() - (style->WindowPadding.x);
}

void CascadeIcon(Im::ImDrawList *draw, Glyph glyph, Im::ImVec2 rowPos, float right)
{
    if (!draw)
        return;
    const float h = Im::GetTextLineHeight();
    const float w = Im::GetFontSize();
    DrawGlyph(draw, glyph, {right - w, rowPos.y}, {right, rowPos.y + h}, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
}

bool BeginCascade(const char *label)
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const float right = CascadeIconRight();
    Im::PushStyleColor(Im::ImGuiCol_Text, Im::ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool open = Im::BeginMenu(label, true);
    Im::PopStyleColor(1);
    if (draw)
        Im::ImDrawListManager::AddText(draw, pos, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), label);
    CascadeIcon(draw, Glyph::CaretRight, pos, right);
    return open;
}

// `textColour` tints the label and nothing else: the tick at the right is
// the menu's, not the item's, and keeps the plain colour.
bool CascadeItem(const char *label, bool selected, const Im::ImVec4 *textColour = nullptr)
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const float right = CascadeIconRight();
    if (textColour)
        Im::PushStyleColor(Im::ImGuiCol_Text, *textColour);
    const bool clicked = Im::MenuItem(label, nullptr, false, true);
    if (textColour)
        Im::PopStyleColor(1);
    if (selected)
        CascadeIcon(draw, Glyph::Tick, pos, right);
    return clicked;
}

// The two about a fight, under one "Combat" heading: Start, End.
// The condition cascade in eight groups, a divider between them: Any; the
// three stats; the fight's edges; the enemy's relation to the party
// (Attacking, Attacked by); the hits (Hit type, Hit by); Status; the
// equipment -- weapon, armour, resistance; the summon. (The corpse
// questions are a subject of their own and fall in one group.)
int ConditionGroup(ft::PredicateKind p)
{
    switch (p)
    {
    case ft::PredicateKind::Any:
        return 0;
    case ft::PredicateKind::HealthPctBelow:
    case ft::PredicateKind::StaminaPctBelow:
    case ft::PredicateKind::MagickaPctBelow:
        return 1;
    case ft::PredicateKind::CombatBegins:
    case ft::PredicateKind::CombatEnds:
        return 2;
    case ft::PredicateKind::Attacking:
    case ft::PredicateKind::AttackedBy:
        return 3;
    case ft::PredicateKind::HitType:
    case ft::PredicateKind::HitBy:
        return 4;
    case ft::PredicateKind::Type:
        return 5;
    case ft::PredicateKind::Status:
        return 6;
    case ft::PredicateKind::SummonNone:
    case ft::PredicateKind::SummonActive:
        return 8;
    default:
        return 7;
    }
}

// Is this predicate the one its heading is drawn at? The rest of a heading
// (Combat end, Summon active, Lowest level, the poison pair) are drawn
// under it and skipped in the walk.
bool DrawsHeading(ft::PredicateKind p)
{
    switch (p)
    {
    case ft::PredicateKind::CombatEnds:
    case ft::PredicateKind::SummonActive:
    case ft::PredicateKind::LevelLowest:
    case ft::PredicateKind::WeaponPoisonNone:
    case ft::PredicateKind::WeaponPoisonActive:
        return false;
    default:
        return !ft::IsAbove(p) && !ft::IsExtreme(p);
    }
}

// The other followers, by name, for the two cascades' headings.
std::vector<FollowerView::Peer> SortedPeers(const FollowerView &view)
{
    std::vector<FollowerView::Peer> peers = view.peers;
    std::sort(peers.begin(), peers.end(),
              [](const FollowerView::Peer &a, const FollowerView::Peer &b) { return a.name < b.name; });
    return peers;
}

bool ConditionCascade(const char *id, ft::Rule &rule, const FollowerView &view, bool setAside)
{
    bool changed = false;

    // A condition naming a follower who is away is greyed, with the reason
    // on it -- and still opens, so another can be named in its place. The
    // colour is pushed round the cell alone, so the menu reads as usual;
    // `setAside` greys it with the rest of a row set aside for its action.
    const bool available = ConditionAvailable(rule, view);
    {
        const DimText grey(setAside || !available);
        CellButtonOpensPopup(id, ConditionText(rule, view));
    }
    if (!available && Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Im::SetTooltip("%s", kFollowerAway);

    PushPopupChrome();
    if (!Im::BeginPopup(id, 0))
    {
        Im::PopStyleVar(kPopupChromeVars);
        return false;
    }

    // Who first: the follower, the player by name, the other followers by
    // name, any ally; then the one being fought, and any enemy. Self first,
    // allies above enemies, the particular above the general.
    struct Heading
    {
        ft::SubjectKind subject;
        std::uint32_t form;
        std::string label;
    };
    std::vector<Heading> headings{{ft::SubjectKind::Self, 0, std::string(ft::DisplayName(ft::SubjectKind::Self))},
                                  {ft::SubjectKind::Player, 0, std::string(ft::DisplayName(ft::SubjectKind::Player))}};
    for (const auto &peer : SortedPeers(view))
        headings.push_back({ft::SubjectKind::Follower, peer.id, peer.name});
    headings.push_back({ft::SubjectKind::Ally, 0, std::string(ft::DisplayName(ft::SubjectKind::Ally))});
    headings.push_back({ft::SubjectKind::Enemy, 0, std::string(ft::DisplayName(ft::SubjectKind::Enemy))});
    headings.push_back({ft::SubjectKind::Corpse, 0, std::string(ft::DisplayName(ft::SubjectKind::Corpse))});

    for (const Heading &heading : headings)
    {
        const ft::SubjectKind subject = heading.subject;
        const std::uint32_t form = heading.form;
        if (!BeginCascade(heading.label.c_str()))
            continue;

        // One item of the cascade: selected when the rule says exactly
        // this, and on a click the rule says it. What "this" is beyond the
        // subject and the predicate -- a number, a kind of damage, a
        // status, a party member -- is the item's extras. The tooltip is
        // the predicate's help.
        struct Extras
        {
            std::optional<float> arg;
            std::optional<ft::DamageKind> damage;
            std::optional<ft::StatusKind> status;
            std::optional<ft::TypeKind> type;
            std::optional<std::uint32_t> member;
        };
        const auto pick = [&](const char *label, ft::PredicateKind which, const Extras &x = {}) {
            const std::uint32_t subjectForm = x.member.value_or(form);
            const bool selected =
                rule.subject == subject && rule.subjectForm == subjectForm && rule.predicate == which &&
                (!x.damage || rule.damageKind == *x.damage) && (!x.status || rule.statusKind == *x.status) &&
                (!x.type || rule.typeKind == *x.type) && (!x.arg || std::abs(rule.conditionArg - *x.arg) < 0.001f);
            if (CascadeItem(label, selected))
            {
                rule.subject = subject;
                rule.subjectForm = subjectForm;
                rule.predicate = which;
                if (x.damage)
                    rule.damageKind = *x.damage;
                if (x.status)
                    rule.statusKind = *x.status;
                if (x.type)
                    rule.typeKind = *x.type;
                if (x.arg)
                    rule.conditionArg = *x.arg;
                changed = true;
            }
            if (const auto text = ft::Describe(which); !text.empty() && Im::IsItemHovered(0))
                Im::SetTooltip("%s", std::string(text).c_str());
        };

        // A heading over a few leaves -- Combat: Start, End -- for the
        // predicates listed under one name, those the subject answers.
        struct Leaf
        {
            ft::PredicateKind predicate;
            const char *label;
        };
        const auto submenu = [&](const char *title, std::initializer_list<Leaf> leaves) {
            if (!BeginCascade(title))
                return;
            for (const Leaf &leaf : leaves)
                if (ft::IsPredicateValidFor(subject, leaf.predicate))
                    pick(leaf.label, leaf.predicate);
            Im::EndMenu();
        };

        // The thresholds of a grid predicate: the group's Lowest and
        // Highest first where the subject is a group, then the percents
        // below, then above. A resistance's carry its kind of damage.
        const auto thresholds = [&](ft::PredicateKind predicate, std::optional<ft::DamageKind> damage) {
            Extras x;
            x.damage = damage;
            if (const auto extremes = ft::ExtremesOf(predicate);
                extremes.lowest != predicate && ft::IsPredicateValidFor(subject, extremes.lowest))
            {
                pick("Lowest", extremes.lowest, x);
                pick("Highest", extremes.highest, x);
                Im::Separator();
            }
            for (const float preset : PresetsFor(predicate))
            {
                x.arg = preset;
                pick(ArgumentText(predicate, preset).c_str(), predicate, x);
            }
            if (const auto above = ft::AboveOf(predicate); above != predicate)
            {
                Im::Separator();
                for (const float preset : PresetsFor(above))
                {
                    x.arg = preset;
                    pick(ArgumentText(above, preset).c_str(), above, x);
                }
            }
        };

        int lastGroup = -1;
        for (std::size_t pi = 0; pi < static_cast<std::size_t>(ft::PredicateKind::COUNT); ++pi)
        {
            const auto predicate = static_cast<ft::PredicateKind>(pi);
            if (!ft::IsPredicateValidFor(subject, predicate))
                continue;
            // An above predicate is listed under its below counterpart's
            // heading, after a divider, not as a heading of its own; the
            // group's extremes likewise, first under theirs; and the rest
            // of a heading's leaves under the first of them.
            if (!DrawsHeading(predicate))
                continue;
            // A divider where one group of conditions ends and the next
            // begins.
            const int group = ConditionGroup(predicate);
            if (lastGroup >= 0 && group != lastGroup)
                Im::Separator();
            lastGroup = group;

            const std::string predicateName(ft::DisplayName(predicate));

            // The headings of a few leaves: the fight's edges, the summons,
            // the corpses' level; the weapons in hand, with the poison pair
            // under a heading of their own.
            if (predicate == ft::PredicateKind::CombatBegins)
            {
                submenu("Combat", {{ft::PredicateKind::CombatBegins, "Start"}, {ft::PredicateKind::CombatEnds, "End"}});
                continue;
            }
            if (predicate == ft::PredicateKind::SummonNone)
            {
                submenu("Summon",
                        {{ft::PredicateKind::SummonNone, "None"}, {ft::PredicateKind::SummonActive, "Active"}});
                continue;
            }
            if (predicate == ft::PredicateKind::LevelHighest)
            {
                submenu("Level",
                        {{ft::PredicateKind::LevelHighest, "Highest"}, {ft::PredicateKind::LevelLowest, "Lowest"}});
                continue;
            }
            if (predicate == ft::PredicateKind::WeaponChargeNeeded)
            {
                if (!BeginCascade("Weapon"))
                    continue;
                pick("Charge needed", ft::PredicateKind::WeaponChargeNeeded);
                submenu("Poison", {{ft::PredicateKind::WeaponPoisonNone, "None"},
                                   {ft::PredicateKind::WeaponPoisonActive, "Active"}});
                Im::EndMenu();
                continue;
            }

            // A resistance: the kinds of damage under "Resistance"; under
            // each, the same shape as Health.
            if (predicate == ft::PredicateKind::ResistancePctBelow)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::DamageKind::COUNT); ++ki)
                {
                    const auto kind = static_cast<ft::DamageKind>(ki);
                    // Nothing resists a blow or an arrow but armour, which
                    // is its own heading, and nothing resists "any".
                    if (!ft::IsDamageKindValidFor(predicate, kind))
                        continue;
                    if (!BeginCascade(std::string(ft::DisplayName(kind)).c_str()))
                        continue;
                    thresholds(predicate, kind);
                    Im::EndMenu();
                }
                Im::EndMenu();
                continue;
            }

            // Hit type and Hit by: how -- a blow, an arrow, a spell of any
            // kind -- then what the spell was, a divider between each group.
            // Hit by opens on Any, hit with anything at all inside the
            // window; Hit type has no Any, because every actor hits with
            // something and the condition would be true of everyone.
            if (predicate == ft::PredicateKind::HitType || predicate == ft::PredicateKind::HitBy)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                const auto kind = [&](ft::DamageKind k) {
                    Extras x;
                    x.damage = k;
                    pick(std::string(ft::DisplayName(k)).c_str(), predicate, x);
                };
                if (ft::IsDamageKindValidFor(predicate, ft::DamageKind::Any))
                {
                    kind(ft::DamageKind::Any);
                    Im::Separator();
                }
                kind(ft::DamageKind::Melee);
                kind(ft::DamageKind::Ranged);
                kind(ft::DamageKind::Magic);
                Im::Separator();
                kind(ft::DamageKind::Fire);
                kind(ft::DamageKind::Frost);
                kind(ft::DamageKind::Shock);
                kind(ft::DamageKind::Poison);
                Im::EndMenu();
                continue;
            }

            // The party: under "Attacking" and "Attacked by", the members by
            // name -- this follower, the player, the other followers -- each
            // a leaf that names the member, a divider between each part.
            if (predicate == ft::PredicateKind::Attacking || predicate == ft::PredicateKind::AttackedBy)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                const auto member = [&](std::uint32_t id, const std::string &label) {
                    Extras x;
                    x.member = id;
                    pick(label.c_str(), predicate, x);
                };
                member(view.id, "Self");
                Im::Separator();
                member(0, std::string(ft::DisplayName(ft::SubjectKind::Player)));
                if (const auto peers = SortedPeers(view); !peers.empty())
                {
                    Im::Separator();
                    for (const auto &peer : peers)
                        member(peer.id, peer.name);
                }
                Im::EndMenu();
                continue;
            }

            // A kind of being: four groups under "Type", each a menu of Any
            // -- the group itself -- and its members by name. Groups and
            // members both by name, not by the enum.
            if (predicate == ft::PredicateKind::Type)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                std::vector<ft::TypeKind> heads;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::TypeKind::COUNT); ++ki)
                    if (ft::IsGroupHead(static_cast<ft::TypeKind>(ki)))
                        heads.push_back(static_cast<ft::TypeKind>(ki));
                const auto byName = [](ft::TypeKind a, ft::TypeKind b) {
                    return ft::DisplayName(a) < ft::DisplayName(b);
                };
                std::sort(heads.begin(), heads.end(), byName);
                for (const ft::TypeKind head : heads)
                {
                    if (!BeginCascade(std::string(ft::DisplayName(head)).c_str()))
                        continue;
                    Extras any;
                    any.type = head;
                    pick("Any", predicate, any);
                    Im::Separator();
                    std::vector<ft::TypeKind> members;
                    for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::TypeKind::COUNT); ++ki)
                        if (const auto kind = static_cast<ft::TypeKind>(ki);
                            ft::GroupOf(kind) == head && !ft::IsGroupHead(kind))
                            members.push_back(kind);
                    std::sort(members.begin(), members.end(), byName);
                    for (const ft::TypeKind kind : members)
                    {
                        Extras x;
                        x.type = kind;
                        pick(std::string(ft::DisplayName(kind)).c_str(), predicate, x);
                    }
                    Im::EndMenu();
                }
                Im::EndMenu();
                continue;
            }

            // A status: the kinds, one leaf each, under "Status", in the
            // order a person looks for them -- by name, not by the enum.
            if (predicate == ft::PredicateKind::Status)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                std::vector<ft::StatusKind> kinds;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::StatusKind::COUNT); ++ki)
                    if (ft::IsStatusValidFor(subject, static_cast<ft::StatusKind>(ki)))
                        kinds.push_back(static_cast<ft::StatusKind>(ki));
                std::sort(kinds.begin(), kinds.end(),
                          [](ft::StatusKind a, ft::StatusKind b) { return ft::DisplayName(a) < ft::DisplayName(b); });
                for (const ft::StatusKind kind : kinds)
                {
                    Extras x;
                    x.status = kind;
                    pick(std::string(ft::DisplayName(kind)).c_str(), predicate, x);
                }
                Im::EndMenu();
                continue;
            }

            // No argument: a leaf. (Any's divider from the rest is the
            // group divider above.)
            if (PresetsFor(predicate).empty())
            {
                pick(predicateName.c_str(), predicate);
                continue;
            }

            // A number: the thresholds under the heading.
            if (!BeginCascade(predicateName.c_str()))
                continue;
            thresholds(predicate, std::nullopt);
            Im::EndMenu();
        }
        Im::EndMenu();
    }

    Im::EndPopup();
    Im::PopStyleVar(kPopupChromeVars);
    // A new subject may not supply the target the actions were aimed at --
    // "Ally" after the condition stopped being about an ally -- so the Then
    // side is put back in order: the target falls to Self, and an action
    // that makes no sense there is blanked.
    if (changed)
        ft::Reconcile(rule);
    return changed;
}

// The action side.
// Does this action name a spell (or a power, which is a spell record)?
bool TakesSpell(ft::ActionKind action)
{
    return ft::IsCast(action) || action == ft::ActionKind::EquipSpell;
}

// Which of the spell menu's kinds an action picks from: a power for Use
// power, a shout for Shout, a scroll for Scroll, a spell for the rest.
SpellOption::Kind SpellKindOf(ft::ActionKind action)
{
    switch (action)
    {
    case ft::ActionKind::UsePower:
        return SpellOption::Kind::Power;
    case ft::ActionKind::Shout:
        return SpellOption::Kind::Shout;
    case ft::ActionKind::UseScroll:
        return SpellOption::Kind::Scroll;
    default:
        return SpellOption::Kind::Spell;
    }
}

// Could a thing with this grip be pinned in this hand, as the equip menu
// asks it? Both means a two-hander, or a spell in each hand at once; one
// weapon cannot be in both hands.
bool Fits(ft::Grip grip, Hand hand, bool spell)
{
    switch (hand)
    {
    case Hand::Left:
        return grip == ft::Grip::Either || grip == ft::Grip::LeftOnly;
    case Hand::Right:
        return grip == ft::Grip::Either || grip == ft::Grip::RightOnly;
    case Hand::Both:
        return grip == ft::Grip::Both || (spell && grip == ft::Grip::Either);
    default:
        return false;
    }
}

// "Equip weapon" -> "weapon", for "Unequip weapon" and the heading.

// The name of the thing an equip rule names, as they carry or know it;
// empty if they do not.
std::string EquipTargetName(const ft::Action &act, const FollowerView &view)
{
    if (act.kind == ft::ActionKind::EquipSpell)
    {
        for (const auto &entry : view.magic)
            if (entry.form == act.form)
                return entry.name;
        return {};
    }
    // The row under the copy named, whatever it holds now; the form
    // alone, any row of it. With nothing under the copy, the name it was
    // last seen with, so a rule keeps reading "Equip Iron Dagger (+4)"
    // while that dagger is away.
    for (const auto &item : view.inventory)
        if (item.form == act.form && ft::SameVariant(item.variant, act.variant))
            return item.name;
    return act.name;
}

// Has the player banned this form? A spell, shout or power by its entry;
// a scroll by its rows, any row of the form banned. For the Cast menus,
// whose options carry the form alone.
bool BannedForm(const FollowerView &view, std::uint32_t form)
{
    for (const auto &entry : view.magic)
        if (entry.form == form && entry.banned)
            return true;
    for (const auto &item : view.inventory)
        if (item.form == form && item.banned)
            return true;
    return false;
}

// The colour an item's name is drawn in wherever it appears -- the
// Inventory tab's rows and pages, the rule pickers' leaves -- so two rows
// of one name read apart the same way everywhere: gold for a Daedric
// artifact, the enchanted tint for an enchanted copy, null for the plain
// text colour. Two copies alike in name and colour are told apart by
// their order alone, which is stable.
const Im::ImVec4 *NameTint(const InventoryItem &item);
// The marks after an item's name: a crown, a bolt, a skull, a hand (defined with
// the tints, below).
bool Badged(const InventoryItem &item);
float DrawNameBadges(Im::ImDrawList *draw, const InventoryItem &item, Im::ImVec2 at, bool dim);

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The thing an action names, capitalised for a heading of its own:
// "Weapon" under Equip, "Strongest soul gem" under Charge.
std::string NounHeading(ft::ActionKind action)
{
    std::string name(ft::Noun(action));
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

// The Then cell's text: the spell by NAME, never by id.
//
// A FormID in the table would be unreadable and, worse, unstable to look at --
// the point of naming the spell is that a rule reads as an instruction. The id
// is what the rule stores; this is what the player sees. Same split as wire
// names versus display names.
// Whom the rule's actions are aimed at, by name where it names someone: the
// first half of the Then cell, before the colon, as the subject is of the If.
std::string TargetText(const ft::Rule &rule, const FollowerView &view)
{
    switch (rule.actionTarget)
    {
    case ft::ActionTargetKind::Player:
        return std::string(ft::DisplayName(rule.actionTarget));
    case ft::ActionTargetKind::Follower:
        for (const auto &peer : view.peers)
            if (peer.id == rule.actionTargetForm)
                return peer.name;
        return "Follower (away)";
    default:
        return std::string(ft::DisplayName(rule.actionTarget));
    }
}

// The name a form has in the load order, for an action naming a thing the
// follower no longer has: the rule keeps the name of what it asked for.
std::string FormName(std::uint32_t form)
{
    const auto *record = form != 0 ? RE::TESForm::LookupByID(form) : nullptr;
    const char *name = record ? record->GetName() : nullptr;
    return name && *name ? name : "";
}

// What a rule calls a thing that is not there to be asked: the name it
// was last seen with (Action::name), else the record's.
std::string LastName(const ft::Action &act)
{
    return act.name.empty() ? FormName(act.form) : act.name;
}

// Whether what the action names is there to be used: the potion carried,
// the spell known, the scroll carried, the weapon in the bag. An action
// that names nothing, or a policy, is always available here; whether it
// can fire is the evaluator's question.
// The editor's questions of a rule are core's (core/Editor.h), asked of
// what the tick found the follower to have; the words are the panel's.
bool ActionAvailable(const ft::Action &act, const FollowerView &view)
{
    return ft::ActionHad(act, view.holdings);
}

bool ConditionAvailable(const ft::Rule &rule, const FollowerView &view)
{
    return ft::ConditionHad(rule, view.holdings);
}

bool TargetAvailable(const ft::Rule &rule, const FollowerView &view)
{
    return ft::TargetHad(rule, view.holdings);
}

// Why a rule is set aside, for its switch; null when it is not.
const char *SetAsideReason(const ft::Rule &rule, const FollowerView &view)
{
    switch (ft::RuleSetAside(rule, view.holdings))
    {
    case ft::Aside::FollowerAway:
        return kFollowerAway;
    case ft::Aside::NotHad:
        return kNotAvailable;
    case ft::Aside::None:
        break;
    }
    return nullptr;
}

std::string ActionText(const ft::Action &act, const FollowerView &view)
{
    std::string base(ft::DisplayName(act.kind));

    // A policy names its effect: "Strongest Health potion", "Weakest Resist
    // Fire potion", "Strongest Fear poison".
    if (ft::IsPolicy(act.kind))
    {
        if (act.effect.empty())
            return base + "...";
        const auto kind = ft::ConsumableOf(act.kind);
        const char *noun = kind == ft::ConsumableKind::Poison       ? " poison"
                           : kind == ft::ConsumableKind::Food       ? " food"
                           : kind == ft::ConsumableKind::Ingredient ? " ingredient"
                                                                    : " potion";
        return std::string(ft::IsStrongest(act.kind) ? "Strongest " : "Weakest ") +
               std::string(ft::EffectLabel(act.effect)) + noun;
    }

    if (ft::NamesConsumable(act.kind))
    {
        if (act.form == 0)
            return base + "...";
        const char *verb = act.kind == ft::ActionKind::DrinkPotion     ? "Drink "
                           : act.kind == ft::ActionKind::ApplyPoison   ? "Apply "
                           : act.kind == ft::ActionKind::ChargeSoulGem ? "Charge with "
                                                                       : "Eat ";
        for (const auto &option : view.consumables)
            if (option.form == act.form && option.kind == ft::ConsumableOf(act.kind))
                return verb + option.name;
        // Not carried: the name it was last seen with, the row set aside.
        const std::string name = LastName(act);
        return name.empty() ? base : verb + name;
    }

    if (ft::IsArrowsPolicy(act.kind))
        return "Equip " + std::string(ft::Noun(act.kind));
    if (ft::IsEquip(act.kind))
    {
        if (act.form == 0)
            return "Unequip " + std::string(ft::Noun(act.kind)) +
                   (ft::TakesHand(act.kind) && act.hand != Hand::None ? " (" + Lower(ft::DisplayName(act.hand)) + ")"
                                                                      : "");
        // Carried or known, else the name it was last seen with: the row
        // set aside.
        std::string name = EquipTargetName(act, view);
        if (name.empty())
            name = FormName(act.form);
        if (name.empty())
            return base;
        return "Equip " + name + (ft::TakesHand(act.kind) ? " (" + Lower(ft::DisplayName(act.hand)) + ")" : "");
    }

    if (!TakesSpell(act.kind))
        return base;

    if (act.form == 0)
        return base + "...";

    const auto kind = SpellKindOf(act.kind);
    for (const auto &option : view.spells)
    {
        if (option.form != act.form || option.kind != kind)
            continue;
        switch (kind)
        {
        case SpellOption::Kind::Power:
            return "Use " + option.name;
        case SpellOption::Kind::Shout:
            return "Shout " + option.name;
        case SpellOption::Kind::Scroll:
            return "Read " + option.name;
        default:
            return (act.dual ? "Dual cast " : "Cast ") + option.name;
        }
    }

    // Named a spell this follower does not know, or a scroll not carried:
    // the name it was last seen with, the row set aside.
    const std::string name = LastName(act);
    if (name.empty())
        return base;
    return (act.kind == ft::ActionKind::UsePower    ? "Use "
            : act.kind == ft::ActionKind::Shout     ? "Shout "
            : act.kind == ft::ActionKind::UseScroll ? "Read "
            : act.dual                              ? "Dual cast "
                                                    : "Cast ") +
           name;
}

// Is this spell offered for pinning in this hand? Not a shout or a power,
// which take no hand; fits the hand; and not above the follower's skill,
// which the combat AI would never choose -- so a pin on it would be a
// promise unkept, and it is left out rather than listed and greyed.
bool Offered(const MagicEntry &entry, Hand hand)
{
    return entry.category != MagicCategory::Shouts && entry.category != MagicCategory::Powers &&
           Fits(entry.grip, hand, true) && !entry.aboveSkill;
}

// One leaf of an equip menu: a thing the follower has, pinned in `hand`
// when chosen.
// `label` is the leaf's text; `rowName` the row's own name, kept on the
// action (Action::name); `banned` whether the player has banned it, which
// dims the leaf; `tint` the row's colour (NameTint), on the leaf too;
// `row` the inventory row behind it, for the count and the marks after
// the name (null for a spell, which has neither). An item's leaf reads as
// the Inventory tab's row does, so two rows of one form tell apart in the
// picker as they do there.
bool EquipLeaf(ft::Action &act, ft::ActionKind action, std::uint32_t form, const std::string &label, Hand hand,
               const std::optional<ft::ItemVariant> &variant, const std::string &rowName, bool banned,
               const Im::ImVec4 *tint = nullptr, const InventoryItem *row = nullptr)
{
    const bool selected = act.kind == action && act.form == form && act.variant.has_value() == variant.has_value() &&
                          ft::SameVariant(act.variant, variant) && act.hand == hand;
    // The text as the Inventory tab shows it, the count after the name; and
    // an id of its own past the ##, since two rows of a form may read the
    // same (the plain stack and the poisoned dagger) and ImGui tells menu
    // items apart by their label.
    std::string text = label;
    if (row && row->count > 1)
        text += " (" + std::to_string(row->count) + ")";
    std::string id = text;
    // Room for the marks after the text: spaces their width on the label
    // itself, so the menu sizes to text and marks together and no more. The
    // tick has the column ImGui keeps for a menu item's own check mark.
    if (row && Badged(*row))
    {
        const float need = kBadgeGap + DrawNameBadges(nullptr, *row, {}, false);
        const float space = TextWidth(" ");
        id.append(space > 0.0f ? static_cast<std::size_t>(std::ceil(need / space)) : 0, ' ');
    }
    if (row)
    {
        char key[24];
        std::snprintf(key, sizeof key, "##%016llX", static_cast<unsigned long long>(row->Key()));
        id += key;
    }
    // Banned: dimmed, as the Inventory and Magic tabs dim it, but still a
    // choice. The rules are the player's voice and a ban is the AI's leash;
    // a rule that names a banned thing is deliberate, and the leaf only
    // says so.
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const Im::ImVec4 *colour = banned ? &Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled] : tint;
    const bool clicked = CascadeItem(id.c_str(), selected, colour);
    // The menu item draws its label at the row's left edge; the marks go
    // after the label's width, laid out by hand since the item owns the row.
    if (row && Badged(*row))
        DrawNameBadges(Im::GetWindowDrawList(), *row, {pos.x + TextWidth(text) + kBadgeGap, pos.y}, banned);
    if (banned && Im::IsItemHovered(0))
        Im::SetTooltip("%s", "Banned");
    if (!clicked)
        return false;
    act.kind = action;
    act.form = form;
    act.variant = variant;
    act.name = rowName;
    act.hand = hand;
    return true;
}

// The Equip weapon / Equip spell / Equip arrows / Equip armor cascades.
//
// Unequip first: let go of every pin of this kind, and the AI chooses
// again. For the two that take a hand it heads each hand's menu instead,
// Right, Left and Both -- the weapon hand first, as a player thinks of them
// -- each listing what fits that hand; for arrows and armour, the things
// themselves. Every list is theirs, so a rule cannot name a
// thing they do not have.
bool EquipMenu(ft::Action &act, ft::ActionKind action, const FollowerView &view)
{
    bool changed = false;

    // Unequip: let go of every pin of the kind and take those things off,
    // and the AI decides again -- not "None", which would promise a hand
    // kept empty, and nothing does that. For a weapon or a spell it is the
    // first leaf of each hand's menu, the hand being the thing let go of;
    // for arrows and armour it heads the menu.
    const auto none = [&](Hand hand) {
        const bool selected = act.kind == action && act.form == 0 && act.hand == hand;
        if (CascadeItem("Unequip", selected))
        {
            act.kind = action;
            act.form = 0;
            act.hand = hand;
            changed = true;
        }
    };

    // What is not there is not listed: no greyed "(carries none)" lines,
    // an empty kind simply offers None and nothing beneath it.
    const bool spell = action == ft::ActionKind::EquipSpell;
    const bool handed = spell || action == ft::ActionKind::EquipWeapon;
    if (!handed)
    {
        none(Hand::None);
        const ItemCategory category =
            action == ft::ActionKind::EquipArrows ? ItemCategory::Arrows : ItemCategory::Armor;
        // The Inventory tab's rows, a leaf each, the plain stack among
        // them: every leaf names a row's variant, none the form alone.
        std::vector<const InventoryItem *> rows;
        for (const auto &item : view.inventory)
            if (item.category == category)
                rows.push_back(&item);
        // The arrows' two policies before the named kinds, as the potions
        // have theirs: the hardest-hitting carried, or the weakest.
        if (action == ft::ActionKind::EquipArrows && !rows.empty())
        {
            Im::Separator();
            for (const auto [label, kind] : {std::pair{"Strongest", ft::ActionKind::EquipStrongestArrows},
                                             std::pair{"Weakest", ft::ActionKind::EquipWeakestArrows}})
            {
                if (CascadeItem(label, act.kind == kind))
                {
                    act.kind = kind;
                    act.form = 0;
                    act.hand = Hand::None;
                    act.name.clear();
                    changed = true;
                }
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(kind)).c_str());
            }
        }
        if (!rows.empty())
            Im::Separator();
        for (const InventoryItem *item : rows)
        {
            if (EquipLeaf(act, action, item->form, item->name, Hand::None, item->variant, item->name, item->banned,
                          NameTint(*item), item))
                changed = true;
        }
        return changed;
    }

    for (const Hand hand : {Hand::Right, Hand::Left, Hand::Both})
    {
        const std::string label(ft::DisplayName(hand));
        if (!BeginCascade(label.c_str()))
            continue;
        // Unequip first: let go of that hand's pin. Both lets go of both.
        none(hand);
        bool any = false;
        if (spell)
        {
            for (const auto &entry : view.magic)
                any = any || Offered(entry, hand);
        }
        else
        {
            for (const auto &item : view.inventory)
                any = any || (item.category == ItemCategory::Weapons && Fits(item.grip, hand, false));
        }
        if (any)
            Im::Separator();
        if (spell)
        {
            for (const auto &entry : view.magic)
            {
                if (!Offered(entry, hand))
                    continue;
                if (EquipLeaf(act, action, entry.form, entry.name, hand, {}, entry.name, entry.banned))
                    changed = true;
            }
        }
        else
        {
            std::vector<const InventoryItem *> rows;
            for (const auto &item : view.inventory)
                if (item.category == ItemCategory::Weapons && Fits(item.grip, hand, false))
                    rows.push_back(&item);
            for (const InventoryItem *item : rows)
            {
                if (EquipLeaf(act, action, item->form, item->name, hand, item->variant, item->name, item->banned,
                              NameTint(*item), item))
                    changed = true;
            }
        }
        Im::EndMenu();
    }
    return changed;
}

// The action side of the cascade: the actions offered under one target
// heading of the Then cascade, those that make sense on that target
// (IsActionValidFor), in a fixed order with the more active thing first --
// Target; Potion, Food, Ingredient; Cast, Shout, Power; Charge, Poison;
// Weapon, Arrows, Armor, Spell -- a divider between the groups. Every list
// is the follower's own, so a rule cannot name a thing they do not have:
// the same guarantee the condition side gets from the validity matrix, and
// for the same reason -- an unfireable rule should be unauthorable, not
// merely discouraged. Choosing one sets the rule's target and the action
// together, as choosing a condition sets subject and predicate.
bool ActionItems(ft::Rule &rule, ft::Action &act, ft::ActionTargetKind target, std::uint32_t form,
                 const FollowerView &view)
{
    bool changed = false;
    // Is this heading the rule's current target? Only then is an item under
    // it shown selected.
    const bool here = rule.actionTarget == target && rule.actionTargetForm == form;
    const auto choose = [&]() {
        rule.actionTarget = target;
        rule.actionTargetForm = form;
        changed = true;
    };

    // The groups, the more active thing first: what is taken; what is
    // cast; what is done to the weapon in hand; what is put on. A divider
    // between the groups that draw anything -- a heading with nothing
    // under it (no food carried, no spell that suits) is not drawn, so the
    // divider is placed as the items come, never before an empty group.
    int lastGroup = -1;
    const auto group = [&](int g) {
        if (lastGroup >= 0 && g != lastGroup)
            Im::Separator();
        lastGroup = g;
    };
    const auto valid = [&](ft::ActionKind action) { return ft::IsActionValidFor(target, action); };

    // One leaf that picks a policy: the strongest of a kind, the weakest.
    const auto policy = [&](ft::ActionKind kind) {
        const bool selected = here && act.kind == kind;
        if (CascadeItem(NounHeading(kind).c_str(), selected))
        {
            act.kind = kind;
            act.form = 0;
            choose();
        }
        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", std::string(ft::Describe(kind)).c_str());
    };
    const auto carried = [&](ft::ConsumableKind kind) {
        bool any = false;
        for (const auto &option : view.consumables)
            any = any || option.kind == kind;
        return any;
    };
    // The named things of one kind, under a menu already open.
    const auto named = [&](ft::ActionKind kind) {
        for (const auto &option : view.consumables)
        {
            if (option.kind != ft::ConsumableOf(kind))
                continue;
            const std::string label = option.name + " (" + std::to_string(option.count) + ")";
            const bool selected = here && act.kind == kind && act.form == option.form;
            if (CascadeItem(label.c_str(), selected))
            {
                act.kind = kind;
                act.form = option.form;
                act.name = option.name;
                choose();
            }
        }
    };

    // Attack this one, bash them; then the power blows. Each says what it
    // does in its name; no tooltip.
    for (const auto kind :
         {ft::ActionKind::Attack, ft::ActionKind::Bash, ft::ActionKind::PowerAttack, ft::ActionKind::PowerBash})
    {
        if (!valid(kind))
            continue;
        group(kind == ft::ActionKind::Attack || kind == ft::ActionKind::Bash ? 0 : 1);
        const bool selected = here && act.kind == kind;
        if (CascadeItem(std::string(ft::DisplayName(kind)).c_str(), selected))
        {
            act.kind = kind;
            act.form = 0;
            act.hand = Hand::None;
            choose();
        }
    }

    // Strongest and Weakest for a kind of bottle, each a submenu of the
    // effects the carried ones have -- the known effects in their groups,
    // a divider between, the rest by name at the end -- so a follower with
    // no potion of an effect is not offered it. Then, after a divider,
    // every bottle of the kind by name. Under a menu already open.
    const auto byEffect = [&](ft::ConsumableKind ckind, ft::ActionKind strongestKind, ft::ActionKind weakestKind,
                              ft::ActionKind namedKind) {
        std::vector<std::string> names;
        for (const auto &option : view.consumables)
            if (option.kind == ckind)
                names.insert(names.end(), option.effects.begin(), option.effects.end());
        const auto arranged = ft::ArrangeEffects(ckind, std::move(names));
        for (const auto [label, kind] : {std::pair{"Strongest", strongestKind}, std::pair{"Weakest", weakestKind}})
        {
            if (arranged.empty() || !BeginCascade(label))
                continue;
            int last = -1;
            for (const auto &entry : arranged)
            {
                if (last >= 0 && entry.group != last)
                    Im::Separator();
                last = entry.group;
                const bool selected = here && act.kind == kind && act.effect == entry.name;
                if (CascadeItem(std::string(ft::EffectLabel(entry.name)).c_str(), selected))
                {
                    act.kind = kind;
                    act.form = 0;
                    act.effect = entry.name;
                    choose();
                }
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(kind)).c_str());
            }
            Im::EndMenu();
        }
        if (!arranged.empty())
            Im::Separator();
        named(namedKind);
    };

    // What is taken. Potion, Food and Ingredient list what is carried of
    // each, and are not drawn with nothing under them.
    if (valid(ft::ActionKind::DrinkStrongest))
    {
        // The group is entered only for a heading that is drawn, or the
        // divider would stand above the next group with nothing over it.
        if (carried(ft::ConsumableKind::Potion))
        {
            group(1);
            if (BeginCascade("Potion"))
            {
                byEffect(ft::ConsumableKind::Potion, ft::ActionKind::DrinkStrongest, ft::ActionKind::DrinkWeakest,
                         ft::ActionKind::DrinkPotion);
                Im::EndMenu();
            }
        }
        if (carried(ft::ConsumableKind::Food))
        {
            group(1);
            if (BeginCascade("Food"))
            {
                byEffect(ft::ConsumableKind::Food, ft::ActionKind::EatStrongestFood, ft::ActionKind::EatWeakestFood,
                         ft::ActionKind::EatFood);
                Im::EndMenu();
            }
        }
        if (carried(ft::ConsumableKind::Ingredient))
        {
            group(1);
            if (BeginCascade("Ingredient"))
            {
                byEffect(ft::ConsumableKind::Ingredient, ft::ActionKind::EatStrongestIngredient,
                         ft::ActionKind::EatWeakestIngredient, ft::ActionKind::EatIngredient);
                Im::EndMenu();
            }
        }
    }

    // What is cast: the spells that suit this target -- a Self-delivery
    // spell (Fast Healing, Oakflesh) is cast on oneself and on no one
    // else; an aimed one (Heal Other, Firebolt) goes at someone else --
    // then the same from both hands, only the spells the follower can dual
    // cast; then the shouts, then the powers. A follower with none that
    // fit is offered nothing rather than an empty submenu that looks broken.
    struct CastMenu
    {
        const char *label;
        ft::ActionKind action;
        bool dual;
    };
    for (const CastMenu menu :
         {CastMenu{"Cast", ft::ActionKind::CastSpell, false}, CastMenu{"Dual Cast", ft::ActionKind::CastSpell, true},
          CastMenu{"Scroll", ft::ActionKind::UseScroll, false}, CastMenu{"Shout", ft::ActionKind::Shout, false},
          CastMenu{"Power", ft::ActionKind::UsePower, false}})
    {
        const auto action = menu.action;
        if (!valid(action))
            continue;
        const auto kind = SpellKindOf(action);
        std::vector<const SpellOption *> suited;
        for (const auto &option : view.spells)
        {
            // A corpse takes a Reanimate and nothing else.
            if (target == ft::ActionTargetKind::Corpse && !option.reanimate)
                continue;
            if (menu.dual && !option.dualCast)
                continue;
            if (option.kind == kind && (option.location || option.selfOnly == (target == ft::ActionTargetKind::Self)))
                suited.push_back(&option);
        }
        if (suited.empty())
            continue;
        // The hand's casts -- spells, scrolls -- then the voice's, a
        // divider between.
        group(action == ft::ActionKind::CastSpell || action == ft::ActionKind::UseScroll ? 2 : 3);
        if (!BeginCascade(menu.label))
            continue;
        for (const auto *option : suited)
        {
            const bool selected = here && act.kind == action && act.form == option->form && act.dual == menu.dual;
            // Banned: dimmed as the equip leaves are, and still a choice,
            // for the reason EquipLeaf gives.
            const bool banned = BannedForm(view, option->form);
            if (CascadeItem(option->name.c_str(), selected,
                            banned ? &Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled] : nullptr))
            {
                act.kind = action;
                act.form = option->form;
                act.name = option->name;
                act.dual = menu.dual;
                choose();
            }
            if (banned && Im::IsItemHovered(0))
                Im::SetTooltip("%s", "Banned");
        }
        Im::EndMenu();
    }

    // What is done to the weapon in hand: Charge (the strongest gem that
    // fits, the weakest, then every spendable gem by name) and Poison (the
    // strongest and the weakest by effect, then every poison carried by
    // name; not drawn with none carried).
    if (valid(ft::ActionKind::ChargeStrongestSoulGem))
    {
        group(4);
        // The named things of one kind, after a divider when there are any.
        const auto namedAfterDivider = [&](ft::ActionKind kind) {
            if (!carried(ft::ConsumableOf(kind)))
                return;
            Im::Separator();
            named(kind);
        };
        if (BeginCascade("Charge"))
        {
            policy(ft::ActionKind::ChargeStrongestSoulGem);
            policy(ft::ActionKind::ChargeWeakestSoulGem);
            namedAfterDivider(ft::ActionKind::ChargeSoulGem);
            Im::EndMenu();
        }
        if (carried(ft::ConsumableKind::Poison) && BeginCascade("Poison"))
        {
            byEffect(ft::ConsumableKind::Poison, ft::ActionKind::ApplyStrongest, ft::ActionKind::ApplyWeakest,
                     ft::ActionKind::ApplyPoison);
            Im::EndMenu();
        }
    }

    // What is put on, and PINNED: Weapon, Arrows, Armor, Spell, each its
    // own menu of what is carried or known. The heading's tooltip is the
    // action's name, "Equip weapon": what choosing from it writes into the
    // rule, not the promise the pin makes.
    if (valid(ft::ActionKind::EquipWeapon))
    {
        group(5);
        for (const auto kind : {ft::ActionKind::EquipWeapon, ft::ActionKind::EquipArrows, ft::ActionKind::EquipArmor,
                                ft::ActionKind::EquipSpell})
        {
            const bool open = BeginCascade(NounHeading(kind).c_str());
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", std::string(ft::DisplayName(kind)).c_str());
            if (!open)
                continue;
            if (EquipMenu(act, kind, view))
                choose();
            Im::EndMenu();
        }
    }
    return changed;
}

// The Then cascade: whom first, then what -- the mirror of the If cascade's
// subject, then predicate. The headings are the same cast, less those the
// condition cannot supply: "Ally" on this side means the ally the condition
// matched, so it is offered only when the condition is about one.
bool ActionMenu(const char *id, ft::Action &act, const FollowerView &view, bool *addAnother, ft::Rule &rule,
                bool setAside)
{
    bool changed = false;

    // An action naming a thing the follower no longer has, or aimed at a
    // follower who is away, is greyed, with the reason on it -- and still
    // opens: the potion drunk up wants choosing again, here, not deleting
    // and writing afresh. The colour is pushed round the cell alone, so
    // the menu it opens reads as usual; `setAside` greys it with the rest
    // of a row set aside for its condition.
    const char *reason = !TargetAvailable(rule, view)  ? kFollowerAway
                         : !ActionAvailable(act, view) ? kNotAvailable
                                                       : nullptr;
    {
        const DimText grey(setAside || reason != nullptr);
        CellButtonOpensPopup(id, TargetText(rule, view) + ": " + ActionText(act, view));
    }
    if (reason && Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Im::SetTooltip("%s", reason);

    PushPopupChrome();
    if (!Im::BeginPopup(id, 0))
    {
        Im::PopStyleVar(kPopupChromeVars);
        return false;
    }

    // A rule with one action edits it here, in its row. The way to a second
    // is this entry, first and in a section of its own so it is not taken
    // for an action: the rule then opens as a drawer, where its actions
    // are listed, ordered and added to.
    if (addAnother)
    {
        if (CascadeItem("Add action...", false))
        {
            *addAnother = true;
            changed = true;
        }
        Im::Separator();
    }

    struct Heading
    {
        ft::ActionTargetKind target;
        std::uint32_t form;
        std::string label;
    };
    // The same order as the If cascade's: self, the player, the other
    // followers by name, any ally; then the threats -- the attacker, and
    // the enemy (the condition's, or the one being fought).
    std::vector<Heading> headings{
        {ft::ActionTargetKind::Self, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Self))},
        {ft::ActionTargetKind::Player, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Player))}};
    for (const auto &peer : SortedPeers(view))
        headings.push_back({ft::ActionTargetKind::Follower, peer.id, peer.name});
    headings.push_back({ft::ActionTargetKind::Ally, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Ally))});
    headings.push_back(
        {ft::ActionTargetKind::Attacker, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Attacker))});
    headings.push_back({ft::ActionTargetKind::Enemy, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Enemy))});
    headings.push_back({ft::ActionTargetKind::Corpse, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Corpse))});

    for (const Heading &heading : headings)
    {
        if (!ft::IsActionTargetValidFor(rule.subject, heading.target))
            continue;
        // Under "Corpse: None" there is no corpse to aim at.
        if (heading.target == ft::ActionTargetKind::Corpse && rule.predicate == ft::PredicateKind::CorpseNone)
            continue;
        if (!BeginCascade(heading.label.c_str()))
            continue;
        if (ActionItems(rule, act, heading.target, heading.form, view))
            changed = true;
        Im::EndMenu();
    }

    Im::EndPopup();
    Im::PopStyleVar(kPopupChromeVars);
    return changed;
}

// Which rows are open -- a skill's perks, a rule's actions -- keyed
// "section/label" or "rule/<follower>/<index>". Ours rather than any ImGui
// widget state, because widget state is keyed on the ID stack, and a row's
// ID stack includes which table PIECE it landed in (see DrawSections) --
// which changes as soon as a row above it opens, at which point ImGui
// would forget the row was open. Render thread only.
std::unordered_set<std::string> g_openRows;

float DisclosureWidth();
void DrawDisclosure(Im::ImVec2 pos, bool open);
std::string RuleKey(ft::ActorId follower, std::size_t index)
{
    return "rule/" + std::to_string(follower) + "/" + std::to_string(index);
}

// The open state follows the rule when rules are moved or removed, so a
// drawer does not stay behind at an index another rule has taken.
void MoveOpenState(ft::ActorId follower, std::size_t from, std::size_t to)
{
    const bool fromOpen = g_openRows.erase(RuleKey(follower, from)) > 0;
    const bool toOpen = g_openRows.erase(RuleKey(follower, to)) > 0;
    if (fromOpen)
        g_openRows.insert(RuleKey(follower, to));
    if (toOpen)
        g_openRows.insert(RuleKey(follower, from));
}

void RemoveOpenState(ft::ActorId follower, std::size_t at, std::size_t count)
{
    g_openRows.erase(RuleKey(follower, at));
    for (std::size_t i = at + 1; i < count; ++i)
    {
        if (g_openRows.erase(RuleKey(follower, i)) > 0)
            g_openRows.insert(RuleKey(follower, i - 1));
    }
}

// The drawer an open rule reveals: its actions, one row each in the order
// they are done, each its own menu; and up, down and remove, as the rule
// table's Order column. A plus beneath for one more. Set under the Then
// column -- its left edge on Then's border, its right on the table's --
// with Order the parent's width, so its columns line up with the parent's
// and need no headings of their own. Returns whether the rules changed.
bool DrawActionsDrawer(ft::Rule &rule, std::size_t ruleIndex, const FollowerView &view, float left, float right,
                       float spacing)
{
    // Seamless with the row above: the drawer's left border on the Then
    // column's, its top border ON the row's bottom border, so the two lines
    // are one line.
    //
    // No nudge. Moving up a pixel to close that seam is what OPENED it: with
    // ItemSpacing.y pushed to zero the cursor the piece above leaves behind
    // already sits on that piece's bottom border, so the drawer's own top
    // border lands on the same pixel unaided, and the pixel of clearance put
    // one line above the other instead of on it. Measured off a screenshot:
    // the border under Action/Order was two pixels where the same
    // row's border under On/#/Condition was one.
    Im::SetCursorScreenPos(Im::ImVec2(left, Im::GetCursorScreenPos().y));

    const float row = Im::GetFrameHeight();
    const float gutter = kCellPadX * 2.0f;
    const float orderWidth = row * 3.0f + kOrderGap * 2.0f + gutter;
    const float width = (std::max)(0.0f, right - left);

    bool changed = false;
    int moveFrom = -1;
    int moveTo = -1;
    int removeAt = -1;
    const std::string id = std::to_string(view.id) + "/" + std::to_string(ruleIndex);

    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    if (Im::BeginTable(("actions##" + id).c_str(), 2, flags, Im::ImVec2(width, 0.0f), 0.0f))
    {
        Im::TableSetupColumn("Action", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableSetupColumn("Order", Im::ImGuiTableColumnFlags_WidthFixed, orderWidth, 0);

        for (std::size_t a = 0; a < rule.actions.size(); ++a)
        {
            const std::string actId = id + "/" + std::to_string(a);
            Im::TableNextRow(0, 0.0f);

            Im::TableSetColumnIndex(0);
            if (ActionMenu(("##act" + actId).c_str(), rule.actions[a], view, nullptr, rule, false))
                changed = true;

            Im::TableSetColumnIndex(1);
            {
                const float group = row * 3.0f + kOrderGap * 2.0f;
                const float cell = Im::GetContentRegionAvail().x;
                if (cell > group)
                    Im::SetCursorPosX(Im::GetCursorPosX() + (cell - group) * 0.5f);
            }
            Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
            Im::BeginDisabled(a == 0);
            if (GlyphButton("up" + actId, row, Glyph::Up))
            {
                moveFrom = static_cast<int>(a);
                moveTo = static_cast<int>(a) - 1;
            }
            Im::EndDisabled();
            Im::SameLine(0.0f, kOrderGap);
            Im::BeginDisabled(a + 1 >= rule.actions.size());
            if (GlyphButton("dn" + actId, row, Glyph::Down))
            {
                moveFrom = static_cast<int>(a);
                moveTo = static_cast<int>(a) + 1;
            }
            Im::EndDisabled();
            Im::SameLine(0.0f, kOrderGap);
            if (DeleteButton("rm" + actId, row))
                removeAt = static_cast<int>(a);
            Im::PopStyleVar(1);
        }

        Im::EndTable();
    }

    // One more, done after the ones above: beneath the table, at its edge,
    // spaced as the rule table's own plus is spaced from it.
    Im::Dummy(Im::ImVec2(0.0f, spacing));
    Im::SetCursorScreenPos(Im::ImVec2(left, Im::GetCursorScreenPos().y));
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (GlyphButton("addact" + id, row, Glyph::Plus))
    {
        rule.actions.emplace_back();
        changed = true;
    }
    Im::PopStyleVar(1);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("Click to add action");

    if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(rule.actions.size()))
    {
        std::swap(rule.actions[static_cast<std::size_t>(moveFrom)], rule.actions[static_cast<std::size_t>(moveTo)]);
        changed = true;
    }
    if (removeAt >= 0)
    {
        rule.actions.erase(rule.actions.begin() + removeAt);
        changed = true;
    }

    Im::Dummy(Im::ImVec2(0.0f, spacing));
    return changed;
}

// Draw and EDIT the rule table.
//
// The set is taken by reference and `changed` reported back, so the caller
// writes the whole thing to the engine in one go. Editing a copy is what makes
// this safe: the tick never sees a half-applied change, and no lock is held
// while rendering.
//
// Edits go into the co-save with the next save (game/Profiles.h), and
// roll back with it.
//
// A rule with several actions opens like a drawer, as a skill opens its
// perks, and the table is drawn in PIECES for the same reason and by the
// same means as DrawSections: a piece is closed above the drawer and
// another opened beneath it with the same columns, the striping counted
// across pieces, the outer borders drawn by hand down the drawer's sides.
bool DrawRuleTable(ft::RuleSet &rules, const FollowerView &view)
{
    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_SizingStretchProp;

    // Fixed widths are MEASURED, not hardcoded. The panel's font size comes
    // from SKSEMenuFramework.ini (FontSizeMedium, 32 by default), so a pixel
    // count that fits at one size clips at another -- which is exactly how
    // three buttons ended up in a column too narrow to hold them.
    const float row = Im::GetFrameHeight();
    const float gutter = kCellPadX * 2.0f;
    // The heading or the visible tick, whichever is wider: the cell is the
    // click target now, so it needs no room for a button around the glyph.
    const float onWidth = (std::max)(TextWidth("On"), row * 0.4f) + gutter;
    const float numWidth = Im::CalcTextSize("99", nullptr, false, -1.0f).x + gutter;
    const float orderWidth = row * 3.0f + kOrderGap * 2.0f + gutter;

    const auto border = Im::GetColorU32(Im::ImGuiCol_TableBorderStrong, 1.0f);
    const auto stripe = Im::GetColorU32(Im::ImGuiCol_TableRowBgAlt, 1.0f);
    const auto hovered = Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f);
    const auto opened = Im::GetColorU32(Im::ImGuiCol_Header, 1.0f);
    auto *draw = Im::GetWindowDrawList();

    // The theme's spacing, read before it is pushed away: the drawer spaces
    // its plus with it, as Spacing() spaces the table's own plus below.
    const auto *style = Im::GetStyle();
    const float spacing = style->ItemSpacing.y;
    const float framePadY = style->FramePadding.y;

    // Cells keep a normal margin so headers and the number column are not
    // jammed against the border. The If/Then buttons cancel it locally -- see
    // CellButtonOpensPopup -- so their highlight still fills the whole cell.
    // Pieces abut: no item spacing between one and the next.
    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, 2.0f));
    Im::PushStyleVar(Im::ImGuiStyleVar_ItemSpacing, Im::ImVec2(kCellPadX, 0.0f));

    int piece = 0;
    bool inTable = false;
    float left = 0.0f;
    float right = 0.0f;
    float drawerTop = 0.0f;
    bool drawerOpen = false;

    bool changed = false;
    const auto beginPiece = [&]() {
        const std::string id = "rules##" + std::to_string(piece++);
        if (!Im::BeginTable(id.c_str(), 5, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
            return false;
        Im::TableSetupColumn("On", Im::ImGuiTableColumnFlags_WidthFixed, onWidth, 0);
        Im::TableSetupColumn("#", Im::ImGuiTableColumnFlags_WidthFixed, numWidth, 0);
        Im::TableSetupColumn("Condition", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        // The wider share, because an action reads as a phrase ("Drink magicka
        // potion") where a condition is mostly short words and a number.
        Im::TableSetupColumn("Action", Im::ImGuiTableColumnFlags_WidthStretch, 1.25f, 0);
        Im::TableSetupColumn("Order", Im::ImGuiTableColumnFlags_WidthFixed, orderWidth, 0);
        if (piece == 1)
        {
            // Plain headings (PlainHeaderRow says why), but On is a switch
            // for the whole list: off for all while any rule is on, on for
            // all otherwise. Every rule, the set-aside ones too: a rule's
            // own switch is the player's, kept across the setting-aside.
            Im::TableNextRow(Im::ImGuiTableRowFlags_Headers, 0.0f);
            Im::TableSetColumnIndex(0);
            const bool anyOn =
                std::any_of(rules.rules.begin(), rules.rules.end(), [](const ft::Rule &rule) { return rule.enabled; });
            const Im::ImVec2 pos = Im::GetCursorScreenPos();
            if (CellClicked("##onAll"))
            {
                for (ft::Rule &rule : rules.rules)
                    rule.enabled = !anyOn;
                if (!rules.rules.empty())
                    changed = true;
            }
            if (Im::IsItemHovered(0))
                Im::SetTooltip(anyOn ? "Click to disable all" : "Click to enable all");
            Im::SetCursorScreenPos(pos);
            Im::Text("On");
            int column = 1;
            for (const char *label : {"#", "Condition", "Action", "Order"})
            {
                Im::TableSetColumnIndex(column++);
                Im::Text("%s", label);
            }
        }
        inTable = true;
        return true;
    };

    const auto endPiece = [&]() {
        Im::EndTable();
        inTable = false;
        const Im::ImVec2 lo = Im::GetItemRectMin();
        const Im::ImVec2 hi = Im::GetItemRectMax();
        left = lo.x;
        right = hi.x;
        if (drawerOpen && draw)
        {
            Im::ImDrawListManager::AddLine(draw, {lo.x, drawerTop}, {lo.x, lo.y}, border, 1.0f);
            Im::ImDrawListManager::AddLine(draw, {hi.x, drawerTop}, {hi.x, lo.y}, border, 1.0f);
        }
        drawerOpen = false;
        drawerTop = hi.y;
    };

    if (!beginPiece())
    {
        Im::PopStyleVar(2);
        return false;
    }

    int moveFrom = -1;
    int moveTo = -1;
    int removeAt = -1;

    for (std::size_t i = 0; i < rules.rules.size(); ++i)
    {
        if (!inTable && !beginPiece())
            break;

        auto &rule = rules.rules[i];
        const std::string rowId = std::to_string(i);
        // A rule naming a thing the follower no longer has -- the potion
        // drunk up, the spell forgotten, the sword sold -- or a follower
        // who is away, is set aside: its switch slashed and dead, as an
        // equip cell that does not apply is, the row dimmed, the reason on
        // the switch and on the cell concerned. It keeps its text, its
        // place and its delete; the switch's own state is untouched, so the
        // rule comes back as it was when the thing, or the follower, does.
        const char *setAside = SetAsideReason(rule, view);
        const bool available = setAside == nullptr;
        Im::TableNextRow(0, 0.0f);
        if (i % 2 == 1)
            Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg0, stripe, -1);

        Im::TableSetColumnIndex(0);
        {
            // The whole cell is the switch, lit while hovered; the tick is
            // drawn centred in it at the size of the other glyphs on the row.
            // Off is no tick at all, as an unequipped item's cell on the
            // Inventory tab; the row's dimming says the rest.
            const Im::ImVec2 pos = Im::GetCursorScreenPos();
            if (!available)
            {
                // Slashed at the end of the row, once its height is known.
                Im::Dummy(Im::ImVec2(Im::GetContentRegionAvail().x, Im::GetFrameHeight()));
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", setAside);
            }
            else if (CellClicked(("##on" + rowId).c_str(), Im::GetFrameHeight()))
            {
                rule.enabled = !rule.enabled;
                changed = true;
            }
            if (available && Im::IsItemHovered(0))
                Im::SetTooltip(rule.enabled ? "Click to disable" : "Click to enable");

            if (auto *drawList = Im::GetWindowDrawList(); drawList && rule.enabled && available)
            {
                const float size = Im::GetFrameHeight();
                const float cell = Im::GetContentRegionAvail().x;
                const float leftEdge = pos.x + (cell - size) * 0.5f;
                DrawGlyph(drawList, Glyph::Tick, {leftEdge, pos.y}, {leftEdge + size, pos.y + size},
                          Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
            }
        }

        // A rule that is off reads as off: its number, condition and action
        // dim together, and the If and Then cells stop answering, so
        // it cannot be edited without turning it on. The switch itself and
        // the order and delete controls stay live: an off rule is still in
        // the list and can still be moved or removed. A rule set aside for
        // what it names reads the same but is not disabled: its If and Then
        // cells grey themselves and still answer, since naming something
        // else there is the way out of being set aside, and deleting the
        // rule to write it again is not (2026-09-10).
        BeginDimmed(!rule.enabled);

        Im::TableSetColumnIndex(1);
        Im::AlignTextToFramePadding();
        {
            const DimText grey(!available);
            Im::Text("%zu", i + 1);
        }

        Im::TableSetColumnIndex(2);
        if (ConditionCascade(("##cond" + rowId).c_str(), rule, view, !available))
            changed = true;

        Im::TableSetColumnIndex(3);
        // Where the Then column begins, for the drawer's border to sit on
        // it: the cell's content less the cell padding and less the half
        // item spacing ImGui puts before a cell's content, which this table
        // pushes to kCellPadX -- read from the style, not assumed. Without
        // the spacing the drawer sat three pixels right of the column's
        // border (2026-09-12).
        const float thenLeft = Im::GetCursorScreenPos().x - kCellPadX - std::floor(style->ItemSpacing.x * 0.5f);
        if (rule.actions.empty())
            rule.actions.emplace_back();
        const std::string key = RuleKey(view.id, i);
        bool open = false;
        if (rule.actions.size() == 1)
        {
            // One action: edited here, in its row. Its menu offers a
            // second, and the rule then opens as a drawer.
            bool addAnother = false;
            if (ActionMenu(("##act" + rowId).c_str(), rule.actions.front(), view, &addAnother, rule, !available))
                changed = true;
            if (addAnother)
            {
                rule.actions.emplace_back();
                g_openRows.insert(key);
            }
        }
        else
        {
            // Several: the cell says how many and opens the drawer they are
            // listed in. An invisible button the size of the cell, lit
            // through the cell background so it fits by construction, with
            // the marker and the summary drawn over it.
            //
            // A BUTTON, and the same one the one-action cell uses, because
            // the two cells have to come out the same height. ImGui offsets a
            // Selectable by the row's text baseline -- `pos.y +=
            // CurrLineTextBaseOffset`, with ItemSize then charging that
            // offset ON TOP of the height asked for -- and a table carries
            // that baseline left to right along the row, so the `#` column's
            // AlignTextToFramePadding reached this cell and made every rule
            // with several actions one frame padding taller than a rule with
            // one. A Button counts the baseline as its own frame padding and
            // stays put: measured on this font, 50 px of row became 44, which
            // is what a one-action row and the drawer's own rows are.
            open = g_openRows.count(key) > 0;
            const Im::ImVec2 pos = Im::GetCursorScreenPos();
            const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
            Im::PushStyleColor(Im::ImGuiCol_Button, invisible);
            Im::PushStyleColor(Im::ImGuiCol_ButtonHovered, invisible);
            Im::PushStyleColor(Im::ImGuiCol_ButtonActive, invisible);
            Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
            const bool clicked =
                Im::Button(("##open" + key).c_str(), Im::ImVec2(Im::GetContentRegionAvail().x, Im::GetFrameHeight()));
            Im::PopStyleVar(1);
            Im::PopStyleColor(3);
            if (clicked)
            {
                open = !open;
                if (open)
                    g_openRows.insert(key);
                else
                    g_openRows.erase(key);
            }
            if (Im::IsItemHovered(0))
                Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, hovered, -1);
            else if (open)
                Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, opened, -1);
            // The marker on the text's line: the row is a frame high for
            // its buttons, and the text sits a frame padding down in it.
            DrawDisclosure(Im::ImVec2(pos.x, pos.y + framePadY), open);
            Im::SetCursorScreenPos(Im::ImVec2(pos.x + DisclosureWidth(), pos.y));
            Im::AlignTextToFramePadding();
            // How many, not which: the first by name and "2 more" did not
            // fit the column, and the drawer is one click away. Greyed with
            // the row when set aside: the cell answers, but reads as the
            // rest of the row does.
            const DimText grey(!available);
            Im::Text("%zu actions", rule.actions.size());
        }

        EndDimmed();

        // Order is semantics, not decoration: rules are first-match-wins, so
        // moving a row changes which rule shadows which.
        Im::TableSetColumnIndex(4);
        {
            // Centre the three as a group, using the SAME gap the layout below
            // actually uses. Measuring with ItemSpacing while laying out with
            // kOrderGap overstated the group by ~12px and shifted it left.
            const float group = row * 3.0f + kOrderGap * 2.0f;
            const float cell = Im::GetContentRegionAvail().x;
            if (cell > group)
                Im::SetCursorPosX(Im::GetCursorPosX() + (cell - group) * 0.5f);
        }
        Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
        // Arrows from the icon font, like every other glyph on the row. The
        // first row's up and the last row's down are greyed the panel's
        // way, not ImGui's: the table sits in a BeginDimmed whose disabled
        // alpha is 1, so a plain BeginDisabled took no clicks but looked
        // exactly like its neighbours.
        BeginDimmed(i == 0);
        if (GlyphButton("up" + rowId, row, Glyph::Up))
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        EndDimmed();

        Im::SameLine(0.0f, kOrderGap);
        BeginDimmed(i + 1 >= rules.rules.size());
        if (GlyphButton("dn" + rowId, row, Glyph::Down))
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        EndDimmed();

        Im::SameLine(0.0f, kOrderGap);
        if (DeleteButton("rm" + rowId, row))
            removeAt = static_cast<int>(i);
        Im::PopStyleVar(1);

        // Back to the switch: every cell is drawn, so the row's height is
        // final and the slash reaches its bottom corner.
        if (!available)
        {
            Im::TableSetColumnIndex(0);
            SlashCell();
        }

        if (!open)
            continue;

        // The drawer: close this piece, draw beneath, reopen for the rest.
        endPiece();
        BeginDimmed(!rule.enabled);
        if (DrawActionsDrawer(rule, i, view, thenLeft, right, spacing))
            changed = true;
        EndDimmed();
        drawerOpen = true;
    }

    if (inTable)
    {
        endPiece();
    }
    else if (drawerOpen && draw)
    {
        // The drawer was the last thing in the table: close the frame under
        // it by hand, since no piece follows to do so.
        const float bottom = Im::GetCursorScreenPos().y;
        Im::ImDrawListManager::AddLine(draw, {left, drawerTop}, {left, bottom}, border, 1.0f);
        Im::ImDrawListManager::AddLine(draw, {right, drawerTop}, {right, bottom}, border, 1.0f);
        Im::ImDrawListManager::AddLine(draw, {left, bottom}, {right, bottom}, border, 1.0f);
    }
    Im::PopStyleVar(2);

    // Applied after the loop: mutating the vector mid-iteration would invalidate
    // the reference the current row still holds.
    if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(rules.rules.size()))
    {
        std::swap(rules.rules[static_cast<std::size_t>(moveFrom)], rules.rules[static_cast<std::size_t>(moveTo)]);
        MoveOpenState(view.id, static_cast<std::size_t>(moveFrom), static_cast<std::size_t>(moveTo));
        changed = true;
    }
    if (removeAt >= 0)
    {
        RemoveOpenState(view.id, static_cast<std::size_t>(removeAt), rules.rules.size());
        rules.rules.erase(rules.rules.begin() + removeAt);
        changed = true;
    }
    Im::Spacing();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool addClicked = GlyphButton("addrule", Im::GetFrameHeight(), Glyph::Plus);
    Im::PopStyleVar(1);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("Click to add tactic");

    if (addClicked)
    {
        // Blank, and inert. A new rule must not do anything until it has been
        // told what to do: defaulting the action to "drink a health potion"
        // meant adding a row intended for magicka immediately started feeding
        // the follower health potions, before it had been finished.
        //
        // ActionKind::None cannot fire -- the evaluator rejects it before the
        // condition is even looked at -- so the row is safe to leave half
        // written for as long as you like.
        ft::Rule fresh;
        fresh.subject = ft::SubjectKind::Self;
        fresh.predicate = ft::PredicateKind::Any;
        fresh.conditionArg = 0.0f;
        fresh.actionTarget = ft::ActionTargetKind::Self;
        fresh.actions = {ft::Action{}};
        rules.rules.push_back(fresh);
        changed = true;
    }
    return changed;
}

// A header row that is only a header. TableHeadersRow() draws each heading
// as a widget: it lights up under the mouse and opens a column menu on a
// right click, both of which promise something the sheet does not offer. A
// row flagged as a header gets the header background; plain text on it gets
// the look without the behaviour.
void PlainHeaderRow(const std::vector<const char *> &labels)
{
    Im::TableNextRow(Im::ImGuiTableRowFlags_Headers, 0.0f);
    int column = 0;
    for (const char *label : labels)
    {
        Im::TableSetColumnIndex(column++);
        Im::Text("%s", label);
    }
}

// The open/closed marker before a skill that has perks: a small triangle,
// pointing right when closed and down when open, drawn rather than typed
// for the reason DeleteButton gives. Drawn at `pos`, the top-left of the
// text line it sits beside, and vertically centred on that line.
float DisclosureWidth()
{
    return Im::GetTextLineHeight() * 0.5f + kCellPadX;
}

void DrawDisclosure(Im::ImVec2 pos, bool open)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw)
        return;
    const float h = Im::GetTextLineHeight();
    const float s = h * 0.5f;
    const float top = pos.y + (h - s) * 0.5f;
    const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
    if (open)
        Im::ImDrawListManager::AddTriangleFilled(draw, {pos.x, top}, {pos.x + s, top},
                                                 {pos.x + s * 0.5f, top + s * 0.8f}, ink);
    else
        Im::ImDrawListManager::AddTriangleFilled(draw, {pos.x, top}, {pos.x + s * 0.8f, top + s * 0.5f},
                                                 {pos.x, top + s}, ink);
}

// The drawer an open skill row reveals: its perks, name and description,
// set in from both edges of the parent table and given air above and below.
// A table of perks: Perk, Rank, Description, the name a link to the perk's
// page. The drawer under a skill and the Other section are both this.
void DrawPerkTable(const std::string &id, const std::vector<SheetRow> &perks, float width,
                   const std::function<void(std::uint32_t)> &onLink)
{
    float nameWidth = TextWidth("Perk");
    float rankWidth = TextWidth("Rank");
    for (const auto &sub : perks)
    {
        nameWidth = (std::max)(nameWidth, TextWidth(sub.label));
        rankWidth = (std::max)(rankWidth, TextWidth(sub.value));
    }
    const float pad = kTablePad;

    const auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    if (!Im::BeginTable(id.c_str(), 3, flags, Im::ImVec2(width, 0.0f), 0.0f))
        return;
    Im::TableSetupColumn("Perk", Im::ImGuiTableColumnFlags_WidthFixed, nameWidth + pad, 0);
    Im::TableSetupColumn("Rank", Im::ImGuiTableColumnFlags_WidthFixed, rankWidth + pad, 0);
    Im::TableSetupColumn("Description", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    PlainHeaderRow({"Perk", "Rank", "Description"});
    for (const auto &sub : perks)
    {
        Im::TableNextRow(0, 0.0f);
        Im::TableSetColumnIndex(0);
        // A perk set aside -- its conditions fail for this actor -- is the
        // shadowed rows' grey, with the reason on its name.
        const bool aside = !sub.aside.empty();
        const DimText grey(aside);
        if (sub.form != 0 && onLink)
        {
            // The name is a link to the perk's page. The click cell's ID
            // starts with "##": ImGui draws whatever precedes it as text.
            const Im::ImVec2 pos = Im::GetCursorScreenPos();
            if (CellClicked(("##" + id + "/" + sub.label).c_str()))
                onLink(sub.form);
            if (aside && Im::IsItemHovered(0))
                Im::SetTooltip("%s", sub.aside.c_str());
            Im::SetCursorScreenPos(pos);
        }
        Im::Text("%s", sub.label.c_str());
        if (aside && !(sub.form != 0 && onLink) && Im::IsItemHovered(0))
            Im::SetTooltip("%s", sub.aside.c_str());
        Im::TableSetColumnIndex(1);
        Im::Text("%s", sub.value.c_str());
        Im::TableSetColumnIndex(2);
        Im::TextWrapped("%s", sub.modifiers.c_str());
    }
    Im::EndTable();
}

// A table of conditions: the call, the comparison, a tick where met.
void DrawConditionTable(const std::string &id, const std::vector<SheetRow> &rows, float width)
{
    float callWidth = TextWidth("Condition");
    float valueWidth = TextWidth("Value");
    for (const auto &row : rows)
    {
        callWidth = (std::max)(callWidth, TextWidth(row.label));
        valueWidth = (std::max)(valueWidth, TextWidth(row.value));
    }
    const float pad = kTablePad;
    const auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    if (!Im::BeginTable(id.c_str(), 3, flags, Im::ImVec2(width, 0.0f), 0.0f))
        return;
    Im::TableSetupColumn("Condition", Im::ImGuiTableColumnFlags_WidthFixed, callWidth + pad, 0);
    Im::TableSetupColumn("Value", Im::ImGuiTableColumnFlags_WidthFixed, valueWidth + pad, 0);
    Im::TableSetupColumn("Met", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    PlainHeaderRow({"Condition", "Value", "Met"});
    for (const auto &row : rows)
    {
        Im::TableNextRow(0, 0.0f);
        Im::TableSetColumnIndex(0);
        Im::Text("%s", row.label.c_str());
        Im::TableSetColumnIndex(1);
        Im::Text("%s", row.value.c_str());
        Im::TableSetColumnIndex(2);
        if (row.icon != 0)
        {
            FontAwesome::PushSolid();
            Im::Text("%s", Utf8(row.icon).c_str());
            FontAwesome::Pop();
        }
        else if (!row.extra.empty())
        {
            // N/A, nobody to ask, or ?, no answer to be had: blank is false.
            const DimText grey(true);
            Im::Text("%s", row.extra.c_str());
        }
    }
    Im::EndTable();
}

// The drawer an open row reveals -- an effect's conditions, a skill's
// perks -- set in from both edges, a gap above and below, the table the
// caller's, drawn at the width left.
void DrawDrawer(float left, float right, const std::function<void(float)> &table)
{
    constexpr float kGap = 6.0f;
    const float inset = 4.0f * kCellPadX;
    Im::Dummy(Im::ImVec2(0.0f, kGap));
    Im::SetCursorScreenPos(Im::ImVec2(left + inset, Im::GetCursorScreenPos().y));
    table((std::max)(0.0f, right - left - 2.0f * inset));
    Im::Dummy(Im::ImVec2(0.0f, kGap));
}

void DrawConditionDrawer(const SheetRow &row, const std::string &key, float left, float right)
{
    DrawDrawer(left, right, [&](float width) { DrawConditionTable("conditions##" + key, row.detail, width); });
}

void DrawPerkDrawer(const SheetRow &row, float left, float right, const std::function<void(std::uint32_t)> &onLink = {})
{
    DrawDrawer(left, right, [&](float width) { DrawPerkTable("perks##" + row.label, row.detail, width, onLink); });
}

void NoteTooltip(const std::string &note);
// A number written out as the calculation that made it: the lines in
// two columns, the amounts right-aligned, a rule, then the total -- the
// same shape everywhere a value has sources, so the eye can check the
// arithmetic. A line's detail sits indented beneath it.
void BreakdownTooltip(const ft::Breakdown &b);

// A run of headed sections, each a bordered table in the style of the rule
// table. The name and value columns are FIXED, measured across every section
// in the run so the tables line up down the page, and sized to their
// contents so a value sits beside its name rather than at the far edge of
// the panel. A Modifiers column, when asked for, takes the rest. Plain
// headings rather than collapsing ones: nothing here is long enough to want
// hiding, and a heading that can be clicked invites clicking to see what
// happens. The wording was done on the game thread; this only lays it out.
//
// No header row for the name and value columns. What they are is obvious
// from a glance at any row, and a label saying so is one more line of chrome
// per table. The Modifiers column keeps its heading because that one is not
// obvious.
//
// A row with perks opens like a drawer, and ImGui has no such thing: a table
// is a grid with no spanning. So the table is drawn in PIECES. It is closed
// at an open row, the drawer is drawn beneath, and a fresh table with the
// same column widths is opened for the rows that follow. Three things make
// the seam invisible: no vertical spacing between pieces, so the bottom
// border of one is the row line above the drawer and the top border of the
// next is the row line below it; the striping is counted across pieces
// rather than restarted by each; and the outer left and right borders are
// drawn by hand across the drawer's height, so the frame is continuous.
// What an open row reveals, given the row, its key and the parent table's
// edges. The perk drawer by default.
using RowDrawer = std::function<void(const SheetRow &, const std::string &, float, float)>;

// A column a table may carry after the value: its heading, and each row's
// text in it. Left out of the table altogether when every row's text is
// empty -- a Remaining column on a page of effects that never end says
// nothing. `glyph`: the text is only whether to draw the tick. `link`:
// the cell is a link to the row's form, where the table has an onLink.
struct ExtraColumn
{
    const char *heading{""};
    std::function<std::string(const SheetRow &)> text;
    bool glyph{false};
    bool link{false};
    bool wrap{false}; // prose: wrapped to the column, which takes the rest of the table
};

// The columns an effect table carries after Name and Effect. Each drawn
// only where some row has it. An item's or a spell's page ends with the
// effects' descriptions, wrapped; the effect's own page ends with Source.
const std::vector<ExtraColumn> kEffectColumns{
    {"Remaining", [](const SheetRow &r) { return r.remaining; }},
    {"Duration", [](const SheetRow &r) { return r.extra; }},
    {"Hidden", [](const SheetRow &r) { return std::string(r.mark != 0 ? "x" : ""); }, true},
};
const ExtraColumn kDescriptionColumn{"Description", [](const SheetRow &r) { return r.description; }, false, false,
                                     true};
std::vector<ExtraColumn> WithDescription()
{
    std::vector<ExtraColumn> columns = kEffectColumns;
    columns.push_back(kDescriptionColumn);
    return columns;
}

// `modifiers`: a third column, headed `third`, carrying each row's
// modifiers text or its mark glyph -- none at all when `third` is null;
// `first` and `second` head the name and value columns then, where the
// table has a header row at all; and `wanted` are the columns after it,
// of which those with anything in them are drawn, the last column taking
// the rest of the table.
void DrawSections(const std::vector<SheetSection> &sections, bool modifiers,
                  const std::function<void(std::uint32_t)> &onLink = {}, const char *third = "Modifiers",
                  const RowDrawer &drawer = {}, const char *first = "", const char *second = "",
                  const std::vector<ExtraColumn> &wanted = {})
{
    std::vector<ExtraColumn> extras;
    for (const auto &column : wanted)
    {
        const bool any = std::any_of(sections.begin(), sections.end(), [&](const SheetSection &section) {
            return std::any_of(section.rows.begin(), section.rows.end(),
                               [&](const SheetRow &row) { return !column.text(row).empty(); });
        });
        if (any)
            extras.push_back(column);
    }
    const bool hasThird = modifiers && third != nullptr;

    float nameWidth = modifiers ? TextWidth(first) : 0.0f;
    float valueWidth = modifiers ? TextWidth(second) : 0.0f;
    std::vector<float> extraWidths;
    extraWidths.reserve(extras.size());
    for (const auto &column : extras)
        extraWidths.push_back((std::max)(TextWidth(column.heading), column.glyph ? Im::GetFontSize() : 0.0f));
    // A row with perks carries the disclosure marker before its name and
    // is measured with it; a row without starts its name where the marker
    // would be, so the two kinds line up on their left edge.
    const float marker = modifiers ? DisclosureWidth() : 0.0f;
    for (const auto &section : sections)
    {
        for (const auto &row : section.rows)
        {
            const float lead = row.detail.empty() ? 0.0f : marker;
            nameWidth = (std::max)(nameWidth, lead + TextWidth(row.label));
            valueWidth = (std::max)(valueWidth, TextWidth(row.value));
            for (std::size_t i = 0; i < extras.size(); ++i)
                if (!extras[i].glyph && !extras[i].wrap)
                    extraWidths[i] = (std::max)(extraWidths[i], TextWidth(extras[i].text(row)));
        }
    }
    const float pad = kTablePad;
    const int columns = modifiers ? 2 + (hasThird ? 1 : 0) + static_cast<int>(extras.size()) : 2;
    // Where a column carries the link -- an effect's source -- the name
    // does not: one link per row, on the cell that names where it goes.
    const bool linkColumn =
        std::any_of(extras.begin(), extras.end(), [](const ExtraColumn &column) { return column.link; });

    const auto border = Im::GetColorU32(Im::ImGuiCol_TableBorderStrong, 1.0f);
    const auto stripe = Im::GetColorU32(Im::ImGuiCol_TableRowBgAlt, 1.0f);
    const auto hovered = Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f);
    const auto opened = Im::GetColorU32(Im::ImGuiCol_Header, 1.0f);
    auto *draw = Im::GetWindowDrawList();

    // The rule table's horizontal padding, but more above and below the text:
    // at the theme's default the last row sat on the table's bottom border.
    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, 4.0f));

    std::string lastHeading;
    bool firstInGroup = true;
    for (const auto &section : sections)
    {
        // One heading per group, and a plain label over each table within it
        // -- Attack, then "Right Hand" and "Left Hand". A section with no
        // group is its own heading, as every section was before.
        const std::string &heading = section.group.empty() ? section.title : section.group;
        if (heading != lastHeading)
        {
            CentredHeading(heading.c_str());
            lastHeading = heading;
            firstInGroup = true;
        }
        if (!section.group.empty())
        {
            // Air between one table and the next label under a shared
            // heading: the two spacings after a table were not enough to
            // keep "Left Hand" from reading as a footer to the table above.
            if (!firstInGroup)
            {
                Im::Spacing();
                Im::Spacing();
                Im::Spacing();
            }
            Im::Text("%s", section.title.c_str());
        }
        firstInGroup = false;

        // Pieces abut: no item spacing between one table and the next.
        Im::PushStyleVar(Im::ImGuiStyleVar_ItemSpacing, Im::ImVec2(kCellPadX, 0.0f));

        int piece = 0;
        int stripeIndex = 0;
        bool inTable = false;
        float left = 0.0f;
        float right = 0.0f;
        float drawerTop = 0.0f; // bottom of the piece above an open drawer
        bool drawerOpen = false;

        const auto beginPiece = [&]() {
            const std::string id = section.title + "##" + std::to_string(piece++);
            const auto flags = Im::ImGuiTableFlags_Borders;
            if (!Im::BeginTable(id.c_str(), columns, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
                return false;
            // The last column takes the rest of the table, so a value cell
            // that is a link lights up to the table's edge rather than
            // stopping at the widest value; every column before it is as
            // wide as its widest text.
            int index = 0;
            const auto column = [&](const char *label, float width) {
                const bool last = ++index == columns;
                if (last)
                    Im::TableSetupColumn(label, Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
                else
                    Im::TableSetupColumn(label, Im::ImGuiTableColumnFlags_WidthFixed, width + pad, 0);
            };
            column("##name", nameWidth);
            column("##value", valueWidth);
            if (modifiers)
            {
                std::vector<const char *> labels{first, second};
                if (hasThird)
                {
                    column(third, TextWidth(third));
                    labels.push_back(third);
                }
                for (std::size_t i = 0; i < extras.size(); ++i)
                {
                    column(extras[i].heading, extraWidths[i]);
                    labels.push_back(extras[i].heading);
                }
                if (piece == 1)
                    PlainHeaderRow(labels);
            }
            inTable = true;
            return true;
        };

        // Closes the current piece and, if a drawer sat above it, draws the
        // outer borders down the drawer's sides so the frame is unbroken.
        const auto endPiece = [&]() {
            Im::EndTable();
            inTable = false;
            const Im::ImVec2 lo = Im::GetItemRectMin();
            const Im::ImVec2 hi = Im::GetItemRectMax();
            left = lo.x;
            right = hi.x;
            if (drawerOpen && draw)
            {
                Im::ImDrawListManager::AddLine(draw, {lo.x, drawerTop}, {lo.x, lo.y}, border, 1.0f);
                Im::ImDrawListManager::AddLine(draw, {hi.x, drawerTop}, {hi.x, lo.y}, border, 1.0f);
            }
            drawerOpen = false;
            drawerTop = hi.y;
        };

        if (!beginPiece())
        {
            Im::PopStyleVar(1);
            continue;
        }

        for (std::size_t rowIndex = 0; rowIndex < section.rows.size(); ++rowIndex)
        {
            const SheetRow &row = section.rows[rowIndex];
            if (!inTable && !beginPiece())
                break;

            Im::TableNextRow(0, 0.0f);
            if (stripeIndex++ % 2 == 1)
                Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg0, stripe, -1);
            // A row set aside -- an effect whose conditions do not hold --
            // is the shadowed rows' grey, with the reason on its name.
            const DimText grey(!row.aside.empty());
            Im::TableSetColumnIndex(0);
            bool open = false;
            if (row.detail.empty())
            {
                if (modifiers && row.form != 0 && onLink && !linkColumn)
                {
                    // A loose perk: its name is the link to its page, as the
                    // value cell is elsewhere.
                    const Im::ImVec2 pos = Im::GetCursorScreenPos();
                    if (CellClicked(("##link" + section.title + "/" + row.label).c_str()))
                        onLink(row.form);
                    Im::SetCursorScreenPos(pos);
                }
                Im::Text("%s", row.label.c_str());
            }
            else
            {
                // A row that opens is a selectable spanning every column, so
                // the whole row is the click target -- but drawn INVISIBLE,
                // and the highlight painted through the table's own row
                // background instead. The selectable's own highlight is the
                // size of its label, which is what left the far end of the
                // row unlit; the row background is the rect ImGui derived
                // for the row, padding and all, so it fits by construction.
                // The marker and the name are then drawn over it.
                const std::string key = section.title + "/" + row.label + "#" + std::to_string(rowIndex);
                open = g_openRows.count(key) > 0;

                const Im::ImVec2 pos = Im::GetCursorScreenPos();
                const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
                Im::PushStyleColor(Im::ImGuiCol_Header, invisible);
                Im::PushStyleColor(Im::ImGuiCol_HeaderHovered, invisible);
                Im::PushStyleColor(Im::ImGuiCol_HeaderActive, invisible);
                const bool clicked = Im::Selectable(("##" + key).c_str(), false,
                                                    Im::ImGuiSelectableFlags_SpanAllColumns, Im::ImVec2(0.0f, 0.0f));
                Im::PopStyleColor(3);

                if (clicked)
                {
                    open = !open;
                    if (open)
                        g_openRows.insert(key);
                    else
                        g_openRows.erase(key);
                }
                if (Im::IsItemHovered(0))
                    Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg1, hovered, -1);
                else if (open)
                    Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg1, opened, -1);

                DrawDisclosure(pos, open);
                Im::SetCursorScreenPos(Im::ImVec2(pos.x + marker, pos.y));
                Im::Text("%s", row.label.c_str());
            }
            if (!row.aside.empty() && Im::IsItemHovered(0))
                Im::SetTooltip("%s", row.aside.c_str());

            Im::TableSetColumnIndex(1);
            if (row.form != 0 && onLink && !modifiers)
            {
                // The value names an item: that cell is a link to its page,
                // lit like the inventory's name cell.
                const Im::ImVec2 pos = Im::GetCursorScreenPos();
                if (CellClicked(("##link" + section.title + "/" + row.label).c_str()))
                    onLink(row.form);
                Im::SetCursorScreenPos(pos);
            }
            if (row.icon != 0)
            {
                // A glyph in the value's place, laid as text so it starts
                // where the value would and keeps its own width: centred in
                // a square, the infinity, twice as wide as tall, spilled
                // over the cell's left border.
                FontAwesome::PushSolid();
                Im::Text("%s", Utf8(row.icon).c_str());
                if (row.icon2 != 0)
                {
                    Im::SameLine(0.0f, -1.0f);
                    Im::Text("%s", Utf8(row.icon2).c_str());
                }
                FontAwesome::Pop();
            }
            else
            {
                Im::Text("%s", row.value.c_str());
            }
            // What made the value is hover text on the value itself, beside
            // the number it explains; on the Modifiers cell where the row
            // has one, since that is the number it explains there.
            const auto explain = [&] {
                if (!Im::IsItemHovered(0))
                    return;
                if (!row.breakdown.empty())
                    BreakdownTooltip(row.breakdown);
                else if (!row.note.empty())
                    NoteTooltip(row.note);
            };
            if (!modifiers)
                explain();
            if (modifiers)
            {
                int index = 2;
                if (hasThird)
                {
                    Im::TableSetColumnIndex(index++);
                    if (row.mark != 0)
                    {
                        FontAwesome::PushSolid();
                        Im::Text("%s", Utf8(row.mark).c_str());
                        FontAwesome::Pop();
                    }
                    else if (!row.modifierParts.empty())
                    {
                        // Each figure hovers apart: the damage over the
                        // damage, the cost over the cost.
                        bool separate = false;
                        for (const auto &part : row.modifierParts)
                        {
                            if (separate)
                            {
                                Im::SameLine(0.0f, 0.0f);
                                Im::Text("%s", ", ");
                                Im::SameLine(0.0f, 0.0f);
                            }
                            separate = true;
                            Im::Text("%s", part.text.c_str());
                            if (!part.breakdown.empty() && Im::IsItemHovered(0))
                                BreakdownTooltip(part.breakdown);
                        }
                    }
                    else
                    {
                        Im::Text("%s", row.modifiers.c_str());
                        explain();
                    }
                }
                for (const auto &column : extras)
                {
                    Im::TableSetColumnIndex(index++);
                    const std::string text = column.text(row);
                    if (column.glyph)
                    {
                        // The tick centred in its cell, under a heading
                        // wider than it.
                        if (!text.empty())
                        {
                            FontAwesome::PushSolid();
                            const std::string tick = Utf8(kGlyphTick);
                            const float slack = Im::GetContentRegionAvail().x - TextWidth(tick);
                            Im::SetCursorPosX(Im::GetCursorPosX() + (std::max)(0.0f, slack * 0.5f));
                            Im::Text("%s", tick.c_str());
                            FontAwesome::Pop();
                        }
                        continue;
                    }
                    // A link to the row's page where it has one, as the
                    // value cell is elsewhere.
                    if (column.link && row.form != 0 && onLink)
                    {
                        // Its own ID: the same as the name cell's, and the
                        // click went to the name (2026-09-11).
                        const Im::ImVec2 at = Im::GetCursorScreenPos();
                        if (CellClicked(("##source" + section.title + "/" + row.label).c_str()))
                            onLink(row.form);
                        Im::SetCursorScreenPos(at);
                    }
                    if (column.wrap)
                        Im::TextWrapped("%s", text.c_str());
                    else
                        Im::Text("%s", text.c_str());
                }
            }
            if (!open)
                continue;

            // The drawer: close this piece, draw beneath, reopen for the rest.
            endPiece();
            if (drawer)
                drawer(row, section.title + "/" + row.label + "#" + std::to_string(rowIndex), left, right);
            else
                DrawPerkDrawer(row, left, right, onLink);
            drawerOpen = true;
        }

        if (inTable)
        {
            endPiece();
        }
        else if (drawerOpen && draw)
        {
            // The drawer was the last thing in the section: close the frame
            // under it by hand, since no piece follows to do so.
            const float bottom = Im::GetCursorScreenPos().y;
            Im::ImDrawListManager::AddLine(draw, {left, drawerTop}, {left, bottom}, border, 1.0f);
            Im::ImDrawListManager::AddLine(draw, {right, drawerTop}, {right, bottom}, border, 1.0f);
            Im::ImDrawListManager::AddLine(draw, {left, bottom}, {right, bottom}, border, 1.0f);
        }

        Im::PopStyleVar(1); // item spacing
        Im::Spacing();
        Im::Spacing();
    }

    Im::PopStyleVar(1); // cell padding
}

// --- inventory ---------------------------------------------------------------

// What each follower's Inventory tab is showing: the list narrowed to one
// category, or one item in detail. Keyed by follower so switching pages does
// not lose the place. Render thread only, like g_openRows.
enum class Tab
{
    None,
    Character,
    Inventory,
    Magic,
    Summons,
    Effects,
    Skills,
    CombatStyle, // a follower's page only
    Tactics,     // a follower's page only
};

// The Inventory and Magic categories are one for every page, as the filters
// are: Weapons on one follower is Weapons on the next and on the player.
// Render thread only.
int g_inventoryCategory = -1;
int g_magicCategory = -1;

struct InventoryTabState
{
    std::uint64_t detail{0}; // the row open in detail, by InventoryItem::Key; 0 for the list
    // This frame's category: g_inventoryCategory, or All on a page with
    // nothing in it.
    int category{-1};
    Tab openedFrom{Tab::Inventory}; // where the detail page returns to
    Tab select{Tab::None};          // a tab to switch to on the next frame
};

std::unordered_map<ft::ActorId, InventoryTabState> g_inventoryTabs;

// One filter for every follower. Shared on purpose: "where are the lockpicks"
// is a question about the party, not about one bag.
char g_inventoryFilter[64]{};

// The category row's icons. Font Awesome's free set has no sword, so
// Weapons gets a hammer; swap the codepoint here if a better one turns up.
constexpr unsigned kIconAll = 0xF49E;  // box-open
constexpr unsigned kIconMisc = 0xF1B2; // cube
unsigned IconFor(ItemCategory category)
{
    switch (category)
    {
    case ItemCategory::Weapons:
        return 0xF6E3; // hammer
    case ItemCategory::Arrows:
        return 0xF140; // bullseye
    case ItemCategory::Armor:
        return 0xF553; // tshirt
    case ItemCategory::Potions:
        return 0xF0C3; // flask
    case ItemCategory::Poisons:
        return 0xF714; // skull-crossbones
    case ItemCategory::Food:
        return 0xF5D1; // apple-alt
    case ItemCategory::Ingredients:
        return 0xF5A7; // mortar-pestle
    case ItemCategory::Scrolls:
        return 0xF70E; // scroll
    case ItemCategory::Books:
        return 0xF02D; // book
    case ItemCategory::Keys:
        return 0xF084; // key
    case ItemCategory::Misc:
    default:
        return kIconMisc;
    }
}

constexpr Im::ImVec4 kEnchanted{0.70f, 0.75f, 1.00f, 1.0f};
// A Daedric artifact: light gold, over the enchanted blue; every artifact
// is enchanted, and the colour says which kind of enchanted it is. Red was
// tried on 2026-09-12 and read as a warning.
constexpr Im::ImVec4 kArtifact{0.95f, 0.85f, 0.55f, 1.0f};
// A poison on a weapon: green, as the bottle is.
constexpr Im::ImVec4 kPoison{0.55f, 0.85f, 0.45f, 1.0f};
// A stolen copy: red.
constexpr Im::ImVec4 kStolen{0.90f, 0.35f, 0.30f, 1.0f};

const Im::ImVec4 *NameTint(const InventoryItem &item)
{
    return item.artifact ? &kArtifact : item.enchanted ? &kEnchanted : nullptr;
}

// The marks after an item's name, wherever the name is drawn: a crown for a
// Daedric artifact, else a bolt for an enchanted piece (an artifact is
// always enchanted, and the crown says so), then a skull for a poison on
// it, then a hand for a stolen copy. Each in its own colour -- the crown and
// the bolt the name's tint, the skull green, the hand red -- or the row's disabled colour when the row is dimmed, or
// the marks would light up a greyed row. Drawn at kPinScale like the pin: a
// font glyph fills its em and reads too big beside text at full size.
// Returns the width drawn, so a caller laying the marks out by hand (a menu
// leaf) can advance past them.
bool Badged(const InventoryItem &item)
{
    return item.artifact || item.enchanted || !item.poison.rows.empty() || item.stolen;
}

// With no draw list, measures only: the width the marks would take.
float DrawNameBadges(Im::ImDrawList *draw, const InventoryItem &item, Im::ImVec2 at, bool dim)
{
    const float h = Im::GetTextLineHeight();
    float x = at.x;
    const auto ink = [dim](const Im::ImVec4 &colour) {
        Im::PushStyleColor(Im::ImGuiCol_Text, dim ? Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled] : colour);
        const Im::ImU32 u32 = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
        Im::PopStyleColor(1);
        return u32;
    };
    const auto badge = [&](unsigned codepoint, const Im::ImVec4 &colour) {
        // A box the glyph's own width, so the mark sits where the text ends
        // and not a margin past it.
        const float box = CodepointWidth(codepoint, kPinScale);
        if (draw)
            DrawCodepoint(draw, codepoint, {x, at.y}, {x + box, at.y + h}, ink(colour), kPinScale, true);
        x += box + kBadgeGap;
    };
    if (item.artifact)
        badge(0xF521, kArtifact); // crown
    else if (item.enchanted)
        badge(0xF0E7, kEnchanted); // bolt
    if (!item.poison.rows.empty())
        badge(0xF714, kPoison); // skull-crossbones
    if (item.stolen)
        badge(0xF256, kStolen); // hand
    return x - at.x;
}

// The marks after the name just drawn, on the same line. `framed` for a
// name drawn with AlignTextToFramePadding beside a button, as the item
// page's heading is: the text sits a frame padding below the line's top,
// and the marks go down with it, or they ride high beside it.
void NameBadges(const InventoryItem &item, bool dim, bool framed = false)
{
    if (!Badged(item))
        return;
    Im::SameLine(0.0f, kBadgeGap);
    Im::ImVec2 at = Im::GetCursorScreenPos();
    if (framed)
        at.y += Im::GetStyle()->FramePadding.y;
    const float width = DrawNameBadges(Im::GetWindowDrawList(), item, at, dim);
    Im::Dummy({width, framed ? Im::GetFrameHeight() : Im::GetTextLineHeight()});
}

// The filter the list tabs share: a box with "Filter" for its hint, and a
// cross inside its right end to clear it, shown only while there is
// something to clear. Returns whether the text changed.
bool FilterBox(const char *id, char *buffer, std::size_t size)
{
    const float width = Im::GetFontSize() * 9.0f;
    Im::SetNextItemWidth(width);
    // The cross is drawn over the box's right end, and ImGui gives the
    // hover to the item drawn first unless it allows overlap: without this
    // the cross could be seen but never clicked.
    Im::SetNextItemAllowOverlap();
    bool changed = Im::InputTextWithHint(id, "Filter", buffer, size);
    if (buffer[0] == '\0')
        return changed;

    // The cross sits over the box's right end: a square button the box's
    // height, painted invisible, the glyph drawn over it.
    const Im::ImVec2 lo = Im::GetItemRectMin();
    const Im::ImVec2 hi = Im::GetItemRectMax();
    const float side = hi.y - lo.y;
    const Im::ImVec2 keep = Im::GetCursorScreenPos();
    Im::SetCursorScreenPos(Im::ImVec2(hi.x - side, lo.y));
    const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
    Im::PushStyleColor(Im::ImGuiCol_Button, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonHovered, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonActive, invisible);
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (GlyphButton(std::string(id) + "clear", side, Glyph::Cross))
    {
        buffer[0] = '\0';
        changed = true;
    }
    Im::PopStyleVar(1);
    Im::PopStyleColor(3);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("Clear the filter");
    Im::SetCursorScreenPos(keep);
    return changed;
}

bool ContainsNoCase(const std::string &text, const char *needle)
{
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    const std::string_view n(needle);
    return n.empty() || std::search(text.begin(), text.end(), n.begin(), n.end(), same) != text.end();
}

// A list's filter box, and on its line against the right edge how many of
// the list's rows the filter leaves: "12 items", "3 of 12 items". Above the
// table, not under it, where a long list pushed the count out of sight;
// counted after the box, so the number answers this frame's text.
void FilterRow(const char *id, char *buffer, std::size_t size, const std::function<std::size_t()> &shown,
               std::size_t total, const char *noun)
{
    const float right = Im::GetCursorPosX() + Im::GetContentRegionAvail().x - Im::GetStyle()->ItemSpacing.x;
    FilterBox(id, buffer, size);
    const std::size_t count = shown();
    const std::string text =
        (count == total ? std::to_string(total) : std::to_string(count) + " of " + std::to_string(total)) + " " + noun;
    Im::SameLine((std::max)(0.0f, right - TextWidth(text)), -1.0f);
    Im::AlignTextToFramePadding();
    Im::TextDisabled("%s", text.c_str());
}

// Does any of a row's cells, as its table shows them, hold the filter's
// text? A filter on the name alone missed what the other columns are for:
// "Fire" among the spells, "Heavy" among the armour.
bool AnyContains(const std::vector<std::string> &cells, const char *needle)
{
    return std::any_of(cells.begin(), cells.end(),
                       [needle](const std::string &cell) { return ContainsNoCase(cell, needle); });
}

// Text flush with the right edge of the current table cell, for a column of
// numbers: the ones and the tens then line up.
void TextRightInCell(const std::string &text)
{
    const float slack = Im::GetContentRegionAvail().x - TextWidth(text);
    if (slack > 0.0f)
        Im::SetCursorPosX(Im::GetCursorPosX() + slack);
    Im::Text("%s", text.c_str());
}

// A sheet row's hover text as a table: each "Label: value" line a row, the
// values right-aligned so the numbers line up down the column -- "3.00%"
// under "+3.00%" ends on the same edge. A line with no ": " is a plain
// line.
void NoteTooltip(const std::string &note)
{
    Im::BeginTooltip();
    if (Im::BeginTable("note", 2, Im::ImGuiTableFlags_SizingFixedFit, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        std::size_t from = 0;
        while (from <= note.size())
        {
            const std::size_t end = note.find('\n', from);
            const std::string line = note.substr(from, end == std::string::npos ? std::string::npos : end - from);
            from = end == std::string::npos ? note.size() + 1 : end + 1;
            if (line.empty())
                continue;
            Im::TableNextRow(0, 0.0f);
            Im::TableSetColumnIndex(0);
            const std::size_t colon = line.rfind(": ");
            if (colon == std::string::npos)
            {
                Im::Text("%s", line.c_str());
                continue;
            }
            Im::Text("%s", line.substr(0, colon + 1).c_str());
            Im::TableSetColumnIndex(1);
            TextRightInCell(line.substr(colon + 2));
        }
        Im::EndTable();
    }
    Im::EndTooltip();
}

void BreakdownTooltip(const ft::Breakdown &b)
{
    Im::BeginTooltip();
    // A detail line's amount goes in a column of its own, inset from the
    // lines' amounts: lined up with them, a multiplier's Base 1.00 read as
    // another term of the weapon's sum (2026-09-14). Two columns where no
    // line opens.
    const bool opens =
        std::any_of(b.lines.begin(), b.lines.end(), [](const ft::BreakdownLine &line) { return !line.detail.empty(); });
    const int columns = opens ? 3 : 2;
    const int amounts = columns - 1;
    if (Im::BeginTable("breakdown", columns, Im::ImGuiTableFlags_SizingFixedFit, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        const auto lines = [&](const auto &self, const std::vector<ft::BreakdownLine> &list, int depth) -> void {
            for (const ft::BreakdownLine &line : list)
            {
                Im::TableNextRow(0, 0.0f);
                Im::TableSetColumnIndex(0);
                // Detail is indented and nothing more: dimmed, it read as
                // inactive, which is what dimming means on the perk page.
                std::string label(static_cast<std::size_t>(depth) * 3, ' ');
                label += line.label;
                Im::Text("%s", label.c_str());
                Im::TableSetColumnIndex(depth == 0 ? amounts : 1);
                TextRightInCell(ft::AmountText(b, line));
                self(self, line.detail, depth + 1);
            }
        };
        lines(lines, b.lines, 0);
        // The rule under the last line, then the total: the sum as a
        // schoolbook writes it. Not for a base alone, which is the sum.
        const bool onlyBase = b.lines.size() == 1 && b.lines.front().op == ft::Op::Start;
        if (!onlyBase)
        {
            Im::TableNextRow(0, 0.0f);
            for (int column = 0; column < columns; ++column)
            {
                Im::TableSetColumnIndex(column);
                Im::Separator();
            }
            Im::TableNextRow(0, 0.0f);
            Im::TableSetColumnIndex(0);
            Im::Text("%s", b.totalLabel.c_str());
            Im::TableSetColumnIndex(amounts);
            TextRightInCell(ft::TotalText(b));
        }
        Im::EndTable();
    }
    Im::EndTooltip();
}

// An invisible, cell-filling click target, lit through the cell background
// while hovered for the reason DrawSections gives. Draws nothing itself: the
// caller puts the cursor back and draws the cell's content over it.
bool CellClicked(const char *id, float height)
{
    const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
    Im::PushStyleColor(Im::ImGuiCol_Header, invisible);
    Im::PushStyleColor(Im::ImGuiCol_HeaderHovered, invisible);
    Im::PushStyleColor(Im::ImGuiCol_HeaderActive, invisible);
    const bool clicked = Im::Selectable(id, false, 0, Im::ImVec2(0.0f, height));
    Im::PopStyleColor(3);
    if (Im::IsItemHovered(0))
        Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f), -1);
    return clicked;
}

// The Worn column's glyphs, centred in the cell whose top-left is `pos` and
// drawn over whatever the cell already laid out. The tick for equipped,
// the pin beside it for pinned, in the word the panel uses for it:
// equipped, and kept so. A pin without a tick is a pin the AI is fighting:
// the thing is promised to the hand but not in it this instant (the sword
// the AI drew over a pinned bow, 14:48), and the pin must not read as
// cleared. Banned is the ban sign alone: off, and kept off.
void DrawTickAt(Im::ImVec2 pos, Im::ImU32 ink, bool on, bool pinned, bool banned)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw || (!on && !pinned && !banned))
        return;
    // Boxes the height of the text line the row was laid out with, and a
    // glyph's width each, so the pair sits centred with the row's own margin
    // above and below.
    const float h = Im::GetTextLineHeight();
    const float box = Im::GetFontSize();
    const float cell = Im::GetContentRegionAvail().x;
    if (banned)
    {
        const float left = pos.x + (std::max)(0.0f, (cell - box) * 0.5f);
        DrawGlyph(draw, Glyph::Ban, {left, pos.y}, {left + box, pos.y + h}, ink, kPinScale);
        return;
    }
    const float width = on && pinned ? 2.0f * box : box;
    float left = pos.x + (std::max)(0.0f, (cell - width) * 0.5f);
    if (on)
    {
        DrawGlyph(draw, Glyph::Tick, {left, pos.y}, {left + box, pos.y + h}, ink);
        left += box;
    }
    if (pinned)
        DrawGlyph(draw, Glyph::Pin, {left, pos.y}, {left + box, pos.y + h}, ink, kPinScale);
}

void CentredHeading(const char *title)
{
    Im::PushStyleVar(Im::ImGuiStyleVar_SeparatorTextAlign, Im::ImVec2(0.5f, 0.5f));
    Im::SeparatorText(title);
    Im::PopStyleVar(1);
}

// A row of chips: a strip of tabs in SkyUI's manner, each an icon and a
// word, flowing onto a second line when the panel is narrow. Chips are
// selectables with no label of their own; the icon and the word are painted
// over them through the draw list, so the layout cursor stays on the chip's
// full width and the next one lands beside it, not beside the text.
struct Chip
{
    std::string label;
    unsigned icon; // Font Awesome codepoint
    int id;
};

void DrawChips(const std::vector<Chip> &chips, int &selected)
{
    const auto *style = Im::GetStyle();
    const float padX = style->FramePadding.x;
    const float spacing = style->ItemSpacing.x;
    const float gap = padX;
    const float right = Im::GetCursorPosX() + Im::GetContentRegionAvail().x;
    auto *draw = Im::GetWindowDrawList();

    FontAwesome::PushSolid();
    const Im::ImFont *iconFont = Im::GetFont();
    const float iconSize = Im::GetFontSize();
    FontAwesome::Pop();

    bool first = true;
    for (const Chip &chip : chips)
    {
        const std::string icon = Utf8(chip.icon);
        float iconWidth = 0.0f;
        if (iconFont)
        {
            FontAwesome::PushSolid();
            iconWidth = TextWidth(icon);
            FontAwesome::Pop();
        }
        const float width = padX + iconWidth + gap + TextWidth(chip.label) + padX;

        if (!first)
        {
            Im::SameLine(0.0f, spacing);
            if (Im::GetCursorPosX() + width > right)
                Im::NewLine();
        }
        first = false;

        const Im::ImVec2 pos = Im::GetCursorScreenPos();
        // By id, not label: two summons of one creature share a name, and
        // one ImGui id for both left the second unclickable.
        if (Im::Selectable(("##chip" + std::to_string(chip.id)).c_str(), selected == chip.id, 0,
                           Im::ImVec2(width, 0.0f)))
            selected = chip.id;

        if (!draw)
            continue;
        const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
        if (iconFont)
            Im::ImDrawListManager::AddText(draw, iconFont, iconSize, {pos.x + padX, pos.y}, ink, icon.c_str());
        Im::ImDrawListManager::AddText(draw, {pos.x + padX + iconWidth + gap, pos.y}, ink, chip.label.c_str());
    }
}

// SkyUI's tab strip: All, then every category they have something in. Empty
// categories are left out, as SkyUI leaves them out -- a tab promising
// nothing is noise.
void DrawCategoryRow(const CharacterView &view, InventoryTabState &state)
{
    std::array<int, static_cast<std::size_t>(ItemCategory::COUNT)> counts{};
    for (const auto &item : view.inventory)
        ++counts[static_cast<std::size_t>(item.category)];

    std::vector<Chip> chips{{"All", kIconAll, -1}};
    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        if (counts[i] == 0)
            continue;
        const auto category = static_cast<ItemCategory>(i);
        chips.push_back({DisplayName(category), IconFor(category), static_cast<int>(i)});
    }

    // A category with nothing in it here -- the last potion drunk, or a
    // follower with no keys -- shows All rather than an empty table under a
    // tab that is not there, and leaves the choice standing for the pages
    // that have it.
    const int shared = g_inventoryCategory;
    state.category = shared >= 0 && counts[static_cast<std::size_t>(shared)] > 0 ? shared : -1;
    int chosen = state.category;
    DrawChips(chips, chosen);
    if (chosen != state.category)
        g_inventoryCategory = state.category = chosen;
}

// The inventory table's columns, by id rather than by position: which of
// them a list shows depends on the category, and a sort spec names the
// column by this id.
enum class Column : unsigned
{
    Name = 1,
    Type,
    Damage,
    Armor,
    Weight,
    Value,
    Equipped,
    School,
    Level,
    Cast,
    Cost,
    Left,
    Right,
    Magnitude,
    Remaining,
    Source
};

// A cell that says whether something is on -- in a hand, or worn -- with a
// tick, a pin beside it if we are keeping it there, and, when clickable, a
// click that rounds the three states: off -> kept on -> theirs to change ->
// off. The request goes to the game thread and the cell answers when the
// view comes back.
// A diagonal across the current cell, corner to corner: this cell does not
// apply -- a right hand for a shield, a hand for a cuirass. The corners are
// the table's own: the rectangle it fills a cell's background in, from the
// column's borders and the row's. Not rebuilt from the cursor, the text
// height and a padding constant: the rule table's rows are a frame high
// and padded by 2, and that guess ran the slash short of both corners.
//
// The rectangle's bottom is where the row has got to so far, so call this
// once the cells that set the row's height are drawn. The equip cells come
// after a row's text; the rule table's switch is its first column, and the
// row comes back to it at the end.
void SlashCell()
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImGuiTable *table = Im::GetCurrentTable();
    if (!draw || !table)
        return;
    const Im::ImRect cell = Im::TableGetCellBgRect(table, Im::TableGetColumnIndex());
    Im::ImDrawListManager::AddLine(draw, {cell.Min.x, cell.Max.y}, {cell.Max.x, cell.Min.y},
                                   Im::GetColorU32(Im::ImGuiCol_TableBorderStrong, 1.0f), 1.0f);
}

// One equip cell's state: what the cell draws, and what its column sorts
// by. The two read the SAME struct, from the same reader, so a cell cannot
// sort otherwise than it looks -- a spell above the follower's skill was
// drawn slashed and sorted among the unequipped once, the sort re-deriving
// the state on its own and reading less of it (2026-09-10).
struct EquipCell
{
    bool allowed{true};   // false: slashed, the cell cannot take the thing
    bool disabled{false}; // the row is dim: set aside by a pin, or above their skill
    bool on{false};
    bool pinned{false};
    bool banned{false};
};

// The readers, one per table and cell. The row's dimming is the same
// state: disabled or banned.
bool Disabled(const InventoryItem &item)
{
    return item.setAside;
}
EquipCell LeftCell(const InventoryItem &item)
{
    return {item.handItem && !item.rightOnly, Disabled(item), item.equippedLeft, item.pinnedLeft, item.banned};
}
EquipCell RightCell(const InventoryItem &item)
{
    return {item.handItem && !item.leftOnly, Disabled(item), item.equippedRight, item.pinnedRight, item.banned};
}
EquipCell WornCell(const InventoryItem &item)
{
    return {!item.handItem, Disabled(item), item.worn, item.pinned, item.banned};
}

bool VoiceEntry(const MagicEntry &entry)
{
    return entry.category == MagicCategory::Shouts || entry.category == MagicCategory::Powers;
}
// A spell above the follower's skill is disabled, and takes no hand at
// all, as the tactics menus offer it for neither casting nor pinning: one
// rule, not an equip-only state beside it.
bool Disabled(const MagicEntry &entry)
{
    return entry.setAside || entry.aboveSkill;
}
EquipCell LeftCell(const MagicEntry &entry)
{
    return {VoiceEntry(entry) || (entry.leftAllowed && !entry.aboveSkill), Disabled(entry), entry.equippedLeft,
            entry.pinnedLeft, entry.banned};
}
EquipCell RightCell(const MagicEntry &entry)
{
    return {VoiceEntry(entry) || (entry.rightAllowed && !entry.aboveSkill), Disabled(entry), entry.equippedRight,
            entry.pinnedRight, entry.banned};
}
EquipCell VoiceCell(const MagicEntry &entry)
{
    return {true, Disabled(entry), entry.equipped, entry.pinned, entry.banned};
}

// A click walks the cell round: unequipped, equipped, pinned, banned, and
// back to unequipped. Each state is one request to the game thread.
void OnCell(const char *id, const CharacterView &view, std::uint32_t form, const EquipCell &cell, Hand hand,
            bool clickable, const std::optional<ft::ItemVariant> &variant = std::nullopt,
            RE::ExtraDataList *row = nullptr)
{
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    if (!cell.allowed)
    {
        SlashCell();
        return;
    }
    if (clickable)
    {
        // Banned and pinned first: a pin whose thing the AI has swapped out
        // is still a pin, and a ban is a ban whatever is on. The player's
        // cell only equips and unequips: the pin and the ban are a leash on
        // the combat AI, and nothing chooses for the player.
        const WearRequest next = view.player   ? (cell.on ? WearRequest::Unequip : WearRequest::Equip)
                                 : cell.banned ? WearRequest::Unban
                                 : cell.pinned ? WearRequest::Ban
                                 : cell.on     ? WearRequest::Pin
                                               : WearRequest::Equip;
        if (CellClicked(id))
            RequestWear(view.id, form, next, hand, variant, row);
        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", cell.banned              ? "Banned. Click to unban."
                                 : cell.pinned            ? "Pinned. Click to ban."
                                 : cell.on && view.player ? "Equipped. Click to unequip."
                                 : cell.on                ? "Equipped. Click to pin."
                                                          : "Unequipped. Click to equip.");
    }
    DrawTickAt(pos, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), cell.on, cell.pinned, cell.banned);
}

// The order of an equip cell when its column is sorted, ascending: pinned,
// then equipped, then unequipped, then banned, then disabled -- the dim
// row, and the slashed cell that cannot take it at all. What the follower
// holds to comes first, what cannot be held last.
int CellRank(const EquipCell &cell)
{
    return !cell.allowed || cell.disabled ? 4 : cell.banned ? 3 : cell.pinned ? 0 : cell.on ? 1 : 2;
}

// Which columns an inventory list shows, by its category: the table lays
// them out by these, and the filter searches the cells they show.
struct ItemColumns
{
    bool weapons{false};     // a damage column
    bool armour{false};      // an armour column
    bool scrolls{false};     // cast and magnitude columns
    bool consumables{false}; // the second column says what the thing does, not its type
};

ItemColumns ColumnsOf(const InventoryTabState &state)
{
    const auto is = [&state](ItemCategory category) { return state.category == static_cast<int>(category); };
    ItemColumns columns;
    columns.weapons = is(ItemCategory::Weapons) || is(ItemCategory::Arrows);
    columns.armour = is(ItemCategory::Armor);
    columns.scrolls = is(ItemCategory::Scrolls);
    columns.consumables = columns.scrolls || is(ItemCategory::Potions) || is(ItemCategory::Poisons) ||
                          is(ItemCategory::Food) || is(ItemCategory::Ingredients);
    return columns;
}

// Is the row on the list: in its category, with the filter's text in a cell
// the list shows for it, the numbers as they print.
bool ItemShown(const InventoryItem &item, const InventoryTabState &state)
{
    if (state.category >= 0 && static_cast<int>(item.category) != state.category)
        return false;
    const ItemColumns columns = ColumnsOf(state);
    std::vector<std::string> cells{item.name, columns.consumables ? item.effect : item.type, Fmt("%.1f", item.weight),
                                   std::to_string(item.value)};
    if (const float stat = columns.weapons ? item.damage : columns.armour ? item.armor : 0.0f; stat > 0.0f)
        cells.push_back(Fmt("%.0f", stat));
    if (columns.scrolls)
    {
        cells.push_back(item.cast);
        if (item.magnitude > 0.0f)
            cells.push_back(Fmt("%.0f", item.magnitude));
    }
    return AnyContains(cells, g_inventoryFilter);
}

// The rows to show, in the order the table's header asks for. Sorted every
// frame rather than on change: a hundred pointers is nothing, and the set
// itself changes with the filter and with what they pick up.
std::vector<const InventoryItem *> VisibleItems(const CharacterView &view, const InventoryTabState &state)
{
    std::vector<const InventoryItem *> rows;
    for (const auto &item : view.inventory)
    {
        if (ItemShown(item, state))
            rows.push_back(&item);
    }

    const auto *specs = Im::TableGetSortSpecs();
    if (!specs || specs->SpecsCount < 1 || !specs->Specs)
        return rows;
    const auto &spec = specs->Specs[0];
    const bool ascending = spec.SortDirection != Im::ImGuiSortDirection_Descending;

    const auto compare = [&](const InventoryItem &a, const InventoryItem &b) -> int {
        const auto number = [](float x, float y) { return x < y ? -1 : (x > y ? 1 : 0); };
        const auto rank = [&](auto of) { return number(static_cast<float>(of(a)), static_cast<float>(of(b))); };
        switch (static_cast<Column>(spec.ColumnUserID))
        {
        case Column::Type:
            // The consumables' lists show the effect in this column.
            return a.effect.empty() && b.effect.empty() ? a.type.compare(b.type) : a.effect.compare(b.effect);
        case Column::Damage:
            return number(a.damage, b.damage);
        case Column::Armor:
            return number(a.armor, b.armor);
        case Column::Cast:
            return a.cast.compare(b.cast);
        case Column::Magnitude:
            return number(a.magnitude, b.magnitude);
        case Column::Weight:
            return number(a.weight, b.weight);
        case Column::Value:
            return number(static_cast<float>(a.value), static_cast<float>(b.value));
        case Column::Equipped:
            return rank([](const InventoryItem &i) { return CellRank(WornCell(i)); });
        case Column::Left:
            return rank([](const InventoryItem &i) { return CellRank(LeftCell(i)); });
        case Column::Right:
            return rank([](const InventoryItem &i) { return CellRank(RightCell(i)); });
        case Column::Name:
        default:
            return a.name.compare(b.name);
        }
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const InventoryItem *a, const InventoryItem *b) {
        const int c = compare(*a, *b);
        if (c == 0)
            return a->name < b->name; // ties by name, whichever way the column goes
        return ascending ? c < 0 : c > 0;
    });
    return rows;
}

// The list: SkyUI's columns, sortable by clicking a heading, one row per
// kind of item with the count in brackets. Clicking a row opens it.
void DrawInventoryList(const CharacterView &view, InventoryTabState &state)
{
    Im::Spacing();
    DrawCategoryRow(view, state);
    Im::Spacing();

    // Counted against the category, not the whole bag: on Weapons, "3 of
    // 3" until the filter box takes some away. Only All counts everything.
    std::size_t inCategory = 0;
    for (const auto &item : view.inventory)
        inCategory += (state.category < 0 || static_cast<int>(item.category) == state.category) ? 1 : 0;
    FilterRow(
        "##invfilter", g_inventoryFilter, sizeof(g_inventoryFilter),
        [&] {
            return static_cast<std::size_t>(
                std::count_if(view.inventory.begin(), view.inventory.end(),
                              [&](const InventoryItem &item) { return ItemShown(item, state); }));
        },
        inCategory, "items");
    Im::Spacing();

    // Which columns this list has. A stat column only where the stat means
    // something -- damage for weapons, rating for armour -- and an Equipped
    // column only where something can be equipped. SkyUI's lists differ the
    // same way.
    const ItemColumns columns = ColumnsOf(state);
    const bool weapons = columns.weapons;
    const bool armour = columns.armour;
    // Hand columns where something is held in a hand; an Equipped column
    // where something is worn. A cell that does not apply to its row -- a
    // right hand for a shield, a hand for a cuirass -- is slashed. Not on
    // All: three equip columns leave no room for the rest, and equipping
    // is done from the category lists.
    bool anyHand = false;
    bool anyWorn = false;
    for (const auto &item : view.inventory)
    {
        if (state.category < 0 || static_cast<int>(item.category) != state.category)
            continue;
        anyHand = anyHand || item.handItem;
        anyWorn = anyWorn || (item.equipable && !item.handItem);
    }

    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_Sortable;
    const float gutter = kCellPadX * 2.0f;
    // A consumable list's second column is what the thing does, not a Type
    // that would only repeat the heading.
    const bool scrolls = columns.scrolls;
    const bool consumables = columns.consumables;
    const auto typeText = [consumables](const InventoryItem &item) -> const std::string & {
        return consumables ? item.effect : item.type;
    };
    float typeWidth = TextWidth(consumables ? "Effect" : "Type");
    for (const auto &item : view.inventory)
        typeWidth = (std::max)(typeWidth, TextWidth(typeText(item)));
    // A sortable heading keeps room beside its label for the sort arrow, and
    // a column sized to the label alone clips it to "D...". The allowance is
    // ImGui's own (TableHeader: FontSize * 0.65 + FramePadding.x), so each
    // fixed column is exactly its heading-with-arrow or its widest content,
    // and the Name column gets everything that is left.
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    const float damageWidth = (std::max)(TextWidth("Dmg") + arrow, TextWidth("999")) + gutter;
    const float armorWidth = (std::max)(TextWidth("Armor") + arrow, TextWidth("999")) + gutter;
    const float weightWidth = (std::max)(TextWidth("Wgt") + arrow, TextWidth("999.9")) + gutter;
    const float valueWidth = (std::max)(TextWidth("Val") + arrow, TextWidth("99999")) + gutter;
    // Content is the tick and, pinned, the pin beside it: two glyph boxes.
    const float wornWidth = (std::max)(TextWidth("Equipped") + arrow, Im::GetFontSize() * 2.0f) + gutter;

    const float handWidth = (std::max)(TextWidth("Right") + arrow, Im::GetFontSize() * 2.0f) + gutter;
    // A scroll's list: Cast and Mag after the effect, as a spell's list.
    float castWidth = TextWidth("Cast") + arrow;
    if (scrolls)
        for (const auto &item : view.inventory)
            castWidth = (std::max)(castWidth, TextWidth(item.cast));
    castWidth += gutter;
    const float magWidth = (std::max)(TextWidth("Mag") + arrow, TextWidth("999")) + gutter;
    const int columnCount =
        4 + ((weapons || armour) ? 1 : 0) + (scrolls ? 2 : 0) + (anyHand ? 2 : 0) + (anyWorn ? 1 : 0);

    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("inventory", columnCount, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn("Name", Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Name));
    Im::TableSetupColumn(consumables ? "Effect" : "Type", Im::ImGuiTableColumnFlags_WidthFixed, typeWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Type));
    // The stat, highest first on the first click: for a weapon or a piece
    // of armour it is the number, and the rest wait on the item's page.
    if (weapons)
        Im::TableSetupColumn("Dmg",
                             Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                             damageWidth, static_cast<Im::ImGuiID>(Column::Damage));
    else if (armour)
        Im::TableSetupColumn("Armor",
                             Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                             armorWidth, static_cast<Im::ImGuiID>(Column::Armor));
    if (scrolls)
    {
        Im::TableSetupColumn("Cast", Im::ImGuiTableColumnFlags_WidthFixed, castWidth,
                             static_cast<Im::ImGuiID>(Column::Cast));
        Im::TableSetupColumn("Mag",
                             Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                             magWidth, static_cast<Im::ImGuiID>(Column::Magnitude));
    }
    Im::TableSetupColumn("Wgt", Im::ImGuiTableColumnFlags_WidthFixed, weightWidth,
                         static_cast<Im::ImGuiID>(Column::Weight));
    Im::TableSetupColumn("Val", Im::ImGuiTableColumnFlags_WidthFixed, valueWidth,
                         static_cast<Im::ImGuiID>(Column::Value));
    // Ascending first, like the rest: pinned, equipped, unequipped, then the
    // slashed cells. "Equipped", not "Worn": it is the word the item's page
    // uses, and the one that fits a weapon.
    if (anyHand)
    {
        Im::TableSetupColumn("Left", Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Left));
        Im::TableSetupColumn("Right", Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Right));
    }
    if (anyWorn)
    {
        Im::TableSetupColumn("Equipped", Im::ImGuiTableColumnFlags_WidthFixed, wornWidth,
                             static_cast<Im::ImGuiID>(Column::Equipped));
    }
    Im::TableHeadersRow();

    const std::vector<const InventoryItem *> rows = VisibleItems(view, state);
    for (const InventoryItem *item : rows)
    {
        // Ids by the row's key, not the form: the plain stack and an
        // enchanted copy share the form.
        const auto key = static_cast<unsigned long long>(item->Key());
        char buf[32];
        std::snprintf(buf, sizeof(buf), "##item%016llX", key);

        Im::TableNextRow(0, 0.0f);
        // Set aside -- kept from the combat AI while a pinned spell holds a
        // hand it would take -- the whole row goes to the disabled colour.
        // Not on All, where nothing can be equipped and the dimming would
        // have no cell to explain it.
        const bool dim = (Disabled(*item) || item->banned) && state.category >= 0;
        const DimText grey(dim);
        Im::TableSetColumnIndex(0);

        // The NAME is the click target for the detail page, not the row: the
        // Worn cell has a click of its own. Highlighted through the cell
        // background for the reason DrawSections gives.
        Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
        {
            state.detail = item->Key();
            state.openedFrom = Tab::Inventory;
        }
        // Over the whole cell: the Selectable is the last item here. The
        // reason and nothing else; what a pin means belongs in a help
        // section, not on every row.
        if (dim && Im::IsItemHovered(0))
            Im::SetTooltip("%s", item->banned ? "Banned" : item->asideBy.c_str());
        Im::SetCursorScreenPos(pos);
        std::string name = item->name;
        if (item->count > 1)
            name += " (" + std::to_string(item->count) + ")";
        // The enchanted tint would override the disabled colour a set-aside
        // row was pushed: dimmed wins, or the row does not read as greyed.
        if (const Im::ImVec4 *tint = dim ? nullptr : NameTint(*item))
            Im::TextColored(*tint, "%s", name.c_str());
        else
            Im::Text("%s", name.c_str());
        NameBadges(*item, dim);

        Im::TableNextColumn();
        Im::Text("%s", typeText(*item).c_str());

        char num[32];
        if (weapons || armour)
        {
            Im::TableNextColumn();
            const float stat = weapons ? item->damage : item->armor;
            if (stat > 0.0f)
            {
                std::snprintf(num, sizeof(num), "%.0f", stat);
                TextRightInCell(num);
            }
        }
        if (scrolls)
        {
            Im::TableNextColumn();
            Im::Text("%s", item->cast.c_str());
            Im::TableNextColumn();
            if (item->magnitude > 0.0f)
            {
                std::snprintf(num, sizeof(num), "%.0f", item->magnitude);
                TextRightInCell(num);
            }
        }

        Im::TableNextColumn();
        std::snprintf(num, sizeof(num), "%.1f", item->weight);
        TextRightInCell(num);

        Im::TableNextColumn();
        TextRightInCell(std::to_string(item->value));

        // Equipped: a tick if it is, a pin beside it if we are the ones
        // keeping it so. A click equips or unequips; the request goes to the
        // game thread and the column answers when the view comes back.
        // A cell that does not apply to this row is slashed; a row that can
        // be equipped nowhere (a potion) is left blank.
        if (anyHand)
        {
            std::snprintf(buf, sizeof(buf), "##left%016llX", key);
            Im::TableNextColumn();
            if (item->equipable)
                OnCell(buf, view, item->form, LeftCell(*item), Hand::Left, true, item->variant, item->row);
            std::snprintf(buf, sizeof(buf), "##right%016llX", key);
            Im::TableNextColumn();
            if (item->equipable)
                OnCell(buf, view, item->form, RightCell(*item), Hand::Right, true, item->variant, item->row);
        }
        if (anyWorn)
        {
            std::snprintf(buf, sizeof(buf), "##wear%016llX", key);
            Im::TableNextColumn();
            if (item->equipable)
                OnCell(buf, view, item->form, WornCell(*item), Hand::None, true, item->variant, item->row);
        }
    }
    Im::EndTable();
    Im::PopStyleVar(1);

    // What it weighs, under the list: the reason to look in a follower's bag
    // is usually to decide whether they can carry more.
    Im::Spacing();
    char carried[64];
    std::snprintf(carried, sizeof(carried), "Carrying %.0f / %.0f", view.carriedWeight, view.carryCapacity);
    const auto *style = Im::GetStyle();
    const float inset = style->ItemSpacing.x;
    const float rightEdge = Im::GetCursorPosX() + Im::GetContentRegionAvail().x - inset;
    Im::SetCursorPosX((std::max)(Im::GetCursorPosX(), rightEdge - TextWidth(carried)));
    if (view.carryCapacity > 0.0f && view.carriedWeight > view.carryCapacity)
        Im::TextColored(kAlarm, "%s", carried);
    else
        Im::Text("%s", carried);
}

// One item: a back arrow, the name, then the numbers as sheet sections and
// the prose beneath, each under its own heading only when there is any.
void DrawItemDetail(const InventoryItem &item, InventoryTabState &state)
{
    Im::Spacing();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (GlyphButton("back", Im::GetFrameHeight(), Glyph::Back))
    {
        // Back to wherever this was opened from: the list, or the sheet.
        state.detail = 0;
        if (state.openedFrom != Tab::Inventory)
            state.select = state.openedFrom;
    }
    Im::PopStyleVar(1);

    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    if (const Im::ImVec4 *tint = NameTint(item))
        Im::TextColored(*tint, "%s", item.name.c_str());
    else
        Im::Text("%s", item.name.c_str());
    NameBadges(item, false, true);
    Im::SameLine(0.0f, kCellPadX * 2.0f);
    Im::AlignTextToFramePadding();
    Im::TextDisabled("%s", item.type.c_str());

    Im::Spacing();
    DrawSections(item.detail, false);

    // The enchantment as one headed row; then the effects, a table in the
    // perk page's shape with the author's text wrapped in its last column,
    // each row opening on its conditions, greyed where they do not hold.
    if (!item.enchantment.rows.empty())
        DrawSections({item.enchantment}, true, {}, nullptr, {}, "Name", "Charge");
    if (!item.effectsTable.rows.empty())
    {
        DrawSections(
            {item.effectsTable}, true, {}, nullptr,
            [](const SheetRow &entry, const std::string &key, float left, float right) {
                DrawConditionDrawer(entry, key, left, right);
            },
            "Name", "Effect", WithDescription());
    }
    // The poison as the enchantment is drawn, so an enchanted and poisoned
    // blade reads as two things, which it is.
    if (!item.poison.rows.empty())
        DrawSections({item.poison}, true, {}, nullptr, {}, "Name", "Hits Left");
    if (!item.poisonEffects.rows.empty())
    {
        DrawSections(
            {item.poisonEffects}, true, {}, nullptr,
            [](const SheetRow &entry, const std::string &key, float left, float right) {
                DrawConditionDrawer(entry, key, left, right);
            },
            "Name", "Effect", WithDescription());
    }
    if (!item.description.empty())
    {
        CentredHeading("Description");
        Im::TextWrapped("%s", item.description.c_str());
        Im::Spacing();
    }
}

// The Inventory tab. Either the list or one item, never both: the detail
// takes the item's place rather than opening beside it, because the panel is
// not wide enough for two columns of text at this font size, and a back
// arrow is a gesture everyone already knows.
void DrawInventory(const CharacterView &view)
{
    InventoryTabState &state = g_inventoryTabs[view.id];

    if (state.detail != 0)
    {
        for (const auto &item : view.inventory)
        {
            if (item.Key() == state.detail)
            {
                DrawItemDetail(item, state);
                return;
            }
        }
        // Gone -- drunk, dropped, or handed over. Back to the list.
        state.detail = 0;
    }

    if (view.inventory.empty())
    {
        Im::Spacing();
        Im::TextDisabled("Nothing carried.");
        return;
    }
    DrawInventoryList(view, state);
}

// --- magic -------------------------------------------------------------------

// SkyUI's Magic menu: All, the five schools, Shouts, Powers.
struct MagicTabState
{
    std::uint32_t detail{0}; // the entry open in detail; 0 for the list
    // This frame's category: g_magicCategory, or All on a page with nothing
    // in it.
    int category{-1};
    Tab openedFrom{Tab::Magic}; // where the detail page returns to: the list, or the Character sheet
};

std::unordered_map<ft::ActorId, MagicTabState> g_magicTabs;
char g_magicFilter[64]{};

unsigned IconFor(MagicCategory category)
{
    switch (category)
    {
    case MagicCategory::Alteration:
        return 0xF1BB; // tree
    case MagicCategory::Conjuration:
        return 0xF52B; // door-open
    case MagicCategory::Destruction:
        return 0xF06D; // fire
    case MagicCategory::Illusion:
        return 0xF72B; // wand-sparkles
    case MagicCategory::Restoration:
        return 0xE4FB; // hands-holding-circle
    case MagicCategory::Other:
        return 0xF043; // droplet, for the drain that put the chip here
    case MagicCategory::Shouts:
        return 0xF72E; // wind
    case MagicCategory::Powers:
    default:
        return 0xE05D; // hand-sparkles
    }
}

constexpr unsigned kIconMagicAll = 0xF6E8; // hat-wizard
// The Summons tab's chips: a summoned creature, a raised corpse.
constexpr unsigned kIconSummoned = 0xF6D5; // dragon
constexpr unsigned kIconRaised = 0xF54C;   // skull

// A list of powers or shouts, which are readied rather than held: no school,
// level or cost columns, and one Equipped cell.
bool VoiceList(const MagicTabState &state)
{
    return state.category == static_cast<int>(MagicCategory::Shouts) ||
           state.category == static_cast<int>(MagicCategory::Powers);
}

// Is the entry on the list: in its category, with the filter's text in a
// cell the list shows for it.
bool MagicShown(const MagicEntry &entry, const MagicTabState &state)
{
    if (state.category >= 0 && static_cast<int>(entry.category) != state.category)
        return false;
    const bool voice = VoiceEntry(entry);
    std::vector<std::string> cells{entry.name, entry.type, entry.cast};
    if (state.category < 0 && !voice)
        cells.push_back(entry.school);
    if (!VoiceList(state) && !voice)
    {
        cells.push_back(entry.level);
        cells.push_back(entry.cost);
    }
    if (entry.magnitude > 0.0f)
        cells.push_back(Fmt("%.0f", entry.magnitude));
    return AnyContains(cells, g_magicFilter);
}

std::vector<const MagicEntry *> VisibleMagic(const CharacterView &view, const MagicTabState &state)
{
    std::vector<const MagicEntry *> rows;
    for (const auto &entry : view.magic)
    {
        if (MagicShown(entry, state))
            rows.push_back(&entry);
    }

    const auto *specs = Im::TableGetSortSpecs();
    if (!specs || specs->SpecsCount < 1 || !specs->Specs)
        return rows;
    const auto &spec = specs->Specs[0];
    const bool ascending = spec.SortDirection != Im::ImGuiSortDirection_Descending;

    const auto compare = [&](const MagicEntry &a, const MagicEntry &b) -> int {
        const auto number = [](float x, float y) { return x < y ? -1 : (x > y ? 1 : 0); };
        const auto rank = [&](auto of) { return number(static_cast<float>(of(a)), static_cast<float>(of(b))); };
        switch (static_cast<Column>(spec.ColumnUserID))
        {
        case Column::School:
            return a.school.compare(b.school);
        case Column::Type:
            return a.type.compare(b.type);
        case Column::Level:
            return number(static_cast<float>(a.levelValue), static_cast<float>(b.levelValue));
        case Column::Cast:
            return a.castValue != b.castValue ? number(static_cast<float>(a.castValue), static_cast<float>(b.castValue))
                                              : a.cast.compare(b.cast);
        case Column::Cost:
            return number(a.costValue, b.costValue);
        case Column::Magnitude:
            return number(a.magnitude, b.magnitude);
        case Column::Equipped:
            return rank([](const MagicEntry &e) { return CellRank(VoiceCell(e)); });
        case Column::Left:
            return rank([](const MagicEntry &e) { return CellRank(LeftCell(e)); });
        case Column::Right:
            return rank([](const MagicEntry &e) { return CellRank(RightCell(e)); });
        case Column::Name:
        default:
            return a.name.compare(b.name);
        }
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const MagicEntry *a, const MagicEntry *b) {
        const int c = compare(*a, *b);
        if (c == 0)
            return a->name < b->name;
        return ascending ? c < 0 : c > 0;
    });
    return rows;
}

void DrawMagicList(const CharacterView &view, MagicTabState &state)
{
    Im::Spacing();
    {
        std::array<int, static_cast<std::size_t>(MagicCategory::COUNT)> counts{};
        for (const auto &entry : view.magic)
            ++counts[static_cast<std::size_t>(entry.category)];
        std::vector<Chip> chips{{"All", kIconMagicAll, -1}};
        for (std::size_t i = 0; i < counts.size(); ++i)
        {
            if (counts[i] == 0)
                continue;
            const auto category = static_cast<MagicCategory>(i);
            chips.push_back({DisplayName(category), IconFor(category), static_cast<int>(i)});
        }
        // As the Inventory tab's (DrawCategoryRow): All where the shared
        // category has nothing, the choice kept for the pages that have it.
        const int shared = g_magicCategory;
        state.category = shared >= 0 && counts[static_cast<std::size_t>(shared)] > 0 ? shared : -1;
        int chosen = state.category;
        DrawChips(chips, chosen);
        if (chosen != state.category)
            g_magicCategory = state.category = chosen;
    }
    Im::Spacing();

    // Counted against the category, as the Inventory tab counts: on
    // Destruction, "3 of 3" until the filter box takes some away.
    std::size_t inCategory = 0;
    for (const auto &entry : view.magic)
        inCategory += (state.category < 0 || static_cast<int>(entry.category) == state.category) ? 1 : 0;
    FilterRow(
        "##magicfilter", g_magicFilter, sizeof(g_magicFilter),
        [&] {
            return static_cast<std::size_t>(
                std::count_if(view.magic.begin(), view.magic.end(),
                              [&](const MagicEntry &entry) { return MagicShown(entry, state); }));
        },
        inCategory, "spells");
    Im::Spacing();

    // Which columns. Only the All list has a School column; every list has
    // the Type. Spells show a cell per hand;
    // powers and shouts, which are selected rather than held, show one
    // Equipped cell for the voice slot, clicked like a hand cell: ready
    // it, pin it, put it away. One voice pin sets every other power and
    // shout aside, as a pinned quiver does the arrows.
    const bool voiceList = VoiceList(state);

    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_Sortable;
    const float gutter = kCellPadX * 2.0f;
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    float schoolWidth = TextWidth("School") + arrow;
    float typeWidth = TextWidth("Type") + arrow;
    float levelWidth = TextWidth("Level") + arrow;
    float castWidth = TextWidth("Cast") + arrow;
    float costWidth = TextWidth("Cost") + arrow;
    for (const auto &entry : view.magic)
    {
        schoolWidth = (std::max)(schoolWidth, TextWidth(entry.school));
        typeWidth = (std::max)(typeWidth, TextWidth(entry.type));
        levelWidth = (std::max)(levelWidth, TextWidth(entry.level));
        castWidth = (std::max)(castWidth, TextWidth(entry.cast));
        costWidth = (std::max)(costWidth, TextWidth(entry.cost));
    }
    const float magnitudeWidth = (std::max)(TextWidth("Mag") + arrow, TextWidth("999")) + gutter;
    const float handWidth = (std::max)(TextWidth("Right") + arrow, Im::GetFontSize() * 2.0f) + gutter;
    const float wornWidth = (std::max)(TextWidth("Equipped") + arrow, Im::GetFontSize()) + gutter;

    // No equip columns on All, as the Inventory tab has it: equipping is
    // done from the school lists.
    const bool allList = state.category < 0;
    // All: Name, School, Type, Level, Mag, Cost, Cast. A school's list: the
    // same less School, plus the two hand cells. A voice list (shouts,
    // powers) has no school, level or cost to show: Name, Type, Mag, Cast,
    // Equipped.
    const int columnCount = voiceList ? 5 : allList ? 7 : 8;

    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("magic", columnCount, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn("Name", Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Name));
    if (allList)
        Im::TableSetupColumn("School", Im::ImGuiTableColumnFlags_WidthFixed, schoolWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::School));
    Im::TableSetupColumn("Type", Im::ImGuiTableColumnFlags_WidthFixed, typeWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Type));
    if (!voiceList)
        Im::TableSetupColumn("Level", Im::ImGuiTableColumnFlags_WidthFixed, levelWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::Level));
    Im::TableSetupColumn("Mag", Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                         magnitudeWidth, static_cast<Im::ImGuiID>(Column::Magnitude));
    if (!voiceList)
        Im::TableSetupColumn("Cost", Im::ImGuiTableColumnFlags_WidthFixed, costWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::Cost));
    // Cast: what it does when cast -- Self, Touch, Spray, Projectile, Target,
    // Location -- delivery and casting type in one word.
    Im::TableSetupColumn("Cast", Im::ImGuiTableColumnFlags_WidthFixed, castWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Cast));
    if (allList)
    {
        // no equip columns
    }
    else if (voiceList)
    {
        Im::TableSetupColumn("Equipped", Im::ImGuiTableColumnFlags_WidthFixed, wornWidth,
                             static_cast<Im::ImGuiID>(Column::Equipped));
    }
    else
    {
        Im::TableSetupColumn("Left", Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Left));
        Im::TableSetupColumn("Right", Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Right));
    }
    Im::TableHeadersRow();

    const std::vector<const MagicEntry *> rows = VisibleMagic(view, state);
    for (const MagicEntry *entry : rows)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "##magic%08X", entry->form);

        Im::TableNextRow(0, 0.0f);
        // Kept from the AI -- a pin holds a hand it would take -- or above their
        // skill, so the AI would not choose it: the whole row is drawn in
        // the disabled colour, ticks included, since every glyph takes the
        // text colour.
        const bool dim = (Disabled(*entry) || entry->banned) && !allList;
        const DimText grey(dim);
        Im::TableSetColumnIndex(0);
        Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
        {
            state.detail = entry->form;
            state.openedFrom = Tab::Magic; // back to the list, wherever the last page was opened from
        }
        // Why the row is dimmed, over the whole cell: asked of the
        // Selectable, before the name is drawn over it. A spell above the
        // follower's skill says so first, even when a pin shadows it too:
        // the skill is the reason nothing about the row can change, the
        // pin only the reason for now.
        if (dim && entry->aboveSkill && Im::IsItemHovered(0))
        {
            // The two labels right-aligned to one edge, so the school and
            // the numbers line up beneath each other.
            Im::BeginTooltip();
            const float labelWidth = (std::max)(TextWidth("Needs:"), TextWidth("Has:"));
            const auto line = [&](const char *label, int value) {
                Im::SetCursorPosX(Im::GetCursorPos().x + labelWidth - TextWidth(label));
                Im::Text("%s", label);
                Im::SameLine(0.0f, -1.0f);
                Im::Text("%s (%d)", entry->school.c_str(), value);
            };
            line("Needs:", entry->levelValue);
            line("Has:", entry->skill);
            Im::EndTooltip();
        }
        else if (dim && entry->banned && Im::IsItemHovered(0))
            Im::SetTooltip("%s", "Banned");
        else if (dim && entry->setAside && Im::IsItemHovered(0))
            Im::SetTooltip("%s", entry->asideBy.c_str());
        Im::SetCursorScreenPos(pos);
        Im::Text("%s", entry->name.c_str());

        // A power or a shout has no school, level or cost: those cells stay
        // empty rather than saying "Power" or "0".
        const bool voice = VoiceEntry(*entry);
        if (allList)
        {
            Im::TableNextColumn();
            if (!voice)
                Im::Text("%s", entry->school.c_str());
        }
        Im::TableNextColumn();
        Im::Text("%s", entry->type.c_str());
        if (!voiceList)
        {
            Im::TableNextColumn();
            if (!voice)
                Im::Text("%s", entry->level.c_str());
        }
        Im::TableNextColumn();
        if (entry->magnitude > 0.0f)
        {
            char num[32];
            std::snprintf(num, sizeof(num), "%.0f", entry->magnitude);
            TextRightInCell(num);
        }
        if (!voiceList)
        {
            Im::TableNextColumn();
            if (!voice)
            {
                TextRightInCell(entry->cost);
                // What the follower pays and why, on the number.
                if (!entry->costBreakdown.empty() && Im::IsItemHovered(0))
                    BreakdownTooltip(entry->costBreakdown);
            }
        }
        Im::TableNextColumn();
        Im::Text("%s", entry->cast.c_str());

        if (allList)
        {
            // no equip cells
        }
        else if (voiceList)
        {
            std::snprintf(buf, sizeof(buf), "##voice%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view, entry->form, VoiceCell(*entry), Hand::None, true);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "##left%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view, entry->form, LeftCell(*entry), Hand::Left, !voice);
            std::snprintf(buf, sizeof(buf), "##right%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view, entry->form, RightCell(*entry), Hand::Right, !voice);
        }
    }
    Im::EndTable();
    Im::PopStyleVar(1);
}

void DrawMagicDetail(const MagicEntry &entry, MagicTabState &state)
{
    Im::Spacing();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (GlyphButton("back", Im::GetFrameHeight(), Glyph::Back))
        state.detail = 0;
    Im::PopStyleVar(1);

    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    Im::Text("%s", entry.name.c_str());
    Im::SameLine(0.0f, kCellPadX * 2.0f);
    Im::AlignTextToFramePadding();
    Im::TextDisabled("%s", entry.school.c_str());

    Im::Spacing();
    DrawSections(entry.detail, false);

    // The effects, as the item page has them: the record with the author's
    // text wrapped in the last column.
    if (!entry.effectTables.empty())
    {
        DrawSections(
            entry.effectTables, true, {}, nullptr,
            [](const SheetRow &line, const std::string &key, float left, float right) {
                DrawConditionDrawer(line, key, left, right);
            },
            "Name", "Effect", WithDescription());
    }
    if (!entry.description.empty())
    {
        CentredHeading("Description");
        Im::TextWrapped("%s", entry.description.c_str());
        Im::Spacing();
    }
}

// The Effects tab: what is running on the follower, as the game's own
// Active Effects list shows it, with what is left of each and where it
// comes from. Read on the tick, so with the clock frozen behind the panel
// the times stand still, as they do in the game's own menu. A name opens
// the effect's page, as on the Inventory and Magic tabs.
struct EffectsTabState
{
    // The row open in detail, by effect, source and the worn item behind
    // the source; 0 for the list. The item is part of it because two
    // pieces enchanted alike share one enchantment form, and their rows
    // were one row to the panel: the necklace's clicks went to the ring's
    // (Remiel's Silver Ruby pair, 2026-09-11).
    std::uint32_t detailForm{0};
    std::uint32_t detailSource{0};
    std::uint32_t detailLink{0};
};

std::unordered_map<ft::ActorId, EffectsTabState> g_effectsTabs;
char g_effectsFilter[64]{};

// Does the row hold the filter's text in a cell the table shows?
bool EffectShown(const EffectRow &row)
{
    std::vector<std::string> cells{row.name, row.remainingText, row.source};
    if (row.magnitude != 0.0f)
        cells.push_back(Fmt("%.0f", row.magnitude));
    return AnyContains(cells, g_effectsFilter);
}

// The rows that pass the filter, in the order the header asks for.
std::vector<const EffectRow *> VisibleEffects(const CharacterView &view)
{
    std::vector<const EffectRow *> rows;
    for (const auto &row : view.effects)
    {
        if (EffectShown(row))
            rows.push_back(&row);
    }

    const auto *specs = Im::TableGetSortSpecs();
    if (!specs || specs->SpecsCount < 1 || !specs->Specs)
        return rows;
    const auto &spec = specs->Specs[0];
    const bool ascending = spec.SortDirection != Im::ImGuiSortDirection_Descending;

    const auto compare = [&](const EffectRow &a, const EffectRow &b) -> int {
        const auto number = [](float x, float y) { return x < y ? -1 : (x > y ? 1 : 0); };
        // No duration sorts after every duration: it is the one that never
        // runs out.
        const auto left = [](const EffectRow &e) { return e.remaining < 0.0f ? 1.0e9f : e.remaining; };
        switch (static_cast<Column>(spec.ColumnUserID))
        {
        case Column::Magnitude:
            return number(a.magnitude, b.magnitude);
        case Column::Remaining:
            return number(left(a), left(b));
        case Column::Source:
            return a.source.compare(b.source);
        case Column::Name:
        default:
            return a.name.compare(b.name);
        }
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const EffectRow *a, const EffectRow *b) {
        const int c = compare(*a, *b);
        if (c == 0)
            return a->name < b->name;
        return ascending ? c < 0 : c > 0;
    });
    return rows;
}

// The page an effect's source opens, if it has one: the worn piece on the
// Inventory tab, the spell on the Magic tab; None for a source with no
// page of its own -- a racial ability, a potion drunk up. The tabs' own
// lists are the rule for what has a page.
Tab SourcePage(const CharacterView &view, std::uint32_t form)
{
    if (form == 0)
        return Tab::None;
    if (std::any_of(view.inventory.begin(), view.inventory.end(),
                    [form](const InventoryItem &item) { return item.form == form; }))
        return Tab::Inventory;
    if (std::any_of(view.magic.begin(), view.magic.end(),
                    [form](const MagicEntry &entry) { return entry.form == form; }))
        return Tab::Magic;
    return Tab::None;
}

// The page a link to a form opens on the Inventory tab: of the form's
// rows, the worn one, else the first; an enchanted piece is a row of its
// own, so the form alone (the plain stack's key) would open nothing.
std::uint64_t ItemPageOf(const CharacterView &view, std::uint32_t form)
{
    const InventoryItem *page = nullptr;
    for (const auto &item : view.inventory)
    {
        if (item.form == form && (!page || (item.worn && !page->worn)))
            page = &item;
    }
    return page ? page->Key() : form;
}

// Open it, with the back arrow returning to the Effects tab.
void OpenSourcePage(const CharacterView &view, std::uint32_t form)
{
    auto &inventory = g_inventoryTabs[view.id];
    switch (SourcePage(view, form))
    {
    case Tab::Inventory:
        inventory.detail = ItemPageOf(view, form);
        inventory.openedFrom = Tab::Effects;
        inventory.select = Tab::Inventory;
        break;
    case Tab::Magic:
        g_magicTabs[view.id].detail = form;
        g_magicTabs[view.id].openedFrom = Tab::Effects;
        inventory.select = Tab::Magic;
        break;
    default:
        break;
    }
}

void DrawEffectDetail(const EffectRow &row, EffectsTabState &state, const CharacterView &view)
{
    Im::Spacing();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (GlyphButton("back", Im::GetFrameHeight(), Glyph::Back))
        state = {};
    Im::PopStyleVar(1);

    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    Im::Text("%s", row.name.c_str());

    Im::Spacing();
    // One row, in the table the item page lists its effects in, with the
    // source as a last column: what the effect does, for how long, whether
    // the game hides it, and where it comes from, its conditions beneath.
    // The source is a link to its page where it has one (SourcePage); a
    // source with none loses its link here rather than lighting a cell
    // that would go nowhere.
    std::vector<SheetSection> sections = row.detail;
    for (auto &section : sections)
        for (auto &line : section.rows)
            if (SourcePage(view, line.form) == Tab::None)
                line.form = 0;
    std::vector<ExtraColumn> columns = kEffectColumns;
    columns.push_back({"Source", [](const SheetRow &r) { return r.link; }, false, true});
    DrawSections(
        sections, true, [&view](std::uint32_t form) { OpenSourcePage(view, form); }, nullptr,
        [](const SheetRow &entry, const std::string &key, float left, float right) {
            DrawConditionDrawer(entry, key, left, right);
        },
        "Name", "Effect", columns);

    if (!row.description.empty())
    {
        CentredHeading("Description");
        Im::TextWrapped("%s", row.description.c_str());
        Im::Spacing();
    }
}

void DrawEffects(const CharacterView &view)
{
    auto &state = g_effectsTabs[view.id];
    if (state.detailForm != 0)
    {
        for (const auto &row : view.effects)
        {
            if (row.form == state.detailForm && row.sourceForm == state.detailSource &&
                row.linkForm == state.detailLink)
            {
                DrawEffectDetail(row, state, view);
                return;
            }
        }
        // It has run out since the page was opened: back to the list.
        state = {};
    }

    Im::Spacing();
    FilterRow(
        "##effectsfilter", g_effectsFilter, sizeof(g_effectsFilter),
        [&] { return static_cast<std::size_t>(std::count_if(view.effects.begin(), view.effects.end(), EffectShown)); },
        view.effects.size(), "effects");
    Im::Spacing();

    if (view.effects.empty())
    {
        Im::SetCursorPosX(Im::GetCursorPosX() + kCellPadX);
        Im::TextDisabled("No active effects.");
        return;
    }

    const float gutter = kCellPadX * 2.0f;
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    float magnitudeWidth = TextWidth("Magnitude") + arrow;
    float remainingWidth = TextWidth("Remaining") + arrow;
    for (const auto &row : view.effects)
    {
        char num[32];
        std::snprintf(num, sizeof(num), "%.0f", row.magnitude);
        magnitudeWidth = (std::max)(magnitudeWidth, TextWidth(num));
        remainingWidth = (std::max)(remainingWidth, TextWidth(row.remainingText));
    }

    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_Sortable;
    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("effects", 4, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn("Effect", Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Name));
    Im::TableSetupColumn("Magnitude",
                         Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                         magnitudeWidth + gutter, static_cast<Im::ImGuiID>(Column::Magnitude));
    Im::TableSetupColumn("Remaining", Im::ImGuiTableColumnFlags_WidthFixed, remainingWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Remaining));
    Im::TableSetupColumn("Source", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Source));
    Im::TableHeadersRow();

    for (const EffectRow *row : VisibleEffects(view))
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "##effect%08X_%08X_%08X", row->form, row->sourceForm, row->linkForm);

        Im::TableNextRow(0, 0.0f);
        // Running but changing nothing for this follower, or running but
        // not acting, its conditions unmet: the row is drawn in the
        // disabled colour, and its name hovers as which. One the game's
        // own list hides is running and applied all the same, and reads as
        // any other; its page says it is hidden.
        const DimText grey(!row->applied || !row->active);
        Im::TableSetColumnIndex(0);
        const Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
        {
            state.detailForm = row->form;
            state.detailSource = row->sourceForm;
            state.detailLink = row->linkForm;
        }
        if (!row->applied && Im::IsItemHovered(0))
            Im::SetTooltip("Not applied");
        else if (!row->active && Im::IsItemHovered(0))
            Im::SetTooltip("Inactive");
        Im::SetCursorScreenPos(pos);
        Im::Text("%s", row->name.c_str());

        Im::TableSetColumnIndex(1);
        if (row->magnitude != 0.0f)
        {
            char num[32];
            std::snprintf(num, sizeof(num), "%.0f", row->magnitude);
            TextRightInCell(num);
        }
        Im::TableSetColumnIndex(2);
        // Right-aligned, as a number: the minutes line up down the column.
        if (!row->remainingText.empty())
            TextRightInCell(row->remainingText);
        Im::TableSetColumnIndex(3);
        // The source, a link to its page where it has one, as on the
        // effect's own page.
        if (SourcePage(view, row->linkForm) != Tab::None)
        {
            const Im::ImVec2 at = Im::GetCursorScreenPos();
            if (CellClicked((std::string(buf) + "source").c_str()))
                OpenSourcePage(view, row->linkForm);
            Im::SetCursorScreenPos(at);
        }
        Im::Text("%s", row->source.c_str());
    }
    Im::EndTable();
    Im::PopStyleVar(1);
}

void DrawMagic(const CharacterView &view)
{
    MagicTabState &state = g_magicTabs[view.id];

    if (state.detail != 0)
    {
        for (const auto &entry : view.magic)
        {
            if (entry.form == state.detail)
            {
                DrawMagicDetail(entry, state);
                // Back to wherever this was opened from: the list, or the
                // sheet, whose tab is selected again.
                if (state.detail == 0 && state.openedFrom != Tab::Magic)
                {
                    g_inventoryTabs[view.id].select = state.openedFrom;
                    state.openedFrom = Tab::Magic;
                }
                return;
            }
        }
        state.detail = 0;
    }

    if (view.magic.empty())
    {
        Im::Spacing();
        Im::TextDisabled("Knows no spells.");
        return;
    }
    DrawMagicList(view, state);
}

// One summon or raised corpse, laid out as the Character tab is: the three
// bars on the left, level, kind and time left on the right, then its sheet.
// The sheet's links go nowhere: a summon's sword is not in their inventory.
void DrawSummon(const SummonView &summon)
{
    Im::Spacing();

    const std::string levelText = std::to_string(static_cast<unsigned>(summon.level));
    const std::string kindText = summon.raised ? "raised" : "summoned";
    char remainingBuf[32];
    if (summon.remaining > 0.0f)
        std::snprintf(remainingBuf, sizeof(remainingBuf), "%.0f s", summon.remaining);
    else
        std::snprintf(remainingBuf, sizeof(remainingBuf), "-");
    const std::string remainingText = remainingBuf;

    const float originX = Im::GetCursorPosX();
    const auto *style = Im::GetStyle();
    const float inset = style->ItemSpacing.x;

    RowGeometry geo;
    geo.barLabelRight = originX + inset + WidestLabel({"Health", "Stamina", "Magicka"});
    geo.barLeft = geo.barLabelRight + 12.0f;
    const float contentRight = originX + Im::GetContentRegionAvail().x - inset;
    const float valueWidth = (std::max)({TextWidth(levelText), TextWidth(kindText), TextWidth(remainingText)});
    geo.valueLeft = contentRight - valueWidth;
    geo.statLabelRight = geo.valueLeft - 12.0f;

    DrawStatRow(
        geo, "Health", summon.health, Im::ImVec4(0.75f, 0.25f, 0.25f, 1.0f), "Level",
        [&] { Im::Text("%s", levelText.c_str()); }, summon.healthBreakdown);
    DrawStatRow(
        geo, "Stamina", summon.stamina, Im::ImVec4(0.30f, 0.65f, 0.35f, 1.0f), "Kind",
        [&] { Im::TextDisabled("%s", kindText.c_str()); }, summon.staminaBreakdown);
    DrawStatRow(
        geo, "Magicka", summon.magicka, Im::ImVec4(0.25f, 0.40f, 0.80f, 1.0f), "Remaining",
        [&] {
            Im::Text("%s", remainingText.c_str());
            // Where the time comes from, the summoner's perks on the spell
            // among it, on the figure as Carrying's is.
            if (!summon.remainingBreakdown.empty() && Im::IsItemHovered(0))
                BreakdownTooltip(summon.remainingBreakdown);
        },
        summon.magickaBreakdown);

    Im::Spacing();
    // The sheet's General table has the reference, its base and the name,
    // as a follower's does.
    DrawSections(summon.sheet, false);
}

// The Summons tab: what they command right now. A chip per summon above the
// page, as the Inventory tab has categories -- always, one summon included,
// since the chip is where its name is.
std::unordered_map<ft::ActorId, int> g_summonTabs;

void DrawSummons(const CharacterView &view)
{
    if (view.summons.empty())
    {
        Im::Spacing();
        Im::TextDisabled("Nothing summoned or raised.");
        return;
    }
    // The summon chosen, by its reference: two of one creature share a
    // name, and a place in the list moves when one ahead of it expires.
    int &chosen = g_summonTabs[view.id];
    const auto isChosen = [&chosen](const SummonView &summon) { return static_cast<int>(summon.id) == chosen; };
    if (std::none_of(view.summons.begin(), view.summons.end(), isChosen))
        chosen = static_cast<int>(view.summons.front().id);
    {
        Im::Spacing();
        std::vector<Chip> chips;
        chips.reserve(view.summons.size());
        for (const SummonView &summon : view.summons)
            chips.push_back({summon.name, summon.raised ? kIconRaised : kIconSummoned, static_cast<int>(summon.id)});
        DrawChips(chips, chosen);
    }
    DrawSummon(*std::find_if(view.summons.begin(), view.summons.end(), isChosen));
}

// The character sheet: what they are, as opposed to what they have been told to
// do. Everything here is display only and already on the view, so it costs
// the game thread nothing extra to show.
void DrawCharacter(const CharacterView &view)
{
    // Breathing room at the top -- the first bar sat flush against the panel
    // border.
    Im::Spacing();

    // Three rows, each carrying a bar on the left and a stat on the right, laid
    // out from measured widths so every column is flush and nothing depends on
    // the length of an English word.
    const std::string levelText = std::to_string(static_cast<unsigned>(view.level));
    const std::string statusText = view.inCombat ? "in combat" : "idle";
    char carriedBuf[64];
    std::snprintf(carriedBuf, sizeof(carriedBuf), "%.0f / %.0f", view.carriedWeight, view.carryCapacity);
    const std::string carriedText = carriedBuf;

    // Everything below is an ABSOLUTE x within the window, so it has to start
    // from where the content actually begins. Using the bare label width as the
    // right edge put the longest label at x = 0 -- hard against the panel
    // border -- which is what "hitting the edge" was.
    const float originX = Im::GetCursorPosX();
    const auto *style = Im::GetStyle();
    const float inset = style->ItemSpacing.x;

    RowGeometry geo;
    geo.barLabelRight = originX + inset + WidestLabel({"Health", "Stamina", "Magicka"});
    geo.barLeft = geo.barLabelRight + 12.0f;

    // The stat column is pinned to the RIGHT edge of the panel rather than left
    // against the bars, so it lines up with the rule table on the other tab.
    // Mirror the inset on the right so the stat values sit inboard of the border
    // by the same amount the labels do on the left.
    const float contentRight = originX + Im::GetContentRegionAvail().x - inset;
    const float valueWidth = (std::max)({TextWidth(levelText), TextWidth(statusText), TextWidth(carriedText)});
    geo.valueLeft = contentRight - valueWidth;
    geo.statLabelRight = geo.valueLeft - 12.0f;

    DrawStatRow(
        geo, "Health", view.health, Im::ImVec4(0.75f, 0.25f, 0.25f, 1.0f), "Level",
        [&] { Im::Text("%s", levelText.c_str()); }, view.healthBreakdown);

    DrawStatRow(
        geo, "Stamina", view.stamina, Im::ImVec4(0.30f, 0.65f, 0.35f, 1.0f), "Status",
        [&] {
            if (view.inCombat)
                Im::TextColored(Im::ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", statusText.c_str());
            else
                Im::TextDisabled("%s", statusText.c_str());
        },
        view.staminaBreakdown);

    DrawStatRow(
        geo, "Magicka", view.magicka, Im::ImVec4(0.25f, 0.40f, 0.80f, 1.0f), "Carrying",
        [&] {
            // Over capacity is worth seeing: an overencumbered follower
            // cannot fight properly, and otherwise you would only notice
            // by wondering why they are standing still.
            if (view.carryCapacity > 0.0f && view.carriedWeight > view.carryCapacity)
                Im::TextColored(kAlarm, "%s", carriedText.c_str());
            else
                Im::Text("%s", carriedText.c_str());
            // Where the capacity comes from, on the figure.
            if (!view.carryBreakdown.empty() && Im::IsItemHovered(0))
                BreakdownTooltip(view.carryBreakdown);
        },
        view.magickaBreakdown);

    Im::Spacing();

    // A weapon, shield, ammo or torch named on the sheet is a link to its
    // page on the Inventory tab.
    const ft::ActorId id = view.id;
    DrawSections(view.sheet, false, [id, &view](std::uint32_t form) {
        // A spell in hand has its page on the Magic tab; anything else on
        // the Inventory tab.
        const bool spell = std::any_of(view.magic.begin(), view.magic.end(),
                                       [form](const MagicEntry &entry) { return entry.form == form; });
        auto &state = g_inventoryTabs[id];
        if (spell)
        {
            g_magicTabs[id].detail = form;
            g_magicTabs[id].openedFrom = Tab::Character;
            state.select = Tab::Magic;
            return;
        }
        // Of the form's rows, the worn one: what the sheet names is the
        // copy in hand, and a row of the form that is not worn is a spare.
        state.detail = ItemPageOf(view, form);
        state.openedFrom = Tab::Character;
        state.select = Tab::Inventory;
    });
}

// The rule list and its switch.
void DrawTactics(const ft::RuleSet &rules, const FollowerView &view)
{
    Im::Spacing();

    // This follower's switch. The UI runs on the render thread and the tick
    // on the game thread, so the setter takes a lock rather than writing
    // shared state directly.
    //
    // Read LIVE, not from the view. The view is rebuilt by the tick, and the
    // tick is held while this panel has the clock frozen -- so a copy taken
    // from it showed the old state until the panel closed and a tick ran.
    // The global switch on the Settings page never had this problem because
    // it reads its flag directly; this now does the same.
    const bool followerEnabled = IsFollowerEnabled(view.id);
    // With the Settings switch off nothing here runs whatever this switch
    // says, so the switch and its word are greyed like the rules beneath,
    // and the hover says where to look. Not toggled while greyed: the
    // setting to change is on the other page.
    const bool all = IsEnabled();
    BeginDimmed(!all);
    // The same tick as the rule rows, and like theirs absent when off --
    // not a ghost of one -- with the word beside it: one glyph for "on"
    // everywhere on this tab, not ImGui's boxed tick next to ours.
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool toggled = GlyphButton("enabled", Im::GetFrameHeight(), Glyph::Tick, followerEnabled);
    Im::PopStyleVar(1);
    // On the switch, not the word, as on the Settings page. A disabled item
    // reports no hover unless asked, and the greyed switch is exactly when
    // the hover has something to say.
    if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Im::SetTooltip("%s", !all              ? "Tactics are turned off for all followers in Settings"
                             : followerEnabled ? "Click to turn off tactics"
                                               : "Click to turn on tactics");
    if (toggled && all)
        SetFollowerEnabled(view.id, !followerEnabled);
    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    Im::Text("Enabled");
    EndDimmed();

    // Space, but no rule: the table's own border already reads as the boundary,
    // and a separator immediately above it draws a second line doing the same
    // job. The gap is what was missing, not the line.
    Im::Spacing();
    Im::Spacing();

    // Greyed out, NOT rewritten. Turning a follower off is a presentation change
    // over the rules, not an edit to them: each rule keeps its own `enabled`
    // exactly as the player left it, so switching back restores the list rather
    // than handing back a set of boxes they have to re-tick. The Settings
    // switch greys the same way, for the same reason.
    BeginDimmed(!followerEnabled || !all);
    // Edit a copy, then hand the whole set back. Nothing partial is ever
    // visible to the tick.
    ft::RuleSet editable = rules;
    if (DrawRuleTable(editable, view))
        SetRules(view.id, std::move(editable));
    EndDimmed();
}

// The Skills tab: the skills, or one perk's page. Keyed by follower, as the
// other tabs' states are. Render thread only.
struct SkillsTabState
{
    std::uint32_t detail{0}; // the perk open in detail; 0 for the skills
};
std::unordered_map<ft::ActorId, SkillsTabState> g_skillsTabs;
// The Other Perks table's filter, one for every page as the list tabs' are.
char g_perksFilter[64]{};

// Does a perk's row hold the filter's text: its name, rank or description?
bool PerkShown(const SheetRow &row)
{
    return AnyContains({row.label, row.value, row.modifiers}, g_perksFilter);
}

void DrawSkills(const CharacterView &view)
{
    SkillsTabState &state = g_skillsTabs[view.id];
    if (state.detail != 0)
    {
        const PerkPage *page = nullptr;
        for (const auto &p : view.perks)
            if (p.form == state.detail)
                page = &p;
        if (!page)
        {
            state.detail = 0;
        }
        else
        {
            Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
            if (GlyphButton("back", Im::GetFrameHeight(), Glyph::Back))
                state.detail = 0;
            Im::PopStyleVar(1);
            Im::SameLine(0.0f, kCellPadX);
            Im::AlignTextToFramePadding();
            Im::Text("%s", page->name.c_str());
            Im::Spacing();
            // The perk's facts; then its effects, each with the conditions
            // that gate it beneath, in the table the skills open into.
            const auto split = page->sections.begin() + (page->sections.empty() ? 0 : 1);
            const std::vector<SheetSection> info(page->sections.begin(), split);
            const std::vector<SheetSection> effects(split, page->sections.end());
            DrawSections(info, false);
            // No third column: an entry whose conditions fail is greyed,
            // as an effect's row is, not marked.
            DrawSections(
                effects, true, {}, nullptr,
                [](const SheetRow &row, const std::string &key, float left, float right) {
                    DrawConditionDrawer(row, key, left, right);
                },
                "Name", "Value");
            if (!page->description.empty())
            {
                CentredHeading("Description");
                Im::TextWrapped("%s", page->description.c_str());
                Im::Spacing();
            }
            return;
        }
    }
    // The skills, with their trees; then the perks in no tree, in the same
    // table the trees open into, under a heading of their own.
    const auto open = [&state](std::uint32_t form) { state.detail = form; };
    std::vector<SheetSection> skills;
    const SheetSection *other = nullptr;
    for (const auto &section : view.skills)
    {
        if (section.title == "Other Perks")
            other = &section;
        else
            skills.push_back(section);
    }
    DrawSections(skills, true, open);
    if (other)
    {
        CentredHeading("Other Perks");
        // The perks outside the trees run long in a large load order.
        FilterRow(
            "##perksfilter", g_perksFilter, sizeof(g_perksFilter),
            [&] { return static_cast<std::size_t>(std::count_if(other->rows.begin(), other->rows.end(), PerkShown)); },
            other->rows.size(), "perks");
        Im::Spacing();
        std::vector<SheetRow> rows;
        for (const SheetRow &row : other->rows)
            if (PerkShown(row))
                rows.push_back(row);
        Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, 4.0f));
        DrawPerkTable("perks##other", rows, 0.0f, open);
        Im::PopStyleVar(1);
        Im::Spacing();
        Im::Spacing();
    }
}

// A tab's content in a scrolling region of its own under the tab bar, so the
// tabs stay in place when the page scrolls. By actor: each page keeps its own
// place in its list. EndChild whether or not the region is visible, as ImGui
// asks.
//
// A borderless child gets no padding, so a table as wide as the region put
// its right border on the clip edge and lost it, scrollbar or not. A couple
// of pixels either side keeps the border inside; none top or bottom, where
// nothing is clipped. Popped before drawing, so the tab's own popups and
// tooltips keep the style's padding.
void TabBody(const char *name, ft::ActorId actor, const std::function<void()> &draw)
{
    const std::string id = std::string("##tab/") + name + "/" + std::to_string(actor);
    Im::PushStyleVar(Im::ImGuiStyleVar_WindowPadding, Im::ImVec2(2.0f, 0.0f));
    const bool open = Im::BeginChild(id.c_str(), Im::ImVec2(0.0f, 0.0f), Im::ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    Im::PopStyleVar(1);
    if (open)
        draw();
    Im::EndChild();
}

// The top tab on the page drawn last, and whose page that was. Followers
// share one tab bar and the player has another, and ImGui keeps each bar's
// choice apart, so the tab is carried from page to page here: the Skills of
// one follower, then of the player, then of the next follower. Render
// thread only.
Tab g_shownTab = Tab::None;
ft::ActorId g_shownPage = 0;

// The tab a page opens on, on the frame it is drawn after another page's;
// None on the frames after, when its bar keeps the choice. The player's
// page has no Combat Style or Tactics, and opens on Character from either.
Tab CarriedTab(const CharacterView &view)
{
    if (view.id == g_shownPage)
        return Tab::None;
    g_shownPage = view.id;
    if (view.player && (g_shownTab == Tab::CombatStyle || g_shownTab == Tab::Tactics))
        return Tab::Character;
    return g_shownTab;
}

// A top tab: selected when `select` names it, and noted as the tab shown
// while it is open.
bool BeginSheetTab(const char *label, Tab tab, Tab select)
{
    if (!Im::BeginTabItem(label, nullptr, select == tab ? Im::ImGuiTabItemFlags_SetSelected : 0))
        return false;
    g_shownTab = tab;
    return true;
}

// The sheet's tabs, inside the caller's tab bar, reading left to right as
// who they are, what they carry, what they can cast, what they command,
// what is running on them and what they can do: a follower's page and the
// player's alike. `carried` is CarriedTab's answer for this page.
void DrawSheetTabs(const CharacterView &view, Tab carried)
{
    // A pending switch, from a link on the sheet or the back arrow on an
    // item page, consumed here so it acts for one frame only; else the tab
    // carried from the last page.
    auto &inventoryState = g_inventoryTabs[view.id];
    const Tab select = inventoryState.select != Tab::None ? inventoryState.select : carried;
    inventoryState.select = Tab::None;

    if (BeginSheetTab("Character", Tab::Character, select))
    {
        TabBody("character", view.id, [&] { DrawCharacter(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab("Inventory", Tab::Inventory, select))
    {
        TabBody("inventory", view.id, [&] { DrawInventory(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab("Magic", Tab::Magic, select))
    {
        TabBody("magic", view.id, [&] { DrawMagic(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab("Summons", Tab::Summons, select))
    {
        TabBody("summons", view.id, [&] { DrawSummons(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab("Effects", Tab::Effects, select))
    {
        TabBody("effects", view.id, [&] { DrawEffects(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab("Skills", Tab::Skills, select))
    {
        TabBody("skills", view.id, [&] {
            Im::Spacing();
            DrawSkills(view);
        });
        Im::EndTabItem();
    }
}

// One page per follower: the sheet's tabs, then how their combat AI is
// tuned and what they have been told to do. The first page drawn opens on
// Tactics, which is what the mod is for; every page after opens on the tab
// the last one showed (CarriedTab).
void DrawFollower(const ft::RuleSet &rules, const FollowerView &view)
{
    if (!Im::BeginTabBar("follower##tabs"))
        return;

    const bool firstPage = g_shownTab == Tab::None;
    Tab carried = CarriedTab(view);
    if (firstPage)
        carried = Tab::Tactics;

    DrawSheetTabs(view, carried);
    // What the combat AI is tuned by, before what it is told: a rule works
    // with, or against, these numbers.
    if (BeginSheetTab("Combat Style", Tab::CombatStyle, carried))
    {
        TabBody("combatstyle", view.id, [&] {
            Im::Spacing();
            DrawSections(view.combatStyle, false);
        });
        Im::EndTabItem();
    }
    if (BeginSheetTab("Tactics", Tab::Tactics, carried))
    {
        TabBody("tactics", view.id, [&] { DrawTactics(rules, view); });
        Im::EndTabItem();
    }

    Im::EndTabBar();
}

// --- menu entries -----------------------------------------------------------
//
// One entry per follower under "Follower Tactics", rather than everyone stacked
// inside a single page. Two SDK constraints shape how:
//
//  1. RenderFunction is `void(__stdcall*)()` with no user data, so an entry
//     cannot be told which follower it is for. Each needs its own function --
//     hence a fixed pool of slots, one static trampoline apiece. A slot is
//     held for the life of the process (2. below), so the pool counts every
//     follower seen in one launch, not the party of the moment.
//  2. Removing an entry needs DeleteSection, which the framework's source has
//     but no released build yet exports (3.14.1 is the newest; ours is
//     3.14.0). The SDK wrapper returns false when the export is missing, so
//     a dismissed follower's entry is deleted where it can be and otherwise
//     stays, saying so, until the framework catches up.
//
// Entries are registered the first time a follower is seen, so they carry real
// names. Those seen together are registered in name order; a later one comes
// after them, since an entry cannot be moved once added.

constexpr std::size_t kSlots = 64;

struct Slot
{
    ft::ActorId id{0}; // 0: free
    std::string name;  // as registered, for the entry's path
};

std::mutex g_slotMutex;
std::array<Slot, kSlots> g_slots{};

[[nodiscard]] ft::ActorId SlotOwner(std::size_t slot)
{
    std::scoped_lock lock(g_slotMutex);
    return slot < kSlots ? g_slots[slot].id : 0;
}

void DrawSlot(std::size_t slot)
{
    const ft::ActorId id = SlotOwner(slot);
    if (id == 0)
    {
        Im::TextDisabled("Nobody is assigned to this entry.");
        return;
    }

    if (const auto view = ObserveFollower(id))
    {
        DrawFollower(GetRules(view->id), *view);
        return;
    }

    Im::TextDisabled("Dismissed. This entry cannot be removed until the menu framework's next "
                     "release; it is reused if they come back.");
}

void DrawSettings()
{
    // The title, larger than the page's text. SetWindowFontScale sets the
    // window's scale outright rather than multiplying it, so the scale in
    // force is measured first -- the font size at it against the size at 1 --
    // and put back after, in case the framework sets one of its own.
    const float fontSize = Im::GetFontSize();
    Im::SetWindowFontScale(1.0f);
    const float baseSize = Im::GetFontSize();
    const float windowScale = baseSize > 0.0f ? fontSize / baseSize : 1.0f;
    Im::SetWindowFontScale(windowScale * 1.5f);
    Im::Text("Follower Tactics (%s)", FT_VERSION);
    Im::SetWindowFontScale(windowScale);

    Im::Separator();
    Im::Spacing();

    // The same switch as each follower's on their Tactics tab, and read
    // live the same way.
    const bool enabled = IsEnabled();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool toggled = GlyphButton("enabledAll", Im::GetFrameHeight(), Glyph::Tick, enabled);
    Im::PopStyleVar(1);
    // On the switch, not the word: the hover says what a click does, and
    // the switch is what is clicked. Read before the toggle, so the text
    // matches the tick shown this frame.
    if (Im::IsItemHovered(0))
        Im::SetTooltip(enabled ? "Click to turn off tactics for all followers"
                               : "Click to turn on tactics for all followers");
    if (toggled)
        SetEnabled(!enabled);
    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    Im::Text("Enable for all");
}

void __stdcall RenderSettings()
{
    DrawSettings();
}

// The player's page: the sheet's tabs alone, in a tab bar of its own, on
// the tab carried from the last page drawn.
void __stdcall RenderPlayer()
{
    // Nothing until the open's task has read the player: a frame.
    const auto view = ObservePlayer();
    if (!view || !Im::BeginTabBar("player##tabs"))
        return;
    DrawSheetTabs(*view, CarriedTab(*view));
    Im::EndTabBar();
}

// The framework's own open event. The views behind every page are the
// tick's, and the tick stops with the clock the moment the panel opens,
// so what a page shows is otherwise whatever the last tick saw -- up to
// half a second old, or older after a paused menu held the tick. One
// fresh publish of every follower and of the player on the game thread,
// at the open, so the charge a fight just drew down reads right away.
void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType type)
{
    if (type != SKSEMenuFramework::Model::EventType::kOpenMenu)
        return;
    if (auto *task = SKSE::GetTaskInterface())
        task->AddTask([]() {
            PublishAllFollowers();
            PublishPlayer();
        });
}

// One trampoline per slot: a render callback takes no argument, so the
// slot's index is the template's, and the table of them is made from the
// count.
template <std::size_t N> void __stdcall RenderSlot()
{
    DrawSlot(N);
}

template <std::size_t... N>
constexpr std::array<SKSEMenuFramework::Model::RenderFunction, sizeof...(N)> Renderers(std::index_sequence<N...>)
{
    return {RenderSlot<N>...};
}

} // namespace

void SyncFollowers()
{
    if (!SKSEMenuFramework::IsInstalled())
        return;

    static const auto renderers = Renderers(std::make_index_sequence<kSlots>{});

    const auto followers = ObserveFollowers();
    const auto present = [&](ft::ActorId id) {
        for (const auto &view : followers)
        {
            if (view.id == id)
                return true;
        }
        return false;
    };

    // Dismissed: delete the entry where the framework allows it, and free
    // the slot. Where it does not, the slot stays theirs, so the entry still
    // reads as their page if they are recruited again.
    {
        std::scoped_lock lock(g_slotMutex);
        for (auto &slot : g_slots)
        {
            if (slot.id == 0 || present(slot.id))
                continue;
            if (SKSEMenuFramework::DeleteSection("Follower Tactics/Followers/" + slot.name))
            {
                log::ui.debug("menu entry removed for {}", slot.name);
                slot = {};
            }
        }
    }

    // New: the first free slot, in name order, so the followers who appear
    // together list alphabetically. The frameworks in the field export
    // only AddSectionItem and AddWindow (dumpbin, 2026-09-11): an entry,
    // once added, can be neither removed nor moved, so one recruited later
    // goes after them. A load reveals the party over a few ticks -- Serana
    // a tick after the other three, and at the end of the list -- so the
    // newcomers are held until nobody new has appeared for a moment, and
    // added as one batch.
    std::vector<const FollowerView *> arriving;
    {
        std::scoped_lock lock(g_slotMutex);
        for (const auto &view : followers)
        {
            bool known = false;
            for (const auto &slot : g_slots)
                known = known || slot.id == view.id;
            if (!known)
                arriving.push_back(&view);
        }
    }
    static std::unordered_set<ft::ActorId> pending;
    static std::chrono::steady_clock::time_point lastArrival;
    std::unordered_set<ft::ActorId> now;
    for (const auto *view : arriving)
        now.insert(view->id);
    if (now != pending)
    {
        pending = std::move(now);
        lastArrival = std::chrono::steady_clock::now();
    }
    if (pending.empty() || std::chrono::steady_clock::now() - lastArrival < std::chrono::seconds(2))
        return;
    pending.clear();

    std::sort(arriving.begin(), arriving.end(),
              [](const FollowerView *a, const FollowerView *b) { return a->name < b->name; });
    for (const auto *viewPtr : arriving)
    {
        const auto &view = *viewPtr;
        std::size_t index = kSlots;
        {
            std::scoped_lock lock(g_slotMutex);
            for (std::size_t i = 0; i < kSlots; ++i)
            {
                if (g_slots[i].id == 0)
                {
                    index = i;
                    g_slots[i] = {view.id, view.name};
                    break;
                }
            }
        }
        if (index == kSlots)
        {
            // Said once, since the panel would otherwise just lack a
            // name.
            static std::unordered_set<ft::ActorId> said;
            if (said.insert(view.id).second)
                log::ui.warn("no menu entry for {}: all {} are taken", view.name, kSlots);
            continue;
        }

        // Under a Followers subsection, apart from Settings: the path's
        // components are the tree.
        SKSEMenuFramework::FullPathAddSectionItem("Follower Tactics/Followers/" + view.name, renderers[index]);
        log::ui.debug("menu entry added for {} (slot {})", view.name, index);
    }
}

void Install()
{
    // Soft dependency, and the reason this whole file is safe to ship: without
    // the framework installed there is simply no menu, and the mod carries on
    // drinking potions.
    if (!SKSEMenuFramework::IsInstalled())
    {
        log::ui.info("SKSE Menu Framework not installed -- no in-game panel. "
                     "Tactics still run; see this log for what they decide.");
        return;
    }

    SKSEMenuFramework::SetSection("Follower Tactics");
    SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
    // Before the Followers subsection the tick fills as followers are
    // recruited: an entry cannot be moved once added.
    SKSEMenuFramework::AddSectionItem("Player", RenderPlayer);
    // Kept for the life of the process; the framework unregisters on
    // destruction, which never comes.
    static auto *const openEvent = SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
    (void)openEvent;

    log::ui.info("registered with SKSE Menu Framework (F1). Follower entries appear as followers do.");
}

} // namespace ft::game::ui
