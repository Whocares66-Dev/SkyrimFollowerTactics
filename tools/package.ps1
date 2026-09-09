<#
.SYNOPSIS
    Build the Release plugin and zip it as an installable mod.

.DESCRIPTION
    A releasable FollowerTactics is one file: SKSE\Plugins\FollowerTactics.dll.
    There is no plugin file (the forms are made in memory at load, docs/MAGIC.md
    "Forms at runtime"), no scripts, no assets. The zip is laid out as a mod root,
    so Mod Organizer installs it from the archive as it is, and a manual install
    is "extract into Data".

    Builds with the release preset through tools\build.ps1 -NoDeploy (the dev
    deploy is the DEBUG build's; a release build must not overwrite it), then
    writes out\follower-tactics-<version>.zip, the version being the CMake
    project's. Install it in Mod Organizer from the archive; a new mod appears
    unticked, so tick it.

    Requirements at run time, none of which are in the zip: SKSE, Address
    Library for SKSE Plugins, SKSE Menu Framework (with its ImGui Icons).

.EXAMPLE
    .\tools\package.ps1
#>

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(\w+ VERSION ([\d.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "No project VERSION in CMakeLists.txt" }

& (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoDeploy

$dll = Join-Path $root 'build\release\FollowerTactics.dll'
if (-not (Test-Path $dll)) { throw "No $dll after the build" }

# The mod root, staged: SKSE\Plugins\FollowerTactics.dll and a short README.
$stage = Join-Path $root 'build\release\package'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugins = Join-Path $stage 'SKSE\Plugins'
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item $dll $plugins

$readme = @"
FollowerTactics $version

Dragon Age-style tactics for followers: an ordered list of IF <subject>: <condition>
THEN <target>: <action> rules per follower, edited in game through the SKSE Menu
Framework panel. One file, no plugin, nothing left in the save: uninstall by
removing it.

Requires: SKSE, Address Library for SKSE Plugins, SKSE Menu Framework.
Built for Skyrim SE/AE 1.6.1170 (Address Library resolves other 1.6 runtimes).

Install as a mod (this zip is a mod root), or extract into Data.
"@
Set-Content -Path (Join-Path $stage 'FollowerTactics.txt') -Value $readme -Encoding UTF8

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "follower-tactics-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "wrote $zip" -ForegroundColor Green
Write-Host ("  " + ((Get-Item $dll).Length / 1KB).ToString('0') + " KB dll, release, version $version")
