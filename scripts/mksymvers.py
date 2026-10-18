#!/usr/bin/env python3
"""Rebuild Module.symvers for the X45's running kernel.

The vendor kernel is built with CONFIG_MODVERSIONS, so an out-of-tree module needs the CRC of
every exported symbol it imports. /proc/kallsyms hides the __crc_* symbols, but the kernel
carries __ksymtab/__kcrctab in memory (and the __start_/__stop_ markers are in kallsyms).

  # on the unit: dump the rodata window that holds the tables
  dd if=/dev/mem bs=4096 skip=$((0x10c00)) count=256 | ...   -> rodata.bin  (phys 0x10c00000 = virt 0x80c00000)
  mksymvers.py rodata.bin 0x80c00000 ksymtab.txt > Module.symvers
"""
import struct, sys

def main(dump, base, markers):
    d = open(dump, 'rb').read()
    base = int(base, 0)
    m = {}
    for ln in open(markers):
        p = ln.split()
        if len(p) == 3:
            m[p[2]] = int(p[0], 16)
    def u32(a): return struct.unpack_from('<I', d, a - base)[0]
    def cstr(a):
        o = a - base
        return d[o:d.index(b'\0', o)].decode()
    out = []
    for kind, lic in (('', 'EXPORT_SYMBOL'), ('_gpl', 'EXPORT_SYMBOL_GPL')):
        s, e = m['__start___ksymtab' + kind], m['__stop___ksymtab' + kind]
        c = m['__start___kcrctab' + kind]
        for i in range((e - s) // 8):
            out.append('0x%08x\t%s\tvmlinux\t%s' % (u32(c + i * 4), cstr(u32(s + i * 8 + 4)), lic))
    sys.stdout.write('\n'.join(out) + '\n')

main(*sys.argv[1:4])
