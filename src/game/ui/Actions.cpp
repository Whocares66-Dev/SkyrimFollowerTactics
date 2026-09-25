// The target and action cells of the rule editor: their text, why a rule
// is set aside, and the menus that pick a target and the actions.

#include "game/ui/Rules.h"
#include "game/ui/Widgets.h"

#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Routes.h"
#include "core/Vocabulary.h"
#include "game/Log.h"
#include "game/Sheet.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

bool TakesSpell(ft::ActionKind action);

// A rule set aside for what it names keeps its text, its place and its
// switch state, greyed, and its cells still open so the player can name
// something else. The reasons, on the switch and on the cell concerned.
constexpr const char *kNotAvailable = N_("Item or ability not available");

bool TargetAvailable(const ft::Rule &rule, const FollowerView &view);

// The action side.
// Does this action name a spell (or a power, which is a spell record)?
bool TakesSpell(ft::ActionKind action)
{
    return ft::IsCast(action) || action == ft::ActionKind::EquipSpell;
}

// Which of the spell menu's kinds an action picks from: a power for Use
// power, a shout for Shout, a scroll for Scroll, a spell for the rest.
SpellOption::Kind SpellKindOf(ft::ActionKind action)
{
    switch (action)
    {
    case ft::ActionKind::UsePower:
        return SpellOption::Kind::Power;
    case ft::ActionKind::Shout:
        return SpellOption::Kind::Shout;
    case ft::ActionKind::UseScroll:
        return SpellOption::Kind::Scroll;
    default:
        return SpellOption::Kind::Spell;
    }
}

// Could a thing with this grip be pinned in this hand, as the equip menu
// asks it? Both means a two-hander, or a spell in each hand at once; one
// weapon cannot be in both hands.
bool Fits(ft::Grip grip, Hand hand, bool spell)
{
    switch (hand)
    {
    case Hand::Left:
        return grip == ft::Grip::Either || grip == ft::Grip::LeftOnly;
    case Hand::Right:
        return grip == ft::Grip::Either || grip == ft::Grip::RightOnly;
    case Hand::Both:
        return grip == ft::Grip::Both || (spell && grip == ft::Grip::Either);
    default:
        return false;
    }
}

// "Equip weapon" -> "weapon", for "Unequip weapon" and the heading.

// The name of the thing an equip rule names, as they carry or know it;
// empty if they do not.
std::string EquipTargetName(const ft::Action &act, const FollowerView &view)
{
    if (act.kind == ft::ActionKind::EquipSpell)
    {
        for (const auto &entry : view.magic)
            if (entry.form == act.form)
                return entry.name;
        return {};
    }
    // The row under the copy named, whatever it holds now; the form
    // alone, any row of it. With nothing under the copy, the name it was
    // last seen with, so a rule keeps reading "Equip Iron Dagger (+4)"
    // while that dagger is away.
    for (const auto &item : view.inventory)
        if (item.form == act.form && ft::SameVariant(item.variant, act.variant))
            return item.name;
    return act.name;
}

