// The Magic and Shouts tabs: the lists and a spell's detail.

#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/UI.h"
#include "game/ui/Widgets.h"

#include "core/Breakdown.h"
#include "core/I18n.h"
#include "core/Rows.h"
#include "game/Pins.h"
#include "game/Tactics.h"
#include "progression/game/Service.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

// The magic lists, in two tabs: Magic is All and the five schools, and
// Shouts is All, Shouts and Powers -- what rides the one voice slot rather
// than a hand, which is a different list with different columns, not a
// school. One scan (view.magic) feeds both, and VoiceEntry says which tab a
// row is on.
//
// One of those two lists, as everything that draws one is told it: which
// half of view.magic it is, the state this follower keeps for it, and the
// chip and filter it shares with every other page. Built by MagicListFor,
// so "which list is this" is answered once and then carried, rather than
// re-derived from a bool at each call.
struct MagicList
{
    Tab home; // Magic, or Shouts for the voice list
    bool voice;
    MagicTabState &state;
    ListView &shared;
};

MagicList MagicListFor(ft::ActorId id, Tab home)
{
    PanelState &panel = Panel(id);
    return home == Tab::Shouts ? MagicList{Tab::Shouts, true, panel.shouts, g_shoutList}
                               : MagicList{Tab::Magic, false, panel.magic, g_magicList};
}

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

// What the count above a list counts: "25 shouts", "25 powers", both
// together on the Shouts tab's All, and spells on every Magic list.
const char *MagicNoun(int category, bool voice)
{
    if (category == static_cast<int>(MagicCategory::Shouts))
        return Tr("shouts");
    if (category == static_cast<int>(MagicCategory::Powers))
        return Tr("powers");
    return voice ? Tr("shouts and powers") : Tr("spells");
}

// Is the entry on this tab's list: on the tab at all, in its category, and
// with the filter's text in a cell the list shows for it.
bool MagicShown(const MagicEntry &entry, const MagicList &list)
{
    return ft::MagicShown(entry, list.state.category, list.voice, list.shared.filter);
}

std::vector<const MagicEntry *> VisibleMagic(const CharacterView &view, const MagicList &list)
{
    std::vector<const MagicEntry *> rows;
    for (const auto &entry : view.magic)
    {
        if (MagicShown(entry, list))
            rows.push_back(&entry);
    }

    SortRows(rows, ft::CompareMagic);
    return rows;
}

