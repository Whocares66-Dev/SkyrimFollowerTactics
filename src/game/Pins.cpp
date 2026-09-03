// Pins: what stays in a hand, and how the promise is kept. The rules are
// core/Loadout.cpp; this is where they meet the engine. Everything here runs
// on the game thread -- the tick, or a task the panel queued.

#include "game/Pins.h"

#include "game/Tactics.h"
#include "game/Util.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft::game
{
namespace
{

// What the panel has pinned on each follower. Written by the request task,
// read by the tick; both on the game thread, but the lock costs nothing and
// keeps the next writer honest.
std::mutex g_pinMutex;
// What the panel has pinned on each follower: the form, and the hands it is
// pinned to (None for armour and ammunition). Written by the request task,
// read by the tick; both on the game thread, but the lock costs nothing and
// keeps the next writer honest.
std::unordered_map<ft::ActorId, std::unordered_map<std::uint32_t, Hand>> g_pins;

// The hand's equip slot record, by FormID: LeftHand 013F43, RightHand
// 013F42 in Skyrim.esm. Not through the default object table, which did
// not answer for these on this game (01:29): a null slot here means "the
// default", and the default is the right hand -- which is where every
// left-hand dagger went (01:51).
const RE::BGSEquipSlot *HandSlot(Hand hand)
{
    return RE::TESForm::LookupByID<RE::BGSEquipSlot>(hand == Hand::Left ? 0x00013F43 : 0x00013F42);
}

// The hand a slot record names: RightHand 013F42, LeftHand 013F43,
// BothHands 013F45. EitherHand (013F44) and no record are None.
Hand SlotHand(const RE::BGSEquipSlot *slot)
{
    switch (slot ? slot->GetFormID() : 0)
    {
    case 0x00013F42:
        return Hand::Right;
    case 0x00013F43:
        return Hand::Left;
    case 0x00013F45:
        return Hand::Both;
    default:
        return Hand::None;
    }
}

// For the log: which hand an entry of the AI's list would go into.
const char *HandTag(Hand hand)
{
    switch (hand)
    {
    case Hand::Left:
        return "[L]";
    case Hand::Right:
        return "[R]";
    case Hand::Both:
        return "[LR]";
    default:
        return "";
    }
}

// The planner's description of a form: which hands its record lets it
// take, whether the combat AI would choose it, which body slots it covers.
// The ONLY place the pin rules meet a record; the rules themselves are in
// core/Loadout.cpp, where they are tested.
Holdable DescribeHoldable(RE::Actor *actor, RE::TESForm *form)
{
    Holdable thing;
    thing.form = form->GetFormID();
    if (auto *weapon = form->As<RE::TESObjectWEAP>())
    {
        const bool bothHands =
            weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
        thing.grip = bothHands ? Grip::Both : Grip::Either;
    }
    else if (auto *spell = form->As<RE::SpellItem>())
    {
        if (spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell)
            return thing; // a power, an ability: no hand
        // The slot records, by FormID from Skyrim.esm: RightHand 013F42,
        // LeftHand 013F43, EitherHand 013F44, BothHands 013F45. The default
        // object table did not answer for them on this game.
        const auto *slot = spell->GetEquipSlot();
        const std::uint32_t slotId = slot ? slot->GetFormID() : 0;
        thing.grip = spell->IsTwoHanded()   ? Grip::Both
                     : slotId == 0x00013F43 ? Grip::LeftOnly
                     : slotId == 0x00013F42 ? Grip::RightOnly
                                            : Grip::Either;
        // Above her skill in its school: the combat AI will not choose it.
        const auto *costliest = spell->GetCostliestEffectItem();
        const auto *effect = costliest ? costliest->baseEffect : nullptr;
        auto *owner = actor->AsActorValueOwner();
        if (effect && owner)
            thing.unusable = effect->GetMinimumSkillLevel() > owner->GetActorValue(effect->GetMagickSkill());
    }
    else if (auto *armor = form->As<RE::TESObjectARMO>())
    {
        thing.slots = static_cast<std::uint32_t>(armor->GetSlotMask());
        thing.grip = ArmorGrip(armor);
    }
    else if (form->Is(RE::FormType::Light))
    {
        thing.grip = Grip::LeftOnly;
    }
    else if (form->Is(RE::FormType::Ammo))
    {
        thing.ammo = true;
    }
    return thing;
}

// This follower's pins as the planner takes them.
// One pin as the planner takes it: the hands, and for a pin with none,
// the body slots or the quiver it holds.
Pin PlannedPin(RE::Actor *actor, std::uint32_t form, Hand hands)
{
    Pin pin;
    pin.form = form;
    pin.hands = hands;
    if (auto *thing = RE::TESForm::LookupByID(form); thing && hands == Hand::None)
    {
        const Holdable described = DescribeHoldable(actor, thing);
        pin.slots = described.slots;
        pin.ammo = described.ammo;
    }
    return pin;
}

std::vector<Pin> PinsOf(ft::ActorId id)
{
    std::scoped_lock lock(g_pinMutex);
    std::vector<Pin> out;
    auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
    if (const auto it = g_pins.find(id); actor && it != g_pins.end())
    {
        for (const auto &[form, hands] : it->second)
            out.push_back(PlannedPin(actor, form, hands));
    }
    return out;
}

// Is the form in those hands right now?
bool EquippedIn(RE::Actor *actor, RE::TESForm *form, Hand hands)
{
    if (auto *spell = form->As<RE::SpellItem>())
    {
        const auto &data = actor->GetActorRuntimeData();
        const bool left = data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell;
        const bool right = data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell;
        return (!Overlap(hands, Hand::Left) || left) && (!Overlap(hands, Hand::Right) || right);
    }
    // A two-hander sits in the right hand; the left reports it too, or
    // nothing. Asking the right is enough.
    if (hands == Hand::Both)
        return actor->GetEquippedObject(false) == form;
    return actor->GetEquippedObject(hands == Hand::Left) == form;
}

// Put a pinned form on, in its hands. `now` clears the engine's queue flag:
// queued -- the potion path's shape, right in a fight -- the change waits
// for the actor's next update, and an actor gets no update while the clock
// is frozen, so a click in the panel showed nothing until the panel closed
// (20:26, Marcurio's boots). The click path takes `now`; the tick, with
// time running, keeps the queue. Items go on with the prevent-removal flag,
// the pin; a spell has no such flag.
// For the log: the selected spell and the hand's caster, both hands. The
// two differ while a spell equip is only half done -- the menu's equip
// sounds once at the click and once more when the panel closes (14:19),
// and this says which half plays the second.
std::string CasterState(RE::Actor *actor)
{
    const auto &data = actor->GetActorRuntimeData();
    const auto name = [](const RE::MagicItem *spell) { return spell && spell->GetName() ? spell->GetName() : "-"; };
    const auto *left = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
    const auto *right = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
    return std::string("selected L=") + name(data.selectedSpells[RE::Actor::SlotTypes::kLeftHand]) +
           " R=" + name(data.selectedSpells[RE::Actor::SlotTypes::kRightHand]) +
           " -- caster L=" + name(left ? left->currentSpell : nullptr) +
           " R=" + name(right ? right->currentSpell : nullptr);
}

void EquipPinned(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager)
        return;
    if (auto *spell = form->As<RE::SpellItem>())
    {
        for (const Hand hand : {Hand::Left, Hand::Right})
        {
            if (Overlap(hands, hand))
                EquipSpellIn(actor, spell, hand);
        }
        return;
    }
    auto *object = form->As<RE::TESBoundObject>();
    if (!object)
        return;
    // A one-handed weapon goes to the hand asked for; everything else finds
    // its own slot.
    const RE::BGSEquipSlot *slot = nullptr;
    if (object->Is(RE::FormType::Weapon) && hands != Hand::Both && hands != Hand::None)
        slot = HandSlot(hands);
    manager->EquipObject(actor, object, nullptr, 1, slot, !now, true, false, false);
}

// Equip an item WITHOUT the pin, for letting go of one while it stays on.
void EquipPlain(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager)
        return;
    const RE::BGSEquipSlot *slot = nullptr;
    if (object->Is(RE::FormType::Weapon) && hands != Hand::Both && hands != Hand::None)
        slot = HandSlot(hands);
    manager->EquipObject(actor, object, nullptr, 1, slot, false, false, false, false);
}

