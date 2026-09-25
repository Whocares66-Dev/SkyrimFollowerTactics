// The Inventory tab: the item list and an item's detail.

#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/UI.h"
#include "game/ui/Widgets.h"

#include "core/I18n.h"
#include "core/Rows.h"
#include "game/Actions.h"
#include "game/Pins.h"
#include "game/Tactics.h"
#include "progression/game/Service.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

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

void DrawCategoryRow(const CharacterView &view, InventoryTabState &state)
{
    std::array<int, static_cast<std::size_t>(ItemCategory::COUNT)> counts{};
    for (const auto &item : view.inventory)
        ++counts[static_cast<std::size_t>(item.category)];

    state.category = DrawCategoryChips(
        counts, kIconAll, g_inventoryList, [](std::size_t i) { return DisplayName(static_cast<ItemCategory>(i)); },
        [](std::size_t i) { return IconFor(static_cast<ItemCategory>(i)); });
}

// Which columns a list shows, which cells the filter searches and whether
// a row is on the list at all are core's (core/Rows.h, tested).
ItemColumns ColumnsOf(const InventoryTabState &state)
{
    return ft::ColumnsOf(state.category);
}

bool ItemShown(const InventoryItem &item, const InventoryTabState &state)
{
    return ft::ItemShown(item, state.category, g_inventoryList.filter);
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

    SortRows(rows, ft::CompareItems);
    return rows;
}

