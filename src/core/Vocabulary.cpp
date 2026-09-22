#include "Vocabulary.h"

#include "core/I18n.h"

#include <array>
#include <utility>

namespace ft
{
using i18n::Tr;

namespace
{

// An entry carries both names, so a value cannot acquire one without the other
// and the two cannot drift apart in separate tables. Keeping them adjacent is
// also the clearest possible statement of which is which.
template <typename Enum> struct Entry
{
    Enum value;
    std::string_view wire;
    std::string_view display;
};

// Lookups are linear scans: these tables have single-digit entry counts and are
// touched when a profile loads or a dropdown opens, never in the tick.
template <typename Enum, std::size_t N>
[[nodiscard]] std::string_view LookupWire(const std::array<Entry<Enum>, N> &table, Enum value) noexcept
{
    for (const auto &e : table)
    {
        if (e.value == value)
            return e.wire;
    }
    return "Unknown";
}

template <typename Enum, std::size_t N>
[[nodiscard]] std::string_view LookupDisplay(const std::array<Entry<Enum>, N> &table, Enum value) noexcept
{
    for (const auto &e : table)
    {
        if (e.value == value)
            return Tr(e.display);
    }
    return "Unknown";
}

// Deliberately matches the WIRE name only. There is no display-name parser, so
// keying logic off translated text is not something this API permits.
template <typename Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> Parse(const std::array<Entry<Enum>, N> &table, std::string_view s) noexcept
{
    for (const auto &e : table)
    {
        if (e.wire == s)
            return e.value;
    }
    return std::nullopt;
}

constexpr std::array<Entry<SubjectKind>, 6> kSubjects{{
    {SubjectKind::Self, "self", N_("Self")},
    {SubjectKind::Player, "player", N_("Player")},
    {SubjectKind::Ally, "ally", N_("Ally")},
    {SubjectKind::Enemy, "enemy", N_("Enemy")},
    {SubjectKind::Follower, "follower", N_("Follower")},
    {SubjectKind::Corpse, "corpse", N_("Corpse")},
}};

constexpr std::array<Entry<PredicateKind>, 37> kPredicates{{
    {PredicateKind::Any, "any", N_("Any")}, // Dragon Age's word: "Enemy: Any", "Self: Any"
    {PredicateKind::CombatBegins, "combat-begins", N_("Combat start")},
    {PredicateKind::CombatEnds, "combat-ends", N_("Combat end")},
    {PredicateKind::HealthPctBelow, "health-pct-below", N_("Health")},
    {PredicateKind::StaminaPctBelow, "stamina-pct-below", N_("Stamina")},
    {PredicateKind::MagickaPctBelow, "magicka-pct-below", N_("Magicka")},
    {PredicateKind::Type, "type", N_("Type")},
    {PredicateKind::Status, "status", N_("Status")},
    {PredicateKind::ArmorPctBelow, "armor-pct-below", N_("Armor")},
    {PredicateKind::ResistancePctBelow, "resistance-pct-below", N_("Resistance")},
    {PredicateKind::Attacking, "attacking", N_("Attacking")},
    {PredicateKind::AttackedBy, "attacked-by", N_("Attacked by")},
    {PredicateKind::HitType, "hit-type", N_("Attacks with")},
    {PredicateKind::HitBy, "hit-by", N_("Hit by")},
    {PredicateKind::HealthLowest, "health-lowest", N_("Health lowest")},
    {PredicateKind::HealthHighest, "health-highest", N_("Health highest")},
    {PredicateKind::StaminaLowest, "stamina-lowest", N_("Stamina lowest")},
    {PredicateKind::StaminaHighest, "stamina-highest", N_("Stamina highest")},
    {PredicateKind::MagickaLowest, "magicka-lowest", N_("Magicka lowest")},
    {PredicateKind::MagickaHighest, "magicka-highest", N_("Magicka highest")},
    {PredicateKind::ArmorLowest, "armor-lowest", N_("Armor lowest")},
    {PredicateKind::ArmorHighest, "armor-highest", N_("Armor highest")},
    {PredicateKind::ResistanceLowest, "resistance-lowest", N_("Resistance lowest")},
    {PredicateKind::ResistanceHighest, "resistance-highest", N_("Resistance highest")},
    {PredicateKind::HealthPctAbove, "health-pct-above", N_("Health")},
    {PredicateKind::StaminaPctAbove, "stamina-pct-above", N_("Stamina")},
    {PredicateKind::MagickaPctAbove, "magicka-pct-above", N_("Magicka")},
    {PredicateKind::ArmorPctAbove, "armor-pct-above", N_("Armor")},
    {PredicateKind::ResistancePctAbove, "resistance-pct-above", N_("Resistance")},
    {PredicateKind::SummonNone, "summon-none", N_("Summon: none")},
    {PredicateKind::SummonActive, "summon-active", N_("Summon: active")},
    {PredicateKind::CorpseNone, "corpse-none", N_("None")},
    {PredicateKind::LevelHighest, "level-highest", N_("Highest level")},
    {PredicateKind::LevelLowest, "level-lowest", N_("Lowest level")},
    {PredicateKind::WeaponChargeNeeded, "weapon-charge-needed", N_("Weapon charge: needed")},
    {PredicateKind::WeaponPoisonNone, "weapon-poison-none", N_("Weapon poison: none")},
    {PredicateKind::WeaponPoisonActive, "weapon-poison-active", N_("Weapon poison: active")},
}};

constexpr std::array<Entry<ActionTargetKind>, 7> kActionTargets{{
    {ActionTargetKind::Self, "self", N_("Self")},
    {ActionTargetKind::Player, "player", N_("Player")},
    {ActionTargetKind::Ally, "ally", N_("Ally")},
    {ActionTargetKind::Enemy, "enemy", N_("Enemy")},
    {ActionTargetKind::Attacker, "attacker", N_("Attacker")},
    {ActionTargetKind::Follower, "follower", N_("Follower")},
    {ActionTargetKind::Corpse, "corpse", N_("Corpse")},
}};

constexpr std::array<Entry<ActionKind>, 33> kActions{{
    // The consumable slugs name the SELECTION POLICY, not just the item
    // type, because that is part of the behaviour a profile is asking for:
    // "drink-weakest" -- don't burn a strong potion on a scratch -- beside
    // "drink-strongest", the effect named on the action.
    {ActionKind::None, "none", N_("None")},
    {ActionKind::Attack, "attack", N_("Focus on target")},
    {ActionKind::PowerAttack, "power-attack", N_("Power Attack")},
    {ActionKind::Bash, "bash", N_("Bash")},
    {ActionKind::PowerBash, "power-bash", N_("Power Bash")},
    {ActionKind::EquipWeapon, "equip-weapon", N_("Equip weapon")},
    {ActionKind::EquipArrows, "equip-arrows", N_("Equip arrows")},
    {ActionKind::EquipStrongestArrows, "equip-strongest-arrows", N_("Equip strongest arrows")},
    {ActionKind::EquipWeakestArrows, "equip-weakest-arrows", N_("Equip weakest arrows")},
    {ActionKind::EquipSpell, "equip-spell", N_("Equip spell")},
    {ActionKind::EquipArmor, "equip-armor", N_("Equip armor")},
    {ActionKind::ChargeStrongestSoulGem, "charge-strongest-soul-gem", N_("Charge with strongest soul gem")},
    {ActionKind::ChargeWeakestSoulGem, "charge-weakest-soul-gem", N_("Charge with weakest soul gem")},
    {ActionKind::ChargeSoulGem, "charge-soul-gem", N_("Charge with soul gem")},
    {ActionKind::ApplyStrongest, "apply-strongest", N_("Apply strongest poison")},
    {ActionKind::ApplyWeakest, "apply-weakest", N_("Apply weakest poison")},
    {ActionKind::ApplyAny, "apply-any", N_("Apply any poison")},
    {ActionKind::ApplyPoison, "apply-poison", N_("Apply poison")},
    {ActionKind::DrinkStrongest, "drink-strongest", N_("Drink strongest potion")},
    {ActionKind::DrinkWeakest, "drink-weakest", N_("Drink weakest potion")},
    {ActionKind::DrinkAny, "drink-any", N_("Drink any buff potion")},
    {ActionKind::DrinkPotion, "drink-potion", N_("Drink potion")},
    {ActionKind::EatStrongestFood, "eat-strongest-food", N_("Eat strongest food")},
    {ActionKind::EatWeakestFood, "eat-weakest-food", N_("Eat weakest food")},
    {ActionKind::EatAnyFood, "eat-any-food", N_("Eat any buff food")},
    {ActionKind::EatStrongestIngredient, "eat-strongest-ingredient", N_("Eat strongest ingredient")},
    {ActionKind::EatWeakestIngredient, "eat-weakest-ingredient", N_("Eat weakest ingredient")},
    {ActionKind::EatFood, "eat-food", N_("Eat food")},
    {ActionKind::EatIngredient, "eat-ingredient", N_("Eat ingredient")},
    {ActionKind::CastSpell, "cast-spell", N_("Cast spell")},
    {ActionKind::UsePower, "use-power", N_("Use power")},
    {ActionKind::Shout, "shout", N_("Shout")},
    {ActionKind::UseScroll, "use-scroll", N_("Scroll")},
}};

// The hand an equip rule names. Both is one value, not two flags, on the
// wire: a profile says "both", not a bit set.
constexpr std::array<Entry<Hand>, 4> kHands{{
    {Hand::None, "none", N_("None")},
    {Hand::Left, "left", N_("Left")},
    {Hand::Right, "right", N_("Right")},
    {Hand::Both, "both", N_("Both")},
}};

// A status, as the rule names it and as the menu shows it.
constexpr std::array<Entry<StatusKind>, 14> kStatuses{{
    {StatusKind::Poisoned, "poisoned", N_("Poisoned")},
    {StatusKind::Burning, "burning", N_("Burning")},
    {StatusKind::Frostbitten, "frostbitten", N_("Frostbitten")},
    {StatusKind::Shocked, "shocked", N_("Shocked")},
    {StatusKind::Paralysed, "paralyzed", N_("Paralyzed")},
    {StatusKind::Staggered, "staggered", N_("Staggered")},
    {StatusKind::Fleeing, "fleeing", N_("Fleeing")},
    {StatusKind::BleedingOut, "bleeding-out", N_("Bleeding out")},
    {StatusKind::Invisible, "invisible", N_("Invisible")},
    {StatusKind::Ethereal, "ethereal", N_("Ethereal")},
    {StatusKind::Blocking, "blocking", N_("Blocking")},
    {StatusKind::Casting, "casting", N_("Casting")},
    {StatusKind::Sneaking, "sneaking", N_("Sneaking")},
    {StatusKind::Diseased, "diseased", N_("Diseased")},
}};

// A kind of being, as the rule names it and as the menu shows it. A group's
// head reads as the group: "Enemy: Man".
constexpr std::array<Entry<TypeKind>, 26> kTypes{{
    {TypeKind::Man, "man", N_("Man")},
    {TypeKind::Breton, "breton", N_("Breton")},
    {TypeKind::Imperial, "imperial", N_("Imperial")},
    {TypeKind::Nord, "nord", N_("Nord")},
    {TypeKind::Redguard, "redguard", N_("Redguard")},
    {TypeKind::Elf, "elf", N_("Elf")},
    {TypeKind::DarkElf, "dark-elf", N_("Dark Elf")},
    {TypeKind::Falmer, "falmer", N_("Falmer")},
    {TypeKind::HighElf, "high-elf", N_("High Elf")},
    {TypeKind::SnowElf, "snow-elf", N_("Snow Elf")},
    {TypeKind::WoodElf, "wood-elf", N_("Wood Elf")},
    {TypeKind::Beast, "beast", N_("Beast")},
    {TypeKind::Argonian, "argonian", N_("Argonian")},
    {TypeKind::Khajiit, "khajiit", N_("Khajiit")},
    {TypeKind::Orc, "orc", N_("Orc")},
    {TypeKind::Creature, "creature", N_("Creature")},
    {TypeKind::Animal, "animal", N_("Animal")},
    {TypeKind::Automaton, "automaton", N_("Automaton")},
    {TypeKind::Daedra, "daedra", N_("Daedra")},
    {TypeKind::Dragon, "dragon", N_("Dragon")},
    {TypeKind::Giant, "giant", N_("Giant")},
    {TypeKind::Spriggan, "spriggan", N_("Spriggan")},
    {TypeKind::Troll, "troll", N_("Troll")},
    {TypeKind::Undead, "undead", N_("Undead")},
    {TypeKind::Vampire, "vampire", N_("Vampire")},
    {TypeKind::Werewolf, "werewolf", N_("Werewolf")},
}};

constexpr std::array<Entry<DamageKind>, 8> kDamageKinds{{
    {DamageKind::Melee, "melee", N_("Melee")},
    {DamageKind::Ranged, "ranged", N_("Ranged")},
    {DamageKind::Magic, "magic", N_("Magic")},
    {DamageKind::Fire, "fire", N_("Fire")},
    {DamageKind::Frost, "frost", N_("Frost")},
    {DamageKind::Shock, "shock", N_("Shock")},
    {DamageKind::Poison, "poison", N_("Poison")},
    {DamageKind::Any, "any", N_("Any")},
}};

// Every enumerator must appear in its table, or a rule would serialise as
// "Unknown" and fail to load back. Cheap to assert, impossible to forget.
static_assert(kSubjects.size() == static_cast<std::size_t>(SubjectKind::COUNT));
static_assert(kPredicates.size() == static_cast<std::size_t>(PredicateKind::COUNT));
static_assert(kActionTargets.size() == static_cast<std::size_t>(ActionTargetKind::COUNT));
static_assert(kActions.size() == static_cast<std::size_t>(ActionKind::COUNT));
static_assert(kStatuses.size() == static_cast<std::size_t>(StatusKind::COUNT));
static_assert(kTypes.size() == static_cast<std::size_t>(TypeKind::COUNT));
static_assert(kDamageKinds.size() == static_cast<std::size_t>(DamageKind::COUNT));

} // namespace

std::string_view WireName(SubjectKind v) noexcept
{
    return LookupWire(kSubjects, v);
}
std::string_view WireName(PredicateKind v) noexcept
{
    return LookupWire(kPredicates, v);
}
std::string_view WireName(ActionTargetKind v) noexcept
{
    return LookupWire(kActionTargets, v);
}
std::string_view WireName(ActionKind v) noexcept
{
    return LookupWire(kActions, v);
}
std::string_view WireName(Hand v) noexcept
{
    return LookupWire(kHands, v);
}
std::string_view WireName(StatusKind v) noexcept
{
    return LookupWire(kStatuses, v);
}
std::string_view WireName(TypeKind v) noexcept
{
    return LookupWire(kTypes, v);
}
std::string_view WireName(DamageKind v) noexcept
{
    return LookupWire(kDamageKinds, v);
}

std::optional<SubjectKind> SubjectFromWireName(std::string_view s) noexcept
{
    return Parse(kSubjects, s);
}
std::optional<PredicateKind> PredicateFromWireName(std::string_view s) noexcept
{
    return Parse(kPredicates, s);
}
std::optional<ActionTargetKind> ActionTargetFromWireName(std::string_view s) noexcept
{
    return Parse(kActionTargets, s);
}
std::optional<ActionKind> ActionFromWireName(std::string_view s) noexcept
{
    return Parse(kActions, s);
}
std::optional<Hand> HandFromWireName(std::string_view s) noexcept
{
    return Parse(kHands, s);
}
std::optional<StatusKind> StatusFromWireName(std::string_view s) noexcept
{
    return Parse(kStatuses, s);
}
std::optional<TypeKind> TypeFromWireName(std::string_view s) noexcept
{
    return Parse(kTypes, s);
}
std::optional<DamageKind> DamageFromWireName(std::string_view s) noexcept
{
    return Parse(kDamageKinds, s);
}

bool IsWireName(std::string_view s) noexcept
{
    // A slug: lowercase ASCII letters and digits, hyphen-separated. No leading,
    // trailing or doubled hyphen.
    //
    // Strict on purpose. The shape is what makes "never localise these"
    // enforceable rather than merely requested: no translated string survives
    // it, since display text carries capitals, spaces and accents.
    if (s.empty())
        return false;

    const auto isLower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };

