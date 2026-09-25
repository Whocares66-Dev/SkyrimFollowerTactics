#include "game/Actions.h"

#include "core/Routes.h"
#include "game/Bag.h"
#include "game/Blows.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/PlayerCast.h"
#include "game/Sensors.h"
#include "game/Sheet.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include <algorithm>
#include <cmath>

namespace ft::game
{
namespace
{

// Making an NPC actually consume a potion -- or a food, or an ingredient:
// all three are eaten by the same equip call, and the game consumes the
// item through its normal path. (Food and ingredients: built 2026-09-04,
// their effects on an NPC unverified in play; dev/ACTIONS.md 7.)
//
// These parameter values are NOT guesses. They are copied from NPCsUsePotions
// (github.com/muenchk/NPCsUsePotions), which has solved this problem in
// production and is follower-aware:
//
//     EquipObject(actor, potion, nullptr, 1, nullptr, true, false, false)
//
// Note playSounds = false. dev/PLAN.md originally guessed true; the working
// implementation passes false, so we match it. If the drink turns out to be
// silent in a way that matters, that is the one flag to flip -- but reliability
// first, polish second.
//
// EquipObject is not library code: CommonLibSSE resolves an address via Address
// Library and calls Skyrim's own equip routine. That is precisely why this
// works where the Papyrus EquipItem-on-a-potion trick does not -- the game
// consumes the item through its normal path rather than us simulating it.
ActionResult Consume(RE::Actor *actor, RE::TESBoundObject *item)
{
    if (!item)
        return ActionResult::MissingItem;

    auto *equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager)
        return ActionResult::NoEquipManager;

    // What it is taken against, at debug: each effect's magnitude beside
    // what is in force under that name -- as it runs, and as its record has
    // it, which is what a rule compares (core's PotionStock::Outdone,
    // RunningEffects) -- and from what.
    if (auto *magic = item->As<RE::MagicItem>(); magic && log::Enabled(log::Level::Debug))
    {
        std::string text;
        for (const RE::Effect *effect : ResolvedEffects(*magic))
        {
            const char *name = effect->baseEffect->GetFullName();
            if (!name || !*name)
                continue;
            std::string running;
            ForEachActiveEffect(actor, [&](RE::ActiveEffect &ae) {
                const char *other = ae.effect->baseEffect->GetFullName();
                if (other && std::string_view(other) == name)
                    running += fmt::format("{}{:.2f} (record {:.2f}) from {}", running.empty() ? "" : ", ",
                                           ae.magnitude, ae.effect->effectItem.magnitude, log::NameOf(ae.spell));
            });
            text += fmt::format("{}{} {:.2f} (in force: {})", text.empty() ? "" : "; ", name,
                                effect->effectItem.magnitude, running.empty() ? std::string("none") : running);
        }
        log::actions.debug("{} takes {}: {}", Describe(actor), log::NameOf(item), text);
    }

    equipManager->EquipObject(actor, item,
                              /*extraData*/ nullptr,
                              /*count*/ 1,
                              /*slot*/ nullptr,
                              /*queueEquip*/ true,
                              /*forceEquip*/ false,
                              /*playSounds*/ false,
                              /*applyNow*/ false);
    return ActionResult::Performed;
}

// How many hits the dose is worth. The engine's own count: the inventory
// menu's routine starts at one and puts it through the Mod Poison Dose
// Count entry point, which is where Concentrated Poison keeps its "Set 2"
// (105F2F), so the perk decides this here as it does in the menu -- and a
// mod that retunes the perk, or gives another one entries of its own, is
// followed without a table of our own.
//
// The value is a float because a perk entry's own value is one
// (BGSEntryPointFunctionDataOneValue::data), as every other entry point
// called here is. Read from the 1.6.1170 executable (2026-09-17): the entry
// point's three parameters are named "Perk Owner", "Attacker Weapon" and
// "Item", which is the order they go in; which of the latter two the engine
// itself passes first was not read, and matters only to a perk with
// conditions on them, since Concentrated Poison has none.
//
// Rounded, and never below one: a count of no hits would put a poison on
// that does nothing, and that is also where a float the engine did not
// write lands -- a wrong reading comes out as the single dose this gave
// before the entry point was asked.
std::int32_t PoisonDoses(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::AlchemyItem *poison)
{
    float doses = 1.0f;
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModPoisonDoseCount, actor,
                                        static_cast<RE::TESForm *>(weapon), static_cast<RE::TESForm *>(poison), &doses);
    return (std::max)(std::int32_t{1}, static_cast<std::int32_t>(std::lround(doses)));
}

