#pragma once
// What an actor is right now, for the conditions: statuses, kind of
// being, what the hands hold, resistances and armour (dev/CONDITIONS.md).

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class BGSAttackData;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// What an actor is right now, read off the actor: its statuses, its kinds,
// its hands, its resistances and armour, what hit it lately. Every actor in
// a snapshot, the follower and each ally and enemy, each tick.
[[nodiscard]] ft::ActorTraits ReadTraits(RE::Actor *actor);

// A load or a new game: the statuses last read of every actor forgotten, so
// the debug line that says when an actor's statuses change starts afresh --
// a created reference's FormID names another actor in another save.
void ForgetStatuses();
} // namespace ft::game
