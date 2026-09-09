#include "game/Sensors.h"

#include "core/Effects.h"

#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Packages.h"
#include "game/Pins.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
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

// The effects of a bottle a policy could choose it by: a potion's boons
// and a poison's banes, by the name the game shows, with the bottle's
// magnitude and duration of each. A potion's harmful side (the Slow in
// Sleeping Tree Sap, the regen loss in an ale) is not a reason to drink
// it, and a poison is chosen for what it does to the enemy.
// An ingredient eaten gives its FIRST effect and no other (the rest are for
// the alchemy table), so that one is the ingredient's effect here.
std::vector<ft::PotionStock::Effect> EffectsOf(RE::MagicItem *item, ft::ConsumableKind kind)
{
    std::vector<ft::PotionStock::Effect> out;
    if (!item)
        return out;
    const bool poison = kind == ft::ConsumableKind::Poison;
    const bool firstOnly = kind == ft::ConsumableKind::Ingredient;
    for (auto *effect : item->effects)
    {
        if (!effect || !effect->baseEffect)
            continue;
        const auto *base = effect->baseEffect;
        const bool harmful =
            base->IsDetrimental() || base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHostile);
        const char *name = base->GetFullName();
        if (harmful == poison && name && *name && !ft::EffectUseless(name))
            out.push_back({name, effect->effectItem.magnitude, static_cast<float>(effect->effectItem.duration)});
        if (firstOnly)
            break;
    }
    return out;
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
        stock.carried.push_back(
            {object->GetFormID(), static_cast<int>(count), *kind, EffectsOf(object->As<RE::MagicItem>(), *kind)});
    }
}

// The effects still running on the actor, by name.
//
// An INSTANT effect has duration 0 and never lingers here, so on a vanilla
// game a Restore is never listed and the settle time in MinimumCooldown
// does the spacing. Potion overhauls convert restores to over-time effects,
// and there this is the exact answer where a fixed settle would be a guess.
// A Fortify, a Resist, an Invisibility runs for a minute and is listed
// throughout, so the rule that drank it waits as a buff rule waits.
//
// Deliberately not restricted to effects whose source is a potion: a spell
// or an enchantment of the same effect ticking away is just as good a
// reason not to drink.
std::vector<std::string> RunningEffects(RE::Actor *actor)
{
    std::vector<std::string> out;
    auto *target = actor->AsMagicTarget();
    if (!target)
        return out;
    auto *effects = target->GetActiveEffectList();
    if (!effects)
        return out;
    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
            continue;
        // duration 0 is an instant effect that has already happened.
        if (!(ae->duration > 0.0f && ae->elapsedSeconds < ae->duration))
            continue;
        const char *name = ae->effect->baseEffect->GetFullName();
        if (name && *name)
            out.emplace_back(name);
    }
    return out;
}

// Defined further down, in this same unnamed namespace, with the sheets.
SheetRow Row(std::string label, std::string value);
bool ReadsSkillMods(const RE::Actor *actor);
bool ReadsSkillPowerMods(const RE::Actor *actor);

// Does this effect change anything for this actor? A value-modifying effect
// on a skill modifier -- Fortify One-handed's OneHandedModifier, Fortify
// Destruction's DestructionModifier -- is read only by the two hidden perks
// a follower does not carry (docs/RESEARCH.md 6): the value moves, and
// nothing looks at it.
bool EffectApplies(const RE::Actor *actor, const RE::EffectSetting *base)
{
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    const auto archetype = base->GetArchetype();
    if (archetype != Archetype::kValueModifier && archetype != Archetype::kPeakValueModifier &&
        archetype != Archetype::kDualValueModifier)
        return true;
    const auto av = static_cast<int>(base->data.primaryAV);
    constexpr int kFirstModifier = static_cast<int>(RE::ActorValue::kOneHandedModifier);
    constexpr int kLastModifier = static_cast<int>(RE::ActorValue::kEnchantingModifier);
    constexpr int kFirstPower = static_cast<int>(RE::ActorValue::kOneHandedPowerModifier);
    constexpr int kLastPower = static_cast<int>(RE::ActorValue::kEnchantingPowerModifier);
    if (av >= kFirstModifier && av <= kLastModifier)
        return ReadsSkillMods(actor);
    if (av >= kFirstPower && av <= kLastPower)
        return ReadsSkillPowerMods(actor);
    return true;
}
std::string Fmt(const char *fmt, double value);
float GameSetting(const char *name, float vanilla);

} // namespace

ft::Stat ReadStat(RE::Actor *actor, RE::ActorValue av)
{
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return {};
    // Current is the damaged value. The maximum is the permanent value --
    // base plus the permanent modifiers, perks and race -- plus the
    // TEMPORARY modifier, where a Fortify enchantment or potion lands: a
    // circlet of +50 magicka raises what the bar can show, and reading the
    // permanent value alone put 346 over 246 (2026-09-09). Their ratio is
    // what the rules read, so both are logged in Tactics.cpp to make a
    // wrong reading visible rather than merely wrong.
    const float temporary = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, av);
    return ft::Stat{owner->GetActorValue(av), owner->GetPermanentActorValue(av) + temporary};
}

void ForEachSpell(RE::Actor *actor, const std::function<void(RE::SpellItem *)> &fn)
{
    if (!actor)
        return;
    const auto walk = [&fn](const RE::TESSpellList::SpellData *list) {
        if (!list)
            return;
        for (std::uint32_t i = 0; i < list->numSpells; ++i)
        {
            if (list->spells[i])
                fn(list->spells[i]);
        }
    };
    if (auto *npc = actor->GetActorBase())
        walk(npc->GetSpellList());
    if (auto *race = actor->GetRace())
        walk(race->actorEffects);
    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
    {
        if (spell)
            fn(spell);
    }
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
    char buf[32];
    if (hours > 0)
        std::snprintf(buf, sizeof(buf), "%d h %d min", hours, minutes);
    else if (minutes > 0)
        std::snprintf(buf, sizeof(buf), "%d min %d s", minutes, secs);
    else
        std::snprintf(buf, sizeof(buf), "%d s", secs);
    return buf;
}

// The worn item carrying this enchantment, by the name the game shows for
// it, or empty if none is worn.
std::string WornSourceOf(RE::Actor *actor, const RE::MagicItem *magic)
{
    for (const auto &[object, entry] : actor->GetInventory())
    {
        if (!object || entry.first <= 0 || !entry.second || !entry.second->IsWorn())
            continue;
        if (entry.second->GetEnchantment() != magic)
            continue;
        const char *given = entry.second->GetDisplayName();
        if (given && *given)
            return given;
        return object->GetName() ? object->GetName() : "";
    }
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
        source = WornSourceOf(actor, ae->spell);
    if (source.empty() && ae->spell->GetName())
        source = ae->spell->GetName();
    return source;
}

std::vector<Contribution> Contributions(RE::Actor *actor, RE::ActorValue value)
{
    std::vector<Contribution> out;
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    auto *effects = target ? target->GetActiveEffectList() : nullptr;
    if (!effects)
        return out;
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
            continue;
        const auto *base = ae->effect->baseEffect;
        const auto archetype = base->GetArchetype();
        const bool moves = archetype == Archetype::kValueModifier || archetype == Archetype::kPeakValueModifier ||
                           archetype == Archetype::kDualValueModifier;
        if (!moves)
            continue;
        const bool primary = base->data.primaryAV == value;
        const bool secondary = archetype == Archetype::kDualValueModifier && base->data.secondaryAV == value;
        if (!primary && !secondary)
            continue;
        std::string source = SourceName(actor, ae);
        if (source.empty())
            source = base->GetName() ? base->GetName() : "?";
        // The active effect's magnitude already carries the engine's sign:
        // a detrimental modifier (Weakness to Fire on a vampire) is -50
        // here, not 50 with a flag to read. Negating it again showed the
        // weakness as +50% beside a total that had subtracted it
        // (2026-09-08).
        // An effect on the value with nothing to add (a vampire's Blood
        // Aura carries a zero here) is not a source.
        if (std::abs(ae->magnitude) < 0.05f)
            continue;
        out.push_back({std::move(source), ae->magnitude});
    }
    // Smallest first: the weaknesses, then the boons, the largest last.
    std::stable_sort(out.begin(), out.end(),
                     [](const Contribution &a, const Contribution &b) { return a.amount < b.amount; });
    return out;
}

float DamageReduction(RE::Actor *actor); // below, with the armour readings
float HiddenArmor(RE::Actor *actor);
float EffectiveArmor(RE::Actor *actor);

