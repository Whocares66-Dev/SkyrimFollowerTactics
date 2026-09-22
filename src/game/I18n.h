#pragma once
// The panel's catalog, chosen and read once (core/I18n.h, dev/I18N.md):
// the ini's `[Interface] language`, else the game's own sLanguage, names a
// file in Data/SKSE/Plugins/FollowerTactics/Translations. English needs
// none. At data loaded, before the panel registers its entries, whose
// names are translated when added and cannot be renamed after.

namespace ft::game
{

void LoadLanguage();

} // namespace ft::game
