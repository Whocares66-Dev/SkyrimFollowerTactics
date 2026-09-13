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
    if (IsPolicy(action.kind) && IsConsume(action.kind))
    {
        const auto kind = ConsumableOf(action.kind);
        return std::any_of(has.consumables.begin(), has.consumables.end(), [&](const Holdings::Consumable &c) {
            return c.kind == kind && std::find(c.effects.begin(), c.effects.end(), action.effect) != c.effects.end();
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
        return Has(has.castable, action.form);
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
    const bool all = std::all_of(rule.actions.begin(), rule.actions.end(),
                                 [&](const Action &action) { return ActionHad(action, has); });
    return all ? Aside::None : Aside::NotHad;
}

} // namespace ft
