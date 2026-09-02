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
#include "game/Tactics.h"

#include "SKSEMenuFramework.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
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
        return {TakesSpell(action) ? "no spell" : "no potion", held};
    case ft::Verdict::EffectActive:
        return {action == ft::ActionKind::EquipSpell ? "equipped" : "active", held};
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

// Padding around the enable tick, and it is deliberately not the theme's.
//
// A checkbox is square at GetFrameHeight(), which with a 32pt font is a ~46px
// box around a tick that needs nothing like it -- and every pixel of that is
// width the Then column does not get. This one control gets a tight frame so
// the column can be sized to the tick instead of to the font.
constexpr float kTickPad = 3.0f;

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
        return "< " + std::to_string(static_cast<int>(value * 100.0f + 0.5f)) + "%";
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
        return {0.10f, 0.25f, 0.50f, 0.75f, 0.90f};
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
// A delete button whose X is drawn, not typed.
//
// The letter "X" is sized and positioned by the font, so beside two vector
// triangles it never quite agrees with them. The obvious glyph upgrades are a
// worse bet, not a better one: MainFont.ttf is a Latin face -- every non-Latin
// range in SKSEMenuFramework.ini is opt-in and off by default -- so U+2715 and
// a trashcan emoji would likely come back as blank boxes. Two lines cost
// nothing, centre by construction, and cannot go missing.
bool DeleteButton(const std::string &id, float size)
{
    const bool clicked = Im::Button(("##" + id).c_str(), Im::ImVec2(size, size));

    const Im::ImVec2 lo = Im::GetItemRectMin();
    const Im::ImVec2 hi = Im::GetItemRectMax();
    const float inset = size * 0.30f;
    const float thickness = (std::max)(1.0f, size * 0.07f);
    const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);

    if (auto *draw = Im::GetWindowDrawList())
    {
        Im::ImDrawListManager::AddLine(draw, {lo.x + inset, lo.y + inset}, {hi.x - inset, hi.y - inset}, ink,
                                       thickness);
        Im::ImDrawListManager::AddLine(draw, {hi.x - inset, lo.y + inset}, {lo.x + inset, hi.y - inset}, ink,
                                       thickness);
    }
    return clicked;
}

// Chrome for the cascade popups, shared by the condition and action menus.
//
// ImGui positions a nested menu deliberately overlapping its parent by
// ItemSpacing.x -- a mouse-travel convenience, since it means a diagonal drag
// toward the submenu does not fall off the parent. With a theme whose spacing
// and borders are as heavy as SKYRIMDEFAULT's, that overlap stops reading as
// helpful and starts reading as two windows colliding. Tightening the spacing
// shrinks the overlap and thinning the border stops the two edges doubling up.
//
// WindowPadding is deliberately NOT touched. Overriding it here is what left
// the menu text jammed against the popup border: the overlap is driven by
// ItemSpacing alone, so padding was never the lever and shrinking it only cost
// the margin. The theme's own padding is the right value.
//
// Pushed BEFORE BeginPopup: a popup takes its padding and border when the
// window is created, not while it is being filled. Popped on every path out,
// the closed one included.
void PushPopupChrome()
{
    Im::PushStyleVar(Im::ImGuiStyleVar_PopupBorderSize, 1.0f);
    Im::PushStyleVar(Im::ImGuiStyleVar_ItemSpacing, Im::ImVec2(6.0f, 4.0f));
}

constexpr int kPopupChromeVars = 2;

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
        if (!Im::BeginMenu(std::string(ft::DisplayName(subject)).c_str(), true))
            continue;

        for (std::size_t pi = 0; pi < static_cast<std::size_t>(ft::PredicateKind::COUNT); ++pi)
        {
            const auto predicate = static_cast<ft::PredicateKind>(pi);
            if (!ft::IsPredicateValidFor(subject, predicate))
                continue;

            const auto presets = PresetsFor(predicate);
            const std::string predicateName(ft::DisplayName(predicate));

            if (presets.empty())
            {
                // No argument -- a leaf.
                const bool selected = rule.subject == subject && rule.predicate == predicate;
                if (Im::MenuItem(predicateName.c_str(), nullptr, selected, true))
                {
                    rule.subject = subject;
                    rule.predicate = predicate;
                    changed = true;
                }
                if (Im::IsItemHovered(0))
                    Im::SetTooltip("%s", std::string(ft::Describe(predicate)).c_str());
                continue;
            }

            if (!Im::BeginMenu(predicateName.c_str(), true))
                continue;

            for (const float preset : presets)
            {
                const bool selected = rule.subject == subject && rule.predicate == predicate &&
                                      std::abs(rule.conditionArg - preset) < 0.001f;
                if (Im::MenuItem(ArgumentText(predicate, preset).c_str(), nullptr, selected, true))
                {
                    rule.subject = subject;
                    rule.predicate = predicate;
                    rule.conditionArg = preset;
                    changed = true;
                }
            }
            Im::EndMenu();
        }
        Im::EndMenu();
    }

    Im::EndPopup();
    Im::PopStyleVar(kPopupChromeVars);
    return changed;
}