// Has the player banned this form? A spell, shout or power by its entry;
// a scroll by its rows, any row of the form banned. For the Cast menus,
// whose options carry the form alone.
bool BannedForm(const FollowerView &view, std::uint32_t form)
{
    for (const auto &entry : view.magic)
        if (entry.form == form && entry.banned)
            return true;
    for (const auto &item : view.inventory)
        if (item.form == form && item.banned)
            return true;
    return false;
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The thing an action names, capitalised for a heading of its own:
// "Weapon" under Equip, "Strongest soul gem" under Charge.
std::string NounHeading(ft::ActionKind action)
{
    std::string name(ft::Noun(action));
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

// The Then cell's text: the spell by NAME, never by id.
//
// A FormID in the table would be unreadable and, worse, unstable to look at --
// the point of naming the spell is that a rule reads as an instruction. The id
// is what the rule stores; this is what the player sees. Same split as wire
// names versus display names.
// Whom the rule's actions are aimed at, by name where it names someone: the
// first half of the Then cell, before the colon, as the subject is of the If.
std::string TargetText(const ft::Rule &rule, const FollowerView &view)
{
    switch (rule.actionTarget)
    {
    case ft::ActionTargetKind::Player:
        return std::string(ft::DisplayName(rule.actionTarget));
    case ft::ActionTargetKind::Follower:
        for (const auto &peer : view.peers)
            if (peer.id == rule.actionTargetForm)
                return peer.name;
        return Tr("Follower (away)");
    default:
        return std::string(ft::DisplayName(rule.actionTarget));
    }
}

// What a rule calls a thing that is not there to be asked: the name it
// was last seen with (Action::name), else the record's.
std::string LastName(const ft::Action &act)
{
    return act.name.empty() ? FormName(act.form) : act.name;
}

// Whether what the action names is there to be used: the potion carried,
// the spell known, the scroll carried, the weapon in the bag. An action
// that names nothing, or a policy, is always available here; whether it
// can fire is the evaluator's question.
// The editor's questions of a rule are core's (core/Editor.h), asked of
// what the tick found the follower to have; the words are the panel's.
bool ActionAvailable(const ft::Action &act, const FollowerView &view)
{
    // What Settings requires of the follower is part of what they have
    // (core/Editor.h): a power bash without the perk, or a dual cast of a
    // spell they may not cast from both hands, reads as unavailable.
    return ft::ActionHad(act, view.holdings);
}

bool TargetAvailable(const ft::Rule &rule, const FollowerView &view)
{
    return ft::TargetHad(rule, view.holdings);
}

// Why an action could not be done this moment, for its cell's hover, or
// empty for one that could -- or one whose only obstacle is the fight
// itself (no fight on, no one to aim at, out of reach, a hand a rule above
// holds), or one already in effect (a buff still up, a "none" with nothing
// to take off), which is what the rule waits on and not the action being
// unavailable: a Combat end rule's "unequip" reads as done all the way
// through the fight it is written for. The words are the panel's where a
// player would read them most; the rest are the verdict's own,
// capitalised.
std::string UnavailableText(ft::Verdict verdict, ft::ActionKind kind)
{
    switch (verdict)
    {
    case ft::Verdict::CannotAfford:
        return Tr("Not enough magicka");
    case ft::Verdict::NoStamina:
        return Tr("Not enough stamina");
    case ft::Verdict::PowerUsed:
        return Tr("Greater power can only be used once per day");
    case ft::Verdict::Recovering:
        return kind == ft::ActionKind::Shout ? Tr("Still recovering from the last shout")
                                             : Tr("Voice still recovering from the last shout");
    case ft::Verdict::ActionCooldown:
        return Tr("Used too recently");
    case ft::Verdict::Casting:
        return Tr("Already casting a spell");
    case ft::Verdict::Busy:
        return Tr("A cast from another rule is still in progress");
    case ft::Verdict::CannotDualCast:
        return Tr("Cannot dual cast that spell");
    case ft::Verdict::AboveSkill:
    case ft::Verdict::NoMeleeWeapon:
    case ft::Verdict::NoPerk:
    case ft::Verdict::NothingToPoison:
    case ft::Verdict::NothingToCharge:
    case ft::Verdict::NoResource:
    case ft::Verdict::Unsupported: {
        std::string text(Tr(ft::Explain(verdict, kind)));
        if (!text.empty())
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }
    default:
        return {};
    }
}

// A follower who has read every tome has a spell menu nobody can find
// anything in, so the casts and the equips are grouped by school. The
// order the Magic tab lists them in, with Other last for a spell that has
// none -- a vampire's Drain Life, a race's ability cast as a spell.
constexpr std::array<MagicCategory, 6> kSchools{MagicCategory::Alteration,  MagicCategory::Conjuration,
                                                MagicCategory::Destruction, MagicCategory::Illusion,
                                                MagicCategory::Restoration, MagicCategory::Other};

// Under this many, the schools cost more than they save: a follower with
// four spells should not have to guess which heading one is under and open
// it to find out. A short list stays flat.
//
// Counted over what the menu is ABOUT to offer, not over everything the
// follower knows: the lists reaching DrawBySchool are already cut to the
// target and the hand, so a follower with thirty spells of which four are
// Self gets a flat list under Self and the schools under an enemy.
constexpr std::size_t kGroupSchoolsAtLeast = 10;

// One submenu per school over `items`, `schoolOf` saying which school an
// item is in and `leaf` drawing one. A school nothing is in is not drawn:
// a follower with only Restoration spells gets Restoration and no five
// empty headings to open before finding it.
template <typename Items, typename SchoolOf, typename Leaf>
void DrawBySchool(const Items &items, SchoolOf schoolOf, Leaf leaf)
{
    const auto anyOf = [&](MagicCategory school) {
        return std::any_of(items.begin(), items.end(), [&](const auto &item) { return schoolOf(item) == school; });
    };
    // Grouped only where it pays: enough of them that a flat list is hard
    // to read, AND more than one school to divide them into. Twelve
    // Destruction spells under a lone Destruction heading is a layer that
    // tells the player nothing they did not know before opening it.
    const auto schools = static_cast<std::size_t>(std::count_if(kSchools.begin(), kSchools.end(), anyOf));
    if (items.size() < kGroupSchoolsAtLeast || schools < 2)
    {
        for (const auto &item : items)
            leaf(item);
        return;
    }
    for (const MagicCategory school : kSchools)
    {
        if (!anyOf(school))
            continue;
        if (!BeginCascade(DisplayName(school)))
            continue;
        for (const auto &item : items)
            if (schoolOf(item) == school)
                leaf(item);
        Im::EndMenu();
    }
}

// Which cast menus are grouped: the spells and the scrolls, a scroll being
// a spell in a wrapper and carrying a school like one. A power and a shout
// have no school and stay a flat list.
bool SchoolGrouped(SpellOption::Kind kind)
{
    return kind == SpellOption::Kind::Spell || kind == SpellOption::Kind::Scroll;
}

// Is this spell offered for pinning in this hand? Not a shout or a power,
// which take no hand; fits the hand; and not above the follower's skill,
// which the combat AI would never choose -- so a pin on it would be a
// promise unkept, and it is left out rather than listed and greyed.
bool Offered(const MagicEntry &entry, Hand hand)
{
    return entry.category != MagicCategory::Shouts && entry.category != MagicCategory::Powers &&
           Fits(entry.grip, hand, true) && !entry.aboveSkill;
}

// One leaf of an equip menu: a thing the follower has, pinned in `hand`
// when chosen.
// `label` is the leaf's text; `rowName` the row's own name, kept on the
// action (Action::name); `banned` whether the player has banned it, which
// dims the leaf; `tint` the row's colour (NameTint), on the leaf too;
// `row` the inventory row behind it, for the count and the marks after
// the name (null for a spell, which has neither). An item's leaf reads as
// the Inventory tab's row does, so two rows of one form tell apart in the
// picker as they do there.
bool EquipLeaf(ft::Action &act, ft::ActionKind action, std::uint32_t form, const std::string &label, Hand hand,
               const std::optional<ft::ItemVariant> &variant, const std::string &rowName, bool banned,
               const Im::ImVec4 *tint = nullptr, const InventoryItem *row = nullptr)
{
    const bool selected = act.kind == action && act.form == form && act.variant.has_value() == variant.has_value() &&
                          ft::SameVariant(act.variant, variant) && act.hand == hand;
    // The text as the Inventory tab shows it, the count after the name; and
    // an id of its own past the ##, since two rows of a form may read the
    // same (the plain stack and the poisoned dagger) and ImGui tells menu
    // items apart by their label.
    std::string text = label;
    if (row && row->count > 1)
        text += " (" + std::to_string(row->count) + ")";
    std::string id = text;
    // Room for the marks after the text: spaces their width on the label
    // itself, so the menu sizes to text and marks together and no more. The
    // tick has the column ImGui keeps for a menu item's own check mark.
    if (row && Badged(*row))
    {
        const float need = kBadgeGap + DrawNameBadges(nullptr, *row, {}, false);
        const float space = TextWidth(" ");
        id.append(space > 0.0f ? static_cast<std::size_t>(std::ceil(need / space)) : 0, ' ');
    }
    if (row)
    {
        char key[24];
        std::snprintf(key, sizeof key, "##%016llX", static_cast<unsigned long long>(row->Key()));
        id += key;
    }
    // Banned: dimmed, as the Inventory and Magic tabs dim it, but still a
    // choice. The rules are the player's voice and a ban is the AI's leash;
    // a rule that names a banned thing is deliberate, and the leaf only
    // says so.
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const Im::ImVec4 *colour = banned ? &Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled] : tint;
    const bool clicked = CascadeItem(id.c_str(), selected, colour);
    // The menu item draws its label at the row's left edge; the marks go
    // after the label's width, laid out by hand since the item owns the row.
    if (row && Badged(*row))
        DrawNameBadges(Im::GetWindowDrawList(), *row, {pos.x + TextWidth(text) + kBadgeGap, pos.y}, banned);
    if (banned && Im::IsItemHovered(0))
        Im::SetTooltip("%s", Tr("Banned"));
    if (!clicked)
        return false;
    act.kind = action;
    act.form = form;
    act.variant = variant;
    act.name = rowName;
    act.hand = hand;
    return true;
}

// The Equip weapon / Equip spell / Equip arrows / Equip armor cascades.
//
// Unequip first: let go of every pin of this kind, and the AI chooses
// again. For the two that take a hand it heads each hand's menu instead,
// Right, Left and Both -- the weapon hand first, as a player thinks of them
// -- each listing what fits that hand; for arrows and armour, the things
// themselves. Every list is theirs, so a rule cannot name a
// thing they do not have.
bool EquipMenu(ft::Action &act, ft::ActionKind action, const FollowerView &view)
{
    bool changed = false;

    // Unequip: let go of every pin of the kind and take those things off,
    // and the AI decides again -- not "None", which would promise a hand
    // kept empty, and nothing does that. For a weapon or a spell it is the
    // first leaf of each hand's menu, the hand being the thing let go of;
    // for arrows and armour it heads the menu.
    const auto none = [&](Hand hand) {
        const bool selected = act.kind == action && act.form == 0 && act.hand == hand;
        if (CascadeItem(Tr("Unequip"), selected))
        {
            act.kind = action;
            act.form = 0;
            act.hand = hand;
            changed = true;
        }
    };

    // What is not there is not listed: no greyed "(carries none)" lines,
    // an empty kind simply offers None and nothing beneath it.
    const bool spell = action == ft::ActionKind::EquipSpell;
    const bool handed = spell || action == ft::ActionKind::EquipWeapon;
    if (!handed)
    {
        none(Hand::None);
        const ItemCategory category =
            action == ft::ActionKind::EquipArrows ? ItemCategory::Arrows : ItemCategory::Armor;
        // The Inventory tab's rows, a leaf each, the plain stack among
        // them: every leaf names a row's variant, none the form alone.
        std::vector<const InventoryItem *> rows;
        for (const auto &item : view.inventory)
            if (item.category == category)
                rows.push_back(&item);
        // The arrows' two policies before the named kinds, as the potions
        // have theirs: the hardest-hitting carried, or the weakest.
        if (action == ft::ActionKind::EquipArrows && !rows.empty())
        {
            Im::Separator();
            for (const auto [label, kind] : {std::pair{Tr("Strongest"), ft::ActionKind::EquipStrongestArrows},
                                             std::pair{Tr("Weakest"), ft::ActionKind::EquipWeakestArrows}})
            {
                if (CascadeItem(label, act.kind == kind))
                {
                    act.kind = kind;
                    act.form = 0;
                    act.hand = Hand::None;
                    act.name.clear();
                    changed = true;
                }
                if (Im::IsItemHovered(0))
                    Tooltip(ft::Describe(kind));
            }
        }
        if (!rows.empty())
            Im::Separator();
        for (const InventoryItem *item : rows)
        {
            if (EquipLeaf(act, action, item->form, item->name, Hand::None, item->variant, item->name, item->banned,
                          NameTint(*item), item))
                changed = true;
        }
        return changed;
    }

    for (const Hand hand : {Hand::Right, Hand::Left, Hand::Both})
    {
        const std::string label(ft::DisplayName(hand));
        if (!BeginCascade(label.c_str()))
            continue;
        // Unequip first: let go of that hand's pin. Both lets go of both.
        none(hand);
        if (spell)
        {
            // Grouped by school, as the casts are: this is the same list of
            // spells and runs as long. Offered has already left out the
            // shouts and powers, so what is left is the schools and Other.
            std::vector<const MagicEntry *> offered;
            for (const auto &entry : view.magic)
                if (Offered(entry, hand))
                    offered.push_back(&entry);
            if (!offered.empty())
                Im::Separator();
            DrawBySchool(
                offered, [](const MagicEntry *entry) { return entry->category; },
                [&](const MagicEntry *entry) {
                    if (EquipLeaf(act, action, entry->form, entry->name, hand, {}, entry->name, entry->banned))
                        changed = true;
                });
        }
        else
        {
            std::vector<const InventoryItem *> rows;
            for (const auto &item : view.inventory)
                if (item.category == ItemCategory::Weapons && Fits(item.grip, hand, false))
                    rows.push_back(&item);
            if (!rows.empty())
                Im::Separator();
            for (const InventoryItem *item : rows)
            {
                if (EquipLeaf(act, action, item->form, item->name, hand, item->variant, item->name, item->banned,
                              NameTint(*item), item))
                    changed = true;
            }
        }
        Im::EndMenu();
    }
    return changed;
}

// The action side of the cascade: the actions offered under one target
// heading of the Then cascade, those that make sense on that target
// (IsActionValidFor), in a fixed order with the more active thing first --
// Target; Potion, Food, Ingredient; Cast, Shout, Power; Charge, Poison;
// Weapon, Arrows, Armor, Spell -- a divider between the groups. Every list
// is the follower's own, so a rule cannot name a thing they do not have:
// the same guarantee the condition side gets from the validity matrix, and
// for the same reason -- an unfireable rule should be unauthorable, not
// merely discouraged. Choosing one sets the rule's target and the action
// together, as choosing a condition sets subject and predicate.
//
// With `probe`, nothing is drawn and the answer is only whether anything
// would be: the heading asks before it is drawn, so a target with no action
// for this follower -- the player, with no spell that suits -- is left out,
// by the same checks that fill the menu rather than a copy of them.
bool ActionItems(ft::Rule &rule, ft::Action &act, ft::ActionTargetKind target, std::uint32_t form,
                 const FollowerView &view, ft::Moment moment, bool probe = false)
{
    bool changed = false;
    // Is this heading the rule's current target? Only then is an item under
    // it shown selected.
    const bool here = rule.actionTarget == target && rule.actionTargetForm == form;
    const auto choose = [&]() {
        rule.actionTarget = target;
        rule.actionTargetForm = form;
        changed = true;
    };

    // The groups, the more active thing first: what is taken; what is
    // cast; what is done to the weapon in hand; what is put on. A divider
    // between the groups that draw anything -- a heading with nothing
    // under it (no food carried, no spell that suits) is not drawn, so the
    // divider is placed as the items come, never before an empty group.
    // Every item below enters its group just before it is drawn, so entering
    // one is where a probe has its answer.
    int lastGroup = -1;
    const auto enter = [&](int g) {
        if (probe)
            return false;
        if (lastGroup >= 0 && g != lastGroup)
            Im::Separator();
        lastGroup = g;
        return true;
    };
    // Valid on this target and in this list -- no blow out of a fight --
    // and one this page's actor has a route for (core/Routes.h).
    const auto valid = [&](ft::ActionKind action) {
        return ft::IsActionValidFor(target, action) && ft::IsActionValidIn(moment, action) &&
               ft::RouteOf(action, view.player ? ft::Performer::Player : ft::Performer::Follower) != ft::Route::None;
    };

    // One leaf that picks a policy: the strongest of a kind, the weakest.
    const auto policy = [&](ft::ActionKind kind) {
        const bool selected = here && act.kind == kind;
        if (CascadeItem(NounHeading(kind).c_str(), selected))
        {
            act.kind = kind;
            act.form = 0;
            choose();
        }
        if (Im::IsItemHovered(0))
            Tooltip(ft::Describe(kind));
    };
    const auto carried = [&](ft::ConsumableKind kind) {
        bool any = false;
        for (const auto &option : view.consumables)
            any = any || option.kind == kind;
        return any;
    };
    // The named things of one kind, under a menu already open.
    const auto named = [&](ft::ActionKind kind) {
        for (const auto &option : view.consumables)
        {
            if (option.kind != ft::ConsumableOf(kind))
                continue;
            const std::string label = option.name + " (" + std::to_string(option.count) + ")";
            const bool selected = here && act.kind == kind && act.form == option.form;
            if (CascadeItem(label.c_str(), selected))
            {
                act.kind = kind;
                act.form = option.form;
                act.name = option.name;
                choose();
            }
        }
    };

    // What the weapon does, then what blocks: Attack and Power Attack, a
    // divider, Bash and Power Bash. Each says what it does in its name; no
    // tooltip.
    for (const auto kind :
         {ft::ActionKind::Attack, ft::ActionKind::PowerAttack, ft::ActionKind::Bash, ft::ActionKind::PowerBash})
    {
        if (!valid(kind))
            continue;
        // Where Settings asks the Power Bash perk of a follower who has not
        // got it, a power bash is not offered at all.
        if (kind == ft::ActionKind::PowerBash && !view.holdings.powerBashPerk)
            continue;
        if (!enter(kind == ft::ActionKind::Attack || ft::ActionKind::PowerAttack == kind ? 0 : 1))
            return true;
        const bool selected = here && act.kind == kind;
        if (CascadeItem(std::string(ft::DisplayName(kind)).c_str(), selected))
        {
            act.kind = kind;
            act.form = 0;
            act.hand = Hand::None;
            choose();
        }
    }

    // Strongest and Weakest for a kind of bottle, each a submenu of the
    // effects the carried ones have -- the known effects in their groups,
    // a divider between, the rest by name at the end -- so a follower with
    // no potion of an effect is not offered it. Then, after a divider,
    // every bottle of the kind by name. Under a menu already open.
    // Anything of the kind an "any" rule could roll, and what such a choice
    // is called for it: a poison rolls among the lot, everything else among
    // its buffs alone (core's PotionStock::WantedByAny says which).
    const auto rollable = [&](ft::ConsumableKind ckind) {
        return std::any_of(view.consumables.begin(), view.consumables.end(),
                           [&](const ConsumableOption &o) { return o.kind == ckind && o.any; });
    };
    const auto anyLabel = [](ft::ConsumableKind ckind) {
        return ckind == ft::ConsumableKind::Poison ? Tr("Any") : Tr("Any buff");
    };
    // Listed whether or not anything carried would answer it, so the rule
    // can be written before the bottles are in the bag; with none, dimmed as
    // a banned leaf is, still a choice, and the tooltip says why.
    const auto anyItem = [&](ft::ConsumableKind ckind, ft::ActionKind kind, bool selected) {
        const bool had = rollable(ckind);
        if (CascadeItem(anyLabel(ckind), selected, had ? nullptr : &Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled]))
        {
            act.kind = kind;
            act.form = 0;
            act.effect.clear();
            choose();
        }
        if (!Im::IsItemHovered(0))
            return;
        if (!had)
            Im::SetTooltip("%s", Tr("No applicable buffs available"));
        else if (kind == ft::ActionKind::DrinkStrongest)
            Im::SetTooltip("%s", Tr("Drink the strongest potion that applies a buff"));
        else if (kind == ft::ActionKind::DrinkWeakest)
            Im::SetTooltip("%s", Tr("Drink the weakest potion that applies a buff"));
        else
            Tooltip(ft::Describe(kind));
    };
    const auto byEffect = [&](ft::ConsumableKind ckind, ft::ActionKind strongestKind, ft::ActionKind weakestKind,
                              ft::ActionKind namedKind, ft::ActionKind anyKind = ft::ActionKind::None) {
        std::vector<std::string> names;
        for (const auto &option : view.consumables)
            if (option.kind == ckind)
                names.insert(names.end(), option.effects.begin(), option.effects.end());
        const auto arranged = ft::ArrangeEffects(ckind, std::move(names));
        // The roll that names nothing at all, at the head of the menu:
        // "Poison: Any" puts SOMETHING on the blade.
        if (anyKind != ft::ActionKind::None)
            anyItem(ckind, anyKind, here && act.kind == anyKind);
        for (const auto [label, kind] :
             {std::pair{Tr("Strongest"), strongestKind}, std::pair{Tr("Weakest"), weakestKind}})
        {
            if (arranged.empty() || !BeginCascade(label))
                continue;
            // "Strongest: Any" rolls the EFFECT and then takes the strongest
            // of that one, since magnitudes do not compare across effects.
            // An empty effect is what carries that on the wire.
            anyItem(ckind, kind, here && act.kind == kind && act.effect.empty());
            Im::Separator();
            int last = -1;
            for (const auto &entry : arranged)
            {
                if (last >= 0 && entry.group != last)
                    Im::Separator();
                last = entry.group;
                const bool selected = here && act.kind == kind && act.effect == entry.name;
                if (CascadeItem(std::string(ft::EffectLabel(entry.name)).c_str(), selected))
                {
                    act.kind = kind;
                    act.form = 0;
                    act.effect = entry.name;
                    choose();
                }
                if (Im::IsItemHovered(0))
                    Tooltip(ft::Describe(kind));
            }
            Im::EndMenu();
        }
        if (!arranged.empty())
            Im::Separator();
        named(namedKind);
    };

    // What is taken. Potion, Food and Ingredient list what is carried of
    // each, and are not drawn with nothing under them.
    if (valid(ft::ActionKind::DrinkStrongest))
    {
        // The group is entered only for a heading that is drawn, or the
        // divider would stand above the next group with nothing over it.
        if (carried(ft::ConsumableKind::Potion))
        {
            if (!enter(1))
                return true;
            if (BeginCascade(Tr("Potion")))
            {
                byEffect(ft::ConsumableKind::Potion, ft::ActionKind::DrinkStrongest, ft::ActionKind::DrinkWeakest,
                         ft::ActionKind::DrinkPotion, ft::ActionKind::DrinkAny);
                Im::EndMenu();
            }
        }
        if (carried(ft::ConsumableKind::Food))
        {
            if (!enter(1))
                return true;
            if (BeginCascade(Tr("Food")))
            {
                byEffect(ft::ConsumableKind::Food, ft::ActionKind::EatStrongestFood, ft::ActionKind::EatWeakestFood,
                         ft::ActionKind::EatFood, ft::ActionKind::EatAnyFood);
                Im::EndMenu();
            }
        }
        if (carried(ft::ConsumableKind::Ingredient))
        {
            if (!enter(1))
                return true;
            if (BeginCascade(Tr("Ingredient")))
            {
                byEffect(ft::ConsumableKind::Ingredient, ft::ActionKind::EatStrongestIngredient,
                         ft::ActionKind::EatWeakestIngredient, ft::ActionKind::EatIngredient);
                Im::EndMenu();
            }
        }
    }

    // What is cast: the spells that suit this target -- a Self-delivery
    // spell (Fast Healing, Oakflesh) is cast on oneself and on no one
    // else; an aimed one (Heal Other, Firebolt) goes at someone else --
    // then the same from both hands, only the spells the follower can dual
    // cast; then the shouts, then the powers. A follower with none that
    // fit is offered nothing rather than an empty submenu that looks broken.
    struct CastMenu
    {
        const char *label;
        ft::ActionKind action;
        bool dual;
    };
    for (const CastMenu menu :
         {CastMenu{Tr("Cast"), ft::ActionKind::CastSpell, false},
          CastMenu{Tr("Dual Cast"), ft::ActionKind::CastSpell, true},
          CastMenu{Tr("Scroll"), ft::ActionKind::UseScroll, false}, CastMenu{Tr("Shout"), ft::ActionKind::Shout, false},
          CastMenu{Tr("Power"), ft::ActionKind::UsePower, false}})
    {
        const auto action = menu.action;
        if (!valid(action))
            continue;
        const auto kind = SpellKindOf(action);
        std::vector<const SpellOption *> suited;
        for (const auto &option : view.spells)
        {
            // A corpse takes a Reanimate and nothing else.
            if (target == ft::ActionTargetKind::Corpse && !option.reanimate)
                continue;
            if (menu.dual && !option.dualCast)
                continue;
            if (option.kind == kind && (option.location || option.selfOnly == (target == ft::ActionTargetKind::Self)))
                suited.push_back(&option);
        }
        if (suited.empty())
            continue;
        // The hand's casts -- spells, scrolls -- then the voice's, a
        // divider between.
        if (!enter(action == ft::ActionKind::CastSpell || action == ft::ActionKind::UseScroll ? 2 : 3))
            return true;
        if (!BeginCascade(menu.label))
            continue;
        const auto leaf = [&](const SpellOption *optionPtr) {
            const SpellOption &option = *optionPtr;
            const bool selected = here && act.kind == action && act.form == option.form && act.dual == menu.dual;
            // Banned: dimmed as the equip leaves are, and still a choice,
            // for the reason EquipLeaf gives.
            const bool banned = BannedForm(view, option.form);
            if (CascadeItem(option.name.c_str(), selected,
                            banned ? &Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled] : nullptr))
            {
                act.kind = action;
                act.form = option.form;
                act.name = option.name;
                act.dual = menu.dual;
                choose();
            }
            if (banned && Im::IsItemHovered(0))
                Im::SetTooltip("%s", Tr("Banned"));
        };
        if (SchoolGrouped(kind))
            DrawBySchool(suited, [](const SpellOption *option) { return option->school; }, leaf);
        else
            for (const auto *option : suited)
                leaf(option);
        Im::EndMenu();
    }

    // What is done to the weapon in hand: Charge (the strongest gem that
    // fits, the weakest, then every spendable gem by name) and Poison (the
    // strongest and the weakest by effect, then every poison carried by
    // name). Neither is drawn with nothing to spend, and a gem counts only
    // filled: the scan lists no empty one, so a bag of empty gems offers no
    // Charge. Each enters the group only when drawn, as Potion does.
    if (valid(ft::ActionKind::ChargeStrongestSoulGem))
    {
        if (carried(ft::ConsumableKind::SoulGem))
        {
            if (!enter(4))
                return true;
            if (BeginCascade(Tr("Charge")))
            {
                policy(ft::ActionKind::ChargeStrongestSoulGem);
                policy(ft::ActionKind::ChargeWeakestSoulGem);
                Im::Separator();
                named(ft::ActionKind::ChargeSoulGem);
                Im::EndMenu();
            }
        }
        if (carried(ft::ConsumableKind::Poison))
        {
            if (!enter(4))
                return true;
            if (BeginCascade(Tr("Poison")))
            {
                byEffect(ft::ConsumableKind::Poison, ft::ActionKind::ApplyStrongest, ft::ActionKind::ApplyWeakest,
                         ft::ActionKind::ApplyPoison, ft::ActionKind::ApplyAny);
                Im::EndMenu();
            }
        }
    }

    // What is put on, and PINNED: Weapon, Arrows, Armor, Spell, each its
    // own menu of what is carried or known. The heading's tooltip is the
    // action's name, "Equip weapon": what choosing from it writes into the
    // rule, not the promise the pin makes.
    if (valid(ft::ActionKind::EquipWeapon))
    {
        if (!enter(5))
            return true;
        for (const auto kind : {ft::ActionKind::EquipWeapon, ft::ActionKind::EquipArrows, ft::ActionKind::EquipArmor,
                                ft::ActionKind::EquipSpell})
        {
            const bool open = BeginCascade(NounHeading(kind).c_str());
            if (Im::IsItemHovered(0))
                Tooltip(ft::DisplayName(kind));
            if (!open)
                continue;
            if (EquipMenu(act, kind, view))
                choose();
            Im::EndMenu();
        }
    }
    return changed;
}

} // namespace

