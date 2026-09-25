// The rule table -- a row per rule, the drawer of its actions, the
// switch, reordering -- and the Tactics tabs that hold it.

#include "game/ui/Panel.h"
#include "game/ui/Rules.h"
#include "game/ui/Tabs.h"
#include "game/ui/Widgets.h"

#include "core/OpenRows.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

// Width of the widest of a set of labels. Measured rather than hardcoded so the
// columns survive translation, where "Magicka" might be "Zauberkraft".
// Spacing between the three order buttons. Named because it is used twice --
// once to lay them out and once to centre them -- and the two silently
// disagreeing (2px of layout, ItemSpacing of maths) is what pushed the group
// off centre and clipped the delete button.
constexpr float kOrderGap = 2.0f;

std::string RuleKey(ft::ActorId follower, ft::Moment moment, std::size_t index)
{
    return ft::OpenRows::Key(follower, moment, index);
}

// The open state follows the rule when rules are moved or removed, so a
// drawer does not stay behind at an index another rule has taken
// (core/OpenRows.h, tested). A move is from one place to another, the
// rules between shifting a place: a drag's, or an arrow's one step.
void MoveOpenState(ft::ActorId follower, ft::Moment moment, std::size_t from, std::size_t to)
{
    g_openRows.Move(follower, moment, from, to);
}

void RemoveOpenState(ft::ActorId follower, ft::Moment moment, std::size_t at, std::size_t count)
{
    g_openRows.Remove(follower, moment, at, count);
}

// The Order cell's three buttons -- up, down, remove -- centred in the cell
// as a group and borderless, for the rule table and for the action table
// inside it. A move writes its two indices into `moveFrom` and `moveTo`, a
// removal writes its one into `removeAt`, and whatever was not asked for is
// left as it was.
//
// `dimmed` greys the arrow that would go nowhere the panel's way rather
// than ImGui's. The rule table needs that: it sits inside a BeginDimmed
// whose disabled alpha is 1, so a plain BeginDisabled there took no clicks
// while looking exactly like its neighbours.
void OrderButtons(const std::string &id, float row, std::size_t index, std::size_t count, bool dimmed, int &moveFrom,
                  int &moveTo, int &removeAt)
{
    // A frame height each, filling the row: tried at the font's height with
    // a padding above and below, to sit in the same margin as the text
    // beside them, and the three read better big (2026-09-17).
    {
        // Centre the three as a group, using the SAME gap the layout below
        // actually uses. Measuring with ItemSpacing while laying out with
        // kOrderGap overstated the group by ~12px and shifted it left.
        const float group = row * 3.0f + kOrderGap * 2.0f;
        const float cell = Im::GetContentRegionAvail().x;
        if (cell > group)
            Im::SetCursorPosX(Im::GetCursorPosX() + (cell - group) * 0.5f);
    }

    const auto beginGrey = [dimmed](bool grey) {
        if (dimmed)
            BeginDimmed(grey);
        else
            Im::BeginDisabled(grey);
    };
    const auto endGrey = [dimmed] {
        if (dimmed)
            EndDimmed();
        else
            Im::EndDisabled();
    };

    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    // Arrows from the icon font, like every other glyph on the row.
    beginGrey(index == 0);
    if (GlyphButton("up" + id, row, Glyph::Up))
    {
        moveFrom = static_cast<int>(index);
        moveTo = static_cast<int>(index) - 1;
    }
    endGrey();

    Im::SameLine(0.0f, kOrderGap);
    beginGrey(index + 1 >= count);
    if (GlyphButton("dn" + id, row, Glyph::Down))
    {
        moveFrom = static_cast<int>(index);
        moveTo = static_cast<int>(index) + 1;
    }
    endGrey();

    Im::SameLine(0.0f, kOrderGap);
    if (DeleteButton("rm" + id, row))
        removeAt = static_cast<int>(index);
    Im::PopStyleVar(1);
}

