#include "Vocabulary.h"

#include <array>
#include <utility>

namespace ft
{
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
            return e.display;
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
    {SubjectKind::Self, "self", "Self"},
    {SubjectKind::Player, "player", "Player"},
    {SubjectKind::Ally, "ally", "Ally"},
    {SubjectKind::Enemy, "enemy", "Enemy"},
    {SubjectKind::Follower, "follower", "Follower"},
    {SubjectKind::Corpse, "corpse", "Corpse"},
}};

constexpr std::array<Entry<PredicateKind>, 37> kPredicates{{
    {PredicateKind::Any, "any", "Any"}, // Dragon Age's word: "Enemy: Any", "Self: Any"
    {PredicateKind::CombatBegins, "combat-begins", "Combat start"},
    {PredicateKind::CombatEnds, "combat-ends", "Combat end"},
    {PredicateKind::HealthPctBelow, "health-pct-below", "Health"},
    {PredicateKind::StaminaPctBelow, "stamina-pct-below", "Stamina"},
    {PredicateKind::MagickaPctBelow, "magicka-pct-below", "Magicka"},
    {PredicateKind::Type, "type", "Type"},
    {PredicateKind::Status, "status", "Status"},
    {PredicateKind::ArmorPctBelow, "armor-pct-below", "Armor"},
    {PredicateKind::ResistancePctBelow, "resistance-pct-below", "Resistance"},
    {PredicateKind::Attacking, "attacking", "Attacking"},
    {PredicateKind::AttackedBy, "attacked-by", "Attacked by"},
    {PredicateKind::HitType, "hit-type", "Attacks with"},
    {PredicateKind::HitBy, "hit-by", "Hit by"},
    {PredicateKind::HealthLowest, "health-lowest", "Health lowest"},
    {PredicateKind::HealthHighest, "health-highest", "Health highest"},
    {PredicateKind::StaminaLowest, "stamina-lowest", "Stamina lowest"},
    {PredicateKind::StaminaHighest, "stamina-highest", "Stamina highest"},
    {PredicateKind::MagickaLowest, "magicka-lowest", "Magicka lowest"},
    {PredicateKind::MagickaHighest, "magicka-highest", "Magicka highest"},
    {PredicateKind::ArmorLowest, "armor-lowest", "Armor lowest"},
    {PredicateKind::ArmorHighest, "armor-highest", "Armor highest"},
    {PredicateKind::ResistanceLowest, "resistance-lowest", "Resistance lowest"},
    {PredicateKind::ResistanceHighest, "resistance-highest", "Resistance highest"},
    {PredicateKind::HealthPctAbove, "health-pct-above", "Health"},
    {PredicateKind::StaminaPctAbove, "stamina-pct-above", "Stamina"},
    {PredicateKind::MagickaPctAbove, "magicka-pct-above", "Magicka"},
    {PredicateKind::ArmorPctAbove, "armor-pct-above", "Armor"},
    {PredicateKind::ResistancePctAbove, "resistance-pct-above", "Resistance"},
    {PredicateKind::SummonNone, "summon-none", "Summon: none"},
    {PredicateKind::SummonActive, "summon-active", "Summon: active"},
    {PredicateKind::CorpseNone, "corpse-none", "None"},
    {PredicateKind::LevelHighest, "level-highest", "Highest level"},
    {PredicateKind::LevelLowest, "level-lowest", "Lowest level"},
    {PredicateKind::WeaponChargeNeeded, "weapon-charge-needed", "Weapon charge: needed"},
    {PredicateKind::WeaponPoisonNone, "weapon-poison-none", "Weapon poison: none"},
    {PredicateKind::WeaponPoisonActive, "weapon-poison-active", "Weapon poison: active"},
}};

constexpr std::array<Entry<ActionTargetKind>, 7> kActionTargets{{
    {ActionTargetKind::Self, "self", "Self"},
    {ActionTargetKind::Player, "player", "Player"},
    {ActionTargetKind::Ally, "ally", "Ally"},
    {ActionTargetKind::Enemy, "enemy", "Enemy"},
    {ActionTargetKind::Attacker, "attacker", "Attacker"},
    {ActionTargetKind::Follower, "follower", "Follower"},
    {ActionTargetKind::Corpse, "corpse", "Corpse"},
}};

