#Requires -Version 5.1

<#
.SYNOPSIS
    Helper for downgrading Skyrim Special Edition from 1.7.x to 1.6.1170.

.DESCRIPTION
    Automates every part of the downgrade that CAN be automated: backup,
    installing downloaded depots, verification, and blocking Steam updates.

    It deliberately does NOT download the depots. Steam's `download_depot`
    only exists inside the Steam client's own console and cannot be driven
    from a script. Run `-Step depots` to get the exact lines to paste, or use
    DepotDownloader if you want that part scripted too (see that step's output).

    Nothing here ever deletes a file. Overwrites are backed up first.

.EXAMPLE
    .\Downgrade-Skyrim.ps1 -Step check
    .\Downgrade-Skyrim.ps1 -Step backup
    .\Downgrade-Skyrim.ps1 -Step depots
    .\Downgrade-Skyrim.ps1 -Step install -WhatIf
    .\Downgrade-Skyrim.ps1 -Step install
    .\Downgrade-Skyrim.ps1 -Step lock
#>

[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidateSet('check', 'backup', 'depots', 'install', 'lock', 'unlock', 'restore')]
    [string]$Step,

    [string]$GameRoot,
    [string]$SteamRoot,
    [string]$BackupDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# $IsWindows only exists in PowerShell 6+. Windows still ships 5.1 by default,
# where StrictMode would throw on referencing it.
$script:OnWindows = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $IsWindows
} else {
    $true  # 5.1 only ever runs on Windows
}

# --------------------------------------------------------------------------
# Facts. Sources are in dev/DOWNGRADE.md.
# --------------------------------------------------------------------------
$APP_ID    = 489830
$CK_APP_ID = 1946180

# An array, not an ordered hashtable: indexing an OrderedDictionary with an
# integer key is treated as a POSITIONAL index, so $DEPOTS[489831] would throw.
$DEPOTS = @(
    # Names are Valve's own, from SteamDB. File counts are what the Steam client
    # reported downloading these manifests on 2026-09-01 (correcting a community
    # source that claimed 16 files for 489831 -- it is 19). MB is the ON-DISK
    # size, measured; Steam's console prints the smaller COMPRESSED size, which
    # is not what this script measures.
    [pscustomobject]@{ Id = 489831; Manifest = '8442952117333549665'; Files = 19; MB = 7146; Desc = 'Skyrim Special Edition disk' }
    [pscustomobject]@{ Id = 489832; Manifest = '8042843504692938467'; Files = 26; MB = 8170; Desc = 'Skyrim Special Edition core' }
    [pscustomobject]@{ Id = 489833; Manifest = '1914580699073641964'; Files = 1;  MB = 36;   Desc = 'Skyrim Special Edition exe'  }
)

$TARGET_VERSION = '1.6.1170'

# The one objective check that the Data files actually reverted, not just the exe.
$INTERFACE_BSA          = 'Data/Skyrim - Interface.bsa'
$INTERFACE_SIZE_1_6_1170 = 105799354L

# Copied before any overwrite. ~2 GB.
$CRITICAL_FILES = @(
    'SkyrimSE.exe'
    'SkyrimSELauncher.exe'
    'Data/Skyrim.esm'
    'Data/Update.esm'
    'Data/Dawnguard.esm'
    'Data/HearthFires.esm'
    'Data/Dragonborn.esm'
    'Data/_ResourcePack.esl'
    'Data/Skyrim - Interface.bsa'
)

# --------------------------------------------------------------------------
# Output helpers
# --------------------------------------------------------------------------
function Write-Head($t) { Write-Host ''; Write-Host $t -ForegroundColor Cyan; Write-Host ('-' * $t.Length) -ForegroundColor DarkGray }
function Write-Ok($t)   { Write-Host "  OK    $t" -ForegroundColor Green }
function Write-Warn($t){ Write-Host "  WARN  $t" -ForegroundColor Yellow }
function Write-Bad($t)  { Write-Host "  FAIL  $t" -ForegroundColor Red }
function Write-Info($t) { Write-Host "        $t" -ForegroundColor Gray }

