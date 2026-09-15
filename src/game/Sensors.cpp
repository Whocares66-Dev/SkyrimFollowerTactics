#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/Effects.h"

#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
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
    ForEachActiveEffect(actor, [&out](RE::ActiveEffect &ae) {
        // duration 0 is an instant effect that has already happened.
        if (!(ae.duration > 0.0f && ae.elapsedSeconds < ae.duration))
            return;
        const char *name = ae.effect->baseEffect->GetFullName();
        if (name && *name)
            out.emplace_back(name);
    });
    return out;
}

// Defined further down, in this same unnamed namespace, with the sheets.
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
float GameSetting(const char *name, float vanilla);
std::int32_t GameSetting(const char *name, std::int32_t vanilla);

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
    // goes in the permanent one, docs/MODIFIERS.md): a
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
    // The same spell can be in the record's list and among the added
    // spells; once is enough, and every caller wants it so.
    std::unordered_set<const RE::SpellItem *> seen;
    const auto once = [&](RE::SpellItem *spell) {
        if (spell && seen.insert(spell).second)
            fn(spell);
    };
    const auto walk = [&once](const RE::TESSpellList::SpellData *list) {
        if (!list)
            return;
        for (std::uint32_t i = 0; i < list->numSpells; ++i)
            once(list->spells[i]);
    };
    if (auto *npc = actor->GetActorBase())
        walk(npc->GetSpellList());
    if (auto *race = actor->GetRace())
        walk(race->actorEffects);
    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
        once(spell);
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

// Whether the actor can dual cast a spell: the perk system's answer to the
// Can Dual Cast Spell entry point for this spell -- each school's Dual
// Casting perk sets it for its own school, so a mod's perk counts the same
// -- and a record that leaves a hand free, by its equip slot: vanilla's
// master spells take both hands, a mod's may not, and the level says
// nothing.
bool CanDualCast(RE::Actor *actor, RE::SpellItem *spell)
{
    if (!actor || !spell || !IsCastable(spell) || spell->IsTwoHanded())
        return false;
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

std::vector<Contribution> Contributions(RE::Actor *actor, RE::ActorValue value)
{
    std::vector<Contribution> out;
    ForEachActiveEffect(actor, [&](RE::ActiveEffect &effect) {
        auto *ae = &effect;
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
        out.push_back(
            {std::move(source), NameOr(base, ""), first ? ae->magnitude : ae->magnitude * base->data.secondAVWeight});
    });
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
            c.source += " (" + c.effect + ")";
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
        parts.push_back({"Hidden bonus (x" + std::to_string(pieces) + ")", {}, hidden});
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
            ft::Start(b, "Base", parts.base * scale);
        else
            ft::Add(b, "Base", parts.base * scale);
    }
    for (const Contribution &c : parts.sources)
        ft::Add(b, c.source, c.amount * scale);
}

