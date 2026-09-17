#pragma once
// The tick: find the followers, build each one's snapshot, evaluate their
// rules against it, act on the decision, and publish a view for the panel.
// It also holds the per-follower rules and switches the panel edits and
// the co-save keeps (game/Profiles.h). It began as the Phase 1 spike, to
// retire one risk -- whether an NPC can be made to reliably consume a
// potion.

#include "core/Editor.h"
#include "core/Evaluator.h"
#include "game/Inventory.h"
#include "game/Magic.h"
#include "game/Profiles.h"
#include "game/Sensors.h"

#include <optional>
#include <string>
#include <vector>

namespace ft::game
{

// What the sheet tabs show of one actor -- Character, Inventory, Magic,
// Summons, Effects, Skills -- worded on the game thread. A follower's view
// is this and their tactics; the player's page is this alone.
//
// The UI renders on the render thread while the tick runs on the game thread,
// so nothing hands out a pointer into live state -- callers get a snapshot they
// own. Copying a handful of small vectors once per UI frame is far cheaper than
// the alternative of holding a lock across rendering.
struct CharacterView
{
    ft::ActorId id{0};
    // Plain display name, no FormID. The ID is a debugging detail and belongs
    // in the log, where Describe() still emits it -- on screen it is noise the
    // player can get from the console if they ever need it.
    std::string name;
    // The player's own page. Their equip cells equip and unequip and no
    // more: a pin and a ban are a leash on the combat AI, and nothing is
    // choosing for the player.
    bool player{false};
    bool inCombat{false};

    std::uint16_t level{0};
    ft::Stat health{};
    ft::Stat stamina{};
    ft::Stat magicka{};
    float carriedWeight{0.0f};
    float carryCapacity{0.0f};
    // The three bars' maxima and the carry weight written out -- the base,
    // each effect by name, what else is in the value -- as hover text on the bars
    // and on the Carrying figure.
    ft::Breakdown healthBreakdown;
    ft::Breakdown staminaBreakdown;
    ft::Breakdown magickaBreakdown;
    ft::Breakdown carryBreakdown;
    // The Character and Skills tabs' sections, worded on the game thread.
    std::vector<SheetSection> sheet;
    std::vector<SheetSection> skills;
    // A page per perk held, for the Skills tab's perk page.
    std::vector<PerkPage> perks;
    // What they command right now, for the Summons tab.
    std::vector<SummonView> summons;
    // The Inventory tab: everything they carry, sorted by name.
    std::vector<InventoryItem> inventory;
    // The Magic tab: spells, powers and shouts, sorted by name.
    std::vector<MagicEntry> magic;
    // The Effects tab: what is running on them, sorted by name.
    std::vector<EffectRow> effects;
};

// A copy of what the engine last decided about one follower, over their
// sheet.
struct FollowerView : CharacterView
{
    // With the player, and so fully simulated. False for a follower in
    // another cell -- told to wait, or simply left behind: still theirs, and
    // still under tactics when they catch up. Their page is kept and marked
    // rather than dropped, since away and dismissed are different things;
    // what it shows was read when they were last nearby, and their rules are
    // the panel's to edit either way, being ours rather than the actor's.
    bool nearby{true};
    // The other followers under tactics, by name, for the condition menu's
    // named subjects.
    struct Peer
    {
        ft::ActorId id{0};
        std::string name;
    };
    std::vector<Peer> peers;

    // The spells this follower can be told to cast or equip, sorted by name.
    // Lives on the view rather than in Snapshot because it is menu content, not
    // a rule input -- the evaluator only ever compares FormIDs.
    std::vector<SpellOption> spells;
    // The potions, food and ingredients they carry, for the Consume menu.
    // Same reasoning.
    std::vector<ConsumableOption> consumables;
    // The Tactics tab's Combat Style section.
    std::vector<SheetSection> combatStyle;
    // What the editor greys a rule by (core/Editor.h), from the same scans
    // as the menus above.
    ft::Holdings holdings;
};

// Everything the UI needs, all copied. Includes followers who are NOT fighting:
// tactics are authored before a fight, so the panel has to show them then.
// One follower's alone, for the page that draws one: every open page copied
// every follower's view each frame until 2026-09-11.
[[nodiscard]] std::vector<FollowerView> ObserveFollowers();
[[nodiscard]] std::optional<FollowerView> ObserveFollower(ft::ActorId id);

// This follower's rules, as a copy: none until someone writes some, so a
// fresh install changes nothing.
//
// Copy in, copy out. Rule sets hold a handful of rules, so copying is cheap,
// and it removes a whole class of problem: the UI edits its own copy across as
// many frames as it likes and writes the result back, with no partial state
// visible to the tick and no lock held across rendering.
//
// The panel writes as the player edits, and a load writes what the save
// held (LoadIfNew); the tick reads. A follower's list is never edited by
// two of those at once, which is what makes read-modify-write safe here
// without a version check.
[[nodiscard]] ft::RuleSet GetRules(ft::ActorId id);
void SetRules(ft::ActorId id, ft::RuleSet rules);

// The rules, the switch and the player's pins live in the save, one
// record per follower (game/Profiles.h). Edits are the session's state,
// and the game's own save is what keeps them: this is everything the
// session holds, for the save callback. A follower's record is taken back
// the first time the tick sees them after a load. Game thread.
struct Filed
{
    Identity who;
    ft::Profile profile;
};
[[nodiscard]] std::vector<Filed> ProfilesToSave();

// Before a save loads, and on a new game: forget every follower's rules,
// switch and pins, so nothing from the last session carries into the
// next. The loaded save's records follow. Game thread, from the revert
// callback.
void ForgetSession();

// Rebuild what the panel is drawing -- that character, that page -- and
// publish it. Everything else is left as it was: a page nobody is looking
// at is not built, and with the panel closed nothing is built at all.
//
// Called when the panel changes page, and after something is done to the
// follower from it. Never on a beat: a page's content does not change while
// it sits open -- the clock is frozen behind the panel -- and the player's
// magic page costs ~92 ms to build. Game thread.
void RefreshShownPage();

// Ask for one more build of the page on screen, on the panel's next frame.
// Some of what the panel asks the engine for has not landed by the time the
// asking returns: a spell leaves a hand through a Papyrus native, which the
// VM runs a frame later, so a build straight after the click reads the actor
// before it has moved and the cell redraws as it was.
//
// Another AddTask will not do. SKSE drains its task queue to empty inside
// one call, so a task added from inside a task runs in that same drain --
// still before the frame being waited for. The wait belongs to whatever
// draws every frame, which is the panel: it takes this on its next one
// (game/UI.cpp, ShowingPage). Any thread.
void RefreshShownPageSoon();

// Whether one was asked for, clearing it. Render thread, once a frame.
[[nodiscard]] bool TakeRefreshRequest();

[[nodiscard]] std::optional<CharacterView> ObservePlayer();

// Start ticking. Safe to call once, after kDataLoaded.
void Install();

// Enable/disable at runtime without unregistering the tick. Global: applies to
// every follower.
void SetEnabled(bool enabled);
[[nodiscard]] bool IsEnabled();

// Per follower, on top of the global switch. A follower is evaluated only when
// both are on. Both start on: with no rules by default, on is safe, and the
// switch is for silencing a written list without losing it.
void SetFollowerEnabled(ft::ActorId id, bool enabled);
[[nodiscard]] bool IsFollowerEnabled(ft::ActorId id);

} // namespace ft::game