// Take a form off.
//
// Items go through the equip manager WITHOUT the prevent-equip flag: the
// Creation Kit wiki notes that flag does nothing for weapons on an NPC and
// works only too well for ammunition, leaving an archer holding a bow she
// cannot use. A spell has no unequip in CommonLibSSE 3.7.0 or in SKSE; the
// engine's is the Papyrus native Actor.UnequipSpell(spell, source), 0 for
// the left hand and 1 for the right, so it is dispatched to the script VM,
// which runs it on the game thread a frame later.
// Is the item on, in a hand or worn? The no-op check before an unequip.
bool Worn(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    if (hands != Hand::None)
        return EquippedIn(actor, object, hands);
    auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
    const auto found = inventory.find(object);
    return found != inventory.end() && found->second.second && found->second.second->IsWorn();
}

// Take a spell out of a hand, or an item off. A no-op when it is not
// there, like EquipSpellIn: a spell's unequip is a Papyrus call and a
// republish, and neither is owed for a hand that was already empty.
void UnequipForm(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now)
{
    if (auto *spell = form->As<RE::SpellItem>())
    {
        auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
        if (!policy)
            return;
        const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
        for (const Hand hand : {Hand::Left, Hand::Right})
        {
            if (!Overlap(hands, hand) || !EquippedIn(actor, spell, hand))
                continue;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
            vm->DispatchMethodCall2(
                handle, "Actor", "UnequipSpell",
                RE::MakeFunctionArguments(std::move(spell), static_cast<std::int32_t>(hand == Hand::Left ? 0 : 1)),
                result);
        }
        return;
    }
    if (auto *object = form->As<RE::TESBoundObject>())
    {
        if (!Worn(actor, object, hands))
            return;
        // A one-handed weapon comes out of the hand named; anything else
        // out of wherever it is.
        const RE::BGSEquipSlot *slot = nullptr;
        if (object->Is(RE::FormType::Weapon) && (hands == Hand::Left || hands == Hand::Right))
            slot = HandSlot(hands);
        if (auto *manager = RE::ActorEquipManager::GetSingleton())
            manager->UnequipObject(actor, object, nullptr, 1, slot, !now, false, false, false, nullptr);
    }
}

