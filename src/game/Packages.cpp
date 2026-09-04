#include "game/Packages.h"

#include "game/Util.h"

#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ft::game
{
namespace
{

std::array<RE::TESPackage *, kPackageSlots> g_slots{};
RE::TESFaction *g_faction = nullptr;
bool g_available = false;

// How long the AI gets to START the cast before the record is taken back.
// Measured: every cast that happened fired 0.65-2.2 s after arming; the ones
// that did not had not started by 4 s either. The window only costs anything
// in that second case -- it is how long she stands held and unreactive -- so
// it is set just above the slowest measured start.
constexpr double kArmWindowSeconds = 2.5;

// The rank that means "no request". Every slot condition is an equality
// against 0..7, so any other value fails them all.
constexpr std::int8_t kRankNone = -1;

void SetRank(RE::Actor *actor, std::int8_t rank, const char *why);

// The rank, as an owned resource.
//
// Setting a follower's rank is what makes her package's condition pass, and
// forgetting to unset it is the one mistake this file must not be able to
// make: she would pass that condition on every evaluation for the rest of the
// session. So the rank is held by an object, and the ONLY way to give a record
// back is to destroy that object. The destructor clears the rank and asks the
// AI to re-evaluate, so there is no release path that can skip either.
//
// What this does NOT guarantee is timing. Nothing in C++ ends the lease on its
// own; the tick does, on a signal or at the deadline. RAII makes the cleanup
// unskippable, the deadline makes it prompt.
class RankLease
{
  public:
    explicit RankLease(RE::Actor *actor, std::int8_t rank) : actor_(actor->GetHandle()), id_(actor->GetFormID())
    {
        SetRank(actor, rank, "leased");
    }

    RankLease(const RankLease &) = delete;
    RankLease &operator=(const RankLease &) = delete;

    ~RankLease()
    {
        if (!live_)
            return;
        auto actor = actor_.get();
        if (!actor)
            return; // gone; the stale-rank sweep covers her if she returns
        SetRank(actor.get(), kRankNone, "lease ended");

        // Without this she stays in the package until the AI's own next
        // evaluation, which after a completed cast can be a long time: that
        // was the "he healed and then froze" of the first successful run.
        actor->EvaluatePackage(/*immediate*/ true, /*resetAI*/ false);
    }

    // After a game load the handle may resolve to an unrelated actor. Abandon
    // rather than release: the ranks, if any survived in the save, are swept.
    void Abandon() noexcept
    {
        live_ = false;
    }

    [[nodiscard]] RE::NiPointer<RE::Actor> Actor() const
    {
        return actor_.get();
    }
    [[nodiscard]] std::uint32_t FormID() const noexcept
    {
        return id_;
    }

  private:
    RE::ActorHandle actor_;
    std::uint32_t id_;
    bool live_ = true;
};

// A slot is a package record and, while a cast is in flight, exactly one
// follower's lease on it. One holder per record, always: every input in the
// record -- spell today, target tomorrow -- is hers alone for as long as she
// holds it, so nothing that gets repointed later can be shared by accident.
// A slot with no lease is free, whatever spell it was last pointed at.
struct Slot
{
    std::uint32_t spell = kCanarySpellID; // what the record holds right now
    std::optional<RankLease> lease;
    double armedAt = 0.0;
    double until = 0.0;
    bool seenRunning = false;

    // A concentration stream: the fire event is its start, not a release.
    bool sustained = false;
    float sustain = 0.0f;   // how long it was asked to run
    bool streaming = false; // our fire event has been seen
    // Whom the stream is aimed at, so it can stop when they are dead.
    RE::ActorHandle target;

    // Set from the animation thread; read and cleared by the tick. The ONLY
    // things the sink writes. `fired`: our spell left her hand. `stopped`: a
    // CastStop arrived after that -- for a stream, its end.
    std::atomic<bool> fired{false};
    std::atomic<bool> stopped{false};

    [[nodiscard]] bool Busy() const noexcept
    {
        return lease.has_value();
    }
};
std::array<Slot, kPackageSlots> g_pool{};

// The release signals come from her animation graph.
//
// A UseMagic package does NOT complete after its cast (measured: still her
// current package four seconds later, with her standing idle), so the end of
// a cast has to be observed. The graph emits MRh_SpellFire_Event /
// MLh_SpellFire_Event when a spell leaves a hand, and CastStop when a cast
// ends. Both fire for EVERY spell she casts, her own combat spells included,
// so the sink reads which spell is equipped in the firing hand and flags the
// slot only for ours (the caster's currentSpell is already null by then).
// The tick does the releasing; the sink only sets flags.
class SpellFireSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
  public:
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent *ev,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent> *) override
    {
        if (!ev || !ev->holder || ev->tag.empty())
            return RE::BSEventNotifyControl::kContinue;

        const std::uint32_t who = ev->holder->GetFormID();
        const char *tag = ev->tag.c_str();
        const bool right = _stricmp(tag, "MRh_SpellFire_Event") == 0;
        const bool left = _stricmp(tag, "MLh_SpellFire_Event") == 0;
        const bool stop = _stricmp(tag, "CastStop") == 0;

        for (auto &slot : g_pool)
        {
            if (!slot.Busy() || slot.lease->FormID() != who)
                continue;
            // Every cast-related tag while a record is held, so the graph's
            // vocabulary is on record and a missing fire event is diagnosable.
            if (right || left || strstr(tag, "Cast") || strstr(tag, "Spell"))
                logger::info("  anim {:08X}: {}", who, tag);
            // A stream that has fired and now stops has ended, whether the
            // CastTime ran out or something interrupted it.
            if (stop && slot.fired.load(std::memory_order_relaxed))
                slot.stopped.store(true, std::memory_order_relaxed);
            if (!right && !left)
                continue;

            // Which spell just left that hand? The spell EQUIPPED in it: the
            // UseMagic procedure equips what it casts, and the caster's own
            // currentSpell is already null when this event arrives.
            auto *actor = const_cast<RE::TESObjectREFR *>(ev->holder)->As<RE::Actor>();
            const auto *spell =
                actor ? actor->GetActorRuntimeData()
                            .selectedSpells[right ? RE::Actor::SlotTypes::kRightHand : RE::Actor::SlotTypes::kLeftHand]
                      : nullptr;
            const std::uint32_t firedID = spell ? spell->GetFormID() : 0;
            const bool ours = firedID == slot.spell;
            logger::info("  anim {:08X}: {} hand fired {:08X} \"{}\" -- {}", who, right ? "right" : "left", firedID,
                         spell && spell->GetName() ? spell->GetName() : "?",
                         ours ? "OURS" : "the follower's own, ignored");
            if (ours)
                slot.fired.store(true, std::memory_order_relaxed);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
SpellFireSink g_fireSink;

// Followers the sink is registered on. Once per actor per session; the actor
// objects are new after a load, so ResetPackages clears this too.
std::unordered_set<std::uint32_t> g_sinked;

// Where the Spell form sits inside the package data, learned at load by
// finding the canary rather than hardcoded: BGSPackageDataTargetSelector is
// not mapped by CommonLibSSE, and a wrong write here corrupts a live game.
constexpr std::size_t kNotCalibrated = static_cast<std::size_t>(-1);
std::size_t g_spellOuter = kNotCalibrated;
std::size_t g_spellInner = kNotCalibrated;

// PackageTarget::targType values, from the PTDA type field. The record ships
// every slot's Target as Self, which is the canary for this input: the
// pointer at the same outer offset as the Spell input must lead to a
// PackageTarget whose type reads kSelf before anything is written through it.
// LEARNED from game data rather than assumed: the first version took Self =
// 5 from the record library's arm order and the engine read 6. Two authored
// records are the canaries: every one of our slots ships with Target = Self,
// so its type byte IS the Self value; Mercer's
// TG08BMercerCombatOverrideCastAtPlayer ships with Target = PlayerRef, so its
// type byte is the specific-reference value -- confirmed by its handle
// matching the player's. Nothing is written until both read consistently.
constexpr std::uint32_t kMercerCastAtPlayerID = 0x000FDBC3;

// The CastTime inputs: how long the UseMagic procedure holds a CONCENTRATION
// stream. Two floats, authored 0.5 and 1.0 in every slot -- the canary for
// their layout, which is the named package data's 8-byte slot at +08 (the
// Spell input's hex dump showed the same slot empty and its pointer at +10).
constexpr float kAuthoredCastTimeMin = 0.5f;
constexpr float kAuthoredCastTimeMax = 1.0f;
std::size_t g_castTimeOffset = kNotCalibrated; // found by scanning for the authored values
bool g_castTimeCalibrated = false;

// How long a stream runs when the rule does not say. Long enough to matter
// against a bear, short enough that the AI has her back for the next turn.
// (Later, perhaps a random length within a range.)
constexpr float kDefaultSustainSeconds = 3.0f;
std::int8_t g_typeSpecificReference = -1;
std::int8_t g_typeSelf = -1;
bool g_targetCalibrated = false;

bool LooksLikePointer(std::uintptr_t value)
{
    return value > 0x10000 && value < 0x7FFFFFFFFFFFULL && (value % 8) == 0;
}

std::string HexDump(const void *base, std::size_t bytes)
{
    const auto *p = static_cast<const std::uint8_t *>(base);
    std::string out;
    char buf[8]{};
    for (std::size_t i = 0; i < bytes; ++i)
    {
        if (i > 0 && (i % 8) == 0)
            out += ' ';
        std::snprintf(buf, sizeof(buf), "%02X", p[i]);
        out += buf;
    }
    return out;
}

// Case-INsensitive: the template spells it "SPELL".
bool SearchNameMap(RE::TESCustomPackageData *data, const char *wanted, std::int8_t &uid)
{
    if (!data || !data->nameMap)
        return false;

    for (const auto &entry : data->nameMap->nameMap)
    {
        if (entry.name.empty())
            continue;
        if (_stricmp(entry.name.c_str(), wanted) == 0)
        {
            uid = entry.uid;
            return true;
        }
    }
    return false;
}

void LogNameMap(RE::TESCustomPackageData *data, const char *which)
{
    if (!data || !data->nameMap)
    {
        logger::info("  {} name map: absent", which);
        return;
    }
    std::string names;
    for (const auto &entry : data->nameMap->nameMap)
    {
        if (entry.name.empty())
            continue;
        if (!names.empty())
            names += ", ";
        names += entry.name.c_str();
        names += "=" + std::to_string(static_cast<int>(entry.uid));
    }
    logger::info("  {} name map: {}", which, names.empty() ? "(empty)" : names);
}

// The names live on the TEMPLATE, not on packages built from it.
bool FindInputUID(RE::TESCustomPackageData *data, const char *wanted, std::int8_t &uid)
{
    if (SearchNameMap(data, wanted, uid))
        return true;

    if (data && data->templateParent)
    {
        auto *parent = skyrim_cast<RE::TESCustomPackageData *>(data->templateParent->data);
        LogNameMap(parent, "template");
        if (SearchNameMap(parent, wanted, uid))
            return true;
    }
    return false;
}

RE::IPackageData *InputByUID(RE::TESCustomPackageData *data, std::int8_t uid)
{
    if (!data || !data->data.data || !data->data.uids)
        return nullptr;

    for (std::uint16_t i = 0; i < data->data.dataSize; ++i)
    {
        if (data->data.uids[i] == uid)
            return data->data.data[i];
    }
    return nullptr;
}

bool SetPackageSpell(RE::TESPackage *pkg, RE::TESForm *spell)
{
    if (g_spellOuter == kNotCalibrated || !pkg || !spell)
        return false;

    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    if (!custom)
        return false;

    std::int8_t uid = 0;
    if (!FindInputUID(custom, "Spell", uid))
        return false;

    auto *input = InputByUID(custom, uid);
    if (!input)
        return false;

    const auto base = reinterpret_cast<std::uintptr_t>(input);
    const std::uintptr_t target = *reinterpret_cast<std::uintptr_t *>(base + g_spellOuter);
    if (!LooksLikePointer(target))
        return false;

    *reinterpret_cast<RE::TESForm **>(target + g_spellInner) = spell;
    return true;
}

// Is this actor the one the vanilla follower alias holds? The combat override
// list we spliced into belongs to that alias, so a follower who is not in it
// -- recruited by a framework, or made a teammate from the console -- never
// sees our packages, and the log should say so rather than leave "she did not
// cast" ambiguous. Read from the ACTOR: every alias she fills is recorded on
// her as ExtraAliasInstanceArray, and the whole table goes in the log.
bool InFollowerAlias(RE::Actor *actor)
{
    const auto *extra = actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
    if (!extra)
    {
        logger::info("  {} is in NO quest alias at all", actor->GetName() ? actor->GetName() : "?");
        return false;
    }

    bool found = false;
    for (const auto *inst : extra->aliases)
    {
        if (!inst || !inst->quest)
            continue;
        const bool ours = inst->quest->GetFormID() == kDialogueFollowerQuestID;
        found = found || (ours && inst->alias && inst->alias->aliasID == 0);
        logger::info("  alias: quest {:08X} \"{}\" alias {} \"{}\" ({} instanced packages){}", inst->quest->GetFormID(),
                     inst->quest->GetFormEditorID() ? inst->quest->GetFormEditorID() : "",
                     inst->alias ? inst->alias->aliasID : 0xFFFFFFFF, inst->alias ? inst->alias->aliasName.c_str() : "",
                     inst->instancedPackages ? inst->instancedPackages->size() : 0, ours ? "  <- follower alias" : "");
    }

    // And the faction the follower dialogue puts her in, which "Follow me"
    // sets alongside the alias and the console script used to set alone.
    auto *currentFollower = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);
    logger::info("  CurrentFollowerFaction rank {}, teammate {}",
                 currentFollower ? actor->GetFactionRank(currentFollower, false) : -99, actor->IsPlayerTeammate());

    return found;
}

void SetRank(RE::Actor *actor, std::int8_t rank, const char *why)
{
    actor->AddToFaction(g_faction, rank);

    // Read it back. Whether AddToFaction updates an existing membership's rank
    // or only inserts is not something the header says, and the condition on
    // every package reads exactly this value.
    //
    // Measured: setting -1 REMOVES her from the faction, and GetFactionRank
    // then reports -2 ("not in faction"). Either negative answer means no
    // slot condition can pass, which is all "cleared" has to mean.
    const auto readBack = actor->GetFactionRank(g_faction, false);
    const bool ok = (readBack == rank) || (rank < 0 && readBack < 0);
    if (!ok)
        logger::warn("  {:08X} rank {} ({}) -- read back {} INSTEAD", actor->GetFormID(), rank, why, readBack);
    else
        logger::info("  {:08X} rank {} ({}, read back {})", actor->GetFormID(), rank, why, readBack);
}

// The PackageTarget behind a named input, or null if the layout has not been
// established. Same shape as the Spell input: IPackageData + outer -> a
// PackageTarget, which CommonLibSSE maps (targType at 00, target union at 08).
RE::PackageTarget *TargetOfInput(RE::TESPackage *pkg, const char *inputName)
{
    if (g_spellOuter == kNotCalibrated || !pkg)
        return nullptr;
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    if (!custom)
        return nullptr;
    std::int8_t uid = 0;
    if (!FindInputUID(custom, inputName, uid))
        return nullptr;
    auto *input = InputByUID(custom, uid);
    if (!input)
        return nullptr;
    const std::uintptr_t p =
        *reinterpret_cast<std::uintptr_t *>(reinterpret_cast<std::uintptr_t>(input) + g_spellOuter);
    return LooksLikePointer(p) ? reinterpret_cast<RE::PackageTarget *>(p) : nullptr;
}

// The float behind a named input (CastTimeMin, CastTimeMax), or null.
float *FloatOfInput(RE::TESPackage *pkg, const char *inputName)
{
    if (!pkg)
        return nullptr;
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    if (!custom)
        return nullptr;
    std::int8_t uid = 0;
    if (!FindInputUID(custom, inputName, uid))
        return nullptr;
    auto *input = InputByUID(custom, uid);
    if (!input || g_castTimeOffset == kNotCalibrated)
        return nullptr;
    return reinterpret_cast<float *>(reinterpret_cast<std::uintptr_t>(input) + g_castTimeOffset);
}

// Where, inside a float input, the float sits. The first guess (+08, the
// named data slot) read 0 / 0 in the engine. So: dump the input and look for
// the authored bit patterns, at the same offset in both inputs.
std::size_t FindCastTimeOffset(RE::TESPackage *pkg)
{
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    std::int8_t uidMin = 0;
    std::int8_t uidMax = 0;
    if (!custom || !FindInputUID(custom, "CastTimeMin", uidMin) || !FindInputUID(custom, "CastTimeMax", uidMax))
        return kNotCalibrated;
    auto *lo = InputByUID(custom, uidMin);
    auto *hi = InputByUID(custom, uidMax);
    if (!lo || !hi)
        return kNotCalibrated;

    logger::info("probe: CastTimeMin +00 {}", HexDump(lo, 32));
    logger::info("probe: CastTimeMax +00 {}", HexDump(hi, 32));

    for (std::size_t off = 0; off + sizeof(float) <= 32; off += sizeof(float))
    {
        const float a = *reinterpret_cast<const float *>(reinterpret_cast<std::uintptr_t>(lo) + off);
        const float b = *reinterpret_cast<const float *>(reinterpret_cast<std::uintptr_t>(hi) + off);
        if (a == kAuthoredCastTimeMin && b == kAuthoredCastTimeMax)
            return off;
    }
    return kNotCalibrated;
}

// Set how long a slot holds a concentration stream. Both min and max, so the
// procedure has no range to roll in. Restored to the authored values when a
// fire-and-forget spell takes the slot, so a record never carries a stale
// four-second cast time into a one-second spell.
bool SetPackageCastTime(RE::TESPackage *pkg, float seconds)
{
    if (!g_castTimeCalibrated)
        return false;
    float *lo = FloatOfInput(pkg, "CastTimeMin");
    float *hi = FloatOfInput(pkg, "CastTimeMax");
    if (!lo || !hi)
        return false;
    *lo = seconds;
    *hi = seconds;
    return true;
}

// Aim a slot's Target input at an actor, or back at Self with nullptr.
bool SetPackageTarget(RE::TESPackage *pkg, RE::Actor *target)
{
    auto *pt = g_targetCalibrated ? TargetOfInput(pkg, "Target") : nullptr;
    if (!pt)
        return false;

    if (target)
    {
        pt->targType = g_typeSpecificReference;
        pt->target.handle = RE::ObjectRefHandle(target);
    }
    else
    {
        pt->targType = g_typeSelf;
        pt->target.object = nullptr;
    }
    return true;
}

// Is this actor already casting through some slot? A second request from the
// same follower before the first resolves would put two ranks on her.
bool AlreadyCasting(const RE::Actor *actor)
{
    for (const auto &slot : g_pool)
        if (slot.Busy() && slot.lease->FormID() == actor->GetFormID())
            return true;
    return false;
}

// The one way a record comes back. Destroying the lease clears the rank and
// re-evaluates; nothing else here touches the rank on the way out.
void Release(std::size_t i)
{
    g_pool[i].lease.reset();
    g_pool[i].target = {};
    SetPackageTarget(g_slots[i], nullptr); // no target handle outlives its lease
    g_pool[i].fired.store(false, std::memory_order_relaxed);
    g_pool[i].stopped.store(false, std::memory_order_relaxed);
    g_pool[i].seenRunning = false;
    g_pool[i].streaming = false;
}

// Put our slots at the FRONT of every follower combat-override list we know
// of. Order matters: the vanilla list's last entry has no conditions, so
// anything appended after it is never reached. Returns how many lists were
// spliced; zero means casting cannot work in this load order.
std::size_t SpliceIntoOverrideLists()
{
    auto *handler = RE::TESDataHandler::GetSingleton();
    std::size_t spliced = 0;

    for (const auto &entry : kOverrideLists)
    {
        auto *list = handler ? handler->LookupForm<RE::BGSListForm>(entry.localID, entry.plugin) : nullptr;
        if (!list)
        {
            logger::info("packages: {} not loaded -- its follower list is not spliced", entry.plugin);
            continue;
        }

        std::vector<RE::TESForm *> keep;
        for (auto *form : list->forms)
        {
            bool ours = false;
            for (auto *slot : g_slots)
                ours = ours || (form == slot);
            if (!ours)
                keep.push_back(form);
        }

        list->forms.clear();
        for (auto *slot : g_slots)
            list->forms.push_back(slot);
        for (auto *form : keep)
            list->forms.push_back(form);

        // FormIDs, not editor ids: packages carry no editor id at runtime, and
        // the first version of this line printed "[]" for a ten-entry list.
        std::string ids;
        for (auto *form : list->forms)
        {
            if (!ids.empty())
                ids += ", ";
            ids += fmt::format("{:08X}", form ? form->GetFormID() : 0);
        }
        logger::info("packages: {} list {:08X} is now [{}] ({} entries, ours first: {})", entry.plugin,
                     list->GetFormID(), ids, list->forms.size(), !list->forms.empty() && list->forms[0] == g_slots[0]);
        ++spliced;
    }
    return spliced;
}

} // namespace

const char *ToString(CastRequest r) noexcept
{
    switch (r)
    {
    case CastRequest::Armed:
        return "cast requested";
    case CastRequest::NoPackages:
        return "FollowerTactics.esp not loaded";
    case CastRequest::PoolBusy:
        return "every package slot is mid-cast; skipped this turn";
    case CastRequest::AlreadyCasting:
        return "already mid-cast; skipped this turn";
    case CastRequest::SpellNotInSlot:
        return "could not repoint the package at that spell";
    case CastRequest::TargetGone:
        return "the target is no longer a loaded actor";
    }
    return "?";
}

bool IsMidCast(const RE::Actor *actor)
{
    return g_available && actor && AlreadyCasting(actor);
}

bool HasFreeSlot()
{
    if (!g_available)
        return false;
    for (const auto &slot : g_pool)
        if (!slot.Busy())
            return true;
    return false;
}

CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId, float sustainSeconds)
{
    if (!g_available || !actor)
        return CastRequest::NoPackages;

    // Self, or someone else. Anyone else must be a loaded actor right now;
    // the record will hold a handle to her for the duration of the lease.
    RE::Actor *target = nullptr;
    if (targetId != 0 && targetId != actor->GetFormID())
    {
        target = RE::TESForm::LookupByID<RE::Actor>(targetId);
        if (!target || !target->Is3DLoaded())
        {
            logger::info("  target {:08X} is not a loaded actor", targetId);
            return CastRequest::TargetGone;
        }
        if (!g_targetCalibrated)
        {
            logger::info("  target input not calibrated; only self-casts are possible");
            return CastRequest::TargetGone;
        }
    }

    if (AlreadyCasting(actor))
        return CastRequest::AlreadyCasting;

    // Take any free record. Never one in use, even for the same spell: the
    // record is hers for the duration, so the pool cannot be caught out by an
    // input it did not think to compare.
    std::size_t chosen = kPackageSlots;
    for (std::size_t i = 0; i < kPackageSlots && chosen == kPackageSlots; ++i)
        if (!g_pool[i].Busy())
            chosen = i;
    if (chosen == kPackageSlots)
    {
        // Should be unreachable: the evaluator saw HasFreeSlot() false and
        // reported Busy instead of firing. Reaching it means two casts fired
        // in one tick against one free record, which is worth a line.
        logger::warn("  pool exhausted at dispatch: all {} records held", kPackageSlots);
        return CastRequest::PoolBusy;
    }

    auto &slot = g_pool[chosen];

    // Two spells can share a display name (Marcurio's heal is 0007231C, the
    // vanilla one 0002F3B8, both "Fast Healing"), so the form is what counts.
    if (slot.spell != spellFormID)
    {
        auto *wanted = RE::TESForm::LookupByID(spellFormID);
        if (!wanted || !SetPackageSpell(g_slots[chosen], wanted))
        {
            logger::info("  slot {} still casts {:08X}; could not repoint to {:08X}", chosen, slot.spell, spellFormID);
            return CastRequest::SpellNotInSlot;
        }
        slot.spell = spellFormID;
    }

    SetPackageTarget(g_slots[chosen], target);
    slot.target = target ? target->GetHandle() : RE::ActorHandle{};

    // A concentration spell streams for as long as the procedure's CastTime
    // says. Set that to the sustain, and remember that the fire event is
    // not the end of this one.
    auto *spellItem = RE::TESForm::LookupByID<RE::SpellItem>(spellFormID);
    slot.sustained = spellItem && spellItem->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    const float sustain = sustainSeconds > 0.0f ? sustainSeconds : kDefaultSustainSeconds;
    if (slot.sustained)
    {
        if (SetPackageCastTime(g_slots[chosen], sustain))
            logger::info("  slot {} sustains {} for {:.1f} s", chosen,
                         spellItem->GetName() ? spellItem->GetName() : "?", sustain);
        else
            logger::info("  slot {} cast time not calibrated; the stream will run the authored {:.1f}-{:.1f} s", chosen,
                         kAuthoredCastTimeMin, kAuthoredCastTimeMax);
    }
    else
        SetPackageCastTime(g_slots[chosen], kAuthoredCastTimeMax);
    logger::info("  slot {} aims at {}", chosen,
                 target ? fmt::format("{:08X} \"{}\"", target->GetFormID(), target->GetName() ? target->GetName() : "?")
                        : std::string("self"));

    // The diagnostic that decides what a silence means. Not in the alias: our
    // list was never consulted and no amount of package tuning will help.
    logger::info("  {} in the DialogueFollower alias: {}", actor->GetName() ? actor->GetName() : "?",
                 InFollowerAlias(actor) ? "yes" : "NO -- recruit through dialogue, not the console");

    slot.armedAt = TacticsSeconds();
    // The window covers the AI's start-up latency. For a stream it is
    // extended when the stream actually starts (see the tick), so a stream
    // that never starts does not hold her for the sustain on top.
    slot.until = slot.armedAt + kArmWindowSeconds;
    slot.sustain = sustain;
    slot.seenRunning = false;
    slot.fired.store(false, std::memory_order_relaxed);
    slot.stopped.store(false, std::memory_order_relaxed);

    if (g_sinked.insert(actor->GetFormID()).second)
        logger::info("  animation sink {} on {:08X}",
                     actor->AddAnimationGraphEventSink(&g_fireSink) ? "added" : "REFUSED", actor->GetFormID());

    // The lease sets the rank in its constructor. From here on the record is
    // hers until the lease is destroyed, and only that clears the rank.
    slot.lease.emplace(actor, static_cast<std::int8_t>(chosen));

    // Immediate, or she finishes whatever she is doing first and the rule's
    // timing -- the entire point of this route -- is lost.
    actor->EvaluatePackage(/*immediate*/ true, /*resetAI*/ false);

    const auto *current = actor->GetCurrentPackage();
    logger::info("  current package after evaluate: {:08X} ({})", current ? current->GetFormID() : 0,
                 current == g_slots[chosen] ? "OURS" : "not ours yet -- watching");
    slot.seenRunning = current == g_slots[chosen];

    return CastRequest::Armed;
}

