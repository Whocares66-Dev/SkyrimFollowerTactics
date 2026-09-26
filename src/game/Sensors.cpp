#include "game/Sensors.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"
#include "game/Bag.h"
#include "game/Blows.h"
#include "game/CustomSkillsFramework.h"
#include "game/Effects.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
#include "game/Sheet.h"
#include "game/Spells.h"
#include "game/Toggles.h"
#include "game/Traits.h"
#include "game/Util.h"
#include "progression/game/Service.h"
#include "progression/game/ValueView.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ft::game
{

namespace
{

// MagicNoReanimate, Skyrim.esm: the keyword the Reanimate archetype's one
// condition refuses.
constexpr std::uint32_t kMagicNoReanimateKeyword = 0x0006F6FB;

void ScanPotions(RE::Actor *actor, ft::PotionStock &stock)
{
    // Filtered at the source: asking GetInventory for only the consumable
    // types is markedly cheaper than pulling the whole inventory and sorting
    // it here, and a follower's bag can be large.
    auto inventory = actor->GetInventory(
        [](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::AlchemyItem) || obj.Is(RE::FormType::Ingredient); });

    for (auto &[object, entry] : inventory)
    {
        const auto count = entry.first;
        if (count <= 0 || !object)
            continue;
        const auto kind = ConsumableKindOf(object);
        if (!kind)
            continue;
        stock.carried.push_back({object->GetFormID(), static_cast<int>(count), *kind,
                                 ConsumableEffects(actor, object->As<RE::MagicItem>(), *kind)});
    }
}

// The alchemy effects in force on an actor -- a potion's, a poison's, a
// food's, an ingredient's -- by name and strength.
//
// ALCHEMY sources only, because that is the stacking rule: alchemy effects
// do not add to one another, only the strongest of a name is in force, but
// they do stack with enchantments (UESP, Skyrim:Alchemy_Effects). A worn
// Fortify One-handed ring shares the potion's name and writes a different
// value (dev/RESEARCH.md 6); counting it would keep a follower off a potion
// that would have stacked. (Until 2026-09-16 any source counted, when the
// question was only "is something of this name up".)
//
// An INSTANT effect has duration 0 and never lingers here, so on a vanilla
// game a Restore is never listed and the settle time in MinimumCooldown
// does the spacing. Potion overhauls convert restores to over-time effects,
// and there this is the exact answer where a fixed settle would be a guess.
// A Fortify or a Resist runs for a minute and is listed throughout.
std::vector<ft::RunningEffect> RunningEffects(RE::Actor *actor)
{
    using Type = RE::MagicSystem::SpellType;
    std::vector<ft::RunningEffect> out;
    ForEachActiveEffect(actor, [&out](RE::ActiveEffect &ae) {
        // duration 0 is an instant effect that has already happened.
        if (!(ae.duration > 0.0f && ae.elapsedSeconds < ae.duration))
            return;
        const auto type = ae.spell ? ae.spell->GetSpellType() : Type::kSpell;
        if (type != Type::kPotion && type != Type::kPoison && type != Type::kIngredient)
            return;
        // The strength as the record of what applied it has it, not as it
        // runs: the bag's side is read off the records too (ConsumableEffects),
        // and the two must be the same kind of number. As it runs, a mod's
        // rescaling after it lands reads as a weaker dose than the same food
        // in the bag: Gourmet's goat cheese, 25 on the record, ran at 10, and
        // "eat the strongest" ate one every few seconds (2026-09-24).
        const char *name = ae.effect->baseEffect->GetFullName();
        if (name && *name)
            out.push_back({name, ae.effect->effectItem.magnitude});
    });
    return out;
}

// Whom an actor is fighting, as the engine sees it, if they are still
// alive: the dead are nobody's target.
ft::ActorId LiveTargetOf(RE::Actor *actor)
{
    auto target = actor ? actor->GetActorRuntimeData().currentCombatTarget.get() : nullptr;
    return target && !target->IsDead() ? target->GetFormID() : 0;
}

} // namespace

