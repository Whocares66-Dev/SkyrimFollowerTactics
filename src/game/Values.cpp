#include "game/Values.h"

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
#include "game/EffectRows.h"
#include "game/Effects.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/PerkSheet.h"
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

using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace ft::game
{

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
namespace
{

bool HeldPerkGrants(RE::Actor *actor, const RE::MagicItem *spell); // below, with the perks

bool Hidden(const RE::EffectSetting *base)
{
    return base && base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
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
        // record's weight: the effect copies the weight when it is made
        // (34319) and applies it to the second value (34324).
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

std::vector<ft::BreakdownLine> ValueLines(RE::Actor *actor, RE::ActorValue value, float reading, float scale)
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

ft::Breakdown SkillBreakdown(RE::Actor *actor, RE::ActorValue skill)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return {};
    const ValueParts parts = PartsOf(actor, skill);
    const float own = actor->IsPlayerRef() ? parts.base : fp::game::valueview::EngineBase(actor, skill);
    ft::Breakdown b;
    ft::Start(b, Tr("Base"), own);
    if (parts.base != own)
        ft::Add(b, Tr("Learned"), parts.base - own);
    for (const Contribution &c : parts.sources)
        ft::Add(b, c.source, c.amount);
    b.total = owner->GetActorValue(skill);
    ft::Close(b);
    if (b.lines.size() == 1)
        return {};
    return b;
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
// hit handler does (44014, read on 1.6.1170): rating x fArmorScalingFactor
// / 100 + the hidden sum, capped at fMaxArmorRating. Two differences, both
// by decision: the attacker's Mod Target Damage Resistance perks (entry
// point 0x25, applied before the cap) are left out, since this is the
// actor's own armour and not one blow's; and the engine sets no floor, so a
// negative rating makes a blow do more, where this stops at 0 -- an
// oversight in the engine's arithmetic, not a state to plan around.
// The formula is inline in the handler, so a mod that hooks it (Armor
// Rating Rescaled, Armor Rating Redux) is not reflected; one that changes
// the settings or the ratings is. The Character sheet's Armor row shows this
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

namespace
{

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

// --- perk entry points -------------------------------------------------------

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

} // namespace ft::game
