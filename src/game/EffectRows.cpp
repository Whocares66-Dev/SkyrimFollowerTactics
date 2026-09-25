#include "game/EffectRows.h"

#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Names.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"

#include "game/CustomSkillsFramework.h"
#include "game/Effects.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
#include "game/Spells.h"
#include "game/Toggles.h"
#include "game/Util.h"
#include "game/Values.h"
#include "progression/game/Service.h"
#include "progression/game/ValueView.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace ft::game
{

namespace
{

// "3 min 24 s", "1 h 5 min", "12 s"; nothing for an effect with no
// duration, an ability's or an enchantment's.
std::string RemainingText(float seconds)
{
    if (seconds < 0.0f)
        return {};
    const int total = static_cast<int>(std::lround(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0)
        return TrFormat("{} h {} min", hours, minutes);
    if (minutes > 0)
        return TrFormat("{} min {} s", minutes, secs);
    return TrFormat("{} s", secs);
}

// The effect's description with <mag> and <dur> filled in. Skyrim.esm
// writes the tokens in lower case; mods are not so consistent, and the
// engine takes either.
std::string EffectDescription(const RE::EffectSetting *base, float magnitude, float duration)
{
    const char *text = base->magicItemDescription.c_str();
    std::string line = text ? text : "";
    const auto replace = [&line](std::string_view token, const std::string &with) {
        const auto same = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
        auto at = std::search(line.begin(), line.end(), token.begin(), token.end(), same);
        while (at != line.end())
        {
            const auto index = static_cast<std::size_t>(at - line.begin());
            line.replace(index, token.size(), with);
            at = std::search(line.begin() + static_cast<std::ptrdiff_t>(index + with.size()), line.end(), token.begin(),
                             token.end(), same);
        }
    };
    char num[32];
    std::snprintf(num, sizeof(num), "%.0f", magnitude);
    replace("<mag>", num);
    std::snprintf(num, sizeof(num), "%.0f", duration);
    replace("<dur>", num);
    return line;
}

bool MovesValue(const RE::EffectSetting *base); // below, with the effect rows

} // namespace

bool ModifiesValue(const RE::ActiveEffect &ae, RE::ActorValue value)
{
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    const auto *base = ae.effect->baseEffect;
    const auto archetype = base->GetArchetype();
    if (!MovesValue(base))
        return false;
    const bool dual = archetype == Archetype::kDualValueModifier || archetype == Archetype::kEnhanceWeapon;
    const bool primary = base->data.primaryAV == value;
    const bool secondary = dual && base->data.secondaryAV == value;
    return primary || secondary;
}

namespace
{

// Does the effect move an actor value: the kinds the Character sheet's
// notes list by source.
bool MovesValue(const RE::EffectSetting *base)
{
    using Archetype = RE::EffectArchetypes::ArchetypeID;
    const auto archetype = base->GetArchetype();
    // Enhance Weapon is a dual value modifier underneath (its active effect
    // derives from DualValueModifierEffect in CommonLibSSE), and vanilla's
    // Elemental Fury is one, on Weapon Speed Mult.
    return archetype == Archetype::kValueModifier || archetype == Archetype::kPeakValueModifier ||
           archetype == Archetype::kDualValueModifier || archetype == Archetype::kEnhanceWeapon;
}

} // namespace

// One effect of a spell, an enchantment or a potion as a row, as a perk's
// entry is (EntryRow): the value it moves on the left, else the kind of
// effect and what it names; the magnitude on the right, signed as the
// engine applies it, with how long it runs; a tick in the third column
// where the game's own list would not show it; and greyed, with the
// reason on the name, where its conditions do not hold for this actor,
// which open beneath. The description is the author's prose and says
// what they meant; this is the record, and says what it does. The two
// parted on a Breton's Spell Warding, whose text promises an absorb
// chance that a second, hidden effect grants the player alone
// (2026-09-11). `magnitude` is SIGNED: negative for what an effect takes
// away, as an active effect's own magnitude is, and as EffectsOf reads a
// record's (Spellbreaker's -5 Stamina read as +5 once, the sign applied
// twice).
SheetRow EffectEntryRow(const RE::Effect &effect, float magnitude, const ConditionParties &parties)
{
    const auto *base = effect.baseEffect;
    const auto valueName = ValueName;

    // The effect by the name the game gives it -- Scourge, Spell Warding,
    // Fortify Health -- and beside it the amount with the value it moves:
    // "-3 Health", "+25 Resist Magic". A scroll in Nordic Souls carries
    // its perk bonuses as extra hidden entries of one value, and the name
    // is what tells them apart (2026-09-11).
    const char *called = base->GetName();
    std::string name = called && *called ? called : TypeWord(base);
    std::string amount;
    const auto duration = effect.effectItem.duration;
    if (MovesValue(base))
    {
        // A dual-value effect moves its second value by the magnitude
        // weighted.
        amount = Fmt("%+g", magnitude) + " " + valueName(base->data.primaryAV);
        if (base->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kDualValueModifier &&
            base->data.secondaryAV != RE::ActorValue::kNone)
        {
            // Two values, one per line. No dash before them: beside a
            // signed number it read as a minus.
            amount +=
                "\n" + Fmt("%+g", magnitude * base->data.secondAVWeight) + " " + valueName(base->data.secondaryAV);
        }
        // The Recover flag is what says "per second". Set, the modifier
        // moves the value once and puts it back when the effect expires:
        // a fortify, "+100 Magicka" for an hour. Clear, it moves the value
        // every second and leaves it there: a heal or a poison, "10 points
        // per second for 5 seconds". A guess from the value alone read
        // Nordic Souls' Bard Song, a hidden Peak Value Modifier of Magicka
        // with Recover set, as "+100 Magicka/s" (2026-09-13).
        const bool recovers = base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover);
        if (duration > 0 && !recovers)
            amount += Tr("/s");
    }
    else
    {
        // The kind, and what it names: the creature summoned, the weapon
        // bound.
        amount = TypeWord(base);
        if (const auto *named = base->data.associatedForm; named && named->GetName() && *named->GetName())
            amount += std::string(" ") + named->GetName();
        if (magnitude != 0.0f)
            amount += " " + Fmt("%g", magnitude);
    }

    SheetRow row = Row(name, amount);
    // The duration in its own column; blank for an effect with none,
    // which holds for as long as it runs.
    if (duration > 0)
        row.extra = TrFormat("{} s", duration);
    // The author's text with the numbers put in, the magnitude unsigned as
    // the text expects it ("Deals <mag> points"); empty where the record
    // has none, and then the table has no column for it.
    row.description = EffectDescription(base, std::abs(magnitude), static_cast<float>(duration));
    if (base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
        row.mark = kGlyphTick;
    // Two lists gate it: the spell's own entry's, and the effect record's
    // -- a Breton's hidden effects are gated on the record, a Nordic Souls
    // scroll's on the entry -- and both must hold. No verdict where a
    // condition is N/A, whose answer would be the engine's false for the
    // party nobody could name, or ?, whose answer is not to be had; nor with no Subject at all, which
    // the engine never passes to a list (a perk's tab without its argument
    // is not asked, ID 23800), so what it would say is unread.
    const std::array<const RE::TESCondition *, 2> lists{&effect.conditions, &base->conditions};
    bool asked = parties.subject != nullptr;
    for (const RE::TESCondition *conditions : lists)
    {
        if (!conditions->head)
            continue;
        for (auto &condition : ConditionRows(*conditions, parties))
        {
            asked = asked && condition.extra.empty();
            row.detail.push_back(std::move(condition));
        }
    }
    bool holds = true;
    for (const RE::TESCondition *conditions : lists)
        if (asked && conditions->head)
            holds = holds && conditions->IsTrue(parties.subject, parties.target);
    if (!holds)
        row.aside = Tr("Conditions not met");
    return row;
}

SheetSection EffectsOf(RE::Actor *actor, const RE::MagicItem *magic,
                       const std::function<float(const RE::Effect *)> &magnitude)
{
    SheetSection section{Tr("Effects"), {}, {}};
    if (!magic)
        return section;
    // Whom the conditions are asked of, as when the item lands: the one it
    // lands on as Subject, the actor using it as Target. A Self spell, a
    // potion, food, an ingredient and a worn enchantment land on the actor.
    // A poison reports Self as every potion does and lands on whoever the
    // blade strikes, as a weapon's enchantment and an aimed spell land on
    // whoever they hit: for a hostile effect, the enemy the actor is
    // fighting while there is one; for anything else nobody the page can
    // name.
    const bool onSelf = magic->GetDelivery() == RE::MagicSystem::Delivery::kSelf && !magic->IsPoison();
    const auto fighting = !onSelf && actor && actor->IsInCombat()
                              ? actor->GetActorRuntimeData().currentCombatTarget.get()
                              : RE::NiPointer<RE::Actor>{};
    RE::Actor *enemy = fighting && !fighting->IsDead() ? fighting.get() : nullptr;
    // Whose skill each effect's level is asked of: a follower's, for a
    // spell, which their combat AI never has while one effect's level is
    // above their skill (AboveSkillForAI). The player casts at any skill,
    // and a scroll, a potion or an enchantment asks none.
    const auto *spell = magic->As<RE::SpellItem>();
    auto *gated = actor && SkillGated(actor) && spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell
                      ? actor->AsActorValueOwner()
                      : nullptr;
    for (const auto *effect : ResolvedEffects(*magic))
    {
        ConditionParties parties{actor, actor};
        if (!onSelf)
        {
            const bool hostile = magic->IsPoison() || effect->baseEffect->IsHostile();
            parties.subject = hostile ? enemy : nullptr;
        }
        // The record's magnitude is unsigned; a detrimental effect takes
        // it away.
        const float amount = magnitude(effect);
        SheetRow row = EffectEntryRow(*effect, effect->baseEffect->IsDetrimental() ? -amount : amount, parties);
        if (const auto school = effect->baseEffect->GetMagickSkill(); school != RE::ActorValue::kNone)
        {
            row.school = ValueName(school);
            // A level only where it gates anything: a spell's. A staff, an
            // enchantment, a potion or a scroll asks no skill (45328 runs
            // only as the engine lists spells).
            if (const auto level = effect->baseEffect->GetMinimumSkillLevel();
                level > 0 && spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell)
            {
                row.level = std::to_string(level);
                if (const float skill = gated ? gated->GetActorValue(school) : 0.0f;
                    gated && skill < static_cast<float>(level))
                {
                    row.needsLevel = static_cast<int>(level);
                    row.hasLevel = static_cast<int>(skill);
                }
            }
        }
        section.rows.push_back(std::move(row));
    }
    return section;
}

std::vector<EffectRow> ScanActiveEffects(RE::Actor *actor)
{
    std::vector<EffectRow> out;
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    auto *effects = target ? target->GetActiveEffectList() : nullptr;
    if (!effects)
        return out;

    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        const auto *base = ae->effect->baseEffect;
        // As the game's own Active Effects list: hidden ones stay hidden,
        // and one that has run out is gone -- except a hidden effect that
        // moves a value, which the Character sheet's notes name by source
        // and which therefore wants a page: kept, marked hidden, for the
        // tab to list when the player asks for hidden effects, running and
        // applied as it is, its page saying it is hidden. A hidden
        // effect that moves nothing -- a script's, a race monitor's, a
        // cloak's -- stays off the list, as there would be many and
        // nothing to show for them.
        const bool hidden = base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
        if (hidden && (!MovesValue(base) || std::abs(ae->magnitude) < 0.05f))
            continue;
        if (ae->duration > 0.0f && ae->elapsedSeconds >= ae->duration)
            continue;
        const char *name = base->GetName();
        if (!name || !*name)
            continue;

        EffectRow row;
        row.token = ae;
        row.form = base->GetFormID();
        row.hidden = hidden;
        row.applied = EffectApplies(actor, base);
        // Running but not acting, by the engine's flag, which the sheet's
        // totals read too (ForEachActiveEffect). Not by asking the
        // conditions: the engine asks an effect record's once, when the
        // effect lands, and Adamant's Bastion asks there whether the cast
        // was dual, which reads false ever after (dev/CONDITIONS.md 10).
        row.active = !ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled);
        row.name = name;
        row.magnitude = ae->magnitude;
        row.duration = ae->duration;
        row.remaining = ae->duration > 0.0f ? ae->duration - ae->elapsedSeconds : -1.0f;
        row.remainingText = RemainingText(row.remaining);
        // The source's name, and what it links to where it has a page: the
        // worn item behind an enchantment -- the one the lookup named, so
        // the link and the name cannot part -- else the spell.
        if (ae->spell)
        {
            if (ae->spell->As<RE::EnchantmentItem>())
            {
                const WornSource worn = WornSourceOf(actor, ae->spell, ae->source);
                row.source = worn.name;
                row.linkForm = worn.form;
            }
            else
                row.linkForm = ae->spell->GetFormID();
            if (row.source.empty() && ae->spell->GetName())
                row.source = ae->spell->GetName();
        }

        // The page: one row, in the table the item page lists its effects
        // in, with the source as a last column -- what THIS effect does,
        // for how long, whether the game's list hides it, where it comes
        // from, its conditions beneath. Its source's other effects are the
        // source's business, on the item's or the spell's own page. The
        // active effect's magnitude is the engine's, signed already; its
        // duration reads as what is left of what there was.
        {
            SheetSection page{Tr("Effect"), {}, {}};
            // Its conditions as the engine asks them of a running effect:
            // of the one it is on, and of whoever cast it -- the player,
            // for a Bastion Dragonhide on a follower. For reference: the
            // row's grey is the flag.
            const auto caster = ae->caster.get();
            SheetRow line = EffectEntryRow(*ae->effect, ae->magnitude, {actor, caster.get()});
            line.aside = row.active ? "" : Tr("Inactive");
            if (ae->duration > 0.0f)
            {
                line.extra = RemainingText(ae->duration);
                line.remaining = row.remainingText;
            }
            line.link = row.source;
            // Whoever cast it, when it was not the follower: the player's
            // Courage, an enemy's Fury.
            if (caster && caster.get() != actor && caster->GetName() && *caster->GetName())
                line.link = TrFormat("{} ({})", line.link, caster->GetName());
            line.form = row.linkForm;
            page.rows.push_back(std::move(line));
            row.detail.push_back(std::move(page));
        }
        row.description = EffectDescription(base, row.magnitude, row.duration);
        // An ability's text lives on the spell, not its effect: Imperial
        // Luck's effect record says nothing, the ability says "find more
        // gold". The spell's own description, as the Magic tab reads it.
        if (row.description.empty())
        {
            if (auto *spell = ae->spell ? ae->spell->As<RE::SpellItem>() : nullptr)
            {
                RE::BSString text;
                spell->GetDescription(text, spell);
                row.description = text.c_str() ? text.c_str() : "";
            }
        }

        out.push_back(std::move(row));
    }

    ft::SortByName(out, [](const auto &item) -> std::string_view { return item.name; });
    return out;
}

