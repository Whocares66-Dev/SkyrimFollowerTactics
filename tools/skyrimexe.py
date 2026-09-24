"""Read code and data out of a SkyrimSE.exe image, by Address Library ID.

The analysis is written once over `Image.read(rva, n)`; `FileImage` reads
an unpacked exe on disk and `LiveImage` the running process (whose code is
decrypted in memory, which is why it exists). disasm.py and livedisasm.py
are the two command lines over them, sharing `run`:

    <id> [max_bytes]           one function, by address-library ID
    --offset <hex> [max_bytes] by file-relative RVA (--rva is the same)
    --lookup <hex_rva>         which ID owns an RVA
    --vtable <id> [count]      the function slots of a vtable, by its ID
    --refs <id | hex_rva>      every instruction in .text that reaches it: calls, jumps, rip-relative operands
    --string <text>            where a NUL-terminated string sits, and its refs
    --name <text>              the IDs whose name holds the text (tools/names.py)
    --match <build> <id>       the functions here shaped like <id> in <build>
    --bytes <hex_rva> <n>      raw bytes

`--match` is how a function is found across the SE/AE line, where the IDs do
not correspond: a body is normalised by making every address the same, then
every function of this build is compared against it. An exact match is the
same function or an identical twin; `--refs` on each tells which.

Every ID printed carries the name its source gives it, where one does
(tools/names.py): CommonLib's function or table, or our Addresses.h entry.

A body stops at the first ret/int3 followed by padding, or at max_bytes; a
shape also stops at a jmp followed by padding, so a tail jump ends one.
Needs `pip install capstone pefile`.
"""

import bisect
import re
import struct

import capstone
import pefile

import addrlib
import names


