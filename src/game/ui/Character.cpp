// The Character tab.

#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/UI.h"
#include "game/ui/Widgets.h"

#include "core/Breakdown.h"
#include "game/Tactics.h"
#include "progression/game/Service.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>

namespace ft::game::ui
{
void DrawCharacter(const CharacterView &view)
{
    // Breathing room at the top -- the first bar sat flush against the panel
    // border.
    Im::Spacing();

    // Four rows of a bar on the left -- health, stamina, magicka, and the
    // level beside the experience toward the next -- and two stats on the
    // right, laid out from measured widths so every column is flush and
    // nothing depends on the length of an English word. The player's
    // experience is the level-up menu's; a follower Progression levels has
    // its own (LevelFor); another follower has a level and no experience.
    const auto progress = view.player ? std::nullopt : fp::game::LevelFor(view.id);
    const int level = progress ? progress->level : static_cast<int>(view.level);
    const std::string levelLabel = TrFormat("Level {}", level);
    const bool hasExperience = view.hasExperience || progress.has_value();
    const ft::Stat experience =
        progress ? ft::Stat{static_cast<float>(progress->into), static_cast<float>(progress->toNext)} : view.experience;
    const std::string statusText = view.inCombat ? Tr("combat") : Tr("idle");
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
    geo.barLabelRight =
        originX + inset + (std::max)(WidestLabel({Tr("Health"), Tr("Stamina"), Tr("Magicka")}), TextWidth(levelLabel));
    geo.barLeft = geo.barLabelRight + 12.0f;

    // The stat column is pinned to the RIGHT edge of the panel rather than left
    // against the bars, so it lines up with the rule table on the other tab.
    // Mirror the inset on the right so the stat values sit inboard of the border
    // by the same amount the labels do on the left.
    const float contentRight = originX + Im::GetContentRegionAvail().x - inset;
    const float valueWidth = (std::max)(TextWidth(statusText), TextWidth(carriedText));
    geo.valueLeft = contentRight - valueWidth;
    geo.statLabelRight = geo.valueLeft - 12.0f;

    // A follower Progression levels: each attribute's label opens its points'
    // controls after the bars, << - + >> as a skill's are, a point 10 of the
    // value (iAVDhmsLevelUp), and closes them again; the points to assign
    // are in the right column while they are open.
    const auto attributes = view.player ? std::nullopt : fp::game::AttributeControlsFor(view.id);
    PanelState &panel = Panel(view.id);
    const auto attributeRow = [&](fp::Attribute attribute) {
        RowControls row;
        if (!attributes)
            return row;
        row.open = &panel.attributeControls;
        row.toOpen = Tr("Click to assign attribute points");
        row.toClose = Tr("Click to hide attribute controls");
        row.draw = [&attributes, attribute] {
            const fp::AttributeButtons &b = attributes->buttons[fp::Index(attribute)];
            const float size = Im::GetFrameHeight();
            const auto button = [size](const char *id, Glyph glyph, bool can, const std::string &why) {
                Im::BeginDisabled(!can);
                Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
                const bool clicked = GlyphButton(id, size, glyph);
                Im::PopStyleVar(1);
                Im::EndDisabled();
                if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
                    Tooltip(why);
                return clicked;
            };
            const auto move = [&attributes, attribute](int direction, bool allTheWay) {
                if (allTheWay)
                    fp::game::AssignAttributeAll(attributes->companion, attribute, direction);
                else
                    fp::game::AssignAttributePoint(attributes->companion, attribute, direction);
                RefreshAfterAction();
            };
            Im::PushID(static_cast<int>(attribute));
            if (button("lowest", Glyph::AllTheWayLeft, b.canLower, b.lowest))
                move(-1, true);
            Im::SameLine(0.0f, kCellPadX);
            if (button("lower", Glyph::Minus, b.canLower, b.lower))
                move(-1, false);
            Im::SameLine(0.0f, kCellPadX);
            if (button("raise", Glyph::Plus, b.canRaise, b.raise))
                move(+1, false);
            Im::SameLine(0.0f, kCellPadX);
            if (button("highest", Glyph::AllTheWayRight, b.canRaise, b.highest))
                move(+1, true);
            Im::PopID();
        };
        return row;
    };
    const RowControls healthRow = attributeRow(fp::Attribute::Health);
    const RowControls staminaRow = attributeRow(fp::Attribute::Stamina);
    const RowControls magickaRow = attributeRow(fp::Attribute::Magicka);
    const std::string pointsText =
        attributes && panel.attributeControls ? std::to_string(attributes->available) : std::string();

    DrawStatRow(
        geo, Tr("Health"), view.health, kHealth, Tr("Status"),
        [&] {
            if (view.inCombat)
                Im::TextColored(kFighting, "%s", statusText.c_str());
            else
                Im::TextDisabled("%s", statusText.c_str());
        },
        view.healthBreakdown, &healthRow);

    DrawStatRow(
        geo, Tr("Stamina"), view.stamina, kStamina, Tr("Carrying"),
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
        view.staminaBreakdown, &staminaRow);

    DrawStatRow(
        geo, Tr("Magicka"), view.magicka, kMagicka, pointsText.empty() ? nullptr : Tr("Attribute points"),
        [&] { Im::Text("%s", pointsText.c_str()); }, view.magickaBreakdown, &magickaRow);

    // The level beside its experience, in a muted gold; a follower with no
    // experience of ours has the level alone.
    if (hasExperience)
    {
        DrawStatRow(geo, levelLabel.c_str(), experience, kExperience, nullptr, {});
        if (progress && progress->engine != progress->level && Im::IsItemHovered(0))
            Tooltip(
                TrFormat("Level {} from the game, {} with what they have learned", progress->engine, progress->level));
    }
    else
    {
        Im::SetCursorPosX((std::max)(0.0f, geo.barLabelRight - TextWidth(levelLabel)));
        Im::AlignTextToFramePadding();
        Im::Text("%s", levelLabel.c_str());
    }

    Im::Spacing();

    // A weapon, shield, ammo or torch named on the sheet is a link to its
    // page on the Inventory tab.
    const ft::ActorId id = view.id;
    DrawSections(view.sheet, false, [id, &view](std::uint32_t form) {
        // A spell in hand has its page on the Magic tab, a power or shout
        // on the Shouts tab; anything else on the Inventory tab.
        const Tab page = MagicPageOf(view, form);
        PanelState &panel = Panel(id);
        if (page != Tab::None)
        {
            MagicTabState &magic = MagicPageState(id, page);
            magic.detail = form;
            magic.openedFrom = Tab::Character;
            panel.select = page;
            return;
        }
        // Of the form's rows, the worn one: what the sheet names is the
        // copy in hand, and a row of the form that is not worn is a spare.
        panel.inventory.detail = ItemPageOf(view, form);
        panel.inventory.openedFrom = Tab::Character;
        panel.select = Tab::Inventory;
    });
}

} // namespace ft::game::ui
