#pragma once
// A follower's tactics as a file: the rules, the follower's own switch, the
// pins, and who it is for. Written as JSON and read back leniently, so a
// file from another version of the mod -- older or newer -- gives up only
// what this build cannot name, and keeps the rest.
//
// The reading rule, in one line: an unknown KEY is ignored, an unknown VALUE
// drops the thing that carries it. A rule whose subject, predicate, target,
// status, damage kind or hand this build has never heard of is dropped, with
// a warning that says so; an action whose kind is unknown, or whose form
// names a plugin that is not installed, is dropped alone and its rule kept.
// Nothing here refuses a file: the next write replaces it whole, so a
// dropped rule is gone for good the moment the player edits anything, and
// that is accepted -- a profile from the future should degrade, not explode.
//
// Pure, like everything in core: forms cross the wire through a codec the
// game supplies (docs/PROFILES.md says what the game writes), and the tests
// use one that passes ids through as hex.

#include "Rule.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

// The number at the top of every file. Bump it only for a change a reader of
// the previous version could not make sense of by ignoring what it does not
// know: a key renamed, a value's meaning changed. Adding a key, a subject, a
// predicate or an action is NOT that -- an older reader drops what it cannot
// name and keeps the rest, which is the whole design -- so the number is
// expected to stay at 1 for a long time.
inline constexpr int kProfileSchema = 1;

// How a form crosses the wire. Core has no idea what a FormID means, and a
// raw one is only meaningful in one load order anyway; the game encodes a
// form as the plugin that defines it plus its id within that plugin, and
// decodes by looking the plugin up. Decode fails, deliberately, for a plugin
// that is not installed: the action naming it is dropped.
struct FormCodec
{
    std::function<std::string(std::uint32_t)> encode;
    std::function<std::optional<std::uint32_t>(std::string_view)> decode;

    // Ids as hex, both ways: for the tests, and for a build with no game.
    [[nodiscard]] static FormCodec Hex();
};

struct PinEntry
{
    std::uint32_t form{0};
    Hand hands{Hand::None}; // None for armour and ammunition
};

struct Profile
{
    // Who the file is for, as a person reading it sees it. The game writes
    // both from the follower the file belongs to; on the way in they are
    // carried, not trusted -- the file's NAME says whose it is.
    std::string followerName;
    std::string followerForm;
    // The follower's own switch, beside the rules because it is theirs: off
    // silences the list without losing it, and that must survive a restart
    // as much as the list does.
    bool enabled{true};
    RuleSet rules;
    // The player's pins: what stays in which hand, or stays on. A form and
    // its hands are all the file needs; what the thing IS is read off its
    // record again on the way in. The game takes a saved pin back only if
    // the follower still has the thing and still has it on -- a pin is a
    // promise about what is worn, and a file cannot re-dress anyone -- so a
    // save made without the mod, or after the thing was lost, sold or
    // swapped, just forgets that pin. That is what makes the mod safe to
    // remove: nothing in the save, and nothing on disk that the game
    // cannot decline.
    std::vector<PinEntry> pins;
};

[[nodiscard]] std::string WriteProfile(const Profile &profile, const FormCodec &codec);

struct ReadResult
{
    // Absent when the text is not a profile at all: not JSON, or not an
    // object. Anything else reads, with whatever had to be dropped listed.
    std::optional<Profile> profile;
    // One line per thing dropped or doubted, worded for the log.
    std::vector<std::string> warnings;
};

[[nodiscard]] ReadResult ReadProfile(std::string_view json, const FormCodec &codec);

} // namespace ft
