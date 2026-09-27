#pragma once
// Script links: a power, a spell or a shout word whose script puts up another
// spell. Blood Sacrifice's power leaves nothing that lasts -- one script
// effect of no duration -- and the ability its script adds is what runs, a
// toggle. Vanilla Battle Fury's shout is the same shape, and its script
// casts a spell on each ally near the shouter. The link is a property of
// that script (dar_simpletogglescript's TriggerSpell0 names
// BLO_abBloodSacrifice; DLC2VoiceBattleFuryScript's DLC2VoiceElementalFury
// names the spell it casts), which the engine hands to the script engine at
// load and keeps nowhere readable, so it is read back from the plugin that
// last defines the effect (core/PluginFile.h): a spell, power or shout word
// with nothing lasting of its own whose script effect names an ability, or
// a spell that lasts and harms no one, puts that up. The Effect condition
// lists it by that spell's effect.

namespace RE
{
class SpellItem;
} // namespace RE

namespace ft::game
{

// Once, at data load: read the links from the plugins.
void FindToggles();

// The spell `source`'s script puts up; null for none. Built at load and
// read-only after: any thread.
[[nodiscard]] const RE::SpellItem *ScriptedSpell(const RE::SpellItem *source);

} // namespace ft::game
