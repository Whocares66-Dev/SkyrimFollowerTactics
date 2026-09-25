#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"

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

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace ft::game
{
namespace
{

// MagicNoReanimate, Skyrim.esm: the keyword the Reanimate archetype's one
// condition refuses.
constexpr std::uint32_t kMagicNoReanimateKeyword = 0x0006F6FB;

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
bool ReadsSkillMods(const RE::Actor *actor);
bool ReadsSkillPowerMods(const RE::Actor *actor);
ft::EffectShape ShapeOf(const RE::EffectSetting *base, float duration);
bool EffectApplies(const RE::Actor *actor, const RE::EffectSetting *base);

// Every effect a consumable gives, by the name the game shows, with the
// item's magnitude and duration of each, marked harmful or not and judged a
// buff or not. EVERY one, the bane beside the boon: which of them a rule may
// choose the item by is policy, and it lives in core where it is tested
// (PotionStock::ChoosableBy). A potion's harmful side -- the Slow in Sleeping
// Tree Sap, the regeneration damage in a wine -- is no reason to drink it,
// and a poison is chosen for what it does to the enemy; core says so, not
// this. An ingredient eaten gives its FIRST effect and no other (the rest
// are for the alchemy table), so that one is the ingredient's effect.
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

// VendorItemFood, Skyrim.esm: the keyword on the few ingredients that are
// food -- a charred skeever hide, an egg, snowberries. Any other
// ingredient is eaten only to learn what it does, and a follower has
// nothing to learn.
constexpr std::uint32_t kVendorItemFoodKeyword = 0x0008CDEA;

// Which consumable kind an inventory object is, or nothing for what is
// neither eaten nor applied.
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

// Defined further down, in this same unnamed namespace, with the sheets.
bool ReadsSkillMods(const RE::Actor *actor);
bool ReadsSkillPowerMods(const RE::Actor *actor);

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

bool EffectApplies(const RE::Actor *actor, const RE::EffectSetting *base)
{
    return ft::EffectApplies(ShapeOf(base, 0.0f), ReadsSkillMods(actor), ReadsSkillPowerMods(actor));
}

} // namespace

Carried CarriedOf(RE::Actor *actor, RE::TESBoundObject *object)
{
    Carried out;
    if (!actor || !object)
        return out;
    auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
    const auto found = inventory.find(object);
    if (found == inventory.end())
        return out;
    out.count = found->second.first;
    out.entry = std::move(found->second.second);
    return out;
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

std::optional<SkillGate> FirstSkillGate(RE::Actor *actor, const RE::MagicItem *spell)
{
    auto *owner = actor && spell ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return std::nullopt;
    // An effect of no school (a power's, an ability's) has no skill to ask
    // about -- its skill is kNone, and asking for that actor value must not
    // happen: the engine's own getter shrugs it off, but
    // ActorValueExtension's hook of it indexes a table with the number and
    // crashes (Nordic Souls, 2026-09-08, on Serana).
    for (const auto *effect : ResolvedEffects(*spell))
    {
        const auto school = effect->baseEffect->GetMagickSkill();
        if (school == RE::ActorValue::kNone)
            continue;
        const auto level = static_cast<int>(effect->baseEffect->GetMinimumSkillLevel());
        if (const float has = owner->GetActorValue(school); static_cast<float>(level) > has)
            return SkillGate{school, level, has};
    }
    return std::nullopt;
}

bool AboveSkillForAI(RE::Actor *actor, const RE::MagicItem *spell)
{
    return FirstSkillGate(actor, spell).has_value();
}

float VoiceRecoveryOf(RE::Actor *actor)
{
    const float recovery = actor ? actor->GetVoiceRecoveryTime() : 0.0f;
    return recovery > 0.0f && recovery < 3600.0f ? recovery : 0.0f;
}

// Whom an actor is fighting, as the engine sees it, if they are still
// alive: the dead are nobody's target.
ft::ActorId LiveTargetOf(RE::Actor *actor)
{
    auto target = actor ? actor->GetActorRuntimeData().currentCombatTarget.get() : nullptr;
    return target && !target->IsDead() ? target->GetFormID() : 0;
}

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

// Hands each spell of the engine's walk to `fn` once: the same spell can be
// in the record's list and among the added spells, and every caller wants
// it once.
class OnceEach final : public RE::Actor::ForEachSpellVisitor
{
  public:
    explicit OnceEach(const std::function<void(RE::SpellItem *)> &fn) : fn_(fn)
    {
    }

    RE::BSContainer::ForEachResult Visit(RE::SpellItem *spell) override
    {
        if (spell && seen_.insert(spell).second)
            fn_(spell);
        return RE::BSContainer::ForEachResult::kContinue;
    }

  private:
    const std::function<void(RE::SpellItem *)> &fn_;
    std::unordered_set<const RE::SpellItem *> seen_;
};

} // namespace

void ForEachSpell(RE::Actor *actor, const std::function<void(RE::SpellItem *)> &fn)
{
    if (!actor)
        return;
    OnceEach once(fn);
    actor->VisitSpells(once);
}

bool IsCastable(const RE::SpellItem *spell)
{
    return spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell;
}

bool IsPower(const RE::SpellItem *spell)
{
    if (!spell)
        return false;
    const auto type = spell->GetSpellType();
    return type == RE::MagicSystem::SpellType::kPower || type == RE::MagicSystem::SpellType::kLesserPower ||
           IsLeasedPower(spell->GetFormID());
}

// Whether the actor can dual cast a spell. The one statement of the rule; the
// menu, the evaluator and the docs point here. A record that fits either
// hand (SpellGrip): the slot decides, never the level, so a mod's one-handed
// master spell is in; a package told to dual cast a one-hand variant fired it
// from that hand alone (Serana's Ice Storm, DLC1IceStormRightHand,
// 2026-09-16). And, where Settings asks for it (dev/PROFILES.md), the perk
// system's answer to the Can Dual Cast Spell entry point, so a mod's perk
// counts the same as the school's Dual Casting perk.
bool CanDualCast(RE::Actor *actor, RE::SpellItem *spell)
{
    if (!actor || !spell || !IsCastable(spell) || SpellGrip(spell) != ft::Grip::Either)
        return false;
    // The setting is about followers, whose package forces a dual cast the
    // perk or no. The player's own handler asks this entry point before it
    // pairs two presses into a dual cast (dev/PLAYER.md "The pairing"),
    // and two presses it does not pair are two single casts: so for the
    // player the perk is asked whatever the setting says.
    if (!CurrentSettings().requireDualCastPerks && !actor->IsPlayerRef())
        return true;
    float allowed = 0.0f;
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kCanDualCastSpell, actor, spell, &allowed);
    return allowed != 0.0f;
}

// What a dual cast costs the actor: the cost times fMagicDualCastingCostMult
// (2.8 in vanilla), unless the spell is flagged to take no dual-cast change.
float DualCastCost(RE::Actor *actor, RE::SpellItem *spell)
{
    const float cost = spell->CalculateMagickaCost(actor);
    return spell->GetNoDualCastModifications() ? cost : cost * GameSetting("fMagicDualCastingCostMult", 2.8f);
}

// "3 min 24 s", "1 h 5 min", "12 s"; nothing for an effect with no
// duration, an ability's or an enchantment's.
std::string RemainingText(float seconds)
{
    if (seconds < 0.0f)
        return {};
    const int total = static_cast<int>(std::lround(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0)
        return TrFormat("{} h {} min", hours, minutes);
    if (minutes > 0)
        return TrFormat("{} min {} s", minutes, secs);
    return TrFormat("{} s", secs);
}

// The worn item carrying this enchantment, by the name the game shows for
// it, or empty if none is worn.
// The worn item an enchantment's effect comes from, by name -- the name
// given at the table, else the record's. `from` is the item the active
// effect itself names (ActiveEffect::source), and is asked for first: two
// pieces enchanted alike at a table share one enchantment form, and
// searching by the form named the ring for the necklace's effect too
// (2026-09-11, a Silver Ruby Ring listed twice). Without it, the first
// worn item carrying the form.
struct WornSource
{
    std::uint32_t form{0};
    std::string name;
};

// Is `worn`, the enchantment an item carries, the one an effect came from?
// The same form, or one made at a table from the other as its template:
// a player's enchantment is a form of its own with the template as its
// base, and which of the two an active effect names is not the same for
// every item (Remiel's Silver Ruby Necklace was named right and linked
// nowhere while the ring beside it did both, 2026-09-11).
bool SameEnchantment(const RE::MagicItem *worn, const RE::MagicItem *magic)
{
    if (!worn || !magic)
        return false;
    if (worn == magic)
        return true;
    const auto *a = worn->As<RE::EnchantmentItem>();
    const auto *b = magic->As<RE::EnchantmentItem>();
    return (a && a->data.baseEnchantment == magic) || (b && b->data.baseEnchantment == worn);
}

WornSource WornSourceOf(RE::Actor *actor, const RE::MagicItem *magic, const RE::TESBoundObject *from)
{
    for (const auto &[object, entry] : actor->GetInventory())
    {
        if (!object || entry.first <= 0 || !entry.second || !entry.second->IsWorn())
            continue;
        if (from ? object != from : !SameEnchantment(entry.second->GetEnchantment(), magic))
            continue;
        const char *given = entry.second->GetDisplayName();
        if (given && *given)
            return {object->GetFormID(), given};
        return {object->GetFormID(), NameOr(object, "")};
    }
    if (from)
        return WornSourceOf(actor, magic, nullptr);
    log::sensors.debug("{} no worn item found for enchantment {} ({:08X}) -- its effect is named for the enchantment",
                       Describe(actor), log::NameOf(magic), magic ? magic->GetFormID() : 0);
    return {};
}

// The effect's description with <mag> and <dur> filled in. Skyrim.esm
// writes the tokens in lower case; mods are not so consistent, and the
// engine takes either.
std::string EffectDescription(const RE::EffectSetting *base, float magnitude, float duration)
{
    const char *text = base->magicItemDescription.c_str();
    std::string line = text ? text : "";
    const auto replace = [&line](std::string_view token, const std::string &with) {
        const auto same = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
        auto at = std::search(line.begin(), line.end(), token.begin(), token.end(), same);
        while (at != line.end())
        {
            const auto index = static_cast<std::size_t>(at - line.begin());
            line.replace(index, token.size(), with);
            at = std::search(line.begin() + static_cast<std::ptrdiff_t>(index + with.size()), line.end(), token.begin(),
                             token.end(), same);
        }
    };
    char num[32];
    std::snprintf(num, sizeof(num), "%.0f", magnitude);
    replace("<mag>", num);
    std::snprintf(num, sizeof(num), "%.0f", duration);
    replace("<dur>", num);
    return line;
}

// What a running effect is called by on the sheets: the worn item carrying
// an enchantment, else the spell or potion.
std::string SourceName(RE::Actor *actor, const RE::ActiveEffect *ae)
{
    std::string source;
    if (!ae->spell)
        return source;
    if (ae->spell->As<RE::EnchantmentItem>())
        source = WornSourceOf(actor, ae->spell, ae->source).name;
    if (source.empty() && ae->spell->GetName())
        source = ae->spell->GetName();
    return source;
}

bool MovesValue(const RE::EffectSetting *base); // below, with the effect rows

// Whether a running effect moves this actor value: a value modifier on
// it, or a dual modifier with it as either half.
bool ModifiesValue(const RE::ActiveEffect &ae, RE::ActorValue value)
{
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    const auto *base = ae.effect->baseEffect;
    const auto archetype = base->GetArchetype();
    if (!MovesValue(base))
        return false;
    const bool dual = archetype == Archetype::kDualValueModifier || archetype == Archetype::kEnhanceWeapon;
    const bool primary = base->data.primaryAV == value;
    const bool secondary = dual && base->data.secondaryAV == value;
    return primary || secondary;
}

namespace
{
bool HeldPerkGrants(RE::Actor *actor, const RE::MagicItem *spell); // below, with the perks

bool Hidden(const RE::EffectSetting *base)
{
    return base && base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
}

// A corpse-raising effect, by its archetype rather than its name, so a mod's
// own Reanimate counts. Asked of a resolved effect.
bool IsReanimate(const RE::Effect *effect)
{
    return effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate;
}

// Every effect on the spell hidden from the player's active effects. Never
// an enchantment's: that is named for its item, which the player sees.
bool AllHidden(const RE::MagicItem *spell)
{
    if (!spell || spell->As<RE::EnchantmentItem>() || spell->effects.empty())
        return false;
    // Not ResolvedEffects: an effect with no base is not known to be hidden,
    // so it must make the answer "no", and the view would skip it.
    for (const RE::Effect *effect : spell->effects)
    {
        if (!effect || !Hidden(effect->baseEffect))
            return false;
    }
    return true;
}
} // namespace

std::vector<Contribution> Contributions(RE::Actor *actor, RE::ActorValue value)
{
    std::vector<Contribution> out;
    // A controller -- a spell whose every effect is hidden, there for its
    // arithmetic and never seen in play -- is not a source by name: "Attack
    // Speed Controller" says nothing, and its amount is left to Other. Hidden
    // is not unseen, though. Mundus splits the Apprentice Stone into two
    // abilities of that name, the Magicka Rate on one and the hidden
    // resistance penalties on the other, and the penalties read as Other
    // (2026-09-14). So a hidden-only spell is named where a visible running
    // effect carries the same spell name, which the player does see, or
    // where a perk the actor holds grants it under that name, a perk being
    // seen on its tree: Stormcrown's Windcaller perk grants a hidden
    // Windcaller ability, which read as Other (2026-09-15). Gathered in the
    // one walk and settled after it.
    std::unordered_set<std::string> seen;
    struct Pending
    {
        std::string name;
        const RE::MagicItem *spell;
        Contribution contribution;
    };
    std::vector<Pending> hidden;
    ForEachActiveEffect(actor, [&](RE::ActiveEffect &effect) {
        auto *ae = &effect;
        const char *spellName = ae->spell ? ae->spell->GetName() : nullptr;
        if (spellName && *spellName && !Hidden(ae->effect->baseEffect))
            seen.insert(spellName);
        if (!ModifiesValue(effect, value))
            return;
        const auto *base = ae->effect->baseEffect;
        std::string source = SourceName(actor, ae);
        if (source.empty())
            source = NameOr(base, "?");
        // The active effect's magnitude already carries the engine's sign:
        // a detrimental modifier (Weakness to Fire on a vampire) is -50
        // here, not 50 with a flag to read. Negating it again showed the
        // weakness as +50% beside a total that had subtracted it
        // (2026-09-08).
        // An effect on the value with nothing to add (a vampire's Blood
        // Aura carries a zero here) is not a source; one too small to print, below half a hundredth, is noise.
        if (std::abs(ae->magnitude) < 0.005f)
            return;
        // A dual effect's second value takes the magnitude times the
        // record's weight: the Creation Kit's definition, not read off the
        // executable.
        const bool first = base->data.primaryAV == value;
        Contribution c{std::move(source), NameOr(base, ""),
                       first ? ae->magnitude : ae->magnitude * base->data.secondAVWeight};
        c.recovers = base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover);
        if (AllHidden(ae->spell))
            hidden.push_back({spellName ? spellName : "", ae->spell, std::move(c)});
        else
            out.push_back(std::move(c));
    });
    for (Pending &pending : hidden)
    {
        if (!pending.name.empty() && (seen.contains(pending.name) || HeldPerkGrants(actor, pending.spell)))
            out.push_back(std::move(pending.contribution));
    }
    // Smallest first: the weaknesses, then the boons, the largest last.
    std::stable_sort(out.begin(), out.end(),
                     [](const Contribution &a, const Contribution &b) { return a.amount < b.amount; });
    return out;
}

float DamageReduction(RE::Actor *actor); // below, with the armour readings
float HiddenArmor(RE::Actor *actor);
float ArmorValue(RE::Actor *actor);
float EffectiveArmor(RE::Actor *actor);

ft::Breakdown ArmorBreakdown(RE::Actor *actor)
{
    // Each piece worn with its rating as the follower wears it, then the
    // spells and enchantments on the armour value itself (Oakflesh, a
    // Fortify Armor), smallest first as the resistances list theirs.
    ft::Breakdown b;
    if (!actor)
        return b;
    std::vector<Contribution> parts;
    auto inventory = actor->GetInventory([](RE::TESBoundObject &o) { return o.Is(RE::FormType::Armor); });
    for (auto &[object, slot] : inventory)
    {
        auto *entry = slot.second.get();
        auto *armor = object ? object->As<RE::TESObjectARMO>() : nullptr;
        if (!armor || !entry || !entry->IsWorn())
            continue;
        const float rating = ArmorRating(actor, armor, entry);
        if (rating <= 0.0f)
            continue;
        const char *name = entry->GetDisplayName() ? entry->GetDisplayName() : armor->GetName();
        parts.push_back({name ? name : "?", {}, rating});
    }
    // Named for the effect as well as its source: the source of a Fortify
    // Armor enchantment is the piece, which is already a line above, and
    // a helmet listed twice at two figures read as a mistake (2026-09-11).
    for (Contribution &c : Contributions(actor, RE::ActorValue::kDamageResist))
    {
        if (!c.effect.empty() && c.effect != c.source)
            c.source = TrFormat("{} ({})", c.source, c.effect);
        parts.push_back(std::move(c));
    }
    // And the engine's hidden bonus per piece worn (fArmorBaseFactor, 0.03
    // of a blow each), in the rating's own units -- 25 a piece at the
    // vanilla settings, the "25 armour per piece" of the wikis. The list
    // sums to the row's number, EffectiveArmor.
    const float perPiece = GameSetting("fArmorBaseFactor", 0.03f);
    const float hidden = HiddenArmor(actor);
    if (hidden > 0.0f && perPiece > 0.0f)
    {
        const int pieces = static_cast<int>(std::lround(actor->GetArmorBaseFactorSum() / perPiece));
        parts.push_back({TrFormat("Hidden bonus (x{})", pieces), {}, hidden});
    }
    // Whatever the engine's figure has that the pieces, the effects and
    // the bonus do not (a formula mod, a rounding) is the Other line, so
    // the list sums to the row and a gap is seen rather than hidden.
    AddSourceLines(b, std::move(parts));
    b.total = EffectiveArmor(actor);
    ft::Close(b);
    return b;
}

// fArmorScalingFactor over 100: what a point of armour rating turns away.
float ArmorScale()
{
    const float scale = GameSetting("fArmorScalingFactor", 0.12f) / 100.0f;
    return scale;
}

float HiddenArmor(RE::Actor *actor)
{
    const float scale = ArmorScale();
    return actor && scale > 0.0f ? actor->GetArmorBaseFactorSum() / scale : 0.0f;
}

float ArmorValue(RE::Actor *actor)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    return owner ? owner->GetActorValue(RE::ActorValue::kDamageResist) : 0.0f;
}

float EffectiveArmor(RE::Actor *actor)
{
    return actor ? ArmorValue(actor) + HiddenArmor(actor) : 0.0f;
}

void AddSourceLines(ft::Breakdown &b, std::vector<Contribution> sources)
{
    std::stable_sort(sources.begin(), sources.end(),
                     [](const Contribution &a, const Contribution &b) { return a.amount < b.amount; });
    for (Contribution &c : sources)
        ft::Add(b, std::move(c.source), c.amount);
}

ValueParts PartsOf(RE::Actor *actor, RE::ActorValue value)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    return {owner ? owner->GetBaseActorValue(value) : 0.0f, Contributions(actor, value)};
}

void AddValueLines(ft::Breakdown &b, const ValueParts &parts, float scale)
{
    if (parts.base != 0.0f)
    {
        if (b.lines.empty())
            ft::Start(b, Tr("Base"), parts.base * scale);
        else
            ft::Add(b, Tr("Base"), parts.base * scale);
    }
    for (const Contribution &c : parts.sources)
        ft::Add(b, c.source, c.amount * scale);
}

namespace
{
// A value a line reads, opened out beneath it. In the value's own units,
// not the breakdown's: a multiplier beneath a recovery time read "+0.2 s".
// `scale` turns the value's units into the line's where they differ: a
// Magicka Rate Mult of 200 is the factor x 2, its terms 1 and 0.5, and
// `reading` is then in the line's units. Empty for a base alone, which is
// the line's own figure again.
std::vector<ft::BreakdownLine> ValueLines(RE::Actor *actor, RE::ActorValue value, float reading, float scale = 1.0f)
{
    ft::Breakdown b;
    AddValueLines(b, PartsOf(actor, value), scale);
    b.total = reading;
    ft::Close(b);
    if (b.lines.size() == 1 && b.lines.front().op == ft::Op::Start)
        return {};
    for (ft::BreakdownLine &line : b.lines)
        line.unit = std::string{};
    return std::move(b.lines);
}
} // namespace

ft::Breakdown ValueBreakdown(RE::Actor *actor, RE::ActorValue value, const char *unit)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return {};
    // A pool's maximum is the permanent value plus what effects add for
    // now; the damage taken is below it and is not a source. Every other
    // value is what it reads. An effect that does not recover moves the pool
    // every second and never its maximum, so it is no source of one: listed,
    // Mutagen's regeneration read +8.8 Health against an Other of -8.8.
    const bool pool =
        value == RE::ActorValue::kHealth || value == RE::ActorValue::kMagicka || value == RE::ActorValue::kStamina;
    ValueParts parts = PartsOf(actor, value);
    if (pool)
        std::erase_if(parts.sources, [](const Contribution &c) { return !c.recovers; });
    ft::Breakdown b;
    b.unit = unit;
    AddValueLines(b, parts);
    b.total = pool ? owner->GetPermanentActorValue(value) +
                         actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, value)
                   : owner->GetActorValue(value);
    // An effect listed but not yet in the value is Other too: an enchanted
    // piece equipped from the panel while the clock is frozen is listed at
    // once, and the value moves on the actor's next update (2026-09-11).
    ft::Close(b);
    return b;
}

ft::Breakdown CarryWeightBreakdown(RE::Actor *actor)
{
    // The limit is not the value: GetTotalCarryWeight (id 37452, read
    // 2026-09-13) puts it through the Get Max Carry Weight entry point, and
    // a mod's "carry weight x10" perk there left the sheet at the value's
    // 400.
    ft::Breakdown b = ValueBreakdown(actor, RE::ActorValue::kCarryWeight, "");
    if (!actor)
        return b;
    AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kGetMaxCarryWeight, {});
    b.total = actor->GetTotalCarryWeight();
    ft::Close(b);
    return b;
}

// The conditions of one list, a row each: the call, the comparison, and a
// tick where it holds for the parties. `on` names the entry's argument the
// tab is on, for a perk's tab other than the first (the owner): those are
// listed, not evaluated.
std::vector<SheetRow> ConditionRows(const RE::TESCondition &condition, const ConditionParties &parties,
                                    const char *on = nullptr); // below, with the perks
