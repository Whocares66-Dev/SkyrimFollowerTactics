#include "game/PlayerCast.h"

#include "core/PlayerCast.h"
#include "game/Actions.h"
#include "game/Addresses.h"
#include "game/Bag.h"
#include "game/Graph.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Pins.h"
#include "game/Sensors.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ft::game
{
namespace
{

// The steps, their windows and the run's standing are core's
// (core/PlayerCast.h, AdvancePlayerCast); this reads the caster and
// sends the presses, or a follower's calls in their place, and the equips.
// How long a concentration spell's stream is held with no time named by the
// rule. The same default a follower's stream takes (game/Packages.cpp).
constexpr float kDefaultSustainSeconds = 3.0f;

// What a hand held before the spell was lent it, to be put back after: a
// spell, or an item and which copy of it, or nothing. The copy is kept as
// its VARIANT (dev/UNIQUE.md), never as the worn extra list: the engine
// frees or merges that list once the item is unequipped, and a list kept
// across the cast was handed back to EquipObject dangling (crash
// 2026-09-18 13:19, an Iron Dagger put back). The copy's list is found
// afresh at the put-back.
struct Held
{
    RE::SpellItem *spell = nullptr;
    RE::TESBoundObject *item = nullptr;
    ft::ItemVariant variant;
};

struct Run
{
    // The run's standing: the step, the times, the outcome, the reason.
    ft::CastState state;
    // Whose body: the player's, or a follower's for a staff.
    std::uint32_t actor = 0;
    std::uint32_t form = 0;
    RE::MagicItem *spell = nullptr; // the spell, or a power; a shout's first word; a staff's enchantment
    // A staff: what the hand is lent, where a spell is lent as itself.
    RE::TESObjectWEAP *staff = nullptr;
    // A follower's: whom it is aimed at. The player aims for themself.
    std::uint32_t target = 0;
    float chargeAtRequest = 0.0f; // a staff's
    RE::TESForm *voiceForm = nullptr;
    // The hand the cast is from -- Left, Right, or Both for a two-handed
    // spell -- and the caster asked about it: the left's for both, which is
    // where the handler sends a two-handed press.
    ft::Hand hand = ft::Hand::None;
    RE::MagicSystem::CastingSource source = RE::MagicSystem::CastingSource::kLeftHand;
    std::array<Held, 2> before{}; // [0] the left hand, [1] the right
    std::array<bool, 2> lent{};
    RE::TESForm *voiceBefore = nullptr;
    bool voiceLent = false;
    float magickaAtRequest = 0.0f;
    int ruleIndex = -1;
    std::string ruleName;
    // The player's graph from the request: the lent hands' equip, the fire
    // (core/PlayerCast.h, Hear).
    ft::GraphWatch watch;
};

// One at a time on a body: the player's, and each follower's for a staff.
// The game thread's, which reads them without the lock and takes it to add
// or drop one; the equip detour and the AI's score ask whose hands a run
// holds from the threads the engine calls them on, and take it shared. The
// flag is the pacing thread's.
std::vector<Run> g_runs;
std::shared_mutex g_runsMutex;
std::atomic<bool> g_inFlight{false};

Run *RunOf(const RE::Actor *actor)
{
    if (!actor)
        return nullptr;
    const auto it = std::ranges::find(g_runs, actor->GetFormID(), &Run::actor);
    return it != g_runs.end() ? &*it : nullptr;
}

// What the run casts, by the name the rule has for it.
const RE::TESForm *Named(const Run &run)
{
    if (run.staff)
        return run.staff;
    return run.voiceForm ? run.voiceForm : run.spell;
}

// --- the handler ------------------------------------------------------------

// One button event to one handler, as the player's own arrives: the control
// by its user-event name, which is all the handler reads, and the value and
// held time that make it a press (1, 0), a hold (1, how long it has been
// down) or a release (0, how long it was down). Made as the engine makes one
// and freed after; the handler keeps no pointer to it (it copies what it
// wants -- dev/PLAYER.md).
void SendButton(RE::PlayerInputHandler *handler, const RE::BSFixedString &control, float value, float heldSeconds)
{
    auto *controls = RE::PlayerControls::GetSingleton();
    if (!handler || !controls)
        return;
    auto *event = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, control, 0, value, heldSeconds);
    if (!event)
        return;
    handler->ProcessButton(event, &controls->data);
    // The string's reference goes before the memory does: the event's own
    // destructor is not declared by CommonLib.
    event->SetUserEvent({});
    RE::free(event);
}

RE::AttackBlockHandler *AttackHandler()
{
    auto *controls = RE::PlayerControls::GetSingleton();
    return controls ? controls->attackBlockHandler : nullptr;
}

RE::ShoutHandler *VoiceHandler()
{
    auto *controls = RE::PlayerControls::GetSingleton();
    return controls ? controls->shoutHandler : nullptr;
}

// Is the player's own button for this hand down, as the handler holds it?
// A second hand pressed inside the dual window turns a cast into a dual
// cast, so ours does not start while the other hand's is held.
bool ButtonHeld(const RE::AttackBlockHandler *handler, bool left)
{
    if (!handler)
        return false;
#ifdef ENABLE_SKYRIM_AE
    const auto &data = handler->GetRuntimeData();
    return left ? data.heldLeft : data.heldRight;
#else
    return left ? handler->heldLeft : handler->heldRight;
#endif
}

const RE::BSFixedString &Control(ft::Hand hand)
{
    auto *events = RE::UserEvents::GetSingleton();
    static const RE::BSFixedString none;
    if (!events)
        return none;
    return hand == ft::Hand::Right ? events->rightAttack : events->leftAttack;
}

const RE::BSFixedString &ShoutControl()
{
    auto *events = RE::UserEvents::GetSingleton();
    static const RE::BSFixedString none;
    return events ? events->shout : none;
}

void SendHand(ft::Hand hand, float value, float heldSeconds)
{
    SendButton(AttackHandler(), Control(hand), value, heldSeconds);
}

// The hand whose control a single press goes to: the left's for a
// two-handed spell, where the handler sends one; the run's own otherwise.
ft::Hand PressedHand(const Run &run)
{
    return run.hand == ft::Hand::Both ? ft::Hand::Left : run.hand;
}

// The press for this run: the shout control; the two hands' controls on one
// frame for a dual cast, which the handler pairs into a dual press; or the
// one hand's.
void PressFor(const Run &run)
{
    if (run.state.voice)
        SendButton(VoiceHandler(), ShoutControl(), 1.0f, 0.0f);
    else if (run.state.dual)
    {
        SendHand(ft::Hand::Left, 1.0f, 0.0f);
        SendHand(ft::Hand::Right, 1.0f, 0.0f);
    }
    else
        SendHand(PressedHand(run), 1.0f, 0.0f);
}

// The release: the one control let go; for a dual cast the other hand's as
// well, which the handler swallows -- it ignores everything after a dual
// release until that button comes up.
void ReleaseFor(const Run &run, double now)
{
    const auto held = static_cast<float>(now - run.state.pressedAt);
    if (run.state.voice)
        SendButton(VoiceHandler(), ShoutControl(), 0.0f, held);
    else if (run.state.dual)
    {
        SendHand(ft::Hand::Left, 0.0f, held);
        SendHand(ft::Hand::Right, 0.0f, held);
    }
    else
        SendHand(PressedHand(run), 0.0f, held);
}

// --- the hands --------------------------------------------------------------

const RE::BGSEquipSlot *Slot(std::uint32_t id)
{
    return RE::TESForm::LookupByID<RE::BGSEquipSlot>(id);
}

// Which hands the record lets the spell take (its equip slot), as the pin
// book words it.
ft::Grip GripOf(const RE::SpellItem *spell)
{
    const auto *slot = spell ? spell->GetEquipSlot() : nullptr;
    const std::uint32_t id = slot ? slot->GetFormID() : 0;
    return id == kLeftHandSlot    ? ft::Grip::LeftOnly
           : id == kRightHandSlot ? ft::Grip::RightOnly
           : id == kBothHandsSlot ? ft::Grip::Both
                                  : ft::Grip::Either;
}

RE::MagicItem *SpellIn(RE::Actor *player, bool left)
{
    return player->GetActorRuntimeData()
        .selectedSpells[left ? RE::Actor::SlotTypes::kLeftHand : RE::Actor::SlotTypes::kRightHand];
}

// The hand the spell is cast from. Its own if it is already in one, and
// both for a two-handed spell. Otherwise a hand that holds a spell -- one
// spell swapped for another leaves the weapon hand its weapon -- else an
// empty one, else the left; the record's grip rules all of it.
ft::Hand ChooseHand(RE::Actor *player, const RE::SpellItem *spell)
{
    const ft::Grip grip = GripOf(spell);
    if (grip == ft::Grip::Both)
        return ft::Hand::Both;
    if (grip == ft::Grip::LeftOnly)
        return ft::Hand::Left;
    if (grip == ft::Grip::RightOnly)
        return ft::Hand::Right;
    if (SpellIn(player, false) == spell)
        return ft::Hand::Right;
    if (SpellIn(player, true) == spell)
        return ft::Hand::Left;
    for (const bool left : {true, false})
        if (SpellIn(player, left))
            return left ? ft::Hand::Left : ft::Hand::Right;
    for (const bool left : {true, false})
        if (!player->GetEquippedObject(left))
            return left ? ft::Hand::Left : ft::Hand::Right;
    return ft::Hand::Left;
}

bool Takes(ft::Hand hand, bool left)
{
    return hand == ft::Hand::Both || hand == (left ? ft::Hand::Left : ft::Hand::Right);
}

Held HeldIn(RE::Actor *player, bool left)
{
    Held held;
    RE::TESForm *form = player->GetEquippedObject(left);
    if (!form)
        return held;
    if (auto *spell = form->As<RE::SpellItem>())
        held.spell = spell;
    else if (auto *item = form->As<RE::TESBoundObject>())
    {
        held.item = item;
        held.variant = VariantOf(WornList(player, item, left ? ft::Hand::Left : ft::Hand::Right));
    }
    return held;
}

// Is this hand holding what the run casts from it: the staff itself, or
// the spell?
bool PlacedIn(RE::Actor *player, const Run &run, bool left)
{
    return run.staff ? player->GetEquippedObject(left) == run.staff : SpellIn(player, left) == run.spell;
}

// Is it in the hands the run wants?
bool SpellPlaced(RE::Actor *player, const Run &run)
{
    for (const bool left : {true, false})
        if (Takes(run.hand, left) && !PlacedIn(player, run, left))
            return false;
    return true;
}

// Lend the hands the spell: remember what each holds, then equip it.
void Lend(RE::Actor *player, Run &run)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    auto *spell = run.spell ? run.spell->As<RE::SpellItem>() : nullptr;
    if (!manager || (!spell && !run.staff))
        return;
    for (const bool left : {true, false})
    {
        if (!Takes(run.hand, left) || PlacedIn(player, run, left))
            continue;
        run.before[left ? 0 : 1] = HeldIn(player, left);
        run.lent[left ? 0 : 1] = true;
    }
    const auto heldText = [&](std::size_t i) {
        const Held &h = run.before[i];
        return !run.lent[i] ? std::string("kept")
               : h.spell    ? log::NameOf(h.spell)
               : h.item     ? log::NameOf(h.item)
                            : std::string("nothing");
    };
    log::player.debug("{}: lending {} the {} hand (left held {}, right held {})", Describe(player),
                      log::NameOf(Named(run)),
                      run.hand == ft::Hand::Both   ? "both"
                      : run.hand == ft::Hand::Left ? "left"
                                                   : "right",
                      heldText(0), heldText(1));
    // A staff goes into the hand as the weapon it is: the copy that can pay
    // for the cast, by its list in the bag, now.
    if (run.staff)
    {
        manager->EquipObject(player, run.staff, StaffCastOf(player, run.staff).list, 1,
                             Slot(run.hand == ft::Hand::Right ? kRightHandSlot : kLeftHandSlot),
                             /*queueEquip*/ false, /*forceEquip*/ false, /*playSounds*/ false, /*applyNow*/ false);
        return;
    }
    // A scroll goes into the hand as the item it is, one copy, now.
    if (spell->Is(RE::FormType::Scroll))
    {
        manager->EquipObject(player, spell, nullptr, 1,
                             Slot(run.hand == ft::Hand::Right ? kRightHandSlot : kLeftHandSlot),
                             /*queueEquip*/ false, /*forceEquip*/ false, /*playSounds*/ false, /*applyNow*/ false);
        return;
    }
    // A dual cast is the spell in each hand, one equip per hand; a
    // two-handed spell takes both with one.
    if (run.state.dual)
    {
        for (const bool left : {true, false})
            if (SpellIn(player, left) != run.spell)
                manager->EquipSpell(player, spell, Slot(left ? kLeftHandSlot : kRightHandSlot));
        return;
    }
    const std::uint32_t slot = run.hand == ft::Hand::Both   ? kBothHandsSlot
                               : run.hand == ft::Hand::Left ? kLeftHandSlot
                                                            : kRightHandSlot;
    manager->EquipSpell(player, spell, Slot(slot));
}

