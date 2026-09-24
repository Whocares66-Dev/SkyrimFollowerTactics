<#
.SYNOPSIS
    Serve one Skyrim build's Ghidra analysis over MCP, for the project's .mcp.json.

.DESCRIPTION
    Each build has a Ghidra project of its own under C:\Modding\SkyrimVersions\ghidra,
    <version>.gpr, analysed once from the Steamless-unpacked exe in
    C:\Modding\SkyrimVersions\<version>. This opens that project headless without
    analysing again, names the build's functions and globals from tools/names.py
    (CommonLibSSE-NG's and our src/game/Addresses.h, through
    tools/ghidra/ImportNames.java; a name set by hand in Ghidra is kept), and runs
    GhidrAssistMCP's server on the program until -Stop.

    -Stop asks the server to close through its completion file, so Ghidra saves what
    was renamed or retyped over MCP and exits. Ctrl+C or closing the window loses
    that; the imported names come back on the next launch either way.

    Ports are fixed per build so .mcp.json can name them: 8080 for 1.6.1170, 8081 for
    1.5.97, 8082 for 1.7.104. A project is locked while it is open: one Ghidra at a
    time -- this, the GUI, or an analysis still running.

.EXAMPLE
    .\tools\ghidra-mcp.ps1                          # 1.6.1170 on 8080
    .\tools\ghidra-mcp.ps1 -Version 1.5.97          # Special Edition on 8081
    .\tools\ghidra-mcp.ps1 -Version 1.5.97 -Stop    # save and close it
#>
[CmdletBinding()]
param(
    [ValidateSet('1.6.1170', '1.5.97', '1.7.104')]
    [string] $Version = '1.6.1170',
    [switch] $Stop,
    [string] $Ghidra = 'C:\Modding\ghidra_12.1.4_PUBLIC',
    [string] $Projects = 'C:\Modding\SkyrimVersions\ghidra',
    [string] $MaxMemory = '24G'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')

$ports = @{ '1.6.1170' = 8080; '1.5.97' = 8081; '1.7.104' = 8082 }
$port = $ports[$Version]
$complete = Join-Path $Projects "$Version.mcp.complete"

if ($Stop) {
    New-Item -ItemType File -Force $complete | Out-Null
    Show-Ok "asked the $Version server to save and close"
    return
}

# PyGhidra's launcher in headless mode: the same analyzer with Python 3 attached,
# which GhidrAssistMCP's eval_python needs. agentic=false below (our branch of
# alandtse's fork, C:\Modding\GhidrAssistMCP, agentic-switch) lists every tool
# rather than the fork's core set: Claude Code loads a tool's definition only
# when it is used, so the long list costs little.
$headless = Join-Path $Ghidra 'support\pyghidraRun.bat'
$project = Join-Path $Projects "$Version.gpr"
# Where Ghidra extracts an installed extension: the user's settings for this release.
$mcpScripts = Join-Path ([Environment]::GetFolderPath('ApplicationData')) `
    "ghidra\$(Split-Path -Leaf $Ghidra)\Extensions\GhidrAssistMCP\ghidra_scripts"
$ourScripts = Join-Path $PSScriptRoot 'ghidra'

if (-not (Test-Path $headless)) { throw "No Ghidra at $Ghidra" }
if (-not (Test-Path $project)) { throw "No analysed project at $project" }
if (-not (Test-Path (Join-Path $mcpScripts 'GAMCPStartServerScript.java'))) {
    throw "GhidrAssistMCP is not installed for this Ghidra (looked in $mcpScripts)"
}
if (Test-Path (Join-Path $Projects "$Version.lock")) {
    Show-Warn "$Version.lock is there: another Ghidra may have the project open"
}

$names = Join-Path $Projects "$Version.names.csv"
& python (Join-Path $PSScriptRoot 'names.py') --csv $Version | Set-Content -Encoding utf8 $names
if ($LASTEXITCODE -ne 0) { throw 'tools/names.py --csv failed' }
Remove-Item -Force $complete -ErrorAction SilentlyContinue

# The PyGhidra launcher takes Java options from here, not GHIDRA_HEADLESS_MAXMEM.
$env:PYGHIDRA_JAVA_OPTIONS = "-Xmx$MaxMemory"
Show-Head "Ghidra MCP: SkyrimSE.exe $Version on http://127.0.0.1:$port/mcp"
Show-Info "stop it with: .\tools\ghidra-mcp.ps1 -Version $Version -Stop"
# Only our folder is named: an installed extension's scripts are on Ghidra's own
# path, and cmd, which runs the .bat launcher, would split a `;` list apart.
& $headless -H $Projects $Version -process 'SkyrimSE.exe' -noanalysis `
    -scriptPath $ourScripts `
    -preScript ImportNames.java $names `
    -postScript GAMCPStartServerScript.java 'host=127.0.0.1' "port=$port" 'wait=true' "completion_file=$complete" `
    'agentic=false'
