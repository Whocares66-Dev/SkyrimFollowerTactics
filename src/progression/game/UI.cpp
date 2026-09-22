#include "progression/game/UI.h"

#include "progression/core/Levelling.h"
#include "progression/core/Perks.h"
#include "progression/core/Spells.h"
#include "progression/game/Learning.h"
#include "progression/game/Log.h"
#include "progression/game/PerkTrees.h"
#include "progression/game/PerkView.h"
#include "progression/game/Service.h"
#include "progression/game/SpellView.h"

#include <SKSEMenuFramework.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fp::game::ui
{
namespace
{

namespace Im = ImGuiMCP;

constexpr std::size_t kSlots = 32;
const std::string kSection = "Follower Tactics/Progression";

// --- the snapshot this frame draws from --------------------------------------------
//
// Copied from the service when its version moves, and only then: a frame
// that changes nothing copies nothing. Render thread only.
const Snapshot &Snap()
{
    static Snapshot snapshot;
    static std::uint64_t seen = 0;
    const std::uint64_t now = Version();
    if (now != seen)
    {
        snapshot = Read();
        seen = now;
    }
    return snapshot;
}

struct Found
{
    const Companion *companion{nullptr};
    const CompanionView *view{nullptr};
};

Found FindIn(const Snapshot &s, const FormKey &key)
{
    for (std::size_t i = 0; i < s.companions.size(); ++i)
        if (s.companions[i].key == key)
            return {&s.companions[i], i < s.views.size() ? &s.views[i] : nullptr};
    return {};
}

// What a page remembers between frames: the perk row asking to be
// confirmed, the spell whose Teach asks to be. Keyed by the companion.
struct PageState
{
    int confirm{-1};
    int confirmReset{-1}; // the skill whose Reset asks to be confirmed
    bool learnableOnly{false};
    std::string confirmTeach; // the spell whose Teach asks to be confirmed
    std::string note;         // a one-line reply to the last click, shown under the header
};

std::unordered_map<std::string, PageState> g_pages;
// Set when the panel opens or closes; the page states are reset on the next
// frame, on the render thread that owns them. The framework's open and close
// events carry no promise about their thread; a frame's own events do
// (Follower Tactics' UI.cpp, OnMenuEvent).
std::atomic<bool> g_resetPages{false};

PageState &PageOf(const FormKey &key)
{
    return g_pages[ToString(key)];
}

// --- drawing helpers ------------------------------------------------------------------

float Width(std::string_view text)
{
    const std::string s(text);
    return Im::CalcTextSize(s.c_str()).x;
}

void Tooltip(std::string_view text)
{
    if (!text.empty() && Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Im::SetTooltip("%s", std::string(text).c_str());
}

// Why a button is greyed: a companion's actor is changed only while they
// are with you, so a page read while they are away can look but not act.
std::string AwayReason(const Companion &c, const CompanionView &v)
{
    return v.loaded ? std::string() : c.name + " must be with you for this.";
}

// Text whose right edge is the content region's right edge, on this line.
void Right(const std::string &text, bool dim = false)
{
    const float right = Im::GetWindowContentRegionMax().x;
    Im::SameLine((std::max)(0.0f, right - Width(text)), -1.0f);
    if (dim)
        Im::TextDisabled("%s", text.c_str());
    else
        Im::Text("%s", text.c_str());
}

void Heading(const char *title)
{
    Im::PushStyleVar(Im::ImGuiStyleVar_SeparatorTextAlign, Im::ImVec2(0.5f, 0.5f));
    Im::SeparatorText(title);
    Im::PopStyleVar(1);
}

// A bar with its numbers centred over it, as Follower Tactics draws its
// pools: ImGui's own overlay follows the fill.
void Bar(float fraction, float width, const std::string &overlay)
{
    Im::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), Im::ImVec2(width, 0.0f), "");
    if (auto *draw = Im::GetWindowDrawList())
    {
        const Im::ImVec2 lo = Im::GetItemRectMin();
        const Im::ImVec2 hi = Im::GetItemRectMax();
        const Im::ImVec2 size = Im::CalcTextSize(overlay.c_str());
        Im::ImDrawListManager::AddText(draw,
                                       {lo.x + (hi.x - lo.x - size.x) * 0.5f, lo.y + (hi.y - lo.y - size.y) * 0.5f},
                                       Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), overlay.c_str());
    }
}

std::string Plus(int value)
{
    return value > 0 ? "+" + std::to_string(value) : (value < 0 ? std::to_string(value) : std::string("-"));
}

std::string Thousands(std::int64_t value)
{
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<std::size_t>(i), ",");
    return value < 0 ? "-" + digits : digits;
}

std::string PointsText(int points)
{
    if (points <= 0)
        return "";
    return points == 1 ? "1 perk point to spend" : std::to_string(points) + " perk points to spend";
}

std::string StatusText(const Companion &c, const CompanionView *v)
{
    if (c.paused)
        return "Paused";
    if (!v || !v->read)
        return "Not seen this session";
    if (v->waiting)
        return "Waiting";
    if (v->following)
        return "With you";
    return "Away";
}

bool TacticsInstalled()
{
    static const bool installed = GetModuleHandleW(L"FollowerTactics.dll") != nullptr;
    return installed;
}

constexpr Im::ImGuiTableFlags kTable = Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg;

// --- the header ------------------------------------------------------------------------