ft::Stat ReadStat(RE::Actor *actor, RE::ActorValue av)
{
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return {};
    // Current is the damaged value. The maximum is the permanent value --
    // base plus the permanent modifier -- plus the TEMPORARY modifier,
    // where a follower's Fortify enchantment or potion lands (the player's
    // goes in the permanent one, dev/MODIFIERS.md): a
    // circlet of +50 magicka raises what the bar can show, and reading the
    // permanent value alone put 346 over 246 (2026-09-09). Their ratio is
    // what the rules read, so both are logged in Tactics.cpp to make a
    // wrong reading visible rather than merely wrong.
    const float temporary = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, av);
    return ft::Stat{owner->GetActorValue(av), owner->GetPermanentActorValue(av) + temporary};
}

namespace
{

// One random number for one evaluation: what every "any" action indexes
// with (Snapshot::roll). Drawn here, on the game side, so that the rule
// engine stays a pure function of its snapshot and a test names the choice
// instead of sampling for it.
//
// A generator of our own rather than the engine's: this is asked once per
// follower per tick, nothing in the game depends on the sequence, and a
// thread_local one needs no lock. Seeded from the platform's entropy, so
// two followers evaluated on the same tick do not choose in step.
std::uint32_t Roll()
{
    static thread_local std::mt19937 gen{std::random_device{}()};
    return gen();
}

// The steps of a snapshot, in the order BuildSnapshot takes them: the
// actor's own stats, blows and traits; the party, the enemies and the
// corpses; the hands; the spells known with their costs; the effects
// running; the bag. Each timed, so the cost line says which one a slow snapshot is
// paying for rather than the whole.
enum class Step : std::size_t
{
    Self,
    Party,
    // The allies' and enemies' traits alone, one sample each, inside Party.
    Traits,
    Hands,
    Spells,
    Effects,
    Bag,
    COUNT
};
constexpr std::array<const char *, static_cast<std::size_t>(Step::COUNT)> kStepNames{
    "self", "party", "traits(each, in party)", "hands", "spells", "effects", "bag"};
std::array<StepCost, static_cast<std::size_t>(Step::COUNT)> g_stepCost;

void Charge(Step step, std::chrono::steady_clock::time_point since)
{
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - since).count();
    StepCost &cost = g_stepCost[static_cast<std::size_t>(step)];
    cost.totalUs += us;
    cost.maxUs = (std::max)(cost.maxUs, us);
    ++cost.samples;
}

// The bag: the potions, food and ingredients; the items of the loadout as
// the pin book sees them, with each variant the bag holds; what is pinned.
// Each a walk of the inventory, and a step of its own on the cost line.
// The scrolls, the spells of the loadout and the soul gems are the spell
// step's (BuildSnapshot), once: until 2026-09-19 this repeated all three,
// left over from a day of reading the bag on demand (on the player it
// measured about a millisecond of a 20 ms snapshot, not worth the
// machinery).
void FillBag(RE::Actor *actor, ft::Snapshot &s)
{
    ScanPotions(actor, s.potions);
    // What they could hold or wear, as the pin book sees it, and what is
    // pinned. A walk of their inventory that keeps only the equipable kinds;
    // the potion scan above walks it too, and the two could share one pass
    // if the cost ever showed, which at tens of microseconds it does not.
    for (const auto &[object, entry] : actor->GetInventory())
    {
        if (!object || entry.first <= 0)
            continue;
        if (!(object->Is(RE::FormType::Weapon) || object->Is(RE::FormType::Armor) || object->Is(RE::FormType::Ammo) ||
              object->Is(RE::FormType::Light)))
            continue;
        // The form, whichever variant; and each variant the bag holds once,
        // for a rule that names one (Action::variant). A variant's count is
        // summed over its rows.
        s.loadout.push_back(DescribeHoldable(actor, object));
        std::vector<ft::ItemVariant> variants;
        const auto noted = [&](const ft::ItemVariant &variant) {
            return std::any_of(variants.begin(), variants.end(),
                               [&](const ft::ItemVariant &had) { return ft::SameVariant(had, variant); });
        };
        std::int32_t listed = 0;
        auto *lists = entry.second ? entry.second->extraLists : nullptr;
        if (lists)
        {
            for (const auto *list : *lists)
            {
                if (!list)
                    continue;
                listed += list->GetCount();
                if (ft::ItemVariant variant = VariantOf(list); !noted(variant))
                    variants.push_back(std::move(variant));
            }
        }
        if (entry.first > listed && !noted(ft::ItemVariant{}))
            variants.emplace_back();
        for (const ft::ItemVariant &variant : variants)
            s.loadout.push_back(DescribeHoldable(actor, object, variant));
    }
    // The player's "pins" are what they have on: an equip rule of theirs is
    // done when the thing is worn, and nothing chooses for them to pin
    // against (game/Pins.h, WornAsPins).
    s.worn = WornAsPins(actor);
    s.pins = actor->IsPlayerRef() ? s.worn : PinsOf(s.self);
}