// Give the hands back: what each held before, or nothing.
// What each borrowed hand and the voice get back is core's
// (core/PlayerCast.h, PlanRestore and PlanVoiceRestore, tested); this
// reads what was held, performs the equips, and resolves each copy
// against the bag as it stands now.
void Restore(RE::Actor *player, Run &run)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager)
        return;
    const auto slotOf = [&run](std::size_t i) {
        ft::HeldSlot slot;
        slot.lent = run.lent[i];
        const Held &held = run.before[i];
        slot.spell = held.spell ? held.spell->GetFormID() : 0;
        slot.item = held.item ? held.item->GetFormID() : 0;
        // A two-handed weapon, a bow or a crossbow reads from both hands.
        const auto *weapon = held.item ? held.item->As<RE::TESObjectWEAP>() : nullptr;
        slot.twoHanded = weapon && TwoHanded(weapon);
        return slot;
    };
    for (const ft::HandRestore &step : ft::PlanRestore(slotOf(0), slotOf(1)))
    {
        const std::size_t i = step.left ? 0 : 1;
        const Held &held = run.before[i];
        switch (step.what)
        {
        case ft::RestoreWhat::Spell:
            log::player.debug("{}: the {} hand back to {}", Describe(player), step.left ? "left" : "right",
                              log::NameOf(held.spell));
            manager->EquipSpell(player, held.spell, Slot(step.left ? kLeftHandSlot : kRightHandSlot));
            break;
        case ft::RestoreWhat::Item: {
            log::player.debug("{}: the {} hand back to {}", Describe(player), step.left ? "left" : "right",
                              log::NameOf(held.item));
            // A weapon into the hand it came from; a shield or a torch has
            // a slot of its own. Not queued: the player's equip lands now.
            // The copy's list is the one the bag holds NOW for that variant,
            // unworn; none for a plain copy without one, which the engine
            // resolves itself.
            const bool weapon = held.item->Is(RE::FormType::Weapon);
            RE::ExtraDataList *extra = UnwornVariantList(player, held.item, held.variant);
            manager->EquipObject(player, held.item, extra, 1,
                                 weapon ? Slot(step.left ? kLeftHandSlot : kRightHandSlot) : nullptr,
                                 /*queueEquip*/ false, /*forceEquip*/ false, /*playSounds*/ false,
                                 /*applyNow*/ false);
            break;
        }
        case ft::RestoreWhat::KeepBorrowed:
            // An empty hand keeps the spell. Unequipped after the cast, the
            // next lend into that hand played the equip animation twice (seen
            // in play 2026-09-18, the engine's own doing on one equip call);
            // left in place, once. An empty hand was holding nothing the player
            // chose, and a spell it has just cast is the least surprising thing
            // to find there.
            log::player.debug("{}: {} left in the {} hand, which held nothing", Describe(player),
                              log::NameOf(Named(run)), step.left ? "left" : "right");
            break;
        }
    }
    run.lent[0] = false;
    run.lent[1] = false;

    auto *beforeShout = run.voiceBefore ? run.voiceBefore->As<RE::TESShout>() : nullptr;
    auto *beforePower = run.voiceBefore ? run.voiceBefore->As<RE::SpellItem>() : nullptr;
    auto *ourShout = run.voiceForm ? run.voiceForm->As<RE::TESShout>() : nullptr;
    auto *ourPower = run.voiceForm ? run.voiceForm->As<RE::SpellItem>() : nullptr;
    switch (ft::PlanVoiceRestore(run.voiceLent, beforeShout || beforePower, ourShout != nullptr))
    {
    case ft::VoiceRestore::PutBack:
        if (beforeShout)
            manager->EquipShout(player, beforeShout);
        else if (beforePower)
            manager->EquipSpell(player, beforePower, Slot(kVoiceSlot));
        break;
    case ft::VoiceRestore::ReleaseShout:
        if (ourShout)
            UnequipShoutNow(player, ourShout);
        break;
    case ft::VoiceRestore::ReleasePower:
        if (ourPower)
            UnequipSpellNow(player, ourPower, 2);
        break;
    case ft::VoiceRestore::None:
        break;
    }
    run.voiceLent = false;
}