    if (!isLower(s.front()) || s.back() == '-')
        return false;

    bool previousWasHyphen = false;
    for (char c : s)
    {
        if (c == '-')
        {
            if (previousWasHyphen)
                return false;
            previousWasHyphen = true;
            continue;
        }
        if (!isLower(c) && !isDigit(c))
            return false;
        previousWasHyphen = false;
    }
    return true;
}

std::string_view DisplayName(SubjectKind v) noexcept
{
    return LookupDisplay(kSubjects, v);
}
std::string_view DisplayName(PredicateKind v) noexcept
{
    return LookupDisplay(kPredicates, v);
}
std::string_view DisplayName(ActionTargetKind v) noexcept
{
    return LookupDisplay(kActionTargets, v);
}
std::string_view DisplayName(ActionKind v) noexcept
{
    return LookupDisplay(kActions, v);
}
std::string_view DisplayName(Hand v) noexcept
{
    return LookupDisplay(kHands, v);
}
std::string_view DisplayName(StatusKind v) noexcept
{
    return LookupDisplay(kStatuses, v);
}
std::string_view DisplayName(TypeKind v) noexcept
{
    return LookupDisplay(kTypes, v);
}
std::string_view DisplayName(DamageKind v) noexcept
{
    return LookupDisplay(kDamageKinds, v);
}
ArgumentKind ArgumentFor(PredicateKind predicate) noexcept
{
    // A threshold, on either side of the grid; the extremes and every
    // predicate off the grid carry nothing.
    const Side side = GridOf(predicate).side;
    return side == Side::Below || side == Side::Above ? ArgumentKind::Percent : ArgumentKind::None;
}

