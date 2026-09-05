// The in-game panel, drawn with ImGui via SKSE Menu Framework.
//
// Read-only for now, and deliberately so. docs/PLAN.md 3.7 says to build the
// debug column FIRST, not last, and the reason is worth restating: authoring
// rules against an opaque engine is guesswork without it. The engine already
// records, per rule, exactly why that rule did not fire -- this puts it on
// screen instead of in a log file. Editing arrives with profile persistence;
// there is little point letting someone change a rule that cannot be saved.
//
// Everything here runs on the render thread. It never touches an RE::Actor and
// never reaches into live engine state -- ObserveFollowers() hands back a copy.
// Reading a follower's inventory from the render thread would be a good way to
// crash the game.

#include "game/UI.h"

#include "core/Vocabulary.h"
#include "game/Pins.h"
#include "game/Tactics.h"

#include "SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft::game::ui
{
namespace
{

namespace Im = ImGuiMCP;

// --- status ----------------------------------------------------------------

bool TakesSpell(ft::ActionKind action);

// One short word for the Status column, and the colour to say it in.
//
// Deliberately terse. This column sits beside two editable cells in a narrow
// table, and a sentence here pushed the Then cell off the row -- "true (action
// used too recently)" is three times the width of "cooldown" and says the same
// thing. The long wording is still one hover away.
//
// The four cooldown verdicts collapse to one word on purpose: which timer is
// holding a rule back is a debugging detail, and the tooltip keeps it.
//
// Note what is NOT shown. Disabled, Unsupported, InvalidCondition and
// NotReached are all decided BEFORE the condition is ever evaluated -- look at
// the order of checks in Evaluate() -- so for those we genuinely do not know
// whether the condition holds, and printing "false" would be inventing an
// answer. They get their own word instead.
struct Status
{
    const char *text;
    Im::ImVec4 color;
};

Status StatusFor(ft::Verdict v, ft::ActionKind action)
{
    constexpr Im::ImVec4 acted{0.55f, 0.90f, 0.55f, 1.0f};  // it happened
    constexpr Im::ImVec4 quiet{0.55f, 0.55f, 0.58f, 1.0f};  // nothing to say
    constexpr Im::ImVec4 held{0.85f, 0.75f, 0.40f, 1.0f};   // true, but blocked
    constexpr Im::ImVec4 broken{0.95f, 0.45f, 0.40f, 1.0f}; // needs fixing

    switch (v)
    {
    case ft::Verdict::Fired:
        return {"fired", acted};
    case ft::Verdict::ConditionFalse:
        return {"false", quiet};

    case ft::Verdict::ActionCooldown:
        return {"cooldown", held};

    // The same verdict means different things to different actions, and the
    // word has to match or it sends someone looking in the wrong place: a
    // spell rule reporting "count: 0" is worse than reporting nothing. A
    // consumable the follower is out of reads as its count, the way the
    // Consume menu shows one.
    case ft::Verdict::NoResource:
        return {action == ft::ActionKind::UsePower ? "no power"
                : action == ft::ActionKind::Shout  ? "no shout"
                : TakesSpell(action)               ? "no spell"
                : ft::IsEquip(action)              ? "not carried"
                                                   : "count: 0",
                held};
    case ft::Verdict::EffectActive:
        return {ft::IsEquip(action) ? "pinned" : "active", held};
    case ft::Verdict::AboveSkill:
        return {"too high", held};
    case ft::Verdict::Outranked:
        return {"outranked", held};
    case ft::Verdict::NoTarget:
        return {"no target", held};
    case ft::Verdict::CannotAfford:
        return {"no magicka", held};
    case ft::Verdict::Busy:
        return {"busy", held};
    case ft::Verdict::Casting:
        return {"casting", held};
    case ft::Verdict::Recovering:
        return {"cooldown", held}; // the shout's own, told apart from the action's in the tooltip

    case ft::Verdict::Disabled:
        return {"off", quiet};
    case ft::Verdict::NotReached:
        return {"", quiet}; // nothing to say: an empty cell, not a placeholder

    case ft::Verdict::InvalidCondition:
        return {"invalid", broken};
    case ft::Verdict::Unsupported:
        return {"n/a", broken};

    default:
        return {"", quiet};
    }
}

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
// Vertical padding of the inventory and magic tables' cells.
constexpr float kCellPadY = 4.0f;

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

void DrawStatRow(const RowGeometry &g, const char *barLabel, const ft::Stat &stat, Im::ImVec4 barColour,
                 const char *statLabel, const std::function<void()> &drawValue)
{
    Im::SetCursorPosX((std::max)(0.0f, g.barLabelRight - Im::CalcTextSize(barLabel).x));
    Im::AlignTextToFramePadding();
    Im::Text("%s", barLabel);

    Im::SameLine(g.barLeft, -1.0f);
    Im::PushStyleColor(Im::ImGuiCol_PlotHistogram, barColour);
    const std::string overlay =
        std::to_string(static_cast<int>(stat.current)) + " / " + std::to_string(static_cast<int>(stat.max));
    Im::ProgressBar(stat.Pct(), Im::ImVec2(g.barWidth, 0.0f), overlay.c_str());
    Im::PopStyleColor(1);

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
        return (ft::IsAbove(predicate) ? "> " : "< ") + std::to_string(static_cast<int>(value * 100.0f + 0.5f)) + "%";
    case ft::ArgumentKind::Count:
        return ">= " + std::to_string(static_cast<int>(value));
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
        return {0.25f, 0.50f, 0.75f};
    case ft::ArgumentKind::Count:
        return {2.0f, 3.0f, 4.0f, 5.0f};
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
    if (r.subject == ft::SubjectKind::Player && !view.playerName.empty())
        return view.playerName;
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
    // A status reads as the status: "Self Poisoned", not "Self Status".
    if (r.predicate == ft::PredicateKind::Status)
    {
        text += ft::DisplayName(r.statusKind);
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
        if (r.predicate == ft::PredicateKind::AttackedBy)
        {
            text += ' ';
            text += ft::DisplayName(r.damageKind);
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

void DrawCodepoint(Im::ImDrawList *draw, unsigned codepoint, Im::ImVec2 lo, Im::ImVec2 hi, Im::ImU32 ink,
                   float scale = 1.0f)
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
    const Im::ImVec2 at{(lo.x + hi.x - extent.x) * 0.5f, (lo.y + hi.y - extent.y) * 0.5f};
    Im::ImDrawListManager::AddText(draw, font, fontSize, at, ink, text.c_str());
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
void BulletedLines(const std::string &text);

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
    const float innerY = style ? style->ItemInnerSpacing.y : 4.0f;
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
    return Im::GetWindowPos().x + Im::GetWindowWidth() - (style ? style->WindowPadding.x : 8.0f);
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

bool CascadeItem(const char *label, bool selected)
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const float right = CascadeIconRight();
    const bool clicked = Im::MenuItem(label, nullptr, false, true);
    if (selected)
        CascadeIcon(draw, Glyph::Tick, pos, right);
    return clicked;
}

// The three about a fight, under one "Combat" heading: Start, During, End.
bool IsCombatPredicate(ft::PredicateKind p)
{
    return p == ft::PredicateKind::CombatBegins || p == ft::PredicateKind::CombatEnds;
}

// Is the rule about this subject -- and, for a named follower, this one?
bool SubjectIs(const ft::Rule &rule, ft::SubjectKind subject, std::uint32_t form)
{
    return rule.subject == subject && (subject != ft::SubjectKind::Follower || rule.subjectForm == form);
}

// The other followers, by name, for the two cascades' headings.
std::vector<FollowerView::Peer> SortedPeers(const FollowerView &view)
{
    std::vector<FollowerView::Peer> peers = view.peers;
    std::sort(peers.begin(), peers.end(),
              [](const FollowerView::Peer &a, const FollowerView::Peer &b) { return a.name < b.name; });
    return peers;
}

bool ConditionCascade(const char *id, ft::Rule &rule, const FollowerView &view)
{
    bool changed = false;

    CellButtonOpensPopup(id, ConditionText(rule, view));

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
    std::vector<Heading> headings{
        {ft::SubjectKind::Self, 0, std::string(ft::DisplayName(ft::SubjectKind::Self))},
        {ft::SubjectKind::Player, 0,
         view.playerName.empty() ? std::string(ft::DisplayName(ft::SubjectKind::Player)) : view.playerName}};
    for (const auto &peer : SortedPeers(view))
        headings.push_back({ft::SubjectKind::Follower, peer.id, peer.name});
    headings.push_back({ft::SubjectKind::Ally, 0, std::string(ft::DisplayName(ft::SubjectKind::Ally))});
    headings.push_back(
        {ft::SubjectKind::CurrentTarget, 0, std::string(ft::DisplayName(ft::SubjectKind::CurrentTarget))});
    headings.push_back({ft::SubjectKind::Enemy, 0, std::string(ft::DisplayName(ft::SubjectKind::Enemy))});
    headings.push_back({ft::SubjectKind::Corpse, 0, std::string(ft::DisplayName(ft::SubjectKind::Corpse))});

    for (const Heading &heading : headings)
    {
        const ft::SubjectKind subject = heading.subject;
        const std::uint32_t form = heading.form;
        if (!BeginCascade(heading.label.c_str()))
            continue;

        for (std::size_t pi = 0; pi < static_cast<std::size_t>(ft::PredicateKind::COUNT); ++pi)
        {
            const auto predicate = static_cast<ft::PredicateKind>(pi);
            if (!ft::IsPredicateValidFor(subject, predicate))
                continue;
            // An above predicate is listed under its below counterpart's
            // heading, after a divider, not as a heading of its own; the
            // group's extremes likewise, first under theirs.
            if (ft::IsAbove(predicate) || ft::IsExtreme(predicate))
                continue;

            // The fight's three, grouped where the first of them falls.
            if (IsCombatPredicate(predicate))
            {
                if (predicate != ft::PredicateKind::CombatBegins)
                    continue;
                if (!BeginCascade("Combat"))
                    continue;
                struct Phase
                {
                    ft::PredicateKind predicate;
                    const char *label;
                };
                constexpr Phase kPhases[] = {{ft::PredicateKind::CombatBegins, "Start"},
                                             {ft::PredicateKind::CombatEnds, "End"}};
                for (const Phase &phase : kPhases)
                {
                    if (!ft::IsPredicateValidFor(subject, phase.predicate))
                        continue;
                    const bool selected = SubjectIs(rule, subject, form) && rule.predicate == phase.predicate;
                    if (CascadeItem(phase.label, selected))
                    {
                        rule.subject = subject;
                        rule.subjectForm = form;
                        rule.predicate = phase.predicate;
                        changed = true;
                    }
                    if (Im::IsItemHovered(0))
                        Im::SetTooltip("%s", std::string(ft::Describe(phase.predicate)).c_str());
                }
                Im::EndMenu();
                continue;
            }

            // The summons, under one "Summon" heading: None, Active.
            if (predicate == ft::PredicateKind::SummonNone || predicate == ft::PredicateKind::SummonActive)
            {
                if (predicate != ft::PredicateKind::SummonNone)
                    continue;
                if (!BeginCascade("Summon"))
                    continue;
                for (const auto [which, label] : {std::pair{ft::PredicateKind::SummonNone, "None"},
                                                  std::pair{ft::PredicateKind::SummonActive, "Active"}})
                {
                    const bool selected = SubjectIs(rule, subject, form) && rule.predicate == which;
                    if (CascadeItem(label, selected))
                    {
                        rule.subject = subject;
                        rule.subjectForm = form;
                        rule.predicate = which;
                        changed = true;
                    }
                    if (Im::IsItemHovered(0))
                        Im::SetTooltip("%s", std::string(ft::Describe(which)).c_str());
                }
                Im::EndMenu();
                continue;
            }

            const auto presets = PresetsFor(predicate);
            const std::string predicateName(ft::DisplayName(predicate));

            // A resistance: the kinds of damage under "Resistance"; under
            // each, Lowest and Highest for a group, the percents below, then
            // above -- the same shape as Health.
            if (predicate == ft::PredicateKind::ResistancePctBelow)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::DamageKind::COUNT); ++ki)
                {
                    const auto kind = static_cast<ft::DamageKind>(ki);
                    if (kind == ft::DamageKind::Melee || kind == ft::DamageKind::Ranged || kind == ft::DamageKind::Any)
                        continue; // nothing resists a blow or an arrow but armour, its own heading
                    if (!BeginCascade(std::string(ft::DisplayName(kind)).c_str()))
                        continue;
                    const auto pick = [&](ft::PredicateKind which, float arg, const std::string &label) {
                        const bool selected = SubjectIs(rule, subject, form) && rule.predicate == which &&
                                              rule.damageKind == kind &&
                                              (ft::IsExtreme(which) || std::abs(rule.conditionArg - arg) < 0.001f);
                        if (CascadeItem(label.c_str(), selected))
                        {
                            rule.subject = subject;
                            rule.subjectForm = form;
                            rule.predicate = which;
                            rule.damageKind = kind;
                            rule.conditionArg = arg;
                            changed = true;
                        }
                        if (Im::IsItemHovered(0))
                            Im::SetTooltip("%s", std::string(ft::Describe(which)).c_str());
                    };
                    if (const auto extremes = ft::ExtremesOf(predicate);
                        ft::IsPredicateValidFor(subject, extremes.lowest))
                    {
                        pick(extremes.lowest, 0.0f, "Lowest");
                        pick(extremes.highest, 0.0f, "Highest");
                        Im::Separator();
                    }
                    for (const float preset : PresetsFor(predicate))
                        pick(predicate, preset, ArgumentText(predicate, preset));
                    Im::Separator();
                    const auto above = ft::AboveOf(predicate);
                    for (const float preset : PresetsFor(above))
                        pick(above, preset, ArgumentText(above, preset));
                    Im::EndMenu();
                }
                Im::EndMenu();
                continue;
            }

            // Attacked by: Any; then how -- a blow, an arrow, a spell of any
            // kind; then what the spell was. A divider between each group.
            if (predicate == ft::PredicateKind::AttackedBy)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                const auto pick = [&](ft::DamageKind kind) {
                    const bool selected =
                        SubjectIs(rule, subject, form) && rule.predicate == predicate && rule.damageKind == kind;
                    if (CascadeItem(std::string(ft::DisplayName(kind)).c_str(), selected))
                    {
                        rule.subject = subject;
                        rule.subjectForm = form;
                        rule.predicate = predicate;
                        rule.damageKind = kind;
                        changed = true;
                    }
                };
                pick(ft::DamageKind::Any);
                Im::Separator();
                pick(ft::DamageKind::Melee);
                pick(ft::DamageKind::Ranged);
                pick(ft::DamageKind::Magic);
                Im::Separator();
                pick(ft::DamageKind::Fire);
                pick(ft::DamageKind::Frost);
                pick(ft::DamageKind::Shock);
                pick(ft::DamageKind::Poison);
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
                    kinds.push_back(static_cast<ft::StatusKind>(ki));
                std::sort(kinds.begin(), kinds.end(),
                          [](ft::StatusKind a, ft::StatusKind b) { return ft::DisplayName(a) < ft::DisplayName(b); });
                for (const ft::StatusKind kind : kinds)
                {
                    const bool selected =
                        SubjectIs(rule, subject, form) && rule.predicate == predicate && rule.statusKind == kind;
                    if (CascadeItem(std::string(ft::DisplayName(kind)).c_str(), selected))
                    {
                        rule.subject = subject;
                        rule.subjectForm = form;
                        rule.predicate = predicate;
                        rule.statusKind = kind;
                        changed = true;
                    }
                }
                Im::EndMenu();
                continue;
            }

            if (presets.empty())
            {
                // No argument -- a leaf.
                const bool selected = SubjectIs(rule, subject, form) && rule.predicate == predicate;
                if (CascadeItem(predicateName.c_str(), selected))
                {
                    rule.subject = subject;
                    rule.subjectForm = form;
                    rule.predicate = predicate;
                    changed = true;
                }
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(predicate)).c_str());
                // Any stands apart from the conditions proper.
                if (predicate == ft::PredicateKind::Any)
                    Im::Separator();
                continue;
            }

            if (!BeginCascade(predicateName.c_str()))
                continue;

            // Lowest and Highest first, for a group: the one with the least
            // or the most of what the heading measures.
            if (const auto extremes = ft::ExtremesOf(predicate);
                extremes.lowest != predicate && ft::IsPredicateValidFor(subject, extremes.lowest))
            {
                for (const auto [which, label] :
                     {std::pair{extremes.lowest, "Lowest"}, std::pair{extremes.highest, "Highest"}})
                {
                    const bool selected = SubjectIs(rule, subject, form) && rule.predicate == which;
                    if (CascadeItem(label, selected))
                    {
                        rule.subject = subject;
                        rule.subjectForm = form;
                        rule.predicate = which;
                        changed = true;
                    }
                    if (Im::IsItemHovered(0))
                        Im::SetTooltip("%s", std::string(ft::Describe(which)).c_str());
                }
                Im::Separator();
            }

            const auto offer = [&](ft::PredicateKind which) {
                for (const float preset : PresetsFor(which))
                {
                    const bool selected = SubjectIs(rule, subject, form) && rule.predicate == which &&
                                          std::abs(rule.conditionArg - preset) < 0.001f;
                    if (CascadeItem(ArgumentText(which, preset).c_str(), selected))
                    {
                        rule.subject = subject;
                        rule.subjectForm = form;
                        rule.predicate = which;
                        rule.conditionArg = preset;
                        changed = true;
                    }
                }
            };
            offer(predicate);
            // The other side of the same number, below first.
            if (const auto above = ft::AboveOf(predicate); above != predicate)
            {
                Im::Separator();
                offer(above);
            }
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

// Is this action one named consumable: a potion, a food, an ingredient?
bool NamesConsumable(ft::ActionKind action)
{
    return action == ft::ActionKind::DrinkPotion || action == ft::ActionKind::EatFood ||
           action == ft::ActionKind::EatIngredient;
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

// "Equip weapon" -> "weapon", for "Unequip weapon" and the None tooltip.
std::string EquipNoun(ft::ActionKind action)
{
    std::string name(ft::DisplayName(action));
    constexpr std::string_view prefix = "Equip ";
    if (name.rfind(prefix, 0) == 0)
        name.erase(0, prefix.size());
    return name;
}

// The name of the thing an equip rule names, as she carries or knows it;
// empty if she does not.
std::string EquipTargetName(const ft::Action &act, const FollowerView &view)
{
    if (act.kind == ft::ActionKind::EquipSpell)
    {
        for (const auto &entry : view.magic)
            if (entry.form == act.form)
                return entry.name;
        return {};
    }
    for (const auto &item : view.inventory)
        if (item.form == act.form)
            return item.name;
    return {};
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A tooltip reads as a sentence: the core's explanations are lowercase so
// they can sit inside a log line, and get their capital here.
std::string Sentence(std::string_view text)
{
    std::string out(text);
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// The status cell's tooltip. The core's sentence, with the one number the
// core does not have: how long the shout's cooldown has to run.
std::string VerdictTooltip(ft::Verdict verdict, ft::ActionKind action, const FollowerView &view)
{
    if (verdict == ft::Verdict::Recovering && view.voiceRecovery > 0.0f)
        return "Shout on cooldown (" + std::to_string(static_cast<int>(view.voiceRecovery + 0.5f)) + " s)";
    return Sentence(ft::Explain(verdict, action));
}

// "Drink strongest health potion" -> "Strongest health potion", for use under
// a menu already headed "Potion".
std::string DrinkSubmenuLabel(ft::ActionKind action)
{
    std::string name(ft::DisplayName(action));
    constexpr std::string_view prefix = "Drink ";
    if (name.rfind(prefix, 0) == 0)
        name.erase(0, prefix.size());
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
        return view.playerName.empty() ? std::string(ft::DisplayName(rule.actionTarget)) : view.playerName;
    case ft::ActionTargetKind::Follower:
        for (const auto &peer : view.peers)
            if (peer.id == rule.actionTargetForm)
                return peer.name;
        return "Follower (away)";
    default:
        return std::string(ft::DisplayName(rule.actionTarget));
    }
}

std::string ActionText(const ft::Action &act, const FollowerView &view)
{
    const std::string base(ft::DisplayName(act.kind));

    if (NamesConsumable(act.kind))
    {
        if (act.form == 0)
            return base + "...";
        const char *verb = act.kind == ft::ActionKind::DrinkPotion ? "Drink " : "Eat ";
        for (const auto &option : view.consumables)
            if (option.form == act.form && option.kind == ft::ConsumableOf(act.kind))
                return verb + option.name;
        // Not carried: the Status column says "count: 0", so the cell need not.
        return base;
    }

    if (ft::IsEquip(act.kind))
    {
        if (act.form == 0)
            return "Unequip " + EquipNoun(act.kind);
        const std::string name = EquipTargetName(act, view);
        if (name.empty())
            return base + (act.kind == ft::ActionKind::EquipSpell ? " (not known)" : " (not carried)");
        const bool handed = act.kind == ft::ActionKind::EquipWeapon || act.kind == ft::ActionKind::EquipSpell;
        return "Equip " + name + (handed ? " (" + Lower(ft::DisplayName(act.hand)) + ")" : "");
    }

    if (!TakesSpell(act.kind))
        return base;

    if (act.form == 0)
        return base + "...";

    const auto kind = act.kind == ft::ActionKind::UsePower ? SpellOption::Kind::Power
                      : act.kind == ft::ActionKind::Shout  ? SpellOption::Kind::Shout
                                                           : SpellOption::Kind::Spell;
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
        default:
            return "Cast " + option.name;
        }
    }

    // Named a spell this follower does not know. Says so rather than showing a
    // plausible-looking action that can never fire -- the status column will
    // report "no spell", and the two need to agree.
    return base + " (not known)";
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
bool EquipLeaf(ft::Action &act, ft::ActionKind action, std::uint32_t form, const std::string &name, Hand hand)
{
    const bool selected = act.kind == action && act.form == form && act.hand == hand;
    if (!CascadeItem(name.c_str(), selected))
        return false;
    act.kind = action;
    act.form = form;
    act.hand = hand;
    return true;
}

// The Equip weapon / Equip spell / Equip arrows / Equip armor cascades.
//
// None first, in a section by itself: let go of every pin of this kind, and
// the AI chooses again. Then, for the two that take a hand, Left, Right and
// Both, each listing what fits that hand; for arrows and armour, the things
// themselves. Every list is hers, so a rule cannot name a thing she does
// not have.
bool EquipMenu(ft::Action &act, ft::ActionKind action, const FollowerView &view)
{
    bool changed = false;

    const bool none = act.kind == action && act.form == 0;
    if (CascadeItem("None", none))
    {
        act.kind = action;
        act.form = 0;
        act.hand = Hand::None;
        changed = true;
    }

    // What is not there is not listed: no greyed "(carries none)" lines,
    // an empty kind simply offers None and nothing beneath it.
    const bool spell = action == ft::ActionKind::EquipSpell;
    const bool handed = spell || action == ft::ActionKind::EquipWeapon;
    if (!handed)
    {
        const ItemCategory category =
            action == ft::ActionKind::EquipArrows ? ItemCategory::Arrows : ItemCategory::Armor;
        bool separated = false;
        for (const auto &item : view.inventory)
        {
            if (item.category != category)
                continue;
            if (!separated)
            {
                Im::Separator();
                separated = true;
            }
            if (EquipLeaf(act, action, item.form, item.name, Hand::None))
                changed = true;
        }
        return changed;
    }
    Im::Separator();

    for (const Hand hand : {Hand::Left, Hand::Right, Hand::Both})
    {
        const std::string label(ft::DisplayName(hand));
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
        if (!any || !BeginCascade(label.c_str()))
            continue;
        if (spell)
        {
            for (const auto &entry : view.magic)
            {
                if (!Offered(entry, hand))
                    continue;
                if (EquipLeaf(act, action, entry.form, entry.name, hand))
                    changed = true;
            }
        }
        else
        {
            for (const auto &item : view.inventory)
            {
                if (item.category != ItemCategory::Weapons || !Fits(item.grip, hand, false))
                    continue;
                if (EquipLeaf(act, action, item.form, item.name, hand))
                    changed = true;
            }
        }
        Im::EndMenu();
    }
    return changed;
}

// The action side of the cascade.
//
// Flat for everything that takes no argument; a submenu of the follower's own
// spells for the one that casts, and the equip cascades for the four that
// pin. Every list is hers, so a rule cannot name a thing she does not have --
// the same guarantee the condition side gets from the validity matrix, and
// for the same reason: an unfireable rule should be unauthorable, not merely
// discouraged.
// The actions offered under one target heading of the Then cascade: those
// that make sense on that target (IsActionValidFor), with the drink, equip
// and cast submenus as before. Choosing one sets the rule's target and the
// action together, as choosing a condition sets subject and predicate.
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

    for (std::size_t i = 0; i < static_cast<std::size_t>(ft::ActionKind::COUNT); ++i)
    {
        const auto action = static_cast<ft::ActionKind>(i);
        if (action == ft::ActionKind::None || !ft::IsActionValidFor(target, action))
            continue;
        const std::string name(ft::DisplayName(action));

        // The consume actions collapse into one "Consume" submenu, drawn
        // where the first of them falls in the list; the others are skipped.
        // Potion holds the three "strongest of a kind" policies, then every
        // potion she carries by name; Food and Ingredient, what she carries
        // of each. Every list is hers.
        if (ft::IsConsume(action))
        {
            if (action != ft::ActionKind::DrinkHealthPotion)
                continue;
            if (!BeginCascade("Consume"))
                continue;

            const auto carried = [&](ft::ConsumableKind kind) {
                bool any = false;
                for (const auto &option : view.consumables)
                    any = any || option.kind == kind;
                return any;
            };
            // The named entries of one kind, under a menu already open.
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
                        choose();
                    }
                }
            };

            if (BeginCascade("Potion"))
            {
                for (auto kind : {ft::ActionKind::DrinkHealthPotion, ft::ActionKind::DrinkStaminaPotion,
                                  ft::ActionKind::DrinkMagickaPotion})
                {
                    const bool selected = here && act.kind == kind;
                    if (CascadeItem(DrinkSubmenuLabel(kind).c_str(), selected))
                    {
                        act.kind = kind;
                        act.form = 0;
                        choose();
                    }
                    if (Im::IsItemHovered(0))
                        Im::SetTooltip("%s", std::string(ft::Describe(kind)).c_str());
                }
                if (carried(ft::ConsumableKind::Potion))
                {
                    Im::Separator();
                    named(ft::ActionKind::DrinkPotion);
                }
                Im::EndMenu();
            }
            for (const auto [label, kind] :
                 {std::pair{"Food", ft::ActionKind::EatFood}, std::pair{"Ingredient", ft::ActionKind::EatIngredient}})
            {
                if (!carried(ft::ConsumableOf(kind)) || !BeginCascade(label))
                    continue;
                named(kind);
                Im::EndMenu();
            }
            Im::EndMenu();
            continue;
        }

        // The four equips under one "Equip" heading, drawn where the first
        // of them falls: Weapon, Arrows, Spell, Armor, each its own menu.
        if (ft::IsEquip(action))
        {
            if (action != ft::ActionKind::EquipWeapon)
                continue;
            if (!BeginCascade("Equip"))
                continue;
            for (const auto kind : {ft::ActionKind::EquipWeapon, ft::ActionKind::EquipArrows,
                                    ft::ActionKind::EquipSpell, ft::ActionKind::EquipArmor})
            {
                std::string noun = EquipNoun(kind);
                if (!noun.empty())
                    noun[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(noun[0])));
                const bool open = BeginCascade(noun.c_str());
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(kind)).c_str());
                if (!open)
                    continue;
                if (EquipMenu(act, kind, view))
                    choose();
                Im::EndMenu();
            }
            Im::EndMenu();
            continue;
        }

        if (!TakesSpell(action))
        {
            const bool selected = here && act.kind == action;
            if (CascadeItem(name.c_str(), selected))
            {
                act.kind = action;
                act.form = 0;
                act.hand = Hand::None;
                choose();
            }
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", std::string(ft::Describe(action)).c_str());
            continue;
        }

        // The spells that suit this target: a Self-delivery spell (Fast
        // Healing, Oakflesh) is cast on oneself and on no one else; an aimed
        // one (Heal Other, Firebolt) goes at someone else. A follower with
        // none that fit is offered nothing rather than an empty submenu that
        // looks broken, nor a greyed line. Cast spell lists the spells, Use
        // power the powers, Shout the shouts.
        const auto kind = action == ft::ActionKind::UsePower ? SpellOption::Kind::Power
                          : action == ft::ActionKind::Shout  ? SpellOption::Kind::Shout
                                                             : SpellOption::Kind::Spell;
        std::vector<const SpellOption *> suited;
        for (const auto &option : view.spells)
            if (option.kind == kind && (option.location || option.selfOnly == (target == ft::ActionTargetKind::Self)))
                suited.push_back(&option);
        if (suited.empty())
            continue;

        if (!BeginCascade(name.c_str()))
            continue;

        for (const auto *option : suited)
        {
            const bool selected = here && act.kind == action && act.form == option->form;
            if (CascadeItem(option->name.c_str(), selected))
            {
                act.kind = action;
                act.form = option->form;
                choose();
            }
        }
        Im::EndMenu();
    }
    return changed;
}