// The list: a column for each thing worth comparing, sortable by clicking a
// heading, one row per kind of item with the count in brackets. Clicking a
// row opens it.
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
    // Ban-all on the lists a ban means something on, Weapons, Armor and
    // Arrows, and not the player's: no ban there.
    const bool bannable = !view.player && (state.category == static_cast<int>(ItemCategory::Weapons) ||
                                           state.category == static_cast<int>(ItemCategory::Armor) ||
                                           state.category == static_cast<int>(ItemCategory::Arrows));
    const BanAll banItems{[&] {
                              std::vector<std::pair<WearTarget, bool>> rows;
                              for (const auto &item : view.inventory)
                                  if (ItemShown(item, state) && item.equipable)
                                      rows.push_back({{item.form, item.variant}, item.banned});
                              return rows;
                          },
                          view.id};
    FilterRow(
        "##invfilter", g_inventoryList.filter, sizeof(g_inventoryList.filter),
        [&] {
            return static_cast<std::size_t>(
                std::count_if(view.inventory.begin(), view.inventory.end(),
                              [&](const InventoryItem &item) { return ItemShown(item, state); }));
        },
        inCategory, Tr("items"),
        bannable ? std::function<void()>([&] { BanAllButton("##invfilter", banItems); }) : nullptr);
    Im::Spacing();

    // Which columns this list has. A stat column only where the stat means
    // something -- damage for weapons, rating for armour -- and an Equipped
    // column only where something can be equipped.
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
    float typeWidth = TextWidth(consumables ? Tr("Effect") : Tr("Type"));
    for (const auto &item : view.inventory)
        typeWidth = (std::max)(typeWidth, TextWidth(typeText(item)));
    // A sortable heading keeps room beside its label for the sort arrow, and
    // a column sized to the label alone clips it to "D...". The allowance is
    // ImGui's own (TableHeader: FontSize * 0.65 + FramePadding.x), so each
    // fixed column is exactly its heading-with-arrow or its widest content,
    // and the Name column gets everything that is left.
    const auto *tableStyle = Im::GetStyle();
    const float arrow = std::floor(Im::GetFontSize() * 0.65f + (tableStyle ? tableStyle->FramePadding.x : 4.0f));
    const float damageWidth = (std::max)(TextWidth(Tr("Dmg")) + arrow, TextWidth("999")) + gutter;
    const float armorWidth = (std::max)(TextWidth(Tr("Armor")) + arrow, TextWidth("999")) + gutter;
    const float weightWidth = (std::max)(TextWidth(Tr("Wgt")) + arrow, TextWidth("999.9")) + gutter;
    const float valueWidth = (std::max)(TextWidth(Tr("Val")) + arrow, TextWidth("99999")) + gutter;
    // Content is the tick and, pinned, the pin beside it: two glyph boxes.
    const float wornWidth = (std::max)(TextWidth(Tr("Equipped")) + arrow, Im::GetFontSize() * 2.0f) + gutter;

    const float handWidth = (std::max)(TextWidth(Tr("Right")) + arrow, Im::GetFontSize() * 2.0f) + gutter;
    // A scroll's list: Cast and Mag after the effect, as a spell's list.
    float castWidth = TextWidth(Tr("Cast")) + arrow;
    if (scrolls)
        for (const auto &item : view.inventory)
            castWidth = (std::max)(castWidth, TextWidth(item.cast));
    castWidth += gutter;
    const float magWidth = (std::max)(TextWidth(Tr("Mag")) + arrow, TextWidth("999")) + gutter;
    const int columnCount =
        4 + ((weapons || armour) ? 1 : 0) + (scrolls ? 2 : 0) + (anyHand ? 2 : 0) + (anyWorn ? 1 : 0);

    Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, kCellPadY));
    if (!Im::BeginTable("inventory", columnCount, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        Im::PopStyleVar(1);
        return;
    }
    Im::TableSetupColumn(Tr("Name"), Im::ImGuiTableColumnFlags_WidthStretch | Im::ImGuiTableColumnFlags_DefaultSort,
                         1.0f, static_cast<Im::ImGuiID>(Column::Name));
    Im::TableSetupColumn(consumables ? Tr("Effect") : Tr("Type"), Im::ImGuiTableColumnFlags_WidthFixed,
                         typeWidth + gutter, static_cast<Im::ImGuiID>(Column::Type));
    // The stat, highest first on the first click: for a weapon or a piece
    // of armour it is the number, and the rest wait on the item's page.
    if (weapons)
        Im::TableSetupColumn(Tr("Dmg"),
                             Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                             damageWidth, static_cast<Im::ImGuiID>(Column::Damage));
    else if (armour)
        Im::TableSetupColumn(Tr("Armor"),
                             Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                             armorWidth, static_cast<Im::ImGuiID>(Column::Armor));
    if (scrolls)
    {
        Im::TableSetupColumn(Tr("Cast"), Im::ImGuiTableColumnFlags_WidthFixed, castWidth,
                             static_cast<Im::ImGuiID>(Column::Cast));
        Im::TableSetupColumn(Tr("Mag"),
                             Im::ImGuiTableColumnFlags_WidthFixed | Im::ImGuiTableColumnFlags_PreferSortDescending,
                             magWidth, static_cast<Im::ImGuiID>(Column::Magnitude));
    }
    Im::TableSetupColumn(Tr("Wgt"), Im::ImGuiTableColumnFlags_WidthFixed, weightWidth,
                         static_cast<Im::ImGuiID>(Column::Weight));
    Im::TableSetupColumn(Tr("Val"), Im::ImGuiTableColumnFlags_WidthFixed, valueWidth,
                         static_cast<Im::ImGuiID>(Column::Value));
    // Ascending first, like the rest: pinned, equipped, unequipped, then the
    // slashed cells. "Equipped", not "Worn": it is the word the item's page
    // uses, and the one that fits a weapon.
    if (anyHand)
    {
        Im::TableSetupColumn(Tr("Left"), Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Left));
        Im::TableSetupColumn(Tr("Right"), Im::ImGuiTableColumnFlags_WidthFixed, handWidth,
                             static_cast<Im::ImGuiID>(Column::Right));
    }
    if (anyWorn)
    {
        Im::TableSetupColumn(Tr("Equipped"), Im::ImGuiTableColumnFlags_WidthFixed, wornWidth,
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
        const bool dim = (Dimmed(*item) || item->banned) && state.category >= 0;
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
            Tooltip(item->banned ? std::string(Tr("Banned")) : item->asideBy);
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
    const std::string carriedText = TrFormat("Carrying {:.0f} / {:.0f}", view.carriedWeight, view.carryCapacity);
    const char *carried = carriedText.c_str();
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
//
// A spell tome a follower Progression levels carries has Learn at the
// right (SpellControlsFor), asked once: as the player reads one, nothing is
// asked but that they do not know the spell. The tome is used, and the
// page goes back to the books.
void DrawItemDetail(const CharacterView &view, const InventoryItem &item, PanelState &panel)
{
    Im::Spacing();
    const float lineRight = Im::GetCursorPosX() + Im::GetContentRegionAvail().x;
    if (BackButton())
        CloseDetail(panel, panel.inventory, Tab::Inventory);
    DetailName(item.name, NameTint(item));
    NameBadges(item, false, true);
    DetailSubtitle(item.type);
    if (const auto controls = item.teaches != 0 && !view.player ? fp::game::SpellControlsFor(view.id) : std::nullopt)
    {
        const fp::SpellButton learn = controls->active ? fp::LearnButton(item.teachesName, item.knowsTaught)
                                                       : fp::SpellButton{false, controls->why};
        bool asking = panel.inventory.confirming == item.Key();
        if (AskedActionAtRight(Tr("Learn"), learn.can, learn.hover, asking, lineRight))
        {
            fp::game::LearnFromTome(view.id, item.form);
            PlayGameSound(kSpellLearnedSound);
            CloseDetail(panel, panel.inventory, Tab::Inventory, false);
            g_inventoryList.category = static_cast<int>(ItemCategory::Books);
            RefreshAfterAction();
            return;
        }
        panel.inventory.confirming = asking ? item.Key() : 0;
    }
    // Charge, straight from the bag: without it a copy is charged only by
    // a rule, once it is in hand.
    if (item.chargeable)
    {
        const bool full = item.charge >= item.maxCharge;
        const bool gem = std::any_of(view.inventory.begin(), view.inventory.end(),
                                     [](const InventoryItem &i) { return i.filledSoulGem; });
        const char *hover = full   ? Tr("Fully charged")
                            : !gem ? Tr("No soul gem available")
                                   : Tr("Click to charge with weakest soul gem");
        if (ActionAtRight(Tr("Charge"), !full && gem, hover, lineRight))
            RequestCharge(view.id, item.form, item.row);
    }

    Im::Spacing();
    DrawSections(item.detail, false);

    // The enchantment as one headed row; then the effects, a table in the
    // perk page's shape with the author's text wrapped in its last column,
    // each row opening on its conditions, greyed where they do not hold.
    if (!item.enchantment.rows.empty())
        DrawSections({item.enchantment}, true, {}, nullptr, {}, N_("Name"), N_("Charge"));
    if (!item.effectsTable.rows.empty())
    {
        DrawSections(
            {item.effectsTable}, true, {}, nullptr,
            [](const SheetRow &entry, const std::string &key, float left, float right) {
                DrawConditionDrawer(entry, key, left, right);
            },
            N_("Name"), N_("Effect"), WithDescription());
    }
    // The poison as the enchantment is drawn, so an enchanted and poisoned
    // blade reads as two things, which it is.
    if (!item.poison.rows.empty())
        DrawSections({item.poison}, true, {}, nullptr, {}, N_("Name"), N_("Hits Left"));
    if (!item.poisonEffects.rows.empty())
    {
        DrawSections(
            {item.poisonEffects}, true, {}, nullptr,
            [](const SheetRow &entry, const std::string &key, float left, float right) {
                DrawConditionDrawer(entry, key, left, right);
            },
            N_("Name"), N_("Effect"), WithDescription());
    }
    if (!item.description.empty())
    {
        CentredHeading(Tr("Description"));
        Im::TextWrapped("%s", item.description.c_str());
        Im::Spacing();
    }
}

} // namespace

void DrawInventory(const CharacterView &view)
{
    PanelState &panel = Panel(view.id);
    InventoryTabState &state = panel.inventory;

    if (const auto *item = OpenRow(view.inventory, state.detail))
    {
        DrawItemDetail(view, *item, panel);
        return;
    }

    if (view.inventory.empty())
    {
        Im::Spacing();
        Im::TextDisabled("%s", Tr("Nothing carried."));
        return;
    }
    DrawInventoryList(view, state);
}

} // namespace ft::game::ui
