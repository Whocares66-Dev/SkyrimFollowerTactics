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
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>

namespace ft::game::ui
{
namespace
{

namespace Im = ImGuiMCP;

// --- status ----------------------------------------------------------------

// Did the condition hold?
//
// Three answers, not two, and the third is the one that matters. Look at the
// order of checks in Evaluate(): Disabled, Unsupported and InvalidCondition are
// all decided BEFORE the condition is ever evaluated, and NotReached means an
// earlier rule fired so this one was never looked at. For those four we
// genuinely do not know whether the condition holds, and saying "false" would
// be inventing an answer.
enum class Held
{
    Yes,
    No,
    NotEvaluated,
};

Held ConditionHeld(ft::Verdict v)
{
    switch (v)
    {
    case ft::Verdict::ConditionFalse:
        return Held::No;

    // Decided before the condition was reached.
    case ft::Verdict::Disabled:
    case ft::Verdict::Unsupported:
    case ft::Verdict::InvalidCondition:
    case ft::Verdict::NotReached:
        return Held::NotEvaluated;

    // Everything else -- fired, or blocked by a cooldown, a missing potion, an
    // unresolvable target -- is only reached once the condition came back true.
    default:
        return Held::Yes;
    }
}

// A short explanation beside the status, or nothing when the status already
// says everything: a true condition that fired, or a plain false.
const char *StatusNote(ft::Verdict v)
{
    switch (v)
    {
    case ft::Verdict::Fired:
    case ft::Verdict::ConditionFalse:
        return nullptr;
    case ft::Verdict::Disabled:
        return "rule is turned off";
    case ft::Verdict::NotReached:
        return "an earlier rule fired";
    case ft::Verdict::InvalidCondition:
        return "this subject and condition do not go together";
    case ft::Verdict::Unsupported:
        return "action not available in this build";
    default:
        return ft::ToString(v);
    }
}

// --- rule text -------------------------------------------------------------

// "Self health below 50%" -- the rule's condition as a player would read it,
// assembled from the display names rather than the wire slugs.
std::string ConditionText(const ft::Rule &r)
{
    std::string text(ft::DisplayName(r.subject));
    text += ' ';
    text += ft::DisplayName(r.predicate);

    switch (ft::ArgumentFor(r.predicate))
    {
    case ft::ArgumentKind::Percent:
        text += ' ' + std::to_string(static_cast<int>(r.conditionArg * 100.0f + 0.5f)) + '%';
        break;
    case ft::ArgumentKind::Distance:
        text += ' ' + std::to_string(static_cast<int>(r.conditionArg)) + " units";
        break;
    case ft::ArgumentKind::Count:
        text += ' ' + std::to_string(static_cast<int>(r.conditionArg));
        break;
    case ft::ArgumentKind::None:
        break;
    }
    return text;
}

// The current value the condition looks at -- just the value.
//
// Deliberately does NOT restate the threshold: the "If" column already says
// "health below 50%", so "72% vs 50%" is noise. What the player cannot
// otherwise see is the 72%.
std::string LiveValueText(const ft::Rule &r, const ft::Snapshot &s)
{
    const auto pct = [](float v) { return std::to_string(static_cast<int>(v * 100.0f + 0.5f)) + '%'; };

    switch (r.predicate)
    {
    case ft::PredicateKind::HealthPctBelow:
        if (r.subject == ft::SubjectKind::Self)
            return pct(s.health.Pct());
        if (r.subject == ft::SubjectKind::Player)
            return pct(s.playerHealth.Pct());
        return "-";
    case ft::PredicateKind::MagickaPctBelow:
        return pct(s.magicka.Pct());
    case ft::PredicateKind::StaminaPctBelow:
        return pct(s.stamina.Pct());
    case ft::PredicateKind::InCombat:
        return s.inCombat ? "yes" : "no";
    case ft::PredicateKind::InBleedout:
        return s.inBleedout ? "yes" : "no";
    case ft::PredicateKind::CountAtLeast:
        return std::to_string(r.subject == ft::SubjectKind::Enemy ? s.enemies.size() : s.allies.size());
    default:
        return "-";
    }
}

// --- drawing ---------------------------------------------------------------

// Width of the widest of a set of labels. Measured rather than hardcoded so the
// columns survive translation, where "Magicka" might be "Zauberkraft".
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

void DrawRuleTable(const ft::RuleSet &rules, const FollowerView &view)
{
    constexpr auto flags =
        Im::ImGuiTableFlags_Borders | Im::ImGuiTableFlags_RowBg | Im::ImGuiTableFlags_SizingStretchProp;

    if (!Im::BeginTable("rules", 6, flags, Im::ImVec2(0.0f, 0.0f), 0.0f))
        return;

    Im::TableSetupColumn("On", Im::ImGuiTableColumnFlags_WidthFixed, 42.0f, 0);
    Im::TableSetupColumn("#", Im::ImGuiTableColumnFlags_WidthFixed, 26.0f, 0);
    Im::TableSetupColumn("If", Im::ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
    Im::TableSetupColumn("Now", Im::ImGuiTableColumnFlags_WidthFixed, 70.0f, 0);
    Im::TableSetupColumn("Then", Im::ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
    Im::TableSetupColumn("Status", Im::ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
    Im::TableHeadersRow();

    for (std::size_t i = 0; i < rules.rules.size(); ++i)
    {
        const auto &rule = rules.rules[i];
        Im::TableNextRow(0, 0.0f);

        // The author's switch. Read-only until profiles can be saved -- offering
        // a toggle that silently forgets itself on reload would be worse than
        // not offering one. Drawn now so the layout is settled before editing
        // lands.
        //
        // Two things about drawing it inside a table cell:
        //
        //  * ImGui's checkbox draws its own frame, and the cell already has a
        //    border, so the default is a box inside a box. Dropping the widget's
        //    own border leaves the fill and the tick, which reads as one thing.
        //  * Nothing centres it for you. The cell is wider than the widget, so
        //    the offset has to be computed: half the leftover width, where the
        //    widget is square and GetFrameHeight() is its side.
        Im::TableSetColumnIndex(0);
        {
            const float cell = Im::GetContentRegionAvail().x;
            const float widget = Im::GetFrameHeight();
            if (cell > widget)
                Im::SetCursorPosX(Im::GetCursorPosX() + (cell - widget) * 0.5f);

            Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
            bool ruleEnabled = rule.enabled;
            Im::BeginDisabled(true);
            Im::Checkbox(("##on" + std::to_string(i)).c_str(), &ruleEnabled);
            Im::EndDisabled();
            Im::PopStyleVar(1);
        }

        Im::TableSetColumnIndex(1);
        Im::Text("%zu", i + 1);

        Im::TableSetColumnIndex(2);
        if (rule.enabled)
            Im::Text("%s", ConditionText(rule).c_str());
        else
            Im::TextDisabled("%s", ConditionText(rule).c_str());
        if (Im::IsItemHovered(0) && !rule.label.empty())
            Im::SetTooltip("%s", rule.label.c_str());

        Im::TableSetColumnIndex(3);
        Im::Text("%s", view.evaluated ? LiveValueText(rule, view.snapshot).c_str() : "-");

        Im::TableSetColumnIndex(4);
        // Red here, rather than greying the whole row, because this is the cell
        // that has to change. A dimmed row hides the very information needed to
        // fix it. Only for rules that can NEVER work -- being out of potions is
        // transient, self-correcting, and must not be dressed up as an error.
        const auto rowVerdict = i < view.trace.size() ? view.trace[i] : ft::Verdict::NotReached;
        const bool broken =
            view.evaluated && (rowVerdict == ft::Verdict::Unsupported || rowVerdict == ft::Verdict::InvalidCondition);

        if (broken)
            Im::TextColored(Im::ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                            std::string(ft::DisplayName(rule.action)).c_str());
        else if (!rule.enabled)
            Im::TextDisabled("%s", std::string(ft::DisplayName(rule.action)).c_str());
        else
            Im::Text("%s", std::string(ft::DisplayName(rule.action)).c_str());

        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", broken ? StatusNote(rowVerdict) : std::string(ft::Describe(rule.action)).c_str());

        Im::TableSetColumnIndex(5);
        if (!view.evaluated)
        {
            // A stale verdict here would be a lie: it is from whenever this
            // follower was last in a fight, possibly an hour ago.
            Im::TextDisabled("-");
        }
        else
        {
            const auto verdict = rowVerdict;

            switch (ConditionHeld(verdict))
            {
            case Held::Yes:
                Im::TextColored(Im::ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "true");
                break;
            case Held::No:
                Im::TextColored(Im::ImVec4(0.55f, 0.55f, 0.58f, 1.0f), "false");
                break;
            case Held::NotEvaluated:
                Im::TextDisabled("-");
                break;
            }

            if (const char *note = StatusNote(verdict))
            {
                Im::SameLine(0.0f, 6.0f);
                const bool broken = verdict == ft::Verdict::InvalidCondition || verdict == ft::Verdict::Unsupported;
                Im::TextColored(broken ? Im::ImVec4(0.95f, 0.45f, 0.40f, 1.0f) : Im::ImVec4(0.85f, 0.75f, 0.40f, 1.0f),
                                "(%s)", note);
            }
        }
    }

    Im::EndTable();
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
    if (Im::Checkbox("Tactics enabled for this follower", &followerEnabled))
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
    DrawRuleTable(rules, view);
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
            DrawFollower(ActiveRuleSet(), view);
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
