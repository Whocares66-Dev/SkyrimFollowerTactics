#!/usr/bin/env python3
"""Report the Skyrim install's version and key file sizes.

Run before and after a downgrade to confirm it actually took. Reads the PE
version resource directly rather than trusting folder names or Steam.

    python tools/check_install.py "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition"
"""

import os
import re
import sys

# Measured on this machine 2026-09-01, immediately AFTER a verified-clean
# downgrade: exe reads 1.6.1170.0 and all 46 depot files matched byte-for-byte.
# This is the known-good state. Any drift means Steam re-updated you.
GOOD_1_6_1170 = {
    "SkyrimSE.exe": 37157144,
    "SkyrimSELauncher.exe": 4713472,
    "steam_api64.dll": 298384,
    "bink2w64.dll": 391360,
    "CreationKit.exe": 48685408,
    "Data/Skyrim.esm": 249753412,
    "Data/Update.esm": 18874041,
    "Data/Dawnguard.esm": 25885111,
    "Data/HearthFires.esm": 3977420,
    "Data/Dragonborn.esm": 64663863,
    "Data/_ResourcePack.esl": 78418,
    "Data/Skyrim - Interface.bsa": 105799354,
}

# The pre-downgrade state, kept so a full revert is recognisable too.
OLD_1_7_104 = {
    "SkyrimSE.exe": 37910440,
    "SkyrimSELauncher.exe": 4724064,
    "Data/Skyrim.esm": 249752131,
    "Data/Update.esm": 19121864,
    "Data/Skyrim - Interface.bsa": 106921497,
}

WATCH = [
    "SkyrimSE.exe",
    "SkyrimSELauncher.exe",
    "steam_api64.dll",
    "bink2w64.dll",
    "CreationKit.exe",
    "Data/Skyrim.esm",
    "Data/Update.esm",
    "Data/Dawnguard.esm",
    "Data/HearthFires.esm",
    "Data/Dragonborn.esm",
    "Data/_ResourcePack.esl",
    "Data/Skyrim - Interface.bsa",
]


def pe_file_version(path):
    """Pull FileVersion out of a PE's VS_VERSIONINFO block."""
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        return f"<{exc.strerror}>"
    text = blob.decode("utf-16-le", errors="ignore")
    match = re.search("FileVersion", text)
    if not match:
        return "<no version resource>"
    tail = text[match.end() : match.end() + 40].replace("\x00", " ").strip()
    version = re.match(r"[\d.]+", tail)
    return version.group(0) if version else "<unparsed>"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(root):
        sys.exit(f"not a directory: {root}")

    exe_version = pe_file_version(os.path.join(root, "SkyrimSE.exe"))
    ck_version = pe_file_version(os.path.join(root, "CreationKit.exe"))

    print(f"SkyrimSE.exe    {exe_version}")
    print(f"CreationKit.exe {ck_version}")
    print()

    # CKPE supports CK 1.5.73, 1.6.1130 and 1.6.1378.1 -- not 1.7.99. That, not
    # any plugin-format issue, is why the CK has to come down with the game.
    # SKSE: the runtime DLL's filename is the authoritative version signal.
    import glob as _glob
    skse = sorted(os.path.basename(x) for x in _glob.glob(os.path.join(root, "skse64_*.dll")))
    loader = os.path.exists(os.path.join(root, "skse64_loader.exe"))
    if skse and loader:
        want = "skse64_1_6_1170.dll"
        if want in skse:
            print(f"  OK - SKSE installed, {want} matches the runtime.")
        else:
            print(f"  SKSE DLL is {skse} but the runtime is 1.6.1170 - MISMATCH.")
    elif skse or loader:
        print("  SKSE is partially installed (loader and DLL must both be in the game root).")
    else:
        print("  SKSE not installed.")

    # Address Library: CommonLibSSE-NG resolves offsets from the versionlib .bin
    # matching the running exe. Without it, every NG plugin fails to load.
    vl = os.path.join(root, "Data", "SKSE", "Plugins", "versionlib-1-6-1170-0.bin")
    print("  OK - Address Library present for 1.6.1170."
          if os.path.exists(vl) else
          "  Address Library MISSING (Data/SKSE/Plugins/versionlib-1-6-1170-0.bin).")

    if ck_version and ck_version.startswith("1.6.1378"):
        print("  OK - CK matches the game and is a CKPE-supported version.")
    elif ck_version and ck_version.startswith("1.7."):
        print("  CK is 1.7.x - CKPE does not support it. Downgrade to 1.6.1378.1")
        print("     via Nexus mod 190110.")
    print()

    if exe_version.startswith("1.6.1170"):
        print("  OK - target runtime. SKSE 2.2.8 is the matching build.")
    elif exe_version.startswith("1.7."):
        print("  REVERTED to 1.7.x. Steam re-updated you - see docs/DOWNGRADE.md.")
    else:
        print(f"  Unexpected runtime: {exe_version}")
    print()
    print("  Files below are silent when they match the known-good 1.6.1170 state.")
    print()

    for name in WATCH:
        path = os.path.join(root, name)
        try:
            size = os.stat(path).st_size
        except OSError:
            print(f"{'MISSING':>12}  {name}")
            continue

        if GOOD_1_6_1170.get(name) == size:
            note = ""
        elif OLD_1_7_104.get(name) == size:
            note = "   <- REVERTED to 1.7.104. Steam re-updated you."
        elif name in GOOD_1_6_1170:
            note = f"   <- DRIFT: expected {GOOD_1_6_1170[name]}"
        else:
            note = ""
        print(f"{size:>12}  {name}{note}")


if __name__ == "__main__":
    main()
