"""Disassemble a function from SkyrimSE.exe by Address Library ID.

    python tools/disasm.py <id> [max_bytes]           one function, by AE address-library ID
    python tools/disasm.py --offset <hex> [max_bytes] by file-relative RVA
    python tools/disasm.py --lookup <hex_rva>         which ID owns an RVA
    python tools/disasm.py --vtable <id> [count]      the function slots of a vtable, by its ID

Needs `pip install capstone pefile`. Reads versionlib-1-6-1170-0.bin (format v2) and the exe named by the SKYRIM_EXE
environment variable. The Steam exe's .text is SteamStub-encrypted (the .bind
section holds the entry point), so point SKYRIM_EXE at a copy unpacked with
Steamless; the installed exe disassembles to garbage. Stops at the first `ret`
that is followed by padding, or at max_bytes.
"""
import struct, sys, os
import capstone, pefile

EXE = os.environ.get("SKYRIM_EXE", r"C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\SkyrimSE.exe")
LIB = r"AddressLibrary/SKSE/Plugins/versionlib-1-6-1170-0.bin"

def load_lib(path):
    with open(path, "rb") as f:
        d = f.read()
    p = 0
    fmt, = struct.unpack_from("<i", d, p); p += 4
    assert fmt == 2, fmt
    ver = struct.unpack_from("<4i", d, p); p += 16
    tlen, = struct.unpack_from("<i", d, p); p += 4
    p += tlen
    ptr_size, count = struct.unpack_from("<ii", d, p); p += 8
    ids = {}
    prev_id = prev_off = 0
    for _ in range(count):
        t = d[p]; p += 1
        lo, hi = t & 0xF, t >> 4
        if lo == 0:
            i, = struct.unpack_from("<Q", d, p); p += 8
        elif lo == 1:
            i = prev_id + 1
        elif lo == 2:
            i = prev_id + d[p]; p += 1
        elif lo == 3:
            i = prev_id - d[p]; p += 1
        elif lo == 4:
            i = prev_id + struct.unpack_from("<H", d, p)[0]; p += 2
        elif lo == 5:
            i = prev_id - struct.unpack_from("<H", d, p)[0]; p += 2
        elif lo == 6:
            i, = struct.unpack_from("<H", d, p); p += 2
        elif lo == 7:
            i, = struct.unpack_from("<I", d, p); p += 4
        else:
            raise ValueError(lo)
        tmp = (prev_off // ptr_size) if (hi & 8) else prev_off
        if hi & 7 == 0:
            o, = struct.unpack_from("<Q", d, p); p += 8
        elif hi & 7 == 1:
            o = tmp + 1
        elif hi & 7 == 2:
            o = tmp + d[p]; p += 1
        elif hi & 7 == 3:
            o = tmp - d[p]; p += 1
        elif hi & 7 == 4:
            o = tmp + struct.unpack_from("<H", d, p)[0]; p += 2
        elif hi & 7 == 5:
            o = tmp - struct.unpack_from("<H", d, p)[0]; p += 2
        elif hi & 7 == 6:
            o, = struct.unpack_from("<H", d, p); p += 2
        elif hi & 7 == 7:
            o, = struct.unpack_from("<I", d, p); p += 4
        if hi & 8:
            o *= ptr_size
        ids[i] = o
        prev_id, prev_off = i, o
    return ids

def main():
    ids = load_lib(LIB)
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    args = sys.argv[1:]
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
    maxb = int(rest[0]) if rest else 0x600
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

main()
