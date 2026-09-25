// The Skills tab: the skills, and each skill's perk tree.

#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/Widgets.h"

#include "core/I18n.h"
#include "core/Table.h"
#include "game/Tactics.h"
#include "progression/game/Service.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ft::game::ui
{
namespace
{

// Does a perk's row hold the filter's text: its name, rank or description?
bool PerkShown(const SheetRow &row)
{
    return AnyContains({row.label, row.value, row.modifiers}, g_perksFilter);
}

// A skill's tree as the perk menu draws it (core/PerkTree.h): a circle a
// node, filled by the share of its ranks held, from the top clockwise; a
// line from each node to those it leads to, drawn full where both ends are
// held. A node whose next rank asks more than the skill's level is dimmed.
// Hovering a circle gives the perk's name and ranks, what it needs, its
// description, and what a click would do; a name lights up under the cursor, since
// a click on it opens the perk's page.
struct TreeHandlers
{
    // A click on a circle, left or right.
    std::function<void(const ft::PerkTreeNode &, bool right)> node;
    // A click on a name.
    std::function<void(const ft::PerkTreeNode &)> name;
    // Whether a click and a right click can act: said last in the hover.
    struct Actions
    {
        bool acquire{false};
        bool remove{false};
    };
    std::function<Actions(const ft::PerkTreeNode &)> actions;
};

void DrawPerkTree(const ft::PerkTreeView &tree, const TreeHandlers &on)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw || tree.nodes.empty())
        return;
    const float font = Im::GetFontSize();
    const float radius = font * 0.7f;
    const float gap = font * 0.35f;
    const Im::ImVec2 origin = Im::GetCursorScreenPos();
    const Im::ImVec2 room = Im::GetContentRegionAvail();
    // The rest of the tab and no more, so the tree never scrolls; a hair
    // short of it, so rounding cannot put a scrollbar there either.
    const float width = room.x;
    const float height = (std::max)(room.y - 2.0f, radius * 4.0f);

    // Where each node and its label go (core/PerkTree.h): across in the
    // tree's columns, up by its first rank's level (by the records' heights
    // in a tree that asks none), each label at the place about its circle
    // that keeps it clearest. A label is measured with every
    // rank held, so a rank taken cannot widen it and move the tree.
    const float ring = radius + 2.0f; // the circle with its hover ring
    std::vector<std::string> labels;
    std::vector<float> labelWidth;
    labels.reserve(tree.nodes.size());
    labelWidth.reserve(tree.nodes.size());
    for (const ft::PerkTreeNode &node : tree.nodes)
    {
        const auto ranksText = [&](int held) {
            return node.ranks > 1 ? " (" + std::to_string(held) + "/" + std::to_string(node.ranks) + ")"
                                  : std::string();
        };
        labels.push_back(node.name + ranksText(node.held));
        labelWidth.push_back(TextWidth(node.name + ranksText(node.ranks)));
    }
    const ft::TreeDrawing drawing =
        ft::LayOutTree(tree.nodes, labelWidth, ring, gap, Im::GetTextLineHeight(), width, height);
    const auto &at = drawing.centres;
    const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
    const auto dim = Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f);
    const auto lit = Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f);
    const auto theirs = Im::GetColorU32(kTheirOwn);
    const auto theirsGiven = Im::GetColorU32(Im::ImVec4(kTheirOwn.x, kTheirOwn.y, kTheirOwn.z, 0.45f));
    const auto centre = [&](std::size_t i) { return Im::ImVec2(origin.x + at[i].x, origin.y + at[i].y); };

    // The links first, each from one circle's edge to the other's, and bowed
    // past any node between them (core/PerkTree.h) so a link that passes a
    // node is not read as one that meets it.
    for (const ft::TreeLink &link : drawing.links)
    {
        const Im::ImVec2 a(origin.x + link.a.x, origin.y + link.a.y);
        const Im::ImVec2 b(origin.x + link.b.x, origin.y + link.b.y);
        const bool taken = tree.nodes[link.from].held > 0 && tree.nodes[link.to].held > 0;
        const auto colour = taken ? ink : dim;
        const float thickness = taken ? 2.0f : 1.0f;
        if (link.bowed)
            Im::ImDrawListManager::AddBezierQuadratic(
                draw, a, Im::ImVec2(origin.x + link.control.x, origin.y + link.control.y), b, colour, thickness, 0);
        else
            Im::ImDrawListManager::AddLine(draw, a, b, colour, thickness);
    }

    std::size_t hovered = tree.nodes.size();
    for (std::size_t i = 0; i < tree.nodes.size(); ++i)
    {
        const ft::PerkTreeNode &node = tree.nodes[i];
        const Im::ImVec2 c = centre(i);
        Im::SetCursorScreenPos(Im::ImVec2(c.x - radius, c.y - radius));
        Im::PushID(static_cast<int>(i));
        const bool clicked = Im::InvisibleButton("node", Im::ImVec2(radius * 2.0f, radius * 2.0f), 0);
        const bool rightClicked = Im::IsItemClicked(Im::ImGuiMouseButton_Right);
        const bool over = Im::IsItemHovered(0);
        if (over)
            hovered = i;
        if ((clicked || rightClicked) && on.node)
            on.node(node, rightClicked);

        // The name, its own click target: lit under the cursor, as a link
        // is, and a click on it opens the perk's page.
        const Im::ImVec2 labelAt(origin.x + drawing.labels[i].x, origin.y + drawing.labels[i].y);
        const Im::ImVec2 labelSize(TextWidth(labels[i]), Im::GetTextLineHeight());
        Im::SetCursorScreenPos(labelAt);
        const bool nameClicked = Im::InvisibleButton("name", labelSize, 0);
        const bool overName = Im::IsItemHovered(0);
        Im::PopID();
        if (nameClicked && on.name)
            on.name(node);
        if (overName)
        {
            Im::ImDrawListManager::AddRectFilled(draw, Im::ImVec2(labelAt.x - 3.0f, labelAt.y),
                                                 Im::ImVec2(labelAt.x + labelSize.x + 3.0f, labelAt.y + labelSize.y),
                                                 lit, 3.0f, 0);
            // One of their own, the amber ring's: not bought here.
            if (node.theirs)
                Tooltip(Tr("Acquired outside of this framework"));
        }
        const bool reachable = node.held > 0 || tree.level >= node.requirement;
        const auto colour = reachable ? ink : dim;
        // Every rank held: the whole circle, in one piece. A share of them:
        // its slices, the second laid a hair over the first, so no seam
        // shows between them.
        if (node.held >= node.ranks)
            Im::ImDrawListManager::AddCircleFilled(draw, c, radius, colour, 0);
        else
        {
            const auto arcs = ft::HeldArcs(node.held, node.ranks);
            for (std::size_t k = 0; k < arcs.size(); ++k)
            {
                Im::ImDrawListManager::PathClear(draw);
                Im::ImDrawListManager::PathLineTo(draw, c);
                Im::ImDrawListManager::PathArcTo(draw, c, radius, arcs[k].from - (k > 0 ? 0.05f : 0.0f), arcs[k].to, 0);
                Im::ImDrawListManager::PathFillConvex(draw, colour);
            }
        }
        // The ring: amber for one of their own, faded while given back.
        const auto rim = over ? lit : !node.theirs ? colour : node.held > 0 ? theirs : theirsGiven;
        Im::ImDrawListManager::AddCircle(draw, c, radius, rim, 0, over || node.theirs ? 2.5f : 1.5f);

        Im::ImDrawListManager::AddText(draw, Im::ImVec2(origin.x + drawing.labels[i].x, origin.y + drawing.labels[i].y),
                                       colour, labels[i].c_str());
    }
    // The canvas is the page's content: the cursor goes below it, and an
    // item there is what makes the region scroll to it.
    Im::SetCursorScreenPos(Im::ImVec2(origin.x, origin.y + height));
    Im::Dummy(Im::ImVec2(width, 0.0f));

    // The hover, laid out to one width: the name at the left and what it
    // needs at the right on the first line -- with what they have beneath,
    // both greyed, when they fall short; the labels right-aligned to one
    // edge, as a greyed spell's are -- then the description, then what a
    // click does at the left and a right click at the right.
    if (hovered < tree.nodes.size())
    {
        const ft::PerkTreeNode &node = tree.nodes[hovered];
        const std::string title =
            node.ranks > 1 ? fmt::format("{} ({}/{})", node.name, node.held, node.ranks) : node.name;
        const bool needs = node.requirement > 0.0f;
        const int need = static_cast<int>(node.requirement);
        const int has = static_cast<int>(tree.level);
        const bool short_ = needs && has < need;
        const NeedsHasBlock requirement = NeedsAndHas(tree.name, need, short_ ? std::optional<int>(has) : std::nullopt);
        const float wide = font * 2.0f;
        const float block = needs ? requirement.Width() : 0.0f;
        const TreeHandlers::Actions can = on.actions ? on.actions(node) : TreeHandlers::Actions{};
        const char *kAcquire = Tr("Click to acquire perk");
        const char *kRemove = Tr("Right click to remove perk");
        const float actions = (can.acquire ? TextWidth(kAcquire) : 0.0f) + (can.remove ? TextWidth(kRemove) : 0.0f) +
                              (can.acquire && can.remove ? wide : 0.0f);
        const float lineWidth = (std::max)({TextWidth(title) + (needs ? wide + block : 0.0f), actions,
                                            (std::min)(TextWidth(node.description), font * 28.0f)});

        Im::BeginTooltip();
        const float x0 = Im::GetCursorPosX();
        Im::Text("%s", title.c_str());
        if (needs)
        {
            const DimText grey(short_);
            requirement.Draw(x0 + lineWidth - block, true);
        }
        if (!node.description.empty())
        {
            Im::Spacing();
            Im::PushTextWrapPos(x0 + lineWidth);
            Im::TextWrapped("%s", node.description.c_str());
            Im::PopTextWrapPos();
        }
        if (can.acquire || can.remove)
        {
            Im::Spacing();
            if (can.acquire)
                Im::Text("%s", kAcquire);
            if (can.remove)
            {
                if (can.acquire)
                    Im::SameLine(0.0f, 0.0f);
                Im::SetCursorPosX(x0 + lineWidth - TextWidth(kRemove));
                Im::Text("%s", kRemove);
            }
        }
        Im::EndTooltip();
    }
}