// --- the caster -------------------------------------------------------------

RE::MagicCaster *CasterOf(RE::Actor *player, const Run &run)
{
    return player->GetMagicCaster(run.state.voice ? RE::MagicSystem::CastingSource::kOther : run.source);
}

int StateOf(const RE::MagicCaster *caster)
{
    return caster ? static_cast<int>(caster->state.underlying()) : -1;
}

bool Idle(const RE::MagicCaster *caster)
{
    return caster && !caster->currentSpell && caster->state.get() == RE::MagicCaster::State::kNone;
}

// The voice as it stands, for the log while a power or shout is watched:
// what is selected, what the caster holds and its state, what the process
// says is being shouted, and the engine's cast check on the form now.
std::string VoiceText(RE::Actor *player, RE::MagicCaster *caster, RE::MagicItem *spell)
{
    const auto &runtime = player->GetActorRuntimeData();
    const auto *process = runtime.currentProcess;
    const auto *high = process ? process->high : nullptr;
    float strength = 1.0f;
    auto reason = RE::MagicSystem::CannotCastReason::kOK;
    const bool ok = caster && spell && caster->CheckCast(spell, false, &strength, &reason, false);
    auto *power = spell ? spell->As<RE::SpellItem>() : nullptr;
    return fmt::format(
        "selected {:08X}, caster spell {:08X} state {}, shouting {:08X} variation {}, check {}, used today {}",
        runtime.selectedPower ? runtime.selectedPower->GetFormID() : 0,
        caster && caster->currentSpell ? caster->currentSpell->GetFormID() : 0,
        caster ? static_cast<int>(caster->state.underlying()) : -1,
        high && high->currentShout ? high->currentShout->GetFormID() : 0,
        high ? static_cast<std::int32_t>(high->currentShoutVariation) : -99,
        ok ? "ok" : CannotCastText(static_cast<std::uint32_t>(reason)),
        power ? (player->IsInCastPowerList(power) ? "yes" : "no") : "-");
}

