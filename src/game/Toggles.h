#pragma once
// Toggles: a power or a spell whose script adds or takes away an ability.
// Blood Sacrifice's power leaves nothing that lasts -- one script effect of
// no duration -- and the ability its script adds is what runs. The link is
// a property of that script (dar_simpletogglescript's TriggerSpell0 names
// BLO_abBloodSacrifice), which the engine hands to the script engine at
// load and keeps nowhere readable, so it is read back from the plugin that
// last defines the effect (core/PluginFile.h): a spell or power with
// nothing lasting of its own whose script effect names an ability is that
// ability's toggle. The Effect condition lists it by the ability's effect.

namespace RE
{
class SpellItem;
} // namespace RE

namespace ft::game
{

// Once, at data load: read the links from the plugins.
void FindToggles();

// The ability `source` toggles; null for none. Built at load and read-only
// after: any thread.
[[nodiscard]] const RE::SpellItem *ToggledAbility(const RE::SpellItem *source);

} // namespace ft::game
