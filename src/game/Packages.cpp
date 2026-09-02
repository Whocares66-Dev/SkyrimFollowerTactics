#include "game/Packages.h"

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ft::game
{
namespace
{

std::array<RE::TESPackage *, kPackageSlots> g_slots{};
bool g_available = false;

// Which follower owns which slot, kept for the life of the session.
//
// Not a cache -- correctness. The spell a package casts is stored in the
// record, so two followers sharing a slot would silently overwrite each other's
// choice the moment repointing works. Assigning once and keeping it means a
// follower's slot is hers.
std::unordered_map<std::uint32_t, std::size_t> g_slotOf;

// Where the Spell form actually sits, learned at load rather than hardcoded.
//
// BGSPackageDataTargetSelector's layout is not mapped by CommonLibSSE, so a
// written-down offset would be maintained by nobody and could move between
// runtime versions -- fragile in exactly the way that matters, since the cost
// of being wrong is a bad write into a live game rather than a failed test.
//
// Instead every slot ships with a form we chose (the canary), so the offsets
// can be FOUND by looking for it. If it is not found, casting stays off and
// nothing is written. That degrades the same way a missing ESL does.
constexpr std::size_t kNotCalibrated = static_cast<std::size_t>(-1);
std::size_t g_spellOuter = kNotCalibrated;
std::size_t g_spellInner = kNotCalibrated;

// Is this plausibly a live heap pointer, or is it a small integer, a flag byte
// pair, or uninitialised padding that happens to sit in a pointer-sized slot?
//
// Deliberately conservative. This gates whether we DEREFERENCE, and the cost of
// being wrong is a crash in someone's game rather than a failed unit test. A
// user-mode heap address on x64 Windows is well above the first 64 KB and well
// below the kernel half; anything else is not worth the risk of being clever
// about.
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

// The uid the template assigns to a named input.
//
// Names come from the template's name map -- "Spell", "Target", "NumToCastMin"
// and so on -- and the values array is indexed by POSITION, not by uid, so the
// two have to be joined through the parallel uid array. Looking the name up
// rather than hardcoding position 1 is what keeps this working if the package
// is ever rebuilt from a different source record.
bool SearchNameMap(RE::TESCustomPackageData *data, const char *wanted, std::int8_t &uid)
{
    if (!data || !data->nameMap)
        return false;

    for (const auto &entry : data->nameMap->nameMap)
    {
        if (entry.name.empty())
            continue;
        // Case-INsensitive: the template spells it "SPELL", not "Spell".
        if (_stricmp(entry.name.c_str(), wanted) == 0)
        {
            uid = entry.uid;
            return true;
        }
    }
    return false;
}

// Log every named input, so a failed lookup says what WAS there rather than
// only that the thing we wanted was not.
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

// The uid the template assigns to a named input.
//
// The names live on the TEMPLATE, not on packages built from it: a copy carries
// the VALUES and a templateParent pointer, and the parent carries the map from
// "Spell" to a uid. Looking only at the copy is why the first probe reported no
// input named Spell while happily counting eleven of them.
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

// Point a slot's Spell input at a different spell.
//
// Only ever called once calibration has located the field by finding the
// canary, and it re-checks the pointer every time: the package data is game
// memory, and "it worked at load" is not the same as "it is valid now".
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

// The slot this follower owns, assigning one if she has none.
//
// Returns kPackageSlots when they are all taken. Eight is the same cap the tick
// already enforces, so running out means something upstream changed, not that a
// player brought a ninth follower.
std::size_t SlotFor(RE::Actor *actor)
{
    const std::uint32_t id = actor->GetFormID();
    const auto it = g_slotOf.find(id);
    if (it != g_slotOf.end())
        return it->second;

    if (g_slotOf.size() >= kPackageSlots)
        return kPackageSlots;

    const std::size_t slot = g_slotOf.size();
    g_slotOf.emplace(id, slot);
    logger::info("packages: slot {} assigned to {:08X}", slot, id);
    return slot;
}

} // namespace

const char *ToString(CastRequest r) noexcept
{
    switch (r)
    {
    case CastRequest::Pushed:
        return "package pushed";
    case CastRequest::NoPackages:
        return "FollowerTactics.esp not loaded";
    case CastRequest::NoSlot:
        return "no free package slot";
    case CastRequest::SpellNotInSlot:
        return "that spell is not the one the package casts";
    }
    return "?";
}

CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID)
{
    if (!g_available || !actor)
        return CastRequest::NoPackages;

    // Until the Spell input can be repointed, a slot casts exactly what it was
    // authored with. Refusing anything else is the whole point: casting the
    // wrong spell while reporting success is precisely the class of failure
    // that made the earlier animation attempt worthless.
    const std::size_t slot = SlotFor(actor);
    if (slot >= kPackageSlots || !g_slots[slot])
        return CastRequest::NoSlot;

    // Repoint this slot at the spell the rule actually names.
    //
    // Two spells can share a display name -- Marcurio's native heal is
    // 0007231C while the vanilla one is 0002F3B8, both called "Fast Healing" --
    // so the FormID is the only thing worth comparing, and repointing is what
    // makes the menu's whole spell list usable rather than one authored spell.
    if (spellFormID != kCanarySpellID)
    {
        auto *wanted = RE::TESForm::LookupByID(spellFormID);
        if (!wanted || !SetPackageSpell(g_slots[slot], wanted))
        {
            logger::info("  slot {} still casts {:08X}; could not repoint to {:08X}", slot, kCanarySpellID,
                         spellFormID);
            return CastRequest::SpellNotInSlot;
        }
    }

    // What she was doing before, so the log can say whether we displaced it.
    const auto *before = actor->GetCurrentPackage();

    // tempPackage = true so it is transient rather than joining her permanent
    // package list. createdPackage = true is a CHANGE from the first attempt,
    // which pushed the package successfully and produced no cast at all: the
    // parameter names are all CommonLibSSE offers, and "created" plausibly
    // means "forced, outranks the actor's own list" rather than "allocated by
    // the caller". Both are still inferences.
    actor->PutCreatedPackage(g_slots[slot], /*tempPackage*/ true, /*createdPackage*/ true,
                             /*allowFromFurniture*/ false);

    // Immediate, or she finishes whatever she is doing first and the rule's
    // timing -- the entire reason for this route -- is lost.
    actor->EvaluatePackage(/*immediate*/ true, /*resetAI*/ false);

    // THE DIAGNOSTIC THAT WAS MISSING.
    //
    // The first attempt logged "package pushed" and no cast happened, which
    // leaves two completely different explanations indistinguishable:
    //
    //   our package did not win     the follow package outranks it, and the
    //                               answer is priority -- a QUST with a
    //                               reference alias, per the CK route.
    //   our package won and did     the package itself is wrong: bad target,
    //   nothing                     unaffordable spell, unreachable condition.
    //
    // Asking the actor which package is actually running separates them, and
    // saves building a quest to fix a problem that might not be priority.
    const auto *after = actor->GetCurrentPackage();
    const bool ours = (after == g_slots[slot]);
    logger::info("  current package {:08X} -> {:08X} ({})", before ? before->GetFormID() : 0,
                 after ? after->GetFormID() : 0, ours ? "OURS WON" : "not ours -- outranked");

    return CastRequest::Pushed;
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

    g_available = (found == kPackageSlots);

    if (!g_available)
    {
        // Not an error worth shouting about: the ESL is optional content and a
        // player who has not enabled it simply does not get cast rules. Saying
        // WHICH is missing matters though -- "not enabled in the mod manager"
        // and "wrong plugin name" look identical otherwise.
        logger::info("packages: {}/{} found in {} -- cast rules unavailable (is the ESL enabled?)", found,
                     kPackageSlots, kPluginName);
        return;
    }

    logger::info("packages: {} UseMagic slots resolved from {}", found, kPluginName);
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
        // No fallback to a remembered uid. One used to live here and it did
        // real harm: the name compare was case-sensitive against a template
        // that spells it SPELL, so the lookup never matched and the fallback
        // quietly carried every run. The right answer by the wrong route looks
        // exactly like the right answer until the day it does not.
        logger::info("probe: no 'Spell' input in the name map -- cast rules stay off");
        return;
    }

    auto *input = InputByUID(custom, uid);
    logger::info("probe: 'Spell' is uid {} -> IPackageData {}", static_cast<int>(uid),
                 static_cast<const void *>(input));
    if (!input)
        return;

    // The canary. We wrote this form into the slot ourselves with xEdit, so the
    // correct interpretation of this memory is whichever one produces it.
    auto *canary = RE::TESForm::LookupByID(kCanarySpellID);
    logger::info("probe: canary {} lives at {}", canary ? "found" : "MISSING", static_cast<const void *>(canary));
    if (!canary)
        return;

    const auto canaryAddr = reinterpret_cast<std::uintptr_t>(canary);

    // Raw bytes first, and only then one guarded hop. Dumping is safe; chasing
    // a value that merely looks like a pointer is not, which is why
    // LooksLikePointer gates every dereference below.
    logger::info("probe: +00 {}", HexDump(input, 32));
    logger::info("probe: +20 {}", HexDump(reinterpret_cast<const std::uint8_t *>(input) + 32, 32));

    const auto *words = reinterpret_cast<const std::uintptr_t *>(input);
    for (std::size_t w = 0; w < 8; ++w)
    {
        const std::uintptr_t value = words[w];
        const std::size_t offset = w * sizeof(std::uintptr_t);

        if (value == canaryAddr)
        {
            // Directly embedded: no second hop. Recorded as inner = 0 so the
            // setter treats both shapes the same way.
            g_spellOuter = 0;
            g_spellInner = offset;
            logger::info("probe: FOUND canary directly at +{:02X} -- Spell is a TESForm* here", offset);
            continue;
        }

        if (!LooksLikePointer(value))
            continue;

        // One hop: if this is a PackageTarget*, the form sits at +08 inside it
        // (PackageTarget is mapped: targType at 00, target union at 08).
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