// The words charged so far while the shout control is held: the process's
// current variation, -1 before the first. Not the actor's shout level,
// which is the level the player MAY shout and read 2 of 2 thirty
// milliseconds after a press (2026-09-18), so a hold released on it shouted
// one word.
int WordsCharged(RE::Actor *player)
{
    const auto *process = player->GetActorRuntimeData().currentProcess;
    const auto *high = process ? process->high : nullptr;
    if (!high || !high->currentShout)
        return -1;
    return static_cast<std::int32_t>(high->currentShoutVariation);
}

bool OnUsedList(RE::Actor *player, RE::MagicItem *spell)
{
    auto *power = spell ? spell->As<RE::SpellItem>() : nullptr;
    return power && player->IsInCastPowerList(power);
}

// A greater power is once a day: the engine keeps the ones used on the
// actor (Actor::AddCastPower), and its cast check refuses one on the list.
// The engine adds it in the caster's own SpellCast callback, which the
// voice reached by the shout control did not run for ours (2026-09-18: a
// rule fired Battle Cry as often as it liked), so a greater power that has
// fired is added here, once, as the engine would have.
void MarkPowerUsed(RE::Actor *player, RE::MagicItem *spell)
{
    auto *power = spell ? spell->As<RE::SpellItem>() : nullptr;
    if (!power || power->GetSpellType() != RE::MagicSystem::SpellType::kPower)
        return;
    if (player->IsInCastPowerList(power))
        return;
    player->AddCastPower(power);
    log::player.info("{}: {} marked used for the day -- the engine had not", Describe(player), log::NameOf(power));
}

