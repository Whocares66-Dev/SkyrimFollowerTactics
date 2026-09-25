#pragma once
// The rule editor's condition, target and action cells: their text, why a
// rule is set aside, and their menus (Conditions.cpp, Actions.cpp).

#include "core/Evaluator.h"
#include "core/I18n.h"
#include "core/Rule.h"
#include "game/Tactics.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ft::game::ui
{

inline constexpr const char *kFollowerAway = N_("Follower not available");

std::string ConditionText(const ft::Rule &r, const FollowerView &view);

// The other followers, by name, for the two cascades' headings.
std::vector<FollowerView::Peer> SortedPeers(const FollowerView &view);

bool ConditionCascade(const char *id, ft::Rule &rule, const FollowerView &view, ft::Moment moment,
                      const std::string &setAside);

// The name a form has in the load order, for an action naming a thing the
// follower no longer has: the rule keeps the name of what it asked for.
std::string FormName(std::uint32_t form);

bool ConditionAvailable(const ft::Rule &rule, const FollowerView &view);

// The probe's verdict for one action of one rule, as the page was built;
// available where the page has none for it.
ft::Verdict VerdictAt(const FollowerView &view, std::size_t rule, std::size_t action);

// Why a rule is set aside, for its switch; empty when it is not. A rule
// whose things are missing or whose follower is away (core/Editor.h), and
// one none of whose actions could be done this moment (UnavailableText):
// its one action's reason, or a word for several.
std::string SetAsideReason(const ft::Rule &rule, const FollowerView &view, std::size_t ruleIndex);

std::string ActionText(const ft::Action &act, const FollowerView &view);

// The Then cascade: whom first, then what -- the mirror of the If cascade's
// subject, then predicate. The headings are the same cast, less those the
// condition cannot supply: "Ally" on this side means the ally the condition
// matched, so it is offered only when the condition is about one.
bool ActionMenu(const char *id, ft::Action &act, const FollowerView &view, ft::Moment moment, bool *addAnother,
                ft::Rule &rule, const std::string &setAside, ft::Verdict verdict = ft::Verdict::Fired);

} // namespace ft::game::ui
