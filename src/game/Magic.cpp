#include "game/Magic.h"

#include "core/I18n.h"
#include "core/Names.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"
#include "game/Sheet.h"

#include "game/Hits.h"
#include "game/Packages.h"

#include "game/Pins.h"

#include "game/EffectRows.h"
#include "game/Effects.h"
#include "game/Inventory.h"
#include "game/Spells.h"
#include "game/Values.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>

namespace ft::game
{
using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace
{

// The seconds left on the longest effect one of `sources` is running on
// `who`, or 0 for none. A power's source is itself; a shout's are its words'
// spells. Marked for Death runs on the enemy, Embrace of Shadows on the
// follower, so the caller asks about both.
float RemainingOn(RE::Actor *who, const std::vector<const RE::MagicItem *> &sources)
{
    std::vector<ft::EffectSeen> effects;
    ForEachActiveEffect(who, [&effects](RE::ActiveEffect &ae) {
        effects.push_back({ae.spell ? ae.spell->GetFormID() : 0, ae.duration, ae.elapsedSeconds});
    });
    std::vector<std::uint32_t> ids;
    for (const RE::MagicItem *source : sources)
        if (source)
            ids.push_back(source->GetFormID());
    return ft::RemainingOn(effects, ids);
}

// The Time section of a power's or shout's page: Cooldown, the voice's
// recovery from the last shout (one timer per actor, shared by every shout
// and power), and Remaining, the time left on an effect this one is running
// on the follower or on whom they are fighting. Each row only when it has a
// number; no section when neither does.
void AddTimeSection(RE::Actor *actor, const std::vector<const RE::MagicItem *> &sources, MagicEntry &entry)
{
    SheetSection time{Tr("Time"), {}, {}};
    if (!actor)
        return;
    if (const float recovery = VoiceRecoveryOf(actor); recovery > 0.0f)
        time.rows.push_back(Row(Tr("Cooldown"), TrFormat("{} s", Fmt("%.0f", recovery))));

    float remaining = RemainingOn(actor, sources);
    if (auto enemy = actor->GetActorRuntimeData().currentCombatTarget.get())
        remaining = (std::max)(remaining, RemainingOn(enemy.get(), sources));
    if (remaining > 0.0f)
        time.rows.push_back(Row(Tr("Remaining"), TrFormat("{} s", Fmt("%.0f", remaining))));

    if (!time.rows.empty())
        entry.detail.push_back(std::move(time));
}

// A word of power in the Latin alphabet. Its name is written for the game's
// dragon-script font, where a digit is one of the nine runes for a pair of
// letters: D4 is Dah, V1z Vaaz. The digits are UESP's Dragon Alphabet table,
// checked against Skyrim.esm's words beside their editor IDs (Nir N7, Mey
// M9, Zoor Z8r, Feim F2m).
std::string DragonLatin(const std::string &name)
{
    static constexpr std::array<const char *, 10> kPairs{"", "aa", "ei", "ii", "ah", "uu", "ur", "ir", "oo", "ey"};
    std::string out;
    for (const char c : name)
    {
        if (c >= '1' && c <= '9')
            out += kPairs[static_cast<std::size_t>(c - '0')];
        else
            out += c;
    }
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// The magic menu's level word for an effect's minimum skill.
const char *LevelWord(int minimumSkill)
{
    if (minimumSkill >= 100)
        return Tr("Master");
    if (minimumSkill >= 75)
        return Tr("Expert");
    if (minimumSkill >= 50)
        return Tr("Adept");
    if (minimumSkill >= 25)
        return Tr("Apprentice");
    return Tr("Novice");
}

} // namespace

// A school as a category, or COUNT for a spell whose costliest effect
// belongs to none of the five (a scripted spell, say).
MagicCategory SchoolOf(RE::ActorValue skill)
{
    switch (skill)
    {
    case RE::ActorValue::kAlteration:
        return MagicCategory::Alteration;
    case RE::ActorValue::kConjuration:
        return MagicCategory::Conjuration;
    case RE::ActorValue::kDestruction:
        return MagicCategory::Destruction;
    case RE::ActorValue::kIllusion:
        return MagicCategory::Illusion;
    case RE::ActorValue::kRestoration:
        return MagicCategory::Restoration;
    default:
        return MagicCategory::COUNT;
    }
}

// The kind of spell, from its costliest effect: the element where it does
// that kind of damage, else the archetype in the magic menu's words.
std::string TypeWord(const RE::EffectSetting *base)
{
    if (!base)
        return "";
    if (const auto kind = KindOfEffect(base); kind != ft::DamageKind::Magic)
        return std::string(ft::DisplayName(kind));
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    using AV = RE::ActorValue;
    switch (base->GetArchetype())
    {
    case Archetype::kValueModifier:
    case Archetype::kPeakValueModifier:
    case Archetype::kDualValueModifier: {
        const AV av = base->data.primaryAV;
        const bool harmful = base->IsDetrimental();
        if (av == AV::kHealth)
            return harmful ? Tr("Damage") : Tr("Heal");
        if (av == AV::kMagicka || av == AV::kStamina)
            return harmful ? Tr("Drain") : Tr("Restore");
        if (av == AV::kDamageResist)
            return Tr("Armor");
        if (av == AV::kWardPower)
            return Tr("Ward");
        if (av == AV::kSpeedMult)
            return harmful ? Tr("Slow") : Tr("Speed");
        return harmful ? Tr("Weaken") : Tr("Fortify");
    }
    case Archetype::kAbsorb:
        return Tr("Absorb");
    case Archetype::kSummonCreature:
        return Tr("Summon");
    case Archetype::kReanimate:
        return Tr("Reanimate");
    case Archetype::kBoundWeapon:
        return Tr("Bound Weapon");
    case Archetype::kCommandSummoned:
        return Tr("Command");
    case Archetype::kBanish:
        return Tr("Banish");
    case Archetype::kSoulTrap:
        return Tr("Soul Trap");
    case Archetype::kCalm:
        return Tr("Calm");
    case Archetype::kDemoralize:
        return Tr("Fear");
    case Archetype::kFrenzy:
        return Tr("Frenzy");
    case Archetype::kRally:
        return Tr("Courage");
    case Archetype::kInvisibility:
        return Tr("Invisibility");
    case Archetype::kLight:
        return Tr("Light");
    case Archetype::kDarkness:
        return Tr("Darkness");
    case Archetype::kNightEye:
        return Tr("Night Eye");
    case Archetype::kDetectLife:
        return Tr("Detect");
    case Archetype::kParalysis:
        return Tr("Paralysis");
    case Archetype::kTelekinesis:
        return Tr("Telekinesis");
    case Archetype::kTurnUndead:
        return Tr("Turn Undead");
    case Archetype::kDispel:
        return Tr("Dispel");
    case Archetype::kCureDisease:
    case Archetype::kCurePoison:
    case Archetype::kCureParalysis:
    case Archetype::kCureAddiction:
        return Tr("Cure");
    case Archetype::kDisarm:
        return Tr("Disarm");
    case Archetype::kStagger:
    case Archetype::kConcussion:
        return Tr("Stagger");
    case Archetype::kCloak:
        return Tr("Cloak");
    case Archetype::kSlowTime:
        return Tr("Slow Time");
    case Archetype::kEtherealize:
        return Tr("Ethereal");
    case Archetype::kEnhanceWeapon:
        return Tr("Enhance Weapon");
    case Archetype::kSpawnHazard:
        return Tr("Hazard");
    case Archetype::kLock:
    case Archetype::kOpen:
        return Tr("Lock");
    case Archetype::kGuide:
        return Tr("Guide");
    case Archetype::kWerewolf:
    case Archetype::kVampireLord:
        return Tr("Transform");
    case Archetype::kScript:
        return Tr("Scripted");
    default:
        return Tr("Magic");
    }
}

// Delivery and casting type as one word: what the spell does when cast.
const char *CastWord(RE::MagicSystem::Delivery delivery, RE::MagicSystem::CastingType casting)
{
    switch (delivery)
    {
    case RE::MagicSystem::Delivery::kSelf:
        return Tr("Self");
    case RE::MagicSystem::Delivery::kTouch:
        return Tr("Touch");
    case RE::MagicSystem::Delivery::kAimed:
        return casting == RE::MagicSystem::CastingType::kConcentration ? Tr("Spray") : Tr("Projectile");
    case RE::MagicSystem::Delivery::kTargetActor:
        return Tr("Target");
    case RE::MagicSystem::Delivery::kTargetLocation:
        return Tr("Location");
    default:
        return "?";
    }
}

namespace
{

// One entry for a spell or a power. Returns false for the kinds the magic
// menu does not list.
bool DescribeSpell(RE::Actor *actor, RE::SpellItem *spell, MagicEntry &entry)
{
    using Type = RE::MagicSystem::SpellType;
    const Type type = spell->GetSpellType();
    // A power a shout slot is leasing reads as Voice for the lease
    // (Packages.cpp); it is still a power to the tab.
    const bool power = IsPower(spell);
    if (type != Type::kSpell && !power)
        return false;

    entry.form = spell->GetFormID();
    entry.name = NameOf(spell);
    if (entry.name.empty())
        return false;

    const auto *costliest = spell->GetCostliestEffectItem();
    const auto *effect = costliest ? costliest->baseEffect : nullptr;

    SheetSection stats{Tr("Spell"), {}, {}};
    if (power)
    {
        entry.category = MagicCategory::Powers;
        entry.school = type == Type::kPower ? Tr("Power") : Tr("Lesser Power");
        stats.title = Tr("Power");
    }
    else
    {
        const RE::ActorValue skill = effect ? effect->GetMagickSkill() : RE::ActorValue::kNone;
        entry.category = SchoolOf(skill);
        if (entry.category == MagicCategory::COUNT)
        {
            // No school: Serana's Drain Life, a Vampire's Drain. Listed
            // under Other, with no school word, no level and no skill gate
            // -- there is no skill to be below (2026-09-13; the tactics
            // menu offered it and the tab did not).
            entry.category = MagicCategory::Other;
        }
        else
        {
            entry.school = DisplayName(entry.category);
            entry.levelValue = effect ? effect->GetMinimumSkillLevel() : 0;
            entry.level = LevelWord(entry.levelValue);
            if (auto *owner = actor->AsActorValueOwner())
            {
                entry.skill = static_cast<int>(owner->GetActorValue(skill));
                // The combat AI's gate, asked of every effect as the engine
                // asks it; never of the player.
                if (const auto gate = FirstSkillGate(actor, spell))
                {
                    entry.aboveSkill = true;
                    entry.needsSchool = DisplayName(SchoolOf(gate->school));
                    entry.needsLevel = gate->level;
                    entry.hasLevel = static_cast<int>(gate->has);
                }
            }
        }
        entry.costValue = spell->CalculateMagickaCost(actor);
        const bool stream = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        entry.cost = stream ? TrFormat("{}/s", Fmt("%.0f", entry.costValue)) : Fmt("%.0f", entry.costValue);
        entry.costBreakdown = SpellCostBreakdown(actor, spell);
        if (stream)
            entry.costBreakdown.unit = Tr("/s");
    }
    entry.type = TypeWord(effect);
    entry.cast = CastWord(spell->GetDelivery(), spell->GetCastingType());
    entry.castValue = static_cast<int>(spell->GetDelivery());
    // Their numbers, not the record's: the perk entry points applied, as the
    // engine applies them when the effect is made.
    entry.magnitude = costliest ? ActualMagnitude(actor, spell, costliest) : 0.0f;
    {
        const ft::Grip grip = SpellGrip(spell);
        entry.leftAllowed = grip != ft::Grip::RightOnly;
        entry.rightAllowed = grip != ft::Grip::LeftOnly;
        entry.hand = power                         ? Tr("Voice")
                     : grip == ft::Grip::Both      ? Tr("Both")
                     : grip == ft::Grip::RightOnly ? Tr("Right")
                     : grip == ft::Grip::LeftOnly  ? Tr("Left")
                                                   : Tr("Either");
        entry.grip = DescribeHoldable(actor, spell).grip;
    }
    {
        const auto &data = actor->GetActorRuntimeData();
        entry.equippedLeft = data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell;
        entry.equippedRight = data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell;
        entry.equipped = entry.equippedLeft || entry.equippedRight || data.selectedPower == spell;
    }

    {
        // The FormID, for the console: `removespell` and `addspell` take it,
        // and whether a base-record spell can be removed is a thing to try.
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", spell->GetFormID());
        stats.rows.push_back(Row(Tr("Base ID"), id));
    }
    if (!power && !entry.school.empty())
        stats.rows.push_back(Row(Tr("School"), entry.school));
    if (!entry.type.empty())
        stats.rows.push_back(Row(Tr("Type"), entry.type));
    if (!entry.level.empty())
    {
        stats.rows.push_back(Row(Tr("Level"), entry.level));
        // The skill the spell asks for, and in brackets what the follower
        // has when it is short: "75 (has 51)". That the AI will not choose
        // it then goes without saying.
        stats.rows.push_back(Row(Tr("Skill"), entry.aboveSkill ? TrFormat("{} (has {})", entry.levelValue, entry.skill)
                                                               : std::to_string(entry.levelValue)));
    }
    if (costliest)
    {
        stats.rows.push_back(Row(Tr("Magnitude"), Fmt("%.0f", entry.magnitude)));
        if (const float duration = ActualDuration(actor, spell, costliest); duration > 0.0f)
            stats.rows.push_back(Row(Tr("Duration"), TrFormat("{} s", Fmt("%.0f", duration))));
    }
    if (!entry.cost.empty())
    {
        SheetRow row = Row(Tr("Cost"), entry.cost);
        row.breakdown = entry.costBreakdown;
        stats.rows.push_back(std::move(row));
    }
    // The list's word, so the page and the list agree.
    stats.rows.push_back(Row(Tr("Cast"), entry.cast));
    if (const float charge = spell->GetChargeTime(); charge > 0.0f)
        stats.rows.push_back(Row(Tr("Charge Time"), TrFormat("{} s", Fmt("%.1f", charge))));
    stats.rows.push_back(Row(Tr("Hand"), entry.hand));
    if (entry.equipped)
        stats.rows.push_back(EquippedRow(false)); // the pin glyph is added by MarkPins, which knows
    entry.detail.push_back(std::move(stats));
    if (power)
        AddTimeSection(actor, {spell}, entry);

    entry.effectTables.push_back(
        EffectsOf(actor, spell, [&](const RE::Effect *e) { return ActualMagnitude(actor, spell, e); }));
    entry.description = DescriptionFor(actor, spell, *spell);
    return true;
}

// A shout: its three words' spells, the first word's effects on the page.
bool DescribeShout(RE::Actor *actor, RE::TESShout *shout, MagicEntry &entry)
{
    entry.form = shout->GetFormID();
    entry.name = NameOf(shout);
    if (entry.name.empty())
        return false;
    entry.category = MagicCategory::Shouts;
    entry.school = Tr("Shout");
    // How it is cast, from the first word's spell, as a spell's is from its
    // record: Unrelenting Force is a Projectile, Dragon Aspect is Self.
    const auto *first = shout->variations[0].spell;
    entry.cast = first ? CastWord(first->GetDelivery(), first->GetCastingType()) : Tr("Shout");
    entry.castValue = first ? static_cast<int>(first->GetDelivery()) : 99;
    // Its type from the same spell's costliest effect: Fire Breath is Fire,
    // Unrelenting Force a Stagger.
    if (const auto *costliest = first ? first->GetCostliestEffectItem() : nullptr)
        entry.type = TypeWord(costliest->baseEffect);
    entry.hand = Tr("Voice");
    entry.equipped = actor->GetActorRuntimeData().selectedPower == shout;

    // No word unlocked -- or no word at all, which the engine answers the
    // same way -- and there is nothing here for anyone to shout.
    entry.locked = HighestUnlockedWord(shout) < 0;

    SheetSection stats{Tr("Shout"), {}, {}};
    for (std::uint32_t i = 0; i < RE::TESShout::VariationIDs::kTotal; ++i)
    {
        const auto &variation = shout->variations[i];
        if (!variation.word)
            continue;
        std::string word = DragonLatin(NameOf(variation.word));
        if (word.empty())
            word = "?";
        // The word's recovery as it applies to them, their shout recovery
        // multiplier in, written out on hover.
        SheetRow row;
        const float recovery = WordRecovery(actor, variation.recoveryTime, &row.breakdown);
        row.label = TrFormat("Word {}", i + 1);
        row.value = TrFormat("{}  ({} s)", word, Fmt("%.0f", recovery));
        // A word still locked is greyed with the reason on it: the shout
        // stops at the last word unlocked, whatever the record holds.
        if (!WordUnlocked(variation.word))
            row.aside = Tr("Not unlocked");
        stats.rows.push_back(std::move(row));
    }
    if (entry.equipped)
        stats.rows.push_back(EquippedRow(false));
    entry.detail.push_back(std::move(stats));
    {
        std::vector<const RE::MagicItem *> words;
        for (const auto &variation : shout->variations)
            if (variation.spell)
                words.push_back(variation.spell);
        AddTimeSection(actor, words, entry);
    }

    // A table per word, under one heading: Soul Tear's later words carry
    // effects its first does not.
    for (std::uint32_t i = 0; i < RE::TESShout::VariationIDs::kTotal; ++i)
    {
        auto *spell = shout->variations[i].spell;
        if (!shout->variations[i].word || !spell)
            continue;
        SheetSection table =
            EffectsOf(actor, spell, [&](const RE::Effect *e) { return ActualMagnitude(actor, spell, e); });
        if (table.rows.empty())
            continue;
        table.group = table.title;
        table.title = TrFormat("Word {}", i + 1);
        if (!WordUnlocked(shout->variations[i].word))
            table.aside = Tr("Not unlocked");
        entry.effectTables.push_back(std::move(table));
    }
    // A shout's description is its own record's; its numbers, when it has
    // any, are the first word's spell's.
    entry.description = DescriptionFor(actor, shout->variations[0].spell, *shout);
    return true;
}

} // namespace

// The engine keeps a word's unlocked state as bit 16 of the word's own form
// flags: `PlayerCharacter::UnlockWord` (vtable 0xD0) tail-calls the flag
// setter that sets that bit, and `TESShout::GetKnown` (vtable 0x17) answers
// for a whole shout by reading it off the first variation that has a word.
// Read from the running 1.6.1170 in memory (dev/VERSIONS.md). CommonLib
// names no accessor for it on a word -- `GetRandomAnim` is the same bit on
// other form types -- so the bit is named here rather than borrowed under a
// wrong name.
constexpr std::uint32_t kWordUnlocked = 1u << 16;

bool WordUnlocked(const RE::TESWordOfPower *word)
{
    return word && (word->GetFormFlags() & kWordUnlocked) != 0;
}

int HighestUnlockedWord(const RE::TESShout *shout)
{
    int top = -1;
    if (shout)
    {
        for (int i = 0; i < static_cast<int>(RE::TESShout::VariationIDs::kTotal); ++i)
            if (WordUnlocked(shout->variations[i].word))
                top = i;
    }
    return top;
}

const char *DisplayName(MagicCategory category)
{
    switch (category)
    {
    case MagicCategory::Alteration:
        return Tr("Alteration");
    case MagicCategory::Conjuration:
        return Tr("Conjuration");
    case MagicCategory::Destruction:
        return Tr("Destruction");
    case MagicCategory::Illusion:
        return Tr("Illusion");
    case MagicCategory::Restoration:
        return Tr("Restoration");
    case MagicCategory::Other:
        return Tr("Other");
    case MagicCategory::Shouts:
        return Tr("Shouts");
    case MagicCategory::Powers:
    default:
        return Tr("Powers");
    }
}

std::vector<MagicEntry> ScanMagic(RE::Actor *actor)
{
    std::vector<MagicEntry> out;
    if (!actor)
        return out;

    ForEachSpell(actor, [&](RE::SpellItem *spell) {
        MagicEntry entry;
        if (DescribeSpell(actor, spell, entry))
            out.push_back(std::move(entry));
    });

    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numShouts; ++i)
            {
                // Not our wrapper shouts, which sit in the list for the
                // length of a power lease and are nobody's to see.
                if (!list->shouts[i] || IsWrapperShout(list->shouts[i]->GetFormID()))
                    continue;
                MagicEntry entry;
                if (DescribeShout(actor, list->shouts[i], entry))
                    out.push_back(std::move(entry));
            }
        }
    }

    ft::SortByName(out, [](const auto &item) -> std::string_view { return item.name; });
    return out;
}

} // namespace ft::game