// The engine's own answer to whether the cast can be made now: the
// magicka, a power already used today, casting while shouting, and the
// rest. Asked on the caster the press would reach, so the press is not
// spent on a cast the engine refuses.
const char *Refused(RE::MagicCaster *caster, RE::MagicItem *spell)
{
    if (!caster || !spell)
        return "no caster";
    float strength = 1.0f;
    auto reason = RE::MagicSystem::CannotCastReason::kOK;
    if (caster->CheckCast(spell, false, &strength, &reason, false))
        return nullptr;
    return CannotCastText(static_cast<std::uint32_t>(reason));
}

// --- a follower's hand ------------------------------------------------------

// A follower's begin, where the player's is a press: the request the
// handler makes of the hand's caster beneath that press, and the combat
// AI's own casters make of it (addr::kRequestCast) -- the item, whom it is
// at, and the charge started. With nothing set to skip the caster's checks,
// so the staff pays as it does in the AI's hands.
void BeginOnCaster(RE::Actor *actor, RE::MagicCaster *caster, const Run &run)
{
    auto *target = RE::TESForm::LookupByID<RE::TESObjectREFR>(run.target);
    using Request = bool(RE::MagicCaster *, RE::MagicItem *, RE::TESObjectREFR *, bool);
    static REL::Relocation<Request *> request{addr::kRequestCast};
    const bool began = caster && run.spell && request(caster, run.spell, target ? target : actor, false);
    log::player.debug("{}: asked the {} hand's caster for {} at {} -- {}", Describe(actor),
                      run.hand == ft::Hand::Left ? "left" : "right", log::NameOf(Named(run)),
                      log::NameOf(target ? target : actor), began ? "begun" : "turned away");
}

// A follower's release: the hand's release action at Ready, which the
// UseMagic procedure sends to fire the cast it began (29346 on AE) and the
// combat AI sends as a CombatAnimation; a stream is ended as the procedure
// ends one. Also what cancels a cast begun and not yet released.
void ReleaseOnCaster(RE::Actor *actor, RE::MagicCaster *caster, const Run &run, bool fire)
{
    if (!caster)
        return;
    if (!fire || run.state.sustained)
    {
        caster->InterruptCast(false);
        return;
    }
    const bool taken = RE::CombatAnimation::Execute(actor, run.hand == ft::Hand::Left
                                                               ? RE::CombatAnimation::ANIM::kActionLeftRelease
                                                               : RE::CombatAnimation::ANIM::kActionRightRelease);
    log::player.debug("{}: the {} hand's release{}", Describe(actor), run.hand == ft::Hand::Left ? "left" : "right",
                      taken ? "" : " was turned away");
}

// --- the run ----------------------------------------------------------------

const char *HandName(const Run &run)
{
    if (run.state.voice)
        return "voice";
    return run.hand == ft::Hand::Both ? "both" : run.hand == ft::Hand::Left ? "left" : "right";
}

void Report(const Run &run, RE::Actor *player, double now)
{
    const auto since = [&run](double at) { return at < 0.0 ? -1.0 : at - run.state.requestedAt; };
    const float magickaNow = player ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka) : -1.0f;
    std::vector<log::Field> fields{{"ruleIndex", run.ruleIndex},
                                   {"ruleName", run.ruleName},
                                   {"kind", run.state.voice ? "voice"
                                            : run.staff     ? "staff"
                                                            : "cast"},
                                   {"outcome", run.state.fired ? "cast" : "not-cast"},
                                   {"reason", run.state.reason},
                                   {"durationS", now - run.state.requestedAt}};
    log::AppendForm(fields, "formId", "formName", run.form);
    fields.emplace_back("hand", HandName(run));
    fields.emplace_back("lent", run.lent[0] || run.lent[1] || run.voiceLent || run.before[0].spell ||
                                    run.before[0].item || run.before[1].spell || run.before[1].item);
    fields.emplace_back("drew", run.state.drew);
    fields.emplace_back("pressedS", since(run.state.pressedAt));
    fields.emplace_back("readyS", since(run.state.readyAt));
    fields.emplace_back("settledS", since(run.state.settledAt));
    fields.emplace_back("releasedS", since(run.state.releasedAt));
    fields.emplace_back("firedS", since(run.state.firedAt));
    fields.emplace_back("highestState", run.state.highestState);
    if (run.voiceForm && run.voiceForm->Is(RE::FormType::Shout))
    {
        fields.emplace_back("wordsWanted", run.state.wordsWanted);
        fields.emplace_back("wordsHeld", run.state.wordsHeld);
    }
    fields.emplace_back("magickaAtRequest", static_cast<double>(run.magickaAtRequest));
    fields.emplace_back("magickaAtEnd", static_cast<double>(magickaNow));
    // A staff pays from its own charge: the copy's that the cast took, read
    // again now, wherever it has gone.
    const float chargeNow = run.staff && player ? StaffCastOf(player, run.staff).charge : -1.0f;
    if (run.staff)
    {
        fields.emplace_back("chargeAtRequest", static_cast<double>(run.chargeAtRequest));
        fields.emplace_back("chargeAtEnd", static_cast<double>(chargeNow));
    }
    log::player.event(log::Level::Info, "rule.resolved", player, fields,
                      "{} rule {} \"{}\": {} {} -- {}, after {:.2f} s ({} hand{}{}; settled at {:.2f} s, pressed at "
                      "{:.2f} s, ready at {:.2f} s, released at {:.2f} s, fired at {:.2f} s; caster state reached {}; "
                      "{} {:.0f} -> {:.0f})",
                      player ? log::NameOf(player) : "the caster", run.ruleIndex, run.ruleName,
                      run.state.voice ? "power or shout"
                      : run.staff     ? "staff"
                                      : "cast",
                      run.state.fired ? "cast" : "not cast", run.state.reason, now - run.state.requestedAt,
                      HandName(run), run.state.drew ? ", drawn for it" : "",
                      (run.lent[0] || run.lent[1] || run.voiceLent) ? ", lent" : "", since(run.state.settledAt),
                      since(run.state.pressedAt), since(run.state.readyAt), since(run.state.releasedAt),
                      since(run.state.firedAt), run.state.highestState, run.staff ? "charge" : "magicka",
                      run.staff ? run.chargeAtRequest : run.magickaAtRequest, run.staff ? chargeNow : magickaNow);
}

