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
    // spell rule reporting "no potion" is worse than reporting nothing.
    case ft::Verdict::NoResource:
        return {TakesSpell(action) ? "no spell" : ft::IsEquip(action) ? "not carried" : "no potion", held};
    case ft::Verdict::EffectActive:
        return {ft::IsEquip(action) ? "pinned" : "active", held};
    case ft::Verdict::CannotHold:
        return {"can't pin", held};
    case ft::Verdict::Outranked:
        return {"outranked", held};
    case ft::Verdict::NoTarget:
        return {"no target", held};
    case ft::Verdict::CannotAfford:
        return {"no magicka", held};
    case ft::Verdict::Busy:
        return {"busy", held};

    case ft::Verdict::Disabled:
        return {"off", quiet};
    case ft::Verdict::NotReached:
        return {"-", quiet};

    case ft::Verdict::InvalidCondition:
        return {"invalid", broken};
    case ft::Verdict::Unsupported:
        return {"n/a", broken};

    default:
        return {"-", quiet};
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
    case ft::ArgumentKind::Distance:
        return "< " + std::to_string(static_cast<int>(value));
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
    case ft::ArgumentKind::Distance:
        return {200.0f, 500.0f, 1000.0f, 2000.0f};
    case ft::ArgumentKind::Count:
        return {2.0f, 3.0f, 4.0f, 5.0f};
    case ft::ArgumentKind::None:
    default:
        return {};
    }
}

