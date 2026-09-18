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
. (Join-Path $PSScriptRoot 'console.ps1')

# The overrides, taken at script scope; the functions below read these.
$script:GameRootOverride = $GameRoot
$script:SteamRootOverride = $SteamRoot
$script:BackupDirOverride = $BackupDir

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

function Format-Size([long]$b) {
    if ($b -ge 1GB) { '{0:N2} GB' -f ($b / 1GB) }
    elseif ($b -ge 1MB) { '{0:N1} MB' -f ($b / 1MB) }
    else { '{0:N0} B' -f $b }
}

# --------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------
function Resolve-SteamRoot {
    if ($script:SteamRootOverride) { return $script:SteamRootOverride }

    # The registry is authoritative when it's there; the fixed paths are a fallback.
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $item = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($item -and $item.SteamPath -and (Test-Path $item.SteamPath)) { return ($item.SteamPath -replace '/', '\') }
    }
    foreach ($p in "${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam", 'C:\Steam') {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

function Resolve-GameRoot {
    if ($script:GameRootOverride) {
        if (-not (Test-Path (Join-Path $script:GameRootOverride 'SkyrimSE.exe'))) {
            throw "No SkyrimSE.exe in '$script:GameRootOverride'."
        }
        return $script:GameRootOverride
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
    $v = (Get-Item $path).VersionInfo.FileVersion
    if ($v) { return $v.Trim() }

    # pwsh on non-Windows leaves VersionInfo empty, so read the PE resource.
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $text  = [System.Text.Encoding]::Unicode.GetString($bytes)
        $i = $text.IndexOf('FileVersion')
        if ($i -lt 0) { return $null }
        $tail = $text.Substring($i + 11, [Math]::Min(40, $text.Length - $i - 11)).Replace("`0", ' ').Trim()
        $m = [regex]::Match($tail, '^[\d.]+')
        if ($m.Success) { return $m.Value }
    } catch {
        Write-Verbose "could not read the version resource of ${path}: $_"
    }
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
    if ($script:BackupDirOverride) { return $script:BackupDirOverride }
    Join-Path (Split-Path $root -Parent) 'Skyrim SE - pre-downgrade backup'
}

# --------------------------------------------------------------------------
# Steps
# --------------------------------------------------------------------------
function Invoke-Check($root) {
    Show-Head 'Install'
    Show-Info $root

    $exe = Get-ExeVersion (Join-Path $root 'SkyrimSE.exe')
    $ck  = Get-ExeVersion (Join-Path $root 'CreationKit.exe')

    Show-Info "SkyrimSE.exe     $exe"
    Show-Info "CreationKit.exe  $(if ($ck) { $ck } else { 'not installed' })"

    $downgraded = $exe -and $exe.StartsWith($TARGET_VERSION)
    if ($downgraded) { Show-Ok "Runtime is $TARGET_VERSION. SKSE 2.2.8 is the matching build." }
    else { Show-Warn "Not on $TARGET_VERSION yet." }

    Show-Head 'Data files'
    $bsa = Join-Path $root $INTERFACE_BSA
    if (Test-Path $bsa) {
        $size = (Get-Item $bsa).Length
        Show-Info "Skyrim - Interface.bsa  $size"
        if ($size -eq $INTERFACE_SIZE_1_6_1170) {
            Show-Ok "Matches 1.6.1170 exactly. The Data files reverted, not just the exe."
        } elseif ($downgraded) {
            Show-Bad "Exe says $TARGET_VERSION but this BSA does not match 1.6.1170 ($INTERFACE_SIZE_1_6_1170)."
            Show-Info "Only depot 489833 (the exe) landed. Re-copy depots 489831 and 489832."
        } else {
            Show-Info "Expected $INTERFACE_SIZE_1_6_1170 after a correct downgrade."
        }
    }

    Show-Head 'Steam update locks'
    $steam = Resolve-SteamRoot
    if (-not $steam) { Show-Warn 'Steam root not found; cannot check manifests.'; return }

    foreach ($pair in @(@{Id = $APP_ID; Name = 'Skyrim SE' }, @{Id = $CK_APP_ID; Name = 'Creation Kit' })) {
        $mf = Join-Path $steam "steamapps\appmanifest_$($pair.Id).acf"
        if (-not (Test-Path $mf)) { Show-Info "$($pair.Name): no manifest (not installed?)"; continue }
        if ((Get-Item $mf).IsReadOnly) { Show-Ok "$($pair.Name) manifest is read-only" }
        else { Show-Warn "$($pair.Name) manifest is WRITABLE - Steam can update it. Run -Step lock" }
    }
}

function Show-DepotDownload($root) {
    $steam = Resolve-SteamRoot
    Show-Head 'Paste these into the Steam console, ONE AT A TIME'
    Show-Info 'Open it with:  Win+R  ->  steam://open/console'
    Show-Info 'Wait for each to report completion before pasting the next.'
    Show-Line
    foreach ($d in $DEPOTS) {
        Show-Line "  download_depot $APP_ID $($d.Id) $($d.Manifest)" -Colour White
        Show-Info "      $($d.Desc), $($d.Files) file(s)"
    }
    Show-Line
    Show-Info 'Roughly 15 GB total. They land in:'
    if ($steam) { Show-Info "  $steam\steamapps\content\app_$APP_ID\depot_<id>\" }
    Show-Line
    Show-Info 'Then run:  -Step install'
    Show-Line
    Show-Info 'Prefer it fully scripted? DepotDownloader takes the same three IDs'
    Show-Info 'non-interactively (it will prompt for your Steam login and 2FA):'
    Show-Info '  https://github.com/SteamRE/DepotDownloader'
    foreach ($d in $DEPOTS) {
        Show-Info "  DepotDownloader -app $APP_ID -depot $($d.Id) -manifest $($d.Manifest) -username <you>"
    }
}

function Invoke-Backup {
    [CmdletBinding(SupportsShouldProcess)]
    param([string]$root)
    $dest = Get-DefaultBackupDir $root
    Show-Head 'Backup'
    Show-Info "-> $dest"

    $total = 0L
    $plan = @()
    foreach ($rel in $CRITICAL_FILES) {
        $src = Join-Path $root $rel
        if (-not (Test-Path $src)) { Show-Warn "missing, skipping: $rel"; continue }
        $len = (Get-Item $src).Length
        $total += $len
        $plan += [pscustomobject]@{ Rel = $rel; Src = $src; Size = $len }
    }
    Show-Info "$(@($plan).Count) files, $(Format-Size $total)"

    if (-not $PSCmdlet.ShouldProcess($dest, "Copy $(@($plan).Count) files")) { return }

    $manifest = @{}
    foreach ($item in $plan) {
        $target = Join-Path $dest $item.Rel
        $parent = Split-Path $target -Parent
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

        if ((Test-Path $target) -and (Get-Item $target).Length -eq $item.Size) {
            Show-Info "already backed up: $($item.Rel)"
        } else {
            Show-Info "copying $($item.Rel)  ($(Format-Size $item.Size))"
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
    Show-Ok "Backed up. Manifest: $(Join-Path $dest 'backup-manifest.json')"
}

function Invoke-Install {
    [CmdletBinding(SupportsShouldProcess)]
    param([string]$root)
    Assert-NotRunning
    $steam = Resolve-SteamRoot
    if (-not $steam) { throw 'Could not locate Steam. Pass -SteamRoot.' }

    $content = Join-Path $steam "steamapps\content\app_$APP_ID"
    if (-not (Test-Path $content)) {
        throw "No downloaded depots at '$content'. Run -Step depots first."
    }

    Show-Head 'Downloaded depots'
    $found = @()
    foreach ($d in $DEPOTS) {
        $dir = Join-Path $content "depot_$($d.Id)"
        if (-not (Test-Path $dir)) { Show-Bad "depot_$($d.Id) missing ($($d.Desc))"; continue }
        $items  = @(Get-ChildItem $dir -Recurse -File)
        $count  = $items.Count
        $sizeMB = [math]::Round((($items | Measure-Object -Property Length -Sum).Sum) / 1MB)

        # Steam prints "Depot download complete" when it is done; that is the real
        # authority. These are sanity checks against an interrupted copy, not a
        # verdict on the download itself.
        if ($count -eq $d.Files) { Show-Ok "depot_$($d.Id) $($d.Desc): $count files, $sizeMB MB" }
        else { Show-Warn "depot_$($d.Id) $($d.Desc): $count files, expected $($d.Files) ($sizeMB MB)" }

        if ($d.MB -gt 0 -and [math]::Abs($sizeMB - $d.MB) -gt [math]::Max(50, $d.MB * 0.05)) {
            Show-Warn "  size is $sizeMB MB, expected around $($d.MB) MB - check the download finished"
        }
        $found += $dir
    }
    if (@($found).Count -ne @($DEPOTS).Count) {
        throw 'Not all three depots are present. Download the missing ones before installing.'
    }

    if (-not (Test-Admin)) {
        Show-Warn 'Not running as Administrator. Writing into Program Files will likely fail.'
    }

    # Back up anything we are about to overwrite, before overwriting it.
    Show-Head 'Backing up files that will be overwritten'
    Invoke-Backup $root

    Show-Head 'Installing'
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
            Show-Info "$rel  ($(Format-Size $src.Length))"
            $overwritten += ($rel -replace '\\', '/')
            $n++
        }
    }
    if ($WhatIfPreference) { Show-Ok "$n file(s) WOULD be installed. Nothing was changed." }
    else                    { Show-Ok "$n file(s) installed." }

    $uncovered = @($overwritten | Where-Object { $_ -notin $CRITICAL_FILES })
    if ($uncovered.Count -gt 0) {
        Show-Line
        $tense = if ($WhatIfPreference) { 'would be overwritten and are' } else { 'overwritten file(s) are' }
        Show-Warn "$($uncovered.Count) file(s) $tense NOT in the backup set:"
        foreach ($u in $uncovered) { Show-Info "  $u" }
        Show-Info 'These are bulk asset archives. -Step restore will not bring them back;'
        Show-Info 'Steam > Verify integrity of game files restores them perfectly.'
    }

    if ($WhatIfPreference) {
        Show-Line
        Show-Info 'Dry run only. Re-run without -WhatIf to apply, then it will verify.'
        return
    }

    Show-Head 'Verifying'
    Invoke-Check $root
}

function Set-ManifestLock {
    [CmdletBinding(SupportsShouldProcess)]
    param([bool]$locked)
    $steam = Resolve-SteamRoot
    if (-not $steam) { throw 'Could not locate Steam. Pass -SteamRoot.' }
    $verb = if ($locked) { 'Locking' } else { 'Unlocking' }
    Show-Head "$verb Steam manifests"

    foreach ($pair in @(@{Id = $APP_ID; Name = 'Skyrim SE' }, @{Id = $CK_APP_ID; Name = 'Creation Kit' })) {
        $mf = Join-Path $steam "steamapps\appmanifest_$($pair.Id).acf"
        if (-not (Test-Path $mf)) { Show-Info "$($pair.Name): no manifest, skipping"; continue }
        if ($PSCmdlet.ShouldProcess($mf, "Set IsReadOnly=$locked")) {
            Set-ItemProperty -LiteralPath $mf -Name IsReadOnly -Value $locked
        }
        Show-Ok "$($pair.Name): read-only = $locked"
    }

    if ($locked) {
        Show-Line
        Show-Warn 'This is necessary but NOT sufficient. Also:'
        Show-Info '  - Steam > Properties > Updates > Automatic Updates >'
        Show-Info '      "Wait until I launch the game"  (there is no "never update" option)'
        Show-Info '  - Never press Play in the Steam Library. Launch via SKSE/MO2 --'
        Show-Info '    the setting above only DEFERS an update until you launch via Steam.'
        Show-Info '  - Never run "Verify integrity of game files" - it restores 1.7.104.'
        Show-Info '  - Re-run -Step check after Steam client updates. The read-only trick'
        Show-Info '    has been reported failing after a few days.'
    }
}

function Invoke-Restore {
    [CmdletBinding(SupportsShouldProcess)]
    param([string]$root)
    Assert-NotRunning
    $dest = Get-DefaultBackupDir $root
    $manifestPath = Join-Path $dest 'backup-manifest.json'
    if (-not (Test-Path $manifestPath)) { throw "No backup manifest at '$manifestPath'." }

    $meta = Get-Content $manifestPath -Raw | ConvertFrom-Json
    Show-Head 'Restore'
    Show-Info "from $dest"
    Show-Info "captured $($meta.capturedUtc), exe was $($meta.exeVersion)"

    foreach ($prop in $meta.files.PSObject.Properties) {
        $rel = $prop.Name
        $src = Join-Path $dest $rel
        if (-not (Test-Path $src)) { Show-Warn "missing from backup: $rel"; continue }
        $target = Join-Path $root $rel
        if ($PSCmdlet.ShouldProcess($target, 'Restore from backup')) {
            if (Test-Path $target) { Set-ItemProperty -LiteralPath $target -Name IsReadOnly -Value $false }
            Copy-Item -LiteralPath $src -Destination $target -Force
        }
        Show-Info "restored $rel"
    }
    Show-Ok 'Restored. Note this only covers the critical file list, not every BSA.'
    Show-Info 'For a guaranteed clean revert: unlock manifests, then Steam > Verify integrity.'
}

# --------------------------------------------------------------------------
try {
    $root = Resolve-GameRoot
    switch ($Step) {
        'check'   { Invoke-Check   $root }
        'backup'  { Invoke-Backup  $root }
        'depots'  { Show-DepotDownload $root }
        'install' { Invoke-Install $root }
        'lock'    { Set-ManifestLock $true }
        'unlock'  { Set-ManifestLock $false }
        'restore' { Invoke-Restore $root }
    }
    Show-Line
} catch {
    Show-Line
    Show-Bad $_.Exception.Message
    exit 1
}
