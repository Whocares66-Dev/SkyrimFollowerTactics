"""Build the working guide and release-tag guides into one GitHub Pages site.

Run after `bundle install` in docs/. Output: build/docs-site/<baseurl>/.
Only the shared sidebar, header, stylesheet, and guide script are overlaid on historical sources.
"""

import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
BUILD = ROOT / "build"
OUTPUT = BUILD / "docs-site"
CHROME = ("_includes/components/sidebar.html", "_includes/header_custom.html",
          "_sass/custom/custom.scss", "assets/js/guide.js")


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def setting(name):
    match = re.search(rf"^{name}:\s*(.+)$", (DOCS / "_config.yml").read_text(), re.M)
    if not match:
        raise ValueError(f"Missing {name} in docs/_config.yml")
    return match[1].strip().strip("\"'")


def pages(source):
    # Guide pages have explicit permalinks, shared by sidebar and search.
    result = []
    for path in source.rglob("*.md"):
        match = re.search(r'^permalink:\s*[\"\']?(/[^\"\'\s]*)', path.read_text(encoding="utf-8"), re.M)
        if match:
            result.append(match[1])
    return sorted(set(result))


def main():
    baseurl = setting("baseurl").rstrip("/")
    if not re.fullmatch(r"(?:/[A-Za-z0-9_-]+)*", baseurl):
        raise ValueError("baseurl must be a path of simple directory names")
    tags = []
    for tag in git("tag", "--list").decode().splitlines():
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", tag)
        if match:
            tags.append((tuple(map(int, match.groups())), tag))
    tags.sort(reverse=True)
    BUILD.mkdir(exist_ok=True)
    # Fixed generated output, checked before recursive cleanup.
    if OUTPUT.is_symlink() or OUTPUT.resolve().parent != BUILD.resolve():
        raise ValueError("Output must stay directly inside the repository build directory")
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    destination = OUTPUT / baseurl.lstrip("/")
    destination.mkdir(parents=True)
    bundle = shutil.which("bundle")
    if not bundle:
        raise RuntimeError("Install Ruby and Bundler, then run bundle install in docs/")
    env = {**os.environ, "BUNDLE_GEMFILE": str(DOCS / "Gemfile")}
    with tempfile.TemporaryDirectory(prefix="docs-sources-", dir=BUILD) as temporary:
        staging = Path(temporary)
        sources = [(setting("docs_version"), DOCS, baseurl)]
        for _, tag in tags:
            source = staging / tag
            archive = git("archive", "--format=zip", tag, "docs")
            with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
                for entry in zipped.infolist():
                    target = source / entry.filename
                    if not target.resolve().is_relative_to(source.resolve()):
                        raise ValueError(f"Unsafe archive entry: {entry.filename}")
                    if entry.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(zipped.read(entry))
            source /= "docs"
            for relative in CHROME:
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(DOCS / relative, target)
            sources.append((tag[1:], source, f"{baseurl}/{tag[1:]}"))
        versions = [dict(label=label, baseurl=url, pages=pages(source)) for label, source, url in sources]
        for index, (label, source, url) in enumerate(sources):
            # JSON is valid YAML; Jekyll merges this after the tag's own config.
            config = staging / f"config-{index}.yml"
            config.write_text(json.dumps(dict(baseurl=url, docs_version=label, docs_versions=versions)), encoding="utf-8")
            target = destination if index == 0 else destination / label
            subprocess.run(
                [bundle, "exec", "jekyll", "build", "--source", str(source),
                 "--destination", str(target), "--config", f"{source / '_config.yml'},{config}"],
                cwd=DOCS, env=env, check=True,
            )
    (destination / ".nojekyll").touch()
    print(f"Pages artifact: {destination}", flush=True)
    print(f"Preview: python -m http.server 4000 --bind 127.0.0.1 --directory {OUTPUT}", flush=True)
    print(f"Open http://127.0.0.1:4000{baseurl}/", flush=True)


if __name__ == "__main__":
    main()
