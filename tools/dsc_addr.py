#!/usr/bin/env python3
"""
DarwinFire: symbolicate a (slid) user PC inside the iOS 16 dyld shared cache extracted to out/dsc.
Finds the image whose __TEXT holds the address and the nearest exported symbol at or below it
(exports trie; local symbols live in the .symbols subcache we did not extract, so a static function
shows as "<nearest export>+off").
Usage: python -I tools/dsc_addr.py DSC_DIR SLIDE ADDR [ADDR ...]   (hex numbers)
"""
import os, struct, sys

LC_SEGMENT_64, LC_DYLD_EXPORTS_TRIE, LC_DYLD_INFO_ONLY = 0x19, 0x80000033, 0x80000022


class Cache:
    def __init__(self, d):
        self.maps = []   # (vmaddr, size, fileoff, fh)
        main = os.path.join(d, 'dyld_shared_cache_arm64')
        for name in sorted(os.listdir(d)):
            if not name.startswith('dyld_shared_cache_arm64') or name.endswith('.symbols'):
                continue
            fh = open(os.path.join(d, name), 'rb')
            h = fh.read(0x200)
            moff, mcnt = struct.unpack_from('<II', h, 0x10)
            fh.seek(moff)
            for i in range(mcnt):
                addr, size, foff, _, _ = struct.unpack('<QQQII', fh.read(32))
                self.maps.append((addr, size, foff, fh))
        fh = open(main, 'rb')
        h = fh.read(0x200)
        ioff, icnt = struct.unpack_from('<II', h, 0x1c0)
        if not icnt:
            ioff, icnt = struct.unpack_from('<II', h, 0x18)
        fh.seek(ioff)
        self.images = []
        for i in range(icnt):
            addr, _, _, poff, _ = struct.unpack('<QQQII', fh.read(32))
            self.images.append([addr, poff, None])
        for im in self.images:
            fh.seek(im[1])
            im[1] = fh.read(256).split(b'\0')[0].decode()

    def read(self, addr, n):
        for a, s, o, fh in self.maps:
            if a <= addr < a + s:
                fh.seek(o + addr - a)
                return fh.read(n)
        return None

    def image_for(self, addr):
        best = None
        for base, path, _ in self.images:
            if base <= addr and (best is None or base > best[0]):
                best = (base, path)
        return best

    def exports(self, base):
        hdr = self.read(base, 32)
        ncmds, sizeofcmds = struct.unpack_from('<II', hdr, 16)
        cmds = self.read(base + 32, sizeofcmds)
        linkedit = text_size = None
        trie = None
        off = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', cmds, off)
            if cmd == LC_SEGMENT_64:
                seg = cmds[off + 8:off + 24].split(b'\0')[0]
                vmaddr, vmsize, fileoff = struct.unpack_from('<QQQ', cmds, off + 24)
                if seg == b'__LINKEDIT':
                    linkedit = (vmaddr, fileoff)
                if seg == b'__TEXT':
                    text_size = vmsize
            elif cmd == LC_DYLD_EXPORTS_TRIE:
                trie = struct.unpack_from('<II', cmds, off + 8)
            elif cmd == LC_DYLD_INFO_ONLY:
                eo, es = struct.unpack_from('<II', cmds, off + 40)
                if es:
                    trie = (eo, es)
            off += size
        syms = []
        if trie and linkedit:
            data = self.read(linkedit[0] + trie[0] - linkedit[1], trie[1])

            def uleb(p):
                r = s = 0
                while True:
                    b = data[p]
                    p += 1
                    r |= (b & 0x7f) << s
                    s += 7
                    if b < 0x80:
                        return r, p

            stack = [(0, '')]
            while stack:
                p, name = stack.pop()
                tsize, p2 = uleb(p)
                if tsize:
                    flags, q = uleb(p2)
                    if not flags & 8:   # not re-export
                        o, _ = uleb(q)
                        syms.append((base + o, name))
                p = p2 + tsize
                n = data[p]
                p += 1
                for _ in range(n):
                    e = data.index(b'\0', p)
                    edge = data[p:e].decode('latin1')
                    child, p = uleb(e + 1)
                    stack.append((child, name + edge))
        return text_size, sorted(syms)


def main():
    c = Cache(sys.argv[1])
    slide = int(sys.argv[2], 16)
    for a in sys.argv[3:]:
        addr = int(a, 16) - slide
        img = c.image_for(addr)
        if not img:
            print(a, '-> not in cache')
            continue
        size, syms = c.exports(img[0])
        near = [s for s in syms if s[0] <= addr]
        sym = '%s+0x%x' % (near[-1][1], addr - near[-1][0]) if near else '?'
        inside = size is None or addr < img[0] + size
        print('%s -> unslid 0x%x  %s +0x%x  %s%s' % (a, addr, img[1], addr - img[0], sym, '' if inside else '  (past __TEXT?)'))


if __name__ == '__main__':
    main()
