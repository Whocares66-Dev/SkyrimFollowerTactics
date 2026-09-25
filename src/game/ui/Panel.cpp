// The panel's memory (Panel.h).

#include "game/ui/Panel.h"
#include "game/ui/UI.h"

#include "core/OpenRows.h"
#include <initializer_list>
#include <unordered_map>
#include <utility>

namespace ft::game::ui
{
namespace
{

std::unordered_map<ft::ActorId, PanelState> g_panels;

} // namespace

ft::OpenRows g_openRows;

ListView g_inventoryList;
ListView g_magicList;
ListView g_shoutList;

char g_effectsFilter[kFilterLen]{};
char g_perksFilter[kFilterLen]{};

bool g_showHiddenEffects = false;

void ClearFilters()
{
    for (char *filter :
         {g_inventoryList.filter, g_magicList.filter, g_shoutList.filter, g_effectsFilter, g_perksFilter})
        filter[0] = '\0';
}

bool g_focusFilter = false;
bool g_clearFilter = false;
std::atomic<bool> g_filterDrawn{false};
std::atomic<bool> g_filterActive{false};
std::atomic<bool> g_leaveFilter{false};
std::atomic<bool> g_escapeTaken{false};

KeyRow g_keyRow = KeyRow::Tabs;
bool g_chipsDrawn = false;
int g_chipStep = 0;

PanelState &Panel(ft::ActorId id)
{
    return g_panels[id];
}

Tab g_shownTab = Tab::None;
ft::ActorId g_shownPage = 0;

void CloseDetails()
{
    for (auto &entry : g_panels)
    {
        PanelState &panel = entry.second;
        const bool read = entry.first == g_shownPage;
        CloseDetail(panel, panel.inventory, Tab::Inventory, read && g_shownTab == Tab::Inventory);
        CloseDetail(panel, panel.magic, Tab::Magic, read && g_shownTab == Tab::Magic);
        CloseDetail(panel, panel.shouts, Tab::Shouts, read && g_shownTab == Tab::Shouts);
        panel.effects = {};
        panel.skills = {};
        panel.attributeControls = false;
    }
}

std::atomic<bool> g_resetPages{false};

} // namespace ft::game::ui
