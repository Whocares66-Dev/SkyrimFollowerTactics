#include "game/Effects.h"

#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Names.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"

#include "game/Addresses.h"
#include "game/CustomSkillsFramework.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
#include "game/Toggles.h"
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

using ft::i18n::TrFormat;

namespace ft::game
{

namespace
{

// A bane rather than a boon: the two flags the Creation Kit shows as
// Detrimental and Hostile. What tells a poison's effects from a potion's,
// and a Weakness to Fire from a Resist Fire.
bool Harmful(const RE::EffectSetting *base)
{
    return base->IsDetrimental() || base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHostile);
}

// A BUFF: a lingering boon an "any" drink or eat rule may reach for, as
// against a Restore, which is the emergency being kept back (Rule.h,
// ActionKind::DrinkAny).
//
// Read off the effect RECORD and not its name, so a mod's own Fortify
// counts and no table of names has to be maintained: the archetype is
// PeakValueModifier -- the engine's word for a temporary modifier it takes
// back when the effect ends -- and the effect is a boon; the judgement
// itself is core's (core/Effects.h, IsBuff, tested), over the record's
// shape this file reads. Held against every
// vanilla alchemy effect (2026-09-16): every Fortify, Resist and Regenerate
// answers yes; every Restore is a plain ValueModifier flagged No Duration
// and answers no; Cure Disease and Cure Poison have archetypes of their
// own; a Weakness or a Slow is a PeakValueModifier and is caught as harmful.
//
// And it must do something for THIS actor. A Fortify One-handed potion
// writes OneHandedPowerModifier, which only a hidden perk reads, and an
// actor without the perk drinks it for nothing (dev/RESEARCH.md 6;
// EffectApplies, below, asks the actor). An "any buff" that rolled one of
// those would be the waste it exists to avoid.
//
// Two edges, stated rather than guarded: an effect the BOTTLE gives no
// duration is no buff whatever its record says, which is the check that
// keeps a constant-effect record out; and a record that forgets the
// detrimental flag on a bane reads as a buff (ccASVSSE001's Slow does).
//
// Waterbreathing is left out by hand, by the actor value it writes rather
// than by its name, so a mod's own goes with it: it does nothing for a
// follower, and an "any" that rolled it would spend the item for nothing.
// It needs saying because it is NOT an archetype of its own -- every
// vanilla Waterbreathing effect, AlchWaterbreathing (03AC2D) included, is a
// PeakValueModifier flagged NoMagnitude (checked against the records
// 2026-09-16; only the enchantment's is a plain archetype, and the duration
// test above already keeps that one out). Invisibility IS its own archetype
// and needs no check. Muffle is a PeakValueModifier like Waterbreathing and
// so still reads as a buff here -- left in, since it does at least do
// something for a follower, but noted rather than guarded.
ft::EffectShape ShapeOf(const RE::EffectSetting *base, float duration);

} // namespace

std::vector<ft::PotionStock::Effect> ConsumableEffects(RE::Actor *actor, RE::MagicItem *item, ft::ConsumableKind kind,
                                                       bool *anyLands)
{
    if (anyLands)
        *anyLands = false;
    if (!item)
        return {};
    std::vector<ft::ConsumableEffectSeen> seen;
    for (auto *effect : ResolvedEffects(*item))
    {
        const auto *base = effect->baseEffect;
        const char *name = base->GetFullName();
        // Taken by the one consuming it, as the engine would land it on them;
        // a poison's lands on whoever is struck, and is not judged here.
        const bool lands = kind == ft::ConsumableKind::Poison || LandsOn(*effect, item, actor, actor, false);
        if (anyLands)
            *anyLands = *anyLands || lands;
        seen.push_back({name ? name : "", effect->effectItem.magnitude, static_cast<float>(effect->effectItem.duration),
                        ShapeOf(base, static_cast<float>(effect->effectItem.duration)), lands});
    }
    // Which of them the rules see, and an ingredient's first effect alone,
    // are core's (core/Effects.h, ConsumableEffectsOf, tested).
    return ft::ConsumableEffectsOf(seen, kind, ReadsSkillMods(actor), ReadsSkillPowerMods(actor));
}

namespace
{

// VendorItemFood, Skyrim.esm: the keyword on the few ingredients that are
// food -- a charred skeever hide, an egg, snowberries. Any other
// ingredient is eaten only to learn what it does, and a follower has
// nothing to learn.
constexpr std::uint32_t kVendorItemFoodKeyword = 0x0008CDEA;

} // namespace

