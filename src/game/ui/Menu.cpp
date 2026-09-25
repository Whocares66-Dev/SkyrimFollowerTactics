// The menu framework's entries -- Settings, the player, one per follower
// -- registered at load and as followers come and go.

#include "game/ui/Pages.h"
#include "game/ui/Panel.h"
#include "game/ui/UI.h"
#include "game/ui/Widgets.h"

#include "core/MenuSlots.h"
#include "game/Log.h"
#include "game/Settings.h"
#include "game/Tactics.h"
#include "game/Util.h"
#include "progression/game/Service.h"
#include <SKSEMenuFramework.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

// One entry per follower under "Follower Tactics", rather than everyone stacked
// inside a single page. Two SDK constraints shape how:
//
//  1. RenderFunction is `void(__stdcall*)()` with no user data, so an entry
//     cannot be told which follower it is for. Each needs its own function --
//     hence a fixed pool of slots, one static trampoline apiece. A slot is
//     held for the life of the process (2. below), so the pool counts every
//     follower seen in one launch, not the party of the moment.
//  2. Removing an entry needs DeleteSection, which the framework's source has
//     but no released build yet exports (3.14.1 is the newest; ours is
//     3.14.0). The SDK wrapper returns false when the export is missing, so
//     a dismissed follower's entry is deleted where it can be and otherwise
//     stays, saying so, until the framework catches up.
//
// Entries are registered the first time a follower is seen, so they carry real
// names. Those seen together are registered in name order; a later one comes
// after them, since an entry cannot be moved once added.
constexpr std::size_t kSlots = 64;

// Who has which slot, and when a newcomer is added: core's
// (core/MenuSlots.h, tested), with the clock handed in.
std::mutex g_slotMutex;
ft::MenuSlots g_slots{kSlots};

[[nodiscard]] ft::ActorId SlotOwner(std::size_t slot)
{
    std::scoped_lock lock(g_slotMutex);
    return g_slots.OwnerOf(slot);
}

void DrawSlot(std::size_t slot)
{
    const ft::ActorId id = SlotOwner(slot);
    if (id == 0)
    {
        Im::TextDisabled("%s", Tr("Nobody is assigned to this entry."));
        return;
    }

    if (const auto view = ObserveFollower(id))
    {
        DrawFollower(*view);
        return;
    }

    Im::TextDisabled("%s", Tr("Dismissed. This entry cannot be removed until the menu framework's next "
                              "release; it is reused if they come back."));
}

