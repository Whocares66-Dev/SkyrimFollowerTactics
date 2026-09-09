#include "game/Magic.h"

#include "game/Hits.h"
#include "game/Packages.h"

#include "game/Pins.h"

#include "game/Inventory.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ft::game
{
namespace
{

std::string Fmt(const char *fmt, double value)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, value);
    return buf;
}

SheetRow Row(std::string label, std::string value)
{
    SheetRow row;
    row.label = std::move(label);
    row.value = std::move(value);
    return row;
}

std::string NameOf(const RE::TESForm *form)
{
    return form && form->GetName() ? form->GetName() : "";
}

// The Equipped row: a tick, and a pin beside it when a pin holds the thing.
// Only on a page of something equipped; a bare "no" says nothing.
SheetRow EquippedRow(bool pinned)
{
    SheetRow row;
    row.label = "Equipped";
    row.icon = kGlyphTick;
    if (pinned)
        row.icon2 = kGlyphPin;
    return row;
}

// The equip slot records, read off Skyrim.esm (not from memory, which had
// them wrong): RightHand 013F42, LeftHand 013F43, EitherHand 013F44,
// BothHands 013F45, Voice 025BEE.
constexpr std::uint32_t kRightHandSlot = 0x00013F42;
constexpr std::uint32_t kLeftHandSlot = 0x00013F43;

// The seconds left on the longest effect one of `sources` is running on
// `who`, or 0 for none. A power's source is itself; a shout's are its words'
// spells. Marked for Death runs on the enemy, Embrace of Shadows on the
// follower, so the caller asks about both.
float RemainingOn(RE::Actor *who, const std::vector<const RE::MagicItem *> &sources)
{
    auto *target = who ? who->AsMagicTarget() : nullptr;
    auto *effects = target ? target->GetActiveEffectList() : nullptr;
    if (!effects)
        return 0.0f;
    float best = 0.0f;
    for (const auto *ae : *effects)
    {
        if (!ae || !ae->spell || ae->duration <= 0.0f)
            continue;
        if (std::find(sources.begin(), sources.end(), ae->spell) == sources.end())
            continue;
        best = (std::max)(best, ae->duration - ae->elapsedSeconds);
    }
    return best;
}

// The Time section of a power's or shout's page: Cooldown, the voice's
// recovery from the last shout (one timer per actor, shared by every shout
// and power), and Remaining, the time left on an effect this one is running
// on the follower or on whom they are fighting. Each row only when it has a
// number; no section when neither does.
void AddTimeSection(RE::Actor *actor, const std::vector<const RE::MagicItem *> &sources, MagicEntry &entry)
{
    SheetSection time{"Time", {}, {}};
    if (!actor)
        return;
    const float recovery = actor->GetVoiceRecoveryTime();
    if (recovery > 0.0f && recovery < 3600.0f)
        time.rows.push_back(Row("Cooldown", Fmt("%.0f", recovery) + " s"));

    float remaining = RemainingOn(actor, sources);
    if (auto enemy = actor->GetActorRuntimeData().currentCombatTarget.get())
        remaining = (std::max)(remaining, RemainingOn(enemy.get(), sources));
    if (remaining > 0.0f)
        time.rows.push_back(Row("Remaining", Fmt("%.0f", remaining) + " s"));

    if (!time.rows.empty())
        entry.detail.push_back(std::move(time));
}

