<#
    Cut a release: bump the version, tag it, package the zips, and put the
    player's one on a GitHub release.

        .\tools\release.ps1 patch      0.1.0 -> 0.1.1
        .\tools\release.ps1 minor      0.1.0 -> 0.2.0
        .\tools\release.ps1 major      0.1.0 -> 1.0.0
        .\tools\release.ps1 none       the version CMakeLists.txt already names
        .\tools\release.ps1 patch -DryRun     say what it would do, change nothing

    A TAG is git's: a name for one commit, in the repository, pushed like a
    branch. A RELEASE is GitHub's, built on top of a tag: a title, notes, and
    -- the reason we need one -- uploaded FILES. A tag alone gets the source
    archives GitHub generates; only a release can carry our built zips.

    The version lives in exactly one place, `project(FollowerTactics VERSION
    ...)` in CMakeLists.txt. It reaches the DLL as FT_VERSION (every log line
    and every event carries it), the SKSE plugin declaration, and the zip
    names, so the bump here is the whole bump.

    The order is deliberate: everything that can fail without leaving a mark
    goes first. The tests and the release build run BEFORE the commit and the
    tag, so a broken tree leaves nothing behind but an edited CMakeLists.txt
    to `git checkout`. Nothing is pushed until the zip exists.

    Only the player's zip is published. Packaging writes the test zip too
    (debug logging), and it stays in dist\: it is ours to install, and a
    second download on the release page is an invitation to take the wrong
    one.

    Release notes come from this version's section of CHANGELOG.md, so the
    published description matches the guide and the repository.

    The per-edit gauntlet in CLAUDE.md -- the formatter, the linters, the
    sanitizer -- is not run here: it belongs to the edits that got us to
    master, not to the cut. What runs is the core tests and the release build.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('major', 'minor', 'patch', 'none')]
    [string]$Bump,

    # Make the release a draft: it exists on GitHub, with its files, and
    # nobody sees it until it is published by hand.
    [switch]$Draft,

    # Say what would happen. Runs no build, writes nothing, pushes nothing.
    [switch]$DryRun,

    # Skip the clean-tree and remote ancestry checks, for a release cut from
    # a master with edits in flight. It does NOT
    # skip the master check: nothing releases off another branch.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$root = Split-Path -Parent $PSScriptRoot
# Every git call names the repository explicitly rather than this script
# changing the caller's directory, which in PowerShell outlives the script.

function Fail($message) { throw $message }

# --- where this would go ----------------------------------------------------

$remote = (git -C $root remote get-url origin).Trim()
# The remote may be an SSH host alias (git@github.com-someone:Owner/Repo.git),
# which gh does not read as a GitHub remote, so the repository is named
# explicitly on every gh call.
if ($remote -notmatch '[:/]([^:/]+/[^/]+?)(\.git)?$') { Fail "Cannot read an owner/repo out of the origin remote: $remote" }
$repo = $Matches[1]

# --- checks -----------------------------------------------------------------

# A release is cut from master, and this one is not negotiable -- not even by
# -Force. Work happens on wip branches here, so "release from the branch I
# happen to be on" is a mistake waiting to be made, and one that puts a tag
# and a public zip on work that was never merged.
$branch = (git -C $root rev-parse --abbrev-ref HEAD).Trim()
if ($branch -ne 'master') {
    Fail "On branch '$branch'. A release is cut from master: merge this first, then git switch master."
}

