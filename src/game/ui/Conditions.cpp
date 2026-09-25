// The condition cell of the rule editor: its text, and the menu that picks
// a subject and a condition.

#include "game/ui/Rules.h"
#include "game/ui/Widgets.h"

#include "core/Effects.h"
#include "core/Names.h"
#include "core/Vocabulary.h"
#include "game/Places.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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

// Preset values offered for a predicate's argument.
//
// Dragon Age offered choices, not a number box -- "has low armour rating", not
// a slider -- and that is the right call for a tactics editor: the point is to
// express intent quickly, and nobody needs 37%. It also keeps the whole
// condition authorable in one cascade with no separate value column.
// How a predicate's value is written, wherever it appears.
//
// One function, so the menu and the row cannot disagree: the preset a player
// picks reads back exactly as the rule then reads. The comparison lives with
// the VALUE rather than in the predicate's name so it is printed once --
// "health < 50%", not "health below < 50%".
std::string ArgumentText(ft::PredicateKind predicate, float value)
{
    switch (ft::ArgumentFor(predicate))
    {
    case ft::ArgumentKind::Percent:
        return (ft::IsAbove(predicate) ? "> " : "< ") + std::to_string(std::lround(value * 100.0f)) + "%";
    case ft::ArgumentKind::None:
    default:
        return {};
    }
}

// The values offered for a predicate's argument.
//
// Dragon Age offered choices, not a number box -- "has low armour rating", not
// a slider -- and that is the right call for a tactics editor: the point is to
// express intent quickly, and nobody needs 37%. It also keeps the whole
// condition authorable in one cascade with no separate value column.
//
// Values only. Their text comes from ArgumentText so there is no second place
// for a label to drift out of step with what the rule actually says.
std::vector<float> PresetsFor(ft::PredicateKind predicate)
{
    switch (ft::ArgumentFor(predicate))
    {
    case ft::ArgumentKind::Percent:
        // A resistance is the one number here that commonly goes negative:
        // a weakness, from a curse, a race or a spell. So it gets 0 as well,
        // where "< 0%" is "weak to this" and "> 0%" "resists it at all" --
        // the questions worth asking about a weakness, and neither of them
        // expressible with the thresholds alone. Health, magicka, stamina
        // and armour never go below zero, so 0 would be a dead entry there.
        if (ft::IsResistance(predicate))
            return {0.0f, 0.25f, 0.50f, 0.75f};
        return {0.25f, 0.50f, 0.75f};
    case ft::ArgumentKind::None:
    default:
        return {};
    }
}

// The whole condition on one line: "Enemy health < 25%".
// Who a condition is about, by name where it names someone: the player, or
// one particular follower.
std::string SubjectText(const ft::Rule &r, const FollowerView &view)
{
    if (r.subject == ft::SubjectKind::Follower)
    {
        for (const auto &peer : view.peers)
            if (peer.id == r.subjectForm)
                return peer.name;
        return Tr("Follower (away)");
    }
    return std::string(ft::DisplayName(r.subject));
}

// The two about a fight, under one "Combat" heading: Start, End.
// The condition cascade in groups, a divider between them: Any; the
// fight's edges; the three stats; the enemy's relation to the party
// (Attacking, Attacked by); the hits (Hit type, Hit by); Status; the
// equipment -- weapon, armour, resistance; the summon; where one is. (The corpse
// questions are a subject of their own and fall in one group.)
int ConditionGroup(ft::PredicateKind p)
{
    switch (p)
    {
    case ft::PredicateKind::Any:
        return 0;
    case ft::PredicateKind::CombatBegins:
    case ft::PredicateKind::CombatEnds:
        return 1;
    case ft::PredicateKind::HealthPctBelow:
    case ft::PredicateKind::StaminaPctBelow:
    case ft::PredicateKind::MagickaPctBelow:
        return 2;
    case ft::PredicateKind::Attacking:
    case ft::PredicateKind::AttackedBy:
        return 3;
    case ft::PredicateKind::HitType:
    case ft::PredicateKind::HitBy:
        return 4;
    case ft::PredicateKind::Type:
        return 5;
    case ft::PredicateKind::Status:
    case ft::PredicateKind::EffectRunning:
        return 6;
    case ft::PredicateKind::SummonNone:
    case ft::PredicateKind::SummonActive:
        return 8;
    case ft::PredicateKind::Location:
        return 9;
    default:
        return 7;
    }
}

