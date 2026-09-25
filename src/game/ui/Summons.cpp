// The Summons tab: what they have raised or called, and its health.

#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/Widgets.h"

#include "core/Breakdown.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace ft::game::ui
{
namespace
{

// One summon or raised corpse, laid out as the Character tab is: the three
// bars on the left, level, kind and time left on the right, then its sheet.
// The sheet's links go nowhere: a summon's sword is not in their inventory.
void DrawSummon(const SummonView &summon)
{
    Im::Spacing();

    const std::string levelText = std::to_string(static_cast<unsigned>(summon.level));
    const std::string kindText = summon.raised ? Tr("raised") : Tr("summoned");
    const std::string remainingText = summon.remaining > 0.0f ? TrFormat("{:.0f} s", summon.remaining) : "-";

    const float originX = Im::GetCursorPosX();
    const auto *style = Im::GetStyle();
    const float inset = style->ItemSpacing.x;

    RowGeometry geo;
    geo.barLabelRight = originX + inset + WidestLabel({Tr("Health"), Tr("Stamina"), Tr("Magicka")});
    geo.barLeft = geo.barLabelRight + 12.0f;
    const float contentRight = originX + Im::GetContentRegionAvail().x - inset;
    const float valueWidth = (std::max)({TextWidth(levelText), TextWidth(kindText), TextWidth(remainingText)});
    geo.valueLeft = contentRight - valueWidth;
    geo.statLabelRight = geo.valueLeft - 12.0f;

    DrawStatRow(
        geo, Tr("Health"), summon.health, kHealth, Tr("Level"), [&] { Im::Text("%s", levelText.c_str()); },
        summon.healthBreakdown);
    DrawStatRow(
        geo, Tr("Stamina"), summon.stamina, kStamina, Tr("Kind"), [&] { Im::TextDisabled("%s", kindText.c_str()); },
        summon.staminaBreakdown);
    DrawStatRow(
        geo, Tr("Magicka"), summon.magicka, kMagicka, Tr("Remaining"),
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

} // namespace

void DrawSummons(const CharacterView &view)
{
    if (view.summons.empty())
    {
        Im::Spacing();
        Im::TextDisabled("%s", Tr("Nothing summoned or raised."));
        return;
    }
    // The summon chosen, by its reference: two of one creature share a
    // name, and a place in the list moves when one ahead of it expires.
    int &chosen = Panel(view.id).summon;
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

} // namespace ft::game::ui