function Format-Size([long]$b) {
    if ($b -ge 1GB) { '{0:N2} GB' -f ($b / 1GB) }
    elseif ($b -ge 1MB) { '{0:N1} MB' -f ($b / 1MB) }
    else { '{0:N0} B' -f $b }
}

# --------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------
function Resolve-SteamRoot {
    if ($SteamRoot) { return $SteamRoot }

    # The registry is authoritative when it's there; the fixed paths are a fallback.
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = (Get-ItemProperty -Path $key -ErrorAction Stop).SteamPath
            if ($p -and (Test-Path $p)) { return ($p -replace '/', '\') }
        } catch { }
    }
    foreach ($p in "${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam", 'C:\Steam') {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

function Resolve-GameRoot {
    if ($GameRoot) {
        if (-not (Test-Path (Join-Path $GameRoot 'SkyrimSE.exe'))) {
            throw "No SkyrimSE.exe in '$GameRoot'."
        }
        return $GameRoot
    }

    $steam = Resolve-SteamRoot
    $candidates = @()
    if ($steam) {
        $candidates += (Join-Path $steam 'steamapps\common\Skyrim Special Edition')

        # Steam libraries can live on other drives; libraryfolders.vdf lists them.
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $lib = $m.Groups[1].Value -replace '\\\\', '\'
                $candidates += (Join-Path $lib 'steamapps\common\Skyrim Special Edition')
            }
        }
    }

    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'SkyrimSE.exe')) { return $c }
    }
    throw "Could not find Skyrim. Pass -GameRoot explicitly."
}

function Get-ExeVersion($path) {
    if (-not (Test-Path $path)) { return $null }
    try {
        $v = (Get-Item $path).VersionInfo.FileVersion
        if ($v) { return $v.Trim() }
    } catch { }

    # pwsh on non-Windows leaves VersionInfo empty, so read the PE resource.
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $text  = [System.Text.Encoding]::Unicode.GetString($bytes)
        $i = $text.IndexOf('FileVersion')
        if ($i -lt 0) { return $null }
        $tail = $text.Substring($i + 11, [Math]::Min(40, $text.Length - $i - 11)).Replace("`0", ' ').Trim()
        $m = [regex]::Match($tail, '^[\d.]+')
        if ($m.Success) { return $m.Value }
    } catch { }
    return $null
}