// The value's display name, where the game has one; else the Creation
// Kit's, read as words. Below, with the perk entry points.
std::string ValueName(RE::ActorValue value);

// Does the effect move an actor value: the kinds the Character sheet's
// notes list by source.
bool MovesValue(const RE::EffectSetting *base)
{
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    const auto archetype = base->GetArchetype();
    // Enhance Weapon is a dual value modifier underneath (its active effect
    // derives from DualValueModifierEffect in CommonLibSSE), and vanilla's
    // Elemental Fury is one, on Weapon Speed Mult.
    return archetype == Archetype::kValueModifier || archetype == Archetype::kPeakValueModifier ||
           archetype == Archetype::kDualValueModifier || archetype == Archetype::kEnhanceWeapon;
}

// One effect of a spell, an enchantment or a potion as a row, as a perk's
// entry is (EntryRow): the value it moves on the left, else the kind of
// effect and what it names; the magnitude on the right, signed as the
// engine applies it, with how long it runs; a tick in the third column
// where the game's own list would not show it; and greyed, with the
// reason on the name, where its conditions do not hold for this actor,
// which open beneath. The description is the author's prose and says
// what they meant; this is the record, and says what it does. The two
// parted on a Breton's Spell Warding, whose text promises an absorb
// chance that a second, hidden effect grants the player alone
// (2026-09-11). `magnitude` is SIGNED: negative for what an effect takes
// away, as an active effect's own magnitude is, and as EffectsOf reads a
// record's (Spellbreaker's -5 Stamina read as +5 once, the sign applied
// twice).
SheetRow EffectEntryRow(const RE::Effect &effect, float magnitude, const ConditionParties &parties)
{
    const auto *base = effect.baseEffect;
    const auto valueName = ValueName;

    // The effect by the name the game gives it -- Scourge, Spell Warding,
    // Fortify Health -- and beside it the amount with the value it moves:
    // "-3 Health", "+25 Resist Magic". A scroll in Nordic Souls carries
    // its perk bonuses as extra hidden entries of one value, and the name
    // is what tells them apart (2026-09-11).
    const char *called = base->GetName();
    std::string name = called && *called ? called : TypeWord(base);
    std::string amount;
    const auto duration = effect.effectItem.duration;
    if (MovesValue(base))
    {
        // A dual-value effect moves its second value by the magnitude
        // weighted.
        amount = Fmt("%+g", magnitude) + " " + valueName(base->data.primaryAV);
        if (base->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kDualValueModifier &&
            base->data.secondaryAV != RE::ActorValue::kNone)
        {
            // Two values, one per line. No dash before them: beside a
            // signed number it read as a minus.
            amount +=
                "\n" + Fmt("%+g", magnitude * base->data.secondAVWeight) + " " + valueName(base->data.secondaryAV);
        }
        // The Recover flag is what says "per second". Set, the modifier
        // moves the value once and puts it back when the effect expires:
        // a fortify, "+100 Magicka" for an hour. Clear, it moves the value
        // every second and leaves it there: a heal or a poison, "10 points
        // per second for 5 seconds". A guess from the value alone read
        // Nordic Souls' Bard Song, a hidden Peak Value Modifier of Magicka
        // with Recover set, as "+100 Magicka/s" (2026-09-13).
        const bool recovers = base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover);
        if (duration > 0 && !recovers)
            amount += Tr("/s");
    }
    else
    {
        // The kind, and what it names: the creature summoned, the weapon
        // bound.
        amount = TypeWord(base);
        if (const auto *named = base->data.associatedForm; named && named->GetName() && *named->GetName())
            amount += std::string(" ") + named->GetName();
        if (magnitude != 0.0f)
            amount += " " + Fmt("%g", magnitude);
    }

    SheetRow row = Row(name, amount);
    // The duration in its own column; blank for an effect with none,
    // which holds for as long as it runs.
    if (duration > 0)
        row.extra = TrFormat("{} s", duration);
    // The author's text with the numbers put in, the magnitude unsigned as
    // the text expects it ("Deals <mag> points"); empty where the record
    // has none, and then the table has no column for it.
    row.description = EffectDescription(base, std::abs(magnitude), static_cast<float>(duration));
    if (base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
        row.mark = kGlyphTick;
    // Two lists gate it: the spell's own entry's, and the effect record's
    // -- a Breton's hidden effects are gated on the record, a Nordic Souls
    // scroll's on the entry -- and both must hold. No verdict where a
    // condition is N/A, whose answer would be the engine's false for the
    // party nobody could name, or ?, whose answer is not to be had; nor with no Subject at all, which
    // the engine never passes to a list (a perk's tab without its argument
    // is not asked, ID 23800), so what it would say is unread.
    const std::array<const RE::TESCondition *, 2> lists{&effect.conditions, &base->conditions};
    bool asked = parties.subject != nullptr;
    for (const RE::TESCondition *conditions : lists)
    {
        if (!conditions->head)
            continue;
        for (auto &condition : ConditionRows(*conditions, parties))
        {
            asked = asked && condition.extra.empty();
            row.detail.push_back(std::move(condition));
        }
    }
    bool holds = true;
    for (const RE::TESCondition *conditions : lists)
        if (asked && conditions->head)
            holds = holds && conditions->IsTrue(parties.subject, parties.target);
    if (!holds)
        row.aside = Tr("Conditions not met");
    return row;
}

SheetSection EffectsOf(RE::Actor *actor, const RE::MagicItem *magic,
                       const std::function<float(const RE::Effect *)> &magnitude)
{
    SheetSection section{Tr("Effects"), {}, {}};
    if (!magic)
        return section;
    // Whom the conditions are asked of, as when the item lands: the one it
    // lands on as Subject, the actor using it as Target. A Self spell, a
    // potion, food, an ingredient and a worn enchantment land on the actor.
    // A poison reports Self as every potion does and lands on whoever the
    // blade strikes, as a weapon's enchantment and an aimed spell land on
    // whoever they hit: for a hostile effect, the enemy the actor is
    // fighting while there is one; for anything else nobody the page can
    // name.
    const bool onSelf = magic->GetDelivery() == RE::MagicSystem::Delivery::kSelf && !magic->IsPoison();
    const auto fighting = !onSelf && actor && actor->IsInCombat()
                              ? actor->GetActorRuntimeData().currentCombatTarget.get()
                              : RE::NiPointer<RE::Actor>{};
    RE::Actor *enemy = fighting && !fighting->IsDead() ? fighting.get() : nullptr;
    // Whose skill each effect's level is asked of: a follower's, for a
    // spell, which their combat AI never has while one effect's level is
    // above their skill (AboveSkillForAI). The player casts at any skill,
    // and a scroll, a potion or an enchantment asks none.
    const auto *spell = magic->As<RE::SpellItem>();
    auto *gated = actor && !actor->IsPlayerRef() && spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell
                      ? actor->AsActorValueOwner()
                      : nullptr;
    for (const auto *effect : ResolvedEffects(*magic))
    {
        ConditionParties parties{actor, actor};
        if (!onSelf)
        {
            const bool hostile = magic->IsPoison() || effect->baseEffect->IsHostile();
            parties.subject = hostile ? enemy : nullptr;
        }
        // The record's magnitude is unsigned; a detrimental effect takes
        // it away.
        const float amount = magnitude(effect);
        SheetRow row = EffectEntryRow(*effect, effect->baseEffect->IsDetrimental() ? -amount : amount, parties);
        if (const auto school = effect->baseEffect->GetMagickSkill(); school != RE::ActorValue::kNone)
        {
            row.school = ValueName(school);
            // A level only where it gates anything: a spell's. A staff, an
            // enchantment, a potion or a scroll asks no skill (45328 runs
            // only as the engine lists spells).
            if (const auto level = effect->baseEffect->GetMinimumSkillLevel();
                level > 0 && spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell)
            {
                row.level = std::to_string(level);
                if (const float skill = gated ? gated->GetActorValue(school) : 0.0f;
                    gated && skill < static_cast<float>(level))
                {
                    row.needsLevel = static_cast<int>(level);
                    row.hasLevel = static_cast<int>(skill);
                }
            }
        }
        section.rows.push_back(std::move(row));
    }
    return section;
}

