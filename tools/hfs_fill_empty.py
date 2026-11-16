#!/usr/bin/env python3
"""
DarwinFire: fill the 0-byte files of ios16_root.img with their real contents.

The macOS build ran out of space, so ~88k regular files exist in the HFS+ catalog with 0 bytes
(cc_fips_test, tzinit, ...). The catalog itself is complete, so instead of re-creating the volume we:
  scan    : for every empty regular file, look up the same path in rootfs.dmg (APFS), then in
            system_cryptex.dmg; write out/fill/plan.tsv (cnid, catalog record offset, size, source, path)
  extract : decompress every planned file into out/fill/data/<cnid> (parallel, resumable, size-checked)
  apply   : optionally grow the volume, allocate blocks (<= 8 extents per file), write the data,
            fix the catalog fork records, the bitmap and both volume headers
Run apply with --dry-run first. Usage:
  python -I tools/hfs_fill_empty.py scan IMG ROOTFS_DMG CRYPTEX_DMG OUT
  python -I tools/hfs_fill_empty.py extract ROOTFS_DMG CRYPTEX_DMG OUT [JOBS]
  python -I tools/hfs_fill_empty.py apply IMG OUT [--grow BLOCKS] [--dry-run]
"""
import multiprocessing as mp, os, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS, DROP

SUPER_BLOCKS = 8707 * 2**20 // BS   # fire "super" partition


def catalog(im):
    folder_name, folder_parent, files = {}, {}, []
    for disk, key, doff, rec in im.leaf_records(im.cat):
        parent = struct.unpack('>I', key[2:6])[0]
        nlen = struct.unpack('>H', key[6:8])[0]
        name = key[8:8 + 2 * nlen].decode('utf-16-be', 'replace')
        rtype = struct.unpack('>h', rec[doff:doff + 2])[0]
        if rtype == 1:
            cnid = struct.unpack('>I', rec[doff + 8:doff + 12])[0]
            folder_name[cnid], folder_parent[cnid] = name, parent
        elif rtype == 2:
            files.append((disk + doff, parent, name, bytes(rec[doff:doff + 248])))

    def path(parent, name):
        parts = [name]
        while parent in folder_parent and parent != 2:
            parts.append(folder_name[parent])
            parent = folder_parent[parent]
        return '/' + '/'.join(reversed(parts))
    return files, path


def vols(rootfs, cryptex):
    import dissect.apfs, lzbitmap_fix
    lzbitmap_fix.install()
    return [('r', dissect.apfs.APFS(open(rootfs, 'rb')).volumes[0]),
            ('c', dissect.apfs.APFS(open(cryptex, 'rb')).volumes[0])]


