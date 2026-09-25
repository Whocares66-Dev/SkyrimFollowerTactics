#pragma once
// The panel's memory: what each actor's page shows, the filters, and the
// flags the render thread and the input thread share.

#include "core/OpenRows.h"
#include "core/Snapshot.h"
#include "core/Views.h"
#include "game/ui/UI.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ft::game::ui
{

// Which rows are open -- a skill's perks, a rule's actions -- keyed
// "section/label" or "rule/<follower>/<index>". Ours rather than any ImGui
// widget state, because widget state is keyed on the ID stack, and a row's
// ID stack includes which table PIECE it landed in (see DrawSections) --
// which changes as soon as a row above it opens, at which point ImGui
// would forget the row was open. Render thread only.
extern ft::OpenRows g_openRows;

// Two kinds of state, kept apart here so neither is looked for in the other:
//
//  - per follower, in PanelState: which page of theirs is open and what is
//    open on it, so moving between followers does not lose anyone's place.
//  - per panel, in the ListView beside each list: the chip picked and the
//    text typed. Shared across followers on purpose -- "where are the
//    lockpicks" is a question about the party, not about one bag, and
//    Weapons on one follower is Weapons on the next and on the player.
//
// Render thread only, like g_openRows. The Tab enum itself is in the header:
// the game thread reads it to know which page to build.
inline constexpr std::size_t kFilterLen = 64;

// What a list remembers for every follower at once.
struct ListView
{
    int category{-1}; // the chip picked; -1: All, and what a list with no chip row stays at
    char filter[kFilterLen]{};
};

// The three lists with a chip row. Magic and Shouts keep theirs apart: no
// category is on both, so one shared int would drop each list's choice on
// the way to the other.
extern ListView g_inventoryList;
extern ListView g_magicList;
extern ListView g_shoutList;
// And the two that only filter.
extern char g_effectsFilter[kFilterLen];
extern char g_perksFilter[kFilterLen];
// Whether the Effects tab lists what the game's own list hides. A way of
// looking at the list, as a chip is, not a search: kept when the panel
// reopens, and one for every page.
extern bool g_showHiddenEffects;

// Every filter box, emptied at once. A filter is what the player is looking
// for now, not a setting: a page reopened shows the whole list again. The
// chips are not touched -- a category is a place in the panel, and a list
// reopened on the one it was left on is where the player put it.
void ClearFilters();

// The keys a list answers as SkyUI's do: Space puts the cursor in the
// page's filter box, Escape takes it out and empties it. Escape would
// otherwise close the whole menu -- the framework closes on any Escape
// event, on the input thread, before the frame that would have shown the
// box active -- so its press is taken out of the queue there (TakeEscape)
// and answered on the render thread instead.
extern bool g_focusFilter;               // render thread: the box drawn next takes the keyboard
extern bool g_clearFilter;               // render thread: the box drawn next empties itself
extern std::atomic<bool> g_filterDrawn;  // a filter box was drawn this frame
extern std::atomic<bool> g_filterActive; // the cursor was in one, as of the last frame drawn
extern std::atomic<bool> g_leaveFilter;  // a taken Escape, for the render thread to act on
extern std::atomic<bool> g_escapeTaken;  // input thread: the taken press has not been released

// A page answers the movement keys as the game does, one row at a time: A
// and D step along the row that has the keys, S goes down from the bar of
// tabs to the strip of chips under it, W back up. A tab with no strip --
// every sheet, and every detail page -- keeps them on the tabs: the row is
// put back at the end of a body that drew none, so S on such a tab does
// nothing rather than swallowing the A and D after it. Which row has them
// is marked by weakening the other's selection rather than by lighting this
// one: see kRestingChip below.
// Render thread only, and read nowhere while an item has the keyboard: in
// a filter box, A and D are letters.
enum class KeyRow
{
    Tabs,
    Chips,
};
extern KeyRow g_keyRow;
extern bool g_chipsDrawn; // this tab's body drew a chip strip
extern int g_chipStep;    // -1 / 1: the strip drawn next moves its choice by this

// The row without the keys shows its selection weakened, which is the whole
// of the mark: no colour is introduced for it. The bar has the theme's own
// answer already -- ImGuiCol_TabUnfocusedActive is what ImGui paints a
// selected tab whose bar is not the focus -- so it is read straight out of
// the style and follows whatever theme the player installed. A chip strip
// is ours and has no second colour, so a resting chip takes this much of
// the selected tint instead.
inline constexpr float kRestingChip = 0.45f;

struct InventoryTabState
{
    std::uint64_t detail{0}; // the row open in detail, by InventoryItem::Key; 0 for the list
    // This frame's category: the list's shared one, or All on a page with
    // nothing in it.
    int category{-1};
    Tab openedFrom{Tab::Inventory}; // where the detail page returns to
    std::uint64_t confirming{0};    // the tome whose Learn asked once, awaiting Confirm
};

// The Magic tab's and the Shouts tab's, one each: two halves of one list
// (VoiceEntry says which), so they remember the same things.
struct MagicTabState
{
    std::uint32_t detail{0}; // the entry open in detail; 0 for the list
    int category{-1};        // this frame's, as the Inventory tab's
    // Where the detail page returns to: the tab's own list, or the sheet it
    // was opened from. Written wherever `detail` is, so the value here is
    // never read before one of those has set it.
    Tab openedFrom{Tab::Magic};
    std::uint32_t confirming{0}; // the spell whose Forget asked once, awaiting Confirm
};

struct EffectsTabState
{
    ft::EffectKey detail{}; // the row open in detail, by EffectRow::Key; empty for the list
};

struct SkillsTabState
{
    std::uint32_t detail{0};  // the perk open in detail; 0 for the skills
    std::uint32_t tree{0};    // the skill whose tree is open (PerkTreeView::key); 0 for the skills
    bool confirmReset{false}; // the open skill's Reset asked once, awaiting Confirm
};

// Everything the panel remembers about one follower's page: a state for each
// tab that keeps one, and the switch the page itself owns. One map, so there
// is one lookup, one place to read what a page remembers, and one place to
// clear when a follower goes.
struct PanelState
{
    InventoryTabState inventory;
    MagicTabState magic;
    MagicTabState shouts;
    EffectsTabState effects;
    SkillsTabState skills;
    // The Summons tab's chip: the summon chosen, by its reference.
    int summon{0};
    // The character sheet's attribute controls, opened by an attribute's
    // label and closed by it again, or by the panel closing.
    bool attributeControls{false};
    // The tab to show on the next frame, asked for by a link on a sheet or
    // by a detail page's back arrow. The page's own and not any one tab's --
    // four of them write it -- though it lived in the Inventory tab's state
    // until 2026-09-16. None: leave the bar as it is.
    Tab select{Tab::None};
};

PanelState &Panel(ft::ActorId id);

// The top tab on the page drawn last, and whose page that was. Followers
// share one tab bar and the player has another, and ImGui keeps each bar's
// choice apart, so the tab is carried from page to page here: the Skills of
// one follower, then of the player, then of the next follower. Render
// thread only.
extern Tab g_shownTab;
extern ft::ActorId g_shownPage;

// The row a tab's page is open on, found again in this frame's list by the
// row's Key(), the one identity its click and its ImGui id use as well;
// null for the list. A row gone since the page was opened -- drunk,
// dropped, run out -- closes the page.
template <typename Row, typename Key> const Row *OpenRow(const std::vector<Row> &rows, Key &open)
{
    if (open == Key{})
        return nullptr;
    const auto found = std::find_if(rows.begin(), rows.end(), [&open](const Row &row) { return row.Key() == open; });
    if (found != rows.end())
        return &*found;
    open = Key{};
    return nullptr;
}

// Leaving a tab's detail page: it closes, and the page goes back where the
// page came from -- the list it is filed under, or the sheet a link opened
// it from, whose tab is selected again. `home` is the tab the state belongs
// to. Both ways out go through here: the back arrow, which always goes
// back, and the panel closing on the page, which goes back only for the tab
// that was being read.
template <typename State> void CloseDetail(PanelState &panel, State &state, Tab home, bool back = true)
{
    if (back && state.detail != 0 && state.openedFrom != home)
        panel.select = state.openedFrom;
    state.detail = 0;
    state.openedFrom = home;
    state.confirming = 0;
}

// Every detail page, closed. A detail page -- an item, a spell, an effect,
// a perk -- is a place the player stepped into, and stepping out of the
// panel is stepping out of it: what the tab should be showing when the
// panel comes back is where the back arrow would have gone, which for a
// weapon opened from the Character sheet is the sheet and not the Inventory
// list it is filed under.
//
// Only the tab that was on screen is sent anywhere: a page can hold an open
// detail on several tabs at once, and only the one being read has a "back"
// the player would recognise. Effects and Skills detail pages have no
// origin -- nothing links into them from another tab -- so closing them is
// the whole of it. The chip and the tab are otherwise left where they are:
// those are where the player put the panel down, not what they were reading
// on it.
void CloseDetails();

// The panel opened or closed, and the pages are to be put back to how they
// are first met -- no filter text, no detail page -- before another frame
// is drawn. Set on the framework's event and acted on inside a frame, which
// is the one place known to be the render thread.
extern std::atomic<bool> g_resetPages;

} // namespace ft::game::ui