// The whole condition on one line: "Enemy health < 25%".
std::string ConditionText(const ft::Rule &r)
{
    std::string text(ft::DisplayName(r.subject));
    text += ' ';
    text += ft::DisplayName(r.predicate);

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
bool GlyphButton(const std::string &id, float size, Glyph glyph)
{
    const bool clicked = Im::Button(("##" + id).c_str(), Im::ImVec2(size, size));
    if (auto *draw = Im::GetWindowDrawList())
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

bool ConditionCascade(const char *id, ft::Rule &rule)
{
    bool changed = false;

    CellButtonOpensPopup(id, ConditionText(rule));

    PushPopupChrome();
    if (!Im::BeginPopup(id, 0))
    {
        Im::PopStyleVar(kPopupChromeVars);
        return false;
    }

    for (std::size_t si = 0; si < static_cast<std::size_t>(ft::SubjectKind::COUNT); ++si)
    {
        const auto subject = static_cast<ft::SubjectKind>(si);
        if (!BeginCascade(std::string(ft::DisplayName(subject)).c_str()))
            continue;

        for (std::size_t pi = 0; pi < static_cast<std::size_t>(ft::PredicateKind::COUNT); ++pi)
        {
            const auto predicate = static_cast<ft::PredicateKind>(pi);
            if (!ft::IsPredicateValidFor(subject, predicate))
                continue;
            // An above predicate is listed under its below counterpart's
            // heading, after a divider, not as a heading of its own.
            if (ft::IsAbove(predicate))
                continue;

            const auto presets = PresetsFor(predicate);
            const std::string predicateName(ft::DisplayName(predicate));

            if (presets.empty())
            {
                // No argument -- a leaf.
                const bool selected = rule.subject == subject && rule.predicate == predicate;
                if (CascadeItem(predicateName.c_str(), selected))
                {
                    rule.subject = subject;
                    rule.predicate = predicate;
                    changed = true;
                }
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(predicate)).c_str());
                continue;
            }

            if (!BeginCascade(predicateName.c_str()))
                continue;

            const auto offer = [&](ft::PredicateKind which) {
                for (const float preset : PresetsFor(which))
                {
                    const bool selected = rule.subject == subject && rule.predicate == which &&
                                          std::abs(rule.conditionArg - preset) < 0.001f;
                    if (CascadeItem(ArgumentText(which, preset).c_str(), selected))
                    {
                        rule.subject = subject;
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
    return changed;
}

// The action side.
// Does this action name a spell?
bool TakesSpell(ft::ActionKind action)
{
    return action == ft::ActionKind::CastSpell || action == ft::ActionKind::EquipSpell;
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

// The four ways of drinking share one "Drink potion" submenu: the three
// "strongest of a kind" policies at the top, then every potion she carries
// by name. One entry in the action list, not four.
bool IsDrinkKind(ft::ActionKind action)
{
    return action == ft::ActionKind::DrinkHealthPotion || action == ft::ActionKind::DrinkMagickaPotion ||
           action == ft::ActionKind::DrinkStaminaPotion || action == ft::ActionKind::DrinkPotion;
}

// "Drink strongest health potion" -> "Strongest health potion", for use under
// a menu already headed "Drink potion".
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
std::string ActionText(const ft::Action &act, const FollowerView &view)
{
    const std::string base(ft::DisplayName(act.kind));

    if (act.kind == ft::ActionKind::DrinkPotion)
    {
        if (act.form == 0)
            return base + "...";
        for (const auto &option : view.potions)
            if (option.form == act.form)
                return "Drink " + option.name;
        return base + " (not carried)";
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

    for (const auto &option : view.spells)
    {
        if (option.form != act.form)
            continue;
        return "Cast " + option.name;
    }

    // Named a spell this follower does not know. Says so rather than showing a
    // plausible-looking action that can never fire -- the status column will
    // report "no spell", and the two need to agree.
    return base + " (not known)";
}

// One leaf of an equip menu: a thing she has, pinned in `hand` when
// chosen. A spell above her skill is listed but cannot be chosen: the AI
// would never pick it, so a pin on it is a promise the rule could not keep.
bool EquipLeaf(ft::Action &act, ft::ActionKind action, std::uint32_t form, const std::string &name, Hand hand,
               bool unusable)
{
    if (unusable)
    {
        Im::MenuItem((name + " (above their skill)").c_str(), nullptr, false, false);
        return false;
    }
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
    Im::Separator();

    const bool spell = action == ft::ActionKind::EquipSpell;
    const bool handed = spell || action == ft::ActionKind::EquipWeapon;
    if (!handed)
    {
        const ItemCategory category =
            action == ft::ActionKind::EquipArrows ? ItemCategory::Arrows : ItemCategory::Armor;
        bool any = false;
        for (const auto &item : view.inventory)
        {
            if (item.category != category)
                continue;
            any = true;
            if (EquipLeaf(act, action, item.form, item.name, Hand::None, false))
                changed = true;
        }
        if (!any)
            Im::MenuItem("(carries none)", nullptr, false, false);
        return changed;
    }

    for (const Hand hand : {Hand::Left, Hand::Right, Hand::Both})
    {
        const std::string label(ft::DisplayName(hand));
        bool any = false;
        if (spell)
        {
            for (const auto &entry : view.magic)
                any = any || (entry.category != MagicCategory::Shouts && entry.category != MagicCategory::Powers &&
                              Fits(entry.grip, hand, true));
        }
        else
        {
            for (const auto &item : view.inventory)
                any = any || (item.category == ItemCategory::Weapons && Fits(item.grip, hand, false));
        }
        if (!any)
        {
            Im::MenuItem((label + " (nothing fits)").c_str(), nullptr, false, false);
            continue;
        }
        if (!BeginCascade(label.c_str()))
            continue;
        if (spell)
        {
            for (const auto &entry : view.magic)
            {
                if (entry.category == MagicCategory::Shouts || entry.category == MagicCategory::Powers ||
                    !Fits(entry.grip, hand, true))
                    continue;
                if (EquipLeaf(act, action, entry.form, entry.name, hand, entry.aboveSkill))
                    changed = true;
            }
        }
        else
        {
            for (const auto &item : view.inventory)
            {
                if (item.category != ItemCategory::Weapons || !Fits(item.grip, hand, false))
                    continue;
                if (EquipLeaf(act, action, item.form, item.name, hand, false))
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
bool ActionMenu(const char *id, ft::Action &act, const FollowerView &view, bool *addAnother)
{
    bool changed = false;

    CellButtonOpensPopup(id, ActionText(act, view));

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
        if (CascadeItem("Add another action...", false))
        {
            *addAnother = true;
            changed = true;
        }
        Im::Separator();
    }

    for (std::size_t i = 0; i < static_cast<std::size_t>(ft::ActionKind::COUNT); ++i)
    {
        const auto action = static_cast<ft::ActionKind>(i);
        const std::string name(ft::DisplayName(action));

        // The drink actions collapse into one submenu, drawn where the first
        // of them falls in the list; the others are skipped.
        if (IsDrinkKind(action))
        {
            if (action != ft::ActionKind::DrinkHealthPotion)
                continue;
            if (!BeginCascade("Drink potion"))
                continue;

            for (auto kind : {ft::ActionKind::DrinkHealthPotion, ft::ActionKind::DrinkStaminaPotion,
                              ft::ActionKind::DrinkMagickaPotion})
            {
                const bool selected = act.kind == kind;
                if (CascadeItem(DrinkSubmenuLabel(kind).c_str(), selected))
                {
                    act.kind = kind;
                    act.form = 0;
                    changed = true;
                }
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(kind)).c_str());
            }

            if (!view.potions.empty())
            {
                Im::Separator();
                for (const auto &option : view.potions)
                {
                    const std::string label = option.name + " (" + std::to_string(option.count) + ")";
                    const bool selected = act.kind == ft::ActionKind::DrinkPotion && act.form == option.form;
                    if (CascadeItem(label.c_str(), selected))
                    {
                        act.kind = ft::ActionKind::DrinkPotion;
                        act.form = option.form;
                        changed = true;
                    }
                }
            }
            Im::EndMenu();
            continue;
        }

        if (ft::IsEquip(action))
        {
            const bool open = BeginCascade(name.c_str());
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", std::string(ft::Describe(action)).c_str());
            if (!open)
                continue;
            if (EquipMenu(act, action, view))
                changed = true;
            Im::EndMenu();
            continue;
        }

        if (!TakesSpell(action))
        {
            const bool selected = act.kind == action;
            if (CascadeItem(name.c_str(), selected))
            {
                act.kind = action;
                act.form = 0;
                act.hand = Hand::None;
                changed = true;
            }
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", std::string(ft::Describe(action)).c_str());
            continue;
        }

        // A follower with no castable spells is offered nothing rather than an
        // empty submenu that looks broken.
        if (view.spells.empty())
        {
            Im::MenuItem((name + " (knows none)").c_str(), nullptr, false, false);
            continue;
        }

        if (!BeginCascade(name.c_str()))
            continue;

        for (const auto &option : view.spells)
        {
            const bool selected = act.kind == action && act.form == option.form;
            if (CascadeItem(option.name.c_str(), selected))
            {
                act.kind = action;
                act.form = option.form;
                changed = true;
            }
        }
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
    return WidestLabel({"cooldown", "no target", "no potion", "no magicka", "invalid", "fired", "false", "pinned",
                        "outranked"}) +
           kCellPadX * 2.0f;
}

// The drawer an open rule reveals: its actions, one row each in the order
// they are done, each its own menu; each action's own verdict; and up,
// down and remove, as the rule table's Order column. A plus beneath for
// one more. Set under the Then column -- its left edge on Then's border,
// its right on the table's -- with Status and Order the parent's widths,
// so its columns line up with the parent's and need no headings of their
// own. Returns whether the rules changed.
bool DrawActionsDrawer(ft::Rule &rule, std::size_t ruleIndex, const FollowerView &view, float textLeft, float right,
                       float spacing)
{
    // The action text lines up with the parent's "N actions": the menu's
    // button starts at the table's edge and puts its text one frame
    // padding in, so the table starts that much before the parent's text.
    const auto *style = Im::GetStyle();
    const float left = textLeft - (style ? style->FramePadding.x : 4.0f);

    Im::Dummy(Im::ImVec2(0.0f, spacing));
    Im::SetCursorScreenPos(Im::ImVec2(left, Im::GetCursorScreenPos().y));

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
            if (ActionMenu(("##act" + actId).c_str(), rule.actions[a], view, nullptr))
                changed = true;

            Im::TableSetColumnIndex(1);
            Im::AlignTextToFramePadding();
            if (!view.evaluated || !perAction)
            {
                Im::TextDisabled("-");
            }
            else
            {
                const Status status = StatusFor((*perAction)[a], rule.actions[a].kind);
                Im::TextColored(status.color, "%s", status.text);
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", ft::Explain((*perAction)[a], rule.actions[a].kind));
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
        Im::TableSetupColumn("If", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        // The wider share, because an action reads as a phrase ("Drink magicka
        // potion") where a condition is mostly short words and a number.
        Im::TableSetupColumn("Then", Im::ImGuiTableColumnFlags_WidthStretch, 1.25f, 0);
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
        if (ConditionCascade(("##cond" + rowId).c_str(), rule))
            changed = true;

        Im::TableSetColumnIndex(3);
        // Where the Then cell's text begins once the marker has had its
        // width, for the drawer's actions to line up under it.
        const float textLeft = Im::GetCursorScreenPos().x + DisclosureWidth();
        if (rule.actions.empty())
            rule.actions.emplace_back();
        const std::string key = RuleKey(view.id, i);
        bool open = false;
        if (rule.actions.size() == 1)
        {
            // One action: edited here, in its row. Its menu offers a
            // second, and the rule then opens as a drawer.
            bool addAnother = false;
            if (ActionMenu(("##act" + rowId).c_str(), rule.actions.front(), view, &addAnother))
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
            Im::TextDisabled("-");
        }
        else
        {
            const auto verdict = i < view.trace.size() ? view.trace[i] : ft::Verdict::NotReached;
            const ft::ActionKind firstKind = rule.actions.front().kind;
            const Status status = StatusFor(verdict, firstKind);
            Im::TextColored(status.color, "%s", status.text);
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", ft::Explain(verdict, firstKind));
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
        if (DrawActionsDrawer(rule, i, view, textLeft, right, spacing))
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
        fresh.actionTarget = ft::ActionTargetKind::ConditionSubject;
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
                    Im::SetTooltip("%s", row.note.c_str());
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
            Im::Text("%s", row.value.c_str());
            if (modifiers)
            {
                Im::TableSetColumnIndex(2);
                Im::Text("%s", row.modifiers.c_str());
                if (!row.note.empty() && Im::IsItemHovered(0))
                    Im::SetTooltip("%s", row.note.c_str());
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
    Magnitude
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
            bool allowed = true, bool pinnable = true)
{
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    if (!allowed)
    {
        SlashCell();
        return;
    }
    if (clickable && !pinnable)
    {
        // A pin is a promise the AI will use it, and for this it would not
        // be kept: equip and unequip only, two states.
        if (CellClicked(id))
            RequestWear(follower, form, !on ? WearRequest::Equip : WearRequest::TakeOff, hand);
        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", !on ? "Equip; it cannot be pinned, the AI would not use it." : "Unequip.");
    }
    else if (clickable)
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

    Im::SetNextItemWidth(Im::GetFontSize() * 9.0f);
    Im::InputTextWithHint("##invfilter", "Filter", g_inventoryFilter, sizeof(g_inventoryFilter));
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
        Im::TextWrapped("%s", item.effects.c_str());
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

    Im::SetNextItemWidth(Im::GetFontSize() * 9.0f);
    Im::InputTextWithHint("##magicfilter", "Filter", g_magicFilter, sizeof(g_magicFilter));
    Im::Spacing();

    // Which columns. A school's own list needs no School column. Spells
    // show a cell per hand; powers and shouts, which are selected rather
    // than held, show one Equipped cell, read-only: they are readied by the
    // voice slot, which this does not drive.
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
        // Selectable, before the name is drawn over it.
        if (dim && entry->setAside && Im::IsItemHovered(0))
            Im::SetTooltip("%s", entry->asideBy.c_str());
        else if (dim && entry->aboveSkill && Im::IsItemHovered(0))
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
        Im::SetCursorScreenPos(pos);
        Im::Text("%s", entry->name.c_str());

        if (!schoolList)
        {
            Im::TableNextColumn();
            Im::Text("%s", entry->school.c_str());
        }
        Im::TableNextColumn();
        Im::Text("%s", entry->level.c_str());
        Im::TableNextColumn();
        if (entry->magnitude > 0.0f)
        {
            char num[32];
            std::snprintf(num, sizeof(num), "%.0f", entry->magnitude);
            TextRightInCell(num);
        }
        Im::TableNextColumn();
        TextRightInCell(entry->cost);
        Im::TableNextColumn();
        Im::Text("%s", entry->cast.c_str());

        const bool voice = entry->category == MagicCategory::Shouts || entry->category == MagicCategory::Powers;
        if (allList)
        {
            // no equip cells
        }
        else if (voiceList)
        {
            Im::TableNextColumn();
            pos = Im::GetCursorScreenPos();
            if (entry->equipped)
                DrawTickAt(pos, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), true, false);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "##left%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view.id, entry->form, entry->equippedLeft, entry->pinnedLeft, Hand::Left, !voice,
                   voice || entry->leftAllowed, !entry->aboveSkill);
            std::snprintf(buf, sizeof(buf), "##right%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view.id, entry->form, entry->equippedRight, entry->pinnedRight, Hand::Right, !voice,
                   voice || entry->rightAllowed, !entry->aboveSkill);
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
        Im::TextWrapped("%s", entry.effects.c_str());
        Im::Spacing();
    }
    if (!entry.description.empty())
    {
        CentredHeading("Description");
        Im::TextWrapped("%s", entry.description.c_str());
        Im::Spacing();
    }
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
    // The same tick as the rule rows, ghosted when off, with the word beside
    // it: one glyph for "on" everywhere on this tab, not ImGui's boxed tick
    // next to ours.
    if (!followerEnabled)
        Im::PushStyleColor(Im::ImGuiCol_Text, Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f));
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool toggled = GlyphButton("enabled", Im::GetFrameHeight(), Glyph::Tick);
    Im::PopStyleVar(1);
    if (!followerEnabled)
        Im::PopStyleColor(1);
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
