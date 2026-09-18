#pragma once
// A follower's tactics in the save. One record per follower in the SKSE
// co-save (the .skse beside every .ess), written when the game saves and
// read when a save loads, so tactics roll back with the save, go with it
// when it is deleted, and are dropped by SKSE when the plugin is absent.
// dev/PROFILES.md is the whole of it. This is the thin part: who a
// record is for, how a form is named in it, and the SKSE callbacks. What
// a record holds is core's (core/Profile.h), as JSON text.

#include "core/Profile.h"

#include <optional>
#include <string>

namespace RE
{
class Actor;
}

namespace ft::game
{

// Which record a follower's tactics are, and how the record names them.
//
// The key is the NPC's base record -- the plugin that defines them and
// their id within it -- not the placed reference: "Lydia's tactics" are
// Lydia's whichever reference they are, and a plugin's own id survives a
// load order change where a runtime FormID does not. Two references of
// one base (a placeatme copy beside the original) share the key: the
// first the tick sees claims the saved record, the other starts empty,
// and both are then saved under the one key, the later write winning. A
// follower with no stable record (spawned at runtime, from a dynamic
// base) gets a key from their reference id, which is as good as it gets,
// and the log says so.
struct Identity
{
    std::string key;  // "Skyrim.esm-A2C94"
    std::string name; // display name, carried in the record for the log
    std::string form; // the base record on the wire, "0xA2C94~Skyrim.esm"
};

// Game thread: reads the actor's records.
[[nodiscard]] Identity IdentifyFollower(RE::Actor *actor);

// Once, at plugin load: register the save, load and revert callbacks with
// SKSE's serialization.
void InstallSerialization();

// The tactics the loaded save holds for this follower, if any, taken out
// of the loaded set: from here on the follower is live and is written
// from the session's state. A record that cannot be read is logged and
// treated as none. Whatever it had to give up is logged too, one line
// each. Game thread.
[[nodiscard]] std::optional<ft::Profile> ClaimSaved(const Identity &who);

// Forms on the wire: "0xA2C94~Skyrim.esm", the plugin's own id and the
// plugin, the form SPID and KID users already know. A form with no plugin
// -- one made at runtime -- goes as its bare id, which is only good in the
// save it came from, which is where it is. Decoding resolves the plugin
// through the data handler, and fails for one that is not loaded.
[[nodiscard]] const ft::FormCodec &GameFormCodec();

} // namespace ft::game
