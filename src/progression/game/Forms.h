#pragma once
// Forms to the core's FormKeys and back (progression/core/Ids.h): the defining plugin's
// name and the local id, which survive a load order change. A form made at
// run time (an FF id) has no plugin and no key: nothing of ours is filed
// under one.

#include "progression/core/Ids.h"

#include <optional>
#include <string>

namespace fp::game
{

[[nodiscard]] std::optional<FormKey> KeyOf(const RE::TESForm *form);

[[nodiscard]] RE::TESForm *Lookup(const FormKey &key);

template <class T> [[nodiscard]] T *Lookup(const FormKey &key)
{
    RE::TESForm *form = Lookup(key);
    return form ? form->As<T>() : nullptr;
}

// A form's name, or `fallback` when it has none.
[[nodiscard]] std::string NameOf(const RE::TESForm *form, std::string_view fallback = "?");
// An actor by the name the game shows. A placed reference is not a named
// form itself, so the overload above gave every companion "?" (first seen in
// play, 2026-09-21, on Jenassa); this is what Follower Tactics reads.
[[nodiscard]] std::string NameOf(RE::Actor *actor, std::string_view fallback = "?");

} // namespace fp::game