// Let go of a press not yet released -- at Ready that fires, earlier it
// cancels -- then give everything back and report.
void Finish(Run &run, RE::Actor *player, const char *reason, double now)
{
    if (run.state.reason.empty())
        run.state.reason = reason;
    log::player.debug("{}: over while {} -- {}", player ? Describe(player) : "the caster", ft::ToString(run.state.step),
                      run.state.reason);
    if (player && run.state.pressed && !run.state.released)
    {
        run.state.released = true;
        run.state.releasedAt = now;
        if (player->IsPlayerRef())
            ReleaseFor(run, now);
        else if (auto *caster = CasterOf(player, run); !Idle(caster))
            ReleaseOnCaster(player, caster, run, /*fire*/ false);
    }
    if (player)
    {
        Restore(player, run);
        if (run.state.drew)
            player->DrawWeaponMagicHands(false);
    }
    Report(run, player, now);
}

// One step, where the run can take it; the reason it is over, or null while
// it goes on. The step is core's (AdvancePlayerCast); this reads the player
// into what it asks about and performs the commands it sends.
const char *Advance(Run &run, RE::Actor *player, double now)
{
    ft::CastSeen seen;
    auto *state = player ? player->AsActorState() : nullptr;
    if (player && !state)
        return "no actor state";
    // The presses are the player's; a follower's caster is asked itself.
    const bool byPress = player && player->IsPlayerRef();
    if (player && !byPress && (player->IsDead() || !player->Is3DLoaded()))
        return "the caster is dead or gone";
    seen.player = player != nullptr;
    RE::MagicCaster *caster = nullptr;
    if (player)
    {
        const auto &runtime = player->GetActorRuntimeData();
        seen.placed = run.state.voice ? runtime.selectedPower == run.voiceForm : SpellPlaced(player, run);
        ft::Hear(seen, run.watch.HeardSoFar());
        const auto weapon = state->GetWeaponState();
        seen.weapon = weapon == RE::WEAPON_STATE::kDrawn      ? ft::CastSeen::Weapon::Drawn
                      : weapon == RE::WEAPON_STATE::kSheathed ? ft::CastSeen::Weapon::Sheathed
                                                              : ft::CastSeen::Weapon::Other;
        caster = CasterOf(player, run);
        seen.casterIdle = Idle(caster);
        seen.casterHasSpell = caster && caster->currentSpell;
        const auto casterState = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
        seen.caster = casterState == RE::MagicCaster::State::kNone      ? ft::CastSeen::Caster::None
                      : casterState == RE::MagicCaster::State::kCasting ? ft::CastSeen::Caster::Casting
                      : casterState == RE::MagicCaster::State::kReady   ? ft::CastSeen::Caster::Ready
                                                                        : ft::CastSeen::Caster::Other;
        seen.casterState = StateOf(caster);
        seen.othersIdle = Idle(player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand)) &&
                          Idle(player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther));
        seen.attacking = state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
        seen.blocking = player->IsBlocking();
        seen.buttonHeld = byPress && (ButtonHeld(AttackHandler(), true) || ButtonHeld(AttackHandler(), false));
        // Asked of the engine only where the step would press.
        if (run.state.step == ft::CastStep::Pressing)
            seen.refusal = Refused(caster, run.spell);
        seen.wordsCharged = run.state.shout ? WordsCharged(player) : -1;
        seen.onUsedList = run.state.voice && OnUsedList(player, run.spell);
    }
    const auto perform = [&](ft::CastCommand command) {
        switch (command)
        {
        case ft::CastCommand::LendVoice: {
            run.voiceBefore = player->GetActorRuntimeData().selectedPower;
            run.voiceLent = true;
            auto *manager = RE::ActorEquipManager::GetSingleton();
            if (auto *shout = run.voiceForm->As<RE::TESShout>(); shout && manager)
                manager->EquipShout(player, shout);
            else if (auto *power = run.voiceForm->As<RE::SpellItem>(); power && manager)
                manager->EquipSpell(player, power, Slot(kVoiceSlot));
            break;
        }
        case ft::CastCommand::LendHands:
            Lend(player, run);
            break;
        case ft::CastCommand::Draw:
            player->DrawWeaponMagicHands(true);
            break;
        case ft::CastCommand::Press:
            if (!byPress)
            {
                BeginOnCaster(player, caster, run);
                break;
            }
            PressFor(run);
            log::player.debug("{}: pressed for {} ({} hand{})", Describe(player), log::NameOf(Named(run)),
                              HandName(run), run.state.dual ? ", dual" : "");
            break;
        case ft::CastCommand::HoldPress:
            SendButton(VoiceHandler(), ShoutControl(), 1.0f, static_cast<float>(now - run.state.pressedAt));
            break;
        case ft::CastCommand::ReplayPress:
            // Sent while the caster has not begun: the player's held button,
            // and a follower's request again where it was turned away.
            if (byPress)
                SendHand(PressedHand(run), 1.0f, static_cast<float>(now - run.state.pressedAt));
            else
                BeginOnCaster(player, caster, run);
            break;
        case ft::CastCommand::Release:
            if (!byPress)
            {
                ReleaseOnCaster(player, caster, run, /*fire*/ true);
                break;
            }
            ReleaseFor(run, now);
            if (run.state.voice)
                log::player.debug("{}: after the release: {}", Describe(player), VoiceText(player, caster, run.spell));
            break;
        case ft::CastCommand::MarkPowerUsed:
            MarkPowerUsed(player, run.spell);
            break;
        }
    };
    return ft::AdvancePlayerCast(run.state, seen, now, perform);
}

