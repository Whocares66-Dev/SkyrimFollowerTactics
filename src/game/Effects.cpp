#include "game/Effects.h"

#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"

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

std::vector<ft::PotionStock::Effect> ConsumableEffects(const RE::Actor *actor, RE::MagicItem *item,
                                                       ft::ConsumableKind kind)
{
    if (!item)
        return {};
    std::vector<ft::ConsumableEffectSeen> seen;
    for (auto *effect : ResolvedEffects(*item))
    {
        const auto *base = effect->baseEffect;
        const char *name = base->GetFullName();
        seen.push_back({name ? name : "", effect->effectItem.magnitude, static_cast<float>(effect->effectItem.duration),
                        ShapeOf(base, static_cast<float>(effect->effectItem.duration))});
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
    std::sort(out.begin(), out.end(),
              [](const ConsumableOption &a, const ConsumableOption &b) { return a.name < b.name; });
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
// lasting; else the ability it has been seen to turn on, a toggle's.
const RE::MagicItem *LastingOrToggled(const RE::MagicItem *item)
{
    if (!item || LastingEffect(item))
        return item;
    const auto *spell = item->As<RE::SpellItem>();
    return spell ? ToggledAbility(spell) : nullptr;
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

std::vector<ft::EffectPick> ScanEffectPicks(const std::vector<SpellOption> &spells,
                                            const std::vector<ConsumableOption> &consumables)
{
    std::vector<ft::EffectPick> candidates;
    const auto add = [&](const RE::MagicItem *item) {
        const RE::Effect *effect = LastingEffect(LastingOrToggled(item));
        // A summon is the Summon condition's: Black Market's merchant,
        // Conjure Familiar. A hidden effect never counts (ReadTraits).
        if (!effect || effect->baseEffect->HasArchetype(RE::EffectSetting::Archetype::kSummonCreature) ||
            effect->baseEffect->data.flags.any(EffectFlag::kHideInUI))
            return;
        candidates.push_back({NameOf(effect->baseEffect), effect->baseEffect->GetFormID()});
    };
    for (const auto &option : consumables)
    {
        if (option.kind == ft::ConsumableKind::Potion || option.kind == ft::ConsumableKind::Food)
            add(RE::TESForm::LookupByID<RE::AlchemyItem>(option.form));
    }
    // A spell, a scroll or a shout cast on oneself: an aimed one leaves its
    // effect on the target, and a drain's share on the caster is not what
    // it is cast for. A scroll is its spell's pick.
    for (const auto &option : spells)
    {
        if ((option.kind == SpellOption::Kind::Spell || option.kind == SpellOption::Kind::Scroll) && option.selfOnly)
            add(RE::TESForm::LookupByID<RE::MagicItem>(option.form));
        else if (option.kind == SpellOption::Kind::Power)
            add(RE::TESForm::LookupByID<RE::SpellItem>(option.form));
        else if (option.kind == SpellOption::Kind::Shout && option.selfOnly)
        {
            // The word a Shout action shouts: the highest unlocked.
            const auto *shout = RE::TESForm::LookupByID<RE::TESShout>(option.form);
            const int word = shout ? HighestUnlockedWord(shout) : -1;
            if (word >= 0)
                add(shout->variations[word].spell);
        }
    }
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