// Whether a left-hand weapon pin also gives her a combat style that allows
// dual wielding. Off: a trial of what the unmodified style does with the
// weapon (2026-09-03).
constexpr bool kDualWieldOnLeftPin = false;

// Let her dual wield.
//
// csHumanMagic, Marcurio's style, and most vanilla styles do not allow it,
// and the AI of an actor whose style forbids it takes a left-hand weapon
// straight off again -- the loop seen with the dagger (00:26), the watchdog
// putting it back every half second. The style is shared by every mage in
// the game, so it is not edited in place: she gets a runtime copy with the
// flag set, on her record and on her live combat controller if she has
// one. Two flags are set because CommonLibSSE names the bit in two places
// (the DATA flags and the record header) and which the engine reads is
// unverified. Not saved: redone on every left-hand pin, gone with the
// session, like the pins.
[[maybe_unused]] void AllowDualWield(RE::Actor *actor)
{
    auto *npc = actor->GetActorBase();
    auto *style = npc ? npc->GetCombatStyle() : nullptr;
    if (!style)
        return;
    if (style->flags.all(RE::TESCombatStyle::FLAG::kAllowDualWielding))
    {
        logger::info("{} combat style {:08X} already allows dual wielding", Describe(actor), style->GetFormID());
        return;
    }

    auto *copy = style->CreateDuplicateForm(true, nullptr);
    auto *ours = copy ? copy->As<RE::TESCombatStyle>() : nullptr;
    if (!ours)
    {
        logger::warn("{} cannot be allowed to dual wield: combat style {:08X} would not duplicate", Describe(actor),
                     style->GetFormID());
        return;
    }
    // CreateDuplicateForm gives a NEW combat style at the engine's defaults
    // -- offensive 0.24, every score 1 -- not a copy of hers (01:55, the
    // Combat Style tab on the copy). The data is five plain structs and the
    // flags, so it is copied by hand.
    ours->generalData = style->generalData;
    ours->meleeData = style->meleeData;
    ours->closeRangeData = style->closeRangeData;
    ours->longRangeData = style->longRangeData;
    ours->flightData = style->flightData;
    ours->flags = style->flags;
    ours->flags.set(RE::TESCombatStyle::FLAG::kAllowDualWielding);
    ours->formFlags |= RE::TESCombatStyle::RecordFlags::kAllowDualWielding;
    npc->SetCombatStyle(ours);
    if (auto *controller = actor->GetActorRuntimeData().combatController)
        controller->combatStyle = ours;
    logger::info("{} allowed to dual wield: combat style {:08X} copied as {:08X} with the flag set", Describe(actor),
                 style->GetFormID(), ours->GetFormID());
}

// A pinned item is locked against the engine's own swap -- the Creation Kit
// wiki: prevent-removal "does prevent removal when using EquipItem(OtherItem)"
// -- so before something new goes on, whatever it displaces has to be
// unpinned and taken off by us, or the equip silently does nothing. That is
// what happened with iron armour pinned and robes clicked.
void ReleaseConflictingPins(RE::Actor *actor, std::unordered_map<std::uint32_t, Hand> &pins, RE::TESForm *incoming,
                            Hand hands)
{
    const Holdable coming = DescribeHoldable(actor, incoming);
    for (auto it = pins.begin(); it != pins.end();)
    {
        auto *held = RE::TESForm::LookupByID(it->first);
        const Holdable holding = held ? DescribeHoldable(actor, held) : Holdable{};
        if (!held || held == incoming || !Conflicts(coming, hands, holding, it->second))
        {
            ++it;
            continue;
        }
        const char *name = held->GetName() ? held->GetName() : "?";
        // A pin holding both hands with one thing per hand -- two daggers
        // -- gives up only the hand asked for and keeps the other. A
        // two-hander, or a pin with no hand, goes whole.
        const Hand taken = Common(hands, it->second);
        const bool partly = holding.grip == Grip::Either && taken != Hand::None && taken != it->second;
        if (partly)
        {
            logger::info("{} unpinning {} from one hand to make room", Describe(actor), name);
            UnequipForm(actor, held, taken, true);
            it->second = Without(it->second, taken);
            ++it;
            continue;
        }
        logger::info("{} unpinning {} to make room", Describe(actor), name);
        UnequipForm(actor, held, it->second, true);
        it = pins.erase(it);
    }
}

