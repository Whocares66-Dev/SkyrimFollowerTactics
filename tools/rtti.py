"""The class name of a vtable, from MSVC's RTTI: the slot before a vtable's
first holds its complete object locator, which names the type descriptor,
whose decorated name follows two pointers. For telling apart the visitors,
entries and tasks the disassembly passes around.

    python tools/rtti.py [--version 1.6.1170] <vtable id | 0xrva> ...
    python tools/rtti.py [--version 1.6.1170] --find <text>   vtables whose class name contains text
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrlib  # noqa: E402
import skyrimexe  # noqa: E402


def name_of(img, vtable_rva):
    col = struct.unpack("<Q", img.read(vtable_rva - 8, 8))[0] - img.base
    if not (0 < col < 0x8000000):
        return None
    td = struct.unpack("<I", img.read(col + 12, 4))[0]
    raw = img.read(td + 16, 256)
    return raw.split(b"\0", 1)[0].decode(errors="replace")


if __name__ == "__main__":
    args = sys.argv[1:]
    img = skyrimexe.FileImage(*addrlib.resolve(args))
    if args and args[0] == "--find":
        want = args[1].lower()
        for i, rva in sorted(img.ids.items()):
            if not (img.sections[".rdata"][0] <= rva < sum(img.sections[".rdata"])):
                continue
            try:
                n = name_of(img, rva)
            except Exception:
                continue
            if n and n.startswith(".?A") and want in n.lower():
                print(f"ID {i} at {rva:#x}: {n}")
    else:
        for a in args:
            rva = int(a, 16) if a.startswith("0x") else img.ids[int(a)]
            print(f"{a}: {name_of(img, rva)}")