// One run's step; true when it is over, finished and reported.
bool Step(Run &run, double now)
{
    auto *actor = RE::TESForm::LookupByID<RE::Actor>(run.actor);
    if (const char *over = Advance(run, actor, now))
    {
        Finish(run, actor, over, now);
        return true;
    }
    run.watch.SetWakes(ft::CastWakes(run.state.step));
    return false;
}

void Drop(std::size_t index)
{
    std::unique_lock lock(g_runsMutex);
    g_runs.erase(g_runs.begin() + static_cast<std::ptrdiff_t>(index));
    g_inFlight.store(!g_runs.empty(), std::memory_order_relaxed);
}

PlayerCastRequest Start(RE::Actor *player, Run run)
{
    if (RunOf(player))
        return PlayerCastRequest::AlreadyCasting;
    run.actor = player->GetFormID();
    run.state.requestedAt = TacticsSeconds();
    run.state.stepAt = run.state.requestedAt;
    run.magickaAtRequest = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
    // A staff's fire is any from the hand it is in: what the hand's slot
    // names as it fires is not yet seen in play, and the run holds the hand.
    run.watch = WatchGraph(
        player, ft::CastWakes(run.state.step),
        ft::CastOwnFires(run.state.voice, run.hand, run.staff ? std::nullopt : std::optional<std::uint32_t>(run.form)));
    log::player.debug("{}: {} requested from the {} hand", Describe(player), log::NameOf(Named(run)), HandName(run));
    {
        std::unique_lock lock(g_runsMutex);
        g_runs.push_back(std::move(run));
        g_inFlight.store(true, std::memory_order_relaxed);
    }
    // The first step now rather than on the next fast tick: a spell already
    // in a drawn hand is pressed on this frame.
    if (Step(g_runs.back(), g_runs.back().state.requestedAt))
        Drop(g_runs.size() - 1);
    return PlayerCastRequest::Started;
}

} // namespace

// The facts the hold is judged on; which of them wins is core's
// (core/PlayerCast.h, HeldBy, tested).
ft::HeldReason PlayerHeld(RE::Actor *player)
{
    ft::HoldFacts facts;
    facts.loaded = player && player->Is3DLoaded();
    if (!facts.loaded)
        return ft::HeldBy(facts);
    if (auto *ui = RE::UI::GetSingleton())
        facts.inDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
    if (auto *controls = RE::ControlMap::GetSingleton())
        facts.controlsDisabled = !controls->IsFightingControlsEnabled();
    if (auto *state = player->AsActorState())
    {
        facts.inFurniture = state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal;
        facts.knockedDown = state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal;
        facts.swimming = state->IsSwimming();
    }
    facts.mounted = player->IsOnMount();
    facts.inKillMove = player->IsInKillMove();
    // A beast race -- the werewolf, the vampire lord -- is not an NPC to the
    // engine's own keyword.
    auto *race = player->GetRace();
    facts.beastForm = race && !race->HasKeywordString("ActorTypeNPC");
    return ft::HeldBy(facts);
}

const char *ToString(PlayerCastRequest r) noexcept
{
    switch (r)
    {
    case PlayerCastRequest::Started:
        return "started";
    case PlayerCastRequest::AlreadyCasting:
        return "a cast of ours is in flight";
    case PlayerCastRequest::SpellMissing:
        return "not a spell, power or shout, or a staff carried with a use left";
    }
    return "?";
}