// The Then cascade: whom first, then what -- the mirror of the If cascade's
// subject, then predicate. The headings are the same cast, less those the
// condition cannot supply: "Ally" on this side means the ally the condition
// matched, so it is offered only when the condition is about one.
bool ActionMenu(const char *id, ft::Action &act, const FollowerView &view, bool *addAnother, ft::Rule &rule)
{
    bool changed = false;

    CellButtonOpensPopup(id, TargetText(rule, view) + ": " + ActionText(act, view));

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
    // followers by name, any ally; then the threats, the particular before
    // the general -- the attacker, the target, any enemy.
    std::vector<Heading> headings{
        {ft::ActionTargetKind::Self, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Self))},
        {ft::ActionTargetKind::Player, 0,
         view.playerName.empty() ? std::string(ft::DisplayName(ft::ActionTargetKind::Player)) : view.playerName}};
    for (const auto &peer : SortedPeers(view))
        headings.push_back({ft::ActionTargetKind::Follower, peer.id, peer.name});
    headings.push_back({ft::ActionTargetKind::Ally, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Ally))});
    headings.push_back(
        {ft::ActionTargetKind::Attacker, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Attacker))});
    headings.push_back(
        {ft::ActionTargetKind::CurrentTarget, 0, std::string(ft::DisplayName(ft::ActionTargetKind::CurrentTarget))});
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

