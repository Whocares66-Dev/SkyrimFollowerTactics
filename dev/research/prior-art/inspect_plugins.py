"""Read-only record-header inventory, not a replacement for a full plugin resolver.

Runs against the already extracted local research copies. Reports master-relative
record identity and editor IDs; does not interpret conditions, VMAD, or winners.
"""

from collections import Counter
import json
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent
PLUGINS = {
    "sff": ROOT / "sff/Main/Simple Follower Framework.esp",
    "aft": ROOT / "aft/AmazingFollowerTweaks.esp",
    "nff": ROOT / "nff/00 Main SSE/nwsFollowerFramework.esp",
    "lucien": ROOT / "lucien/Lucien.esp",
}


def records(data, start=0, end=None):
    end = len(data) if end is None else end
    pos = start
    while pos < end:
        assert pos + 24 <= end
        kind, size = struct.unpack_from("<4sI", data, pos)
        if kind == b"GRUP":
            assert size >= 24 and pos + size <= end
            yield from records(data, pos + 24, pos + size)
            pos += size
            continue
        flags, form = struct.unpack_from("<II", data, pos + 8)
        assert pos + 24 + size <= end
        body = data[pos + 24 : pos + 24 + size]
        if flags & 0x40000:
            expected = struct.unpack_from("<I", body)[0]
            body = zlib.decompress(body[4:])
            assert len(body) == expected
        fields = []
        offset = 0
        extended = None
        while offset < len(body):
            tag, length = struct.unpack_from("<4sH", body, offset)
            offset += 6
            if tag == b"XXXX":
                extended = struct.unpack_from("<I", body, offset)[0]
                offset += length
                continue
            length = extended if extended is not None else length
            extended = None
            assert offset + length <= len(body)
            fields.append((tag, body[offset : offset + length]))
            offset += length
        yield kind.decode(), form, fields
        pos += 24 + size
    assert pos == end


def main():
    summary = {}
    for name, path in PLUGINS.items():
        rows = list(records(path.read_bytes()))
        masters = [v.rstrip(b"\0").decode() for k, v in rows[0][2] if k == b"MAST"]
        identity = masters + [path.name]
        details = []
        for kind, form, fields in rows[1:]:
            edid = next(
                (
                    v.rstrip(b"\0").decode("utf-8", "replace")
                    for k, v in fields
                    if k == b"EDID"
                ),
                "",
            )
            source = identity[form >> 24]
            details.append(
                dict(
                    type=kind,
                    form=f"{form & 0xFFFFFF:06X}:{source}",
                    editorid=edid,
                    override=source != path.name,
                )
            )
        (ROOT / (name + "-records.json")).write_text(
            json.dumps(details, indent=2), encoding="utf-8"
        )
        summary[name] = dict(
            masters=masters,
            records=len(details),
            override_types=dict(Counter(r["type"] for r in details if r["override"])),
            follower_quest=[r for r in details if r["form"] == "0750BA:Skyrim.esm"],
        )
    (ROOT / "plugin-summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
