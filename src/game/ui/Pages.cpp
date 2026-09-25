// A follower's page and the player's (Pages.h).

#include "game/ui/Pages.h"
#include "game/ui/Panel.h"
#include "game/ui/Sections.h"
#include "game/ui/Tabs.h"
#include "game/ui/UI.h"
#include "game/ui/Widgets.h"

#include "game/Addresses.h"
#include "game/Log.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>

namespace ft::game::ui
{
namespace
{

// A tab's content in a scrolling region of its own under the tab bar, so the
// tabs stay in place when the page scrolls. By actor: each page keeps its own
// place in its list. EndChild whether or not the region is visible, as ImGui
// asks.
//
// A tab that opens a page in its list's place names it by `detail`, the page
// open or zeros for the list. The region is ImGui's keeper of the scroll, so a
// page in the list's own region scrolled it, clamped to the page's shorter
// height, and the back arrow came back to a list moved up.
//
// A borderless child gets no padding, so a table as wide as the region put
// its right border on the clip edge and lost it, scrollbar or not. A couple
// of pixels either side keeps the border inside; none top or bottom, where
// nothing is clipped. Popped before drawing, so the tab's own popups and
// tooltips keep the style's padding.
// Whose page is being drawn and which tab of it, packed into one word so the
// game thread always reads a pair that belongs together; 0 when no page of
// ours is on screen. Written on the render thread, read by the tick through
// Shown().
std::atomic<std::uint64_t> g_shownNow{0};
// Whether this frame drew a page of ours, between the framework's before- and
// after-render events.
std::atomic<bool> g_drawnThisFrame{false};

std::uint64_t Packed(ft::ActorId actor, Tab tab)
{
    return (static_cast<std::uint64_t>(actor) << 8) | static_cast<std::uint64_t>(tab);
}

// The input queue's hand-off to its sinks, on the game's input thread.
// The framework rewrites the same call when it loads and closes its menu
// on any Escape event there, before its callbacks and before ImGui sees
// the key; ours is written at data load, after its, so the call reaches
// ours first, and an Escape pressed with the cursor in a filter box is
// unlinked from the queue before the framework reads it. The events of
// the same press that follow -- held, released -- go too: the framework
// closes on those as well.
using DispatchInputQueue = void (*)(RE::BSTEventSource<RE::InputEvent *> *, RE::InputEvent *const *);
DispatchInputQueue g_dispatchInputQueue = nullptr;

void TakeEscape(RE::InputEvent **link)
{
    while (RE::InputEvent *event = *link)
    {
        const RE::ButtonEvent *button = event->AsButtonEvent();
        const bool escape = button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
                            button->GetIDCode() == static_cast<std::uint32_t>(RE::BSKeyboardDevice::Key::kEscape);
        bool take = false;
        if (escape && g_escapeTaken.load(std::memory_order_relaxed))
        {
            take = true;
            if (button->IsUp())
                g_escapeTaken.store(false, std::memory_order_relaxed);
        }
        else if (escape && button->IsDown() && g_filterActive.load(std::memory_order_relaxed))
        {
            take = true;
            g_escapeTaken.store(true, std::memory_order_relaxed);
            g_leaveFilter.store(true, std::memory_order_relaxed);
        }
        if (take)
            *link = event->next;
        else
            link = &event->next;
    }
}

void DispatchInputQueueHook(RE::BSTEventSource<RE::InputEvent *> *source, RE::InputEvent *const *events)
{
    // The head lives in the caller's frame, and the framework rewrites it
    // the same way.
    if (events)
        TakeEscape(const_cast<RE::InputEvent **>(events));
    g_dispatchInputQueue(source, events);
}

void TabBody(Tab tab, ft::ActorId actor, const std::function<void()> &draw,
             std::initializer_list<std::uint64_t> detail = {})
{
    ShowingPage(actor, tab);

    std::string id = std::string("##tab/") + Name(tab) + "/" + std::to_string(actor);
    if (std::any_of(detail.begin(), detail.end(), [](std::uint64_t part) { return part != 0; }))
        for (const std::uint64_t part : detail)
            id += "/" + std::to_string(part);
    Im::PushStyleVar(Im::ImGuiStyleVar_WindowPadding, Im::ImVec2(2.0f, 0.0f));
    const bool open = Im::BeginChild(id.c_str(), Im::ImVec2(0.0f, 0.0f), Im::ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    Im::PopStyleVar(1);
    // Escape, taken from the queue while the cursor was in the filter box,
    // takes the cursor out and empties the box. The box's own answer to the
    // key is neither: it puts the text back as it was when the cursor went
    // in, and the panel never sees the key anyway.
    if (g_leaveFilter.exchange(false, std::memory_order_relaxed))
    {
        Im::ClearActiveID();
        g_clearFilter = true;
    }
    // Space, with the cursor in no box, puts it in this body's filter box.
    // Cleared after the body: a tab with no box has nowhere to put it, and
    // the next tab drawn must not inherit it.
    g_focusFilter = !Im::IsAnyItemActive() && Im::IsKeyPressed(Im::ImGuiKey_Space, false);
    g_chipsDrawn = false;
    if (open)
        draw();
    g_focusFilter = false;
    g_clearFilter = false;
    // A body with no chip strip has no row to go down to, so the keys stay
    // on the tabs whatever S asked for. This is what makes S safe to answer
    // before the body is drawn, which is where it has to be answered: the
    // strip is inside the body and needs this frame's step.
    if (!g_chipsDrawn)
        g_keyRow = KeyRow::Tabs;
    g_chipStep = 0;
    Im::EndChild();
}

// The tab a page opens on, on the frame it is drawn after another page's;
// None on the frames after, when its bar keeps the choice. The first page
// drawn opens on Tactics, which is what the mod is for. The player's page
// has no Combat Style, and opens on Tactics from it.
Tab CarriedTab(const CharacterView &view)
{
    if (view.id == g_shownPage)
        return Tab::None;
    g_shownPage = view.id;
    if (g_shownTab == Tab::None || (view.player && g_shownTab == Tab::CombatStyle))
        return Tab::Tactics;
    return g_shownTab;
}

// The bar's tabs in the order they are drawn: DrawSheetTabs' seven, then
// Combat Style, then the two lists. That is the order they are declared in
// too, and the order this array must keep -- A and D read the bar off it,
// and a tab out of place here steps to the wrong neighbour.
constexpr std::array<Tab, 10> kBarOrder{Tab::Character, Tab::Inventory,  Tab::Magic,  Tab::Shouts,
                                        Tab::Summons,   Tab::Effects,    Tab::Skills, Tab::CombatStyle,
                                        Tab::Tactics,   Tab::IdleTactics};

// The tab `step` places along the bar from `from`, wrapping at both ends. A
// page draws every tab it has -- an empty one says so in its body rather
// than going away -- so the bar is kBarOrder, less the Combat Style the
// player's page has not got.
Tab StepTab(Tab from, int step, bool player)
{
    constexpr auto size = static_cast<int>(kBarOrder.size());
    const auto found = std::find(kBarOrder.begin(), kBarOrder.end(), from);
    int at = found == kBarOrder.end() ? 0 : static_cast<int>(found - kBarOrder.begin());
    do
        at = (at + step % size + size) % size;
    while (player && kBarOrder[static_cast<std::size_t>(at)] == Tab::CombatStyle);
    return kBarOrder[static_cast<std::size_t>(at)];
}

// A page's answer to the movement keys, read once before its bar is drawn:
// the chip strip lives inside the bar and wants this frame's step. The tab
// is asked for through `select`, the way a link on a sheet asks, and the
// caller resolves that on the line after, so the bar moves on this frame.
//
// Nothing is read while an item has the keyboard. In a filter box A and D
// are letters, and the box is what Space put the cursor in.
void StepPage(PanelState &panel, bool player)
{
    if (Im::IsAnyItemActive())
        return;
    if (Im::IsKeyPressed(Im::ImGuiKey_W, false))
        g_keyRow = KeyRow::Tabs;
    if (Im::IsKeyPressed(Im::ImGuiKey_S, false))
        g_keyRow = KeyRow::Chips;

    const int step = static_cast<int>(Im::IsKeyPressed(Im::ImGuiKey_D, false)) -
                     static_cast<int>(Im::IsKeyPressed(Im::ImGuiKey_A, false));
    if (step == 0)
        return;
    if (g_keyRow == KeyRow::Chips)
        g_chipStep = step;
    else
        panel.select = StepTab(g_shownTab, step, player);
}

// A top tab: selected when `select` names it, and noted as the tab shown
// while it is open.
bool BeginSheetTab(const char *label, Tab tab, Tab select)
{
    // Pushed and popped around the one call, which is where ImGui paints
    // the tab: every tab of every bar comes through here, so the bar is
    // weakened in one place rather than at each page that draws one.
    const bool resting = g_keyRow == KeyRow::Chips;
    if (resting)
        Im::PushStyleColor(Im::ImGuiCol_TabActive, Im::GetStyle()->Colors[Im::ImGuiCol_TabUnfocusedActive]);
    const bool open = Im::BeginTabItem(label, nullptr, select == tab ? Im::ImGuiTabItemFlags_SetSelected : 0);
    if (resting)
        Im::PopStyleColor(1);
    if (!open)
        return false;
    g_shownTab = tab;
    return true;
}

} // namespace

void ShowingPage(ft::ActorId actor, Tab tab)
{
    g_drawnThisFrame.store(true, std::memory_order_relaxed);
    const std::uint64_t packed = Packed(actor, tab);
    if (g_shownNow.exchange(packed, std::memory_order_relaxed) != packed)
    {
        if (auto *task = SKSE::GetTaskInterface())
            task->AddTask([] { RefreshShownPage(); });
    }
}

void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType type)
{
    using Event = SKSEMenuFramework::Model::EventType;
    switch (type)
    {
    case Event::kOpenMenu:
    case Event::kCloseMenu:
        g_shownNow.store(0, std::memory_order_relaxed);
        g_filterActive.store(false, std::memory_order_relaxed);
        g_leaveFilter.store(false, std::memory_order_relaxed);
        g_resetPages.store(true, std::memory_order_relaxed);
        break;
    case Event::kBeforeRender:
        g_drawnThisFrame.store(false, std::memory_order_relaxed);
        // Done here rather than where the menu closed: the open and close
        // events are the framework's, and nothing says they are on this
        // thread, while a frame's own events are.
        if (g_resetPages.exchange(false, std::memory_order_relaxed))
        {
            ClearFilters();
            CloseDetails();
        }
        break;
    case Event::kAfterRender:
        if (!g_drawnThisFrame.load(std::memory_order_relaxed))
            g_shownNow.store(0, std::memory_order_relaxed);
        // A frame with no filter box drawn -- another tab, another mod's
        // section -- has no cursor in one, whatever the last frame said.
        if (!g_filterDrawn.exchange(false, std::memory_order_relaxed))
            g_filterActive.store(false, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

void KeepEscapeFromClosingTheMenu()
{
    const REL::Relocation<std::uintptr_t> site{addr::kInputQueueDispatch, addr::kInputQueueDispatchCall};
    g_dispatchInputQueue = reinterpret_cast<DispatchInputQueue>(
        SKSE::GetTrampoline().write_call<5>(site.address(), &DispatchInputQueueHook));
    // Whose call was displaced says whether ours runs first: the
    // framework's thunk if it hooked before us, the engine's own function
    // if it has not hooked yet and will wrap ours, in which case it closes
    // the menu before the press reaches here.
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(g_dispatchInputQueue), &owner);
    if (owner == GetMenuFrameworkModule())
        log::ui.info("input queue call at {:X} rewritten ahead of SKSE Menu Framework's: Escape leaves the filter box",
                     site.address());
    else
        log::ui.warn("input queue call at {:X} rewritten, but not ahead of SKSE Menu Framework's (displaced {:X}): "
                     "Escape will close the menu from the filter box",
                     site.address(), reinterpret_cast<std::uintptr_t>(g_dispatchInputQueue));
}

Tab PageTab(const CharacterView &view)
{
    PanelState &panel = Panel(view.id);
    const Tab carried = CarriedTab(view);
    StepPage(panel, view.player);
    const Tab select = panel.select != Tab::None ? panel.select : carried;
    panel.select = Tab::None;
    return select;
}

void DrawSheetTabs(const CharacterView &view, Tab select)
{
    PanelState &panel = Panel(view.id);
    if (BeginSheetTab(Tr("Character"), Tab::Character, select))
    {
        TabBody(Tab::Character, view.id, [&] { DrawCharacter(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Inventory"), Tab::Inventory, select))
    {
        TabBody(Tab::Inventory, view.id, [&] { DrawInventory(view); }, {panel.inventory.detail});
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Magic"), Tab::Magic, select))
    {
        TabBody(Tab::Magic, view.id, [&] { DrawMagic(view); }, {panel.magic.detail});
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Shouts"), Tab::Shouts, select))
    {
        TabBody(Tab::Shouts, view.id, [&] { DrawShouts(view); }, {panel.shouts.detail});
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Summons"), Tab::Summons, select))
    {
        TabBody(Tab::Summons, view.id, [&] { DrawSummons(view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Effects"), Tab::Effects, select))
    {
        const EffectsTabState &effects = panel.effects;
        TabBody(Tab::Effects, view.id, [&] { DrawEffects(view); },
                {reinterpret_cast<std::uintptr_t>(effects.detail.first), effects.detail.second});
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Skills"), Tab::Skills, select))
    {
        TabBody(Tab::Skills, view.id,
                [&] {
                    Im::Spacing();
                    DrawSkills(view);
                },
                {panel.skills.tree, panel.skills.detail});
        Im::EndTabItem();
    }
}

void DrawTacticsTabs(const FollowerView &view, Tab select)
{
    if (BeginSheetTab(Tr("Tactics"), Tab::Tactics, select))
    {
        TabBody(Tab::Tactics, view.id, [&] { DrawTactics(GetRules(view.id, ft::Moment::Combat), view); });
        Im::EndTabItem();
    }
    if (BeginSheetTab(Tr("Idle Tactics"), Tab::IdleTactics, select))
    {
        TabBody(Tab::IdleTactics, view.id, [&] { DrawTactics(GetRules(view.id, ft::Moment::Idle), view); });
        Im::EndTabItem();
    }
}

void DrawFollower(const FollowerView &view)
{
    // Not here: the whole page, and not a tab of it, is the answer. Every
    // sheet tab is a scan of a simulated actor and an away follower has
    // none to give -- the page keeps what was read when they were last
    // nearby, which for a tab never opened while they were is nothing at
    // all. A bar of tabs each saying the same thing is worse than one line
    // saying it once, so it reads as a dismissed page does: the line, and
    // no tabs behind it (reported in play, 2026-09-19).
    if (!view.nearby)
    {
        Im::TextDisabled("%s", Tr("Follower is not nearby."));
        return;
    }

    if (!Im::BeginTabBar("follower##tabs"))
        return;

    const Tab select = PageTab(view);
    DrawSheetTabs(view, select);
    // What the combat AI is tuned by, before what it is told: a rule works
    // with, or against, these numbers.
    if (BeginSheetTab(Tr("Combat Style"), Tab::CombatStyle, select))
    {
        TabBody(Tab::CombatStyle, view.id, [&] {
            Im::Spacing();
            DrawSections(view.combatStyle, false);
        });
        Im::EndTabItem();
    }
    DrawTacticsTabs(view, select);

    Im::EndTabBar();
}

ShownPage Shown()
{
    const std::uint64_t packed = g_shownNow.load(std::memory_order_relaxed);
    return {static_cast<ft::ActorId>(packed >> 8), static_cast<Tab>(packed & 0xFF)};
}

const char *Name(Tab tab)
{
    switch (tab)
    {
    case Tab::Character:
        return "character";
    case Tab::Inventory:
        return "inventory";
    case Tab::Magic:
        return "magic";
    case Tab::Shouts:
        return "shouts";
    case Tab::Summons:
        return "summons";
    case Tab::Effects:
        return "effects";
    case Tab::Skills:
        return "skills";
    case Tab::CombatStyle:
        return "combatstyle";
    case Tab::Tactics:
        return "tactics";
    case Tab::IdleTactics:
        return "idle";
    case Tab::None:
    default:
        return "none";
    }
}

} // namespace ft::game::ui