// The drawer an open rule reveals: its actions, one row each in the order
// they are done, each its own menu; and up, down and remove, as the rule
// table's Order column. A plus beneath for one more. Set under the Then
// column -- its left edge on Then's border, its right on the table's --
// with Order the parent's width, so its columns line up with the parent's
// and need no headings of their own. Returns whether the rules changed.
bool DrawActionsDrawer(ft::Rule &rule, std::size_t ruleIndex, const FollowerView &view, ft::Moment moment, float left,
                       float right, float spacing)
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
        Im::TableSetupColumn(Tr("Action"), Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableSetupColumn(Tr("Order"), Im::ImGuiTableColumnFlags_WidthFixed, orderWidth, 0);

        for (std::size_t a = 0; a < rule.actions.size(); ++a)
        {
            const std::string actId = id + "/" + std::to_string(a);
            Im::TableNextRow(0, 0.0f);

            Im::TableSetColumnIndex(0);
            if (ActionMenu(("##act" + actId).c_str(), rule.actions[a], view, moment, nullptr, rule, {},
                           VerdictAt(view, ruleIndex, a)))
                changed = true;

            Im::TableSetColumnIndex(1);
            // Greyed the way a rule's arrows are -- the first action's up,
            // the last one's down -- rather than by ImGui's own disabling.
            // The drawer is drawn inside BeginDimmed, which sets the
            // disabled alpha to 1 so that dimming is done by text colour;
            // a plain BeginDisabled under it fades nothing, so the two
            // arrows that could not move looked exactly like the ones that
            // could.
            OrderButtons(actId, row, a, rule.actions.size(), true, moveFrom, moveTo, removeAt);
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
        Im::SetTooltip("%s", Tr("Click to add action"));

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
//
// A rule is moved by dragging its number: what is carried is which rule of
// which list, and a drop on another list is not taken.
constexpr const char *kRulePayload = "FT_RULE";
struct RuleDrag
{
    ft::ActorId actor{0};
    ft::Moment moment{ft::Moment::Combat};
    std::size_t index{0};
};

// What follows the mouse while a rule is dragged: the row as the table has
// it -- number, NOT, condition, action -- at the table's own column widths,
// under the translucency the drag source pushes.
// The rule table's tick: a square the row's height, centred across the cell
// whose top-left is `pos`, in the text colour of the moment, so a row's
// greying reaches it.
void DrawCellTick(Im::ImVec2 pos)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw)
        return;
    const float size = Im::GetFrameHeight();
    const float leftEdge = pos.x + (Im::GetContentRegionAvail().x - size) * 0.5f;
    DrawGlyph(draw, Glyph::Tick, {leftEdge, pos.y}, {leftEdge + size, pos.y + size},
              Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
}

// A rule's On and NOT: the whole cell the switch, lit while hovered, the
// tick in it while on and none while off. A dead cell, `why` its reason,
// answers nothing and says why on hover, and the caller slashes it once
// the row's height is known; its tick stays, greyed, where `keepTick`
// says the state still holds (a negated condition on a rule set aside).
// -> whether it was clicked.
bool SwitchCell(const std::string &id, bool on, const std::string &why, bool keepTick, const char *turnOff,
                const char *turnOn)
{
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const bool live = why.empty();
    bool clicked = false;
    if (live)
    {
        clicked = CellClicked(("##" + id).c_str(), Im::GetFrameHeight());
        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", on ? turnOff : turnOn);
    }
    else
    {
        Im::Dummy(Im::ImVec2(Im::GetContentRegionAvail().x, Im::GetFrameHeight()));
        if (HoveringLastRect())
            Tooltip(why);
    }
    if (on && (live || keepTick))
    {
        const DimText grey(!live);
        DrawCellTick(pos);
    }
    return clicked;
}

void DrawRulePreview(const ft::Rule &rule, std::size_t index, const FollowerView &view,
                     const std::array<float, 4> &widths)
{
    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_SizingFixedFit;
    if (!Im::BeginTable("##rulepreview", 4, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
        return;
    for (const float width : widths)
        Im::TableSetupColumn("", Im::ImGuiTableColumnFlags_WidthFixed, (std::max)(width, 1.0f), 0);
    Im::TableNextRow(0, 0.0f);
    Im::TableSetColumnIndex(0);
    Im::AlignTextToFramePadding();
    Im::Text("%zu", index + 1);
    Im::TableSetColumnIndex(1);
    if (rule.negated)
        DrawCellTick(Im::GetCursorScreenPos());
    Im::TableSetColumnIndex(2);
    Im::AlignTextToFramePadding();
    Im::TextUnformatted(ConditionText(rule, view).c_str());
    Im::TableSetColumnIndex(3);
    Im::AlignTextToFramePadding();
    Im::TextUnformatted((rule.actions.size() == 1 ? ActionText(rule.actions.front(), view)
                                                  : TrFormat("{} actions", rule.actions.size()))
                            .c_str());
    Im::EndTable();
}

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
    const float onWidth = (std::max)(TextWidth(Tr("On")), row * 0.4f) + gutter;
    const float numWidth = Im::CalcTextSize("99", nullptr, false, -1.0f).x + gutter;
    // Wide enough for the header and the tick and no wider: ImGui adds
    // its own cell padding on both sides of what is asked for here, so
    // only one gutter of slack goes in rather than On's two. Measured
    // off ScreenShot107: with two it came out 54 px against a 46 px
    // row, which reads as a wide gap after the word rather than as a
    // square switch.
    const float notWidth = (std::max)(TextWidth(Tr("NOT")), row * 0.4f) + kCellPadX;
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
        if (!Im::BeginTable(id.c_str(), 6, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
            return false;
        Im::TableSetupColumn(Tr("On"), Im::ImGuiTableColumnFlags_WidthFixed, onWidth, 0);
        Im::TableSetupColumn("#", Im::ImGuiTableColumnFlags_WidthFixed, numWidth, 0);
        // Not comes before the condition, because that is the order it is
        // read in: "not enemy: undead".
        Im::TableSetupColumn(Tr("NOT"), Im::ImGuiTableColumnFlags_WidthFixed, notWidth, 0);
        Im::TableSetupColumn(Tr("Condition"), Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        // Seven parts to the condition's four. An action reads as a phrase
        // and the long ones are long -- "Self: Strongest Fortify Health
        // Regeneration food" -- where a condition is mostly short words and
        // a number. Settled by looking at both ends in play (2026-09-19):
        // at 1.25 the action was cut off with the condition column half
        // empty beside it, and at 2.0 "Self: Weapon charge: needed" was cut
        // off instead. Neither column has room for its longest at once, so
        // this is where the cut falls on the rarer one.
        Im::TableSetupColumn(Tr("Action"), Im::ImGuiTableColumnFlags_WidthStretch, 1.75f, 0);
        Im::TableSetupColumn(Tr("Order"), Im::ImGuiTableColumnFlags_WidthFixed, orderWidth, 0);
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
                Im::SetTooltip("%s", anyOn ? Tr("Click to disable all") : Tr("Click to enable all"));
            Im::SetCursorScreenPos(pos);
            Im::Text("%s", Tr("On"));
            int column = 1;
            for (const char *label : {"#", Tr("NOT"), Tr("Condition"), Tr("Action"), Tr("Order")})
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
    // Where each rule's row begins on screen, and where the list ends: what
    // a dragged rule's drop is placed by. A rule's span runs to the next
    // row, its drawer's actions included.
    std::vector<float> rowTops;
    float listBottom = 0.0f;
    // The columns' left edges on the last row laid out, for the preview a
    // dragged rule shows: # NOT Condition Action Order. Kept across frames,
    // since the row being dragged has not laid out its later cells yet.
    static std::array<float, 5> columnX{};

    for (std::size_t i = 0; i < rules.rules.size(); ++i)
    {
        if (!inTable && !beginPiece())
            break;

        auto &rule = rules.rules[i];
        const std::string rowId = std::to_string(i);
        // A rule whose every action names a thing the follower no longer
        // has -- the potion drunk up, the spell forgotten, the sword sold --
        // or a follower who is away, is set aside: its switch slashed and dead, as an
        // equip cell that does not apply is, the row dimmed, the reason on
        // the switch and on the cell concerned. It keeps its text, its
        // place and its delete; the switch's own state is untouched, so the
        // rule comes back as it was when the thing, or the follower, does.
        const std::string setAside = SetAsideReason(rule, view, i);
        const bool available = setAside.empty();
        Im::TableNextRow(0, 0.0f);
        if (i % 2 == 1)
            Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg0, stripe, -1);

        Im::TableSetColumnIndex(0);
        // The row's top: the cell's content, less the padding pushed above.
        rowTops.push_back(Im::GetCursorScreenPos().y - 2.0f);
        // Off is no tick at all, as an unequipped item's cell on the
        // Inventory tab; the row's dimming says the rest. A rule set aside
        // shows no tick either: its switch's state is not what decides it.
        if (SwitchCell("on" + rowId, rule.enabled, setAside, false, Tr("Click to disable"), Tr("Click to enable")))
        {
            rule.enabled = !rule.enabled;
            changed = true;
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

        // The NOT cell is drawn BEFORE the number, though it sits after
        // it: the number's AlignTextToFramePadding sets the row's text
        // baseline, and ImGui charges a Selectable that offset ON TOP of
        // the height it was asked for (the same trap the several-actions
        // cell documents below). Drawn after the number the switch made
        // every row with a condition five pixels taller than the header
        // (measured, ScreenShot107: 51 px against 46).
        Im::TableSetColumnIndex(2);
        columnX[1] = Im::GetCursorScreenPos().x;
        {
            // A rule's condition is negated by ticking it, and the row then
            // reads "not <condition>". Some conditions cannot be negated
            // (ft::CanNegate), and their cell is dead and says why. On a
            // rule set aside the tick stays, grey with the condition it
            // negates, which such a rule greys though its row is not dimmed.
            const bool can = ft::CanNegate(rule.predicate);
            const std::string why = can ? setAside : std::string(Tr("Condition cannot be negated"));
            if (SwitchCell("not" + rowId, rule.negated, why, can, Tr("Click to remove the NOT"),
                           Tr("Click to negate condition")))
            {
                rule.negated = !rule.negated;
                changed = true;
            }
        }

        // The number is the handle a rule is dragged by: the whole cell, lit
        // while hovered, and live for a rule that is off, as the Order
        // column is -- an off rule is still in the list and can still be
        // moved. So it stands outside the row's dimmed region and greys its
        // number by hand.
        EndDimmed();
        Im::TableSetColumnIndex(1);
        columnX[0] = Im::GetCursorScreenPos().x;
        {
            const Im::ImVec2 pos = Im::GetCursorScreenPos();
            Im::InvisibleButton(("##drag" + rowId).c_str(),
                                Im::ImVec2((std::max)(Im::GetContentRegionAvail().x, 1.0f), Im::GetFrameHeight()), 0);
            if (Im::IsItemHovered(0) || Im::IsItemActive())
                Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, hovered, -1);
            if (Im::IsItemHovered(0) && !Im::GetDragDropPayload())
                Im::SetTooltip("%s", Tr("Click and drag to reorder"));
            Im::PushStyleVar(Im::ImGuiStyleVar_Alpha, 0.6f);
            if (Im::BeginDragDropSource(0))
            {
                const RuleDrag drag{view.id, rules.moment, i};
                Im::SetDragDropPayload(kRulePayload, &drag, sizeof(drag), 0);
                DrawRulePreview(rule, i, view,
                                {columnX[1] - columnX[0], columnX[2] - columnX[1], columnX[3] - columnX[2],
                                 columnX[4] - columnX[3]});
                Im::EndDragDropSource();
            }
            Im::PopStyleVar(1);
            Im::SetCursorScreenPos(pos);
            Im::AlignTextToFramePadding();
            const DimText grey(!available || !rule.enabled);
            Im::Text("%zu", i + 1);
        }
        BeginDimmed(!rule.enabled);

        Im::TableSetColumnIndex(3);
        columnX[2] = Im::GetCursorScreenPos().x;
        if (ConditionCascade(("##cond" + rowId).c_str(), rule, view, rules.moment, setAside))
            changed = true;

        Im::TableSetColumnIndex(4);
        // Where the Then column's border is, for the drawer's own left
        // border to land ON it rather than beside it: the cell's content,
        // less the cell padding this table pushes, less the single pixel
        // the inner border itself occupies.
        //
        // Both numbers are measured, off the panel's own pixels rather than
        // guessed from the style (2026-09-17, ScreenShot106): this cell's
        // content starts at x=978, the parent's Condition/Action divider is
        // drawn at x=971, and kCellPadX is 6. The previous correction took
        // half of ItemSpacing.x instead -- 3 px where the border is 1 -- and
        // put the drawer two pixels left of the line it was meant to sit on.
        const float thenLeft = Im::GetCursorScreenPos().x - kCellPadX - 1.0f;
        columnX[3] = Im::GetCursorScreenPos().x;
        if (rule.actions.empty())
            rule.actions.emplace_back();
        const std::string key = RuleKey(view.id, rules.moment, i);
        bool open = false;
        if (rule.actions.size() == 1)
        {
            // One action: edited here, in its row. Its menu offers a
            // second, and the rule then opens as a drawer.
            bool addAnother = false;
            if (ActionMenu(("##act" + rowId).c_str(), rule.actions.front(), view, rules.moment, &addAnother, rule,
                           setAside, VerdictAt(view, i, 0)))
                changed = true;
            if (addAnother)
            {
                rule.actions.emplace_back();
                g_openRows.Open(key);
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
            open = g_openRows.IsOpen(key, true);
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
                    g_openRows.Open(key);
                else
                    g_openRows.Close(key);
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
            if (const std::size_t unavailable = ft::ActionsNotHad(rule, view.holdings); unavailable > 0)
                Im::TextUnformatted(TrFormat("{} actions ({} unavailable)", rule.actions.size(), unavailable).c_str());
            else
                Im::TextUnformatted(TrFormat("{} actions", rule.actions.size()).c_str());
        }

        EndDimmed();

        // Order is semantics, not decoration: rules are first-match-wins, so
        // moving a row changes which rule shadows which.
        Im::TableSetColumnIndex(5);
        columnX[4] = Im::GetCursorScreenPos().x;
        OrderButtons(rowId, row, i, rules.rules.size(), true, moveFrom, moveTo, removeAt);

        // Back to the switches: every cell is drawn, so the row's height is
        // final and a slash reaches its bottom corner. The On cell's says
        // the rule is set aside; the NOT cell's says this condition is not
        // one that can be negated -- a dead cell reads as an empty one
        // otherwise, since an unticked switch is also empty.
        if (!available)
        {
            Im::TableSetColumnIndex(0);
            SlashCell();
        }
        if (!ft::CanNegate(rule.predicate))
        {
            Im::TableSetColumnIndex(2);
            SlashCell();
        }

        if (!open)
            continue;

        // The drawer: close this piece, draw beneath, reopen for the rest.
        endPiece();
        BeginDimmed(!rule.enabled);
        if (DrawActionsDrawer(rule, i, view, rules.moment, thenLeft, right, spacing))
            changed = true;
        EndDimmed();
        drawerOpen = true;
    }

    if (inTable)
    {
        endPiece();
        listBottom = drawerTop;
    }
    else if (drawerOpen && draw)
    {
        // The drawer was the last thing in the table: close the frame under
        // it by hand, since no piece follows to do so.
        const float bottom = Im::GetCursorScreenPos().y;
        Im::ImDrawListManager::AddLine(draw, {left, drawerTop}, {left, bottom}, border, 1.0f);
        Im::ImDrawListManager::AddLine(draw, {right, drawerTop}, {right, bottom}, border, 1.0f);
        Im::ImDrawListManager::AddLine(draw, {left, bottom}, {right, bottom}, border, 1.0f);
        listBottom = bottom;
    }
    Im::PopStyleVar(2);

    // A rule of this list dragged over it: the line where it would land,
    // above the row whose upper half the mouse is in, and the move on the
    // drop. None where it would land where it is.
    const auto *payload = Im::GetDragDropPayload();
    if (payload && !rowTops.empty() && payload->DataSize == static_cast<int>(sizeof(RuleDrag)) &&
        std::strcmp(payload->DataType, kRulePayload) == 0)
    {
        RuleDrag drag;
        std::memcpy(&drag, payload->Data, sizeof(drag));
        if (drag.actor == view.id && drag.moment == rules.moment && drag.index < rowTops.size())
        {
            const float mouseY = Im::GetMousePos().y;
            std::size_t before = rowTops.size();
            for (std::size_t k = 0; k < rowTops.size(); ++k)
            {
                const float next = k + 1 < rowTops.size() ? rowTops[k + 1] : listBottom;
                if (mouseY < (rowTops[k] + next) * 0.5f)
                {
                    before = k;
                    break;
                }
            }
            const std::size_t to = ft::DroppedAt(drag.index, before);
            const Im::ImRect list{{left, rowTops.front()}, {right, listBottom}};
            if (Im::BeginDragDropTargetCustom(list,
                                              Im::GetID(("##ruledrop" + RuleKey(view.id, rules.moment, 0)).c_str())))
            {
                if (to != drag.index && draw)
                {
                    // The theme's colour for a divider being dragged, which
                    // this line is: a theme sets it to its accent, where
                    // DragDropTarget is left at ImGui's own yellow by most.
                    const float y = before < rowTops.size() ? rowTops[before] : listBottom;
                    Im::ImDrawListManager::AddLine(draw, {left, y}, {right, y},
                                                   Im::GetColorU32(Im::ImGuiCol_SeparatorActive, 1.0f), 3.0f);
                }
                if (Im::AcceptDragDropPayload(kRulePayload, Im::ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                {
                    moveFrom = static_cast<int>(drag.index);
                    moveTo = static_cast<int>(to);
                }
                Im::EndDragDropTarget();
            }
        }
    }

    // Applied after the loop: mutating the vector mid-iteration would invalidate
    // the reference the current row still holds. A drag's move may be a
    // long one, the rows between shifting a place; an arrow's is a swap.
    if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(rules.rules.size()) && moveFrom != moveTo)
    {
        ft::MoveItem(rules.rules, static_cast<std::size_t>(moveFrom), static_cast<std::size_t>(moveTo));
        MoveOpenState(view.id, rules.moment, static_cast<std::size_t>(moveFrom), static_cast<std::size_t>(moveTo));
        changed = true;
    }
    if (removeAt >= 0)
    {
        RemoveOpenState(view.id, rules.moment, static_cast<std::size_t>(removeAt), rules.rules.size());
        rules.rules.erase(rules.rules.begin() + removeAt);
        changed = true;
    }
    Im::Spacing();
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool addClicked = GlyphButton("addrule", Im::GetFrameHeight(), Glyph::Plus);
    Im::PopStyleVar(1);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("%s", Tr("Click to add tactic"));

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

} // namespace

void DrawTactics(const ft::RuleSet &rules, const FollowerView &view)
{
    Im::Spacing();

    // This list's switch -- the combat list's and the idle list's are each
    // their own. The UI runs on the render thread and the tick on the game
    // thread, so the setter takes a lock rather than writing shared state
    // directly.
    //
    // Read LIVE, not from the view. The view is rebuilt by the tick, and the
    // tick is held while this panel has the clock frozen -- so a copy taken
    // from it showed the old state until the panel closed and a tick ran.
    // The global switch on the Settings page never had this problem because
    // it reads its flag directly; this now does the same.
    const bool followerEnabled = IsFollowerEnabled(view.id, rules.moment);
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
    const bool idle = rules.moment == ft::Moment::Idle;
    if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Im::SetTooltip("%s", !all ? Tr("Tactics are turned off for the party in Settings")
                             : followerEnabled
                                 ? (idle ? Tr("Click to turn off idle tactics") : Tr("Click to turn off tactics"))
                                 : (idle ? Tr("Click to turn on idle tactics") : Tr("Click to turn on tactics")));
    if (toggled && all)
        SetFollowerEnabled(view.id, rules.moment, !followerEnabled);
    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    Im::Text("%s", Tr("Enabled"));
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
    // visible to the tick. The page is built again after: what each rule
    // could do is judged at the build and read by the rule's place, so a
    // rule added, moved, removed or changed read another's verdict until
    // then -- a Use power rule read "Unsupported", the verdict of the new
    // rule with no action that had been in its place (2026-09-23).
    ft::RuleSet editable = rules;
    if (DrawRuleTable(editable, view))
    {
        SetRules(view.id, std::move(editable));
        RefreshAfterAction();
    }
    EndDimmed();
}

} // namespace ft::game::ui
