# Follower Tactics guide

The Jekyll site lives in `/docs` alongside the mod source. Developer notes live in `/dev`.

## Preview

```sh
cd docs
bundle install
bundle exec jekyll serve
```

Open the address printed by Jekyll. The first build downloads the pinned Just the Docs v0.12.0 theme from GitHub. This preview has no Changelog page: the `github-pages` gem forces Jekyll's safe mode, which skips symlinks.

To preview the version selector and all releases, run from the repository root (Python 3.11+ and the same Ruby dependencies):

```sh
git fetch --tags
python tools/build-docs.py
python -m http.server 4000 --bind 127.0.0.1 --directory build/docs-site
```

Open http://127.0.0.1:4000/SkyrimFollowerTactics/. Re-run the build after editing; this preview does not live-reload. The build replaces only `build/docs-site/`. It copies each version's guide first with symlinks replaced by their targets, so the Changelog is included.

## Editing

`index.md` and `guide/*.md` are the final guide pages. Edit those pages directly; no draft or content generator is required. The wording and section order were taken from the author's draft.

`_data/guide.json` defines the sidebar's page links and heading anchors. Keep it aligned with headings when editing. The sidebar stays on the left at every screen size; its arrow collapses it to a narrow rail. Click **Table of contents** to collapse all sections when all are open, or expand them all otherwise. Individual sections can also be collapsed. Search is Just the Docs' built-in Lunr index, including headings and body content; Ctrl+K / Cmd+K focuses it.

Screenshots live in `assets/img/panel/`. `settings.png` shows the full Settings page; `settings_combat.png` shows the Combat section. The new training screenshots appear in `guide/training.md`.

`guide/changelog.md` is a symlink to the repository's `CHANGELOG.md` and appears before Thanks in the sidebar. Edit the root file when adding release notes. On Windows, a checkout needs `git config core.symlinks true` and Developer Mode to get a real link.

The Nordic color scheme is in `_sass/color_schemes/nordic.scss`, with typography and layout adjustments in `_sass/custom/custom.scss`. The theme owns the page layout and search. `_includes/components/sidebar.html` supplies the persistent table of contents.

## Publishing

The versioned build uses [`.github/workflows/docs.yml`](../.github/workflows/docs.yml) to publish at https://whocares66-dev.github.io/SkyrimFollowerTactics/. To activate it after merging, change **Settings → Pages → Build and deployment → Source** from the existing `master` / `/docs` branch source to **GitHub Actions**. The workflow builds pull requests without deploying, and publishes pushes to `master` and release tags. If the `github-pages` environment restricts deployment refs, allow `master` and release tags (`v*`).

## Versions

- The site root shows the current `master` guide, labeled by `docs_version` in `_config.yml` (currently `0.3.0`). Local previews use the working tree.
- Each `vMAJOR.MINOR.PATCH` tag is built from its own `docs/` directory under `/<version>/`, such as `/SkyrimFollowerTactics/0.2.0/`. Tags must contain a buildable Jekyll guide. Creating `v0.3.0` adds `/0.3.0/` automatically.
- Only the current sidebar and header templates, custom stylesheet, and guide script are shared with historical builds, so older docs get the same title, version selector, and navigation controls. The version selector sits in the top bar before the Nexus Mods and GitHub icons. Page content, navigation data, screenshots, and search indexes come from the tag. Search stays within the selected version.
- The selector keeps the current page when that permalink exists in the other version, otherwise it opens that version's home page.
- Keep release tags unchanged to preserve their docs. Before tagging a release, finish its guide and set `docs_version` to the release number; afterward, advance the working guide to the next development version.

GitHub Pages serves the combined static output; it does not build a separate site for each tag automatically. See [GitHub's custom workflow documentation](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).