class Image:
    def __init__(self, version, lib):
        self.version = version
        self.ids = addrlib.load(lib)[2]
        self.rev = {}
        for i, o in self.ids.items():
            self.rev.setdefault(o, i)
        self.names = names.load(version)
        self.owners = sorted(self.ids.items(), key=lambda kv: kv[1])
        self.offs = [o for _, o in self.owners]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        # The section table, from the headers this image reads: the same
        # bytes on disk and in memory. A Steamless-unpacked exe carries a
        # second, tiny `.text` after the real one; the first of a name wins.
        self.sections = {}
        for s in pefile.PE(data=self.read(0, 0x1000), fast_load=True).sections:
            self.sections.setdefault(
                s.Name.rstrip(b"\0").decode(), (s.VirtualAddress, s.Misc_VirtualSize)
            )

    def read(self, rva, n):
        raise NotImplementedError

    def section(self, name):
        start, size = self.sections[name]
        return start, self.read(start, size)

    def owner(self, rva):
        k = bisect.bisect_right(self.offs, rva) - 1
        return self.owners[k] if k >= 0 else (None, 0)

    def label(self, id_):
        """`ID 16137`, or `ID 16137 addr::kResetInventoryWeight` where the
        source names it."""
        name = self.names.get(id_)
        return f"ID {id_} {name}" if name else f"ID {id_}"

    def id_note(self, v):
        if v in self.rev:
            return f"  ; {self.label(self.rev[v])}"
        if self.base <= v < self.base + 0x8000000 and (v - self.base) in self.rev:
            return f"  ; {self.label(self.rev[v - self.base])}"
        return ""

    def instructions(self, rva, maxb=0x600, tail_jmp=False, stop=True):
        code = self.read(rva, maxb)
        out = []
        ends = ("ret", "int3", "jmp") if tail_jmp else ("ret", "int3")
        for ins in self.md.disasm(code, rva):
            out.append(ins)
            if stop and ins.mnemonic in ends:
                nxt = code[
                    ins.address - rva + ins.size : ins.address - rva + ins.size + 1
                ]
                if nxt == b"\xcc" or ins.mnemonic == "int3":
                    break
        return out

    def shape(self, rva, maxb=0x800):
        out = []
        for i in self.instructions(rva, maxb, True):
            ops = re.sub(r"rip [+-] 0x[0-9a-f]+", "rip + X", i.op_str)
            out.append(f"{i.mnemonic} {re.sub(r'0x[0-9a-f]{4,}', 'X', ops)}")
        return out

    @staticmethod
    def targets(ins):
        """The RVAs an instruction names: call and jump targets, immediates,
        and what a rip-relative operand reaches (capstone prints that one as
        its displacement, so it is resolved from the instruction's end)."""
        out = set()
        for sign, disp in re.findall(r"rip ([+-]) (0x[0-9a-f]+)", ins.op_str):
            out.add(
                ins.address
                + ins.size
                + (int(disp, 16) if sign == "+" else -int(disp, 16))
            )
        if "rip" not in ins.op_str:
            out.update(int(t, 16) for t in re.findall(r"0x[0-9a-f]+", ins.op_str))
        return out

    def vtable(self, id_, n):
        data = self.read(self.ids[id_], 8 * n)
        return [struct.unpack_from("<Q", data, 8 * k)[0] - self.base for k in range(n)]

    def references(self, target):
        """Every instruction in .text that reaches target through a rel32:
        a call or jump, or a rip-relative operand of any instruction. One
        scan finds them all, because each is a 4-byte displacement counted
        from the instruction's end, which is the displacement's own end
        unless an immediate follows (then 1 or 4 bytes later; those two
        cases are tried too). -> [(instruction, owner id, owner rva)]."""
        start, data = self.section(".text")
        hits = set()
        for k in range(4):
            view = memoryview(data[k : len(data) - ((len(data) - k) % 4)]).cast("i")
            for trailing in (0, 1, 4):
                c = target - start - k - 4 - trailing
                hits.update(
                    start + k + 4 * j for j, v in enumerate(view) if v + 4 * j == c
                )
        out = []
        for disp in sorted(hits):
            oid, ooff = self.owner(disp)
            # The instruction holding the displacement, from its owner's
            # start; an owner with no ID of its own for a later function is
            # read straight through.
            for ins in self.instructions(ooff, disp - ooff + 16, stop=False):
                if ins.address <= disp < ins.address + ins.size:
                    if target in self.targets(ins):
                        out.append((ins, oid, ooff))
                    break
        return out

    def strings(self, text):
        """RVAs of the NUL-terminated string in .rdata and .data."""
        needle = text.encode() + b"\0"
        out = []
        for name in (".rdata", ".data"):
            start, data = self.section(name)
            i = data.find(needle)
            while i != -1:
                if i == 0 or data[i - 1] == 0:
                    out.append(start + i)
                i = data.find(needle, i + 1)
        return out

    def matches(self, want, want_bytes):
        """IDs in .text whose shape is `want`, and failing any, the five
        nearest by difflib ratio among functions of about the size that open
        the same way. Only IDs with room before the next one are read.
        -> [(ratio, id, rva)], exact ones at 1.0."""
        import difflib

        start, size = self.sections[".text"]
        exact, near = [], []
        for k, (i, o) in enumerate(self.owners):
            if not (start <= o < start + size):
                continue
            room = self.offs[k + 1] - o if k + 1 < len(self.offs) else 0x800
            if room < want_bytes * 0.8:
                continue
            got = self.shape(o, min(0x800, int(want_bytes * 1.3) + 64))
            if got == want:
                exact.append((1.0, i, o))
            elif got[:3] == want[:3] and 0.8 <= len(got) / len(want) <= 1.25:
                near.append(
                    (
                        difflib.SequenceMatcher(
                            None, want, got, autojunk=False
                        ).ratio(),
                        i,
                        o,
                    )
                )
        return exact or sorted(near, reverse=True)[:5]