// The action side. Flat for now: actions have no sub-options until selectors
// land (docs/PLAN.md 3.5.2), at which point "equip item" grows a submenu of
// what to equip and this becomes a cascade too.
// Does this action name a spell?
bool TakesSpell(ft::ActionKind action)
{
    return action == ft::ActionKind::CastSpell || action == ft::ActionKind::EquipSpell;
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
std::string ActionText(const ft::Rule &rule, const FollowerView &view)
{
    const std::string base(ft::DisplayName(rule.action));

    if (rule.action == ft::ActionKind::DrinkPotion)
    {
        if (rule.actionForm == 0)
            return base + "...";
        for (const auto &option : view.potions)
            if (option.form == rule.actionForm)
                return "Drink " + option.name;
        return base + " (not carried)";
    }

    if (!TakesSpell(rule.action))
        return base;

    if (rule.actionForm == 0)
        return base + "...";

    for (const auto &option : view.spells)
    {
        if (option.form != rule.actionForm)
            continue;
        return (rule.action == ft::ActionKind::CastSpell ? "Cast " : "Equip ") + option.name;
    }

    // Named a spell this follower does not know. Says so rather than showing a
    // plausible-looking action that can never fire -- the status column will
    // report "no spell", and the two need to agree.
    return base + " (not known)";
}

// The action side of the cascade.
//
// Flat for everything that takes no argument; a submenu of the follower's own
// spells for the two that do. The list is hers, so a rule cannot name a spell
// she does not have -- the same guarantee the condition side gets from the
// validity matrix, and for the same reason: an unfireable rule should be
// unauthorable, not merely discouraged.
bool ActionMenu(const char *id, ft::Rule &rule, const FollowerView &view)
{
    bool changed = false;

    CellButtonOpensPopup(id, ActionText(rule, view));

    PushPopupChrome();
    if (!Im::BeginPopup(id, 0))
    {
        Im::PopStyleVar(kPopupChromeVars);
        return false;
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
            if (!Im::BeginMenu("Drink potion", true))
                continue;

            for (auto kind : {ft::ActionKind::DrinkHealthPotion, ft::ActionKind::DrinkStaminaPotion,
                              ft::ActionKind::DrinkMagickaPotion})
            {
                const bool selected = rule.action == kind;
                if (Im::MenuItem(DrinkSubmenuLabel(kind).c_str(), nullptr, selected, true))
                {
                    rule.action = kind;
                    rule.actionForm = 0;
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
                    const bool selected = rule.action == ft::ActionKind::DrinkPotion && rule.actionForm == option.form;
                    if (Im::MenuItem(label.c_str(), nullptr, selected, true))
                    {
                        rule.action = ft::ActionKind::DrinkPotion;
                        rule.actionForm = option.form;
                        changed = true;
                    }
                }
            }
            Im::EndMenu();
            continue;
        }

        if (!TakesSpell(action))
        {
            const bool selected = rule.action == action;
            if (Im::MenuItem(name.c_str(), nullptr, selected, true))
            {
                rule.action = action;
                rule.actionForm = 0;
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

        if (!Im::BeginMenu(name.c_str(), true))
            continue;

        for (const auto &option : view.spells)
        {
            const bool selected = rule.action == action && rule.actionForm == option.form;
            if (Im::MenuItem(option.name.c_str(), nullptr, selected, true))
            {
                rule.action = action;
                rule.actionForm = option.form;
                changed = true;
            }
        }
        Im::EndMenu();
    }

    Im::EndPopup();
    Im::PopStyleVar(kPopupChromeVars);
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
bool DrawRuleTable(ft::RuleSet &rules, const FollowerView &view)
{
    constexpr auto flags =
        Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_SizingStretchProp;

    // Cells keep a normal margin so headers and the number column are not
    // jammed against the border. The If/Then buttons cancel it locally -- see
    // CellButtonOpensPopup -- so their highlight still fills the whole cell.
    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, 2.0f));

    if (!Im::BeginTable("rules", 6, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return false;
    }

    // Fixed widths are MEASURED, not hardcoded. The panel's font size comes
    // from SKSEMenuFramework.ini (FontSizeMedium, 32 by default), so a pixel
    // count that fits at one size clips at another -- which is exactly how
    // three buttons ended up in a column too narrow to hold them.
    const float row = Im::GetFrameHeight();
    const float gutter = kCellPadX * 2.0f;
    const float tick = Im::GetFontSize() + kTickPad * 2.0f;
    const float onWidth = tick + gutter;
    const float numWidth = Im::CalcTextSize("99", nullptr, false, -1.0f).x + gutter;
    const float statusWidth =
        WidestLabel({"cooldown", "no target", "no potion", "no magicka", "invalid", "fired", "false"}) + gutter;
    const float orderWidth = row * 3.0f + kOrderGap * 2.0f + gutter;

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
    Im::TableHeadersRow();

    bool changed = false;
    int moveFrom = -1;
    int moveTo = -1;
    int removeAt = -1;

    for (std::size_t i = 0; i < rules.rules.size(); ++i)
    {
        auto &rule = rules.rules[i];
        const std::string rowId = std::to_string(i);
        Im::TableNextRow(0, 0.0f);

        Im::TableSetColumnIndex(0);
        {
            const float widget = Im::GetFontSize() + kTickPad * 2.0f;
            const float cell = Im::GetContentRegionAvail().x;
            if (cell > widget)
                Im::SetCursorPosX(Im::GetCursorPosX() + (cell - widget) * 0.5f);
            // Centre it against the row too: the row's height comes from the
            // taller buttons beside it, so a tight tick would otherwise ride
            // high in its cell.
            if (const float slack = Im::GetFrameHeight() - widget; slack > 0.0f)
                Im::SetCursorPosY(Im::GetCursorPosY() + slack * 0.5f);

            Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
            Im::PushStyleVar(Im::ImGuiStyleVar_FramePadding, Im::ImVec2(kTickPad, kTickPad));
            if (Im::Checkbox(("##on" + rowId).c_str(), &rule.enabled))
                changed = true;
            Im::PopStyleVar(2);
        }

        Im::TableSetColumnIndex(1);
        Im::AlignTextToFramePadding();
        Im::Text("%zu", i + 1);

        Im::TableSetColumnIndex(2);
        if (ConditionCascade(("##cond" + rowId).c_str(), rule))
            changed = true;

        Im::TableSetColumnIndex(3);
        if (ActionMenu(("##act" + rowId).c_str(), rule, view))
            changed = true;

        Im::TableSetColumnIndex(4);
        Im::AlignTextToFramePadding();

        if (!view.evaluated)
        {
            Im::TextDisabled("-");
        }
        else
        {
            const auto verdict = i < view.trace.size() ? view.trace[i] : ft::Verdict::NotReached;
            const Status status = StatusFor(verdict, rule.action);
            Im::TextColored(status.color, "%s", status.text);
            if (Im::IsItemHovered(0))
                Im::SetTooltip("%s", ft::ToString(verdict));
        }

        // Order is semantics, not decoration: rules are first-match-wins, so
        // moving a row changes which rule shadows which.
        Im::TableSetColumnIndex(5);
        {
            // Centre the three as a group, using the SAME gap the layout below
            // actually uses. Measuring with ItemSpacing while laying out with
            // kOrderGap overstated the group by ~12px and shifted it left.
            const float button = Im::GetFrameHeight();
            const float group = button * 3.0f + kOrderGap * 2.0f;
            const float cell = Im::GetContentRegionAvail().x;
            if (cell > group)
                Im::SetCursorPosX(Im::GetCursorPosX() + (cell - group) * 0.5f);
        }
        Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
        // ArrowButton, not a "^" and a "v".
        //
        // Those are two glyphs with nothing in common: "^" is a diacritic that
        // sits up near the cap line, "v" is a lowercase letter on the baseline.
        // Centring the text box centres neither triangle, so the pair always
        // looked misaligned however the cell was measured. ArrowButton draws a
        // vector triangle centred in the frame, and needs nothing of the font.
        Im::BeginDisabled(i == 0);
        if (Im::ArrowButton(("up" + rowId).c_str(), Im::ImGuiDir_Up))
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        Im::EndDisabled();

        Im::SameLine(0.0f, kOrderGap);
        Im::BeginDisabled(i + 1 >= rules.rules.size());
        if (Im::ArrowButton(("dn" + rowId).c_str(), Im::ImGuiDir_Down))
        {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        Im::EndDisabled();

        Im::SameLine(0.0f, kOrderGap);
        if (DeleteButton("rm" + rowId, Im::GetFrameHeight()))
            removeAt = static_cast<int>(i);
        if (Im::IsItemHovered(0))
            Im::SetTooltip("Delete this rule");
        Im::PopStyleVar(1);
    }

    Im::EndTable();
    Im::PopStyleVar(1);

    // Applied after the loop: mutating the vector mid-iteration would invalidate
    // the reference the current row still holds.
    if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(rules.rules.size()))
    {
        std::swap(rules.rules[static_cast<std::size_t>(moveFrom)], rules.rules[static_cast<std::size_t>(moveTo)]);
        changed = true;
    }
    if (removeAt >= 0)
    {
        rules.rules.erase(rules.rules.begin() + removeAt);
        changed = true;
    }

    Im::Spacing();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool addClicked = Im::Button("+##addrule", Im::ImVec2(Im::GetFrameHeight(), 0.0f));
    Im::PopStyleVar(1);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("%s", "Add a rule. They run top to bottom; the first that can act wins.");

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
        fresh.action = ft::ActionKind::None;
        rules.rules.push_back(fresh);
        changed = true;
    }
    return changed;
}

void DrawFollower(const ft::RuleSet &rules, const FollowerView &view)
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
    // against the bars, so it lines up with the rule table below it.
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

    DrawStatRow(geo, "Magicka", view.snapshot.magicka, Im::ImVec4(0.25f, 0.40f, 0.80f, 1.0f), "Carried", [&] {
        // Over capacity is worth seeing: an overencumbered follower
        // cannot fight properly, and otherwise you would only notice
        // by wondering why they are standing still.
        if (view.carryCapacity > 0.0f && view.carriedWeight > view.carryCapacity)
            Im::TextColored(Im::ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", carriedText.c_str());
        else
            Im::Text("%s", carriedText.c_str());
    });

    Im::Spacing();

    // This follower's switch. The first genuinely interactive control: the UI
    // runs on the render thread and the tick on the game thread, so the setter
    // takes a lock rather than writing shared state directly.
    bool followerEnabled = view.tacticsEnabled;
    if (Im::Checkbox("Tactics enabled", &followerEnabled))
        SetFollowerEnabled(view.id, followerEnabled);

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

// --- menu entries -----------------------------------------------------------
//
// One entry per follower under "Follower Tactics", rather than everyone stacked
// inside a single page. Two SDK constraints shape how:
//
//  1. RenderFunction is `void(__stdcall*)()` with no user data, so an entry
//     cannot be told which follower it is for. Each needs its own function --
//     hence a fixed pool of slots, one static trampoline apiece.
//  2. There is no way to REMOVE a section item. The SDK offers Unregister for
//     events, input and HUD elements, but not for menu entries. So an entry
//     outlives its follower being dismissed, and says so rather than lying.
//
// Entries are registered the first time a follower is seen, so they carry real
// names. Ordering is first-seen rather than alphabetical, forced by the same
// constraint: re-sorting would mean removing and re-adding.

constexpr std::size_t kSlots = 8; // matches kMaxManagedFollowers in Tactics.cpp

std::mutex g_slotMutex;
std::array<ft::ActorId, kSlots> g_slotIds{};
std::size_t g_slotsUsed = 0;

[[nodiscard]] ft::ActorId SlotOwner(std::size_t slot)
{
    std::scoped_lock lock(g_slotMutex);
    return slot < kSlots ? g_slotIds[slot] : 0;
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

    Im::TextWrapped("This follower is not travelling with you at the moment. The entry stays "
                    "because menu items cannot be removed once added.");
}

void DrawGeneral()
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

void __stdcall RenderGeneral()
{
    DrawGeneral();
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

void RegisterNewFollowers()
{
    if (!SKSEMenuFramework::IsInstalled())
        return;

    static const std::array<SKSEMenuFramework::Model::RenderFunction, kSlots> renderers{
        RenderSlot0, RenderSlot1, RenderSlot2, RenderSlot3, RenderSlot4, RenderSlot5, RenderSlot6, RenderSlot7};

    for (const auto &view : ObserveFollowers())
    {
        std::size_t slot = 0;
        {
            std::scoped_lock lock(g_slotMutex);
            bool known = false;
            for (std::size_t i = 0; i < g_slotsUsed; ++i)
            {
                if (g_slotIds[i] == view.id)
                {
                    known = true;
                    break;
                }
            }
            if (known || g_slotsUsed >= kSlots)
                continue;

            slot = g_slotsUsed++;
            g_slotIds[slot] = view.id;
        }

        SKSEMenuFramework::SetSection("Follower Tactics");
        SKSEMenuFramework::AddSectionItem(view.name, renderers[slot]);
        logger::info("ui: menu entry added for {} (slot {})", view.name, slot);
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
    SKSEMenuFramework::AddSectionItem("General", RenderGeneral);

    logger::info("ui: registered with SKSE Menu Framework (F1). "
                 "Follower entries appear as followers do.");
}

} // namespace ft::game::ui
