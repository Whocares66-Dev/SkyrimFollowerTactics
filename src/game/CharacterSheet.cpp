#include "game/CharacterSheet.h"

#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"

#include "game/Bag.h"
#include "game/CustomSkillsFramework.h"
#include "game/Effects.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
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

// "83%", or past the engine's cap "110% (85%)": what the gear adds up to
// first, and in brackets what is actually applied. The gap between them says
// how much of the follower's kit is doing nothing.
std::string CappedPercent(float value, float cap)
{
    // "90% (75%)": the value, and in parentheses what the cap makes of it.
    if (value > cap)
        return Fmt("%.0f%%", value) + " (" + Fmt("%.0f%%", cap) + ")";
    return Fmt("%.0f%%", value);
}

// What one hand holds, as rows: a weapon and its numbers, a spell and its
// cost and strongest effect, a shield and its rating, or a torch. An empty
// hand adds no rows, and the caller shows no table for it.
void HandRows(RE::Actor *actor, bool left, std::vector<SheetRow> &rows)
{
    RE::TESForm *held = actor->GetEquippedObject(left);
    if (!held)
        return;

    if (auto *weapon = held->As<RE::TESObjectWEAP>())
    {
        rows.push_back(Row(Tr("Weapon"), NameOr(weapon, "?")));
        rows.back().form = weapon->GetFormID();
        // In their hands: the carried item, for its tempering. Each figure
        // as it applies now, written out on hover.
        const Carried carried = CarriedOf(actor, weapon);
        {
            SheetRow row;
            const float damage = WeaponDamage(actor, weapon, carried.entry.get(), &row.breakdown);
            row.label = Tr("Damage");
            row.value = Fmt("%.0f", damage);
            rows.push_back(std::move(row));
        }
        // The critical pair and the speed in the details page's words and
        // order. Reach and stagger are the record's and on that page. A
        // critical that never lands or lands for nothing is no critical:
        // neither row, rather than one of them beside a 0.
        {
            SheetRow chance;
            const float percent = CritChance(actor, weapon, &chance.breakdown);
            if (const auto critDamage = weapon->GetCritDamage(); critDamage > 0 && percent >= 0.5f)
            {
                rows.push_back(Row(Tr("Critical Damage"), std::to_string(critDamage)));
                chance.label = Tr("Critical Chance");
                chance.value = Fmt("%.0f%%", percent);
                rows.push_back(std::move(chance));
            }
        }
        {
            SheetRow row;
            float speed = WeaponSpeed(actor, weapon, left, &row.breakdown);
            // The speed the swing plays at, where the animation graph holds
            // it: the engine's figure after every plugin between, which no
            // actor value keeps (Comprehensive Attack Rate Patch caps and
            // tapers it in its detour of the engine's speed, 2026-09-14). The
            // formula stays as the lines and the fallback, and what it misses
            // is Other. The variable is the whole speed, record included: a
            // dagger with no speed effects read its record's 1.30
            // (2026-09-15). It moves only while the game runs, so with the
            // clock frozen behind the panel it can lag -- 1.00 on a first
            // open, 1.30 once the panel was closed and opened -- and the lag
            // reads as Other, as an enchantment equipped from the panel does.
            if (float live = 0.0f; actor->GetGraphVariableFloat(left ? "leftWeaponSpeedMult" : "weaponSpeedMult", live))
            {
                log::sensors.debug("{} {} speed: graph {:.3f}, formula {:.3f}", Describe(actor), NameOr(weapon, "?"),
                                   live, speed);
                speed = live;
                row.breakdown.total = live;
                ft::Close(row.breakdown);
            }
            row.label = Tr("Speed");
            row.value = Fmt("%.2f", speed);
            rows.push_back(std::move(row));
        }
        if (weapon->IsBow() || weapon->IsCrossbow())
        {
            if (auto *ammo = actor->GetCurrentAmmo())
            {
                rows.push_back(Row(Tr("Ammo"), NameOr(ammo, "?")));
                rows.back().form = ammo->GetFormID();
                rows.push_back(Row(Tr("Ammo Damage"), Fmt("%.0f", ammo->GetRuntimeData().data.damage)));
            }
            else
            {
                rows.push_back(Row(Tr("Ammo"), Tr("none")));
            }
        }
        return;
    }

    if (auto *spell = held->As<RE::SpellItem>())
    {
        rows.push_back(Row(Tr("Spell"), NameOr(spell, "?")));
        rows.back().form = spell->GetFormID();
        {
            SheetRow row = Row(Tr("Cost"), Fmt("%.0f", spell->CalculateMagickaCost(actor)));
            row.breakdown = SpellCostBreakdown(actor, spell);
            rows.push_back(std::move(row));
        }
        if (const auto *effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
        {
            // As they cast it -- perks and Fortify effects in -- which is
            // what the Effects table shows; the record's 8 beside the
            // table's 12 read as a mistake (Blood Aura, 2026-09-13).
            std::string what = NameOr(effect->baseEffect, "?");
            what += " " + Fmt("%.0f", ActualMagnitude(actor, spell, effect));
            if (const float duration = ActualDuration(actor, spell, effect); duration > 0.0f)
                what = TrFormat("{} for {} s", what, Fmt("%.0f", duration));
            rows.push_back(Row(Tr("Effect"), what));
        }
        return;
    }

    if (auto *armor = held->As<RE::TESObjectARMO>())
    {
        const bool shield = armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        rows.push_back(Row(shield ? Tr("Shield") : Tr("Held"), NameOr(armor, "?")));
        rows.back().form = armor->GetFormID();
        const Carried carried = CarriedOf(actor, armor);
        SheetRow row;
        const float rating = ArmorRating(actor, armor, carried.entry.get(), &row.breakdown);
        row.label = Tr("Armor");
        row.value = Fmt("%.0f", rating);
        rows.push_back(std::move(row));
        return;
    }

    if (held->Is(RE::FormType::Light))
    {
        rows.push_back(Row(Tr("Held"), NameOr(held, Tr("torch"))));
        rows.back().form = held->GetFormID();
        return;
    }

    rows.push_back(Row(Tr("Held"), NameOr(held, "?")));
}

} // namespace