// The watchdog: put back any pinned form the game has taken off, and forget
// pins for items no longer carried.
//
// Its own pass, not a rider on the inventory scan, and gated on the pin map:
// with nothing pinned it costs a lock and a look at an empty map. With pins
// it asks the engine for each pinned item's entry alone -- a walk of pointer
// compares, no names, no strings -- so a follower with two pins costs two
// lookups a tick, not a sweep of her bag.
//
// In a fight her hands are the combat AI's, and the rules': a mage with a
// pinned dagger wants that hand for a spell, and putting the dagger back
// every half second had the two trading blows -- the flicker seen with
// Marcurio. So anything pinned to a hand -- weapon, spell, shield, torch --
// is what she carries out of combat and starts a fight with; once it is
// over, it goes back. Armour and ammunition are contested by nothing and
// hold throughout.
void EnforcePins(const std::vector<RE::Actor *> &followers)
{
    std::scoped_lock lock(g_pinMutex);
    if (g_pins.empty())
        return;

    for (auto *actor : followers)
    {
        const auto it = g_pins.find(actor->GetFormID());
        if (it == g_pins.end())
            continue;
        auto &pins = it->second;
        const bool fighting = actor->IsInCombat();

        for (auto pin = pins.begin(); pin != pins.end();)
        {
            auto *form = RE::TESForm::LookupByID(pin->first);
            if (!form)
            {
                pin = pins.erase(pin);
                continue;
            }
            const Hand hands = pin->second;

            // Spells first: a SpellItem is a bound object too, and the
            // inventory branch dropped every spell pin as "no longer
            // carried" (00:26, Close Wounds).
            if (form->Is(RE::FormType::Spell))
            {
                if (!fighting && !EquippedIn(actor, form, hands))
                {
                    logger::info("{} put away pinned {} -- readying it again", Describe(actor),
                                 form->GetName() ? form->GetName() : "?");
                    EquipPinned(actor, form, hands, false);
                }
            }
            else if (auto *object = form->As<RE::TESBoundObject>())
            {
                auto inventory =
                    actor->GetInventory([object](RE::TESBoundObject &candidate) { return &candidate == object; });
                const auto found = inventory.find(object);
                const bool carried = found != inventory.end() && found->second.first > 0;
                if (!carried)
                {
                    logger::info("{} no longer carries {} -- pin dropped", Describe(actor),
                                 object->GetName() ? object->GetName() : "?");
                    pin = pins.erase(pin);
                    continue;
                }
                const bool on = hands == Hand::None ? (found->second.second && found->second.second->IsWorn())
                                                    : EquippedIn(actor, object, hands);
                if (!on && !(fighting && hands != Hand::None))
                {
                    logger::info("{} took off pinned {} -- putting it back on", Describe(actor),
                                 object->GetName() ? object->GetName() : "?");
                    EquipPinned(actor, object, hands, false);
                }
            }
            ++pin;
        }
    }

    std::erase_if(g_pins, [](const auto &entry) { return entry.second.empty(); });
}

// --- keeping the AI to the pins ------------------------------------------
//
// The combat AI chooses from a list of its own, the combat inventory it
// builds when a fight begins: spells and items together, scored, in seven
// arrays by role. It does not read her spell lists or her bag again during
// the fight -- Firebolt was cast after being removed from her record, a
// removed dagger never was. So a pin is kept by editing THAT list: every
// tick she fights with something pinned to a hand, every spell or item
// that would take that hand is pruned from the list and whatever is pinned
// goes back into its hand. Nothing of hers changes, nothing is saved, and
// the engine discards the list when the fight ends, so there is nothing to
// restore. The first version removed competing spells from her record for
// the life of a pin; it worked, and left her without those spells for
// every menu, script and mod in between (2026-09-03).

// What the combat AI is choosing from: its combat inventory, seven arrays
// of scored options built for the fight. Logged once per fight, by name,
// to learn the layout -- the AI cast a spell we had removed from her lists
// (03:18), so this list, not those, is what it reads.
std::unordered_set<ft::ActorId> g_probedFights;

