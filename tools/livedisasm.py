"""Disassemble the RUNNING SkyrimSE.exe, whose code is decrypted in memory:
tools/skyrimexe.py has the modes. The IDs come from the database for
`--version <build>` (or SKYRIM_VERSION; 1.6.1170 otherwise), which has to
be the build that is running.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrlib  # noqa: E402
import skyrimexe  # noqa: E402

if __name__ == "__main__":
    args = sys.argv[1:]
    version, _, lib = addrlib.resolve(args)
    img = skyrimexe.LiveImage(version, lib)
    print(f"SkyrimSE.exe pid {img.pid} base {img.base:#x}, IDs of {version}")
    skyrimexe.run(img, args)