std::string ArmorNote(RE::Actor *actor)
{
    // Each piece worn with its rating as the follower wears it, then the
    // spells and enchantments on the armour value itself (Oakflesh, a
    // Fortify Armor), smallest first as the resistances list theirs.
    if (!actor)
        return {};
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
        parts.push_back({name ? name : "?", rating});
    }
    for (Contribution &c : Contributions(actor, RE::ActorValue::kDamageResist))
        parts.push_back(std::move(c));
    // And the engine's hidden bonus per piece worn (fArmorBaseFactor, 0.03
    // of a blow each), in the rating's own units -- 25 a piece at the
    // vanilla settings, the "25 armour per piece" of the wikis. The list
    // sums to the row's number, EffectiveArmor.
    static const float perPiece = GameSetting("fArmorBaseFactor", 0.03f);
    const float hidden = HiddenArmor(actor);
    if (hidden > 0.0f && perPiece > 0.0f)
    {
        const int pieces = static_cast<int>(actor->GetArmorBaseFactorSum() / perPiece + 0.5f);
        parts.push_back({"Hidden bonus (x" + std::to_string(pieces) + ")", hidden});
    }
    std::stable_sort(parts.begin(), parts.end(),
                     [](const Contribution &a, const Contribution &b) { return a.amount < b.amount; });
    // Whatever the engine's figure has that the pieces, the effects and
    // the bonus do not (a formula mod, a rounding): last, as a remainder,
    // so the list sums to the row and a gap is seen rather than hidden.
    // Against the engine's live recomputation, as the row is, not the
    // DamageResist actor value, which is written at equip time and can
    // trail the skill.
    float sum = 0.0f;
    for (const Contribution &c : parts)
        sum += c.amount;
    if (const float gap = EffectiveArmor(actor) - sum; std::abs(gap) >= 1.0f)
        parts.push_back({"Other", gap});
    std::string note;
    for (const Contribution &c : parts)
        note += (note.empty() ? "" : "\n") + c.source + ": " + Fmt("%+.0f", c.amount);
    return note;
}

float HiddenArmor(RE::Actor *actor)
{
    static const float scale = GameSetting("fArmorScalingFactor", 0.12f) / 100.0f;
    return actor && scale > 0.0f ? actor->GetArmorBaseFactorSum() / scale : 0.0f;
}

float EffectiveArmor(RE::Actor *actor)
{
    return actor ? actor->CalcArmorRating() + HiddenArmor(actor) : 0.0f;
}

std::string ValueNote(RE::Actor *actor, RE::ActorValue value, const char *unit)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return {};
    const float base = owner->GetBaseActorValue(value);
    const float permanent = owner->GetPermanentActorValue(value);
    std::string note = "Base: " + Fmt("%.0f", base) + unit;
    for (const Contribution &c : Contributions(actor, value))
        note += "\n" + c.source + ": " + Fmt("%+.0f", c.amount) + unit;
    // What is permanent beyond the base is perks and race: not effects,
    // which are temporary, and not damage, which is below the base.
    if (const float perks = permanent - base; std::abs(perks) > 0.05f)
        note += "\nPerks and race: " + Fmt("%+.0f", perks) + unit;
    return note;
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
        // and one that has run out is gone.
        if (base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
            continue;
        if (ae->duration > 0.0f && ae->elapsedSeconds >= ae->duration)
            continue;
        const char *name = base->GetName();
        if (!name || !*name)
            continue;

        EffectRow row;
        row.form = base->GetFormID();
        row.sourceForm = ae->spell ? ae->spell->GetFormID() : 0;
        row.applied = EffectApplies(actor, base);
        row.name = name;
        row.magnitude = ae->magnitude;
        row.duration = ae->duration;
        row.remaining = ae->duration > 0.0f ? ae->duration - ae->elapsedSeconds : -1.0f;
        row.remainingText = RemainingText(row.remaining);
        if (ae->spell)
        {
            if (ae->spell->As<RE::EnchantmentItem>())
                row.source = WornSourceOf(actor, ae->spell);
            if (row.source.empty() && ae->spell->GetName())
                row.source = ae->spell->GetName();
        }

        // The page.
        SheetSection stats{"Effect", {}, {}};
        char num[32];
        std::snprintf(num, sizeof(num), "%.0f", row.magnitude);
        if (row.magnitude != 0.0f)
            stats.rows.push_back(Row("Magnitude", num));
        if (ae->duration > 0.0f)
        {
            stats.rows.push_back(Row("Duration", RemainingText(ae->duration)));
            stats.rows.push_back(Row("Remaining", row.remainingText));
        }
        else
        {
            // No end to it: the infinity, and no Remaining row.
            SheetRow forever = Row("Duration", "");
            forever.icon = kIconInfinity;
            stats.rows.push_back(std::move(forever));
        }
        if (!row.source.empty())
            stats.rows.push_back(Row("Source", row.source));
        // Whoever cast it, when it was not the follower: the player's
        // Courage, an enemy's Fury.
        if (auto caster = ae->caster.get(); caster && caster.get() != actor && caster->GetName() && *caster->GetName())
            stats.rows.push_back(Row("Caster", caster->GetName()));
        row.detail.push_back(std::move(stats));
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

void LogActiveEffects(RE::Actor *actor, const char *when)
{
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    if (!target)
        return;

    auto *effects = target->GetActiveEffectList();
    if (!effects)
    {
        logger::info("  active effects [{}]: <none>", when);
        return;
    }

    int count = 0;
    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        ++count;

        const auto *base = ae->effect->baseEffect;
        const char *sourceName = ae->spell ? ae->spell->GetName() : "<none>";
        logger::info("  active effect [{}]: \"{}\" from \"{}\"  elapsed {:.1f}/{:.1f}s  mag {:.1f}", when,
                     base->GetName(), sourceName, ae->elapsedSeconds, ae->duration, ae->magnitude);
    }

    if (count == 0)
        logger::info("  active effects [{}]: <none>", when);
}

// What an actor is in the middle of, as docs/CONDITIONS.md 2 reads it: the
// hostile effects running on them by the kind of damage, the poison and
// the disease by their spell type, the paralysis and the rest by the
// actor's own flags. One walk of the effect list, a handful of flag reads.
// The share of a blow the actor's armour turns away, from the engine's own
// two numbers rather than a recount of the slots: CalcArmorRating is the
// rating as the engine applies it, perks included, and GetArmorBaseFactorSum
// the hidden bonus for the pieces worn -- fArmorBaseFactor (0.03) per piece,
// which is the "25 armour per piece" of the wikis in the engine's own
// terms. Combined as the vanilla damage code does: rating x
// fArmorScalingFactor / 100 + the hidden sum, capped at fMaxArmorRating.
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
    static const float scale = GameSetting("fArmorScalingFactor", 0.12f) / 100.0f;
    static const float cap = GameSetting("fMaxArmorRating", 80.0f) / 100.0f;
    const float rating = actor->CalcArmorRating();
    const float hidden = actor->GetArmorBaseFactorSum();
    return (std::min)(cap, (std::max)(0.0f, rating * scale + hidden));
}

// For the log, once per actor per session: the engine's armour numbers
// beside the actor value and our old slot count, so a wrong reading of
// either accessor shows up as a disagreement rather than a wrong percent.
std::unordered_set<std::uint32_t> g_armorLogged;
void LogArmorReadings(RE::Actor *actor)
{
    if (!actor || !g_armorLogged.insert(actor->GetFormID()).second)
        return;
    auto *owner = actor->AsActorValueOwner();
    int pieces = 0;
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    for (const Slot slot : {Slot::kBody, Slot::kHead, Slot::kHands, Slot::kFeet})
        if (actor->GetWornArmor(slot))
            ++pieces;
    const auto &runtime = actor->GetActorRuntimeData();
    logger::info("armor {}: AV DamageResist {:.1f}, CalcArmorRating {:.1f} (cached {:.1f}), base factor sum {:.3f} "
                 "(cached {:.3f}) over {} pieces x fArmorBaseFactor {:.2f} -- reduction {:.1f}%",
                 actor->GetName() ? actor->GetName() : "?",
                 owner ? owner->GetActorValue(RE::ActorValue::kDamageResist) : 0.0f, actor->CalcArmorRating(),
                 runtime.armorRating, actor->GetArmorBaseFactorSum(), runtime.armorBaseFactorSum, pieces,
                 GameSetting("fArmorBaseFactor", 0.03f), DamageReduction(actor) * 100.0f);
}

// The enchantment on a weapon the actor carries: a player-made one on the
// entry (ExtraEnchantment), else the record's. The same reading as
// ChargeOf.
RE::EnchantmentItem *EnchantmentOn(RE::Actor *actor, RE::TESObjectWEAP *weapon)
{
    auto inventory = actor->GetInventory([weapon](RE::TESBoundObject &c) { return &c == weapon; });
    const auto found = inventory.find(weapon);
    if (found != inventory.end() && found->second.second && found->second.second->extraLists)
        for (auto *list : *found->second.second->extraLists)
            if (auto *xEnch = list ? list->GetByType<RE::ExtraEnchantment>() : nullptr; xEnch && xEnch->enchantment)
                return xEnch->enchantment;
    return weapon->formEnchanting;
}