std::vector<SheetSection> BuildCharacterSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return out;

    const auto av = [owner](RE::ActorValue value) { return owner->GetActorValue(value); };

    {
        SheetSection s{Tr("General"), {}, {}};
        // The reference and the base record, as the console names them --
        // what "prid" takes, and what the log calls the follower.
        {
            char id[16];
            std::snprintf(id, sizeof(id), "%08X", actor->GetFormID());
            s.rows.push_back(Row(Tr("Ref ID"), id));
            const auto *base = actor->GetActorBase();
            std::snprintf(id, sizeof(id), "%08X", base ? base->GetFormID() : 0u);
            s.rows.push_back(Row(Tr("Base ID"), id));
        }
        s.rows.push_back(Row(Tr("Name"), NameOr(actor, "?")));
        auto *race = actor->GetRace();
        s.rows.push_back(Row(Tr("Race"), NameOr(race, "?")));
        if (const auto *base = actor->GetActorBase())
        {
            const auto sex = base->GetSex();
            if (sex == RE::SEX::kMale || sex == RE::SEX::kFemale)
                s.rows.push_back(Row(Tr("Gender"), sex == RE::SEX::kMale ? Tr("Male") : Tr("Female")));
        }
        // Speed is the multiplier every buff lands on -- 100 for plain, and
        // a Fortify Speed or a Slow moves it -- so it reads the same
        // standing and sprinting. What moves it and by whom is the hover
        // text, as for the regen rates.
        {
            SheetRow row = Row(Tr("Speed"), Fmt("%.0f%%", av(RE::ActorValue::kSpeedMult)));
            row.breakdown = ValueBreakdown(actor, RE::ActorValue::kSpeedMult, "%");
            s.rows.push_back(std::move(row));
        }
        s.rows.push_back(Row(Tr("Noise"), Fmt("%.0f%%", av(RE::ActorValue::kMovementNoiseMult) * 100.0)));
        out.push_back(std::move(s));
    }

    // Attack: what each hand holds, whatever it is. The old Attack section
    // knew only weapons, which left a mage's page saying "unarmed". A hand
    // holding nothing gets no table; with both empty, the one thing worth
    // saying is what their fists do. A two-handed weapon or a spell cast
    // with both hands is one thing in both, and one table: a Left Hand
    // table saying "the same" was a table of nothing.
    {
        RE::TESForm *held = actor->GetEquippedObject(false);
        auto *weapon = held ? held->As<RE::TESObjectWEAP>() : nullptr;
        auto *spell = held ? held->As<RE::SpellItem>() : nullptr;
        const bool both = TwoHanded(weapon) || (spell && spell->IsTwoHanded());
        SheetSection right{both ? Tr("Both Hands") : Tr("Right Hand"), {}, Tr("Attack")};
        HandRows(actor, false, right.rows);
        SheetSection left{Tr("Left Hand"), {}, Tr("Attack")};
        if (!both)
            HandRows(actor, true, left.rows);

        if (right.rows.empty() && left.rows.empty())
        {
            SheetSection s{Tr("Attack"), {}, {}};
            s.rows.push_back(Row(Tr("Held"), Tr("unarmed")));
            s.rows.push_back(Row(Tr("Base Damage"), Fmt("%.0f", av(RE::ActorValue::kUnarmedDamage))));
            out.push_back(std::move(s));
        }
        else
        {
            if (!right.rows.empty())
                out.push_back(std::move(right));
            if (!left.rows.empty())
                out.push_back(std::move(left));
        }
    }

    {
        SheetSection s{Tr("Defense"), {}, {}};
        // The armour rating the game shows is not the one it applies: each
        // piece worn adds a hidden bonus before the scaling factor, which is
        // why a displayed 609 lands at 85% and not 73%. One row: the rating
        // WITH that bonus in its own units, so the hover text's pieces and
        // bonus sum to it, and in parentheses the share of a blow it turns
        // away -- the same DamageReduction the Armor condition reads, so the
        // sheet and the rules cannot disagree. Robes and boots alone read
        // 50 (6%): two pieces' hidden bonus and no rating.
        const float resistCap = GameSetting("fPlayerMaxResistance", 85.0f);
        // The share of a blow turned away, clamped at the cap: one
        // percent, the one that applies -- "582 (75%)" in a list that caps
        // at 75.
        SheetRow armorRow = Row(Tr("Armor"), Fmt("%.0f", EffectiveArmor(actor)) + " (" +
                                                 Fmt("%.0f%%", DamageReduction(actor) * 100.0f) + ")");
        armorRow.breakdown = ArmorBreakdown(actor);
        s.rows.push_back(std::move(armorRow));
        // Each resistance with where it comes from as its hover text: the
        // ring, the potion, the race.
        const auto resist = [&](const char *label, RE::ActorValue value, bool capped) {
            SheetRow row = Row(label, capped ? CappedPercent(av(value), resistCap) : Fmt("%.0f%%", av(value)));
            row.breakdown = ValueBreakdown(actor, value, "%");
            s.rows.push_back(std::move(row));
        };
        // The chance to reflect a blow back, after the rating it did not
        // turn away.
        resist(Tr("Reflect"), RE::ActorValue::kReflectDamage, false);
        // Magic first, with the chance to absorb a spell outright beside
        // it, then the elements, then poison; disease last, the one that
        // matters to the player alone.
        resist(Tr("Magic"), RE::ActorValue::kResistMagic, true);
        resist(Tr("Spell Absorb"), RE::ActorValue::kAbsorbChance, false);
        resist(Tr("Fire"), RE::ActorValue::kResistFire, true);
        resist(Tr("Frost"), RE::ActorValue::kResistFrost, true);
        resist(Tr("Shock"), RE::ActorValue::kResistShock, true);
        resist(Tr("Poison"), RE::ActorValue::kPoisonResist, true);
        resist(Tr("Disease"), RE::ActorValue::kResistDisease, false);
        out.push_back(std::move(s));
    }

    {
        // The rate the follower regenerates at: the rate, a share of the pool
        // a second, times its multiplier, where a Magicka Rate Mult of 200 is
        // x 2. A buff lands on either: robes of Destruction's "magicka
        // regenerates 100% faster" is +100 on the multiplier, and Mundus's
        // Elfborn stone is +3 on the rate itself. Written as the two factors,
        // each over its terms: as lines of flat rates, each multiplier source
        // read as the rate it added, and a stone that doubled the rate
        // doubled them unseen (2026-09-15). Rate times multiplier is the
        // engine's regeneration (38460), which also slows each in a fight
        // and passes health through the Mod Recovered Health perks; this is
        // the rate out of a fight, without those perks.
        SheetSection s{Tr("Regen"), {}, {}};
        const auto regen = [&](const char *label, RE::ActorValue rate, RE::ActorValue mult) {
            const float current = av(rate);
            const float factor = av(mult) / 100.0f;
            const float total = current * factor;
            SheetRow row = Row(label, Fmt("%.2f%%", total));
            ft::Breakdown &b = row.breakdown;
            b.decimals = 2;
            b.unit = "%";
            ft::Start(b, ValueName(rate), current).detail = ValueLines(actor, rate, current);
            if (factor != 1.0f)
                ft::Multiply(b, ValueName(mult), factor).detail = ValueLines(actor, mult, factor, 0.01f);
            b.total = total;
            ft::Close(b);
            s.rows.push_back(std::move(row));
        };
        regen(Tr("Health Rate"), RE::ActorValue::kHealRate, RE::ActorValue::kHealRateMult);
        regen(Tr("Stamina Rate"), RE::ActorValue::kStaminaRate, RE::ActorValue::kStaminaRateMult);
        regen(Tr("Magicka Rate"), RE::ActorValue::kMagickaRate, RE::ActorValue::kMagickaRateMult);
        out.push_back(std::move(s));
    }

    return out;
}

