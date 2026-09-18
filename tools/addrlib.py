"""Read Address Library databases of every format, and look IDs up across builds.

    python tools/addrlib.py <bin>                       header and entry count
    python tools/addrlib.py <bin> <id> [<id> ...]       the offsets of those IDs in one database
    python tools/addrlib.py --all <id> [<id> ...]       the same, in every database under AddressLibrary/
    python tools/addrlib.py --rlookup <bin> <hex rva>   the ID owning an RVA (largest offset <= rva)

Formats, from CommonLibSSE-NG's src/REL/IDDB.cpp: 1 is the Special Edition line
(version-1-5-*.bin), 2 the Anniversary line to 1.6.1179 (versionlib-1-6-*.bin),
both a delta-coded sparse list; 5 is 1.7.99 and later, a fixed 96-byte header
and a dense uint32 array indexed by ID, a zero meaning the ID is not in the
build. The ID space is per line: an SE ID and an AE ID with the same number
name different things, and the 1.7 files carry on the AE numbering.
"""
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIBDIR = os.path.join(HERE, "..", "AddressLibrary", "SKSE", "Plugins")
VERSIONS = r"C:\Modding\SkyrimVersions"


def parse_version(text):
    parts = [int(x) for x in text.split(".")]
    return tuple(parts + [0] * (4 - len(parts)))


def resolve(args):
    """Which build a tool works on: `--version 1.7.104` at the front of args
    (consumed), else SKYRIM_VERSION, else 1.6.1170. -> (version text, exe
    path, database path). The exe is VERSIONS/<version>/SkyrimSE.exe, a copy
    unpacked with Steamless (SKYRIM_EXE overrides); the database is whichever
    file under LIBDIR names that version."""
    version = os.environ.get("SKYRIM_VERSION", "1.6.1170")
    if args[:1] == ["--version"]:
        version = args[1]
        del args[:2]
    exe = os.environ.get("SKYRIM_EXE") or os.path.join(VERSIONS, version, "SkyrimSE.exe")
    # By file name, not by the version in the header: versionlib-1-6-1170-0-1.bin
    # also says 1.6.1170.0 inside and is a different table.
    dashed = "-".join(str(v) for v in parse_version(version))
    for prefix in ("versionlib-", "version-"):
        path = os.path.join(LIBDIR, f"{prefix}{dashed}.bin")
        if os.path.exists(path):
            return version, exe, path
    raise SystemExit(f"no address library for {version} under {LIBDIR}")


def _sparse(d, p, count, ptr_size):
    ids = {}
    prev_id = prev_off = 0
    for _ in range(count):
        t = d[p]
        p += 1
        lo, hi = t & 0xF, t >> 4
        if lo == 0:
            (i,) = struct.unpack_from("<Q", d, p)
            p += 8
        elif lo == 1:
            i = prev_id + 1
        elif lo == 2:
            i = prev_id + d[p]
            p += 1
        elif lo == 3:
            i = prev_id - d[p]
            p += 1
        elif lo == 4:
            i = prev_id + struct.unpack_from("<H", d, p)[0]
            p += 2
        elif lo == 5:
            i = prev_id - struct.unpack_from("<H", d, p)[0]
            p += 2
        elif lo == 6:
            (i,) = struct.unpack_from("<H", d, p)
            p += 2
        elif lo == 7:
            (i,) = struct.unpack_from("<I", d, p)
            p += 4
        else:
            raise ValueError(lo)
        tmp = (prev_off // ptr_size) if (hi & 8) else prev_off
        k = hi & 7
        if k == 0:
            (o,) = struct.unpack_from("<Q", d, p)
            p += 8
        elif k == 1:
            o = tmp + 1
        elif k == 2:
            o = tmp + d[p]
            p += 1
        elif k == 3:
            o = tmp - d[p]
            p += 1
        elif k == 4:
            o = tmp + struct.unpack_from("<H", d, p)[0]
            p += 2
        elif k == 5:
            o = tmp - struct.unpack_from("<H", d, p)[0]
            p += 2
        elif k == 6:
            (o,) = struct.unpack_from("<H", d, p)
            p += 2
        else:
            (o,) = struct.unpack_from("<I", d, p)
            p += 4
        if hi & 8:
            o *= ptr_size
        ids[i] = o
        prev_id, prev_off = i, o
    return ids


def load(path):
    """-> (format, version tuple, {id: offset})."""
    with open(path, "rb") as f:
        d = f.read()
    (fmt,) = struct.unpack_from("<i", d, 0)
    p = 4
    ver = struct.unpack_from("<4i", d, p)
    p += 16
    if fmt in (1, 2):
        (tlen,) = struct.unpack_from("<i", d, p)
        p += 4 + tlen
        ptr_size, count = struct.unpack_from("<ii", d, p)
        p += 8
        return fmt, ver, _sparse(d, p, count, ptr_size)
    if fmt == 5:
        p += 64  # name
        ptr_size, data_fmt, count = struct.unpack_from("<iii", d, p)
        p += 12
        dense = struct.unpack_from(f"<{count}I", d, p)
        return fmt, ver, {i: o for i, o in enumerate(dense) if o}
    raise ValueError(f"{path}: unknown format {fmt}")


def version_key(path):
    _, ver, _ = load(path)
    return ver


def all_databases():
    paths = glob.glob(os.path.join(LIBDIR, "*.bin"))
    return sorted(paths, key=version_key)


def main(argv):
    if not argv:
        print(__doc__)
        return
    if argv[0] == "--all":
        wanted = [int(x) for x in argv[1:]]
        print(f"{'file':<30}{'fmt':<5}{'entries':<9}" + "".join(f"{i:>12}" for i in wanted))
        for path in all_databases():
            fmt, ver, ids = load(path)
            name = os.path.basename(path)[: -len(".bin")]
            cells = "".join(f"{ids[i]:>#12x}" if i in ids else f"{'-':>12}" for i in wanted)
            print(f"{name:<30}{fmt:<5}{len(ids):<9}{cells}")
        return
    if argv[0] == "--rlookup":
        _, _, ids = load(argv[1])
        rva = int(argv[2], 16)
        i, o = max(((i, o) for i, o in ids.items() if o <= rva), key=lambda x: x[1])
        print(f"ID {i} at {o:#x} (+{rva - o:#x})")
        return
    fmt, ver, ids = load(argv[0])
    print(f"format {fmt}, version {'.'.join(map(str, ver))}, {len(ids)} entries")
    for x in argv[1:]:
        i = int(x)
        print(f"  {i}: {ids[i]:#x}" if i in ids else f"  {i}: not present")


if __name__ == "__main__":
    main(sys.argv[1:])
