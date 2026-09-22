"""Read a Skyrim SE save's change forms without the game.

    python tools/ess_scan.py <save prefix> npc <form id>   decode one NPC base record's saved sections
    python tools/ess_scan.py <save prefix> scan            list change forms mentioning FF3F08xx
    options: --folder <saves folder>   (default: the dev MO2 profile's profile-local saves)

The prefix is the start of the file name, e.g. Save9. Written 2026-09-14 to find
why saves stopped loading: `npc 000B9982` showed Jenassa's base record carrying a
spell list the console's addshout had added to a record with none in the plugin,
which the loader reads through a null pointer.

What is known of the format, and how far to trust it:
- The body is LZ4 block compressed (compression type 2); decoded here in pure
  Python, checked against the length the header gives.
- A reference is 3 bytes: the top two bits say how to read the other 22 --
  0 an index into the form ID array, 1 a Skyrim.esm ID, 2 a created form
  (FF000000 | value). Read from the executable (35927, 35928).
- An NPC base record's sections are in the order base data (24 bytes), factions,
  spell list (spells, leveled spells, shouts), AI data (20 bytes), skills. That
  order was fitted to one layout, flags 0x25A, where it summed to the record's
  exact length; a record with attributes or a full name is dumped as hex, not
  guessed at.
- `scan` matches bytes, not structure: float data in placed references matches
  BF 08 xx by chance (it hits in saves older than our runtime forms), so treat a
  hit as a lead to decode, not a finding.
"""

import glob
import os
import struct
import sys
import zlib

DEFAULT_FOLDER = r"C:\project\SkyrimFollowerTactics\MO2\profiles\Default\saves"

NPC_FLAGS = [
    (0x2, "BASE_DATA"),
    (0x4, "ATTRIBUTES"),
    (0x8, "AIDATA"),
    (0x10, "SPELLLIST"),
    (0x20, "FULLNAME"),
    (0x40, "FACTIONS"),
    (0x200, "SKILLS"),
    (0x400, "CLASS"),
    (0x800, "FACE"),
    (0x1000000, "DEFAULT_OUTFIT"),
    (0x2000000, "SLEEP_OUTFIT"),
    (0x4000000, "GENDER"),
    (0x8000000, "RACE"),
]


def lz4_block(src, usize):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i : i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start : start + ml]
        else:
            for k in range(ml):
                dst.append(dst[start + k])
    if len(dst) != usize:
        raise SystemExit(f"lz4: {len(dst)} bytes out, expected {usize}")
    return bytes(dst)


def wstr(b, p):
    n = struct.unpack_from("<H", b, p)[0]
    return b[p + 2 : p + 2 + n].decode("latin-1"), p + 2 + n


def walk(body, pos, count):
    forms = []
    for _ in range(count):
        if pos + 9 > len(body):
            return None
        ref = (body[pos] << 16) | (body[pos + 1] << 8) | body[pos + 2]
        flags = struct.unpack_from("<I", body, pos + 3)[0]
        t, ver = body[pos + 7], body[pos + 8]
        pos += 9
        fmt = {0: "<B", 1: "<H", 2: "<I"}.get(t >> 6)
        if fmt is None:
            return None
        w = struct.calcsize(fmt)
        if pos + 2 * w > len(body):
            return None
        l1 = struct.unpack_from(fmt, body, pos)[0]
        l2 = struct.unpack_from(fmt, body, pos + w)[0]
        pos += 2 * w
        if pos + l1 > len(body):
            return None
        forms.append((ref, flags, t & 0x3F, ver, l2, body[pos : pos + l1]))
        pos += l1
    return forms