RE::TESCombatStyle *LiveCombatStyle(RE::Actor *actor)
{
    if (!actor)
        return nullptr;
    auto *npc = actor->GetActorBase();
    auto *record = npc ? npc->GetCombatStyle() : nullptr;
    auto *controller = actor->GetActorRuntimeData().combatController;
    return controller && controller->combatStyle ? controller->combatStyle : record;
}

bool DualWieldAllowed(RE::Actor *actor)
{
    // The style tunes the combat AI; the player holds what they like.
    if (actor && actor->IsPlayerRef())
        return true;
    // With the requirement off, anyone may hold two weapons and what the AI
    // makes of them is the AI's business (game/Settings.h).
    if (!CurrentSettings().requireDualWieldStyle)
        return true;
    auto *style = LiveCombatStyle(actor);
    return !style || style->flags.all(RE::TESCombatStyle::FLAG::kAllowDualWielding);
}

std::vector<SheetSection> BuildCombatStyleSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *npc = actor->GetActorBase();
    auto *record = npc ? npc->GetCombatStyle() : nullptr;
    auto *controller = actor->GetActorRuntimeData().combatController;
    auto *live = LiveCombatStyle(actor);
    if (!live)
        return out;

    // Two scales, read off every style in the load order (163 of them):
    // the chances and movement multipliers run 0 to 1, and the score and
    // attack multipliers run 0 to 10, with 1 as the neutral value.
    const auto chance = [](float x) { return Fmt("%.2f", x) + " / 1"; };
    const auto score = [](float x) { return Fmt("%.2f", x) + " / 10"; };
    // Hover text: the Creation Kit wiki's word on each field, as bullets.
    // dev/COMBAT_AI.md "Combat styles" has the page.
    const auto note = [](SheetRow row, const char *text) {
        row.note = text;
        return row;
    };

    using Flag = RE::TESCombatStyle::FLAG;
    const bool flanking = live->flags.all(Flag::kFlankingStyle);
    {
        SheetSection s{Tr("Style"), {}, {}};
        // A runtime copy has a 0xFF FormID; a record's is its plugin's.
        const bool ours = (live->GetFormID() & 0xFF000000U) == 0xFF000000U;
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", live->GetFormID());
        s.rows.push_back(Row(Tr("Base ID"), ours ? TrFormat("{}  (our copy)", std::string(id)) : std::string(id)));
        if (controller && controller->combatStyle && record && controller->combatStyle != record)
        {
            char recordId[16];
            std::snprintf(recordId, sizeof(recordId), "%08X", record->GetFormID());
            s.rows.push_back(Row(Tr("On Record"), recordId));
        }
        s.rows.push_back(note(Row(Tr("Close Range"), flanking ? Tr("Flanking") : Tr("Dueling")),
                              Tr("- Dueling: circles, falls back\n"
                                 "- Flanking: keeps a distance, stalks")));
        // A tick when allowed, as the equipped state is shown; no row at all
        // when not.
        if (live->flags.all(Flag::kAllowDualWielding))
        {
            SheetRow row = note(Row(Tr("Dual Wield"), ""), Tr("- Can hold a weapon in each hand\n"
                                                              "- Staves do not count"));
            row.icon = kGlyphTick;
            s.rows.push_back(std::move(row));
        }
        out.push_back(std::move(s));
    }
    {
        const auto &g = live->generalData;
        SheetSection s{Tr("General"), {}, {}};
        s.rows.push_back(note(Row(Tr("Offensive"), chance(g.offensiveMult)), Tr("- Higher: attacks more often\n"
                                                                                "- More power attacks")));
        s.rows.push_back(
            note(Row(Tr("Defensive"), chance(g.defensiveMult)), Tr("- Higher: blocks more, holds it longer\n"
                                                                   "- Bashes more, given a shield or a weapon")));
        s.rows.push_back(note(Row(Tr("Group Offensive"), chance(g.groupOffensiveMult)),
                              Tr("- Replaces Offensive when several attack one target\n"
                                 "- Higher: stays offensive in a crowd")));
        out.push_back(std::move(s));
    }
    {
        // The six that decide what they prefer to hold.
        const auto &g = live->generalData;
        SheetSection s{Tr("Equipment Scores"), {}, {}};
        const char *kScore = Tr("- Multiplies the damage of attacks of this kind\n"
                                "- The highest score is what gets used\n"
                                "- A weak weapon needs a high score to beat a strong spell");
        s.rows.push_back(note(Row(Tr("Melee"), score(g.meleeScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Magic"), score(g.magicScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Ranged"), score(g.rangedScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Staff"), score(g.staffScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Shout"), score(g.shoutScoreMult)), kScore));
        s.rows.push_back(note(Row(Tr("Unarmed"), score(g.unarmedScoreMult)), kScore));
        out.push_back(std::move(s));
    }
    {
        const auto &m = live->meleeData;
        SheetSection s{Tr("Melee"), {}, {}};
        s.rows.push_back(note(Row(Tr("Attack, Staggered"), score(m.attackIncapacitatedMult)),
                              Tr("- Higher: attacks a staggered target more")));
        s.rows.push_back(note(Row(Tr("Power Attack, Staggered"), score(m.powerAttackIncapacitatedMult)),
                              Tr("- Higher: power-attacks a staggered target more")));
        s.rows.push_back(note(Row(Tr("Power Attack, Blocking"), score(m.powerAttackBlockingMult)),
                              Tr("- Higher: power-attacks a blocking target more\n"
                                 "- Breaks the block")));
        s.rows.push_back(
            note(Row(Tr("Bash"), score(m.bashMult)), Tr("- Higher: bashes more, with a shield or a bash attack\n"
                                                        "- A bash can stagger")));
        s.rows.push_back(note(Row(Tr("Bash, Recoiled"), score(m.bashRecoilMult)),
                              Tr("- Higher: bashes a target recoiling from its blocked attack")));
        s.rows.push_back(
            note(Row(Tr("Bash, Attacking"), score(m.bashAttackMult)), Tr("- Higher: bashes a target mid-attack")));
        s.rows.push_back(note(Row(Tr("Bash, Power Attacking"), score(m.bashPowerAttackMult)),
                              Tr("- Higher: bashes a target mid-power-attack")));
        out.push_back(std::move(s));
    }
    {
        // Only the active pair: dueling circles and falls back, flanking
        // keeps a distance and stalks. The other pair is dead data.
        const auto &c = live->closeRangeData;
        SheetSection s{Tr("Range"), {}, {}};
        if (flanking)
        {
            s.rows.push_back(
                note(Row(Tr("Flank Distance"), chance(c.flankDistanceMult)), Tr("- Distance kept while flanking")));
            s.rows.push_back(
                note(Row(Tr("Stalk Time"), chance(c.stalkTimeMult)), Tr("- Time spent flanking before attacking")));
        }
        else
        {
            s.rows.push_back(note(Row(Tr("Circle"), chance(c.circleMult)), Tr("- Higher: circles the target more")));
            s.rows.push_back(note(Row(Tr("Fallback"), chance(c.fallbackMult)), Tr("- Chance to back off")));
        }
        s.rows.push_back(note(Row(Tr("Strafe"), chance(live->longRangeData.strafeMult)),
                              Tr("- Higher: strafes more to dodge projectiles at range")));
        out.push_back(std::move(s));
    }
    return out;
}
namespace
{

// An effect's time left, written out as the engine made its duration: the
// record's duration; a dual cast (id 34058); the caster's Mod Spell Duration
// entries given the spell and the target, then the target's Mod Incoming
// Spell Duration given the spell (ActiveEffect::AdjustForPerks, id 34053);
// less the time run (dev/MODIFIERS.md). Whatever else moves a duration is
// not read, and shows as Other.
ft::Breakdown RemainingBreakdown(const RE::ActiveEffect &effect)
{
    ft::Breakdown b;
    if (!effect.effect || effect.duration <= 0.0f)
        return b;
    b.unit = Tr(" s");
    b.totalLabel = Tr("Remaining");
    ft::Start(b, Tr("Base"), static_cast<float>(effect.effect->effectItem.duration));
    const auto caster = effect.GetCasterActor();
    // A dual cast's effectiveness as id 26518 makes it: the base setting
    // plus the mult setting times the spell's cost for the caster, 2.5 and
    // 0 in Nordic Souls, 2.2 and 0 in vanilla. It scales the duration where
    // the record has Power Affects Duration, Scrambled Bugs' magicEffectFlags
    // reading, which Nordic Souls runs; vanilla's own body scales an effect
    // with No Magnitude too. The engine skips an effectiveness of 1 or
    // below 0.
    const auto *base = effect.effect->baseEffect;
    if (effect.flags.any(RE::ActiveEffect::Flag::kDual) && base &&
        base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kPowerAffectsDuration))
    {
        const float fixed = GameSetting("fMagicDualCastingEffectivenessBase", 2.2f);
        const float perCost = GameSetting("fMagicDualCastingEffectivenessMult", 0.0f);
        const float cost = perCost != 0.0f && effect.spell ? effect.spell->CalculateMagickaCost(caster.get()) : 0.0f;
        const float effectiveness = fixed + perCost * cost;
        if (effectiveness >= 0.0f && effectiveness != 1.0f)
        {
            ft::BreakdownLine &line = ft::Multiply(b, Tr("Dual cast"), effectiveness);
            if (perCost != 0.0f)
                line.amountText = "x (" + Fmt("%g", fixed) + " + " + Fmt("%g", perCost) + " x " + Fmt("%g", cost) + ")";
        }
    }
    // Not ActiveEffect::GetTargetActor: CommonLib reinterpret_casts the
    // MagicTarget base to Actor, a pointer 0xA0 inside the actor, and the
    // perk check handed it crashed calling a virtual through it
    // (2026-09-13). The target's own accessor gives the reference.
    auto *targetRef = effect.target ? effect.target->GetTargetStatsObject() : nullptr;
    auto *target = targetRef ? targetRef->As<RE::Actor>() : nullptr;
    if (caster)
        AddEntryPointLines(b, caster.get(), RE::BGSEntryPoint::ENTRY_POINT::kModSpellDuration, {effect.spell, target});
    if (target)
        AddEntryPointLines(b, target, RE::BGSEntryPoint::ENTRY_POINT::kModIncomingSpellDuration, {effect.spell});
    ft::Add(b, Tr("Elapsed"), -effect.elapsedSeconds);
    b.total = (std::max)(0.0f, effect.duration - effect.elapsedSeconds);
    ft::Close(b);
    return b;
}

} // namespace