function Test-Admin {
    if (-not $script:OnWindows) { return $true }
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-NotRunning {
    if (-not $script:OnWindows) { return }
    foreach ($n in 'SkyrimSE', 'CreationKit') {
        if (Get-Process -Name $n -ErrorAction SilentlyContinue) {
            throw "$n is running. Close it first."
        }
    }
}

function Get-DefaultBackupDir($root) {
    if ($BackupDir) { return $BackupDir }
    Join-Path (Split-Path $root -Parent) 'Skyrim SE - pre-downgrade backup'
}

# --------------------------------------------------------------------------
# Steps
# --------------------------------------------------------------------------
function Invoke-Check($root) {
    Write-Head 'Install'
    Write-Info $root

    $exe = Get-ExeVersion (Join-Path $root 'SkyrimSE.exe')
    $ck  = Get-ExeVersion (Join-Path $root 'CreationKit.exe')

    Write-Info "SkyrimSE.exe     $exe"
    Write-Info "CreationKit.exe  $(if ($ck) { $ck } else { 'not installed' })"

    $downgraded = $exe -and $exe.StartsWith($TARGET_VERSION)
    if ($downgraded) { Write-Ok "Runtime is $TARGET_VERSION. SKSE 2.2.8 is the matching build." }
    else { Write-Warn "Not on $TARGET_VERSION yet." }

    Write-Head 'Data files'
    $bsa = Join-Path $root $INTERFACE_BSA
    if (Test-Path $bsa) {
        $size = (Get-Item $bsa).Length
        Write-Info "Skyrim - Interface.bsa  $size"
        if ($size -eq $INTERFACE_SIZE_1_6_1170) {
            Write-Ok "Matches 1.6.1170 exactly. The Data files reverted, not just the exe."
        } elseif ($downgraded) {
            Write-Bad "Exe says $TARGET_VERSION but this BSA does not match 1.6.1170 ($INTERFACE_SIZE_1_6_1170)."
            Write-Info "Only depot 489833 (the exe) landed. Re-copy depots 489831 and 489832."
        } else {
            Write-Info "Expected $INTERFACE_SIZE_1_6_1170 after a correct downgrade."
        }
    }

    Write-Head 'Steam update locks'
    $steam = Resolve-SteamRoot
    if (-not $steam) { Write-Warn 'Steam root not found; cannot check manifests.'; return }

    foreach ($pair in @(@{Id = $APP_ID; Name = 'Skyrim SE' }, @{Id = $CK_APP_ID; Name = 'Creation Kit' })) {
        $mf = Join-Path $steam "steamapps\appmanifest_$($pair.Id).acf"
        if (-not (Test-Path $mf)) { Write-Info "$($pair.Name): no manifest (not installed?)"; continue }
        if ((Get-Item $mf).IsReadOnly) { Write-Ok "$($pair.Name) manifest is read-only" }
        else { Write-Warn "$($pair.Name) manifest is WRITABLE - Steam can update it. Run -Step lock" }
    }
}

function Invoke-Depots($root) {
    $steam = Resolve-SteamRoot
    Write-Head 'Paste these into the Steam console, ONE AT A TIME'
    Write-Info 'Open it with:  Win+R  ->  steam://open/console'
    Write-Info 'Wait for each to report completion before pasting the next.'
    Write-Host ''
    foreach ($d in $DEPOTS) {
        Write-Host "  download_depot $APP_ID $($d.Id) $($d.Manifest)" -ForegroundColor White
        Write-Info "      $($d.Desc), $($d.Files) file(s)"
    }
    Write-Host ''
    Write-Info 'Roughly 15 GB total. They land in:'
    if ($steam) { Write-Info "  $steam\steamapps\content\app_$APP_ID\depot_<id>\" }
    Write-Host ''
    Write-Info 'Then run:  -Step install'
    Write-Host ''
    Write-Info 'Prefer it fully scripted? DepotDownloader takes the same three IDs'
    Write-Info 'non-interactively (it will prompt for your Steam login and 2FA):'
    Write-Info '  https://github.com/SteamRE/DepotDownloader'
    foreach ($d in $DEPOTS) {
        Write-Info "  DepotDownloader -app $APP_ID -depot $($d.Id) -manifest $($d.Manifest) -username <you>"
    }
}

function Invoke-Backup($root) {
    $dest = Get-DefaultBackupDir $root
    Write-Head 'Backup'
    Write-Info "-> $dest"

    $total = 0L
    $plan = @()
    foreach ($rel in $CRITICAL_FILES) {
        $src = Join-Path $root $rel
        if (-not (Test-Path $src)) { Write-Warn "missing, skipping: $rel"; continue }
        $len = (Get-Item $src).Length
        $total += $len
        $plan += [pscustomobject]@{ Rel = $rel; Src = $src; Size = $len }
    }
    Write-Info "$(@($plan).Count) files, $(Format-Size $total)"

    if (-not $PSCmdlet.ShouldProcess($dest, "Copy $(@($plan).Count) files")) { return }

    $manifest = @{}
    foreach ($item in $plan) {
        $target = Join-Path $dest $item.Rel
        $parent = Split-Path $target -Parent
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

        if ((Test-Path $target) -and (Get-Item $target).Length -eq $item.Size) {
            Write-Info "already backed up: $($item.Rel)"
        } else {
            Write-Info "copying $($item.Rel)  ($(Format-Size $item.Size))"
            Copy-Item -LiteralPath $item.Src -Destination $target -Force
        }
        $manifest[$item.Rel] = @{ size = $item.Size }
    }

    $meta = @{
        capturedUtc  = (Get-Date).ToUniversalTime().ToString('o')
        gameRoot     = $root
        exeVersion   = Get-ExeVersion (Join-Path $root 'SkyrimSE.exe')
        ckVersion    = Get-ExeVersion (Join-Path $root 'CreationKit.exe')
        files        = $manifest
    }
    $meta | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dest 'backup-manifest.json')
    Write-Ok "Backed up. Manifest: $(Join-Path $dest 'backup-manifest.json')"
}

function Invoke-Install($root) {
    Assert-NotRunning
    $steam = Resolve-SteamRoot
    if (-not $steam) { throw 'Could not locate Steam. Pass -SteamRoot.' }

    $content = Join-Path $steam "steamapps\content\app_$APP_ID"
    if (-not (Test-Path $content)) {
        throw "No downloaded depots at '$content'. Run -Step depots first."
    }

    Write-Head 'Downloaded depots'
    $found = @()
    foreach ($d in $DEPOTS) {
        $dir = Join-Path $content "depot_$($d.Id)"
        if (-not (Test-Path $dir)) { Write-Bad "depot_$($d.Id) missing ($($d.Desc))"; continue }
        $items  = @(Get-ChildItem $dir -Recurse -File)
        $count  = $items.Count
        $sizeMB = [math]::Round((($items | Measure-Object -Property Length -Sum).Sum) / 1MB)

        # Steam prints "Depot download complete" when it is done; that is the real
        # authority. These are sanity checks against an interrupted copy, not a
        # verdict on the download itself.
        if ($count -eq $d.Files) { Write-Ok "depot_$($d.Id) $($d.Desc): $count files, $sizeMB MB" }
        else { Write-Warn "depot_$($d.Id) $($d.Desc): $count files, expected $($d.Files) ($sizeMB MB)" }

        if ($d.MB -gt 0 -and [math]::Abs($sizeMB - $d.MB) -gt [math]::Max(50, $d.MB * 0.05)) {
            Write-Warn "  size is $sizeMB MB, expected around $($d.MB) MB - check the download finished"
        }
        $found += $dir
    }
    if (@($found).Count -ne @($DEPOTS).Count) {
        throw 'Not all three depots are present. Download the missing ones before installing.'
    }

    if (-not (Test-Admin)) {
        Write-Warn 'Not running as Administrator. Writing into Program Files will likely fail.'
    }

    # Back up anything we are about to overwrite, before overwriting it.
    Write-Head 'Backing up files that will be overwritten'
    Invoke-Backup $root

    Write-Head 'Installing'
    $n = 0
    $overwritten = @()
    foreach ($dir in $found) {
        foreach ($src in Get-ChildItem $dir -Recurse -File) {
            $rel    = $src.FullName.Substring($dir.Length).TrimStart('\', '/')
            $target = Join-Path $root $rel

            if ((Test-Path $target) -and (Get-Item $target).Length -eq $src.Length) {
                continue  # identical size: already this build
            }
            if ($PSCmdlet.ShouldProcess($target, 'Overwrite from depot')) {
                $parent = Split-Path $target -Parent
                if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
                if (Test-Path $target) { Set-ItemProperty -LiteralPath $target -Name IsReadOnly -Value $false }
                Copy-Item -LiteralPath $src.FullName -Destination $target -Force
            }
            Write-Info "$rel  ($(Format-Size $src.Length))"
            $overwritten += ($rel -replace '\\', '/')
            $n++
        }
    }
    if ($WhatIfPreference) { Write-Ok "$n file(s) WOULD be installed. Nothing was changed." }
    else                    { Write-Ok "$n file(s) installed." }

    $uncovered = @($overwritten | Where-Object { $_ -notin $CRITICAL_FILES })
    if ($uncovered.Count -gt 0) {
        Write-Host ''
        $tense = if ($WhatIfPreference) { 'would be overwritten and are' } else { 'overwritten file(s) are' }
        Write-Warn "$($uncovered.Count) file(s) $tense NOT in the backup set:"
        foreach ($u in $uncovered) { Write-Info "  $u" }
        Write-Info 'These are bulk asset archives. -Step restore will not bring them back;'
        Write-Info 'Steam > Verify integrity of game files restores them perfectly.'
    }

    if ($WhatIfPreference) {
        Write-Host ''
        Write-Info 'Dry run only. Re-run without -WhatIf to apply, then it will verify.'
        return
    }

    Write-Head 'Verifying'
    Invoke-Check $root
}

function Set-ManifestLock($locked) {
    $steam = Resolve-SteamRoot
    if (-not $steam) { throw 'Could not locate Steam. Pass -SteamRoot.' }
    $verb = if ($locked) { 'Locking' } else { 'Unlocking' }
    Write-Head "$verb Steam manifests"

    foreach ($pair in @(@{Id = $APP_ID; Name = 'Skyrim SE' }, @{Id = $CK_APP_ID; Name = 'Creation Kit' })) {
        $mf = Join-Path $steam "steamapps\appmanifest_$($pair.Id).acf"
        if (-not (Test-Path $mf)) { Write-Info "$($pair.Name): no manifest, skipping"; continue }
        if ($PSCmdlet.ShouldProcess($mf, "Set IsReadOnly=$locked")) {
            Set-ItemProperty -LiteralPath $mf -Name IsReadOnly -Value $locked
        }
        Write-Ok "$($pair.Name): read-only = $locked"
    }

    if ($locked) {
        Write-Host ''
        Write-Warn 'This is necessary but NOT sufficient. Also:'
        Write-Info '  - Steam > Properties > Updates > Automatic Updates >'
        Write-Info '      "Wait until I launch the game"  (there is no "never update" option)'
        Write-Info '  - Never press Play in the Steam Library. Launch via SKSE/MO2 --'
        Write-Info '    the setting above only DEFERS an update until you launch via Steam.'
        Write-Info '  - Never run "Verify integrity of game files" - it restores 1.7.104.'
        Write-Info '  - Re-run -Step check after Steam client updates. The read-only trick'
        Write-Info '    has been reported failing after a few days.'
    }
}

function Invoke-Restore($root) {
    Assert-NotRunning
    $dest = Get-DefaultBackupDir $root
    $manifestPath = Join-Path $dest 'backup-manifest.json'
    if (-not (Test-Path $manifestPath)) { throw "No backup manifest at '$manifestPath'." }

    $meta = Get-Content $manifestPath -Raw | ConvertFrom-Json
    Write-Head 'Restore'
    Write-Info "from $dest"
    Write-Info "captured $($meta.capturedUtc), exe was $($meta.exeVersion)"

    foreach ($prop in $meta.files.PSObject.Properties) {
        $rel = $prop.Name
        $src = Join-Path $dest $rel
        if (-not (Test-Path $src)) { Write-Warn "missing from backup: $rel"; continue }
        $target = Join-Path $root $rel
        if ($PSCmdlet.ShouldProcess($target, 'Restore from backup')) {
            if (Test-Path $target) { Set-ItemProperty -LiteralPath $target -Name IsReadOnly -Value $false }
            Copy-Item -LiteralPath $src -Destination $target -Force
        }
        Write-Info "restored $rel"
    }
    Write-Ok 'Restored. Note this only covers the critical file list, not every BSA.'
    Write-Info 'For a guaranteed clean revert: unlock manifests, then Steam > Verify integrity.'
}

# --------------------------------------------------------------------------
try {
    $root = Resolve-GameRoot
    switch ($Step) {
        'check'   { Invoke-Check   $root }
        'backup'  { Invoke-Backup  $root }
        'depots'  { Invoke-Depots  $root }
        'install' { Invoke-Install $root }
        'lock'    { Set-ManifestLock $true }
        'unlock'  { Set-ManifestLock $false }
        'restore' { Invoke-Restore $root }
    }
    Write-Host ''
} catch {
    Write-Host ''
    Write-Bad $_.Exception.Message
    exit 1
}