std::string_view Describe(PredicateKind v) noexcept
{
    // One short sentence each: these are tooltips.
    switch (v)
    {
    case PredicateKind::Any:
        return ""; // says it all in its name; no tooltip
    case PredicateKind::HealthPctBelow:
        return Tr("Health under this share of its maximum.");
    // Only where the name does not say it all: a tooltip that repeats the
    // label, or common sense, is noise (2026-09-18).
    case PredicateKind::CombatBegins:
        return Tr("Runs immediately after combat starts");
    case PredicateKind::CombatEnds:
        return Tr("Runs immediately after combat ends");
    case PredicateKind::HitBy:
        return Tr("In the last few seconds.");
    case PredicateKind::Attacking:
        return Tr("This enemy's target is that party member.");
    case PredicateKind::AttackedBy:
        return Tr("That party member's target is this enemy.");
    case PredicateKind::SummonNone:
    case PredicateKind::SummonActive:
        return Tr("A summon or a raised corpse.");
    case PredicateKind::WeaponChargeNeeded:
        return Tr("An enchanted weapon in hand cannot pay for one more hit.");
    case PredicateKind::CorpseNone:
    case PredicateKind::LevelHighest:
    case PredicateKind::LevelLowest:
        return Tr("Counts only corpses the rule's Reanimate spell can raise.");
    default:
        return "";
    }
}