// The widest word the Status column shows, measured once.
float StatusColumnWidth()
{
    return WidestLabel({"cooldown", "no target", "count: 0", "no magicka", "invalid", "fired", "false", "pinned",
                        "outranked", "not carried"}) +
           kCellPadX * 2.0f;
}

// The drawer an open rule reveals: its actions, one row each in the order
// they are done, each its own menu; each action's own verdict; and up,
// down and remove, as the rule table's Order column. A plus beneath for
// one more. Set under the Then column -- its left edge on Then's border,
// its right on the table's -- with Status and Order the parent's widths,
// so its columns line up with the parent's and need no headings of their
// own. Returns whether the rules changed.
bool DrawActionsDrawer(ft::Rule &rule, std::size_t ruleIndex, const FollowerView &view, float left, float right,
                       float spacing)
{
    // Seamless with the row above: the drawer's left border on the Then
    // column's, its top border on the row's bottom border -- one pixel up,
    // so the two lines are one line and not a doubled one.
    Im::SetCursorScreenPos(Im::ImVec2(left, Im::GetCursorScreenPos().y - 1.0f));

    const float row = Im::GetFrameHeight();
    const float gutter = kCellPadX * 2.0f;
    const float statusWidth = StatusColumnWidth();
    const float orderWidth = row * 3.0f + kOrderGap * 2.0f + gutter;
    const float width = (std::max)(0.0f, right - left);

    bool changed = false;
    int moveFrom = -1;
    int moveTo = -1;
    int removeAt = -1;
    const std::string id = std::to_string(view.id) + "/" + std::to_string(ruleIndex);

    const auto *perAction =
        ruleIndex < view.actionTrace.size() && view.actionTrace[ruleIndex].size() == rule.actions.size()
            ? &view.actionTrace[ruleIndex]
            : nullptr;

    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    if (Im::BeginTable(("actions##" + id).c_str(), 3, flags, Im::ImVec2(width, 0.0f), 0.0f))
    {
        Im::TableSetupColumn("Action", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableSetupColumn("Status", Im::ImGuiTableColumnFlags_WidthFixed, statusWidth, 0);
        Im::TableSetupColumn("Order", Im::ImGuiTableColumnFlags_WidthFixed, orderWidth, 0);

        for (std::size_t a = 0; a < rule.actions.size(); ++a)
        {
            const std::string actId = id + "/" + std::to_string(a);
            Im::TableNextRow(0, 0.0f);

            Im::TableSetColumnIndex(0);
            if (ActionMenu(("##act" + actId).c_str(), rule.actions[a], view, nullptr, rule))
                changed = true;

            Im::TableSetColumnIndex(1);
            Im::AlignTextToFramePadding();
            if (!view.evaluated || !perAction)
            {
                // Not evaluated yet: an empty cell, not a placeholder.
            }
            else
            {
                const Status status = StatusFor((*perAction)[a], rule.actions[a].kind);
                Im::TextColored(status.color, "%s", status.text);
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", VerdictTooltip((*perAction)[a], rule.actions[a].kind, view).c_str());
            }

            Im::TableSetColumnIndex(2);
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
            if (Im::IsItemHovered(0))
                Im::SetTooltip("Remove this action");
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
        Im::SetTooltip("Add an action, done after the ones above.");

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
// Edits do not survive a reload yet. That is a real limitation, and the panel
// says so rather than letting someone spend ten minutes on a rule set that
// quietly evaporates.
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
    const float statusWidth = StatusColumnWidth();
    const float orderWidth = row * 3.0f + kOrderGap * 2.0f + gutter;

    const auto border = Im::GetColorU32(Im::ImGuiCol_TableBorderStrong, 1.0f);
    const auto stripe = Im::GetColorU32(Im::ImGuiCol_TableRowBgAlt, 1.0f);
    const auto hovered = Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f);
    const auto opened = Im::GetColorU32(Im::ImGuiCol_Header, 1.0f);
    auto *draw = Im::GetWindowDrawList();

    // The theme's spacing, read before it is pushed away: the drawer spaces
    // its plus with it, as Spacing() spaces the table's own plus below.
    const auto *style = Im::GetStyle();
    const float spacing = style ? style->ItemSpacing.y : 4.0f;
    const float framePadY = style ? style->FramePadding.y : 3.0f;

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

    const auto beginPiece = [&]() {
        const std::string id = "rules##" + std::to_string(piece++);
        if (!Im::BeginTable(id.c_str(), 6, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
            return false;
        Im::TableSetupColumn("On", Im::ImGuiTableColumnFlags_WidthFixed, onWidth, 0);
        Im::TableSetupColumn("#", Im::ImGuiTableColumnFlags_WidthFixed, numWidth, 0);
        Im::TableSetupColumn("Condition", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        // The wider share, because an action reads as a phrase ("Drink magicka
        // potion") where a condition is mostly short words and a number.
        Im::TableSetupColumn("Action", Im::ImGuiTableColumnFlags_WidthStretch, 1.25f, 0);
        // Fixed, not stretched: a stretched Status column grew with its longest
        // verdict and ate the Then cell, which is what covered the action text.
        Im::TableSetupColumn("Status", Im::ImGuiTableColumnFlags_WidthFixed, statusWidth, 0);
        Im::TableSetupColumn("Order", Im::ImGuiTableColumnFlags_WidthFixed, orderWidth, 0);
        if (piece == 1)
            Im::TableHeadersRow();
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

    bool changed = false;
    int moveFrom = -1;
    int moveTo = -1;
    int removeAt = -1;

    for (std::size_t i = 0; i < rules.rules.size(); ++i)
    {
        if (!inTable && !beginPiece())
            break;

        auto &rule = rules.rules[i];
        const std::string rowId = std::to_string(i);
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
            if (CellClicked(("##on" + rowId).c_str(), Im::GetFrameHeight()))
            {
                rule.enabled = !rule.enabled;
                changed = true;
            }
            if (Im::IsItemHovered(0))
                Im::SetTooltip(rule.enabled ? "On -- click to turn this rule off"
                                            : "Off -- click to turn this rule on");

            if (auto *drawList = Im::GetWindowDrawList(); drawList && rule.enabled)
            {
                const float size = Im::GetFrameHeight();
                const float cell = Im::GetContentRegionAvail().x;
                const float leftEdge = pos.x + (cell - size) * 0.5f;
                DrawGlyph(drawList, Glyph::Tick, {leftEdge, pos.y}, {leftEdge + size, pos.y + size},
                          Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
            }
        }

        // A rule that is off reads as off: its number, condition, action and
        // status dim together, and the If and Then cells stop answering, so
        // it cannot be edited without turning it on. The switch itself and
        // the order and delete controls stay live: an off rule is still in
        // the list and can still be moved or removed.
        Im::BeginDisabled(!rule.enabled);

        Im::TableSetColumnIndex(1);
        Im::AlignTextToFramePadding();
        Im::Text("%zu", i + 1);

        Im::TableSetColumnIndex(2);
        if (ConditionCascade(("##cond" + rowId).c_str(), rule, view))
            changed = true;

        Im::TableSetColumnIndex(3);
        // Where the Then column begins, for the drawer's border to sit on
        // it: the cell's content less its padding is the column's border.
        const float thenLeft = Im::GetCursorScreenPos().x - kCellPadX;
        if (rule.actions.empty())
            rule.actions.emplace_back();
        const std::string key = RuleKey(view.id, i);
        bool open = false;
        if (rule.actions.size() == 1)
        {
            // One action: edited here, in its row. Its menu offers a
            // second, and the rule then opens as a drawer.
            bool addAnother = false;
            if (ActionMenu(("##act" + rowId).c_str(), rule.actions.front(), view, &addAnother, rule))
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
            // listed in. A selectable the size of the cell, drawn invisible
            // and lit through the cell background so it fits by
            // construction, with the marker and the summary drawn over it.
            open = g_openRows.count(key) > 0;
            const Im::ImVec2 pos = Im::GetCursorScreenPos();
            const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
            Im::PushStyleColor(Im::ImGuiCol_Header, invisible);
            Im::PushStyleColor(Im::ImGuiCol_HeaderHovered, invisible);
            Im::PushStyleColor(Im::ImGuiCol_HeaderActive, invisible);
            const bool clicked =
                Im::Selectable(("##open" + key).c_str(), false, 0, Im::ImVec2(0.0f, Im::GetFrameHeight()));
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
            // fit the column, and the drawer is one click away.
            Im::Text("%zu actions", rule.actions.size());
        }

        Im::TableSetColumnIndex(4);
        Im::AlignTextToFramePadding();
        if (!view.evaluated)
        {
            // Not evaluated yet: an empty cell, not a placeholder.
        }
        else
        {
            const auto verdict = i < view.trace.size() ? view.trace[i] : ft::Verdict::NotReached;
            const ft::ActionKind firstKind = rule.actions.front().kind;
            const Status status = StatusFor(verdict, firstKind);
            Im::TextColored(status.color, "%s", status.text);
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", VerdictTooltip(verdict, firstKind, view).c_str());
        }

        Im::EndDisabled();

        // Order is semantics, not decoration: rules are first-match-wins, so
        // moving a row changes which rule shadows which.
        Im::TableSetColumnIndex(5);
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
        // Arrows from the icon font, like every other glyph on the row.
        Im::BeginDisabled(i == 0);
        if (GlyphButton("up" + rowId, row, Glyph::Up))
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        Im::EndDisabled();

        Im::SameLine(0.0f, kOrderGap);
        Im::BeginDisabled(i + 1 >= rules.rules.size());
        if (GlyphButton("dn" + rowId, row, Glyph::Down))
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        Im::EndDisabled();

        Im::SameLine(0.0f, kOrderGap);
        if (DeleteButton("rm" + rowId, row))
            removeAt = static_cast<int>(i);
        if (Im::IsItemHovered(0))
            Im::SetTooltip("Delete this rule");
        Im::PopStyleVar(1);

        if (!open)
            continue;

        // The drawer: close this piece, draw beneath, reopen for the rest.
        endPiece();
        Im::BeginDisabled(!rule.enabled);
        if (DrawActionsDrawer(rule, i, view, thenLeft, right, spacing))
            changed = true;
        Im::EndDisabled();
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
        fresh.actions = {{ft::ActionKind::None}};
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
void PlainHeaderRow(std::initializer_list<const char *> labels)
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
void DrawPerkDrawer(const SheetRow &row, float left, float right)
{
    constexpr float kGap = 6.0f;
    const float inset = 4.0f * kCellPadX;

    Im::Dummy(Im::ImVec2(0.0f, kGap));
    Im::SetCursorScreenPos(Im::ImVec2(left + inset, Im::GetCursorScreenPos().y));

    float nameWidth = TextWidth("Perk");
    float rankWidth = TextWidth("Rank");
    for (const auto &sub : row.detail)
    {
        nameWidth = (std::max)(nameWidth, TextWidth(sub.label));
        rankWidth = (std::max)(rankWidth, TextWidth(sub.value));
    }
    const float pad = 2.0f * kCellPadX + 8.0f;

    const auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    const float width = (std::max)(0.0f, right - left - 2.0f * inset);
    if (Im::BeginTable(("perks##" + row.label).c_str(), 3, flags, Im::ImVec2(width, 0.0f), 0.0f))
    {
        Im::TableSetupColumn("Perk", Im::ImGuiTableColumnFlags_WidthFixed, nameWidth + pad, 0);
        Im::TableSetupColumn("Rank", Im::ImGuiTableColumnFlags_WidthFixed, rankWidth + pad, 0);
        Im::TableSetupColumn("Description", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        PlainHeaderRow({"Perk", "Rank", "Description"});
        for (const auto &sub : row.detail)
        {
            Im::TableNextRow(0, 0.0f);
            Im::TableSetColumnIndex(0);
            Im::Text("%s", sub.label.c_str());
            Im::TableSetColumnIndex(1);
            Im::Text("%s", sub.value.c_str());
            Im::TableSetColumnIndex(2);
            Im::TextWrapped("%s", sub.modifiers.c_str());
        }
        Im::EndTable();
    }
    Im::Dummy(Im::ImVec2(0.0f, kGap));
}

void NoteTooltip(const std::string &note);

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
void DrawSections(const std::vector<SheetSection> &sections, bool modifiers,
                  const std::function<void(std::uint32_t)> &onLink = {})
{
    float nameWidth = 0.0f;
    float valueWidth = 0.0f;
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
        }
    }
    const float pad = 2.0f * kCellPadX + 8.0f;

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
            Im::SetCursorPosX(Im::GetCursorPosX() + kCellPadX);
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
            if (!Im::BeginTable(id.c_str(), modifiers ? 3 : 2, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
                return false;
            Im::TableSetupColumn("##name", Im::ImGuiTableColumnFlags_WidthFixed, nameWidth + pad, 0);
            // The value column takes the rest of the table when nothing
            // follows it, so a value cell that is a link lights up to the
            // table's edge rather than stopping at the widest value.
            if (modifiers)
                Im::TableSetupColumn("##value", Im::ImGuiTableColumnFlags_WidthFixed, valueWidth + pad, 0);
            else
                Im::TableSetupColumn("##value", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            if (modifiers)
            {
                Im::TableSetupColumn("Modifiers", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
                if (piece == 1)
                    PlainHeaderRow({"", "", "Modifiers"});
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

        for (const auto &row : section.rows)
        {
            if (!inTable && !beginPiece())
                break;

            Im::TableNextRow(0, 0.0f);
            if (stripeIndex++ % 2 == 1)
                Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg0, stripe, -1);
            Im::TableSetColumnIndex(0);
            bool open = false;
            if (row.detail.empty())
            {
                Im::Text("%s", row.label.c_str());
                // A row's note is hover text on its label, where there is no
                // Modifiers column to carry it.
                if (!modifiers && !row.note.empty() && Im::IsItemHovered(0))
                    NoteTooltip(row.note);
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
                const std::string key = section.title + "/" + row.label;
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

            Im::TableSetColumnIndex(1);
            if (row.form != 0 && onLink)
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
            if (modifiers)
            {
                Im::TableSetColumnIndex(2);
                Im::Text("%s", row.modifiers.c_str());
                if (!row.note.empty() && Im::IsItemHovered(0))
                    NoteTooltip(row.note);
            }
            if (!open)
                continue;

            // The drawer: close this piece, draw beneath, reopen for the rest.
            endPiece();
            DrawPerkDrawer(row, left, right);
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
    Inventory
};

struct InventoryTabState
{
    std::uint32_t detail{0};        // the item open in detail; 0 for the list
    int category{-1};               // an ItemCategory, or -1 for all of them
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

// The name filter the list tabs share: a box with "Filter name" for its
// hint, and a cross inside its right end to clear it, shown only while
// there is something to clear. Returns whether the text changed.
bool FilterBox(const char *id, char *buffer, std::size_t size)
{
    const float width = Im::GetFontSize() * 9.0f;
    Im::SetNextItemWidth(width);
    // The cross is drawn over the box's right end, and ImGui gives the
    // hover to the item drawn first unless it allows overlap: without this
    // the cross could be seen but never clicked.
    Im::SetNextItemAllowOverlap();
    bool changed = Im::InputTextWithHint(id, "Filter name", buffer, size);
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

// The Worn column's tick, centred in the cell whose top-left is `pos` and
// drawn over whatever the cell already laid out. Pinned adds a pin beside
// the tick, in the word the panel uses for it: equipped, and kept so.
// The tick for equipped, the pin for pinned, side by side when both. A
// pin without a tick is a pin the AI is fighting: the thing is promised
// to the hand but not in it this instant (the sword the AI drew over a
// pinned bow, 14:48), and the pin must not read as cleared.
void DrawTickAt(Im::ImVec2 pos, Im::ImU32 ink, bool on, bool pinned)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw || (!on && !pinned))
        return;
    // Boxes the height of the text line the row was laid out with, and a
    // glyph's width each, so the pair sits centred with the row's own margin
    // above and below.
    const float h = Im::GetTextLineHeight();
    const float box = Im::GetFontSize();
    const float cell = Im::GetContentRegionAvail().x;
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

// One bullet per line of `text`, each wrapped: the effects of a spell, a
// shout, an enchantment, a potion, which come one per line. The bullet is a
// plain dash. ImGui's Bullet() draws a circle tessellated with a handful of
// segments at that radius and reads as a polygon; U+2022 the framework's
// text face does not carry (a "?"); and U+00B7 came out as a stray symbol
// (2026-09-05). ASCII is the one thing every face has.
void BulletedLines(const std::string &text)
{
    const std::string bullet = "-";
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
        {
            Im::TextUnformatted(bullet.c_str(), nullptr);
            Im::SameLine(0.0f, -1.0f);
            Im::TextWrapped("%s", text.substr(start, end - start).c_str());
        }
        start = end + 1;
    }
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
    const float padX = style ? style->FramePadding.x : 4.0f;
    const float spacing = style ? style->ItemSpacing.x : 8.0f;
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
        if (Im::Selectable(("##chip" + chip.label).c_str(), selected == chip.id, 0, Im::ImVec2(width, 0.0f)))
            selected = chip.id;

        if (!draw)
            continue;
        const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
        if (iconFont)
            Im::ImDrawListManager::AddText(draw, iconFont, iconSize, {pos.x + padX, pos.y}, ink, icon.c_str());
        Im::ImDrawListManager::AddText(draw, {pos.x + padX + iconWidth + gap, pos.y}, ink, chip.label.c_str());
    }
}

// SkyUI's tab strip: All, then every category she has something in. Empty
// categories are left out, as SkyUI leaves them out -- a tab promising
// nothing is noise.
void DrawCategoryRow(const FollowerView &view, InventoryTabState &state)
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
    DrawChips(chips, state.category);

    // A category that has just emptied -- the last potion drunk -- falls back
    // to All rather than showing an empty table under a tab that is no
    // longer there.
    if (state.category >= 0 && counts[static_cast<std::size_t>(state.category)] == 0)
        state.category = -1;
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
// click that rounds the three states: off -> kept on -> hers to change ->
// off. The request goes to the game thread and the cell answers when the
// view comes back.
// A diagonal across the current cell, corner to corner: this cell does not
// apply -- a right hand for a shield, a hand for a cuirass. The cell's
// rectangle is the content rectangle plus the table's cell padding on each
// side, which is what the row lines are drawn around.
void SlashCell()
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw)
        return;
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const float h = Im::GetTextLineHeight();
    const float w = Im::GetContentRegionAvail().x;
    const Im::ImVec2 lo{pos.x - kCellPadX, pos.y - kCellPadY};
    const Im::ImVec2 hi{pos.x + w + kCellPadX, pos.y + h + kCellPadY};
    Im::ImDrawListManager::AddLine(draw, {lo.x, hi.y}, {hi.x, lo.y},
                                   Im::GetColorU32(Im::ImGuiCol_TableBorderStrong, 1.0f), 1.0f);
}

void OnCell(const char *id, ft::ActorId follower, std::uint32_t form, bool on, bool pinned, Hand hand, bool clickable,
            bool allowed = true)
{
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    if (!allowed)
    {
        SlashCell();
        return;
    }
    if (clickable)
    {
        // Pinned first: a pin whose thing the AI has swapped out is still a
        // pin, and the click releases it rather than pinning it again.
        if (CellClicked(id))
            RequestWear(follower, form,
                        pinned ? WearRequest::Unpin
                        : !on  ? WearRequest::Pin
                               : WearRequest::TakeOff,
                        hand);
        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", pinned ? "Release the pin; it stays equipped." : !on ? "Equip and pin." : "Unequip.");
    }
    DrawTickAt(pos, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), on, pinned);
}

// The order of an equip cell when its column is sorted, ascending: pinned,
// then equipped, then unequipped, then the slashed cells that cannot take
// it at all. What she holds to comes first, what she cannot hold last.
int CellRank(bool allowed, bool on, bool pinned)
{
    return !allowed ? 3 : pinned ? 0 : on ? 1 : 2;
}

// The rows to show, in the order the table's header asks for. Sorted every
// frame rather than on change: a hundred pointers is nothing, and the set
// itself changes with the filter and with what she picks up.
std::vector<const InventoryItem *> VisibleItems(const FollowerView &view, const InventoryTabState &state)
{
    std::vector<const InventoryItem *> rows;
    for (const auto &item : view.inventory)
    {
        if (state.category >= 0 && static_cast<int>(item.category) != state.category)
            continue;
        if (!ContainsNoCase(item.name, g_inventoryFilter))
            continue;
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
            return a.type.compare(b.type);
        case Column::Damage:
            return number(a.damage, b.damage);
        case Column::Armor:
            return number(a.armor, b.armor);
        case Column::Weight:
            return number(a.weight, b.weight);
        case Column::Value:
            return number(static_cast<float>(a.value), static_cast<float>(b.value));
        case Column::Equipped:
            return rank([](const InventoryItem &i) { return CellRank(!i.handItem, i.worn, i.pinned); });
        case Column::Left:
            return rank([](const InventoryItem &i) {
                return CellRank(i.handItem && !i.rightOnly, i.equippedLeft, i.pinnedLeft);
            });
        case Column::Right:
            return rank([](const InventoryItem &i) {
                return CellRank(i.handItem && !i.leftOnly, i.equippedRight, i.pinnedRight);
            });
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
void DrawInventoryList(const FollowerView &view, InventoryTabState &state)
{
    Im::Spacing();
    DrawCategoryRow(view, state);
    Im::Spacing();

    FilterBox("##invfilter", g_inventoryFilter, sizeof(g_inventoryFilter));
    Im::Spacing();

    // Which columns this list has. A stat column only where the stat means
    // something -- damage for weapons, rating for armour -- and an Equipped
    // column only where something can be equipped. SkyUI's lists differ the
    // same way.
    const bool weapons = state.category == static_cast<int>(ItemCategory::Weapons) ||
                         state.category == static_cast<int>(ItemCategory::Arrows);
    const bool armour = state.category == static_cast<int>(ItemCategory::Armor);
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
    float typeWidth = TextWidth("Type");
    for (const auto &item : view.inventory)
        typeWidth = (std::max)(typeWidth, TextWidth(item.type));
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
    const int columnCount = 4 + ((weapons || armour) ? 1 : 0) + (anyHand ? 2 : 0) + (anyWorn ? 1 : 0);

    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("inventory", columnCount, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn("Name", Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Name));
    Im::TableSetupColumn("Type", Im::ImGuiTableColumnFlags_WidthFixed, typeWidth + gutter,
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
        char buf[32];
        std::snprintf(buf, sizeof(buf), "##item%08X", item->form);

        Im::TableNextRow(0, 0.0f);
        // Set aside -- kept from the combat AI while a pinned spell holds a
        // hand it would take -- the whole row goes to the disabled colour.
        // Not on All, where nothing can be equipped and the dimming would
        // have no cell to explain it.
        const bool dim = item->setAside && state.category >= 0;
        if (dim)
            Im::PushStyleColor(Im::ImGuiCol_Text, Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f));
        Im::TableSetColumnIndex(0);

        // The NAME is the click target for the detail page, not the row: the
        // Worn cell has a click of its own. Highlighted through the cell
        // background for the reason DrawSections gives.
        Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
        {
            state.detail = item->form;
            state.openedFrom = Tab::Inventory;
        }
        // Over the whole cell: the Selectable is the last item here. The
        // reason and nothing else; what a pin means belongs in a help
        // section, not on every row.
        if (dim && Im::IsItemHovered(0))
            Im::SetTooltip("%s", item->asideBy.c_str());
        Im::SetCursorScreenPos(pos);
        std::string name = item->name;
        if (item->count > 1)
            name += " (" + std::to_string(item->count) + ")";
        // The enchanted tint would override the disabled colour a set-aside
        // row was pushed: dimmed wins, or the row does not read as greyed.
        if (item->enchanted && !dim)
            Im::TextColored(kEnchanted, "%s", name.c_str());
        else
            Im::Text("%s", name.c_str());

        Im::TableNextColumn();
        Im::Text("%s", item->type.c_str());

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
            std::snprintf(buf, sizeof(buf), "##left%08X", item->form);
            Im::TableNextColumn();
            if (item->handItem && !item->rightOnly)
                OnCell(buf, view.id, item->form, item->equippedLeft, item->pinnedLeft, Hand::Left, true);
            else if (item->equipable)
                SlashCell();
            std::snprintf(buf, sizeof(buf), "##right%08X", item->form);
            Im::TableNextColumn();
            if (item->handItem && !item->leftOnly)
                OnCell(buf, view.id, item->form, item->equippedRight, item->pinnedRight, Hand::Right, true);
            else if (item->equipable)
                SlashCell();
        }
        if (anyWorn)
        {
            std::snprintf(buf, sizeof(buf), "##wear%08X", item->form);
            Im::TableNextColumn();
            if (item->equipable && !item->handItem)
                OnCell(buf, view.id, item->form, item->worn, item->pinned, Hand::None, true);
            else if (item->equipable)
                SlashCell();
        }
        if (dim)
            Im::PopStyleColor(1);
    }
    Im::EndTable();
    Im::PopStyleVar(1);

    // What the list came to, and what it weighs: the reason to look in a
    // follower's bag is usually to decide whether she can carry more.
    Im::Spacing();
    // Counted against the category, not the whole bag: on Weapons, "3 of
    // 3" until the filter box takes some away. Only All counts everything.
    std::size_t inCategory = 0;
    for (const auto &item : view.inventory)
        inCategory += (state.category < 0 || static_cast<int>(item.category) == state.category) ? 1 : 0;
    const std::string shown = rows.size() == inCategory
                                  ? std::to_string(rows.size()) + " items"
                                  : std::to_string(rows.size()) + " of " + std::to_string(inCategory) + " items";
    Im::TextDisabled("%s", shown.c_str());

    char carried[64];
    std::snprintf(carried, sizeof(carried), "Carrying %.0f / %.0f", view.carriedWeight, view.carryCapacity);
    const auto *style = Im::GetStyle();
    const float inset = style ? style->ItemSpacing.x : 8.0f;
    const float rightEdge = Im::GetCursorPosX() + Im::GetContentRegionAvail().x - inset;
    Im::SameLine((std::max)(0.0f, rightEdge - TextWidth(carried)), -1.0f);
    if (view.carryCapacity > 0.0f && view.carriedWeight > view.carryCapacity)
        Im::TextColored(Im::ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", carried);
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
    if (item.enchanted)
        Im::TextColored(kEnchanted, "%s", item.name.c_str());
    else
        Im::Text("%s", item.name.c_str());
    Im::SameLine(0.0f, kCellPadX * 2.0f);
    Im::AlignTextToFramePadding();
    Im::TextDisabled("%s", item.type.c_str());

    Im::Spacing();
    DrawSections(item.detail, false);

    if (!item.effects.empty())
    {
        CentredHeading("Effects");
        BulletedLines(item.effects);
        Im::Spacing();
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
void DrawInventory(const FollowerView &view)
{
    InventoryTabState &state = g_inventoryTabs[view.id];

    if (state.detail != 0)
    {
        for (const auto &item : view.inventory)
        {
            if (item.form == state.detail)
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
    int category{-1};        // a MagicCategory, or -1 for all of them
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

std::vector<const MagicEntry *> VisibleMagic(const FollowerView &view, const MagicTabState &state)
{
    std::vector<const MagicEntry *> rows;
    for (const auto &entry : view.magic)
    {
        if (state.category >= 0 && static_cast<int>(entry.category) != state.category)
            continue;
        if (!ContainsNoCase(entry.name, g_magicFilter))
            continue;
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
            return rank([](const MagicEntry &e) { return CellRank(true, e.equipped, false); });
        case Column::Left:
            return rank([](const MagicEntry &e) { return CellRank(e.leftAllowed, e.equippedLeft, e.pinnedLeft); });
        case Column::Right:
            return rank([](const MagicEntry &e) { return CellRank(e.rightAllowed, e.equippedRight, e.pinnedRight); });
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

void DrawMagicList(const FollowerView &view, MagicTabState &state)
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
        DrawChips(chips, state.category);
        if (state.category >= 0 && counts[static_cast<std::size_t>(state.category)] == 0)
            state.category = -1;
    }
    Im::Spacing();

    FilterBox("##magicfilter", g_magicFilter, sizeof(g_magicFilter));
    Im::Spacing();

    // Which columns. A school's own list needs no School column. Spells
    // show a cell per hand; powers and shouts, which are selected rather
    // than held, show one Equipped cell for the voice slot, clicked like a
    // hand cell: ready it, pin it, put it away. One voice pin sets every
    // other power and shout aside, as a pinned quiver does the arrows.
    const bool schoolList = state.category >= 0 && state.category < static_cast<int>(MagicCategory::Shouts);
    const bool voiceList = state.category == static_cast<int>(MagicCategory::Shouts) ||
                           state.category == static_cast<int>(MagicCategory::Powers);

    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_Sortable;
    const float gutter = kCellPadX * 2.0f;
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    float schoolWidth = TextWidth("School") + arrow;
    float levelWidth = TextWidth("Level") + arrow;
    float castWidth = TextWidth("Cast") + arrow;
    float costWidth = TextWidth("Cost") + arrow;
    for (const auto &entry : view.magic)
    {
        schoolWidth = (std::max)(schoolWidth, TextWidth(entry.school));
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
    const int columnCount = 5 + (schoolList ? 0 : 1) + (allList ? 0 : voiceList ? 1 : 2);

    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("magic", columnCount, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn("Name", Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Name));
    if (!schoolList)
        Im::TableSetupColumn("School", Im::ImGuiTableColumnFlags_WidthFixed, schoolWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::School));
    Im::TableSetupColumn("Level", Im::ImGuiTableColumnFlags_WidthFixed, levelWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Level));
    Im::TableSetupColumn("Mag", Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                         magnitudeWidth, static_cast<Im::ImGuiID>(Column::Magnitude));
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
        // Kept from the AI -- a pin holds a hand it would take -- or above her
        // skill, so the AI would not choose it: the whole row is drawn in
        // the disabled colour, ticks included, since every glyph takes the
        // text colour.
        const bool dim = (entry->setAside || entry->aboveSkill) && !allList;
        if (dim)
            Im::PushStyleColor(Im::ImGuiCol_Text, Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f));
        Im::TableSetColumnIndex(0);
        Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
            state.detail = entry->form;
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
        else if (dim && entry->setAside && Im::IsItemHovered(0))
            Im::SetTooltip("%s", entry->asideBy.c_str());
        Im::SetCursorScreenPos(pos);
        Im::Text("%s", entry->name.c_str());

        // A power or a shout has no school, level or cost: those cells stay
        // empty rather than saying "Power" or "0".
        const bool voice = entry->category == MagicCategory::Shouts || entry->category == MagicCategory::Powers;
        if (!schoolList)
        {
            Im::TableNextColumn();
            if (!voice)
                Im::Text("%s", entry->school.c_str());
        }
        Im::TableNextColumn();
        if (!voice)
            Im::Text("%s", entry->level.c_str());
        Im::TableNextColumn();
        if (entry->magnitude > 0.0f)
        {
            char num[32];
            std::snprintf(num, sizeof(num), "%.0f", entry->magnitude);
            TextRightInCell(num);
        }
        Im::TableNextColumn();
        if (!voice)
            TextRightInCell(entry->cost);
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
            OnCell(buf, view.id, entry->form, entry->equipped, entry->pinned, Hand::None, true, true);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "##left%08X", entry->form);
            Im::TableNextColumn();
            // A spell above the follower's skill takes no hand at all, as
            // the tactics menus offer it for neither casting nor pinning:
            // one rule, not an equip-only state beside it.
            OnCell(buf, view.id, entry->form, entry->equippedLeft, entry->pinnedLeft, Hand::Left, !voice,
                   voice || (entry->leftAllowed && !entry->aboveSkill));
            std::snprintf(buf, sizeof(buf), "##right%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view.id, entry->form, entry->equippedRight, entry->pinnedRight, Hand::Right, !voice,
                   voice || (entry->rightAllowed && !entry->aboveSkill));
        }
        if (dim)
            Im::PopStyleColor(1);
    }
    Im::EndTable();
    Im::PopStyleVar(1);

    Im::Spacing();
    const std::string shown = rows.size() == view.magic.size() ? std::to_string(rows.size()) + " spells"
                                                               : std::to_string(rows.size()) + " of " +
                                                                     std::to_string(view.magic.size()) + " spells";
    Im::TextDisabled("%s", shown.c_str());
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

    if (!entry.effects.empty())
    {
        CentredHeading("Effects");
        BulletedLines(entry.effects);
        Im::Spacing();
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
    // The row open in detail, by effect and source; 0 for the list.
    std::uint32_t detailForm{0};
    std::uint32_t detailSource{0};
};

std::unordered_map<ft::ActorId, EffectsTabState> g_effectsTabs;
char g_effectsFilter[64]{};

// The rows that pass the filter, in the order the header asks for.
std::vector<const EffectRow *> VisibleEffects(const FollowerView &view)
{
    std::vector<const EffectRow *> rows;
    for (const auto &row : view.effects)
    {
        if (!ContainsNoCase(row.name, g_effectsFilter))
            continue;
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

void DrawEffectDetail(const EffectRow &row, EffectsTabState &state)
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
    DrawSections(row.detail, false);

    if (!row.description.empty())
    {
        CentredHeading("Description");
        Im::TextWrapped("%s", row.description.c_str());
        Im::Spacing();
    }
}

void DrawEffects(const FollowerView &view)
{
    auto &state = g_effectsTabs[view.id];
    if (state.detailForm != 0)
    {
        for (const auto &row : view.effects)
        {
            if (row.form == state.detailForm && row.sourceForm == state.detailSource)
            {
                DrawEffectDetail(row, state);
                return;
            }
        }
        // It has run out since the page was opened: back to the list.
        state = {};
    }

    Im::Spacing();
    FilterBox("##effectsfilter", g_effectsFilter, sizeof(g_effectsFilter));
    Im::Spacing();

    if (view.effects.empty())
    {
        Im::SetCursorPosX(Im::GetCursorPosX() + kCellPadX);
        Im::TextDisabled("Nothing is running on this follower.");
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
        char buf[48];
        std::snprintf(buf, sizeof(buf), "##effect%08X_%08X", row->form, row->sourceForm);

        Im::TableNextRow(0, 0.0f);
        // Running but changing nothing for this follower: the row is drawn
        // in the disabled colour, and its name hovers as "Not applied".
        if (!row->applied)
            Im::PushStyleColor(Im::ImGuiCol_Text, Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f));
        Im::TableSetColumnIndex(0);
        const Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
        {
            state.detailForm = row->form;
            state.detailSource = row->sourceForm;
        }
        if (!row->applied && Im::IsItemHovered(0))
            Im::SetTooltip("Not applied");
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
        Im::Text("%s", row->source.c_str());
        if (!row->applied)
            Im::PopStyleColor(1);
    }
    Im::EndTable();
    Im::PopStyleVar(1);
}

void DrawMagic(const FollowerView &view)
{
    MagicTabState &state = g_magicTabs[view.id];

    if (state.detail != 0)
    {
        for (const auto &entry : view.magic)
        {
            if (entry.form == state.detail)
            {
                DrawMagicDetail(entry, state);
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
// The sheet's links go nowhere: a summon's sword is not in her inventory.
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
    const float inset = style ? style->ItemSpacing.x : 8.0f;

    RowGeometry geo;
    geo.barLabelRight = originX + inset + WidestLabel({"Health", "Stamina", "Magicka"});
    geo.barLeft = geo.barLabelRight + 12.0f;
    const float contentRight = originX + Im::GetContentRegionAvail().x - inset;
    const float valueWidth = (std::max)({TextWidth(levelText), TextWidth(kindText), TextWidth(remainingText)});
    geo.valueLeft = contentRight - valueWidth;
    geo.statLabelRight = geo.valueLeft - 12.0f;

    DrawStatRow(geo, "Health", summon.health, Im::ImVec4(0.75f, 0.25f, 0.25f, 1.0f), "Level",
                [&] { Im::Text("%s", levelText.c_str()); });
    DrawStatRow(geo, "Stamina", summon.stamina, Im::ImVec4(0.30f, 0.65f, 0.35f, 1.0f), "Kind",
                [&] { Im::TextDisabled("%s", kindText.c_str()); });
    DrawStatRow(geo, "Magicka", summon.magicka, Im::ImVec4(0.25f, 0.40f, 0.80f, 1.0f), "Remaining",
                [&] { Im::Text("%s", remainingText.c_str()); });

    Im::Spacing();
    {
        // Who it is, for the console: the reference and its base.
        SheetSection identity{"Identity", {}, {}};
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", summon.id);
        SheetRow ref;
        ref.label = "Ref ID";
        ref.value = id;
        identity.rows.push_back(std::move(ref));
        std::snprintf(id, sizeof(id), "%08X", summon.baseId);
        SheetRow base;
        base.label = "Base ID";
        base.value = id;
        identity.rows.push_back(std::move(base));
        std::vector<SheetSection> sections{std::move(identity)};
        sections.insert(sections.end(), summon.sheet.begin(), summon.sheet.end());
        DrawSections(sections, false);
    }
}

// The Summons tab: what she commands right now. One page; with more than
// one, a chip per summon above it, as the Inventory tab has categories.
std::unordered_map<ft::ActorId, int> g_summonTabs;

void DrawSummons(const FollowerView &view)
{
    if (view.summons.empty())
    {
        Im::Spacing();
        Im::TextDisabled("Nothing summoned or raised.");
        return;
    }
    int &chosen = g_summonTabs[view.id];
    if (chosen < 0 || chosen >= static_cast<int>(view.summons.size()))
        chosen = 0;
    if (view.summons.size() > 1)
    {
        Im::Spacing();
        std::vector<Chip> chips;
        for (std::size_t i = 0; i < view.summons.size(); ++i)
            chips.push_back(
                {view.summons[i].name, view.summons[i].raised ? kIconRaised : kIconSummoned, static_cast<int>(i)});
        DrawChips(chips, chosen);
    }
    DrawSummon(view.summons[static_cast<std::size_t>(chosen)]);
}

// The character sheet: what she is, as opposed to what she has been told to
// do. Everything here is display only and already on the view, so it costs
// the game thread nothing extra to show.
void DrawCharacter(const FollowerView &view)
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
    const float inset = style ? style->ItemSpacing.x : 8.0f;

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

    DrawStatRow(geo, "Health", view.snapshot.health, Im::ImVec4(0.75f, 0.25f, 0.25f, 1.0f), "Level",
                [&] { Im::Text("%s", levelText.c_str()); });

    DrawStatRow(geo, "Stamina", view.snapshot.stamina, Im::ImVec4(0.30f, 0.65f, 0.35f, 1.0f), "Status", [&] {
        if (view.inCombat)
            Im::TextColored(Im::ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", statusText.c_str());
        else
            Im::TextDisabled("%s", statusText.c_str());
    });

    DrawStatRow(geo, "Magicka", view.snapshot.magicka, Im::ImVec4(0.25f, 0.40f, 0.80f, 1.0f), "Carrying", [&] {
        // Over capacity is worth seeing: an overencumbered follower
        // cannot fight properly, and otherwise you would only notice
        // by wondering why they are standing still.
        if (view.carryCapacity > 0.0f && view.carriedWeight > view.carryCapacity)
            Im::TextColored(Im::ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", carriedText.c_str());
        else
            Im::Text("%s", carriedText.c_str());
    });

    Im::Spacing();

    // A weapon, shield, ammo or torch named on the sheet is a link to its
    // page on the Inventory tab.
    const ft::ActorId id = view.id;
    DrawSections(view.sheet, false, [id](std::uint32_t form) {
        auto &state = g_inventoryTabs[id];
        state.detail = form;
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
    // The same tick as the rule rows, and like theirs absent when off --
    // not a ghost of one -- with the word beside it: one glyph for "on"
    // everywhere on this tab, not ImGui's boxed tick next to ours.
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool toggled = GlyphButton("enabled", Im::GetFrameHeight(), Glyph::Tick, followerEnabled);
    Im::PopStyleVar(1);
    if (toggled)
        SetFollowerEnabled(view.id, !followerEnabled);
    if (Im::IsItemHovered(0))
        Im::SetTooltip(followerEnabled ? "On -- click to turn this follower's tactics off"
                                       : "Off -- click to turn this follower's tactics on");
    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    Im::Text("Enabled");

    // Space, but no rule: the table's own border already reads as the boundary,
    // and a separator immediately above it draws a second line doing the same
    // job. The gap is what was missing, not the line.
    Im::Spacing();
    Im::Spacing();

    // Greyed out, NOT rewritten. Turning a follower off is a presentation change
    // over the rules, not an edit to them: each rule keeps its own `enabled`
    // exactly as the player left it, so switching back restores the list rather
    // than handing back a set of boxes they have to re-tick.
    Im::BeginDisabled(!followerEnabled);
    // Edit a copy, then hand the whole set back. Nothing partial is ever
    // visible to the tick.
    ft::RuleSet editable = rules;
    if (DrawRuleTable(editable, view))
        SetRules(view.id, std::move(editable));
    Im::EndDisabled();
}

// Followers whose page has been drawn at least once. The first time a page
// opens it lands on Tactics, which is what the mod is for; from then on the
// tab bar keeps whatever was last chosen, as tab bars do. Render thread only.
std::unordered_set<ft::ActorId> g_pagesOpened;

// One page per follower, six tabs, reading left to right as who she is,
// what she can do, what she carries, what she can cast, how her combat AI
// is tuned, and what she has been told to do. The tab bar is keyed by
// follower so each page remembers its own tab.
void DrawFollower(const ft::RuleSet &rules, const FollowerView &view)
{
    if (!Im::BeginTabBar(("follower##" + std::to_string(view.id)).c_str()))
        return;

    const bool firstOpen = g_pagesOpened.insert(view.id).second;
    const Im::ImGuiTabItemFlags tacticsFlags = firstOpen ? Im::ImGuiTabItemFlags_SetSelected : 0;

    // A pending switch, from a link on the sheet or the back arrow on an
    // item page; consumed here so it acts for one frame only.
    auto &inventoryState = g_inventoryTabs[view.id];
    const Tab select = inventoryState.select;
    inventoryState.select = Tab::None;

    if (Im::BeginTabItem("Character", nullptr, select == Tab::Character ? Im::ImGuiTabItemFlags_SetSelected : 0))
    {
        DrawCharacter(view);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Inventory", nullptr, select == Tab::Inventory ? Im::ImGuiTabItemFlags_SetSelected : 0))
    {
        DrawInventory(view);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Magic"))
    {
        DrawMagic(view);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Summons"))
    {
        DrawSummons(view);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Effects"))
    {
        DrawEffects(view);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Skills"))
    {
        Im::Spacing();
        DrawSections(view.skills, true);
        Im::EndTabItem();
    }
    // What the combat AI is tuned by, before what it is told: a rule works
    // with, or against, these numbers.
    if (Im::BeginTabItem("Combat Style"))
    {
        Im::Spacing();
        DrawSections(view.combatStyle, false);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Tactics", nullptr, tacticsFlags))
    {
        DrawTactics(rules, view);
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
//     hence a fixed pool of slots, one static trampoline apiece.
//  2. Removing an entry needs DeleteSection, which the framework's source has
//     but no released build yet exports (3.14.1 is the newest; ours is
//     3.14.0). The SDK wrapper returns false when the export is missing, so
//     a dismissed follower's entry is deleted where it can be and otherwise
//     stays, saying so, until the framework catches up.
//
// Entries are registered the first time a follower is seen, so they carry real
// names. Ordering is first-seen rather than alphabetical.

constexpr std::size_t kSlots = 8; // matches kMaxManagedFollowers in Tactics.cpp

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

    for (const auto &view : ObserveFollowers())
    {
        if (view.id == id)
        {
            DrawFollower(GetRules(view.id), view);
            return;
        }
    }

    Im::TextDisabled("Dismissed. This entry cannot be removed until the menu framework's next "
                     "release; it is reused if they come back.");
}

void DrawSettings()
{
    bool enabled = IsEnabled();
    if (Im::Checkbox("Tactics enabled for all followers", &enabled))
        SetEnabled(enabled);

    Im::TextWrapped("Turning this off stops every follower. Each follower also has their own "
                    "switch, and each rule its own -- all three must be on for a rule to run.");
    Im::TextWrapped("Rules are evaluated only while a follower is fighting. Out of combat they "
                    "are listed but not run, and the inventory is not scanned.");
    // Read the SAME predicate the tick gates on, so this cannot disagree with
    // what actually happened. Saying which of the two states we are in matters:
    // "nothing is happening because time is stopped" and "nothing is happening
    // because something is broken" otherwise look identical from here.
    const auto clock = ft::game::ReadClock();
    if (clock.stopped())
    {
        Im::TextDisabled("%s, so nothing is being evaluated. Set FreezeTimeOnMenu = false in "
                         "SKSEMenuFramework.ini to keep playing with this panel open and watch "
                         "rules fire live.",
                         clock.frozenClock ? "Time is frozen" : "The game is paused");
    }
    else
    {
        Im::TextColored(Im::ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "Time is running: rules are being evaluated right now.");
    }

    Im::Separator();

    const auto cost = ObserveCost();
    if (cost.samples > 0)
    {
        // Kept visible rather than buried in a log: docs/PLAN.md 3.2 sets a
        // budget and asks for it to be measured, not assumed.
        Im::Text("Evaluation cost: %.0f us average, %.0f us peak, per follower", cost.avgUs, cost.maxUs);
    }
    else
    {
        Im::TextDisabled("Evaluation cost: nothing measured yet.");
    }
    Im::Separator();

    const auto followers = ObserveFollowers();
    if (followers.empty())
    {
        Im::TextWrapped("No followers. Anyone travelling with you appears here as soon as they "
                        "are a teammate -- you do not need to be in a fight to set their rules "
                        "up, only to watch them run.");
        return;
    }

    Im::Text("Followers under tactics control:");
    for (const auto &view : followers)
    {
        Im::BulletText("%s  %s", view.name.c_str(), view.inCombat ? "(in combat)" : "(idle)");
        if (!view.tacticsEnabled)
        {
            Im::SameLine(0.0f, 8.0f);
            Im::TextDisabled("- tactics off");
        }
    }
}

void __stdcall RenderSettings()
{
    DrawSettings();
}

// One trampoline per slot. Tedious, and unavoidable with a render callback that
// takes no argument.
void __stdcall RenderSlot0()
{
    DrawSlot(0);
}
void __stdcall RenderSlot1()
{
    DrawSlot(1);
}
void __stdcall RenderSlot2()
{
    DrawSlot(2);
}
void __stdcall RenderSlot3()
{
    DrawSlot(3);
}
void __stdcall RenderSlot4()
{
    DrawSlot(4);
}
void __stdcall RenderSlot5()
{
    DrawSlot(5);
}
void __stdcall RenderSlot6()
{
    DrawSlot(6);
}
void __stdcall RenderSlot7()
{
    DrawSlot(7);
}

} // namespace

void SyncFollowers()
{
    if (!SKSEMenuFramework::IsInstalled())
        return;

    static const std::array<SKSEMenuFramework::Model::RenderFunction, kSlots> renderers{
        RenderSlot0, RenderSlot1, RenderSlot2, RenderSlot3, RenderSlot4, RenderSlot5, RenderSlot6, RenderSlot7};

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
    // the slot. Where it does not, the slot stays hers, so the entry still
    // reads as her page if she is recruited again.
    {
        std::scoped_lock lock(g_slotMutex);
        for (auto &slot : g_slots)
        {
            if (slot.id == 0 || present(slot.id))
                continue;
            if (SKSEMenuFramework::DeleteSection("Follower Tactics/" + slot.name))
            {
                logger::info("ui: menu entry removed for {}", slot.name);
                slot = {};
            }
        }
    }

    // New: the first free slot.
    for (const auto &view : followers)
    {
        std::size_t index = kSlots;
        {
            std::scoped_lock lock(g_slotMutex);
            bool known = false;
            for (const auto &slot : g_slots)
                known = known || slot.id == view.id;
            if (known)
                continue;
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
            continue;

        SKSEMenuFramework::SetSection("Follower Tactics");
        SKSEMenuFramework::AddSectionItem(view.name, renderers[index]);
        logger::info("ui: menu entry added for {} (slot {})", view.name, index);
    }
}

void Install()
{
    // Soft dependency, and the reason this whole file is safe to ship: without
    // the framework installed there is simply no menu, and the mod carries on
    // drinking potions.
    if (!SKSEMenuFramework::IsInstalled())
    {
        logger::info("ui: SKSE Menu Framework not installed -- no in-game panel. "
                     "Tactics still run; see this log for what they decide.");
        return;
    }

    SKSEMenuFramework::SetSection("Follower Tactics");
    SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);

    logger::info("ui: registered with SKSE Menu Framework (F1). "
                 "Follower entries appear as followers do.");
}

} // namespace ft::game::ui
