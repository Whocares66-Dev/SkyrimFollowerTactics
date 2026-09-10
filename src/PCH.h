#pragma once

// Required by add_commonlibsse_plugin. Only the SKSE plugin target uses this;
// ft_core and the test target are deliberately built without it.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// CommonLibSSE-NG's own PCH pulls in <spdlog/spdlog.h> but NOT the sinks, so a
// plugin that wants to log to a file has to include this itself.
#include <spdlog/sinks/basic_file_sink.h>

// There is deliberately no `logger` alias here any more. CommonLibSSE-NG does
// not define one -- every tutorial's `logger::info` is a line like this one,
// not the library -- and while it existed, a new call site could log prose at
// a hardcoded level with a hand-typed module prefix and nobody would notice.
// game/Log.h is the way in now: ft::log::<module>.info(...), and .event(...)
// for anything docs/LOGGING.md gives an event name.

using namespace std::literals;
