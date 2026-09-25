#include "game/Spells.h"

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

namespace ft::game
{

bool SkillGated(RE::Actor *actor)
{
    return actor && !actor->IsPlayerRef();
}

std::optional<SkillGate> FirstSkillGate(RE::Actor *actor, const RE::MagicItem *spell)
{
    auto *owner = SkillGated(actor) && spell ? actor->AsActorValueOwner() : nullptr;
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

float DualCastCost(RE::Actor *actor, RE::SpellItem *spell)
{
    const float cost = spell->CalculateMagickaCost(actor);
    return spell->GetNoDualCastModifications() ? cost : cost * GameSetting("fMagicDualCastingCostMult", 2.8f);
}

bool IsReanimate(const RE::Effect *effect)
{
    return effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate;
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

} // namespace ft::game