// What casting a spell, a scroll or a shout on oneself would put up (core's
// SpellState::lasting): its effects that last -- a duration, or a constant
// effect -- shown and not hostile, as its record has them. A shout by the
// word its action shouts, the highest unlocked.
std::vector<ft::RunningEffect> LastingEffectsOf(RE::TESForm *form)
{
    RE::MagicItem *item = form ? form->As<RE::MagicItem>() : nullptr;
    if (auto *shout = form ? form->As<RE::TESShout>() : nullptr)
        if (const int word = HighestUnlockedWord(shout); word >= 0)
            item = shout->variations[word].spell;
    std::vector<ft::RunningEffect> out;
    if (!item)
        return out;
    using Flag = RE::EffectSetting::EffectSettingData::Flag;
    const bool constant = item->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect;
    for (const RE::Effect *effect : ResolvedEffects(*item))
    {
        const auto *base = effect->baseEffect;
        const char *name = base->GetFullName();
        const bool lasts = constant || (effect->effectItem.duration > 0 && !base->data.flags.any(Flag::kNoDuration));
        if (lasts && name && *name && !base->IsHostile() && !base->data.flags.any(Flag::kHideInUI))
            out.push_back({name, effect->effectItem.magnitude});
    }
    return out;
}

} // namespace

