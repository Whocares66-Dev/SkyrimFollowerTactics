# Follower Tactics guide

The Jekyll site lives in `/docs` alongside the mod source. Documentation work is
currently on `wip-docs`, branched from `master`. Developer notes live in `/dev`.

## Preview

```sh
cd docs
bundle install
bundle exec jekyll serve
```

Open the address printed by Jekyll. The first build downloads the pinned Just the Docs v0.12.0 theme from GitHub.

## Editing

`index.md` and `guide/*.md` are the final guide pages. Edit those pages directly; no draft or content generator is required. The wording and section order were taken from the author's draft.

`_data/guide.json` defines the sidebar's page links and heading anchors. Keep it aligned with headings when editing. The sidebar stays on the left at every screen size; its arrow collapses it to a narrow rail. Individual sections can also be collapsed. Search is Just the Docs' built-in Lunr index, including headings and body content; Ctrl+K / Cmd+K focuses it.

Screenshots live in `assets/img/panel/`. The Settings screenshot appears in the enabling and disabling section of `guide/tactics.md`.

The Nordic color scheme is in `_sass/color_schemes/nordic.scss`, with typography and layout adjustments in `_sass/custom/custom.scss`. The theme owns the page layout and search. `_includes/components/sidebar.html` supplies the persistent table of contents.

## Publishing

After merging this branch into `master`, set the repository's **Settings > Pages**
to **Deploy from a branch**, branch **master**, folder **/docs**. GitHub Pages
builds Jekyll automatically; no custom Actions workflow is needed. Subsequent
pushes to `master` publish the guide.

Until that setting is changed, the existing `gh-pages` publication is unaffected.
The original `site-preview` worktree is retained locally as a migration backup.
