#include "game/Packages.h"

#include "game/Forms.h"
#include "game/Util.h"

#include <algorithm>
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
bool g_available = false;

// How long the AI gets to START the cast before the record is taken back.
// Measured: every cast that happened fired 0.65-2.2 s after arming; the ones
// that did not had not started by 4 s either. The window only costs anything
// in that second case -- it is how long she stands held and unreactive -- so
// it is set just above the slowest measured start.
constexpr double kArmWindowSeconds = 2.5;
// A shout slot releases on the voice's fire event. Measured (2026-09-05,
// Voice of the Emperor through a one-word wrapper): request to fire 0.3 s
// and 1.5 s, the shout animation itself 0.3 s. Three seconds is the never-
// started case, as 2.5 s is for a hand cast; a bit longer because the AI
// had a second's hesitation on one of the two.
constexpr double kVoiceArmWindowSeconds = 3.0;

void TakeOutOfLists(RE::TESPackage *pkg);

// The condition, as an owned resource.
//
// Pointing a slot's condition at a follower is what makes her package's
// condition pass, and forgetting to clear it is the one mistake this file
// must not be able to make: she would pass that condition on every
// evaluation for the rest of the session. So the pointer is held by an
// object, and the ONLY way to give a record back is to destroy that object.
// The destructor clears the parameter and asks the AI to re-evaluate, so
// there is no release path that can skip either.
//
// What this does NOT guarantee is timing. Nothing in C++ ends the lease on its
// own; the tick does, on a signal or at the deadline. RAII makes the cleanup
// unskippable, the deadline makes it prompt.
//
// The condition is GetIsReference(param) == 1 on the subject: the engine
// compares the evaluating actor's pointer with the parameter's, null-safe
// (read from the executable, docs/MAGIC.md "Forms at runtime"). Nothing is
// written to the actor, so nothing about a lease can reach a save through
// her.
class SlotLease
{
  public:
    SlotLease(RE::Actor *actor, RE::TESConditionItem *condition)
        : actor_(actor->GetHandle()), id_(actor->GetFormID()), condition_(condition)
    {
        condition_->data.functionData.params[0] = actor;
        logger::info("  {:08X} holds the condition (leased)", id_);
    }

    SlotLease(const SlotLease &) = delete;
    SlotLease &operator=(const SlotLease &) = delete;

    ~SlotLease()
    {
        // The parameter is cleared whatever became of the actor: a pointer
        // to an actor object that may be gone must not stay in a condition
        // the AI evaluates.
        condition_->data.functionData.params[0] = nullptr;
        if (!live_)
            return;
        auto actor = actor_.get();
        if (!actor)
            return;
        logger::info("  {:08X} releases the condition (lease ended)", id_);

        // Without this she stays in the package until the AI's own next
        // evaluation, which after a completed cast can be a long time: that
        // was the "he healed and then froze" of the first successful run.
        actor->EvaluatePackage(/*immediate*/ true, /*resetAI*/ false);
    }