void DrawMagicList(const CharacterView &view, const MagicList &list)
{
    MagicTabState &state = list.state;
    const bool voice = list.voice;

    Im::Spacing();
    {
        std::array<int, static_cast<std::size_t>(MagicCategory::COUNT)> counts{};
        for (const auto &entry : view.magic)
        {
            if (VoiceEntry(entry) == voice)
                ++counts[static_cast<std::size_t>(entry.category)];
        }
        state.category = DrawCategoryChips(
            counts, kIconMagicAll, list.shared,
            [](std::size_t i) { return DisplayName(static_cast<MagicCategory>(i)); },
            [](std::size_t i) { return IconFor(static_cast<MagicCategory>(i)); });
    }
    Im::Spacing();

    // Counted against the category, as the Inventory tab counts: on
    // Destruction, "3 of 3" until the filter box takes some away.
    std::size_t inCategory = 0;
    for (const auto &entry : view.magic)
    {
        if (VoiceEntry(entry) == voice && (state.category < 0 || static_cast<int>(entry.category) == state.category))
            ++inCategory;
    }
    // Ban-all on a school's list, not All's, which has no equip cells, and
    // not the player's: no ban there.
    const BanAll banMagic{[&] {
                              std::vector<std::pair<WearTarget, bool>> rows;
                              for (const auto &entry : view.magic)
                                  if (MagicShown(entry, list))
                                      rows.push_back({{entry.form, std::nullopt}, entry.banned});
                              return rows;
                          },
                          view.id};
    const char *filterId = voice ? "##shoutfilter" : "##magicfilter";
    FilterRow(
        filterId, list.shared.filter, sizeof(list.shared.filter),
        [&] {
            return static_cast<std::size_t>(
                std::count_if(view.magic.begin(), view.magic.end(),
                              [&](const MagicEntry &entry) { return MagicShown(entry, list); }));
        },
        inCategory, MagicNoun(state.category, voice),
        state.category >= 0 && !view.player ? std::function<void()>([&] { BanAllButton(filterId, banMagic); })
                                            : nullptr);
    Im::Spacing();

    // Which columns. Only the Magic tab's All list has a School column;
    // every list has the Type. Spells show a cell per hand; powers and
    // shouts, which are selected rather than held, show one Equipped cell
    // for the voice slot, clicked like a hand cell: ready it, pin it, put
    // it away. One voice pin sets every other power and shout aside, as a
    // pinned quiver does the arrows.
    constexpr auto flags = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_Sortable;
    const float gutter = kCellPadX * 2.0f;
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    float schoolWidth = TextWidth(Tr("School")) + arrow;
    float typeWidth = TextWidth(Tr("Type")) + arrow;
    float levelWidth = TextWidth(Tr("Level")) + arrow;
    float castWidth = TextWidth(Tr("Cast")) + arrow;
    float costWidth = TextWidth(Tr("Cost")) + arrow;
    for (const auto &entry : view.magic)
    {
        schoolWidth = (std::max)(schoolWidth, TextWidth(entry.school));
        typeWidth = (std::max)(typeWidth, TextWidth(entry.type));
        levelWidth = (std::max)(levelWidth, TextWidth(entry.level));
        castWidth = (std::max)(castWidth, TextWidth(entry.cast));
        costWidth = (std::max)(costWidth, TextWidth(entry.cost));
    }
    const float magnitudeWidth = (std::max)(TextWidth(Tr("Mag")) + arrow, TextWidth("999")) + gutter;
    const float handWidth = (std::max)(TextWidth(Tr("Right")) + arrow, Im::GetFontSize() * 2.0f) + gutter;
    const float wornWidth = (std::max)(TextWidth(Tr("Equipped")) + arrow, Im::GetFontSize()) + gutter;

    // No equip columns on the Magic tab's All, as the Inventory tab has it:
    // equipping is done from the school lists. The Shouts tab's All is
    // shouts and powers together, readied the same way as each is on its
    // own, so it keeps the Equipped cell.
    const bool allList = !voice && state.category < 0;
    // All: Name, School, Type, Level, Mag, Cost, Cast. A school's list: the
    // same less School, plus the two hand cells. The voice list has no
    // school, level or cost to show: Name, Type, Mag, Cast, Equipped.
    const int columnCount = voice ? 5 : allList ? 7 : 8;

    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("magic", columnCount, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn(Tr("Name"), Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort,
                         1.0f, static_cast<Im::ImGuiID>(Column::Name));
    if (allList)
        Im::TableSetupColumn(Tr("School"), Im::ImGuiTableColumnFlags_WidthFixed, schoolWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::School));
    Im::TableSetupColumn(Tr("Type"), Im::ImGuiTableColumnFlags_WidthFixed, typeWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Type));
    if (!voice)
        Im::TableSetupColumn(Tr("Level"), Im::ImGuiTableColumnFlags_WidthFixed, levelWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::Level));
    Im::TableSetupColumn(Tr("Mag"),
                         Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                         magnitudeWidth, static_cast<Im::ImGuiID>(Column::Magnitude));
    if (!voice)
        Im::TableSetupColumn(Tr("Cost"), Im::ImGuiTableColumnFlags_WidthFixed, costWidth + gutter,
                             static_cast<Im::ImGuiID>(Column::Cost));
    // Cast: what it does when cast -- Self, Touch, Spray, Projectile, Target,
    // Location -- delivery and casting type in one word.
    Im::TableSetupColumn(Tr("Cast"), Im::ImGuiTableColumnFlags_WidthFixed, castWidth + gutter,
                         static_cast<Im::ImGuiID>(Column::Cast));
    if (allList)
    {
        // no equip columns
    }
    else if (voice)
    {
        Im::TableSetupColumn(Tr("Equipped"), Im::ImGuiTableColumnFlags_WidthFixed, wornWidth,
                             static_cast<Im::ImGuiID>(Column::Equipped));
    }
    else
    {
        Im::TableSetupColumn(Tr("Left"), Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Left));
        Im::TableSetupColumn(Tr("Right"), Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Right));
    }
    Im::TableHeadersRow();

    const std::vector<const MagicEntry *> rows = VisibleMagic(view, list);
    for (const MagicEntry *entry : rows)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "##magic%08X", entry->form);

        Im::TableNextRow(0, 0.0f);
        // Kept from the AI -- a pin holds a hand it would take -- or above their
        // skill, so the AI would not choose it: the whole row is drawn in
        // the disabled colour, ticks included, since every glyph takes the
        // text colour.
        const bool dim = (Dimmed(*entry) || entry->banned) && !allList;
        const DimText grey(dim);
        Im::TableSetColumnIndex(0);
        Im::ImVec2 pos = Im::GetCursorScreenPos();
        if (CellClicked(buf))
        {
            state.detail = entry->form;
            state.openedFrom = list.home; // back to the list, wherever the last page was opened from
        }
        // Why the row is dimmed, over the whole cell: asked of the
        // Selectable, before the name is drawn over it. A spell above the
        // follower's skill says so first, even when a pin shadows it too:
        // the skill is the reason nothing about the row can change, the
        // pin only the reason for now.
        if (dim && entry->aboveSkill && Im::IsItemHovered(0))
        {
            NeedsAndHasTooltip(entry->needsSchool, entry->needsLevel, entry->hasLevel);
        }
        else if (dim && entry->locked && Im::IsItemHovered(0))
            Im::SetTooltip("%s", Tr("No word unlocked"));
        else if (dim && entry->banned && Im::IsItemHovered(0))
            Im::SetTooltip("%s", Tr("Banned"));
        else if (dim && entry->setAside && Im::IsItemHovered(0))
            Tooltip(entry->asideBy);
        Im::SetCursorScreenPos(pos);
        Im::Text("%s", entry->name.c_str());

        if (allList)
        {
            Im::TableNextColumn();
            Im::Text("%s", entry->school.c_str());
        }
        Im::TableNextColumn();
        Im::Text("%s", entry->type.c_str());
        if (!voice)
        {
            Im::TableNextColumn();
            Im::Text("%s", entry->level.c_str());
        }
        Im::TableNextColumn();
        if (entry->magnitude > 0.0f)
        {
            char num[32];
            std::snprintf(num, sizeof(num), "%.0f", entry->magnitude);
            TextRightInCell(num);
        }
        if (!voice)
        {
            Im::TableNextColumn();
            TextRightInCell(entry->cost);
            // What the follower pays and why, on the number.
            if (!entry->costBreakdown.empty() && Im::IsItemHovered(0))
                BreakdownTooltip(entry->costBreakdown);
        }
        Im::TableNextColumn();
        Im::Text("%s", entry->cast.c_str());

        if (allList)
        {
            // no equip cells
        }
        else if (voice)
        {
            std::snprintf(buf, sizeof(buf), "##voice%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view, entry->form, VoiceCell(*entry), Hand::None, true);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "##left%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view, entry->form, LeftCell(*entry), Hand::Left, true);
            std::snprintf(buf, sizeof(buf), "##right%08X", entry->form);
            Im::TableNextColumn();
            OnCell(buf, view, entry->form, RightCell(*entry), Hand::Right, true);
        }
    }
    Im::EndTable();
    Im::PopStyleVar(1);
}