class FileImage(Image):
    def __init__(self, version, exe, lib):
        self.pe = pefile.PE(exe, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        super().__init__(version, lib)

    def read(self, rva, n):
        return self.pe.get_data(rva, n)


class LiveImage(Image):
    def __init__(self, version, lib, process="SkyrimSE.exe"):
        import ctypes
        import ctypes.wintypes as w
        import subprocess

        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {process}", "/FO", "CSV", "/NH"],
            text=True,
        )
        pid = None
        for line in out.splitlines():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) > 1 and parts[0].lower() == process.lower():
                pid = int(parts[1])
        if pid is None:
            raise SystemExit(f"{process} is not running")
        k32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
        self.handle = k32.OpenProcess(
            0x0400 | 0x0010, False, pid
        )  # query information, read memory
        if not self.handle:
            raise SystemExit(f"OpenProcess failed: {ctypes.GetLastError()}")
        mods = (w.HMODULE * 1)()
        needed = w.DWORD()
        psapi.EnumProcessModulesEx.argtypes = [
            w.HANDLE,
            ctypes.POINTER(w.HMODULE),
            w.DWORD,
            ctypes.POINTER(w.DWORD),
            w.DWORD,
        ]
        if not psapi.EnumProcessModulesEx(
            self.handle, mods, ctypes.sizeof(mods), ctypes.byref(needed), 0x03
        ):
            raise SystemExit(f"EnumProcessModules failed: {ctypes.GetLastError()}")
        self.base = ctypes.cast(mods[0], ctypes.c_void_p).value
        self.pid = pid
        k32.ReadProcessMemory.argtypes = [
            w.HANDLE,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._k32, self._ctypes = k32, ctypes
        super().__init__(version, lib)

    def read(self, rva, n):
        ctypes = self._ctypes
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not self._k32.ReadProcessMemory(
            self.handle, ctypes.c_void_p(self.base + rva), buf, n, ctypes.byref(got)
        ):
            raise SystemExit(
                f"ReadProcessMemory at {rva:#x} failed: {ctypes.GetLastError()}"
            )
        return buf.raw[: got.value]


def print_references(img, target):
    found = img.references(target)
    for ins, oid, ooff in found:
        print(
            f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}  in {img.label(oid)} ({ooff:#x} +{ins.address - ooff:#x})"
        )
    print(f"{len(found)} reference(s)")


def target_of(img, text):
    return int(text, 16) if text.startswith("0x") else img.ids[int(text)]


def run(img, args):
    if args[0] == "--lookup":
        rva = int(args[1], 16)
        oid, ooff = img.owner(rva)
        print(f"{img.label(oid)} at {ooff:#x} (+{rva - ooff:#x})")
    elif args[0] == "--vtable":
        n = int(args[2]) if len(args) > 2 else 16
        for k, r in enumerate(img.vtable(int(args[1]), n)):
            print(
                f"[{k:02X}] {r:#x}"
                + (f"  {img.label(img.rev[r])}" if r in img.rev else "")
            )
    elif args[0] == "--refs":
        print_references(img, target_of(img, args[1]))
    elif args[0] == "--string":
        for rva in img.strings(args[1]):
            print(f"{args[1]!r} at {rva:#x}" + img.id_note(rva))
            print_references(img, rva)
    elif args[0] == "--name":
        want = args[1].lower()
        found = sorted((i, n) for i, n in img.names.items() if want in n.lower())
        for i, n in found:
            where = f" at {img.ids[i]:#x}" if i in img.ids else " (not in this build)"
            print(f"ID {i} {n}{where}")
        print(f"{len(found)} name(s) in {img.version}")
    elif args[0] == "--match":
        src = FileImage(*addrlib.resolve(["--version", args[1]]))
        rva = src.ids[int(args[2])]
        want = src.shape(rva)
        want_bytes = sum(len(i.bytes) for i in src.instructions(rva, 0x800, True))
        print(f"{args[1]} ID {args[2]}: {len(want)} instructions, {want_bytes} bytes")
        found = img.matches(want, want_bytes)
        for ratio, i, o in found:
            print(
                f"{img.label(i)} at {o:#x}"
                + ("" if ratio == 1.0 else f"  ({ratio:.0%} alike)")
            )
        print(
            f"{sum(1 for r, _, _ in found if r == 1.0)} exact match(es) in {img.version}"
        )
    elif args[0] == "--bytes":
        print(img.read(int(args[1], 16), int(args[2])).hex(" "))
    else:
        if args[0] in ("--offset", "--rva"):
            rva, rest = int(args[1], 16), args[2:]
        else:
            rva, rest = img.ids[int(args[0])], args[1:]
        maxb = int(rest[0], 0) if rest else 0x600
        for ins in img.instructions(rva, maxb):
            note = "".join(img.id_note(t) for t in sorted(img.targets(ins)))
            print(f"{ins.address:#08x}: {ins.mnemonic:8} {ins.op_str}{note}")