// The magic menu's level word for an effect's minimum skill.
const char *LevelWord(int minimumSkill)
{
    if (minimumSkill >= 100)
        return "Master";
    if (minimumSkill >= 75)
        return "Expert";
    if (minimumSkill >= 50)
        return "Adept";
    if (minimumSkill >= 25)
        return "Apprentice";
    return "Novice";
}

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
    switch (KindOfEffect(base))
    {
    case ft::DamageKind::Fire:
        return "Fire";
    case ft::DamageKind::Frost:
        return "Frost";
    case ft::DamageKind::Shock:
        return "Shock";
    case ft::DamageKind::Poison:
        return "Poison";
    default:
        break;
    }
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
            return harmful ? "Damage" : "Heal";
        if (av == AV::kMagicka || av == AV::kStamina)
            return harmful ? "Drain" : "Restore";
        if (av == AV::kDamageResist)
            return "Armor";
        if (av == AV::kWardPower)
            return "Ward";
        if (av == AV::kSpeedMult)
            return harmful ? "Slow" : "Speed";
        return harmful ? "Weaken" : "Fortify";
    }
    case Archetype::kAbsorb:
        return "Absorb";
    case Archetype::kSummonCreature:
        return "Summon";
    case Archetype::kReanimate:
        return "Reanimate";
    case Archetype::kBoundWeapon:
        return "Bound Weapon";
    case Archetype::kCommandSummoned:
        return "Command";
    case Archetype::kBanish:
        return "Banish";
    case Archetype::kSoulTrap:
        return "Soul Trap";
    case Archetype::kCalm:
        return "Calm";
    case Archetype::kDemoralize:
        return "Fear";
    case Archetype::kFrenzy:
        return "Frenzy";
    case Archetype::kRally:
        return "Courage";
    case Archetype::kInvisibility:
        return "Invisibility";
    case Archetype::kLight:
        return "Light";
    case Archetype::kDarkness:
        return "Darkness";
    case Archetype::kNightEye:
        return "Night Eye";
    case Archetype::kDetectLife:
        return "Detect";
    case Archetype::kParalysis:
        return "Paralysis";
    case Archetype::kTelekinesis:
        return "Telekinesis";
    case Archetype::kTurnUndead:
        return "Turn Undead";
    case Archetype::kDispel:
        return "Dispel";
    case Archetype::kCureDisease:
    case Archetype::kCurePoison:
    case Archetype::kCureParalysis:
    case Archetype::kCureAddiction:
        return "Cure";
    case Archetype::kDisarm:
        return "Disarm";
    case Archetype::kStagger:
    case Archetype::kConcussion:
        return "Stagger";
    case Archetype::kCloak:
        return "Cloak";
    case Archetype::kSlowTime:
        return "Slow Time";
    case Archetype::kEtherealize:
        return "Ethereal";
    case Archetype::kEnhanceWeapon:
        return "Enhance Weapon";
    case Archetype::kSpawnHazard:
        return "Hazard";
    case Archetype::kLock:
    case Archetype::kOpen:
        return "Lock";
    case Archetype::kGuide:
        return "Guide";
    case Archetype::kWerewolf:
    case Archetype::kVampireLord:
        return "Transform";
    case Archetype::kScript:
        return "Scripted";
    default:
        return "Magic";
    }
}

// Delivery and casting type as one word: what the spell does when cast.
const char *CastWord(RE::MagicSystem::Delivery delivery, RE::MagicSystem::CastingType casting)
{
    switch (delivery)
    {
    case RE::MagicSystem::Delivery::kSelf:
        return "Self";
    case RE::MagicSystem::Delivery::kTouch:
        return "Touch";
    case RE::MagicSystem::Delivery::kAimed:
        return casting == RE::MagicSystem::CastingType::kConcentration ? "Spray" : "Projectile";
    case RE::MagicSystem::Delivery::kTargetActor:
        return "Target";
    case RE::MagicSystem::Delivery::kTargetLocation:
        return "Location";
    default:
        return "?";
    }
}

