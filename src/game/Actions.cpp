#include "game/Actions.h"

#include "game/Packages.h"
#include "game/Pins.h"

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
    }
    return "?";
}

const char *ToString(ActionResult r) noexcept
{
    switch (r)
    {
    case ActionResult::Performed:
        return "performed";
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
    }
    return "?";
}

ActionResult Execute(const ft::Action &action, ft::ActorId target, RE::Actor *actor, const PotionChoice &choice)
{
    if (!actor)
        return ActionResult::MissingItem;

    switch (action.kind)
    {
    case ft::ActionKind::DrinkHealthPotion:
        return Consume(actor, choice.health);
    case ft::ActionKind::DrinkMagickaPotion:
        return Consume(actor, choice.magicka);
    case ft::ActionKind::DrinkStaminaPotion:
        return Consume(actor, choice.stamina);
    case ft::ActionKind::DrinkWeakestHealthPotion:
        return Consume(actor, choice.weakestHealth);
    case ft::ActionKind::DrinkWeakestMagickaPotion:
        return Consume(actor, choice.weakestMagicka);
    case ft::ActionKind::DrinkWeakestStaminaPotion:
        return Consume(actor, choice.weakestStamina);
    case ft::ActionKind::DrinkPotion:
    case ft::ActionKind::EatFood:
        // One named potion or food. The evaluator only fires this when the
        // snapshot says she carries it, so a null here is a form that
        // stopped being one between snapshot and dispatch.
        return Consume(actor, RE::TESForm::LookupByID<RE::AlchemyItem>(action.form));
    case ft::ActionKind::EatIngredient:
        return Consume(actor, RE::TESForm::LookupByID<RE::IngredientItem>(action.form));

    case ft::ActionKind::UsePower:
    case ft::ActionKind::Shout: {
        // A power is performed through a Shout slot: a one-word wrapper shout
        // whose word casts the power, fired by the Shout procedure from the
        // voice, which is where a power lives. The UseMagic route was
        // measured first (2026-09-04, Voice of the Emperor): the package was
        // selected on every request and the AI never cast, because that
        // procedure casts from a hand. The instant caster would apply the
        // effect with no animation; a performance was wanted, so the shout
        // pool it is (docs/ACTIONS.md 7). A shout goes through the same slot
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
        logger::info("  {}: {} through a shout slot", what, form->GetName() ? form->GetName() : "?");
        const auto request = RequestShout(actor, action.form, targetId);
        logger::info("  {}: {}", what, ToString(request));
        switch (request)
        {
        case CastRequest::Armed:
            return ActionResult::Performed;
        case CastRequest::SpellNotInSlot:
        case CastRequest::TargetGone:
            return ActionResult::MissingItem;
        case CastRequest::PoolBusy:
        case CastRequest::AlreadyCasting:
            return ActionResult::Busy;
        case CastRequest::NoPackages:
            return ActionResult::NoSuchAction;
        }
        return ActionResult::NoSuchAction;
    }

    case ft::ActionKind::CastSpell: {
        // The package route, on its own. The combat-AI hook is off by default
        // and not called here: running two mechanisms would mean a cast could
        // not be attributed to either, which is what made the earlier
        // animation-event experiment worthless.
        // Who the spell goes at. A Self-delivery spell (Fast Healing,
        // Oakflesh) cannot take a target. Anything else goes at whom the
        // RULE aimed it: the ally it matched, the player, their attacker --
        // that is how Heal Other reaches the hurt one. Aimed at the follower
        // themself, or at no one, a targeted spell goes at the enemy they
        // are engaging, as it always did.
        std::uint32_t targetId = actor->GetFormID();
        auto *spell = FindSpell(action.form);
        if (spell)
            logger::info("  cast: {} is {} / {}", spell->GetName() ? spell->GetName() : "?",
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
                    logger::info("  cast: {} needs a target and the follower is fighting no one",
                                 spell->GetName() ? spell->GetName() : "?");
                    return ActionResult::NoTarget;
                }
                targetId = enemy->GetFormID();
            }
        }

        // actionArg is the sustain time for a concentration spell, when a rule
        // sets one; zero takes the default.
        const auto request = RequestCast(actor, action.form, targetId, action.arg);
        logger::info("  cast: {}", ToString(request));
        switch (request)
        {
        case CastRequest::Armed:
            return ActionResult::Performed;
        case CastRequest::SpellNotInSlot:
            return ActionResult::MissingItem;
        case CastRequest::PoolBusy:
        case CastRequest::AlreadyCasting:
            return ActionResult::Busy;
        case CastRequest::TargetGone:
            return ActionResult::MissingItem;
        case CastRequest::NoPackages:
            return ActionResult::NoSuchAction;
        }
        return ActionResult::NoSuchAction;
    }

    case ft::ActionKind::EquipWeapon:
    case ft::ActionKind::EquipSpell:
    case ft::ActionKind::EquipArrows:
    case ft::ActionKind::EquipArmor:
        // A pin, in the same book as the panel's. Naming nothing lets go of
        // every pin of the kind and takes those things off, so the AI
        // decides again. The evaluator only fires this when the snapshot
        // says she has the thing, so a miss here is a form that left her
        // between snapshot and dispatch.
        if (action.form == 0)
        {
            ReleaseKind(actor, ft::KindOf(action.kind));
            return ActionResult::Performed;
        }
        return PinNow(actor, action.form, action.hand) ? ActionResult::Performed : ActionResult::MissingItem;

    case ft::ActionKind::Target: {
        // Point the combat AI at whom the rule aimed: the target the
        // controller holds and the actor's own mirror of it. Everything
        // else -- weapon, spell, spacing -- stays the AI's, re-scored for
        // the new target. Whether the standard target selector lets the
        // choice stand is the open question (docs/ACTIONS.md 6): the rule
        // reports "already fighting them" on the next tick if it did, and
        // fires again after its cooldown if it did not, so the log answers
        // it without any extra instrumentation.
        auto *enemy = RE::TESForm::LookupByID<RE::Actor>(target);
        if (!enemy || enemy->IsDead())
            return ActionResult::MissingItem;
        auto &runtime = actor->GetActorRuntimeData();
        auto *controller = runtime.combatController;
        if (!controller)
        {
            logger::info("  target: {} has no combat controller -- not fighting",
                         actor->GetName() ? actor->GetName() : "?");
            return ActionResult::NoTarget;
        }
        const auto before = runtime.currentCombatTarget.get();
        logger::info("  target: {} was fighting {} ({:08X}), now {} ({:08X})",
                     actor->GetName() ? actor->GetName() : "?",
                     before && before->GetName() ? before->GetName() : "no one", before ? before->GetFormID() : 0,
                     enemy->GetName() ? enemy->GetName() : "?", enemy->GetFormID());
        const RE::ActorHandle handle = enemy->GetHandle();
        controller->previousTargetHandle = controller->targetHandle;
        controller->targetHandle = handle;
        controller->cachedTarget = RE::NiPointer<RE::Actor>(enemy);
        runtime.currentCombatTarget = handle;
        return ActionResult::Performed;
    }

    default:
        // Every other action is Phase 4. The rule engine's Capabilities table is
        // what should stop these being authored at all; reaching here means the
        // capability flags and this switch have drifted apart.
        return ActionResult::NoSuchAction;
    }
}

} // namespace ft::game
