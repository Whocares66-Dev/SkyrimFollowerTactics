#include "core/Tick.h"

#include <algorithm>

namespace ft
{

std::vector<std::uint32_t> SpellsNamedBy(const ActorRules &rules)
{
    std::vector<std::uint32_t> named;
    for (const RuleSet *list : {&rules.combat, &rules.idle})
        for (const Rule &rule : list->rules)
            for (const Action &action : rule.actions)
                if (IsCast(action.kind) && action.form != 0 &&
                    std::find(named.begin(), named.end(), action.form) == named.end())
                    named.push_back(action.form);
    return named;
}

TickPlan ActorTick::Plan(EvalContext &ctx, const Now &now) noexcept
{
    if (now.held)
        return {};

    // At most twice round: a list switched off gives way to the other, and
    // the other, once, to nothing.
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool began = now.fighting && !fighting;
        const bool ended = !now.fighting && fighting;

        std::optional<Moment> list;
        if (now.fighting || ended || (ctx.InProgress() && moment == Moment::Combat))
            list = Moment::Combat;
        else if (ctx.InProgress() || now.idleHasRules)
            list = Moment::Idle;
        if (!list)
            return {};

        const bool enabled = *list == Moment::Combat ? now.combatEnabled : now.idleEnabled;
        if (enabled)
        {
            // Handed to the other list: whatever the last one had in
            // progress is not the new one's to run. The evaluator drops a
            // sequence on a fight's edge; without one -- the combat list
            // switched on mid-fight, its edge already spent below -- an
            // idle list part way through would run as combat tactics.
            if (*list != moment)
            {
                ctx.pending = {};
                ctx.queued.clear();
            }
            fighting = now.fighting;
            moment = *list;
            return {list, began, ended};
        }

        // Switched off: the edge is spent on it all the same, and whatever
        // of its own it had in progress goes.
        fighting = now.fighting;
        if (moment == *list)
        {
            ctx.pending = {};
            ctx.queued.clear();
        }
    }
    return {};
}

} // namespace ft
