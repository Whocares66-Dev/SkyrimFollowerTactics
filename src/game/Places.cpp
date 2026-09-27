#include "game/Places.h"

#include "core/Vocabulary.h"
#include "core/Weather.h"
#include "game/Log.h"
#include "game/Util.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ft::game
{
namespace
{

using K = ft::LocationKind;

// Each kind by the keywords that mark it, any one of them, by editor ID:
// Skyrim.esm's, Dragonborn's for the riekling camp, one kind to several
// where the game marks the same thing two ways. Read with houseCARL in
// Nordic Souls, 2026-09-25 (dev/CONDITIONS.md 2c).
struct Source
{
    K kind;
    std::string_view keyword;
};
constexpr Source kSources[] = {
    {K::Home, "LocTypePlayerHouse"},
    // Any building the keywords mark: the kinds listed, and the three
    // left out of the list.
    {K::Building, "LocTypeCastle"},
    {K::Building, "LocTypeGuild"},
    {K::Building, "LocTypeHouse"},
    {K::Building, "LocTypeInn"},
    {K::Building, "LocTypeStore"},
    {K::Building, "LocTypeTemple"},
    {K::Building, "LocTypeDwelling"},
    {K::Building, "LocTypeBarracks"},
    {K::Building, "LocTypeJail"},
    {K::Castle, "LocTypeCastle"},
    {K::Guild, "LocTypeGuild"},
    {K::House, "LocTypeHouse"},
    {K::Inn, "LocTypeInn"},
    {K::Store, "LocTypeStore"},
    {K::Temple, "LocTypeTemple"},
    // Some ice caves carry only the ice keyword (Dragonborn's Frossel).
    {K::Cave, "LocSetCave"},
    {K::Cave, "LocSetCaveIce"},
    {K::Dungeon, "LocTypeDungeon"},
    {K::AnimalDen, "LocTypeAnimalDen"},
    {K::BanditCamp, "LocTypeBanditCamp"},
    {K::DragonLair, "LocTypeDragonLair"},
    {K::DragonPriestLair, "LocTypeDragonPriestLair"},
    {K::DraugrCrypt, "LocTypeDraugrCrypt"},
    {K::FalmerHive, "LocTypeFalmerHive"},
    {K::ForswornCamp, "LocTypeForswornCamp"},
    {K::GiantCamp, "LocTypeGiantCamp"},
    {K::HagravenNest, "LocTypeHagravenNest"},
    {K::RieklingCamp, "DLC2LocTypeRieklingCamp"},
    {K::SprigganGrove, "LocTypeSprigganGrove"},
    {K::VampireLair, "LocTypeVampireLair"},
    {K::WarlockLair, "LocTypeWarlockLair"},
    {K::WerebearLair, "LocTypeWerebearLair"},
    {K::WerewolfLair, "LocTypeWerewolfLair"},
    // The built one and the garrisoned one: Northwatch Keep has only the
    // first, Fort Greymoor only the second.
    {K::Fort, "LocSetMilitaryFort"},
    {K::Fort, "LocTypeMilitaryFort"},
    {K::Ruin, "LocSetNordicRuin"},
    {K::Ruin, "LocSetDwarvenRuin"},
    {K::Ruin, "LocTypeDwarvenAutomatons"},
    {K::DwarvenRuin, "LocSetDwarvenRuin"},
    {K::DwarvenRuin, "LocTypeDwarvenAutomatons"},
    {K::NordicRuin, "LocSetNordicRuin"},
    // Not LocTypeHabitation: it marks where anyone lives, Deekus Camp and
    // the Hearthfire homesteads among them, and a town it marks is marked
    // by one of these too, or lies in one that is.
    {K::Settlement, "LocTypeCity"},
    {K::Settlement, "LocTypeTown"},
    {K::Settlement, "LocTypeSettlement"},
    {K::Settlement, "LocTypeOrcStronghold"},
    {K::City, "LocTypeCity"},
    {K::Town, "LocTypeTown"},
    {K::OrcStronghold, "LocTypeOrcStronghold"},
};

// Every kind is read somewhere: by the cell, as the hold, or by a keyword.
constexpr bool EveryKindRead()
{
    for (unsigned i = 0; i < static_cast<unsigned>(K::COUNT); ++i)
    {
        const auto kind = static_cast<K>(i);
        bool read = kind == K::Interior || kind == K::Exterior || kind == K::Hold;
        for (const Source &source : kSources)
            read = read || source.kind == kind;
        if (!read)
            return false;
    }
    return true;
}
static_assert(EveryKindRead());

// A location dug into the ground, which a settlement it is filed under does
// not reach down into (ft::PlacesUp). The mines in towns carry the last
// only; the Midden, Cidhna Mine and Esbern's Vault carry none of them and
// read as their city.
constexpr std::string_view kDug[] = {"LocTypeDungeon", "LocTypeClearable", "LocTypeMine"};

struct Marks
{
    std::vector<std::pair<K, const RE::BGSKeyword *>> kinds;
    std::vector<const RE::BGSKeyword *> dug;
    const RE::BGSKeyword *hold{nullptr};
    // Every location with a worldspace of its own. Of the settlements, the
    // five walled cities: the worldspace is the town within the walls.
    std::vector<const RE::BGSLocation *> walled;
};

// Looked up once: keywords and worldspaces are not made or unmade after the
// data loads. A keyword not in the load order marks nothing, and says so
// once.
const Marks &Resolved()
{
    static const Marks k = [] {
        Marks out;
        for (const Source &source : kSources)
        {
            const std::string id(source.keyword);
            if (const auto *keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(id))
                out.kinds.emplace_back(source.kind, keyword);
            else
                log::sensors.warn("location: no keyword {} in the load order; {} reads without it", id,
                                  ft::WireName(source.kind));
        }
        for (const std::string_view dug : kDug)
        {
            const std::string id(dug);
            if (const auto *keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(id))
                out.dug.push_back(keyword);
            else
                log::sensors.warn("location: no keyword {} in the load order; a settlement reaches into what it marks",
                                  id);
        }
        out.hold = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("LocTypeHold");
        if (auto *data = RE::TESDataHandler::GetSingleton())
            for (const auto *world : data->GetFormArray<RE::TESWorldSpace>())
                if (world && world->location)
                    out.walled.push_back(world->location);
        log::sensors.debug("location: {} worldspace(s) with a location of their own", out.walled.size());
        return out;
    }();
    return k;
}

// A weather record as the sky blends it. Its precipitation bytes are read
// unsigned and scaled as the engine's IsRaining and IsSnowing conditions
// scale them (1.6.1170, 2026-09-26); CommonLib types them int8_t.
std::optional<ft::SkyWeather> Blended(const RE::TESWeather *weather)
{
    if (!weather)
        return std::nullopt;
    using F = RE::TESWeather::WeatherDataFlag;
    using W = ft::WeatherKind;
    const auto &data = weather->data;
    ft::SkyWeather out;
    for (const auto &[flag, kind] :
         {std::pair{F::kPleasant, W::Pleasant}, {F::kCloudy, W::Cloudy}, {F::kRainy, W::Rain}, {F::kSnow, W::Snow}})
        if (data.flags.any(flag))
            out.classes |= ft::Bit(kind);
    out.precipitationBegins =
        static_cast<float>(static_cast<std::uint8_t>(data.precipitationBeginFadeIn)) * 0.0039176475f;
    out.precipitationEnds =
        static_cast<float>(static_cast<std::uint8_t>(data.precipitationEndFadeOut)) * 0.003917647f + 0.001f;
    return out;
}

struct Seen
{
    std::uint64_t places{0};
    std::uint32_t hold{0};
    std::uint8_t weather{0};
    ft::TimeKind time{ft::TimeKind::Morning};
    bool operator==(const Seen &) const = default;
};

std::mutex g_placesMutex;
std::unordered_map<RE::FormID, Seen> g_places;

template <typename Kind, typename Bits> std::string Listed(Bits bits)
{
    std::string out;
    for (unsigned i = 0; i < static_cast<unsigned>(Kind::COUNT); ++i)
        if ((bits & ft::Bit(static_cast<Kind>(i))) != 0)
            out += (out.empty() ? "" : ", ") + std::string(ft::WireName(static_cast<Kind>(i)));
    return out;
}

// With the log at debug, each change of where an actor is, of the weather
// over them and of the part of the day, as the statuses' changes are:
// without it a Location, Weather or Time rule that never holds cannot tell
// a place not marked from one never reached, or the climate's sunset from
// the one expected.
void LogPlaceChanges(RE::Actor *actor, const Seen &seen)
{
    {
        std::scoped_lock lock(g_placesMutex);
        auto &last = g_places[actor->GetFormID()];
        if (last == seen)
            return;
        last = seen;
    }
    const std::string at = Listed<K>(seen.places);
    const std::string weather = Listed<ft::WeatherKind>(seen.weather);
    const auto *holdForm = seen.hold ? RE::TESForm::LookupByID(seen.hold) : nullptr;
    log::sensors.debug("{}: now at {}; hold {}; weather {}; {}", Describe(actor), at.empty() ? "nowhere known" : at,
                       holdForm ? log::NameOf(holdForm) : "none", weather.empty() ? "none" : weather,
                       ft::WireName(seen.time));
}

} // namespace

void ReadPlaces(RE::Actor *actor, ft::Snapshot &s)
{
    s.places = 0;
    s.hold = 0;
    if (const auto *cell = actor->GetParentCell())
        s.places |= ft::Bit(cell->IsInteriorCell() ? K::Interior : K::Exterior);
    // In Breezehome the actor is in a player house, in a city and in
    // Whiterun Hold. The nearest hold is theirs, however deep they are.
    const Marks &k = Resolved();
    // Out of doors, where the worldspace lies: a walled city's own, or one
    // within it (Dragonsreach's, Windhelm's Pit), is inside the walls.
    // Tamriel has no location, so before a gate this is nothing.
    const bool outdoors = s.At(K::Exterior);
    const RE::TESWorldSpace *world = outdoors ? actor->GetWorldspace() : nullptr;
    const RE::BGSLocation *worldAt = world ? world->location : nullptr;
    const auto within = [worldAt](const RE::BGSLocation *city) {
        for (const RE::BGSLocation *at = worldAt; at; at = at->parentLoc)
            if (at == city)
                return true;
        return false;
    };
    std::vector<ft::ChainLink> chain;
    for (const RE::BGSLocation *at = actor->GetCurrentLocation(); at; at = at->parentLoc)
    {
        ft::ChainLink &link = chain.emplace_back();
        for (const auto &[kind, keyword] : k.kinds)
            if (at->HasKeyword(keyword))
                link.kinds |= ft::Bit(kind);
        link.dug = std::ranges::any_of(k.dug, [at](const RE::BGSKeyword *dug) { return at->HasKeyword(dug); });
        link.outsideWalls = outdoors && std::ranges::find(k.walled, at) != k.walled.end() && !within(at);
        if (s.hold == 0 && k.hold && at->HasKeyword(k.hold))
            s.hold = at->GetFormID();
    }
    s.places |= ft::PlacesUp(chain);
    // The weather out of doors only. Indoors the sky keeps the weather
    // outside, and a cell flagged to show the sky -- Breezehome, the inns,
    // many caves -- is under a roof all the same.
    s.weather = 0;
    if (const auto *sky = RE::Sky::GetSingleton(); sky && s.At(K::Exterior))
    {
        ft::Sky read;
        read.current = Blended(sky->currentWeather);
        read.last = Blended(sky->lastWeather);
        read.progress = sky->currentWeatherPct;
        s.weather = ft::WeatherOf(read);
    }
    // The part of the day by the climate's sun, indoors too.
    if (const auto *calendar = RE::Calendar::GetSingleton())
        s.timeOfDay = ft::TimeOfDay(calendar->GetHour(), SunNow());
    LogPlaceChanges(actor, {s.places, s.hold, s.weather, s.timeOfDay});
}

ft::SunTimes SunNow()
{
    // The getters Sky::IsDaytime reads, cached from the last climate the
    // sky had when it has none.
    auto *sky = RE::Sky::GetSingleton();
    if (!sky)
        return {};
    return {sky->GetSunriseBegin(), sky->GetSunsetBegin(), sky->GetSunsetEnd()};
}

const std::vector<HoldPick> &Holds()
{
    static const std::vector<HoldPick> holds = [] {
        std::vector<HoldPick> out;
        const auto *keyword = Resolved().hold;
        auto *data = RE::TESDataHandler::GetSingleton();
        if (!keyword || !data)
            return out;
        for (const auto *location : data->GetFormArray<RE::BGSLocation>())
        {
            if (!location || !location->HasKeyword(keyword))
                continue;
            if (std::string name = HoldName(location->GetFormID()); !name.empty())
                out.push_back({location->GetFormID(), std::move(name)});
        }
        log::sensors.debug("location: {} hold(s) in the load order", out.size());
        return out;
    }();
    return holds;
}

std::string HoldName(std::uint32_t form)
{
    const auto *location = form ? RE::TESForm::LookupByID<RE::BGSLocation>(form) : nullptr;
    const char *full = location ? location->GetFullName() : nullptr;
    std::string name = full ? full : "";
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

void ForgetPlaces()
{
    std::scoped_lock lock(g_placesMutex);
    g_places.clear();
}

} // namespace ft::game