// The entry points by number, from the engine's enum (BGSEntryPoint.h),
// in the Creation Kit's words: "Mod Attack Damage", "Mod Spell Cost".
#include "game/ConditionNames.inc"

namespace
{

std::string HexId(std::uint32_t id)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", id);
    return text;
}

// The engine's own entry for a condition function, which says what each
// parameter is: the script command table, indexed by the function's id.
// Null past the table's end, or where the entry's name is not ours for the
// id, which would mean the table is not laid out as assumed: said once per
// id, and the parameters are then not read as anything.
const RE::SCRIPT_FUNCTION *ConditionCommand(std::size_t id, const char *name)
{
    auto *first = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
    if (!first || id >= RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd)
        return nullptr;
    const RE::SCRIPT_FUNCTION &command = first[id];
    if (name && command.functionName && _stricmp(command.functionName, name) == 0)
        return &command;
    static std::unordered_set<std::size_t> said;
    if (said.insert(id).second)
        log::sensors.warn(
            "condition function {}: the engine's table names it {}, ours {} -- its parameters are not read", id,
            command.functionName ? command.functionName : "nothing", name ? name : "nothing");
    return nullptr;
}

// Does a parameter of this type hold a form, a record or a reference? The
// types that certainly do; every other -- a number, an actor value, a
// script variable's name, a runtime object -- is never read as one.
bool HoldsForm(RE::SCRIPT_PARAM_TYPE type)
{
    using T = RE::SCRIPT_PARAM_TYPE;
    switch (type)
    {
    case T::kInventoryObject:
    case T::kObjectRef:
    case T::kActor:
    case T::kSpellItem:
    case T::kCell:
    case T::kMagicItem:
    case T::kSound:
    case T::kTopic:
    case T::kQuest:
    case T::kRace:
    case T::kClass:
    case T::kFaction:
    case T::kGlobal:
    case T::kFurnitureOrFormList:
    case T::kObject:
    case T::kMapMarker:
    case T::kActorBase:
    case T::kContainerRef:
    case T::kWorldOrList:
    case T::kPackage:
    case T::kCombatStyle:
    case T::kMagicEffect:
    case T::kWeather:
    case T::kNPC:
    case T::kOwner:
    case T::kShaderEffect:
    case T::kFormList:
    case T::kPerk:
    case T::kImagespaceMod:
    case T::kImagespace:
    case T::kVoiceType:
    case T::kEncounterZone:
    case T::kIdleForm:
    case T::kMessage:
    case T::kInvObjectOrFormList:
    case T::kEquipType:
    case T::kObjectOrFormList:
    case T::kMusic:
    case T::kKeyword:
    case T::kRefType:
    case T::kLocation:
    case T::kForm:
    case T::kShout:
    case T::kWordOfPower:
    case T::kBGSScene:
    case T::kAssociationType:
    case T::kKnowableForm:
    case T::kRegion:
        return true;
    default:
        return false;
    }
}

