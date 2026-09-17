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
    {PredicateKind::HitType, "hit-type", "Hit type"},
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

constexpr std::array<Entry<ActionKind>, 32> kActions{{
    // The consumable slugs name the SELECTION POLICY, not just the item
    // type, because that is part of the behaviour a profile is asking for:
    // "drink-weakest" -- don't burn a strong potion on a scratch -- beside
    // "drink-strongest", the effect named on the action.
    {ActionKind::None, "none", "None"},
    {ActionKind::Attack, "attack", "Attack"},
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
    {ActionKind::DrinkAny, "drink-any", "Drink any buff"},
    {ActionKind::DrinkPotion, "drink-potion", "Drink potion"},
    {ActionKind::EatStrongestFood, "eat-strongest-food", "Eat strongest food"},
    {ActionKind::EatWeakestFood, "eat-weakest-food", "Eat weakest food"},
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
    case PredicateKind::HealthPctAbove:
        return "Health over this share of its maximum.";
    case PredicateKind::MagickaPctBelow:
        return "Magicka under this share of its maximum.";
    case PredicateKind::MagickaPctAbove:
        return "Magicka over this share of its maximum.";
    case PredicateKind::StaminaPctBelow:
        return "Stamina under this share of its maximum.";
    case PredicateKind::StaminaPctAbove:
        return "Stamina over this share of its maximum.";
    case PredicateKind::CombatBegins:
        return "A fight has just begun.";
    case PredicateKind::CombatEnds:
        return "A fight has just ended; no other condition holds on that pass.";
    case PredicateKind::Type:
        return ""; // the kind's own name says it; no tooltip
    case PredicateKind::Status:
        return "In this state right now.";
    case PredicateKind::ArmorPctBelow:
        return "The armour turns away under this share of a blow; the most is 80%.";
    case PredicateKind::ArmorPctAbove:
        return "The armour turns away over this share of a blow; the most is 80%.";
    case PredicateKind::ResistancePctBelow:
        return "Resistance to that kind of damage under this much; a weakness is below zero.";
    case PredicateKind::ResistancePctAbove:
        return "Resistance to that kind of damage over this much; 100% is immune.";
    case PredicateKind::StaminaLowest:
        return "The one with the least stamina.";
    case PredicateKind::StaminaHighest:
        return "The one with the most stamina.";
    case PredicateKind::MagickaLowest:
        return "The one with the least magicka.";
    case PredicateKind::MagickaHighest:
        return "The one with the most magicka.";
    case PredicateKind::ResistanceLowest:
        return "The one least resistant to that kind of damage.";
    case PredicateKind::ResistanceHighest:
        return "The one most resistant to that kind of damage.";
    case PredicateKind::HitType:
        return "Hits with that: a blade, a bow, a spell or a staff in hand, or anything that does that kind of damage.";
    case PredicateKind::HitBy:
        return "Hit with that kind of damage in the last few seconds.";
    case PredicateKind::Attacking:
        return "Attacking that member of the party: they are its combat target.";
    case PredicateKind::AttackedBy:
        return "Attacked by that member of the party: it is their combat target.";
    case PredicateKind::HealthLowest:
        return "The one with the least health.";
    case PredicateKind::HealthHighest:
        return "The one with the most health.";
    case PredicateKind::ArmorLowest:
        return "The least armoured one.";
    case PredicateKind::ArmorHighest:
        return "The best armoured one.";
    case PredicateKind::SummonNone:
        return "Commands no summon or raised corpse right now.";
    case PredicateKind::SummonActive:
        return "Commands a summon or a raised corpse right now.";
    case PredicateKind::WeaponChargeNeeded:
        return "An enchanted weapon in hand cannot pay for one more hit.";
    case PredicateKind::WeaponPoisonNone:
        return "A weapon in hand takes a poison and has none on it.";
    case PredicateKind::WeaponPoisonActive:
        return "A weapon in hand has a poison on it.";
    case PredicateKind::CorpseNone:
        return "No corpse nearby that the rule's spell could raise.";
    case PredicateKind::LevelHighest:
        return "The nearby corpse of the highest level the rule's spell can raise.";
    case PredicateKind::LevelLowest:
        return "The nearby corpse of the lowest level the rule's spell can raise.";
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
    switch (v)
    {
    case ActionKind::None:
        return "Nothing.";
    case ActionKind::DrinkStrongest:
        return "Drink the strongest potion with this effect.";
    case ActionKind::DrinkWeakest:
        return "Drink the weakest potion with this effect.";
    case ActionKind::ChargeStrongestSoulGem:
        return "Recharge the weapon in hand that needs it with the largest soul gem that would not overfill it.";
    case ActionKind::ChargeWeakestSoulGem:
        return "Recharge the weapon in hand that needs it with the smallest soul gem carried.";
    case ActionKind::ChargeSoulGem:
        return "Recharge the weapon in hand that needs it with this soul gem.";
    case ActionKind::ApplyStrongest:
        return "Put the strongest poison with this effect on the weapon in hand.";
    case ActionKind::ApplyWeakest:
        return "Put the weakest poison with this effect on the weapon in hand.";
    case ActionKind::ApplyAny:
        return "Apply any poison";
    case ActionKind::DrinkAny:
        return "Drink a potion that applies a buff";
    case ActionKind::ApplyPoison:
        return "Put this poison on the weapon in hand.";
    case ActionKind::DrinkPotion:
        return "Drink this potion.";
    case ActionKind::EatStrongestFood:
        return "Eat the strongest food with this effect.";
    case ActionKind::EatWeakestFood:
        return "Eat the weakest food with this effect.";
    case ActionKind::EatStrongestIngredient:
        return "Eat the strongest ingredient with this effect.";
    case ActionKind::EatWeakestIngredient:
        return "Eat the weakest ingredient with this effect.";
    case ActionKind::EatFood:
        return "Eat this food.";
    case ActionKind::EatIngredient:
        return "Eat this ingredient.";
    case ActionKind::CastSpell:
        return "Cast this spell now.";
    case ActionKind::UsePower:
        return "Use this power now.";
    case ActionKind::Shout:
        return "Shout this now.";
    case ActionKind::UseScroll:
        return "Read this scroll now: cast from a hand, no magicka, the scroll spent.";
    case ActionKind::EquipWeapon:
        return "Hold this in that hand until another rule or the Inventory tab lets go.";
    case ActionKind::EquipSpell:
        return "Ready this spell in that hand until another rule or the Magic tab lets go.";
    case ActionKind::EquipArrows:
        return "Use this ammunition until another rule or the Inventory tab lets go.";
    case ActionKind::EquipStrongestArrows:
        return "Use the hardest-hitting arrows.";
    case ActionKind::EquipWeakestArrows:
        return "Use the weakest arrows.";
    case ActionKind::EquipArmor:
        return "Wear this until another rule or the Inventory tab lets go.";
    case ActionKind::Attack:
        return "Attack them: make them the combat target, and fight however the follower fights. Nothing happens "
               "if they already are.";
    case ActionKind::PowerAttack:
        return "One power attack at them with what is in hand -- a blade, a two-hander, both hands, the fists -- "
               "pointing the follower at them first if need be. Needs the stamina it costs.";
    case ActionKind::Bash:
        return "One bash at them with what blocks -- the shield or torch, or the weapon in the right hand with the "
               "left hand empty: the interrupt against a caster. Pointing the follower at them first if need be. "
               "Needs the stamina it costs.";
    case ActionKind::PowerBash:
        return "One power bash at them with what blocks -- the shield or torch, or the weapon in the right hand "
               "with the left hand empty: the hardest stagger of any blow. Pointing the follower at them first if "
               "need be. Needs the stamina it costs.";
    default:
        return "";
    }
}

} // namespace ft