// What the actor is wielding, a bit per DamageKind: a blade is Melee, a
// bow or crossbow Ranged, a spell or a staff Magic; and the kind of damage
// any of it does -- the enchantment's, the staff's or the spell's effects,
// a poison on the blade -- by what resists it. Fists are nothing. A
// two-hander reports from both hands, which is the same bits twice.
void ReadHands(RE::Actor *actor, ft::ActorTraits &traits)
{
    const auto effectsOf = [&](const RE::MagicItem *magic) {
        if (!magic)
            return;
        for (const auto *effect : magic->effects)
            if (effect && effect->baseEffect)
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
                continue;
            if (type == RE::WEAPON_TYPE::kStaff)
                traits.Wield(ft::DamageKind::Magic);
            else if (weapon->IsBow() || weapon->IsCrossbow())
                traits.Wield(ft::DamageKind::Ranged);
            else
                traits.Wield(ft::DamageKind::Melee);
            effectsOf(EnchantmentOn(actor, weapon));
            if (WeaponPoisoned(actor, weapon))
                traits.Wield(ft::DamageKind::Poison);
        }
        else if (auto *magic = held->As<RE::MagicItem>())
        {
            traits.Wield(ft::DamageKind::Magic);
            effectsOf(magic);
        }
    }
}

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
    traits.attackedBy = attacked.kinds;
    traits.attacker = attacked.attacker;
    if (auto *owner = actor->AsActorValueOwner())
    {
        traits.SetResist(ft::DamageKind::Magic, owner->GetActorValue(RE::ActorValue::kResistMagic));
        traits.SetResist(ft::DamageKind::Fire, owner->GetActorValue(RE::ActorValue::kResistFire));
        traits.SetResist(ft::DamageKind::Frost, owner->GetActorValue(RE::ActorValue::kResistFrost));
        traits.SetResist(ft::DamageKind::Shock, owner->GetActorValue(RE::ActorValue::kResistShock));
        traits.SetResist(ft::DamageKind::Poison, owner->GetActorValue(RE::ActorValue::kPoisonResist));
    }
    using Archetype = RE::EffectArchetypes::ArchetypeID;

    if (auto *target = actor->AsMagicTarget())
    {
        if (auto *effects = target->GetActiveEffectList())
        {
            for (auto *ae : *effects)
            {
                if (!ae || !ae->effect || !ae->effect->baseEffect)
                    continue;
                if (ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
                    continue;
                const auto *base = ae->effect->baseEffect;
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
            }
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

ft::Snapshot BuildSnapshot(RE::Actor *actor, double now)
{
    ft::Snapshot s;

    if (!actor)
        return s;

    s.self = actor->GetFormID();
    s.now = now;

    s.health = ReadStat(actor, RE::ActorValue::kHealth);
    s.magicka = ReadStat(actor, RE::ActorValue::kMagicka);
    s.stamina = ReadStat(actor, RE::ActorValue::kStamina);

    s.inCombat = actor->IsInCombat();
    // Per actor, NPCs included: the last shout's word recovery, counting
    // down. Negative or nonsense reads as "can shout".
    {
        const float recovery = actor->GetVoiceRecoveryTime();
        s.voiceRecovery = recovery > 0.0f && recovery < 3600.0f ? recovery : 0.0f;
    }
    if (auto *state = actor->AsActorState())
    {
        s.weaponDrawn = state->IsWeaponDrawn();
        s.sneaking = state->IsSneaking();
    }

    s.traits = ReadTraits(actor);
    if (auto *player = RE::PlayerCharacter::GetSingleton())
    {
        s.playerHealth = ReadStat(player, RE::ActorValue::kHealth);
        s.playerMagicka = ReadStat(player, RE::ActorValue::kMagicka);
        s.playerStamina = ReadStat(player, RE::ActorValue::kStamina);
        s.playerTraits = ReadTraits(player);
    }

    // Whom the follower is fighting, as the engine sees it: what "current
    // target" resolves to.
    if (auto target = actor->GetActorRuntimeData().currentCombatTarget.get(); target && !target->IsDead())
        s.currentTarget = target->GetFormID();
    if (auto *player = RE::PlayerCharacter::GetSingleton())
    {
        if (auto target = player->GetActorRuntimeData().currentCombatTarget.get(); target && !target->IsDead())
            s.playerTarget = target->GetFormID();
    }

    // The party and the enemies, by definition (docs/CONDITIONS.md 6). An
    // ally is the player and every other actor with the teammate flag; an
    // enemy is anyone the compass paints red for the player, in combat and
    // hostile to them. One walk of the loaded actors, alive ones only.
    auto *player = RE::PlayerCharacter::GetSingleton();
    const auto enemyOf = [&](RE::Actor *other) {
        ft::EnemyView enemy;
        enemy.id = other->GetFormID();
        enemy.health = ReadStat(other, RE::ActorValue::kHealth);
        enemy.magicka = ReadStat(other, RE::ActorValue::kMagicka);
        enemy.stamina = ReadStat(other, RE::ActorValue::kStamina);
        enemy.distance = actor->GetPosition().GetDistance(other->GetPosition());
        if (auto theirTarget = other->GetActorRuntimeData().currentCombatTarget.get(); theirTarget)
            enemy.attacking = theirTarget->GetFormID();
        bool losArg = false;
        enemy.hasLineOfSight = actor->HasLineOfSight(other, losArg);
        enemy.traits = ReadTraits(other);
        enemy.isCasting = enemy.traits.Has(ft::StatusKind::Casting);
        return enemy;
    };
    const auto allyOf = [&](RE::Actor *other) {
        ft::AllyView ally;
        ally.id = other->GetFormID();
        ally.health = ReadStat(other, RE::ActorValue::kHealth);
        ally.distance = actor->GetPosition().GetDistance(other->GetPosition());
        ally.magicka = ReadStat(other, RE::ActorValue::kMagicka);
        ally.stamina = ReadStat(other, RE::ActorValue::kStamina);
        ally.traits = ReadTraits(other);
        if (auto theirTarget = other->GetActorRuntimeData().currentCombatTarget.get();
            theirTarget && !theirTarget->IsDead())
            ally.target = theirTarget->GetFormID();
        return ally;
    };
    if (player && !player->IsDead())
        s.allies.push_back(allyOf(player));
    if (auto *lists = RE::ProcessLists::GetSingleton())
    {
        lists->ForEachHighActor([&](RE::Actor *otherPtr) {
            if (!otherPtr)
                return RE::BSContainer::ForEachResult::kContinue;
            RE::Actor &other = *otherPtr;
            if (&other == actor || &other == player || other.IsDead())
                return RE::BSContainer::ForEachResult::kContinue;
            if (other.IsPlayerTeammate())
                s.allies.push_back(allyOf(&other));
            else if (player && other.IsInCombat() && other.IsHostileToActor(player))
                s.enemies.push_back(enemyOf(&other));
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }
    // The corpses: the dead nearby that a Reanimate could take. Not one
    // already commanded (a raised corpse is someone's), not one the effect's
    // own condition refuses (the MagicNoReanimate keyword, 06F6FB, which is
    // the Reanimate archetype's one condition), and within the reach a rule
    // could act on. The level is what the spell's cap is measured against.
    if (auto *lists = RE::ProcessLists::GetSingleton())
    {
        constexpr float kCorpseReach = 3000.0f;
        auto *noReanimate = RE::TESForm::LookupByID<RE::BGSKeyword>(kMagicNoReanimateKeyword);
        lists->ForEachHighActor([&](RE::Actor *otherPtr) {
            if (!otherPtr)
                return RE::BSContainer::ForEachResult::kContinue;
            RE::Actor &other = *otherPtr;
            if (&other == actor || !other.IsDead() || other.IsCommandedActor())
                return RE::BSContainer::ForEachResult::kContinue;
            if (noReanimate && other.HasKeyword(noReanimate))
                return RE::BSContainer::ForEachResult::kContinue;
            const float distance = actor->GetPosition().GetDistance(other.GetPosition());
            if (distance > kCorpseReach)
                return RE::BSContainer::ForEachResult::kContinue;
            s.corpses.push_back({other.GetFormID(), static_cast<int>(other.GetLevel()), distance});
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    // The follower's own target is an enemy whether or not the player is
    // in its fight yet.
    if (s.currentTarget != 0 && !std::any_of(s.enemies.begin(), s.enemies.end(),
                                             [&](const ft::EnemyView &e) { return e.id == s.currentTarget; }))
    {
        if (auto *target = RE::TESForm::LookupByID<RE::Actor>(s.currentTarget))
            s.enemies.push_back(enemyOf(target));
    }

    ScanPotions(actor, s.potions);
    for (const bool left : {false, true})
    {
        auto &hand = left ? s.leftWeapon : s.rightWeapon;
        if (auto *weapon = PoisonableWeaponIn(actor, left))
        {
            hand.takesPoison = true;
            hand.poisoned = WeaponPoisoned(actor, weapon);
        }
        if (auto *weapon = WeaponIn(actor, left))
        {
            const WeaponCharge c = ChargeOf(actor, weapon);
            hand.enchanted = c.enchanted;
            hand.charge = c.charge;
            hand.maxCharge = c.maxCharge;
            hand.costPerHit = c.costPerHit;
        }
    }
    s.soulGems = ScanSoulGems(actor);

    s.potions.running = RunningEffects(actor);

    // Spells: what she knows, what is running, what is in hand. All three are
    // ids only -- Snapshot never sees an RE:: type -- and all three are needed
    // to tell "cannot", "already up" and "already held" apart in the status
    // column.
    // The shouts on the base record, for a Shout rule: known, cost nothing,
    // held in no hand.
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numShouts; ++i)
            {
                if (list->shouts[i] && !IsWrapperShout(list->shouts[i]->GetFormID()))
                    s.spells.known.push_back(list->shouts[i]->GetFormID());
            }
        }
    }
    ForEachSpell(actor, [&s, actor](RE::SpellItem *spell) {
        // A power is known too, for a Use power rule; it costs nothing and
        // is not held in a hand, so it is in neither of the lists below.
        if (IsPower(spell))
        {
            s.spells.known.push_back(spell->GetFormID());
            return;
        }
        if (!IsCastable(spell))
            return;
        s.spells.known.push_back(spell->GetFormID());
        // Her cost, not the base cost: CalculateMagickaCost applies her skill
        // and perks, which is what the AI will charge her.
        s.spells.costs.push_back({spell->GetFormID(), spell->CalculateMagickaCost(actor)});
        // A Reanimate's cap: the level of corpse it can raise is its
        // effect's magnitude (Reanimate Corpse 13, Revenant 21, Dread
        // Zombie 30) -- as SHE casts it, perks and Fortify effects in, the
        // same way the engine judges the corpse. The Corpse subject
        // measures the dead against it.
        for (const auto *effect : spell->effects)
        {
            if (effect && effect->baseEffect &&
                effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate)
            {
                s.spells.caps.push_back({spell->GetFormID(), static_cast<int>(ActualMagnitude(actor, spell, effect))});
                break;
            }
        }
        // And as the pin book sees it, for an equip rule.
        s.loadout.push_back(DescribeHoldable(actor, spell));
    });

    // What she could hold or wear, as the pin book sees it, and what is
    // pinned. A walk of her inventory that keeps only the equipable kinds;
    // the potion scan above walks it too, and the two could share one pass
    // if the cost ever showed, which at tens of microseconds it does not.
    for (const auto &[object, entry] : actor->GetInventory())
    {
        if (!object || entry.first <= 0)
            continue;
        if (!(object->Is(RE::FormType::Weapon) || object->Is(RE::FormType::Armor) || object->Is(RE::FormType::Ammo) ||
              object->Is(RE::FormType::Light)))
            continue;
        s.loadout.push_back(DescribeHoldable(actor, object));
    }
    s.pins = PinsOf(s.self);

    if (auto *target = actor->AsMagicTarget())
    {
        if (auto *effects = target->GetActiveEffectList())
        {
            for (auto *ae : *effects)
            {
                if (!ae || !ae->spell)
                    continue;
                // Instant effects have already happened and never lapse, so
                // treating them as "still up" would block the rule forever.
                if (ae->duration <= 0.0f)
                    continue;
                if (ae->elapsedSeconds >= ae->duration)
                    continue;
                s.spells.active.push_back(ae->spell->GetFormID());
            }
        }
    }

    // selectedSpells is indexed by Actor::SlotTypes, NOT by
    // MagicSystem::CastingSource. The two enums start with the same two names
    // in the same order, which makes mixing them up easy and silent.
    for (const auto slot : {RE::Actor::SlotTypes::kLeftHand, RE::Actor::SlotTypes::kRightHand})
    {
        if (auto *held = actor->GetActorRuntimeData().selectedSpells[slot])
            s.spells.equipped.push_back(held->GetFormID());
    }

    return s;
}

namespace
{
const char *SoulLevelName(RE::SOUL_LEVEL level)
{
    switch (level)
    {
    case RE::SOUL_LEVEL::kPetty:
        return "Petty";
    case RE::SOUL_LEVEL::kLesser:
        return "Lesser";
    case RE::SOUL_LEVEL::kCommon:
        return "Common";
    case RE::SOUL_LEVEL::kGreater:
        return "Greater";
    case RE::SOUL_LEVEL::kGrand:
        return "Grand";
    default:
        return "Empty";
    }
}
} // namespace

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
            std::string name = object->GetName() ? object->GetName() : "?";
            const auto *gem = object->As<RE::TESSoulGem>();
            if (!gem || level < gem->GetMaximumCapacity())
                name += std::string(" (") + SoulLevelName(level) + ")";
            out.push_back({object->GetFormID(), name, static_cast<int>(count), ft::ConsumableKind::SoulGem, {}});
            continue;
        }
        const auto kind = ConsumableKindOf(object);
        if (!kind)
            continue;
        std::vector<std::string> effects;
        for (const auto &effect : EffectsOf(object->As<RE::MagicItem>(), *kind))
            effects.push_back(effect.name);
        out.push_back({object->GetFormID(), object->GetName() ? object->GetName() : "?", static_cast<int>(count), *kind,
                       std::move(effects)});
    }
    std::sort(out.begin(), out.end(),
              [](const ConsumableOption &a, const ConsumableOption &b) { return a.name < b.name; });
    return out;
}

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
        // The same spell can appear in both sources; show it once.
        const std::uint32_t id = spell->GetFormID();
        if (std::any_of(out.begin(), out.end(), [id](const SpellOption &o) { return o.form == id; }))
            return;

        std::string name = spell->GetName() ? spell->GetName() : "";
        if (name.empty())
            return; // nameless entries are internal; nothing to show a player
        bool reanimate = false;
        for (const auto *effect : spell->effects)
            reanimate =
                reanimate || (effect && effect->baseEffect &&
                              effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate);
        out.push_back(SpellOption{id, std::move(name), spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf,
                                  spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation, reanimate,
                                  power ? SpellOption::Kind::Power : SpellOption::Kind::Spell});
    });

    // The shouts on the base record. A shout's delivery is its first word's
    // spell's: Whirlwind Sprint and Become Ethereal are Self, the rest aimed.
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numShouts; ++i)
            {
                auto *shout = list->shouts[i];
                if (!shout || IsWrapperShout(shout->GetFormID()) || !shout->GetName() || !*shout->GetName())
                    continue;
                const auto *word = shout->variations[0].spell;
                const bool self = word && word->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
                out.push_back(
                    SpellOption{shout->GetFormID(), shout->GetName(), self, false, false, SpellOption::Kind::Shout});
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const SpellOption &a, const SpellOption &b) { return a.name < b.name; });
    return out;
}