std::string FormName(std::uint32_t form)
{
    const auto *record = form != 0 ? RE::TESForm::LookupByID(form) : nullptr;
    const char *name = record ? record->GetName() : nullptr;
    return name && *name ? name : "";
}

bool ConditionAvailable(const ft::Rule &rule, const FollowerView &view)
{
    return ft::ConditionHad(rule, view.holdings);
}

ft::Verdict VerdictAt(const FollowerView &view, std::size_t rule, std::size_t action)
{
    if (rule < view.availability.size() && action < view.availability[rule].size())
        return view.availability[rule][action];
    return ft::Verdict::Fired;
}

std::string SetAsideReason(const ft::Rule &rule, const FollowerView &view, std::size_t ruleIndex)
{
    switch (ft::RuleSetAside(rule, view.holdings))
    {
    case ft::Aside::FollowerAway:
        return Tr(kFollowerAway);
    case ft::Aside::NotHad:
        return Tr(kNotAvailable);
    case ft::Aside::None:
        break;
    }
    if (rule.actions.empty())
        return {};
    std::string first;
    for (std::size_t a = 0; a < rule.actions.size(); ++a)
    {
        const std::string why = UnavailableText(VerdictAt(view, ruleIndex, a), rule.actions[a].kind);
        if (why.empty())
            return {};
        if (first.empty())
            first = why;
    }
    return rule.actions.size() == 1 ? first : std::string(Tr("No action can be done right now"));
}