// Is this predicate the one its heading is drawn at? The rest of a heading
// (Combat end, Summon active, Lowest level, the poison pair) are drawn
// under it and skipped in the walk.
bool DrawsHeading(ft::PredicateKind p)
{
    switch (p)
    {
    case ft::PredicateKind::CombatEnds:
    case ft::PredicateKind::SummonActive:
    case ft::PredicateKind::LevelLowest:
    case ft::PredicateKind::WeaponPoisonNone:
    case ft::PredicateKind::WeaponPoisonActive:
    case ft::PredicateKind::WeaponBoundNone:
    case ft::PredicateKind::WeaponBoundActive:
    case ft::PredicateKind::ArrowsAvailable:
        return false;
    default:
        return !ft::IsAbove(p) && !ft::IsExtreme(p);
    }
}

} // namespace

std::string ConditionText(const ft::Rule &r, const FollowerView &view)
{
    // Who, a colon, then what: "Self: Attacked by Fire". The colon keeps
    // the two halves from having to agree grammatically.
    const std::string subject = SubjectText(r, view);
    // A status reads as the status: "Self Poisoned", not "Self Status";
    // a kind of being as the kind: "Enemy: Undead", "Enemy: Nord".
    if (r.predicate == ft::PredicateKind::Status)
        return TrFormat("{}: {}", subject, ft::DisplayName(r.statusKind));
    if (r.predicate == ft::PredicateKind::Type)
        return TrFormat("{}: {}", subject, ft::DisplayName(r.typeKind));
    // A place with where it is: "Self: In Cave"; a hold by the game's
    // name for it, "Self: In Whiterun".
    if (r.predicate == ft::PredicateKind::Location && r.locationKind == ft::LocationKind::Hold)
        return TrFormat("{}: {}", subject, TrFormat("In {}", HoldName(r.conditionForm)));
    if (r.predicate == ft::PredicateKind::Location)
        return TrFormat("{}: {}", subject, TrFormat("In {}", ft::DisplayName(r.locationKind)));
    // An effect reads by its name, as a status does: "Self: Oakflesh".
    if (r.predicate == ft::PredicateKind::EffectRunning)
        return TrFormat("{}: {}", subject, FormName(r.conditionForm));
    // A resistance reads as "Resistance Fire", then lowest, highest or the
    // number; an attack as "Attacked by Fire". Each a line of its own, so
    // a language may put the kind first.
    std::string what;
    const std::string_view damage = ft::DisplayName(r.damageKind);
    if (ft::IsResistance(r.predicate))
    {
        if (r.predicate == ft::PredicateKind::ResistanceLowest)
            return TrFormat("{}: {}", subject, TrFormat("Resistance {} lowest", damage));
        if (r.predicate == ft::PredicateKind::ResistanceHighest)
            return TrFormat("{}: {}", subject, TrFormat("Resistance {} highest", damage));
        what = TrFormat("Resistance {}", damage);
    }
    else if (r.predicate == ft::PredicateKind::HitBy)
        what = TrFormat("Hit by {}", damage);
    else if (r.predicate == ft::PredicateKind::HitType)
        what = TrFormat("Attacks with {}", damage);
    // The party member: "Enemy: Attacking Self", "Enemy: Attacked by
    // Player", "... Attacking Lydia".
    else if (r.predicate == ft::PredicateKind::Attacking || r.predicate == ft::PredicateKind::AttackedBy)
    {
        std::string name;
        if (r.subjectForm == 0)
            name = ft::DisplayName(ft::SubjectKind::Player);
        else if (r.subjectForm == view.id)
            name = ft::DisplayName(ft::SubjectKind::Self);
        else
        {
            name = Tr("a follower (away)");
            for (const auto &peer : view.peers)
                if (peer.id == r.subjectForm)
                    name = peer.name;
        }
        what = r.predicate == ft::PredicateKind::Attacking ? TrFormat("Attacking {}", name)
                                                           : TrFormat("Attacked by {}", name);
    }
    else
        what = ft::DisplayName(r.predicate);

    if (const std::string arg = ArgumentText(r.predicate, r.conditionArg); !arg.empty())
        what += ' ' + arg;
    return TrFormat("{}: {}", subject, what);
}