// --- character sheet ---------------------------------------------------------

namespace
{

std::string Fmt(const char *fmt, double value)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, value);
    return buf;
}

// A float game setting, or the vanilla value if the collection has no such
// entry. The fallbacks are vanilla's numbers so a missing setting degrades to
// "what the unmodded game does", not to a zero that reads as a broken sheet.
float GameSetting(const char *name, float vanilla)
{
    auto *collection = RE::GameSettingCollection::GetSingleton();
    auto *setting = collection ? collection->GetSetting(name) : nullptr;
    return setting ? setting->GetFloat() : vanilla;
}

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

SheetRow Row(std::string label, std::string value)
{
    SheetRow row;
    row.label = std::move(label);
    row.value = std::move(value);
    return row;
}

std::string NameOr(const RE::TESForm *form, const char *fallback)
{
    return form && form->GetName() && *form->GetName() ? form->GetName() : fallback;
}

// --- perks -------------------------------------------------------------------

struct TreePerk
{
    RE::BGSPerk *perk;
    int rank;          // 1-based position in the perk's rank chain
    int ranks;         // length of that chain
    float requirement; // the skill level the perk asks for; 0 if it asks nothing
    std::string description;
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
        std::vector<RE::BGSSkillPerkTreeNode *> stack{info->perkTree};
        std::unordered_set<const RE::BGSSkillPerkTreeNode *> seen;
        while (!stack.empty())
        {
            auto *node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second)
                continue;

            // A node names the first rank; the rest chain through nextPerk.
            // Bounded, because a malformed chain that loops would hang the
            // game thread, and a chain longer than this is not a rank chain.
            std::vector<RE::BGSPerk *> chain;
            for (auto *perk = node->perk; perk && chain.size() < 16; perk = perk->nextPerk)
                chain.push_back(perk);
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                RE::BSString text;
                chain[i]->GetDescription(text, chain[i]);
                out.push_back({chain[i], static_cast<int>(i) + 1, static_cast<int>(chain.size()),
                               SkillRequirement(chain[i], skill), text.c_str() ? text.c_str() : ""});
            }

            for (auto *child : node->children)
                stack.push_back(child);
        }
    }

    // Least demanding first: the requirement is the game's own statement of
    // how strong a perk is, so the list reads weakest to strongest.
    std::sort(out.begin(), out.end(), [](const TreePerk &a, const TreePerk &b) {
        if (a.requirement != b.requirement)
            return a.requirement < b.requirement;
        return a.rank < b.rank;
    });
    return cache.emplace(skill, std::move(out)).first->second;
}

// The perks this follower holds in one skill's tree, one row per perk at
// the highest rank held. Asked of the engine with HasPerk rather than read
// off her record, so a perk a mod granted at runtime counts the same as one
// she was authored with. Ordered by the skill level each perk asks for,
// weakest first; the modifiers column carries its own in-game description.
std::vector<SheetRow> OwnedPerks(RE::Actor *actor, RE::ActorValue skill)
{
    std::vector<SheetRow> rows;
    for (const TreePerk &entry : TreePerks(skill))
    {
        if (!actor->HasPerk(entry.perk))
            continue;
        if (entry.perk->nextPerk && actor->HasPerk(entry.perk->nextPerk))
            continue; // a higher rank is held; that one gets the row

        // Trimmed, because the records are not: Skyrim.esm's first rank of
        // Magic Resistance is named " Magic Resistance", leading space and
        // all, and on screen that reads as a row set in for no reason.
        std::string label = entry.perk->GetName() ? entry.perk->GetName() : "?";
        const auto first = label.find_first_not_of(' ');
        const auto last = label.find_last_not_of(' ');
        label = first == std::string::npos ? "?" : label.substr(first, last - first + 1);

        const std::string rank = entry.ranks > 1 ? std::to_string(entry.rank) + "/" + std::to_string(entry.ranks) : "";
        SheetRow row = Row(std::move(label), rank);
        row.modifiers = entry.description;
        row.form = entry.perk->GetFormID(); // the name opens the perk's page
        rows.push_back(std::move(row));
    }
    return rows;
}

