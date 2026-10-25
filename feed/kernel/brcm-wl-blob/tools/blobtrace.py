#!/usr/bin/env python3
"""Instrument the vendor core object for MMIO record/replay (see src/wltrace.c).

The core reads and writes device registers with plain ldr/str instructions on the ioremapped BAR,
so there is no software hook to log them.  This rewrites every single-register load/store in the
code sections into a branch to a per-site thunk appended to the same section; the thunk saves the
register file and flags, calls wltrace_emulate(frame, insn) which decodes and performs the original
access (logging or replaying it when the address is inside a registered window), restores the
registers and branches back.

Sites left alone (they can never touch a register window or cannot be relocated into a thunk):
pc-relative loads, instructions using sp or pc, ldm/stm, ldrd/strd, ldrex/strex, unprivileged
(ldrt..) forms, words carrying a relocation, and data words inside code ($d mapping symbols).

Usage: blobtrace.py in.o out.o
"""
import struct, sys

SHT_NOBITS, SHT_REL = 8, 9
R_ARM_CALL = 28
CODE_SECTIONS = (".text", ".text.fastpath")

data = bytearray(open(sys.argv[1], "rb").read())
(e_shoff,) = struct.unpack_from("<I", data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2e)
SH = ("name", "type", "flags", "addr", "offset", "size", "link", "info", "addralign", "entsize")
secs = [dict(zip(SH, struct.unpack_from("<10I", data, e_shoff + i * e_shentsize))) for i in range(e_shnum)]
content = [bytearray() if s["type"] == SHT_NOBITS or i == 0 else bytearray(data[s["offset"]:s["offset"] + s["size"]])
           for i, s in enumerate(secs)]
shstr = secs[e_shstrndx]
def cstr(off):
    end = data.index(b"\0", off); return data[off:end].decode()
names = [cstr(shstr["offset"] + s["name"]) for s in secs]
idx = {n: i for i, n in enumerate(names)}
symtab_i = idx[".symtab"]; symtab = secs[symtab_i]; strtab_i = symtab["link"]
nsyms = symtab["size"] // 16
syms = []
for i in range(nsyms):
    name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, symtab["offset"] + i * 16)
    syms.append((name, value, size, info, other, shndx, cstr(secs[strtab_i]["offset"] + name)))

def data_ranges(sec_i):
    """[lo, hi) ranges of literal pools / tables inside a code section, from $a/$d mapping symbols."""
    marks = sorted((s[1], s[6]) for s in syms if s[5] == sec_i and s[6] in ("$a", "$d", "$t"))
    out, start = [], None
    for v, k in marks:
        if k == "$d" and start is None: start = v
        elif k != "$d" and start is not None: out.append((start, v)); start = None
    if start is not None: out.append((start, secs[sec_i]["size"]))
    return out

def reloc_offsets(sec_i):
    out = set()
    for j, s in enumerate(secs):
        if s["type"] == SHT_REL and s["info"] == sec_i:
            for k in range(0, s["size"], 8):
                out.add(struct.unpack_from("<I", content[j], k)[0])
    return out

def classify(w):
    """Return a short name if w is a patchable single load/store, else the reason it is skipped."""
    cond = w >> 28
    if cond == 0xF: return "uncond"
    b = (w >> 25) & 7
    Rn, Rt = (w >> 16) & 15, (w >> 12) & 15
    P, W = (w >> 24) & 1, (w >> 21) & 1
    if b in (2, 3):                                   # single data transfer: ldr/str/ldrb/strb
        if b == 3 and (w >> 4) & 1: return None       # media instruction
        if P == 0 and W == 1: return "ldrt"
        if Rn == 15: return "literal"
        if Rn == 13 or Rt in (13, 15) or (b == 3 and (w & 15) in (13, 15)): return "sp/pc"
        return "sdt"
    if b == 0 and (w >> 4) & 0xF in (0xB, 0xD, 0xF):  # extra load/store: ldrh/strh/ldrsb/ldrsh/ldrd/strd
        op2, L, I = (w >> 5) & 3, (w >> 20) & 1, (w >> 22) & 1
        if op2 >= 2 and L == 0: return "ldrd/strd"
        if P == 0 and W == 1: return "ldrt"
        if Rn == 15: return "literal"
        if Rn == 13 or Rt in (13, 15) or (not I and (w & 15) in (13, 15)): return "sp/pc"
        if not I and (w >> 8) & 15: return None
        return "xls"
    return None

