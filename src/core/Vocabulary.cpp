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

constexpr std::array<Entry<SubjectKind>, 5> kSubjects{{
    {SubjectKind::Self, "self", "Self"},
    {SubjectKind::Player, "player", "Player"},
    {SubjectKind::Ally, "ally", "Ally"},
    {SubjectKind::Enemy, "enemy", "Enemy"},
    {SubjectKind::CurrentTarget, "current-target", "Target"},
}};

constexpr std::array<Entry<PredicateKind>, 21> kPredicates{{
    {PredicateKind::Any, "any", "Any"}, // Dragon Age's word: "Enemy: Any", "Self: Any"
    {PredicateKind::HealthPctBelow, "health-pct-below", "Health"},
    {PredicateKind::StaminaPctBelow, "stamina-pct-below", "Stamina"},
    {PredicateKind::MagickaPctBelow, "magicka-pct-below", "Magicka"},
    {PredicateKind::InBleedout, "in-bleedout", "Bleeding out"},
    {PredicateKind::InCombat, "in-combat", "In combat"},
    {PredicateKind::CombatBegins, "combat-begins", "Combat begins"},
    {PredicateKind::CombatEnds, "combat-ends", "Combat ends"},
    {PredicateKind::WithinDistance, "within-distance", "Distance"},
    {PredicateKind::CountAtLeast, "count-at-least", "Count"},
    {PredicateKind::Status, "status", "Status"},
    {PredicateKind::Armor, "armor", "Armor"},
    {PredicateKind::Resistance, "resistance", "Resistance"},
    {PredicateKind::AttackedBy, "attacked-by", "Attacked by"},
    {PredicateKind::HealthLowest, "health-lowest", "Health lowest"},
    {PredicateKind::HealthHighest, "health-highest", "Health highest"},
    {PredicateKind::ArmorLowest, "armor-lowest", "Armor lowest"},
    {PredicateKind::ArmorHighest, "armor-highest", "Armor highest"},
    {PredicateKind::HealthPctAbove, "health-pct-above", "Health"},
    {PredicateKind::StaminaPctAbove, "stamina-pct-above", "Stamina"},
    {PredicateKind::MagickaPctAbove, "magicka-pct-above", "Magicka"},
}};

constexpr std::array<Entry<ActionTargetKind>, 5> kActionTargets{{
    {ActionTargetKind::ConditionSubject, "condition-subject", "Whoever matched"},
    {ActionTargetKind::Self, "self", "Self"},
    {ActionTargetKind::Player, "player", "Player"},
    {ActionTargetKind::CurrentTarget, "current-target", "Target"},
    {ActionTargetKind::Attacker, "attacker", "Their attacker"},
}};