// What one hand holds, as rows: a weapon and its numbers, a spell and its
// cost and strongest effect, a shield and its rating, or a torch. An empty
// hand adds no rows, and the caller shows no table for it. A two-hander
// shows in the right hand and the left says so.
void HandRows(RE::Actor *actor, bool left, std::vector<SheetRow> &rows)
{
    RE::TESForm *held = actor->GetEquippedObject(left);
    if (!held)
        return;

    if (auto *weapon = held->As<RE::TESObjectWEAP>())
    {
        const bool twoHanded =
            weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
        if (left && twoHanded)
        {
            rows.push_back(Row("Held", "the same, two-handed"));
            return;
        }
        rows.push_back(Row("Weapon", NameOr(weapon, "?")));
        rows.back().form = weapon->GetFormID();
        // In her hands: the carried item, for its tempering.
        auto inventory = actor->GetInventory([weapon](RE::TESBoundObject &o) { return &o == weapon; });
        const auto found = inventory.find(weapon);
        auto *entry = found != inventory.end() ? found->second.second.get() : nullptr;
        rows.push_back(Row("Damage", Fmt("%.0f", WeaponDamage(actor, weapon, entry))));
        rows.push_back(Row("Speed", Fmt("%.2f", weapon->GetSpeed())));
        rows.push_back(Row("Reach", Fmt("%.2f", weapon->GetReach())));
        rows.push_back(Row("Stagger", Fmt("%.2f", weapon->GetStagger())));
        if (weapon->IsBow() || weapon->IsCrossbow())
        {
            if (auto *ammo = actor->GetCurrentAmmo())
            {
                rows.push_back(Row("Ammo", NameOr(ammo, "?")));
                rows.back().form = ammo->GetFormID();
                rows.push_back(Row("Ammo Damage", Fmt("%.0f", ammo->GetRuntimeData().data.damage)));
            }
            else
            {
                rows.push_back(Row("Ammo", "none"));
            }
        }
        return;
    }

    if (auto *spell = held->As<RE::SpellItem>())
    {
        rows.push_back(Row("Spell", NameOr(spell, "?")));
        rows.back().form = spell->GetFormID();
        rows.push_back(Row("Cost", Fmt("%.0f", spell->CalculateMagickaCost(actor))));
        if (const auto *effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
        {
            std::string what = NameOr(effect->baseEffect, "?");
            what += " " + Fmt("%.0f", effect->effectItem.magnitude);
            if (effect->effectItem.duration > 0)
                what += " for " + std::to_string(effect->effectItem.duration) + " s";
            rows.push_back(Row("Effect", what));
        }
        return;
    }

    if (auto *armor = held->As<RE::TESObjectARMO>())
    {
        const bool shield = armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        rows.push_back(Row(shield ? "Shield" : "Held", NameOr(armor, "?")));
        rows.back().form = armor->GetFormID();
        auto inventory = actor->GetInventory([armor](RE::TESBoundObject &o) { return &o == armor; });
        const auto found = inventory.find(armor);
        auto *entry = found != inventory.end() ? found->second.second.get() : nullptr;
        rows.push_back(Row("Armor", Fmt("%.0f", ArmorRating(actor, armor, entry))));
        return;
    }

    if (held->Is(RE::FormType::Light))
    {
        rows.push_back(Row("Held", NameOr(held, "torch")));
        rows.back().form = held->GetFormID();
        return;
    }

    rows.push_back(Row("Held", NameOr(held, "?")));
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

} // namespace

namespace
{
// The two hidden perks that turn the Fortify skill values into anything:
// PerkSkillBoosts reads the enchantment values (OneHandedModifier and its
// kin), AlchemySkillBoosts the potion ones (OneHandedPowerModifier ...).
// The player carries both. On the records no follower does (docs/RESEARCH.md
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

float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry)
{
    if (!actor || !weapon)
        return 0.0f;
    float damage = weapon->GetAttackDamage() * Tempering(entry);

    // The skill curve: UESP gives it as (1 + skill / 200), which is what the
    // fallbacks below encode. The settings are read by the names the engine
    // uses so a rebalancing mod that changes them is honoured; the resolved
    // curve is logged once so a wrong name shows up as a wrong number in the
    // log rather than as a silently vanilla curve.
    using AV = RE::ActorValue;
    AV skill = AV::kOneHanded;
    AV fortify = AV::kOneHandedModifier;
    AV fortifyPower = AV::kOneHandedPowerModifier;
    if (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe())
    {
        skill = AV::kTwoHanded;
        fortify = AV::kTwoHandedModifier;
        fortifyPower = AV::kTwoHandedPowerModifier;
    }
    else if (weapon->IsBow() || weapon->IsCrossbow())
    {
        skill = AV::kArchery;
        fortify = AV::kMarksmanModifier;
        fortifyPower = AV::kMarksmanPowerModifier;
    }

    // The skill curve: min + (max - min) * skill / 100, and the engine
    // keeps one pair of settings for the player and another for everyone
    // else (read off the armour multiplier's disassembly, 2026-09-09, which
    // branches on IsPlayerOwner; the damage pair is named the same way).
    // The wikis' "1 + skill / 200" is the player's pair. Logged once, so a
    // mod that retunes them is visible.
    static const float npcMin = GameSetting("fDamageSkillMin", 1.0f);
    static const float npcMax = GameSetting("fDamageSkillMax", 1.5f);
    static const float pcMin = GameSetting("fDamagePCSkillMin", 1.0f);
    static const float pcMax = GameSetting("fDamagePCSkillMax", 1.5f);
    static const bool logged = [] {
        logger::info("damage: skill curve NPC {:.2f} to {:.2f}, player {:.2f} to {:.2f} over skill 0 to 100", npcMin,
                     npcMax, pcMin, pcMax);
        return true;
    }();
    (void)logged;

    auto *owner = actor->AsActorValueOwner();
    const float skillLevel = owner ? owner->GetActorValue(skill) : 0.0f;
    const bool player = actor->IsPlayerRef();
    const float lo = player ? pcMin : npcMin;
    const float hi = player ? pcMax : npcMax;
    damage *= lo + (hi - lo) * skillLevel / 100.0f;

    // Perks, through the engine's own entry point, so Armsman and the rest
    // count exactly as they do in a swing. The entry point wants a target,
    // and there is none outside a fight; she stands in for it herself. A
    // perk that reads the target (against undead, say) evaluates against
    // her and so stays out of the figure -- the same figure the player's
    // own inventory menu shows, which has no target either.
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModAttackDamage, actor, weapon, actor,
                                        &damage);

    // Fortify One-handed and its kin: enchantments on the first value,
    // potions on the second, both in percent -- for an actor with the perk
    // that reads them, which a follower is not.
    if (owner)
    {
        const float mods = (ReadsSkillMods(actor) ? owner->GetActorValue(fortify) : 0.0f) +
                           (ReadsSkillPowerMods(actor) ? owner->GetActorValue(fortifyPower) : 0.0f);
        damage *= 1.0f + mods / 100.0f;
    }

    return damage;
}

