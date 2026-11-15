#!/usr/bin/env python3
"""Decompress the dyld_shared_cache files out of system_cryptex.dmg in parallel (pure-Python LZBITMAP is
~1 MB/s per core). Each output size must equal the APFS size; lzbitmap_fix raises on any bad chunk.
Usage: python -I tools/extract_dsc.py CRYPTEX_DMG OUT_DIR"""
import multiprocessing as mp, os, sys, time

DIR = '/System/Library/Caches/com.apple.dyld/'


def vol(dmg):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import dissect.apfs, lzbitmap_fix
    lzbitmap_fix.install()
    return dissect.apfs.APFS(open(dmg, 'rb')).volumes[0]


def one(args):
    dmg, out, name = args
    dst = os.path.join(out, name)
    v = vol(dmg)
    e = v.get(DIR + name)
    if os.path.exists(dst) and os.path.getsize(dst) == e.size:
        return name, e.size, 'kept'
    src, n = e.open(), 0
    with open(dst + '.part', 'wb') as f:
        while True:
            c = src.read(4 << 20)
            if not c:
                break
            f.write(c)
            n += len(c)
    if n != e.size:
        raise RuntimeError('%s: %d bytes, expected %d' % (name, n, e.size))
    os.replace(dst + '.part', dst)
    return name, n, 'ok'


if __name__ == '__main__':
    dmg, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    names = sorted((n for n in vol(dmg).get(DIR).listdir() if n.startswith('dyld_shared_cache_arm64')
                    and not n.endswith('.symbols')), key=lambda n: -vol(dmg).get(DIR + n).size)
    t = time.time()
    with mp.Pool(os.cpu_count()) as pool:
        for i, (name, n, st) in enumerate(pool.imap_unordered(one, [(dmg, out, n) for n in names]), 1):
            print('[%2d/%d] %4ds %-42s %10d %s' % (i, len(names), time.time() - t, name, n, st), flush=True)