# thunk: 11 words, the bl at word 6 gets an R_ARM_CALL relocation to wltrace_emulate
def thunk_words(insn, site, thunk):
    def mov16(opc, imm): return opc | ((imm >> 12) << 16) | (1 << 12) | (imm & 0xFFF)   # into r1
    back = ((site + 4) - (thunk + 40 + 8)) >> 2
    return [0xE92D5FFF,                        # push {r0-r12, lr}
            0xE10F0000,                        # mrs  r0, cpsr
            0xE52D0004,                        # push {r0}
            0xE1A0000D,                        # mov  r0, sp          ; frame: cpsr, r0..r12, lr
            mov16(0xE3000000, insn & 0xFFFF),  # movw r1, #insn[15:0]
            mov16(0xE3400000, insn >> 16),     # movt r1, #insn[31:16]
            0xEBFFFFFE,                        # bl   wltrace_emulate (reloc)
            0xE49D0004,                        # pop  {r0}
            0xE12CF000,                        # msr  cpsr_fs, r0     ; flags back
            0xE8BD5FFF,                        # pop  {r0-r12, lr}
            0xEA000000 | (back & 0xFFFFFF)]    # b    site+4
THUNK_WORDS = 11

# new symbol: wltrace_emulate, UND GLOBAL NOTYPE
sym_i = nsyms
content[strtab_i] += b"\0" if content[strtab_i][-1:] != b"\0" else b""
name_off = len(content[strtab_i]); content[strtab_i] += b"wltrace_emulate\0"
content[symtab_i] += struct.pack("<IIIBBH", name_off, 0, 0, 0x10, 0, 0)
secs[symtab_i]["size"] += 16; secs[strtab_i]["size"] = len(content[strtab_i])

report = {}
for si, sname in enumerate(names):
    if sname not in CODE_SECTIONS: continue
    code = content[si]; size = len(code)
    rel_i = next((j for j, s in enumerate(secs) if s["type"] == SHT_REL and s["info"] == si), None)
    assert rel_i is not None, f"no relocation section for {sname}"
    relocs = reloc_offsets(si); dranges = data_ranges(si)
    stats = {"patched": 0, "data": 0, "reloc": 0}
    thunks = bytearray(); new_rel = bytearray()
    thunk_base = (size + 3) & ~3
    code += b"\0" * (thunk_base - size)
    di = 0
    for off in range(0, size, 4):
        while di < len(dranges) and off >= dranges[di][1]: di += 1
        if di < len(dranges) and dranges[di][0] <= off: stats["data"] += 1; continue
        w = struct.unpack_from("<I", code, off)[0]
        kind = classify(w)
        if kind is None: continue
        if kind not in ("sdt", "xls"): stats[kind] = stats.get(kind, 0) + 1; continue
        if off in relocs: stats["reloc"] += 1; continue
        t = thunk_base + len(thunks)
        struct.pack_into("<I", code, off, (w & 0xF0000000) | 0x0A000000 | (((t - (off + 8)) >> 2) & 0xFFFFFF))
        for x in thunk_words(w, off, t): thunks += struct.pack("<I", x)
        new_rel += struct.pack("<II", t + 24, (sym_i << 8) | R_ARM_CALL)
        stats["patched"] += 1
    code += thunks
    secs[si]["size"] = len(code)
    content[rel_i] += new_rel; secs[rel_i]["size"] = len(content[rel_i])
    assert len(thunks) + size < (1 << 25), "section grew beyond branch range"
    report[f"{sname}#{si}"] = stats

# relayout: sections in the original order, headers at the end
out = bytearray(data[:52])
pos = 52
for i, s in enumerate(secs):
    if i == 0: continue
    al = max(s["addralign"], 1) if s["type"] != SHT_NOBITS else 1
    pos = (pos + al - 1) // al * al
    out += b"\0" * (pos - len(out))
    s["offset"] = pos
    if s["type"] != SHT_NOBITS:
        out += content[i]; pos += len(content[i])
pos = (pos + 3) & ~3; out += b"\0" * (pos - len(out))
struct.pack_into("<I", out, 0x20, pos)
for s in secs:
    out += struct.pack("<10I", *[s[k] for k in SH])
open(sys.argv[2], "wb").write(out)
for sname, st in report.items():
    print(f"{sname}: " + " ".join(f"{k}={v}" for k, v in sorted(st.items())))
