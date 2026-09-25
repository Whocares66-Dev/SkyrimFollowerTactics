#include "game/Traits.h"

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

#include "game/Bag.h"
#include "game/CustomSkillsFramework.h"
#include "game/Effects.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
#include "game/Toggles.h"
#include "game/Util.h"
#include "game/Values.h"
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
namespace
{

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

bool LowersOwnValue(const RE::EffectSetting *effect)
{
    if (!effect->IsDetrimental())
        return false;
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    switch (effect->GetArchetype())
    {
    case Archetype::kValueModifier:
    case Archetype::kPeakValueModifier:
    case Archetype::kDualValueModifier:
    case Archetype::kAbsorb:
        return true;
    default:
        return false;
    }
}

// Skyrim.esm's Bleeding Damage, what the axe perks apply.
constexpr RE::FormID kPerkBleedingDamage = 0x000C367A;
// Skyrim.esm's Targe of the Blooded bash (dunTargeOfTheBloodedME): named
// Damage Health, as hundreds of effects are, and described as bleeding
// damage, so it is a bleed by this record alone.
constexpr RE::FormID kTargeOfTheBloodedBleed = 0x0010582C;

// Is this a bleed? The engine has no bleed of its own: vanilla's is an
// unresisted Damage Health, as are a hundred effects that are not, so the
// record is all that tells one apart. Vanilla's, then any of its name --
// the Redguard CC's and most mods' own take it -- then the Targe's, then
// any carrying the keyword Simonrim's mods share, which marks Adamant's axe
// wound, named Damage Health. A bleed that is none of these is not one.
// Indexed once, on first use, after the data has loaded.
bool IsBleed(const RE::EffectSetting *effect)
{
    static const auto bleeds = [] {
        std::unordered_set<RE::FormID> ids;
        if (const auto *vanilla = RE::TESForm::LookupByID<RE::EffectSetting>(kPerkBleedingDamage))
            for (const RE::FormID id : SameNamedEffects(vanilla, vanilla->GetFormID()))
                ids.insert(id);
        const std::size_t named = ids.size();
        ids.insert(kTargeOfTheBloodedBleed);
        const std::size_t listed = ids.size();
        const auto *keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("MAG_MagicDamageBleed");
        if (auto *data = RE::TESDataHandler::GetSingleton(); data && keyword)
            for (const auto *each : data->GetFormArray<RE::EffectSetting>())
                if (each && each->HasKeyword(keyword))
                    ids.insert(each->GetFormID());
        log::sensors.debug("bleeding: {} effect(s), {} by vanilla's name, the Targe of the Blooded's, {} more by "
                           "keyword{}",
                           ids.size(), named, ids.size() - listed,
                           keyword ? "" : " (the keyword is not in the load order)");
        return ids;
    }();
    return bleeds.contains(effect->GetFormID());
}

// Each actor's statuses as last read, so the log can say when they change:
// otherwise a Status rule that never holds cannot tell "not detected" from
// "never happened". Read by the tick and by the panel's pages.
std::mutex g_statusesMutex;
std::unordered_map<RE::FormID, std::uint32_t> g_statuses;

void LogStatusChanges(RE::Actor *actor, std::uint32_t now)
{
    std::uint32_t was = 0;
    {
        std::scoped_lock lock(g_statusesMutex);
        std::uint32_t &last = g_statuses[actor->GetFormID()];
        was = last;
        last = now;
    }
    if (was == now)
        return;
    std::string gained;
    std::string lost;
    for (std::size_t i = 0; i < static_cast<std::size_t>(ft::StatusKind::COUNT); ++i)
    {
        const auto kind = static_cast<ft::StatusKind>(i);
        if (((was ^ now) & ft::Bit(kind)) == 0)
            continue;
        std::string &into = (now & ft::Bit(kind)) != 0 ? gained : lost;
        into += (into.empty() ? "" : ", ") + std::string(ft::WireName(kind));
    }
    log::sensors.debug("{}: {}{}{}", Describe(actor), gained.empty() ? "" : "now " + gained,
                       gained.empty() || lost.empty() ? "" : "; ", lost.empty() ? "" : "no longer " + lost);
}

} // namespace

void ForgetStatuses()
{
    std::scoped_lock lock(g_statusesMutex);
    g_statuses.clear();
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
                // Burning, frostbitten, shocked: the element's damage to the
                // actor, an effect resisted by it that lowers a value of the
                // actor it runs on. The keyword would say the same of vanilla
                // spells; the resist value says it of modded ones too. The
                // lowering keeps out what only names the resistance: an
                // atronach's cloak on the atronach, which harms whoever
                // comes near through an effect of its own, and a visual.
                if (LowersOwnValue(base))
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
                if (IsBleed(base))
                    traits.Set(ft::StatusKind::Bleeding);
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
    LogStatusChanges(actor, traits.status);
    return traits;
}

} // namespace ft::game
