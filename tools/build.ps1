<#
.SYNOPSIS
    Build FollowerTactics from any shell -- no "x64 Native Tools Command Prompt" needed.

.DESCRIPTION
    CMakePresets.json pins CMAKE_CXX_COMPILER=cl.exe, which only exists inside the
    Visual Studio developer environment. Rather than requiring a specific Start-menu
    shortcut (the most common way to get a confusing compiler-not-found error), this
    script locates the VS install, imports vcvars64.bat's environment into the current
    session, and then runs CMake.

.PARAMETER Preset
    core       core rule engine + tests only. No Skyrim, no vcpkg, no CommonLibSSE.
               This is the fast loop -- use it constantly.
    core-asan  same, built with AddressSanitizer.
    core-cov   same, built with clang-cl and instrumented for coverage; -Coverage
               runs the tests and reports which lines of src/core they reach.
    debug      full build: SKSE plugin + tests. The first run compiles CommonLibSSE-NG
               from source via vcpkg and is slow.
    release    same as debug, optimized.

.PARAMETER Analyze
    Compile our own targets under MSVC's static analyser (/analyze). A
    different engine from clang-tidy's, so it finds different things; the
    findings come out as C6xxx compiler warnings. Compiles several times
    slower, and the flag is cached, so this run and the next plain run each
    recompile our sources (not CommonLibSSE). Works with any preset.

.PARAMETER Coverage
    After building, run the tests once and report line coverage of src/core
    (the `coverage` target). Only the core-cov preset is instrumented, so
    only there does this do anything. The HTML report lands in
    build\core-cov\coverage\html\index.html.

.EXAMPLE
    .\tools\build.ps1 -Preset core -Test
    .\tools\build.ps1 -Preset debug
    .\tools\build.ps1 -Preset debug -Analyze
    .\tools\build.ps1 -Preset core-cov -Coverage
#>
[CmdletBinding()]
param(
    [ValidateSet('core', 'core-asan', 'core-cov', 'debug', 'release')]
    [string] $Preset = 'core',
    [switch] $Test,
    [switch] $Fresh,
    [switch] $Analyze,
    [switch] $Coverage
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Get-VcVarsPath {
    # Do NOT locate the toolset with `vswhere -latest -requires ...`. A VS install
    # that is perfectly functional but flagged isComplete=False -- which is what a
    # `--quiet --norestart` workload install leaves behind -- is invisible to
    # -latest and reports an empty package list, so the -requires filter finds
    # nothing while cl.exe is sitting right there on disk. Probe the filesystem
    # and let the presence of vcvars64.bat be the answer.
    $candidates = @()

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $candidates += & $vswhere -all -prerelease -products * -property installationPath
    }
    foreach ($root in ${env:ProgramFiles}, ${env:ProgramFiles(x86)}) {
        foreach ($ed in 'Enterprise', 'Professional', 'Community', 'BuildTools') {
            $candidates += (Join-Path $root "Microsoft Visual Studio\2022\$ed")
        }
    }

    foreach ($c in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $vcvars = Join-Path $c 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path $vcvars) { return $vcvars }
    }
    return $null
}

function Import-VcVars {
    $vcvars = Get-VcVarsPath
    if (-not $vcvars) {
        throw @'
No Visual Studio 2022 C++ toolset found (no vcvars64.bat anywhere).

Visual Studio is commonly installed WITHOUT the C++ workload -- that is the
default for a plain "Community" install, and it surfaces as a confusing
"cl.exe not found" rather than an honest "C++ is not installed".

Install it, with Visual Studio CLOSED (the installer refuses to modify an
in-use install and fails with exit code 8006, "VSProcessesRunning"):

  & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify `
      --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" `
      --add Microsoft.VisualStudio.Workload.NativeDesktop `
      --add Microsoft.VisualStudio.Component.VC.ASAN `
      --includeRecommended --quiet --norestart

Note the installPath must be passed as one already-quoted token; the installer
splits on spaces otherwise and reports "installPath: C:\Program".
'@
    }

    Write-Host "Importing developer environment from $vcvars" -ForegroundColor DarkGray

    # `set` after vcvars64 dumps the whole environment; replay it into this session.
    $output = & ${env:ComSpec} /s /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }
}

# vcvars64.bat sets VCPKG_ROOT to the vcpkg bundled inside Visual Studio, which
# is NOT the one this project uses -- the manifest wants a bootstrapped
# standalone vcpkg. Preserve whatever was set beforehand, or the build silently
# resolves the manifest against the wrong package tree.
$userVcpkgRoot = $env:VCPKG_ROOT

Import-VcVars

if ($userVcpkgRoot) {
    if ($env:VCPKG_ROOT -ne $userVcpkgRoot) {
        Write-Host "  restoring VCPKG_ROOT=$userVcpkgRoot (vcvars64 had reset it to $env:VCPKG_ROOT)" -ForegroundColor DarkGray
    }
    $env:VCPKG_ROOT = $userVcpkgRoot
}

foreach ($tool in 'cl', 'cmake', 'ninja') {
    $found = Get-Command $tool -ErrorAction SilentlyContinue
    if (-not $found) { throw "'$tool' is still not on PATH after importing vcvars64." }
    Write-Host ("  {0,-6} {1}" -f $tool, $found.Source) -ForegroundColor DarkGray
}

if ($Preset -in 'debug', 'release') {
    if (-not $env:VCPKG_ROOT) { throw "VCPKG_ROOT is not set; the '$Preset' preset needs vcpkg." }
    if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
        throw "vcpkg.exe missing. Run: & '$env:VCPKG_ROOT\bootstrap-vcpkg.bat'"
    }
}

$buildDir = Join-Path $repo "build\$Preset"
if ($Fresh -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Push-Location $repo
try {
    if ($Coverage -and $Preset -ne 'core-cov') { throw "-Coverage needs the core-cov preset (instrumented build); got '$Preset'." }
    Write-Host "`n== configure ($Preset) ==" -ForegroundColor Cyan
    $analyzeFlag = if ($Analyze) { 'ON' } else { 'OFF' }
    cmake --preset $Preset "-DFT_ANALYZE=$analyzeFlag"
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

    Write-Host "`n== build ($Preset) ==" -ForegroundColor Cyan
    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

    if ($Test) {
        Write-Host "`n== test ($Preset) ==" -ForegroundColor Cyan
        ctest --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
    }

    if ($Coverage) {
        Write-Host "`n== coverage ($Preset) ==" -ForegroundColor Cyan
        cmake --build --preset $Preset --target coverage
        if ($LASTEXITCODE -ne 0) { throw "coverage failed ($LASTEXITCODE)" }
    }
    Write-Host "`nOK" -ForegroundColor Green
} finally {
    Pop-Location
}