constexpr std::array<Entry<ActionKind>, 33> kActions{{
    // The consumable slugs name the SELECTION POLICY, not just the item
    // type, because that is part of the behaviour a profile is asking for:
    // "drink-weakest" -- don't burn a strong potion on a scratch -- beside
    // "drink-strongest", the effect named on the action.
    {ActionKind::None, "none", "None"},
    {ActionKind::Attack, "attack", "Focus on target"},
    {ActionKind::PowerAttack, "power-attack", "Power Attack"},
    {ActionKind::Bash, "bash", "Bash"},
    {ActionKind::PowerBash, "power-bash", "Power Bash"},
    {ActionKind::EquipWeapon, "equip-weapon", "Equip weapon"},
    {ActionKind::EquipArrows, "equip-arrows", "Equip arrows"},
    {ActionKind::EquipStrongestArrows, "equip-strongest-arrows", "Equip strongest arrows"},
    {ActionKind::EquipWeakestArrows, "equip-weakest-arrows", "Equip weakest arrows"},
    {ActionKind::EquipSpell, "equip-spell", "Equip spell"},
    {ActionKind::EquipArmor, "equip-armor", "Equip armor"},
    {ActionKind::ChargeStrongestSoulGem, "charge-strongest-soul-gem", "Charge with strongest soul gem"},
    {ActionKind::ChargeWeakestSoulGem, "charge-weakest-soul-gem", "Charge with weakest soul gem"},
    {ActionKind::ChargeSoulGem, "charge-soul-gem", "Charge with soul gem"},
    {ActionKind::ApplyStrongest, "apply-strongest", "Apply strongest poison"},
    {ActionKind::ApplyWeakest, "apply-weakest", "Apply weakest poison"},
    {ActionKind::ApplyAny, "apply-any", "Apply any poison"},
    {ActionKind::ApplyPoison, "apply-poison", "Apply poison"},
    {ActionKind::DrinkStrongest, "drink-strongest", "Drink strongest potion"},
    {ActionKind::DrinkWeakest, "drink-weakest", "Drink weakest potion"},
    {ActionKind::DrinkAny, "drink-any", "Drink any buff potion"},
    {ActionKind::DrinkPotion, "drink-potion", "Drink potion"},
    {ActionKind::EatStrongestFood, "eat-strongest-food", "Eat strongest food"},
    {ActionKind::EatWeakestFood, "eat-weakest-food", "Eat weakest food"},
    {ActionKind::EatAnyFood, "eat-any-food", "Eat any buff food"},
    {ActionKind::EatStrongestIngredient, "eat-strongest-ingredient", "Eat strongest ingredient"},
    {ActionKind::EatWeakestIngredient, "eat-weakest-ingredient", "Eat weakest ingredient"},
    {ActionKind::EatFood, "eat-food", "Eat food"},
    {ActionKind::EatIngredient, "eat-ingredient", "Eat ingredient"},
    {ActionKind::CastSpell, "cast-spell", "Cast spell"},
    {ActionKind::UsePower, "use-power", "Use power"},
    {ActionKind::Shout, "shout", "Shout"},
    {ActionKind::UseScroll, "use-scroll", "Scroll"},
}};

// The hand an equip rule names. Both is one value, not two flags, on the
// wire: a profile says "both", not a bit set.
constexpr std::array<Entry<Hand>, 4> kHands{{
    {Hand::None, "none", "None"},
    {Hand::Left, "left", "Left"},
    {Hand::Right, "right", "Right"},
    {Hand::Both, "both", "Both"},
}};

// A status, as the rule names it and as the menu shows it.
constexpr std::array<Entry<StatusKind>, 13> kStatuses{{
    {StatusKind::Poisoned, "poisoned", "Poisoned"},
    {StatusKind::Burning, "burning", "Burning"},
    {StatusKind::Frostbitten, "frostbitten", "Frostbitten"},
    {StatusKind::Shocked, "shocked", "Shocked"},
    {StatusKind::Paralysed, "paralyzed", "Paralyzed"},
    {StatusKind::Staggered, "staggered", "Staggered"},
    {StatusKind::Fleeing, "fleeing", "Fleeing"},
    {StatusKind::BleedingOut, "bleeding-out", "Bleeding out"},
    {StatusKind::Invisible, "invisible", "Invisible"},
    {StatusKind::Ethereal, "ethereal", "Ethereal"},
    {StatusKind::Blocking, "blocking", "Blocking"},
    {StatusKind::Casting, "casting", "Casting"},
    {StatusKind::Sneaking, "sneaking", "Sneaking"},
}};