// A tree page's tree as Progression names it: a skill's, a custom tree's
// by its id, or none for a skill Progression does not level (Vampire
// Lord's).
std::optional<fp::TreeRef> ProgressionTree(const ft::PerkTreeView &tree)
{
    if (!tree.custom.empty())
        return fp::TreeRef::Custom(tree.custom);
    if (const auto skill = tree.skill ? fp::SkillFromActorValue(*tree.skill) : std::nullopt)
        return fp::TreeRef(*skill);
    return std::nullopt;
}

} // namespace

void DrawSkills(const CharacterView &view)
{
    SkillsTabState &state = Panel(view.id).skills;
    if (const PerkPage *page = OpenRow(view.perks, state.detail))
    {
        if (BackButton())
            state.detail = 0;
        DetailName(page->name);
        Im::Spacing();
        // The perk's facts; then its effects, each with the conditions
        // that gate it beneath, in the table the skills open into.
        const auto split = page->sections.begin() + (page->sections.empty() ? 0 : 1);
        const std::vector<SheetSection> info(page->sections.begin(), split);
        const std::vector<SheetSection> effects(split, page->sections.end());
        DrawSections(info, false);
        // No third column: an entry whose conditions fail is greyed,
        // as an effect's row is, not marked.
        DrawSections(
            effects, true, {}, nullptr,
            [](const SheetRow &row, const std::string &key, float left, float right) {
                DrawConditionDrawer(row, key, left, right);
            },
            N_("Name"), N_("Value"));
        if (!page->description.empty())
        {
            CentredHeading(Tr("Description"));
            Im::TextWrapped("%s", page->description.c_str());
            Im::Spacing();
        }
        return;
    }
    // A skill's page: its tree, under its name and its level, as the perk
    // menu heads it. A perk's page opened from here comes back to it.
    if (const auto *tree = OpenRow(view.trees, state.tree))
    {
        // The name at the left; the level in the middle; the perks to
        // spend and Reset perks at the right. A follower Progression
        // levels has <<, -, + and >> about a skill's level (Progression's
        // ControlsFor), and in any tree it knows, a skill's or a custom
        // one, the perks to spend and Reset perks (TreeControlsFor), each
        // greyed with why when it cannot act; the player, and a follower
        // it does not level, the level alone.
        const auto controls = view.player || !tree->skill ? std::nullopt : fp::game::ControlsFor(view.id, *tree->skill);
        const auto progressionTree = ProgressionTree(*tree);
        const auto perks =
            view.player || !progressionTree ? std::nullopt : fp::game::TreeControlsFor(view.id, *progressionTree);
        const float lineX = Im::GetCursorPosX();
        const float lineWidth = Im::GetContentRegionAvail().x;
        const float button = Im::GetFrameHeight();
        const auto pointButton = [&](const char *id, Glyph glyph, bool can, const std::string &why) {
            Im::BeginDisabled(!can);
            Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
            const bool clicked = GlyphButton(id, button, glyph);
            Im::PopStyleVar(1);
            Im::EndDisabled();
            if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
                Tooltip(why);
            return clicked;
        };
        const auto move = [&](int direction, bool allTheWay) {
            if (allTheWay)
                fp::game::AssignSkillAll(controls->companion, controls->skill, direction);
            else
                fp::game::AssignSkillPoint(controls->companion, controls->skill, direction);
            RefreshAfterAction();
        };

        if (BackButton())
        {
            state.tree = 0;
            state.confirmReset = false;
        }
        DetailName(tree->name);

        const std::string level = controls ? std::to_string(controls->level) : tree->value;
        const float group = TextWidth(level) + (controls ? 4.0f * (button + kCellPadX) : 0.0f);
        Im::SameLine(0.0f, 0.0f);
        Im::SetCursorPosX((std::max)(Im::GetCursorPosX() + kCellPadX, lineX + (lineWidth - group) / 2.0f));
        if (controls)
        {
            const fp::SkillButtons &b = controls->buttons;
            if (pointButton("lowest", Glyph::AllTheWayLeft, b.canLower, b.lowest))
                move(-1, true);
            Im::SameLine(0.0f, kCellPadX);
            if (pointButton("lower", Glyph::Minus, b.canLower, b.lower))
                move(-1, false);
            Im::SameLine(0.0f, kCellPadX);
        }
        Im::AlignTextToFramePadding();
        Im::Text("%s", level.c_str());
        if (Im::IsItemHovered(0))
        {
            if (controls && controls->learned != 0)
                Tooltip(TrFormat("{} their own, {:+} learned", controls->base, controls->learned));
            else if (!controls && tree->current != tree->level)
                Tooltip(TrFormat("{:.0f} with the effects on it", tree->current));
        }
        if (controls)
        {
            const fp::SkillButtons &b = controls->buttons;
            Im::SameLine(0.0f, kCellPadX);
            if (pointButton("raise", Glyph::Plus, b.canRaise, b.raise))
                move(+1, false);
            Im::SameLine(0.0f, kCellPadX);
            if (pointButton("highest", Glyph::AllTheWayRight, b.canRaise, b.highest))
                move(+1, true);
        }
        if (perks)
        {
            // The perks to spend, then Reset perks, which asks once: it
            // gives back every perk bought in the tree, free to buy again
            // but not one click away.
            const std::string available = perks->perkPoints <= 0   ? std::string()
                                          : perks->perkPoints == 1 ? std::string(Tr("1 perk available"))
                                                                   : TrFormat("{} perks available", perks->perkPoints);
            const float buttons = AskedActionWidth(Tr("Reset perks"), state.confirmReset && perks->reset.can);
            Im::SameLine(0.0f, 0.0f);
            const float lead = available.empty() ? 0.0f : TextWidth(available) + kCellPadX;
            Im::SetCursorPosX((std::max)(Im::GetCursorPosX() + kCellPadX, lineX + lineWidth - buttons - lead));
            if (!available.empty())
            {
                Im::AlignTextToFramePadding();
                Im::Text("%s", available.c_str());
                Im::SameLine(0.0f, kCellPadX);
            }
            if (AskedAction(Tr("Reset perks"), perks->reset.can, perks->reset.text, state.confirmReset))
            {
                fp::game::ResetPerks(perks->companion, perks->tree);
                PlayGameSound(kPerkReturnedSound);
                RefreshAfterAction();
            }
        }
        Im::Spacing();

        // The tree: a circle's click acquires its next rank, a right
        // click gives the top one back, each where Progression says it
        // can (PerkControlsFor) and with the game's own sound; where it
        // cannot, the failure sound and nothing else. A name's click
        // opens the perk's page -- the top rank held, or the first.
        TreeHandlers on;
        const auto can = [&view](const ft::PerkTreeNode &node) {
            return view.player || node.firstForm == 0 ? std::nullopt
                                                      : fp::game::PerkControlsFor(view.id, node.firstForm);
        };
        on.node = [&view, can](const ft::PerkTreeNode &node, bool right) {
            const auto controlsNow = can(node);
            if (!right && controlsNow && controlsNow->canLearn)
            {
                fp::game::LearnPerkByForm(view.id, node.firstForm);
                PlayGameSound(kPerkTakenSound);
                RefreshAfterAction();
            }
            else if (right && controlsNow && controlsNow->canUnlearn)
            {
                fp::game::UnlearnPerkByForm(view.id, node.firstForm);
                PlayGameSound(kPerkReturnedSound);
                RefreshAfterAction();
            }
            else
                PlayGameSound(kRefusedSound);
        };
        on.name = [&state](const ft::PerkTreeNode &node) {
            state.detail = node.form != 0 ? node.form : node.firstForm;
        };
        on.actions = [can](const ft::PerkTreeNode &node) {
            TreeHandlers::Actions out;
            if (const auto controlsNow = can(node))
            {
                out.acquire = controlsNow->canLearn;
                out.remove = controlsNow->canUnlearn;
            }
            return out;
        };
        DrawPerkTree(*tree, on);
        return;
    }
    // The skills, with their trees; then the perks in no tree, in the same
    // table the trees open into, under a heading of their own. A perk's
    // name opens its page; a skill's name opens its tree, and its level the
    // perks it holds.
    const auto open = [&state](std::uint32_t form) { state.detail = form; };
    const auto openTree = [&state](const SheetRow &row) {
        state.tree = row.tree;
        state.confirmReset = false;
    };
    std::vector<SheetSection> skills;
    const SheetSection *other = nullptr;
    for (const auto &section : view.skills)
    {
        if (section.title == Tr("Other Perks"))
            other = &section;
        else
            skills.push_back(section);
    }
    DrawSections(skills, true, open, N_("Modifiers"), {}, N_("Skill"), N_("Level"), {}, openTree);
    if (other)
    {
        CentredHeading(Tr("Other Perks"));
        // The perks outside the trees run long in a large load order.
        FilterRow(
            "##perksfilter", g_perksFilter, sizeof(g_perksFilter),
            [&] { return static_cast<std::size_t>(std::count_if(other->rows.begin(), other->rows.end(), PerkShown)); },
            other->rows.size(), Tr("perks"));
        Im::Spacing();
        std::vector<SheetRow> rows;
        for (const SheetRow &row : other->rows)
            if (PerkShown(row))
                rows.push_back(row);
        Im::PushStyleVar(Im::ImGuiStyleVar_CellPadding, Im::ImVec2(kCellPadX, 4.0f));
        DrawPerkTable("perks##other", rows, 0.0f, open);
        Im::PopStyleVar(1);
        Im::Spacing();
        Im::Spacing();
    }
}

} // namespace ft::game::ui