std::optional<ft::ConsumableKind> ConsumableKindOf(RE::TESBoundObject *object)
{
    if (auto *alch = object->As<RE::AlchemyItem>())
    {
        if (alch->IsPoison())
            return ft::ConsumableKind::Poison;
        return alch->IsFood() ? ft::ConsumableKind::Food : ft::ConsumableKind::Potion;
    }
    if (auto *ingredient = object->As<RE::IngredientItem>())
    {
        auto *food = RE::TESForm::LookupByID<RE::BGSKeyword>(kVendorItemFoodKeyword);
        if (food && ingredient->HasKeyword(food))
            return ft::ConsumableKind::Ingredient;
        return std::nullopt;
    }
    return std::nullopt;
}

namespace
{

// Does this effect change anything for this actor? A value-modifying effect
// on a skill modifier -- Fortify One-handed's OneHandedModifier, Fortify
// Destruction's DestructionModifier -- is read only by the two hidden perks
// a follower does not carry (dev/RESEARCH.md 6): the value moves, and
// nothing looks at it.
// The record's shape, for core's EffectApplies and IsBuff (core/Effects.h).
ft::EffectShape ShapeOf(const RE::EffectSetting *base, float duration)
{
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    ft::EffectShape shape;
    const auto archetype = base->GetArchetype();
    shape.valueModifier = archetype == Archetype::kValueModifier || archetype == Archetype::kPeakValueModifier ||
                          archetype == Archetype::kDualValueModifier;
    shape.peakValue = base->HasArchetype(RE::EffectSetting::Archetype::kPeakValueModifier);
    const auto av = static_cast<int>(base->data.primaryAV);
    constexpr int kFirstModifier = static_cast<int>(RE::ActorValue::kOneHandedModifier);
    constexpr int kLastModifier = static_cast<int>(RE::ActorValue::kEnchantingModifier);
    constexpr int kFirstPower = static_cast<int>(RE::ActorValue::kOneHandedPowerModifier);
    constexpr int kLastPower = static_cast<int>(RE::ActorValue::kEnchantingPowerModifier);
    shape.skillModifier = av >= kFirstModifier && av <= kLastModifier;
    shape.skillPower = av >= kFirstPower && av <= kLastPower;
    shape.harmful = Harmful(base);
    shape.waterbreathing = base->data.primaryAV == RE::ActorValue::kWaterBreathing;
    shape.duration = duration;
    return shape;
}

} // namespace

bool EffectApplies(const RE::Actor *actor, const RE::EffectSetting *base)
{
    return ft::EffectApplies(ShapeOf(base, 0.0f), ReadsSkillMods(actor), ReadsSkillPowerMods(actor));
}

void ForEachActiveEffect(RE::Actor *actor, const std::function<void(RE::ActiveEffect &)> &fn)
{
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    auto *effects = target ? target->GetActiveEffectList() : nullptr;
    if (!effects)
        return;
    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
            continue;
        fn(*ae);
    }
}