std::vector<SummonView> ScanSummons(RE::Actor *actor)
{
    std::vector<SummonView> out;
    const auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    if (!process || !process->middleHigh)
        return out;
    for (const auto &commanded : process->middleHigh->commandedActors)
    {
        auto summon = commanded.commandedActor.get();
        if (!summon)
            continue;
        SummonView view;
        view.id = summon->GetFormID();
        view.baseId = summon->GetActorBase() ? summon->GetActorBase()->GetFormID() : 0;
        view.name = NameOr(summon.get(), "?");
        view.level = summon->GetLevel();
        view.health = ReadStat(summon.get(), RE::ActorValue::kHealth);
        view.magicka = ReadStat(summon.get(), RE::ActorValue::kMagicka);
        view.stamina = ReadStat(summon.get(), RE::ActorValue::kStamina);
        view.healthBreakdown = ValueBreakdown(summon.get(), RE::ActorValue::kHealth, "");
        view.magickaBreakdown = ValueBreakdown(summon.get(), RE::ActorValue::kMagicka, "");
        view.staminaBreakdown = ValueBreakdown(summon.get(), RE::ActorValue::kStamina, "");
        // The commanding effect runs on the FOLLOWER: its duration less its
        // elapsed time is how long the summon has left. A reanimate's effect
        // is a ReanimateEffect; a summon's a SummonCreatureEffect.
        if (const auto *effect = commanded.activeEffect)
        {
            if (effect->duration > 0.0f)
            {
                view.remaining = (std::max)(0.0f, effect->duration - effect->elapsedSeconds);
                view.remainingBreakdown = RemainingBreakdown(*effect);
            }
            view.raised = effect->GetBaseObject() &&
                          effect->GetBaseObject()->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kReanimate;
        }
        view.sheet = BuildCharacterSheet(summon.get());
        out.push_back(std::move(view));
    }
    return out;
}

} // namespace ft::game