// Put a poison on the weapon in hand: the dose on the worn copy's extra
// list, one bottle out of the bag. The engine's own PoisonObject writes to
// an entry's FIRST extra list, which for a follower with two of the sword
// need not be the one in hand; the worn list is found here instead.
ActionResult ApplyPoison(RE::Actor *actor, RE::AlchemyItem *poison)
{
    if (!poison)
        return ActionResult::MissingItem;
    // The right hand's weapon if it is clean, else the left's: both hands
    // dressed by two firings of the rule, and none when both carry one
    // (the evaluator does not fire this then). The dose goes on that
    // hand's copy: with the same dagger in each hand, the clean one.
    const auto [weapon, hand] = WeaponToPoison(actor);
    if (!weapon)
        return ActionResult::MissingItem;
    RE::ExtraDataList *worn = WornList(actor, weapon, hand);
    if (!worn)
        return ActionResult::MissingItem;

    // The order is core's (core/Blows.h, PlanPoison, tested): the dose on
    // the copy first, the vial spent after it.
    const std::int32_t doses = PoisonDoses(actor, weapon, poison);
    for (const ft::ItemStep step : ft::PlanPoison(weapon != nullptr, worn != nullptr))
    {
        switch (step)
        {
        case ft::ItemStep::WriteDose:
            worn->Add(new RE::ExtraPoison(poison, doses));
            break;
        case ft::ItemStep::SpendPoison:
            actor->RemoveItem(poison, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            break;
        case ft::ItemStep::PlaySound:
            // What the inventory menu's own routine plays after the dose
            // goes on (read from the executable): the vial, as a UI sound.
            // There is no character animation for it in the engine either.
            RE::PlaySound("ITMPoisonUse");
            break;
        default:
            break;
        }
    }
    log::actions.event(log::Level::Info, "poison.applied", actor,
                       {{"poisonFormId", log::Id(poison->GetFormID())},
                        {"poisonName", log::NameOf(poison)},
                        {"weaponFormId", log::Id(weapon->GetFormID())},
                        {"weaponName", log::NameOf(weapon)},
                        {"doses", doses}},
                       "{} put {} on {} ({} hit{})", Describe(actor), log::NameOf(poison), log::NameOf(weapon), doses,
                       doses == 1 ? "" : "s");
    return ActionResult::Performed;
}

// Spend a soul gem into one copy of an enchanted weapon: what the engine's
// own recharge routine does, read from the executable -- the gem's soul
// value through the Mod Soul Gem Recharge perk entry point, added to what is
// left and capped at the full charge, written to ExtraCharge on the copy;
// the weapon's ability refreshed where the copy is held (`held`, its hand);
// the gem removed, or emptied if it is reusable (Azura's Star: the routine
// sets the soul on its entry back to none); the recharge sound played.
ActionResult RechargeCopy(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::ExtraDataList *list,
                          std::optional<Hand> held, const WeaponCharge &state, std::uint32_t gemForm)
{
    const auto gems = ScanSoulGems(actor);
    const auto it = std::find_if(gems.begin(), gems.end(), [&](const auto &g) { return g.form == gemForm; });
    auto *gem = RE::TESForm::LookupByID<RE::TESSoulGem>(gemForm);
    if (it == gems.end() || !gem)
        return ActionResult::MissingItem;

    float value = it->charge;
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSoulGemRecharge, actor,
                                        static_cast<RE::TESForm *>(weapon), &value);
    const float charge = ft::ChargeAfterRecharge(state.charge, state.maxCharge, value);
    const bool reusable = gem->HasKeywordString("ReusableSoulGem");
    // The order is core's (core/Blows.h, PlanRecharge, tested): the charge
    // written, the ability refreshed from it, the gem spent last.
    for (const ft::ItemStep step : ft::PlanRecharge(weapon != nullptr, list != nullptr, true, reusable))
    {
        switch (step)
        {
        case ft::ItemStep::WriteCharge:
            if (auto *xCharge = list->GetByType<RE::ExtraCharge>())
                xCharge->charge = charge;
            else
            {
                auto *fresh = new RE::ExtraCharge();
                fresh->charge = charge;
                list->Add(fresh);
            }
            break;
        case ft::ItemStep::RefreshAbility:
            // The refresh is what puts the record's charge into the hand's
            // ItemCharge actor value, the live copy the engine draws from
            // and the meter reads (read from the executable: it sets that
            // value from the record, or the full charge with no record).
            // The engine's own recharge writes nothing else, so neither
            // does this.
            if (held)
                actor->UpdateWeaponAbility(weapon, list, *held == Hand::Left);
            break;
        case ft::ItemStep::EmptyGem: {
            // The soul a reusable gem holds is ExtraSoul on its entry; the
            // record's own soul is none. Cleared, the Star is empty and stays.
            const Carried carried = CarriedOf(actor, gem);
            auto *gemEntry = carried.entry.get();
            bool emptied = false;
            if (gemEntry && gemEntry->extraLists)
            {
                for (auto *gemList : *gemEntry->extraLists)
                {
                    if (gemList && gemList->GetSoulLevel() != RE::SOUL_LEVEL::kNone &&
                        gemList->RemoveByType(RE::ExtraDataType::kSoul))
                    {
                        emptied = true;
                        break;
                    }
                }
            }
            if (!emptied)
                log::actions.warn("{} {} is reusable but its soul was not found on an extra list -- not emptied",
                                  Describe(actor), log::NameOf(gem));
            break;
        }
        case ft::ItemStep::SpendGem:
            actor->RemoveItem(gem, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            break;
        case ft::ItemStep::PlaySound:
            RE::PlaySound("UIEnchantRecharge");
            break;
        default:
            break;
        }
    }
    log::actions.event(log::Level::Info, "soul.spent", actor,
                       {{"gemFormId", log::Id(gem->GetFormID())},
                        {"gemName", log::NameOf(gem)},
                        {"soul", it->charge},
                        {"weaponFormId", log::Id(weapon->GetFormID())},
                        {"weaponName", log::NameOf(weapon)},
                        {"chargeBefore", state.charge},
                        {"chargeAfter", charge},
                        {"chargeMax", state.maxCharge}},
                       "{} spent {} ({:.0f}) into {}: charge {:.0f} -> {:.0f} of {:.0f}", Describe(actor),
                       log::NameOf(gem), it->charge, log::NameOf(weapon), state.charge, charge, state.maxCharge);
    return ActionResult::Performed;
}

// The weapon in hand whose charge is wanted, the right hand before the
// left, charged with the rule's gem. The gem is the rule's: a policy's is
// chosen by the evaluator (ChosenForm) and arrives as the step's form like
// a named one.
ActionResult ChargeWeapon(RE::Actor *actor, std::uint32_t gemForm)
{
    for (const bool left : {false, true})
    {
        auto *weapon = WeaponIn(actor, left);
        const Hand hand = left ? Hand::Left : Hand::Right;
        const WeaponCharge c = ChargeOf(actor, weapon, hand);
        if (!weapon || !c.enchanted || !ft::ChargeWanted(c.charge, c.maxCharge, c.costPerHit))
            continue;
        // That hand's copy: with the same sword in each hand, the one in need.
        RE::ExtraDataList *worn = WornList(actor, weapon, hand);
        return worn ? RechargeCopy(actor, weapon, worn, hand, c, gemForm) : ActionResult::MissingItem;
    }
    return ActionResult::MissingItem;
}

// What a cast request comes back as, in the action's words.
ActionResult ResultOf(CastRequest request)
{
    switch (request)
    {
    case CastRequest::Armed:
        return ActionResult::Requested;
    case CastRequest::SpellNotInSlot:
    case CastRequest::TargetGone:
        return ActionResult::MissingItem;
    case CastRequest::AlreadyCasting:
        return ActionResult::Busy;
    case CastRequest::NoPackages:
        return ActionResult::NoSuchAction;
    }
    return ActionResult::NoSuchAction;
}

// The same for a cast on the player's own body (game/PlayerCast.h).
ActionResult ResultOf(PlayerCastRequest request)
{
    switch (request)
    {
    case PlayerCastRequest::Started:
        return ActionResult::Requested;
    case PlayerCastRequest::AlreadyCasting:
        return ActionResult::Busy;
    case PlayerCastRequest::SpellMissing:
        return ActionResult::MissingItem;
    }
    return ActionResult::NoSuchAction;
}

} // namespace

