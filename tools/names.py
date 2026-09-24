"""Names for Address Library IDs, from the source that binds them:
CommonLibSSE-NG's functions (each `RELOCATION_ID(se, ae)` named by the
definition it sits in), its vtable and RTTI tables, and our own
src/game/Addresses.h. The disassembler prints them beside an ID, so a
finding written into either is read back wherever that ID appears.

A name is what the source calls the ID, not a verdict on it: CommonLib has
been wrong before (dev/COMMONLIB.md), and an ID bound in two places gets
both names. `REL::ID(n)` is left out, since it does not say which line's
number it is.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
COMMONLIB = os.path.join(ROOT, "extern", "commonlibsse-ng")
ADDRESSES = os.path.join(ROOT, "src", "game", "Addresses.h")

PAIR = r"\s*(\d+)\s*,\s*(\d+)"
RELOCATION = re.compile(r"(?:RELOCATION_ID|REL::RelocationID)\s*\(" + PAIR)
VARIANT = re.compile(r"REL::VariantID\s*\(" + PAIR + r"\s*,")
# A definition's head: a name, perhaps qualified, then its parameter list,
# on a line that is not a statement or a control keyword's.
DEFINITION = re.compile(r"(~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*)\s*\([^;]*$")
NOT_A_DEFINITION = re.compile(
    r"^\s*(if|for|while|switch|return|else|case|do|using|static_assert)\b"
)
CLASS = re.compile(r"^(\s*)(?:class|struct)\s+(?:[A-Z_]+\s+)?([A-Za-z_]\w*)\b[^;]*$")
TABLE = re.compile(r"\b((?:VTABLE|RTTI|NiRTTI)_\w+)\s*\{?\(?")
ADDRESS = re.compile(r"REL::RelocationID\s+(k\w+)\s*\{" + PAIR)


def _add(names, key, name):
    have = names.setdefault(key, [])
    if name not in have:
        have.append(name)


def _enclosing(lines, index, qualified):
    """The definition a line sits in: the nearest head above it, qualified
    by the class it is declared in when a header leaves it bare. In a .cpp
    CommonLib always qualifies a definition, so a bare head there is
    something else: a constructor's member initialiser, `_data(nullptr)`."""
    for k in range(index, -1, -1):
        line = lines[k]
        if NOT_A_DEFINITION.match(line):
            continue
        m = DEFINITION.search(line)
        if not m or m.group(1) in ("func", "decltype", "RELOCATION_ID"):
            continue
        name = m.group(1)
        if qualified and "::" not in name:
            continue
        if "::" not in name:
            depth = len(line) - len(line.lstrip("\t "))
            for j in range(k - 1, -1, -1):
                c = CLASS.match(lines[j])
                if c and len(c.group(1)) < depth:
                    name = f"{c.group(2)}::{name}"
                    break
        return name
    return None


def _scan(path, se, ae):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return
    table_file = os.path.basename(path).startswith("Offsets_")
    for i, line in enumerate(lines):
        if table_file:
            m = TABLE.search(line)
            if not m:
                continue
            for k, v in enumerate(VARIANT.finditer(line)):
                label = f"{m.group(1)}[{k}]" if "std::array" in line else m.group(1)
                _add(se, int(v.group(1)), label)
                _add(ae, int(v.group(2)), label)
            continue
        for r in RELOCATION.finditer(line):
            name = (
                _enclosing(lines, i, path.endswith(".cpp"))
                or os.path.splitext(os.path.basename(path))[0]
            )
            _add(se, int(r.group(1)), name)
            _add(ae, int(r.group(2)), name)


def load(version):
    """{id: name} for the line `version` belongs to: Special Edition numbers
    for a 1.5 build, Anniversary numbers for 1.6 and 1.7."""
    se, ae = {}, {}
    for base in ("src", "include"):
        for folder, _, files in os.walk(os.path.join(COMMONLIB, base)):
            for file in files:
                if file.endswith((".h", ".cpp")):
                    _scan(os.path.join(folder, file), se, ae)
    try:
        with open(ADDRESSES, encoding="utf-8") as f:
            for m in ADDRESS.finditer(f.read()):
                _add(se, int(m.group(2)), f"addr::{m.group(1)}")
                _add(ae, int(m.group(3)), f"addr::{m.group(1)}")
    except OSError:
        pass
    side = se if version.startswith("1.5") else ae
    return {i: " | ".join(n) for i, n in side.items() if i != 0}
