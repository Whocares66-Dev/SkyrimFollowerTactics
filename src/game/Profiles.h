#pragma once
// A follower's tactics on disk. One file per follower, written whole after
// every edit and read the first time the tick sees them; docs/PROFILES.md
// is the format. This is the thin part: which file, and how a form is
// named in it. What the file holds is core's (core/Profile.h).

#include "core/Profile.h"

#include <optional>
#include <string>

namespace RE
{
class Actor;
}

namespace ft::game
{

// Which file a follower's tactics live in, and how the file names them.
//
// The key is the NPC's base record -- the plugin that defines them and
// their id within it -- not the placed reference: "Lydia's tactics" are
// Lydia's whichever save she is in, and a plugin's own id survives a load
// order change where a runtime FormID does not. A follower with no stable
// record (spawned at runtime, from a dynamic base) gets a key from their
// reference id, which is as good as it gets, and the log says so.
struct Identity
{
    std::string key;  // the file's stem, "Skyrim.esm-A2C94"
    std::string name; // display name, written into the file for the reader
    std::string form; // the base record on the wire, "0xA2C94~Skyrim.esm"
};

// Game thread: reads the actor's records.
[[nodiscard]] Identity IdentifyFollower(RE::Actor *actor);

// Read a follower's file, if there is one. Absent when there is none; a
// file that cannot be read or is not a profile is logged and treated as
// none. Whatever the file had to give up is logged too, one line each.
[[nodiscard]] std::optional<ft::Profile> LoadProfile(const Identity &who);

// Write the whole file, replacing what was there. A failure is logged and
// the last good file is left in place.
void SaveProfile(const Identity &who, const ft::Profile &profile);

// Forms on the wire: "0xA2C94~Skyrim.esm", the plugin's own id and the
// plugin, the form SPID and KID users already know. A form with no plugin
// -- one made at runtime -- goes as its bare id, which is only good in the
// save it came from. Decoding resolves the plugin through the data handler,
// and fails for one that is not loaded.
[[nodiscard]] const ft::FormCodec &GameFormCodec();

} // namespace ft::game