ft::Snapshot BuildSnapshot(RE::Actor *actor, double now, const std::vector<std::uint32_t> &priced)
{
    ft::Snapshot s;

    if (!actor)
        return s;

    s.self = actor->GetFormID();
    s.now = now;
    s.roll = Roll();

    // Where the time goes, step by step, for the cost line (TakeSnapshotCosts).
    auto last = std::chrono::steady_clock::now();
    const auto lap = [&last](Step step) {
        Charge(step, last);
        last = std::chrono::steady_clock::now();
    };

    s.health = ReadStat(actor, RE::ActorValue::kHealth);
    s.magicka = ReadStat(actor, RE::ActorValue::kMagicka);
    s.stamina = ReadStat(actor, RE::ActorValue::kStamina);

    s.inCombat = actor->IsInCombat();
    s.voiceRecovery = VoiceRecoveryOf(actor);
    for (const auto kind : {ft::ActionKind::PowerAttack, ft::ActionKind::Bash, ft::ActionKind::PowerBash})
    {
        const BlowPlan plan = PlanBlow(actor, kind);
        s.BlowFor(kind) = {plan.Possible(), plan.perk, plan.stamina, plan.reach};
    }

    s.traits = ReadTraits(actor);
    lap(Step::Self);

    // Whom the follower is fighting, as the engine sees it: what "current
    // target" resolves to.
    s.currentTarget = LiveTargetOf(actor);

    // The party, the enemies and the corpses: who is who is core's
    // (core/Party.h, AssembleParty, tested) over one walk of the loaded
    // actors, each read the same way; the views are then built for the
    // ones chosen, in the order the plan gives.
    auto *player = RE::PlayerCharacter::GetSingleton();
    const auto viewOf = [&](RE::Actor *other) {
        ft::ActorView view;
        view.id = other->GetFormID();
        view.health = ReadStat(other, RE::ActorValue::kHealth);
        view.magicka = ReadStat(other, RE::ActorValue::kMagicka);
        view.stamina = ReadStat(other, RE::ActorValue::kStamina);
        view.distance = actor->GetPosition().GetDistance(other->GetPosition());
        view.reachDistance = ReachDistance(actor, other);
        view.target = LiveTargetOf(other);
        const auto started = std::chrono::steady_clock::now();
        view.traits = ReadTraits(other);
        Charge(Step::Traits, started);
        return view;
    };
    std::vector<ft::ActorSeen> loaded;
    std::unordered_map<ft::ActorId, RE::Actor *> byId;
    if (auto *lists = RE::ProcessLists::GetSingleton())
    {
        auto *noReanimate = RE::TESForm::LookupByID<RE::BGSKeyword>(kMagicNoReanimateKeyword);
        lists->ForEachHighActor([&](RE::Actor *otherPtr) {
            if (!otherPtr || otherPtr == player)
                return RE::BSContainer::ForEachResult::kContinue;
            RE::Actor &other = *otherPtr;
            ft::ActorSeen seen;
            seen.id = other.GetFormID();
            seen.dead = other.IsDead();
            if (!seen.dead)
            {
                seen.teammate = other.IsPlayerTeammate();
                seen.inCombat = other.IsInCombat();
                seen.hostile = player && other.IsHostileToActor(player);
            }
            else
            {
                seen.commanded = other.IsCommandedActor();
                seen.noReanimate = noReanimate && other.HasKeyword(noReanimate);
                seen.distance = actor->GetPosition().GetDistance(other.GetPosition());
                seen.level = static_cast<int>(other.GetLevel());
            }
            loaded.push_back(seen);
            byId[seen.id] = otherPtr;
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }
    if (player)
        byId[player->GetFormID()] = player;
    const ft::PartyPlan party = ft::AssembleParty(actor->GetFormID(), player ? player->GetFormID() : 0,
                                                  player && !player->IsDead(), loaded, s.currentTarget);
    const auto actorOf = [&](ft::ActorId id) -> RE::Actor * {
        const auto it = byId.find(id);
        return it != byId.end() ? it->second : RE::TESForm::LookupByID<RE::Actor>(id);
    };
    for (const ft::ActorId id : party.allies)
        if (auto *other = actorOf(id))
            s.allies.push_back(viewOf(other));
    for (const ft::ActorId id : party.enemies)
        if (auto *other = actorOf(id))
            s.enemies.push_back(viewOf(other));
    s.corpses = party.corpses;

    lap(Step::Party);
    for (const bool left : {false, true})
    {
        auto &hand = left ? s.leftWeapon : s.rightWeapon;
        const Hand which = left ? Hand::Left : Hand::Right;
        if (auto *weapon = PoisonableWeaponIn(actor, left))
        {
            hand.takesPoison = true;
            hand.poisoned = WeaponPoisoned(actor, weapon, which);
        }
        if (auto *weapon = WeaponIn(actor, left))
        {
            const WeaponCharge c = ChargeOf(actor, weapon, which);
            hand.enchanted = c.enchanted;
            hand.charge = c.charge;
            hand.maxCharge = c.maxCharge;
            hand.costPerHit = c.costPerHit;
            hand.bound = weapon->IsBound();
        }
    }
    s.soulGems = ScanSoulGems(actor);

    s.potions.running = RunningEffects(actor);
    // What a poison would land on, as far as can be told before the blow:
    // the follower's own mark. One more walk of an effect list, and only
    // with a mark.
    if (auto *mark = s.currentTarget ? RE::TESForm::LookupByID<RE::Actor>(s.currentTarget) : nullptr)
        s.targetRunning = RunningEffects(mark);

    lap(Step::Hands);
    // Spells: what they know, what is running, what is in hand. All three
    // are ids only -- Snapshot never sees an RE:: type -- and which of the
    // records read is known, used today, castable or active is core's
    // (core/Spells.h, ClassifySpells and ActiveSpells, tested); this reads
    // the records, and prices the castable spells a rule names.
    std::vector<ft::SpellSeen> seen;
    std::vector<ft::ShoutWords> shoutWords;
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numShouts; ++i)
            {
                RE::TESShout *shout = list->shouts[i];
                if (!shout)
                    continue;
                ft::SpellSeen fact;
                fact.id = shout->GetFormID();
                fact.kind = ft::SpellSeen::Kind::Shout;
                fact.wrapper = IsWrapperShout(fact.id);
                fact.highestWord = HighestUnlockedWord(shout);
                seen.push_back(fact);
                if (fact.wrapper)
                    continue;
                ft::ShoutWords words;
                words.shout = fact.id;
                for (const auto &variation : shout->variations)
                    words.words.push_back(variation.spell ? variation.spell->GetFormID() : 0);
                shoutWords.push_back(std::move(words));
            }
        }
    }
    for (const auto &[object, entry] :
         actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::Scroll); }))
    {
        if (!object)
            continue;
        ft::SpellSeen fact;
        fact.id = object->GetFormID();
        fact.kind = ft::SpellSeen::Kind::Scroll;
        fact.carried = entry.first;
        seen.push_back(fact);
    }
    std::unordered_map<std::uint32_t, RE::SpellItem *> spellsById;
    ForEachSpell(actor, [&](RE::SpellItem *spell) {
        ft::SpellSeen fact;
        fact.id = spell->GetFormID();
        if (IsPower(spell))
        {
            fact.kind = ft::SpellSeen::Kind::Power;
            fact.greater = spell->GetSpellType() == RE::MagicSystem::SpellType::kPower;
            fact.usedToday = fact.greater && actor->IsInCastPowerList(spell);
        }
        else
        {
            fact.castable = IsCastable(spell);
            spellsById[fact.id] = spell;
        }
        seen.push_back(fact);
    });
    const ft::SpellsKnown known = ft::ClassifySpells(seen);
    s.spells.known.insert(s.spells.known.end(), known.known.begin(), known.known.end());
    s.spells.usedToday.insert(s.spells.usedToday.end(), known.usedToday.begin(), known.usedToday.end());
    for (const std::uint32_t id : known.castable)
    {
        RE::SpellItem *spell = spellsById[id];
        // As the pin book sees it, for an equip rule.
        s.loadout.push_back(DescribeHoldable(actor, spell));
        // Priced only if a rule names it (Sensors.h): the engine's cost
        // calculation is the dear part of the whole snapshot.
        if (std::find(priced.begin(), priced.end(), id) == priced.end())
            continue;
        // Their cost, not the base cost: CalculateMagickaCost applies their
        // skill and perks, which is what the AI will charge them.
        const bool dualable = CanDualCast(actor, spell);
        s.spells.costs.push_back(
            {id, spell->CalculateMagickaCost(actor), dualable, dualable ? DualCastCost(actor, spell) : 0.0f});
        // A Reanimate's cap: the level of corpse it can raise is its
        // effect's magnitude (Reanimate Corpse 13, Revenant 21, Dread
        // Zombie 30) -- as they cast it, perks and Fortify effects in, the
        // same way the engine judges the corpse. The Corpse subject
        // measures the dead against it.
        for (const auto *effect : ResolvedEffects(*spell))
        {
            if (IsReanimate(effect))
            {
                s.spells.caps.push_back({id, static_cast<int>(ActualMagnitude(actor, spell, effect))});
                break;
            }
        }
    }

    // What each cast a rule names would put up, and what spells have in
    // force: whether a cast on oneself would add anything (core's
    // AnyWouldLand), by the records on both sides.
    for (const std::uint32_t id : priced)
        if (auto effects = LastingEffectsOf(RE::TESForm::LookupByID(id)); !effects.empty())
            s.spells.lasting.push_back({id, std::move(effects)});

    lap(Step::Spells);
    std::vector<ft::EffectSeen> effects;
    ForEachActiveEffect(actor, [&effects, &s](RE::ActiveEffect &ae) {
        effects.push_back({ae.spell ? ae.spell->GetFormID() : 0, ae.duration, ae.elapsedSeconds});
        // A spell's, a scroll's, a power's or a shout's, live and shown: not
        // an ability's or an enchantment's, which stack with a cast of the
        // name, and not alchemy's, which the bag asks of its own.
        using Type = RE::MagicSystem::SpellType;
        const auto type = ae.spell ? ae.spell->GetSpellType() : Type::kAbility;
        const auto *base = ae.effect->baseEffect;
        const char *name = base->GetFullName();
        if ((type == Type::kSpell || type == Type::kScroll || type == Type::kPower || type == Type::kLesserPower ||
             type == Type::kVoicePower) &&
            ae.duration > 0.0f && ae.elapsedSeconds < ae.duration && name && *name &&
            !base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
            s.spells.running.push_back({name, ae.effect->effectItem.magnitude});
    });
    const auto active = ft::ActiveSpells(effects, shoutWords);
    s.spells.active.insert(s.spells.active.end(), active.begin(), active.end());

    lap(Step::Effects);
    FillBag(actor, s);
    lap(Step::Bag);

    return s;
}