std::vector<EffectRow> ScanActiveEffects(RE::Actor *actor)
{
    std::vector<EffectRow> out;
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    auto *effects = target ? target->GetActiveEffectList() : nullptr;
    if (!effects)
        return out;

    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        const auto *base = ae->effect->baseEffect;
        // As the game's own Active Effects list: hidden ones stay hidden,
        // and one that has run out is gone -- except a hidden effect that
        // moves a value, which the Character sheet's notes name by source
        // and which therefore wants a page: kept, marked hidden, for the
        // tab to list when the player asks for hidden effects, running and
        // applied as it is, its page saying it is hidden. A hidden
        // effect that moves nothing -- a script's, a race monitor's, a
        // cloak's -- stays off the list, as there would be many and
        // nothing to show for them.
        const bool hidden = base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
        if (hidden && (!MovesValue(base) || std::abs(ae->magnitude) < 0.05f))
            continue;
        if (ae->duration > 0.0f && ae->elapsedSeconds >= ae->duration)
            continue;
        const char *name = base->GetName();
        if (!name || !*name)
            continue;

        EffectRow row;
        row.token = ae;
        row.form = base->GetFormID();
        row.hidden = hidden;
        row.applied = EffectApplies(actor, base);
        // Running but not acting, by the engine's flag, which the sheet's
        // totals read too (ForEachActiveEffect). Not by asking the
        // conditions: the engine asks an effect record's once, when the
        // effect lands, and Adamant's Bastion asks there whether the cast
        // was dual, which reads false ever after (dev/CONDITIONS.md 10).
        row.active = !ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled);
        row.name = name;
        row.magnitude = ae->magnitude;
        row.duration = ae->duration;
        row.remaining = ae->duration > 0.0f ? ae->duration - ae->elapsedSeconds : -1.0f;
        row.remainingText = RemainingText(row.remaining);
        // The source's name, and what it links to where it has a page: the
        // worn item behind an enchantment -- the one the lookup named, so
        // the link and the name cannot part -- else the spell.
        if (ae->spell)
        {
            if (ae->spell->As<RE::EnchantmentItem>())
            {
                const WornSource worn = WornSourceOf(actor, ae->spell, ae->source);
                row.source = worn.name;
                row.linkForm = worn.form;
            }
            else
                row.linkForm = ae->spell->GetFormID();
            if (row.source.empty() && ae->spell->GetName())
                row.source = ae->spell->GetName();
        }

        // The page: one row, in the table the item page lists its effects
        // in, with the source as a last column -- what THIS effect does,
        // for how long, whether the game's list hides it, where it comes
        // from, its conditions beneath. Its source's other effects are the
        // source's business, on the item's or the spell's own page. The
        // active effect's magnitude is the engine's, signed already; its
        // duration reads as what is left of what there was.
        {
            SheetSection page{Tr("Effect"), {}, {}};
            // Its conditions as the engine asks them of a running effect:
            // of the one it is on, and of whoever cast it -- the player,
            // for a Bastion Dragonhide on a follower. For reference: the
            // row's grey is the flag.
            const auto caster = ae->caster.get();
            SheetRow line = EffectEntryRow(*ae->effect, ae->magnitude, {actor, caster.get()});
            line.aside = row.active ? "" : Tr("Inactive");
            if (ae->duration > 0.0f)
            {
                line.extra = RemainingText(ae->duration);
                line.remaining = row.remainingText;
            }
            line.link = row.source;
            // Whoever cast it, when it was not the follower: the player's
            // Courage, an enemy's Fury.
            if (caster && caster.get() != actor && caster->GetName() && *caster->GetName())
                line.link = TrFormat("{} ({})", line.link, caster->GetName());
            line.form = row.linkForm;
            page.rows.push_back(std::move(line));
            row.detail.push_back(std::move(page));
        }
        row.description = EffectDescription(base, row.magnitude, row.duration);
        // An ability's text lives on the spell, not its effect: Imperial
        // Luck's effect record says nothing, the ability says "find more
        // gold". The spell's own description, as the Magic tab reads it.
        if (row.description.empty())
        {
            if (auto *spell = ae->spell ? ae->spell->As<RE::SpellItem>() : nullptr)
            {
                RE::BSString text;
                spell->GetDescription(text, spell);
                row.description = text.c_str() ? text.c_str() : "";
            }
        }

        out.push_back(std::move(row));
    }

    std::sort(out.begin(), out.end(),
              [](const EffectRow &a, const EffectRow &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

// What an actor is in the middle of, as dev/CONDITIONS.md 2 reads it: the
// hostile effects running on them by the kind of damage, the poison and
// the disease by their spell type, the paralysis and the rest by the
// actor's own flags. One walk of the effect list, a handful of flag reads.
// The share of a blow the actor's armour turns away, from the engine's own
// two numbers rather than a recount of the slots: the DamageResist actor
// value is the rating as the engine applies it -- the pieces worn, with
// tempering, skill and perks, plus every effect running on the value: a
// Fortify Armor Rating enchantment, a potion, a flesh spell -- and
// GetArmorBaseFactorSum the hidden bonus for the pieces worn --
// fArmorBaseFactor (0.03) per piece, which is the "25 armour per piece"
// of the wikis in the engine's own terms. Not CalcArmorRating, which is
// the pieces alone: Frea in Nordic Carved with a +100 Fortify Armor
// Rating helmet read 392.5 on the value and 292.5 there (2026-09-11), and
// the sheet's tooltip listed the enchantment against a total without it.
// The value is written when the pieces change and can trail a skill
// gained since; the effects are worth more than that. Combined as the
// vanilla damage code does: rating x fArmorScalingFactor / 100 + the
// hidden sum, capped at fMaxArmorRating.
// The combination is the one thing not read from the engine -- it is inline
// in the damage code, so a mod that hooks the FORMULA (Armor Rating
// Rescaled, Armor Rating Redux) is not reflected; one that changes the
// settings or the ratings is. The Character sheet's Armor row shows this
// number in parentheses after the rating, and ArmorReadings logs the parts
// once per actor so the semantics can be checked against the sheet in play.
float DamageReduction(RE::Actor *actor)
{
    if (!actor)
        return 0.0f;
    const float scale = ArmorScale();
    const float cap = GameSetting("fMaxArmorRating", 80.0f) / 100.0f;
    const float hidden = actor->GetArmorBaseFactorSum();
    return (std::min)(cap, (std::max)(0.0f, ArmorValue(actor) * scale + hidden));
}

// For the log, once per actor per session: the engine's armour numbers
// beside the actor value and our old slot count, so a wrong reading of
// either accessor shows up as a disagreement rather than a wrong percent.
std::unordered_set<std::uint32_t> g_armorLogged;
void LogArmorReadings(RE::Actor *actor)
{
    if (!actor || !log::Enabled(log::Level::Debug) || !g_armorLogged.insert(actor->GetFormID()).second)
        return;
    auto *owner = actor->AsActorValueOwner();
    int pieces = 0;
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    for (const Slot slot : {Slot::kBody, Slot::kHead, Slot::kHands, Slot::kFeet})
        if (actor->GetWornArmor(slot))
            ++pieces;
    const auto &runtime = actor->GetActorRuntimeData();
    log::sensors.debug("armor {}: AV DamageResist {:.1f}, CalcArmorRating {:.1f} (cached {:.1f}), base factor sum "
                       "{:.3f} (cached {:.3f}) over {} pieces x fArmorBaseFactor {:.2f} -- reduction {:.1f}%",
                       NameOr(actor, "?"), owner ? owner->GetActorValue(RE::ActorValue::kDamageResist) : 0.0f,
                       actor->CalcArmorRating(), runtime.armorRating, actor->GetArmorBaseFactorSum(),
                       runtime.armorBaseFactorSum, pieces, GameSetting("fArmorBaseFactor", 0.03f),
                       DamageReduction(actor) * 100.0f);
}

// The enchantment on the weapon in that hand: a player-made one on the
// worn copy's list (ExtraEnchantment), else the record's. The hand's own
// copy, as ChargeOf reads it: two copies of one sword enchanted
// differently are two lists, and the first found is not the one held.
RE::EnchantmentItem *EnchantmentOn(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand)
{
    if (const RE::ExtraDataList *worn = WornList(actor, weapon, hand))
        if (const auto *xEnch = worn->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment)
            return xEnch->enchantment;
    return weapon->formEnchanting;
}

// What each hand holds, in core's words, for the blow rules. A
// two-hander, a bow or a crossbow is the right hand's with the left
// described as empty: the engine reports it from both hands.
ft::Hands DescribeHands(RE::Actor *actor)
{
    const auto held = [](RE::TESForm *form) {
        if (!form)
            return ft::Held::Nothing;
        if (auto *weapon = form->As<RE::TESObjectWEAP>())
        {
            switch (weapon->GetWeaponType())
            {
            case RE::WEAPON_TYPE::kOneHandSword:
            case RE::WEAPON_TYPE::kOneHandDagger:
            case RE::WEAPON_TYPE::kOneHandAxe:
            case RE::WEAPON_TYPE::kOneHandMace:
                return ft::Held::OneHander;
            case RE::WEAPON_TYPE::kTwoHandSword:
            case RE::WEAPON_TYPE::kTwoHandAxe:
                return ft::Held::TwoHander;
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:
                return ft::Held::Bow;
            case RE::WEAPON_TYPE::kStaff:
                return ft::Held::Staff;
            default:
                return ft::Held::Nothing; // the fists' record
            }
        }
        if (auto *armor = form->As<RE::TESObjectARMO>())
            return armor->IsShield() ? ft::Held::Shield : ft::Held::Nothing;
        if (form->As<RE::TESObjectLIGH>())
            return ft::Held::Torch;
        if (form->As<RE::MagicItem>())
            return ft::Held::Spell;
        return ft::Held::Nothing;
    };
    ft::Hands hands;
    RE::TESForm *rightHeld = actor->GetEquippedObject(false);
    RE::TESForm *leftHeld = actor->GetEquippedObject(true);
    hands.right = held(rightHeld);
    hands.left = leftHeld == rightHeld && (hands.right == ft::Held::TwoHander || hands.right == ft::Held::Bow)
                     ? ft::Held::Nothing
                     : held(leftHeld);
    return hands;
}

namespace
{
// A body's radius as the engine's melee test takes it (47276, and 37868,
// whose cached result 37443 reads, on 1.6.1170): the bound max Y times the
// scale, 16 for an empty box. Not CommonLib's Actor::GetBoundRadius, which
// reads another field.
// What the reach measure reads of an actor (core/Reach.h).
ft::Body BodyOf(const RE::Actor &actor)
{
    const RE::NiPoint3 at = actor.GetPosition();
    const RE::NiPoint3 min = actor.GetBoundMin();
    const RE::NiPoint3 max = actor.GetBoundMax();
    return {at.x, at.y, at.z, min.y, max.y, min.z, max.z, actor.GetScale()};
}
} // namespace

// The engine's measure, core's (core/Reach.h, tested).
float ReachDistance(const RE::Actor *from, const RE::Actor *to)
{
    if (!from || !to)
        return (std::numeric_limits<float>::max)();
    return ft::ReachDistance(BodyOf(*from), BodyOf(*to));
}

namespace
{
// Skyrim.esm's Unarmed weapon: what the engine prices a power attack with
// when the right hand holds no weapon.
constexpr RE::FormID kUnarmedWeapon = 0x000001F4;

// The stamina multiplier of the attack an event starts: the follower's base
// record's attack data, where the engine reads it, else the race's; 1 when
// neither names the event.
float StaminaMultOf(RE::Actor *actor, const char *event)
{
    if (!actor || !event)
        return 1.0f;
    const RE::BSFixedString key(event);
    const auto lookup = [&key](const RE::BGSAttackDataForm *form) -> const RE::BGSAttackData * {
        const auto *map = form ? form->attackDataMap.get() : nullptr;
        if (!map)
            return nullptr;
        const auto it = map->attackDataMap.find(key);
        return it != map->attackDataMap.end() ? it->second.get() : nullptr;
    };
    const RE::BGSAttackData *attack = lookup(actor->GetActorBase());
    if (!attack)
        attack = lookup(actor->GetRace());
    return attack ? attack->data.staminaMult : 1.0f;
}
} // namespace

BlowPlan PlanPowerAttack(RE::Actor *actor)
{
    BlowPlan plan;
    if (!actor)
        return plan;
    RE::TESForm *rightHeld = actor->GetEquippedObject(false);
    RE::TESForm *leftHeld = actor->GetEquippedObject(true);
    auto *right = rightHeld ? rightHeld->As<RE::TESObjectWEAP>() : nullptr;
    auto *left = leftHeld ? leftHeld->As<RE::TESObjectWEAP>() : nullptr;

    // The attack, by the hands (core's rule). A swing names a hand with a
    // weapon in it, and DescribeHands read the same two objects, so the
    // checks below never fail; they are for the reader and the analyser, per
    // case because the analyser does not carry one check across a switch.
    plan.swing = ft::SwingWith(DescribeHands(actor));
    switch (plan.swing)
    {
    case ft::Swing::Both:
        if (!right || !left)
            return plan;
        break;
    case ft::Swing::Right:
        if (!right)
            return plan;
        break;
    case ft::Swing::Left:
        if (!left)
            return plan;
        break;
    case ft::Swing::Fists:
        break;
    case ft::Swing::None:
        return plan;
    }
    plan.event = ft::PowerAttackEvent(plan.swing);

    // The cost as the engine's own routine prices a power attack (26429 on
    // 1.6.1170, which the UseWeapon procedure asks too; dev/ACTIONS.md 6):
    // the RIGHT hand's weapon's weight, 1 with none there, times
    // fStaminaAttackWeaponMult, plus fStaminaAttackWeaponBase, times
    // fPowerAttackStaminaPenalty -- 1, 20 and 2 in vanilla; then the Mod Power
    // Attack Stamina entry point with that weapon, or Unarmed; then the
    // attack's own stamina multiplier. A left-hand swing is priced by the
    // right hand, as the engine prices it.
    float cost = ft::PowerAttackStamina(
        right ? right->GetWeight() : 1.0f, GameSetting("fStaminaAttackWeaponMult", 1.0f),
        GameSetting("fStaminaAttackWeaponBase", 20.0f), GameSetting("fPowerAttackStaminaPenalty", 2.0f));
    if (auto *priced = right ? right : RE::TESForm::LookupByID<RE::TESObjectWEAP>(kUnarmedWeapon))
        RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModPowerAttackStamina, actor, priced,
                                            &cost);
    plan.stamina = (std::max)(0.0f, cost * StaminaMultOf(actor, plan.event));
    // The engine's own reach for the actor and what they hold -- the weapon's
    // reach times fCombatDistance, or the race's unarmed reach, times the
    // actor's scale (dev/ACTIONS.md 6). The bodies are in the enemy's
    // ReachDistance, as the engine leaves them out of its distance.
    plan.reach = actor->GetReach();
    return plan;
}

bool PowerBashPerkMet(RE::Actor *actor)
{
    // The Block tree's Power Bash perk (058F67). The idle tree asks it of
    // the player alone, so a follower needs it only where Settings says so,
    // and the player needs it whatever Settings says.
    if (!CurrentSettings().requirePowerBashPerk && !(actor && actor->IsPlayerRef()))
        return true;
    constexpr std::uint32_t kPowerBashPerk = 0x00058F67;
    auto *perk = RE::TESForm::LookupByID<RE::BGSPerk>(kPowerBashPerk);
    return actor && perk && actor->HasPerk(perk);
}

BlowPlan PlanBash(RE::Actor *actor, bool power)
{
    BlowPlan plan;
    if (!actor || !ft::BashesWith(DescribeHands(actor)))
        return plan;
    plan.event = ft::BashEvent(power);
    if (power)
        plan.perk = PowerBashPerkMet(actor);
    // The cost as the engine prices a bash (26429): the setting for the kind
    // -- fStaminaBashBase 35, fStaminaPowerBashBase 55 in vanilla -- times the
    // attack's own stamina multiplier. No perk entry point prices a bash.
    plan.stamina = (power ? GameSetting("fStaminaPowerBashBase", 55.0f) : GameSetting("fStaminaBashBase", 35.0f)) *
                   StaminaMultOf(actor, plan.event);
    // The bash's own reach setting (fCombatBashReach, 141 in vanilla) at the
    // actor's scale, held against the same measure as a swing's; that the
    // engine measures a bash that way was not read.
    plan.reach = GameSetting("fCombatBashReach", 141.0f) * actor->GetScale();
    return plan;
}

BlowPlan PlanBlow(RE::Actor *actor, ft::ActionKind kind)
{
    switch (kind)
    {
    case ft::ActionKind::PowerAttack:
        return PlanPowerAttack(actor);
    case ft::ActionKind::Bash:
        return PlanBash(actor, false);
    case ft::ActionKind::PowerBash:
        return PlanBash(actor, true);
    default:
        return {};
    }
}

// What the actor hits with, a bit per DamageKind: a blade is Melee, a bow
// or crossbow Ranged, a spell or a staff Magic; and the kind of damage any
// of it does -- the enchantment's, the staff's or the spell's effects, a
// poison on the blade -- by what resists it. Hands with no weapon and no
// spell in them are Melee too: the fists, and the claws, teeth and horns
// of a bear, a wolf, a troll, whose hands hold nothing (the Hit type
// condition on a bear found nothing, 2026-09-09). A two-hander reports
// from both hands, which is the same bits twice.
void ReadHands(RE::Actor *actor, ft::ActorTraits &traits)
{
    bool armed = false;
    const auto effectsOf = [&](const RE::MagicItem *magic) {
        if (!magic)
            return;
        for (const auto *effect : ResolvedEffects(*magic))
            if (const auto kind = KindOfEffect(effect->baseEffect); kind != ft::DamageKind::Magic)
                traits.Wield(kind);
    };
    for (const bool left : {false, true})
    {
        RE::TESForm *held = actor->GetEquippedObject(left);
        if (!held)
            continue;
        if (auto *weapon = held->As<RE::TESObjectWEAP>())
        {
            const auto type = weapon->GetWeaponType();
            if (type == RE::WEAPON_TYPE::kHandToHandMelee)
                continue; // the fists' record: bare hands, below
            armed = true;
            if (type == RE::WEAPON_TYPE::kStaff)
                traits.Wield(ft::DamageKind::Magic);
            else if (weapon->IsBow() || weapon->IsCrossbow())
                traits.Wield(ft::DamageKind::Ranged);
            else
                traits.Wield(ft::DamageKind::Melee);
            effectsOf(EnchantmentOn(actor, weapon, left ? Hand::Left : Hand::Right));
            if (WeaponPoisoned(actor, weapon, left ? Hand::Left : Hand::Right))
                traits.Wield(ft::DamageKind::Poison);
        }
        else if (auto *magic = held->As<RE::MagicItem>())
        {
            armed = true;
            traits.Wield(ft::DamageKind::Magic);
            effectsOf(magic);
        }
    }
    if (!armed)
        traits.Wield(ft::DamageKind::Melee);
}

// The kind of being, for the Type condition (dev/CONDITIONS.md 2a). The
// engine's own classes are keywords on the race and the actor base, asked
// of the actor as its conditions ask them (HasKeyword: the sun spells gate
// on ActorTypeUndead this way, and a ghost carries it on the base over a
// living race). The races the classes do not split -- the ten peoples,
// Falmer, giants, spriggans, the were-beasts -- go by the race record's
// editor id, which the vampire, child and DLC variants of a race contain
// (NordRaceVampire, DLC1NordRace). A vampire Nord is a Nord; a ghost of a
// Nord is Undead and not a Nord.
void ReadKinds(RE::Actor *actor, ft::ActorTraits &traits)
{
    struct Keywords
    {
        RE::BGSKeyword *creature, *animal, *daedra, *dragon, *dwarven, *undead, *ghost, *troll, *giant, *vampire,
            *ashSpawn;
    };
    static const Keywords k = [] {
        const auto by = [](const char *id) { return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(id); };
        return Keywords{by("ActorTypeCreature"), by("ActorTypeAnimal"), by("ActorTypeDaedra"),    by("ActorTypeDragon"),
                        by("ActorTypeDwarven"),  by("ActorTypeUndead"), by("ActorTypeGhost"),     by("ActorTypeTroll"),
                        by("ActorTypeGiant"),    by("Vampire"),         by("DLC2AshSpawnKeyword")};
    }();
    using ft::TypeKind;
    const auto has = [&](const RE::BGSKeyword *keyword) { return keyword && actor->HasKeyword(keyword); };
    if (has(k.creature))
        traits.SetType(TypeKind::Creature);
    if (has(k.animal))
        traits.SetType(TypeKind::Animal);
    // Ash Spawn carry the Dwarven keyword, an oddity of Dragonborn; they
    // are not automatons to a player.
    if (has(k.dwarven) && !has(k.ashSpawn))
        traits.SetType(TypeKind::Automaton);
    if (has(k.daedra))
        traits.SetType(TypeKind::Daedra);
    if (has(k.dragon))
        traits.SetType(TypeKind::Dragon);
    if (has(k.giant))
        traits.SetType(TypeKind::Giant);
    if (has(k.troll))
        traits.SetType(TypeKind::Troll);
    if (has(k.undead))
        traits.SetType(TypeKind::Undead);
    if (has(k.vampire))
        traits.SetType(TypeKind::Vampire);

    const RE::TESRace *race = actor->GetRace();
    std::string id = race && race->GetFormEditorID() ? race->GetFormEditorID() : "";
    for (char &c : id)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto in = [&](const char *token) { return id.find(token) != std::string::npos; };
    if (in("werewolfbeast") || in("werebearbeast"))
        traits.SetType(TypeKind::Werewolf);
    if (in("spriggan"))
        traits.SetType(TypeKind::Spriggan);
    // The Lurker's race is named Giant; it is not one.
    if (in("giant") && !in("lurker"))
        traits.SetType(TypeKind::Giant);
    // A creature to the engine and an elf to Wuuthrad's perk, which lists
    // the race with the three elven ones: both here.
    if (in("falmer"))
        traits.SetType(TypeKind::Falmer);
    if (has(k.ghost))
        return;
    struct People
    {
        const char *token;
        TypeKind kind;
    };
    static constexpr People kPeoples[] = {
        {"bretonrace", TypeKind::Breton},     {"imperialrace", TypeKind::Imperial}, {"nordrace", TypeKind::Nord},
        {"redguardrace", TypeKind::Redguard}, {"elderrace", TypeKind::Man},         {"darkelfrace", TypeKind::DarkElf},
        {"highelfrace", TypeKind::HighElf},   {"woodelfrace", TypeKind::WoodElf},   {"snowelfrace", TypeKind::SnowElf},
        {"argonianrace", TypeKind::Argonian}, {"khajiitrace", TypeKind::Khajiit},   {"orcrace", TypeKind::Orc},
    };
    for (const People &people : kPeoples)
        if (in(people.token))
            traits.SetType(people.kind);
}

namespace
{

// Every base effect of the effect's name: the load order's records of one
// name, indexed once, on first use, after the data has loaded. A nameless
// effect is only itself, `own` its FormID.
std::span<const RE::FormID> SameNamedEffects(const RE::EffectSetting *effect, const RE::FormID &own)
{
    static const auto byName = [] {
        std::unordered_map<std::string, std::vector<RE::FormID>> names;
        if (auto *data = RE::TESDataHandler::GetSingleton())
            for (const auto *each : data->GetFormArray<RE::EffectSetting>())
                if (const char *name = each ? each->GetName() : nullptr; name && *name)
                    names[name].push_back(each->GetFormID());
        return names;
    }();
    const char *name = effect->GetName();
    if (const auto it = name && *name ? byName.find(name) : byName.end(); it != byName.end())
        return it->second;
    return {&own, 1};
}

} // namespace

ft::ActorTraits ReadTraits(RE::Actor *actor)
{
    ft::ActorTraits traits;
    if (!actor)
        return traits;
    traits.armor = DamageReduction(actor);
    ReadHands(actor, traits);
    LogArmorReadings(actor);
    // The engine's own list of what the actor commands: a summon, a raised
    // corpse, each with the effect that made it.
    if (const auto *process = actor->GetActorRuntimeData().currentProcess; process && process->middleHigh)
        traits.summons = static_cast<int>(process->middleHigh->commandedActors.size());
    const Attacked attacked = AttackedLately(actor->GetFormID());
    traits.hitBy = attacked.kinds;
    traits.attacker = attacked.attacker;
    ReadKinds(actor, traits);
    if (auto *owner = actor->AsActorValueOwner())
    {
        for (const auto kind : {ft::DamageKind::Magic, ft::DamageKind::Fire, ft::DamageKind::Frost,
                                ft::DamageKind::Shock, ft::DamageKind::Poison})
            traits.SetResist(kind, owner->GetActorValue(ResistValueOf(kind)));
    }
    using Archetype = RE::EffectArchetypes::ArchetypeID;

    {
        {
            ForEachActiveEffect(actor, [&](RE::ActiveEffect &effect) {
                auto *ae = &effect;
                const auto *base = ae->effect->baseEffect;
                // The effect by its name, whatever record carries it: a
                // rule's record answers for all of its name. A hidden one
                // counts for nothing -- survival mode's bookkeeping named
                // Fortify Health Regeneration would hold the condition
                // true, unseen.
                const RE::FormID own = base->GetFormID();
                if (!base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
                    for (const RE::FormID id : SameNamedEffects(base, own))
                        if (!traits.HasEffect(id))
                            traits.effects.push_back(id);
                // Burning, frostbitten, shocked: a hostile effect resisted by
                // that element. The keyword would say the same of vanilla
                // spells; the resist value says it of modded ones too.
                if (base->IsDetrimental())
                {
                    switch (base->data.resistVariable)
                    {
                    case RE::ActorValue::kResistFire:
                        traits.Set(ft::StatusKind::Burning);
                        break;
                    case RE::ActorValue::kResistFrost:
                        traits.Set(ft::StatusKind::Frostbitten);
                        break;
                    case RE::ActorValue::kResistShock:
                        traits.Set(ft::StatusKind::Shocked);
                        break;
                    default:
                        break;
                    }
                }
                if (ae->spell && ae->spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease)
                    traits.Set(ft::StatusKind::Diseased);
                if (ae->spell && ae->spell->IsPoison())
                    traits.Set(ft::StatusKind::Poisoned);
                switch (base->GetArchetype())
                {
                case Archetype::kParalysis:
                    traits.Set(ft::StatusKind::Paralysed);
                    break;
                case Archetype::kInvisibility:
                    traits.Set(ft::StatusKind::Invisible);
                    break;
                case Archetype::kEtherealize:
                    traits.Set(ft::StatusKind::Ethereal);
                    break;
                default:
                    break;
                }
            });
        }
    }

    const auto &runtime = actor->GetActorRuntimeData();
    if (runtime.boolBits.any(RE::Actor::BOOL_BITS::kParalyzed))
        traits.Set(ft::StatusKind::Paralysed);
    if (auto *state = actor->AsActorState())
    {
        if (state->actorState2.staggered)
            traits.Set(ft::StatusKind::Staggered);
        if (state->IsBleedingOut())
            traits.Set(ft::StatusKind::BleedingOut);
    }
    if (runtime.combatController && runtime.combatController->IsFleeing())
        traits.Set(ft::StatusKind::Fleeing);
    if (actor->IsBlocking())
        traits.Set(ft::StatusKind::Blocking);
    if (actor->IsSneaking())
        traits.Set(ft::StatusKind::Sneaking);
    // A hand charging or casting. WhoIsCasting is the engine's own summary
    // of the casters' states, one bit per source.
    if (actor->WhoIsCasting() != 0)
        traits.Set(ft::StatusKind::Casting);
    return traits;
}

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
    s.pins = actor->IsPlayerRef() ? WornAsPins(actor) : PinsOf(s.self);
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

// The school the menus group a spell or a scroll under, read the same way
// the Magic tab reads it (game/Magic.h): the costliest effect's skill.
// Other where there is none, so a spell with no school is still offered
// rather than dropped for want of a heading.
ft::MagicCategory SchoolOfSpell(const RE::MagicItem *item)
{
    const auto *costliest = item ? item->GetCostliestEffectItem() : nullptr;
    const auto *effect = costliest ? costliest->baseEffect : nullptr;
    const ft::MagicCategory school = SchoolOf(effect ? effect->GetMagickSkill() : RE::ActorValue::kNone);
    return school == ft::MagicCategory::COUNT ? ft::MagicCategory::Other : school;
}

} // namespace

std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor)
{
    std::vector<SpellOption> out;
    if (!actor)
        return out;

    ForEachSpell(actor, [&out, actor](RE::SpellItem *spell) {
        const bool power = IsPower(spell);
        if (!IsCastable(spell) && !power)
            return;
        // Above the follower's skill: not offered for casting, as it is not
        // for pinning, so the two menus agree on what they can use. A power
        // has no level.
        if (!power && DescribeHoldable(actor, spell).unusable)
            return;

        std::string name = NameOr(spell, "");
        if (name.empty())
            return; // nameless entries are internal; nothing to show a player
        const bool reanimate = std::ranges::any_of(ResolvedEffects(*spell), IsReanimate);
        out.push_back(SpellOption{spell->GetFormID(), std::move(name),
                                  spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf,
                                  spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation, reanimate,
                                  !power && CanDualCast(actor, spell),
                                  power ? SpellOption::Kind::Power : SpellOption::Kind::Spell, SchoolOfSpell(spell)});
    });

    // The scrolls carried, by the scroll's own delivery: a Self one under
    // Self, an aimed one under everyone else, as a spell is.
    for (const auto &[object, entry] :
         actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::Scroll); }))
    {
        auto *scroll = object ? object->As<RE::ScrollItem>() : nullptr;
        if (!scroll || entry.first <= 0)
            continue;
        std::string name = NameOr(scroll, "");
        if (name.empty())
            continue;
        const bool reanimate = std::ranges::any_of(ResolvedEffects(*scroll), IsReanimate);
        out.push_back(SpellOption{scroll->GetFormID(), std::move(name),
                                  scroll->GetDelivery() == RE::MagicSystem::Delivery::kSelf,
                                  scroll->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation, reanimate, false,
                                  SpellOption::Kind::Scroll, SchoolOfSpell(scroll)});
    }

    // The shouts on the base record. A shout's delivery is its first word's
    // spell's: Whirlwind Sprint and Become Ethereal are Self, the rest aimed.
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numShouts; ++i)
            {
                auto *shout = list->shouts[i];
                // Not one with every word still locked: the menu would be
                // offering a rule the follower could never perform.
                if (!shout || IsWrapperShout(shout->GetFormID()) || !shout->GetName() || !*shout->GetName() ||
                    HighestUnlockedWord(shout) < 0)
                    continue;
                const auto *word = shout->variations[0].spell;
                const bool self = word && word->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
                out.push_back(SpellOption{shout->GetFormID(), shout->GetName(), self, false, false, false,
                                          SpellOption::Kind::Shout});
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const SpellOption &a, const SpellOption &b) { return a.name < b.name; });
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

// --- character sheet ---------------------------------------------------------

namespace
{

// The setting of that name in the game's collection, asked on every use and
// never kept: a mod that changes a parameter mid-session (an MCM slider)
// changes the sheet with it, and the lookup is a hash of the name. A missing
// name is said once, at warn, since a misspelt one would otherwise be a
// vanilla number that looks right. CommonLib's "name"_gs literal is the same
// lookup but keeps the setting in a static and says nothing of a miss.
template <class T> const RE::Setting *FindSetting(const char *name, T vanilla)
{
    auto *collection = RE::GameSettingCollection::GetSingleton();
    if (const auto *setting = collection ? collection->GetSetting(name) : nullptr)
        return setting;
    static std::unordered_set<std::string> missing;
    if (missing.insert(name).second)
        log::sensors.warn("game setting {} not found -- using vanilla's {}", name, vanilla);
    return nullptr;
}

} // namespace

// A float game setting ("f" names), or the vanilla value where the collection
// has none. The fallbacks are vanilla's numbers so a missing setting degrades
// to what the unmodded game does, not to a zero that reads as a broken sheet.
float GameSetting(const char *name, float vanilla)
{
    const auto *setting = FindSetting(name, vanilla);
    return setting ? setting->GetFloat() : vanilla;
}

// An integer game setting ("i" names), the same way.
std::int32_t GameSetting(const char *name, std::int32_t vanilla)
{
    const auto *setting = FindSetting(name, vanilla);
    return setting ? setting->GetInteger() : vanilla;
}

namespace
{

// "83%", or past the engine's cap "110% (85%)": what the gear adds up to
// first, and in brackets what is actually applied. The gap between them says
// how much of the follower's kit is doing nothing.
std::string CappedPercent(float value, float cap)
{
    // "90% (75%)": the value, and in parentheses what the cap makes of it.
    if (value > cap)
        return Fmt("%.0f%%", value) + " (" + Fmt("%.0f%%", cap) + ")";
    return Fmt("%.0f%%", value);
}

// --- perks -------------------------------------------------------------------

struct TreePerk
{
    RE::BGSPerk *perk;
    int rank;          // 1-based position in the perk's rank chain
    int ranks;         // length of that chain
    float requirement; // the skill level the perk asks for; 0 if it asks nothing
    std::string description;
    int order{0}; // its node's place in the tree's own order (ft::TreeOrder)
};

// The skill level a perk requires, read from its own conditions. It is not a
// field on the record: the perk menu's "requires Archery 20" is a condition,
// GetBaseActorValue(Archery) >= 20, and the first perk in a tree has none at
// all. The highest such bound is what the menu shows, so that is what this
// returns.
float SkillRequirement(const RE::BGSPerk *perk, RE::ActorValue skill)
{
    float best = 0.0f;
    for (const auto *item = perk->perkConditions.head; item; item = item->next)
    {
        const auto &d = item->data;
        if (d.functionData.function != RE::FUNCTION_DATA::FunctionID::kGetBaseActorValue)
            continue;
        if (static_cast<RE::ActorValue>(reinterpret_cast<std::uintptr_t>(d.functionData.params[0])) != skill)
            continue;
        if (d.flags.global) // compared against a global, not a number
            continue;
        using Op = RE::CONDITION_ITEM_DATA::OpCode;
        if (d.flags.opCode != Op::kGreaterThanOrEqualTo && d.flags.opCode != Op::kGreaterThan)
            continue;
        best = (std::max)(best, d.comparisonValue.f);
    }
    return best;
}

// Every perk in a skill's tree, ranks included, found by walking the same
// tree the perk menu draws. Walked once per skill and kept, description and
// requirement included: all of it is static data, and this is called from
// the tick, where reading a description every half second for every perk of
// every follower would be the only real cost on the sheet.
const std::vector<TreePerk> &TreePerks(RE::ActorValue skill)
{
    static std::unordered_map<RE::ActorValue, std::vector<TreePerk>> cache;
    if (const auto it = cache.find(skill); it != cache.end())
        return it->second;

    std::vector<TreePerk> out;
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValueInfo(skill) : nullptr;
    if (info && info->perkTree)
    {
        std::vector<RE::BGSSkillPerkTreeNode *> nodes;
        std::unordered_map<const RE::BGSSkillPerkTreeNode *, std::size_t> index;
        std::vector<RE::BGSSkillPerkTreeNode *> stack{info->perkTree};
        while (!stack.empty())
        {
            auto *node = stack.back();
            stack.pop_back();
            if (!node || index.contains(node))
                continue;
            index.emplace(node, nodes.size());
            nodes.push_back(node);
            for (auto *child : node->children)
                stack.push_back(child);
        }

        // Where the menu draws each node across -- its grid column plus its
        // offset within it -- and whom it leads to, for the tree's order.
        // The first node of a tree is a root with no perk and nonsense in its
        // grid; it lists nothing, so its place is harmless.
        std::vector<ft::TreeNodePlace> places(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            places[i].x = static_cast<double>(nodes[i]->perkGridX) + nodes[i]->horizontalPosition;
            for (auto *child : nodes[i]->children)
            {
                if (const auto it = index.find(child); it != index.end())
                    places[i].children.push_back(it->second);
            }
        }
        const std::vector<std::size_t> order = ft::TreeOrder(places);

        for (std::size_t place = 0; place < order.size(); ++place)
        {
            const auto *node = nodes[order[place]];
            // A node names the first rank; the rest chain through nextPerk,
            // walked and bounded by core (ft::RankChain, tested).
            const std::vector<RE::BGSPerk *> chain =
                ft::RankChain(node->perk, [](RE::BGSPerk *rank) { return rank->nextPerk; });
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                RE::BSString text;
                chain[i]->GetDescription(text, chain[i]);
                out.push_back({chain[i], static_cast<int>(i) + 1, static_cast<int>(chain.size()),
                               SkillRequirement(chain[i], skill), text.c_str() ? text.c_str() : "",
                               static_cast<int>(place)});
            }
        }
    }

    // Least demanding first: the requirement is the game's own statement of
    // how strong a perk is, so the list reads weakest to strongest. Perks
    // asking the same read as the tree does, by depth and then left to right
    // as the menu draws them (ft::TreeOrder); a chain's ranks in turn.
    std::sort(out.begin(), out.end(), [](const TreePerk &a, const TreePerk &b) {
        if (a.requirement != b.requirement)
            return a.requirement < b.requirement;
        if (a.order != b.order)
            return a.order < b.order;
        return a.rank < b.rank;
    });
    return cache.emplace(skill, std::move(out)).first->second;
}

