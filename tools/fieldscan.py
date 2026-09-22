"""Find code that reads a structure's field through a pointer loaded from
another's: `mov rX, [rY + OUTER]`, then, before rX is overwritten, a read
of `[rX + INNER]` for one of the INNER offsets. For the readers of a list
that no function wraps -- an NPC's spell list is `[npc + 0xA8]`, and its
count `[list + 0x18]` -- where --refs has no target to search for.

    python tools/fieldscan.py [--version 1.6.1170] OUTER INNER[,INNER...] [--window N] [--size N] [--sites]

--size keeps only inner accesses of that many bytes (4 for a u32 count).
Prints each function (by Address Library ID) holding a match and how many;
--sites prints every match. A linear window of N instructions (default 24)
after the load is read, so a use after a branch elsewhere is missed and a
register reused on another path may be counted; it is a list of places to
read, not a proof.
"""

import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrlib  # noqa: E402
import skyrimexe  # noqa: E402

import capstone  # noqa: E402
from capstone import x86  # noqa: E402


def main(args):
    img = skyrimexe.FileImage(*addrlib.resolve(args))
    window = 24
    if "--window" in args:
        k = args.index("--window")
        window = int(args[k + 1])
        del args[k : k + 2]
    size = None
    if "--size" in args:
        k = args.index("--size")
        size = int(args[k + 1])
        del args[k : k + 2]
    sites = "--sites" in args
    args = [a for a in args if a != "--sites"]
    outer = int(args[0], 0)
    inner = {int(x, 0) for x in args[1].split(",")}

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    start, data = img.section(".text")

    # mov r64, [base + disp]: REX.W (48-4F), 8B, ModRM, [SIB], disp8 or disp32.
    if outer < 0x80:
        pattern = re.compile(
            rb"[\x48-\x4f]\x8b([\x40-\x7f])" + b"(?:.)?" + re.escape(bytes([outer])),
            re.S,
        )
    else:
        pattern = re.compile(
            rb"[\x48-\x4f]\x8b([\x80-\xbf])"
            + b"(?:.)?"
            + re.escape(outer.to_bytes(4, "little")),
            re.S,
        )

    found = defaultdict(list)
    for m in pattern.finditer(data):
        rva = start + m.start()
        code = data[m.start() : m.start() + 16 * window]
        insns = list(md.disasm(code, rva))
        if not insns:
            continue
        first = insns[0]
        if first.mnemonic != "mov" or len(first.operands) != 2:
            continue
        dst, src = first.operands
        if (
            dst.type != x86.X86_OP_REG
            or src.type != x86.X86_OP_MEM
            or src.mem.disp != outer
        ):
            continue
        reg = dst.reg
        for ins in insns[1:window]:
            hit = False
            for op in ins.operands:
                if (
                    op.type == x86.X86_OP_MEM
                    and op.mem.base == reg
                    and op.mem.index == 0
                    and op.mem.disp in inner
                    and (size is None or op.size == size)
                ):
                    hit = True
            if hit:
                oid, ooff = img.owner(rva)
                found[(oid, ooff)].append((rva, ins))
                break
            # rX written: the chain ends. (Reads through it were checked above.)
            written = ins.regs_access()[1]
            if (
                reg in written
                or ins.mnemonic in ("ret", "jmp", "call")
                and ins.mnemonic != "call"
            ):
                break
            if ins.mnemonic == "call" and reg in (
                x86.X86_REG_RAX,
                x86.X86_REG_RCX,
                x86.X86_REG_RDX,
                x86.X86_REG_R8,
                x86.X86_REG_R9,
                x86.X86_REG_R10,
                x86.X86_REG_R11,
            ):
                break

    for (oid, ooff), hits in sorted(found.items(), key=lambda kv: kv[0][1]):
        print(f"ID {oid} at {ooff:#x}: {len(hits)}")
        if sites:
            for rva, ins in hits:
                print(
                    f"    load at {rva:#x}, then {ins.address:#x}: {ins.mnemonic} {ins.op_str}"
                )
    print(
        f"{sum(len(h) for h in found.values())} match(es) in {len(found)} function(s)"
    )


if __name__ == "__main__":
    main(sys.argv[1:])
