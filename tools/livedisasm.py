"""Disassemble from the RUNNING SkyrimSE.exe: the code is decrypted in memory.
    python build/livedisasm.py --vtable <id> [count]
    python build/livedisasm.py <id> [max_bytes]
    python build/livedisasm.py --rva <hex> [max_bytes]
"""
import ctypes, ctypes.wintypes as w, struct, sys, os, importlib.util
import capstone

sys.argv_backup = sys.argv
spec = importlib.util.spec_from_file_location("disasm", os.path.join("tools", "disasm.py"))
src = open(os.path.join("tools", "disasm.py"), encoding="utf-8").read().replace("\nmain()\n", "\n")
ns = {}
exec(compile(src, "disasm.py", "exec"), ns)
ids = ns["load_lib"](ns["LIB"])
rev = {}
for i, o in ids.items():
    rev.setdefault(o, i)

k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010

def find_pid(name):
    import subprocess
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True)
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name.lower():
            return int(parts[1])
    raise SystemExit(f"{name} is not running")

pid = find_pid("SkyrimSE.exe")
h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
if not h:
    raise SystemExit(f"OpenProcess failed: {ctypes.GetLastError()}")
mods = (w.HMODULE * 1)()
needed = w.DWORD()
psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, ctypes.POINTER(w.HMODULE), w.DWORD, ctypes.POINTER(w.DWORD), w.DWORD]
if not psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 0x03):
    raise SystemExit(f"EnumProcessModules failed: {ctypes.GetLastError()}")
base = ctypes.cast(mods[0], ctypes.c_void_p).value

def read(rva, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(base + rva), buf, n, ctypes.byref(got)):
        raise SystemExit(f"ReadProcessMemory at {rva:#x} failed: {ctypes.GetLastError()}")
    return buf.raw[:got.value]

args = sys.argv[1:]
print(f"SkyrimSE.exe pid {pid} base {base:#x}")
if args[0] == "--vtable":
    rva = ids[int(args[1])]
    n = int(args[2]) if len(args) > 2 else 16
    data = read(rva, 8 * n)
    for k in range(n):
        v, = struct.unpack_from("<Q", data, 8 * k)
        r = v - base
        print(f"[{k:02X}] {r:#x}" + (f"  ID {rev[r]}" if r in rev else ""))
    raise SystemExit
if args[0] == "--callers":
    # Every `call rel32` / `jmp rel32` in the image that lands on the function: the
    # caller's RVA and the ID that owns it.
    target = ids[int(args[1])]
    lo, hi = 0x1000, 0x1900000
    chunk = 4 << 20
    owners = sorted(ids.items(), key=lambda kv: kv[1])
    offs = [o for _, o in owners]
    import bisect
    def owner(rva):
        k = bisect.bisect_right(offs, rva) - 1
        return owners[k] if k >= 0 else (None, 0)
    found = []
    for start in range(lo, hi, chunk):
        data = read(start, min(chunk + 5, hi - start + 5))
        i = data.find(b"\xe8")
        while i != -1:
            if i + 5 <= len(data):
                rel, = struct.unpack_from("<i", data, i + 1)
                if start + i + 5 + rel == target:
                    found.append(start + i)
            i = data.find(b"\xe8", i + 1)
    for rva in found:
        oid, ooff = owner(rva)
        print(f"call at {rva:#x}  in ID {oid} ({ooff:#x} +{rva-ooff:#x})")
    print(f"{len(found)} caller(s)")
    raise SystemExit
if args[0] == "--refs":
    # Every rip-relative lea/mov (48/4c 8d/8b modrm=xx101) whose target is the address:
    # the sites that take a vtable's or a global's address.
    target = ids[int(args[1])] if not args[1].startswith("0x") else int(args[1], 16)
    lo, hi = 0x1000, 0x1900000
    chunk = 4 << 20
    owners = sorted(ids.items(), key=lambda kv: kv[1])
    offs = [o for _, o in owners]
    import bisect
    def owner(rva):
        k = bisect.bisect_right(offs, rva) - 1
        return owners[k] if k >= 0 else (None, 0)
    found = []
    for start in range(lo, hi, chunk):
        data = read(start, min(chunk + 8, hi - start + 8))
        for i in range(len(data) - 7):
            if data[i] in (0x48, 0x4c) and data[i + 1] in (0x8d, 0x8b) and (data[i + 2] & 0xC7) == 0x05:
                rel, = struct.unpack_from("<i", data, i + 3)
                if start + i + 7 + rel == target:
                    found.append(start + i)
    for rva in found:
        oid, ooff = owner(rva)
        print(f"ref at {rva:#x}  in ID {oid} ({ooff:#x} +{rva-ooff:#x})")
    print(f"{len(found)} ref(s)")
    raise SystemExit
if args[0] == "--bytes":
    rva = int(args[1], 16); n = int(args[2])
    print(read(rva, n).hex(" "))
    raise SystemExit
if args[0] == "--rva":
    rva = int(args[1], 16); rest = args[2:]
else:
    rva = ids[int(args[0])]; rest = args[1:]
maxb = int(rest[0]) if rest else 0x600
code = read(rva, maxb)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for ins in md.disasm(code, rva):
    note = ""
    for tok in ins.op_str.replace(",", " ").replace("[", " ").replace("]", " ").split():
        if tok.startswith("0x"):
            try:
                v = int(tok, 16)
            except ValueError:
                continue
            if v in rev:
                note += f"  ; ID {rev[v]}"
    print(f"{ins.address:#08x}: {ins.mnemonic:8} {ins.op_str}{note}")
    if ins.mnemonic in ("ret", "int3"):
        nxt = code[ins.address - rva + ins.size: ins.address - rva + ins.size + 2]
        if nxt[:1] == b"\xcc" or ins.mnemonic == "int3":
            break