float ArmorRating(RE::Actor *actor, RE::TESObjectARMO *armor, RE::InventoryEntryData *entry)
{
    if (!actor || !armor)
        return 0.0f;
    using Class = RE::BGSBipedObjectForm::ArmorType;
    const Class armorClass = armor->GetArmorType();
    if (armorClass == Class::kClothing)
        return 0.0f;
    float rating = armor->GetArmorRating() * Tempering(entry);

    // The skill curve. UESP gives displayed armour as base * (1 + 0.4 *
    // skill / 100), which the fallbacks encode; the names are the engine's,
    // logged once, as for damage.
    using AV = RE::ActorValue;
    const bool heavy = armorClass == Class::kHeavyArmor;
    const AV skill = heavy ? AV::kHeavyArmor : AV::kLightArmor;
    const AV fortify = heavy ? AV::kHeavyArmorModifier : AV::kLightArmorModifier;
    const AV fortifyPower = heavy ? AV::kHeavyArmorPowerModifier : AV::kLightArmorPowerModifier;

    // The skill curve is the engine's own: fArmorRatingBase to
    // fArmorRatingMax over skill 0 to 100 for an NPC, the PC pair for the
    // player (GetArmorRatingSkillMultiplier branches on IsPlayerOwner).
    // The wikis' "1 + 0.4 * skill / 100" is the player's pair, and used
    // for a follower it read 46 where the engine had 66. Logged once.
    static const bool logged = [] {
        logger::info("armor: skill curve NPC {:.2f} to {:.2f}, player {:.2f} to {:.2f} over skill 0 to 100",
                     GameSetting("fArmorRatingBase", 1.0f), GameSetting("fArmorRatingMax", 1.4f),
                     GameSetting("fArmorRatingPCBase", 1.0f), GameSetting("fArmorRatingPCMax", 1.4f));
        return true;
    }();
    (void)logged;

    auto *owner = actor->AsActorValueOwner();
    const float skillLevel = owner ? owner->GetActorValue(skill) : 0.0f;
    if (owner)
        rating *= owner->GetArmorRatingSkillMultiplier(skillLevel);

    // Perks: Juggernaut, Agile Defender and their kin, through the engine's
    // entry point for armour, which takes the piece and the value.
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModArmorRating, actor, armor, &rating);

    if (owner)
    {
        const float mods = (ReadsSkillMods(actor) ? owner->GetActorValue(fortify) : 0.0f) +
                           (ReadsSkillPowerMods(actor) ? owner->GetActorValue(fortifyPower) : 0.0f);
        rating *= 1.0f + mods / 100.0f;
    }
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
        SheetSection s{"General", {}, {}};
        // The reference and the base record, as the console names them --
        // what "prid" takes, and what the log calls the follower.
        {
            char id[16];
            std::snprintf(id, sizeof(id), "%08X", actor->GetFormID());
            s.rows.push_back(Row("Ref ID", id));
            const auto *base = actor->GetActorBase();
            std::snprintf(id, sizeof(id), "%08X", base ? base->GetFormID() : 0u);
            s.rows.push_back(Row("Base ID", id));
        }
        s.rows.push_back(Row("Name", actor->GetName() ? actor->GetName() : "?"));
        auto *race = actor->GetRace();
        s.rows.push_back(Row("Race", race && race->GetName() ? race->GetName() : "?"));
        if (const auto *base = actor->GetActorBase())
        {
            const auto sex = base->GetSex();
            if (sex == RE::SEX::kMale || sex == RE::SEX::kFemale)
                s.rows.push_back(Row("Gender", sex == RE::SEX::kMale ? "Male" : "Female"));
        }
        // Speed is the multiplier every buff lands on -- 100 for plain, and
        // a Fortify Speed or a Slow moves it -- so it reads the same
        // standing and sprinting. What moves it and by whom is the hover
        // text, as for the regen rates.
        {
            SheetRow row = Row("Speed", Fmt("%.0f%%", av(RE::ActorValue::kSpeedMult)));
            row.note = "Base: " + Fmt("%.0f%%", owner->GetBaseActorValue(RE::ActorValue::kSpeedMult));
            for (const Contribution &c : Contributions(actor, RE::ActorValue::kSpeedMult))
                row.note += "\n" + c.source + ": " + Fmt("%+.0f%%", c.amount);
            if (const float perks = owner->GetPermanentActorValue(RE::ActorValue::kSpeedMult) -
                                    owner->GetBaseActorValue(RE::ActorValue::kSpeedMult);
                std::abs(perks) > 0.5f)
                row.note += "\nPerks and race: " + Fmt("%+.0f%%", perks);
            s.rows.push_back(std::move(row));
        }
        s.rows.push_back(Row("Noise", Fmt("%.0f%%", av(RE::ActorValue::kMovementNoiseMult) * 100.0)));
        out.push_back(std::move(s));
    }

    // Attack: what each hand holds, whatever it is. The old Attack section
    // knew only weapons, which left a mage's page saying "unarmed". A hand
    // holding nothing gets no table; with both empty, the one thing worth
    // saying is what her fists do.
    {
        SheetSection right{"Right Hand", {}, "Attack"};
        HandRows(actor, false, right.rows);
        SheetSection left{"Left Hand", {}, "Attack"};
        HandRows(actor, true, left.rows);

        if (right.rows.empty() && left.rows.empty())
        {
            SheetSection s{"Attack", {}, {}};
            s.rows.push_back(Row("Held", "unarmed"));
            s.rows.push_back(Row("Base Damage", Fmt("%.0f", av(RE::ActorValue::kUnarmedDamage))));
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
        SheetSection s{"Defense", {}, {}};
        // The armour rating the game shows is not the one it applies: each
        // piece worn adds a hidden bonus before the scaling factor, which is
        // why a displayed 609 lands at 85% and not 73%. One row: the rating
        // WITH that bonus in its own units, so the hover text's pieces and
        // bonus sum to it, and in parentheses the share of a blow it turns
        // away -- the same DamageReduction the Armor condition reads, so the
        // sheet and the rules cannot disagree. Robes and boots alone read
        // 50 (6%): two pieces' hidden bonus and no rating.
        const float resistCap = GameSetting("fPlayerMaxResistance", 85.0f);
        SheetRow armorRow = Row("Armor", Fmt("%.0f", EffectiveArmor(actor)) + " (" +
                                             Fmt("%.0f%%", DamageReduction(actor) * 100.0f) + ")");
        armorRow.note = ArmorNote(actor);
        s.rows.push_back(std::move(armorRow));
        // Each resistance with where it comes from as its hover text: the
        // ring, the potion, the race.
        const auto resist = [&](const char *label, RE::ActorValue value, bool capped) {
            SheetRow row = Row(label, capped ? CappedPercent(av(value), resistCap) : Fmt("%.0f%%", av(value)));
            row.note = ValueNote(actor, value, "%");
            s.rows.push_back(std::move(row));
        };
        // Magic first, then the elements, then poison; disease last, the one
        // that matters to the player alone.
        resist("Magic", RE::ActorValue::kResistMagic, true);
        resist("Fire", RE::ActorValue::kResistFire, true);
        resist("Frost", RE::ActorValue::kResistFrost, true);
        resist("Shock", RE::ActorValue::kResistShock, true);
        resist("Poison", RE::ActorValue::kPoisonResist, true);
        resist("Disease", RE::ActorValue::kResistDisease, false);
        out.push_back(std::move(s));
    }

    {
        // The rate the follower actually regenerates at: the base rate times
        // its multiplier, which is where every buff lands -- robes of
        // Destruction's "magicka regenerates 100% faster" is +100 on the
        // multiplier, and the rate itself stays at 3. The base and the
        // multiplier are the row's hover text when they differ from plain.
        SheetSection s{"Regen", {}, {}};
        const auto regen = [&](const char *label, RE::ActorValue rate, RE::ActorValue mult) {
            const float base = av(rate);
            const float factor = av(mult) / 100.0f;
            SheetRow row = Row(label, Fmt("%.2f%%", base * factor));
            // The base rate, then what speeds it up and by whom.
            // Each as the rate it adds, not the speed it multiplies by:
            // "+3.00%" for robes that double a 3% base reads straight off.
            row.note = "Base: " + Fmt("%.2f%%", base);
            for (const Contribution &c : Contributions(actor, mult))
                row.note += "\n" + c.source + ": " + Fmt("%+.2f%%", base * c.amount / 100.0f);
            if (const float perks = owner->GetPermanentActorValue(mult) - owner->GetBaseActorValue(mult);
                std::abs(perks) > 0.05f)
                row.note += "\nPerks and race: " + Fmt("%+.2f%%", base * perks / 100.0f);
            s.rows.push_back(std::move(row));
        };
        regen("Health Rate", RE::ActorValue::kHealRate, RE::ActorValue::kHealRateMult);
        regen("Stamina Rate", RE::ActorValue::kStaminaRate, RE::ActorValue::kStaminaRateMult);
        regen("Magicka Rate", RE::ActorValue::kMagickaRate, RE::ActorValue::kMagickaRateMult);
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
    // docs/COMBAT_STYLE.md has the page.
    const auto note = [](SheetRow row, const char *text) {
        row.note = text;
        return row;
    };

    using Flag = RE::TESCombatStyle::FLAG;
    const bool flanking = live->flags.all(Flag::kFlankingStyle);
    {
        SheetSection s{"Style", {}, {}};
        // A runtime copy has a 0xFF FormID; a record's is its plugin's.
        const bool ours = (live->GetFormID() & 0xFF000000U) == 0xFF000000U;
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", live->GetFormID());
        s.rows.push_back(Row("Base ID", ours ? std::string(id) + "  (our copy)" : id));
        if (controller && controller->combatStyle && record && controller->combatStyle != record)
        {
            char recordId[16];
            std::snprintf(recordId, sizeof(recordId), "%08X", record->GetFormID());
            s.rows.push_back(Row("On Record", recordId));
        }
        s.rows.push_back(note(Row("Close Range", flanking ? "Flanking" : "Dueling"),
                              "- Dueling: circles, falls back\n"
                              "- Flanking: keeps a distance, stalks\n"
                              "- One or the other"));
        // A tick when allowed, as the equipped state is shown; no row at all
        // when not.
        if (live->flags.all(Flag::kAllowDualWielding))
        {
            SheetRow row = note(Row("Dual Wield", ""), "- May hold a weapon in each hand\n"
                                                       "- Humanoids only");
            row.icon = kGlyphTick;
            s.rows.push_back(std::move(row));
        }
        out.push_back(std::move(s));
    }
    {
        const auto &g = live->generalData;
        SheetSection s{"General", {}, {}};
        s.rows.push_back(note(Row("Offensive", chance(g.offensiveMult)), "- Higher: attacks more often\n"
                                                                         "- More power attacks\n"
                                                                         "- Paired with Defensive"));
        s.rows.push_back(note(Row("Defensive", chance(g.defensiveMult)), "- Higher: blocks more, holds it longer\n"
                                                                         "- Bashes more, given a shield or a weapon"));
        s.rows.push_back(note(Row("Group Offensive", chance(g.groupOffensiveMult)),
                              "- Replaces Offensive when several attack one target\n"
                              "- Higher: stays offensive in a crowd"));
        out.push_back(std::move(s));
    }
    {
        // The six that decide what she prefers to hold.
        const auto &g = live->generalData;
        SheetSection s{"Equipment Scores", {}, {}};
        constexpr const char *kScore = "- Multiplies the damage of attacks of this kind\n"
                                       "- The highest score is what gets used\n"
                                       "- A weak weapon needs a high score to beat a strong spell";
        s.rows.push_back(note(Row("Melee", score(g.meleeScoreMult)), kScore));
        s.rows.push_back(note(Row("Magic", score(g.magicScoreMult)), kScore));
        s.rows.push_back(note(Row("Ranged", score(g.rangedScoreMult)), kScore));
        s.rows.push_back(note(Row("Staff", score(g.staffScoreMult)), kScore));
        s.rows.push_back(note(Row("Shout", score(g.shoutScoreMult)), kScore));
        s.rows.push_back(note(Row("Unarmed", score(g.unarmedScoreMult)), kScore));
        out.push_back(std::move(s));
    }
    {
        const auto &m = live->meleeData;
        SheetSection s{"Melee", {}, {}};
        s.rows.push_back(note(Row("Attack, Staggered", score(m.attackIncapacitatedMult)),
                              "- Higher: attacks a staggered target more"));
        s.rows.push_back(note(Row("Power Attack, Staggered", score(m.powerAttackIncapacitatedMult)),
                              "- Higher: power-attacks a staggered target more"));
        s.rows.push_back(note(Row("Power Attack, Blocking", score(m.powerAttackBlockingMult)),
                              "- Higher: power-attacks a blocking target more\n"
                              "- Breaks the block"));
        s.rows.push_back(note(Row("Bash", score(m.bashMult)), "- Higher: bashes more, with a shield or a bash attack\n"
                                                              "- A bash can stagger"));
        s.rows.push_back(note(Row("Bash, Recoiled", score(m.bashRecoilMult)),
                              "- Higher: bashes a target recoiling from its blocked attack"));
        s.rows.push_back(note(Row("Bash, Attacking", score(m.bashAttackMult)), "- Higher: bashes a target mid-attack"));
        s.rows.push_back(note(Row("Bash, Power Attacking", score(m.bashPowerAttackMult)),
                              "- Higher: bashes a target mid-power-attack"));
        out.push_back(std::move(s));
    }
    {
        // Only the active pair: dueling circles and falls back, flanking
        // keeps a distance and stalks. The other pair is dead data.
        const auto &c = live->closeRangeData;
        SheetSection s{"Range", {}, {}};
        if (flanking)
        {
            s.rows.push_back(
                note(Row("Flank Distance", chance(c.flankDistanceMult)), "- Distance kept while flanking"));
            s.rows.push_back(
                note(Row("Stalk Time", chance(c.stalkTimeMult)), "- Time spent flanking before attacking"));
        }
        else
        {
            s.rows.push_back(note(Row("Circle", chance(c.circleMult)), "- Higher: circles the target more"));
            s.rows.push_back(note(Row("Fallback", chance(c.fallbackMult)), "- Chance to back off"));
        }
        s.rows.push_back(note(Row("Strafe", chance(live->longRangeData.strafeMult)),
                              "- Higher: strafes more to dodge projectiles at range"));
        out.push_back(std::move(s));
    }
    return out;
}

// The entry points by number, from the engine's enum (BGSEntryPoint.h),
// in the Creation Kit's words: "Mod Attack Damage", "Mod Spell Cost".
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
        return Row("Ability", ability->ability && ability->ability->GetName() ? ability->ability->GetName() : "?");
    }
    case Type::kQuest:
        // The quest entry's record is not modelled in this CommonLibSSE
        // fork; the kind is all that can be said.
        return Row("Quest", "a stage set");
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
            value = "+ a range";
            break;
        case Function::kAddActorValueMult:
            value = "+ a share of an actor value";
            break;
        case Function::kAddLeveledList:
            value = "a leveled list";
            break;
        case Function::kAddActivateChoice:
            value = "an activate choice";
            break;
        case Function::kSetToActorValueMult:
            value = "= a share of an actor value";
            break;
        case Function::kMultiplyActorValueMult:
            value = "x a share of an actor value";
            break;
        case Function::kMultiplyOnePlusActorValueMult:
            value = "x (1 + a share of an actor value)";
            break;
        case Function::kSetText:
            value = "a text";
            break;
        default:
            break;
        }
        if (dataType == DataType::kSpellItem)
        {
            const auto *spell = static_cast<const RE::BGSEntryPointFunctionDataSpellItem *>(data)->spell;
            value = spell && spell->GetName() ? spell->GetName() : "a spell";
        }
        return Row(name, value);
    }
    default:
        return Row("Entry", "?");
    }
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
        p.name = perk->GetName() ? perk->GetName() : "?";
        RE::BSString text;
        perk->GetDescription(text, perk);
        p.description = text.c_str() ? text.c_str() : "";

        SheetSection info{"Perk", {}, {}};
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", perk->GetFormID());
        info.rows.push_back(Row("Base ID", id));
        if (ranks > 1)
            info.rows.push_back(Row("Rank", std::to_string(rank) + " / " + std::to_string(ranks)));
        if (!skill.empty())
            info.rows.push_back(Row("Skill", skill));
        if (perk->data.hidden)
            info.rows.push_back(Row("Hidden", "yes"));
        p.sections.push_back(std::move(info));

        SheetSection entries{"Entries", {}, {}};
        for (const auto *entry : perk->perkEntries)
            if (entry)
                entries.rows.push_back(EntryRow(entry));
        if (!entries.rows.empty())
            p.sections.push_back(std::move(entries));
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
            for (const TreePerk &entry : TreePerks(value))
            {
                if (!actor->HasPerk(entry.perk))
                    continue;
                if (entry.perk->nextPerk && actor->HasPerk(entry.perk->nextPerk))
                    continue;
                page(entry.perk, entry.rank, entry.ranks, skillName ? skillName : "");
            }
        }
    }
    if (const auto *base = actor->GetActorBase(); base && base->perks)
        for (std::uint32_t i = 0; i < base->perkCount; ++i)
            if (auto *perk = base->perks[i].perk; perk && !perk->data.hidden && actor->HasPerk(perk))
                page(perk, base->perks[i].currentRank, 1, "");
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
    // rather than a wiki (docs/RESEARCH.md, "Skill modifiers").
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
        // "-17% cost". Power first, then the other, as the two read best.
        const auto add = [&row](const std::string &text) {
            row.modifiers += (row.modifiers.empty() ? "" : ", ") + text;
        };
        if (k.mod.effect && k.power.effect && std::string_view(k.mod.effect) == k.power.effect)
        {
            // One quantity, two factors: multiply them and show the change.
            if (m != 0.0f || p != 0.0f)
            {
                const double factor = (1.0 + k.mod.sign * m / 100.0) * (1.0 + k.power.sign * p / 100.0);
                add(Fmt("%+.0f%% ", (factor - 1.0) * 100.0) + k.mod.effect);
            }
        }
        else
        {
            if (p != 0.0f)
                add(Fmt("%+.0f%% ", k.power.sign * p) + k.power.effect);
            if (m != 0.0f)
                add(Fmt("%+.0f%% ", k.mod.sign * m) + k.mod.effect);
        }

        // Each modifier by its source: the gauntlets, the potion, and what
        // is left to perks. A line per source, "+20% damage" each.
        const auto bySource = [&](const Modifier &mod, float total) {
            if (!mod.effect || total == 0.0f)
                return;
            float explained = 0.0f;
            for (const Contribution &c : Contributions(actor, mod.value))
            {
                row.note += (row.note.empty() ? "" : "\n") + c.source + ": " + Fmt("%+.0f%% ", mod.sign * c.amount) +
                            mod.effect;
                explained += c.amount;
            }
            if (const float rest = total - explained; std::abs(rest) > 0.05f)
                row.note += (row.note.empty() ? "" : "\n") + std::string("Perks: ") + Fmt("%+.0f%% ", mod.sign * rest) +
                            mod.effect;
        };
        bySource(k.mod, m);
        bySource(k.power, p);

        row.detail = OwnedPerks(actor, k.value);
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
        {AV::kOneHanded, {{AV::kOneHandedModifier, "damage", +1}, {AV::kOneHandedPowerModifier, "damage", +1}}},
        {AV::kTwoHanded, {{AV::kTwoHandedModifier, "damage", +1}, {AV::kTwoHandedPowerModifier, "damage", +1}}},
        {AV::kBlock, {{AV::kBlockModifier, "blocked", +1}, {AV::kBlockPowerModifier, "blocked", +1}}},
        {AV::kSmithing, {{AV::kSmithingModifier, "tempering", +1}, {AV::kSmithingPowerModifier, "tempering", +1}}},
        {AV::kHeavyArmor, {{AV::kHeavyArmorModifier, "damage", -1}, {AV::kHeavyArmorPowerModifier, "damage", -1}}},
        {AV::kLightArmor, {{AV::kLightArmorModifier, "damage", -1}, {AV::kLightArmorPowerModifier, "damage", -1}}},
        {AV::kArchery, {{AV::kMarksmanModifier, "damage", +1}, {AV::kMarksmanPowerModifier, "damage", +1}}},
        {AV::kPickpocket, {{AV::kPickpocketModifier, "chance", +1}, {AV::kPickpocketPowerModifier, "chance", +1}}},
        {AV::kLockpicking,
         {{AV::kLockpickingModifier, "sweet spot", +1}, {AV::kLockpickingPowerModifier, "sweet spot", +1}}},
        {AV::kSneak, {{AV::kSneakingModifier, "stealth", +1}, {AV::kSneakingPowerModifier, "stealth", +1}}},
        {AV::kAlchemy,
         {{AV::kAlchemyModifier, "potion strength", +1}, {AV::kAlchemyPowerModifier, "potion strength", +1}}},
        // Sell prices up and buy prices down by the same factor: "better prices".
        {AV::kSpeech,
         {{AV::kSpeechcraftModifier, "better prices", +1}, {AV::kSpeechcraftPowerModifier, "better prices", +1}}},
        {AV::kAlteration, {{AV::kAlterationModifier, "cost", -1}, {AV::kAlterationPowerModifier, "duration", +1}}},
        {AV::kConjuration, {{AV::kConjurationModifier, "cost", -1}, {AV::kConjurationPowerModifier, "duration", +1}}},
        {AV::kDestruction, {{AV::kDestructionModifier, "cost", -1}, {AV::kDestructionPowerModifier, "damage", +1}}},
        {AV::kIllusion, {{AV::kIllusionModifier, "cost", -1}, {AV::kIllusionPowerModifier, "magnitude", +1}}},
        {AV::kRestoration, {{AV::kRestorationModifier, "cost", -1}, {AV::kRestorationPowerModifier, "healing", +1}}},
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
            found.push_back({name && *name ? name : (info->enumName ? info->enumName : "?"), value, info->unk124});
        }
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.name < b.name; });

    struct Category
    {
        std::uint32_t code;
        const char *title;
    };
    constexpr Category kCategories[] = {{1, "Warrior"}, {3, "Thief"}, {2, "Magic"}, {0, "Other Skills"}};
    for (const Category &category : kCategories)
    {
        SheetSection s{category.title, {}, {}};
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
        if (!s.rows.empty())
            out.push_back(std::move(s));
    }

    // The perks the follower holds that sit in no skill's tree: a mod's
    // loose perk, a race's, a quest's. Read off the base record's perk
    // list; the hidden ones (the two boost perks every actor carries) and
    // the unnamed stay out.
    {
        std::unordered_set<const RE::BGSPerk *> inTrees;
        for (const Found &f : found)
            for (const TreePerk &entry : TreePerks(f.value))
                inTrees.insert(entry.perk);
        SheetSection s{"Other Perks", {}, {}};
        if (const auto *base = actor->GetActorBase(); base && base->perks)
        {
            for (std::uint32_t i = 0; i < base->perkCount; ++i)
            {
                auto *perk = base->perks[i].perk;
                if (!perk || perk->data.hidden || inTrees.contains(perk) || !actor->HasPerk(perk))
                    continue;
                const char *name = perk->GetName();
                if (!name || !*name)
                    continue;
                RE::BSString text;
                perk->GetDescription(text, perk);
                SheetRow row = Row(name, base->perks[i].currentRank > 1 ? std::to_string(base->perks[i].currentRank)
                                                                        : std::string());
                row.modifiers = text.c_str() ? text.c_str() : "";
                row.form = perk->GetFormID();
                s.rows.push_back(std::move(row));
            }
        }
        std::sort(s.rows.begin(), s.rows.end(), [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        if (!s.rows.empty())
            out.push_back(std::move(s));
    }

    return out;
}

RE::SpellItem *FindSpell(std::uint32_t form)
{
    if (form == 0)
        return nullptr;
    return RE::TESForm::LookupByID<RE::SpellItem>(form);
}

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
        view.name = summon->GetName() ? summon->GetName() : "?";
        view.level = summon->GetLevel();
        view.health = ReadStat(summon.get(), RE::ActorValue::kHealth);
        view.magicka = ReadStat(summon.get(), RE::ActorValue::kMagicka);
        view.stamina = ReadStat(summon.get(), RE::ActorValue::kStamina);
        // The commanding effect runs on the FOLLOWER: its duration less its
        // elapsed time is how long the summon has left. A reanimate's effect
        // is a ReanimateEffect; a summon's a SummonCreatureEffect.
        if (const auto *effect = commanded.activeEffect)
        {
            if (effect->duration > 0.0f)
                view.remaining = (std::max)(0.0f, effect->duration - effect->elapsedSeconds);
            view.raised = effect->GetBaseObject() &&
                          effect->GetBaseObject()->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate;
        }
        view.sheet = BuildCharacterSheet(summon.get());
        out.push_back(std::move(view));
    }
    return out;
}

