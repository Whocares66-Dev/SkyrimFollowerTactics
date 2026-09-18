"""Disassemble a function from SkyrimSE.exe by Address Library ID.

    python tools/disasm.py <id> [max_bytes]           one function, by address-library ID
    python tools/disasm.py --offset <hex> [max_bytes] by file-relative RVA
    python tools/disasm.py --lookup <hex_rva>         which ID owns an RVA
    python tools/disasm.py --vtable <id> [count]      the function slots of a vtable, by its ID

Which build: `--version 1.7.104` before the other arguments, or SKYRIM_VERSION;
1.6.1170 otherwise. The exe is C:/Modding/SkyrimVersions/<version>/SkyrimSE.exe
(SKYRIM_EXE overrides) and the database is whichever file under
AddressLibrary/SKSE/Plugins/ names that version, in any of the library's
formats (tools/addrlib.py). IDs are the line's own: Special Edition numbers
for a 1.5 build, Anniversary numbers for 1.6 and 1.7.

Needs `pip install capstone pefile`. The Steam exe's .text is SteamStub-
encrypted (the .bind section holds the entry point), so the exe under
SkyrimVersions is a copy unpacked with Steamless; the installed exe
disassembles to garbage. Stops at the first `ret` that is followed by
padding, or at max_bytes.
"""
import os
import struct
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrlib  # noqa: E402

ARGS = sys.argv[1:]
VERSION, EXE, LIB = addrlib.resolve(ARGS)


def main():
    ids = addrlib.load(LIB)[2]
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    args = ARGS
    if args[0] == "--lookup":
        rva = int(args[1], 16)
        best = max(((i, o) for i, o in ids.items() if o <= rva), key=lambda x: x[1])
        print(f"ID {best[0]} at {best[1]:#x} (+{rva-best[1]:#x})")
        return
    if args[0] == "--vtable":
        # Dump a vtable's function slots: --vtable <id> [count]. Each slot is
        # an absolute address in .rdata; print it as an RVA with its ID.
        rva = ids[int(args[1])]
        n = int(args[2]) if len(args) > 2 else 16
        rev = {o: i for i, o in ids.items()}
        data = pe.get_data(rva, 8 * n)
        for k in range(n):
            v, = struct.unpack_from("<Q", data, 8 * k)
            r = v - base
            print(f"[{k:02X}] {r:#x}" + (f"  ID {rev[r]}" if r in rev else ""))
        return
    if args[0] == "--offset":
        rva = int(args[1], 16); rest = args[2:]
    else:
        rva = ids[int(args[0])]; rest = args[1:]
    maxb = int(rest[0], 0) if rest else 0x600
    code = pe.get_data(rva, maxb)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    rev = {}
    for i, o in ids.items():
        rev.setdefault(o, i)
    for ins in md.disasm(code, rva):
        note = ""
        for tok in ins.op_str.replace(",", " ").split():
            if tok.startswith("0x"):
                try:
                    v = int(tok, 16)
                except ValueError:
                    continue
                if v in rev:
                    note += f"  ; ID {rev[v]}"
                elif base <= v < base + 0x8000000:
                    r = v - base
                    if r in rev:
                        note += f"  ; ID {rev[r]}"
        print(f"{ins.address:#08x}: {ins.mnemonic:8} {ins.op_str}{note}")
        if ins.mnemonic == "ret" or ins.mnemonic == "int3":
            nxt = code[ins.address - rva + ins.size: ins.address - rva + ins.size + 2]
            if nxt[:1] == b"\xcc" or ins.mnemonic == "int3":
                break


if __name__ == "__main__":
    main()