constexpr std::array<Entry<ActionKind>, 13> kActions{{
    // The potion slugs name the SELECTION POLICY, not just the item type,
    // because that is part of the behaviour a profile is asking for. It also
    // leaves room: "drink-health-potion-weakest" -- don't burn a strong potion
    // on a scratch -- becomes a new value rather than a breaking change to an
    // existing one.
    {ActionKind::None, "none", "None"},
    {ActionKind::DrinkHealthPotion, "drink-strongest-health-potion", "Drink strongest health potion"},
    {ActionKind::DrinkMagickaPotion, "drink-strongest-magicka-potion", "Drink strongest magicka potion"},
    {ActionKind::DrinkStaminaPotion, "drink-strongest-stamina-potion", "Drink strongest stamina potion"},
    {ActionKind::DrinkPotion, "drink-potion", "Drink potion"},
    {ActionKind::CastSpell, "cast-spell", "Cast spell"},
    {ActionKind::EquipWeapon, "equip-weapon", "Equip weapon"},
    {ActionKind::EquipArrows, "equip-arrows", "Equip arrows"},
    {ActionKind::EquipSpell, "equip-spell", "Equip spell"},
    {ActionKind::EquipArmor, "equip-armor", "Equip armor"},
    {ActionKind::StopCombat, "stop-combat", "Stop fighting"},
    {ActionKind::Flee, "flee", "Flee"},
    {ActionKind::HoldPosition, "hold-position", "Hold position"},
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
constexpr std::array<Entry<StatusKind>, 14> kStatuses{{
    {StatusKind::Poisoned, "poisoned", "Poisoned"},
    {StatusKind::Burning, "burning", "Burning"},
    {StatusKind::Frostbitten, "frostbitten", "Frostbitten"},
    {StatusKind::Shocked, "shocked", "Shocked"},
    {StatusKind::Diseased, "diseased", "Diseased"},
    {StatusKind::Paralysed, "paralysed", "Paralysed"},
    {StatusKind::Staggered, "staggered", "Staggered"},
    {StatusKind::Fleeing, "fleeing", "Fleeing"},
    {StatusKind::BleedingOut, "bleeding-out", "Bleeding out"},
    {StatusKind::Invisible, "invisible", "Invisible"},
    {StatusKind::Ethereal, "ethereal", "Ethereal"},
    {StatusKind::Blocking, "blocking", "Blocking"},
    {StatusKind::Casting, "casting", "Casting"},
    {StatusKind::Sneaking, "sneaking", "Sneaking"},
}};

constexpr std::array<Entry<DamageKind>, 7> kDamageKinds{{
    {DamageKind::Physical, "physical", "Physical"},
    {DamageKind::Magic, "magic", "Magic"},
    {DamageKind::Fire, "fire", "Fire"},
    {DamageKind::Frost, "frost", "Frost"},
    {DamageKind::Shock, "shock", "Shock"},
    {DamageKind::Poison, "poison", "Poison"},
    {DamageKind::Disease, "disease", "Disease"},
}};

constexpr std::array<Entry<ResistBand>, 4> kResistBands{{
    {ResistBand::Weak, "weak", "Weak"},
    {ResistBand::Normal, "normal", "Normal"},
    {ResistBand::High, "high", "High"},
    {ResistBand::Immune, "immune", "Immune"},
}};

constexpr std::array<Entry<ArmorBand>, 3> kArmorBands{{
    {ArmorBand::Low, "low", "Low"},
    {ArmorBand::Medium, "medium", "Medium"},
    {ArmorBand::High, "high", "High"},
}};

// Every enumerator must appear in its table, or a rule would serialise as
// "Unknown" and fail to load back. Cheap to assert, impossible to forget.
static_assert(kSubjects.size() == static_cast<std::size_t>(SubjectKind::COUNT));
static_assert(kPredicates.size() == static_cast<std::size_t>(PredicateKind::COUNT));
static_assert(kActionTargets.size() == static_cast<std::size_t>(ActionTargetKind::COUNT));
static_assert(kActions.size() == static_cast<std::size_t>(ActionKind::COUNT));
static_assert(kStatuses.size() == static_cast<std::size_t>(StatusKind::COUNT));
static_assert(kArmorBands.size() == static_cast<std::size_t>(ArmorBand::COUNT));
static_assert(kDamageKinds.size() == static_cast<std::size_t>(DamageKind::COUNT));
static_assert(kResistBands.size() == static_cast<std::size_t>(ResistBand::COUNT));

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
std::string_view DisplayName(ArmorBand v) noexcept
{
    return LookupDisplay(kArmorBands, v);
}
std::string_view DisplayName(DamageKind v) noexcept
{
    return LookupDisplay(kDamageKinds, v);
}
std::string_view DisplayName(ResistBand v) noexcept
{
    return LookupDisplay(kResistBands, v);
}

ArgumentKind ArgumentFor(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthPctBelow:
    case PredicateKind::MagickaPctBelow:
    case PredicateKind::StaminaPctBelow:
    case PredicateKind::HealthPctAbove:
    case PredicateKind::MagickaPctAbove:
    case PredicateKind::StaminaPctAbove:
        return ArgumentKind::Percent;

    case PredicateKind::WithinDistance:
        return ArgumentKind::Distance;

    case PredicateKind::CountAtLeast:
        return ArgumentKind::Count;

    case PredicateKind::Armor:
        return ArgumentKind::ArmorBand;
    case PredicateKind::Resistance:
        return ArgumentKind::ResistBand;

    case PredicateKind::Any:
    case PredicateKind::InBleedout:
    case PredicateKind::InCombat:
        return ArgumentKind::None;

    default:
        return ArgumentKind::None;
    }
}

std::string_view Describe(PredicateKind v) noexcept
{
    // One short sentence each: these are tooltips.
    switch (v)
    {
    case PredicateKind::Any:
        return "Always true.";
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
    case PredicateKind::InBleedout:
        return "Down and dying, not dead.";
    case PredicateKind::InCombat:
        return "Fighting something.";
    case PredicateKind::CombatBegins:
        return "A fight has just begun.";
    case PredicateKind::CombatEnds:
        return "A fight has just ended; no other condition holds on that pass.";
    case PredicateKind::WithinDistance:
        return "Closer than this many units.";
    case PredicateKind::CountAtLeast:
        return "At least this many of them.";
    case PredicateKind::Status:
        return "In this state right now.";
    case PredicateKind::Armor:
        return "How much of a blow the armour turns away: under a quarter, up to half, or more.";
    case PredicateKind::Resistance:
        return "Resistance to that kind of damage: a weakness, none to speak of, half or more, or immune.";
    case PredicateKind::AttackedBy:
        return "Hit with that kind of damage in the last few seconds.";
    case PredicateKind::HealthLowest:
        return "The one with the least health.";
    case PredicateKind::HealthHighest:
        return "The one with the most health.";
    case PredicateKind::ArmorLowest:
        return "The least armoured one.";
    case PredicateKind::ArmorHighest:
        return "The best armoured one.";
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
    case ActionKind::DrinkHealthPotion:
        return "Drink the strongest healing potion carried.";
    case ActionKind::DrinkMagickaPotion:
        return "Drink the strongest magicka potion carried.";
    case ActionKind::DrinkStaminaPotion:
        return "Drink the strongest stamina potion carried.";
    case ActionKind::DrinkPotion:
        return "Drink this potion.";
    case ActionKind::CastSpell:
        return "Cast this spell now.";
    case ActionKind::EquipWeapon:
        return "Hold this in that hand until another rule or the Inventory tab lets go.";
    case ActionKind::EquipSpell:
        return "Ready this spell in that hand until another rule or the Magic tab lets go.";
    case ActionKind::EquipArrows:
        return "Use this ammunition until another rule or the Inventory tab lets go.";
    case ActionKind::EquipArmor:
        return "Wear this until another rule or the Inventory tab lets go.";
    case ActionKind::StopCombat:
        return "Break off the fight.";
    case ActionKind::Flee:
        return "Retreat from the fight.";
    case ActionKind::HoldPosition:
        return "Stay put.";
    default:
        return "";
    }
}

} // namespace ft
