#pragma once

// Required by add_commonlibsse_plugin. Only the SKSE plugin target uses this;
// ft_core and the test target are deliberately built without it.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// CommonLibSSE-NG's own PCH pulls in <spdlog/spdlog.h> but NOT the sinks, so a
// plugin that wants to log to a file has to include this itself.
#include <spdlog/sinks/basic_file_sink.h>

// CommonLibSSE-NG does NOT define a `logger` alias -- verified against the
// headers. Every tutorial uses `logger::info`, which is this line, not the library.
namespace logger = SKSE::log;

using namespace std::literals;
