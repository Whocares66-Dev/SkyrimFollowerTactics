# Releasing Follower Tactics

The standard release command is `tools/release.ps1`. It runs from `master`, builds and tests before publishing, makes an annotated tag, pushes `master` and the tag, and creates a GitHub release with the player zip. The test zip stays local.

## Prepare on a work branch

1. Set `project(FollowerTactics VERSION x.y.z)` in `CMakeLists.txt`. Set the same `docs_version` in `docs/_config.yml`.
2. Add `## x.y.z` at the top of `CHANGELOG.md` and mirror its text in `docs/guide/changelog.md`. The script uses that section verbatim as the GitHub release notes.
3. Finish the guide and package contents, including any new screenshots and translations. Run the relevant build, tests, formatter, and lint checks for the changes. Preview the versioned docs with `python tools/build-docs.py`.
4. Commit the release work on the work branch. Keep unrelated local edits out of the release commit.

## Cut the release

Fast-forward `master` to the finished work branch, leaving unrelated edits safely aside. The release script requires a clean tracked tree and verifies that `origin/master` is an ancestor of local `master`; it can then push a local `master` that is ahead.

```powershell
git switch master
git merge --ff-only wip-0-3-0
.\tools\release.ps1 none -DryRun
.\tools\release.ps1 none
```

Use `none` when `CMakeLists.txt` already names the release version. For a release whose version has not yet been bumped, use `patch`, `minor`, or `major` instead and prepare its changelog and docs version first. `-DryRun` checks the branch, remote, version, changelog, docs version, and GitHub CLI but does not build, tag, push, or publish. `gh` must be authenticated; the script finds it on PATH or in the standard Windows installation directory.

The full run builds and tests the core, builds the release DLL, writes `dist/follower-tactics-x.y.z.zip` and `dist/follower-tactics-x.y.z-test.zip`, then tags and pushes. It uploads only the player zip and marks 0.x releases as prereleases. The notes file used for publication is retained in `build/release-notes-x.y.z.md`.

## Verify

Check the GitHub release's tag, notes, prerelease flag, and single player zip. Compare the uploaded zip's size or checksum with the local file. Check that the tag-triggered documentation workflow publishes the new version in the site's version selector. Keep the tag unchanged after release; corrections need a new patch release.

If the build fails, no tag or push has occurred. If publication fails after the push, inspect the printed error and create the release from the existing tag with `gh release create vX.Y.Z --repo Whocares66-Dev/SkyrimFollowerTactics --verify-tag --notes-file build/release-notes-X.Y.Z.md dist/follower-tactics-X.Y.Z.zip` (add `--prerelease` for 0.x). Do not rerun the full script against an existing tag.
