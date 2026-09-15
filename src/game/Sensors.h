#pragma once
// Sensors: turn a live RE::Actor into an ft::Snapshot.
//
// This is the boundary. Everything above it (ft::Snapshot, ft::Evaluate) is
// RE::-free and unit tested; everything below is imperative Skyrim code that
// can only be verified by playing. Keep this file thin and obvious.

#include "core/Breakdown.h"
#include "core/Rule.h"
#include "core/Snapshot.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// The equip slot records, by FormID in Skyrim.esm: read off the game, not
// from memory, which had them wrong. Not through the default object
// table, which did not answer for them on this game (01:29): a null slot
// handed to an equip means "the default", and the default is the right
// hand -- which is where every left-hand dagger went (01:51).
inline constexpr std::uint32_t kRightHandSlot = 0x00013F42;
inline constexpr std::uint32_t kLeftHandSlot = 0x00013F43;
inline constexpr std::uint32_t kEitherHandSlot = 0x00013F44;
inline constexpr std::uint32_t kBothHandsSlot = 0x00013F45;
inline constexpr std::uint32_t kVoiceSlot = 0x00025BEE;

// One thing the actor carries, as the engine's inventory map reports it:
// how many, and the entry -- a copy, as GetInventory makes, with its
// extra lists -- or null for none carried. The twelve hand-written walks
// of that map were one lookup each.
struct Carried
{
    std::int32_t count{0};
    std::unique_ptr<RE::InventoryEntryData> entry;
};
[[nodiscard]] Carried CarriedOf(RE::Actor *actor, RE::TESBoundObject *object);

// Every effect running on the actor that is live -- with a base effect,
// not inactive, not dispelled -- in one walk. What the snapshot's
// statuses, running effects and active spells read, and the sheets'
// contributions. (The Effects tab walks the list itself: it shows the
// inactive ones, greyed.)
void ForEachActiveEffect(RE::Actor *actor, const std::function<void(RE::ActiveEffect &)> &fn);

// Seconds until the voice can shout again, 0 when it can. The engine
// keeps this per actor, NPCs too; negative or nonsense (an hour or more)
// reads as "can shout".
[[nodiscard]] float VoiceRecoveryOf(RE::Actor *actor);

// The hands are asked one at a time, and answered per COPY: with the same
// dagger in each hand -- two entries of one record -- the left's poison and
// charge are the left's, read off the extra list worn in that hand, not
// the right's read twice. A two-hander sits in the right hand and the left
// reports it again; asked about the left, these say nothing is there.
[[nodiscard]] bool TwoHanded(const RE::TESObjectWEAP *weapon);

// The extra list of the copy worn in that hand -- Left, Right, or either
// for a thing with no hand -- or the one worn nowhere, for an equip into
// the other hand. Null where there is none: an item with no extra data,
// which the engine takes as the plain case.
[[nodiscard]] RE::ExtraDataList *WornList(RE::Actor *actor, RE::TESBoundObject *object, Hand hand);
[[nodiscard]] RE::ExtraDataList *UnwornList(RE::Actor *actor, RE::TESBoundObject *object);
// Is the copy on this list worn in those hands: the right and armour are
// Worn, the left is WornLeft, None asks for either. False for no list.
[[nodiscard]] bool ListWorn(const RE::ExtraDataList *list, Hand hands);
// The same asked of a copy of this object: the worn marks by hand are a
// weapon's. A shield or a torch in the left hand carries the one Worn
// mark, as every piece of armour does, so for anything but a weapon the
// left hand or no hand is any mark, and the right hand is none.
[[nodiscard]] bool WornIn(const RE::TESBoundObject *object, const RE::ExtraDataList *list, Hand hands);
// Is the copy on this list a row of its own on the Inventory tab, rather
// than one of the plain stack: the engine's own answer, IsInventoryStackable
// (11598), which InventoryChanges::GetInventoryItemAt asks of each list as
// it numbers an inventory's items, and which the engine's equip asks before
// it reaches for "a plain copy". Its table, read from the running game
// 2026-09-13: tempering, a charge, a poison, a custom name, an enchantment
// and a soul keep a copy apart; ownership, a unique id, a reference handle,
// a scale, a torch's time left, the outfit and alias marks, the count and
// the hotkey fold it into the stack; worn marks are ignored. A poisoned
// dagger is its own row and rejoins the stack when the dose is gone. False
// for no list.
[[nodiscard]] bool RowOfItsOwn(const RE::ExtraDataList *list);
// The bag's list at this address, or null: a token from an earlier scan
// made safe to use, since the copy may have left and the address be
// another's or nobody's.
[[nodiscard]] RE::ExtraDataList *ListOfAddress(RE::Actor *actor, RE::TESBoundObject *object,
                                               const RE::ExtraDataList *address);