RE::TESObjectWEAP *PoisonableWeaponIn(RE::Actor *actor, bool left)
{
    if (!actor)
        return nullptr;
    auto *object = actor->GetEquippedObject(left);
    auto *weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon)
        return nullptr;
    // What the inventory menu offers a poison to: any weapon but a staff
    // (read from its poisoning routine, which checks that one type).
    // Unarmed is a weapon record too and never in a bag. A two-hander or a
    // bow reports from the right hand and the left reports it again; the
    // dose sits on the one entry either way.
    const auto type = weapon->GetWeaponType();
    if (type == RE::WEAPON_TYPE::kStaff || type == RE::WEAPON_TYPE::kHandToHandMelee)
        return nullptr;
    if (left && actor->GetEquippedObject(false) == weapon)
        return nullptr; // the right hand's two-hander, seen from the left
    return weapon;
}

RE::TESObjectWEAP *WeaponToPoison(RE::Actor *actor)
{
    for (const bool left : {false, true})
    {
        auto *weapon = PoisonableWeaponIn(actor, left);
        if (weapon && !WeaponPoisoned(actor, weapon))
            return weapon;
    }
    return nullptr;
}

bool WeaponPoisoned(RE::Actor *actor, RE::TESObjectWEAP *weapon)
{
    if (!actor || !weapon)
        return false;
    auto inventory = actor->GetInventory([weapon](RE::TESBoundObject &c) { return &c == weapon; });
    const auto found = inventory.find(weapon);
    if (found == inventory.end() || !found->second.second)
        return false;
    // The poison sits on the worn copy's extra list, which is what the
    // engine's IsPoisoned reads across every list of the entry.
    return found->second.second->IsPoisoned();
}