// Whether a perk the actor holds does something for them right now.
// A perk has no on-off switch. The conditions on its record are what the
// skill tree asks before the player may take it -- the perk before it in
// the chain, a skill level -- and gate nothing once it is held; Augmented
// Frost given without Elementalist works. What gates the effect is the
// conditions on each entry, whose first tab is the perk's owner: a mod
// that hands every NPC its perks and gates them on a power writes those to
// fail until the power is taken. So a perk is active when any of its
// entries could fire: an entry-point entry with no owner conditions or
// with them met, or an ability or quest entry; and a perk with no entries
// at all, a marker for conditions elsewhere, counts as active.
const std::vector<std::string> &PerkReaders(const RE::BGSPerk *perk); // below

bool PerkActive(RE::Actor *actor, RE::BGSPerk *perk)
{
    if (!actor || !perk)
        return false;
    bool anyEntry = false;
    for (const auto *entry : perk->perkEntries)
    {
        if (!entry)
            continue;
        anyEntry = true;
        if (entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint)
            return true;
        const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
        if (point->conditions.size() == 0 || !point->conditions[0] || point->conditions[0].IsTrue(actor, actor))
            return true;
    }
    // A perk with no entries is a marker for conditions elsewhere -- Skald,
    // which the Bard's perks and spells ask for -- or a stub nothing asks
    // for: Adamant leaves the vanilla third rank of Armsman with its name
    // and its "60% more damage" and no entries, and an NPC authored with
    // it holds a perk that does nothing (Teldryn Sero, 2026-09-13). The
    // load order tells the two apart.
    return !anyEntry && !PerkReaders(perk).empty();
}

// Why a perk held is set aside, for the row's grey: nothing, "Inactive"
// for one whose entries' conditions fail, "Does nothing" for one with no
// entries that nothing reads.
const char *PerkAside(RE::Actor *actor, RE::BGSPerk *perk)
{
    if (PerkActive(actor, perk))
        return nullptr;
    return perk && perk->perkEntries.empty() ? Tr("Does nothing") : Tr("Inactive");
}

// The perks this follower holds in one skill's tree, one row per perk at
// the highest rank held. Asked of the engine with HasPerk rather than read
// off their record, so a perk a mod granted at runtime counts the same as one
// they were authored with. Ordered by the skill level each perk asks for,
// weakest first; the modifiers column carries its own in-game description.
namespace
{
// A perk's name, trimmed, because the records are not: Skyrim.esm's first
// rank of Magic Resistance is named " Magic Resistance", leading space and
// all, and on screen that reads as a row set in for no reason. Empty for
// a perk with no name.
std::string PerkName(const RE::BGSPerk *perk)
{
    const std::string raw = NameOr(perk, "");
    const auto first = raw.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return raw.substr(first, raw.find_last_not_of(' ') - first + 1);
}
} // namespace

// Whether a perk the actor holds grants this ability under the ability's own
// name. Which perks grant which abilities is the load order's, read once.
bool HeldPerkGrants(RE::Actor *actor, const RE::MagicItem *spell)
{
    static const std::unordered_map<const RE::MagicItem *, std::vector<RE::BGSPerk *>> grants = [] {
        std::unordered_map<const RE::MagicItem *, std::vector<RE::BGSPerk *>> out;
        auto *handler = RE::TESDataHandler::GetSingleton();
        if (!handler)
            return out;
        for (auto *perk : handler->GetFormArray<RE::BGSPerk>())
        {
            if (!perk)
                continue;
            for (const auto *entry : perk->perkEntries)
            {
                if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kAbility)
                    continue;
                if (const auto *ability = static_cast<const RE::BGSAbilityPerkEntry *>(entry)->ability)
                    out[ability].push_back(perk);
            }
        }
        return out;
    }();
    if (!actor || !spell)
        return false;
    const auto found = grants.find(spell);
    if (found == grants.end())
        return false;
    std::string name = NameOr(spell, "");
    const auto first = name.find_first_not_of(' ');
    if (first == std::string::npos)
        return false;
    name = name.substr(first, name.find_last_not_of(' ') - first + 1);
    return std::any_of(found->second.begin(), found->second.end(),
                       [&](RE::BGSPerk *perk) { return actor->HasPerk(perk) && PerkName(perk) == name; });
}

// The records whose conditions ask HasPerk of a perk -- other perks'
// entries on any tab, spells' effects, magic effects -- by name, so a
// perk with no entries can say who reads it, or that nobody does. The
// whole load order once, on first use: a few thousand records, a few
// milliseconds, and the answer does not change while the game runs.
const std::vector<std::string> &PerkReaders(const RE::BGSPerk *perk)
{
    static const std::unordered_map<RE::FormID, std::vector<std::string>> readers = [] {
        std::unordered_map<RE::FormID, std::vector<std::string>> out;
        auto *handler = RE::TESDataHandler::GetSingleton();
        if (!handler)
            return out;
        const auto note = [&out](const RE::TESCondition &condition, const std::string &who) {
            for (const auto *item = condition.head; item; item = item->next)
            {
                if (item->data.functionData.function.get() != RE::FUNCTION_DATA::FunctionID::kHasPerk)
                    continue;
                const auto *asked = static_cast<const RE::BGSPerk *>(item->data.functionData.params[0]);
                if (!asked || who.empty())
                    continue;
                auto &names = out[asked->GetFormID()];
                if (std::find(names.begin(), names.end(), who) == names.end())
                    names.push_back(who);
            }
        };
        for (const auto *other : handler->GetFormArray<RE::BGSPerk>())
        {
            if (!other)
                continue;
            for (const auto *entry : other->perkEntries)
            {
                if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint)
                    continue;
                const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
                for (std::uint32_t tab = 0; tab < point->conditions.size(); ++tab)
                    note(point->conditions[tab], PerkName(other));
            }
        }
        for (const auto *spell : handler->GetFormArray<RE::SpellItem>())
        {
            if (!spell)
                continue;
            // Not ResolvedEffects: the conditions are the effect's own and
            // need no base, so an unresolved effect's still count.
            for (const auto *effect : spell->effects)
                if (effect)
                    note(effect->conditions, NameOr(spell, ""));
        }
        for (const auto *effect : handler->GetFormArray<RE::EffectSetting>())
            if (effect)
                note(effect->conditions, NameOr(effect, ""));
        return out;
    }();
    static const std::vector<std::string> none;
    if (!perk)
        return none;
    const auto found = readers.find(perk->GetFormID());
    return found == readers.end() ? none : found->second;
}

namespace
{
// Held, and the rank a held chain shows: no higher rank held, which would
// get the row instead.
bool TopRankHeld(RE::Actor *actor, RE::BGSPerk *perk)
{
    return actor->HasPerk(perk) && !(perk->nextPerk && actor->HasPerk(perk->nextPerk));
}

SheetRow PerkRow(RE::Actor *actor, RE::BGSPerk *perk, int rank, int ranks, std::string description)
{
    std::string label = PerkName(perk);
    if (label.empty())
        label = "?";
    SheetRow row = Row(std::move(label), ranks > 1 ? std::to_string(rank) + "/" + std::to_string(ranks) : "");
    row.modifiers = std::move(description);
    row.form = perk->GetFormID(); // the name opens the perk's page
    if (const char *aside = PerkAside(actor, perk))
        row.aside = aside;
    return row;
}
} // namespace

std::vector<SheetRow> OwnedPerks(RE::Actor *actor, RE::ActorValue skill)
{
    std::vector<SheetRow> rows;
    for (const TreePerk &entry : TreePerks(skill))
    {
        if (TopRankHeld(actor, entry.perk))
            rows.push_back(PerkRow(actor, entry.perk, entry.rank, entry.ranks, entry.description));
    }
    return rows;
}

// The same for a Custom Skills Framework tree, in the tree's order.
std::vector<SheetRow> OwnedPerks(RE::Actor *actor, const CustomSkillTree &tree)
{
    std::vector<SheetRow> rows;
    for (const CustomSkillPerk &entry : tree.perks)
    {
        if (!TopRankHeld(actor, entry.perk))
            continue;
        RE::BSString text;
        entry.perk->GetDescription(text, entry.perk);
        rows.push_back(PerkRow(actor, entry.perk, entry.rank, entry.ranks, text.c_str() ? text.c_str() : ""));
    }
    return rows;
}

// What one hand holds, as rows: a weapon and its numbers, a spell and its
// cost and strongest effect, a shield and its rating, or a torch. An empty
// hand adds no rows, and the caller shows no table for it.
void HandRows(RE::Actor *actor, bool left, std::vector<SheetRow> &rows)
{
    RE::TESForm *held = actor->GetEquippedObject(left);
    if (!held)
        return;

    if (auto *weapon = held->As<RE::TESObjectWEAP>())
    {
        rows.push_back(Row(Tr("Weapon"), NameOr(weapon, "?")));
        rows.back().form = weapon->GetFormID();
        // In their hands: the carried item, for its tempering. Each figure
        // as it applies now, written out on hover.
        const Carried carried = CarriedOf(actor, weapon);
        {
            SheetRow row;
            const float damage = WeaponDamage(actor, weapon, carried.entry.get(), &row.breakdown);
            row.label = Tr("Damage");
            row.value = Fmt("%.0f", damage);
            rows.push_back(std::move(row));
        }
        // The critical pair and the speed in the details page's words and
        // order. Reach and stagger are the record's and on that page. A
        // critical that never lands or lands for nothing is no critical:
        // neither row, rather than one of them beside a 0.
        {
            SheetRow chance;
            const float percent = CritChance(actor, weapon, &chance.breakdown);
            if (const auto critDamage = weapon->GetCritDamage(); critDamage > 0 && percent >= 0.5f)
            {
                rows.push_back(Row(Tr("Critical Damage"), std::to_string(critDamage)));
                chance.label = Tr("Critical Chance");
                chance.value = Fmt("%.0f%%", percent);
                rows.push_back(std::move(chance));
            }
        }
        {
            SheetRow row;
            float speed = WeaponSpeed(actor, weapon, left, &row.breakdown);
            // The speed the swing plays at, where the animation graph holds
            // it: the engine's figure after every plugin between, which no
            // actor value keeps (Comprehensive Attack Rate Patch caps and
            // tapers it in its detour of the engine's speed, 2026-09-14). The
            // formula stays as the lines and the fallback, and what it misses
            // is Other. The variable is the whole speed, record included: a
            // dagger with no speed effects read its record's 1.30
            // (2026-09-15). It moves only while the game runs, so with the
            // clock frozen behind the panel it can lag -- 1.00 on a first
            // open, 1.30 once the panel was closed and opened -- and the lag
            // reads as Other, as an enchantment equipped from the panel does.
            if (float live = 0.0f; actor->GetGraphVariableFloat(left ? "leftWeaponSpeedMult" : "weaponSpeedMult", live))
            {
                log::sensors.debug("{} {} speed: graph {:.3f}, formula {:.3f}", Describe(actor), NameOr(weapon, "?"),
                                   live, speed);
                speed = live;
                row.breakdown.total = live;
                ft::Close(row.breakdown);
            }
            row.label = Tr("Speed");
            row.value = Fmt("%.2f", speed);
            rows.push_back(std::move(row));
        }
        if (weapon->IsBow() || weapon->IsCrossbow())
        {
            if (auto *ammo = actor->GetCurrentAmmo())
            {
                rows.push_back(Row(Tr("Ammo"), NameOr(ammo, "?")));
                rows.back().form = ammo->GetFormID();
                rows.push_back(Row(Tr("Ammo Damage"), Fmt("%.0f", ammo->GetRuntimeData().data.damage)));
            }
            else
            {
                rows.push_back(Row(Tr("Ammo"), Tr("none")));
            }
        }
        return;
    }

    if (auto *spell = held->As<RE::SpellItem>())
    {
        rows.push_back(Row(Tr("Spell"), NameOr(spell, "?")));
        rows.back().form = spell->GetFormID();
        {
            SheetRow row = Row(Tr("Cost"), Fmt("%.0f", spell->CalculateMagickaCost(actor)));
            row.breakdown = SpellCostBreakdown(actor, spell);
            rows.push_back(std::move(row));
        }
        if (const auto *effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
        {
            // As they cast it -- perks and Fortify effects in -- which is
            // what the Effects table shows; the record's 8 beside the
            // table's 12 read as a mistake (Blood Aura, 2026-09-13).
            std::string what = NameOr(effect->baseEffect, "?");
            what += " " + Fmt("%.0f", ActualMagnitude(actor, spell, effect));
            if (const float duration = ActualDuration(actor, spell, effect); duration > 0.0f)
                what = TrFormat("{} for {} s", what, Fmt("%.0f", duration));
            rows.push_back(Row(Tr("Effect"), what));
        }
        return;
    }

    if (auto *armor = held->As<RE::TESObjectARMO>())
    {
        const bool shield = armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        rows.push_back(Row(shield ? Tr("Shield") : Tr("Held"), NameOr(armor, "?")));
        rows.back().form = armor->GetFormID();
        const Carried carried = CarriedOf(actor, armor);
        SheetRow row;
        const float rating = ArmorRating(actor, armor, carried.entry.get(), &row.breakdown);
        row.label = Tr("Armor");
        row.value = Fmt("%.0f", rating);
        rows.push_back(std::move(row));
        return;
    }

    if (held->Is(RE::FormType::Light))
    {
        rows.push_back(Row(Tr("Held"), NameOr(held, Tr("torch"))));
        rows.back().form = held->GetFormID();
        return;
    }

    rows.push_back(Row(Tr("Held"), NameOr(held, "?")));
}

} // namespace

namespace
{

// Tempering lives on the carried item, not the record: an item's health is
// 1.0 untempered and climbs with each visit to a grindstone or workbench,
// and the engine multiplies damage and armour by it.
float Tempering(RE::InventoryEntryData *entry)
{
    if (!entry || !entry->extraLists)
        return 1.0f;
    for (auto *list : *entry->extraLists)
    {
        if (auto *health = list ? list->GetByType<RE::ExtraHealth>() : nullptr; health && health->health > 0.0f)
            return health->health;
    }
    return 1.0f;
}

// The health the armour walk rates a piece at: the highest among the
// entry's copies, and never below 1.0 (id 15990, read from the running game
// 2026-09-14). Each copy's health is its own list's, worn or not (11703),
// and the worn-items walk (InventoryChanges::VisitWornItems, 16096) hands
// it every copy of the form, so a worn piece is rated at the best-tempered
// copy carried, even one in the bag. The first copy's health, as
// Tempering takes it, rated the player's boots 10 points short before
// perks, and the Armor row showed the gap as Other.
float ArmorHealth(RE::InventoryEntryData *entry)
{
    float best = 1.0f;
    if (!entry || !entry->extraLists)
        return best;
    for (auto *list : *entry->extraLists)
    {
        if (auto *health = list ? list->GetByType<RE::ExtraHealth>() : nullptr; health && health->health > best)
            best = health->health;
    }
    return best;
}

} // namespace

namespace
{
// The two hidden perks that turn the Fortify skill values into anything:
// PerkSkillBoosts reads the enchantment values (OneHandedModifier and its
// kin), AlchemySkillBoosts the potion ones (OneHandedPowerModifier ...).
// The player carries both. On the records no follower does (dev/RESEARCH.md
// 6), and UESP agrees: Fortify One-handed on a follower's gauntlets does
// nothing. So the sheets multiply a Fortify value in only for an actor who
// has the perk that reads it, and say so otherwise.
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
} // namespace

// --- perk entry points -------------------------------------------------------

// The value's display name, where the game has one; else the Creation
// Kit's, read as words: Ward Power, Damage Resist. Most values the game
// never shows have no display name (Spellbreaker's ward read as "? +100",
// 2026-09-11).
std::string ValueName(RE::ActorValue value)
{
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValueInfo(value) : nullptr;
    if (!info)
        return "?";
    if (info->GetFullName() && *info->GetFullName())
        return info->GetFullName();
    std::string words;
    for (const char *c = info->enumName ? info->enumName : ""; *c; ++c)
    {
        if (std::isupper(static_cast<unsigned char>(*c)) && !words.empty() &&
            !std::isupper(static_cast<unsigned char>(words.back())))
            words += ' ';
        words += *c;
    }
    return words.empty() ? "?" : words;
}

namespace
{
// The entries the actor holds on one entry point, in the order the
// engine visits them: the arrays on the actor's process, kept sorted by
// priority (dev/MODIFIERS.md).
struct EntryCollector : RE::PerkEntryVisitor
{
    // A virtual destructor after Visit keeps Visit in the slot the engine
    // calls; the engine never destroys one, this stack frame does.
    virtual ~EntryCollector() = default;
    std::vector<RE::BGSEntryPointPerkEntry *> entries;
    RE::BSContainer::ForEachResult Visit(RE::BGSPerkEntry *entry) override
    {
        if (entry && entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint)
            entries.push_back(static_cast<RE::BGSEntryPointPerkEntry *>(entry));
        return RE::BSContainer::ForEachResult::kContinue;
    }
};

// The two floats of a two-value function record, read where the engine's
// handlers read them (dev/MODIFIERS.md): the first is an actor value's
// index for the actor-value functions, the second the multiplier.
struct TwoValueData
{
    void *vtable;
    float data[2];
};
} // namespace

void AddEntryPointLines(ft::Breakdown &b, RE::Actor *actor, RE::BGSEntryPoint::ENTRY_POINT point,
                        const std::vector<void *> &args)
{
    if (!actor)
        return;
    EntryCollector collector;
    actor->ForEachPerkEntry(point, collector);
    // The condition arguments as the engine hands them to an entry: the
    // perk owner first, then the call's own.
    std::vector<void *> argv;
    argv.push_back(actor);
    argv.insert(argv.end(), args.begin(), args.end());
    auto *owner = actor->AsActorValueOwner();

    using Fn = RE::BGSEntryPointFunction::ENTRY_POINT_FUNCTION;
    using DataType = RE::BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA;
    for (RE::BGSEntryPointPerkEntry *entry : collector.entries)
    {
        const auto *data = entry->functionData;
        const auto dataType = data ? data->GetType() : DataType::kInvalid;
        const float one = dataType == DataType::kOneValue
                              ? static_cast<const RE::BGSEntryPointFunctionDataOneValue *>(data)->data
                              : 0.0f;
        const float *two =
            dataType == DataType::kTwoValue ? reinterpret_cast<const TwoValueData *>(data)->data : nullptr;
        // The engine takes the argument list as one untyped pointer. An
        // entry whose conditions fail is not a line: only what applies.
        if (!entry->CheckConditionFilters(
                static_cast<std::uint32_t>(argv.size()),
                reinterpret_cast<void *>(argv.data()))) // NOLINT(bugprone-multi-level-implicit-pointer-conversion)
            continue;

        // The perk's name, trimmed: the records are not (" Magic
        // Resistance", Skyrim.esm).
        std::string label = NameOr(entry->perk, "");
        if (const auto first = label.find_first_not_of(' '); first != std::string::npos)
            label = label.substr(first, label.find_last_not_of(' ') - first + 1);
        else
            label = "?";
        ft::BreakdownLine line;
        line.label = label;
        const auto av = two ? static_cast<RE::ActorValue>(static_cast<int>(two[0])) : RE::ActorValue::kNone;
        const float value = two && owner ? owner->GetActorValue(av) : 0.0f;
        const float mult = two ? two[1] : 0.0f;
        // A perk that reads a value is named for what feeds the value, not
        // for itself: not the controller perk that turned the gauntlets'
        // +25 into a factor. Added, each share of the value -- a source, the
        // base, what neither explains -- is a line of its own, and the lines
        // sum exactly. Multiplied, the engine takes one factor of the whole
        // value, so the line is that factor, named for the sources' effect,
        // with its terms beneath: "Fortify One-handed x 1.5" over Base 1,
        // the gauntlets +0.25 and the ring +0.25. A factor per source,
        // x 1.25 x 1.25, overstated it by their cross term (2026-09-14).
        std::vector<ft::BreakdownLine> bySource;
        const auto withValue = [&](bool multiply, bool onePlus) {
            const ValueParts parts = PartsOf(actor, av);
            std::vector<std::pair<std::string, float>> shares;
            float explained = parts.base;
            // Named for the value: a bare "Base" beside the weapon's own
            // read as the same thing.
            if (std::abs(parts.base) > 0.05f)
                shares.emplace_back(TrFormat("Base {}", ValueName(av)), parts.base);
            for (const Contribution &c : parts.sources)
            {
                explained += c.amount;
                shares.emplace_back(c.source, c.amount);
            }
            if (const float rest = value - explained; std::abs(rest) > 0.05f)
                shares.emplace_back(Tr("Other"), rest);
            const auto push = [&](std::string label, double amount) {
                ft::BreakdownLine each;
                each.op = multiply ? ft::Op::Multiply : ft::Op::Add;
                each.label = std::move(label);
                each.amount = amount;
                bySource.push_back(std::move(each));
            };
            if (!multiply)
            {
                for (auto &[label, amount] : shares)
                    push(std::move(label), static_cast<double>(amount) * mult);
                return;
            }
            // The sources' effect where they share one, "Fortify
            // One-handed"; else the value's own name.
            std::string named = parts.sources.empty() ? std::string{} : parts.sources.front().effect;
            for (const Contribution &c : parts.sources)
            {
                if (c.effect != named)
                    named.clear();
            }
            push(named.empty() ? ValueName(av) : named, (onePlus ? 1.0 : 0.0) + static_cast<double>(value) * mult);
            // The factor's terms, in the factor's own units.
            ft::Breakdown terms;
            if (onePlus)
                ft::Start(terms, Tr("Base"), 1.0);
            for (auto &[label, amount] : shares)
                ft::Add(terms, std::move(label), static_cast<double>(amount) * mult);
            for (ft::BreakdownLine &term : terms.lines)
                term.unit = std::string{};
            bySource.back().detail = std::move(terms.lines);
        };
        switch (entry->entryData.function.get())
        {
        case Fn::kSetValue:
            line.op = ft::Op::Start;
            line.label = TrFormat("{} (set)", line.label);
            line.amount = one;
            break;
        case Fn::kAddValue:
            line.op = ft::Op::Add;
            line.amount = one;
            break;
        case Fn::kMultiplyValue:
            line.op = ft::Op::Multiply;
            line.amount = one;
            break;
        case Fn::kAddRangeToValue:
            // A fresh roll each call; the low end is listed and the roll
            // is the Other line's.
            line.op = ft::Op::Add;
            line.amount = two ? two[0] : 0.0f;
            if (two)
                line.label = TrFormat("{} ({} to {})", line.label, Fmt("%g", two[0]), Fmt("%g", two[1]));
            break;
        case Fn::kAddActorValueMult:
            line.op = ft::Op::Add;
            line.amount = value * mult;
            withValue(false, false);
            break;
        case Fn::kSetToActorValueMult:
            line.op = ft::Op::Start;
            line.amount = value * mult;
            break;
        case Fn::kMultiplyActorValueMult:
            line.op = ft::Op::Multiply;
            line.amount = value * mult;
            break;
        case Fn::kMultiplyOnePlusActorValueMult:
            line.op = ft::Op::Multiply;
            line.amount = 1.0 + value * mult;
            withValue(true, true);
            break;
        default:
            // Nothing a number can carry: a leveled list, a text.
            continue;
        }
        // An entry that applies and changes nothing -- a controller perk's
        // multiply by one -- is not a line either.
        if ((line.op == ft::Op::Multiply && std::abs(line.amount - 1.0) < 1e-6) ||
            (line.op == ft::Op::Add && std::abs(line.amount) < 1e-6))
            continue;
        if (!bySource.empty())
        {
            for (ft::BreakdownLine &each : bySource)
                b.lines.push_back(std::move(each));
            continue;
        }
        b.lines.push_back(std::move(line));
    }
}

