#!/usr/bin/env python3
"""
DarwinFire: put the real dyld_shared_cache into the HFS+ root image (ios16_root.img).

The image was built full: every /System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*
file exists in the catalog with 0 bytes, so dyld reports "no dyld cache". This tool:
  1. empties every file under the DROP directories (size 0, extents cleared, blocks freed in the bitmap;
     names stay, like patch_dyld_hfs.py does for dyld),
  2. writes the 44 cache files (decompressed from system_cryptex.dmg, .symbols skipped) into the freed
     blocks, at most 8 extents per file (no extents-overflow inserts), and fixes their catalog records,
  3. updates freeBlocks in both volume headers.
In-place edits only (no B-tree restructuring). Run with --dry-run first.
Usage: python -I tools/hfs_inject_cache.py IMG DSC_DIR [--grow BLOCKS] [--dry-run]
(DSC_DIR = output of tools/extract_dsc.py)
(--grow appends 4K blocks; the result must still fit the "super" partition, 8707 MiB on fire)
"""
import hashlib, struct, sys

BS = 4096
DROP = [
    'System/Library/LinguisticData', 'System/Library/PreinstalledAssetsV2', 'System/Library/NanoTimeKit',
    'usr/standalone', 'System/Library/Health', 'System/Library/OnBoardingBundles', 'System/Library/Assistant',
    'System/Library/ScreenReader', 'System/Library/NanoPreferenceBundles',
]
CACHE_DIR = 'System/Library/Caches/com.apple.dyld'


class Img:
    def __init__(self, path, write):
        self.f = open(path, 'r+b' if write else 'rb')
        self.f.seek(1024)
        self.vh = bytearray(self.f.read(512))
        assert self.vh[0:2] in (b'H+', b'HX'), 'not HFS+'
        assert struct.unpack('>I', self.vh[40:44])[0] == BS
        self.total = struct.unpack('>I', self.vh[44:48])[0]
        self.alloc = self.fork(0x70)
        self.ext = self.fork(0xC0)
        self.cat = self.fork(0x110)
        self.bitmap = bytearray(self.read_fork(self.alloc, 0, self.alloc[0]))   # whole allocation file

    def grow(self, n, resize=True):
        """add n blocks at the end (the allocation file already has bits for them); the new last block
        holds the alternate volume header, so it is marked used"""
        new = self.total + n
        assert new <= len(self.bitmap) * 8, 'allocation file too small for the new size'
        self.set_bits(self.total, n, 0)
        self.set_bits(new - 1, 1, 1)
        self.total = new
        self.vh[44:48] = struct.pack('>I', new)
        if resize:
            self.f.truncate(new * BS)

    def fork(self, off):
        size = struct.unpack('>Q', self.vh[off:off + 8])[0]
        ext = [struct.unpack('>II', self.vh[off + 16 + 8 * i: off + 24 + 8 * i]) for i in range(8)]
        return size, [e for e in ext if e[1]]

    def fork_disk(self, fork, off):
        """disk byte offset of fork byte `off` (8 inline extents are enough for the system files here)"""
        blk = off // BS
        for start, cnt in fork[1]:
            if blk < cnt:
                return (start + blk) * BS + off % BS
            blk -= cnt
        raise ValueError('offset past inline extents')

    def read_fork(self, fork, off, n):
        out = bytearray()
        while n:
            d = self.fork_disk(fork, off)
            k = min(n, BS - off % BS)
            self.f.seek(d)
            out += self.f.read(k)
            off += k
            n -= k
        return bytes(out)

    def leaf_records(self, fork):
        """yield (disk_offset_of_record, key_bytes, data_offset_in_record, record_bytes) for every leaf record"""
        hdr = self.read_fork(fork, 0, 512)
        first_leaf = struct.unpack('>I', hdr[14 + 10:14 + 14])[0]
        node_size = struct.unpack('>H', hdr[14 + 18:14 + 20])[0]
        node = first_leaf
        while node:
            buf = self.read_fork(fork, node * node_size, node_size)
            flink, _, kind, _, nrec = struct.unpack('>IIbBH', buf[:12])
            assert kind == -1, 'not a leaf node'
            offs = [struct.unpack('>H', buf[node_size - 2 * (i + 1): node_size - 2 * i])[0] for i in range(nrec + 1)]
            for i in range(nrec):
                rec = buf[offs[i]:offs[i + 1]]
                klen = struct.unpack('>H', rec[:2])[0]
                yield self.fork_disk(fork, node * node_size + offs[i]), rec[:2 + klen], 2 + klen, rec
            node = flink

    def set_bits(self, start, cnt, val):
        for b in range(start, start + cnt):
            if val:
                self.bitmap[b >> 3] |= 0x80 >> (b & 7)
            else:
                self.bitmap[b >> 3] &= ~(0x80 >> (b & 7)) & 0xFF