std::vector<FollowerView::Peer> SortedPeers(const FollowerView &view)
{
    std::vector<FollowerView::Peer> peers = view.peers;
    ft::SortByName(peers, [](const auto &item) -> std::string_view { return item.name; });
    return peers;
}

bool ConditionCascade(const char *id, ft::Rule &rule, const FollowerView &view, ft::Moment moment,
                      const std::string &setAside)
{
    bool changed = false;

    // A condition naming a follower who is away is greyed, with the reason
    // on it -- and still opens, so another can be named in its place. The
    // colour is pushed round the cell alone, so the menu reads as usual;
    // `setAside` greys it with the rest of a row set aside for its action.
    const bool available = ConditionAvailable(rule, view);
    const std::string text = ConditionText(rule, view);
    Im::ImVec2 below;
    bool elided = false;
    {
        const DimText grey(!setAside.empty() || !available);
        below = CellButtonOpensPopup(id, text, &elided);
    }
    // The named follower being gone, which is about the condition itself,
    // else the whole of the condition where the cell had to cut it. Why the
    // ROW is set aside belongs on the action cell, which is where the thing
    // that is missing is named.
    if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Tooltip(!available ? std::string(Tr(kFollowerAway)) : (elided ? text : std::string{}));

    PushPopupChrome();
    Im::SetNextWindowPos(below, Im::ImGuiCond_Always, Im::ImVec2(0.0f, 0.0f));
    if (!Im::BeginPopup(id, 0))
    {
        Im::PopStyleVar(kPopupChromeVars);
        return false;
    }

    // Who first: the follower, the player by name, the other followers by
    // name, any ally; then the one being fought, and any enemy. Self first,
    // allies above enemies, the particular above the general.
    struct Heading
    {
        ft::SubjectKind subject;
        std::uint32_t form;
        std::string label;
    };
    std::vector<Heading> headings{{ft::SubjectKind::Self, 0, std::string(ft::DisplayName(ft::SubjectKind::Self))}};
    // On the player's own page Self is the player, and the Player heading
    // would say it twice.
    if (!view.player)
        headings.push_back({ft::SubjectKind::Player, 0, std::string(ft::DisplayName(ft::SubjectKind::Player))});
    for (const auto &peer : SortedPeers(view))
        headings.push_back({ft::SubjectKind::Follower, peer.id, peer.name});
    headings.push_back({ft::SubjectKind::Ally, 0, std::string(ft::DisplayName(ft::SubjectKind::Ally))});
    headings.push_back({ft::SubjectKind::Enemy, 0, std::string(ft::DisplayName(ft::SubjectKind::Enemy))});
    headings.push_back({ft::SubjectKind::Corpse, 0, std::string(ft::DisplayName(ft::SubjectKind::Corpse))});

    for (const Heading &heading : headings)
    {
        const ft::SubjectKind subject = heading.subject;
        const std::uint32_t form = heading.form;
        // No Enemy in the idle list: there is none out of a fight.
        if (!ft::IsSubjectValidIn(moment, subject))
            continue;
        if (!BeginCascade(heading.label.c_str()))
            continue;

        // One item of the cascade: selected when the rule says exactly
        // this, and on a click the rule says it. What "this" is beyond the
        // subject and the predicate -- a number, a kind of damage, a
        // status, a party member -- is the item's extras. The tooltip is
        // the predicate's help.
        struct Extras
        {
            std::optional<float> arg;
            std::optional<ft::DamageKind> damage;
            std::optional<ft::StatusKind> status;
            std::optional<ft::TypeKind> type;
            std::optional<ft::LocationKind> location;
            std::optional<std::uint32_t> member;
            std::optional<std::uint32_t> conditionForm; // a family's keyword, an effect's source
        };
        const auto pick = [&](const char *label, ft::PredicateKind which, const Extras &x = {}, bool tip = true) {
            const std::uint32_t subjectForm = x.member.value_or(form);
            const bool selected =
                rule.subject == subject && rule.subjectForm == subjectForm && rule.predicate == which &&
                (!x.damage || rule.damageKind == *x.damage) && (!x.status || rule.statusKind == *x.status) &&
                (!x.type || rule.typeKind == *x.type) && (!x.location || rule.locationKind == *x.location) &&
                (!x.conditionForm || rule.conditionForm == *x.conditionForm) &&
                (!x.arg || std::abs(rule.conditionArg - *x.arg) < 0.001f);
            if (CascadeItem(label, selected))
            {
                rule.subject = subject;
                rule.subjectForm = subjectForm;
                rule.predicate = which;
                if (x.damage)
                    rule.damageKind = *x.damage;
                if (x.status)
                    rule.statusKind = *x.status;
                if (x.type)
                    rule.typeKind = *x.type;
                if (x.location)
                    rule.locationKind = *x.location;
                if (x.conditionForm)
                    rule.conditionForm = *x.conditionForm;
                if (x.arg)
                    rule.conditionArg = *x.arg;
                changed = true;
            }
            if (const auto text = tip ? ft::Describe(which) : std::string_view{}; !text.empty() && Im::IsItemHovered(0))
                Tooltip(text);
        };

        // A heading over a few leaves -- Combat: Start, End -- for the
        // predicates listed under one name, those the subject answers.
        struct Leaf
        {
            ft::PredicateKind predicate;
            const char *label;
        };
        const auto submenu = [&](const char *title, std::initializer_list<Leaf> leaves) {
            if (!BeginCascade(title))
                return;
            for (const Leaf &leaf : leaves)
                if (ft::IsPredicateValidFor(subject, leaf.predicate))
                    pick(leaf.label, leaf.predicate);
            Im::EndMenu();
        };

        // The thresholds of a grid predicate: the group's Lowest and
        // Highest first where the subject is a group, then the percents
        // below, then above. A resistance's carry its kind of damage.
        const auto thresholds = [&](ft::PredicateKind predicate, std::optional<ft::DamageKind> damage) {
            Extras x;
            x.damage = damage;
            if (const auto extremes = ft::ExtremesOf(predicate);
                extremes.lowest != predicate && ft::IsPredicateValidFor(subject, extremes.lowest))
            {
                pick(Tr("Lowest"), extremes.lowest, x);
                pick(Tr("Highest"), extremes.highest, x);
                Im::Separator();
            }
            for (const float preset : PresetsFor(predicate))
            {
                x.arg = preset;
                pick(ArgumentText(predicate, preset).c_str(), predicate, x);
            }
            if (const auto above = ft::AboveOf(predicate); above != predicate)
            {
                Im::Separator();
                for (const float preset : PresetsFor(above))
                {
                    x.arg = preset;
                    pick(ArgumentText(above, preset).c_str(), above, x);
                }
            }
        };

        int lastGroup = -1;
        for (std::size_t pi = 0; pi < static_cast<std::size_t>(ft::PredicateKind::COUNT); ++pi)
        {
            const auto predicate = static_cast<ft::PredicateKind>(pi);
            if (!ft::IsPredicateValidFor(subject, predicate) || !ft::IsPredicateValidIn(moment, predicate))
                continue;
            // An above predicate is listed under its below counterpart's
            // heading, after a divider, not as a heading of its own; the
            // group's extremes likewise, first under theirs; and the rest
            // of a heading's leaves under the first of them.
            if (!DrawsHeading(predicate))
                continue;
            // A divider where one group of conditions ends and the next
            // begins.
            const int group = ConditionGroup(predicate);
            if (lastGroup >= 0 && group != lastGroup)
                Im::Separator();
            lastGroup = group;

            const std::string predicateName(ft::DisplayName(predicate));

            // The headings of a few leaves: the fight's edges, the summons,
            // the corpses' level; the weapons in hand, with the poison pair
            // under a heading of their own.
            if (predicate == ft::PredicateKind::CombatBegins)
            {
                submenu(Tr("Combat"),
                        {{ft::PredicateKind::CombatBegins, Tr("Start")}, {ft::PredicateKind::CombatEnds, Tr("End")}});
                continue;
            }
            if (predicate == ft::PredicateKind::ArrowsNone)
            {
                submenu(Tr("Arrows"), {{ft::PredicateKind::ArrowsNone, Tr("None")},
                                       {ft::PredicateKind::ArrowsAvailable, Tr("Available")}});
                continue;
            }
            if (predicate == ft::PredicateKind::SummonNone)
            {
                submenu(Tr("Summon"),
                        {{ft::PredicateKind::SummonNone, Tr("None")}, {ft::PredicateKind::SummonActive, Tr("Active")}});
                continue;
            }
            if (predicate == ft::PredicateKind::LevelHighest)
            {
                submenu(Tr("Level"), {{ft::PredicateKind::LevelHighest, Tr("Highest")},
                                      {ft::PredicateKind::LevelLowest, Tr("Lowest")}});
                continue;
            }
            if (predicate == ft::PredicateKind::WeaponChargeNeeded)
            {
                if (!BeginCascade(Tr("Weapon")))
                    continue;
                // By name in the language shown, as the statuses are.
                const char *bound = Tr("Bound");
                const char *charge = Tr("Charge needed");
                const char *poison = Tr("Poison");
                std::vector<std::pair<const char *, std::function<void()>>> entries{
                    {bound,
                     [&] {
                         submenu(bound, {{ft::PredicateKind::WeaponBoundNone, Tr("None")},
                                         {ft::PredicateKind::WeaponBoundActive, Tr("Active")}});
                     }},
                    {charge, [&] { pick(charge, ft::PredicateKind::WeaponChargeNeeded); }},
                    {poison,
                     [&] {
                         submenu(poison, {{ft::PredicateKind::WeaponPoisonNone, Tr("None")},
                                          {ft::PredicateKind::WeaponPoisonActive, Tr("Active")}});
                     }},
                };
                ft::SortByName(entries, [](const auto &entry) { return std::string_view(entry.first); });
                for (const auto &entry : entries)
                    entry.second();
                Im::EndMenu();
                continue;
            }

            // A resistance: the kinds of damage under "Resistance"; under
            // each, the same shape as Health.
            if (predicate == ft::PredicateKind::ResistancePctBelow)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::DamageKind::COUNT); ++ki)
                {
                    const auto kind = static_cast<ft::DamageKind>(ki);
                    // Nothing resists a blow or an arrow but armour, which
                    // is its own heading, and nothing resists "any".
                    if (!ft::IsDamageKindValidFor(predicate, kind))
                        continue;
                    if (!BeginCascade(std::string(ft::DisplayName(kind)).c_str()))
                        continue;
                    thresholds(predicate, kind);
                    Im::EndMenu();
                }
                Im::EndMenu();
                continue;
            }

            // Hit type and Hit by: how -- a blow, an arrow, a spell of any
            // kind -- then what the spell was, a divider between each group.
            // Hit by opens on Any, hit with anything at all inside the
            // window; Hit type has no Any, because every actor hits with
            // something and the condition would be true of everyone.
            if (predicate == ft::PredicateKind::HitType || predicate == ft::PredicateKind::HitBy)
            {
                // The explanation sits on the heading, once, and not on each
                // of the eight kinds under it: "Attacks with this type of
                // damage" says the same thing over Fire as over Melee, and a
                // tooltip on every leaf only gets in the way of reading the
                // list (2026-09-17).
                if (!BeginCascade(predicateName.c_str(), std::string(ft::Describe(predicate)).c_str()))
                    continue;
                const auto kind = [&](ft::DamageKind k) {
                    Extras x;
                    x.damage = k;
                    pick(std::string(ft::DisplayName(k)).c_str(), predicate, x, false);
                };
                if (ft::IsDamageKindValidFor(predicate, ft::DamageKind::Any))
                {
                    kind(ft::DamageKind::Any);
                    Im::Separator();
                }
                kind(ft::DamageKind::Melee);
                kind(ft::DamageKind::Ranged);
                kind(ft::DamageKind::Magic);
                Im::Separator();
                kind(ft::DamageKind::Fire);
                kind(ft::DamageKind::Frost);
                kind(ft::DamageKind::Shock);
                kind(ft::DamageKind::Poison);
                Im::EndMenu();
                continue;
            }

            // The party: under "Attacking" and "Attacked by", the members by
            // name -- this follower, the player, the other followers -- each
            // a leaf that names the member, a divider between each part.
            if (predicate == ft::PredicateKind::Attacking || predicate == ft::PredicateKind::AttackedBy)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                const auto member = [&](std::uint32_t id, const std::string &label) {
                    Extras x;
                    x.member = id;
                    pick(label.c_str(), predicate, x);
                };
                member(view.id, std::string(ft::DisplayName(ft::SubjectKind::Self)));
                if (!view.player)
                {
                    Im::Separator();
                    member(0, std::string(ft::DisplayName(ft::SubjectKind::Player)));
                }
                if (const auto peers = SortedPeers(view); !peers.empty())
                {
                    Im::Separator();
                    for (const auto &peer : peers)
                        member(peer.id, peer.name);
                }
                Im::EndMenu();
                continue;
            }

            // A kind of being: four groups under "Type", each a menu of Any
            // -- the group itself -- and its members by name. Groups and
            // members both by name, not by the enum.
            if (predicate == ft::PredicateKind::Type)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                std::vector<ft::TypeKind> heads;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::TypeKind::COUNT); ++ki)
                    if (ft::IsGroupHead(static_cast<ft::TypeKind>(ki)))
                        heads.push_back(static_cast<ft::TypeKind>(ki));
                const auto name = [](ft::TypeKind kind) { return ft::DisplayName(kind); };
                ft::SortByName(heads, name);
                for (const ft::TypeKind head : heads)
                {
                    if (!BeginCascade(std::string(ft::DisplayName(head)).c_str()))
                        continue;
                    Extras any;
                    any.type = head;
                    pick(Tr("Any"), predicate, any);
                    Im::Separator();
                    std::vector<ft::TypeKind> members;
                    for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::TypeKind::COUNT); ++ki)
                        if (const auto kind = static_cast<ft::TypeKind>(ki);
                            ft::GroupOf(kind) == head && !ft::IsGroupHead(kind))
                            members.push_back(kind);
                    ft::SortByName(members, name);
                    for (const ft::TypeKind kind : members)
                    {
                        Extras x;
                        x.type = kind;
                        pick(std::string(ft::DisplayName(kind)).c_str(), predicate, x);
                    }
                    Im::EndMenu();
                }
                Im::EndMenu();
                continue;
            }

            // A status: the kinds, one leaf each, under "Status", in the
            // order a person looks for them -- by name, not by the enum.
            if (predicate == ft::PredicateKind::Status)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                std::vector<ft::StatusKind> kinds;
                for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::StatusKind::COUNT); ++ki)
                    if (ft::IsStatusValidFor(subject, static_cast<ft::StatusKind>(ki)) &&
                        ft::IsStatusValidIn(moment, static_cast<ft::StatusKind>(ki)))
                        kinds.push_back(static_cast<ft::StatusKind>(ki));
                ft::SortByName(kinds, [](ft::StatusKind kind) { return ft::DisplayName(kind); });
                for (const ft::StatusKind kind : kinds)
                {
                    Extras x;
                    x.status = kind;
                    pick(std::string(ft::DisplayName(kind)).c_str(), predicate, x);
                }
                Im::EndMenu();
                continue;
            }

            // A place: Home, Interior, Exterior, then the groups by name, each
            // a heading of Any and its kinds by name; a group of one kind a
            // leaf; the holds the load order has, by the game's names.
            if (predicate == ft::PredicateKind::Location)
            {
                if (!BeginCascade(predicateName.c_str()))
                    continue;
                const auto place = [&](const char *label, ft::LocationKind kind, std::uint32_t hold = 0) {
                    Extras x;
                    x.location = kind;
                    if (kind == ft::LocationKind::Hold)
                        x.conditionForm = hold;
                    pick(label, predicate, x);
                };
                const auto name = [](ft::LocationKind kind) { return std::string(ft::DisplayName(kind)); };
                for (const ft::LocationKind kind :
                     {ft::LocationKind::Home, ft::LocationKind::Interior, ft::LocationKind::Exterior})
                    place(name(kind).c_str(), kind);
                Im::Separator();
                std::vector<ft::LocationGroup> sections;
                for (std::size_t gi = 1; gi < static_cast<std::size_t>(ft::LocationGroup::COUNT); ++gi)
                    sections.push_back(static_cast<ft::LocationGroup>(gi));
                ft::SortByName(sections, [](ft::LocationGroup section) { return ft::DisplayName(section); });
                for (const ft::LocationGroup section : sections)
                {
                    const std::string title(ft::DisplayName(section));
                    if (section == ft::LocationGroup::Hold)
                    {
                        std::vector<HoldPick> holds = Holds();
                        if (holds.empty() || !BeginCascade(title.c_str()))
                            continue;
                        ft::SortByName(holds, [](const HoldPick &hold) -> std::string_view { return hold.name; });
                        for (const HoldPick &hold : holds)
                            place(hold.name.c_str(), ft::LocationKind::Hold, hold.form);
                        Im::EndMenu();
                        continue;
                    }
                    std::optional<ft::LocationKind> any;
                    std::vector<ft::LocationKind> kinds;
                    for (std::size_t ki = 0; ki < static_cast<std::size_t>(ft::LocationKind::COUNT); ++ki)
                    {
                        const auto kind = static_cast<ft::LocationKind>(ki);
                        if (ft::GroupOf(kind) != section)
                            continue;
                        if (ft::IsGroupAny(kind))
                            any = kind;
                        else
                            kinds.push_back(kind);
                    }
                    if (kinds.empty())
                    {
                        if (any)
                            place(title.c_str(), *any);
                        continue;
                    }
                    if (!BeginCascade(title.c_str()))
                        continue;
                    if (any)
                    {
                        place(Tr("Any"), *any);
                        Im::Separator();
                    }
                    ft::SortByName(kinds, [](ft::LocationKind kind) { return ft::DisplayName(kind); });
                    for (const ft::LocationKind kind : kinds)
                        place(name(kind).c_str(), kind);
                    Im::EndMenu();
                }
                Im::EndMenu();
                continue;
            }

            // Effect, right below Status: an effect running on the subject,
            // one of those the page's actor carries or knows leaves, by name;
            // only what is there to pick, and not drawn with nothing under
            // it. A rule answers by name, so its record need not be the
            // pick's to be the one ticked.
            if (predicate == ft::PredicateKind::EffectRunning)
            {
                if (view.effectPicks.empty() || !BeginCascade(Tr("Effect")))
                    continue;
                const std::string current =
                    rule.predicate == ft::PredicateKind::EffectRunning ? FormName(rule.conditionForm) : "";
                for (const ft::EffectPick &effect : view.effectPicks)
                {
                    Extras x;
                    x.conditionForm = effect.name == current ? rule.conditionForm : effect.effect;
                    pick(effect.name.c_str(), predicate, x, false);
                }
                Im::EndMenu();
                continue;
            }

            // No argument: a leaf. (Any's divider from the rest is the
            // group divider above.)
            if (PresetsFor(predicate).empty())
            {
                pick(predicateName.c_str(), predicate);
                continue;
            }

            // A number: the thresholds under the heading.
            if (!BeginCascade(predicateName.c_str()))
                continue;
            thresholds(predicate, std::nullopt);
            Im::EndMenu();
        }
        Im::EndMenu();
    }

    Im::EndPopup();
    Im::PopStyleVar(kPopupChromeVars);
    // A new subject may not supply the target the actions were aimed at --
    // "Ally" after the condition stopped being about an ally -- so the Then
    // side is put back in order: the target falls to Self, and an action
    // that makes no sense there is blanked.
    if (changed)
        ft::Reconcile(rule);
    return changed;
}

} // namespace ft::game::ui