// The entries on a list as their type numbers, "16 3E" (ExtraDataType,
// hex), for a log line about which copy is which; "-" for no list.
[[nodiscard]] std::string ListEntries(const RE::ExtraDataList *list);
// The variant of the copy on this list (docs/UNIQUE.md "The variant"): its
// enchantment, tempering and custom name. Plain for a list with none of
// them, and for no list.
[[nodiscard]] ft::ItemVariant VariantOf(const RE::ExtraDataList *list);

// How many copies of the variant the bag holds: its rows summed, the
// listless remainder counting as plain; of the form, with no variant. What
// the one-copy rule asks.
[[nodiscard]] std::int32_t CountVariant(RE::Actor *actor, RE::TESBoundObject *object,
                                        const std::optional<ft::ItemVariant> &variant);
// A list of the variant, worn in those hands (None: worn at all) or not
// worn at all; null for none. Of the unworn, one of the plain stack before
// a row of its own: the poisoned dagger is the plain variant, but a click
// on the plain stack or a pin on it means a clean one while any is there.
// The plain variant may have no list to give: a listless copy is the
// engine's to resolve from a null list.
[[nodiscard]] RE::ExtraDataList *WornVariantList(RE::Actor *actor, RE::TESBoundObject *object,
                                                 const ft::ItemVariant &variant, Hand hands);
[[nodiscard]] RE::ExtraDataList *UnwornVariantList(RE::Actor *actor, RE::TESBoundObject *object,
                                                   const ft::ItemVariant &variant);
// A list of the plain stack -- one that is not a row of its own -- worn in
// those hands (None: worn at all), or not worn at all; null for none. The
// panel's click on the stack means one of these, or a listless copy.
[[nodiscard]] RE::ExtraDataList *WornStackList(RE::Actor *actor, RE::TESBoundObject *object, Hand hands);
[[nodiscard]] RE::ExtraDataList *UnwornStackList(RE::Actor *actor, RE::TESBoundObject *object);
// The form's rows in the bag as the Inventory tab splits them, by variant:
// one per list that is a row of its own, and the plain stack once when any
// copy is in it. What the core's whole-form questions take.
[[nodiscard]] std::vector<ft::ItemVariant> RowsOf(RE::Actor *actor, RE::TESBoundObject *object);
// Are there plain copies on no list at all, which only a null list can
// reach?
[[nodiscard]] bool HasListlessCopy(RE::Actor *actor, RE::TESBoundObject *object);

// The weapon in a hand, if it takes a poison (anything but a staff). Null
// for no weapon there, or a staff.
[[nodiscard]] RE::TESObjectWEAP *PoisonableWeaponIn(RE::Actor *actor, bool left);

// The weapon a poison would go on, and which hand it is in: the right
// hand's if it takes one and is clean, else the left's on the same terms,
// as the inventory menu goes to the right hand alone. Null when neither
// qualifies.
struct WeaponInHand
{
    RE::TESObjectWEAP *weapon{nullptr};
    Hand hand{Hand::None};
};
[[nodiscard]] WeaponInHand WeaponToPoison(RE::Actor *actor);

// Does the copy of that weapon worn in that hand have a poison on it?
[[nodiscard]] bool WeaponPoisoned(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand);

// A weapon's enchantment charge as the actor carries it: what is left, the
// full amount, and what one hit draws in the actor's hands. Not enchanted
// reads as all zero. `hand` names the worn copy to read -- and the hand
// whose live charge actor value holds what is left -- or None for a copy
// in the bag, read off its record and whatever list it has.
struct WeaponCharge
{
    bool enchanted{false};
    float charge{0.0f};
    float maxCharge{0.0f};
    float costPerHit{0.0f};
};
[[nodiscard]] WeaponCharge ChargeOf(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand);