// A condition's call, as the Creation Kit shows it: "HasSpell(Whirlwind
// Cloak)", "GetActorValue(Alteration)". What a parameter is comes from the
// engine's table: guessed from the value, any pointer taken for a form, it
// crashed the game on a player's spell whose condition held a pointer to
// something else (2026-09-13). Trailing zero parameters are dropped, as a
// function with no parameters holds zeros there. `subject` and `target`
// are what the parties are called after "on"; an empty Subject goes
// unsaid.
std::string ConditionCall(const RE::CONDITION_ITEM_DATA &data, const std::string &subject, const std::string &target)
{
    const auto id = static_cast<std::size_t>(data.functionData.function.get());
    const char *name = id < kConditionNames.size() && *kConditionNames[id] ? kConditionNames[id] : nullptr;
    std::string call = name ? name : TrFormat("Function {}", id);

    const RE::SCRIPT_FUNCTION *command = ConditionCommand(id, name);
    std::vector<std::string> args;
    for (std::size_t i = 0; i < std::size(data.functionData.params); ++i)
    {
        const void *param = data.functionData.params[i];
        const auto raw = reinterpret_cast<std::uintptr_t>(param);
        const bool typed = command && command->params && i < command->numParams;
        const RE::SCRIPT_PARAM_TYPE type = typed ? command->params[i].paramType.get() : RE::SCRIPT_PARAM_TYPE::kInt;
        if (typed && HoldsForm(type) && param)
        {
            // The form's name; a keyword has none, only an editor ID, which
            // its record keeps in memory; failing both, the ID. A perk goes
            // by its editor ID first: the ranks of one perk share a name,
            // and a rank's own entry is conditioned on the NEXT rank not
            // being held ("HasPerk(Augmented Frost) = 0" on Augmented
            // Frost read as nonsense until it said AugmentedFrost60).
            const auto *form = static_cast<const RE::TESForm *>(param);
            const char *formName = form->GetName();
            const char *editorID = form->GetFormEditorID();
            const bool byEditorID = form->Is(RE::FormType::Perk) || !formName || !*formName;
            args.push_back(form->IsPlayerRef()                   ? Tr("Player")
                           : byEditorID && editorID && *editorID ? editorID
                           : formName && *formName               ? formName
                                                                 : HexId(form->GetFormID()));
        }
        else if (typed && type == RE::SCRIPT_PARAM_TYPE::kActorValue)
        {
            auto *list = RE::ActorValueList::GetSingleton();
            auto *info = list ? list->GetActorValueInfo(static_cast<RE::ActorValue>(raw)) : nullptr;
            args.push_back(info && info->GetFullName() && *info->GetFullName() ? info->GetFullName()
                                                                               : std::to_string(raw));
        }
        else
        {
            // A pointer to what is not a form has nothing safe to print.
            args.push_back(raw > 0xFFFFFFFFu ? "?" : std::to_string(raw));
        }
    }
    while (!args.empty() && args.back() == "0")
        args.pop_back();
    call += "(";
    for (std::size_t i = 0; i < args.size(); ++i)
        call += (i ? ", " : "") + args[i];
    call += ")";

    using Object = RE::CONDITIONITEMOBJECT;
    switch (data.object.get())
    {
    case Object::kSelf:
        if (!subject.empty())
            call = TrFormat("{} on {}", call, subject);
        break;
    case Object::kTarget:
        call = TrFormat("{} on {}", call, target);
        break;
    case Object::kCombatTarget:
        call = TrFormat("{} on {}", call, Tr("Combat Target"));
        break;
    case Object::kRef: {
        // A particular reference, named in the condition: the player, as a
        // rule, for a perk given to followers that turns on with one of
        // the player's.
        const auto ref = data.runOnRef.get();
        if (ref && ref->IsPlayerRef())
            call = TrFormat("{} on {}", call, Tr("Player"));
        else if (ref && ref->GetDisplayFullName() && *ref->GetDisplayFullName())
            call = TrFormat("{} on {}", call, ref->GetDisplayFullName());
        else
            call = TrFormat("{} on {}", call, ref ? HexId(ref->GetFormID()) : std::string(Tr("Reference")));
        break;
    }
    case Object::kLinkedRef:
        call = TrFormat("{} on {}", call, Tr("Linked Reference"));
        break;
    case Object::kQuestAlias:
        call = TrFormat("{} on {}", call, Tr("Quest Alias"));
        break;
    case Object::kPackData:
        call = TrFormat("{} on {}", call, Tr("Package Data"));
        break;
    case Object::kEventData:
        call = TrFormat("{} on {}", call, Tr("Event Data"));
        break;
    case Object::kCommandTarget:
        call = TrFormat("{} on {}", call, Tr("Command Target"));
        break;
    }
    return call;
}