std::vector<ConsumableOption> ScanCarriedConsumables(RE::Actor *actor)
{
    std::vector<ConsumableOption> out;
    if (!actor)
        return out;

    auto inventory = actor->GetInventory([](RE::TESBoundObject &obj) {
        return obj.Is(RE::FormType::AlchemyItem) || obj.Is(RE::FormType::Ingredient) || obj.Is(RE::FormType::SoulGem);
    });
    for (auto &[object, entry] : inventory)
    {
        const auto count = entry.first;
        if (count <= 0 || !object)
            continue;
        if (object->Is(RE::FormType::SoulGem))
        {
            // A filled, spendable gem, named with its soul where that is
            // less than the gem holds, as the game's own inventory names it:
            // "Common Soul Gem (Lesser)". A full one is just the gem; the
            // game's "Common Soul Gem (Common)" says it twice.
            const auto level = entry.second ? entry.second->GetSoulLevel() : RE::SOUL_LEVEL::kNone;
            if (level == RE::SOUL_LEVEL::kNone)
                continue;
            std::string name = NameOr(object, "?");
            const auto *gem = object->As<RE::TESSoulGem>();
            if (!gem || level < gem->GetMaximumCapacity())
                name = TrFormat("{} ({})", name, SoulName(level));
            out.push_back({object->GetFormID(), name, static_cast<int>(count), ft::ConsumableKind::SoulGem, {}});
            continue;
        }
        const auto kind = ConsumableKindOf(object);
        if (!kind)
            continue;
        std::vector<std::string> effects;
        bool any = false;
        for (const auto &effect : ConsumableEffects(actor, object->As<RE::MagicItem>(), *kind))
        {
            if (!ft::PotionStock::ChoosableBy(*kind, effect))
                continue;
            effects.push_back(effect.name);
            any = any || ft::PotionStock::WantedByAny(*kind, effect);
        }
        out.push_back(
            {object->GetFormID(), NameOr(object, "?"), static_cast<int>(count), *kind, std::move(effects), any});
    }
    ft::SortByName(out, [](const auto &item) -> std::string_view { return item.name; });
    return out;
}
namespace
{

using EffectFlag = RE::EffectSetting::EffectSettingData::Flag;

bool Lasts(const RE::MagicItem &item, const RE::Effect &effect)
{
    if (item.GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect)
        return true;
    return effect.effectItem.duration > 1 && !effect.baseEffect->data.flags.any(EffectFlag::kNoDuration);
}

// What a pick of this item stands for: the item, where it leaves something
// lasting; else the spell its script puts up, a toggle's ability or a
// shout's cast on each ally.
const RE::MagicItem *LastingOrScripted(const RE::MagicItem *item)
{
    if (!item || LastingEffect(item))
        return item;
    const auto *spell = item->As<RE::SpellItem>();
    return spell ? ScriptedSpell(spell) : nullptr;
}

// One character's picks, into `into`: what their scans say they can put up.
void AddEffectPicks(std::vector<ft::EffectPick> &into, const std::vector<SpellOption> &spells,
                    const std::vector<ConsumableOption> &consumables)
{
    const auto add = [&](const RE::MagicItem *item) {
        const RE::Effect *effect = LastingEffect(LastingOrScripted(item));
        // A summon is the Summon condition's: Black Market's merchant,
        // Conjure Familiar. A hidden effect never counts (ReadTraits).
        if (!effect || effect->baseEffect->HasArchetype(RE::EffectSetting::Archetype::kSummonCreature) ||
            effect->baseEffect->data.flags.any(EffectFlag::kHideInUI))
            return;
        into.push_back({NameOf(effect->baseEffect), effect->baseEffect->GetFormID()});
    };
    // An aimed one only where it is a buff: a spell cast at an enemy is
    // mostly a Status, and a drain's share on the caster is not what it is
    // cast for.
    const auto addAs = [&](const RE::MagicItem *item, bool self) {
        if (item && (self || !HasHarm(*item)))
            add(item);
    };
    for (const auto &option : consumables)
    {
        if (option.kind == ft::ConsumableKind::Potion || option.kind == ft::ConsumableKind::Food)
            add(RE::TESForm::LookupByID<RE::AlchemyItem>(option.form));
    }
    // A scroll is its spell's pick, a staff its enchantment's.
    for (const auto &option : spells)
    {
        if (option.kind == SpellOption::Kind::Spell || option.kind == SpellOption::Kind::Scroll)
            addAs(RE::TESForm::LookupByID<RE::MagicItem>(option.form), option.selfOnly);
        else if (option.kind == SpellOption::Kind::Staff)
        {
            const auto *staff = RE::TESForm::LookupByID<RE::TESObjectWEAP>(option.form);
            addAs(staff ? staff->formEnchanting : nullptr, option.selfOnly);
        }
        else if (option.kind == SpellOption::Kind::Power)
            add(RE::TESForm::LookupByID<RE::SpellItem>(option.form));
        else if (option.kind == SpellOption::Kind::Shout)
        {
            // The word a Shout action shouts: the highest unlocked.
            const auto *shout = RE::TESForm::LookupByID<RE::TESShout>(option.form);
            const int word = shout ? HighestUnlockedWord(shout) : -1;
            if (word >= 0)
                addAs(shout->variations[word].spell, option.selfOnly);
        }
    }
}

} // namespace

const RE::Effect *LastingEffect(const RE::MagicItem *item)
{
    if (!item)
        return nullptr;
    const RE::Effect *best = nullptr;
    const auto rank = [](const RE::Effect *effect) {
        return std::pair(!effect->baseEffect->data.flags.any(EffectFlag::kHideInUI), effect->cost);
    };
    for (const RE::Effect *effect : ResolvedEffects(*item))
        if (Lasts(*item, *effect) && (!best || rank(effect) > rank(best)))
            best = effect;
    return best;
}

bool HasHarm(const RE::MagicItem &item)
{
    return std::ranges::any_of(ResolvedEffects(item), [](const RE::Effect *effect) {
        return effect->baseEffect->IsHostile() || effect->baseEffect->IsDetrimental();
    });
}

