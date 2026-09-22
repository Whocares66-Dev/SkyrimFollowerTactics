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

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ft::game
{

// What the sheet tabs show of one actor -- Character, Inventory, Magic,
// Summons, Effects, Skills -- worded on the game thread. A follower's view
// is this and their tactics, and so is the player's since 2026-09-18.
//
// The UI renders on the render thread while the tick runs on the game thread,
// so nothing hands out a pointer into live state: a view is published whole
// and never edited after -- a change is a fresh copy put in its place -- and
// the render thread shares the published one for the frame (SharedView).
// Until 2026-09-18 each frame took a copy of it instead, which for the
// player's page is 190 spells each with its sections and description, deep
// copied sixty times a second: the panel's lag.
struct CharacterView
{
    ft::ActorId id{0};
    // Plain display name, no FormID. The ID is a debugging detail and belongs
    // in the log, where Describe() still emits it -- on screen it is noise the
    // player can get from the console if they ever need it.
    std::string name;
    // The player's own page. Their equip cells equip and unequip and no
    // more: a pin and a ban are a leash on the combat AI, and nothing is
    // choosing for the player. Their rules offer what their body has a
    // route for (game/PlayerCast.h, PlayerSupports).
    bool player{false};
    bool inCombat{false};
    // With the player, and so fully simulated. False for a follower in
    // another cell -- told to wait, or simply left behind: still theirs, and
    // still under tactics when they catch up. Their page is kept and marked
    // rather than dropped, since away and dismissed are different things;
    // what it shows was read when they were last nearby, and their rules are
    // the panel's to edit either way, being ours rather than the actor's.
    // An away follower's sheet tabs say so instead of drawing readings
    // nobody took (game/UI.cpp, AwayNotice); their tactics are not a
    // reading and are theirs to write wherever they are. The player is
    // nearby by definition.
    bool nearby{true};

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
    // Each skill's perk tree and what they hold of it, for the Skills tab's
    // skill page.
    std::vector<ft::PerkTreeView> trees;
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
    // Every action's availability as the page was built, rule by rule
    // (core/Evaluator.h, ProbeAvailability): what the action cell greys by
    // and says on its hover -- not enough magicka, the voice recovering, a
    // power used today. Read against a fresh snapshot and the actor's own
    // cooldowns, decided nothing.
    ft::ActionTrace availability;
};

// Everything the UI needs, shared and immutable. Includes followers who are
// NOT fighting: tactics are authored before a fight, so the panel has to
// show them then. One follower's alone, for the page that draws one: every
// open page copied every follower's view each frame until 2026-09-11.
using SharedView = std::shared_ptr<const FollowerView>;
[[nodiscard]] std::vector<SharedView> ObserveFollowers();
[[nodiscard]] SharedView ObserveFollower(ft::ActorId id);

// This follower's rules, as a copy: none until someone writes some, so a
// fresh install changes nothing. Two lists each, one per moment (core/Rule.h
// Moment): the combat list, and the idle list evaluated out of a fight. A
// set knows its moment, so a write needs no second word.
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
[[nodiscard]] ft::RuleSet GetRules(ft::ActorId id, ft::Moment moment);
void SetRules(ft::ActorId id, ft::RuleSet rules);
// The names of the things the actor's rules name, refreshed in place on
// the current list (core/Editor.h, RefreshActionNames). Not a SetRules of
// a copy read earlier: the panel edits the list from the render thread,
// and a copy written back whole would put an edit made meanwhile under it.
void RefreshActionNames(ft::ActorId id, ft::Moment moment,
                        const std::function<std::string(const ft::Action &)> &currentName);

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

// The player's page: the sheet, and since 2026-09-18 their tactics too, so
// it is a follower's view with `player` set (dev/PLAYER.md).
[[nodiscard]] SharedView ObservePlayer();

// Start ticking. Safe to call once, after kDataLoaded.
void Install();

// Enable/disable at runtime without unregistering the tick. Global: applies to
// every follower.
void SetEnabled(bool enabled);
[[nodiscard]] bool IsEnabled();

// Per follower, on top of the global switch. A follower is evaluated only when
// both are on. Both start on: with no rules by default, on is safe, and the
// switch is for silencing a written list without losing it.
// One switch per list: the Tactics tab's and the Idle Tactics tab's are
// each their own, so a follower's fight can be silenced and their upkeep
// kept, or the other way about.
void SetFollowerEnabled(ft::ActorId id, ft::Moment moment, bool enabled);
[[nodiscard]] bool IsFollowerEnabled(ft::ActorId id, ft::Moment moment);

} // namespace ft::game
