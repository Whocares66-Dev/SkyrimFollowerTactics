#pragma once
// Each tab's body, drawn by the page it is on (Pages.cpp), and what one
// tab needs of another.

#include "core/Rule.h"
#include "core/Snapshot.h"
#include "game/Tactics.h"
#include "game/ui/Panel.h"
#include "game/ui/UI.h"
#include <cstdint>

namespace ft::game::ui
{

// The Inventory tab. Either the list or one item, never both: the detail
// takes the item's place rather than opening beside it, because the panel is
// not wide enough for two columns of text at this font size, and a back
// arrow is a gesture everyone already knows.
void DrawInventory(const CharacterView &view);

// Which of the two a form's page is on -- Shouts for a power or a shout,
// Magic for a spell -- and None for a form the follower does not have.
// Every link into either tab asks this, so the split is stated once.
Tab MagicPageOf(const CharacterView &view, std::uint32_t form);

// And that page's state.
MagicTabState &MagicPageState(ft::ActorId id, Tab page);

// The Summons tab's chips: a summoned creature, a raised corpse.
inline constexpr unsigned kIconSummoned = 0xF6D5; // dragon
inline constexpr unsigned kIconRaised = 0xF54C;   // skull

// The page a link to a form opens on the Inventory tab: of the form's
// rows, the worn one, else the first; an enchanted piece is a row of its
// own, so the form alone (the plain stack's key) would open nothing.
std::uint64_t ItemPageOf(const CharacterView &view, std::uint32_t form);

void DrawEffects(const CharacterView &view);

void DrawMagic(const CharacterView &view);

void DrawShouts(const CharacterView &view);

// The Summons tab: what they command right now. A chip per summon above the
// page, as the Inventory tab has categories -- always, one summon included,
// since the chip is where its name is.
void DrawSummons(const CharacterView &view);

// The character sheet: what they are, as opposed to what they have been told to
// do. Everything here is display only and already on the view, so it costs
// the game thread nothing extra to show.
void DrawCharacter(const CharacterView &view);

// The rule list and its switch.
void DrawTactics(const ft::RuleSet &rules, const FollowerView &view);

void DrawSkills(const CharacterView &view);

} // namespace ft::game::ui
