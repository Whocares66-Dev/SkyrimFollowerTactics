#include "game/Places.h"

#include "core/Vocabulary.h"
#include "game/Log.h"
#include "game/Util.h"

#include <cctype>
#include <mutex>
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
    {K::Settlement, "LocTypeHabitation"},
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

struct Keywords
{
    std::vector<std::pair<K, const RE::BGSKeyword *>> kinds;
    const RE::BGSKeyword *hold{nullptr};
};

// Looked up once: keywords are not made or unmade after the data loads. A
// keyword not in the load order marks nothing, and says so once.
const Keywords &Resolved()
{
    static const Keywords k = [] {
        Keywords out;
        for (const Source &source : kSources)
        {
            const std::string id(source.keyword);
            if (const auto *keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(id))
                out.kinds.emplace_back(source.kind, keyword);
            else
                log::sensors.warn("location: no keyword {} in the load order; {} reads without it", id,
                                  ft::WireName(source.kind));
        }
        out.hold = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("LocTypeHold");
        return out;
    }();
    return k;
}

std::mutex g_placesMutex;
std::unordered_map<RE::FormID, std::pair<std::uint64_t, std::uint32_t>> g_places;

// With the log at debug, each change of where an actor is, as the
// statuses' changes are: without it a Location rule that never holds
// cannot tell a place not marked from one never reached.
void LogPlaceChanges(RE::Actor *actor, std::uint64_t places, std::uint32_t hold)
{
    {
        std::scoped_lock lock(g_placesMutex);
        auto &last = g_places[actor->GetFormID()];
        if (last == std::pair{places, hold})
            return;
        last = {places, hold};
    }
    std::string at;
    for (unsigned i = 0; i < static_cast<unsigned>(K::COUNT); ++i)
        if ((places & ft::Bit(static_cast<K>(i))) != 0)
            at += (at.empty() ? "" : ", ") + std::string(ft::WireName(static_cast<K>(i)));
    const auto *holdForm = hold ? RE::TESForm::LookupByID(hold) : nullptr;
    log::sensors.debug("{}: now at {}; hold {}", Describe(actor), at.empty() ? "nowhere known" : at,
                       holdForm ? log::NameOf(holdForm) : "none");
}

} // namespace

void ReadPlaces(RE::Actor *actor, ft::Snapshot &s)
{
    s.places = 0;
    s.hold = 0;
    if (const auto *cell = actor->GetParentCell())
        s.places |= ft::Bit(cell->IsInteriorCell() ? K::Interior : K::Exterior);
    // A location does not carry the keywords of the one it lies in, so the
    // chain is walked: in Breezehome the actor is in a player house, in a
    // city and in Whiterun Hold. The nearest hold is theirs.
    const Keywords &k = Resolved();
    for (const RE::BGSLocation *at = actor->GetCurrentLocation(); at; at = at->parentLoc)
    {
        for (const auto &[kind, keyword] : k.kinds)
            if (at->HasKeyword(keyword))
                s.places |= ft::Bit(kind);
        if (s.hold == 0 && k.hold && at->HasKeyword(k.hold))
            s.hold = at->GetFormID();
    }
    LogPlaceChanges(actor, s.places, s.hold);
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