const char *CannotCastText(std::uint32_t reason) noexcept
{
    switch (static_cast<RE::MagicSystem::CannotCastReason>(reason))
    {
    case RE::MagicSystem::CannotCastReason::kOK:
        return "ok";
    case RE::MagicSystem::CannotCastReason::kMagicka:
        return "not enough magicka";
    case RE::MagicSystem::CannotCastReason::kPowerUsed:
        return "power already used today";
    case RE::MagicSystem::CannotCastReason::kRangedUnderWater:
        return "cannot cast that underwater";
    case RE::MagicSystem::CannotCastReason::kMultipleCast:
        return "already casting";
    case RE::MagicSystem::CannotCastReason::kItemCharge:
        return "not enough charge";
    case RE::MagicSystem::CannotCastReason::kCastWhileShouting:
        return "shouting";
    case RE::MagicSystem::CannotCastReason::kShoutWhileCasting:
        return "casting";
    case RE::MagicSystem::CannotCastReason::kShoutWhileRecovering:
        return "recovering from a shout";
    default:
        return "?";
    }
}

const char *ToString(ActionResult r) noexcept
{
    switch (r)
    {
    case ActionResult::Performed:
        return "performed";
    case ActionResult::Requested:
        return "requested";
    case ActionResult::NoSuchAction:
        return "no way to perform it here";
    case ActionResult::MissingItem:
        return "item missing at dispatch";
    case ActionResult::NoEquipManager:
        return "ActorEquipManager unavailable";
    case ActionResult::Busy:
        return "a cast or a blow of theirs is still in flight";
    case ActionResult::NoTarget:
        return "the spell needs a target and there is no one to fight";
    }
    return "?";
}

