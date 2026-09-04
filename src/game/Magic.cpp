#include "game/Magic.h"

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

// Walk every spell an actor has, from both places the game keeps them: what
// the character was authored with, and everything granted at runtime.
template <typename Fn> void ForEachSpell(RE::Actor *actor, Fn &&fn)
{
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numSpells; ++i)
            {
                if (list->spells[i])
                    fn(list->spells[i]);
            }
        }
    }
    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
    {
        if (spell)
            fn(spell);
    }
}

// The equip slot records, read off Skyrim.esm (not from memory, which had
// them wrong): RightHand 013F42, LeftHand 013F43, EitherHand 013F44,
// BothHands 013F45, Voice 025BEE.
constexpr std::uint32_t kRightHandSlot = 0x00013F42;
constexpr std::uint32_t kLeftHandSlot = 0x00013F43;

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
    const bool power = type == Type::kPower || type == Type::kLesserPower;
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
    entry.cast = CastWord(spell->GetDelivery(), spell->GetCastingType());
    entry.castValue = static_cast<int>(spell->GetDelivery());
    entry.magnitude = costliest ? costliest->effectItem.magnitude : 0.0f;
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
    stats.rows.push_back(Row("School", entry.school));
    stats.rows.push_back(Row("Hand", entry.hand));
    if (!entry.level.empty())
    {
        stats.rows.push_back(Row("Level", entry.level));
        stats.rows.push_back(Row("Skill", std::to_string(entry.levelValue) + " needed, has " +
                                              std::to_string(entry.skill) +
                                              (entry.aboveSkill ? " -- the combat AI will not choose it" : "")));
    }
    if (costliest)
    {
        stats.rows.push_back(Row("Magnitude", Fmt("%.0f", costliest->effectItem.magnitude)));
        if (costliest->effectItem.duration > 0)
            stats.rows.push_back(Row("Duration", std::to_string(costliest->effectItem.duration) + " s"));
    }
    if (const float charge = spell->GetChargeTime(); charge > 0.0f)
        stats.rows.push_back(Row("Charge Time", Fmt("%.1f s", charge)));
    if (!entry.cost.empty())
        stats.rows.push_back(Row("Cost", entry.cost));
    // The list's word, so the page and the list agree.
    stats.rows.push_back(Row("Cast", entry.cast));
    if (entry.equipped)
        stats.rows.push_back(Row("Equipped", "yes"));
    entry.detail.push_back(std::move(stats));

    entry.effects = EffectLines(spell);
    RE::BSString text;
    spell->GetDescription(text, spell);
    entry.description = text.c_str() ? text.c_str() : "";
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
    entry.cast = "Shout";
    entry.castValue = 99;
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
        stats.rows.push_back(Row("Equipped", "yes"));
    entry.detail.push_back(std::move(stats));

    if (shout->variations[0].spell)
        entry.effects = EffectLines(shout->variations[0].spell);
    RE::BSString text;
    shout->GetDescription(text, shout);
    entry.description = text.c_str() ? text.c_str() : "";
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
                MagicEntry entry;
                if (list->shouts[i] && DescribeShout(actor, list->shouts[i], entry))
                    out.push_back(std::move(entry));
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const MagicEntry &a, const MagicEntry &b) { return a.name < b.name; });
    return out;
}

} // namespace ft::game