WeaponSkillCurve SkillCurveOf(RE::Actor *actor, RE::TESObjectWEAP *weapon)
{
    // The skill curve: UESP gives it as (1 + skill / 200), which is what the
    // fallbacks below encode. The settings are read by the names the engine
    // uses so a rebalancing mod that changes them is honoured; the resolved
    // curve is logged once so a wrong name shows up as a wrong number in the
    // log rather than as a silently vanilla curve.
    using AV = RE::ActorValue;
    WeaponSkillCurve out;
    out.skill = AV::kOneHanded;
    if (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe())
        out.skill = AV::kTwoHanded;
    else if (weapon->IsBow() || weapon->IsCrossbow())
        out.skill = AV::kArchery;

    // The skill curve: min + (max - min) * skill / 100, and the engine
    // keeps one pair of settings for the player and another for everyone
    // else (read off the armour multiplier's disassembly, 2026-09-09, which
    // branches on IsPlayerOwner; the damage pair is named the same way).
    // The wikis' "1 + skill / 200" is the player's pair. Logged once, so a
    // mod that retunes them is visible.
    const float npcMin = GameSetting("fDamageSkillMin", 1.0f);
    const float npcMax = GameSetting("fDamageSkillMax", 1.5f);
    const float pcMin = GameSetting("fDamagePCSkillMin", 1.0f);
    const float pcMax = GameSetting("fDamagePCSkillMax", 1.5f);
    static const bool logged = [&] {
        log::sensors.debug("damage: skill curve NPC {:.2f} to {:.2f}, player {:.2f} to {:.2f} over skill 0 to 100",
                           npcMin, npcMax, pcMin, pcMax);
        return true;
    }();
    (void)logged;

    auto *owner = actor->AsActorValueOwner();
    out.level = owner ? owner->GetActorValue(out.skill) : 0.0f;
    const bool player = actor->IsPlayerRef();
    const float lo = player ? pcMin : npcMin;
    const float hi = player ? pcMax : npcMax;
    out.factor = lo + (hi - lo) * out.level / 100.0f;
    return out;
}

float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry, ft::Breakdown *out,
                   RE::Actor *target)
{
    if (!actor || !weapon)
        return 0.0f;
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    float damage = weapon->GetAttackDamage();
    ft::Start(b, Tr("Base"), damage);
    if (const float tempering = Tempering(entry); tempering != 1.0f)
    {
        damage *= tempering;
        ft::Multiply(b, Tr("Tempering"), tempering);
    }

    using AV = RE::ActorValue;
    const WeaponSkillCurve curve = SkillCurveOf(actor, weapon);
    damage *= curve.factor;
    // The lines' sources are walked only for a breakdown someone reads: the
    // AI's score asks for the figure alone, once a second per weapon.
    auto &skillLine = ft::Multiply(b, ValueName(curve.skill) + " (" + Fmt("%.0f", curve.level) + ")", curve.factor);
    if (out)
        skillLine.detail = ValueLines(actor, curve.skill, curve.level);

    // Perks, through the engine's own entry point, so Armsman and the rest
    // count exactly as they do in a swing. The entry point wants a target;
    // with none given -- the sheet, outside a fight -- they stand in for it
    // themself. A perk that reads the target (against undead, say) then
    // evaluates against them and so stays out of the figure -- the same
    // figure the player's own inventory menu shows, which has no target
    // either. The AI's score passes the enemy, and such a perk counts.
    // Fortify One-handed and its kin count here too, for whoever holds the
    // hidden perk that reads them (every NPC in Nordic Souls; no follower in
    // vanilla): multiplying the value in by hand as well doubled it
    // (dev/MODIFIERS.md, 2026-09-13).
    RE::Actor *against = target ? target : actor;
    if (out)
        AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kModAttackDamage, {weapon, against});
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModAttackDamage, actor, weapon, against,
                                        &damage);

    // The multiplier on every physical hit (a Vampire Lord's, a mod's), 1
    // for plain, and flat points on the weapon's listed damage: both per
    // UESP's account of what the listed damage carries, not yet read off
    // the executable (dev/MODIFIERS.md).
    if (auto *owner = actor->AsActorValueOwner())
    {
        if (const float mult = owner->GetActorValue(AV::kAttackDamageMult); mult > 0.0f && mult != 1.0f)
        {
            damage *= mult;
            auto &line = ft::Multiply(b, Tr("Attack Damage Mult"), mult);
            if (out)
                line.detail = ValueLines(actor, AV::kAttackDamageMult, mult);
        }
        if (const float flat = owner->GetActorValue(AV::kMeleeDamage); flat != 0.0f)
        {
            damage += flat;
            auto &line = ft::Add(b, Tr("Melee Damage"), flat);
            if (out)
                line.detail = ValueLines(actor, AV::kMeleeDamage, flat);
        }
    }

    b.total = damage;
    ft::Close(b);
    return damage;
}

float CritChance(RE::Actor *actor, RE::TESObjectWEAP *weapon, ft::Breakdown *out)
{
    if (!actor || !weapon)
        return 0.0f;
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    b.unit = "%";
    auto *owner = actor->AsActorValueOwner();
    float chance = owner ? owner->GetActorValue(RE::ActorValue::kCriticalChance) : 0.0f;
    AddValueLines(b, PartsOf(actor, RE::ActorValue::kCriticalChance));
    // Bladesman and its kin set or add to it here. The entry point takes
    // the weapon and a target; they stand in for the target, as for
    // damage. Where the engine starts its own figure from is not yet read
    // off the executable (dev/MODIFIERS.md).
    AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kCalculateMyCriticalHitChance, {weapon, actor});
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kCalculateMyCriticalHitChance, actor, weapon,
                                        actor, &chance);
    b.total = chance;
    ft::Close(b);
    return chance;
}

float WeaponSpeed(RE::Actor *actor, RE::TESObjectWEAP *weapon, bool left, ft::Breakdown *out)
{
    if (!weapon)
        return 0.0f;
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    b.decimals = 2;
    // The engine's figure (id 26417, read from the running game 2026-09-14):
    // the record's speed, times fWeaponTwoHandedAnimationSpeedMult for a
    // two-handed sword or axe -- not a bow or a crossbow -- times the hand's
    // multiplier, where zero or less is none. A plugin that rewrites the
    // multiplier's read (Comprehensive Attack Rate Patch) is Other beneath it.
    float speed = weapon->GetSpeed();
    ft::Start(b, Tr("Base"), speed);
    if (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe())
    {
        if (const float twoHanded = GameSetting("fWeaponTwoHandedAnimationSpeedMult", 1.0f); twoHanded != 1.0f)
        {
            speed *= twoHanded;
            ft::Multiply(b, Tr("Two-handed"), twoHanded);
        }
    }
    const auto value = left ? RE::ActorValue::kLeftWeaponSpeedMultiply : RE::ActorValue::kWeaponSpeedMult;
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (const float mult = owner ? owner->GetActorValue(value) : 0.0f; mult > 0.0f && mult != 1.0f)
    {
        speed *= mult;
        ft::Multiply(b, ValueName(value), mult).detail = ValueLines(actor, value, mult);
    }
    b.total = speed;
    ft::Close(b);
    return speed;
}

float WordRecovery(RE::Actor *actor, float recovery, ft::Breakdown *out)
{
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    b.unit = Tr(" s");
    ft::Start(b, Tr("Base"), recovery);
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (const float mult = owner ? owner->GetActorValue(RE::ActorValue::kShoutRecoveryMult) : 1.0f;
        mult > 0.0f && mult != 1.0f)
    {
        recovery *= mult;
        ft::Multiply(b, Tr("Shout Recovery Mult"), mult).detail =
            ValueLines(actor, RE::ActorValue::kShoutRecoveryMult, mult);
    }
    b.total = recovery;
    ft::Close(b);
    return recovery;
}

ft::Breakdown SpellCostBreakdown(RE::Actor *actor, const RE::SpellItem *spell)
{
    // The engine's own cost, read off the executable (dev/MODIFIERS.md):
    // the sum of each effect's cost, each scaled by the caster's skill in
    // the effect's school, then the Mod Spell Cost entries, then clamped
    // at zero. A power gets none of it: the caster is dropped and the sum
    // is the cost.
    ft::Breakdown b;
    if (!actor || !spell)
        return b;
    auto *owner = actor->AsActorValueOwner();
    const auto type = spell->GetSpellType();
    const bool power = type == RE::MagicSystem::SpellType::kPower || type == RE::MagicSystem::SpellType::kLesserPower;
    const RE::Effect *costliest = spell->GetCostliestEffectItem();
    if (spell->data.flags.any(RE::SpellItem::SpellFlag::kCostOverride))
        ft::Start(b, Tr("Base"), static_cast<float>(spell->data.costOverride));
    else
    {
        for (const auto *effect : ResolvedEffects(*spell))
            ft::Add(b, NameOr(effect->baseEffect, "?"), effect->cost);
    }
    // The skill curve: cost times mult times (1 - (base * level)^scale),
    // one triple of settings for the player and another for everyone
    // else, matched to their names by reading the setting objects behind
    // the constants (2026-09-13). Applied per effect by the engine; here
    // as one factor from the costliest effect's school, so a spell of two
    // schools shows the rest as Other.
    // Only for an effect of a school: the engine checks the skill is one
    // of the eighteen before reading it, and so must we -- asking the
    // value owner for None crashed inside another plugin's hook on
    // Serana's Drain Life, whose effect has no school (2026-09-13).
    const RE::ActorValue skill =
        costliest && costliest->baseEffect ? costliest->baseEffect->GetMagickSkill() : RE::ActorValue::kNone;
    const bool schooled = skill >= RE::ActorValue::kOneHanded && skill <= RE::ActorValue::kEnchanting;
    if (!power && owner && schooled)
    {
        const bool player = actor->IsPlayerRef();
        const float npcBase = GameSetting("fMagicCasterSkillCostBase", 0.005f);
        const float npcScale = GameSetting("fMagicSkillCostScale", 0.5f);
        const float npcMult = GameSetting("fMagicCasterSkillCostMult", 0.5f);
        const float pcBase = GameSetting("fMagicCasterPCSkillCostBase", 0.0034f);
        const float pcScale = GameSetting("fMagicPCSkillCostScale", 0.65f);
        const float pcMult = GameSetting("fMagicCasterPCSkillCostMult", 1.0f);
        const float level = (std::max)(0.0f, owner->GetActorValue(skill));
        const float factor = player ? pcMult * (1.0f - std::pow(pcBase * level, pcScale))
                                    : npcMult * (1.0f - std::pow(npcBase * level, npcScale));
        ft::Multiply(b, ValueName(skill) + " (" + Fmt("%.0f", level) + ")", factor).detail =
            ValueLines(actor, skill, level);
        AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kModSpellCost,
                           {const_cast<RE::SpellItem *>(spell)});
    }
    b.total = spell->CalculateMagickaCost(actor);
    ft::Close(b);
    return b;
}

float ArmorRating(RE::Actor *actor, RE::TESObjectARMO *armor, RE::InventoryEntryData *entry, ft::Breakdown *out)
{
    // The engine's own per-piece figure, read off its armour rating walk
    // (2026-09-13, dev/MODIFIERS.md): the record's rating plus the
    // tempering bonus in points, times the skill multiplier plus the Armor
    // Perks value, rounded up, then the Mod Armor Rating entries.
    if (!actor || !armor)
        return 0.0f;
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    using Class = RE::BGSBipedObjectForm::ArmorType;
    const Class armorClass = armor->GetArmorType();
    if (armorClass == Class::kClothing)
        return 0.0f;
    float rating = armor->GetArmorRating();
    ft::Start(b, Tr("Base"), rating);

    // Tempering is flat points: one plus the item health's place between
    // the first and last health steps, times the armour smithing maximum
    // less one, floored at zero, so an untempered piece at health 1.0 gets
    // nothing. Doubled, before the floor, for a piece carrying the default
    // object Keyword Cuirass: Serana's tempered Vampire Armor rated 82 on
    // our sheet and 106 in the engine, the whole of an Other +24
    // (measured 2026-09-13, dev/MODIFIERS.md).
    const float healthLow = GameSetting("fHealthDataValue1", 1.1f);
    const float healthHigh = GameSetting("fHealthDataValue6", 1.6f);
    const float smithingMax = GameSetting("fSmithingArmorMax", 10.0f);
    if (healthHigh > healthLow)
    {
        float bonus = 1.0f + (ArmorHealth(entry) - healthLow) / (healthHigh - healthLow) * (smithingMax - 1.0f);
        auto *defaults = RE::BGSDefaultObjectManager::GetSingleton();
        // wingdi.h's GetObject macro reaches this file through the
        // precompiled header, after CommonLib's declaration, and renames
        // the member at the call.
#pragma push_macro("GetObject")
#undef GetObject
        auto **slot = defaults && defaults->IsObjectInitialized(RE::DefaultObjectID::kKeywordCuirass)
                          ? defaults->GetObject<RE::BGSKeyword>(RE::DefaultObjectID::kKeywordCuirass)
                          : nullptr;
#pragma pop_macro("GetObject")
        const RE::BGSKeyword *cuirass = slot ? *slot : nullptr;
        const bool body = cuirass && static_cast<const RE::BGSKeywordForm *>(armor)->HasKeyword(cuirass);
        if (body)
            bonus *= 2.0f;
        if (bonus > 0.0f)
        {
            rating += bonus;
            ft::Add(b, body ? Tr("Tempering (body, doubled)") : Tr("Tempering"), bonus);
        }
    }

    using AV = RE::ActorValue;
    const bool heavy = armorClass == Class::kHeavyArmor;
    const AV skill = heavy ? AV::kHeavyArmor : AV::kLightArmor;

    // The skill curve is the engine's own: fArmorRatingBase to
    // fArmorRatingMax over skill 0 to 100 for an NPC, the PC pair for the
    // player (GetArmorRatingSkillMultiplier branches on IsPlayerOwner).
    // The wikis' "1 + 0.4 * skill / 100" is the player's pair, and used
    // for a follower it read 46 where the engine had 66. Logged once.
    static const bool logged = [] {
        log::sensors.debug("armor: skill curve NPC {:.2f} to {:.2f}, player {:.2f} to {:.2f} over skill 0 to 100",
                           GameSetting("fArmorRatingBase", 1.0f), GameSetting("fArmorRatingMax", 1.4f),
                           GameSetting("fArmorRatingPCBase", 1.0f), GameSetting("fArmorRatingPCMax", 1.4f));
        return true;
    }();
    (void)logged;

    auto *owner = actor->AsActorValueOwner();
    const float skillLevel = owner ? owner->GetActorValue(skill) : 0.0f;
    if (owner)
    {
        // The Armor Perks value is added to the skill multiplier: one
        // factor, written as the sum it is, "x (1.35 + 0.20)". What set
        // the value is on the Skills tab, on the armour skill's row.
        const float curve = owner->GetArmorRatingSkillMultiplier(skillLevel);
        const float perks = owner->GetActorValue(AV::kArmorPerks);
        rating *= curve + perks;
        ft::BreakdownLine &line =
            ft::Multiply(b, ValueName(skill) + " (" + Fmt("%.0f", skillLevel) + ")", curve + perks);
        if (perks != 0.0f)
            line.amountText = "x (" + Fmt("%.2f", curve) + " + " + Fmt("%.2f", perks) + ")";
        line.detail = ValueLines(actor, skill, skillLevel);
    }
    // Rounded up to whole points before the perks.
    if (const float up = std::ceil(rating) - rating; up > 0.0f)
    {
        rating += up;
        // Its own line, with its decimals: hidden below one printed digit,
        // it came back as Other wherever a perk multiplied it afterwards.
        if (ft::Visible(b, up))
            ft::Add(b, Tr("Rounding"), up);
    }

    // Perks: Juggernaut, Agile Defender and their kin, through the engine's
    // entry point for armour, which takes the piece and the value. A
    // Fortify Heavy Armor value is not multiplied in: the hidden perk that
    // reads it cuts incoming damage, not the rating (dev/RESEARCH.md 6),
    // and vanilla writes the skill itself.
    AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kModArmorRating, {armor});
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModArmorRating, actor, armor, &rating);
    b.total = rating;
    ft::Close(b);
    return rating;
}

std::vector<SheetSection> BuildCharacterSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return out;

    const auto av = [owner](RE::ActorValue value) { return owner->GetActorValue(value); };

    {
        SheetSection s{Tr("General"), {}, {}};
        // The reference and the base record, as the console names them --
        // what "prid" takes, and what the log calls the follower.
        {
            char id[16];
            std::snprintf(id, sizeof(id), "%08X", actor->GetFormID());
            s.rows.push_back(Row(Tr("Ref ID"), id));
            const auto *base = actor->GetActorBase();
            std::snprintf(id, sizeof(id), "%08X", base ? base->GetFormID() : 0u);
            s.rows.push_back(Row(Tr("Base ID"), id));
        }
        s.rows.push_back(Row(Tr("Name"), NameOr(actor, "?")));
        auto *race = actor->GetRace();
        s.rows.push_back(Row(Tr("Race"), NameOr(race, "?")));
        if (const auto *base = actor->GetActorBase())
        {
            const auto sex = base->GetSex();
            if (sex == RE::SEX::kMale || sex == RE::SEX::kFemale)
                s.rows.push_back(Row(Tr("Gender"), sex == RE::SEX::kMale ? Tr("Male") : Tr("Female")));
        }
        // Speed is the multiplier every buff lands on -- 100 for plain, and
        // a Fortify Speed or a Slow moves it -- so it reads the same
        // standing and sprinting. What moves it and by whom is the hover
        // text, as for the regen rates.
        {
            SheetRow row = Row(Tr("Speed"), Fmt("%.0f%%", av(RE::ActorValue::kSpeedMult)));
            row.breakdown = ValueBreakdown(actor, RE::ActorValue::kSpeedMult, "%");
            s.rows.push_back(std::move(row));
        }
        s.rows.push_back(Row(Tr("Noise"), Fmt("%.0f%%", av(RE::ActorValue::kMovementNoiseMult) * 100.0)));
        out.push_back(std::move(s));
    }

    // Attack: what each hand holds, whatever it is. The old Attack section
    // knew only weapons, which left a mage's page saying "unarmed". A hand
    // holding nothing gets no table; with both empty, the one thing worth
    // saying is what their fists do. A two-handed weapon or a spell cast
    // with both hands is one thing in both, and one table: a Left Hand
    // table saying "the same" was a table of nothing.
    {
        RE::TESForm *held = actor->GetEquippedObject(false);
        auto *weapon = held ? held->As<RE::TESObjectWEAP>() : nullptr;
        auto *spell = held ? held->As<RE::SpellItem>() : nullptr;
        const bool both = TwoHanded(weapon) || (spell && spell->IsTwoHanded());
        SheetSection right{both ? Tr("Both Hands") : Tr("Right Hand"), {}, Tr("Attack")};
        HandRows(actor, false, right.rows);
        SheetSection left{Tr("Left Hand"), {}, Tr("Attack")};
        if (!both)
            HandRows(actor, true, left.rows);

        if (right.rows.empty() && left.rows.empty())
        {
            SheetSection s{Tr("Attack"), {}, {}};
            s.rows.push_back(Row(Tr("Held"), Tr("unarmed")));
            s.rows.push_back(Row(Tr("Base Damage"), Fmt("%.0f", av(RE::ActorValue::kUnarmedDamage))));
            out.push_back(std::move(s));
        }
        else
        {
            if (!right.rows.empty())
                out.push_back(std::move(right));
            if (!left.rows.empty())
                out.push_back(std::move(left));
        }
    }

    {
        SheetSection s{Tr("Defense"), {}, {}};
        // The armour rating the game shows is not the one it applies: each
        // piece worn adds a hidden bonus before the scaling factor, which is
        // why a displayed 609 lands at 85% and not 73%. One row: the rating
        // WITH that bonus in its own units, so the hover text's pieces and
        // bonus sum to it, and in parentheses the share of a blow it turns
        // away -- the same DamageReduction the Armor condition reads, so the
        // sheet and the rules cannot disagree. Robes and boots alone read
        // 50 (6%): two pieces' hidden bonus and no rating.
        const float resistCap = GameSetting("fPlayerMaxResistance", 85.0f);
        // The share of a blow turned away, clamped at the cap: one
        // percent, the one that applies -- "582 (75%)" in a list that caps
        // at 75.
        SheetRow armorRow = Row(Tr("Armor"), Fmt("%.0f", EffectiveArmor(actor)) + " (" +
                                                 Fmt("%.0f%%", DamageReduction(actor) * 100.0f) + ")");
        armorRow.breakdown = ArmorBreakdown(actor);
        s.rows.push_back(std::move(armorRow));
        // Each resistance with where it comes from as its hover text: the
        // ring, the potion, the race.
        const auto resist = [&](const char *label, RE::ActorValue value, bool capped) {
            SheetRow row = Row(label, capped ? CappedPercent(av(value), resistCap) : Fmt("%.0f%%", av(value)));
            row.breakdown = ValueBreakdown(actor, value, "%");
            s.rows.push_back(std::move(row));
        };
        // The chance to reflect a blow back, after the rating it did not
        // turn away.
        resist(Tr("Reflect"), RE::ActorValue::kReflectDamage, false);
        // Magic first, with the chance to absorb a spell outright beside
        // it, then the elements, then poison; disease last, the one that
        // matters to the player alone.
        resist(Tr("Magic"), RE::ActorValue::kResistMagic, true);
        resist(Tr("Spell Absorb"), RE::ActorValue::kAbsorbChance, false);
        resist(Tr("Fire"), RE::ActorValue::kResistFire, true);
        resist(Tr("Frost"), RE::ActorValue::kResistFrost, true);
        resist(Tr("Shock"), RE::ActorValue::kResistShock, true);
        resist(Tr("Poison"), RE::ActorValue::kPoisonResist, true);
        resist(Tr("Disease"), RE::ActorValue::kResistDisease, false);
        out.push_back(std::move(s));
    }

    {
        // The rate the follower regenerates at: the rate, a share of the pool
        // a second, times its multiplier, where a Magicka Rate Mult of 200 is
        // x 2. A buff lands on either: robes of Destruction's "magicka
        // regenerates 100% faster" is +100 on the multiplier, and Mundus's
        // Elfborn stone is +3 on the rate itself. Written as the two factors,
        // each over its terms: as lines of flat rates, each multiplier source
        // read as the rate it added, and a stone that doubled the rate
        // doubled them unseen (2026-09-15). Rate times multiplier is UESP's
        // account, not read off the executable.
        SheetSection s{Tr("Regen"), {}, {}};
        const auto regen = [&](const char *label, RE::ActorValue rate, RE::ActorValue mult) {
            const float current = av(rate);
            const float factor = av(mult) / 100.0f;
            const float total = current * factor;
            SheetRow row = Row(label, Fmt("%.2f%%", total));
            ft::Breakdown &b = row.breakdown;
            b.decimals = 2;
            b.unit = "%";
            ft::Start(b, ValueName(rate), current).detail = ValueLines(actor, rate, current);
            if (factor != 1.0f)
                ft::Multiply(b, ValueName(mult), factor).detail = ValueLines(actor, mult, factor, 0.01f);
            b.total = total;
            ft::Close(b);
            s.rows.push_back(std::move(row));
        };
        regen(Tr("Health Rate"), RE::ActorValue::kHealRate, RE::ActorValue::kHealRateMult);
        regen(Tr("Stamina Rate"), RE::ActorValue::kStaminaRate, RE::ActorValue::kStaminaRateMult);
        regen(Tr("Magicka Rate"), RE::ActorValue::kMagickaRate, RE::ActorValue::kMagickaRateMult);
        out.push_back(std::move(s));
    }

    return out;
}

