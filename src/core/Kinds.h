#pragma once
// The kinds a condition can name beside its number: a status an actor is
// in, a kind of damage. Pure, like everything in core; the game side reads
// the actor and sets them, the evaluator only compares them.

#include <cstdint>

namespace ft
{

// What an actor is in the middle of, read off the actor each tick. One bit
// each in ActorTraits::status. dev/CONDITIONS.md 2 says what each reads.
enum class StatusKind : std::uint8_t
{
    Poisoned,
    Burning,
    Frostbitten,
    Shocked,
    Paralysed,
    Staggered,
    Fleeing,
    BleedingOut,
    Invisible,
    Ethereal,
    Blocking,
    Casting,
    Sneaking,
    // A disease running: an effect whose spell is of the Disease type.
    // About anyone; the idle list's cure rule is what it is for.
    Diseased,
    // Losing health to a wound: the axe perks' bleeding, a mod's. Not
    // Bleeding out, which is being down.
    Bleeding,

    COUNT
};

[[nodiscard]] constexpr std::uint32_t Bit(StatusKind kind) noexcept
{
    return 1u << static_cast<std::uint32_t>(kind);
}

// What kind of being an actor is, for the Type condition: four groups, each
// with a head that means any of its members, the members contiguous after
// it. The peoples by race; the creatures by the engine's own actor-type
// keywords where it has one and by race where it does not (dev/CONDITIONS.md
// 2a). One bit each in ActorTraits::kinds; the game side sets the bits it
// reads, a head's own bit included where a being is of the group and of no
// member (the Elder race is a man; a hagraven is a creature). Overlap is by
// design: a vampire Nord is a Nord, a Vampire and Undead; a Falmer is an
// Elf and a Creature, as it is to the engine and to Wuuthrad both.
enum class TypeKind : std::uint8_t
{
    Man,
    Breton,
    Imperial,
    Nord,
    Redguard,
    Elf,
    DarkElf,
    Falmer,
    HighElf,
    SnowElf,
    WoodElf,
    Beast,
    Argonian,
    Khajiit,
    Orc,
    Creature,
    Animal,
    Automaton,
    Daedra,
    Dragon,
    Giant,
    Spriggan,
    Troll,
    Undead,
    Vampire,
    Werewolf,

    COUNT
};

[[nodiscard]] constexpr std::uint32_t Bit(TypeKind kind) noexcept
{
    return 1u << static_cast<std::uint32_t>(kind);
}

// The head of the group a kind belongs to; a head is its own.
[[nodiscard]] constexpr TypeKind GroupOf(TypeKind kind) noexcept
{
    if (kind >= TypeKind::Creature)
        return TypeKind::Creature;
    if (kind >= TypeKind::Beast)
        return TypeKind::Beast;
    if (kind >= TypeKind::Elf)
        return TypeKind::Elf;
    return TypeKind::Man;
}

[[nodiscard]] constexpr bool IsGroupHead(TypeKind kind) noexcept
{
    return GroupOf(kind) == kind;
}

// The bits of a head's members, the head's own included.
[[nodiscard]] constexpr std::uint32_t GroupBits(TypeKind head) noexcept
{
    std::uint32_t bits = 0;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(TypeKind::COUNT); ++i)
        if (GroupOf(static_cast<TypeKind>(i)) == head)
            bits |= 1u << i;
    return bits;
}

// A kind of damage: what a resistance is against, what an attack was made
// with. How it arrived first -- a blow, an arrow or bolt, a spell of any
// kind -- then what a spell was. A Fire hit is Magic as well; an arrow is
// Ranged and not Melee. Nothing resists a blow or an arrow but armour,
// which is its own condition. Any is for Hit by alone: hit with anything at
// all, inside the window. It is not offered as a hit TYPE -- hands with
// nothing in them read as Melee, so every actor hits with something and the
// condition would be true of everyone (IsDamageKindValidFor). Disease is not
// here: nothing attacks with it, and only the player has a resistance worth
// asking about. The order is the menu's.
enum class DamageKind : std::uint8_t
{
    Melee,
    Ranged,
    Magic,
    Fire,
    Frost,
    Shock,
    Poison,
    Any,

    COUNT
};

[[nodiscard]] constexpr std::uint8_t Bit(DamageKind kind) noexcept
{
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind));
}

// Where an actor is, for the Location condition (dev/CONDITIONS.md 2c): in
// a player home, inside or out -- the three first, in that order -- then
// by group. Inside or out is the cell's; a hold is the location record the
// rule names (Rule::conditionForm); every other kind is a keyword the
// location or one it lies in carries (game/Places.cpp says which). One bit
// each in Snapshot::places, Hold aside: Snapshot::hold. Exterior is not
// read as Interior's opposite: an actor with no cell under them is
// neither.
enum class LocationKind : std::uint8_t
{
    Home,
    Interior,
    Exterior,
    Building, // the group's Any
    Castle,
    Guild,
    House,
    Inn,
    Store,
    Temple,
    Cave, // a group of one, as asked: which cave is which is who holds it
    // Who holds a dungeon, indoors or out, apart from what it is built as.
    Dungeon, // the group's Any
    AnimalDen,
    BanditCamp,
    DragonLair,
    DragonPriestLair,
    DraugrCrypt,
    FalmerHive,
    ForswornCamp,
    GiantCamp,
    HagravenNest,
    RieklingCamp,
    SprigganGrove,
    VampireLair,
    WarlockLair,
    WerebearLair,
    WerewolfLair,
    Fort, // a group of one: no kinds of fort in the keywords
    Hold,
    Ruin, // the group's Any
    DwarvenRuin,
    NordicRuin,
    Settlement, // the group's Any
    City,
    Town,
    OrcStronghold,

    COUNT
};

static_assert(static_cast<unsigned>(LocationKind::COUNT) <= 64);

[[nodiscard]] constexpr std::uint64_t Bit(LocationKind kind) noexcept
{
    return std::uint64_t{1} << static_cast<unsigned>(kind);
}

// The headings of the Location menu after the first three, each a group of
// kinds. None is the first three.
enum class LocationGroup : std::uint8_t
{
    None,
    Building,
    Cave,
    Dungeon,
    Fort,
    Hold,
    Ruin,
    Settlement,

    COUNT
};

[[nodiscard]] constexpr LocationGroup GroupOf(LocationKind kind) noexcept
{
    using K = LocationKind;
    using G = LocationGroup;
    if (kind >= K::Settlement)
        return G::Settlement;
    if (kind >= K::Ruin)
        return G::Ruin;
    if (kind == K::Hold)
        return G::Hold;
    if (kind == K::Fort)
        return G::Fort;
    if (kind >= K::Dungeon)
        return G::Dungeon;
    if (kind >= K::Cave)
        return G::Cave;
    if (kind >= K::Building)
        return G::Building;
    return G::None;
}

// The kind a group's Any is, its first; none for the first three, and for
// Hold, whose kind is one hold named.
[[nodiscard]] constexpr bool IsGroupAny(LocationKind kind) noexcept
{
    using K = LocationKind;
    return kind == K::Building || kind == K::Cave || kind == K::Dungeon || kind == K::Fort || kind == K::Ruin ||
           kind == K::Settlement;
}

// Which consumable a consume action names, and which each carried one is.
// The snapshot tags every carried consumable with one, so a hand-edited
// profile cannot drink a cabbage: the form has to be carried AS that kind.
enum class ConsumableKind : std::uint8_t
{
    Potion,
    Food,
    Ingredient,
    Poison,  // applied to the weapon in hand, not drunk
    SoulGem, // spent into the weapon in hand's enchantment
};

} // namespace ft
