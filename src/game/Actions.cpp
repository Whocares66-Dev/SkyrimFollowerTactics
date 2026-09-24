#include "game/Actions.h"

#include "game/Blows.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/PlayerCast.h"
#include "game/Sensors.h"
#include "game/Sheet.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include "RE/B/BGSAction.h"
#include "RE/C/CombatAnimation.h"

#include <algorithm>
#include <cmath>

// wingdi.h names a GetObject of its own, over the default-object lookup.
#undef GetObject

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

// A power attack on the player's own body: the engine's action the attack
// handler sends for a hold past the power-attack delay (read from
// 1.6.1170: the right, left or dual power attack action by the hands),
// through the graph as a CombatAnimation, as a follower's bash goes.
bool PerformPlayerPowerAttack(RE::Actor *player, ft::Swing swing)
{
    auto *defaults = RE::BGSDefaultObjectManager::GetSingleton();
    using Object = RE::BGSDefaultObjectManager::DefaultObject;
    const auto id = swing == ft::Swing::Left   ? Object::kActionLeftPowerAttack
                    : swing == ft::Swing::Both ? Object::kActionDualPowerAttack
                                               : Object::kActionRightPowerAttack;
    auto *action = defaults ? defaults->GetObject<RE::BGSAction>(id) : nullptr;
    auto *anim = action ? RE::CombatAnimation::Create(player, action) : nullptr;
    if (!anim)
        return false;
    const bool performed = anim->Execute();
    anim->~CombatAnimation();
    RE::free(anim);
    return performed;
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
    case ft::ActionKind::ApplyAny:
    case ft::ActionKind::ApplyPoison: {
        auto *poison = RE::TESForm::LookupByID<RE::AlchemyItem>(action.form);
        return ApplyPoison(actor, poison && poison->IsPoison() ? poison : nullptr);
    }
    case ft::ActionKind::DrinkStrongest:
    case ft::ActionKind::DrinkWeakest:
    case ft::ActionKind::DrinkAny:
    case ft::ActionKind::DrinkPotion:
    case ft::ActionKind::EatStrongestFood:
    case ft::ActionKind::EatWeakestFood:
    case ft::ActionKind::EatAnyFood:
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
        // package it is (dev/ACTIONS.md 7). A shout goes through the same package
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
        // The player's own voice, by the shout control; a power or shout
        // goes at whom the player aims, as their own does.
        if (actor->IsPlayerRef())
        {
            const auto request = RequestPlayerVoice(actor, action.form, ruleIndex, ruleName);
            log::actions.debug("{} on the player: {}", shout ? "shout" : "power", ToString(request));
            return ResultOf(request);
        }
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
        // The player casts from their own hand, by a press of its control
        // (game/PlayerCast.h), and aims as they aim; a scroll is not read yet.
        if (actor->IsPlayerRef())
        {
            const auto request = RequestPlayerCast(actor, action.form, action.arg, action.dual, ruleIndex, ruleName);
            log::actions.debug("cast on the player: {}", ToString(request));
            return ResultOf(request);
        }
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
        // The player's is a plain equip: a pin is a leash on a combat AI
        // the player does not run.
        if (actor->IsPlayerRef())
            return WearNow(actor, action.form, WearRequest::Equip, action.hand, action.variant)
                       ? ActionResult::Performed
                       : ActionResult::MissingItem;
        return PinNow(actor, action.form, action.hand, action.variant) ? ActionResult::Performed
                                                                       : ActionResult::MissingItem;

    case ft::ActionKind::Attack:
        return PointAt(actor, target);

    case ft::ActionKind::PowerAttack:
    case ft::ActionKind::Bash:
    case ft::ActionKind::PowerBash: {
        // At an enemy who is not the follower's target, point them there
        // first, as Attack does. The player aims for themself: the blow
        // goes where they are looking.
        const bool player = actor->IsPlayerRef();
        const auto current = actor->GetActorRuntimeData().currentCombatTarget.get();
        const std::uint32_t currentId = current ? current->GetFormID() : 0;
        const bool elsewhere = !player && target != 0 && target != actor->GetFormID();
        if (elsewhere && target != currentId)
        {
            if (PointAt(actor, target) != ActionResult::Performed)
                return ActionResult::NoTarget;
        }
        const BlowPlan blow = PlanBlow(actor, action.kind);
        if (!blow.Possible())
            return ActionResult::MissingItem;

        // A bash is made from a block, as the engine makes one: raised, the
        // bash asked for once it is up, lowered (game/Blows.h). The same
        // actions the attack handler sends for the player's own block and
        // attack, so the sequence is theirs too, aimed by nobody.
        if (action.kind != ft::ActionKind::PowerAttack)
            return RequestBash(actor, player ? 0 : target, action.kind == ft::ActionKind::PowerBash, ruleIndex,
                               ruleName) == BashRequest::Started
                       ? ActionResult::Requested
                       : ActionResult::Busy;

        if (player)
        {
            auto *state = actor->AsActorState();
            if (!state || !state->IsWeaponDrawn())
                return ActionResult::WeaponSheathed;
            if (state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone)
                return ActionResult::MidSwing;
            const bool sent = PerformPlayerPowerAttack(actor, blow.swing);
            log::actions.debug("power attack: {} by the engine's action ({:.0f} stamina){}", Describe(actor),
                               blow.stamina, sent ? "" : " -- the action was refused");
            return sent ? ActionResult::Performed : ActionResult::GraphRefused;
        }

        // A power attack with the right hand's weapon goes through the
        // follower's UseWeapon record, which waits for their own swing to end
        // instead of being turned away mid-swing: sent as an event, 3 of 11
        // blows landed in play (2026-09-09, dev/ACTIONS.md 6). The procedure
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
