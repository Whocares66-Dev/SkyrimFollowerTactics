#include "game/Packages.h"

#include "game/Forms.h"
#include "game/Log.h"
#include "game/Sensors.h"
#include "game/Sheet.h"
#include "game/Util.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft::game
{
namespace
{

bool g_available = false;
// What each follower's records are copied from and first pointed at, looked
// up at load.
RE::TESPackage *g_mercer = nullptr;
RE::TESPackage *g_tsun = nullptr;
RE::TESForm *g_canary = nullptr;

// How long the AI gets to START the cast before the record is taken back.
// Measured: every cast that happened fired 0.65-2.2 s after arming; the ones
// that did not had not started by 4 s either. The window only costs anything
// in that second case -- it is how long they stand held and unreactive -- so
// it is set just above the slowest measured start.
constexpr double kArmWindowSeconds = 2.5;
// A shout slot releases on the voice's fire event. Measured (2026-09-05,
// Voice of the Emperor through a one-word wrapper): request to fire 0.3 s
// and 1.5 s, the shout animation itself 0.3 s. Three seconds is the never-
// started case, as 2.5 s is for a hand cast; a bit longer because the AI
// had a second's hesitation on one of the two.
constexpr double kVoiceArmWindowSeconds = 3.0;
// A power attack's: the AI's pick-up, as for a cast, and the weapon drawn if
// it is away. Stepped back once the swing is seen, so the swing runs out.
constexpr double kWeaponArmWindowSeconds = 3.0;
constexpr double kWeaponSwingSeconds = 3.0;

// The condition, as an owned resource.
//
// Pointing a slot's condition at a follower is what makes their package's
// condition pass, and forgetting to clear it is the one mistake this file
// must not be able to make: they would pass that condition on every
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
// them.
class SlotLease
{
  public:
    SlotLease(RE::Actor *actor, RE::TESConditionItem *condition)
        : actor_(actor->GetHandle()), id_(actor->GetFormID()), condition_(condition)
    {
        condition_->data.functionData.params[0] = actor;
        log::packages.debug("{:08X} holds the condition (leased)", id_);
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
        log::packages.debug("{:08X} releases the condition (lease ended)", id_);

        // Without this they stay in the package until the AI's own next
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

// A slot is one of a follower's package records and, while a cast is in
// flight, their lease on it. A slot with no lease is idle, whatever spell it
// was last pointed at.
struct Slot
{
    RE::TESPackage *package = nullptr;
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
    // The voice type the follower's record had before the lease lent them
    // one the shout words are recorded in (see LendShoutVoice). Null when
    // nothing is lent; restored on release.
    RE::TESNPC *voiceOf = nullptr;
    RE::BGSVoiceType *ownVoice = nullptr;
    // How many of the scroll the actor carried when the read was asked for;
    // 0 for a spell. See SpendScroll.
    std::int32_t scrollsBefore = 0;
    // For a follower outside the vanilla alias: the record was put at the
    // front of one of their alias instances' package arrays (PutOnStack),
    // and comes off on release by a fresh walk of those arrays -- never a
    // stored pointer, since the arrays are the actor's and go with them.
    bool onStack = false;
    // The record's one condition; the lease points its parameter at the
    // holder.
    RE::TESConditionItem *condition = nullptr;
    std::optional<SlotLease> lease;
    double armedAt = 0.0;
    double until = 0.0;
    bool seenRunning = false;

    // A concentration stream: the fire event is its start, not a release.
    bool sustained = false;
    float sustain = 0.0f; // how long it was asked to run
    // A power attack's price and reach, and the follower's stamina and
    // distance to the target when it was asked: what rule.resolved sets
    // beside the same at the end.
    float staminaCost = 0.0f;
    float reach = 0.0f;
    float staminaAtArm = -1.0f;
    float distanceAtArm = -1.0f;
    float headingAtArm = -1.0f; // degrees between their facing and the target
    bool streaming = false;     // our fire event has been seen
    // Whom the stream is aimed at, so it can stop when they are dead.
    RE::ActorHandle target;

    // Set from the animation thread; read and cleared by the tick. The ONLY
    // things the sink writes. `fired`: our spell left the follower's hand.
    // `stopped`: a CastStop arrived after that -- for a stream, its end.
    // `begun`: a BeginCast event (voice or either hand), so the deadline can
    // step back and let a cast that has started finish, whatever it takes.
    std::atomic<bool> fired{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> begun{false};
    // What the sink READS, and the only things it reads of the slot: whose
    // the lease is, which spell the slot casts, which shout it shouts (0 for
    // none). Set by Arm once the lease is held, cleared by Release before
    // it goes; the lease, `spell` and `shouting` themselves are the game
    // thread's and are reset under the sink's feet.
    std::atomic<std::uint32_t> holder{0};
    std::atomic<std::uint32_t> spellId{0};
    std::atomic<std::uint32_t> shoutId{0};
    bool extended = false;
    // A power attack's record (RequestPowerAttack), and whether its swing has
    // been seen. Beside the other flags: between the pointers they padded
    // the slot past what the linter allows.
    bool weapon = false;
    bool swinging = false;
    // Whom the record was aimed at when armed (the holder, for a self-cast)
    // and the rule that asked for it (given to Arm; -1 when idle): what the
    // release reports as rule.resolved. Kept as an id because the handle
    // above may not outlive the target.
    std::uint32_t targetId = 0;
    int ruleIndex = -1;
    std::string ruleName;
    // A power attack's target input is "Target to Attack" where the others'
    // is "Target". The attack the follower was in when it was armed, so a
    // swing of their own already running is not taken for ours; and the
    // event of ours, once seen.
    const char *targetInput = "Target";
    const RE::BGSAttackData *attackAtArm = nullptr;
    // The override list the record was put in (FindStack), a form that
    // outlives the actor and the load, so it is kept to take the record out;
    // and where it went, which rule.resolved reports.
    RE::BGSListForm *overrideList = nullptr;
    const char *placedIn = nullptr;
    std::string attackEvent;

    [[nodiscard]] bool Busy() const noexcept
    {
        return lease.has_value();
    }
};
// One follower's records: a UseMagic package for a spell, a Shout package
// with its wrapper for a power or a shout, and a UseWeapon package for a
// power attack, which has no package when its copy could not be made.
struct Kit
{
    Slot spell;
    Slot voice;
    Slot weapon;
};

// How many power attack records are held: the pacing thread's question.
std::atomic<int> g_weaponLeases{0};

// Every follower's records by reference ID, until the game quits: forms are
// never deleted, so the map is never pruned either. Only the game thread
// inserts, under the lock, and it reads without; a reader on another thread
// -- the animation sink, an equip detour -- takes the lock shared. A kit
// never moves once made.
std::unordered_map<RE::FormID, std::unique_ptr<Kit>> g_kits;
std::shared_mutex g_kitsMutex;
// Followers whose records could not be made, so the tick does not make
// half a set again every half second. Game thread only.
std::unordered_set<RE::FormID> g_kitsFailed;

// Game thread.
Kit *KitOf(RE::FormID id)
{
    const auto it = g_kits.find(id);
    return it == g_kits.end() ? nullptr : it->second.get();
}

// Any thread.
Kit *SharedKitOf(RE::FormID id)
{
    std::shared_lock lock(g_kitsMutex);
    return KitOf(id);
}

// Game thread.
template <class Fn> void ForEachSlot(Fn &&fn)
{
    for (auto &entry : g_kits)
    {
        fn(entry.second->spell);
        fn(entry.second->voice);
        fn(entry.second->weapon);
    }
}

std::uint32_t PackageId(const Slot &slot)
{
    return slot.package ? slot.package->GetFormID() : 0;
}

// The release signals come from their animation graph.
//
// A UseMagic package does NOT complete after its cast (measured: still their
// current package four seconds later, with them standing idle), so the end of
// a cast has to be observed. The graph emits MRh_SpellFire_Event /
// MLh_SpellFire_Event when a spell leaves a hand, and CastStop when a cast
// ends. Both fire for EVERY spell they cast, their own combat spells included,
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

        auto *kit = SharedKitOf(who);
        if (!kit)
            return RE::BSEventNotifyControl::kContinue;
        for (auto *slotPtr : {&kit->spell, &kit->voice})
        {
            auto &slot = *slotPtr;
            if (slot.holder.load(std::memory_order_acquire) != who)
                continue;
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
            const std::uint32_t shoutId = slot.shoutId.load(std::memory_order_relaxed);
            if (voice && shoutId != 0)
            {
                auto *actor = const_cast<RE::TESObjectREFR *>(ev->holder)->As<RE::Actor>();
                const auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
                const auto *high = process ? process->high : nullptr;
                const auto *voiceItem =
                    actor ? actor->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kPowerOrShout] : nullptr;
                const auto *caster =
                    actor ? actor->GetActorRuntimeData().magicCasters[RE::Actor::SlotTypes::kPowerOrShout] : nullptr;
                const bool ourVoice = high && high->currentShout && high->currentShout->GetFormID() == shoutId;
                log::packages.at(ourVoice ? log::Level::Info : log::Level::Debug,
                                 "anim {:08X}: voice fired: shout {:08X} variation {} level {}, voice slot "
                                 "holds {:08X}, caster spell {:08X} -- {}",
                                 who, high && high->currentShout ? high->currentShout->GetFormID() : 0,
                                 high ? static_cast<std::int32_t>(high->currentShoutVariation) : -99,
                                 actor ? actor->GetCurrentShoutLevel() : -99, voiceItem ? voiceItem->GetFormID() : 0,
                                 caster && caster->currentSpell ? caster->currentSpell->GetFormID() : 0,
                                 ourVoice ? "OURS" : "not ours");
                if (ourVoice)
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
            const bool ours = firedID == slot.spellId.load(std::memory_order_relaxed);
            log::packages.at(ours ? log::Level::Info : log::Level::Debug,
                             "anim {:08X}: {} hand fired {:08X} \"{}\" -- {}", who, right ? "right" : "left", firedID,
                             log::NameOf(spell), ours ? "OURS" : "the follower's own, ignored");
            if (ours)
                slot.fired.store(true, std::memory_order_relaxed);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
SpellFireSink g_fireSink;

// Where the PackageTarget sits inside a TargetSelector input's data, learned
// at load by finding the canary rather than hardcoded:
// BGSPackageDataTargetSelector is not mapped by CommonLibSSE, and a wrong
// write here corrupts a live game. The form itself is then read and written
// through CommonLibSSE's PackageTarget (target union at +08), which the
// calibration checks the canary against.
constexpr std::size_t kNotCalibrated = static_cast<std::size_t>(-1);
std::size_t g_spellOuter = kNotCalibrated;
constexpr std::size_t kTargetUnionOffset = 8; // PackageTarget::target, RE/T/TESPackage.h

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
// against a bear, short enough that the AI has them back for the next turn.
// (Later, perhaps a random length within a range.)
constexpr float kDefaultSustainSeconds = 3.0f;
std::int8_t g_typeSpecificReference = -1;
std::int8_t g_typeSelf = -1;
// The type of a target input that names one form, as a Spell input names its
// spell (Mercer's Nightingale Strife): what a power attack's Weapon Type is
// set to, naming the weapon in the right hand.
std::int8_t g_typeObjectId = -1;
constexpr const char *kWeaponTypeInput = "Weapon Type";
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
        log::packages.debug("{} name map: absent", which);
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
    log::packages.debug("{} name map: {}", which, names.empty() ? "(empty)" : names);
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

RE::PackageTarget *TargetOfInput(RE::TESPackage *pkg, const char *inputName);

// Point a named TargetSelector input -- the UseMagic template's "Spell", the
// Shout template's "Shout" -- at a form. Both are the same kind of input, so
// the offset the canary found on "Spell" serves both.
bool SetPackageInput(RE::TESPackage *pkg, const char *inputName, RE::TESForm *form)
{
    auto *target = form ? TargetOfInput(pkg, inputName) : nullptr;
    if (!target)
        return false;
    target->target.object = form;
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

    log::packages.debug("probe: CastTimeMin +00 {}", HexDump(lo, 32));
    log::packages.debug("probe: CastTimeMax +00 {}", HexDump(hi, 32));

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

// Where, inside an Int input, its number sits, and inside a Location input
// the pointer to its PackageLocation: found at load on values vanilla records
// were authored with (CalibrateWeapon), as the Spell and CastTime layouts are.
std::size_t g_intOffset = kNotCalibrated;
std::size_t g_locationOffset = kNotCalibrated;

RE::IPackageData *NamedInput(RE::TESPackage *pkg, const char *inputName)
{
    auto *custom = pkg ? skyrim_cast<RE::TESCustomPackageData *>(pkg->data) : nullptr;
    std::int8_t uid = 0;
    if (!custom || !FindInputUID(custom, inputName, uid))
        return nullptr;
    return InputByUID(custom, uid);
}

std::int32_t *IntOfInput(RE::TESPackage *pkg, const char *inputName)
{
    auto *input = g_intOffset != kNotCalibrated ? NamedInput(pkg, inputName) : nullptr;
    return input ? reinterpret_cast<std::int32_t *>(reinterpret_cast<std::uintptr_t>(input) + g_intOffset) : nullptr;
}

// Read as SetPackageBool writes: bit 1 of the data word, on an input whose
// type name says Bool.
std::optional<bool> BoolOfInput(RE::TESPackage *pkg, const char *inputName)
{
    auto *input = NamedInput(pkg, inputName);
    if (!input || input->GetTypeName() != "Bool")
        return std::nullopt;
    return (static_cast<RE::BGSPackageDataBool *>(input)->data.i & 0x2u) != 0;
}

RE::PackageLocation *LocationOfInput(RE::TESPackage *pkg, const char *inputName)
{
    auto *input = g_locationOffset != kNotCalibrated ? NamedInput(pkg, inputName) : nullptr;
    if (!input)
        return nullptr;
    const std::uintptr_t p =
        *reinterpret_cast<std::uintptr_t *>(reinterpret_cast<std::uintptr_t>(input) + g_locationOffset);
    return LooksLikePointer(p) ? reinterpret_cast<RE::PackageLocation *>(p) : nullptr;
}

// Aim a slot's target input -- "Target", or a power attack's "Target to
// Attack" -- at an actor, or back at Self with nullptr.
bool SetPackageTarget(RE::TESPackage *pkg, RE::Actor *target, const char *inputName = "Target")
{
    auto *pt = g_targetCalibrated ? TargetOfInput(pkg, inputName) : nullptr;
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

// Point a named target input at one form, its type included: a Spell input
// holds that type already, a power attack's Weapon Type holds an object type
// until this names the weapon.
bool SetPackageObject(RE::TESPackage *pkg, const char *inputName, RE::TESForm *form)
{
    auto *pt = g_typeObjectId >= 0 && form ? TargetOfInput(pkg, inputName) : nullptr;
    if (!pt)
        return false;
    pt->targType = g_typeObjectId;
    pt->target.object = form;
    return true;
}

// The Shout procedure fires only a shout the actor HAS (CK wiki), and an
// NPC's shouts live on the base record's spell list, where the Greybeards'
// are. So a wrapper goes into that list for the lease and comes out after.
// The base is shared by every actor spawned from it; a wrapper left behind
// there would be listed under Shouts for all of them, which is why the
// sweep in the tick removes any idle wrapper from a follower casting nothing.
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
    // Done before the log line, not inside its arguments: this has to happen
    // whether or not anything is written.
    const bool added = list->AddShout(wrapper);
    log::packages.debug("wrapper {:08X} {} to {:08X}'s shout list", wrapper->GetFormID(), added ? "added" : "NOT added",
                        actor->GetFormID());
}

void TakeWrapper(RE::Actor *actor, RE::TESShout *wrapper)
{
    if (!actor)
        return;
    // The Shout procedure readies what it fires, and a save writes the voice
    // slot as a bare form ID that a load looks up again with no type check
    // (docs/MAGIC.md "Forms at runtime"). So a power's wrapper does not stay
    // there once the list gives it up, nor does anything of ours an old save
    // put back; a real shout or power is the follower's and stays. A plain
    // write: Papyrus's UnequipShout runs a frame later, after the save
    // message's release has already let the file be written.
    auto &voice = actor->GetActorRuntimeData().selectedPower;
    if (voice && MadeByUs(voice->GetFormID()))
    {
        log::packages.debug("{:08X}'s voice slot held {:08X}, ours -- cleared", actor->GetFormID(), voice->GetFormID());
        voice = nullptr;
    }
    auto *list = actor->GetActorBase() ? actor->GetActorBase()->actorEffects : nullptr;
    if (!list || !wrapper || !list->GetIndex(wrapper).has_value())
        return;
    const bool removed = list->RemoveShout(wrapper);
    log::packages.debug("wrapper {:08X} {} from {:08X}'s shout list", wrapper->GetFormID(),
                        removed ? "removed" : "NOT removed", actor->GetFormID());
}

// Is this follower already casting through one of their records? A second
// request before the first resolves would put two records on them.
bool AlreadyCasting(const Kit *kit)
{
    return kit && (kit->spell.Busy() || kit->voice.Busy() || kit->weapon.Busy());
}

// The one way a record comes back. Destroying the lease clears the condition
// and re-evaluates; nothing else here touches the condition on the way out.
// The words of a shout are dialogue: lines under the Voice Powers quest
// (topics Shout01a..03, subtype VoicePowerStart), each conditioned on the
// shouter's voice type being in a list -- VoicesPlayer, the ten voice types
// a player can have, and VoicePowerVoicesList, those plus five story
// characters. A follower whose voice type is in neither has no line, and
// shouts in silence, or in whatever else the load order plays. So for the
// lease the record is lent the player voice type of the follower's race
// and sex -- the race record names them, an Orc's, a Khajiit's, an elf's
// haughty one, EvenToned for the humans -- which has every word recorded;
// EvenToned stands in for a race whose own is not on the list. The
// condition reads the record's voice type at the shout, and any bark in
// the same two seconds comes out in the lent voice, which is the cost.
// The record is changed in memory only: no NPC change flag covers the
// voice type (TESNPC::ChangeFlags), so a save made mid-lease carries the
// record's own, and a load reads it back.
constexpr RE::FormID kVoicesPlayer = 0x00068ACA;
constexpr RE::FormID kVoicePowerVoicesList = 0x0010D29D;
constexpr RE::FormID kFemaleEvenToned = 0x00013ADD;
constexpr RE::FormID kMaleEvenToned = 0x00013AD2;

void LendShoutVoice(Slot &slot, RE::Actor *actor)
{
    auto *base = actor ? actor->GetActorBase() : nullptr;
    if (!base)
        return;
    auto *players = RE::TESForm::LookupByID<RE::BGSListForm>(kVoicesPlayer);
    auto *powers = RE::TESForm::LookupByID<RE::BGSListForm>(kVoicePowerVoicesList);
    const auto hasWords = [&](const RE::BGSVoiceType *voice) {
        return voice && ((players && players->HasForm(voice)) || (powers && powers->HasForm(voice)));
    };
    RE::BGSVoiceType *own = base->voiceType;
    if (hasWords(own))
        return; // their own voice has the words
    const bool female = base->GetSex() == RE::SEX::kFemale;
    const auto *race = actor->GetRace();
    RE::BGSVoiceType *lent = race ? race->defaultVoiceTypes[female ? RE::SEXES::kFemale : RE::SEXES::kMale] : nullptr;
    if (!hasWords(lent))
        lent = RE::TESForm::LookupByID<RE::BGSVoiceType>(female ? kFemaleEvenToned : kMaleEvenToned);
    if (!lent)
        return;
    slot.voiceOf = base;
    slot.ownVoice = own;
    base->voiceType = lent;
    log::packages.debug("{:08X} lends {} the {} voice for the shout (own: {})", PackageId(slot), Describe(actor),
                        lent->GetFormEditorID() ? lent->GetFormEditorID() : "?",
                        own && own->GetFormEditorID() ? own->GetFormEditorID() : "none");
}

void ReturnShoutVoice(Slot &slot)
{
    if (slot.voiceOf)
        slot.voiceOf->voiceType = slot.ownVoice;
    slot.voiceOf = nullptr;
    slot.ownVoice = nullptr;
}

// A quest alias's override package lists are not on the alias: its loader
// (24013) files a BGSOverridePackCollection for it in a global map keyed by
// the alias, and the follower alias's combat override list is where a
// follower's package in a fight comes from (docs/ATTACK.md "Facing").
// CommonLib has the table's layout (BSTScatterTable, RE/B/BSTHashMap.h) but
// not this global, so the table is read through a copy of that layout,
// anchored on the capacity the loader reads (Address Library 369298, 0x0C
// into the table): an address known for 1.6.1170 alone (docs/VERSIONS.md).
struct AliasOverrideEntry
{
    const RE::BGSRefAlias *alias;
    const RE::BGSOverridePackCollection *lists;
    const AliasOverrideEntry *next; // null in an empty slot; the sentinel ends a chain
};
struct AliasOverrideMap
{
    std::uint64_t pad00;
    std::uint32_t pad08;
    std::uint32_t capacity;
    std::uint32_t free;
    std::uint32_t good;
    const AliasOverrideEntry *sentinel;
    std::uint64_t pad20;
    const AliasOverrideEntry *entries;
};
static_assert(sizeof(AliasOverrideEntry) == 0x18);
static_assert(offsetof(AliasOverrideMap, capacity) == 0x0C && offsetof(AliasOverrideMap, sentinel) == 0x18 &&
              offsetof(AliasOverrideMap, entries) == 0x28);
// Nothing reads the map unless CheckAliasOverrideLists found it as expected.
bool g_aliasOverrideListsRead = false;

// AE alone: the ID means something else to the Special Edition's library.
const AliasOverrideMap &AliasOverrideTable()
{
    static REL::Relocation<const AliasOverrideMap *> at{REL::ID(369298), -0x0C};
    return *at.get();
}

// An alias's override lists: null when it has none, or the map is not read.
// The keys are reference aliases, whose base is their first and only one, so
// an instance's base alias pointer compares equal to its key.
const RE::BGSOverridePackCollection *OverrideListsOf(const RE::BGSBaseAlias *alias)
{
    if (!g_aliasOverrideListsRead || !alias)
        return nullptr;
    const AliasOverrideMap &map = AliasOverrideTable();
    for (std::uint32_t i = 0; i < map.capacity; ++i)
    {
        const AliasOverrideEntry &entry = map.entries[i];
        if (entry.next && entry.alias == alias)
            return entry.lists;
    }
    return nullptr;
}

// The package data a power attack's record runs with, by where it is put.
// On an alias's package array it needs IgnoreCombat, or the combat override
// is picked over it in a fight, and then nothing faces the follower but
// TurnToward. In an override list it takes the data of the one vanilla
// UseWeapon record kept in such a list, Karliah's in Blindsighted: Weapon
// Drawn and interrupt override Combat, without IgnoreCombat, so combat goes
// on facing and moving the follower. Copied, not written: the file's
// interrupt override values and the library's names for them do not agree
// (docs/MAGIC.md "Forms at runtime").
struct PackageData
{
    decltype(RE::PACKAGE_DATA::packFlags) flags;
    decltype(RE::PACKAGE_DATA::interruptOverrideType) interruptOverride;
    decltype(RE::PACKAGE_DATA::foBehaviorFlags) interruptFlags;
};
constexpr std::uint32_t kKarliahCombatOverrideID = 0x000FCC2A; // TG08BKarliahUseWeaponCombatOverride
std::optional<PackageData> g_weaponOnStack;
std::optional<PackageData> g_weaponInOverrideList;

PackageData DataOf(const RE::TESPackage &pkg)
{
    return {pkg.packData.packFlags, pkg.packData.interruptOverrideType, pkg.packData.foBehaviorFlags};
}

void SetData(RE::TESPackage &pkg, const PackageData &data)
{
    pkg.packData.packFlags = data.flags;
    pkg.packData.interruptOverrideType = data.interruptOverride;
    pkg.packData.foBehaviorFlags = data.interruptFlags;
}

template <class T> void PutFirst(RE::BSTArray<T *> &items, std::type_identity_t<T *> item)
{
    std::vector<T *> keep(items.begin(), items.end());
    items.clear();
    items.push_back(item);
    for (auto *p : keep)
        if (p != item)
            items.push_back(p);
}

template <class T> bool TakeOut(RE::BSTArray<T *> &items, std::type_identity_t<const T *> item)
{
    std::vector<T *> keep;
    for (auto *p : items)
        if (p != item)
            keep.push_back(p);
    if (keep.size() == items.size())
        return false;
    items.clear();
    for (auto *p : keep)
        items.push_back(p);
    return true;
}

std::string AliasName(const RE::BGSRefAliasInstanceData &inst)
{
    return fmt::format("{} \"{}\" alias {}", inst.quest ? fmt::format("{:08X}", inst.quest->GetFormID()) : "?",
                       inst.quest && inst.quest->GetFormEditorID() ? inst.quest->GetFormEditorID() : "",
                       inst.alias ? inst.alias->aliasID : 0xFFFFFFFF);
}

// Where a record is put: one of the actor's alias package arrays, or an
// override list, a form shared by everyone whose alias or record names it.
struct Stack
{
    RE::BSTArray<RE::TESPackage *> *packages = nullptr;
    RE::BGSListForm *list = nullptr;
    const char *kind = nullptr; // "alias packages", or which override list
    std::string owner;
};

// How a record reaches a follower: at the front of the list their running
// package came from, where it is evaluated first; the lease's condition
// gates it. Every alias an actor fills is instanced for them as an array of
// packages on the actor (ExtraAliasInstanceArray). When `overrideLists`, an
// override list that holds the running package is looked for next, on each
// alias they fill and on their record: in a fight that is the combat
// override list. Failing both, the fullest array, which is the quest that
// drives them. The created-package route (PutCreatedPackage) was tried and
// is not evaluated in a fight.
Stack FindStack(RE::Actor *actor, bool overrideLists)
{
    Stack fullest;
    const auto *running = actor->GetCurrentPackage();
    auto *extra = actor->extraList.GetByType<RE::ExtraAliasInstanceArray>();
    if (extra)
    {
        std::uint32_t most = 0;
        for (const auto *inst : extra->aliases)
        {
            if (!inst || !inst->instancedPackages)
                continue;
            auto *packages = const_cast<RE::BSTArray<RE::TESPackage *> *>(inst->instancedPackages);
            if (running && std::find(packages->begin(), packages->end(), running) != packages->end())
                return {packages, nullptr, "alias packages", AliasName(*inst)};
            if (packages->size() > most)
            {
                most = packages->size();
                fullest = {packages, nullptr, "alias packages", AliasName(*inst)};
            }
        }
    }
    if (overrideLists && running)
    {
        Stack found;
        const auto holding = [running, &found](const RE::BGSOverridePackCollection *lists, std::string owner) {
            if (!lists)
                return false;
            const std::array<std::pair<RE::BGSListForm *, const char *>, 4> named{
                {{lists->enterCombatOverRidePackList, "combat override list"},
                 {lists->spectatorOverRidePackList, "spectator override list"},
                 {lists->observeCorpseOverRidePackList, "observe corpse override list"},
                 {lists->guardWarnOverRidePackList, "guard warn override list"}}};
            for (const auto &[list, kind] : named)
            {
                if (list && std::find(list->forms.begin(), list->forms.end(), running) != list->forms.end())
                {
                    found = {nullptr, list, kind, std::move(owner)};
                    return true;
                }
            }
            return false;
        };
        if (extra)
        {
            for (const auto *inst : extra->aliases)
                if (inst && holding(OverrideListsOf(inst->alias), AliasName(*inst)))
                    return found;
        }
        if (holding(actor->GetActorBase(), "their record"))
            return found;
    }
    return fullest;
}

void PutOnStack(const Stack &stack, RE::TESPackage *pkg)
{
    if (stack.list)
        PutFirst(stack.list->forms, pkg);
    else if (stack.packages)
        PutFirst(*stack.packages, pkg);
    else
        return;
    log::packages.debug("{:08X} at the front of the {}{} of {} ({} entries)", pkg->GetFormID(), stack.kind,
                        stack.list ? fmt::format(" {:08X}", stack.list->GetFormID()) : "", stack.owner,
                        stack.list ? stack.list->forms.size() : stack.packages->size());
}

void TakeOffStack(RE::Actor *actor, RE::TESPackage *pkg)
{
    auto *extra = actor ? actor->extraList.GetByType<RE::ExtraAliasInstanceArray>() : nullptr;
    if (!extra)
        return;
    for (const auto *inst : extra->aliases)
    {
        if (inst && inst->instancedPackages)
            TakeOut(*const_cast<RE::BSTArray<RE::TESPackage *> *>(inst->instancedPackages), pkg);
    }
}

// A scroll read is spent. If the engine spent it on the package cast the
// count has dropped by one and nothing is done; if not, one is taken off
// by hand, so a Scroll rule can never read the same scroll for free.
void SpendScroll(Slot &slot, RE::Actor *actor)
{
    if (slot.scrollsBefore <= 0 || !actor)
        return;
    auto *scroll = RE::TESForm::LookupByID<RE::ScrollItem>(slot.spell);
    if (!scroll)
        return;
    const auto counts = actor->GetInventoryCounts([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::Scroll); });
    const auto it = counts.find(scroll);
    const std::int32_t now = it != counts.end() ? it->second : 0;
    if (now < slot.scrollsBefore)
    {
        log::packages.event(log::Level::Info, "scroll.spent", actor,
                            {{"formId", log::Id(scroll->GetFormID())},
                             {"itemName", log::NameOf(scroll)},
                             {"by", "the engine"},
                             {"carriedBefore", slot.scrollsBefore},
                             {"carriedAfter", now}},
                            "scroll: the engine spent {} ({} -> {})", log::NameOf(scroll), slot.scrollsBefore, now);
    }
    else
    {
        actor->RemoveItem(scroll, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        log::packages.event(log::Level::Info, "scroll.spent", actor,
                            {{"formId", log::Id(scroll->GetFormID())},
                             {"itemName", log::NameOf(scroll)},
                             {"by", "hand"},
                             {"carriedBefore", now},
                             {"carriedAfter", now - 1}},
                            "scroll: {} spent by hand ({} -> {})", log::NameOf(scroll), now, now - 1);
    }
    slot.scrollsBefore = 0;
}

// What came of a requested cast, as the rule that asked for it reads it:
// cast or not, whether the AI picked the package up, and the release's own
// reason. Before Release, which forgets all of it. `holder` is null when the
// follower has gone, and their id is given instead.
void ReportResolved(const Slot &slot, RE::Actor *holder, std::uint32_t holderId, const char *reason, double seconds)
{
    const bool cast = slot.fired.load(std::memory_order_relaxed);
    const char *kind = "spell";
    if (slot.weapon)
        kind = "power attack";
    else if (slot.wrapper)
        kind = slot.power ? "power" : "shout";
    else if (RE::TESForm::LookupByID<RE::ScrollItem>(slot.spell))
        kind = "scroll";
    // A power attack is made or not; its form is the weapon in the right hand.
    const char *outcome = slot.weapon ? (cast ? "made" : "not-made") : (cast ? "cast" : "not-cast");

    std::vector<log::Field> fields;
    if (!holder)
        fields.emplace_back("followerId", log::Id(holderId));
    fields.emplace_back("ruleIndex", slot.ruleIndex);
    fields.emplace_back("ruleName", slot.ruleName);
    fields.emplace_back("kind", kind);
    log::AppendForm(fields, "formId", "formName", slot.spell);
    log::AppendActor(fields, "targetFormId", "targetBaseFormId", "targetName", slot.targetId);
    fields.emplace_back("outcome", outcome);
    fields.emplace_back("pickedUp", slot.seenRunning);
    fields.emplace_back("placedIn", slot.placedIn ? slot.placedIn : "nowhere");
    std::string blowNote;
    if (slot.weapon)
    {
        // What a power attack not made could not do: pay, reach, or get
        // between swings. At the request and now; -1 with no one to measure.
        const auto target = slot.target.get();
        const auto *state = holder ? holder->AsActorState() : nullptr;
        const double staminaNow =
            holder ? static_cast<double>(holder->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina)) : -1.0;
        const double distanceNow =
            holder && target ? static_cast<double>(holder->GetPosition().GetDistance(target->GetPosition())) : -1.0;
        const int attackState = state ? static_cast<int>(state->GetAttackState()) : -1;
        // The procedure attacks only a target within the attack's strike
        // angle of their heading (docs/ATTACK.md).
        const double headingNow =
            holder && target ? static_cast<double>(holder->GetHeadingAngle(target->GetPosition(), true)) : -1.0;
        fields.emplace_back("attackEvent", slot.attackEvent);
        fields.emplace_back("staminaCost", static_cast<double>(slot.staminaCost));
        fields.emplace_back("staminaAtRequest", static_cast<double>(slot.staminaAtArm));
        fields.emplace_back("staminaAtEnd", staminaNow);
        fields.emplace_back("reach", static_cast<double>(slot.reach));
        fields.emplace_back("distanceAtRequest", static_cast<double>(slot.distanceAtArm));
        fields.emplace_back("distanceAtEnd", distanceNow);
        fields.emplace_back("headingAtRequest", static_cast<double>(slot.headingAtArm));
        fields.emplace_back("headingAtEnd", headingNow);
        fields.emplace_back("attackStateAtEnd", attackState);
        blowNote = fmt::format(" (stamina {:.0f} -> {:.0f} for {:.0f}; distance {:.0f} -> {:.0f} of {:.0f}; facing "
                               "{:.0f} -> {:.0f} deg off; attack state {})",
                               slot.staminaAtArm, staminaNow, slot.staminaCost, slot.distanceAtArm, distanceNow,
                               slot.reach, slot.headingAtArm, headingNow, attackState);
    }
    fields.emplace_back("reason", reason);
    fields.emplace_back("durationS", seconds);
    log::packages.event(log::Level::Info, "rule.resolved", holder, fields,
                        "{} rule {} \"{}\": {} {} {} -- {}, after {:.1f} s{}",
                        holder ? log::NameOf(holder) : log::Id(holderId), slot.ruleIndex, slot.ruleName, kind,
                        log::NameOf(RE::TESForm::LookupByID(slot.spell)), outcome, reason, seconds, blowNote);
}

// Turning an actor toward a point, the engine's own way: the UseWeapon
// procedure asks it on every update out of combat and never in one, where it
// leaves turning to the combat controller (docs/ATTACK.md "Facing") -- which
// a record with IgnoreCombat suspends. 37834 hands the actor's movement
// controller the point, a tolerance in radians (the procedure's,
// fCombatAngleTolerance degrees) and two factors of 1; 37839 takes the point
// back. Both read from 1.6.1170. The Special Edition addresses are not known,
// so off AE neither is called, and a power attack still needs the follower
// to be facing the enemy already.
void TurnToward(RE::Actor *actor, RE::TESObjectREFR *target)
{
    if (!actor || !target || !REL::Module::IsAE())
        return;
    using func_t = void (*)(RE::Actor *, const RE::NiPoint3 *, float, float, float);
    static REL::Relocation<func_t> turn{REL::ID(37834)};
    constexpr float kRadiansPerDegree = 0.017453292f;
    // The target's own position, as the procedure passes it, not a copy: the
    // controller may keep the pointer until StopTurning.
    turn(actor, &target->data.location, GameSetting("fCombatAngleTolerance", 1.0f) * kRadiansPerDegree, 1.0f, 1.0f);
}

void StopTurning(RE::Actor *actor)
{
    if (!actor || !REL::Module::IsAE())
        return;
    using func_t = void (*)(RE::Actor *);
    static REL::Relocation<func_t> stop{REL::ID(37839)};
    stop(actor);
}

void Release(Slot &slot)
{
    if (slot.weapon && slot.lease)
    {
        g_weaponLeases.fetch_sub(1, std::memory_order_relaxed);
        if (auto holder = slot.lease->Actor())
            StopTurning(holder.get());
    }
    // The record comes off the stack it was put on, if it was, while the
    // lease still knows whose.
    if (slot.onStack && slot.lease)
    {
        if (auto actor = slot.lease->Actor())
            TakeOffStack(actor.get(), slot.package);
    }
    slot.onStack = false;
    if (slot.overrideList)
        TakeOut(slot.overrideList->forms, slot.package);
    slot.overrideList = nullptr;
    slot.placedIn = nullptr;
    // The wrapper comes off before the lease goes: the lease is what still
    // knows whose list it is in.
    if (slot.wrapper && slot.lease)
    {
        if (auto actor = slot.lease->Actor())
            TakeWrapper(actor.get(), slot.wrapper);
    }
    ReturnShoutVoice(slot);
    if (slot.power)
    {
        slot.power->data.spellType = slot.powerType;
        slot.power = nullptr;
    }
    // The sink stops looking before the lease goes.
    slot.holder.store(0, std::memory_order_release);
    slot.spellId.store(0, std::memory_order_relaxed);
    slot.shoutId.store(0, std::memory_order_relaxed);
    slot.shouting = nullptr;
    // The lease's destructor asks the AI to re-evaluate; the record is off
    // the stack by then.
    slot.lease.reset();
    slot.target = {};
    SetPackageTarget(slot.package, nullptr, slot.targetInput); // no target handle outlives its lease
    slot.fired.store(false, std::memory_order_relaxed);
    slot.stopped.store(false, std::memory_order_relaxed);
    slot.begun.store(false, std::memory_order_relaxed);
    slot.extended = false;
    slot.seenRunning = false;
    slot.streaming = false;
    slot.attackAtArm = nullptr;
    slot.swinging = false;
    slot.attackEvent.clear();
    slot.targetId = 0;
    slot.ruleIndex = -1;
    slot.ruleName.clear();
}

} // namespace

const char *ToString(CastRequest r) noexcept
{
    switch (r)
    {
    case CastRequest::Armed:
        return "cast requested";
    case CastRequest::NoPackages:
        return "no cast packages for this follower (see FollowerTactics.log)";
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
    return g_available && actor && AlreadyCasting(SharedKitOf(actor->GetFormID()));
}

bool IsOurCast(const RE::Actor *actor, std::uint32_t formID)
{
    const Kit *kit = g_available && actor && formID != 0 ? SharedKitOf(actor->GetFormID()) : nullptr;
    if (!kit)
        return false;
    for (const auto *slot : {&kit->spell, &kit->voice})
    {
        if (!slot->Busy())
            continue;
        if (slot->spell == formID)
            return true;
        if (slot->shouting && slot->shouting->GetFormID() == formID)
            return true;
        if (slot->wrapper && slot->wrapper->GetFormID() == formID)
            return true;
        if (slot->power && slot->power->GetFormID() == formID)
            return true;
    }
    return false;
}

bool HasCastForms(const RE::Actor *actor)
{
    return g_available && actor && SharedKitOf(actor->GetFormID()) != nullptr;
}

bool IsWrapperShout(std::uint32_t formID)
{
    std::shared_lock lock(g_kitsMutex);
    for (const auto &entry : g_kits)
        if (entry.second->voice.wrapper && entry.second->voice.wrapper->GetFormID() == formID)
            return true;
    return false;
}

bool IsLeasedPower(std::uint32_t formID)
{
    std::shared_lock lock(g_kitsMutex);
    for (const auto &entry : g_kits)
        if (entry.second->voice.power && entry.second->voice.power->GetFormID() == formID)
            return true;
    return false;
}

namespace
{
// The shared end of a request: the slot is pointed where it should be, and
// this points the slot's condition at the follower and asks the AI to look.
CastRequest Arm(Slot &slot, RE::Actor *actor, float sustain, double window, int ruleIndex, std::string_view ruleName)
{
    slot.armedAt = TacticsSeconds();
    slot.ruleIndex = ruleIndex;
    slot.ruleName = ruleName;
    // The window covers the AI's start-up latency. For a stream it is
    // extended when the stream actually starts (see the tick), so a stream
    // that never starts does not hold them for the sustain on top.
    slot.until = slot.armedAt + window;
    slot.sustain = sustain;
    slot.seenRunning = false;
    slot.fired.store(false, std::memory_order_relaxed);
    slot.stopped.store(false, std::memory_order_relaxed);
    slot.begun.store(false, std::memory_order_relaxed);
    slot.extended = false;

    // Registering the sink is what makes a fire event reach us at all. On
    // every request, not once per actor: the graph is rebuilt on a cell
    // change and a 3D reload, and a sink on the old one hears nothing; the
    // library's AddAnimationGraphEventSink looks for the sink first and
    // adds it only where it is missing (RE/A/Actor.cpp), so this costs a
    // walk of the graph's sinks and nothing else.
    if (actor->AddAnimationGraphEventSink(&g_fireSink))
        log::packages.debug("animation sink added on {:08X}", actor->GetFormID());

    // Onto the follower's stack, then the lease points the condition at
    // them in its constructor. From here on the record is theirs until the
    // lease is destroyed, and only that clears the condition.
    slot.lease.emplace(actor, slot.condition);
    // What the sink may read, the holder last: from here the sink looks.
    slot.spellId.store(slot.spell, std::memory_order_relaxed);
    slot.shoutId.store(slot.shouting ? slot.shouting->GetFormID() : 0, std::memory_order_relaxed);
    slot.holder.store(actor->GetFormID(), std::memory_order_release);
    // A power attack's record may go in an override list, with the package
    // data for it; a cast's stays on the alias arrays, the route verified
    // for casts.
    const bool overrideLists = slot.weapon && g_weaponOnStack && g_weaponInOverrideList;
    const Stack stack = FindStack(actor, overrideLists);
    if (overrideLists)
        SetData(*slot.package, stack.list ? *g_weaponInOverrideList : *g_weaponOnStack);
    PutOnStack(stack, slot.package);
    slot.onStack = stack.packages != nullptr;
    slot.overrideList = stack.list;
    slot.placedIn = stack.kind;
    if (!stack.packages && !stack.list)
        log::packages.warn("{} fills no alias with packages; the record has no way to them", Describe(actor));

    // Immediate, or they finish whatever they are doing first and the rule's
    // timing -- the entire point of this route -- is lost.
    actor->EvaluatePackage(/*immediate*/ true, /*resetAI*/ false);

    const auto *current = actor->GetCurrentPackage();
    log::packages.debug("current package after evaluate: {:08X} ({})", current ? current->GetFormID() : 0,
                        current == slot.package ? "OURS" : "not ours yet -- watching");
    slot.seenRunning = current == slot.package;

    return CastRequest::Armed;
}
} // namespace

CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId, float sustainSeconds,
                        bool dualCast, int ruleIndex, std::string_view ruleName)
{
    auto *kit = g_available && actor ? KitOf(actor->GetFormID()) : nullptr;
    if (!kit)
        return CastRequest::NoPackages;

    // Self, or someone else. Anyone else must be a loaded actor right now;
    // the record will hold a handle to them for the duration of the lease.
    RE::Actor *target = nullptr;
    if (targetId != 0 && targetId != actor->GetFormID())
    {
        target = RE::TESForm::LookupByID<RE::Actor>(targetId);
        if (!target || !target->Is3DLoaded())
        {
            log::packages.debug("target {:08X} is not a loaded actor", targetId);
            return CastRequest::TargetGone;
        }
        if (!g_targetCalibrated)
        {
            log::packages.debug("target input not calibrated; only self-casts are possible");
            return CastRequest::TargetGone;
        }
    }

    if (AlreadyCasting(kit))
        return CastRequest::AlreadyCasting;

    auto &slot = kit->spell;

    // Two spells can share a display name (Marcurio's heal is 0007231C, the
    // vanilla one 0002F3B8, both "Fast Healing"), so the form is what counts.
    if (slot.spell != spellFormID)
    {
        auto *wanted = RE::TESForm::LookupByID(spellFormID);
        if (!wanted || !SetPackageSpell(slot.package, wanted))
        {
            log::packages.debug("{:08X} still casts {:08X}; could not repoint to {:08X}", PackageId(slot), slot.spell,
                                spellFormID);
            return CastRequest::SpellNotInSlot;
        }
        slot.spell = spellFormID;
    }

    SetPackageTarget(slot.package, target);
    slot.target = target ? target->GetHandle() : RE::ActorHandle{};
    slot.targetId = target ? target->GetFormID() : actor->GetFormID();

    // Both hands or one: set on every request, since the last lease may have
    // left it either way.
    if (!SetPackageBool(slot.package, "DualCast", dualCast))
        log::packages.debug("{:08X} has no DualCast input to set{}", PackageId(slot),
                            dualCast ? " -- the cast will be one-handed" : "");
    else if (dualCast)
        log::packages.debug("{:08X} casts from both hands", PackageId(slot));

    // A concentration spell streams for as long as the procedure's CastTime
    // says. Set that to the sustain, and remember that the fire event is
    // not the end of this one.
    // A spell or a scroll: both MagicItems with a casting type.
    auto *spellItem = RE::TESForm::LookupByID<RE::MagicItem>(spellFormID);
    slot.sustained = spellItem && spellItem->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    // A scroll is spent by the read. Whether the engine spends it on a
    // package cast is the open question, so the count is kept and one is
    // taken by hand on the fire event if it did not drop (SpendScroll).
    slot.scrollsBefore = 0;
    if (auto *scroll = spellItem ? spellItem->As<RE::ScrollItem>() : nullptr)
    {
        const auto counts =
            actor->GetInventoryCounts([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::Scroll); });
        if (const auto it = counts.find(scroll); it != counts.end())
            slot.scrollsBefore = it->second;
    }
    const float sustain = sustainSeconds > 0.0f ? sustainSeconds : kDefaultSustainSeconds;
    if (slot.sustained)
    {
        if (SetPackageCastTime(slot.package, sustain))
            log::packages.debug("{:08X} sustains {} for {:.1f} s", PackageId(slot), log::NameOf(spellItem), sustain);
        else
            log::packages.debug("{:08X} cast time not calibrated; the stream will run the authored "
                                "{:.1f}-{:.1f} s",
                                PackageId(slot), kAuthoredCastTimeMin, kAuthoredCastTimeMax);
    }
    else
        SetPackageCastTime(slot.package, kAuthoredCastTimeMax);
    log::packages.info("{}: {:08X} casts {:08X} at {}", log::NameOf(actor), PackageId(slot), spellFormID,
                       target ? fmt::format("{:08X} \"{}\"", target->GetFormID(), log::NameOf(target))
                              : std::string("self"));

    return Arm(slot, actor, sustain, kArmWindowSeconds, ruleIndex, ruleName);
}

CastRequest RequestShout(RE::Actor *actor, std::uint32_t formID, std::uint32_t targetId, int ruleIndex,
                         std::string_view ruleName)
{
    auto *kit = g_available && actor ? KitOf(actor->GetFormID()) : nullptr;
    if (!kit)
        return CastRequest::NoPackages;

    RE::Actor *target = nullptr;
    if (targetId != 0 && targetId != actor->GetFormID())
    {
        target = RE::TESForm::LookupByID<RE::Actor>(targetId);
        if (!target || !target->Is3DLoaded())
        {
            log::packages.debug("target {:08X} is not a loaded actor", targetId);
            return CastRequest::TargetGone;
        }
        if (!g_targetCalibrated)
        {
            log::packages.debug("target input not calibrated; only self-casts are possible");
            return CastRequest::TargetGone;
        }
    }

    if (AlreadyCasting(kit))
        return CastRequest::AlreadyCasting;

    auto &slot = kit->voice;
    auto *form = RE::TESForm::LookupByID(formID);
    auto *shout = form ? form->As<RE::TESShout>() : nullptr;
    auto *power = form ? form->As<RE::SpellItem>() : nullptr;
    if ((!shout && !power) || !slot.wrapper)
    {
        log::packages.debug("{:08X} has nothing to shout for {:08X}", PackageId(slot), formID);
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
        log::packages.debug("{:08X} wrapper {:08X} word one now casts {:08X} \"{}\" (type {} -> Voice for the "
                            "lease)",
                            PackageId(slot), slot.wrapper->GetFormID(), formID, log::NameOf(power),
                            static_cast<int>(slot.powerType));
    }
    else
    {
        slot.shouting = shout;
        log::packages.debug("{:08X} shouts {:08X} \"{}\" itself", PackageId(slot), formID, log::NameOf(shout));
    }

    // The package's Shout input: the wrapper for a power, the shout itself
    // for a shout. Written only when it changes, as the Spell input is.
    if (slot.spell != formID)
    {
        if (!SetPackageInput(slot.package, "Shout", slot.shouting))
        {
            log::packages.warn("{:08X} could not point its Shout input at {:08X}", PackageId(slot),
                               slot.shouting->GetFormID());
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

    SetPackageTarget(slot.package, target);
    slot.target = target ? target->GetHandle() : RE::ActorHandle{};
    slot.targetId = target ? target->GetFormID() : actor->GetFormID();
    slot.sustained = false;
    log::packages.info("{}: {:08X} voices {:08X} at {}", log::NameOf(actor), PackageId(slot), formID,
                       target ? fmt::format("{:08X} \"{}\"", target->GetFormID(), log::NameOf(target))
                              : std::string("self"));

    // The procedure fires only a shout the actor has: the wrapper is given
    // for the lease; a shout of their own they have already. A shout speaks
    // its words in a voice that has them.
    if (power)
        GiveWrapper(actor, slot.wrapper);
    else
        LendShoutVoice(slot, actor);

    return Arm(slot, actor, 0.0f, kVoiceArmWindowSeconds, ruleIndex, ruleName);
}

namespace
{
// The attack the follower's high process is in, or was last in: the race's
// attack data entry, which names the event and says power attack or not.
const RE::BGSAttackData *AttackDataOf(RE::Actor *actor)
{
    auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    auto *high = process ? process->high : nullptr;
    return high ? high->attackData.get() : nullptr;
}

// A power attack's lease. The procedure does complete once its one attack is
// counted (docs/ATTACK.md), but the condition still passes then and the AI
// would pick the package again, so the lease goes as soon as the swing has
// ended: a power attack seen after pick-up, then the attack state back at none.
void TickWeaponSlot(Slot &slot, double now)
{
    auto actor = slot.lease->Actor();
    if (!actor)
    {
        ReportResolved(slot, nullptr, slot.lease->FormID(), "holder vanished", now - slot.armedAt);
        Release(slot);
        return;
    }
    const bool running = actor->GetCurrentPackage() == slot.package;
    if (running && !slot.seenRunning)
    {
        slot.seenRunning = true;
        log::packages.debug("{} is RUNNING {:08X} (power attack) after {:.2f} s", Describe(actor.get()),
                            PackageId(slot), now - slot.armedAt);
    }
    auto *state = actor->AsActorState();
    const auto attack = state ? state->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
    const auto *attackData = AttackDataOf(actor.get());
    // A swing of their own running when the lease began is not ours; once
    // they are between swings, any power attack is.
    if (attack == RE::ATTACK_STATE_ENUM::kNone)
        slot.attackAtArm = nullptr;
    if (!slot.swinging && slot.seenRunning && attack != RE::ATTACK_STATE_ENUM::kNone && attackData &&
        attackData != slot.attackAtArm && attackData->data.flags.all(RE::AttackData::AttackFlag::kPowerAttack))
    {
        slot.swinging = true;
        slot.fired.store(true, std::memory_order_relaxed);
        slot.attackEvent = attackData->event.c_str();
        slot.until = (std::max)(slot.until, now + kWeaponSwingSeconds);
        log::packages.info("{} power attacks: {} after {:.2f} s", Describe(actor.get()), slot.attackEvent,
                           now - slot.armedAt);
    }

    // Turned toward the target while no swing of ours is under way: the
    // procedure attacks only a target in front, and in a fight it does not
    // turn the follower itself. In an override list the record leaves combat
    // running, and facing is combat's.
    if (!slot.swinging && !slot.overrideList)
    {
        if (const auto target = slot.target.get())
            TurnToward(actor.get(), target.get());
    }

    const char *why = nullptr;
    if (slot.swinging && attack == RE::ATTACK_STATE_ENUM::kNone)
        why = "power attack made";
    else if (slot.seenRunning && !running)
        why = slot.swinging ? "package ended mid-swing" : "package ended";
    else if (now >= slot.until)
    {
        if (!slot.seenRunning)
            why = "deadline, AI never picked it up";
        else
            why = slot.swinging ? "deadline, still swinging" : "deadline, no power attack";
    }
    if (!why)
        return;
    ReportResolved(slot, actor.get(), actor->GetFormID(), why, now - slot.armedAt);
    log::packages.debug("{} releases {:08X} after {:.2f} s: {}", Describe(actor.get()), PackageId(slot),
                        now - slot.armedAt, why);
    Release(slot);
}
} // namespace

CastRequest RequestPowerAttack(RE::Actor *actor, std::uint32_t targetId, const BlowPlan &plan, int ruleIndex,
                               std::string_view ruleName)
{
    auto *kit = g_available && actor ? KitOf(actor->GetFormID()) : nullptr;
    if (!kit || !kit->weapon.package)
        return CastRequest::NoPackages;

    auto *target =
        targetId != 0 && targetId != actor->GetFormID() ? RE::TESForm::LookupByID<RE::Actor>(targetId) : nullptr;
    if (!target || !target->Is3DLoaded())
    {
        log::packages.debug("power attack: target {:08X} is not a loaded actor", targetId);
        return CastRequest::TargetGone;
    }
    if (AlreadyCasting(kit))
        return CastRequest::AlreadyCasting;

    auto &slot = kit->weapon;
    if (!SetPackageTarget(slot.package, target, slot.targetInput))
    {
        log::packages.warn("{:08X} could not aim its {} input", PackageId(slot), slot.targetInput);
        return CastRequest::SpellNotInSlot;
    }
    slot.target = target->GetHandle();
    slot.targetId = target->GetFormID();
    // The weapon in the right hand, by name. With the object type the record
    // was copied with, the procedure's Find step took the first of that type
    // in the bag -- a Staff of Flames over Jenassa's pinned sword -- and tried
    // to equip it every frame; the pin refused, and no attack came
    // (2026-09-15). Named, Find finds the weapon already held.
    auto *inHand = actor->GetEquippedObject(false);
    auto *weapon = inHand ? inHand->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon || !SetPackageObject(slot.package, kWeaponTypeInput, weapon))
    {
        log::packages.warn("{:08X} could not name {} as its weapon", PackageId(slot), log::NameOf(inHand));
        return CastRequest::SpellNotInSlot;
    }
    slot.spell = weapon->GetFormID();
    slot.sustained = false;
    slot.swinging = false;
    slot.attackEvent.clear();
    slot.attackAtArm = AttackDataOf(actor);
    slot.staminaCost = plan.stamina;
    slot.reach = plan.reach;
    slot.staminaAtArm = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
    slot.distanceAtArm = actor->GetPosition().GetDistance(target->GetPosition());
    slot.headingAtArm = actor->GetHeadingAngle(target->GetPosition(), true);
    log::packages.info("{}: {:08X} power attacks {:08X} \"{}\"", log::NameOf(actor), PackageId(slot),
                       target->GetFormID(), log::NameOf(target));
    g_weaponLeases.fetch_add(1, std::memory_order_relaxed);
    return Arm(slot, actor, 0.0f, kWeaponArmWindowSeconds, ruleIndex, ruleName);
}

bool AnyWeaponLease() noexcept
{
    return g_weaponLeases.load(std::memory_order_relaxed) > 0;
}

void TickWeaponLeases(double now)
{
    if (!g_available)
        return;
    for (auto &entry : g_kits)
        if (entry.second->weapon.Busy())
            TickWeaponSlot(entry.second->weapon, now);
}

void ResetPackages()
{
    ForEachSlot([](Slot &slot) {
        if (slot.power)
        {
            slot.power->data.spellType = slot.powerType;
            slot.power = nullptr;
        }
        ReturnShoutVoice(slot);
        // A stack entry is not taken off here: the arrays are the actor's
        // and are rebuilt with them on load; the record's condition is
        // false by then and the entry never passes.
        slot.onStack = false;
        // An override list is a form, not the actor's: it is not rebuilt on
        // load, and the record would stay in it.
        if (slot.overrideList)
            TakeOut(slot.overrideList->forms, slot.package);
        slot.overrideList = nullptr;
        slot.placedIn = nullptr;
        slot.holder.store(0, std::memory_order_release);
        slot.spellId.store(0, std::memory_order_relaxed);
        slot.shoutId.store(0, std::memory_order_relaxed);
        slot.shouting = nullptr;
        if (slot.lease)
            slot.lease->Abandon();
        slot.lease.reset();
        // As Release leaves a slot: no target handle outlives its lease.
        slot.target = {};
        SetPackageTarget(slot.package, nullptr, slot.targetInput);
        slot.seenRunning = false;
        slot.streaming = false;
        slot.extended = false;
        slot.attackAtArm = nullptr;
        slot.swinging = false;
        slot.attackEvent.clear();
        slot.fired.store(false, std::memory_order_relaxed);
        slot.stopped.store(false, std::memory_order_relaxed);
        slot.begun.store(false, std::memory_order_relaxed);
        slot.targetId = 0;
        slot.ruleIndex = -1;
        slot.ruleName.clear();
    });
    g_weaponLeases.store(0, std::memory_order_relaxed);
}

void ReleaseAllLeases(const char *why)
{
    ForEachSlot([why](Slot &slot) {
        if (!slot.Busy())
            return;
        {
            const auto holder = slot.lease->Actor();
            ReportResolved(slot, holder.get(), slot.lease->FormID(), why, TacticsSeconds() - slot.armedAt);
        }
        log::packages.debug("{:08X} held by {:08X} released after {:.1f} s: {}", PackageId(slot), slot.lease->FormID(),
                            TacticsSeconds() - slot.armedAt, why);
        Release(slot);
    });
}

void TickPackages(double now, const std::vector<RE::Actor *> &followers)
{
    if (!g_available)
        return;

    // A wrapper shout left in a base's spell list by a lease that never
    // ended would list under Shouts for every actor of that base. Any wrapper
    // not mid-lease is taken back from a follower casting nothing: bases are
    // shared, so it need not be their own.
    for (auto *follower : followers)
    {
        if (!follower || AlreadyCasting(KitOf(follower->GetFormID())))
            continue;
        for (const auto &entry : g_kits)
            if (!entry.second->voice.Busy())
                TakeWrapper(follower, entry.second->voice.wrapper);
    }

    ForEachSlot([now](Slot &slot) {
        if (!slot.Busy())
            return;
        if (slot.weapon)
        {
            TickWeaponSlot(slot, now);
            return;
        }

        auto actor = slot.lease->Actor();
        if (!actor)
        {
            // Unloaded or gone. The lease's destructor finds no actor and
            // clears nothing; the sweep above catches them if they come back.
            ReportResolved(slot, nullptr, slot.lease->FormID(), "holder vanished", now - slot.armedAt);
            log::packages.debug("{:08X} holder vanished -- released", PackageId(slot));
            Release(slot);
            return;
        }
        const std::string name = NameOr(actor.get(), "?");

        const bool running = actor->GetCurrentPackage() == slot.package;
        if (running && !slot.seenRunning)
        {
            slot.seenRunning = true;
            log::packages.debug("{} is RUNNING {:08X} (spell {:08X}) after {:.1f} s", name, PackageId(slot), slot.spell,
                                now - slot.armedAt);
        }

        // ONE release, with a reason. The deadline is the guarantee: a record
        // is never held past it, whatever the game did or did not do. The
        // other two are only signals that the hold can end sooner -- the spell
        // has left their hand, or the AI has already moved on -- so a follower
        // is not kept for four seconds after a one-second cast.
        // A stream that has started gets its sustain added to the window,
        // once, from the moment it started.
        if (slot.sustained && !slot.streaming && slot.fired.load(std::memory_order_relaxed))
        {
            slot.streaming = true;
            slot.until = now + slot.sustain + 1.0;
            log::packages.debug("{} stream started on {:08X} -- {:.1f} s to run", name, PackageId(slot), slot.sustain);
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
            log::packages.debug("{} began the {} on {:08X} after {:.1f} s -- deadline stepped back", name,
                                slot.shouting ? "shout" : "cast", PackageId(slot), now - slot.armedAt);
        }

        const char *why = nullptr;
        if (!slot.sustained && slot.fired.load(std::memory_order_relaxed))
        {
            why = slot.power ? "power fired" : slot.wrapper ? "shout fired" : "spell fired";
            SpendScroll(slot, actor.get());
        }
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
            ReportResolved(slot, actor.get(), actor->GetFormID(), why, now - slot.armedAt);
            log::packages.debug("{} releases {:08X} after {:.1f} s: {}", name, PackageId(slot), now - slot.armedAt,
                                why);
            Release(slot);
        }
    });
}

namespace
{
// Find the Spell, Target and CastTime input layout on the vanilla records.
// Writes nothing; what it fails to find, RequestCast refuses to write. False
// means no follower's records are made: a package whose inputs cannot be
// checked is a package that might cast the wrong thing at the wrong person.
bool Calibrate()
{
    auto *mercer = RE::TESForm::LookupByID<RE::TESPackage>(kMercerCastAtPlayerID);
    auto *colette = RE::TESForm::LookupByID<RE::TESPackage>(kColetteHealID);
    if (!mercer || !colette)
    {
        log::packages.error("probe: vanilla records missing (Mercer {}, Colette {}) -- cast rules stay off",
                            static_cast<const void *>(mercer), static_cast<const void *>(colette));
        return false;
    }

    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(mercer->data);
    if (!custom)
    {
        log::packages.error("probe: Mercer's package data is not TESCustomPackageData -- template inputs "
                            "unreachable");
        return false;
    }
    log::packages.debug("probe: Mercer's package has {} inputs", custom->data.dataSize);
    LogNameMap(custom, "package");

    std::int8_t uid = 0;
    if (!FindInputUID(custom, "Spell", uid))
    {
        log::packages.error("probe: no 'Spell' input in the name map -- cast rules stay off");
        return false;
    }

    auto *input = InputByUID(custom, uid);
    log::packages.debug("probe: 'Spell' is uid {} -> IPackageData {}", static_cast<int>(uid),
                        static_cast<const void *>(input));
    if (!input)
        return false;

    auto *canary = RE::TESForm::LookupByID(kMercerSpellID);
    log::packages.debug("probe: canary {} lives at {}", canary ? "found" : "MISSING",
                        static_cast<const void *>(canary));
    if (!canary)
        return false;

    const auto canaryAddr = reinterpret_cast<std::uintptr_t>(canary);

    log::packages.debug("probe: +00 {}", HexDump(input, 32));
    log::packages.debug("probe: +20 {}", HexDump(reinterpret_cast<const std::uint8_t *>(input) + 32, 32));

    // The canary is expected behind a pointer, in the PackageTarget's union
    // at +08. Found anywhere else -- inline in the input, or at another
    // offset of what the pointer reaches -- the layout is not the one the
    // writes assume, and nothing is written. (A first version accepted the
    // canary inline and would then have written through the input's first
    // word, its vtable pointer.)
    const auto *words = reinterpret_cast<const std::uintptr_t *>(input);
    for (std::size_t w = 0; w < 8 && g_spellOuter == kNotCalibrated; ++w)
    {
        const std::uintptr_t value = words[w];
        const std::size_t offset = w * sizeof(std::uintptr_t);
        if (value == canaryAddr)
        {
            log::packages.error("probe: canary found inline at +{:02X}, not behind a PackageTarget -- cast rules "
                                "stay off. Nothing will be written.",
                                offset);
            return false;
        }
        if (!LooksLikePointer(value))
            continue;
        const auto *inner = reinterpret_cast<const std::uintptr_t *>(value);
        for (std::size_t k = 0; k < 4; ++k)
        {
            if (inner[k] != canaryAddr)
                continue;
            if (k * sizeof(std::uintptr_t) != kTargetUnionOffset)
            {
                log::packages.error("probe: canary at +{:02X} -> +{:02X}, not +{:02X} -- cast rules stay off. "
                                    "Nothing will be written.",
                                    offset, k * sizeof(std::uintptr_t), kTargetUnionOffset);
                return false;
            }
            g_spellOuter = offset;
            log::packages.debug("probe: FOUND canary at +{:02X} -> +{:02X} -- Spell is a PackageTarget", offset,
                                kTargetUnionOffset);
        }
    }

    if (g_spellOuter == kNotCalibrated)
    {
        log::packages.error("probe: layout NOT identified -- cast rules stay off. Nothing will be written.");
        return false;
    }
    log::packages.debug("probe: calibrated (+{:02X} -> +{:02X}); cast rules can name any spell", g_spellOuter,
                        kTargetUnionOffset);

    // The Target input, same outer offset, two canaries. Colette's is Self:
    // its type byte is the Self value. Mercer's is the player: its type byte
    // is the specific-reference value, and its handle must be the player's
    // or the union is not where we think.
    auto *cpt = TargetOfInput(colette, "Target");
    auto *mpt = TargetOfInput(mercer, "Target");
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!cpt || !mpt || !player || mpt->target.handle.native_handle() != player->GetHandle().native_handle())
    {
        log::packages.error("probe: Target inputs {} -- cast rules stay off",
                            mpt ? fmt::format("Mercer's type {} handle {:08X}, player handle {:08X}", mpt->targType,
                                              mpt->target.handle.native_handle(),
                                              player ? player->GetHandle().native_handle() : 0)
                                : "unreachable");
        return false;
    }
    if (mpt->targType == cpt->targType)
    {
        log::packages.error("probe: Self and specific-reference read the same type {} -- cast rules stay off",
                            cpt->targType);
        return false;
    }
    g_typeSelf = cpt->targType;
    g_typeSpecificReference = mpt->targType;
    g_targetCalibrated = true;
    log::packages.debug("probe: Target input calibrated: Self = {} (Colette), specific reference = {} (Mercer); "
                        "cast rules can name any loaded actor",
                        g_typeSelf, g_typeSpecificReference);

    // The CastTime floats: Mercer's record has 0.5 and 1.0.
    g_castTimeOffset = FindCastTimeOffset(mercer);
    if (g_castTimeOffset == kNotCalibrated)
    {
        log::packages.warn("probe: CastTime floats not found; concentration spells will run the copied time");
        return true; // a stream at a fixed length is a loss, not a hazard
    }
    g_castTimeCalibrated = true;
    log::packages.debug("probe: CastTime floats at +{:02X}; a concentration spell can be sustained for a chosen "
                        "time",
                        g_castTimeOffset);
    return true;
}

// Point a fresh copy at the canary spell through the calibrated layout and
// read it back: the proof that the layout found on Mercer's record holds on
// a record the engine copied from it.
bool ProveCopy(RE::TESPackage *pkg, const char *inputName, RE::TESForm *canary)
{
    if (!SetPackageInput(pkg, inputName, canary))
    {
        log::packages.error("{:08X}: could not write its {} input", pkg->GetFormID(), inputName);
        return false;
    }
    auto *pt = TargetOfInput(pkg, inputName);
    if (!pt || pt->target.object != canary)
    {
        log::packages.error("{:08X}: {} input read back {} after writing {:08X}", pkg->GetFormID(), inputName,
                            pt ? fmt::format("{:08X}", pt->target.object ? pt->target.object->GetFormID() : 0)
                               : std::string("nothing"),
                            canary->GetFormID());
        return false;
    }
    return true;
}

// A power attack's record is a copy of Edorfin's attack-a-target package
// (EdorfinAttackTarget): an instance of the UseWeapon template, whose
// procedure node maps Always Power Attack to an input. UseWeaponAlreadyHeld,
// the template of Karliah's combat override in Blindsighted, maps it to none
// (docs/ATTACK.md). Edorfin's trigger is himself and the record has no
// conditions; its Use Weapon Location, his editor location, is rewritten.
constexpr std::uint32_t kEdorfinAttackTargetID = 0x00055D48;
// The values the checks read, as authored. The heroes of Sovngarde's package
// (MQ206HeroAttackAlduin) pauses between barrages of one to three attacks,
// triggers at 1500 and attacks from around an alias, radius 425; Vilkas's
// training package (C00VilkasTrainInTrainingYard) aims at one reference.
constexpr std::uint32_t kHeroAttackAlduinID = 0x000CD9F9;
constexpr std::uint32_t kVilkasTrainID = 0x000F7952;
constexpr const char *kTargetToAttack = "Target to Attack";
constexpr const char *kUseWeaponLocation = "Use Weapon Location";
RE::TESPackage *g_weaponSource = nullptr;

std::size_t FindIntOffset(RE::TESPackage *pkg, const char *inputName, std::int32_t authored)
{
    auto *input = NamedInput(pkg, inputName);
    if (!input)
        return kNotCalibrated;
    log::packages.debug("probe: {} +00 {}", inputName, HexDump(input, 32));
    for (std::size_t off = sizeof(std::uintptr_t); off + sizeof(std::int32_t) <= 32; off += sizeof(std::int32_t))
        if (*reinterpret_cast<const std::int32_t *>(reinterpret_cast<std::uintptr_t>(input) + off) == authored)
            return off;
    return kNotCalibrated;
}

// The pointer, past the vtable, whose PackageLocation has the authored type
// and radius.
std::size_t FindLocationOffset(RE::TESPackage *pkg, const char *inputName, RE::PackageLocation::Type type,
                               std::uint32_t radius)
{
    auto *input = NamedInput(pkg, inputName);
    if (!input)
        return kNotCalibrated;
    log::packages.debug("probe: {} +00 {}", inputName, HexDump(input, 32));
    const auto *words = reinterpret_cast<const std::uintptr_t *>(input);
    for (std::size_t w = 1; w < 4; ++w)
    {
        if (!LooksLikePointer(words[w]))
            continue;
        const auto *location = reinterpret_cast<const RE::PackageLocation *>(words[w]);
        if (location->locType.get() == type && location->rad == radius)
            return w * sizeof(std::uintptr_t);
    }
    return kNotCalibrated;
}

// The Int, Bool, Location and target layouts of the UseWeapon inputs, each
// against a second record before anything is written through it. False
// leaves Power Attack to the animation event; the casts are not affected.
bool CalibrateWeapon()
{
    auto *edorfin = RE::TESForm::LookupByID<RE::TESPackage>(kEdorfinAttackTargetID);
    auto *hero = RE::TESForm::LookupByID<RE::TESPackage>(kHeroAttackAlduinID);
    auto *vilkas = RE::TESForm::LookupByID<RE::TESPackage>(kVilkasTrainID);
    if (!edorfin || !hero || !vilkas)
    {
        log::packages.error("probe: UseWeapon records missing (Edorfin {}, heroes {}, Vilkas {})",
                            static_cast<const void *>(edorfin), static_cast<const void *>(hero),
                            static_cast<const void *>(vilkas));
        return false;
    }

    g_intOffset = FindIntOffset(edorfin, "Trigger Radius", 350);
    const auto *heroRadius = IntOfInput(hero, "Trigger Radius");
    const auto *heroAttacks = IntOfInput(hero, "Max Attacks per Barrage");
    if (g_intOffset == kNotCalibrated || !heroRadius || *heroRadius != 1500 || !heroAttacks || *heroAttacks != 3)
    {
        log::packages.error("probe: Int inputs not identified (the heroes' radius {}, attacks {})",
                            heroRadius ? *heroRadius : -1, heroAttacks ? *heroAttacks : -1);
        g_intOffset = kNotCalibrated;
        return false;
    }

    const auto heroPause = BoolOfInput(hero, "Pause between Barrages?");
    const auto edorfinPause = BoolOfInput(edorfin, "Pause between Barrages?");
    if (!heroPause || !*heroPause || !edorfinPause || *edorfinPause)
    {
        log::packages.error("probe: Bool inputs do not read as authored (the heroes pause {}, Edorfin {})",
                            heroPause ? (*heroPause ? "true" : "false") : "unread",
                            edorfinPause ? (*edorfinPause ? "true" : "false") : "unread");
        return false;
    }

    g_locationOffset =
        FindLocationOffset(edorfin, kUseWeaponLocation, RE::PackageLocation::Type::kNearEditorLocation, 32);
    const auto *heroLocation = LocationOfInput(hero, kUseWeaponLocation);
    const auto *search = LocationOfInput(edorfin, "Search for Weapon Location");
    if (g_locationOffset == kNotCalibrated || !heroLocation ||
        heroLocation->locType.get() != RE::PackageLocation::Type::kAlias_Reference || heroLocation->rad != 425 ||
        !search || search->locType.get() != RE::PackageLocation::Type::kNearSelf)
    {
        log::packages.error("probe: Location inputs not identified");
        g_locationOffset = kNotCalibrated;
        return false;
    }

    // A named form's type, off Mercer's Spell input; Edorfin's Weapon Type is
    // an object type and must read otherwise.
    const auto *spell = TargetOfInput(RE::TESForm::LookupByID<RE::TESPackage>(kMercerCastAtPlayerID), "Spell");
    const auto *weaponType = TargetOfInput(edorfin, kWeaponTypeInput);
    if (!spell || !weaponType || spell->targType == weaponType->targType ||
        spell->targType == g_typeSpecificReference || spell->targType == g_typeSelf)
    {
        log::packages.error("probe: a named form's target type not identified (Mercer's Spell {}, Edorfin's {} {})",
                            spell ? static_cast<int>(spell->targType) : -1, kWeaponTypeInput,
                            weaponType ? static_cast<int>(weaponType->targType) : -1);
        return false;
    }
    g_typeObjectId = spell->targType;

    const auto *aim = TargetOfInput(vilkas, kTargetToAttack);
    if (!aim || aim->targType != g_typeSpecificReference)
    {
        log::packages.error("probe: Vilkas's {} reads type {}, not a specific reference ({})", kTargetToAttack,
                            aim ? static_cast<int>(aim->targType) : -1, static_cast<int>(g_typeSpecificReference));
        return false;
    }
    log::packages.debug("probe: UseWeapon inputs calibrated: Int at +{:02X}, Location at +{:02X}", g_intOffset,
                        g_locationOffset);
    return true;
}

constexpr std::uint32_t kDialogueFollowerID = 0x000750BA;
constexpr std::uint32_t kFollowerCombatOverrideListID = 0x0005C852; // PlayerFollowerCombatOverridePackageList
constexpr std::uint32_t kMostAliasesFiled = 1U << 20;

bool InGameData(std::uintptr_t address)
{
    const auto &module = REL::Module::get();
    for (const auto name : {REL::Segment::rdata, REL::Segment::data})
    {
        const auto segment = module.segment(name);
        if (address >= segment.address() && address < segment.address() + segment.size())
            return true;
    }
    return false;
}

// The map where it should be and shaped as the table is, and vanilla's
// follower alias (DialogueFollower's Follower) filed in it with its combat
// override list at +20 as a list form, not the form ID the loader reads.
// The header is checked before the entries pointer is followed, and every
// chain link against the entries before it is trusted; a wrong address stops
// at a header that does not add up. False leaves the lists unread.
bool CheckAliasOverrideLists()
{
    if (!REL::Module::IsAE())
    {
        log::packages.info("alias override lists: their address is known for 1.6.1170 alone -- not read on this "
                           "runtime");
        return false;
    }
    auto *quest = RE::TESForm::LookupByID<RE::TESQuest>(kDialogueFollowerID);
    const auto *expected = RE::TESForm::LookupByID<RE::BGSListForm>(kFollowerCombatOverrideListID);
    const RE::BGSRefAlias *follower = nullptr;
    if (quest)
    {
        for (auto *alias : quest->aliases)
            if (alias && alias->aliasID == 0)
                follower = skyrim_cast<RE::BGSRefAlias *>(alias);
    }
    if (!follower || !expected)
    {
        log::packages.warn("probe: alias override lists not read: DialogueFollower's Follower alias {}, its combat "
                           "override list {}",
                           follower ? "found" : "missing", expected ? "found" : "missing");
        return false;
    }

    const AliasOverrideMap *map = &AliasOverrideTable();
    const std::uint32_t capacity = map->capacity;
    const bool powerOfTwo = capacity != 0 && (capacity & (capacity - 1)) == 0;
    const auto entriesAt = reinterpret_cast<std::uintptr_t>(map->entries);
    if (!powerOfTwo || capacity > kMostAliasesFiled || map->free > capacity ||
        !InGameData(reinterpret_cast<std::uintptr_t>(map->sentinel)) || !LooksLikePointer(entriesAt))
    {
        log::packages.warn("probe: alias override lists not read: the table's header does not add up (capacity {}, "
                           "free {}, sentinel {}, entries {:X})",
                           capacity, map->free, static_cast<const void *>(map->sentinel), entriesAt);
        return false;
    }

    const auto entriesEnd = entriesAt + std::size_t{capacity} * sizeof(AliasOverrideEntry);
    std::uint32_t filed = 0;
    const RE::BGSOverridePackCollection *followerLists = nullptr;
    for (std::uint32_t i = 0; i < capacity; ++i)
    {
        const AliasOverrideEntry &entry = map->entries[i];
        if (!entry.next)
            continue;
        const auto next = reinterpret_cast<std::uintptr_t>(entry.next);
        if (entry.next != map->sentinel &&
            (next < entriesAt || next >= entriesEnd || (next - entriesAt) % sizeof(AliasOverrideEntry) != 0))
        {
            log::packages.warn("probe: alias override lists not read: slot {} chains to {:X}, outside the table", i,
                               next);
            return false;
        }
        ++filed;
        if (entry.alias == follower)
            followerLists = entry.lists;
    }
    if (filed != capacity - map->free)
    {
        log::packages.warn("probe: alias override lists not read: {} slots filled, the header counts {}", filed,
                           capacity - map->free);
        return false;
    }
    if (!followerLists || followerLists->enterCombatOverRidePackList != expected)
    {
        log::packages.warn("probe: alias override lists not read: DialogueFollower's Follower alias {}",
                           followerLists ? "does not have PlayerFollowerCombatOverridePackageList at +20"
                                         : "is not filed");
        return false;
    }
    log::packages.debug("probe: alias override lists readable: {} aliases filed, DialogueFollower's Follower alias "
                        "with PlayerFollowerCombatOverridePackageList",
                        filed);
    return true;
}

// A fresh copy of Edorfin's record made into a power attack's. Everything but
// the target is the same for every request, so it is set once, here. Null
// when made, otherwise what could not be.
const char *ConfigureWeapon(RE::TESPackage *pkg, RE::TESPackage *source)
{
    // The engine's copy gives each target input its own PackageTarget, and
    // should give the location its own PackageLocation; checked, because a
    // shared one written here would move Edorfin's.
    auto *location = LocationOfInput(pkg, kUseWeaponLocation);
    if (!location || location == LocationOfInput(source, kUseWeaponLocation))
        return "its Use Weapon Location is not its own";
    // Near the follower wherever they stand, so the procedure's travel to it
    // is over before it starts, as the cast records' location is.
    location->locType = RE::PackageLocation::Type::kNearSelf;
    location->rad = 10000;
    location->data.object = nullptr;

    // Edorfin's record does no damage, holds while anyone is in the line of
    // attack, and never ends. A follower's attacks with the party around it,
    // does damage, and is over after one barrage of one power attack.
    struct Flag
    {
        const char *name;
        bool value;
    };
    constexpr std::array<Flag, 9> kFlags{{{"Always Power Attack?", true},
                                          {"Do No Damage?", false},
                                          {"Always Hit?", false},
                                          {"Hold when Blocked?", false},
                                          {"Never End?", false},
                                          {"Pause between Barrages?", false},
                                          {"Allow Combat Start on hit?", false},
                                          {"Aim Only, Don't Fire? (usually false)", false},
                                          {"Headtrack Target?", true}}};
    for (const Flag &flag : kFlags)
    {
        if (!SetPackageBool(pkg, flag.name, flag.value))
        {
            log::packages.warn("{:08X}: no Bool input \"{}\"", pkg->GetFormID(), flag.name);
            return "a Bool input could not be set";
        }
    }
    for (const char *name : {"End after this many Barrages:", "Min Attacks per Barrage:", "Max Attacks per Barrage"})
    {
        auto *value = IntOfInput(pkg, name);
        if (!value)
        {
            log::packages.warn("{:08X}: no Int input \"{}\"", pkg->GetFormID(), name);
            return "an Int input could not be set";
        }
        *value = 1;
    }

    // The target through the calibrated layout, read back, as ProveCopy
    // proves a cast record's spell.
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!player || !SetPackageTarget(pkg, player, kTargetToAttack))
        return "its Target to Attack could not be aimed";
    const auto *aim = TargetOfInput(pkg, kTargetToAttack);
    const bool aimed = aim && aim->target.handle.native_handle() == player->GetHandle().native_handle();
    SetPackageTarget(pkg, nullptr, kTargetToAttack);
    if (!aimed)
        return "its Target to Attack did not read back";

    if (!TargetOfInput(pkg, kWeaponTypeInput))
        return "its Weapon Type input is not reachable";

    // IgnoreCombat, from ClonePackage, as the cast records: without it the
    // record is passed over in a fight for the alias's combat override
    // (docs/ATTACK.md "Facing"). Facing the target is then ours to do
    // (TurnToward).
    pkg->packData.packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kWeaponDrawn);
    return nullptr;
}

// A follower's records: Mercer's record with Target back to Self and Spell
// on the canary, read back; a word, a wrapper shout on it, and Tsun's record
// with its Shout input on the wrapper and Target back to Self. Null when
// made, otherwise what could not be.
const char *MakeKit(Kit &kit)
{
    auto *pkg = ClonePackage(g_mercer);
    auto *condition = AddIsReferenceCondition(pkg);
    if (!pkg || !condition || !ProveCopy(pkg, "Spell", g_canary) || !SetPackageTarget(pkg, nullptr))
        return "the cast package could not be made";
    SetPackageCastTime(pkg, kAuthoredCastTimeMax);
    kit.spell.package = pkg;
    kit.spell.condition = condition;

    auto *word = CreateWord("Power");
    auto *wrapper = word ? CreateShout(word, g_canary, "FollowerTactics power") : nullptr;
    auto *shoutPkg = wrapper ? ClonePackage(g_tsun) : nullptr;
    auto *shoutCondition = AddIsReferenceCondition(shoutPkg);
    if (!shoutPkg || !shoutCondition || !ProveCopy(shoutPkg, "Shout", wrapper) || !SetPackageTarget(shoutPkg, nullptr))
        return "the shout package could not be made";
    // Weapon Drawn, which the ESP-era records did not have. Tried for
    // the 0.3 to 1.5 s between arming and BeginCastVoice, on the guess
    // that the AI was sheathing first. It was not: measured 2026-09-08
    // with the weapon state logged, "drawn" at arming, at pick-up and at
    // begin, and the delay unchanged (0.5 to 1.0 s, once 2.9 s, the same
    // as a hand cast). The delay is the AI's own start-up. The flag is
    // kept because this is the configuration that was verified, and a
    // procedure that needs no hands has no use for putting them away.
    shoutPkg->packData.packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kWeaponDrawn);
    kit.voice.package = shoutPkg;
    kit.voice.condition = shoutCondition;
    kit.voice.wrapper = wrapper;
    kit.voice.spell = wrapper->GetFormID();

    // A power attack's record. The casts do not need it, so a follower whose
    // copy fails keeps them, and Power Attack goes by the animation event.
    if (g_weaponSource)
    {
        auto *weaponPkg = ClonePackage(g_weaponSource);
        auto *weaponCondition = AddIsReferenceCondition(weaponPkg);
        const char *failed =
            !weaponPkg || !weaponCondition ? "could not be copied" : ConfigureWeapon(weaponPkg, g_weaponSource);
        if (failed)
            log::packages.warn("power attack package: {} -- Power Attack goes by the animation event", failed);
        else
        {
            // Every follower's copy is configured alike.
            g_weaponOnStack = DataOf(*weaponPkg);
            kit.weapon.package = weaponPkg;
            kit.weapon.condition = weaponCondition;
            kit.weapon.weapon = true;
            kit.weapon.targetInput = kTargetToAttack;
            kit.weapon.spell = 0;
        }
    }
    return nullptr;
}
} // namespace

