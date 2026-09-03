#include "game/Actions.h"

#include "game/Packages.h"

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

// Put the spell in her hand and leave the choice of when to use it to her own
// combat AI. A different thing from casting it, and worth having both: a buff
// wants casting now, an attack spell wants equipping and trusting.
ActionResult EquipKnownSpell(RE::Actor *actor, RE::SpellItem *spell)
{
    if (!spell)
        return ActionResult::MissingItem;

    auto *equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager)
        return ActionResult::NoEquipManager;

    equipManager->EquipSpell(actor, spell, nullptr);

    // Equipping always "works" -- the spell goes in her hand whether or not she
    // will ever cast it -- so on its own this action cannot tell the difference
    // between the two ways it fails, and they need opposite fixes:
    //
    //   she CANNOT cast it   too expensive for her pool and skill. Nothing
    //                        about packages or combat styles will help.
    //   she WILL NOT cast it can afford it, her combat AI simply chose
    //                        something else. That is the UseMagic package case.
    //
    // CheckCast answers the first question directly, using the game's own
    // arithmetic including her Alteration skill, so the log separates them
    // instead of leaving it to inference.
    auto *caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
    if (!caster)
        return ActionResult::Performed;

    float alchStrength = 1.0f;
    RE::MagicSystem::CannotCastReason reason{};
    const bool couldCast = caster->CheckCast(spell, /*dualCast*/ false, &alchStrength, &reason, false);

    float magicka = 0.0f;
    if (auto *owner = actor->AsActorValueOwner())
        magicka = owner->GetActorValue(RE::ActorValue::kMagicka);

    // CalculateMagickaCost(actor), not caster->GetCurrentSpellCost(). The latter
    // reports whatever spell the caster happens to have selected right now,
    // which is usually something else entirely -- it logged "cost 1" for a
    // spell with a base cost of 73, which is not a number anyone can act on.
    // This one is the cost of THIS spell for THIS actor, skill included.
    logger::info("  equipped {}: castable={} ({}), cost {:.0f}, magicka {:.0f}",
                 spell->GetName() ? spell->GetName() : "?", couldCast,
                 CannotCastText(static_cast<std::uint32_t>(reason)), spell->CalculateMagickaCost(actor), magicka);

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
                logger::info("  cast: {} needs a target and she is fighting no one",
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

    case ft::ActionKind::EquipSpell:
        return EquipKnownSpell(actor, FindSpell(decision.actionForm));

    default:
        // Every other action is Phase 4. The rule engine's Capabilities table is
        // what should stop these being authored at all; reaching here means the
        // capability flags and this switch have drifted apart.
        return ActionResult::NoSuchAction;
    }
}

} // namespace ft::game