RE::TESObjectWEAP *WeaponIn(RE::Actor *actor, bool left)
{
    if (!actor)
        return nullptr;
    auto *object = actor->GetEquippedObject(left);
    auto *weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee)
        return nullptr;
    if (left && actor->GetEquippedObject(false) == weapon)
        return nullptr; // the right hand's two-hander, seen from the left
    return weapon;
}

WeaponCharge ChargeOf(RE::Actor *actor, RE::TESObjectWEAP *weapon)
{
    WeaponCharge out;
    if (!actor || !weapon)
        return out;
    auto inventory = actor->GetInventory([weapon](RE::TESBoundObject &c) { return &c == weapon; });
    const auto found = inventory.find(weapon);
    auto *entry = found != inventory.end() ? found->second.second.get() : nullptr;

    // The record's enchantment and full charge, or a player-made one's on
    // the entry (ExtraEnchantment carries both). What is left is
    // ExtraCharge, absent for a weapon never used. The same reading as
    // the engine's recharge routine.
    RE::EnchantmentItem *ench = weapon->formEnchanting;
    float max = static_cast<float>(weapon->amountofEnchantment);
    float charge = max;
    if (entry && entry->extraLists)
    {
        for (auto *list : *entry->extraLists)
        {
            if (!list)
                continue;
            if (auto *xEnch = list->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment)
            {
                ench = xEnch->enchantment;
                max = static_cast<float>(xEnch->charge);
                charge = max;
            }
            if (auto *xCharge = list->GetByType<RE::ExtraCharge>())
                charge = xCharge->charge;
        }
    }
    if (!ench || max <= 0.0f)
        return out;
    // In hand, the live charge is an ACTOR VALUE -- RightItemCharge or
    // LeftItemCharge, what the HUD's charge meter reads -- and the item's
    // own record is only written back on unequip. Measured 2026-09-08: a
    // staff cast down to 491 showed no charge record until it was swapped
    // hands, and then 491 appeared. So the hand it is in says where to
    // read; in the bag, the record.
    if (auto *owner = actor->AsActorValueOwner())
    {
        if (actor->GetEquippedObject(false) == weapon)
            charge = owner->GetActorValue(RE::ActorValue::kRightItemCharge);
        else if (actor->GetEquippedObject(true) == weapon)
            charge = owner->GetActorValue(RE::ActorValue::kLeftItemCharge);
    }
    out.enchanted = true;
    out.charge = (std::min)((std::max)(charge, 0.0f), max);
    out.maxCharge = max;
    out.costPerHit = ench->CalculateMagickaCost(actor);

    return out;
}

float SoulCharge(RE::SOUL_LEVEL level)
{
    const char *setting = nullptr;
    switch (level)
    {
    case RE::SOUL_LEVEL::kPetty:
        setting = "iSoulLevelValuePetty";
        break;
    case RE::SOUL_LEVEL::kLesser:
        setting = "iSoulLevelValueLesser";
        break;
    case RE::SOUL_LEVEL::kCommon:
        setting = "iSoulLevelValueCommon";
        break;
    case RE::SOUL_LEVEL::kGreater:
        setting = "iSoulLevelValueGreater";
        break;
    case RE::SOUL_LEVEL::kGrand:
        setting = "iSoulLevelValueGrand";
        break;
    default:
        return 0.0f;
    }
    auto *settings = RE::GameSettingCollection::GetSingleton();
    auto *value = settings ? settings->GetSetting(setting) : nullptr;
    return value ? static_cast<float>(value->GetInteger()) : 0.0f;
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

} // namespace ft::game
