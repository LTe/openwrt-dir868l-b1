#!/usr/bin/env python3
"""Turn the shipped wl_ap.ko into a relinkable core object (wlc_blob.o):

 * every function in the Linux glue ranges (wl_linux.c: wl_*, init_module, pci probe...) is removed
   as a definition: symbols referenced by core code become UND GLOBAL (the new glue provides them),
   the rest become WEAK ABS 0 renamed __dead_<name> so that neither ld nor the module loader care about them;
 * relocations located inside the glue code ranges are turned into R_ARM_NONE (dead code);
 * relocations from data tables (.rodata/.data/this_module) to glue symbols are turned into R_ARM_NONE;
 * imports that are no longer referenced by any relocation become LOCAL ABS 0;
 * imports listed in RENAME are redirected (by rewriting .strtab in place is impossible, so we
   append new names to .strtab) to compat implementations provided by the new glue;
 * module-only sections are dropped afterwards with objcopy (see tools/mkblob.sh).

Usage: blobsurgery.py in.ko out.o
"""
import struct, sys, re
SHN_UNDEF, SHN_ABS = 0, 0xfff1
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_FUNC, STT_SECTION = 2, 3
R_ARM_NONE = 0

GLUE = {  # section name: list of (lo, hi) half-open offsets of wl_linux.c code
    ".text": [(0x131288, 0x13407c)],
    ".text.fastpath": [(0x15cc, 0x1d4c)],
    ".init.text": [(0, 0xd8)], ".exit.text": [(0, 0xc)],
    ".devinit.text": [(0, 0xac)], ".devexit.text": [(0, 0x98)],
}
DROP_SECTIONS = {"__versions", ".modinfo", "__param", ".gnu.linkonce.this_module"}
# imports redirected to compat implementations in the new glue (kernel ABI changed or symbol gone)
RENAME = {
    "outer_cache": "wlblob_outer_cache",
    "__memzero": "wlblob_memzero",
    "skb_push": "wlblob_skb_push",
    "skb_pull": "wlblob_skb_pull",
    "warn_slowpath_null": "wlblob_warn_slowpath_null",
    "__copy_from_user": "wlblob_copy_from_user",
    "__copy_to_user": "wlblob_copy_to_user",
    "printk": "wlblob_printk",
}

# optional per-blob profile: python dict literal in a file (argv[3]) overriding GLUE / RENAME / DROP_SECTIONS
if len(sys.argv) > 3:
    _prof = eval(open(sys.argv[3]).read(), {})
    GLUE = _prof.get("GLUE", GLUE); RENAME = _prof.get("RENAME", RENAME)
    DROP_SECTIONS = set(_prof.get("DROP_SECTIONS", DROP_SECTIONS))

data = bytearray(open(sys.argv[1], "rb").read())
(e_shoff,) = struct.unpack_from("<I", data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2e)
def sh(i):
    return dict(zip(("name","type","flags","addr","offset","size","link","info","addralign","entsize"),
                    struct.unpack_from("<10I", data, e_shoff + i*e_shentsize)))
secs = [sh(i) for i in range(e_shnum)]
shstr = secs[e_shstrndx]
def cstr(off):
    end = data.index(b"\0", off); return data[off:end].decode()
names = [cstr(shstr["offset"] + s["name"]) for s in secs]
idx = {n: i for i, n in enumerate(names)}
symtab = secs[idx[".symtab"]]; strtab = secs[symtab["link"]]
nsyms = symtab["size"] // 16
def sym(i):
    name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, symtab["offset"] + i*16)
    return dict(name=name, value=value, size=size, info=info, other=other, shndx=shndx, sname=cstr(strtab["offset"]+name))
def put_sym(i, s):
    struct.pack_into("<IIIBBH", data, symtab["offset"] + i*16, s["name"], s["value"], s["size"], s["info"], s["other"], s["shndx"])
syms = [sym(i) for i in range(nsyms)]

def in_glue(secname, off):
    return any(lo <= off < hi for lo, hi in GLUE.get(secname, []))

