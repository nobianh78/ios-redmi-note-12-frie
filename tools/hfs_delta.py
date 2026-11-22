#!/usr/bin/env python3
"""
DarwinFire: delta sparse image of whatever changed in ios16_root.img.
  python -I tools/hfs_delta.py snapshot IMG HASHFILE      # before editing the image
  python -I tools/hfs_delta.py delta IMG HASHFILE OUT.simg  # after: only changed 4K blocks
Unchanged blocks become DONT_CARE chunks, so flashing OUT.simg to super rewrites only the edits
(same format as tools/patch_sb_launchd.py).
"""
import hashlib, os, struct, sys

BS = 4096
CHUNK = 256 * BS


def block_hashes(path):
    out = bytearray()
    with open(path, 'rb') as f:
        while True:
            buf = f.read(CHUNK)
            if not buf:
                break
            for i in range(0, len(buf), BS):
                out += hashlib.blake2b(buf[i:i + BS], digest_size=8).digest()
    return bytes(out)


def main():
    cmd, img, hfile = sys.argv[1], sys.argv[2], sys.argv[3]
    if cmd == 'snapshot':
        h = block_hashes(img)
        open(hfile, 'wb').write(h)
        print('snapshot: %d blocks' % (len(h) // 8))
        return
    out_path = sys.argv[4]
    old = open(hfile, 'rb').read()
    new = block_hashes(img)
    total = len(new) // 8
    changed = [b for b in range(total) if b * 8 >= len(old) or old[b * 8:b * 8 + 8] != new[b * 8:b * 8 + 8]]
    chunks, pos = [], 0
    for b in changed:
        if b > pos:
            chunks.append(('skip', pos, b))
        if chunks and chunks[-1][0] == 'raw' and chunks[-1][2] == b:
            chunks[-1] = ('raw', chunks[-1][1], b + 1)
        else:
            chunks.append(('raw', b, b + 1))
        pos = b + 1
    if pos < total:
        chunks.append(('skip', pos, total))
    with open(img, 'rb') as f, open(out_path, 'wb') as out:
        out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BS, total, len(chunks), 0))
        for kind, s, e in chunks:
            n = e - s
            if kind == 'skip':
                out.write(struct.pack('<HHII', 0xCAC3, 0, n, 12))
            else:
                out.write(struct.pack('<HHII', 0xCAC1, 0, n, 12 + n * BS))
                f.seek(s * BS)
                out.write(f.read(n * BS))
    print('delta: %d changed blocks -> %s (%d bytes)' % (len(changed), out_path, os.path.getsize(out_path)))


if __name__ == '__main__':
    main()