// The weapon in a hand, enchanted or not; null for no weapon there.
[[nodiscard]] RE::TESObjectWEAP *WeaponIn(RE::Actor *actor, bool left);

// What a soul of that level puts into a charge: the five iSoulLevelValue
// game settings, which the engine's own recharge reads.
[[nodiscard]] float SoulCharge(RE::SOUL_LEVEL level);

// The filled soul gems carried, as the snapshot lists them. A reusable one
// (Azura's Star, the ReusableSoulGem keyword) counts: spending it empties
// it, as the engine's own recharge does, rather than removing it.
[[nodiscard]] std::vector<ft::Snapshot::SoulGemView> ScanSoulGems(RE::Actor *actor);

// A stat as the bars and the rules read it: the current value, and the
// maximum with every modifier in -- the permanent (perks, race) and the
// temporary (a Fortify enchantment or potion).
[[nodiscard]] ft::Stat ReadStat(RE::Actor *actor, RE::ActorValue av);

// One castable spell a follower knows, for the editor's menu.
//
// Name and id together because the menu shows one and stores the other: the
// name is what a player picks by and is translated, the FormID is what the
// rule keeps and what survives a language change. Same split as wire names
// versus display names, for the same reason.
struct SpellOption
{
    std::uint32_t form{0};
    std::string name;
    // Delivery Self: cast on oneself and on no one else. The menu offers it
    // under Self only, and everything aimed under everyone but Self -- save
    // a Location spell (a conjuration), which goes at whoever's feet the
    // rule names, the follower's included, and is offered under everyone.
    bool selfOnly{false};
    bool location{false};
    // Carries a Reanimate-archetype effect: the only kind aimed at a corpse.
    // The archetype is the record property the engine raises a corpse by,
    // so a mod's reanimate spell is found by it whatever it is called.
    bool reanimate{false};
    // They can cast it from both hands: the school's Dual Casting perk, as
    // the perk system answers for this spell, and a record that does not
    // take both hands -- the equip slot, not the level: vanilla's master
    // spells take both, a mod's may not. The Dual Cast menu lists these
    // and no other.
    bool dualCast{false};
    // Which menu lists it: Cast spell, Use power, or Shout. One list
    // because all three are found by the same walk of what they know.
    enum class Kind : std::uint8_t
    {
        Spell,
        Power,
        Shout,
        Scroll // carried, not known: read once and spent
    };
    Kind kind{Kind::Spell};
};

// A blow with what the actor holds: the animation event that starts it,
// the stamina it costs, and how far it reaches, centre to centre, with a
// margin for the enemy's own body. No event where the hands hold nothing
// for it. The race record's attack data carries the events and their
// multipliers (docs/ACTIONS.md 6).
struct BlowPlan
{
    const char *event{nullptr};
    float stamina{0.0f};
    float reach{0.0f};
    [[nodiscard]] bool Possible() const noexcept
    {
        return event != nullptr;
    }
};
// A power attack, chosen by the hands: the right hand's blade or
// two-hander (attackPowerStartInPlace), the left's alone (...LeftHand),
// both at once (...DualWield), the fists (the right hand's event). None
// for a bow, a staff or a spell in the hand that would swing.
[[nodiscard]] BlowPlan PlanPowerAttack(RE::Actor *actor);
// A bash (bashStart) or a power bash (bashPowerStart), with what blocks: a
// shield or a torch in the left hand, or the right hand's weapon with the
// left hand empty.
[[nodiscard]] BlowPlan PlanBash(RE::Actor *actor, bool power);
// The blow a kind of action strikes; an empty plan for any other kind.
[[nodiscard]] BlowPlan PlanBlow(RE::Actor *actor, ft::ActionKind kind);

// One consumable they carry -- a potion, a food, an ingredient -- for the
// editor's Consume menu.
struct ConsumableOption
{
    std::uint32_t form{0};
    std::string name;
    int count{0};
    ft::ConsumableKind kind{ft::ConsumableKind::Potion};
    // The effects a policy could choose this by: a potion's boons, a
    // poison's banes, by name. What the Strongest and Weakest menus list.
    std::vector<std::string> effects;
};

// Every consumable they carry, sorted by name. Menu content only. Poisons
// are left out: they go on a weapon, not down the throat.
[[nodiscard]] std::vector<ConsumableOption> ScanCarriedConsumables(RE::Actor *actor);

