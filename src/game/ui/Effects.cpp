// The Effects tab: what is on them, and where it comes from.

#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/UI.h"
#include "game/ui/Widgets.h"

#include "core/I18n.h"
#include "core/Rows.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ft::game::ui
{
namespace
{

// The Effects tab: what is running on the follower, as the game's own
// Active Effects list shows it, with what is left of each and where it
// comes from. Read on the tick, so with the clock frozen behind the panel
// the times stand still, as they do in the game's own menu. A name opens
// the effect's page, as on the Inventory and Magic tabs.
bool EffectListed(const EffectRow &row)
{
    return ft::EffectListed(row, g_showAllEffects);
}

// Is the row listed, and does it hold the filter's text in a cell the
// table shows?
bool EffectShown(const EffectRow &row)
{
    return ft::EffectShown(row, g_showAllEffects, g_effectsFilter);
}

// The switch beside the filter, naming the list shown: Active or All. As
// wide as the wider word, so it does not move the count beside it when
// clicked.
void ActiveEffectsButton()
{
    const char *active = Tr("Active");
    const char *all = Tr("All");
    const float width = WidestLabel({active, all}) + Im::GetStyle()->FramePadding.x * 2.0f;
    const std::string label = std::string(g_showAllEffects ? all : active) + "##effectsactive";
    if (Im::Button(label.c_str(), Im::ImVec2(width, 0.0f)))
        g_showAllEffects = !g_showAllEffects;
    if (Im::IsItemHovered(0))
        Im::SetTooltip("%s", g_showAllEffects ? Tr("Click to show active effects") : Tr("Click to show all effects"));
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

    SortRows(rows, ft::CompareEffects);
    return rows;
}

// The page an effect's source opens, if it has one: the worn piece on the
// Inventory tab, the spell on the Magic tab, the power or shout on the
// Shouts tab; None for a source with no page of its own -- a racial
// ability, a potion drunk up. The tabs' own lists are the rule for what has
// a page.
Tab SourcePage(const CharacterView &view, std::uint32_t form)
{
    if (form == 0)
        return Tab::None;
    if (std::any_of(view.inventory.begin(), view.inventory.end(),
                    [form](const InventoryItem &item) { return item.form == form; }))
        return Tab::Inventory;
    return MagicPageOf(view, form);
}

// Open it, with the back arrow returning to the Effects tab.
void OpenSourcePage(const CharacterView &view, std::uint32_t form)
{
    PanelState &panel = Panel(view.id);
    const Tab page = SourcePage(view, form);
    switch (page)
    {
    case Tab::Inventory:
        panel.inventory.detail = ItemPageOf(view, form);
        panel.inventory.openedFrom = Tab::Effects;
        panel.select = Tab::Inventory;
        break;
    case Tab::Magic:
    case Tab::Shouts: {
        MagicTabState &magic = MagicPageState(view.id, page);
        magic.detail = form;
        magic.openedFrom = Tab::Effects;
        panel.select = page;
        break;
    }
    default:
        break;
    }
}

void DrawEffectDetail(const EffectRow &row, EffectsTabState &state, const CharacterView &view)
{
    Im::Spacing();
    if (BackButton())
        state = {};
    DetailName(row.name);

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
    columns.push_back({N_("Source"), [](const SheetRow &r) { return r.link; }, false, true});
    DrawSections(
        sections, true, [&view](std::uint32_t form) { OpenSourcePage(view, form); }, nullptr,
        [](const SheetRow &entry, const std::string &key, float left, float right) {
            DrawConditionDrawer(entry, key, left, right);
        },
        N_("Name"), N_("Effect"), columns);

    if (!row.description.empty())
    {
        CentredHeading(Tr("Description"));
        Im::TextWrapped("%s", row.description.c_str());
        Im::Spacing();
    }
}

} // namespace

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

void DrawEffects(const CharacterView &view)
{
    auto &state = Panel(view.id).effects;
    if (const auto *row = OpenRow(view.effects, state.detail))
    {
        DrawEffectDetail(*row, state, view);
        return;
    }

    Im::Spacing();
    const auto listed = static_cast<std::size_t>(std::count_if(view.effects.begin(), view.effects.end(), EffectListed));
    FilterRow(
        "##effectsfilter", g_effectsFilter, sizeof(g_effectsFilter),
        [&] { return static_cast<std::size_t>(std::count_if(view.effects.begin(), view.effects.end(), EffectShown)); },
        listed, Tr("effects"), ActiveEffectsButton);
    Im::Spacing();

    if (listed == 0)
    {
        Im::SetCursorPosX(Im::GetCursorPosX() + kCellPadX);
        Im::TextDisabled("%s", Tr("No active effects."));
        return;
    }

    const float gutter = kCellPadX * 2.0f;
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    float magnitudeWidth = TextWidth(Tr("Magnitude")) + arrow;
    float remainingWidth = TextWidth(Tr("Remaining")) + arrow;
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
    Im::TableSetupColumn(Tr("Effect"), Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort,
                         1.0f, static_cast<Im::ImGuiID>(Column::Name));
    Im::TableSetupColumn(Tr("Magnitude"),
                         Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                         magnitudeWidth + gutter, static_cast<Im::ImGuiID>(Column::Magnitude));
    Im::TableSetupColumn(Tr("Remaining"), Im::ImGuiTableColumnFlags_WidthFixed, remainingWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Remaining));
    Im::TableSetupColumn(Tr("Source"), Im::ImGuiTableColumnFlags_WidthStretch, 1.0f,
                         static_cast<Im::ImGuiID>(Column::Source));
    Im::TableHeadersRow();

    for (const EffectRow *row : VisibleEffects(view))
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "##effect%p_%08X", row->token, row->form);

        Im::TableNextRow(0, 0.0f);
        // Running but changing nothing for this follower, or running but
        // not acting, its conditions unmet: the row is drawn in the
        // disabled colour, and its name hovers as which; only All lists
        // them. One the game's own list hides is running and applied all
        // the same, and reads as any other; its page says it is hidden.
        const DimText grey(!row->applied || !row->active);
        Im::TableSetColumnIndex(0);
        const Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
            state.detail = row->Key();
        if (!row->applied && Im::IsItemHovered(0))
            Im::SetTooltip("%s", Tr("Not applied"));
        else if (!row->active && Im::IsItemHovered(0))
            Im::SetTooltip("%s", Tr("Inactive"));
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

} // namespace ft::game::ui