def main():
    img_path, cryptex, dry = sys.argv[1], sys.argv[2], '--dry-run' in sys.argv
    grow = int(sys.argv[sys.argv.index('--grow') + 1]) if '--grow' in sys.argv else 0   # blocks
    im = Img(img_path, not dry)
    if grow:
        im.grow(grow, resize=not dry)   # dry run: simulate in memory only

    # catalog: folders (parent,name)->cnid, file records with their disk location
    folder_by_key, folder_parent, files = {}, {}, []
    for disk, key, doff, rec in im.leaf_records(im.cat):
        parent = struct.unpack('>I', key[2:6])[0]
        nlen = struct.unpack('>H', key[6:8])[0]
        name = key[8:8 + 2 * nlen].decode('utf-16-be', 'replace')
        rtype = struct.unpack('>h', rec[doff:doff + 2])[0]
        if rtype == 1:
            cnid = struct.unpack('>I', rec[doff + 8:doff + 12])[0]
            folder_by_key[(parent, name)] = cnid
            folder_parent[cnid] = parent
        elif rtype == 2:
            files.append((disk + doff, parent, name, bytes(rec[doff:doff + 248])))

    def resolve(path):
        cnid = 2
        for comp in path.split('/'):
            cnid = folder_by_key[(cnid, comp)]
        return cnid

    def under(cnid, roots):
        while cnid in folder_parent and cnid not in roots:
            cnid = folder_parent[cnid]
        return cnid in roots

    drop_roots = {resolve(p) for p in DROP}
    cache_dir = resolve(CACHE_DIR)

    # extents-overflow records per (fileID, forkType)
    overflow = {}
    for disk, key, doff, rec in im.leaf_records(im.ext):
        ftype, fid, sblk = key[2], struct.unpack('>I', key[4:8])[0], struct.unpack('>I', key[8:12])[0]
        overflow.setdefault((fid, ftype), []).extend(
            e for e in (struct.unpack('>II', rec[doff + 8 * i: doff + 8 * i + 8]) for i in range(8)) if e[1])

    # 1. empty the dropped files
    freed, dropped, wipes = 0, 0, []
    for rec_disk, parent, name, rec in files:
        if not under(parent, drop_roots):
            continue
        fid = struct.unpack('>I', rec[8:12])[0]
        for forkoff, ftype in ((88, 0), (168, 0xFF)):
            ext = [struct.unpack('>II', rec[forkoff + 16 + 8 * i: forkoff + 24 + 8 * i]) for i in range(8)]
            for s, c in [e for e in ext if e[1]] + overflow.get((fid, ftype), []):
                im.set_bits(s, c, 0)
                freed += c
            wipes.append(rec_disk + forkoff)
        dropped += 1

    # 2. cache files: catalog records + source sizes. Source = a directory of files already decompressed by
    #    tools/extract_dsc.py (pure-Python LZBITMAP is too slow to stream 2.6 GB here)
    import os
    targets = []
    for rec_disk, parent, name, rec in files:
        if parent == cache_dir and name.startswith('dyld_shared_cache_arm64') and not name.endswith('.symbols'):
            targets.append((name, rec_disk, os.path.getsize(os.path.join(cryptex, name))))
    need = sum((s + BS - 1) // BS for _, _, s in targets)
    free_now = im.total - sum(bin(b).count('1') for b in im.bitmap[:im.total // 8])
    print('dropped files %d, freed %d blocks (%.0f MB); free after drop %d blocks; cache files %d need %d blocks (%.0f MB)'
          % (dropped, freed, freed * BS / 2**20, free_now, len(targets), need, need * BS / 2**20))
    assert len(targets) == 43, 'expected 43 cache files (44 minus .symbols)'
    if need > free_now:
        print('NOT ENOUGH SPACE: short by %d blocks (%.0f MB)' % (need - free_now, (need - free_now) * BS / 2**20))
        sys.exit(2)

    # free runs, biggest first
    runs, b = [], 0
    while b < im.total:
        if im.bitmap[b >> 3] & (0x80 >> (b & 7)):
            b += 1
            continue
        s = b
        while b < im.total and not im.bitmap[b >> 3] & (0x80 >> (b & 7)):
            b += 1
        runs.append([s, b - s])
    runs.sort(key=lambda r: -r[1])

    plan = []
    for name, rec_disk, size in sorted(targets, key=lambda t: -t[2]):
        left, ext = (size + BS - 1) // BS, []
        for r in runs:
            if not left:
                break
            if r[1] == 0:
                continue
            take = min(left, r[1])
            ext.append((r[0], take))
            r[0] += take
            r[1] -= take
            left -= take
        runs.sort(key=lambda r: -r[1])
        if left or len(ext) > 8:
            print('cannot place %s in <= 8 extents (%d)' % (name, len(ext)))
            sys.exit(3)
        plan.append((name, rec_disk, size, ext))
    print('placement ok: max extents per file %d' % max(len(p[3]) for p in plan))
    if dry:
        return

    # apply: wipe dropped forks (logicalSize, clump, totalBlocks, 8 extents = 80 bytes)
    for off in wipes:
        im.f.seek(off)
        im.f.write(b'\0' * 80)
    for name, rec_disk, size, ext in plan:
        h = hashlib.sha256()
        src = open(os.path.join(cryptex, name), 'rb')
        for s, c in ext:
            im.f.seek(s * BS)
            left = c * BS
            while left:
                chunk = src.read(min(left, 8 << 20))
                if not chunk:
                    break
                h.update(chunk)
                im.f.write(chunk)
                left -= len(chunk)
            if left:
                im.f.write(b'\0' * left)   # tail of the last block
            im.set_bits(s, c, 1)
        fork = struct.pack('>QII', size, 0, sum(c for _, c in ext)) + b''.join(struct.pack('>II', s, c) for s, c in ext)
        fork += b'\0' * (80 - len(fork))
        im.f.seek(rec_disk + 88)
        im.f.write(fork)
        im.f.seek(rec_disk + 32 + 9)          # bsdInfo.ownerFlags: clear UF_COMPRESSED (0x20)
        flags = im.f.read(1)[0]
        im.f.seek(rec_disk + 32 + 9)
        im.f.write(bytes([flags & ~0x20 & 0xFF]))
        print('%-42s %10d bytes %d extents sha256 %s' % (name, size, len(ext), h.hexdigest()[:16]))

    # bitmap + free count in both volume headers
    nbytes = len(im.bitmap)
    off = 0
    while off < nbytes:
        d = im.fork_disk(im.alloc, off)
        k = min(nbytes - off, BS - off % BS)
        im.f.seek(d)
        im.f.write(im.bitmap[off:off + k])
        off += k
    free = im.total - sum(bin(b).count('1') for b in im.bitmap[:im.total // 8])
    im.vh[48:52] = struct.pack('>I', free)
    for vh_off in (1024, im.total * BS - 1024):
        im.f.seek(vh_off)
        im.f.write(im.vh)
    im.f.close()
    print('done: freeBlocks %d (%.0f MB)' % (free, free * BS / 2**20))


if __name__ == '__main__':
    main()