namespace
{

// Skyrim.esm's Call to Arms and its scroll. The Master rally spell has no
// Rally effect in vanilla -- skills, health and stamina, and its target may
// still flee -- but it is one of the three, so it rallies by its record.
// Mysticism's has one, and counts either way.
bool IsCallToArms(const RE::MagicItem *spell)
{
    constexpr std::array<RE::FormID, 2> kCallToArms{0x0007E8DD, 0x000A44BE};
    return spell && std::ranges::find(kCallToArms, spell->GetFormID()) != kCallToArms.end();
}

} // namespace

// Is a Rally effect a courage? The Rally type raises Confidence so its
// target does not flee, which is why other spells carry one too: vanilla's
// Frenzy holds one so the frenzied do not run, and Simonrim's mods make it
// the carrier of Paralyze, Silence, Command, a Calm poison. Each of those
// has a harm in the spell beside it; Courage, Rally and Call to Arms have
// none, vanilla's or Mysticism's.
std::optional<ft::StatusKind> InfluenceOf(const RE::EffectSetting &base, const RE::MagicItem *spell)
{
    if (IsCallToArms(spell))
        return ft::StatusKind::Rallied;
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    switch (base.GetArchetype())
    {
    case Archetype::kRally:
        if (base.IsHostile() || base.IsDetrimental() || (spell && HasHarm(*spell)))
            return std::nullopt;
        return ft::StatusKind::Rallied;
    case Archetype::kCalm:
        return ft::StatusKind::Calmed;
    // Turn Undead is the engine's fear for the undead: its effect class
    // derives from Demoralize's.
    case Archetype::kDemoralize:
    case Archetype::kTurnUndead:
        return ft::StatusKind::Feared;
    case Archetype::kFrenzy:
        return ft::StatusKind::Frenzied;
    default:
        return std::nullopt;
    }
}

bool LandsOn(RE::Effect &effect, RE::MagicItem *spell, RE::Actor *caster, RE::Actor *target, bool dual)
{
    auto *magicTarget = target ? target->AsMagicTarget() : nullptr;
    if (!magicTarget || !spell || !effect.baseEffect)
        return false;
    // The entry's conditions are not the landing's: false, the effect lands
    // and waits inactive (34062 asks them of it running), and nothing it puts
    // up holds (ForEachActiveEffect skips it).
    if (effect.conditions.head && !effect.conditions.IsTrue(target, caster))
        return false;
    // The resistance AddTarget hands the check: the target's, but for an
    // ability or a spell that ignores it.
    const float resistance = spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility || spell->IgnoresResistance()
                                 ? 1.0f
                                 : magicTarget->CheckResistance(spell, &effect, nullptr);
    // The rest is the engine's own, whatever a mod has made of it. The
    // record's magnitude, as a cast's AddTargetData carries it: the check
    // puts the caster's perks and a dual cast on it itself. It sets the
    // caster's dual-cast flag around the record's conditions and clears it,
    // as every landing does.
    RE::MagicTarget::AddTargetData data{};
    data.caster = caster;
    data.magicItem = spell;
    data.effect = &effect;
    data.magnitude = effect.effectItem.magnitude;
    data.dualCasted = dual;
    RE::ActiveEffectFactory::CheckTargetArgs args{};
    args.target = magicTarget;
    args.caster = caster;
    args.magnitude = effect.effectItem.magnitude;
    args.effectSetting = effect.baseEffect;
    args.spell = spell;
    args.dualCast = dual;
    using Check = bool(RE::MagicTarget::AddTargetData *, RE::ActiveEffectFactory::CheckTargetArgs *, float);
    static REL::Relocation<Check *> check{addr::kCheckAddEffect};
    return check(&data, &args, resistance);
}

namespace
{

// What a rule's cast of the form casts: a spell or a scroll itself, a shout
// by the word its action shouts, the highest unlocked, a staff by its
// enchantment.
RE::MagicItem *CastItemOf(RE::TESForm *form)
{
    RE::MagicItem *item = form ? form->As<RE::MagicItem>() : nullptr;
    if (auto *shout = form ? form->As<RE::TESShout>() : nullptr)
        if (const int word = HighestUnlockedWord(shout); word >= 0)
            item = shout->variations[word].spell;
    if (auto *weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr; weapon && weapon->IsStaff())
        item = weapon->formEnchanting;
    return item;
}

} // namespace

