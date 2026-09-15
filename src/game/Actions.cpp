#include "game/Actions.h"

#include "game/Blows.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Sensors.h"
#include "game/Sheet.h"
#include "game/Util.h"

#include <algorithm>

namespace ft::game
{
namespace
{

// Making an NPC actually consume a potion -- or a food, or an ingredient:
// all three are eaten by the same equip call, and the game consumes the
// item through its normal path. (Food and ingredients: built 2026-09-04,
// their effects on an NPC unverified in play; docs/ACTIONS.md 7.)
//
// These parameter values are NOT guesses. They are copied from NPCsUsePotions
// (github.com/muenchk/NPCsUsePotions), which has solved this problem in
// production and is follower-aware:
//
//     EquipObject(actor, potion, nullptr, 1, nullptr, true, false, false)
//
// Note playSounds = false. docs/PLAN.md originally guessed true; the working
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

// Put a poison on the weapon in hand: one dose on the worn copy's extra
// list, one bottle out of the bag. The engine's own PoisonObject writes to
// an entry's FIRST extra list, which for a follower with two of the sword
// need not be the one in hand; the worn list is found here instead. The
// dose is one hit, as the inventory menu gives a player without the
// Concentrated Poison perk.
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

    worn->Add(new RE::ExtraPoison(poison, 1));
    actor->RemoveItem(poison, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    // What the inventory menu's own routine plays after the dose goes on
    // (read from the executable): the vial, as a UI sound. There is no
    // character animation for it in the engine either.
    RE::PlaySound("ITMPoisonUse");
    log::actions.event(log::Level::Info, "poison.applied", actor,
                       {{"poisonFormId", log::Id(poison->GetFormID())},
                        {"poisonName", log::NameOf(poison)},
                        {"weaponFormId", log::Id(weapon->GetFormID())},
                        {"weaponName", log::NameOf(weapon)}},
                       "{} put {} on {}", Describe(actor), log::NameOf(poison), log::NameOf(weapon));
    return ActionResult::Performed;
}

// Spend a soul gem into the weapon in hand whose charge is empty, the right
// hand before the left. What the engine's own recharge routine does, read
// from the executable: the gem's soul value through the Mod Soul Gem
// Recharge perk entry point, added to what is left and capped at the full
// charge, written to ExtraCharge on the worn copy; the weapon's ability
// refreshed; the gem removed, or emptied if it is reusable (Azura's Star:
// the routine sets the soul on its entry back to none); the recharge sound
// played. The gem is the rule's: a policy's is chosen by the evaluator
// (ChosenForm) and arrives as the step's form like a named one.
ActionResult ChargeWeapon(RE::Actor *actor, std::uint32_t gemForm)
{
    RE::TESObjectWEAP *weapon = nullptr;
    bool left = false;
    WeaponCharge state;
    for (const bool hand : {false, true})
    {
        auto *candidate = WeaponIn(actor, hand);
        const WeaponCharge c = ChargeOf(actor, candidate, hand ? Hand::Left : Hand::Right);
        if (candidate && c.enchanted && c.charge < c.costPerHit)
        {
            weapon = candidate;
            left = hand;
            state = c;
            break;
        }
    }
    if (!weapon)
        return ActionResult::MissingItem;

    const auto gems = ScanSoulGems(actor);
    const auto it = std::find_if(gems.begin(), gems.end(), [&](const auto &g) { return g.form == gemForm; });
    auto *gem = RE::TESForm::LookupByID<RE::TESSoulGem>(gemForm);
    if (it == gems.end() || !gem)
        return ActionResult::MissingItem;

    // That hand's copy: with the same sword in each hand, the one in need.
    RE::ExtraDataList *worn = WornList(actor, weapon, left ? Hand::Left : Hand::Right);
    if (!worn)
        return ActionResult::MissingItem;

    float value = it->charge;
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSoulGemRecharge, actor,
                                        static_cast<RE::TESForm *>(weapon), &value);
    const float charge = (std::min)(state.charge + (std::max)(value, 0.0f), state.maxCharge);
    if (auto *xCharge = worn->GetByType<RE::ExtraCharge>())
        xCharge->charge = charge;
    else
    {
        auto *fresh = new RE::ExtraCharge();
        fresh->charge = charge;
        worn->Add(fresh);
    }
    // The refresh is what puts the record's charge into the hand's
    // ItemCharge actor value, the live copy the engine draws from and the
    // meter reads (read from the executable: it sets that value from the
    // record, or the full charge with no record). The engine's own
    // recharge writes nothing else, so neither does this.
    actor->UpdateWeaponAbility(weapon, worn, left);
    if (gem->HasKeywordString("ReusableSoulGem"))
    {
        // The soul a reusable gem holds is ExtraSoul on its entry; the
        // record's own soul is none. Cleared, the Star is empty and stays.
        const Carried carried = CarriedOf(actor, gem);
        auto *gemEntry = carried.entry.get();
        bool emptied = false;
        if (gemEntry && gemEntry->extraLists)
        {
            for (auto *list : *gemEntry->extraLists)
            {
                if (list && list->GetSoulLevel() != RE::SOUL_LEVEL::kNone &&
                    list->RemoveByType(RE::ExtraDataType::kSoul))
                {
                    emptied = true;
                    break;
                }
            }
        }
        if (!emptied)
            log::actions.warn("{} {} is reusable but its soul was not found on an extra list -- not emptied",
                              Describe(actor), log::NameOf(gem));
    }
    else
        actor->RemoveItem(gem, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    RE::PlaySound("UIEnchantRecharge");
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
        return "action not implemented in this phase";
    case ActionResult::MissingItem:
        return "item missing at dispatch";
    case ActionResult::NoEquipManager:
        return "ActorEquipManager unavailable";
    case ActionResult::Busy:
        return "every package slot is mid-cast";
    case ActionResult::NoTarget:
        return "the spell needs a target and there is no one to fight";
    case ActionResult::WeaponSheathed:
        return "the weapon is not drawn";
    case ActionResult::MidSwing:
        return "already mid-swing";
    case ActionResult::GraphRefused:
        return "the animation graph refused the blow -- blocking, staggered or recovering";
    }
    return "?";
}

// Point the combat AI at an enemy: the target the controller holds and the
// actor's own mirror of it. Everything else -- weapon, spell, spacing --
// stays the AI's, re-scored for the new target. Whether the standard target
// selector lets the choice stand is the open question (docs/ACTIONS.md 6):
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

ActionResult Execute(const ft::Action &action, ft::ActorId target, RE::Actor *actor, int ruleIndex,
                     std::string_view ruleName)
{
    if (!actor)
        return ActionResult::MissingItem;

    switch (action.kind)
    {
    case ft::ActionKind::ChargeStrongestSoulGem:
    case ft::ActionKind::ChargeWeakestSoulGem:
    case ft::ActionKind::ChargeSoulGem:
        return ChargeWeapon(actor, action.form);
    case ft::ActionKind::ApplyStrongest:
    case ft::ActionKind::ApplyWeakest:
    case ft::ActionKind::ApplyPoison: {
        auto *poison = RE::TESForm::LookupByID<RE::AlchemyItem>(action.form);
        return ApplyPoison(actor, poison && poison->IsPoison() ? poison : nullptr);
    }
    case ft::ActionKind::DrinkStrongest:
    case ft::ActionKind::DrinkWeakest:
    case ft::ActionKind::DrinkPotion:
    case ft::ActionKind::EatStrongestFood:
    case ft::ActionKind::EatWeakestFood:
    case ft::ActionKind::EatFood:
        // One named potion or food. The evaluator only fires this when the
        // snapshot says they carry it, so a null here is a form that
        // stopped being one between snapshot and dispatch.
        return Consume(actor, RE::TESForm::LookupByID<RE::AlchemyItem>(action.form));
    case ft::ActionKind::EatStrongestIngredient:
    case ft::ActionKind::EatWeakestIngredient:
    case ft::ActionKind::EatIngredient:
        return Consume(actor, RE::TESForm::LookupByID<RE::IngredientItem>(action.form));

    case ft::ActionKind::UsePower:
    case ft::ActionKind::Shout: {
        // A power is performed through the follower's Shout package: a one-word wrapper shout
        // whose word casts the power, fired by the Shout procedure from the
        // voice, which is where a power lives. The UseMagic route was
        // measured first (2026-09-04, Voice of the Emperor): the package was
        // selected on every request and the AI never cast, because that
        // procedure casts from a hand. The instant caster would apply the
        // effect with no animation; a performance was wanted, so the Shout
        // package it is (docs/ACTIONS.md 7). A shout goes through the same package
        // with the shout itself in the package's input, no wrapper. Aimed as
        // a cast is: a Self power or shout on the follower, anything else at
        // whom the rule aimed it.
        const bool shout = action.kind == ft::ActionKind::Shout;
        auto *form = RE::TESForm::LookupByID(action.form);
        const RE::SpellItem *delivery = nullptr;
        if (auto *asShout = form ? form->As<RE::TESShout>() : nullptr)
            delivery = asShout->variations[0].spell;
        else if (auto *asSpell = form ? form->As<RE::SpellItem>() : nullptr)
            delivery = asSpell;
        if (!form || (shout && !form->As<RE::TESShout>()) || (!shout && !form->As<RE::SpellItem>()))
            return ActionResult::MissingItem;
        std::uint32_t targetId = actor->GetFormID();
        if (delivery && delivery->GetDelivery() != RE::MagicSystem::Delivery::kSelf && target != 0 &&
            target != actor->GetFormID() && RE::TESForm::LookupByID<RE::Actor>(target))
            targetId = target;
        const char *what = shout ? "shout" : "power";
        log::actions.debug("{}: {} through a shout slot", what, log::NameOf(form));
        const auto request = RequestShout(actor, action.form, targetId, ruleIndex, ruleName);
        log::actions.debug("{}: {}", what, ToString(request));
        return ResultOf(request);
    }

    case ft::ActionKind::CastSpell:
    case ft::ActionKind::UseScroll: {
        // The package route. Who the spell goes at. A Self-delivery spell (Fast Healing,
        // Oakflesh) cannot take a target. Anything else goes at whom the
        // RULE aimed it: the ally it matched, the player, their attacker --
        // that is how Heal Other reaches the hurt one. Aimed at the follower
        // themself, or at no one, a targeted spell goes at the enemy they
        // are engaging, as it always did.
        std::uint32_t targetId = actor->GetFormID();
        // A spell, or a scroll: both MagicItems, cast the same way.
        auto *spell = RE::TESForm::LookupByID<RE::MagicItem>(action.form);
        if (spell)
            log::actions.debug("cast: {} is {} / {}", log::NameOf(spell),
                               spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration
                                   ? "concentration"
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
                    log::actions.debug("cast: {} needs a target and the follower is fighting no one",
                                       log::NameOf(spell));
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

    case ft::ActionKind::EquipWeapon:
    case ft::ActionKind::EquipSpell:
    case ft::ActionKind::EquipArrows:
    case ft::ActionKind::EquipStrongestArrows:
    case ft::ActionKind::EquipWeakestArrows:
    case ft::ActionKind::EquipArmor:
        // A pin, in the same book as the panel's. Naming nothing lets go of
        // every pin of the kind -- in the hand named, for a weapon or a
        // spell -- and takes those things off, so the AI decides again. An
        // arrow policy arrives with the form it chose. The evaluator only
        // fires this when the snapshot says they have the thing, so a miss
        // here is a form that left them between snapshot and dispatch.
        if (action.form == 0)
        {
            ReleaseKind(actor, ft::KindOf(action.kind), ft::TakesHand(action.kind) ? action.hand : Hand::None);
            return ActionResult::Performed;
        }
        return PinNow(actor, action.form, action.hand, action.variant) ? ActionResult::Performed
                                                                       : ActionResult::MissingItem;

    case ft::ActionKind::Attack:
        return PointAt(actor, target);

    case ft::ActionKind::PowerAttack:
    case ft::ActionKind::Bash:
    case ft::ActionKind::PowerBash: {
        // At an enemy who is not the follower's target, point them there
        // first, as Attack does.
        const auto current = actor->GetActorRuntimeData().currentCombatTarget.get();
        const std::uint32_t currentId = current ? current->GetFormID() : 0;
        const bool elsewhere = target != 0 && target != actor->GetFormID();
        if (elsewhere && target != currentId)
        {
            if (PointAt(actor, target) != ActionResult::Performed)
                return ActionResult::NoTarget;
        }
        const BlowPlan blow = PlanBlow(actor, action.kind);
        if (!blow.Possible())
            return ActionResult::MissingItem;

        // A bash is made from a block, as the engine makes one: raised, the
        // bash sent once it is up, lowered (game/Blows.h).
        if (action.kind != ft::ActionKind::PowerAttack)
            return RequestBash(actor, action.kind == ft::ActionKind::PowerBash, ruleIndex, ruleName) ==
                           BashRequest::Started
                       ? ActionResult::Requested
                       : ActionResult::Busy;

        // A power attack with the right hand's weapon goes through the
        // follower's UseWeapon record, which waits for their own swing to end
        // instead of being turned away mid-swing: sent as an event, 3 of 11
        // blows landed in play (2026-09-09, docs/ACTIONS.md 6). The procedure
        // attacks with the right hand alone, so the left's blade and the fists
        // stay events.
        if (blow.swing == ft::Swing::Right || blow.swing == ft::Swing::Both)
        {
            const auto request = RequestPowerAttack(actor, elsewhere ? target : currentId, blow, ruleIndex, ruleName);
            if (request != CastRequest::NoPackages)
            {
                log::actions.debug("power attack: {}", ToString(request));
                return ResultOf(request);
            }
        }

        // The left's blade, the fists, or no record for this follower: the
        // animation event the race's attack data names for what is in the hands. The follower's own combat AI
        // runs the same graph, so it is refused while a swing, a block or a
        // stagger is in progress, and the refusal spends the cooldown the
        // core stamped when it decided.
        auto *state = actor->AsActorState();
        if (!state || !state->IsWeaponDrawn())
            return ActionResult::WeaponSheathed;
        if (state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone)
            return ActionResult::MidSwing;
        const bool sent = actor->NotifyAnimationGraph(blow.event);
        log::actions.debug("blow: {} {} ({:.0f} stamina){}", Describe(actor), blow.event, blow.stamina,
                           sent ? "" : " -- the graph refused it");
        return sent ? ActionResult::Performed : ActionResult::GraphRefused;
    }

    default:
        // Every other action is Phase 4. The rule engine's Capabilities table is
        // what should stop these being authored at all; reaching here means the
        // capability flags and this switch have drifted apart.
        return ActionResult::NoSuchAction;
    }
}

} // namespace ft::game