std::string ActionText(const ft::Action &act, const FollowerView &view)
{
    std::string base(ft::DisplayName(act.kind));

    // A policy names its effect: "Strongest Health potion", "Weakest Resist
    // Fire potion", "Strongest Fear poison".
    if (ft::IsPolicy(act.kind))
    {
        const auto kind = ft::ConsumableOf(act.kind);
        const bool strongest = ft::IsStrongest(act.kind);
        // No effect named is "any": the strongest of whichever effect the
        // roll lands on. "Buff" stands where the effect would, because that
        // is what narrows the roll for anything drunk or eaten; a poison
        // rolls among the lot and needs no word for it.
        if (act.effect.empty())
        {
            switch (kind)
            {
            case ft::ConsumableKind::Poison:
                return strongest ? Tr("Strongest poison") : Tr("Weakest poison");
            case ft::ConsumableKind::Food:
                return strongest ? Tr("Strongest buff food") : Tr("Weakest buff food");
            case ft::ConsumableKind::Ingredient:
                return strongest ? Tr("Strongest buff ingredient") : Tr("Weakest buff ingredient");
            default:
                return strongest ? Tr("Strongest buff potion") : Tr("Weakest buff potion");
            }
        }
        const std::string_view effect = ft::EffectLabel(act.effect);
        switch (kind)
        {
        case ft::ConsumableKind::Poison:
            return strongest ? TrFormat("Strongest {} poison", effect) : TrFormat("Weakest {} poison", effect);
        case ft::ConsumableKind::Food:
            return strongest ? TrFormat("Strongest {} food", effect) : TrFormat("Weakest {} food", effect);
        case ft::ConsumableKind::Ingredient:
            return strongest ? TrFormat("Strongest {} ingredient", effect) : TrFormat("Weakest {} ingredient", effect);
        default:
            return strongest ? TrFormat("Strongest {} potion", effect) : TrFormat("Weakest {} potion", effect);
        }
    }

    if (ft::NamesConsumable(act.kind))
    {
        if (act.form == 0)
            return base + "...";
        const auto verb = [&](const std::string &name) {
            return act.kind == ft::ActionKind::DrinkPotion     ? TrFormat("Drink {}", name)
                   : act.kind == ft::ActionKind::ApplyPoison   ? TrFormat("Apply {}", name)
                   : act.kind == ft::ActionKind::ChargeSoulGem ? TrFormat("Charge with {}", name)
                                                               : TrFormat("Eat {}", name);
        };
        for (const auto &option : view.consumables)
            if (option.form == act.form && option.kind == ft::ConsumableOf(act.kind))
                return verb(option.name);
        // Not carried: the name it was last seen with, the row set aside.
        const std::string name = LastName(act);
        return name.empty() ? base : verb(name);
    }

    if (ft::IsArrowsPolicy(act.kind))
        return TrFormat("Equip {}", ft::Noun(act.kind));
    if (ft::IsEquip(act.kind))
    {
        if (act.form == 0)
            return ft::TakesHand(act.kind) && act.hand != Hand::None
                       ? TrFormat("Unequip {} ({})", ft::Noun(act.kind), Lower(ft::DisplayName(act.hand)))
                       : TrFormat("Unequip {}", ft::Noun(act.kind));
        // Carried or known, else the name it was last seen with: the row
        // set aside.
        std::string name = EquipTargetName(act, view);
        if (name.empty())
            name = FormName(act.form);
        if (name.empty())
            return base;
        return ft::TakesHand(act.kind) ? TrFormat("Equip {} ({})", name, Lower(ft::DisplayName(act.hand)))
                                       : TrFormat("Equip {}", name);
    }

    if (!TakesSpell(act.kind))
        return base;

    if (act.form == 0)
        return base + "...";

    const auto verb = [&](const std::string &name) {
        return act.kind == ft::ActionKind::UsePower    ? TrFormat("Use {}", name)
               : act.kind == ft::ActionKind::Shout     ? TrFormat("Shout {}", name)
               : act.kind == ft::ActionKind::UseScroll ? TrFormat("Read {}", name)
               : act.dual                              ? TrFormat("Dual cast {}", name)
                                                       : TrFormat("Cast {}", name);
    };
    const auto kind = SpellKindOf(act.kind);
    for (const auto &option : view.spells)
        if (option.form == act.form && option.kind == kind)
            return verb(option.name);

    // Named a spell this follower does not know, or a scroll not carried:
    // the name it was last seen with, the row set aside.
    const std::string name = LastName(act);
    if (name.empty())
        return base;
    return verb(name);
}

