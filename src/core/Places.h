#pragma once

#include "Kinds.h"

#include <optional>
#include <string_view>

namespace ft
{

// Physical enclosure, independent of the engine's Interior/Exterior flags.
// Blackreach is enclosed even though its cells are exteriors.
enum class PlaceContext : std::uint8_t
{
    NoCell,
    Enclosed,
    OpenAir
};

[[nodiscard]] constexpr PlaceContext PlaceContextOf(bool interior, bool exterior, bool roofedWorld,
                                                    std::optional<bool> reviewedWorld = {}) noexcept
{
    if (interior)
        return PlaceContext::Enclosed;
    if (!exterior)
        return PlaceContext::NoCell;
    return reviewedWorld.value_or(roofedWorld) ? PlaceContext::Enclosed : PlaceContext::OpenAir;
}

struct SiteKinds
{
    LocationKind dungeon;
    LocationKind camp;
};

inline constexpr SiteKinds kSiteKinds[] = {
    {LocationKind::AnimalDen, LocationKind::AnimalTerritory},
    {LocationKind::BanditHideout, LocationKind::BanditCamp},
    {LocationKind::DragonLair, LocationKind::DragonRoost},
    {LocationKind::DragonPriestLair, LocationKind::DragonPriestCamp},
    {LocationKind::DraugrCrypt, LocationKind::DraugrCamp},
    {LocationKind::FalmerHive, LocationKind::FalmerCamp},
    {LocationKind::ForswornHideout, LocationKind::ForswornCamp},
    {LocationKind::GiantDen, LocationKind::GiantCamp},
    {LocationKind::HagravenNest, LocationKind::HagravenRoost},
    {LocationKind::RieklingDen, LocationKind::RieklingCamp},
    {LocationKind::SprigganDen, LocationKind::SprigganGrove},
    {LocationKind::VampireLair, LocationKind::VampireCamp},
    {LocationKind::WarlockLair, LocationKind::WarlockCamp},
    {LocationKind::WerebearLair, LocationKind::WerebearCamp},
    {LocationKind::WerewolfLair, LocationKind::WerewolfCamp},
};

[[nodiscard]] constexpr std::uint64_t SiteBits() noexcept
{
    std::uint64_t bits = Bit(LocationKind::Dungeon) | Bit(LocationKind::Camp);
    for (const auto &pair : kSiteKinds)
        bits |= Bit(pair.dungeon) | Bit(pair.camp);
    return bits;
}

[[nodiscard]] constexpr std::uint64_t SiteEvidenceBits() noexcept
{
    using K = LocationKind;
    return SiteBits() | Bit(K::Cave) | Bit(K::Fort) | Bit(K::Ruin) | Bit(K::DwarvenRuin) | Bit(K::NordicRuin);
}

[[nodiscard]] constexpr bool IsDungeonSite(std::uint64_t kinds) noexcept
{
    return (kinds & SiteEvidenceBits()) != 0;
}

// Occupant keywords are evidence about a site, never evidence that it is
// indoors. All recognized sites receive the appropriate Any, even when
// Bethesda forgot LocTypeDungeon. Structural fort/ruin choices still work
// both inside and outside; Cave requires physical enclosure.
[[nodiscard]] constexpr std::uint64_t SitePlaces(std::uint64_t kinds, PlaceContext context) noexcept
{
    using K = LocationKind;
    std::uint64_t out = kinds & ~SiteBits();
    if (context != PlaceContext::Enclosed)
        out &= ~Bit(K::Cave);
    if (context == PlaceContext::NoCell || !IsDungeonSite(kinds))
        return out;
    const bool inside = context == PlaceContext::Enclosed;
    out |= Bit(inside ? K::Dungeon : K::Camp);
    for (const auto &pair : kSiteKinds)
        if (kinds & (Bit(pair.dungeon) | Bit(pair.camp)))
            out |= Bit(inside ? pair.dungeon : pair.camp);
    return out;
}

// Reviewed location corrections, not load-order indices or names to match.
// IDs and record names were read with houseCARL on 2026-10-06. The reasoning
// and additional examples are in dev/LOCATIONS.md. Unknown/mod-added sites
// use the keyword evidence plus physical enclosure above.
struct LocationCorrection
{
    std::string_view plugin;
    std::string_view name;
    std::uint64_t add;
    std::uint64_t remove;
    std::uint32_t localForm;
    bool dug;
};

inline constexpr LocationCorrection kLocationCorrections[] = {
    {"Skyrim.esm", "Guardian Stones", 0, SiteEvidenceBits(), 0x10FE43, false},
    {"Skyrim.esm", "Riften Ratway", Bit(LocationKind::Dungeon), 0, 0x03B871, true},
    {"Skyrim.esm", "The Midden", Bit(LocationKind::Dungeon), Bit(LocationKind::Building) | Bit(LocationKind::Guild),
     0x02BCEB, true},
    {"Skyrim.esm", "Markarth Ruins",
     Bit(LocationKind::Dungeon) | Bit(LocationKind::Ruin) | Bit(LocationKind::DwarvenRuin), 0, 0x0E2502, true},
    {"Skyrim.esm", "Esbern's Vault", Bit(LocationKind::Dungeon), 0, 0x022639, true},
    {"Skyrim.esm", "Cidhna Mine", Bit(LocationKind::Dungeon), Bit(LocationKind::Building), 0x018C91, true},
    {"Skyrim.esm", "Broken Fang Cave", Bit(LocationKind::Cave), Bit(LocationKind::Fort), 0x01914D, true},
    {"Skyrim.esm", "Nightcaller Temple", Bit(LocationKind::Temple) | Bit(LocationKind::Building),
     Bit(LocationKind::Fort), 0x0192B9, true},
};

[[nodiscard]] constexpr ChainLink CorrectPlace(ChainLink link, const LocationCorrection &correction) noexcept
{
    link.kinds = (link.kinds & ~correction.remove) | correction.add;
    link.dug = link.dug || correction.dug;
    return link;
}

// Lighting templates alone do not prove that a worldspace is underground.
struct WorldEnclosure
{
    std::string_view plugin;
    std::string_view name;
    std::uint32_t localForm;
    bool enclosed;
};
inline constexpr WorldEnclosure kWorldEnclosures[] = {
    {"Skyrim.esm", "Blackreach", 0x01EE62, true},
    {"Skyrim.esm", "Darkwater Pass", 0x02C965, true},
    {"Dawnguard.esm", "Volkihar Courtyard", 0x007202, false},
    {"Dawnguard.esm", "Soul Cairn", 0x001408, false},
};

} // namespace ft