if (-not $Force) {
    # Tracked changes only: dist\, build\ and the site preview are untracked
    # by design and are not a reason to refuse.
    $dirty = git -C $root status --porcelain --untracked-files=no
    if ($dirty) { Fail "The working tree has changes:`n$($dirty -join "`n")" }

    git -C $root fetch --quiet origin
    if ($LASTEXITCODE -ne 0) { Fail "Could not fetch origin; cannot verify master before release." }
    $local = (git -C $root rev-parse HEAD).Trim()
    $upstream = (git -C $root rev-parse origin/master).Trim()
    git -C $root merge-base --is-ancestor $upstream $local
    if ($LASTEXITCODE -ne 0) { Fail "origin/master is not an ancestor of master. Fetch and merge first (or pass -Force)." }
}

# gh is what makes the release and uploads the zips. A dry run reports it as
# a warning and goes on, so one run says everything that is not ready rather
# than stopping at the first thing.
$ghProblem = $null
$gh = $null
if ($ghCommand = Get-Command gh -ErrorAction SilentlyContinue) {
    $gh = $ghCommand.Source
}
elseif ($env:ProgramFiles) {
    $installedGh = Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe'
    if (Test-Path -LiteralPath $installedGh) { $gh = $installedGh }
}
if (-not $gh) {
    $ghProblem = "The GitHub CLI (gh) is not installed: https://cli.github.com"
}
else {
    & $gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { $ghProblem = "gh is not logged in. Run: gh auth login" }
}
if ($ghProblem) {
    if (-not $DryRun) { Fail $ghProblem }
    Write-Warning $ghProblem
}

# --- the version ------------------------------------------------------------

$cmake = Join-Path $root 'CMakeLists.txt'
$text = Get-Content $cmake -Raw
if ($text -notmatch 'project\((\w+) VERSION (\d+)\.(\d+)\.(\d+)') { Fail "No project(<name> VERSION x.y.z) in CMakeLists.txt" }
$name = $Matches[1]
[int]$major, [int]$minor, [int]$patch = $Matches[2], $Matches[3], $Matches[4]
$from = "$major.$minor.$patch"

switch ($Bump) {
    'major' { $major++; $minor = 0; $patch = 0 }
    'minor' { $minor++; $patch = 0 }
    'patch' { $patch++ }
    'none' { }
}
$version = "$major.$minor.$patch"
$tag = "v$version"

if ((git -C $root tag --list $tag)) { Fail "Tag $tag already exists here." }
git -C $root ls-remote --exit-code --tags origin "refs/tags/$tag" *> $null
if ($LASTEXITCODE -eq 0) { Fail "Tag $tag already exists on origin." }
if ($LASTEXITCODE -ne 2) { Fail "Could not check whether tag $tag exists on origin." }

$changelog = Join-Path $root 'CHANGELOG.md'
$notesMatch = [regex]::Match((Get-Content $changelog -Raw),
    "(?ms)^## $([regex]::Escape($version))\s*\r?\n(?<notes>.*?)(?=^## |\z)")
if (-not $notesMatch.Success -or -not $notesMatch.Groups['notes'].Value.Trim()) {
    Fail "CHANGELOG.md needs a nonempty ## $version section before release."
}
$notes = $notesMatch.Groups['notes'].Value.Trim()
$docsConfig = Get-Content (Join-Path $root 'docs\_config.yml') -Raw
if ($docsConfig -notmatch "(?m)^docs_version:\s*$([regex]::Escape($version))\s*$") {
    Fail "docs/_config.yml must set docs_version: $version before release."
}
$notesFile = Join-Path $root "build\release-notes-$version.md"

# Nothing is shipped below 1.0, and GitHub's own word for that is prerelease:
# it keeps the release off "Latest" and out of the update feeds.
$prerelease = $major -eq 0

Show-Line "release $repo  $from -> $version  (tag $tag)" -Colour Cyan
if ($prerelease) { Show-Line "  marked prerelease (0.x)" }
if ($Draft) { Show-Line "  draft: published by hand" }

if ($DryRun) {
    Show-Line "`n-DryRun, so this is where it stops. It would:" -Colour Yellow
    $steps = @("set $name VERSION to $version in CMakeLists.txt",
        "run the core tests",
        "run tools\package.ps1 -- both zips into dist\",
        "publish the player's zip only; the test zip stays here",
        "use CHANGELOG.md's $version section as the GitHub release notes")
    if ($Bump -ne 'none') { $steps += "commit CMakeLists.txt as `"$name $version`"" }
    $steps += @("tag $tag, push master and the tag",
        "gh release create $tag on $repo with the player's zip attached")
    $steps | ForEach-Object { Show-Line "  - $_" }
    return
}

# --- bump, then prove it builds --------------------------------------------

if ($Bump -ne 'none') {
    $bumped = $text -replace "project\($name VERSION $([regex]::Escape($from))", "project($name VERSION $version"
    if ($bumped -eq $text) { Fail "The version line did not change -- CMakeLists.txt is not what was read." }
    Set-Content -Path $cmake -Value $bumped -NoNewline
}

try {
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset core -Test
    if ($LASTEXITCODE -ne 0) { Fail "The core tests failed. Nothing has been committed; git checkout CMakeLists.txt to undo the bump." }

    & (Join-Path $PSScriptRoot 'package.ps1')
    # Packaging writes both zips; only the player's is published. The test
    # zip's debug log is for us, and an extra download on the release page
    # labelled "test" is an invitation to install the wrong one.
    $zip = Join-Path $root "dist\follower-tactics-$version.zip"
    if (-not (Test-Path $zip)) { Fail "Packaging did not write $zip." }
}
catch {
    Show-Line "`nfailed before anything was committed. To undo the bump: git checkout CMakeLists.txt" -Colour Red
    throw
}

# --- commit, tag, push, release --------------------------------------------

if ($Bump -ne 'none') {
    git -C $root add $cmake
    if ($LASTEXITCODE -ne 0) { Fail "Could not stage the version bump." }
    git -C $root commit --quiet -m "$name $version"
    if ($LASTEXITCODE -ne 0) { Fail "Could not commit the version bump." }
}
New-Item -ItemType Directory -Force (Split-Path -Parent $notesFile) | Out-Null
[System.IO.File]::WriteAllText($notesFile, "$notes`n", [System.Text.UTF8Encoding]::new($false))
git -C $root tag -a $tag -m "$name $version"
if ($LASTEXITCODE -ne 0) { Fail "Could not create tag $tag." }
git -C $root push --quiet origin HEAD
if ($LASTEXITCODE -ne 0) { Fail "Could not push master; tag $tag exists locally but nothing was published." }
git -C $root push --quiet origin $tag
if ($LASTEXITCODE -ne 0) { Fail "Master was pushed, but tag $tag was not; push the tag before creating the release." }

$arguments = @('release', 'create', $tag, '--repo', $repo, '--title', "$name $version",
    '--notes-file', $notesFile, '--verify-tag')
if ($prerelease) { $arguments += '--prerelease' }
if ($Draft) { $arguments += '--draft' }
$arguments += $zip

& $gh @arguments
if ($LASTEXITCODE -ne 0) {
    Fail "The commit, the tag and the push went through; gh release create did not. Re-run just that with --notes-file $notesFile and dist\follower-tactics-$version.zip."
}

Show-Line "`nreleased $name $version" -Colour Green
Show-Line "  https://github.com/$repo/releases/tag/$tag"
Show-Line "  the test zip stays in dist\ -- it is not published"