bool ActionMenu(const char *id, ft::Action &act, const FollowerView &view, ft::Moment moment, bool *addAnother,
                ft::Rule &rule, const std::string &setAside, ft::Verdict verdict)
{
    bool changed = false;

    // An action naming a thing the follower no longer has, or aimed at a
    // follower who is away, is greyed, with the reason on it -- and still
    // opens: the potion drunk up wants choosing again, here, not deleting
    // and writing afresh. So is one that could not be done this moment --
    // the magicka short, the voice recovering, a power used today
    // (UnavailableText) -- which reads as it was when the page was built.
    // The colour is pushed round the cell alone, so the menu it opens reads
    // as usual; `setAside` greys it with the rest of a row set aside for
    // its condition.
    const std::string now = UnavailableText(verdict, act.kind);
    const std::string reason = !TargetAvailable(rule, view)  ? kFollowerAway
                               : !ActionAvailable(act, view) ? kNotAvailable
                                                             : now;
    const std::string text = TargetText(rule, view) + ": " + ActionText(act, view);
    Im::ImVec2 below;
    bool elided = false;
    {
        const DimText grey(!setAside.empty() || !reason.empty());
        below = CellButtonOpensPopup(id, text, &elided);
    }
    // The action's own reason first, else why the whole row is set aside,
    // else the whole of the phrase where the cell had to cut it.
    if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
    {
        const std::string &why = reason.empty() ? setAside : reason;
        Tooltip(!why.empty() ? why : (elided ? text : std::string{}));
    }

    PushPopupChrome();
    Im::SetNextWindowPos(below, Im::ImGuiCond_Always, Im::ImVec2(0.0f, 0.0f));
    if (!Im::BeginPopup(id, 0))
    {
        Im::PopStyleVar(kPopupChromeVars);
        return false;
    }

    // A rule with one action edits it here, in its row. The way to a second
    // is this entry, first and in a section of its own so it is not taken
    // for an action: the rule then opens as a drawer, where its actions
    // are listed, ordered and added to.
    if (addAnother)
    {
        if (CascadeItem(Tr("Add action..."), false))
        {
            *addAnother = true;
            changed = true;
        }
        Im::Separator();
    }

    struct Heading
    {
        ft::ActionTargetKind target;
        std::uint32_t form;
        std::string label;
    };
    // The same order as the If cascade's: self, the player, the other
    // followers by name, any ally; then the threats -- the attacker, and
    // the enemy (the condition's, or the one being fought).
    std::vector<Heading> headings{
        {ft::ActionTargetKind::Self, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Self))}};
    if (!view.player)
        headings.push_back(
            {ft::ActionTargetKind::Player, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Player))});
    for (const auto &peer : SortedPeers(view))
        headings.push_back({ft::ActionTargetKind::Follower, peer.id, peer.name});
    headings.push_back({ft::ActionTargetKind::Ally, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Ally))});
    headings.push_back(
        {ft::ActionTargetKind::Attacker, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Attacker))});
    headings.push_back({ft::ActionTargetKind::Enemy, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Enemy))});
    headings.push_back({ft::ActionTargetKind::Corpse, 0, std::string(ft::DisplayName(ft::ActionTargetKind::Corpse))});

    for (const Heading &heading : headings)
    {
        // Nor an Enemy or an Attacker to aim at, in the idle list.
        if (!ft::IsActionTargetValidFor(rule.subject, heading.target) ||
            !ft::IsActionTargetValidIn(moment, heading.target))
            continue;
        // Under "Corpse: None" there is no corpse to aim at.
        if (heading.target == ft::ActionTargetKind::Corpse && rule.predicate == ft::PredicateKind::CorpseNone)
            continue;
        // Nothing this follower could do to them: no heading, rather than one
        // that opens on an empty menu.
        if (!ActionItems(rule, act, heading.target, heading.form, view, moment, true))
            continue;
        if (!BeginCascade(heading.label.c_str()))
            continue;
        if (ActionItems(rule, act, heading.target, heading.form, view, moment))
            changed = true;
        Im::EndMenu();
    }

    Im::EndPopup();
    Im::PopStyleVar(kPopupChromeVars);
    return changed;
}

} // namespace ft::game::ui