namespace
{
// A value a line reads, opened out beneath it. In the value's own units,
// not the breakdown's: a multiplier beneath a recovery time read "+0.2 s".
// Empty for a base alone, which is the line's own figure again.
std::vector<ft::BreakdownLine> ValueLines(RE::Actor *actor, RE::ActorValue value, float reading)
{
    ft::Breakdown b;
    AddValueLines(b, PartsOf(actor, value));
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
    ft::Breakdown b;
    b.unit = unit;
    AddValueLines(b, PartsOf(actor, value));
    // A pool's maximum is the permanent value plus what effects add for
    // now; the damage taken is below it and is not a source. Every other
    // value is what it reads.
    const bool pool =
        value == RE::ActorValue::kHealth || value == RE::ActorValue::kMagicka || value == RE::ActorValue::kStamina;
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
            amount += "/s";
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
        row.extra = std::to_string(duration) + " s";
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
        row.aside = "Conditions not met";
    return row;
}

SheetSection EffectsOf(RE::Actor *actor, const RE::MagicItem *magic,
                       const std::function<float(const RE::Effect *)> &magnitude)
{
    SheetSection section{"Effects", {}, {}};
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
    for (const auto *effect : magic->effects)
    {
        if (!effect || !effect->baseEffect)
            continue;
        ConditionParties parties{actor, actor};
        if (!onSelf)
        {
            const bool hostile = magic->IsPoison() || effect->baseEffect->IsHostile();
            parties.subject = hostile ? enemy : nullptr;
        }
        // The record's magnitude is unsigned; a detrimental effect takes
        // it away.
        const float amount = magnitude(effect);
        section.rows.push_back(
            EffectEntryRow(*effect, effect->baseEffect->IsDetrimental() ? -amount : amount, parties));
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
        // and which therefore wants a page: listed as any other, running
        // and applied as it is, its page saying it is hidden. A hidden
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
        row.form = base->GetFormID();
        row.sourceForm = ae->spell ? ae->spell->GetFormID() : 0;
        row.applied = EffectApplies(actor, base);
        // Running but not acting, by the engine's flag, which the sheet's
        // totals read too (ForEachActiveEffect). Not by asking the
        // conditions: the engine asks an effect record's once, when the
        // effect lands, and Adamant's Bastion asks there whether the cast
        // was dual, which reads false ever after (docs/CONDITIONS.md 10).
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
                row.linkForm = row.sourceForm;
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
            SheetSection page{"Effect", {}, {}};
            // Its conditions as the engine asks them of a running effect:
            // of the one it is on, and of whoever cast it -- the player,
            // for a Bastion Dragonhide on a follower. For reference: the
            // row's grey is the flag.
            const auto caster = ae->caster.get();
            SheetRow line = EffectEntryRow(*ae->effect, ae->magnitude, {actor, caster.get()});
            line.aside = row.active ? "" : "Inactive";
            if (ae->duration > 0.0f)
            {
                line.extra = RemainingText(ae->duration);
                line.remaining = row.remainingText;
            }
            line.link = row.source;
            // Whoever cast it, when it was not the follower: the player's
            // Courage, an enemy's Fury.
            if (caster && caster.get() != actor && caster->GetName() && *caster->GetName())
                line.link += std::string(" (") + caster->GetName() + ")";
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

// What an actor is in the middle of, as docs/CONDITIONS.md 2 reads it: the
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

// The margin a blow's reach gets for the enemy's own body, since the
// snapshot's distances are centre to centre: a humanoid's half-width and a
// step. A giant's body is wider than that, so a blow at one is judged too
// far a little before it is.
constexpr float kBodyMargin = 40.0f;

BlowPlan PlanPowerAttack(RE::Actor *actor)
{
    BlowPlan plan;
    if (!actor)
        return plan;
    RE::TESForm *rightHeld = actor->GetEquippedObject(false);
    RE::TESForm *leftHeld = actor->GetEquippedObject(true);
    auto *right = rightHeld ? rightHeld->As<RE::TESObjectWEAP>() : nullptr;
    auto *left = leftHeld ? leftHeld->As<RE::TESObjectWEAP>() : nullptr;

    // The attack, by the hands (core's rule); its stamina multiplier is the
    // race record's for that attack (1 for a one-hand or two-hand power
    // attack, 0.5 for the dual-wield one, vanilla's humanoid races).
    float weight = 0.0f;
    float attackMult = 1.0f;
    const RE::TESObjectWEAP *priced = nullptr;
    // A swing names a hand with a weapon in it, and DescribeHands read the
    // same two objects, so the checks below never fail; they are for the
    // reader and the analyser, per case because the analyser does not
    // carry one check across a switch.
    switch (ft::SwingWith(DescribeHands(actor)))
    {
    case ft::Swing::Both:
        if (!right || !left)
            return plan;
        plan.event = "attackPowerStartDualWield";
        weight = right->GetWeight() + left->GetWeight();
        attackMult = 0.5f;
        priced = right;
        break;
    case ft::Swing::Right:
        if (!right)
            return plan;
        plan.event = "attackPowerStartInPlace";
        weight = right->GetWeight();
        priced = right;
        break;
    case ft::Swing::Left:
        if (!left)
            return plan;
        plan.event = "attackPowerStartInPlaceLeftHand";
        weight = left->GetWeight();
        priced = left;
        break;
    case ft::Swing::Fists:
        plan.event = "attackPowerStartInPlace";
        break;
    case ft::Swing::None:
        return plan;
    }

    // The cost: (fStaminaAttackWeaponBase + weight * fStaminaAttackWeaponMult)
    // times the attack's multiplier, then the actor's perks through the Mod
    // Power Attack Stamina entry point, which takes the weapon. Vanilla's
    // settings are 20 and 1. The formula is UESP's; not yet checked against
    // the engine.
    float cost =
        (GameSetting("fStaminaAttackWeaponBase", 20.0f) + weight * GameSetting("fStaminaAttackWeaponMult", 1.0f)) *
        attackMult;
    if (priced)
        RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModPowerAttackStamina, actor,
                                            const_cast<RE::TESObjectWEAP *>(priced), &cost);
    plan.stamina = (std::max)(0.0f, cost);
    // The engine's own reach for the actor and what they hold -- the weapon's
    // reach times fCombatDistance, or the race's unarmed reach, times the
    // actor's scale (docs/ACTIONS.md 6) -- and the margin for the enemy's
    // body.
    plan.reach = actor->GetReach() + kBodyMargin;
    return plan;
}

BlowPlan PlanBash(RE::Actor *actor, bool power)
{
    BlowPlan plan;
    if (!actor || !ft::BashesWith(DescribeHands(actor)))
        return plan;
    plan.event = power ? "bashPowerStart" : "bashStart";
    // The cost is the setting for the kind of bash -- fStaminaBashBase 35,
    // fStaminaPowerBashBase 55 in vanilla -- times the attack's multiplier,
    // 1 for both in the race data. No perk entry point prices a bash. Not
    // yet checked against the engine.
    plan.stamina = power ? GameSetting("fStaminaPowerBashBase", 55.0f) : GameSetting("fStaminaBashBase", 35.0f);
    // The bash's own reach setting (fCombatBashReach, 141 in vanilla) at the
    // actor's scale, with the same margin for the enemy's body as a swing.
    plan.reach = GameSetting("fCombatBashReach", 141.0f) * actor->GetScale() + kBodyMargin;
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

// The kind of being, for the Type condition (docs/CONDITIONS.md 2a). The
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
    s.voiceRecovery = VoiceRecoveryOf(actor);
    for (const auto kind : {ft::ActionKind::PowerAttack, ft::ActionKind::Bash, ft::ActionKind::PowerBash})
    {
        const BlowPlan plan = PlanBlow(actor, kind);
        s.BlowFor(kind) = {plan.Possible(), plan.stamina, plan.reach};
    }

    s.traits = ReadTraits(actor);

    // Whom the follower is fighting, as the engine sees it: what "current
    // target" resolves to.
    s.currentTarget = LiveTargetOf(actor);

    // The party and the enemies, by definition (docs/CONDITIONS.md 6). An
    // ally is the player and every other actor with the teammate flag; an
    // enemy is anyone the compass paints red for the player, in combat and
    // hostile to them. One walk of the loaded actors, alive ones only, each
    // read the same way: the player is the first ally.
    auto *player = RE::PlayerCharacter::GetSingleton();
    const auto viewOf = [&](RE::Actor *other) {
        ft::ActorView view;
        view.id = other->GetFormID();
        view.health = ReadStat(other, RE::ActorValue::kHealth);
        view.magicka = ReadStat(other, RE::ActorValue::kMagicka);
        view.stamina = ReadStat(other, RE::ActorValue::kStamina);
        view.distance = actor->GetPosition().GetDistance(other->GetPosition());
        view.target = LiveTargetOf(other);
        view.traits = ReadTraits(other);
        return view;
    };
    if (player && !player->IsDead())
        s.allies.push_back(viewOf(player));
    if (auto *lists = RE::ProcessLists::GetSingleton())
    {
        lists->ForEachHighActor([&](RE::Actor *otherPtr) {
            if (!otherPtr)
                return RE::BSContainer::ForEachResult::kContinue;
            RE::Actor &other = *otherPtr;
            if (&other == actor || &other == player || other.IsDead())
                return RE::BSContainer::ForEachResult::kContinue;
            if (other.IsPlayerTeammate())
                s.allies.push_back(viewOf(&other));
            else if (player && other.IsInCombat() && other.IsHostileToActor(player))
                s.enemies.push_back(viewOf(&other));
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
    if (s.currentTarget != 0 && !s.Enemy(s.currentTarget))
    {
        if (auto *target = RE::TESForm::LookupByID<RE::Actor>(s.currentTarget))
            s.enemies.push_back(viewOf(target));
    }

    ScanPotions(actor, s.potions);
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

    // Spells: what they know, what is running, what is in hand. All three are
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
    // A scroll carried is "known" for a Scroll rule: knowing and carrying
    // are the one question for it, and it costs no magicka.
    for (const auto &[object, entry] :
         actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::Scroll); }))
        if (object && entry.first > 0)
            s.spells.known.push_back(object->GetFormID());
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
        // Their cost, not the base cost: CalculateMagickaCost applies their skill
        // and perks, which is what the AI will charge them.
        const bool dualable = CanDualCast(actor, spell);
        s.spells.costs.push_back({spell->GetFormID(), spell->CalculateMagickaCost(actor), dualable,
                                  dualable ? DualCastCost(actor, spell) : 0.0f});
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
    s.pins = PinsOf(s.self);

    ForEachActiveEffect(actor, [&s](RE::ActiveEffect &ae) {
        // Instant effects have already happened and never lapse, so
        // treating them as "still up" would block the rule forever.
        if (!ae.spell || ae.duration <= 0.0f || ae.elapsedSeconds >= ae.duration)
            return;
        s.spells.active.push_back(ae.spell->GetFormID());
    });

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
                name += std::string(" (") + SoulName(level) + ")";
            out.push_back({object->GetFormID(), name, static_cast<int>(count), ft::ConsumableKind::SoulGem, {}});
            continue;
        }
        const auto kind = ConsumableKindOf(object);
        if (!kind)
            continue;
        std::vector<std::string> effects;
        for (const auto &effect : EffectsOf(object->As<RE::MagicItem>(), *kind))
            effects.push_back(effect.name);
        out.push_back({object->GetFormID(), NameOr(object, "?"), static_cast<int>(count), *kind, std::move(effects)});
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

        std::string name = NameOr(spell, "");
        if (name.empty())
            return; // nameless entries are internal; nothing to show a player
        bool reanimate = false;
        for (const auto *effect : spell->effects)
            reanimate =
                reanimate || (effect && effect->baseEffect &&
                              effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate);
        out.push_back(SpellOption{
            spell->GetFormID(), std::move(name), spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf,
            spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation, reanimate,
            !power && CanDualCast(actor, spell), power ? SpellOption::Kind::Power : SpellOption::Kind::Spell});
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
        bool reanimate = false;
        for (const auto *effect : scroll->effects)
            reanimate =
                reanimate || (effect && effect->baseEffect &&
                              effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate);
        out.push_back(SpellOption{scroll->GetFormID(), std::move(name),
                                  scroll->GetDelivery() == RE::MagicSystem::Delivery::kSelf,
                                  scroll->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation, reanimate, false,
                                  SpellOption::Kind::Scroll});
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
                if (!shout || IsWrapperShout(shout->GetFormID()) || !shout->GetName() || !*shout->GetName())
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
    return perk && perk->perkEntries.empty() ? "Does nothing" : "Inactive";
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

std::vector<SheetRow> OwnedPerks(RE::Actor *actor, RE::ActorValue skill)
{
    std::vector<SheetRow> rows;
    for (const TreePerk &entry : TreePerks(skill))
    {
        if (!actor->HasPerk(entry.perk))
            continue;
        if (entry.perk->nextPerk && actor->HasPerk(entry.perk->nextPerk))
            continue; // a higher rank is held; that one gets the row

        std::string label = PerkName(entry.perk);
        if (label.empty())
            label = "?";

        const std::string rank = entry.ranks > 1 ? std::to_string(entry.rank) + "/" + std::to_string(entry.ranks) : "";
        SheetRow row = Row(std::move(label), rank);
        row.modifiers = entry.description;
        row.form = entry.perk->GetFormID(); // the name opens the perk's page
        if (const char *aside = PerkAside(actor, entry.perk))
            row.aside = aside;
        rows.push_back(std::move(row));
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
        rows.push_back(Row("Weapon", NameOr(weapon, "?")));
        rows.back().form = weapon->GetFormID();
        // In their hands: the carried item, for its tempering. Each figure
        // as it applies now, written out on hover.
        const Carried carried = CarriedOf(actor, weapon);
        {
            SheetRow row;
            const float damage = WeaponDamage(actor, weapon, carried.entry.get(), &row.breakdown);
            row.label = "Damage";
            row.value = Fmt("%.0f", damage);
            rows.push_back(std::move(row));
        }
        // The critical pair in the details page's words and order. Speed,
        // reach and stagger are the record's and on that page. A critical
        // that never lands or lands for nothing is no critical: neither
        // row, rather than one of them beside a 0.
        {
            SheetRow chance;
            const float percent = CritChance(actor, weapon, &chance.breakdown);
            if (const auto critDamage = weapon->GetCritDamage(); critDamage > 0 && percent >= 0.5f)
            {
                rows.push_back(Row("Critical Damage", std::to_string(critDamage)));
                chance.label = "Critical Chance";
                chance.value = Fmt("%.0f%%", percent);
                rows.push_back(std::move(chance));
            }
        }
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
        {
            SheetRow row = Row("Cost", Fmt("%.0f", spell->CalculateMagickaCost(actor)));
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
                what += " for " + Fmt("%.0f", duration) + " s";
            rows.push_back(Row("Effect", what));
        }
        return;
    }

    if (auto *armor = held->As<RE::TESObjectARMO>())
    {
        const bool shield = armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        rows.push_back(Row(shield ? "Shield" : "Held", NameOr(armor, "?")));
        rows.back().form = armor->GetFormID();
        const Carried carried = CarriedOf(actor, armor);
        SheetRow row;
        const float rating = ArmorRating(actor, armor, carried.entry.get(), &row.breakdown);
        row.label = "Armor";
        row.value = Fmt("%.0f", rating);
        rows.push_back(std::move(row));
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
// priority (docs/MODIFIERS.md).
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
// handlers read them (docs/MODIFIERS.md): the first is an actor value's
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
        // A perk that reads a value is named for the value's sources, not
        // for itself: "Deathbrand Gauntlets x 1.25", not the controller
        // perk that turned the gauntlets' +25 into a factor. One line per
        // source, each with its own share, one for the value's base, and
        // one for what neither explains. Two sources'
        // factors miss their product by the cross term, an Other line;
        // one source, the common case, is exact.
        std::vector<ft::BreakdownLine> bySource;
        const auto withValue = [&](bool multiply, bool onePlus) {
            const ValueParts parts = PartsOf(actor, av);
            float explained = parts.base;
            const auto push = [&](std::string label, float amount) {
                ft::BreakdownLine each;
                each.op = multiply ? ft::Op::Multiply : ft::Op::Add;
                each.label = std::move(label);
                each.amount = (onePlus ? 1.0 : 0.0) + static_cast<double>(amount) * mult;
                bySource.push_back(std::move(each));
            };
            // Named for the value: a bare "Base" beside the weapon's own
            // read as the same thing.
            if (std::abs(parts.base) > 0.05f)
                push("Base " + ValueName(av), parts.base);
            for (const Contribution &c : parts.sources)
            {
                explained += c.amount;
                push(c.source, c.amount);
            }
            if (const float rest = value - explained; std::abs(rest) > 0.05f)
                push("Other", rest);
        };
        switch (entry->entryData.function.get())
        {
        case Fn::kSetValue:
            line.op = ft::Op::Start;
            line.label += " (set)";
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
                line.label += " (" + Fmt("%g", two[0]) + " to " + Fmt("%g", two[1]) + ")";
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

float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry, ft::Breakdown *out)
{
    if (!actor || !weapon)
        return 0.0f;
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    float damage = weapon->GetAttackDamage();
    ft::Start(b, "Base", damage);
    if (const float tempering = Tempering(entry); tempering != 1.0f)
    {
        damage *= tempering;
        ft::Multiply(b, "Tempering", tempering);
    }

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
    const float skillLevel = owner ? owner->GetActorValue(skill) : 0.0f;
    const bool player = actor->IsPlayerRef();
    const float lo = player ? pcMin : npcMin;
    const float hi = player ? pcMax : npcMax;
    const float curve = lo + (hi - lo) * skillLevel / 100.0f;
    damage *= curve;
    ft::Multiply(b, ValueName(skill) + " (" + Fmt("%.0f", skillLevel) + ")", curve).detail =
        ValueLines(actor, skill, skillLevel);

    // Perks, through the engine's own entry point, so Armsman and the rest
    // count exactly as they do in a swing. The entry point wants a target,
    // and there is none outside a fight; they stand in for it themself. A
    // perk that reads the target (against undead, say) evaluates against
    // them and so stays out of the figure -- the same figure the player's
    // own inventory menu shows, which has no target either. Fortify
    // One-handed and its kin count here too, for whoever holds the hidden
    // perk that reads them (every NPC in Nordic Souls; no follower in
    // vanilla): multiplying the value in by hand as well doubled it
    // (docs/MODIFIERS.md, 2026-09-13).
    (void)fortify;
    (void)fortifyPower;
    AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kModAttackDamage, {weapon, actor});
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModAttackDamage, actor, weapon, actor,
                                        &damage);

    // The multiplier on every physical hit (a Vampire Lord's, a mod's), 1
    // for plain, and flat points on the weapon's listed damage: both per
    // UESP's account of what the listed damage carries, not yet read off
    // the executable (docs/MODIFIERS.md).
    if (owner)
    {
        if (const float mult = owner->GetActorValue(AV::kAttackDamageMult); mult > 0.0f && mult != 1.0f)
        {
            damage *= mult;
            ft::Multiply(b, "Attack Damage Mult", mult).detail = ValueLines(actor, AV::kAttackDamageMult, mult);
        }
        if (const float flat = owner->GetActorValue(AV::kMeleeDamage); flat != 0.0f)
        {
            damage += flat;
            ft::Add(b, "Melee Damage", flat).detail = ValueLines(actor, AV::kMeleeDamage, flat);
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
    // off the executable (docs/MODIFIERS.md).
    AddEntryPointLines(b, actor, RE::BGSEntryPoint::ENTRY_POINT::kCalculateMyCriticalHitChance, {weapon, actor});
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kCalculateMyCriticalHitChance, actor, weapon,
                                        actor, &chance);
    b.total = chance;
    ft::Close(b);
    return chance;
}

float WordRecovery(RE::Actor *actor, float recovery, ft::Breakdown *out)
{
    ft::Breakdown local;
    ft::Breakdown &b = out ? *out : local;
    b = {};
    b.unit = " s";
    ft::Start(b, "Base", recovery);
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (const float mult = owner ? owner->GetActorValue(RE::ActorValue::kShoutRecoveryMult) : 1.0f;
        mult > 0.0f && mult != 1.0f)
    {
        recovery *= mult;
        ft::Multiply(b, "Shout Recovery Mult", mult).detail =
            ValueLines(actor, RE::ActorValue::kShoutRecoveryMult, mult);
    }
    b.total = recovery;
    ft::Close(b);
    return recovery;
}

ft::Breakdown SpellCostBreakdown(RE::Actor *actor, const RE::SpellItem *spell)
{
    // The engine's own cost, read off the executable (docs/MODIFIERS.md):
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
        ft::Start(b, "Base", static_cast<float>(spell->data.costOverride));
    else
    {
        for (const auto *effect : spell->effects)
        {
            if (!effect || !effect->baseEffect)
                continue;
            ft::Add(b, NameOr(effect->baseEffect, "?"), effect->cost);
        }
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
    // (2026-09-13, docs/MODIFIERS.md): the record's rating plus the
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
    ft::Start(b, "Base", rating);

    // Tempering is flat points: one plus the item health's place between
    // the first and last health steps, times the armour smithing maximum
    // less one, floored at zero, so an untempered piece at health 1.0 gets
    // nothing. Doubled, before the floor, for a piece carrying the default
    // object Keyword Cuirass: Serana's tempered Vampire Armor rated 82 on
    // our sheet and 106 in the engine, the whole of an Other +24
    // (measured 2026-09-13, docs/MODIFIERS.md).
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
            ft::Add(b, body ? "Tempering (body, doubled)" : "Tempering", bonus);
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
            ft::Add(b, "Rounding", up);
    }

    // Perks: Juggernaut, Agile Defender and their kin, through the engine's
    // entry point for armour, which takes the piece and the value. A
    // Fortify Heavy Armor value is not multiplied in: the hidden perk that
    // reads it cuts incoming damage, not the rating (docs/RESEARCH.md 6),
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
        s.rows.push_back(Row("Name", NameOr(actor, "?")));
        auto *race = actor->GetRace();
        s.rows.push_back(Row("Race", NameOr(race, "?")));
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
            row.breakdown = ValueBreakdown(actor, RE::ActorValue::kSpeedMult, "%");
            s.rows.push_back(std::move(row));
        }
        s.rows.push_back(Row("Noise", Fmt("%.0f%%", av(RE::ActorValue::kMovementNoiseMult) * 100.0)));
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
        SheetSection right{both ? "Both Hands" : "Right Hand", {}, "Attack"};
        HandRows(actor, false, right.rows);
        SheetSection left{"Left Hand", {}, "Attack"};
        if (!both)
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
        // The share of a blow turned away, clamped at the cap: one
        // percent, the one that applies -- "582 (75%)" in a list that caps
        // at 75.
        SheetRow armorRow = Row("Armor", Fmt("%.0f", EffectiveArmor(actor)) + " (" +
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
        resist("Reflect", RE::ActorValue::kReflectDamage, false);
        // Magic first, with the chance to absorb a spell outright beside
        // it, then the elements, then poison; disease last, the one that
        // matters to the player alone.
        resist("Magic", RE::ActorValue::kResistMagic, true);
        resist("Spell Absorb", RE::ActorValue::kAbsorbChance, false);
        resist("Fire", RE::ActorValue::kResistFire, true);
        resist("Frost", RE::ActorValue::kResistFrost, true);
        resist("Shock", RE::ActorValue::kResistShock, true);
        resist("Poison", RE::ActorValue::kPoisonResist, true);
        resist("Disease", RE::ActorValue::kResistDisease, false);
        out.push_back(std::move(s));
    }

    {
        // The rate the follower actually regenerates at: the rate times its
        // multiplier. A buff lands on either: robes of Destruction's
        // "magicka regenerates 100% faster" is +100 on the multiplier, and
        // Mundus's Elfborn stone is +3 on the rate itself. Reading the
        // rate as it stands for the base hid the stone inside it
        // (2026-09-14).
        SheetSection s{"Regen", {}, {}};
        const auto regen = [&](const char *label, RE::ActorValue rate, RE::ActorValue mult) {
            const float current = av(rate);
            const float total = current * av(mult) / 100.0f;
            SheetRow row = Row(label, Fmt("%.2f%%", total));
            // Each source as the rate it adds, not the speed it multiplies
            // by: "+3.00%" for robes that double a 3% rate reads straight
            // off. A multiplier's source scales the whole rate, the rate's
            // own sources included, so the lines sum to the row; its base,
            // plain speed, is the rate's own lines.
            ft::Breakdown &b = row.breakdown;
            b.decimals = 2;
            b.unit = "%";
            AddValueLines(b, PartsOf(actor, rate));
            AddValueLines(b, {.base = 0.0f, .sources = Contributions(actor, mult)}, current / 100.0f);
            b.total = total;
            ft::Close(b);
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
    // The style tunes the combat AI; the player holds what they like.
    if (actor && actor->IsPlayerRef())
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
        // The six that decide what they prefer to hold.
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
    std::string call = name ? name : "Function " + std::to_string(id);

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
            args.push_back(form->IsPlayerRef()                   ? "Player"
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
            call += " on " + subject;
        break;
    case Object::kTarget:
        call += " on " + target;
        break;
    case Object::kCombatTarget:
        call += " on Combat Target";
        break;
    case Object::kRef: {
        // A particular reference, named in the condition: the player, as a
        // rule, for a perk given to followers that turns on with one of
        // the player's.
        const auto ref = data.runOnRef.get();
        if (ref && ref->IsPlayerRef())
            call += " on Player";
        else if (ref && ref->GetDisplayFullName() && *ref->GetDisplayFullName())
            call += std::string(" on ") + ref->GetDisplayFullName();
        else
            call += ref ? " on " + HexId(ref->GetFormID()) : " on Reference";
        break;
    }
    case Object::kLinkedRef:
        call += " on Linked Reference";
        break;
    case Object::kQuestAlias:
        call += " on Quest Alias";
        break;
    case Object::kPackData:
        call += " on Package Data";
        break;
    case Object::kEventData:
        call += " on Event Data";
        break;
    case Object::kCommandTarget:
        call += " on Command Target";
        break;
    }
    return call;
}

// A party by the name the panel gives it: the player is "Player", as
// everywhere in the panel.
std::string PartyName(RE::TESObjectREFR *ref)
{
    if (ref->IsPlayerRef())
        return "Player";
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
        // docs/CONDITIONS.md 10): the Subject, or through the Subject its
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
        std::string subject = on ? "" : parties.subject ? PartyName(parties.subject) : "Subject";
        std::string target = on ? "Target" : parties.target ? PartyName(parties.target) : "Target";
        if (swapped && !on)
            std::swap(subject, target);
        SheetRow row = Row(ConditionCall(data, subject, target) + (on ? std::string(" on ") + on : ""),
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
                row.extra = "N/A";
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
        return Row("Ability", NameOr(ability->ability, "?"));
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
        // The two-value record: a range's ends, or for the actor-value
        // functions the value read and its multiplier -- named, so the
        // page and the breakdown that reads the same entry agree on what
        // "One Handed Power Mod x 0.01" is (2026-09-13).
        const float *two =
            dataType == DataType::kTwoValue ? reinterpret_cast<const TwoValueData *>(data)->data : nullptr;
        const std::string share =
            two ? Fmt("%g", two[1]) + " x " + ValueName(static_cast<RE::ActorValue>(static_cast<int>(two[0])))
                : std::string("a share of an actor value");
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
            value = two ? "+ " + Fmt("%g", two[0]) + " to " + Fmt("%g", two[1]) : "+ a range";
            break;
        case Function::kAddActorValueMult:
            value = "+ " + share;
            break;
        case Function::kAddLeveledList:
            value = "a leveled list";
            break;
        case Function::kAddActivateChoice:
            value = "an activate choice";
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
            value = "a text";
            break;
        default:
            break;
        }
        if (dataType == DataType::kSpellItem)
        {
            const auto *spell = static_cast<const RE::BGSEntryPointFunctionDataSpellItem *>(data)->spell;
            value = NameOr(spell, "a spell");
        }
        return Row(name, value);
    }
    default:
        return Row("Entry", "?");
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

        SheetSection info{"Perk Details", {}, {}};
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", perk->GetFormID());
        info.rows.push_back(Row("Base ID", id));
        if (ranks > 1)
            info.rows.push_back(Row("Rank", std::to_string(rank) + " / " + std::to_string(ranks)));
        if (!skill.empty())
            info.rows.push_back(Row("Skill", skill));
        if (perk->data.hidden)
            info.rows.push_back(Row("Hidden", "yes"));
        // A tick while the perk does something for them; no row while not.
        if (PerkActive(actor, perk))
        {
            SheetRow active = Row("Active", "");
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
                who += (who.empty() ? "" : ", ") + readers[i];
            if (readers.size() > 4)
                who += ", +" + std::to_string(readers.size() - 4);
            info.rows.push_back(Row("Read by", who));
        }
        p.sections.push_back(std::move(info));

        // The effects: an entry each, with the conditions that gate it on
        // its owner beneath -- a mod's perk given to everyone is gated
        // there, on the power that turns it on -- greyed while they are
        // not met, as an effect's row is. Not the record's own conditions,
        // which are what the skill tree asks before the player may take
        // it, and nothing to an NPC.
        SheetSection effects{"Effects", {}, {}};
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
                    const std::string on = "argument " + std::to_string(tab + 1);
                    for (SheetRow &r : ConditionRows(point->conditions[tab], {actor, actor}, on.c_str()))
                        row.detail.push_back(std::move(r));
                }
            }
            if (!active)
                row.aside = "Conditions not met";
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
            SheetRow row = Row("Effect", "N/A");
            if (const auto *file = perk->GetFile(); file && !file->GetFilename().empty())
                row.note = "Record last changed by " + std::string(file->GetFilename());
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
            row.modifiers += (row.modifiers.empty() ? "" : ", ") + text;
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
                part(Fmt("%+.0f%% ", change) + k.mod.effect, {{&k.power, p}, {&k.mod, m}}, change);
            }
        }
        else
        {
            if (p != 0.0f)
                part(Fmt("%+.0f%% ", k.power.sign * p) + k.power.effect, {{&k.power, p}}, k.power.sign * p);
            if (m != 0.0f)
                part(Fmt("%+.0f%% ", k.mod.sign * m) + k.mod.effect, {{&k.mod, m}}, k.mod.sign * m);
        }

        // The Armor Perks value, which the engine adds to either armour
        // skill's multiplier for every piece worn (docs/MODIFIERS.md): on
        // both rows, with what set it on hover.
        if (k.value == AV::kHeavyArmor || k.value == AV::kLightArmor)
        {
            if (const float perks = av(AV::kArmorPerks); perks != 0.0f)
            {
                SheetRow::ModifierPart piece;
                piece.text = Fmt("%+.2f", perks) + " skill multiplier";
                piece.breakdown = ValueBreakdown(actor, AV::kArmorPerks, "");
                piece.breakdown.decimals = 2;
                row.modifiers += (row.modifiers.empty() ? "" : ", ") + piece.text;
                row.modifierParts.push_back(std::move(piece));
            }
        }

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

    // The perks held that sit in no skill's tree: a mod's loose perk, a
    // race's, a quest's. The unnamed stay out.
    {
        std::unordered_set<const RE::BGSPerk *> inTrees;
        for (const Found &f : found)
            for (const TreePerk &entry : TreePerks(f.value))
                inTrees.insert(entry.perk);
        SheetSection s{"Other Perks", {}, {}};
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
// less the time run (docs/MODIFIERS.md). Whatever else moves a duration is
// not read, and shows as Other.
ft::Breakdown RemainingBreakdown(const RE::ActiveEffect &effect)
{
    ft::Breakdown b;
    if (!effect.effect || effect.duration <= 0.0f)
        return b;
    b.unit = " s";
    b.totalLabel = "Remaining";
    ft::Start(b, "Base", static_cast<float>(effect.effect->effectItem.duration));
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
            ft::BreakdownLine &line = ft::Multiply(b, "Dual cast", effectiveness);
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
    ft::Add(b, "Elapsed", -effect.elapsedSeconds);
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
        for (const RE::Effect *effect : ench->enchantment->effects)
        {
            if (!effect || !effect->baseEffect)
                continue;
            variant.enchantment.push_back({effect->baseEffect->GetFormID(), effect->effectItem.magnitude,
                                           effect->effectItem.duration, effect->effectItem.area});
        }
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

std::int32_t CountVariant(RE::Actor *actor, RE::TESBoundObject *object, const std::optional<ft::ItemVariant> &variant)
{
    const Carried carried = CarriedOf(actor, object);
    if (carried.count <= 0)
        return 0;
    if (!variant)
        return carried.count;
    std::int32_t named = 0;
    std::int32_t listed = 0;
    if (carried.entry && carried.entry->extraLists)
    {
        for (const auto *list : *carried.entry->extraLists)
        {
            if (!list)
                continue;
            listed += list->GetCount();
            if (ft::SameVariant(VariantOf(list), *variant))
                named += list->GetCount();
        }
    }
    // The listless remainder is plain.
    if (variant->IsPlain())
        named += (std::max)(0, carried.count - listed);
    return named;
}

RE::ExtraDataList *WornVariantList(RE::Actor *actor, RE::TESBoundObject *object, const ft::ItemVariant &variant,
                                   Hand hands)
{
    return ListOf(actor, object, [&](const RE::ExtraDataList &list) {
        return WornIn(object, &list, hands) && ft::SameVariant(VariantOf(&list), variant);
    });
}

RE::ExtraDataList *UnwornVariantList(RE::Actor *actor, RE::TESBoundObject *object, const ft::ItemVariant &variant)
{
    if (RE::ExtraDataList *stack = ListOf(actor, object, [&](const RE::ExtraDataList &list) {
            return !ListWorn(&list, Hand::None) && !RowOfItsOwn(&list) && ft::SameVariant(VariantOf(&list), variant);
        }))
        return stack;
    return ListOf(actor, object, [&](const RE::ExtraDataList &list) {
        return !ListWorn(&list, Hand::None) && ft::SameVariant(VariantOf(&list), variant);
    });
}

RE::ExtraDataList *WornStackList(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    return ListOf(actor, object,
                  [&](const RE::ExtraDataList &list) { return WornIn(object, &list, hands) && !RowOfItsOwn(&list); });
}

RE::ExtraDataList *UnwornStackList(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ListOf(actor, object,
                  [&](const RE::ExtraDataList &list) { return !ListWorn(&list, Hand::None) && !RowOfItsOwn(&list); });
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
    std::vector<ft::ItemVariant> rows;
    const Carried carried = CarriedOf(actor, object);
    if (carried.count <= 0)
        return rows;
    std::int32_t apart = 0;
    if (carried.entry && carried.entry->extraLists)
    {
        for (const auto *list : *carried.entry->extraLists)
        {
            if (!list || !RowOfItsOwn(list))
                continue;
            apart += list->GetCount();
            rows.push_back(VariantOf(list));
        }
    }
    if (carried.count > apart)
        rows.emplace_back(); // the plain stack: the listless copies and the folded lists
    return rows;
}

bool HasListlessCopy(RE::Actor *actor, RE::TESBoundObject *object)
{
    const Carried carried = CarriedOf(actor, object);
    if (carried.count <= 0)
        return false;
    std::int32_t listed = 0;
    if (carried.entry && carried.entry->extraLists)
        for (const auto *list : *carried.entry->extraLists)
            if (list)
                listed += list->GetCount();
    return carried.count > listed;
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
    if (!list)
        return false;
    const bool right = list->HasType(RE::ExtraDataType::kWorn);
    const bool left = list->HasType(RE::ExtraDataType::kWornLeft);
    switch (hands)
    {
    case Hand::Left:
        return left;
    case Hand::Right:
    case Hand::Both: // a two-hander sits in the right
        return right;
    default:
        return left || right;
    }
}

bool WornIn(const RE::TESBoundObject *object, const RE::ExtraDataList *list, Hand hands)
{
    if (object && !object->IsWeapon())
        return hands == Hand::Right ? false : ListWorn(list, Hand::None);
    return ListWorn(list, hands);
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
    for (const bool left : {false, true})
    {
        const Hand hand = left ? Hand::Left : Hand::Right;
        auto *weapon = PoisonableWeaponIn(actor, left);
        if (weapon && !WeaponPoisoned(actor, weapon, hand))
            return {weapon, hand};
    }
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

} // namespace ft::game