// Every spell the follower can actually cast, sorted by name.
//
// Sorted here rather than in the UI because the order is a property of the
// list, not of how it is drawn, and doing it once per rebuild beats doing it
// every frame the menu is open.
//
// Filtered to SpellType::kSpell and the two power types, plus the shouts on
// the base record. Abilities, diseases and passive effects also live in an
// actor's spell list and none of them are castable, so offering them would
// be offering rules that can never work.
[[nodiscard]] std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor);

// Castable means SpellType::kSpell; a power is kPower or kLesserPower -- or
// a power a shout slot is leasing, which reads as Voice for the lease
// (Packages.cpp). An actor's spell list also carries abilities, diseases and
// passive racial effects, none of which a follower can choose to cast, so a
// rule naming one could never fire.
[[nodiscard]] bool IsCastable(const RE::SpellItem *spell);
[[nodiscard]] bool IsPower(const RE::SpellItem *spell);

// Walk every spell an actor has, from the three places the game keeps them.
// Missing any loses spells that are plainly there:
//   TESNPC::GetSpellList()  what the character was authored with -- Marcurio's
//                           destruction spells come from here.
//   TESRace::actorEffects   the race's: the passive resistances, and the
//                           racial power (Voice of the Emperor on an
//                           Imperial), which is why the Powers chip is not
//                           empty for a vanilla follower.
//   addedSpells             everything granted at runtime, which is what the
//                           console's addspell writes to.
// The same spell can appear in more than one; callers dedupe by form.
void ForEachSpell(RE::Actor *actor, const std::function<void(RE::SpellItem *)> &fn);

// One line of the character sheet, already worded. Worded HERE, not in the
// panel, because every value is an actor-value read and the RE:: enum naming
// it belongs with the read; the panel then has nothing to know about what a
// resistance cap is or which slot counts as armour.
// Font Awesome's infinity, for a sheet row whose value is "no end": an
// effect with no duration. The panel draws a row's icon in place of its
// value text.

// The two glyphs an Equipped row is made of, Font Awesome's check and
// thumbtack: the same codepoints the panel's own Glyph table uses, so the
// row reads as the Inventory and Magic tabs' cells do.
inline constexpr unsigned kGlyphTick = 0xF00C;
inline constexpr unsigned kGlyphPin = 0xF08D;

struct SheetRow
{
    std::string label;
    std::string value;
    unsigned icon{0};      // a Font Awesome codepoint drawn instead of the value, when set
    unsigned icon2{0};     // a second glyph after the first: the pin beside the tick
    std::string modifiers; // Skills tab only: "+35% damage, -17% cost"
    std::string note;      // plain hover text on the value; empty for none
    // The value written out as the calculation that made it, hover text
    // on the value. Drawn in place of `note` when it has lines.
    ft::Breakdown breakdown;
    // The Modifiers cell in pieces, each with its own breakdown: "+50%
    // damage" and "-50% cost" are two figures and hover apart. Drawn in
    // place of `modifiers` when not empty.
    struct ModifierPart
    {
        std::string text;
        ft::Breakdown breakdown;
    };
    std::vector<ModifierPart> modifierParts;
    // The columns an effect's row may carry after its value, each drawn
    // only where some row has it: the duration, what is left of it, and
    // the source, a link to `form` where that has a page. Hidden is the
    // `mark`.
    std::string extra;
    std::string remaining;
    std::string link;
    // An effect's description with its numbers filled in, for a table
    // with a wrapped last column of them.
    std::string description;
    // Rows revealed by expanding this one: a skill's perks. Empty means the
    // row is a plain line and cannot be opened.
    std::vector<SheetRow> detail;
    // The inventory item this row names, if any: a click on it opens the
    // item's page on the Inventory tab. 0 for a row that names nothing.
    std::uint32_t form{0};
    // Why the row is set aside -- a perk whose conditions fail for this
    // actor -- shown on the name; empty for a row that counts.
    std::string aside;
    // A glyph in the third column, where the table has one: an effect's
    // tick for Hidden. 0 for none.
    unsigned mark{0};
    // The Equipped row of a page (EquippedRow), where the pin glyph goes.
    bool equipped{false};
};