namespace
{

// Point the combat AI at an enemy: the target the controller holds and the
// actor's own mirror of it. Everything else -- weapon, spell, spacing --
// stays the AI's, re-scored for the new target. Whether the standard target
// selector lets the choice stand is the open question (dev/ACTIONS.md 6):
// the rule reports "already fighting them" on the next tick if it did, and
// fires again after its cooldown if it did not, so the log answers it
// without any extra instrumentation.
ActionResult PointAt(RE::Actor *actor, std::uint32_t target)
{
    auto *enemy = RE::TESForm::LookupByID<RE::Actor>(target);
    if (!enemy || enemy->IsDead())
        return ActionResult::MissingItem;
    auto &runtime = actor->GetActorRuntimeData();
    auto *controller = runtime.combatController;
    if (!controller)
    {
        log::actions.debug("target: {} has no combat controller -- not fighting", Describe(actor));
        return ActionResult::NoTarget;
    }
    const auto before = runtime.currentCombatTarget.get();
    log::actions.debug("target: {} was fighting {} ({:08X}), now {} ({:08X})", Describe(actor),
                       NameOr(before.get(), "no one"), before ? before->GetFormID() : 0, NameOr(enemy, "?"),
                       enemy->GetFormID());
    const RE::ActorHandle handle = enemy->GetHandle();
    controller->previousTargetHandle = controller->targetHandle;
    controller->targetHandle = handle;
    controller->cachedTarget = RE::NiPointer<RE::Actor>(enemy);
    runtime.currentCombatTarget = handle;
    return ActionResult::Performed;
}

// A potion or a food is an AlchemyItem, an ingredient an IngredientItem.
// The evaluator only fires this when the snapshot says they carry it, so a
// null is a form that stopped being one between snapshot and dispatch.
RE::TESBoundObject *Consumable(std::uint32_t form)
{
    auto *item = RE::TESForm::LookupByID(form);
    return item && (item->Is(RE::FormType::AlchemyItem) || item->Is(RE::FormType::Ingredient))
               ? item->As<RE::TESBoundObject>()
               : nullptr;
}

// The shout or the power a voice action names; null for a form of the
// other kind.
RE::TESForm *VoiceForm(const ft::Action &action)
{
    auto *form = RE::TESForm::LookupByID(action.form);
    const bool shout = action.kind == ft::ActionKind::Shout;
    return form && (shout ? form->As<RE::TESShout>() != nullptr : form->As<RE::SpellItem>() != nullptr) ? form
                                                                                                        : nullptr;
}

// A follower's power or shout, through their Shout record: a one-word
// wrapper shout whose word casts the power, fired by the Shout procedure
// from the voice, which is where a power lives. The UseMagic route was
// measured first (2026-09-04, Voice of the Emperor): the package was
// selected on every request and the AI never cast, because that procedure
// casts from a hand. The instant caster would apply the effect with no
// animation; a performance was wanted, so the Shout package it is
// (dev/ACTIONS.md 7). A shout goes through the same package with the shout
// itself in the package's input, no wrapper. Aimed as a cast is: a Self
// power or shout on the follower, anything else at whom the rule aimed it.
ActionResult VoiceByRecord(RE::Actor *actor, const ft::Action &action, RE::TESForm *form, ft::ActorId target,
                           int ruleIndex, std::string_view ruleName)
{
    const RE::SpellItem *delivery = nullptr;
    if (auto *asShout = form->As<RE::TESShout>())
        delivery = asShout->variations[0].spell;
    else
        delivery = form->As<RE::SpellItem>();
    std::uint32_t targetId = actor->GetFormID();
    if (delivery && delivery->GetDelivery() != RE::MagicSystem::Delivery::kSelf && target != 0 &&
        target != actor->GetFormID() && RE::TESForm::LookupByID<RE::Actor>(target))
        targetId = target;
    const char *what = action.kind == ft::ActionKind::Shout ? "shout" : "power";
    log::actions.debug("{}: {} through a shout slot", what, log::NameOf(form));
    const auto request = RequestShout(actor, action.form, targetId, ruleIndex, ruleName);
    log::actions.debug("{}: {}", what, ToString(request));
    return ResultOf(request);
}

// A follower's spell or scroll, through their UseMagic record. Who it goes
// at: a Self-delivery spell (Fast Healing, Oakflesh) cannot take a target;
// anything else goes at whom the RULE aimed it -- the ally it matched, the
// player, their attacker, which is how Heal Other reaches the hurt one.
// Aimed at the follower themself, or at no one, a targeted spell goes at
// the enemy they are engaging.
ActionResult CastByRecord(RE::Actor *actor, const ft::Action &action, ft::ActorId target, int ruleIndex,
                          std::string_view ruleName)
{
    std::uint32_t targetId = actor->GetFormID();
    // A spell, or a scroll: both MagicItems, cast the same way.
    auto *spell = RE::TESForm::LookupByID<RE::MagicItem>(action.form);
    if (spell)
        log::actions.debug("cast: {} is {} / {}", log::NameOf(spell),
                           spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration ? "concentration"
                                                                                                   : "fire-and-forget",
                           spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf ? "self" : "targeted");
    if (spell && spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
    {
        // A Location spell -- a conjuration -- aimed at the follower goes
        // at their own feet, which is where a summon is wanted; every
        // other aimed spell aimed at no one goes at the enemy.
        const bool atOwnFeet =
            target == actor->GetFormID() && spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation;
        if (target != 0 && target != actor->GetFormID() && RE::TESForm::LookupByID<RE::Actor>(target))
        {
            targetId = target;
        }
        else if (!atOwnFeet)
        {
            auto enemy = actor->GetActorRuntimeData().currentCombatTarget.get();
            if (!enemy)
            {
                log::actions.debug("cast: {} needs a target and the follower is fighting no one", log::NameOf(spell));
                return ActionResult::NoTarget;
            }
            targetId = enemy->GetFormID();
        }
    }
    // actionArg is the sustain time for a concentration spell, when a rule
    // sets one; zero takes the default.
    const auto request = RequestCast(actor, action.form, targetId, action.arg, action.dual, ruleIndex, ruleName);
    log::actions.debug("cast: {}", ToString(request));
    return ResultOf(request);
}

// A pin, in the same book as the panel's, or the player's plain equip.
// Naming nothing lets go of every pin of the kind -- in the hand named,
// for a weapon or a spell -- and takes those things off, so the AI decides
// again. An arrow policy arrives with the form it chose. The evaluator
// only fires this when the snapshot says they have the thing, so a miss
// here is a form that left them between snapshot and dispatch.
ActionResult Equip(RE::Actor *actor, const ft::Action &action, bool pin)
{
    if (action.form == 0)
    {
        ReleaseKind(actor, ft::KindOf(action.kind), ft::TakesHand(action.kind) ? action.hand : Hand::None);
        return ActionResult::Performed;
    }
    const bool worn = pin ? PinNow(actor, action.form, action.hand, action.variant)
                          : WearNow(actor, action.form, WearRequest::Equip, action.hand, action.variant);
    return worn ? ActionResult::Performed : ActionResult::MissingItem;
}

// A bash, a power bash or a power attack, the player's as a follower's
// (game/Blows.h). A follower is pointed at an enemy who is not their target
// first, as Attack does, and the blow goes at whom they fight; the player
// aims for themself, and the blow goes where they look.
ActionResult Blow(RE::Actor *actor, const ft::Action &action, ft::ActorId target, int ruleIndex,
                  std::string_view ruleName)
{
    std::uint32_t at = 0;
    if (!actor->IsPlayerRef())
    {
        const auto current = actor->GetActorRuntimeData().currentCombatTarget.get();
        at = current ? current->GetFormID() : 0;
        if (target != 0 && target != actor->GetFormID() && target != at)
        {
            if (PointAt(actor, target) != ActionResult::Performed)
                return ActionResult::NoTarget;
            at = target;
        }
    }
    const BlowPlan blow = PlanBlow(actor, action.kind);
    if (!blow.Possible())
        return ActionResult::MissingItem;
    const BlowRequest request =
        action.kind == ft::ActionKind::PowerAttack
            ? RequestStrike(actor, at, blow.event, ruleIndex, ruleName)
            : RequestBash(actor, at, action.kind == ft::ActionKind::PowerBash, ruleIndex, ruleName);
    return request == BlowRequest::Started ? ActionResult::Requested : ActionResult::Busy;
}

} // namespace

ActionResult Execute(const ft::Action &action, ft::ActorId target, RE::Actor *actor, int ruleIndex,
                     std::string_view ruleName)
{
    if (!actor)
        return ActionResult::MissingItem;
    // Which way is core's (core/Routes.h, tested): the player's casts go by
    // a press of their own controls (game/PlayerCast.h), their equips are
    // plain, and they have no Attack; the rest is one way for both.
    const ft::Route route =
        ft::RouteOf(action.kind, actor->IsPlayerRef() ? ft::Performer::Player : ft::Performer::Follower);
    switch (route)
    {
    case ft::Route::None:
        // The capabilities stop these being written at all; reaching here
        // means a rule of one came through.
        return ActionResult::NoSuchAction;
    case ft::Route::Consume:
        return Consume(actor, Consumable(action.form));
    case ft::Route::ApplyPoison: {
        auto *poison = RE::TESForm::LookupByID<RE::AlchemyItem>(action.form);
        return ApplyPoison(actor, poison && poison->IsPoison() ? poison : nullptr);
    }
    case ft::Route::Charge:
        return ChargeWeapon(actor, action.form);
    case ft::Route::CastPress: {
        const auto request = RequestPlayerCast(actor, action.form, action.arg, action.dual, ruleIndex, ruleName);
        log::actions.debug("cast on the player: {}", ToString(request));
        return ResultOf(request);
    }
    case ft::Route::CastRecord:
        return CastByRecord(actor, action, target, ruleIndex, ruleName);
    case ft::Route::VoicePress:
    case ft::Route::VoiceRecord: {
        auto *form = VoiceForm(action);
        if (!form)
            return ActionResult::MissingItem;
        if (route == ft::Route::VoiceRecord)
            return VoiceByRecord(actor, action, form, target, ruleIndex, ruleName);
        const auto request = RequestPlayerVoice(actor, action.form, ruleIndex, ruleName);
        log::actions.debug("{} on the player: {}", action.kind == ft::ActionKind::Shout ? "shout" : "power",
                           ToString(request));
        return ResultOf(request);
    }
    case ft::Route::Pin:
    case ft::Route::Wear:
        return Equip(actor, action, route == ft::Route::Pin);
    case ft::Route::Target:
        return PointAt(actor, target);
    case ft::Route::Bash:
    case ft::Route::Strike:
        return Blow(actor, action, target, ruleIndex, ruleName);
    }
    return ActionResult::NoSuchAction;
}

void RequestCharge(ft::ActorId id, std::uint32_t form, const void *row)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    // Queued to the game thread and run there once, republishing the page
    // as the panel's wear clicks do: the clock is frozen while it is open.
    task->AddTask([id, form, row]() {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(form);
        if (!actor || !weapon)
            return;
        // A row of its own is its list; the plain stack's copy short of
        // charge can only be the one in hand, whose live charge is the
        // hand's and whose list carries no ExtraCharge yet.
        RE::ExtraDataList *list = row ? ListOfAddress(actor, weapon, static_cast<const RE::ExtraDataList *>(row))
                                      : WornStackList(actor, weapon, Hand::None);
        if (!list)
        {
            log::actions.info("{} {}: the copy to charge is no longer carried", Describe(actor), log::NameOf(weapon));
            RefreshShownPage();
            return;
        }
        std::optional<Hand> held;
        if (ListWorn(list, Hand::Left))
            held = Hand::Left;
        else if (ListWorn(list, Hand::Right))
            held = Hand::Right;
        WeaponCharge state = ChargeOf(actor, weapon, held.value_or(Hand::None));
        if (!held)
        {
            const auto *xCharge = list->GetByType<RE::ExtraCharge>();
            state.charge = xCharge ? xCharge->charge : state.maxCharge;
        }
        if (!state.enchanted || state.charge >= state.maxCharge)
            return;
        const std::uint32_t gem = ft::ChooseSoulGem(ScanSoulGems(actor), state.maxCharge - state.charge, false);
        if (gem == 0)
            return;
        (void)RechargeCopy(actor, weapon, list, held, state, gem);
        RefreshShownPage();
    });
}

} // namespace ft::game
