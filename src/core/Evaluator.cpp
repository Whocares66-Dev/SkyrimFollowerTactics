#include "Evaluator.h"

#include <algorithm>

namespace ft {
namespace {

const AllyView* LowestHealthAlly(const Snapshot& s) {
    const AllyView* best = nullptr;
    for (const auto& a : s.allies) {
        if (!best || a.health.Pct() < best->health.Pct()) best = &a;
    }
    return best;
}

const EnemyView* LowestHealthEnemy(const Snapshot& s) {
    const EnemyView* best = nullptr;
    for (const auto& e : s.enemies) {
        if (!best || e.health.Pct() < best->health.Pct()) best = &e;
    }
    return best;
}

const EnemyView* NearestEnemy(const Snapshot& s) {
    const EnemyView* best = nullptr;
    for (const auto& e : s.enemies) {
        if (!best || e.distance < best->distance) best = &e;
    }
    return best;
}

const EnemyView* FindEnemy(const Snapshot& s, ActorId id) {
    for (const auto& e : s.enemies) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

}  // namespace

bool ConditionHolds(const Rule& r, const Snapshot& s) {
    switch (r.condition) {
        case ConditionKind::Always:
            return true;

        case ConditionKind::SelfHealthPctBelow:
            return s.health.Pct() < r.conditionArg;

        case ConditionKind::SelfMagickaPctBelow:
            return s.magicka.Pct() < r.conditionArg;

        case ConditionKind::SelfStaminaPctBelow:
            return s.stamina.Pct() < r.conditionArg;

        case ConditionKind::SelfInBleedout:
            return s.inBleedout;

        case ConditionKind::InCombat:
            return s.inCombat;

        case ConditionKind::PlayerHealthPctBelow:
            return s.playerHealth.Pct() < r.conditionArg;

        case ConditionKind::AllyHealthPctBelow: {
            const auto* a = LowestHealthAlly(s);
            return a && a->health.Pct() < r.conditionArg;
        }

        case ConditionKind::EnemyCountAtLeast:
            return static_cast<float>(s.enemies.size()) >= r.conditionArg;

        case ConditionKind::EnemyWithinDistance: {
            const auto* e = NearestEnemy(s);
            return e && e->distance <= r.conditionArg;
        }

        case ConditionKind::TargetHealthPctBelow: {
            const auto* e = FindEnemy(s, s.currentTarget);
            return e && e->health.Pct() < r.conditionArg;
        }

        default:
            return false;
    }
}

ActorId ResolveTarget(TargetKind kind, const Snapshot& s, bool* ok) {
    const auto yes = [&](ActorId id) {
        if (ok) *ok = true;
        return id;
    };
    const auto no = [&]() -> ActorId {
        if (ok) *ok = false;
        return 0;
    };

    switch (kind) {
        case TargetKind::Self:
            return yes(s.self);

        case TargetKind::Player:
            return yes(0x14);  // the player's FormID is always 0x14

        case TargetKind::CurrentTarget:
            return s.currentTarget ? yes(s.currentTarget) : no();

        case TargetKind::NearestEnemy: {
            const auto* e = NearestEnemy(s);
            return e ? yes(e->id) : no();
        }

        case TargetKind::LowestHealthEnemy: {
            const auto* e = LowestHealthEnemy(s);
            return e ? yes(e->id) : no();
        }

        case TargetKind::LowestHealthAlly: {
            const auto* a = LowestHealthAlly(s);
            return a ? yes(a->id) : no();
        }

        default:
            return no();
    }
}

bool HasResource(ActionKind action, const Snapshot& s) {
    switch (action) {
        case ActionKind::DrinkHealthPotion:
            return s.potions.healthCount > 0;
        case ActionKind::DrinkMagickaPotion:
            return s.potions.magickaCount > 0;
        case ActionKind::DrinkStaminaPotion:
            return s.potions.staminaCount > 0;
        default:
            return true;  // most actions cost nothing from inventory
    }
}

Decision Evaluate(const RuleSet& rs, const Snapshot& snap, EvalContext& ctx, Trace* trace) {
    if (ctx.ruleStates.size() != rs.rules.size()) ctx.ruleStates.resize(rs.rules.size());
    if (trace) trace->assign(rs.rules.size(), Verdict::NotReached);

    const bool globalReady = (snap.now - ctx.lastActionAt) >= ctx.globalCooldown;

    Decision decision;

    for (std::size_t i = 0; i < rs.rules.size(); ++i) {
        const Rule& r = rs.rules[i];
        const auto  put = [&](Verdict v) {
            if (trace) (*trace)[i] = v;
        };

        if (!r.enabled) {
            put(Verdict::Disabled);
            continue;
        }
        if (r.action == ActionKind::None || !ctx.caps.Supports(r.action)) {
            put(Verdict::Unsupported);
            continue;
        }
        if (!ConditionHolds(r, snap)) {
            put(Verdict::ConditionFalse);
            continue;
        }
        if (r.cooldown > 0.0 && (snap.now - ctx.ruleStates[i].lastFired) < r.cooldown) {
            put(Verdict::OnCooldown);
            continue;
        }
        if (!HasResource(r.action, snap)) {
            put(Verdict::NoResource);
            continue;
        }

        bool          targetOk = false;
        const ActorId target   = ResolveTarget(r.target, snap, &targetOk);
        if (!targetOk) {
            put(Verdict::NoTarget);
            continue;
        }

        // Condition-true rules are reported honestly even when the global
        // cooldown is what stopped them -- otherwise the debug column would
        // claim the condition was false, which is the wrong thing to go and
        // debug. Evaluation still stops here: a lower-priority rule must not
        // sneak past a higher-priority one that was merely rate-limited.
        if (!globalReady) {
            put(Verdict::GlobalCooldown);
            break;
        }

        put(Verdict::Fired);
        decision.ruleIndex = static_cast<int>(i);
        decision.action    = r.action;
        decision.targetId  = target;
        decision.actionArg = r.actionArg;

        ctx.ruleStates[i].lastFired = snap.now;
        ctx.lastActionAt            = snap.now;
        break;
    }

    return decision;
}

const char* ToString(Verdict v) noexcept {
    switch (v) {
        case Verdict::Fired:          return "fired";
        case Verdict::Disabled:       return "disabled";
        case Verdict::ConditionFalse: return "condition false";
        case Verdict::OnCooldown:     return "on cooldown";
        case Verdict::GlobalCooldown: return "global cooldown";
        case Verdict::NoTarget:       return "no target";
        case Verdict::NoResource:     return "no potion";
        case Verdict::Unsupported:    return "unsupported";
        case Verdict::NotReached:     return "not reached";
    }
    return "?";
}

}  // namespace ft
