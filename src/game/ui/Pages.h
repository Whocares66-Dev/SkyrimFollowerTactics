#pragma once
// A follower's page and the player's: the tab bar, which page is on
// screen, and the Escape key while a filter box has it.

#include "core/Snapshot.h"
#include "game/Tactics.h"
#include "game/ui/UI.h"
#include <SKSEMenuFramework.h>

namespace ft::game::ui
{

// This page is on screen: the framework calls only the visible section's
// renderer, and a tab's body only while that tab is selected. A page that has
// just come up is built at once: another tab or character, or the panel
// opened, or a section of ours shown again after another.
void ShowingPage(ft::ActorId actor, Tab tab);

// No page is on screen once the panel opens or closes, or after a frame that
// drew none (Settings, or another mod's section): so the next page drawn
// counts as come up, and is built.
void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType type);

void KeepEscapeFromClosingTheMenu();

// The tab a page is to show this frame: what a link on a sheet, a back
// arrow or A and D have asked for, consumed here so it acts for one frame
// only; else the tab carried from the last page. Reading the movement keys
// is part of the question, since they are one of the three things that ask.
Tab PageTab(const CharacterView &view);

// The sheet's tabs, inside the caller's tab bar, reading left to right as
// who they are, what they carry, what they can cast, what they can shout,
// what they command, what is running on them and what they can do: a
// follower's page and the player's alike. `select` is the tab the page is to
// show, resolved by the caller: every tab of the bar is asked the same
// question, so the resolving is done once, above all three of the calls that
// draw one.
void DrawSheetTabs(const CharacterView &view, Tab select);

// The two lists, a tab each, on a follower's page and the player's alike:
// what they have been told to do in a fight, and out of one. The rules are
// read live rather than off the view: the view is rebuilt on a page
// change, and an edit must show on the next frame.
void DrawTacticsTabs(const FollowerView &view, Tab select);

// One page per follower: the sheet's tabs, then how their combat AI is
// tuned and what they have been told to do. It opens on the tab CarriedTab
// gives it.
void DrawFollower(const FollowerView &view);

} // namespace ft::game::ui