// A spell a follower Progression levels knows has Forget at the right
// (SpellControlsFor), asked once: the engine is told they do not know it,
// so it leaves this list, the rules that name it, and their hands; a tome
// of it brings it back. Spells only: the Shouts tab's powers and shouts
// are not taught, so not forgotten.
void DrawMagicDetail(const CharacterView &view, const MagicEntry &entry, PanelState &panel, MagicTabState &state,
                     Tab home)
{
    Im::Spacing();
    const float lineRight = Im::GetCursorPosX() + Im::GetContentRegionAvail().x;
    if (BackButton())
        CloseDetail(panel, state, home);
    DetailName(entry.name);
    DetailSubtitle(entry.school);
    if (const auto controls = home == Tab::Magic && !view.player ? fp::game::SpellControlsFor(view.id) : std::nullopt)
    {
        const fp::SpellButton forget =
            controls->active ? fp::ForgetButton(entry.name) : fp::SpellButton{false, controls->why};
        bool asking = state.confirming == entry.form;
        if (AskedActionAtRight(Tr("Forget"), forget.can, forget.hover, asking, lineRight))
        {
            fp::game::ForgetSpellByForm(view.id, entry.form);
            PlayGameSound(kSpellForgottenSound);
            CloseDetail(panel, state, home, false);
            RefreshAfterAction();
            return;
        }
        state.confirming = asking ? entry.form : 0;
    }

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
            N_("Name"), N_("Effect"), WithDescription());
    }
    if (!entry.description.empty())
    {
        CentredHeading(Tr("Description"));
        Im::TextWrapped("%s", entry.description.c_str());
        Im::Spacing();
    }
}

// The Magic tab and the Shouts tab: one list drawn twice, `home` saying
// which half of view.magic it is.
void DrawMagicPage(const CharacterView &view, const MagicList &list)
{
    MagicTabState &state = list.state;
    const bool voice = list.voice;

    if (const auto *entry = OpenRow(view.magic, state.detail))
    {
        DrawMagicDetail(view, *entry, Panel(view.id), state, list.home);
        return;
    }

    const bool any = std::any_of(view.magic.begin(), view.magic.end(),
                                 [voice](const MagicEntry &entry) { return VoiceEntry(entry) == voice; });
    if (!any)
    {
        Im::Spacing();
        Im::TextDisabled("%s", voice ? Tr("Knows no shouts or powers.") : Tr("Knows no spells."));
        return;
    }
    DrawMagicList(view, list);
}

} // namespace

Tab MagicPageOf(const CharacterView &view, std::uint32_t form)
{
    for (const auto &entry : view.magic)
    {
        if (entry.form == form)
            return VoiceEntry(entry) ? Tab::Shouts : Tab::Magic;
    }
    return Tab::None;
}

MagicTabState &MagicPageState(ft::ActorId id, Tab page)
{
    PanelState &panel = Panel(id);
    return page == Tab::Shouts ? panel.shouts : panel.magic;
}

void DrawMagic(const CharacterView &view)
{
    DrawMagicPage(view, MagicListFor(view.id, Tab::Magic));
}

void DrawShouts(const CharacterView &view)
{
    DrawMagicPage(view, MagicListFor(view.id, Tab::Shouts));
}

} // namespace ft::game::ui