glue_syms = set()
for i, s in enumerate(syms):
    if s["info"] & 0xf == STT_FUNC and s["shndx"] < e_shnum and in_glue(names[s["shndx"]], s["value"]):
        glue_syms.add(i)
    elif s["shndx"] < e_shnum and names[s["shndx"]] in DROP_SECTIONS and (s["info"] & 0xf) != STT_SECTION:
        glue_syms.add(i)      # symbols living in dropped module-only sections (__this_module, __param_*, ...)

# pass 1: walk relocations, neutralise dead ones, count live references per symbol
live_refs = {i: 0 for i in range(nsyms)}
referenced_from_core = set()
neutralised = 0
for si, s in enumerate(secs):
    if s["type"] != 9 or names[si][4:] not in idx:   # SHT_REL
        continue
    target = names[si][4:]
    dropped_target = target in DROP_SECTIONS
    for k in range(s["size"] // 8):
        off = s["offset"] + k*8
        r_offset, r_info = struct.unpack_from("<II", data, off)
        rsym, rtype = r_info >> 8, r_info & 0xff
        dead = dropped_target or in_glue(target, r_offset) or \
               (rsym in glue_syms and not target.startswith(".text")) or \
               (syms[rsym]["shndx"] < e_shnum and names[syms[rsym]["shndx"]] in DROP_SECTIONS)
        if dead:
            struct.pack_into("<II", data, off, r_offset, (rsym << 8) | R_ARM_NONE)   # keep sym for readability
            neutralised += 1
        else:
            live_refs[rsym] += 1
            if rsym in glue_syms:
                referenced_from_core.add(rsym)

# pass 2: rewrite symbols
strtab_extra = bytearray(); extra_base = strtab["size"]
def new_name(n):
    global strtab_extra
    off = extra_base + len(strtab_extra); strtab_extra += n.encode() + b"\0"; return off
report = {"glue_to_und": [], "glue_to_abs": [], "dead_imports": [], "renamed": []}
for i, s in enumerate(syms):
    if i in glue_syms:
        if i in referenced_from_core:
            s.update(value=0, size=0, shndx=SHN_UNDEF, info=(STB_GLOBAL << 4) | 0)   # NOTYPE UND
            report["glue_to_und"].append(s["sname"])
        else:
            s.update(value=0, size=0, shndx=SHN_ABS, info=(STB_WEAK << 4) | 0, name=new_name("__dead_" + s["sname"]))
            report["glue_to_abs"].append(s["sname"])
        put_sym(i, s)
    elif s["shndx"] == SHN_UNDEF and i != 0 and s["sname"]:
        if live_refs[i] == 0:
            s.update(shndx=SHN_ABS, info=(STB_WEAK << 4) | 0, name=new_name("__dead_" + s["sname"])); put_sym(i, s)
            report["dead_imports"].append(s["sname"])
        elif s["sname"] in RENAME:
            s["name"] = new_name(RENAME[s["sname"]]); put_sym(i, s)
            report["renamed"].append(f'{s["sname"]}->{RENAME[s["sname"]]}')
# also demote glue-owned data objects? (left in place: harmless dead data)

# append strtab extension: strtab is usually last-but-one; we must grow it in place -> rebuild file
if strtab_extra:
    # rebuild: shift everything after strtab end by len(strtab_extra) (sections are in file order)
    ins = strtab["offset"] + strtab["size"]; grow = len(strtab_extra)
    data[ins:ins] = strtab_extra
    for si, s in enumerate(secs):
        if s["offset"] >= ins and si != symtab["link"] and s["type"] != 8:   # SHT_NOBITS
            s["offset"] += grow
        if si == symtab["link"]:
            s["size"] += grow
        struct.pack_into("<10I", data, (e_shoff + grow if e_shoff >= ins else e_shoff) + si*e_shentsize,
                         *[s[k] for k in ("name","type","flags","addr","offset","size","link","info","addralign","entsize")])
    if e_shoff >= ins:
        struct.pack_into("<I", data, 0x20, e_shoff + grow)

open(sys.argv[2], "wb").write(data)
print(f"glue symbols: {len(glue_syms)}  relocs neutralised: {neutralised}")
for k, v in report.items():
    print(f"{k} ({len(v)}): {' '.join(sorted(v))}")
