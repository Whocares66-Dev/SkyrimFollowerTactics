<#
    Build the release plugin and write the two mod zips to dist\.

    Both come from ONE release build, so the DLL in them is byte for byte the
    same file. What differs is the bundled ini's log level and the name:

      follower-tactics-<version>.zip        level = info   -- what a player installs,
                                                              and the only one released
      follower-tactics-<version>-test.zip   level = debug  -- ours, for testing here;
                                                              it stays in dist\

    The level is a RUNTIME setting, which is why one binary serves both: an
    MSVC debug build is not a thing to hand anyone, since it links the debug
    CRT that no player has. The ini in assets\ is the player's, and the test
    zip is that same file with its level line rewritten, so the two cannot
    drift apart. The log's banner names the level it read, so an installed
    copy says which zip it came from.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$root = Split-Path -Parent $PSScriptRoot

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(\w+ VERSION ([\d.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "No project VERSION in CMakeLists.txt" }

# The .pdb stays in build\release, out of both zips: 27 MB against a 1 MB
# DLL, and a crash log's offsets are read against it here.
& (Join-Path $PSScriptRoot 'build.ps1') -Preset release

$dll = Join-Path $root 'build\release\FollowerTactics.dll'
if (-not (Test-Path $dll)) { throw "No $dll after the build" }

$ini = Get-Content (Join-Path $root 'assets\FollowerTactics.ini') -Raw
if ($ini -notmatch '(?m)^level\s*=\s*info\s*$') {
    throw "assets\FollowerTactics.ini does not set 'level = info' -- the player's zip must not ship a debug log"
}

# The zip's readme is the repository's, the same file: what it says about
# runtimes, requirements and installing is written once, there.
$readme = Join-Path $root 'README.md'

$testNote = @"
This is the TEST build of FollowerTactics ${version}: the same DLL as
follower-tactics-$version.zip, with the log level set to debug.
FollowerTactics.log then carries every per-tick readout, which is what a bug
report wants and is far larger than a player needs. Set level = info in
SKSE\Plugins\FollowerTactics.ini to quieten it.
"@

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null

$written = @()
foreach ($flavour in @(
        @{ Suffix = '';      Level = 'info';  Notes = $null },
        @{ Suffix = '-test'; Level = 'debug'; Notes = $testNote })) {

    $stage = Join-Path $root "build\release\package$($flavour.Suffix)"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    $plugins = Join-Path $stage 'SKSE\Plugins'
    New-Item -ItemType Directory -Force $plugins | Out-Null
    Copy-Item $dll $plugins
    Set-Content -Path (Join-Path $plugins 'FollowerTactics.ini') -Encoding UTF8 `
        -Value ($ini -replace '(?m)^level\s*=\s*info\s*$', "level = $($flavour.Level)")
    Copy-Item $readme $stage
    if ($flavour.Notes) { Set-Content -Path (Join-Path $stage 'TEST-BUILD.txt') -Value $flavour.Notes -Encoding UTF8 }

    $zip = Join-Path $dist "follower-tactics-$version$($flavour.Suffix).zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    Show-Line "wrote $zip  (log level $($flavour.Level))" -Colour Green
    $written += $zip
}

Show-Line ("  " + ((Get-Item $dll).Length / 1KB).ToString('0') + " KB dll, release, version $version")
Show-Line ("  " + $written.Count + " zips in dist\")