std::string_view Noun(ActionKind v) noexcept
{
    switch (v)
    {
    case ActionKind::EquipWeapon:
        return Tr("weapon");
    case ActionKind::EquipArrows:
        return Tr("arrows");
    case ActionKind::EquipStrongestArrows:
        return Tr("strongest arrows");
    case ActionKind::EquipWeakestArrows:
        return Tr("weakest arrows");
    case ActionKind::EquipSpell:
        return Tr("spell");
    case ActionKind::EquipArmor:
        return Tr("armor");
    case ActionKind::ChargeStrongestSoulGem:
        return Tr("strongest soul gem");
    case ActionKind::ChargeWeakestSoulGem:
        return Tr("weakest soul gem");
    case ActionKind::ChargeSoulGem:
        return Tr("soul gem");
    default:
        return "";
    }
}

std::string_view Describe(ActionKind v) noexcept
{
    // Only where the name does not say it all, as for a predicate: what a
    // pin promises, what "strongest" means of a soul gem, what "any" is
    // drawn from.
    switch (v)
    {
    case ActionKind::ChargeStrongestSoulGem:
        return Tr("The largest soul gem that would not overfill the weapon.");
    case ActionKind::ChargeWeakestSoulGem:
        return Tr("The smallest soul gem carried.");
    case ActionKind::ApplyAny:
        return Tr("Any poison carried.");
    case ActionKind::DrinkAny:
        return Tr("Any potion carried that applies a buff.");
    case ActionKind::EatAnyFood:
        return Tr("Any food carried that applies a buff.");
    case ActionKind::EquipWeapon:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArrows:
    case ActionKind::EquipArmor:
        return Tr("Equip until replaced by player or another rule");
    default:
        return "";
    }
}

} // namespace ft