// A party by the name the panel gives it: the player is "Player", as
// everywhere in the panel.
std::string PartyName(RE::TESObjectREFR *ref)
{
    if (ref->IsPlayerRef())
        return Tr("Player");
    const char *name = ref->GetDisplayFullName();
    return name && *name ? name : HexId(ref->GetFormID());
}

} // namespace

// A condition list as rows: the call, the comparison ("== 1", "OR" after
// it where the list reads so), and a tick where the parties meet it now.
std::vector<SheetRow> ConditionRows(const RE::TESCondition &condition, const ConditionParties &parties, const char *on)
{
    std::vector<SheetRow> rows;
    for (const auto *item = condition.head; item; item = item->next)
    {
        const auto &data = item->data;
        // The comparison, in the enum's order: =, !=, >, >=, <, <=.
        constexpr std::array<const char *, 6> kOps{"=", "!=", ">", ">=", "<", "<="};
        const auto opIndex = static_cast<std::size_t>(data.flags.opCode);
        const char *op = opIndex < kOps.size() ? kOps[opIndex] : "?";
        std::string value;
        if (data.flags.global)
        {
            const auto *global = data.comparisonValue.g;
            value = global && global->GetFormEditorID() && *global->GetFormEditorID()
                        ? global->GetFormEditorID()
                        : (global ? HexId(global->GetFormID()) : "?");
            if (global)
                value += Fmt(" (%g)", global->value);
        }
        else
            value = Fmt("%g", data.comparisonValue.f);
        using Object = RE::CONDITIONITEMOBJECT;
        const auto object = data.object.get();
        // The party the engine runs it on (TESConditionItem::IsTrue,
        // dev/CONDITIONS.md 10): the Subject, or through the Subject its
        // combat target or linked reference; the Target; the swap flag
        // trading the two when both are there. A named reference needs
        // neither, and a quest alias, package data or a story event is
        // context no sheet has, asked for the false it gives.
        const bool swapped = data.flags.swapTarget && parties.subject && parties.target;
        bool onTarget = object == Object::kTarget;
        if (swapped && (object == Object::kSelf || object == Object::kTarget))
            onTarget = !onTarget;
        RE::TESObjectREFR *runsOn = onTarget ? parties.target : parties.subject;
        // The call names whom it runs on, so no hover is needed to see it;
        // the Creation Kit's word where there is nobody to name. A perk's
        // later tab keeps the words: its Subject is the entry's argument,
        // listed and not asked.
        std::string subject = on ? "" : parties.subject ? PartyName(parties.subject) : Tr("Subject");
        std::string target = on ? Tr("Target") : parties.target ? PartyName(parties.target) : Tr("Target");
        if (swapped && !on)
            std::swap(subject, target);
        const std::string call = ConditionCall(data, subject, target);
        SheetRow row = Row(on ? TrFormat("{} on {}", call, on) : call,
                           std::string(op) + " " + value + (data.flags.isOR ? "  OR" : ""));
        // Met only where the condition is on the actor: one on another
        // argument -- the spell, the weapon, the target -- has nothing to
        // be asked of here, and a tick from asking the actor would lie.
        if (!on)
        {
            const bool needsParty = object == Object::kSelf || object == Object::kTarget ||
                                    object == Object::kCombatTarget || object == Object::kLinkedRef ||
                                    object == Object::kCommandTarget;
            // N/A: a party it needs is not there -- nobody being fought,
            // the caster gone. ?: there, but the answer cannot be known
            // from a sheet: EffectWasDualCast reads a flag held only while
            // a dual-cast effect is being added (handler 21719), and is 0
            // afterwards whatever the cast was.
            if (needsParty && !runsOn)
                row.extra = Tr("N/A");
            else if (data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kEffectWasDualCast)
                row.extra = "?";
            else
            {
                RE::ConditionCheckParams params(parties.subject, parties.target);
                if (item->IsTrue(params))
                    row.icon = kGlyphTick;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace ft::game
