<#
.SYNOPSIS
    Deploy the console batch files from bat/ to everywhere Skyrim might look for them.

.DESCRIPTION
    Two things about `bat` are easy to get wrong, and both fail the same way -- the
    command appears to do nothing at all:

    1. LINE ENDINGS. These files are authored on a repo that stores LF. Bethesda's
       console parser is Windows-native; feeding it LF-only text is asking for
       trouble. Everything is written out as CRLF here regardless of what is in git.

    2. LOCATION. Sources disagree about where `bat <name>` looks: the game root, the
       game root without a .txt extension, or Data\ with one. Rather than litigate
       it, deploy to all three. They are a few KB of text and the ambiguity is not
       worth one more debugging session.

    The game folder is under C:\Program Files (x86), so this needs an elevated shell.

.EXAMPLE
    .\tools\deploy-tests.ps1
    .\tools\deploy-tests.ps1 -Check      # report drift without writing
#>
[CmdletBinding()]
param(
    [string] $GameFolder = "C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition",
    [switch] $Check
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo 'bat'

if (-not (Test-Path (Join-Path $GameFolder 'SkyrimSE.exe'))) {
    throw "$GameFolder does not look like the Skyrim root (no SkyrimSE.exe)."
}

$dataFolder = Join-Path $GameFolder 'Data'
if (-not (Test-Path $dataFolder)) { throw "No Data folder under $GameFolder" }

$files = Get-ChildItem (Join-Path $src '*.txt')
if (-not $files) { throw "No .txt files found in $src" }

function Write-Crlf([string]$Path, [string]$Text) {
    # Normalise to LF first so an already-CRLF source does not become CRCRLF.
    $normalised = ($Text -replace "`r`n", "`n") -replace "`n", "`r`n"
    [System.IO.File]::WriteAllText($Path, $normalised, [System.Text.UTF8Encoding]::new($false))
}

$written = 0
foreach ($f in $files) {
    $text = Get-Content $f.FullName -Raw
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)

    $destinations = @(
        (Join-Path $GameFolder $f.Name),   # root, with extension  (most common)
        (Join-Path $GameFolder $stem),     # root, no extension    (documented variant)
        (Join-Path $dataFolder $f.Name)    # Data\, with extension (documented variant)
    )

    foreach ($d in $destinations) {
        if ($Check) {
            $state = if (Test-Path $d) { 'present' } else { 'MISSING' }
            Show-Line ("  {0,-8} {1}" -f $state, $d) -Colour DarkGray
            continue
        }
        Write-Crlf -Path $d -Text $text
        $written++
    }
    if (-not $Check) { Show-Line ("  deployed  {0}  (3 locations, CRLF)" -f $f.Name) -Colour Green }
}

if (-not $Check) {
    Show-Line "`n$written files written." -Colour Cyan
    Show-Line "Click a follower in the console FIRST, then:" -Colour Cyan
    Show-Line "  bat ftmake   (repeat per follower)  ->  bat ftbear  ->  bat ftstatus" -Colour Cyan
    Show-Line "From a bat file only player.<cmd> and commands on a PRIOR selection work." -Colour DarkGray
    Show-Line "prid and <refid>.<cmd> were both tried and neither does." -Colour DarkGray
}
