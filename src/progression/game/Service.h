#pragma once
// The ledger's home in the running game, and every change to it.
//
// All state is changed on the game thread: SKSE's event sinks, the paced
// tick (progression/game/Events.cpp) and the pages' actions, which are
// queued there with AddTask. Tactics' pages ask on the render thread, under
// the lock, from views built on the game thread, so they never read an
// actor or the ledger while the game thread writes either. dev/PROGRESSION.md is what the operations
// mean; progression/core/Companion.h is how they are decided.

#include "progression/core/Companion.h"
#include "progression/core/Levelling.h"
#include "progression/core/Perks.h"
#include "progression/core/Serialize.h"
#include "progression/core/Settings.h"
#include "progression/core/Spells.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fp::game
{

struct KnownSpellRow
{
    SpellFacts facts;
    bool onRecord{false}; // on their own record
    bool taught{false};   // taught here
    bool setAside{false}; // their own, set aside here
};

struct TomeRow
{
    FormKey book;
    std::string bookName;
    SpellFacts facts;
    int count{0};
    TeachStatus status;
};

// What the pages show of a companion that the ledger does not hold: read
// off the actor on the game thread, rebuilt after every action and whenever
// a page comes up (RefreshViews).
struct CompanionView
{
    FormKey key;
    RE::FormID actor{0};   // the reference's runtime id, when it resolved
    bool read{false};      // read off an actor at least once this session
    bool loaded{false};    // near the player, fully simulated
    bool following{false}; // on the journey: a follower, not told to wait
    bool waiting{false};
    int level{0};           // the engine's
    LevelProgress progress; // theirs (progression/core/Companion.h)
    int perkPoints{0};
    int attributePoints{0};
    PerSkill<int> floors{};       // where each skill starts for a new character of their race
    PerSkill<double> nextLevel{}; // skill XP from each trainable skill's level to the next
    PerSkill<int> base{};
    PerAttribute<int> attributes{};
    int maxMagicka{0};
    std::unordered_set<FormKey, FormKeyHash>
        onRecord; // perks on their own record not bought here, set-aside ones included
    std::vector<KnownSpellRow> spells;
    std::vector<TomeRow> tomes;
};

// --- the panel's actions: from any thread, carried out on the game thread ---

// One level onto a skill from the reassigning pool (+1) or back into it
// (-1); one attribute point on or off: the panel's buttons, carried out at
// once. progression/core/Companion.h says what is allowed.
void AssignSkillPoint(const FormKey &actor, Skill skill, int delta);
void AssignAttributePoint(const FormKey &actor, Attribute attribute, int delta);
// A skill moved as far as it goes one way (-1 down, +1 up): each level as -
// and + would move it, while they can (progression/core/Companion.h).
void AssignSkillAll(const FormKey &actor, Skill skill, int direction);
// The perks bought in a skill's tree given back; the skill left as it is.
void ResetPerks(const FormKey &actor, Skill skill);
// The next rank of a perk, named by runtime ids -- the actor's and a rank's
// of its node, as Tactics' skill page has them -- resolved on the game
// thread and learned there in the one action. Nothing for an actor who is
// no companion of ours.
void LearnPerkByForm(std::uint32_t actor, std::uint32_t perk);

// A companion's skill as Tactics' skill page heads it: their level with
// what they have learned, and its -, + and Reset (core ButtonsFor), with
// leveling off and their being away folded in. `active` is whether a
// click on the tree can learn. None for an actor who is no companion of
// ours, or a skill Progression does not know. From the render thread,
// under the lock.
struct SkillControls
{
    FormKey companion;
    Skill skill{Skill::OneHanded};
    int level{0};   // theirs, with what they have learned
    int base{0};    // what the engine gives them
    int learned{0}; // on top of it, or taken back below it
    int perkPoints{0};
    bool active{false};
    SkillButtons buttons;
};
[[nodiscard]] std::optional<SkillControls> ControlsFor(RE::FormID actor, int actorValue);

// A companion's level and the experience into it, as their page shows it:
// none for an actor who is no companion of ours, or one not read yet. From
// the render thread, under the lock.
[[nodiscard]] std::optional<LevelProgress> LevelFor(RE::FormID actor);
// Its top rank bought here given back, named as LearnPerkByForm. Both are
// the skill page's, which asks PerkControlsFor first and answers with a
// sound: nothing is shown, a refusal is only logged.
void UnlearnPerkByForm(std::uint32_t actor, std::uint32_t perk);

// Whether a perk, by any rank's runtime id, can be learned or unlearned for
// this companion now, as LearnPerkByForm and UnlearnPerkByForm would decide. None for an
// actor who is no companion of ours, or a perk in no tree. From the render
// thread, under the lock.
struct PerkControls
{
    bool canLearn{false};
    bool canUnlearn{false};
};
[[nodiscard]] std::optional<PerkControls> PerkControlsFor(RE::FormID actor, std::uint32_t perk);
// One of their own perks: set aside (the engine is told it is not held; the
// record keeps it) and taken up again, both free.
void SetAsidePerk(const FormKey &actor, int node);
void RestorePerk(const FormKey &actor, int node);
void Teach(const FormKey &actor, const FormKey &spell);
void Forget(const FormKey &actor, const FormKey &spell);
// One of their own spells: set aside (the engine is told they do not know
// it; the record keeps it) and taken up again, both free.
void SetAsideOwnSpell(const FormKey &actor, const FormKey &spell);
void RestoreOwnSpell(const FormKey &actor, const FormKey &spell);
// Levelling off: everything of ours off every companion -- assigned points
// withdrawn, perks and spells as their records have them -- as they are
// near, the ledger kept. On: all of it back. Taught spells go too, being
// only the view's; on VR, where they were added to the actor, they stay.
// Tactics' Settings page is the switch.
void SetLevelling(bool on);

struct LevellingState
{
    bool inGame{false};
    bool on{true};
    std::vector<std::string> stillHeld; // off, and not yet released
};
// For the switch, drawn every frame: small, under the lock.
[[nodiscard]] LevellingState Levelling();

// --- the game thread ---------------------------------------------------------

void Tick();
// A follower's page has come up (game/Tactics.cpp, RefreshShownPage): the
// views rebuilt now, as the page reads them. Game thread.
void RefreshViews();
// A companion's use of a skill, worth `points`, as the engine works it out
// (progression/game/Learning.h). Game thread.
void OnSkillUse(RE::FormID actor, Skill skill, float points);
// The player reached `level` (LevelIncrease): a follower's engine level, and
// the cap on what learning reaches, move with it. Game thread.
void OnPlayerLevelUp(int level);
// After a load or a new game: the session's own state starts over.
void OnGameStarted();
// Back at the main menu: the panel stops acting on the game just left.
void OnGameLeft();
// SKSE's kPreLoadGame: the views go, so an actor built during the load is
// built as its record has it until the save's ledger is published.
void BeforeLoad();

// --- the co-save (SKSE's callbacks, game thread) ------------------------------

[[nodiscard]] std::vector<CoSaveRecord> SaveRecords();
void LoadRecords(const std::vector<CoSaveRecord> &records);
// SKSE's revert callback, mid-load or before a new game: the views go and
// the ledger is emptied. Nothing of ours is on a record to take off.
void Revert();

} // namespace fp::game
