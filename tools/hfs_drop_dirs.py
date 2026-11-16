#!/usr/bin/env python3
"""
DarwinFire: free space on ios16_root.img by emptying every file under the given directories
(names stay, data forks become 0 bytes, their blocks are freed). Same in-place method as
hfs_inject_cache.py's DROP step. Usage: python -I tools/hfs_drop_dirs.py IMG DIR... [--dry-run]
(DIR without the leading slash, e.g. System/Library/Audio)
"""
import struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS


def main():
    args = [a for a in sys.argv[1:] if a != '--dry-run']
    dry = '--dry-run' in sys.argv
    im = Img(args[0], not dry)
    folder_by_key, folder_parent, files = {}, {}, []
    for disk, key, doff, rec in im.leaf_records(im.cat):
        parent = struct.unpack('>I', key[2:6])[0]
        name = key[8:8 + 2 * struct.unpack('>H', key[6:8])[0]].decode('utf-16-be', 'replace')
        rtype = struct.unpack('>h', rec[doff:doff + 2])[0]
        if rtype == 1:
            cnid = struct.unpack('>I', rec[doff + 8:doff + 12])[0]
            folder_by_key[(parent, name)] = cnid
            folder_parent[cnid] = parent
        elif rtype == 2:
            files.append((disk + doff, parent, bytes(rec[doff:doff + 248])))
    roots = set()
    for p in args[1:]:
        cnid = 2
        try:
            for comp in p.split('/'):
                cnid = folder_by_key[(cnid, comp)]
        except KeyError:
            print('skip (not found):', p)
            continue
        roots.add(cnid)
    overflow = {}
    for disk, key, doff, rec in im.leaf_records(im.ext):
        overflow.setdefault((struct.unpack('>I', key[4:8])[0], key[2]), []).extend(
            e for e in (struct.unpack('>II', rec[doff + 8 * i: doff + 8 * i + 8]) for i in range(8)) if e[1])

    def under(c):
        while c in folder_parent and c not in roots:
            c = folder_parent[c]
        return c in roots

    freed, n, wipes = 0, 0, []
    for rec_disk, parent, rec in files:
        if not under(parent):
            continue
        fid = struct.unpack('>I', rec[8:12])[0]
        for forkoff, ftype in ((88, 0), (168, 0xFF)):
            ext = [struct.unpack('>II', rec[forkoff + 16 + 8 * i: forkoff + 24 + 8 * i]) for i in range(8)]
            for s, c in [e for e in ext if e[1]] + overflow.get((fid, ftype), []):
                im.set_bits(s, c, 0)
                freed += c
            wipes.append(rec_disk + forkoff)
        n += 1
    free = im.total - sum(bin(b).count('1') for b in im.bitmap[:im.total // 8])
    print('emptied %d files, freed %d blocks (%.0f MB); free now %d blocks (%.0f MB)'
          % (n, freed, freed * BS / 2**20, free, free * BS / 2**20))
    if dry:
        return
    for off in wipes:
        im.f.seek(off)
        im.f.write(b'\0' * 80)
    nbytes, off = len(im.bitmap), 0
    while off < nbytes:
        d = im.fork_disk(im.alloc, off)
        k = min(nbytes - off, BS - off % BS)
        im.f.seek(d)
        im.f.write(im.bitmap[off:off + k])
        off += k
    im.vh[48:52] = struct.pack('>I', free)
    for vh_off in (1024, im.total * BS - 1024):
        im.f.seek(vh_off)
        im.f.write(im.vh)
    im.f.close()
    print('done')


if __name__ == '__main__':
    main()