class Save:
    def __init__(self, folder, prefix):
        matches = glob.glob(os.path.join(folder, prefix + "_*.ess"))
        if not matches:
            raise SystemExit(f"no {prefix}_*.ess in {folder}")
        self.path = matches[0]
        b = open(self.path, "rb").read()
        p = 13
        hs = struct.unpack_from("<I", b, p)[0]
        p += 4 + hs
        # the screenshot's size and the compression type close the header
        w, h = struct.unpack_from("<II", b, p - 10)
        comp = struct.unpack_from("<H", b, p - 2)[0]
        p += w * h * 4
        ulen, clen = struct.unpack_from("<II", b, p)
        start = p + 8
        raw = b[start : start + clen]
        body = (
            lz4_block(raw, ulen)
            if comp == 2
            else (zlib.decompress(raw) if comp == 1 else raw)
        )

        q = 1  # form version
        pinfo = struct.unpack_from("<I", body, q)[0]
        q += 4
        pstart = q
        self.plugins, self.light = [], []
        n = body[q]
        q += 1
        for _ in range(n):
            s, q = wstr(body, q)
            self.plugins.append(s)
        lc = struct.unpack_from("<H", body, q)[0]
        q += 2
        for _ in range(lc):
            s, q = wstr(body, q)
            self.light.append(s)
        if q < pstart + pinfo:
            q = pstart + pinfo
        table = struct.unpack_from("<10I", body, q)
        formid_off, cf_off, cf_n = table[0], table[4], table[9]
        # The table's offsets count from a point before the compressed body;
        # the one that walks every change form to the end is the right one.
        for base in (0, start - 8, start, p):
            fo, co = formid_off - base, cf_off - base
            if not (0 <= fo < len(body) and 0 <= co < len(body)):
                continue
            forms = walk(body, co, cf_n)
            if forms is None:
                continue
            cnt = struct.unpack_from("<I", body, fo)[0]
            if fo + 4 + 4 * cnt <= len(body):
                self.forms = forms
                self.formids = list(struct.unpack_from(f"<{cnt}I", body, fo + 4))
                return
        raise SystemExit("could not place the change forms")

    def decode(self, ref):
        kind, v = ref >> 22, ref & 0x3FFFFF
        if kind == 0:
            return self.formids[v - 1] if 0 < v <= len(self.formids) else 0
        if kind == 1:
            return v
        if kind == 2:
            return 0xFF000000 | v
        return None

    def owner(self, fid):
        if fid is None:
            return "?"
        top = fid >> 24
        if top == 0xFF:
            return "created (runtime)"
        if top == 0xFE:
            i = (fid >> 12) & 0xFFF
            return (
                self.light[i]
                if i < len(self.light)
                else f"LIGHT SLOT {i} NOT IN THIS SAVE"
            )
        return (
            self.plugins[top]
            if top < len(self.plugins)
            else f"INDEX {top:02X} NOT IN THIS SAVE"
        )

    def describe(self, ref):
        fid = self.decode(ref)
        text = f"{fid:08X}" if fid is not None else "undecodable"
        return f"{text}  [{self.owner(fid)}]"


def read_ref(d, p):
    return (d[p] << 16) | (d[p + 1] << 8) | d[p + 2], p + 3


def read_vsval(d, p):
    b0 = d[p]
    size = b0 & 3
    if size == 0:
        return b0 >> 2, p + 1
    if size == 1:
        return struct.unpack_from("<H", d, p)[0] >> 2, p + 2
    return (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16)) >> 2, p + 3


def npc(save, target):
    for ref, flags, typ, ver, l2, data in save.forms:
        if save.decode(ref) != target:
            continue
        d = zlib.decompress(data) if l2 else data
        names = [name for bit, name in NPC_FLAGS if flags & bit]
        print(
            f"  {target:08X}: type {typ} flags {flags:08X} ({', '.join(names)}) version {ver}, {len(d)} bytes"
        )
        if flags & (0x4 | 0x20):
            print(
                "  attributes or a full name present: section order not established; hex only"
            )
            print("  hex:", d.hex(" "))
            return
        p = 24 if flags & 0x2 else 0
        if flags & 0x40:
            n, p = read_vsval(d, p)
            print(f"  factions: {n}")
            for _ in range(n):
                r, p = read_ref(d, p)
                rank = struct.unpack_from("<b", d, p)[0]
                p += 1
                print(f"    {save.describe(r)} rank {rank}")
        if flags & 0x10:
            for label in ("spells", "leveled spells", "shouts"):
                n, p = read_vsval(d, p)
                print(f"  {label}: {n}")
                for _ in range(n):
                    r, p = read_ref(d, p)
                    print(f"    {save.describe(r)}")
        if flags & 0x8:
            p += 20
        print(f"  {len(d) - p} bytes after (skills and later sections)")
        return
    print(f"  no change form for {target:08X}")


def scan(save):
    found = 0
    for ref, flags, typ, _ver, l2, data in save.forms:
        try:
            d = zlib.decompress(data) if l2 else data
        except zlib.error:
            d = data
        hits = [i for i in range(len(d) - 2) if d[i] == 0xBF and d[i + 1] == 0x08]
        if hits:
            found += 1
            print(
                f"  {save.describe(ref)}: type {typ} flags {flags:08X}, "
                + ", ".join(f"+{i}=FF3F08{d[i + 2]:02X}" for i in hits[:6])
            )
    print(f"  {found} change form(s) with a byte match")


def main():
    args = sys.argv[1:]
    folder = DEFAULT_FOLDER
    if "--folder" in args:
        i = args.index("--folder")
        folder = args[i + 1]
        del args[i : i + 2]
    if (
        len(args) < 2
        or args[1] not in ("npc", "scan")
        or (args[1] == "npc" and len(args) < 3)
    ):
        raise SystemExit(__doc__)
    save = Save(folder, args[0])
    print(
        f"{os.path.basename(save.path)}: {len(save.plugins)} plugins + {len(save.light)} light, "
        f"{len(save.forms)} change forms, form ID array {len(save.formids)}"
    )
    if args[1] == "npc":
        npc(save, int(args[2], 16))
    else:
        scan(save)


if __name__ == "__main__":
    main()
