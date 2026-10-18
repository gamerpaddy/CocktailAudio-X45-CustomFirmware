#!/usr/bin/env python3
"""Compare the symbol checksums a kernel module was built with against a Module.symvers file.

    chkversions.py Module.symvers module.ko

Prints every checksum that differs (MISMATCH) or that the file does not know (missing), then one summary line:
"ok N bad M missing K". A module whose checksums differ is refused by the kernel when it is loaded.
"""
import struct
import sys

sym = {}
for ln in open(sys.argv[1]):
    p = ln.split('\t')
    sym[p[1]] = int(p[0], 16)
d = open(sys.argv[2], 'rb').read()
shoff, = struct.unpack_from('<I', d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
stro = secs[shstrndx][4]


def name_of(o):
    e = d.index(b'\0', stro + o)
    return d[stro + o:e].decode()


ok = bad = miss = 0
for s in secs:
    if name_of(s[0]) == '__versions':
        off, size = s[4], s[5]
        for j in range(0, size, 64):
            crc, = struct.unpack_from('<I', d, off + j)
            name = d[off + j + 4:off + j + 64].split(b'\0')[0].decode()
            if name not in sym:
                miss += 1
                print('missing', name)
            elif sym[name] == crc:
                ok += 1
            else:
                bad += 1
                print('MISMATCH', name, hex(crc), hex(sym[name]))
print('ok', ok, 'bad', bad, 'missing', miss)