void ResetPackages()
{
    for (auto &slot : g_pool)
    {
        if (slot.lease)
            slot.lease->Abandon();
        slot.lease.reset();
        slot.seenRunning = false;
        slot.streaming = false;
        slot.fired.store(false, std::memory_order_relaxed);
        slot.stopped.store(false, std::memory_order_relaxed);
    }
    g_sinked.clear();
}

void TickPackages(double now, const std::vector<RE::Actor *> &followers)
{
    if (!g_available)
        return;

    // Stale ranks first. A follower who holds no record has no business in
    // the faction at a slot rank; the usual cause is a save made while she
    // was armed. Cleared, or her slot's condition passes on every evaluation
    // for the rest of the session.
    for (auto *follower : followers)
    {
        if (!follower || AlreadyCasting(follower))
            continue;
        const auto rank = follower->GetFactionRank(g_faction, false);
        if (rank >= 0)
        {
            logger::warn("packages: {} carried rank {} with no record held -- clearing",
                         follower->GetName() ? follower->GetName() : "?", rank);
            SetRank(follower, kRankNone, "stale");
        }
    }

    for (std::size_t i = 0; i < kPackageSlots; ++i)
    {
        auto &slot = g_pool[i];
        if (!slot.Busy())
            continue;

        auto actor = slot.lease->Actor();
        if (!actor)
        {
            // Unloaded or gone. The lease's destructor finds no actor and
            // clears nothing; the sweep above catches her if she comes back.
            logger::info("packages: slot {} holder vanished -- released", i);
            Release(i);
            continue;
        }
        const char *name = actor->GetName() ? actor->GetName() : "?";

        const bool running = actor->GetCurrentPackage() == g_slots[i];
        if (running && !slot.seenRunning)
        {
            slot.seenRunning = true;
            logger::info("packages: {} is RUNNING slot {} (spell {:08X})", name, i, slot.spell);
        }

        // ONE release, with a reason. The deadline is the guarantee: a record
        // is never held past it, whatever the game did or did not do. The
        // other two are only signals that the hold can end sooner -- the spell
        // has left her hand, or the AI has already moved on -- so a follower
        // is not kept for four seconds after a one-second cast.
        // A stream that has started gets its sustain added to the window,
        // once, from the moment it started.
        if (slot.sustained && !slot.streaming && slot.fired.load(std::memory_order_relaxed))
        {
            slot.streaming = true;
            slot.until = now + slot.sustain + 1.0;
            logger::info("packages: {} stream started on slot {} -- {:.1f} s to run", name, i, slot.sustain);
        }

        const char *why = nullptr;
        if (!slot.sustained && slot.fired.load(std::memory_order_relaxed))
            why = "spell fired";
        else if (slot.sustained && slot.stopped.load(std::memory_order_relaxed))
            why = "stream ended";
        else if (slot.sustained && slot.target && slot.target.get() && slot.target.get()->IsDead())
            why = "target dead"; // a stream at a corpse is wasted magicka and a follower standing still
        else if (slot.seenRunning && !running)
            why = "package ended";
        else if (now >= slot.until)
            why = slot.seenRunning ? (slot.streaming ? "deadline, stream still running" : "deadline, never cast")
                                   : "deadline, AI never picked it up";

        if (why)
        {
            logger::info("packages: {} releases slot {} after {:.1f} s: {}", name, i, now - slot.armedAt, why);
            Release(i);
        }
    }
}