struct SheetSection
{
    std::string title;
    std::vector<SheetRow> rows;
    // The heading this section sits under when several share one -- Attack
    // over a Right Hand table and a Left Hand table. Empty means the title
    // is the heading.
    std::string group;
};

// Who a record's conditions are asked of: the Subject and the Target the
// engine passes. For an effect that is the one it lands on and whoever
// cast it (docs/CONDITIONS.md 10). A party the page cannot name is null,
// and a condition that runs on it is listed unasked, N/A: asked of nobody
// the engine answers false, which is not "not met".
struct ConditionParties
{
    RE::TESObjectREFR *subject{nullptr};
    RE::TESObjectREFR *target{nullptr};
};

// What a spell, an enchantment or a potion does, effect by effect, as a
// perk's page lists its entries: the value each moves and by how much,
// else its kind and what it names; the engine's archetype, the duration,
// "hidden" where the game's list would not show it; each row opening on
// its conditions, with a tick where they hold for whom the effect would
// land on and the actor using it. `magnitude` says which number a row
// carries: the record's, or the caster's actual one. The record beside the
// author's prose, which says what they meant.
[[nodiscard]] SheetSection EffectsOf(RE::Actor *actor, const RE::MagicItem *magic,
                                     const std::function<float(const RE::Effect *)> &magnitude);
[[nodiscard]] SheetRow EffectEntryRow(const RE::Effect &effect, float magnitude, const ConditionParties &parties);

// One effect running on the follower, for the Effects tab: the effect as
// the game names it, its magnitude, what is left of it, and where it comes
// from -- the spell, the potion, or for an enchantment the worn item that
// carries it, "Robes of Health" rather than the enchantment record's name.
struct EffectRow
{
    // The base effect and what applied it, together the row's identity:
    // the same effect can run twice from two sources.
    std::uint32_t form{0};
    std::uint32_t sourceForm{0};
    // What the source's name links to, where it has a page: the worn item
    // behind an enchantment, else the spell. The panel decides whether a
    // page exists, by its own lists.
    std::uint32_t linkForm{0};
    std::string name;
    float magnitude{0.0f};
    float duration{0.0f};      // in all; 0 for one with no duration
    float remaining{-1.0f};    // seconds left; below zero for one with no duration
    std::string remainingText; // empty for one with no duration
    std::string source;
    // False when the effect is running but changes nothing for this actor:
    // Fortify One-handed on a follower, which writes a value nothing on a
    // follower reads. Listed greyed, hovering as "Not applied".
    bool applied{true};
    // False while the effect is on the list but not acting (Spellbreaker's
    // ward off the block), by the engine's own flag and not by asking the
    // conditions again: a magic effect record's conditions are asked once,
    // when it lands, and can read false ever after (Adamant's Bastion asks
    // whether the cast was dual). Listed greyed, hovering as "Inactive".
    bool active{true};

    // The page: the effect's numbers as the first section, then what its
    // source does, effect by effect, each opening on its conditions -- the
    // perk page's shape -- and the description with the magnitude and
    // duration filled in, as the item card shows it.
    std::vector<SheetSection> detail;
    std::string description;
};

// What a follower commands right now: a summon or a raised corpse, for the
// Summons tab. Its numbers come from its own actor, its page from the same
// sheet builder as the follower's, so the two tabs read alike.
struct SummonView
{
    ft::ActorId id{0};
    std::uint32_t baseId{0};
    std::string name;
    std::uint16_t level{0};
    ft::Stat health{};
    ft::Stat magicka{};
    ft::Stat stamina{};
    // Each bar written out as the follower's are. A summoner's perks reach
    // the summon as effects running on it -- Adamant's are cloaks on the
    // summoner casting onto commanded actors near them -- so they list with
    // the summon's own (docs/MODIFIERS.md).
    ft::Breakdown healthBreakdown;
    ft::Breakdown magickaBreakdown;
    ft::Breakdown staminaBreakdown;
    float remaining{0.0f}; // seconds left on the effect that commands it; 0 when unknown
    // The time left written out: the spell's duration, the summoner's perks
    // on it, the time run. The summoner's perks shape the summon here too,
    // Dark Oath's "last twice as long" among them.
    ft::Breakdown remainingBreakdown;
    bool raised{false}; // a reanimated corpse, as opposed to a summon
    std::vector<SheetSection> sheet;
};