std::vector<StepCost> TakeSnapshotCosts()
{
    std::vector<StepCost> out;
    for (std::size_t i = 0; i < g_stepCost.size(); ++i)
    {
        StepCost step = g_stepCost[i];
        step.name = kStepNames[i];
        out.push_back(step);
        g_stepCost[i] = {};
    }
    return out;
}

std::string FollowerMarks(RE::Actor *actor)
{
    if (!actor)
        return "no actor";
    const auto in = [actor](std::uint32_t id) {
        const auto *faction = RE::TESForm::LookupByID<RE::TESFaction>(id);
        return faction && actor->IsInFaction(faction) ? 1 : 0;
    };
    return fmt::format("teammate={} current={} dismissed={} potential={}", actor->IsPlayerTeammate() ? 1 : 0,
                       in(kCurrentFollowerFaction), in(kDismissedFollowerFaction), in(kPotentialFollowerFaction));
}

bool IsDismissedFollower(RE::Actor *actor)
{
    if (!actor)
        return false;
    // Looked up each call rather than cached: the cache would be the one
    // thing here that outlives a load, and a faction is a pointer lookup.
    const auto *faction = RE::TESForm::LookupByID<RE::TESFaction>(kDismissedFollowerFaction);
    return faction && actor->IsInFaction(faction);
}

bool IsPerson(RE::Actor *actor)
{
    if (!actor)
        return false;
    struct Keywords
    {
        RE::BGSKeyword *npc, *animal, *creature;
    };
    static const Keywords k = [] {
        const auto by = [](const char *id) { return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(id); };
        return Keywords{by("ActorTypeNPC"), by("ActorTypeAnimal"), by("ActorTypeCreature")};
    }();
    const auto has = [&](const RE::BGSKeyword *keyword) { return keyword && actor->HasKeyword(keyword); };
    // Marked a person: that settles it, whatever else is on the race. A
    // werewolf's beast race carries the creature keyword over an NPC.
    if (has(k.npc))
        return true;
    return !has(k.animal) && !has(k.creature);
}

} // namespace ft::game
