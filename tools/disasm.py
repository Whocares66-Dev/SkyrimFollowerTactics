"""Disassemble SkyrimSE.exe on disk by Address Library ID: tools/skyrimexe.py
has the modes. `--version 1.7.104` first (or SKYRIM_VERSION; 1.6.1170
otherwise) picks C:/Modding/SkyrimVersions/<version>/SkyrimSE.exe, a copy
unpacked with Steamless (the installed exe's code is SteamStub-encrypted
and reads as noise; SKYRIM_EXE overrides), and that version's database
under AddressLibrary/SKSE/Plugins/. IDs are the line's own: Special Edition
numbers for a 1.5 build, Anniversary numbers for 1.6 and 1.7.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrlib  # noqa: E402
import skyrimexe  # noqa: E402

if __name__ == "__main__":
    args = sys.argv[1:]
    skyrimexe.run(skyrimexe.FileImage(*addrlib.resolve(args)), args)
