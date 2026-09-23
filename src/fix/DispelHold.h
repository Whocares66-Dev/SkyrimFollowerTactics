#pragma once
// A spell held back while it would put out one of the caster's own running
// effects.
//
// An effect flagged "dispel with keywords" ends every running effect that
// shares one of its keywords as it lands -- two cloaks, both MagicCloak,
// one up at a time. The combat AI's gate asks only whether THIS spell is
// running (45344, dev/COMBAT_AI.md 4), so an actor with two such spells
// casts each in turn, each putting the other out: Serana's Cold Flame Cloak
// and Blood Aura, about ten casts in 90 s (2026-09-22). A modlist that hands
// an enemy two cloaks thrashes the same way. So the score of such a spell is
// answered 0 while an effect it would dispel runs, for every actor, and
// whichever is up stays up until it ends.

namespace RE
{
class Actor;
class CombatInventoryItem;
} // namespace RE

namespace ft::fix
{

// The engine's score for one entry, or 0 while casting it would dispel one
// of the actor's own running effects. Any thread: the AI scores on its own.
[[nodiscard]] float HoldBackDispellers(RE::CombatInventoryItem *entry, RE::Actor *actor, float engine);

// A load or a new game: the log's once-each memory forgotten.
void ResetDispelHold();

} // namespace ft::fix
