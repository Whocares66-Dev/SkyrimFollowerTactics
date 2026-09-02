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

constexpr std::array<Entry<PredicateKind>, 8> kPredicates{{
    {PredicateKind::Always, "always", "Always"},
    {PredicateKind::HealthPctBelow, "health-pct-below", "Health"},
    {PredicateKind::StaminaPctBelow, "stamina-pct-below", "Stamina"},
    {PredicateKind::MagickaPctBelow, "magicka-pct-below", "Magicka"},
    {PredicateKind::InBleedout, "in-bleedout", "Bleeding out"},
    {PredicateKind::InCombat, "in-combat", "In combat"},
    {PredicateKind::WithinDistance, "within-distance", "Distance"},
    {PredicateKind::CountAtLeast, "count-at-least", "Count"},
}};

constexpr std::array<Entry<ActionTargetKind>, 4> kActionTargets{{
    {ActionTargetKind::ConditionSubject, "condition-subject", "Whoever matched"},
    {ActionTargetKind::Self, "self", "Self"},
    {ActionTargetKind::Player, "player", "Player"},
    {ActionTargetKind::CurrentTarget, "current-target", "Target"},
}};

constexpr std::array<Entry<ActionKind>, 7> kActions{{
    // The potion slugs name the SELECTION POLICY, not just the item type,
    // because that is part of the behaviour a profile is asking for. It also
    // leaves room: "drink-health-potion-weakest" -- don't burn a strong potion
    // on a scratch -- becomes a new value rather than a breaking change to an
    // existing one.
    {ActionKind::None, "none", "Do nothing"},
    {ActionKind::DrinkHealthPotion, "drink-health-potion-strongest", "Drink health potion"},
    {ActionKind::DrinkMagickaPotion, "drink-magicka-potion-strongest", "Drink magicka potion"},
    {ActionKind::DrinkStaminaPotion, "drink-stamina-potion-strongest", "Drink stamina potion"},
    {ActionKind::StopCombat, "stop-combat", "Stop fighting"},
    {ActionKind::Flee, "flee", "Flee"},
    {ActionKind::HoldPosition, "hold-position", "Hold position"},
}};

// Every enumerator must appear in its table, or a rule would serialise as
// "Unknown" and fail to load back. Cheap to assert, impossible to forget.
static_assert(kSubjects.size() == static_cast<std::size_t>(SubjectKind::COUNT));
static_assert(kPredicates.size() == static_cast<std::size_t>(PredicateKind::COUNT));
static_assert(kActionTargets.size() == static_cast<std::size_t>(ActionTargetKind::COUNT));
static_assert(kActions.size() == static_cast<std::size_t>(ActionKind::COUNT));

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

ArgumentKind ArgumentFor(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthPctBelow:
    case PredicateKind::MagickaPctBelow:
    case PredicateKind::StaminaPctBelow:
        return ArgumentKind::Percent;

    case PredicateKind::WithinDistance:
        return ArgumentKind::Distance;

    case PredicateKind::CountAtLeast:
        return ArgumentKind::Count;

    case PredicateKind::Always:
    case PredicateKind::InBleedout:
    case PredicateKind::InCombat:
        return ArgumentKind::None;

    default:
        return ArgumentKind::None;
    }
}

std::string_view Describe(PredicateKind v) noexcept
{
    switch (v)
    {
    case PredicateKind::Always:
        return "Always true. Use it for a rule that should fire whenever it is reached.";
    case PredicateKind::HealthPctBelow:
        return "Health has fallen below this fraction of its maximum.";
    case PredicateKind::MagickaPctBelow:
        return "Magicka has fallen below this fraction of its maximum.";
    case PredicateKind::StaminaPctBelow:
        return "Stamina has fallen below this fraction of its maximum.";
    case PredicateKind::InBleedout:
        return "Down and dying, but not dead.";
    case PredicateKind::InCombat:
        return "Currently fighting something.";
    case PredicateKind::WithinDistance:
        return "Closer than this many game units.";
    case PredicateKind::CountAtLeast:
        return "There are at least this many of them.";
    default:
        return "";
    }
}

std::string_view Describe(ActionKind v) noexcept
{
    switch (v)
    {
    case ActionKind::None:
        return "Do nothing.";
    case ActionKind::DrinkHealthPotion:
        return "Drink the strongest healing potion carried.";
    case ActionKind::DrinkMagickaPotion:
        return "Drink the strongest magicka potion carried.";
    case ActionKind::DrinkStaminaPotion:
        return "Drink the strongest stamina potion carried.";
    case ActionKind::StopCombat:
        return "Break off the current fight.";
    case ActionKind::Flee:
        return "Retreat from the fight.";
    case ActionKind::HoldPosition:
        return "Stay put rather than closing on a target.";
    default:
        return "";
    }
}

} // namespace ft