RE::TESCombatStyle *LiveCombatStyle(RE::Actor *actor)
{
    if (!actor)
        return nullptr;
    auto *npc = actor->GetActorBase();
    auto *record = npc ? npc->GetCombatStyle() : nullptr;
    auto *controller = actor->GetActorRuntimeData().combatController;
    return controller && controller->combatStyle ? controller->combatStyle : record;
}

bool DualWieldAllowed(RE::Actor *actor)
{
    // The style tunes the combat AI; the player holds what they like.
    if (actor && actor->IsPlayerRef())
        return true;
    // With the requirement off, anyone may hold two weapons and what the AI
    // makes of them is the AI's business (game/Settings.h).
    if (!CurrentSettings().requireDualWieldStyle)
        return true;
    auto *style = LiveCombatStyle(actor);
    return !style || style->flags.all(RE::TESCombatStyle::FLAG::kAllowDualWielding);
}

std::vector<SheetSection> BuildCombatStyleSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *npc = actor->GetActorBase();
    auto *record = npc ? npc->GetCombatStyle() : nullptr;
    auto *controller = actor->GetActorRuntimeData().combatController;
    auto *live = LiveCombatStyle(actor);
    if (!live)
        return out;

    // Two scales, read off every style in the load order (163 of them):
    // the chances and movement multipliers run 0 to 1, and the score and
    // attack multipliers run 0 to 10, with 1 as the neutral value.
    const auto chance = [](float x) { return Fmt("%.2f", x) + " / 1"; };
    const auto score = [](float x) { return Fmt("%.2f", x) + " / 10"; };
    // Hover text: the Creation Kit wiki's word on each field, as bullets.
    // dev/COMBAT_AI.md "Combat styles" has the page.
    const auto note = [](SheetRow row, const char *text) {
        row.note = text;
        return row;
    };

    using Flag = RE::TESCombatStyle::FLAG;
    const bool flanking = live->flags.all(Flag::kFlankingStyle);
    {
        SheetSection s{Tr("Style"), {}, {}};
        // A runtime copy has a 0xFF FormID; a record's is its plugin's.
        const bool ours = (live->GetFormID() & 0xFF000000U) == 0xFF000000U;
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", live->GetFormID());
        s.rows.push_back(Row(Tr("Base ID"), ours ? TrFormat("{}  (our copy)", std::string(id)) : std::string(id)));
        if (controller && controller->combatStyle && record && controller->combatStyle != record)
        {
            char recordId[16];
            std::snprintf(recordId, sizeof(recordId), "%08X", record->GetFormID());
            s.rows.push_back(Row(Tr("On Record"), recordId));
        }
        s.rows.push_back(note(Row(Tr("Close Range"), flanking ? Tr("Flanking") : Tr("Dueling")),
                              Tr("- Dueling: circles, falls back\n"
                                 "- Flanking: keeps a distance, stalks")));
        // A tick when allowed, as the equipped state is shown; no row at all
        // when not.
        if (live->flags.all(Flag::kAllowDualWielding))
        {
            SheetRow row = note(Row(Tr("Dual Wield"), ""), Tr("- Can hold a weapon in each hand\n"
                                                              "- Staves do not count"));
            row.icon = kGlyphTick;
            s.rows.push_back(std::move(row));
        }
        out.push_back(std::move(s));
    }
    {
        const auto &g = live->generalData;
        SheetSection s{Tr("General"), {}, {}};
        s.rows.push_back(note(Row(Tr("Offensive"), chance(g.offensiveMult)), Tr("- Higher: attacks more often\n"
                                                                                "- More power attacks")));
        s.rows.push_back(
            note(Row(Tr("Defensive"), chance(g.defensiveMult)), Tr("- Higher: blocks more, holds it longer\n"
                                                                   "- Bashes more, given a shield or a weapon")));
        s.rows.push_back(note(Row(Tr("Group Offensive"), chance(g.groupOffensiveMult)),
                              Tr("- Replaces Offensive when several attack one target\n"
                                 "- Higher: stays offensive in a crowd")));
        out.push_back(std::move(s));
    }
    {
        // The six that decide what they prefer to hold.
        const auto &g = live->generalData;
        SheetSection s{Tr("Equipment Scores"), {}, {}};
        const char *kScore = Tr("- Multiplies the damage of attacks of this kind\n"
                                "- The highest score is what gets used\n"
                                "- A weak weapon needs a high score to beat a strong spell");
        s.rows.push_back(note(Row(Tr("Melee"), score(g.meleeScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Magic"), score(g.magicScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Ranged"), score(g.rangedScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Staff"), score(g.staffScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Shout"), score(g.shoutScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Unarmed"), score(g.unarmedScoreMult)), kScore));
        out.push_back(std::move(s));
    }
    {
        const auto &m = live->meleeData;
        SheetSection s{Tr("Melee"), {}, {}};
        s.rows.push_back(note(Row(Tr("Attack, Staggered"), score(m.attackIncapacitatedMult)),
                              Tr("- Higher: attacks a staggered target more")));
        s.rows.push_back(note(Row(Tr("Power Attack, Staggered"), score(m.powerAttackIncapacitatedMult)),
                              Tr("- Higher: power-attacks a staggered target more")));
        s.rows.push_back(note(Row(Tr("Power Attack, Blocking"), score(m.powerAttackBlockingMult)),
                              Tr("- Higher: power-attacks a blocking target more\n"
                                 "- Breaks the block")));
        s.rows.push_back(
            note(Row(Tr("Bash"), score(m.bashMult)), Tr("- Higher: bashes more, with a shield or a bash attack\n"
                                                        "- A bash can stagger")));
        s.rows.push_back(note(Row(Tr("Bash, Recoiled"), score(m.bashRecoilMult)),
                              Tr("- Higher: bashes a target recoiling from its blocked attack")));
        s.rows.push_back(
            note(Row(Tr("Bash, Attacking"), score(m.bashAttackMult)), Tr("- Higher: bashes a target mid-attack")));
        s.rows.push_back(note(Row(Tr("Bash, Power Attacking"), score(m.bashPowerAttackMult)),
                              Tr("- Higher: bashes a target mid-power-attack")));
        out.push_back(std::move(s));
    }
    {
        // Only the active pair: dueling circles and falls back, flanking
        // keeps a distance and stalks. The other pair is dead data.
        const auto &c = live->closeRangeData;
        SheetSection s{Tr("Range"), {}, {}};
        if (flanking)
        {
            s.rows.push_back(
                note(Row(Tr("Flank Distance"), chance(c.flankDistanceMult)), Tr("- Distance kept while flanking")));
            s.rows.push_back(
                note(Row(Tr("Stalk Time"), chance(c.stalkTimeMult)), Tr("- Time spent flanking before attacking")));
        }
        else
        {
            s.rows.push_back(note(Row(Tr("Circle"), chance(c.circleMult)), Tr("- Higher: circles the target more")));
            s.rows.push_back(note(Row(Tr("Fallback"), chance(c.fallbackMult)), Tr("- Chance to back off")));
        }
        s.rows.push_back(note(Row(Tr("Strafe"), chance(live->longRangeData.strafeMult)),
                              Tr("- Higher: strafes more to dodge projectiles at range")));
        out.push_back(std::move(s));
    }
    return out;
}

// The entry points by number, from the engine's enum (BGSEntryPoint.h),
// in the Creation Kit's words: "Mod Attack Damage", "Mod Spell Cost".
#include "game/ConditionNames.inc"

std::string HexId(std::uint32_t id)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", id);
    return text;
}

// The engine's own entry for a condition function, which says what each
// parameter is: the script command table, indexed by the function's id.
// Null past the table's end, or where the entry's name is not ours for the
// id, which would mean the table is not laid out as assumed: said once per
// id, and the parameters are then not read as anything.
const RE::SCRIPT_FUNCTION *ConditionCommand(std::size_t id, const char *name)
{
    auto *first = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
    if (!first || id >= RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd)
        return nullptr;
    const RE::SCRIPT_FUNCTION &command = first[id];
    if (name && command.functionName && _stricmp(command.functionName, name) == 0)
        return &command;
    static std::unordered_set<std::size_t> said;
    if (said.insert(id).second)
        log::sensors.warn(
            "condition function {}: the engine's table names it {}, ours {} -- its parameters are not read", id,
            command.functionName ? command.functionName : "nothing", name ? name : "nothing");
    return nullptr;
}

// Does a parameter of this type hold a form, a record or a reference? The
// types that certainly do; every other -- a number, an actor value, a
// script variable's name, a runtime object -- is never read as one.
bool HoldsForm(RE::SCRIPT_PARAM_TYPE type)
{
    using T = RE::SCRIPT_PARAM_TYPE;
    switch (type)
    {
    case T::kInventoryObject:
    case T::kObjectRef:
    case T::kActor:
    case T::kSpellItem:
    case T::kCell:
    case T::kMagicItem:
    case T::kSound:
    case T::kTopic:
    case T::kQuest:
    case T::kRace:
    case T::kClass:
    case T::kFaction:
    case T::kGlobal:
    case T::kFurnitureOrFormList:
    case T::kObject:
    case T::kMapMarker:
    case T::kActorBase:
    case T::kContainerRef:
    case T::kWorldOrList:
    case T::kPackage:
    case T::kCombatStyle:
    case T::kMagicEffect:
    case T::kWeather:
    case T::kNPC:
    case T::kOwner:
    case T::kShaderEffect:
    case T::kFormList:
    case T::kPerk:
    case T::kImagespaceMod:
    case T::kImagespace:
    case T::kVoiceType:
    case T::kEncounterZone:
    case T::kIdleForm:
    case T::kMessage:
    case T::kInvObjectOrFormList:
    case T::kEquipType:
    case T::kObjectOrFormList:
    case T::kMusic:
    case T::kKeyword:
    case T::kRefType:
    case T::kLocation:
    case T::kForm:
    case T::kShout:
    case T::kWordOfPower:
    case T::kBGSScene:
    case T::kAssociationType:
    case T::kKnowableForm:
    case T::kRegion:
        return true;
    default:
        return false;
    }
}

// A condition's call, as the Creation Kit shows it: "HasSpell(Whirlwind
// Cloak)", "GetActorValue(Alteration)". What a parameter is comes from the
// engine's table: guessed from the value, any pointer taken for a form, it
// crashed the game on a player's spell whose condition held a pointer to
// something else (2026-09-13). Trailing zero parameters are dropped, as a
// function with no parameters holds zeros there. `subject` and `target`
// are what the parties are called after "on"; an empty Subject goes
// unsaid.
std::string ConditionCall(const RE::CONDITION_ITEM_DATA &data, const std::string &subject, const std::string &target)
{
    const auto id = static_cast<std::size_t>(data.functionData.function.get());
    const char *name = id < kConditionNames.size() && *kConditionNames[id] ? kConditionNames[id] : nullptr;
    std::string call = name ? name : TrFormat("Function {}", id);

    const RE::SCRIPT_FUNCTION *command = ConditionCommand(id, name);
    std::vector<std::string> args;
    for (std::size_t i = 0; i < std::size(data.functionData.params); ++i)
    {
        const void *param = data.functionData.params[i];
        const auto raw = reinterpret_cast<std::uintptr_t>(param);
        const bool typed = command && command->params && i < command->numParams;
        const RE::SCRIPT_PARAM_TYPE type = typed ? command->params[i].paramType.get() : RE::SCRIPT_PARAM_TYPE::kInt;
        if (typed && HoldsForm(type) && param)
        {
            // The form's name; a keyword has none, only an editor ID, which
            // its record keeps in memory; failing both, the ID. A perk goes
            // by its editor ID first: the ranks of one perk share a name,
            // and a rank's own entry is conditioned on the NEXT rank not
            // being held ("HasPerk(Augmented Frost) = 0" on Augmented
            // Frost read as nonsense until it said AugmentedFrost60).
            const auto *form = static_cast<const RE::TESForm *>(param);
            const char *formName = form->GetName();
            const char *editorID = form->GetFormEditorID();
            const bool byEditorID = form->Is(RE::FormType::Perk) || !formName || !*formName;
            args.push_back(form->IsPlayerRef()                   ? Tr("Player")
                           : byEditorID && editorID && *editorID ? editorID
                           : formName && *formName               ? formName
                                                                 : HexId(form->GetFormID()));
        }
        else if (typed && type == RE::SCRIPT_PARAM_TYPE::kActorValue)
        {
            auto *list = RE::ActorValueList::GetSingleton();
            auto *info = list ? list->GetActorValueInfo(static_cast<RE::ActorValue>(raw)) : nullptr;
            args.push_back(info && info->GetFullName() && *info->GetFullName() ? info->GetFullName()
                                                                               : std::to_string(raw));
        }
        else
        {
            // A pointer to what is not a form has nothing safe to print.
            args.push_back(raw > 0xFFFFFFFFu ? "?" : std::to_string(raw));
        }
    }
    while (!args.empty() && args.back() == "0")
        args.pop_back();
    call += "(";
    for (std::size_t i = 0; i < args.size(); ++i)
        call += (i ? ", " : "") + args[i];
    call += ")";

    using Object = RE::CONDITIONITEMOBJECT;
    switch (data.object.get())
    {
    case Object::kSelf:
        if (!subject.empty())
            call = TrFormat("{} on {}", call, subject);
        break;
    case Object::kTarget:
        call = TrFormat("{} on {}", call, target);
        break;
    case Object::kCombatTarget:
        call = TrFormat("{} on {}", call, Tr("Combat Target"));
        break;
    case Object::kRef: {
        // A particular reference, named in the condition: the player, as a
        // rule, for a perk given to followers that turns on with one of
        // the player's.
        const auto ref = data.runOnRef.get();
        if (ref && ref->IsPlayerRef())
            call = TrFormat("{} on {}", call, Tr("Player"));
        else if (ref && ref->GetDisplayFullName() && *ref->GetDisplayFullName())
            call = TrFormat("{} on {}", call, ref->GetDisplayFullName());
        else
            call = TrFormat("{} on {}", call, ref ? HexId(ref->GetFormID()) : std::string(Tr("Reference")));
        break;
    }
    case Object::kLinkedRef:
        call = TrFormat("{} on {}", call, Tr("Linked Reference"));
        break;
    case Object::kQuestAlias:
        call = TrFormat("{} on {}", call, Tr("Quest Alias"));
        break;
    case Object::kPackData:
        call = TrFormat("{} on {}", call, Tr("Package Data"));
        break;
    case Object::kEventData:
        call = TrFormat("{} on {}", call, Tr("Event Data"));
        break;
    case Object::kCommandTarget:
        call = TrFormat("{} on {}", call, Tr("Command Target"));
        break;
    }
    return call;
}

// A party by the name the panel gives it: the player is "Player", as
// everywhere in the panel.
std::string PartyName(RE::TESObjectREFR *ref)
{
    if (ref->IsPlayerRef())
        return Tr("Player");
    const char *name = ref->GetDisplayFullName();
    return name && *name ? name : HexId(ref->GetFormID());
}

// A condition list as rows: the call, the comparison ("== 1", "OR" after
// it where the list reads so), and a tick where the parties meet it now.
std::vector<SheetRow> ConditionRows(const RE::TESCondition &condition, const ConditionParties &parties, const char *on)
{
    std::vector<SheetRow> rows;
    for (const auto *item = condition.head; item; item = item->next)
    {
        const auto &data = item->data;
        // The comparison, in the enum's order: =, !=, >, >=, <, <=.
        constexpr std::array<const char *, 6> kOps{"=", "!=", ">", ">=", "<", "<="};
        const auto opIndex = static_cast<std::size_t>(data.flags.opCode);
        const char *op = opIndex < kOps.size() ? kOps[opIndex] : "?";
        std::string value;
        if (data.flags.global)
        {
            const auto *global = data.comparisonValue.g;
            value = global && global->GetFormEditorID() && *global->GetFormEditorID()
                        ? global->GetFormEditorID()
                        : (global ? HexId(global->GetFormID()) : "?");
            if (global)
                value += Fmt(" (%g)", global->value);
        }
        else
            value = Fmt("%g", data.comparisonValue.f);
        using Object = RE::CONDITIONITEMOBJECT;
        const auto object = data.object.get();
        // The party the engine runs it on (TESConditionItem::IsTrue,
        // dev/CONDITIONS.md 10): the Subject, or through the Subject its
        // combat target or linked reference; the Target; the swap flag
        // trading the two when both are there. A named reference needs
        // neither, and a quest alias, package data or a story event is
        // context no sheet has, asked for the false it gives.
        const bool swapped = data.flags.swapTarget && parties.subject && parties.target;
        bool onTarget = object == Object::kTarget;
        if (swapped && (object == Object::kSelf || object == Object::kTarget))
            onTarget = !onTarget;
        RE::TESObjectREFR *runsOn = onTarget ? parties.target : parties.subject;
        // The call names whom it runs on, so no hover is needed to see it;
        // the Creation Kit's word where there is nobody to name. A perk's
        // later tab keeps the words: its Subject is the entry's argument,
        // listed and not asked.
        std::string subject = on ? "" : parties.subject ? PartyName(parties.subject) : Tr("Subject");
        std::string target = on ? Tr("Target") : parties.target ? PartyName(parties.target) : Tr("Target");
        if (swapped && !on)
            std::swap(subject, target);
        const std::string call = ConditionCall(data, subject, target);
        SheetRow row = Row(on ? TrFormat("{} on {}", call, on) : call,
                           std::string(op) + " " + value + (data.flags.isOR ? "  OR" : ""));
        // Met only where the condition is on the actor: one on another
        // argument -- the spell, the weapon, the target -- has nothing to
        // be asked of here, and a tick from asking the actor would lie.
        if (!on)
        {
            const bool needsParty = object == Object::kSelf || object == Object::kTarget ||
                                    object == Object::kCombatTarget || object == Object::kLinkedRef ||
                                    object == Object::kCommandTarget;
            // N/A: a party it needs is not there -- nobody being fought,
            // the caster gone. ?: there, but the answer cannot be known
            // from a sheet: EffectWasDualCast reads a flag held only while
            // a dual-cast effect is being added (handler 21719), and is 0
            // afterwards whatever the cast was.
            if (needsParty && !runsOn)
                row.extra = Tr("N/A");
            else if (data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kEffectWasDualCast)
                row.extra = "?";
            else
            {
                RE::ConditionCheckParams params(parties.subject, parties.target);
                if (item->IsTrue(params))
                    row.icon = kGlyphTick;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

constexpr std::array<const char *, 92> kEntryPointNames{{
    "Calculate Weapon Damage",
    "Calculate My Critical Hit Chance",
    "Calculate My Critical Hit Damage",
    "Calculate Mine Explode Chance",
    "Adjust Limb Damage",
    "Adjust Book Skill Points",
    "Mod Recovered Health",
    "Get Should Attack",
    "Mod Buy Prices",
    "Add Leveled List On Death",
    "Get Max Carry Weight",
    "Mod Addiction Chance",
    "Mod Addiction Duration",
    "Mod Positive Chem Duration",
    "Activate",
    "Ignore Running During Detection",
    "Ignore Broken Lock",
    "Mod Enemy Critical Hit Chance",
    "Mod Sneak Attack Mult",
    "Mod Max Placeable Mines",
    "Mod Bow Zoom",
    "Mod Recover Arrow Chance",
    "Mod Skill Use",
    "Mod Telekinesis Distance",
    "Mod Telekinesis Damage Mult",
    "Mod Telekinesis Damage",
    "Mod Bashing Damage",
    "Mod Power Attack Stamina",
    "Mod Power Attack Damage",
    "Mod Spell Magnitude",
    "Mod Spell Duration",
    "Mod Secondary Value Weight",
    "Mod Armor Weight",
    "Mod Incoming Stagger",
    "Mod Target Stagger",
    "Mod Attack Damage",
    "Mod Incoming Damage",
    "Mod Target Damage Resistance",
    "Mod Spell Cost",
    "Mod Percent Blocked",
    "Mod Shield Deflect Arrow Chance",
    "Mod Incoming Spell Magnitude",
    "Mod Incoming Spell Duration",
    "Mod Player Intimidation",
    "Mod Player Reputation",
    "Mod Favor Points",
    "Mod Bribe Amount",
    "Mod Detection Light",
    "Mod Detection Movement",
    "Mod Soul Gem Recharge",
    "Set Sweep Attack",
    "Apply Combat Hit Spell",
    "Apply Bashing Spell",
    "Apply Reanimate Spell",
    "Set Boolean Graph Variable",
    "Mod Spell Casting Sound Event",
    "Mod Pickpocket Chance",
    "Mod Detection Sneak Skill",
    "Mod Falling Damage",
    "Mod Lockpick Sweet Spot",
    "Mod Sell Prices",
    "Can Pickpocket Equipped Item",
    "Mod Lockpick Level Allowed",
    "Set Lockpick Starting Arc",
    "Set Progression Picking",
    "Make Lockpicks Unbreakable",
    "Mod Alchemy Effectiveness",
    "Apply Weapon Swing Spell",
    "Mod Commanded Actor Limit",
    "Apply Sneaking Spell",
    "Mod Player Magic Slowdown",
    "Mod Ward Magicka Absorption Pct",
    "Mod Initial Ingredient Effects Learned",
    "Purify Alchemy Ingredients",
    "Filter Activation",
    "Can Dual Cast Spell",
    "Mod Tempering Health",
    "Mod Enchantment Power",
    "Mod Soul Pct Captured To Weapon",
    "Mod Soul Gem Enchanting",
    "Mod Number Applied Enchantments Allowed",
    "Set Activate Label",
    "Mod Shout OK",
    "Mod Poison Dose Count",
    "Should Apply Placed Item",
    "Mod Armor Rating",
    "Mod Lockpicking Crime Chance",
    "Mod Ingredients Harvested",
    "Mod Spell Range Target Loc",
    "Mod Potions Created",
    "Mod Lockpicking Key Reward Chance",
    "Allow Mount Actor",
}};

// One entry of a perk as a row: the entry point in the Creation Kit's
// words with what the function does to it, an ability by name, a quest
// by name and stage.
SheetRow EntryRow(const RE::BGSPerkEntry *entry)
{
    using Type = RE::PERK_ENTRY_TYPE;
    switch (entry->GetType())
    {
    case Type::kAbility: {
        const auto *ability = static_cast<const RE::BGSAbilityPerkEntry *>(entry);
        return Row(Tr("Ability"), NameOr(ability->ability, "?"));
    }
    case Type::kQuest:
        // The quest entry's record is not modelled in this CommonLibSSE
        // fork; the kind is all that can be said.
        return Row(Tr("Quest"), Tr("a stage set"));
    case Type::kEntryPoint: {
        const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
        const auto index = static_cast<std::size_t>(point->entryData.entryPoint.get());
        const char *name = index < kEntryPointNames.size() ? kEntryPointNames[index] : "?";
        using Function = RE::BGSEntryPointPerkEntry::Function;
        using DataType = RE::BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA;
        const auto *data = point->functionData;
        const auto dataType = data ? data->GetType() : DataType::kInvalid;
        const float one = dataType == DataType::kOneValue
                              ? static_cast<const RE::BGSEntryPointFunctionDataOneValue *>(data)->data
                              : 0.0f;
        // The two-value record: a range's ends, or for the actor-value
        // functions the value read and its multiplier -- named, so the
        // page and the breakdown that reads the same entry agree on what
        // "One Handed Power Mod x 0.01" is (2026-09-13).
        const float *two =
            dataType == DataType::kTwoValue ? reinterpret_cast<const TwoValueData *>(data)->data : nullptr;
        const std::string share =
            two ? Fmt("%g", two[1]) + " x " + ValueName(static_cast<RE::ActorValue>(static_cast<int>(two[0])))
                : std::string(Tr("a share of an actor value"));
        std::string value;
        switch (point->entryData.function.get())
        {
        case Function::kSetValue:
            value = "= " + Fmt("%g", one);
            break;
        case Function::kAddValue:
            value = Fmt("%+g", one);
            break;
        case Function::kMultiplyValue:
            value = "x " + Fmt("%g", one);
            break;
        case Function::kAddRangeToValue:
            value = two ? TrFormat("+ {} to {}", Fmt("%g", two[0]), Fmt("%g", two[1])) : std::string(Tr("+ a range"));
            break;
        case Function::kAddActorValueMult:
            value = "+ " + share;
            break;
        case Function::kAddLeveledList:
            value = Tr("a leveled list");
            break;
        case Function::kAddActivateChoice:
            value = Tr("an activate choice");
            break;
        case Function::kSetToActorValueMult:
            value = "= " + share;
            break;
        case Function::kMultiplyActorValueMult:
            value = "x " + share;
            break;
        case Function::kMultiplyOnePlusActorValueMult:
            value = "x (1 + " + share + ")";
            break;
        case Function::kSetText:
            value = Tr("a text");
            break;
        default:
            break;
        }
        if (dataType == DataType::kSpellItem)
        {
            const auto *spell = static_cast<const RE::BGSEntryPointFunctionDataSpellItem *>(data)->spell;
            value = NameOr(spell, Tr("a spell"));
        }
        return Row(name, value);
    }
    default:
        return Row(Tr("Entry"), "?");
    }
}

namespace
{
// A perk held that the skill trees' walk does not find, with its rank.
struct HeldPerk
{
    RE::BGSPerk *perk;
    int rank;
};

// The perks outside the trees. An NPC's are on the base record's list,
// where a distributor puts them too. The player's taken in play are on the
// player, not the record, in an array whose offset CommonLib marks as
// guessed on this runtime; so for the player every perk in the load order
// is asked of the engine instead, a few thousand lookups. Hidden perks stay
// out: the skill-boost perks every actor carries.
std::vector<HeldPerk> PerksOutsideTrees(RE::Actor *actor)
{
    std::vector<HeldPerk> out;
    const auto held = [actor](RE::BGSPerk *perk) { return perk && !perk->data.hidden && actor->HasPerk(perk); };
    if (actor->IsPlayerRef())
    {
        auto *handler = RE::TESDataHandler::GetSingleton();
        if (!handler)
            return out;
        for (auto *perk : handler->GetFormArray<RE::BGSPerk>())
        {
            // One row per chain, at the highest rank held, as the trees do.
            if (held(perk) && !(perk->nextPerk && actor->HasPerk(perk->nextPerk)))
                out.push_back({perk, 1});
        }
        return out;
    }
    if (const auto *base = actor->GetActorBase(); base && base->perks)
        for (std::uint32_t i = 0; i < base->perkCount; ++i)
            if (held(base->perks[i].perk))
                out.push_back({base->perks[i].perk, base->perks[i].currentRank});
    return out;
}
} // namespace

namespace
{

// A skill's tree as the menu draws it, walked once per skill and kept, as
// TreePerks is: the positions, links, names, descriptions and requirements
// are the load order's. The actor's part, which ranks they hold, is read per
// page. The root, which names no perk, is left out with its links.
struct TreeShape
{
    struct Node
    {
        std::string name;
        double x{0.0};
        double y{0.0};
        std::vector<RE::BGSPerk *> ranks;
        std::vector<std::string> descriptions; // a rank's each
        std::vector<float> requirements;
        std::vector<std::size_t> children;
    };
    std::vector<Node> nodes;
};

const TreeShape &ShapeOf(RE::ActorValue skill)
{
    static std::unordered_map<RE::ActorValue, TreeShape> cache;
    if (const auto it = cache.find(skill); it != cache.end())
        return it->second;

    TreeShape shape;
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValueInfo(skill) : nullptr;
    if (info && info->perkTree)
    {
        // Every node that names a perk, once, each given its index here.
        std::vector<RE::BGSSkillPerkTreeNode *> nodes;
        std::unordered_map<const RE::BGSSkillPerkTreeNode *, std::size_t> index;
        std::unordered_set<const RE::BGSSkillPerkTreeNode *> seen;
        std::vector<RE::BGSSkillPerkTreeNode *> stack{info->perkTree};
        while (!stack.empty())
        {
            auto *node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second)
                continue;
            if (node->perk)
            {
                index.emplace(node, nodes.size());
                nodes.push_back(node);
            }
            for (auto *child : node->children)
                stack.push_back(child);
        }
        for (auto *node : nodes)
        {
            TreeShape::Node out;
            out.name = PerkName(node->perk);
            out.x = static_cast<double>(node->perkGridX) + node->horizontalPosition;
            out.y = static_cast<double>(node->perkGridY) + node->verticalPosition;
            out.ranks = ft::RankChain(node->perk, [](RE::BGSPerk *rank) { return rank->nextPerk; });
            for (RE::BGSPerk *rank : out.ranks)
            {
                RE::BSString text;
                rank->GetDescription(text, rank);
                out.descriptions.emplace_back(text.c_str() ? text.c_str() : "");
                out.requirements.push_back(SkillRequirement(rank, skill));
            }
            for (auto *child : node->children)
                if (const auto it = index.find(child); it != index.end())
                    out.children.push_back(it->second);
            shape.nodes.push_back(std::move(out));
        }
    }
    return cache.emplace(skill, std::move(shape)).first->second;
}

} // namespace

std::vector<ft::PerkTreeView> BuildPerkTrees(RE::Actor *actor)
{
    std::vector<ft::PerkTreeView> out;
    auto *list = RE::ActorValueList::GetSingleton();
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!list || !owner)
        return out;
    for (int i = 0; i < static_cast<int>(RE::ActorValue::kTotal); ++i)
    {
        const auto value = static_cast<RE::ActorValue>(i);
        auto *info = list->GetActorValueInfo(value);
        if (!info || !info->skill)
            continue;
        const TreeShape &shape = ShapeOf(value);
        if (shape.nodes.empty())
            continue;
        ft::PerkTreeView tree;
        tree.key = static_cast<std::uint32_t>(i) + 1;
        const char *name = info->GetFullName();
        tree.name = name && *name ? name : (info->enumName ? info->enumName : "?");
        tree.level = actor->IsPlayerRef() ? owner->GetBaseActorValue(value) : owner->GetPermanentActorValue(value);
        tree.current = owner->GetActorValue(value);
        tree.value = Fmt("%.0f", tree.level);
        // Their own record's perks, held or not: what was chosen for them.
        std::unordered_set<const RE::BGSPerk *> record;
        if (const auto *npc = actor->GetActorBase(); npc && npc->perks)
            for (std::uint32_t k = 0; k < npc->perkCount; ++k)
                record.insert(npc->perks[k].perk);
        for (const TreeShape::Node &node : shape.nodes)
        {
            ft::PerkTreeNode n;
            n.name = node.name.empty() ? "?" : node.name;
            n.x = node.x;
            n.y = node.y;
            n.ranks = static_cast<int>(node.ranks.size());
            if (!node.ranks.empty())
                n.firstForm = node.ranks.front()->GetFormID();
            if (!node.requirements.empty())
                n.firstRequirement = node.requirements.front();
            for (RE::BGSPerk *rank : node.ranks)
            {
                if (actor->HasPerk(rank))
                {
                    ++n.held;
                    n.form = rank->GetFormID(); // the top rank held, as the perk rows name it
                }
                n.theirs = n.theirs || record.contains(rank);
            }
            const std::size_t shown = static_cast<std::size_t>((std::min)(n.held, n.ranks - 1));
            n.requirement = node.requirements[shown];
            n.description = node.descriptions[shown];
            n.children = node.children;
            tree.nodes.push_back(std::move(n));
        }
        out.push_back(std::move(tree));
    }
    return out;
}

std::vector<PerkPage> BuildPerkPages(RE::Actor *actor)
{
    std::vector<PerkPage> out;
    if (!actor)
        return out;
    std::unordered_set<const RE::BGSPerk *> seen;
    // A page for a perk held: its id, its rank in its chain, the skill
    // whose tree it sits in, and what it does, entry by entry.
    const auto page = [&](RE::BGSPerk *perk, int rank, int ranks, const std::string &skill) {
        if (!perk || !seen.insert(perk).second)
            return;
        PerkPage p;
        p.form = perk->GetFormID();
        p.name = PerkName(perk);
        if (p.name.empty())
            p.name = "?";
        RE::BSString text;
        perk->GetDescription(text, perk);
        p.description = text.c_str() ? text.c_str() : "";

        SheetSection info{Tr("Perk Details"), {}, {}};
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", perk->GetFormID());
        info.rows.push_back(Row(Tr("Base ID"), id));
        if (ranks > 1)
            info.rows.push_back(Row(Tr("Rank"), std::to_string(rank) + " / " + std::to_string(ranks)));
        if (!skill.empty())
            info.rows.push_back(Row(Tr("Skill"), skill));
        if (perk->data.hidden)
            info.rows.push_back(Row(Tr("Hidden"), Tr("yes")));
        // A tick while the perk does something for them; no row while not.
        if (PerkActive(actor, perk))
        {
            SheetRow active = Row(Tr("Active"), "");
            active.icon = kGlyphTick;
            info.rows.push_back(std::move(active));
        }
        // Who asks for it, for a marker; and for a perk with no entries
        // that nobody asks for, that it does nothing, and whose record
        // left it so -- the last plugin to touch it.
        if (const auto &readers = PerkReaders(perk); !readers.empty())
        {
            std::string who;
            for (std::size_t i = 0; i < readers.size() && i < 4; ++i)
                who += (who.empty() ? "" : Tr(", ")) + readers[i];
            if (readers.size() > 4)
                who += TrFormat(", +{}", readers.size() - 4);
            info.rows.push_back(Row(Tr("Read by"), who));
        }
        p.sections.push_back(std::move(info));

        // The effects: an entry each, with the conditions that gate it on
        // its owner beneath -- a mod's perk given to everyone is gated
        // there, on the power that turns it on -- greyed while they are
        // not met, as an effect's row is. Not the record's own conditions,
        // which are what the skill tree asks before the player may take
        // it, and nothing to an NPC.
        SheetSection effects{Tr("Effects"), {}, {}};
        for (const auto *entry : perk->perkEntries)
        {
            if (!entry)
                continue;
            SheetRow row = EntryRow(entry);
            bool active = true;
            if (entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint)
            {
                // Tab 0 is on the owner and decides `active`; the other
                // tabs are on the entry point's further arguments (Mod
                // Spell Magnitude's second is the spell), which the library
                // numbers and does not name. They are listed as such,
                // unevaluated.
                const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
                if (point->conditions.size() > 0 && point->conditions[0])
                {
                    row.detail = ConditionRows(point->conditions[0], {actor, actor});
                    active = point->conditions[0].IsTrue(actor, actor);
                }
                for (std::uint32_t tab = 1; tab < point->conditions.size(); ++tab)
                {
                    if (!point->conditions[tab])
                        continue;
                    const std::string on = TrFormat("argument {}", tab + 1);
                    for (SheetRow &r : ConditionRows(point->conditions[tab], {actor, actor}, on.c_str()))
                        row.detail.push_back(std::move(r));
                }
            }
            if (!active)
                row.aside = Tr("Conditions not met");
            effects.rows.push_back(std::move(row));
        }
        // By name, the record's order being the author's; two of one name
        // keep their order, the weaker first as a rule.
        std::stable_sort(effects.rows.begin(), effects.rows.end(),
                         [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        // A perk with no entries shows the table all the same, one row of
        // N/A beside the description below it, so a description promising
        // 60% over an effect of nothing is seen as the mismatch it is
        // (Adamant's stub of Armsman's third rank, 2026-09-13); the hover
        // names the plugin that last changed the record.
        if (effects.rows.empty())
        {
            SheetRow row = Row(Tr("Effect"), Tr("N/A"));
            if (const auto *file = perk->GetFile(); file && !file->GetFilename().empty())
                row.note = TrFormat("Record last changed by {}", file->GetFilename());
            effects.rows.push_back(std::move(row));
        }
        p.sections.push_back(std::move(effects));
        out.push_back(std::move(p));
    };

    if (auto *list = RE::ActorValueList::GetSingleton())
    {
        for (int i = 0; i < static_cast<int>(RE::ActorValue::kTotal); ++i)
        {
            const auto value = static_cast<RE::ActorValue>(i);
            auto *info = list->GetActorValueInfo(value);
            if (!info || !info->skill)
                continue;
            const char *skillName = info->GetFullName();
            // The top rank held; and a perk not held at all, by its first
            // rank, since the skill page's names open any perk's page.
            for (const TreePerk &entry : TreePerks(value))
            {
                const bool held = actor->HasPerk(entry.perk);
                if (held ? entry.perk->nextPerk && actor->HasPerk(entry.perk->nextPerk) : entry.rank != 1)
                    continue;
                page(entry.perk, entry.rank, entry.ranks, skillName ? skillName : "");
            }
        }
    }
    for (const CustomSkillTree &tree : CustomSkillTrees())
    {
        for (const CustomSkillPerk &entry : tree.perks)
            if (TopRankHeld(actor, entry.perk))
                page(entry.perk, entry.rank, entry.ranks, tree.name);
    }
    for (const HeldPerk &held : PerksOutsideTrees(actor))
        page(held.perk, held.rank, 1, "");
    return out;
}

std::vector<SheetSection> BuildSkillSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return out;

    const auto av = [owner](RE::ActorValue value) { return owner->GetActorValue(value); };

    // What a skill's two modifier values do, read from the game's own records
    // rather than a wiki (dev/RESEARCH.md, "Skill modifiers").
    //
    // Every actor carries two hidden perks, PerkSkillBoosts and
    // AlchemySkillBoosts in Skyrim.esm. Each multiplies ONE game quantity by
    // (1 + 0.01 * value): the first reads <skill>Modifier, which Fortify
    // enchantments and perks write; the second reads <skill>PowerModifier,
    // which Fortify potions write. For every skill but the magic schools both
    // perks hit the SAME quantity -- one-handed damage, percent blocked,
    // pickpocket chance -- so the two values are one bonus and are shown as
    // one: "+35% damage". For a school they differ: the modifier cuts spell
    // cost, the power modifier raises magnitude (Destruction, Illusion,
    // Restoration) or duration (Alteration, Conjuration), so both are shown,
    // as what spells now are: "83% cost, 200% magnitude".
    //
    // Enchanting appears in neither perk: Fortify Enchanting writes the skill
    // itself. So do Fortify Heavy Armor and Fortify Light Armor -- the perks
    // would honour those two modifiers as a cut to damage taken, but nothing
    // in the base game sets them, so on a vanilla install they stay at zero
    // and the row stays plain.
    //
    // Brackets appear only when a value is off zero, which for a follower is
    // rare. The tooltip says where the number came from.
    struct Modifier
    {
        RE::ActorValue value;
        const char *effect; // nullptr: nothing reads this value
        int sign;           // +1: each point raises the quantity; -1: lowers it
    };
    struct Skill
    {
        const char *label;
        RE::ActorValue value;
        Modifier mod;   // Fortify enchantments and perks
        Modifier power; // Fortify potions
    };
    using AV = RE::ActorValue;
    constexpr Modifier none{AV::kNone, nullptr, 0};

    const auto skill = [&](SheetSection &s, const Skill &k) {
        SheetRow row = Row(k.label, Fmt("%.0f", av(k.value)));
        // Only what applies. A follower has Fortify One-handed +35 on the
        // value and no perk to turn it into damage; a bonus that changes
        // nothing is not shown.
        const float m = k.mod.effect && ReadsSkillMods(actor) ? av(k.mod.value) : 0.0f;
        const float p = k.power.effect && ReadsSkillPowerMods(actor) ? av(k.power.value) : 0.0f;

        // Every modifier is a signed change from normal: "+90% damage",
        // "-17% cost", each its own figure with its own breakdown: the
        // sources by name -- the gauntlets, the potion -- and what no
        // effect explains as Other. Power first, then the other, as the
        // two read best.
        const auto part = [&](const std::string &text, std::initializer_list<std::pair<const Modifier *, float>> from,
                              double total) {
            SheetRow::ModifierPart piece;
            piece.text = text;
            piece.breakdown.unit = "%";
            for (const auto &[mod, amount] : from)
            {
                if (amount == 0.0f)
                    continue;
                AddValueLines(piece.breakdown, PartsOf(actor, mod->value), static_cast<float>(mod->sign));
            }
            piece.breakdown.total = total;
            ft::Close(piece.breakdown);
            row.modifiers += (row.modifiers.empty() ? "" : Tr(", ")) + text;
            row.modifierParts.push_back(std::move(piece));
        };
        if (k.mod.effect && k.power.effect && std::string_view(k.mod.effect) == k.power.effect)
        {
            // One quantity, two factors: multiply them and show the change.
            // The sources' sum misses the product by their cross term, an
            // Other line.
            if (m != 0.0f || p != 0.0f)
            {
                const double change = ((1.0 + k.mod.sign * m / 100.0) * (1.0 + k.power.sign * p / 100.0) - 1.0) * 100.0;
                part(TrFormat("{}% {}", Fmt("%+.0f", change), Tr(k.mod.effect)), {{&k.power, p}, {&k.mod, m}}, change);
            }
        }
        else
        {
            if (p != 0.0f)
                part(TrFormat("{}% {}", Fmt("%+.0f", k.power.sign * p), Tr(k.power.effect)), {{&k.power, p}},
                     k.power.sign * p);
            if (m != 0.0f)
                part(TrFormat("{}% {}", Fmt("%+.0f", k.mod.sign * m), Tr(k.mod.effect)), {{&k.mod, m}}, k.mod.sign * m);
        }

        // The Armor Perks value, which the engine adds to either armour
        // skill's multiplier for every piece worn (dev/MODIFIERS.md): on
        // both rows, with what set it on hover.
        if (k.value == AV::kHeavyArmor || k.value == AV::kLightArmor)
        {
            if (const float perks = av(AV::kArmorPerks); perks != 0.0f)
            {
                SheetRow::ModifierPart piece;
                piece.text = TrFormat("{} skill multiplier", Fmt("%+.2f", perks));
                piece.breakdown = ValueBreakdown(actor, AV::kArmorPerks, "");
                piece.breakdown.decimals = 2;
                row.modifiers += (row.modifiers.empty() ? "" : Tr(", ")) + piece.text;
                row.modifierParts.push_back(std::move(piece));
            }
        }

        row.detail = OwnedPerks(actor, k.value);
        if (!ShapeOf(k.value).nodes.empty())
            row.tree = static_cast<std::uint32_t>(k.value) + 1; // BuildPerkTrees' key
        // A skill at zero with no perk in it -- Vampire Lord on a mortal --
        // says nothing; a section of those says nothing either.
        if (av(k.value) == 0.0f && row.detail.empty() && row.modifiers.empty())
            return;
        s.rows.push_back(std::move(row));
    };

    // What the two modifier values of each vanilla skill do. No record
    // links a skill to them, so this is a table; a skill a mod adds gets
    // none, and its row shows the level alone.
    struct Pair
    {
        Modifier mod;
        Modifier power;
    };
    static const std::unordered_map<AV, Pair> kPairs{
        {AV::kOneHanded, {{AV::kOneHandedModifier, N_("damage"), +1}, {AV::kOneHandedPowerModifier, N_("damage"), +1}}},
        {AV::kTwoHanded, {{AV::kTwoHandedModifier, N_("damage"), +1}, {AV::kTwoHandedPowerModifier, N_("damage"), +1}}},
        {AV::kBlock, {{AV::kBlockModifier, N_("blocked"), +1}, {AV::kBlockPowerModifier, N_("blocked"), +1}}},
        {AV::kSmithing,
         {{AV::kSmithingModifier, N_("tempering"), +1}, {AV::kSmithingPowerModifier, N_("tempering"), +1}}},
        {AV::kHeavyArmor,
         {{AV::kHeavyArmorModifier, N_("damage"), -1}, {AV::kHeavyArmorPowerModifier, N_("damage"), -1}}},
        {AV::kLightArmor,
         {{AV::kLightArmorModifier, N_("damage"), -1}, {AV::kLightArmorPowerModifier, N_("damage"), -1}}},
        {AV::kArchery, {{AV::kMarksmanModifier, N_("damage"), +1}, {AV::kMarksmanPowerModifier, N_("damage"), +1}}},
        {AV::kPickpocket,
         {{AV::kPickpocketModifier, N_("chance"), +1}, {AV::kPickpocketPowerModifier, N_("chance"), +1}}},
        {AV::kLockpicking,
         {{AV::kLockpickingModifier, N_("sweet spot"), +1}, {AV::kLockpickingPowerModifier, N_("sweet spot"), +1}}},
        {AV::kSneak, {{AV::kSneakingModifier, N_("stealth"), +1}, {AV::kSneakingPowerModifier, N_("stealth"), +1}}},
        {AV::kAlchemy,
         {{AV::kAlchemyModifier, N_("potion strength"), +1}, {AV::kAlchemyPowerModifier, N_("potion strength"), +1}}},
        // Sell prices up and buy prices down by the same factor: "better prices".
        {AV::kSpeech,
         {{AV::kSpeechcraftModifier, N_("better prices"), +1},
          {AV::kSpeechcraftPowerModifier, N_("better prices"), +1}}},
        {AV::kAlteration,
         {{AV::kAlterationModifier, N_("cost"), -1}, {AV::kAlterationPowerModifier, N_("duration"), +1}}},
        {AV::kConjuration,
         {{AV::kConjurationModifier, N_("cost"), -1}, {AV::kConjurationPowerModifier, N_("duration"), +1}}},
        {AV::kDestruction,
         {{AV::kDestructionModifier, N_("cost"), -1}, {AV::kDestructionPowerModifier, N_("damage"), +1}}},
        {AV::kIllusion, {{AV::kIllusionModifier, N_("cost"), -1}, {AV::kIllusionPowerModifier, N_("magnitude"), +1}}},
        {AV::kRestoration,
         {{AV::kRestorationModifier, N_("cost"), -1}, {AV::kRestorationPowerModifier, N_("healing"), +1}}},
        {AV::kEnchanting, {none, none}},
    };

    // The skills themselves are read off the records, not a list here:
    // every actor value with a skill block, under the category its record
    // carries (CNAM: 1 combat, 2 magic, 3 stealth -- which puts Archery
    // with the warriors and Alchemy with the mages, as the game's own
    // constellations do), by name within it. A mod that retunes, renames
    // or re-trees a skill is read as it stands; one that adds a skill
    // block to another value gets a row, without modifiers.
    struct Found
    {
        std::string name;
        AV value;
        std::uint32_t category;
    };
    std::vector<Found> found;
    if (auto *list = RE::ActorValueList::GetSingleton())
    {
        for (int i = 0; i < static_cast<int>(AV::kTotal); ++i)
        {
            const auto value = static_cast<AV>(i);
            auto *info = list->GetActorValueInfo(value);
            if (!info || !info->skill)
                continue;
            const char *name = info->GetFullName();
            // The record's category (1 combat, 2 magic, 3 stealth) is the
            // constellation, with one exception: Alchemy's record says
            // Magic, and the skill menu, the Thief Stone and every player
            // put it under the Thief. The constellation wins.
            const std::uint32_t category = value == AV::kAlchemy ? 3u : info->unk124;
            found.push_back({name && *name ? name : (info->enumName ? info->enumName : "?"), value, category});
        }
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.name < b.name; });

    struct Category
    {
        std::uint32_t code;
        const char *title;
    };
    constexpr Category kCategories[] = {
        {1, N_("Warrior")}, {3, N_("Thief")}, {2, N_("Magic")}, {0, N_("Other Skills")}};
    for (const Category &category : kCategories)
    {
        SheetSection s{Tr(category.title), {}, {}};
        for (const Found &f : found)
        {
            const bool here = category.code == 0 ? (f.category != 1 && f.category != 2 && f.category != 3)
                                                 : f.category == category.code;
            if (!here)
                continue;
            const auto pair = kPairs.find(f.value);
            skill(s, {f.name.c_str(), f.value, pair != kPairs.end() ? pair->second.mod : none,
                      pair != kPairs.end() ? pair->second.power : none});
        }
        // Custom Skills Framework's trees sit with the other skills: a row
        // each with its perks held, and its level where the tree keeps one,
        // for the player alone, whose level the framework's globals are. A
        // tree with nothing to show says nothing, as a vanilla skill at zero.
        if (category.code == 0)
        {
            for (const CustomSkillTree &tree : CustomSkillTrees())
            {
                const bool level = tree.level && actor->IsPlayerRef();
                SheetRow row = Row(tree.name, level ? Fmt("%.0f", tree.level->value) : std::string{});
                row.detail = OwnedPerks(actor, tree);
                if (!row.detail.empty() || (level && tree.level->value > 0.0f))
                    s.rows.push_back(std::move(row));
            }
        }
        // By name within a section, the custom trees among the game's own.
        std::stable_sort(s.rows.begin(), s.rows.end(),
                         [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        if (!s.rows.empty())
            out.push_back(std::move(s));
    }

    // The perks held that sit in no skill's tree: a mod's loose perk, a
    // race's, a quest's. The unnamed stay out.
    {
        std::unordered_set<const RE::BGSPerk *> inTrees;
        for (const Found &f : found)
            for (const TreePerk &entry : TreePerks(f.value))
                inTrees.insert(entry.perk);
        for (const CustomSkillTree &tree : CustomSkillTrees())
            for (const CustomSkillPerk &entry : tree.perks)
                inTrees.insert(entry.perk);
        SheetSection s{Tr("Other Perks"), {}, {}};
        for (const HeldPerk &held : PerksOutsideTrees(actor))
        {
            if (inTrees.contains(held.perk))
                continue;
            const std::string name = PerkName(held.perk);
            if (name.empty())
                continue;
            RE::BSString text;
            held.perk->GetDescription(text, held.perk);
            SheetRow row = Row(name, held.rank > 1 ? std::to_string(held.rank) : std::string());
            row.modifiers = text.c_str() ? text.c_str() : "";
            row.form = held.perk->GetFormID();
            if (const char *aside = PerkAside(actor, held.perk))
                row.aside = aside;
            s.rows.push_back(std::move(row));
        }
        std::sort(s.rows.begin(), s.rows.end(), [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        if (!s.rows.empty())
            out.push_back(std::move(s));
    }

    return out;
}

namespace
{
// An effect's time left, written out as the engine made its duration: the
// record's duration; a dual cast (id 34058); the caster's Mod Spell Duration
// entries given the spell and the target, then the target's Mod Incoming
// Spell Duration given the spell (ActiveEffect::AdjustForPerks, id 34053);
// less the time run (dev/MODIFIERS.md). Whatever else moves a duration is
// not read, and shows as Other.
ft::Breakdown RemainingBreakdown(const RE::ActiveEffect &effect)
{
    ft::Breakdown b;
    if (!effect.effect || effect.duration <= 0.0f)
        return b;
    b.unit = Tr(" s");
    b.totalLabel = Tr("Remaining");
    ft::Start(b, Tr("Base"), static_cast<float>(effect.effect->effectItem.duration));
    const auto caster = effect.GetCasterActor();
    // A dual cast's effectiveness as id 26518 makes it: the base setting
    // plus the mult setting times the spell's cost for the caster, 2.5 and
    // 0 in Nordic Souls, 2.2 and 0 in vanilla. It scales the duration where
    // the record has Power Affects Duration, Scrambled Bugs' magicEffectFlags
    // reading, which Nordic Souls runs; vanilla's own body scales an effect
    // with No Magnitude too. The engine skips an effectiveness of 1 or
    // below 0.
    const auto *base = effect.effect->baseEffect;
    if (effect.flags.any(RE::ActiveEffect::Flag::kDual) && base &&
        base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kPowerAffectsDuration))
    {
        const float fixed = GameSetting("fMagicDualCastingEffectivenessBase", 2.2f);
        const float perCost = GameSetting("fMagicDualCastingEffectivenessMult", 0.0f);
        const float cost = perCost != 0.0f && effect.spell ? effect.spell->CalculateMagickaCost(caster.get()) : 0.0f;
        const float effectiveness = fixed + perCost * cost;
        if (effectiveness >= 0.0f && effectiveness != 1.0f)
        {
            ft::BreakdownLine &line = ft::Multiply(b, Tr("Dual cast"), effectiveness);
            if (perCost != 0.0f)
                line.amountText = "x (" + Fmt("%g", fixed) + " + " + Fmt("%g", perCost) + " x " + Fmt("%g", cost) + ")";
        }
    }
    // Not ActiveEffect::GetTargetActor: CommonLib reinterpret_casts the
    // MagicTarget base to Actor, a pointer 0xA0 inside the actor, and the
    // perk check handed it crashed calling a virtual through it
    // (2026-09-13). The target's own accessor gives the reference.
    auto *targetRef = effect.target ? effect.target->GetTargetStatsObject() : nullptr;
    auto *target = targetRef ? targetRef->As<RE::Actor>() : nullptr;
    if (caster)
        AddEntryPointLines(b, caster.get(), RE::BGSEntryPoint::ENTRY_POINT::kModSpellDuration, {effect.spell, target});
    if (target)
        AddEntryPointLines(b, target, RE::BGSEntryPoint::ENTRY_POINT::kModIncomingSpellDuration, {effect.spell});
    ft::Add(b, Tr("Elapsed"), -effect.elapsedSeconds);
    b.total = (std::max)(0.0f, effect.duration - effect.elapsedSeconds);
    ft::Close(b);
    return b;
}
} // namespace

std::vector<SummonView> ScanSummons(RE::Actor *actor)
{
    std::vector<SummonView> out;
    const auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    if (!process || !process->middleHigh)
        return out;
    for (const auto &commanded : process->middleHigh->commandedActors)
    {
        auto summon = commanded.commandedActor.get();
        if (!summon)
            continue;
        SummonView view;
        view.id = summon->GetFormID();
        view.baseId = summon->GetActorBase() ? summon->GetActorBase()->GetFormID() : 0;
        view.name = NameOr(summon.get(), "?");
        view.level = summon->GetLevel();
        view.health = ReadStat(summon.get(), RE::ActorValue::kHealth);
        view.magicka = ReadStat(summon.get(), RE::ActorValue::kMagicka);
        view.stamina = ReadStat(summon.get(), RE::ActorValue::kStamina);
        view.healthBreakdown = ValueBreakdown(summon.get(), RE::ActorValue::kHealth, "");
        view.magickaBreakdown = ValueBreakdown(summon.get(), RE::ActorValue::kMagicka, "");
        view.staminaBreakdown = ValueBreakdown(summon.get(), RE::ActorValue::kStamina, "");
        // The commanding effect runs on the FOLLOWER: its duration less its
        // elapsed time is how long the summon has left. A reanimate's effect
        // is a ReanimateEffect; a summon's a SummonCreatureEffect.
        if (const auto *effect = commanded.activeEffect)
        {
            if (effect->duration > 0.0f)
            {
                view.remaining = (std::max)(0.0f, effect->duration - effect->elapsedSeconds);
                view.remainingBreakdown = RemainingBreakdown(*effect);
            }
            view.raised = effect->GetBaseObject() &&
                          effect->GetBaseObject()->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate;
        }
        view.sheet = BuildCharacterSheet(summon.get());
        out.push_back(std::move(view));
    }
    return out;
}

bool TwoHanded(const RE::TESObjectWEAP *weapon)
{
    return weapon &&
           (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow());
}

namespace
{
// The first extra list of the actor's entry for the object that `pick`
// accepts. A player's enchantment, tempering, poison and charge live on
// the INSTANCE's list, not the record, and each copy worn has its own.
RE::ExtraDataList *ListOf(RE::Actor *actor, RE::TESBoundObject *object, auto pick)
{
    auto *changes = actor && object ? actor->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList)
        return nullptr;
    for (auto *entry : *changes->entryList)
    {
        if (!entry || entry->object != object)
            continue;
        if (!entry->extraLists)
            return nullptr;
        for (auto *list : *entry->extraLists)
        {
            if (list && pick(*list))
                return list;
        }
        return nullptr;
    }
    return nullptr;
}
} // namespace

RE::ExtraDataList *UnwornList(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ListOf(actor, object, [](const RE::ExtraDataList &list) {
        return !list.HasType(RE::ExtraDataType::kWorn) && !list.HasType(RE::ExtraDataType::kWornLeft);
    });
}

RE::ExtraDataList *WornList(RE::Actor *actor, RE::TESBoundObject *object, Hand hand)
{
    return ListOf(actor, object, [object, hand](const RE::ExtraDataList &list) { return WornIn(object, &list, hand); });
}

ft::ItemVariant VariantOf(const RE::ExtraDataList *list)
{
    ft::ItemVariant variant;
    if (!list)
        return variant;
    using T = RE::ExtraDataType;
    // The enchantment as its recipe, not its form: one made at the table is
    // a form the save mints, found again by these same fields
    // (Effect::IsMatch), and no plugin names it.
    if (const auto *ench = static_cast<const RE::ExtraEnchantment *>(list->GetByType(T::kEnchantment));
        ench && ench->enchantment)
    {
        for (const RE::Effect *effect : ResolvedEffects(*ench->enchantment))
            variant.enchantment.push_back({effect->baseEffect->GetFormID(), effect->effectItem.magnitude,
                                           effect->effectItem.duration, effect->effectItem.area});
    }
    if (const auto *health = static_cast<const RE::ExtraHealth *>(list->GetByType(T::kHealth)); health)
        variant.tempering = health->health;
    // Only a variant the player gave: the engine adds a text entry of its own
    // to a tempered item the first time it draws it, with no text in it.
    if (const auto *text = static_cast<const RE::ExtraTextDisplayData *>(list->GetByType(T::kTextDisplayData));
        text && text->IsPlayerSet() && text->displayName.c_str())
        variant.label = text->displayName.c_str();
    return variant;
}

namespace
{
// The list's marks and count as a row, for the questions asked of one
// list: what the core's ListWorn and WornIn read.
ft::BagRow MarksOf(const RE::ExtraDataList *list)
{
    ft::BagRow row;
    row.wornRight = list->HasType(RE::ExtraDataType::kWorn);
    row.wornLeft = list->HasType(RE::ExtraDataType::kWornLeft);
    row.count = list->GetCount();
    row.token = list;
    return row;
}
} // namespace

Bag ViewBag(RE::Actor *actor, RE::TESBoundObject *object)
{
    const Carried carried = CarriedOf(actor, object);
    return ViewOf(object, carried.count, carried.entry.get());
}

Bag ViewOf(RE::TESBoundObject *object, std::int32_t count, const RE::InventoryEntryData *entry)
{
    Bag bag;
    bag.view.weapon = object && object->IsWeapon();
    if (count <= 0)
        return bag;
    bag.view.total = count;
    if (entry && entry->extraLists)
    {
        for (auto *list : *entry->extraLists)
        {
            if (!list)
                continue;
            ft::BagRow row = MarksOf(list);
            row.variant = VariantOf(list);
            row.ownRow = RowOfItsOwn(list);
            bag.view.rows.push_back(std::move(row));
            bag.lists.push_back(list);
        }
    }
    return bag;
}

RE::ExtraDataList *Bag::ListOf(const ft::BagRow *row) const noexcept
{
    return row ? lists[view.IndexOf(row)] : nullptr;
}

RE::ExtraDataList *Bag::ListAt(std::optional<std::size_t> index) const noexcept
{
    return index && *index < lists.size() ? lists[*index] : nullptr;
}

std::int32_t CountVariant(RE::Actor *actor, RE::TESBoundObject *object, const std::optional<ft::ItemVariant> &variant)
{
    return ft::CountVariant(ViewBag(actor, object).view, variant);
}

RE::ExtraDataList *WornVariantList(RE::Actor *actor, RE::TESBoundObject *object, const ft::ItemVariant &variant,
                                   Hand hands)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::WornVariantRow(bag.view, variant, hands));
}

RE::ExtraDataList *UnwornVariantList(RE::Actor *actor, RE::TESBoundObject *object, const ft::ItemVariant &variant)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::UnwornVariantRow(bag.view, variant));
}

RE::ExtraDataList *WornStackList(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::WornStackRow(bag.view, hands));
}

RE::ExtraDataList *UnwornStackList(RE::Actor *actor, RE::TESBoundObject *object)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::UnwornStackRow(bag.view));
}

