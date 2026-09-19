#include "Editor.h"

#include <algorithm>

namespace ft
{
namespace
{

bool Has(const std::vector<std::uint32_t> &forms, std::uint32_t form)
{
    return std::find(forms.begin(), forms.end(), form) != forms.end();
}

bool With(const Holdings &has, ActorId id)
{
    return Has(has.peers, id);
}

} // namespace

bool ActionHad(const Action &action, const Holdings &has)
{
    // A blow names no form, so it would pass below; this one is asked for
    // the perk the Settings page may require of it.
    if (action.kind == ActionKind::PowerBash)
        return has.powerBashPerk;
    // The two gem sizes name no gem and choose one at the firing, so they
    // are had while the bag holds one the scan calls spendable -- it lists
    // no empty gem, so a bag of empty ones is an empty bag here.
    if (IsCharge(action.kind) && action.kind != ActionKind::ChargeSoulGem)
        return std::any_of(has.consumables.begin(), has.consumables.end(),
                           [](const Holdings::Consumable &c) { return c.kind == ConsumableKind::SoulGem; });
    // Asked of the POISONS as much as of what is drunk and eaten: they are
    // the other half of the policies, and were falling through to the
    // form-of-zero line below, which reads "names nothing, so nothing to
    // miss" and is true only of an Unequip. A charge rule with no gem and
    // an Apply rule with no poison both read as available until 2026-09-17.
    if ((IsConsume(action.kind) || IsApply(action.kind)) && (IsPolicy(action.kind) || IsAny(action.kind)))
    {
        // A rule that names an effect is had when a bottle carries it; one
        // that names none -- an "any" -- when a bottle is worth rolling at
        // all, which for anything drunk or eaten means it buffs and for a
        // poison means any of them. A bag of six health potions answers the
        // first for "Restore Health" and the second for nothing.
        const auto kind = ConsumableOf(action.kind);
        const bool any = IsAny(action.kind) || action.effect.empty();
        return std::any_of(has.consumables.begin(), has.consumables.end(), [&](const Holdings::Consumable &c) {
            return c.kind == kind &&
                   (any ? c.any : std::find(c.effects.begin(), c.effects.end(), action.effect) != c.effects.end());
        });
    }
    // An arrow policy has what it would choose: any ammunition carried.
    if (IsArrowsPolicy(action.kind))
        return std::any_of(has.things.begin(), has.things.end(),
                           [](const Holdings::Thing &t) { return t.kind == Kind::Ammo; });
    if (action.form == 0)
        return true;
    if (NamesConsumable(action.kind))
    {
        const auto kind = ConsumableOf(action.kind);
        return std::any_of(has.consumables.begin(), has.consumables.end(),
                           [&](const Holdings::Consumable &c) { return c.form == action.form && c.kind == kind; });
    }
    if (IsEquip(action.kind))
        return std::any_of(has.things.begin(), has.things.end(), [&](const Holdings::Thing &t) {
            return t.form == action.form && SameVariant(t.variant, action.variant);
        });
    if (IsCast(action.kind))
        return Has(has.castable, action.form) && (!action.dual || Has(has.dualCastable, action.form));
    return true;
}

bool ConditionHad(const Rule &rule, const Holdings &has)
{
    if (rule.subject == SubjectKind::Follower)
        return With(has, rule.subjectForm);
    const bool member = rule.predicate == PredicateKind::Attacking || rule.predicate == PredicateKind::AttackedBy;
    if (member && rule.subjectForm != 0 && rule.subjectForm != has.self)
        return With(has, rule.subjectForm);
    return true;
}

bool TargetHad(const Rule &rule, const Holdings &has)
{
    return rule.actionTarget != ActionTargetKind::Follower || With(has, rule.actionTargetForm);
}

Aside RuleSetAside(const Rule &rule, const Holdings &has)
{
    if (!ConditionHad(rule, has) || !TargetHad(rule, has))
        return Aside::FollowerAway;
    const bool none = !rule.actions.empty() && ActionsNotHad(rule, has) == rule.actions.size();
    return none ? Aside::NotHad : Aside::None;
}

std::size_t ActionsNotHad(const Rule &rule, const Holdings &has)
{
    return static_cast<std::size_t>(std::count_if(rule.actions.begin(), rule.actions.end(),
                                                  [&](const Action &action) { return !ActionHad(action, has); }));
}

bool RefreshActionNames(RuleSet &rules, const std::function<std::string(const Action &)> &currentName)
{
    bool renamed = false;
    for (Rule &rule : rules.rules)
    {
        for (Action &action : rule.actions)
        {
            if (!NamesForm(action.kind) || action.form == 0)
                continue;
            const std::string now = currentName(action);
            if (!now.empty() && now != action.name)
            {
                action.name = now;
                renamed = true;
            }
        }
    }
    return renamed;
}

} // namespace ft