// A kind of being, as the rule names it and as the menu shows it. A group's
// head reads as the group: "Enemy: Man".
constexpr std::array<Entry<TypeKind>, 26> kTypes{{
    {TypeKind::Man, "man", "Man"},
    {TypeKind::Breton, "breton", "Breton"},
    {TypeKind::Imperial, "imperial", "Imperial"},
    {TypeKind::Nord, "nord", "Nord"},
    {TypeKind::Redguard, "redguard", "Redguard"},
    {TypeKind::Elf, "elf", "Elf"},
    {TypeKind::DarkElf, "dark-elf", "Dark Elf"},
    {TypeKind::Falmer, "falmer", "Falmer"},
    {TypeKind::HighElf, "high-elf", "High Elf"},
    {TypeKind::SnowElf, "snow-elf", "Snow Elf"},
    {TypeKind::WoodElf, "wood-elf", "Wood Elf"},
    {TypeKind::Beast, "beast", "Beast"},
    {TypeKind::Argonian, "argonian", "Argonian"},
    {TypeKind::Khajiit, "khajiit", "Khajiit"},
    {TypeKind::Orc, "orc", "Orc"},
    {TypeKind::Creature, "creature", "Creature"},
    {TypeKind::Animal, "animal", "Animal"},
    {TypeKind::Automaton, "automaton", "Automaton"},
    {TypeKind::Daedra, "daedra", "Daedra"},
    {TypeKind::Dragon, "dragon", "Dragon"},
    {TypeKind::Giant, "giant", "Giant"},
    {TypeKind::Spriggan, "spriggan", "Spriggan"},
    {TypeKind::Troll, "troll", "Troll"},
    {TypeKind::Undead, "undead", "Undead"},
    {TypeKind::Vampire, "vampire", "Vampire"},
    {TypeKind::Werewolf, "werewolf", "Werewolf"},
}};

constexpr std::array<Entry<DamageKind>, 8> kDamageKinds{{
    {DamageKind::Melee, "melee", "Melee"},
    {DamageKind::Ranged, "ranged", "Ranged"},
    {DamageKind::Magic, "magic", "Magic"},
    {DamageKind::Fire, "fire", "Fire"},
    {DamageKind::Frost, "frost", "Frost"},
    {DamageKind::Shock, "shock", "Shock"},
    {DamageKind::Poison, "poison", "Poison"},
    {DamageKind::Any, "any", "Any"},
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
        return "Health under this share of its maximum.";
    // Only where the name does not say it all: a tooltip that repeats the
    // label, or common sense, is noise (2026-09-18).
    case PredicateKind::CombatBegins:
        return "Runs immediately after combat starts";
    case PredicateKind::CombatEnds:
        return "Runs immediately after combat ends";
    case PredicateKind::HitBy:
        return "In the last few seconds.";
    case PredicateKind::Attacking:
        return "This enemy's target is that party member.";
    case PredicateKind::AttackedBy:
        return "That party member's target is this enemy.";
    case PredicateKind::SummonNone:
    case PredicateKind::SummonActive:
        return "A summon or a raised corpse.";
    case PredicateKind::WeaponChargeNeeded:
        return "An enchanted weapon in hand cannot pay for one more hit.";
    case PredicateKind::CorpseNone:
    case PredicateKind::LevelHighest:
    case PredicateKind::LevelLowest:
        return "Counts only corpses the rule's Reanimate spell can raise.";
    default:
        return "";
    }
}

std::string_view Noun(ActionKind v) noexcept
{
    switch (v)
    {
    case ActionKind::EquipWeapon:
        return "weapon";
    case ActionKind::EquipArrows:
        return "arrows";
    case ActionKind::EquipStrongestArrows:
        return "strongest arrows";
    case ActionKind::EquipWeakestArrows:
        return "weakest arrows";
    case ActionKind::EquipSpell:
        return "spell";
    case ActionKind::EquipArmor:
        return "armor";
    case ActionKind::ChargeStrongestSoulGem:
        return "strongest soul gem";
    case ActionKind::ChargeWeakestSoulGem:
        return "weakest soul gem";
    case ActionKind::ChargeSoulGem:
        return "soul gem";
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
        return "The largest soul gem that would not overfill the weapon.";
    case ActionKind::ChargeWeakestSoulGem:
        return "The smallest soul gem carried.";
    case ActionKind::ApplyAny:
        return "Any poison carried.";
    case ActionKind::DrinkAny:
        return "Any potion carried that applies a buff.";
    case ActionKind::EatAnyFood:
        return "Any food carried that applies a buff.";
    case ActionKind::EquipWeapon:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArrows:
    case ActionKind::EquipArmor:
        return "Equip until replaced by player or another rule";
    default:
        return "";
    }
}

} // namespace ft
