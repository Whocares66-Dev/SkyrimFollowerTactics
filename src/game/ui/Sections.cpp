// The sheet's tables (Sections.h).

#include "game/ui/Sections.h"
#include "game/ui/Panel.h"
#include "game/ui/Widgets.h"

#include "core/Breakdown.h"
#include "core/I18n.h"
#include "core/OpenRows.h"
#include "game/Sheet.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace ft::game::ui
{
namespace
{

// A table of conditions: the call, the comparison, a tick where met.
void DrawConditionTable(const std::string &id, const std::vector<SheetRow> &rows, float width)
{
    const char *conditionLabel = Tr("Condition");
    const char *valueLabel = Tr("Value");
    const char *metLabel = Tr("Met");
    float callWidth = TextWidth(conditionLabel);
    float valueWidth = TextWidth(valueLabel);
    for (const auto &row : rows)
    {
        callWidth = (std::max)(callWidth, TextWidth(row.label));
        valueWidth = (std::max)(valueWidth, TextWidth(row.value));
    }
    const float pad = kTablePad;
    const auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    if (!Im::BeginTable(id.c_str(), 3, flags, Im::ImVec2(width, 0.0f), 0.0f))
        return;
    Im::TableSetupColumn(conditionLabel, Im::ImGuiTableColumnFlags_WidthFixed, callWidth + pad, 0);
    Im::TableSetupColumn(valueLabel, Im::ImGuiTableColumnFlags_WidthFixed, valueWidth + pad, 0);
    Im::TableSetupColumn(metLabel, Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    PlainHeaderRow({conditionLabel, valueLabel, metLabel});
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

void DrawPerkDrawer(const SheetRow &row, float left, float right, const std::function<void(std::uint32_t)> &onLink = {})
{
    DrawDrawer(left, right, [&](float width) { DrawPerkTable("perks##" + row.label, row.detail, width, onLink); });
}

const ExtraColumn kDescriptionColumn{N_("Description"), [](const SheetRow &r) { return r.description; }, false, false,
                                     true};

} // namespace

void DrawPerkTable(const std::string &id, const std::vector<SheetRow> &perks, float width,
                   const std::function<void(std::uint32_t)> &onLink)
{
    const char *perkLabel = Tr("Perk");
    const char *rankLabel = Tr("Rank");
    const char *descriptionLabel = Tr("Description");
    float nameWidth = TextWidth(perkLabel);
    float rankWidth = TextWidth(rankLabel);
    for (const auto &sub : perks)
    {
        nameWidth = (std::max)(nameWidth, TextWidth(sub.label));
        rankWidth = (std::max)(rankWidth, TextWidth(sub.value));
    }
    const float pad = kTablePad;

    const auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;
    if (!Im::BeginTable(id.c_str(), 3, flags, Im::ImVec2(width, 0.0f), 0.0f))
        return;
    Im::TableSetupColumn(perkLabel, Im::ImGuiTableColumnFlags_WidthFixed, nameWidth + pad, 0);
    Im::TableSetupColumn(rankLabel, Im::ImGuiTableColumnFlags_WidthFixed, rankWidth + pad, 0);
    Im::TableSetupColumn(descriptionLabel, Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    PlainHeaderRow({perkLabel, rankLabel, descriptionLabel});
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
                Tooltip(sub.aside);
            Im::SetCursorScreenPos(pos);
        }
        Im::Text("%s", sub.label.c_str());
        if (aside && !(sub.form != 0 && onLink) && Im::IsItemHovered(0))
            Tooltip(sub.aside);
        Im::TableSetColumnIndex(1);
        Im::Text("%s", sub.value.c_str());
        Im::TableSetColumnIndex(2);
        Im::TextWrapped("%s", sub.modifiers.c_str());
    }
    Im::EndTable();
}

void DrawConditionDrawer(const SheetRow &row, const std::string &key, float left, float right)
{
    DrawDrawer(left, right, [&](float width) { DrawConditionTable("conditions##" + key, row.detail, width); });
}

std::vector<ExtraColumn> WithDescription()
{
    std::vector<ExtraColumn> columns = kEffectColumns;
    columns.push_back(kDescriptionColumn);
    return columns;
}

void DrawSections(const std::vector<SheetSection> &sections, bool modifiers,
                  const std::function<void(std::uint32_t)> &onLink, const char *third, const RowDrawer &drawer,
                  const char *first, const char *second, const std::vector<ExtraColumn> &wanted,
                  const std::function<void(const SheetRow &)> &onTree)
{
    first = Tr(first);
    second = Tr(second);
    if (third)
        third = Tr(third);
    std::vector<ExtraColumn> extras;
    for (const auto &column : wanted)
    {
        const bool any = std::any_of(sections.begin(), sections.end(), [&](const SheetSection &section) {
            return std::any_of(section.rows.begin(), section.rows.end(),
                               [&](const SheetRow &row) { return !column.text(row).empty(); });
        });
        if (any)
        {
            extras.push_back(column);
            extras.back().heading = Tr(column.heading);
        }
    }
    const bool hasThird = modifiers && third != nullptr;
    // The Skills tab (`onTree`): the caret that opens a row's drawer sits
    // before the level, and the level's cell opens it -- so the name is free
    // to be the link to the skill's page. Every level starts a caret's width
    // in, so the numbers line up whether a row has perks or not.
    const bool levelOpens = modifiers && static_cast<bool>(onTree);

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
            const float lead = row.detail.empty() || levelOpens ? 0.0f : marker;
            nameWidth = (std::max)(nameWidth, lead + TextWidth(row.label));
            valueWidth = (std::max)(valueWidth, (levelOpens ? marker : 0.0f) + TextWidth(row.value));
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
        // A section set aside is greyed whole, its label and its rows, from
        // here to the end of the section; the heading above it is not, since
        // a group's other sections count. Live throughout, as a row set
        // aside is: the drawers still open.
        const DimText greySection(!section.aside.empty());
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
            const DimText grey(!row.aside.empty() || row.needsLevel > 0);
            bool open = false;
            if (levelOpens)
            {
                // The name's cell opens the skill's page, lit under the
                // cursor as a link is; the level's, below, the drawer.
                const std::string key = section.title + "/" + row.label + "#" + std::to_string(rowIndex);
                Im::TableSetColumnIndex(0);
                if (row.tree != 0)
                {
                    const Im::ImVec2 pos = Im::GetCursorScreenPos();
                    if (CellClicked(("##tree" + key).c_str()))
                        onTree(row);
                    Im::SetCursorScreenPos(pos);
                }
                Im::Text("%s", row.label.c_str());
            }
            else if (row.detail.empty())
            {
                Im::TableSetColumnIndex(0);
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
                Im::TableSetColumnIndex(0);
                // A row that opens is a selectable spanning every column, so
                // the whole row is the click target -- but drawn INVISIBLE,
                // and the highlight painted through the table's own row
                // background instead. The selectable's own highlight is the
                // size of its label, which is what left the far end of the
                // row unlit; the row background is the rect ImGui derived
                // for the row, padding and all, so it fits by construction.
                // The marker and the name are then drawn over it.
                const std::string key = section.title + "/" + row.label + "#" + std::to_string(rowIndex);
                open = g_openRows.IsOpen(key, false);

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
                        g_openRows.Open(key);
                    else
                        g_openRows.Close(key);
                }
                if (Im::IsItemHovered(0))
                    Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg1, hovered, -1);
                else if (open)
                    Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg1, opened, -1);

                DrawDisclosure(pos, open);
                Im::SetCursorScreenPos(Im::ImVec2(pos.x + marker, pos.y));
                Im::Text("%s", row.label.c_str());
            }
            if ((!row.aside.empty() || row.needsLevel > 0) && Im::IsItemHovered(0))
            {
                if (row.needsLevel > 0)
                    NeedsAndHasTooltip(row.school, row.needsLevel, row.hasLevel, row.aside);
                else
                {
                    Tooltip(row.aside);
                }
            }

            Im::TableSetColumnIndex(1);
            if (levelOpens)
            {
                // The level's cell opens the perks it holds, the caret before
                // the number saying so; a row with none has the caret's room
                // and no caret.
                const std::string key = section.title + "/" + row.label + "#" + std::to_string(rowIndex);
                const Im::ImVec2 pos = Im::GetCursorScreenPos();
                if (!row.detail.empty())
                {
                    open = g_openRows.IsOpen(key, false);
                    if (CellClicked(("##open" + key).c_str()))
                    {
                        open = !open;
                        if (open)
                            g_openRows.Open(key);
                        else
                            g_openRows.Close(key);
                    }
                    if (open)
                        Im::TableSetBgColor(Im::ImGuiTableBgTarget_RowBg1, opened, -1);
                    DrawDisclosure(pos, open);
                }
                Im::SetCursorScreenPos(Im::ImVec2(pos.x + marker, pos.y));
            }
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
            // the number it explains; on the Modifiers cell where that cell
            // holds one plain figure, since that is the number it explains
            // there. Where the figures hover apart, or there is none, the
            // row's is the value's: a skill's level beside its bonuses.
            const auto explain = [&] {
                if (!Im::IsItemHovered(0))
                    return;
                if (!row.breakdown.empty())
                    BreakdownTooltip(row.breakdown);
                else if (!row.note.empty())
                    NoteTooltip(row.note);
            };
            const bool modifierExplains =
                hasThird && row.mark == 0 && row.modifierParts.empty() && !row.modifiers.empty();
            if (!modifiers || (hasThird && !modifierExplains))
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
                        if (modifierExplains)
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

} // namespace ft::game::ui