bool RowOfItsOwn(const RE::ExtraDataList *list)
{
    return list && !list->IsInventoryStackable(true);
}

RE::ExtraDataList *ListOfAddress(RE::Actor *actor, RE::TESBoundObject *object, const RE::ExtraDataList *address)
{
    if (!address)
        return nullptr;
    return ListOf(actor, object, [address](const RE::ExtraDataList &list) { return &list == address; });
}

std::vector<ft::ItemVariant> RowsOf(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ft::RowsOf(ViewBag(actor, object).view);
}

bool HasListlessCopy(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ViewBag(actor, object).view.HasListlessCopy();
}

std::string ListEntries(const RE::ExtraDataList *list)
{
    if (!list)
        return "-";
    std::string out;
    for (const auto &extra : *list)
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%s%02X", out.empty() ? "" : " ", static_cast<unsigned>(extra.GetType()));
        out += buf;
    }
    return out.empty() ? "empty" : out;
}

bool ListWorn(const RE::ExtraDataList *list, Hand hands)
{
    return list && ft::ListWorn(MarksOf(list), hands);
}

bool WornIn(const RE::TESBoundObject *object, const RE::ExtraDataList *list, Hand hands)
{
    if (!list)
        return false;
    ft::BagView kind;
    kind.weapon = !object || object->IsWeapon();
    return ft::WornIn(kind, MarksOf(list), hands);
}