// When a list was marked for rebuild, per follower, to measure how soon the
// engine clears the flag: that is the rebuild, and its latency decides
// whether a mid-fight pin change takes effect in a frame or a while.
std::unordered_map<ft::ActorId, double> g_rebuildMarkedAt;

void MeasureRebuild(RE::Actor *actor)
{
    const ft::ActorId id = actor->GetFormID();
    const auto it = g_rebuildMarkedAt.find(id);
    if (it == g_rebuildMarkedAt.end())
        return;
    auto *controller = actor->GetActorRuntimeData().combatController;
    if (!controller || !controller->inventory)
    {
        g_rebuildMarkedAt.erase(it);
        return;
    }
    if (controller->inventory->dirty)
        return;
    logger::info("{} combat list rebuilt {:.0f} ms after being marked", Describe(actor),
                 (NowSeconds() - it->second) * 1000.0);
    g_rebuildMarkedAt.erase(it);
}

void ProbeCombatInventory(RE::Actor *actor)
{
    const ft::ActorId id = actor->GetFormID();
    auto *controller = actor->GetActorRuntimeData().combatController;
    if (!controller || !controller->inventory)
    {
        g_probedFights.erase(id);
        return;
    }
    if (!g_probedFights.insert(id).second)
        return;
    std::unordered_set<const RE::TESForm *> listed;
    for (int slot = 0; slot < 7; ++slot)
    {
        std::string names;
        for (const auto &entry : controller->inventory->inventoryItems[slot])
        {
            const auto *form = entry ? entry->item : nullptr;
            listed.insert(form);
            names += (names.empty() ? "" : ", ") + std::string(form && form->GetName() ? form->GetName() : "?") +
                     HandTag(entry ? SlotHand(entry->itemSlot.equipSlot) : Hand::None);
        }
        logger::info("{} combat inventory [{}]: {}", Describe(actor), slot, names.empty() ? "-" : names);
    }
    // Which of her spells the AI did not list, and her magicka at the
    // moment, since a cost above the pool is the first guess at the filter
    // that kept a pinned Chain Lightning out (03:25).
    std::string missing;
    const auto consider = [&](RE::SpellItem *spell) {
        if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell && !listed.contains(spell))
            missing += (missing.empty() ? "" : ", ") + std::string(spell->GetName() ? spell->GetName() : "?") + " (" +
                       std::to_string(static_cast<int>(spell->CalculateMagickaCost(actor))) + ")";
    };
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
            for (std::uint32_t i = 0; i < list->numSpells; ++i)
                consider(list->spells[i]);
    }
    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
        consider(spell);
    auto *owner = actor->AsActorValueOwner();
    // The two form arrays beside the seven: if the 5 s rebuild
    // (fCombatInventoryUpdateTimer) draws from these rather than from the
    // bag, pruning them would hold where pruning the seven does not.
    const auto forms = [](const RE::BSTArray<RE::TESForm *> &array) {
        std::string names;
        for (const auto *form : array)
            names += (names.empty() ? "" : ", ") + std::string(form && form->GetName() ? form->GetName() : "?");
        return names.empty() ? std::string("-") : names;
    };
    logger::info("{} combat inventory forms A: {}", Describe(actor), forms(controller->inventory->unk0B0));
    logger::info("{} combat inventory forms B: {}", Describe(actor), forms(controller->inventory->unk0C8));
    logger::info("{} combat inventory left out: {} -- magicka {:.0f}/{:.0f}", Describe(actor),
                 missing.empty() ? "nothing" : missing, owner ? owner->GetActorValue(RE::ActorValue::kMagicka) : 0.0f,
                 owner ? owner->GetPermanentActorValue(RE::ActorValue::kMagicka) : 0.0f);
}

// When the last prune took something, per follower: the interval between
// regrowths is the clue to what rebuilds the list.
std::unordered_map<ft::ActorId, double> g_lastPruneAt;

