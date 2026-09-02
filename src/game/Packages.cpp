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

// How long a request stays armed before the tick withdraws it. The AI
// re-evaluates on its own schedule as well as when we ask; a request it has
// not taken up in this long is stale, and firing later would land at a moment
// the rule never intended.
constexpr double kArmWindowSeconds = 4.0;

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

    // Set from the animation thread when her spell-fire event arrives; read
    // and cleared by the tick. The ONLY thing the sink writes.
    std::atomic<bool> fired{false};

    [[nodiscard]] bool Busy() const noexcept
    {
        return lease.has_value();
    }
};
std::array<Slot, kPackageSlots> g_pool{};

// Measured 2026-09-02: the UseMagic package casts (health 75 -> 175 about 2 s
// after arming) and then does NOT complete. NumToCast is 1, the procedure
// carries SuccessCompletesPackage, and still GetCurrentPackage() was ours
// four seconds later, with the follower standing idle. Waiting for the
// package to end is therefore not a release path we can rely on.
//
// The spell leaving her hand is. The animation graph emits MRh_SpellFire_Event
// / MLh_SpellFire_Event at exactly that moment (NPC Spell Variance receives
// the same events; docs/MAGIC.md dead end 2). A sink on the casting follower
// flags the slot, and the next tick releases it and asks the AI to
// re-evaluate. That is the difference between "she cast" and "she cast and
// then stood there".
//
// Measured again the same afternoon: the event fires for EVERY spell she
// casts. Marcurio is a Destruction mage, and three of four requests were
// released 65-216 ms after arming -- his own firebolt, not our heal -- which
// dropped the rank and cancelled our package before it cast. So the sink
// checks which spell is in the firing hand and accepts only ours.
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

        for (auto &slot : g_pool)
        {
            if (!slot.Busy() || slot.lease->FormID() != who)
                continue;
            // Every cast-related tag while a record is held, so the graph's
            // vocabulary is on record and a missing fire event is diagnosable.
            if (right || left || strstr(tag, "Cast") || strstr(tag, "Spell"))
                logger::info("  anim {:08X}: {}", who, tag);
            if (!right && !left)
                continue;

            // Which spell just left that hand? Measured: the caster's
            // currentSpell is already null when the fire event arrives (every
            // fire in the 12:21 run read 00000000). The spell EQUIPPED in that
            // hand is still there, and a UseMagic package equips the spell it
            // casts, so that is what we compare. Not ours: her own combat
            // casting, and the record stays hers until ours fires or the
            // deadline.
            auto *actor = const_cast<RE::TESObjectREFR *>(ev->holder)->As<RE::Actor>();
            const auto *spell =
                actor ? actor->GetActorRuntimeData()
                            .selectedSpells[right ? RE::Actor::SlotTypes::kRightHand : RE::Actor::SlotTypes::kLeftHand]
                      : nullptr;
            const std::uint32_t firedID = spell ? spell->GetFormID() : 0;
            const bool ours = firedID == slot.spell;
            logger::info("  anim {:08X}: {} hand fired {:08X} \"{}\" -- {}", who, right ? "right" : "left", firedID,
                         spell && spell->GetName() ? spell->GetName() : "?", ours ? "OURS" : "her own, ignored");
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
// cast" ambiguous.
//
// Read from the ACTOR: every alias an actor fills is recorded on her as
// ExtraAliasInstanceArray, quest and alias id together. The first version of
// this asked the quest through CreateRefHandleByAliasID, whose contract the
// header does not state, and it answered "no" for a follower who may well
// have been in the alias. The actor's own table is the game's source of
// truth for "which alias packages apply to me", so it is what we read, and
// the whole table goes in the log so a wrong answer is visible.
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

    // Second opinion, kept only so the two methods can be compared in the log.
    auto *quest = RE::TESForm::LookupByID<RE::TESQuest>(kDialogueFollowerQuestID);
    if (quest)
    {
        RE::ObjectRefHandle handle;
        quest->CreateRefHandleByAliasID(handle, 0);
        const auto ref = handle.get();
        logger::info("  DialogueFollower alias 0 by quest lookup: {:08X}", ref ? ref->GetFormID() : 0);
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
    g_pool[i].fired.store(false, std::memory_order_relaxed);
    g_pool[i].seenRunning = false;
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
        return "she is already mid-cast; skipped this turn";
    case CastRequest::SpellNotInSlot:
        return "could not repoint the package at that spell";
    case CastRequest::NotSelfTarget:
        return "packages cast on self only";
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

CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId)
{
    if (!g_available || !actor)
        return CastRequest::NoPackages;

    if (targetId != 0 && targetId != actor->GetFormID())
    {
        logger::info("  target {:08X} is not the caster; the self-cast pool cannot reach it", targetId);
        return CastRequest::NotSelfTarget;
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

    // The diagnostic that decides what a silence means. Not in the alias: our
    // list was never consulted and no amount of package tuning will help.
    logger::info("  {} in the DialogueFollower alias: {}", actor->GetName() ? actor->GetName() : "?",
                 InFollowerAlias(actor) ? "yes" : "NO -- recruit her through dialogue, not the console");

    slot.armedAt = TacticsSeconds();
    slot.until = slot.armedAt + kArmWindowSeconds;
    slot.seenRunning = false;
    slot.fired.store(false, std::memory_order_relaxed);

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
        slot.fired.store(false, std::memory_order_relaxed);
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
        const char *why = nullptr;
        if (slot.fired.load(std::memory_order_relaxed))
            why = "spell fired";
        else if (slot.seenRunning && !running)
            why = "package ended";
        else if (now >= slot.until)
            why = slot.seenRunning ? "deadline, package still running" : "deadline, AI never picked it up";

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

void ProbeSpellInput()
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
        logger::info("probe: layout NOT identified -- cast rules will refuse anything but the "
                     "spell each slot was authored with. Nothing will be written.");
    else
        logger::info("probe: calibrated (+{:02X} -> +{:02X}); cast rules can name any spell", g_spellOuter,
                     g_spellInner);
}

} // namespace ft::game