PlayerCastRequest RequestPlayerCast(RE::Actor *player, std::uint32_t spellFormID, float sustainSeconds, bool dualCast,
                                    int ruleIndex, std::string_view ruleName)
{
    // A spell, or a scroll: a scroll is a spell record that is also an item,
    // equipped to a hand as one and cast by the same press; the handler
    // fetches the item itself, and the engine spends it.
    auto *spell = player ? RE::TESForm::LookupByID<RE::SpellItem>(spellFormID) : nullptr;
    if (!spell || !(IsCastable(spell) || spell->GetSpellType() == RE::MagicSystem::SpellType::kScroll))
        return PlayerCastRequest::SpellMissing;
    Run run;
    run.form = spellFormID;
    run.spell = spell;
    // A dual cast is both hands, and the handler fires it from the left's
    // caster; only a spell either hand takes can be cast from both.
    run.state.dual = dualCast && GripOf(spell) == ft::Grip::Either;
    run.hand = run.state.dual ? ft::Hand::Both : ChooseHand(player, spell);
    run.source = run.hand == ft::Hand::Right ? RE::MagicSystem::CastingSource::kRightHand
                                             : RE::MagicSystem::CastingSource::kLeftHand;
    run.state.sustained = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    run.state.sustain = sustainSeconds > 0.0f ? sustainSeconds : kDefaultSustainSeconds;
    run.state.chargeTime = spell->GetChargeTime();
    run.ruleIndex = ruleIndex;
    run.ruleName = ruleName;
    return Start(player, std::move(run));
}

PlayerCastRequest RequestPlayerVoice(RE::Actor *player, std::uint32_t formID, int ruleIndex, std::string_view ruleName)
{
    auto *form = player ? RE::TESForm::LookupByID(formID) : nullptr;
    Run run;
    if (auto *shout = form ? form->As<RE::TESShout>() : nullptr)
    {
        run.spell = shout->variations[0].spell;
        run.state.wordsWanted = (std::max)(0, HighestUnlockedWord(shout));
        run.state.shout = true;
    }
    else if (auto *power = form ? form->As<RE::SpellItem>() : nullptr; power && IsPower(power))
        run.spell = power;
    else
        return PlayerCastRequest::SpellMissing;
    run.state.voice = true;
    run.form = formID;
    run.voiceForm = form;
    run.ruleIndex = ruleIndex;
    run.ruleName = ruleName;
    return Start(player, std::move(run));
}

PlayerCastRequest RequestStaffCast(RE::Actor *actor, std::uint32_t staffFormID, std::uint32_t targetId,
                                   float sustainSeconds, int ruleIndex, std::string_view ruleName)
{
    if (!actor)
        return PlayerCastRequest::SpellMissing;
    auto *staff = RE::TESForm::LookupByID<RE::TESObjectWEAP>(staffFormID);
    const StaffCast copy = StaffCastOf(actor, staff);
    if (!copy.enchantment || !copy.canPay)
        return PlayerCastRequest::SpellMissing;
    Run run;
    run.form = staffFormID;
    run.staff = staff;
    run.spell = copy.enchantment;
    run.target = targetId;
    run.chargeAtRequest = copy.charge;
    // The hand it is in. One from the bag goes to the right, which leaves a
    // shield where it is -- or to the left, where the right holds another
    // copy of it, one that cannot pay.
    run.hand = copy.hand != ft::Hand::None                ? copy.hand
               : actor->GetEquippedObject(false) == staff ? ft::Hand::Left
                                                          : ft::Hand::Right;
    run.source = run.hand == ft::Hand::Right ? RE::MagicSystem::CastingSource::kRightHand
                                             : RE::MagicSystem::CastingSource::kLeftHand;
    run.state.sustained = copy.enchantment->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    run.state.sustain = sustainSeconds > 0.0f ? sustainSeconds : kDefaultSustainSeconds;
    run.state.chargeTime = copy.enchantment->GetChargeTime();
    run.ruleIndex = ruleIndex;
    run.ruleName = ruleName;
    return Start(actor, std::move(run));
}

bool IsPlayerMidCast()
{
    return RunOf(RE::PlayerCharacter::GetSingleton()) != nullptr;
}

bool IsMidHandCast(const RE::Actor *actor)
{
    std::shared_lock lock(g_runsMutex);
    return RunOf(actor) != nullptr;
}

bool IsHandCastEquip(const RE::Actor *actor, std::uint32_t formID)
{
    std::shared_lock lock(g_runsMutex);
    const Run *run = formID != 0 ? RunOf(actor) : nullptr;
    if (!run)
        return false;
    if (run->staff && run->staff->GetFormID() == formID)
        return true;
    return std::ranges::any_of(run->before, [formID](const Held &held) {
        return (held.spell && held.spell->GetFormID() == formID) || (held.item && held.item->GetFormID() == formID);
    });
}

bool AnyPlayerCastInFlight() noexcept
{
    return g_inFlight.load(std::memory_order_relaxed);
}

void TickPlayerCasts(double now)
{
    for (std::size_t i = 0; i < g_runs.size();)
    {
        if (Step(g_runs[i], now))
            Drop(i);
        else
            ++i;
    }
}

void EndAllPlayerCasts(const char *why)
{
    const double now = TacticsSeconds();
    while (!g_runs.empty())
    {
        Run &run = g_runs.back();
        Finish(run, RE::TESForm::LookupByID<RE::Actor>(run.actor), why, now);
        Drop(g_runs.size() - 1);
    }
}

void ResetPlayerCasts()
{
    std::unique_lock lock(g_runsMutex);
    g_runs.clear();
    g_inFlight.store(false, std::memory_order_relaxed);
}

} // namespace ft::game