int PruneCombatList(RE::Actor *actor)
{
    auto *controller = actor->GetActorRuntimeData().combatController;
    if (!controller || !controller->inventory)
        return 0;
    const std::vector<Pin> pins = PinsOf(actor->GetFormID());
    const bool dirtyNow = controller->inventory->dirty;
    int removed = 0;
    std::string names;
    for (auto &array : controller->inventory->inventoryItems)
    {
        for (auto it = array.begin(); it != array.end();)
        {
            auto *form = *it ? (*it)->item : nullptr;
            const Hand slot = *it ? SlotHand((*it)->itemSlot.equipSlot) : Hand::None;
            if (form && KeptFromAI(pins, DescribeHoldable(actor, form), slot))
            {
                names +=
                    (names.empty() ? "" : ", ") + std::string(form->GetName() ? form->GetName() : "?") + HandTag(slot);
                it = array.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
    }
    if (removed > 0)
    {
        const ft::ActorId id = actor->GetFormID();
        const double now = NowSeconds();
        const auto last = g_lastPruneAt.find(id);
        if (last == g_lastPruneAt.end())
            logger::info("{} pruned {} from the combat list: {}", Describe(actor), removed, names);
        else
            logger::info("{} pruned {} from the combat list: {} -- back {:.1f} s after the last prune, dirty={}",
                         Describe(actor), removed, names, now - last->second, dirtyNow);
        g_lastPruneAt[id] = now;
    }
    return removed;
}

// After a prune, whatever is pinned to a hand goes back into it: what the
// AI had reached for is no longer on its list, so this holds.
void ReadyPinnedHands(RE::Actor *actor)
{
    std::unordered_map<std::uint32_t, Hand> pins;
    {
        std::scoped_lock lock(g_pinMutex);
        pins = g_pins[actor->GetFormID()];
    }
    for (const auto &[form, hands] : pins)
    {
        auto *thing = RE::TESForm::LookupByID(form);
        if (thing && hands != Hand::None && !EquippedIn(actor, thing, hands))
        {
            logger::info("{} readying pinned {} after the prune", Describe(actor),
                         thing->GetName() ? thing->GetName() : "?");
            EquipPinned(actor, thing, hands, false);
        }
    }
}

// Followers whose view is to be republished on the next pacing beat, whether
// or not the clock is running. A spell leaves a hand through the Papyrus
// native, which the script VM runs a frame or so after the request, so
// the view republished in the request still showed the spell in hand and a
// second click was needed to see it gone (04:15). Game thread only.
std::unordered_set<ft::ActorId> g_republish;

// The hands this follower's pins hold, all together.
Hand PinnedHands(ft::ActorId id)
{
    return ft::PinnedHands(PinsOf(id));
}

} // namespace

// Mark the scanned items and spells that are pinned, and those the AI is
// kept from, for the panel's cells; and drop any pin for something the
// scans did not find: sold, dropped, the last arrow shot. The watchdog
// catches that case on its own, but there is no reason to leave a dead pin
// for it to find.
void MarkPins(RE::Actor *actor, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic)
{
    std::scoped_lock lock(g_pinMutex);
    const auto it = g_pins.find(actor->GetFormID());
    if (it == g_pins.end() || it->second.empty())
        return;
    auto &pins = it->second;
    std::vector<Pin> asPlanned;
    for (const auto &[form, hands] : pins)
        asPlanned.push_back(PlannedPin(actor, form, hands));

    // The tooltip's reason: the pins in the way, one per line, "Firebolt
    // is pinned". Which hand or slot is plain from the table itself.
    const auto why = [&](const std::vector<Pin> &shadowing) {
        std::string lines;
        for (const Pin &pin : shadowing)
        {
            const auto *holder = RE::TESForm::LookupByID(pin.form);
            const char *name = holder && holder->GetName() ? holder->GetName() : "Something";
            lines += (lines.empty() ? "" : "\n") + std::string(name) + " is pinned";
        }
        return lines;
    };

    std::unordered_set<std::uint32_t> present;
    const auto mark = [&](std::uint32_t form, bool &left, bool &right, bool &aside, std::string &asideBy, bool *whole) {
        present.insert(form);
        if (const auto pin = pins.find(form); pin != pins.end())
        {
            left = Overlap(pin->second, Hand::Left);
            right = Overlap(pin->second, Hand::Right);
            if (whole)
                *whole = pin->second == Hand::None;
            return;
        }
        auto *thing = RE::TESForm::LookupByID(form);
        if (!thing)
            return;
        const Holdable described = DescribeHoldable(actor, thing);
        aside = SetAside(asPlanned, described);
        if (aside)
            asideBy = why(Shadowing(asPlanned, described));
    };
    for (auto &item : items)
        mark(item.form, item.pinnedLeft, item.pinnedRight, item.setAside, item.asideBy, &item.pinned);
    for (auto &entry : magic)
        mark(entry.form, entry.pinnedLeft, entry.pinnedRight, entry.setAside, entry.asideBy, nullptr);
    std::erase_if(pins, [&](const auto &pin) { return !present.contains(pin.first); });
}

bool EquipSpellIn(RE::Actor *actor, RE::SpellItem *spell, Hand hand)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager || !actor || !spell)
        return false;
    const auto &data = actor->GetActorRuntimeData();
    const bool left = data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell;
    const bool right = data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell;
    const bool already = hand == Hand::Left ? left : hand == Hand::Right ? right : (left || right);
    if (already)
        return false;
    manager->EquipSpell(actor, spell, hand == Hand::None ? nullptr : HandSlot(hand));
    return true;
}