void DrawHeader(const Companion &c, const CompanionView *v, const Snapshot &s, PageState &page)
{
    const bool read = v && v->read;
    const float fontSize = Im::GetFontSize();
    Im::SetWindowFontScale(1.0f);
    const float baseSize = Im::GetFontSize();
    const float scale = baseSize > 0.0f ? fontSize / baseSize : 1.0f;
    Im::SetWindowFontScale(scale * 1.35f);
    Im::Text("%s", c.name.c_str());
    Right(read ? "Level " + std::to_string(v->progress.level) : std::string("Level -"));
    Im::SetWindowFontScale(scale);

    std::string left = "Not seen this session";
    if (read)
    {
        const LevelProgress &p = v->progress;
        const float width = Im::GetContentRegionAvail().x;
        Bar(p.toNext > 0.0 ? static_cast<float>(p.into / p.toNext) : 0.0f, width,
            Thousands(static_cast<std::int64_t>(p.into)) + " / " + Thousands(static_cast<std::int64_t>(p.toNext)) +
                " experience");
        Tooltip(fmt::format("Level {} from the game{}. What they learn by doing stacks on it, on your own leveling's "
                            "curve, up to {} levels past yours.",
                            p.engine,
                            p.level > p.engine
                                ? fmt::format(", {} more from what they have learned", p.level - p.engine)
                                : std::string(),
                            s.rules.levelsAbovePlayer));
        const std::string toAssign = ToAssign(v->perkPoints, v->attributePoints, c.learning.pool);
        left = toAssign.empty() ? std::string("Nothing to assign") : toAssign + " to assign";
        if (p.capped)
            left += fmt::format("  \xC2\xB7  at the limit, {} levels past yours", s.rules.levelsAbovePlayer);
    }
    Im::TextDisabled("%s", left.c_str());
    Right(StatusText(c, v));

    if (!s.refused.empty())
        Im::TextColored(Im::ImVec4(0.90f, 0.62f, 0.45f, 1.0f), "%s", s.refused.c_str());
    if (!page.note.empty())
        Im::TextDisabled("%s", page.note.c_str());
}

// --- Skills ---------------------------------------------------------------------------------

// A -/+ pair for one row. Each click is one level or point, carried out at
// once (the game thread applies it to the actor), as Companions' Path does.
struct PointButtons
{
    bool lower{false};
    bool raise{false};
};

PointButtons Buttons(bool canLower, const std::string &lowerWhy, bool canRaise, const std::string &raiseWhy)
{
    PointButtons out;
    Im::BeginDisabled(!canLower);
    out.lower = Im::SmallButton(" - ");
    Im::EndDisabled();
    Tooltip(lowerWhy);
    Im::SameLine(0.0f, 4.0f);
    Im::BeginDisabled(!canRaise);
    out.raise = Im::SmallButton(" + ");
    Im::EndDisabled();
    Tooltip(raiseWhy);
    return out;
}

// What resetting a skill returns: every level above the floor, at what the
// level is worth.
double ResetWorth(const Rules &r, int level, int floor)
{
    double xp = 0.0;
    for (int l = floor + 1; l <= level; ++l)
        xp += XpForSkillLevel(r, l);
    return xp;
}