void DrawSettings()
{
    // The title, larger than the page's text. SetWindowFontScale sets the
    // window's scale outright rather than multiplying it, so the scale in
    // force is measured first -- the font size at it against the size at 1 --
    // and put back after, in case the framework sets one of its own.
    const float fontSize = Im::GetFontSize();
    Im::SetWindowFontScale(1.0f);
    const float baseSize = Im::GetFontSize();
    const float windowScale = baseSize > 0.0f ? fontSize / baseSize : 1.0f;
    Im::SetWindowFontScale(windowScale * 1.5f);
    Im::TextUnformatted(TrFormat("Follower Tactics ({})", FT_VERSION).c_str());
    Im::SetWindowFontScale(windowScale);

    Im::Separator();
    Im::Spacing();

    // One switch: the tick, then the words beside it. The hover is on the
    // switch, not the word, and says what a click does; it is read before
    // the click is answered, so the text matches the tick shown this frame.
    const auto toggle = [](const char *id, bool on, const char *label, const char *toTurnOn, const char *toTurnOff) {
        Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
        const bool clicked = GlyphButton(id, Im::GetFrameHeight(), Glyph::Tick, on);
        Im::PopStyleVar(1);
        if (Im::IsItemHovered(0))
            Tooltip(on ? toTurnOff : toTurnOn);
        Im::SameLine(0.0f, kCellPadX);
        Im::AlignTextToFramePadding();
        Im::Text("%s", label);
        return clicked;
    };

    // The same switch as each follower's on their Tactics tab, and read
    // live the same way. No heading over it: the page's title is above it
    // and it is the only switch there.
    const bool enabled = IsEnabled();
    if (toggle("enabledAll", enabled, Tr("Enable tactics for party"), Tr("Click to turn on tactics for the party"),
               Tr("Click to turn off tactics for the party")))
        SetEnabled(!enabled);

    Im::Spacing();
    // What a follower must have before a thing is offered at all
    // (game/Settings.h). Each is saved with the game.
    CentredHeading(Tr("Requirements"));
    ft::Settings settings = CurrentSettings();
    const ft::Settings was = settings;
    // Every one is followers' alone, and each says so: a combat style is a
    // thing only an NPC has, which not every player knows; the player's own
    // dual cast is paired by the engine behind its perk check
    // (game/PlayerCast.h); and the player has no blow to require a perk of.
    if (toggle("requireDualWieldStyle", settings.requireDualWieldStyle, Tr("Require dual wield combat style"),
               Tr("Click to require dual wield combat style for dual wielding (follower only)"),
               Tr("Click to not require dual wield combat style for dual wielding (follower only)")))
        settings.requireDualWieldStyle = !settings.requireDualWieldStyle;
    if (toggle("requireDualCastPerks", settings.requireDualCastPerks, Tr("Require Dual Casting perks"),
               Tr("Click to require the school's Dual Casting perk for dual casting (follower only)"),
               Tr("Click to not require the school's Dual Casting perk for dual casting (follower only)")))
        settings.requireDualCastPerks = !settings.requireDualCastPerks;
    if (toggle("requirePowerBashPerk", settings.requirePowerBashPerk, Tr("Require Power Bash perk"),
               Tr("Click to require the Power Bash perk for power bashing (follower only)"),
               Tr("Click to not require the Power Bash perk for power bashing (follower only)")))
        settings.requirePowerBashPerk = !settings.requirePowerBashPerk;

    // How a follower's combat AI chooses, over the engine's own
    // (dev/COMBAT_AI.md "What we change"); the player's choices are theirs.
    Im::Spacing();
    CentredHeading(Tr("Combat AI"));
    if (toggle("variedAiChoices", settings.variedAiChoices, Tr("Varied AI choices"),
               Tr("Click for more variation in weapon use, spellcasting, etc. (follower only)"),
               Tr("Click for less variation in weapon use, spellcasting, etc. (follower only)")))
        settings.variedAiChoices = !settings.variedAiChoices;
    if (toggle("selfDamageSpells", settings.selfDamageSpells, Tr("Use self-targeting damage spells"),
               Tr("Click to have AI use self-targeting damage spells like Firestorm (follower only)"),
               Tr("Click to have AI not use self-targeting damage spells like Firestorm (follower only)")))
        settings.selfDamageSpells = !settings.selfDamageSpells;
    if (settings.requireDualWieldStyle != was.requireDualWieldStyle ||
        settings.requireDualCastPerks != was.requireDualCastPerks ||
        settings.requirePowerBashPerk != was.requirePowerBashPerk || settings.variedAiChoices != was.variedAiChoices ||
        settings.selfDamageSpells != was.selfDamageSpells)
        SetSettings(settings);

    // Progression's switch, kept with the save. Off, every follower is as
    // their record has them at once -- skills, attributes, perks and
    // spells are views in front of the engine, never written -- and their
    // pages show what they learned without changing it; what the engine
    // keeps on an actor (a bought perk's ability) goes as each is near. On
    // puts it all back (progression/game/Service.h).
    const fp::game::LevellingState progression = fp::game::Levelling();
    if (progression.inGame)
    {
        Im::Spacing();
        CentredHeading(Tr("Progression"));
        if (toggle("progression", progression.on, Tr("Manage follower progression"),
                   Tr("Click to enable follower leveling, skills, perks, and spell learning"),
                   Tr("Click to disable follower leveling, skills, perks, and spell learning")))
            fp::game::SetLevelling(!progression.on);
        if (!progression.on && !progression.stillHeld.empty())
        {
            std::string names;
            for (const std::string &name : progression.stillHeld)
                names += (names.empty() ? "" : Tr(", ")) + name;
            Im::TextDisabled("%s", TrFormat("Back to their records when next near: {}", names).c_str());
        }
    }
}

void __stdcall RenderSettings()
{
    DrawSettings();
}

// The player's page: the sheet's tabs and their tactics, in a tab bar of
// its own, on the tab carried from the last page drawn. No Combat Style:
// the player runs no combat AI to be tuned.
void __stdcall RenderPlayer()
{
    // Nothing until the first frame's refresh has built the page: a frame.
    const auto view = ObservePlayer();
    // Said here as well as in TabBody, because the two would otherwise wait
    // on each other: a page is built because the panel says it is on screen,
    // the tabs are what say so, and the tabs are not drawn until there is a
    // page. A follower's list seeds their view every tick, so only the
    // player's page ever sat at that standstill -- blank, for good.
    if (auto *player = RE::PlayerCharacter::GetSingleton(); player && !view)
    {
        // The player's page has no Combat Style tab, as CarriedTab knows: it
        // carries over to Tactics.
        const bool sheet = g_shownTab != Tab::None && g_shownTab != Tab::CombatStyle;
        ShowingPage(player->GetFormID(), sheet ? g_shownTab : Tab::Tactics);
    }
    if (!view || !Im::BeginTabBar("player##tabs"))
        return;
    const Tab select = PageTab(*view);
    DrawSheetTabs(*view, select);
    DrawTacticsTabs(*view, select);
    Im::EndTabBar();
}

