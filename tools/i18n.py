"""The panel's catalogs (dev/I18N.md), kept in step with the source.

    python tools/i18n.py            # rewrite assets/Translations/*.json
    python tools/i18n.py --check    # say what is out of step; exit 1 if any

Every string marked Tr("..."), TrFormat("...", ...) or N_("...") under src/ is a line to translate; adjacent literals are one
line, as the compiler joins them. en-US.json is the list itself, each line its own
translation, for a translator to start from. Every other catalog keeps
what it has for a line still marked, gains an empty entry for a new one
(read in English until filled) and loses the lines no longer marked.
Lines are in the order the source first marks them, file by file, so a
translator reads a page's words together.

--check also says where a translation's {} fields differ from the
English's: TrFormat passes such a translation over for the English, so
it would never be seen.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
CATALOGS = ROOT / "assets" / "Translations"
SOURCE_LOCALE = "en-US"

# A marker, then one or more literals separated by whitespace.
MARKER = re.compile(r"\b(?:Tr|TrFormat|N_)\s*\(\s*")
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
BETWEEN = re.compile(r"\s*")  # comments are blanked before the scan
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'", "0": "\0"}
FIELD = re.compile(r"\{[^{}]*\}")


def unescape(text: str, where: str) -> str:
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "\\":
            nxt = text[i + 1]
            if nxt not in ESCAPES:
                sys.exit(f"{where}: escape \\{nxt} is not read by this tool")
            out.append(ESCAPES[nxt])
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


CODE = re.compile(
    r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|//[^\n]*|/\*.*?\*/', re.S
)


def without_comments(text: str) -> str:
    """The code alone: a comment's example, Tr("Rules"), is no line. Its
    newlines are kept, so a line number still points at the source."""

    def blank(m):
        s = m.group(0)
        return s if s[0] in "\"'" else re.sub(r"[^\n]", " ", s)

    return CODE.sub(blank, text)


def marked() -> dict:
    """Every marked line, in first-marked order, with where it is marked."""
    lines = {}
    files = sorted(p for p in SRC.rglob("*") if p.suffix in (".cpp", ".h", ".inc"))
    for path in files:
        text = without_comments(path.read_text(encoding="utf-8"))
        for m in MARKER.finditer(text):
            pos, parts = m.end(), []
            while True:
                lit = LITERAL.match(text, pos)
                if not lit:
                    break
                line_no = text.count("\n", 0, pos) + 1
                parts.append(
                    unescape(lit.group(1), f"{path.relative_to(ROOT)}:{line_no}")
                )
                pos = BETWEEN.match(text, lit.end()).end()
            if not parts:
                continue  # Tr(variable): translated where it is drawn
            key = "".join(parts)
            if key not in lines:
                lines[key] = (
                    f"{path.relative_to(ROOT).as_posix()}:{text.count(chr(10), 0, m.start()) + 1}"
                )
    return lines


def fields(text: str) -> int:
    # How many arguments a line takes: {} is the next, {1} the second
    # (a translation reorders by number); the escape {{ is none.
    found = FIELD.findall(text.replace("{{", "").replace("}}", ""))
    numbered = [
        int(f[1:-1].split(":")[0]) for f in found if f[1:-1].split(":")[0].isdigit()
    ]
    return max(numbered) + 1 if numbered else len(found)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    lines = marked()
    CATALOGS.mkdir(parents=True, exist_ok=True)
    problems = 0

    source = {key: key for key in lines}
    paths = sorted(CATALOGS.glob("*.json"))
    if CATALOGS / f"{SOURCE_LOCALE}.json" not in paths:
        paths.insert(0, CATALOGS / f"{SOURCE_LOCALE}.json")

    for path in paths:
        locale = path.stem
        old = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        if locale == SOURCE_LOCALE:
            new = source
        else:
            new = {key: old.get(key, "") for key in lines}
        stale = [k for k in old if k not in lines]
        missing = [k for k in lines if k not in old]
        done = sum(1 for v in new.values() if v)
        mismatched = []
        for key, value in new.items():
            if value and "{" in key + value and fields(key) != fields(value):
                # Only TrFormat's lines are formatted; a plain line's braces
                # are text. The source is where that is known.
                mismatched.append(key)
        print(
            f"{locale}: {done}/{len(lines)} translated, {len(missing)} new, {len(stale)} gone"
        )
        for key in mismatched:
            print(f"  fields differ ({lines[key]}): {key!r} -> {new[key]!r}")

        text = json.dumps(new, ensure_ascii=False, indent=2) + "\n"
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if args.check:
            if current != text:
                print(f"  {path.relative_to(ROOT)} is out of step with the source")
                problems += 1
            problems += len(mismatched)
        elif current != text:
            path.write_text(text, encoding="utf-8", newline="\n")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
