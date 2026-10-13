#!/usr/bin/env python3
"""Minimal romfs reader for the X45 .pkg firmware package files: list / extract.

The X45 firmware package is a plain romfs image (volume name X20_PKG) with no
signature - only the 32-bit header checksum, which genromfs computes.

Usage:  romfs.py <image> list
        romfs.py <image> extract <outdir>
"""
import sys, os, struct

TYPES = {0: 'hardlink', 1: 'dir', 2: 'file', 3: 'symlink',
         4: 'blockdev', 5: 'chardev', 6: 'socket', 7: 'fifo'}


def align16(n):
    return (n + 15) & ~15


def cstr(buf, off):
    end = buf.index(b'\0', off)
    return buf[off:end].decode('utf-8', 'replace'), align16(end + 1 - off) + off


class Romfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(16)
        if hdr[:8] != b'-rom1fs-':
            raise SystemExit('not a romfs image: ' + path)
        self.size, self.checksum = struct.unpack('>II', hdr[8:16])
        self.f.seek(16)
        blk = self.f.read(256)
        self.volname, nxt = cstr(blk, 0)
        self.root = 16 + nxt

    def read_hdr(self, off):
        self.f.seek(off)
        blk = self.f.read(272)
        nextoff, spec, size, chk = struct.unpack('>IIII', blk[:16])
        name, after = cstr(blk, 16)
        return {
            'next': nextoff & ~0xF,
            'exec': bool(nextoff & 0x8),
            'type': nextoff & 0x7,
            'spec': spec,
            'size': size,
            'name': name,
            'data': off + after,
        }

    def walk(self, off, prefix=''):
        seen = set()
        while off:
            if off in seen:
                break
            seen.add(off)
            e = self.read_hdr(off)
            if e['name'] not in ('.', '..'):
                path = prefix + '/' + e['name']
                yield path, e
                if e['type'] == 1:
                    yield from self.walk(e['spec'], path)
            off = e['next']

    def data(self, e):
        self.f.seek(e['data'])
        return self.f.read(e['size'])


def main():
    img, mode = sys.argv[1], sys.argv[2]
    r = Romfs(img)
    print(f'# volume={r.volname!r} size={r.size} checksum=0x{r.checksum:08x}',
          file=sys.stderr)
    if mode == 'list':
        for path, e in r.walk(r.root):
            extra = ''
            if e['type'] == 3:
                extra = ' -> ' + r.data(e).split(b'\0')[0].decode('utf-8', 'replace')
            print(f'{TYPES.get(e["type"], "?"):8} '
                  f'{"x" if e["exec"] else "-"} {e["size"]:>12} {path}{extra}')
    elif mode == 'extract':
        out = sys.argv[3]
        for path, e in r.walk(r.root):
            dest = os.path.join(out, path.lstrip('/'))
            if e['type'] == 1:
                os.makedirs(dest, exist_ok=True)
            elif e['type'] == 2:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, 'wb') as fh:
                    fh.write(r.data(e))
        print('extracted to ' + out, file=sys.stderr)
    else:
        raise SystemExit('mode must be list or extract')


if __name__ == '__main__':
    main()