// EXPERIMENT (14:33): the combat AI re-adds the pruned melee weapons,
// spells and shield to its list every 2.5 to 7 s with the dirty flag
// clear, and fCombatInventoryUpdateTimer is 5. Raise it, in memory only,
// and see whether the regrowth stops. Global: every NPC's periodic
// re-scan slows with it; the dirty flag still rebuilds on a change.
constexpr float kCombatInventoryUpdateTimer = 1.0e6f;

void LogCombatInventorySettings()
{
    auto *collection = RE::GameSettingCollection::GetSingleton();
    if (!collection)
        return;
    if (auto *timer = collection->GetSetting("fCombatInventoryUpdateTimer"))
    {
        logger::info("setting fCombatInventoryUpdateTimer was {} -- set to {}", timer->GetFloat(),
                     kCombatInventoryUpdateTimer);
        timer->data.f = kCombatInventoryUpdateTimer;
    }
    for (const auto &entry : collection->settings)
    {
        const RE::Setting *setting = entry.second;
        if (!setting || !setting->GetName())
            continue;
        const std::string_view name = setting->GetName();
        if (name.find("CombatInventory") == std::string_view::npos &&
            name.find("CombatEquip") == std::string_view::npos && name.find("Equipment") == std::string_view::npos)
            continue;
        switch (setting->GetType())
        {
        case RE::Setting::Type::kFloat:
            logger::info("setting {} = {}", name, setting->GetFloat());
            break;
        case RE::Setting::Type::kSignedInteger:
            logger::info("setting {} = {}", name, setting->GetSInt());
            break;
        case RE::Setting::Type::kBool:
            logger::info("setting {} = {}", name, setting->GetBool());
            break;
        default:
            logger::info("setting {} (not a number)", name);
            break;
        }
    }
}

void RepublishOwed()
{
    // Views owed after a spell unequip: the
    // clock is frozen while the panel is open, and this is what the panel
    // is waiting on.
    for (const ft::ActorId id : g_republish)
    {
        if (auto *actor = RE::TESForm::LookupByID<RE::Actor>(id))
        {
            logger::info("{} time running again -- {}", Describe(actor), CasterState(actor));
            PublishFollower(actor);
        }
    }
    g_republish.clear();
}

void KeepPins(const std::vector<RE::Actor *> &followers)
{
    EnforcePins(followers);
    for (auto *follower : followers)
    {
        ProbeCombatInventory(follower);
        MeasureRebuild(follower);
        if (follower->IsInCombat())
        {
            if (PinnedHands(follower->GetFormID()) != Hand::None && PruneCombatList(follower) > 0)
                ReadyPinnedHands(follower);
        }
    }
}