RE::TESObjectWEAP *PoisonableWeaponIn(RE::Actor *actor, bool left)
{
    auto *weapon = WeaponIn(actor, left);
    // What the inventory menu offers a poison to: any weapon but a staff
    // (read from its poisoning routine, which checks that one type).
    if (!weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff)
        return nullptr;
    return weapon;
}

WeaponInHand WeaponToPoison(RE::Actor *actor)
{
    auto *right = PoisonableWeaponIn(actor, false);
    auto *left = PoisonableWeaponIn(actor, true);
    const Hand hand = ft::HandToPoison(right != nullptr, right && WeaponPoisoned(actor, right, Hand::Right),
                                       left != nullptr, left && WeaponPoisoned(actor, left, Hand::Left));
    if (hand == Hand::Right)
        return {right, hand};
    if (hand == Hand::Left)
        return {left, hand};
    return {};
}

bool WeaponPoisoned(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand)
{
    // The poison sits on the worn copy's extra list: that hand's, so two
    // of one dagger with one dosed read as one poisoned and one clean.
    // (The engine's own IsPoisoned reads every list of the entry, and
    // answered yes for both.)
    const RE::ExtraDataList *worn = WornList(actor, weapon, hand);
    return worn && worn->HasType(RE::ExtraDataType::kPoison);
}

RE::TESObjectWEAP *WeaponIn(RE::Actor *actor, bool left)
{
    if (!actor)
        return nullptr;
    auto *object = actor->GetEquippedObject(left);
    auto *weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
    // Unarmed is a weapon record too and never in a bag. A two-hander or a
    // bow reports from the right hand and the left reports it again; a
    // one-hander in each hand is the same record twice, and both are
    // there, so two-handedness is what is asked, not sameness.
    if (!weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee)
        return nullptr;
    if (left && TwoHanded(weapon))
        return nullptr;
    return weapon;
}

WeaponCharge ChargeOf(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand)
{
    WeaponCharge out;
    if (!actor || !weapon)
        return out;

    // The record's enchantment and full charge, or a player-made one's on
    // the copy's list (ExtraEnchantment carries both). What is left is
    // ExtraCharge, absent for a weapon never used. The same reading as
    // the engine's recharge routine. In a hand, that hand's copy; in the
    // bag, whichever lists the entry has.
    RE::EnchantmentItem *ench = weapon->formEnchanting;
    float max = static_cast<float>(weapon->amountofEnchantment);
    float charge = max;
    const auto read = [&](const RE::ExtraDataList *list) {
        if (!list)
            return;
        if (auto *xEnch = list->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment)
        {
            ench = xEnch->enchantment;
            max = static_cast<float>(xEnch->charge);
            charge = max;
        }
        if (auto *xCharge = list->GetByType<RE::ExtraCharge>())
            charge = xCharge->charge;
    };
    if (hand != Hand::None)
        read(WornList(actor, weapon, hand));
    else if (const Carried carried = CarriedOf(actor, weapon); carried.entry && carried.entry->extraLists)
    {
        for (auto *list : *carried.entry->extraLists)
            read(list);
    }
    if (!ench || max <= 0.0f)
        return out;
    // In hand, the live charge is an ACTOR VALUE -- RightItemCharge or
    // LeftItemCharge, what the HUD's charge meter reads -- and the item's
    // own record is only written back on unequip. Measured 2026-09-08: a
    // staff cast down to 491 showed no charge record until it was swapped
    // hands, and then 491 appeared. So the hand it is in says where to
    // read; in the bag, the record. Asked with no hand, a copy found worn
    // reads from the hand it is in, the right first.
    Hand in = hand;
    if (in == Hand::None)
        in = actor->GetEquippedObject(false) == weapon  ? Hand::Right
             : actor->GetEquippedObject(true) == weapon ? Hand::Left
                                                        : Hand::None;
    if (auto *owner = actor->AsActorValueOwner(); owner && in != Hand::None)
        charge = owner->GetActorValue(in == Hand::Right ? RE::ActorValue::kRightItemCharge
                                                        : RE::ActorValue::kLeftItemCharge);
    out.enchanted = true;
    out.charge = (std::min)((std::max)(charge, 0.0f), max);
    out.maxCharge = max;
    out.costPerHit = ench->CalculateMagickaCost(actor);

    return out;
}

float SoulCharge(RE::SOUL_LEVEL level)
{
    // The fallbacks are the executable's own defaults, read from its static
    // data: Skyrim.esm carries no record for these settings.
    switch (level)
    {
    case RE::SOUL_LEVEL::kPetty:
        return static_cast<float>(GameSetting("iSoulLevelValuePetty", 250));
    case RE::SOUL_LEVEL::kLesser:
        return static_cast<float>(GameSetting("iSoulLevelValueLesser", 500));
    case RE::SOUL_LEVEL::kCommon:
        return static_cast<float>(GameSetting("iSoulLevelValueCommon", 1000));
    case RE::SOUL_LEVEL::kGreater:
        return static_cast<float>(GameSetting("iSoulLevelValueGreater", 2000));
    case RE::SOUL_LEVEL::kGrand:
        return static_cast<float>(GameSetting("iSoulLevelValueGrand", 3000));
    default:
        return 0.0f;
    }
}

std::vector<ft::Snapshot::SoulGemView> ScanSoulGems(RE::Actor *actor)
{
    std::vector<ft::Snapshot::SoulGemView> out;
    if (!actor)
        return out;
    auto inventory = actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::SoulGem); });
    for (auto &[object, entry] : inventory)
    {
        const auto count = entry.first;
        if (count <= 0 || !object || !entry.second)
            continue;
        const float charge = SoulCharge(entry.second->GetSoulLevel());
        if (charge <= 0.0f)
            continue;
        out.push_back({object->GetFormID(), static_cast<int>(count), charge});
    }
    return out;
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