    // After a game load the handle may resolve to an unrelated actor. Do not
    // re-evaluate anyone; the parameter is still cleared.
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
    RE::TESConditionItem *condition_;
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
    // A shout slot's wrapper: the one-word shout its package ships pointing
    // at, whose word's spell is repointed per power lease. Null for a spell
    // slot. `shouting` is what the package's Shout input holds right now --
    // the wrapper for a power, the shout itself for a shout -- and what the
    // sink matches the voice's fire event against.
    RE::TESShout *wrapper = nullptr;
    RE::TESShout *shouting = nullptr;
    // The power a shout slot is casting, and the spell type it had before
    // the lease made it a Voice spell (see RequestShout). Restored on
    // release; null when nothing is leased, and for a shout.
    RE::SpellItem *power = nullptr;
    RE::MagicSystem::SpellType powerType = RE::MagicSystem::SpellType::kSpell;
    // The record's one condition; the lease points its parameter at the
    // holder.
    RE::TESConditionItem *condition = nullptr;
    std::optional<SlotLease> lease;
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
    // CastStop arrived after that -- for a stream, its end. `begun`: a
    // BeginCast event (voice or either hand), so the deadline can step back
    // and let a cast that has started finish, whatever it takes.
    std::atomic<bool> fired{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> begun{false};
    bool extended = false;

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
        const bool voice = _stricmp(tag, "Voice_SpellFire_Event") == 0;
        const bool begin = _stricmp(tag, "BeginCastVoice") == 0 || _stricmp(tag, "BeginCastRight") == 0 ||
                           _stricmp(tag, "BeginCastLeft") == 0;
        const bool stop = _stricmp(tag, "CastStop") == 0;

        for (auto &slot : g_pool)
        {
            if (!slot.Busy() || slot.lease->FormID() != who)
                continue;
            // Every cast-related tag while a record is held, so the graph's
            // vocabulary is on record and a missing fire event is diagnosable.
            // Shout and Voice too, for the shout slots: which tags a power
            // emits on its way out is being learned from this line.
            if (right || left || strstr(tag, "Cast") || strstr(tag, "Spell") || strstr(tag, "Shout") ||
                strstr(tag, "Voice"))
                logger::info("  anim {:08X}: {}", who, tag);
            // A stream that has fired and now stops has ended, whether the
            // CastTime ran out or something interrupted it.
            if (stop && slot.fired.load(std::memory_order_relaxed))
                slot.stopped.store(true, std::memory_order_relaxed);

            if (begin)
                slot.begun.store(true, std::memory_order_relaxed);
            // A shout slot fires from the voice. Measured 2026-09-04: a
            // power through the wrapper emits BeginCastVoice and, 0.1 s
            // later, Voice_SpellFire_Event. Which of the wrapper's words the
            // engine chose, and what the voice caster holds, go in the log
            // so a shout that lands nothing can be read.
            if (voice && slot.shouting)
            {
                auto *actor = const_cast<RE::TESObjectREFR *>(ev->holder)->As<RE::Actor>();
                const auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
                const auto *high = process ? process->high : nullptr;
                const auto *voiceItem =
                    actor ? actor->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kPowerOrShout] : nullptr;
                const auto *caster =
                    actor ? actor->GetActorRuntimeData().magicCasters[RE::Actor::SlotTypes::kPowerOrShout] : nullptr;
                logger::info("  anim {:08X}: voice fired: shout {:08X} variation {} level {}, voice slot holds {:08X}, "
                             "caster spell {:08X} -- {}",
                             who, high && high->currentShout ? high->currentShout->GetFormID() : 0,
                             high ? static_cast<std::int32_t>(high->currentShoutVariation) : -99,
                             actor ? actor->GetCurrentShoutLevel() : -99, voiceItem ? voiceItem->GetFormID() : 0,
                             caster && caster->currentSpell ? caster->currentSpell->GetFormID() : 0,
                             high && high->currentShout == slot.shouting ? "OURS" : "not ours");
                if (high && high->currentShout == slot.shouting)
                    slot.fired.store(true, std::memory_order_relaxed);
                continue;
            }
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

// The vanilla records the layout is read from. Mercer's cast-at-player
// package (TG08BMercerCombatOverrideCastAtPlayer) is authored with Spell =
// Nightingale Strife, Target = the player, CastTime 0.5 / 1.0 -- and is the
// record our spell slots are copied from. Colette's practice heal
// (WCollegeColettePracticeHeal13x2) is authored with Target = Self. Tsun's
// Clear Skies package (MQ305TsunReturnShout) is the Shout-template instance
// the voice slots are copied from.
constexpr std::uint32_t kMercerCastAtPlayerID = 0x000FDBC3;
constexpr std::uint32_t kMercerSpellID = 0x000FDBC7; // Nightingale Strife
constexpr std::uint32_t kColetteHealID = 0x00098BAD;
constexpr std::uint32_t kTsunShoutID = 0x000EC3A5;

// PackageTarget::targType values, from the PTDA type field. LEARNED from
// game data rather than assumed: the first version took Self = 5 from the
// record library's arm order and the engine read 6. Colette's Target is
// Self, so its type byte IS the Self value; Mercer's is the player, so its
// type byte is the specific-reference value -- confirmed by its handle
// matching the player's. Nothing is written until both read consistently.

// The CastTime inputs: how long the UseMagic procedure holds a CONCENTRATION
// stream. Two floats, authored 0.5 and 1.0 in Mercer's record -- the canary
// for their layout, which is the named package data's 8-byte slot at +08 (the
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

// Point a named TargetSelector input -- the UseMagic template's "Spell", the
// Shout template's "Shout" -- at a form. Both are the same kind of input, so
// the offsets the canary found on "Spell" serve both.
bool SetPackageInput(RE::TESPackage *pkg, const char *inputName, RE::TESForm *form)
{
    if (g_spellOuter == kNotCalibrated || !pkg || !form)
        return false;

    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    if (!custom)
        return false;

    std::int8_t uid = 0;
    if (!FindInputUID(custom, inputName, uid))
        return false;

    auto *input = InputByUID(custom, uid);
    if (!input)
        return false;

    const auto base = reinterpret_cast<std::uintptr_t>(input);
    const std::uintptr_t target = *reinterpret_cast<std::uintptr_t *>(base + g_spellOuter);
    if (!LooksLikePointer(target))
        return false;

    *reinterpret_cast<RE::TESForm **>(target + g_spellInner) = form;
    return true;
}

bool SetPackageSpell(RE::TESPackage *pkg, RE::TESForm *spell)
{
    return SetPackageInput(pkg, "Spell", spell);
}

// Set a named Bool input -- the UseMagic template's "DualCast". The library
// maps the input's data word (BGSPackageDataBool, +08) and reads the value
// off bit 1, as its GetDataAsString does; the input's own type name is
// checked first so a name that is not a Bool is left alone.
bool SetPackageBool(RE::TESPackage *pkg, const char *inputName, bool value)
{
    if (!pkg)
        return false;
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    if (!custom)
        return false;
    std::int8_t uid = 0;
    if (!FindInputUID(custom, inputName, uid))
        return false;
    auto *input = InputByUID(custom, uid);
    if (!input || input->GetTypeName() != "Bool")
        return false;
    auto &data = static_cast<RE::BGSPackageDataBool *>(input)->data;
    data.i = value ? (data.i | 0x2u) : (data.i & ~0x2u);
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

// The Shout procedure fires only a shout the actor HAS (CK wiki), and an
// NPC's shouts live on the base record's spell list, where the Greybeards'
// are. So a wrapper goes into that list for the lease and comes out after.
// The base is shared by every actor spawned from it; a wrapper left behind
// there would be listed under Shouts for all of them, which is why the
// sweep in the tick removes any wrapper from a follower holding no slot.
RE::TESSpellList::SpellData *ShoutListOf(RE::Actor *actor)
{
    auto *npc = actor ? actor->GetActorBase() : nullptr;
    if (!npc)
        return nullptr;
    if (!npc->actorEffects)
        npc->actorEffects = new RE::TESSpellList::SpellData();
    return npc->actorEffects;
}

void GiveWrapper(RE::Actor *actor, RE::TESShout *wrapper)
{
    auto *list = ShoutListOf(actor);
    if (!list || !wrapper)
        return;
    if (list->GetIndex(wrapper).has_value())
        return;
    logger::info("  wrapper {:08X} {} to {:08X}'s shout list", wrapper->GetFormID(),
                 list->AddShout(wrapper) ? "added" : "NOT added", actor->GetFormID());
}

void TakeWrapper(RE::Actor *actor, RE::TESShout *wrapper)
{
    auto *list = actor ? (actor->GetActorBase() ? actor->GetActorBase()->actorEffects : nullptr) : nullptr;
    if (!list || !wrapper || !list->GetIndex(wrapper).has_value())
        return;
    logger::info("  wrapper {:08X} {} from {:08X}'s shout list", wrapper->GetFormID(),
                 list->RemoveShout(wrapper) ? "removed" : "NOT removed", actor->GetFormID());
}

bool IsWrapperForm(std::uint32_t formID)
{
    for (const auto &slot : g_pool)
        if (slot.wrapper && slot.wrapper->GetFormID() == formID)
            return true;
    return false;
}

// Where her weapons are, for the timing lines: the Shout procedure has been
// measured taking 0.3 to 1.5 s to begin, and a sheathe-and-draw around the
// shout is one candidate explanation.
const char *WeaponStateName(const RE::Actor *actor)
{
    const auto *state = actor ? actor->AsActorState() : nullptr;
    if (!state)
        return "?";
    switch (state->GetWeaponState())
    {
    case RE::WEAPON_STATE::kSheathed:
        return "sheathed";
    case RE::WEAPON_STATE::kWantToDraw:
        return "wants to draw";
    case RE::WEAPON_STATE::kDrawing:
        return "drawing";
    case RE::WEAPON_STATE::kDrawn:
        return "drawn";
    case RE::WEAPON_STATE::kWantToSheathe:
        return "wants to sheathe";
    case RE::WEAPON_STATE::kSheathing:
        return "sheathing";
    }
    return "?";
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

// The one way a record comes back. Destroying the lease clears the condition
// and re-evaluates; nothing else here touches the condition on the way out.
void Release(std::size_t i)
{
    // The wrapper comes off before the lease goes: the lease is what still
    // knows whose list it is in.
    if (g_pool[i].wrapper && g_pool[i].lease)
    {
        if (auto actor = g_pool[i].lease->Actor())
            TakeWrapper(actor.get(), g_pool[i].wrapper);
    }
    if (g_pool[i].power)
    {
        g_pool[i].power->data.spellType = g_pool[i].powerType;
        g_pool[i].power = nullptr;
    }
    g_pool[i].shouting = nullptr;
    // Out of the lists before the lease goes: the lease's destructor asks the
    // AI to re-evaluate, and the record must not be there to be found.
    TakeOutOfLists(g_slots[i]);
    g_pool[i].lease.reset();
    g_pool[i].target = {};
    SetPackageTarget(g_slots[i], nullptr); // no target handle outlives its lease
    g_pool[i].fired.store(false, std::memory_order_relaxed);
    g_pool[i].stopped.store(false, std::memory_order_relaxed);
    g_pool[i].begun.store(false, std::memory_order_relaxed);
    g_pool[i].extended = false;
    g_pool[i].seenRunning = false;
    g_pool[i].streaming = false;
}

// The follower combat-override lists, resolved once. Empty means casting
// cannot work in this load order.
std::vector<RE::BGSListForm *> g_lists;

std::string ListContents(const RE::BGSListForm *list)
{
    // FormIDs, not editor ids: packages carry no editor id at runtime, and
    // the first version of this line printed "[]" for a ten-entry list.
    std::string ids;
    for (auto *form : list->forms)
    {
        if (!ids.empty())
            ids += ", ";
        ids += fmt::format("{:08X}", form ? form->GetFormID() : 0);
    }
    return ids;
}

std::size_t ResolveOverrideLists()
{
    auto *handler = RE::TESDataHandler::GetSingleton();
    g_lists.clear();
    for (const auto &entry : kOverrideLists)
    {
        auto *list = handler ? handler->LookupForm<RE::BGSListForm>(entry.localID, entry.plugin) : nullptr;
        if (!list)
        {
            logger::info("packages: {} not loaded -- its follower list is not used", entry.plugin);
            continue;
        }
        logger::info("packages: {} list {:08X} is [{}] ({} entries); a record is put at its front for each cast",
                     entry.plugin, list->GetFormID(), ListContents(list), list->forms.size());
        g_lists.push_back(list);
    }
    return g_lists.size();
}

// A record is in the lists only while it is leased. At the FRONT: the
// vanilla list's last entry has no conditions, so anything appended after it
// is never reached. Between casts the lists are exactly vanilla. Game-thread
// only, as everything here is: the AI reads these lists on the same thread
// the tick's task runs on, so nothing walks a list while it is rebuilt.
void PutInLists(RE::TESPackage *pkg)
{
    for (auto *list : g_lists)
    {
        std::vector<RE::TESForm *> keep(list->forms.begin(), list->forms.end());
        list->forms.clear();
        list->forms.push_back(pkg);
        for (auto *form : keep)
            if (form != pkg)
                list->forms.push_back(form);
        logger::info("  list {:08X} is now [{}]", list->GetFormID(), ListContents(list));
    }
}

void TakeOutOfLists(RE::TESPackage *pkg)
{
    for (auto *list : g_lists)
    {
        std::vector<RE::TESForm *> keep;
        for (auto *form : list->forms)
            if (form != pkg)
                keep.push_back(form);
        if (keep.size() == list->forms.size())
            continue;
        list->forms.clear();
        for (auto *form : keep)
            list->forms.push_back(form);
        logger::info("  list {:08X} is back to [{}]", list->GetFormID(), ListContents(list));
    }
}

} // namespace

const char *ToString(CastRequest r) noexcept
{
    switch (r)
    {
    case CastRequest::Armed:
        return "cast requested";
    case CastRequest::NoPackages:
        return "the cast packages could not be made at load (see FollowerTactics.log)";
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
    for (std::size_t i = 0; i < kSpellSlots; ++i)
        if (!g_pool[i].Busy())
            return true;
    return false;
}

bool HasFreeVoiceSlot()
{
    if (!g_available)
        return false;
    for (std::size_t i = kSpellSlots; i < kPackageSlots; ++i)
        if (!g_pool[i].Busy())
            return true;
    return false;
}

bool IsWrapperShout(std::uint32_t formID)
{
    return g_available && IsWrapperForm(formID);
}

bool IsLeasedPower(std::uint32_t formID)
{
    for (const auto &slot : g_pool)
        if (slot.power && slot.power->GetFormID() == formID)
            return true;
    return false;
}

namespace
{
// The first free record in [from, to), or kPackageSlots for none.
std::size_t FreeSlot(std::size_t from, std::size_t to)
{
    for (std::size_t i = from; i < to; ++i)
        if (!g_pool[i].Busy())
            return i;
    return kPackageSlots;
}

// The shared end of a request: the slot is pointed where it should be, and
// this points the slot's condition at the follower and asks the AI to look.
CastRequest Arm(std::size_t chosen, RE::Actor *actor, float sustain, double window)
{
    auto &slot = g_pool[chosen];
    // The diagnostic that decides what a silence means. Not in the alias: our
    // list was never consulted and no amount of package tuning will help.
    logger::info("  {} in the DialogueFollower alias: {}", actor->GetName() ? actor->GetName() : "?",
                 InFollowerAlias(actor) ? "yes" : "NO -- recruit through dialogue, not the console");

    slot.armedAt = TacticsSeconds();
    // The window covers the AI's start-up latency. For a stream it is
    // extended when the stream actually starts (see the tick), so a stream
    // that never starts does not hold her for the sustain on top.
    slot.until = slot.armedAt + window;
    slot.sustain = sustain;
    slot.seenRunning = false;
    slot.fired.store(false, std::memory_order_relaxed);
    slot.stopped.store(false, std::memory_order_relaxed);
    slot.begun.store(false, std::memory_order_relaxed);
    slot.extended = false;

    if (g_sinked.insert(actor->GetFormID()).second)
        logger::info("  animation sink {} on {:08X}",
                     actor->AddAnimationGraphEventSink(&g_fireSink) ? "added" : "REFUSED", actor->GetFormID());

    // Into the lists, then the lease points the condition at her in its
    // constructor. From here on the record is hers until the lease is
    // destroyed, and only that clears the condition.
    PutInLists(g_slots[chosen]);
    slot.lease.emplace(actor, slot.condition);

    // Immediate, or she finishes whatever she is doing first and the rule's
    // timing -- the entire point of this route -- is lost.
    actor->EvaluatePackage(/*immediate*/ true, /*resetAI*/ false);

    const auto *current = actor->GetCurrentPackage();
    logger::info("  current package after evaluate: {:08X} ({}); weapons {}", current ? current->GetFormID() : 0,
                 current == g_slots[chosen] ? "OURS" : "not ours yet -- watching", WeaponStateName(actor));
    slot.seenRunning = current == g_slots[chosen];

    return CastRequest::Armed;
}
} // namespace

CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId, float sustainSeconds,
                        bool dualCast)
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

    // Take any free spell record. Never one in use, even for the same
    // spell: the record is hers for the duration, so the pool cannot be
    // caught out by an input it did not think to compare.
    const std::size_t chosen = FreeSlot(0, kSpellSlots);
    if (chosen == kPackageSlots)
    {
        // Should be unreachable: the evaluator saw HasFreeSlot() false and
        // reported Busy instead of firing. Reaching it means two casts fired
        // in one tick against one free record, which is worth a line.
        logger::warn("  pool exhausted at dispatch: all {} spell records held", kSpellSlots);
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

    // Both hands or one: set on every request, since the record is shared
    // and the last lease may have left it either way.
    if (!SetPackageBool(g_slots[chosen], "DualCast", dualCast))
        logger::info("  slot {} has no DualCast input to set{}", chosen,
                     dualCast ? " -- the cast will be one-handed" : "");
    else if (dualCast)
        logger::info("  slot {} casts from both hands", chosen);

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

    return Arm(chosen, actor, sustain, kArmWindowSeconds);
}

CastRequest RequestShout(RE::Actor *actor, std::uint32_t formID, std::uint32_t targetId)
{
    if (!g_available || !actor)
        return CastRequest::NoPackages;

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

    const std::size_t chosen = FreeSlot(kSpellSlots, kPackageSlots);
    if (chosen == kPackageSlots)
    {
        logger::warn("  pool exhausted at dispatch: all {} shout records held", kVoiceSlots);
        return CastRequest::PoolBusy;
    }
    auto &slot = g_pool[chosen];
    auto *form = RE::TESForm::LookupByID(formID);
    auto *shout = form ? form->As<RE::TESShout>() : nullptr;
    auto *power = form ? form->As<RE::SpellItem>() : nullptr;
    if ((!shout && !power) || !slot.wrapper)
    {
        logger::info("  slot {} has nothing to shout for {:08X}", chosen, formID);
        return CastRequest::SpellNotInSlot;
    }

    if (power)
    {
        // The power goes into the wrapper's first word alone. Measured
        // 2026-09-05: an NPC shouts the highest FILLED word, so one word
        // gives variation 0 and the short shout (0.3 s); with all three
        // filled the engine shouted variation 2, the three-word animation,
        // a second long. TESShout::variations is a plain array on the
        // record, so this is a pointer write, no canary needed.
        slot.wrapper->variations[0].spell = power;
        for (std::size_t w = 1; w < RE::TESShout::VariationIDs::kTotal; ++w)
        {
            slot.wrapper->variations[w].spell = nullptr;
            slot.wrapper->variations[w].word = nullptr;
        }
        // And for the lease the power IS a Voice spell, on the shared record,
        // in memory, back on release. Measured 2026-09-04/05: a Power-typed
        // spell in a shout's word shouts the animation and casts nothing;
        // every vanilla word spell is type Voice, and with the type flipped
        // the effect lands. The alternative -- our own Voice spells in the
        // ESP carrying the power's effects -- needs the active-effect check
        // taught which spell stood for which power; this needs nothing, and
        // the window is the two seconds of the lease, during which the Magic
        // tab and the menus ask IsLeasedPower so the power stays listed.
        slot.power = power;
        slot.powerType = power->data.spellType;
        power->data.spellType = RE::MagicSystem::SpellType::kVoicePower;
        slot.shouting = slot.wrapper;
        logger::info("  slot {} wrapper {:08X} word one now casts {:08X} \"{}\" (type {} -> Voice for the lease)",
                     chosen, slot.wrapper->GetFormID(), formID, power->GetName() ? power->GetName() : "?",
                     static_cast<int>(slot.powerType));
    }
    else
    {
        slot.shouting = shout;
        logger::info("  slot {} shouts {:08X} \"{}\" itself", chosen, formID,
                     shout->GetName() ? shout->GetName() : "?");
    }

    // The package's Shout input: the wrapper for a power, the shout itself
    // for a shout. Written only when it changes, as the Spell input is.
    if (slot.spell != formID)
    {
        if (!SetPackageInput(g_slots[chosen], "Shout", slot.shouting))
        {
            logger::info("  slot {} could not point its Shout input at {:08X}", chosen, slot.shouting->GetFormID());
            if (slot.power)
            {
                slot.power->data.spellType = slot.powerType;
                slot.power = nullptr;
            }
            slot.shouting = nullptr;
            return CastRequest::SpellNotInSlot;
        }
        slot.spell = formID;
    }

    SetPackageTarget(g_slots[chosen], target);
    slot.target = target ? target->GetHandle() : RE::ActorHandle{};
    slot.sustained = false;
    logger::info("  slot {} aims at {}", chosen,
                 target ? fmt::format("{:08X} \"{}\"", target->GetFormID(), target->GetName() ? target->GetName() : "?")
                        : std::string("self"));

    // The procedure fires only a shout the actor has: the wrapper is given
    // for the lease; a shout of their own they have already.
    if (power)
        GiveWrapper(actor, slot.wrapper);

    return Arm(chosen, actor, 0.0f, kVoiceArmWindowSeconds);
}

void ResetPackages()
{
    // The lists live in memory across a load; an entry a lease left there
    // (abandoned below) would otherwise stay.
    for (auto *pkg : g_slots)
        if (pkg)
            TakeOutOfLists(pkg);
    for (auto &slot : g_pool)
    {
        if (slot.power)
        {
            slot.power->data.spellType = slot.powerType;
            slot.power = nullptr;
        }
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

void ReleaseAllLeases(const char *why)
{
    if (!g_available)
        return;
    for (std::size_t i = 0; i < kPackageSlots; ++i)
    {
        if (!g_pool[i].Busy())
            continue;
        logger::info("packages: slot {} released after {:.1f} s: {}", i, TacticsSeconds() - g_pool[i].armedAt, why);
        Release(i);
    }
}

void TickPackages(double now, const std::vector<RE::Actor *> &followers)
{
    if (!g_available)
        return;

    // A wrapper shout left in a base's spell list by a lease that never
    // ended -- a crash mid-cast, since a save releases every lease first --
    // would list under Shouts for every actor of that base. Taken back from
    // any follower holding no record.
    for (auto *follower : followers)
    {
        if (!follower || AlreadyCasting(follower))
            continue;
        for (std::size_t i = kSpellSlots; i < kPackageSlots; ++i)
            TakeWrapper(follower, g_pool[i].wrapper);
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
            logger::info("packages: {} is RUNNING slot {} (spell {:08X}) after {:.1f} s; weapons {}", name, i,
                         slot.spell, now - slot.armedAt, WeaponStateName(actor.get()));
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
        // A cast or shout that has begun is not taken away: the deadline
        // steps back once so the animation, however long this one's is, gets
        // to its fire event. The fire event is what releases; this only keeps
        // the deadline from landing in the middle of it. The graph's own
        // BeginCast events say when, so no animation's length is assumed.
        if (!slot.extended && slot.begun.load(std::memory_order_relaxed))
        {
            slot.extended = true;
            slot.until = (std::max)(slot.until, now + 3.0);
            logger::info("packages: {} began the {} on slot {} after {:.1f} s -- deadline stepped back; weapons {}",
                         name, slot.shouting ? "shout" : "cast", i, now - slot.armedAt, WeaponStateName(actor.get()));
        }

        const char *why = nullptr;
        if (!slot.sustained && slot.fired.load(std::memory_order_relaxed))
            why = slot.power ? "power fired" : slot.wrapper ? "shout fired" : "spell fired";
        else if (slot.sustained && slot.stopped.load(std::memory_order_relaxed))
            why = "stream ended";
        else if (slot.sustained && slot.target && slot.target.get() && slot.target.get()->IsDead())
            why = "target dead"; // a stream at a corpse is wasted magicka and a follower standing still
        else if (slot.seenRunning && !running)
            why = "package ended";
        else if (now >= slot.until)
            why = slot.seenRunning ? (slot.streaming ? "deadline, stream still running"
                                      : slot.wrapper ? "deadline, shout never ended"
                                                     : "deadline, never cast")
                                   : "deadline, AI never picked it up";

        if (why)
        {
            logger::info("packages: {} releases slot {} after {:.1f} s: {}", name, i, now - slot.armedAt, why);
            Release(i);
        }
    }
}

namespace
{
// Find the Spell, Target and CastTime input layout on the vanilla records.
// Writes nothing; what it fails to find, RequestCast refuses to write. False
// means the pool cannot be made: a package whose inputs cannot be checked
// is a package that might cast the wrong thing at the wrong person.
bool Calibrate()
{
    auto *mercer = RE::TESForm::LookupByID<RE::TESPackage>(kMercerCastAtPlayerID);
    auto *colette = RE::TESForm::LookupByID<RE::TESPackage>(kColetteHealID);
    if (!mercer || !colette)
    {
        logger::info("probe: vanilla records missing (Mercer {}, Colette {}) -- cast rules stay off",
                     static_cast<const void *>(mercer), static_cast<const void *>(colette));
        return false;
    }

    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(mercer->data);
    if (!custom)
    {
        logger::info("probe: Mercer's package data is not TESCustomPackageData -- template inputs unreachable");
        return false;
    }
    logger::info("probe: Mercer's package has {} inputs", custom->data.dataSize);
    LogNameMap(custom, "package");

    std::int8_t uid = 0;
    if (!FindInputUID(custom, "Spell", uid))
    {
        logger::info("probe: no 'Spell' input in the name map -- cast rules stay off");
        return false;
    }

    auto *input = InputByUID(custom, uid);
    logger::info("probe: 'Spell' is uid {} -> IPackageData {}", static_cast<int>(uid),
                 static_cast<const void *>(input));
    if (!input)
        return false;

    auto *canary = RE::TESForm::LookupByID(kMercerSpellID);
    logger::info("probe: canary {} lives at {}", canary ? "found" : "MISSING", static_cast<const void *>(canary));
    if (!canary)
        return false;

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
        logger::info("probe: layout NOT identified -- cast rules stay off. Nothing will be written.");
        return false;
    }
    logger::info("probe: calibrated (+{:02X} -> +{:02X}); cast rules can name any spell", g_spellOuter, g_spellInner);

    // The Target input, same outer offset, two canaries. Colette's is Self:
    // its type byte is the Self value. Mercer's is the player: its type byte
    // is the specific-reference value, and its handle must be the player's
    // or the union is not where we think.
    auto *cpt = TargetOfInput(colette, "Target");
    auto *mpt = TargetOfInput(mercer, "Target");
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!cpt || !mpt || !player || mpt->target.handle.native_handle() != player->GetHandle().native_handle())
    {
        logger::info("probe: Target inputs {} -- cast rules stay off",
                     mpt ? fmt::format("Mercer's type {} handle {:08X}, player handle {:08X}", mpt->targType,
                                       mpt->target.handle.native_handle(),
                                       player ? player->GetHandle().native_handle() : 0)
                         : "unreachable");
        return false;
    }
    if (mpt->targType == cpt->targType)
    {
        logger::info("probe: Self and specific-reference read the same type {} -- cast rules stay off", cpt->targType);
        return false;
    }
    g_typeSelf = cpt->targType;
    g_typeSpecificReference = mpt->targType;
    g_targetCalibrated = true;
    logger::info("probe: Target input calibrated: Self = {} (Colette), specific reference = {} (Mercer); cast "
                 "rules can name any loaded actor",
                 g_typeSelf, g_typeSpecificReference);

    // The CastTime floats: Mercer's record has 0.5 and 1.0.
    g_castTimeOffset = FindCastTimeOffset(mercer);
    if (g_castTimeOffset == kNotCalibrated)
    {
        logger::info("probe: CastTime floats not found; concentration spells will run the copied time");
        return true; // a stream at a fixed length is a loss, not a hazard
    }
    g_castTimeCalibrated = true;
    logger::info("probe: CastTime floats at +{:02X}; a concentration spell can be sustained for a chosen time",
                 g_castTimeOffset);
    return true;
}

// Point a fresh copy at the canary spell through the calibrated layout and
// read it back: the proof that the layout found on Mercer's record holds on
// a record the engine copied from it.
bool ProveCopy(RE::TESPackage *pkg, const char *inputName, RE::TESForm *canary, std::size_t slot)
{
    if (!SetPackageInput(pkg, inputName, canary))
    {
        logger::error("packages: slot {}: could not write its {} input", slot, inputName);
        return false;
    }
    auto *pt = TargetOfInput(pkg, inputName);
    if (!pt || pt->target.object != canary)
    {
        logger::error("packages: slot {}: {} input read back {} after writing {:08X}", slot, inputName,
                      pt ? fmt::format("{:08X}", pt->target.object ? pt->target.object->GetFormID() : 0)
                         : std::string("nothing"),
                      canary->GetFormID());
        return false;
    }
    return true;
}
} // namespace

void InitPackages()
{
    g_available = false;
    if (!Calibrate())
        return;

    auto *mercer = RE::TESForm::LookupByID<RE::TESPackage>(kMercerCastAtPlayerID);
    auto *tsun = RE::TESForm::LookupByID<RE::TESPackage>(kTsunShoutID);
    auto *fastHealing = RE::TESForm::LookupByID(kCanarySpellID);
    if (!tsun || !fastHealing)
    {
        logger::info("packages: vanilla records missing (Tsun's shout package {}, Fast Healing {}) -- cast rules "
                     "stay off",
                     static_cast<const void *>(tsun), static_cast<const void *>(fastHealing));
        return;
    }

    // The spell slots: Mercer's record, Target back to Self, Spell to the
    // canary and read back.
    for (std::size_t i = 0; i < kSpellSlots; ++i)
    {
        auto *pkg = ClonePackage(mercer, kFirstPackageLocalID + static_cast<std::uint32_t>(i));
        auto *condition = AddIsReferenceCondition(pkg);
        if (!pkg || !condition || !ProveCopy(pkg, "Spell", fastHealing, i) || !SetPackageTarget(pkg, nullptr))
        {
            logger::error("packages: spell slot {} could not be made -- cast rules stay off", i);
            return;
        }
        SetPackageCastTime(pkg, kAuthoredCastTimeMax);
        g_slots[i] = pkg;
        g_pool[i].condition = condition;
        g_pool[i].spell = kCanarySpellID;
    }

    // The voice slots: a word, a wrapper shout on it, and Tsun's record with
    // its Shout input on the wrapper and Target back to Self.
    for (std::size_t i = kSpellSlots; i < kPackageSlots; ++i)
    {
        const auto k = static_cast<std::uint32_t>(i - kSpellSlots);
        auto *word = CreateWord(kFirstWordLocalID + k, "Power");
        auto *wrapper =
            word ? CreateShout(kFirstWrapperShoutLocalID + k, word, fastHealing, "FollowerTactics power") : nullptr;
        auto *pkg = wrapper ? ClonePackage(tsun, kFirstShoutPackageLocalID + k) : nullptr;
        auto *condition = AddIsReferenceCondition(pkg);
        if (!pkg || !condition || !ProveCopy(pkg, "Shout", wrapper, i) || !SetPackageTarget(pkg, nullptr))
        {
            logger::error("packages: voice slot {} could not be made -- cast and power rules stay off", i);
            return;
        }
        // Weapon Drawn, which the ESP-era records did not have. Tried for
        // the 0.3 to 1.5 s between arming and BeginCastVoice, on the guess
        // that the AI was sheathing first. It was not: measured 2026-09-08
        // with the weapon state logged, "drawn" at arming, at pick-up and at
        // begin, and the delay unchanged (0.5 to 1.0 s, once 2.9 s, the same
        // as a hand cast). The delay is the AI's own start-up. The flag is
        // kept because this is the configuration that was verified, and a
        // procedure that needs no hands has no use for putting them away.
        pkg->packData.packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kWeaponDrawn);
        g_slots[i] = pkg;
        g_pool[i].condition = condition;
        g_pool[i].wrapper = wrapper;
        g_pool[i].spell = wrapper->GetFormID();
    }

    logger::info("packages: {} UseMagic slots and {} Shout slots with wrappers made in memory", kSpellSlots,
                 kVoiceSlots);
    g_available = ResolveOverrideLists() > 0;
}

bool PackagesAvailable()
{
    return g_available;
}

} // namespace ft::game