void DrawSkills(const Companion &c, const CompanionView &v, const Snapshot &s, PageState &page)
{
    const Rules &r = s.rules;
    const PerSkill<int> effective = Effective(c, v.base);
    const Holdings holdings = HoldingsOf(c, v.onRecord);
    const std::string away = AwayReason(c, v);

    Im::TextWrapped("They learn by doing, as you do: a spell cast, a blow landed, a hit taken on their armour or their "
                    "shield raises the skill used, by your own rules. Their level rises with what they learn.");
    if (c.learning.pool >= 1.0)
        Im::Text("%s", fmt::format("{} XP to reassign: + buys a level at what it is worth.",
                                   Thousands(static_cast<std::int64_t>(c.learning.pool)))
                           .c_str());
    else
        Im::TextDisabled("- takes a level back into a pool, and Reset takes a skill back to where it starts; the pool "
                         "buys levels of others.");
    Im::Spacing();

    if (Im::BeginTable("##skills", 7, kTable | Im::ImGuiTableFlags_SizingFixedFit))
    {
        Im::TableSetupColumn("Skill", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableSetupColumn("Their own", 0, 0.0f, 0);
        Im::TableSetupColumn("Learned", 0, 0.0f, 0);
        Im::TableSetupColumn("Total", 0, 0.0f, 0);
        Im::TableSetupColumn("Next level", 0, 0.0f, 0);
        Im::TableSetupColumn("", 0, 0.0f, 0);
        Im::TableSetupColumn("", 0, 0.0f, 0);
        Im::TableHeadersRow();
        for (const Skill skill : AllSkills())
        {
            if (!IsTrainable(skill))
                continue;
            const std::size_t i = Index(skill);
            const int level = effective[i];
            const int floor = v.floors[i];
            Im::PushID(static_cast<int>(i));
            Im::TableNextRow();
            Im::TableNextColumn();
            Im::AlignTextToFramePadding();
            Im::Text("%s", std::string(Name(skill)).c_str());
            Im::TableNextColumn();
            Im::Text("%d", v.base[i]);
            Tooltip("What the engine gives them. It still rises with their level, as vanilla leveling does.");
            Im::TableNextColumn();
            Im::Text("%s", Plus(c.learning.skills[i]).c_str());
            Im::TableNextColumn();
            Im::Text("%d", level);
            Im::TableNextColumn();
            const double threshold = v.nextLevel[i];
            if (threshold > 0.0)
                Bar(static_cast<float>(c.learning.progress[i] / threshold), 110.0f,
                    fmt::format("{:.0f} / {:.0f}", c.learning.progress[i], threshold));
            else
                Im::TextDisabled("at %d", r.skillCap);
            Im::TableNextColumn();
            const AssignCheck lower =
                CheckSkill(c, skill, -1, v.base, floor, Graph(), holdings, s.settings.showNoEffectPerks, r);
            const AssignCheck raise =
                CheckSkill(c, skill, +1, v.base, floor, Graph(), holdings, s.settings.showNoEffectPerks, r);
            const std::string lowerWhy =
                !v.loaded ? away
                : lower.block == AssignBlock::PerkNeedsIt
                    ? lower.perk + " needs this much " + std::string(Name(skill)) + ": unlearn it, or reset the skill."
                : lower.block == AssignBlock::AtFloor
                    ? fmt::format("At {}: where a new character of their race starts it.", floor)
                    : fmt::format("Into the pool: {} XP.",
                                  Thousands(static_cast<std::int64_t>(XpForSkillLevel(r, level))));
            const std::string raiseWhy =
                !v.loaded ? away
                : raise.block == AssignBlock::AtCap
                    ? fmt::format("At {}.", r.skillCap)
                    : fmt::format("{} XP from the pool; {} in it.",
                                  Thousands(static_cast<std::int64_t>(XpForSkillLevel(r, level + 1))),
                                  Thousands(static_cast<std::int64_t>(c.learning.pool)));
            const PointButtons clicked = Buttons(v.loaded && lower.block == AssignBlock::None, lowerWhy,
                                                 v.loaded && raise.block == AssignBlock::None, raiseWhy);
            if (clicked.lower)
                AssignSkillPoint(c.key, skill, -1);
            if (clicked.raise)
                AssignSkillPoint(c.key, skill, +1);
            Im::TableNextColumn();
            if (page.confirmReset == static_cast<int>(i))
            {
                if (Im::SmallButton("Confirm"))
                {
                    ResetSkill(c.key, skill);
                    page.confirmReset = -1;
                }
                Im::SameLine(0.0f, 4.0f);
                if (Im::SmallButton("Cancel"))
                    page.confirmReset = -1;
            }
            else
            {
                Im::BeginDisabled(!v.loaded || level <= floor);
                if (Im::SmallButton("Reset"))
                    page.confirmReset = static_cast<int>(i);
                Im::EndDisabled();
                Tooltip(!v.loaded
                            ? away
                            : fmt::format("Back to {}: {} XP to reassign, and the perks bought in its tree "
                                          "returned, as making a skill Legendary does. Free.",
                                          floor, Thousands(static_cast<std::int64_t>(ResetWorth(r, level, floor)))));
            }
            Im::PopID();
        }
        Im::EndTable();
    }
    Im::TextDisabled("Smithing, Alchemy, Enchanting, Speech, Lockpicking and Pickpocket are not offered: companions "
                     "don't use them.");

    Im::Spacing();
    Im::Text("%s", fmt::format("{} attribute point{} to assign, {} each", v.attributePoints,
                               v.attributePoints == 1 ? "" : "s", r.attributePerLevel)
                       .c_str());
    Tooltip("One for each level after the first, as your own level-ups give, less what their own health, magicka and "
            "stamina already carry above their race's start.");
    if (Im::BeginTable("##attributes", 5, kTable | Im::ImGuiTableFlags_SizingFixedFit))
    {
        Im::TableSetupColumn("Attribute", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableSetupColumn("Their own", 0, 0.0f, 0);
        Im::TableSetupColumn("Assigned", 0, 0.0f, 0);
        Im::TableSetupColumn("Total", 0, 0.0f, 0);
        Im::TableSetupColumn("", 0, 0.0f, 0);
        Im::TableHeadersRow();
        for (std::size_t a = 0; a < kAttributeCount; ++a)
        {
            const auto attribute = static_cast<Attribute>(a);
            const int added = c.learning.attributes[a] * r.attributePerLevel;
            Im::PushID(100 + static_cast<int>(a));
            Im::TableNextRow();
            Im::TableNextColumn();
            Im::AlignTextToFramePadding();
            Im::Text("%s", std::string(Name(attribute)).c_str());
            Im::TableNextColumn();
            Im::Text("%d", v.attributes[a]);
            Im::TableNextColumn();
            Im::Text("%s", Plus(added).c_str());
            Im::TableNextColumn();
            Im::Text("%d", v.attributes[a] + added);
            Im::TableNextColumn();
            const bool canLower = v.loaded && CheckAttribute(c, attribute, -1, v.attributePoints) == AssignBlock::None;
            const bool canRaise = v.loaded && CheckAttribute(c, attribute, +1, v.attributePoints) == AssignBlock::None;
            const PointButtons clicked =
                Buttons(canLower, !v.loaded ? away : std::string(canLower ? "Take a point back." : ""), canRaise,
                        !v.loaded ? away : std::string(canRaise ? "" : "No attribute points left."));
            if (clicked.lower)
                AssignAttributePoint(c.key, attribute, -1);
            if (clicked.raise)
                AssignAttributePoint(c.key, attribute, +1);
            Im::PopID();
        }
        Im::EndTable();
    }
}

// --- Perks -----------------------------------------------------------------------------------

std::string VerdictLine(const PerkNode &node)
{
    switch (node.effect)
    {
    case PerkEffect::Works:
        return "For companions: " + node.note + " Expected from the record; not yet measured on an NPC.";
    case PerkEffect::Situational:
        return "For companions: " + node.note;
    case PerkEffect::Unverified:
        return "For companions: unverified. " + node.note;
    case PerkEffect::NoEffect:
    default:
        return "No effect on companions: " + node.note;
    }
}

// The Needs cell: each requirement of the next rank, met ones dimmed, the
// alternatives of an OR group joined by "or".
void DrawNeeds(const PerkStatus &status, const PerkNode &node)
{
    if (status.block == PerkBlock::Maxed)
        return;
    if (status.block == PerkBlock::NoEffect)
    {
        const int req = node.ranks.empty() ? 0 : SkillRequirement(node.ranks.front(), node.skill);
        Im::TextDisabled("%s", status.bridge
                                   ? "No effect on companions: counted as learned for the tree"
                                   : fmt::format("No effect on companions: counted as learned once {} reaches {}",
                                                 Name(node.skill), req)
                                         .c_str());
        return;
    }
    bool first = true;
    for (std::size_t i = 0; i < status.requirements.size(); ++i)
    {
        const Requirement &r = status.requirements[i];
        const bool joinedByOr = i > 0 && r.alternative && status.requirements[i - 1].alternative &&
                                r.kind == status.requirements[i - 1].kind;
        if (!first)
        {
            Im::SameLine(0.0f, 0.0f);
            Im::TextDisabled("%s", joinedByOr ? " or " : "  \xC2\xB7  ");
            Im::SameLine(0.0f, 0.0f);
        }
        first = false;
        std::string text;
        switch (r.kind)
        {
        case Requirement::Kind::Skill:
            text = fmt::format("{} {}", Name(r.skill), r.need);
            if (!r.met)
                text += fmt::format(" (has {})", r.have);
            break;
        case Requirement::Kind::Perk:
        case Requirement::Kind::Other:
        default:
            text = r.name;
            break;
        }
        if (r.met)
            Im::TextDisabled("%s", text.c_str());
        else
            Im::Text("%s", text.c_str());
    }
}

void DrawTree(const Companion &c, const CompanionView &v, PageState &page, const PerkRules &rules, Skill skill)
{
    const PerkGraph &graph = rules.graph;
    const std::string id = "##tree" + std::string(Key(skill));
    if (!Im::BeginTable(id.c_str(), 4, kTable | Im::ImGuiTableFlags_SizingFixedFit))
        return;
    Im::TableSetupColumn("Perk", 0, 0.0f, 0);
    Im::TableSetupColumn("Learned", 0, 0.0f, 0);
    Im::TableSetupColumn("Needs", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    Im::TableSetupColumn("", 0, 0.0f, 0);
    Im::TableHeadersRow();
    for (const int nodeId : graph.Tree(skill))
    {
        const PerkNode &node = graph.Node(nodeId);
        const PerkStatus status = Status(rules, nodeId);
        if (page.learnableOnly && status.block != PerkBlock::None && status.held == 0)
            continue;
        Im::PushID(nodeId);
        Im::TableNextRow();

        Im::TableNextColumn();
        Im::AlignTextToFramePadding();
        const bool dim = status.block == PerkBlock::NoEffect || (status.held == 0 && status.block != PerkBlock::None);
        if (dim)
            Im::TextDisabled("%s", node.name.c_str());
        else
            Im::Text("%s", node.name.c_str());
        if (Im::IsItemHovered(0))
        {
            const std::size_t shown =
                static_cast<std::size_t>(std::clamp(status.held, 0, static_cast<int>(node.ranks.size()) - 1));
            Im::BeginTooltip();
            Im::PushTextWrapPos(420.0f);
            Im::TextWrapped("%s", node.ranks[shown].description.c_str());
            Im::Spacing();
            Im::TextDisabled("%s", VerdictLine(node).c_str());
            Im::PopTextWrapPos();
            Im::EndTooltip();
        }

        Im::TableNextColumn();
        const std::string learned =
            status.bridge ? std::string("-")
                          : (status.held > 0 ? fmt::format("{}/{}", status.held, node.ranks.size()) : std::string("-"));
        const bool setAside = IsSetAside(c, node);
        Im::Text("%s", learned.c_str());
        if (setAside)
        {
            Im::SameLine(0.0f, -1.0f);
            Im::TextDisabled("their own, set aside");
        }
        else if (status.innate > 0)
        {
            Im::SameLine(0.0f, -1.0f);
            Im::TextDisabled("%s", status.learned > 0 ? "(part their own)" : "their own");
        }

        Im::TableNextColumn();
        DrawNeeds(status, node);

        Im::TableNextColumn();
        if (page.confirm == nodeId)
        {
            Im::TextDisabled(
                "%s", fmt::format("Learn {}{}?", node.name,
                                  node.ranks.size() > 1 ? fmt::format(" (rank {})", status.held + 1) : std::string())
                          .c_str());
            Im::SameLine(0.0f, -1.0f);
            if (Im::SmallButton("Confirm"))
            {
                LearnPerk(c.key, nodeId);
                page.confirm = -1;
            }
            Im::SameLine(0.0f, -1.0f);
            if (Im::SmallButton("Cancel"))
                page.confirm = -1;
        }
        else if (setAside)
        {
            Im::BeginDisabled(!v.loaded);
            if (Im::SmallButton("Restore"))
                RestorePerk(c.key, nodeId);
            Im::EndDisabled();
            Tooltip(v.loaded ? "Takes it up again, for nothing: it was always on their record." : AwayReason(c, v));
        }
        else if (status.block == PerkBlock::None)
        {
            const std::string label = status.held > 0 ? fmt::format("Learn rank {}", status.held + 1) : "Learn";
            Im::BeginDisabled(!v.loaded);
            if (Im::SmallButton(label.c_str()))
                page.confirm = nodeId;
            Im::EndDisabled();
            Tooltip(AwayReason(c, v));
        }
        else if (status.canUnlearn)
        {
            Im::BeginDisabled(!v.loaded);
            if (Im::SmallButton("Unlearn"))
                UnlearnPerk(c.key, nodeId);
            Im::EndDisabled();
            Tooltip(!v.loaded ? AwayReason(c, v) : std::string("Gives the point back."));
        }
        else if (status.innate > 0 && status.learned == 0 && !status.bridge)
        {
            // One of their own. The engine is told it is not held; their
            // record keeps it, so this is undone for nothing (progression/game/PerkView.h).
            std::vector<FormKey> forms;
            for (const PerkRank &rank : node.ranks)
                if (v.onRecord.contains(rank.form))
                    forms.push_back(rank.form);
            const auto broken = WouldBreak(rules, forms);
            Im::BeginDisabled(!v.loaded || !broken.empty());
            if (Im::SmallButton("Set aside"))
                SetAsidePerk(c.key, nodeId);
            Im::EndDisabled();
            std::string why;
            for (const int b : broken)
                why += (why.empty() ? "" : ", ") + graph.Node(b).name;
            Tooltip(!v.loaded ? AwayReason(c, v)
                              : (broken.empty() ? "Stops them using it, for nothing; Restore takes it up again."
                                                : "Cannot be set aside while " + why + " needs it."));
        }
        else if (status.learned > 0 && !status.dependants.empty())
        {
            Im::TextDisabled("needed");
            std::string names;
            for (const int d : status.dependants)
                names += (names.empty() ? "" : ", ") + graph.Node(d).name;
            Tooltip("Cannot be unlearned while " + names + " needs it.");
        }
        Im::PopID();
    }
    Im::EndTable();
}

void DrawPerks(const Companion &c, const CompanionView &v, const Snapshot &s, PageState &page)
{
    const PerkGraph &graph = Graph();
    const Holdings holdings = HoldingsOf(c, v.onRecord);
    const PerSkill<int> skills = Effective(c, v.base);
    const int points = v.perkPoints;
    const PerkRules rules{graph, holdings, skills, points, s.settings.showNoEffectPerks};

    Im::AlignTextToFramePadding();
    const std::string pointsText = points > 0 ? PointsText(points) : std::string("No perk points to spend");
    Im::Text("%s", pointsText.c_str());
    Im::SameLine(0.0f, 24.0f);
    Im::Checkbox("Learnable now only", &page.learnableOnly);

    // Trees with something held first, then the rest.
    std::vector<Skill> order;
    std::vector<Skill> held;
    std::vector<Skill> rest;
    for (const Skill skill : AllSkills())
    {
        if (!IsTrainable(skill) && !s.settings.showNoEffectPerks)
            continue;
        bool any = false;
        for (const int id : graph.Tree(skill))
            for (const PerkRank &rank : graph.Node(id).ranks)
                any = any || holdings.innate.contains(rank.form) || holdings.learned.contains(rank.form);
        (any ? held : rest).push_back(skill);
    }
    order.insert(order.end(), held.begin(), held.end());
    order.insert(order.end(), rest.begin(), rest.end());

    for (const Skill skill : order)
    {
        const bool open = std::find(held.begin(), held.end(), skill) != held.end();
        // Where a point can go, without opening each tree: the one thing the
        // prototype added to the design that earned its place.
        int heldCount = 0;
        int ready = 0;
        const auto tree = graph.Tree(skill);
        for (const int id : tree)
        {
            const PerkStatus st = Status(rules, id);
            heldCount += st.held > 0 && !st.bridge ? 1 : 0;
            ready += st.block == PerkBlock::None ? 1 : 0;
        }
        const std::string title = fmt::format(
            "{} {}  \xC2\xB7  {} of {} held{}###perks{}", Name(skill), skills[Index(skill)], heldCount, tree.size(),
            ready > 0 && points > 0 ? fmt::format(", {} ready", ready) : std::string(), Key(skill));
        if (Im::CollapsingHeader(title.c_str(), open ? Im::ImGuiTreeNodeFlags_DefaultOpen : 0))
            DrawTree(c, v, page, rules, skill);
    }
}

// --- Spells ------------------------------------------------------------------------------------

void DrawSpells(const Companion &c, const CompanionView &v, const Snapshot &s, PageState &page)
{
    (void)s;
    Heading("Teach from a tome");
    if (v.tomes.empty())
        Im::TextDisabled("You carry no spell tomes.");
    else if (Im::BeginTable("##tomes", 5, kTable | Im::ImGuiTableFlags_SizingFixedFit))
    {
        Im::TableSetupColumn("Spell", 0, 0.0f, 0);
        Im::TableSetupColumn("School", 0, 0.0f, 0);
        Im::TableSetupColumn("Level", 0, 0.0f, 0);
        Im::TableSetupColumn("Cost", 0, 0.0f, 0);
        Im::TableSetupColumn("", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableHeadersRow();
        int row = 0;
        for (const TomeRow &t : v.tomes)
        {
            Im::PushID(row++);
            Im::TableNextRow();
            Im::TableNextColumn();
            Im::AlignTextToFramePadding();
            Im::Text("%s", t.facts.name.c_str());
            Tooltip(t.bookName + (t.count > 1 ? " (you carry " + std::to_string(t.count) + ")" : ""));
            Im::TableNextColumn();
            Im::Text("%s", t.facts.school ? std::string(Name(*t.facts.school)).c_str() : "-");
            Im::TableNextColumn();
            Im::Text("%s", std::string(LevelName(t.facts.minimumSkill)).c_str());
            Im::TableNextColumn();
            Im::Text("%d", t.facts.cost);
            Im::TableNextColumn();
            switch (t.status.block)
            {
            case TeachBlock::None:
                if (page.confirmTeach == ToString(t.facts.spell))
                {
                    Im::Text("%s",
                             fmt::format("Teach {}? One tome is used ({} carried).", t.facts.name, t.count).c_str());
                    Im::SameLine(0.0f, -1.0f);
                    if (Im::SmallButton("Confirm"))
                    {
                        Teach(c.key, t.facts.spell);
                        page.confirmTeach.clear();
                    }
                    Im::SameLine(0.0f, -1.0f);
                    if (Im::SmallButton("Cancel"))
                        page.confirmTeach.clear();
                    break;
                }
                Im::BeginDisabled(!v.loaded);
                if (Im::SmallButton("Teach"))
                    page.confirmTeach = ToString(t.facts.spell);
                Im::EndDisabled();
                Tooltip(v.loaded ? "Uses one tome. The book is the teacher: you need not know the spell yourself."
                                 : AwayReason(c, v));
                break;
            case TeachBlock::Skill:
                Im::TextDisabled("%s",
                                 fmt::format("Needs {} {} (has {})", t.facts.school ? Name(*t.facts.school) : "skill",
                                             t.status.need, t.status.have)
                                     .c_str());
                break;
            case TeachBlock::Magicka:
                Im::TextDisabled("%s", fmt::format("Needs {} magicka (has {})", t.status.need, t.status.have).c_str());
                break;
            case TeachBlock::Known:
                Im::TextDisabled("Already knows");
                break;
            case TeachBlock::NotTeachable:
            default:
                Im::TextDisabled("Not a spell a companion can be taught");
                break;
            }
            Im::PopID();
        }
        Im::EndTable();
    }

    Heading("Known spells");
    if (v.spells.empty())
        Im::TextDisabled("No spells.");
    else if (Im::BeginTable("##known", 5, kTable | Im::ImGuiTableFlags_SizingFixedFit))
    {
        Im::TableSetupColumn("Spell", 0, 0.0f, 0);
        Im::TableSetupColumn("School", 0, 0.0f, 0);
        Im::TableSetupColumn("Level", 0, 0.0f, 0);
        Im::TableSetupColumn("From", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        Im::TableSetupColumn("", 0, 0.0f, 0);
        Im::TableHeadersRow();
        int row = 0;
        for (const KnownSpellRow &k : v.spells)
        {
            Im::PushID(row++);
            Im::TableNextRow();
            Im::TableNextColumn();
            Im::AlignTextToFramePadding();
            Im::Text("%s", k.facts.name.c_str());
            Im::TableNextColumn();
            Im::Text("%s", k.facts.school ? std::string(Name(*k.facts.school)).c_str() : "-");
            Im::TableNextColumn();
            Im::Text("%s", std::string(LevelName(k.facts.minimumSkill)).c_str());
            Im::TableNextColumn();
            if (k.taught)
                Im::Text("Taught from a tome");
            else if (k.setAside)
                Im::TextDisabled("Their own, set aside");
            else
                Im::TextDisabled("%s", k.onRecord ? "Their own" : "Added by something else");
            Im::TableNextColumn();
            if (k.taught)
            {
                Im::BeginDisabled(!v.loaded);
                if (Im::SmallButton("Forget"))
                    Forget(c.key, k.facts.spell);
                Im::EndDisabled();
                Tooltip(v.loaded ? "The tome is not given back." : AwayReason(c, v));
            }
            else if (k.setAside)
            {
                Im::BeginDisabled(!v.loaded);
                if (Im::SmallButton("Restore"))
                    RestoreOwnSpell(c.key, k.facts.spell);
                Im::EndDisabled();
                Tooltip(v.loaded ? "Takes it up again, for nothing: it was always on their record." : AwayReason(c, v));
            }
            else if (k.onRecord && k.facts.ordinary)
            {
                const bool hooked = spellview::Installed();
                Im::BeginDisabled(!v.loaded || !hooked);
                if (Im::SmallButton("Set aside"))
                    SetAsideOwnSpell(c.key, k.facts.spell);
                Im::EndDisabled();
                Tooltip(!hooked     ? "Needs the spell hooks, which are not installed (Skyrim VR)."
                        : !v.loaded ? AwayReason(c, v)
                                    : "They stop knowing it, for nothing; Restore takes it up again. Their "
                                      "record keeps it.");
            }
            Im::PopID();
        }
        Im::EndTable();
    }
    if (TacticsInstalled())
        Im::TextDisabled("Follower Tactics decides when a spell is cast: give it a rule on %s's Tactics tab.",
                         c.name.c_str());
}

// --- a companion's page ------------------------------------------------------------------------------

void DrawCompanion(const FormKey &key)
{
    NoteDrawn();
    if (key.Empty())
    {
        Im::TextDisabled("Nobody is assigned to this entry.");
        return;
    }
    const Snapshot &s = Snap();
    if (!s.inGame)
    {
        Im::TextDisabled("No game is loaded.");
        return;
    }
    const Found found = FindIn(s, key);
    if (!found.companion)
    {
        Im::TextDisabled("Not a companion in this save.");
        return;
    }
    const Companion &c = *found.companion;
    PageState &page = PageOf(key);
    DrawHeader(c, found.view, s, page);
    if (s.settings.released)
        Im::TextColored(Im::ImVec4(0.90f, 0.62f, 0.45f, 1.0f),
                        "Leveling is off: nothing here is on them now. Follower Tactics' Settings turns it on.");
    if (c.paused)
    {
        if (Im::SmallButton("Resume earning"))
            SetPaused(key, false);
    }
    Im::Spacing();
    if (!found.view || !found.view->read)
    {
        Im::TextDisabled("Their sheet is read when they are nearby.");
        return;
    }
    const CompanionView &v = *found.view;
    if (!Im::BeginTabBar("##companion", 0))
        return;
    if (Im::BeginTabItem("Skills", nullptr, 0))
    {
        DrawSkills(c, v, s, page);
        Im::EndTabItem();
    }
    const std::string perksTab =
        v.perkPoints > 0 ? fmt::format("Perks ({})###perks", v.perkPoints) : std::string("Perks###perks");
    if (Im::BeginTabItem(perksTab.c_str(), nullptr, 0))
    {
        DrawPerks(c, v, s, page);
        Im::EndTabItem();
    }
    if (Im::BeginTabItem("Spells", nullptr, 0))
    {
        DrawSpells(c, v, s, page);
        Im::EndTabItem();
    }
    Im::EndTabBar();
}

// --- the Overview --------------------------------------------------------------------------------------

void DrawOverview()
{
    NoteDrawn();
    const Snapshot &s = Snap();
    if (!s.inGame)
    {
        Im::TextDisabled("No game is loaded.");
        return;
    }
    if (s.companions.empty() && s.candidates.empty())
    {
        Im::TextWrapped("No companions yet. Followers are enrolled when they join you%s.",
                        s.settings.autoEnroll ? "" : " and you enrol them here");
        return;
    }
    if (!Im::BeginTable("##overview", 6, kTable | Im::ImGuiTableFlags_SizingFixedFit))
        return;
    Im::TableSetupColumn("Companion", 0, 0.0f, 0);
    Im::TableSetupColumn("Level", 0, 0.0f, 0);
    Im::TableSetupColumn("Experience", 0, 0.0f, 0);
    Im::TableSetupColumn("To assign", Im::ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    Im::TableSetupColumn("Status", 0, 0.0f, 0);
    Im::TableSetupColumn("", 0, 0.0f, 0);
    Im::TableHeadersRow();
    for (std::size_t i = 0; i < s.companions.size(); ++i)
    {
        const Companion &c = s.companions[i];
        const CompanionView *v = i < s.views.size() ? &s.views[i] : nullptr;
        Im::PushID(static_cast<int>(i));
        Im::TableNextRow();
        Im::TableNextColumn();
        Im::AlignTextToFramePadding();
        Im::Text("%s", c.name.c_str());
        Im::TableNextColumn();
        if (v && v->read)
        {
            const LevelProgress &p = v->progress;
            Im::Text("%d", p.level);
            Im::TableNextColumn();
            Bar(p.toNext > 0.0 ? static_cast<float>(p.into / p.toNext) : 0.0f, 170.0f,
                Thousands(static_cast<std::int64_t>(p.into)) + " / " + Thousands(static_cast<std::int64_t>(p.toNext)));
            Im::TableNextColumn();
            Im::Text("%s", ToAssign(v->perkPoints, v->attributePoints, c.learning.pool).c_str());
        }
        else
        {
            Im::TextDisabled("-");
            Im::TableNextColumn();
            Im::TableNextColumn();
        }
        Im::TableNextColumn();
        Im::TextDisabled("%s", StatusText(c, v).c_str());
        Im::TableNextColumn();
        if (Im::SmallButton(c.paused ? "Resume" : "Pause"))
            SetPaused(c.key, !c.paused);
        Tooltip(c.paused ? "Learn by doing again." : "Stop learning by doing; everything learned is kept.");
        Im::PopID();
    }
    int row = 1000;
    for (const Candidate &candidate : s.candidates)
    {
        Im::PushID(row++);
        Im::TableNextRow();
        Im::TableNextColumn();
        Im::AlignTextToFramePadding();
        Im::Text("%s", candidate.name.c_str());
        Im::TableNextColumn();
        Im::TextDisabled("-");
        Im::TableNextColumn();
        Im::TextDisabled("not enrolled");
        Im::TableNextColumn();
        Im::TableNextColumn();
        Im::TableNextColumn();
        Im::TextDisabled("With you");
        Im::TableNextColumn();
        Im::BeginDisabled(!candidate.unique);
        if (Im::SmallButton("Enroll"))
            Enroll(candidate.key);
        Im::EndDisabled();
        Tooltip(candidate.unique ? "Start learning from what they do with you."
                                 : "This build trains unique followers only.");
        Im::PopID();
    }
    Im::EndTable();
    Im::TextDisabled("Everyone this save knows, with you or not. A companion's page stays readable while they are "
                     "away; learning waits for them to come back.");
    if (!s.refused.empty())
        Im::TextColored(Im::ImVec4(0.90f, 0.62f, 0.45f, 1.0f), "%s", s.refused.c_str());
}

// --- Settings ----------------------------------------------------------------------------------------------

void DrawSettings()
{
    const Snapshot &s = Snap();
    const float fontSize = Im::GetFontSize();
    Im::SetWindowFontScale(1.0f);
    const float baseSize = Im::GetFontSize();
    const float scale = baseSize > 0.0f ? fontSize / baseSize : 1.0f;
    Im::SetWindowFontScale(scale * 1.35f);
    Im::Text("Follower Progression");
    Im::SetWindowFontScale(scale);
    Im::TextDisabled("A proof of concept: nothing here has been verified in play yet.");
    Im::Separator();
    if (!s.inGame)
    {
        Im::TextDisabled("No game is loaded. Settings are kept with each save.");
        return;
    }

    Settings next = s.settings;
    Im::Checkbox("Enroll new followers automatically", &next.autoEnroll);
    Heading("Learning");
    const Rules &r = s.rules;
    Im::TextWrapped("%s", fmt::format("Your own leveling's rules, read from the game now: a skill's next level "
                                      "takes improve x level ^ {:.2f} + offset skill XP, a skill-up gives {:g} x its "
                                      "level, a level takes {:g} + {:g} x level, and brings {} to an attribute. A mod "
                                      "that changes them for you changes them for companions. Learning takes them at "
                                      "most {} levels past yours.",
                                      r.skillUseCurve, r.xpPerSkillRank, r.levelUpBase, r.levelUpMult,
                                      r.attributePerLevel, r.levelsAbovePlayer)
                              .c_str());
    Heading("Notifications");
    Im::Checkbox("Level-ups and what they bring", &next.notifyLevels);
    Im::Checkbox("Each skill increase", &next.notifySkills);
    Heading("Perks");
    Im::Checkbox("Offer perks with no effect on companions, and the crafting trees", &next.showNoEffectPerks);
    if (!(next == s.settings))
        ChangeSettings(next);

    Heading("Testing");
    Im::TextWrapped("For trying the proof of concept: character experience without doing anything for it.");
    if (Im::Button("Give 1,000 experience to everyone following"))
        Gift({}, 1000.0);
    Im::SameLine(0.0f, -1.0f);
    if (Im::Button("Give 10,000"))
        Gift({}, 10000.0);
    const learning::Counters heard = learning::Count();
    const std::string heardText =
        !learning::Installed()
            ? std::string("Learning hooks: not installed (Skyrim VR)")
            : fmt::format("Skill uses heard: {} from magic, {} from blows landed, {} from hits taken", heard.magic,
                          heard.blows, heard.struck);
    Im::TextDisabled("%s", heardText.c_str());
    if (Im::Button("Check perks and spells against the engine"))
        CheckViews();
    Tooltip("Asks the engine's own HasPerk and HasSpell about every perk and spell of each companion here, and "
            "compares them with what they should hold. The details go to FollowerTactics.log.");
    const perkview::Counters n = perkview::Count();
    const std::string hooks =
        !perkview::Installed()
            ? std::string("Perk hooks: not installed (Skyrim VR), so perks are not changed")
            : fmt::format("Perk hooks: {} walks answered for companions, {} process builds ({} companions'), {} rank "
                          "changes queued",
                          n.forEachPerkManaged, n.applyFromBase, n.applyFromBaseManaged, n.queued);
    Im::TextDisabled("%s", hooks.c_str());
    const spellview::Counters sn = spellview::Count();
    const std::string spellHooks =
        !spellview::Installed()
            ? std::string("Spell hooks: not installed, so taught spells go onto the actor and none can be set aside")
            : fmt::format("Spell hooks: {} walks answered for companions ({} by the combat AI), {} casts of a "
                          "set-aside spell refused",
                          sn.visitsManaged, sn.gathersManaged, sn.castsRefused);
    Im::TextDisabled("%s", spellHooks.c_str());
    if (Im::Button("Write the perk trees to the log folder"))
        DumpPerks();
    Tooltip("FollowerTactics.perks.json, beside FollowerTactics.log: the trees as this load order has "
            "them, to compare with tests/progression/data/vanilla-perks.json.");

    Heading("Leveling on and off");
    Im::TextWrapped("Follower Tactics' Settings page has the switch. Off takes everything of this off your "
                    "companions as each is near and keeps the record of it; on puts it back.");
}

// --- menu entries ------------------------------------------------------------------------------------------
//
// A render callback takes no argument, so each companion's entry needs its
// own function: a fixed pool of trampolines, one per slot, as Follower
// Tactics does it (its UI.cpp, "menu entries").

std::mutex g_slotMutex;
std::array<FormKey, kSlots> g_owner;
std::array<std::string, kSlots> g_entryName;

FormKey OwnerOf(std::size_t slot)
{
    std::scoped_lock lock(g_slotMutex);
    return g_owner[slot];
}

template <std::size_t N> void __stdcall RenderSlot()
{
    DrawCompanion(OwnerOf(N));
}

template <std::size_t... N>
constexpr std::array<SKSEMenuFramework::Model::RenderFunction, sizeof...(N)> Renderers(std::index_sequence<N...>)
{
    return {RenderSlot<N>...};
}

void __stdcall RenderOverview()
{
    DrawOverview();
}

void __stdcall RenderSettings()
{
    DrawSettings();
}

void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType type)
{
    using Event = SKSEMenuFramework::Model::EventType;
    switch (type)
    {
    case Event::kOpenMenu:
        PanelShown(true);
        g_resetPages.store(true, std::memory_order_relaxed);
        break;
    case Event::kCloseMenu:
        PanelShown(false);
        g_resetPages.store(true, std::memory_order_relaxed);
        break;
    case Event::kBeforeRender:
        // A question left open, or a note from the last visit, does not
        // outlive the panel it was asked in.
        if (g_resetPages.exchange(false, std::memory_order_relaxed))
            for (auto &[key, page] : g_pages)
            {
                page.confirm = -1;
                page.confirmTeach.clear();
                page.note.clear();
            }
        break;
    default:
        break;
    }
}

// A menu path's component: the framework splits paths on '/'.
std::string EntryName(const std::string &name)
{
    std::string out = name.empty() ? std::string("Companion") : name;
    std::replace(out.begin(), out.end(), '/', '-');
    return out;
}

} // namespace

void SyncEntries()
{
    if (!SKSEMenuFramework::IsInstalled())
        return;
    static const auto renderers = Renderers(std::make_index_sequence<kSlots>{});
    static std::uint64_t seen = 0;
    const std::uint64_t now = Version();
    if (now == seen)
        return;
    seen = now;
    const Snapshot snapshot = Read();

    // Slots are claimed under the lock; the entries are added after it is
    // released. The render callbacks take the same lock (OwnerOf) from
    // inside the framework's drawing, and the framework may hold its own
    // lock over its tree while it draws: adding under ours would invert the
    // order (Follower Tactics' SyncFollowers does the same).
    std::vector<std::pair<std::size_t, std::string>> added;
    {
        std::scoped_lock lock(g_slotMutex);
        for (const Companion &c : snapshot.companions)
        {
            if (std::find(g_owner.begin(), g_owner.end(), c.key) != g_owner.end())
                continue;
            const auto free = std::find_if(g_owner.begin(), g_owner.end(), [](const FormKey &k) { return k.Empty(); });
            if (free == g_owner.end())
            {
                static bool said = false;
                if (!said)
                    log::ui.warn("all {} companion entries are taken; {} has none", kSlots, c.name);
                said = true;
                continue;
            }
            const std::size_t slot = static_cast<std::size_t>(free - g_owner.begin());
            const std::string base = EntryName(c.name);
            std::string name = base;
            int suffix = 2;
            while (std::find(g_entryName.begin(), g_entryName.end(), name) != g_entryName.end())
                name = base + " (" + std::to_string(suffix++) + ")";
            g_owner[slot] = c.key;
            g_entryName[slot] = name;
            added.emplace_back(slot, name);
        }
    }
    for (const auto &[slot, name] : added)
    {
        std::string path = kSection;
        path += "/Companions/";
        path += name;
        SKSEMenuFramework::FullPathAddSectionItem(path, renderers[slot]);
        log::ui.debug("menu entry for {} in slot {}", name, slot);
    }
}

void Install()
{
    if (!SKSEMenuFramework::IsInstalled())
    {
        log::ui.info("SKSE Menu Framework is not installed: no panel. Progression still runs.");
        return;
    }
    static const auto *events = SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
    (void)events;
    // Under Tactics' section, after its Settings and Player entries (an
    // entry cannot be moved once added, so this runs after Tactics' own).
    SKSEMenuFramework::FullPathAddSectionItem(kSection + "/Overview", RenderOverview);
    SKSEMenuFramework::FullPathAddSectionItem(kSection + "/Settings", RenderSettings);
    log::ui.info("registered under Follower Tactics in SKSE Menu Framework (F1)");
}

} // namespace fp::game::ui