def scan(img, rootfs, cryptex, out):
    im = Img(img, False)
    files, path = catalog(im)
    vs = vols(rootfs, cryptex)
    os.makedirs(out, exist_ok=True)
    n = total = missing = zero = 0
    with open(os.path.join(out, 'plan.tsv'), 'w', encoding='utf-8') as f:
        for rec_disk, parent, name, rec in files:
            mode = struct.unpack('>H', rec[42:44])[0]
            if mode & 0o170000 != 0o100000 or struct.unpack('>Q', rec[88:96])[0] or rec[104:112] != b'\0' * 8:
                continue
            p = path(parent, name)
            if any(p.startswith('/' + d + '/') for d in DROP):
                continue   # dropped on purpose (Siri, Watch, ...): stay empty
            for tag, v in vs:
                try:
                    e = v.get(p)
                    if not e.is_file():
                        continue
                    size = e.size
                except Exception:
                    continue
                break
            else:
                missing += 1
                continue
            if size == 0:
                zero += 1
                continue
            cnid = struct.unpack('>I', rec[8:12])[0]
            f.write('%d\t%d\t%d\t%s\t%s\n' % (cnid, rec_disk, size, tag, p))
            n += 1
            total += (size + BS - 1) // BS
    free = im.total - sum(bin(b).count('1') for b in im.bitmap[:im.total // 8])
    print('to fill %d files, %d blocks (%.0f MB); truly empty %d; not in APFS %d' % (n, total, total * BS / 2**20, zero, missing))
    print('free now %d blocks (%.0f MB); can grow by %d blocks (%.0f MB) up to super'
          % (free, free * BS / 2**20, SUPER_BLOCKS - im.total, (SUPER_BLOCKS - im.total) * BS / 2**20))


_V = None


def _one(args):
    global _V
    rootfs, cryptex, out, cnid, size, tag, p = args
    dst = os.path.join(out, 'data', cnid)
    if os.path.exists(dst) and os.path.getsize(dst) == size:
        return 0
    if _V is None:
        _V = dict(vols(rootfs, cryptex))
    src, n = _V[tag].get(p).open(), 0
    with open(dst + '.part', 'wb') as f:
        while True:
            c = src.read(4 << 20)
            if not c:
                break
            f.write(c)
            n += len(c)
    if n != size:
        raise RuntimeError('%s: %d bytes, expected %d' % (p, n, size))
    os.replace(dst + '.part', dst)
    return size


def extract(rootfs, cryptex, out, jobs):
    os.makedirs(os.path.join(out, 'data'), exist_ok=True)
    work = []
    for line in open(os.path.join(out, 'plan.tsv'), encoding='utf-8'):
        cnid, _, size, tag, p = line.rstrip('\n').split('\t')
        work.append((rootfs, cryptex, out, cnid, int(size), tag, p))
    work.sort(key=lambda w: -w[4])   # big files first, so the tail is short
    t0, done = time.time(), 0
    with mp.Pool(jobs) as pool:
        for i, n in enumerate(pool.imap_unordered(_one, work, chunksize=8), 1):
            done += n
            if i % 2000 == 0 or i == len(work):
                print('%d/%d files, %.0f MB new, %.0f s' % (i, len(work), done / 2**20, time.time() - t0), flush=True)


def extend_alloc(im, blocks):
    """the allocation file (bitmap) only covers ~2.13M blocks; add an extent of free blocks so it covers `blocks`"""
    size, ext = im.alloc
    have = sum(c for _, c in ext)
    want = ((blocks + 7) // 8 + BS - 1) // BS
    add = want - have
    assert len(ext) < 8, 'allocation file already has 8 extents'
    run, b = 0, 0
    while b < im.total:
        run = run + 1 if not im.bitmap[b >> 3] & (0x80 >> (b & 7)) else 0
        if run == add:
            break
        b += 1
    assert run == add, 'no %d free contiguous blocks for the allocation file' % add
    start = b - add + 1
    im.set_bits(start, add, 1)
    ext = ext + [(start, add)]
    im.bitmap += bytearray(want * BS - len(im.bitmap))
    im.alloc = (want * BS, ext)
    im.vh[0x70:0x78] = struct.pack('>Q', want * BS)
    im.vh[0x7C:0x80] = struct.pack('>I', want)
    for i, (s, c) in enumerate(ext):
        im.vh[0x80 + 8 * i:0x88 + 8 * i] = struct.pack('>II', s, c)
    print('allocation file: +%d blocks at %d, now covers %d blocks' % (add, start, want * BS * 8))


def apply(img, out, grow, dry):
    im = Img(img, not dry)
    if grow and im.total + grow > len(im.bitmap) * 8:
        extend_alloc(im, im.total + grow)
    if grow:
        assert im.total + grow <= SUPER_BLOCKS, 'grow past the super partition'
        im.grow(grow, resize=not dry)
    plan = []
    for line in open(os.path.join(out, 'plan.tsv'), encoding='utf-8'):
        cnid, rec_disk, size, tag, p = line.rstrip('\n').split('\t')
        d = os.path.join(out, 'data', cnid)
        assert os.path.getsize(d) == int(size), 'not extracted: ' + p
        plan.append((int(rec_disk), int(size), d, p))
    need = sum((s + BS - 1) // BS for _, s, _, _ in plan)
    free = im.total - sum(bin(b).count('1') for b in im.bitmap[:im.total // 8])
    print('files %d need %d blocks (%.0f MB), free %d blocks (%.0f MB)' % (len(plan), need, need * BS / 2**20, free, free * BS / 2**20))
    if need > free:
        print('NOT ENOUGH SPACE: short by %.0f MB' % ((need - free) * BS / 2**20))
        sys.exit(2)

    # free runs; small files go into the first fitting run, big files take the biggest runs
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
    placed = []
    for rec_disk, size, d, p in sorted(plan, key=lambda t: -t[1]):
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
        if left or len(ext) > 8:
            print('cannot place %s (%d extents)' % (p, len(ext)))
            sys.exit(3)
        runs = [r for r in runs if r[1]]
        runs.sort(key=lambda r: -r[1])
        placed.append((rec_disk, size, d, ext))
    print('placement ok: max extents per file %d' % max(len(x[3]) for x in placed))
    if dry:
        return
    for i, (rec_disk, size, d, ext) in enumerate(placed, 1):
        src = open(d, 'rb')
        for s, c in ext:
            data = src.read(c * BS)
            im.f.seek(s * BS)
            im.f.write(data + b'\0' * (c * BS - len(data)))
            im.set_bits(s, c, 1)
        src.close()
        fork = struct.pack('>QII', size, 0, sum(c for _, c in ext)) + b''.join(struct.pack('>II', s, c) for s, c in ext)
        im.f.seek(rec_disk + 88)
        im.f.write(fork + b'\0' * (80 - len(fork)))
        im.f.seek(rec_disk + 41)              # bsdInfo.ownerFlags: clear UF_COMPRESSED
        fl = im.f.read(1)[0]
        im.f.seek(rec_disk + 41)
        im.f.write(bytes([fl & ~0x20 & 0xFF]))
        if i % 5000 == 0:
            print('%d/%d written' % (i, len(placed)), flush=True)
    nbytes, off = len(im.bitmap), 0
    while off < nbytes:
        dd = im.fork_disk(im.alloc, off)
        k = min(nbytes - off, BS - off % BS)
        im.f.seek(dd)
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
    a = sys.argv
    if a[1] == 'scan':
        scan(a[2], a[3], a[4], a[5])
    elif a[1] == 'extract':
        extract(a[2], a[3], a[4], int(a[5]) if len(a) > 5 else os.cpu_count())
    elif a[1] == 'apply':
        apply(a[2], a[3], int(a[a.index('--grow') + 1]) if '--grow' in a else 0, '--dry-run' in a)
