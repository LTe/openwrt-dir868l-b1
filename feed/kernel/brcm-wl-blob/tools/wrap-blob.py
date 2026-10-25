#!/usr/bin/env python3
"""Interpose ELF32 ARM function symbols, retaining explicit original aliases.

Run after normal blob surgery. Code and relocation addends remain unchanged;
existing references resolve to the new C wrapper, which calls the alias.
"""
import struct
import sys

data = bytearray(open(sys.argv[1], 'rb').read())
assert data[:6] == b'\x7fELF\x01\x01'
assert struct.unpack_from('<H', data, 18)[0] == 40
shoff = struct.unpack_from('<I', data, 32)[0]
shsize, nsh, shstridx = struct.unpack_from('<HHH', data, 46)
assert shsize == 40
sections = [list(struct.unpack_from('<10I', data, shoff + i * 40)) for i in range(nsh)]
contents = [bytearray(data[s[4]:s[4] + s[5]]) if s[1] != 8 and i else bytearray()
            for i, s in enumerate(sections)]
def string(buf, off):
    return bytes(buf[off:buf.index(0, off)]).decode()
names = [string(contents[shstridx], s[0]) for s in sections]
si = names.index('.symtab')
sti = sections[si][6]
symbols = [list(struct.unpack_from('<IIIBBH', contents[si], i))
           for i in range(0, len(contents[si]), 16)]
for target in sys.argv[3:]:
    matches = [(i, s) for i, s in enumerate(symbols)
               if string(contents[sti], s[0]) == target]
    assert len(matches) == 1, (target, matches)
    idx, sym = matches[0]
    assert sym[3] == 0x12 and 0 < sym[5] < nsh, (target, sym)
    refs = []
    for ri, section in enumerate(sections):
        if section[1] != 9:
            continue
        for pos in range(0, len(contents[ri]), 8):
            offset, info = struct.unpack_from('<II', contents[ri], pos)
            if info >> 8 == idx and info & 255:
                # ARM branch relocations must address the function entry.
                assert info & 255 in (28, 29), (target, names[ri], offset, info & 255)
                insn = struct.unpack_from('<I', contents[section[7]], offset)[0]
                assert insn & 0xffffff == 0xfffffe, (target, offset, hex(insn))
                refs.append((names[ri], offset))
    assert refs, target
    alias = sym.copy()
    alias[0] = len(contents[sti])
    contents[sti] += ('wlblob_original_' + target + '\0').encode()
    contents[si] += struct.pack('<IIIBBH', *alias)
    struct.pack_into('<IIIBBH', contents[si], idx * 16, sym[0], 0, 0, 0x10, 0, 0)
    print(target, 'wrapped references:', len(refs))

out = bytearray(data[:52])
for i, section in enumerate(sections):
    if not i:
        continue
    align = max(1, section[8])
    out += bytes((-len(out)) % align)
    section[4] = len(out)
    if section[1] != 8:
        section[5] = len(contents[i])
        out += contents[i]
out += bytes((-len(out)) % 4)
struct.pack_into('<I', out, 32, len(out))
for section in sections:
    out += struct.pack('<10I', *section)
open(sys.argv[2], 'wb').write(out)