void RequestWear(ft::ActorId id, std::uint32_t form, WearRequest request, Hand hand)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    // Queued to the game thread and run there once. The panel is open while
    // this is clicked, and with FreezeTimeOnMenu the tick is held, so the
    // task also republishes her view: the cell answers now rather than when
    // the panel closes.
    task->AddTask([id, form, request, hand]() mutable {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *thing = RE::TESForm::LookupByID(form);
        if (!actor || !thing)
            return;

        const Holdable described = DescribeHoldable(actor, thing);
        Hand hands = HandsFor(described.grip, hand);
        if (request == WearRequest::Pin && !Pinnable(described))
        {
            // The panel does not offer this, but a pin is a promise, and it
            // is kept here too: the AI would not choose it, so equip only.
            logger::info("{} {} cannot be pinned (the AI would not choose it); equipping instead", Describe(actor),
                         thing->GetName() ? thing->GetName() : "?");
            request = WearRequest::Equip;
        }
        // One weapon cannot be in both hands. Asked to move her only copy to
        // the other hand, take it out of the first; otherwise the engine's
        // equip, finding none free, conjures a second (02:05, the doubled
        // dagger). Two in the bag may go one per hand.
        bool moving = false;
        if (request == WearRequest::Pin && thing->Is(RE::FormType::Weapon) &&
            (hands == Hand::Left || hands == Hand::Right))
        {
            const Hand other = hands == Hand::Left ? Hand::Right : Hand::Left;
            if (EquippedIn(actor, thing, other))
            {
                auto *object = thing->As<RE::TESBoundObject>();
                auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
                const auto found = inventory.find(object);
                moving = (found != inventory.end() ? found->second.first : 0) < 2;
            }
        }

        {
            std::scoped_lock lock(g_pinMutex);
            auto &pins = g_pins[id];
            const auto pin = pins.find(form);
            const bool eitherHand = described.grip == Grip::Either && pin != pins.end() && hand != Hand::None;
            if (request == WearRequest::Pin || request == WearRequest::Equip)
            {
                ReleaseConflictingPins(actor, pins, thing, hands);
                // An either-hand thing already pinned in the other hand is
                // pinned in both now, a spell once in each; unless this is
                // her one weapon changing hands.
                if (request == WearRequest::Pin)
                    pins[form] = eitherHand && !moving ? pin->second | hands : hands;
            }
            else if (pin != pins.end())
            {
                // One cell of a two-handed pin lets that hand go and keeps
                // the other; anything else is the whole pin.
                if (eitherHand && pin->second == Hand::Both)
                {
                    pin->second = Without(pin->second, hand);
                    hands = hand;
                }
                else
                {
                    hands = pin->second;
                    pins.erase(pin);
                }
            }
        }

        const char *name = thing->GetName() ? thing->GetName() : "?";
        switch (request)
        {
        case WearRequest::Equip:
            logger::info("{} told to ready {} (not pinned)", Describe(actor), name);
            EquipPinned(actor, thing, hands, true);
            break;
        case WearRequest::Pin:
            logger::info("{} told to ready {} (pinned)", Describe(actor), name);
            // Off for now, to see what her own style does with a left-hand
            // weapon; the copy stays available for the combat-style work.
            if constexpr (kDualWieldOnLeftPin)
            {
                if (thing->Is(RE::FormType::Weapon) && hands == Hand::Left)
                    AllowDualWield(actor);
            }
            if (moving)
                UnequipForm(actor, thing, hands == Hand::Left ? Hand::Right : Hand::Left, true);
            EquipPinned(actor, thing, hands, true);
            // Which hand a weapon or spell lands in is the AI's call as much
            // as ours: say what was asked and where it went, so the rule can
            // be read off the log.
            if (thing->Is(RE::FormType::Weapon))
            {
                logger::info("{} weapon {} asked {} -- now left {} right {}", Describe(actor), name,
                             static_cast<int>(hands), actor->GetEquippedObject(true) == thing,
                             actor->GetEquippedObject(false) == thing);
            }
            if (auto *spell = thing->As<RE::SpellItem>())
            {
                const auto *slot = spell->GetEquipSlot();
                const auto &data = actor->GetActorRuntimeData();
                logger::info("{} spell {} asked {} -- record slot {:06X} -- now left {} right {} -- {}",
                             Describe(actor), name, static_cast<int>(hands), slot ? slot->GetFormID() : 0,
                             data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell,
                             data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell, CasterState(actor));
                // Look again once time runs, to see what the engine finishes.
                g_republish.insert(id);
            }
            break;
        case WearRequest::Unpin:
            // An item's lock lives on the worn item, and the engine offers no
            // way to lift it in place: off, then on again without the flag.
            // A spell has no lock; forgetting the pin is the whole of it.
            logger::info("{} told to keep {} but not held to it", Describe(actor), name);
            // Spell first: a SpellItem is a bound object too, and the item
            // branch took a spell off and "put it back" with an item equip,
            // which left it off (01:47, Chain Lightning).
            if (thing->Is(RE::FormType::Spell))
                break;
            if (auto *object = thing->As<RE::TESBoundObject>())
            {
                UnequipForm(actor, object, hands, true);
                EquipPlain(actor, object, hands);
            }
            break;
        case WearRequest::TakeOff:
            logger::info("{} told to put away {}", Describe(actor), name);
            UnequipForm(actor, thing, hands, true);
            if (thing->Is(RE::FormType::Spell))
                g_republish.insert(id);
            break;
        }

        // Redraw her now. The Creation Kit wiki, on EquipItem: armour
        // equipped while a menu holds the actor "will not be visible ...
        // until the dialogue is ended" unless the model is refreshed
        // straight after -- which is this call, the one SKSE's
        // QueueNiNodeUpdate wraps.
        actor->Update3DModel();

        // Pins changed mid-fight: the AI's list was pruned to the OLD pins,
        // and a prune is one way -- what was erased does not come back on
        // its own. The list's dirty flag is the engine's own "rebuild me",
        // so the AI rebuilds it whole on its next update and the next tick
        // prunes it to the new pins. That is how a change overwrites.
        if (auto *controller = actor->GetActorRuntimeData().combatController)
        {
            if (controller->inventory)
            {
                controller->inventory->dirty = true;
                g_rebuildMarkedAt[id] = NowSeconds();
                logger::info("{} combat list marked for rebuild: pins changed mid-fight", Describe(actor));
            }
        }

        // NOT applied here: the item's enchantment. The equip path applies it
        // on the actor's next update, which the frozen clock withholds, so
        // the Skills tab shows the change only once the panel has closed and
        // time has run. Applying it here as well (UpdateArmorAbility) put
        // robes of Destruction at -34% instead of -17%: the engine's own
        // application still came, on top. Deferred it stays.

        PublishFollower(actor);
    });
}

} // namespace ft::game