// One trampoline per slot: a render callback takes no argument, so the
// slot's index is the template's, and the table of them is made from the
// count.
template <std::size_t N> void __stdcall RenderSlot()
{
    DrawSlot(N);
}

// Where a follower's entry goes. The mod's name is its name in any
// language; the subsection's is translated, before the first entry is
// added (game/I18n.h).
std::string FollowersPath()
{
    return std::string("Follower Tactics/") + Tr("Followers") + "/";
}

template <std::size_t... N>
constexpr std::array<SKSEMenuFramework::Model::RenderFunction, sizeof...(N)> Renderers(std::index_sequence<N...>)
{
    return {RenderSlot<N>...};
}

} // namespace

void SyncFollowers()
{
    if (!SKSEMenuFramework::IsInstalled())
        return;

    static const auto renderers = Renderers(std::make_index_sequence<kSlots>{});

    const auto followers = ObserveFollowers();
    std::vector<ft::ActorId> ids;
    std::vector<ft::MenuSlot> present;
    for (const auto &view : followers)
    {
        ids.push_back(view->id);
        present.push_back({view->id, view->name});
    }

    // Dismissed: delete the entry where the framework allows it, and free
    // the slot. Where it does not, the slot stays theirs, so the entry still
    // reads as their page if they are recruited again.
    // New: the first free slot, in name order, so the followers who appear
    // together list alphabetically. The frameworks in the field export
    // only AddSectionItem and AddWindow (dumpbin, 2026-09-11): an entry,
    // once added, can be neither removed nor moved, so one recruited later
    // goes after them. A load reveals the party over a few ticks -- Serana
    // a tick after the other three, and at the end of the list -- so the
    // newcomers are held until nobody new has appeared for a moment, and
    // added as one batch. On the steady clock: this is pacing, not play.
    std::vector<ft::MenuSlots::Placed> placed;
    {
        std::scoped_lock lock(g_slotMutex);
        for (const std::size_t slot : g_slots.Gone(ids))
        {
            const std::string name = g_slots.NameOf(slot);
            if (SKSEMenuFramework::DeleteSection(FollowersPath() + name))
            {
                log::ui.debug("menu entry removed for {}", name);
                g_slots.Free(slot);
            }
        }
        placed = g_slots.Arrivals(present, NowSeconds(), 2.0);
    }
    for (const auto &[index, who] : placed)
    {
        if (index == kSlots)
        {
            // Said once, since the panel would otherwise just lack a
            // name.
            static std::unordered_set<ft::ActorId> said;
            if (said.insert(who.id).second)
                log::ui.warn("no menu entry for {}: all {} are taken", who.name, kSlots);
            continue;
        }
        // Under a Followers subsection, apart from Settings: the path's
        // components are the tree.
        SKSEMenuFramework::FullPathAddSectionItem(FollowersPath() + who.name, renderers[index]);
        log::ui.debug("menu entry added for {} (slot {})", who.name, index);
    }
}

void Install()
{
    // Soft dependency, and the reason this whole file is safe to ship: without
    // the framework installed there is simply no menu, and the mod carries on
    // drinking potions.
    if (!SKSEMenuFramework::IsInstalled())
    {
        log::ui.info("SKSE Menu Framework not installed -- no in-game panel. "
                     "Tactics still run; see this log for what they decide.");
        return;
    }

    // Registered for the life of the game: the event unregisters when freed.
    static const auto *menuEvents = SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
    (void)menuEvents;
    KeepEscapeFromClosingTheMenu();

    SKSEMenuFramework::SetSection("Follower Tactics");
    SKSEMenuFramework::AddSectionItem(Tr("Settings"), RenderSettings);
    // Before the Followers subsection the tick fills as followers are
    // recruited: an entry cannot be moved once added.
    SKSEMenuFramework::AddSectionItem(Tr("Player"), RenderPlayer);

    log::ui.info("registered with SKSE Menu Framework (F1). Follower entries appear as followers do.");
}

} // namespace ft::game::ui