void InitPackages()
{
    g_available = false;
    g_aliasOverrideListsRead = CheckAliasOverrideLists();
    if (!Calibrate())
        return;

    g_mercer = RE::TESForm::LookupByID<RE::TESPackage>(kMercerCastAtPlayerID);
    g_tsun = RE::TESForm::LookupByID<RE::TESPackage>(kTsunShoutID);
    g_canary = RE::TESForm::LookupByID(kCanarySpellID);
    if (!g_tsun || !g_canary)
    {
        log::packages.error("vanilla records missing (Tsun's shout package {}, Fast Healing {}) -- cast rules "
                            "stay off",
                            static_cast<const void *>(g_tsun), static_cast<const void *>(g_canary));
        return;
    }

    log::packages.info("input layout found; a follower's cast records are made when the tick first sees them");
    g_available = true;

    // A power attack's record needs layouts of its own; without them only
    // that action falls back, to the animation event.
    if (CalibrateWeapon())
        g_weaponSource = RE::TESForm::LookupByID<RE::TESPackage>(kEdorfinAttackTargetID);
    else
        log::packages.warn("UseWeapon inputs not identified -- Power Attack goes by the animation event");

    // Without Karliah's record, a power attack's record keeps to the alias
    // arrays, with IgnoreCombat and TurnToward.
    if (const auto *karliah = RE::TESForm::LookupByID<RE::TESPackage>(kKarliahCombatOverrideID))
    {
        g_weaponInOverrideList = DataOf(*karliah);
        log::packages.debug("Karliah's package data for an override list: flags {:08X}, interrupt override {}, "
                            "interrupt flags {:04X}",
                            karliah->packData.packFlags.underlying(),
                            static_cast<int>(karliah->packData.interruptOverrideType.underlying()),
                            karliah->packData.foBehaviorFlags.underlying());
    }
    else
        log::packages.warn("Karliah's combat override record is missing -- a power attack stays off the override "
                           "lists");
}

void ProvideCastForms(RE::Actor *actor)
{
    if (!g_available || !actor)
        return;
    const RE::FormID id = actor->GetFormID();
    if (KitOf(id) || g_kitsFailed.contains(id))
        return;

    auto kit = std::make_unique<Kit>();
    if (const char *failed = MakeKit(*kit))
    {
        // What was made before the failure stays registered: forms are never
        // deleted, and trying again would only make more.
        g_kitsFailed.insert(id);
        log::packages.error("{}: {} -- their cast, power and shout rules stay off", Describe(actor), failed);
        return;
    }
    log::packages.info("{}: cast package {:08X}, shout package {:08X}, wrapper {:08X}, power attack package {:08X}",
                       Describe(actor), PackageId(kit->spell), PackageId(kit->voice), kit->voice.wrapper->GetFormID(),
                       PackageId(kit->weapon));
    std::unique_lock lock(g_kitsMutex);
    g_kits.emplace(id, std::move(kit));
}

} // namespace ft::game
