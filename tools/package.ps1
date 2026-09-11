$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(\w+ VERSION ([\d.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "No project VERSION in CMakeLists.txt" }

& (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoDeploy

$dll = Join-Path $root 'build\release\FollowerTactics.dll'
if (-not (Test-Path $dll)) { throw "No $dll after the build" }

$stage = Join-Path $root 'build\release\package'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugins = Join-Path $stage 'SKSE\Plugins'
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item $dll $plugins
Copy-Item (Join-Path $root 'assets\FollowerTactics.ini') $plugins

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