// A cast of the form, as core's WouldHaveEffect weighs it (SpellState's
// casts and landings): whom it reaches, and whether anything of it would
// take on each of them -- at its centre, the follower, and for one aimed
// each ally, each enemy and, for one that raises, each corpse; about the
// centre, for one with an area, each of those again with its area effects
// alone; singly, and dual cast where they can. Each effect as the engine
// lands it (LandsOn), which for a Reanimate is the corpse's fitness and its
// level against the magnitude. Nothing for a form that is not a spell, a
// scroll, a staff, a power or a shout: core then takes it to act.
void AddLandings(RE::Actor *caster, std::uint32_t id, ft::Snapshot &s)
{
    auto *form = RE::TESForm::LookupByID(id);
    RE::MagicItem *item = CastItemOf(form);
    if (!item)
        return;
    using Delivery = RE::MagicSystem::Delivery;
    using Reach = ft::SpellState::Reach;
    const bool area = std::ranges::any_of(ResolvedEffects(*item),
                                          [](const RE::Effect *effect) { return AreaRadius(*effect) > 0.0f; });
    const Delivery delivery = item->GetDelivery();
    const Reach reach = delivery == Delivery::kTargetLocation ? Reach::Place
                        : delivery == Delivery::kSelf         ? Reach::Self
                                                              : Reach::Target;
    const bool raises = std::ranges::any_of(ResolvedEffects(*item), IsReanimate);
    const bool concentration = item->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    // What running there is this cast's: a shout's, from any of its words,
    // an aspect shouted at word one being up all the same; a staff's, from
    // its enchantment, the staff's own form being a weapon's.
    std::vector<std::uint32_t> from;
    if (auto *shout = form->As<RE::TESShout>())
        for (const auto &variation : shout->variations)
            if (variation.spell)
                from.push_back(variation.spell->GetFormID());
    if (form->Is(RE::FormType::Weapon))
        from.push_back(item->GetFormID());
    s.spells.casts.push_back({id, reach, raises, area, concentration, std::move(from)});
    if (reach == Reach::Place)
        return;
    auto *spell = item->As<RE::SpellItem>();
    const bool dualable = spell && IsCastable(spell) && CanDualCast(caster, spell);
    const auto judge = [&](ft::ActorId who, bool about) {
        auto *target = who == s.self ? caster : RE::TESForm::LookupByID<RE::Actor>(who);
        if (!target)
            return;
        // About a cast on oneself, the centre is the caster, and how far
        // away they stand says whether an effect's ring reaches them. About
        // an aimed one it is wherever the cast lands, which is not known.
        const float away =
            about && reach == Reach::Self ? caster->GetPosition().GetDistance(target->GetPosition()) : 0.0f;
        for (const bool dual : {false, true})
        {
            if (dual && !dualable)
                continue;
            // About the centre, only what spreads reaches, and only as far
            // as it spreads.
            const bool takes = std::ranges::any_of(ResolvedEffects(*item), [&](RE::Effect *effect) {
                return !(about && AreaRadius(*effect) < (std::max)(away, 1.0f)) &&
                       LandsOn(*effect, item, caster, target, dual);
            });
            s.spells.landings.push_back({id, who, dual, takes, about});
        }
    };
    const auto others = [&](bool about) {
        for (const auto &ally : s.allies)
            judge(ally.id, about);
        for (const auto &enemy : s.enemies)
            judge(enemy.id, about);
        if (raises)
            for (const auto &corpse : s.corpses)
                judge(corpse.id, about);
    };
    judge(s.self, false);
    if (reach == Reach::Target)
        others(false);
    if (!area)
        return;
    // The caster is about the centre of an aimed area, and at the centre
    // of their own.
    if (reach == Reach::Target)
        judge(s.self, true);
    others(true);
}

std::vector<ft::EffectPick> ScanEffectPicks(const std::vector<SpellOption> &spells,
                                            const std::vector<ConsumableOption> &consumables,
                                            const std::vector<RE::Actor *> &others)
{
    std::vector<ft::EffectPick> candidates;
    AddEffectPicks(candidates, spells, consumables);
    for (RE::Actor *other : others)
        AddEffectPicks(candidates, ScanCastableSpells(other), ScanCarriedConsumables(other));
    return ft::ArrangeEffectPicks(std::move(candidates));
}

bool ReadsSkillMods(const RE::Actor *actor)
{
    static auto *perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x000CF788);
    return perk && actor && actor->HasPerk(perk);
}

bool ReadsSkillPowerMods(const RE::Actor *actor)
{
    static auto *perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x000A725C);
    return perk && actor && actor->HasPerk(perk);
}

} // namespace ft::game