void InitPackages()
{
    auto *handler = RE::TESDataHandler::GetSingleton();
    if (!handler)
    {
        logger::info("packages: no data handler");
        return;
    }

    std::size_t found = 0;
    for (std::size_t i = 0; i < kPackageSlots; ++i)
    {
        g_slots[i] =
            handler->LookupForm<RE::TESPackage>(static_cast<RE::FormID>(kFirstPackageLocalID + i), kPluginName);
        if (g_slots[i])
            ++found;
    }
    g_faction = handler->LookupForm<RE::TESFaction>(static_cast<RE::FormID>(kCastFactionLocalID), kPluginName);

    g_available = (found == kPackageSlots) && g_faction;

    if (!g_available)
    {
        // Optional content: a player who has not enabled the ESL simply does
        // not get cast rules. Saying WHICH is missing matters though.
        logger::info("packages: {}/{} packages, faction {} in {} -- cast rules unavailable (is the ESL "
                     "enabled, and is it the version with FT_CastNow?)",
                     found, kPackageSlots, g_faction ? "found" : "MISSING", kPluginName);
        return;
    }

    logger::info("packages: {} UseMagic slots and FT_CastNow resolved from {}", found, kPluginName);
    g_available = SpliceIntoOverrideLists() > 0;
}

bool PackagesAvailable()
{
    return g_available;
}

