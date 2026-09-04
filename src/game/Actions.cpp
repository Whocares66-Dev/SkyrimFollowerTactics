#include "game/Actions.h"

#include "game/Packages.h"
#include "game/Pins.h"

namespace ft::game
{
namespace
{

// Making an NPC actually consume a potion.
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
ActionResult DrinkPotion(RE::Actor *actor, RE::AlchemyItem *potion)
{
    if (!potion)
        return ActionResult::MissingItem;

    auto *equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager)
        return ActionResult::NoEquipManager;

    equipManager->EquipObject(actor, potion,
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

ActionResult Execute(const ft::Decision &decision, RE::Actor *actor, const PotionChoice &choice)
{
    if (!actor)
        return ActionResult::MissingItem;

    switch (decision.action)
    {
    case ft::ActionKind::DrinkHealthPotion:
        return DrinkPotion(actor, choice.health);
    case ft::ActionKind::DrinkMagickaPotion:
        return DrinkPotion(actor, choice.magicka);
    case ft::ActionKind::DrinkStaminaPotion:
        return DrinkPotion(actor, choice.stamina);
    case ft::ActionKind::DrinkPotion:
        // One named potion. The evaluator only fires this when the snapshot
        // says she carries it, so a null here is a form that stopped being a
        // potion between snapshot and dispatch.
        return DrinkPotion(actor, RE::TESForm::LookupByID<RE::AlchemyItem>(decision.actionForm));

    case ft::ActionKind::CastSpell: {
        // The package route, on its own. The combat-AI hook is off by default
        // and not called here: running two mechanisms would mean a cast could
        // not be attributed to either, which is what made the earlier
        // animation-event experiment worthless.
        // Who the spell goes at is decided by the SPELL, not by the rule. A
        // Self-delivery spell (Fast Healing, Oakflesh) cannot take a target;
        // anything else goes at the enemy she is engaging. No target picker in
        // the editor yet, and this is what one would default to. A non-hostile
        // targeted spell (Healing Hands) will need the player instead -- that
        // is the case to revisit when such a spell is authored.
        std::uint32_t targetId = actor->GetFormID();
        auto *spell = FindSpell(decision.actionForm);
        if (spell)
            logger::info("  cast: {} is {} / {}", spell->GetName() ? spell->GetName() : "?",
                         spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration ? "concentration"
                                                                                                 : "fire-and-forget",
                         spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf ? "self" : "targeted");
        if (spell && spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
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

        // actionArg is the sustain time for a concentration spell, when a rule
        // sets one; zero takes the default.
        const auto request = RequestCast(actor, decision.actionForm, targetId, decision.actionArg);
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
        if (decision.actionForm == 0)
        {
            ReleaseKind(actor, ft::KindOf(decision.action));
            return ActionResult::Performed;
        }
        return PinNow(actor, decision.actionForm, decision.hand) ? ActionResult::Performed : ActionResult::MissingItem;

    default:
        // Every other action is Phase 4. The rule engine's Capabilities table is
        // what should stop these being authored at all; reaching here means the
        // capability flags and this switch have drifted apart.
        return ActionResult::NoSuchAction;
    }
}

} // namespace ft::game