// Everything the follower commands, in the engine's order.
[[nodiscard]] std::vector<SummonView> ScanSummons(RE::Actor *actor);

// Everything running on the follower that the game would list, sorted by
// name. Effects flagged hidden, and ones already run out, are left out.
[[nodiscard]] std::vector<EffectRow> ScanActiveEffects(RE::Actor *actor);

// Where a number on a sheet comes from. The engine keeps an actor value as
// a base plus modifiers and names no source; but every running effect says
// which value it moves, by how much, and what applied it. So what magic
// adds can be told by source -- "Adept Robes of Destruction: +100" -- and
// what anything else wrote there (a script, another plugin) cannot.
struct Contribution
{
    std::string source; // the worn item, the potion, the spell
    std::string effect; // the magic effect's own name, "Fortify Armor Rating"
    float amount{0.0f}; // signed: a detrimental effect takes away
};
[[nodiscard]] std::vector<Contribution> Contributions(RE::Actor *actor, RE::ActorValue value);

// The sources as lines added to a breakdown, smallest first -- the
// weaknesses, then the boons, the largest last: "Silver Ruby Ring: +50%".
void AddSourceLines(ft::Breakdown &b, std::vector<Contribution> sources);

// A value as the parts a sheet can name: the engine's base, which no effect
// moves, and each running effect on it by source, smallest first. Every
// figure that reads a value opens it out from these, so none shows a buff
// folded into its base: the value as it reads was once the base, and hid
// Mundus's Elfborn stone in the Magicka Rate (2026-09-14). Whatever else
// is in the value -- a script's ModActorValue, another plugin's write, as
// Blade and Blunt's injuries are -- has no name, and is the Other line
// wherever the parts are summed against the value.
struct ValueParts
{
    float base{0.0f};
    std::vector<Contribution> sources;
};
[[nodiscard]] ValueParts PartsOf(RE::Actor *actor, RE::ActorValue value);

// The parts as lines, each amount times `scale`: "Base" where the base is
// not zero, starting the calculation when it is the first line, then each
// source.
void AddValueLines(ft::Breakdown &b, const ValueParts &parts, float scale = 1.0f);

// A value written out: its parts, and the value as it reads now as the
// total, with Other for the rest. For a pool (Health, Magicka, Stamina) the
// total is the maximum, not what is left of it.
[[nodiscard]] ft::Breakdown ValueBreakdown(RE::Actor *actor, RE::ActorValue value, const char *unit);

// The carry weight the engine holds them to: the Carry Weight value written
// out, then each Get Max Carry Weight perk entry, and the engine's own
// figure as the total.
[[nodiscard]] ft::Breakdown CarryWeightBreakdown(RE::Actor *actor);

// The armour rating's sources: each piece worn with its rating, the
// spells and enchantments on the armour value, the engine's hidden bonus
// per piece, and the rating they make.
[[nodiscard]] ft::Breakdown ArmorBreakdown(RE::Actor *actor);

// The perk entries an actor holds on one entry point, as lines: each
// entry in the order the engine applies them (highest priority first, the
// arrays on the actor's process being kept sorted -- docs/MODIFIERS.md),
// named for its perk, its function applied as the engine applies it; an
// entry whose conditions fail against `args` listed dimmed with the
// condition that stopped it. `args` are the call's arguments after the
// perk owner, as the engine's own call takes them: the weapon and the
// target for attack damage, the spell for cost, the piece for armour.
void AddEntryPointLines(ft::Breakdown &b, RE::Actor *actor, RE::BGSEntryPoint::ENTRY_POINT point,
                        const std::vector<void *> &args);

// The cost of a spell as the caster pays it, written out: the effects'
// costs, the skill curve, each Mod Spell Cost entry, and the engine's
// figure as the total.
[[nodiscard]] ft::Breakdown SpellCostBreakdown(RE::Actor *actor, const RE::SpellItem *spell);
// The engine's hidden per-piece bonus in the rating's own units, and the
// rating with it added: what the Armor row shows, and what its sources sum
// to.
[[nodiscard]] float HiddenArmor(RE::Actor *actor);
[[nodiscard]] float EffectiveArmor(RE::Actor *actor);

// The Character tab: race, movement, defence and the equipped weapon. Display
// only -- none of it is a rule input. Cheap reads, done in and out of combat
// alike.
[[nodiscard]] std::vector<SheetSection> BuildCharacterSheet(RE::Actor *actor);

