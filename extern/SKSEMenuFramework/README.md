# SKSE Menu Framework SDK

The in-game panel (`src/game/UI.cpp`) is built against `SKSEMenuFramework.h`, which is
**not committed here**. It is 520 KB of third-party code carrying no licence notice in the
file, so this repo references it rather than redistributing it.

## Getting it

Download **"SKSEMenuFramework Header"** from the Files tab of
[SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) and
extract `SKSEMenuFramework.h` into this directory.

Last built against **3.11** (the header) with the mod at **3.14.1**.

## What it is, and what it is not

A single self-contained header. It declares the whole ImGui API under namespace
**`ImGuiMCP`** and forwards every call into `SKSEMenuFramework.dll` through
`GetProcAddress`. It includes only `<windows.h>`, `<codecvt>`, `<locale>` and `<string>`.

So there is **nothing to link**: no vcpkg `imgui`, no extra library. The mod page's
"import both imgui and this mod into your plugin" reads as though ImGui were a build
dependency; it is not.

## Runtime

The header's own `IsInstalled()` checks for `Data/SKSE/Plugins/SKSEMenuFramework.dll`, so
the framework is a **soft** dependency: without it there is no panel, `ui::Install()`
returns early with a log line, and the mod carries on running tactics. Building does not
require the mod to be installed -- only this header.