// One entry for a spell or a power. Returns false for the kinds the magic
// menu does not list.
bool DescribeSpell(RE::Actor *actor, RE::SpellItem *spell, MagicEntry &entry)
{
    using Type = RE::MagicSystem::SpellType;
    const Type type = spell->GetSpellType();
    // A power a shout slot is leasing reads as Voice for the lease
    // (Packages.cpp); it is still a power to the tab.
    const bool power = type == Type::kPower || type == Type::kLesserPower || IsLeasedPower(spell->GetFormID());
    if (type != Type::kSpell && !power)
        return false;

    entry.form = spell->GetFormID();
    entry.name = NameOf(spell);
    if (entry.name.empty())
        return false;

    const auto *costliest = spell->GetCostliestEffectItem();
    const auto *effect = costliest ? costliest->baseEffect : nullptr;

    SheetSection stats{"Spell", {}, {}};
    if (power)
    {
        entry.category = MagicCategory::Powers;
        entry.school = type == Type::kPower ? "Power" : "Lesser Power";
        stats.title = "Power";
    }
    else
    {
        const RE::ActorValue skill = effect ? effect->GetMagickSkill() : RE::ActorValue::kNone;
        entry.category = SchoolOf(skill);
        if (entry.category == MagicCategory::COUNT)
            return false;
        entry.school = DisplayName(entry.category);
        entry.levelValue = effect ? effect->GetMinimumSkillLevel() : 0;
        entry.level = LevelWord(entry.levelValue);
        if (auto *owner = actor->AsActorValueOwner())
        {
            entry.skill = static_cast<int>(owner->GetActorValue(skill));
            entry.aboveSkill = entry.levelValue > entry.skill;
        }
        entry.costValue = spell->CalculateMagickaCost(actor);
        const bool stream = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        entry.cost = Fmt("%.0f", entry.costValue) + (stream ? "/s" : "");
    }
    entry.type = TypeWord(effect);
    entry.cast = CastWord(spell->GetDelivery(), spell->GetCastingType());
    entry.castValue = static_cast<int>(spell->GetDelivery());
    // Her numbers, not the record's: the perk entry points applied, as the
    // engine applies them when the effect is made.
    entry.magnitude = costliest ? ActualMagnitude(actor, spell, costliest) : 0.0f;
    {
        // The equip slot record: the left-hand or right-hand slot means that
        // hand only -- the NPC-only variants, which carry the same display
        // name as the player's spell (Marcurio's Lightning Bolt is one). By
        // FormID: the four slots are fixed in Skyrim.esm, and the default
        // object table did not answer for them (01:29, "Either" for a
        // left-hand record).
        if (const auto *slot = spell->GetEquipSlot())
        {
            entry.leftAllowed = slot->GetFormID() != kRightHandSlot;
            entry.rightAllowed = slot->GetFormID() != kLeftHandSlot;
        }
        entry.hand = power                  ? "Voice"
                     : spell->IsTwoHanded() ? "Both"
                     : !entry.leftAllowed   ? "Right"
                     : !entry.rightAllowed  ? "Left"
                                            : "Either";
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
        stats.rows.push_back(Row("Base ID", id));
    }
    if (!power)
        stats.rows.push_back(Row("School", entry.school));
    if (!entry.type.empty())
        stats.rows.push_back(Row("Type", entry.type));
    stats.rows.push_back(Row("Hand", entry.hand));
    if (!entry.level.empty())
    {
        stats.rows.push_back(Row("Level", entry.level));
        // "51 (needs 100)": the follower's skill, and the spell's bar when
        // it is above them. That the AI will not choose it goes without
        // saying.
        stats.rows.push_back(
            Row("Skill", std::to_string(entry.skill) +
                             (entry.aboveSkill ? " (needs " + std::to_string(entry.levelValue) + ")" : "")));
    }
    if (costliest)
    {
        stats.rows.push_back(Row("Magnitude", Fmt("%.0f", entry.magnitude)));
        if (const float duration = ActualDuration(actor, spell, costliest); duration > 0.0f)
            stats.rows.push_back(Row("Duration", Fmt("%.0f", duration) + " s"));
    }
    if (const float charge = spell->GetChargeTime(); charge > 0.0f)
        stats.rows.push_back(Row("Charge Time", Fmt("%.1f s", charge)));
    if (!entry.cost.empty())
        stats.rows.push_back(Row("Cost", entry.cost));
    // The list's word, so the page and the list agree.
    stats.rows.push_back(Row("Cast", entry.cast));
    if (entry.equipped)
        stats.rows.push_back(EquippedRow(false)); // the pin glyph is added by MarkPins, which knows
    entry.detail.push_back(std::move(stats));
    if (power)
        AddTimeSection(actor, {spell}, entry);

    entry.effects = EffectLines(actor, spell);
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
    entry.school = "Shout";
    // How it is cast, from the first word's spell, as a spell's is from its
    // record: Unrelenting Force is a Projectile, Dragon Aspect is Self.
    const auto *first = shout->variations[0].spell;
    entry.cast = first ? CastWord(first->GetDelivery(), first->GetCastingType()) : "Shout";
    entry.castValue = first ? static_cast<int>(first->GetDelivery()) : 99;
    entry.hand = "Voice";
    entry.equipped = actor->GetActorRuntimeData().selectedPower == shout;

    SheetSection stats{"Shout", {}, {}};
    for (std::uint32_t i = 0; i < RE::TESShout::VariationIDs::kTotal; ++i)
    {
        const auto &variation = shout->variations[i];
        if (!variation.word)
            continue;
        std::string word = NameOf(variation.word);
        if (word.empty())
            word = "?";
        stats.rows.push_back(
            Row("Word " + std::to_string(i + 1), word + "  (" + Fmt("%.0f", variation.recoveryTime) + " s)"));
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

    if (shout->variations[0].spell)
        entry.effects = EffectLines(actor, shout->variations[0].spell);
    // A shout's description is its own record's; its numbers, when it has
    // any, are the first word's spell's.
    entry.description = DescriptionFor(actor, shout->variations[0].spell, *shout);
    return true;
}

} // namespace

const char *DisplayName(MagicCategory category)
{
    switch (category)
    {
    case MagicCategory::Alteration:
        return "Alteration";
    case MagicCategory::Conjuration:
        return "Conjuration";
    case MagicCategory::Destruction:
        return "Destruction";
    case MagicCategory::Illusion:
        return "Illusion";
    case MagicCategory::Restoration:
        return "Restoration";
    case MagicCategory::Shouts:
        return "Shouts";
    case MagicCategory::Powers:
    default:
        return "Powers";
    }
}

std::vector<MagicEntry> ScanMagic(RE::Actor *actor)
{
    std::vector<MagicEntry> out;
    if (!actor)
        return out;

    ForEachSpell(actor, [&](RE::SpellItem *spell) {
        // The same spell can appear in both sources; show it once.
        const std::uint32_t id = spell->GetFormID();
        if (std::any_of(out.begin(), out.end(), [id](const MagicEntry &e) { return e.form == id; }))
            return;
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

    std::sort(out.begin(), out.end(), [](const MagicEntry &a, const MagicEntry &b) { return a.name < b.name; });
    return out;
}

} // namespace ft::game