// The Skills tab: the eighteen skills grouped as the game groups them, with
// any fortify or potion modifier folded into the same line.
[[nodiscard]] std::vector<SheetSection> BuildSkillSheet(RE::Actor *actor);

// One perk's page: what the record says. The Perk section (id, rank,
// skill) and an Entries section, one row per thing the perk does -- an
// entry point with its function, an ability it grants, a quest it
// advances -- with the description beneath as prose.
struct PerkPage
{
    std::uint32_t form{0};
    std::string name;
    std::string description;
    // "Perk Details", the facts; then "Effects", one row per entry -- what it does,
    // a tick in the mark while it is active -- opening on the conditions
    // that gate it, as the engine reads them for this actor: the call, the
    // comparison, a tick when met. Not the record's own conditions: those
    // are the skill tree's, for the player.
    std::vector<SheetSection> sections;
};

// A page for every perk the follower holds, in a skill's tree or loose.
[[nodiscard]] std::vector<PerkPage> BuildPerkPages(RE::Actor *actor);

// The Tactics tab's Combat Style section: the numbers and flags the combat
// AI is tuned by, read off the style they are using right now -- their live
// combat controller's in a fight, their record's otherwise -- so a copy the
// panel gave them shows as what it is.
[[nodiscard]] std::vector<SheetSection> BuildCombatStyleSheet(RE::Actor *actor);

// The combat style the actor fights by: the controller's live copy when
// there is one, else the record's. Null for an actor with none.
[[nodiscard]] RE::TESCombatStyle *LiveCombatStyle(RE::Actor *actor);

// May the actor hold a one-handed weapon in each hand? The combat style's
// flag; an actor with no style may, and so may the player. The panel's cells, and a request, read
// it (core/Loadout.h WouldDualWield).
[[nodiscard]] bool DualWieldAllowed(RE::Actor *actor);

// The damage a weapon does in their hands, as the inventory menu would show
// it: base, times tempering, times the skill curve, through their perks
// (which is where a Fortify effect on the skill counts, read by the hidden
// skill-boost perk for whoever holds it), times the attack damage
// multiplier, plus flat melee damage. `entry` may be null, in which case
// the weapon is taken as untempered. `out`, when given, is the figure
// written out.
[[nodiscard]] float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry,
                                 ft::Breakdown *out = nullptr);

// The armour rating a piece gives them, the same way: base, times tempering,
// times the armour skill's curve, through their perks. Clothing rates 0.
[[nodiscard]] float ArmorRating(RE::Actor *actor, RE::TESObjectARMO *armor, RE::InventoryEntryData *entry,
                                ft::Breakdown *out = nullptr);

// Their chance of a critical hit with a weapon, in percent: the critical
// chance value, through the perks on the critical hit chance entry point.
[[nodiscard]] float CritChance(RE::Actor *actor, RE::TESObjectWEAP *weapon, ft::Breakdown *out = nullptr);

// A shout word's recovery as it applies to them: the word's own time,
// times their shout recovery multiplier.
[[nodiscard]] float WordRecovery(RE::Actor *actor, float recovery, ft::Breakdown *out = nullptr);

// Resolve a FormID from a rule back to the spell it names, or nullptr.

// Dump the actor's active magic effects to the log: source item, archetype,
// elapsed/duration, magnitude.
//
// This is here to answer one question empirically rather than from memory --
// does drinking a vanilla healing potion leave anything running that we could
// check? If it does, "is the effect I applied still active" is a far more
// precise availability test than a fixed cooldown, and it generalises to
// spells and food. If the list is empty, the effect is instant, the condition
// itself is the check, and MinimumCooldown stays the right mechanism.
//
// ActiveEffect carries `spell` (the AlchemyItem for a potion), `duration` and
// `elapsedSeconds`, so the check is exact once we know it is worth making.

// Build the snapshot for one follower. `now` is monotonic seconds since plugin
// load; cooldowns are measured against it. Self and player state, the
// potions and the loadout, and the party and the enemies by definition:
// allies are the player and the other teammates, enemies whoever is in
// combat and hostile to the player (docs/CONDITIONS.md 6).
ft::Snapshot BuildSnapshot(RE::Actor *actor, double now);

} // namespace ft::game
