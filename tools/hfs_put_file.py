#!/usr/bin/env python3
"""
DarwinFire: replace the data of EXISTING files in ios16_root.img (no catalog inserts: the target path must
already exist as a regular file). Old data blocks are freed, new ones allocated (<= 8 extents), the fork
record, mode bits, bitmap and volume headers are updated.
Usage: python -I tools/hfs_put_file.py IMG /target/path=local_file[:mode_octal] ... [--dry-run]
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS
from hfs_fill_empty import catalog


def main():
    dry = '--dry-run' in sys.argv
    args = [a for a in sys.argv[1:] if a != '--dry-run']
    im = Img(args[0], not dry)
    files, path = catalog(im)
    by_path = {path(parent, name): (rec_disk, rec) for rec_disk, parent, name, rec in files}
    overflow = set()
    for disk, key, doff, rec in im.leaf_records(im.ext):
        overflow.add(struct.unpack('>I', key[4:8])[0])
    jobs = []
    for spec in args[1:]:
        target, local = spec.split('=', 1)
        mode = None
        if ':' in local[2:]:
            local, m = local.rsplit(':', 1)
            mode = int(m, 8)
        rec_disk, rec = by_path[target]
        assert struct.unpack('>H', rec[42:44])[0] & 0o170000 == 0o100000, target + ' is not a regular file'
        fid = struct.unpack('>I', rec[8:12])[0]
        assert fid not in overflow, target + ' has overflow extents (not handled)'
        for s, c in [struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i]) for i in range(8)]:
            if c:
                im.set_bits(s, c, 0)
        jobs.append((target, rec_disk, rec, open(local, 'rb').read(), mode))
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
    for target, rec_disk, rec, data, mode in jobs:
        left, ext = (len(data) + BS - 1) // BS, []
        for r in runs:
            if not left:
                break
            take = min(left, r[1])
            if take:
                ext.append((r[0], take))
                r[0] += take
                r[1] -= take
                left -= take
        runs = sorted([r for r in runs if r[1]], key=lambda r: -r[1])
        assert not left and len(ext) <= 8, target + ': cannot place in 8 extents'
        plan.append((target, rec_disk, data, mode, ext))
        print('%-50s %8d bytes %d extents%s' % (target, len(data), len(ext), ' mode %o' % mode if mode else ''))
    if dry:
        return
    for target, rec_disk, data, mode, ext in plan:
        off = 0
        for s, c in ext:
            chunk = data[off:off + c * BS]
            im.f.seek(s * BS)
            im.f.write(chunk + b'\0' * (c * BS - len(chunk)))
            off += c * BS
            im.set_bits(s, c, 1)
        fork = struct.pack('>QII', len(data), 0, sum(c for _, c in ext)) + b''.join(struct.pack('>II', s, c) for s, c in ext)
        im.f.seek(rec_disk + 88)
        im.f.write(fork + b'\0' * (80 - len(fork)))
        im.f.seek(rec_disk + 41)               # ownerFlags: clear UF_COMPRESSED
        fl = im.f.read(1)[0]
        im.f.seek(rec_disk + 41)
        im.f.write(bytes([fl & ~0x20 & 0xFF]))
        if mode is not None:
            im.f.seek(rec_disk + 42)
            old = struct.unpack('>H', im.f.read(2))[0]
            im.f.seek(rec_disk + 42)
            im.f.write(struct.pack('>H', (old & 0o170000) | mode))
    nbytes, off = len(im.bitmap), 0
    while off < nbytes:
        d = im.fork_disk(im.alloc, off)
        k = min(nbytes - off, BS - off % BS)
        im.f.seek(d)
        im.f.write(im.bitmap[off:off + k])
        off += k
    free = im.total - sum(bin(x).count('1') for x in im.bitmap[:im.total // 8])
    im.vh[48:52] = struct.pack('>I', free)
    for vh_off in (1024, im.total * BS - 1024):
        im.f.seek(vh_off)
        im.f.write(im.vh)
    im.f.close()
    print('done, free %d blocks' % free)


if __name__ == '__main__':
    main()