void CalibrateInputs()
{
    if (!g_available || !g_slots[0])
        return;

    auto *pkg = g_slots[0];
    logger::info("probe: {} procedure={}", pkg->GetFormEditorID() ? pkg->GetFormEditorID() : "?",
                 static_cast<std::uint32_t>(pkg->procedureType.get()));

    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    if (!custom)
    {
        logger::info("probe: package data is not TESCustomPackageData -- template inputs unreachable");
        return;
    }
    logger::info("probe: {} inputs", custom->data.dataSize);

    LogNameMap(custom, "package");

    std::int8_t uid = 0;
    if (!FindInputUID(custom, "Spell", uid))
    {
        logger::info("probe: no 'Spell' input in the name map -- cast rules stay off");
        return;
    }

    auto *input = InputByUID(custom, uid);
    logger::info("probe: 'Spell' is uid {} -> IPackageData {}", static_cast<int>(uid),
                 static_cast<const void *>(input));
    if (!input)
        return;

    auto *canary = RE::TESForm::LookupByID(kCanarySpellID);
    logger::info("probe: canary {} lives at {}", canary ? "found" : "MISSING", static_cast<const void *>(canary));
    if (!canary)
        return;

    const auto canaryAddr = reinterpret_cast<std::uintptr_t>(canary);

    logger::info("probe: +00 {}", HexDump(input, 32));
    logger::info("probe: +20 {}", HexDump(reinterpret_cast<const std::uint8_t *>(input) + 32, 32));

    const auto *words = reinterpret_cast<const std::uintptr_t *>(input);
    for (std::size_t w = 0; w < 8; ++w)
    {
        const std::uintptr_t value = words[w];
        const std::size_t offset = w * sizeof(std::uintptr_t);

        if (value == canaryAddr)
        {
            g_spellOuter = 0;
            g_spellInner = offset;
            logger::info("probe: FOUND canary directly at +{:02X} -- Spell is a TESForm* here", offset);
            continue;
        }

        if (!LooksLikePointer(value))
            continue;

        const auto *inner = reinterpret_cast<const std::uintptr_t *>(value);
        for (std::size_t k = 0; k < 4; ++k)
        {
            if (inner[k] != canaryAddr)
                continue;
            g_spellOuter = offset;
            g_spellInner = k * sizeof(std::uintptr_t);
            logger::info("probe: FOUND canary at +{:02X} -> +{:02X} -- Spell is behind a pointer", offset,
                         g_spellInner);
        }
    }

    if (g_spellOuter == kNotCalibrated)
    {
        logger::info("probe: layout NOT identified -- cast rules will refuse anything but the "
                     "spell each slot was authored with. Nothing will be written.");
        return;
    }
    logger::info("probe: calibrated (+{:02X} -> +{:02X}); cast rules can name any spell", g_spellOuter, g_spellInner);

    // The Target input, same outer offset, two canaries. Ours all ship as
    // Self: their type byte is the Self value, and all eight must agree.
    std::int8_t self = -1;
    for (std::size_t i = 0; i < kPackageSlots; ++i)
    {
        auto *pt = TargetOfInput(g_slots[i], "Target");
        if (!pt || (i > 0 && pt->targType != self))
        {
            logger::info("probe: Target input of slot {} {} -- targets other than self stay off", i,
                         pt ? fmt::format("reads type {} where slot 0 read {}", pt->targType, self) : "unreachable");
            return;
        }
        self = pt->targType;
    }

    // Mercer's cast-at-player package is authored with a specific reference,
    // the player. Its type byte is the specific-reference value, and its
    // handle must be the player's or the union is not where we think.
    auto *mercer = RE::TESForm::LookupByID<RE::TESPackage>(kMercerCastAtPlayerID);
    auto *mpt = TargetOfInput(mercer, "Target");
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!mpt || !player || mpt->target.handle.native_handle() != player->GetHandle().native_handle())
    {
        logger::info("probe: Mercer's Target input {} -- targets other than self stay off",
                     mpt ? fmt::format("type {} handle {:08X}, player handle {:08X}", mpt->targType,
                                       mpt->target.handle.native_handle(),
                                       player ? player->GetHandle().native_handle() : 0)
                         : "unreachable");
        return;
    }
    if (mpt->targType == self)
    {
        logger::info("probe: Self and specific-reference read the same type {} -- targets other than self stay off",
                     self);
        return;
    }

    g_typeSelf = self;
    g_typeSpecificReference = mpt->targType;
    g_targetCalibrated = true;

    // The CastTime floats: every slot ships 0.5 and 1.0. Find where they sit
    // in slot 0, then read both back at that offset on every slot before any
    // stream length is written.
    g_castTimeOffset = FindCastTimeOffset(g_slots[0]);
    if (g_castTimeOffset == kNotCalibrated)
    {
        logger::info("probe: CastTime floats not found in slot 0; concentration spells will run the authored time");
        return;
    }
    for (std::size_t i = 0; i < kPackageSlots; ++i)
    {
        const float *lo = FloatOfInput(g_slots[i], "CastTimeMin");
        const float *hi = FloatOfInput(g_slots[i], "CastTimeMax");
        if (!lo || !hi || *lo != kAuthoredCastTimeMin || *hi != kAuthoredCastTimeMax)
        {
            logger::info("probe: CastTime of slot {} reads {} / {} at +{:02X} -- expected {} / {}; concentration "
                         "spells will run the authored time",
                         i, lo ? *lo : -1.0f, hi ? *hi : -1.0f, g_castTimeOffset, kAuthoredCastTimeMin,
                         kAuthoredCastTimeMax);
            g_castTimeOffset = kNotCalibrated;
            return;
        }
    }
    g_castTimeCalibrated = true;
    logger::info("probe: CastTime floats at +{:02X} on all {} slots; a concentration spell can be sustained for a "
                 "chosen time",
                 g_castTimeOffset, kPackageSlots);
    logger::info("probe: Target input calibrated: Self = {}, specific reference = {} (from Mercer's package); cast "
                 "rules can name any loaded actor",
                 g_typeSelf, g_typeSpecificReference);
}

} // namespace ft::game
