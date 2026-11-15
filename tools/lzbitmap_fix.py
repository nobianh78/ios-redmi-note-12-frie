"""Fixed LZBITMAP decoder, patched over dissect.util.compression.lzbitmap.decompress.

dissect's version (0) takes the end marker (csize 6, usize 0) for an empty stored chunk and then parses the
padding after it as a header (the 334 MB .42.dyldlinkedit hit this), (1) raises StopIteration when a chunk's last token has no following nibble (the
reference decoder, libzbitmap, just runs that token once), and (2) keeps emitting bytes when a repeat
count runs past the chunk's uncompressed size. Every chunk here must decode to exactly its declared size,
otherwise we raise instead of writing wrong bytes into the root image.
"""
import io
import itertools
import struct

_H = struct.Struct('<H')


def decompress(src):
    if not hasattr(src, 'read'):
        src = io.BytesIO(src)
    if src.read(4) != b'ZBM\x09':
        raise ValueError('Not a valid LZBITMAP stream')
    dst = bytearray()
    while True:
        hdr = src.read(6)
        if len(hdr) < 6:
            break
        csize = int.from_bytes(hdr[0:3], 'little')
        usize = int.from_bytes(hdr[3:6], 'little')
        if usize == 0:                               # end of stream (csize 6 == 0 + 6: must win over "stored")
            break
        if csize == usize + 6:                       # stored chunk
            dst += src.read(usize)
            continue
        dist_off = int.from_bytes(src.read(3), 'little')
        bmp_off = int.from_bytes(src.read(3), 'little')
        tok_off = int.from_bytes(src.read(3), 'little')
        lit_off = 15
        src.seek(-15, io.SEEK_CUR)
        buf = memoryview(src.read(csize))
        tmap, bits = [], int.from_bytes(buf[-17:], 'little')
        for i in range(0xF):
            if i < 3:
                tmap.append((None, i))
            else:
                tmap.append((bits & 0xFF, (bits >> 8) & 3))
                bits >>= 10
        tokens = itertools.chain.from_iterable((b & 0xF, b >> 4) for b in buf[tok_off:-17])
        start, left, distance, prev = len(dst), usize, 8, None
        while left > 0:
            idx = next(tokens, None) if prev is None else prev
            if idx is None:
                break
            if idx == 0xF:
                raise ValueError('Invalid token index in LZBITMAP stream')
            nxt = next(tokens, None)
            if nxt != 0xF:                           # no repeat count (or end of tokens)
                prev, repeat = nxt, 1
            else:
                prev, total = None, 4
                while nxt == 0xF:
                    nxt = next(tokens, None)
                    if nxt is None:
                        raise ValueError('Truncated repeat count in LZBITMAP stream')
                    total += nxt
                repeat = total
            for _ in range(repeat):
                if left <= 0:
                    break
                bitmap, token = tmap[idx]
                if idx < 3:
                    bitmap = buf[bmp_off]
                    bmp_off += 1
                if token == 1:
                    distance = buf[dist_off]
                    dist_off += 1
                elif token == 2:
                    (distance,) = _H.unpack_from(buf, dist_off)
                    dist_off += 2
                for _ in range(8):
                    if bitmap & 1:
                        dst.append(buf[lit_off])
                        lit_off += 1
                    else:
                        if distance > len(dst):
                            raise ValueError('Invalid match distance in LZBITMAP stream')
                        dst.append(dst[-distance])
                    bitmap >>= 1
                    left -= 1
                    if left == 0:
                        break
        if len(dst) - start != usize:
            raise ValueError('LZBITMAP chunk decoded to %d bytes, expected %d' % (len(dst) - start, usize))
    return bytes(dst)


def _filestream_read(self, offset, length):
    """dissect.apfs FileStream._read advanced by the whole extent length even when the read started inside
    the extent, so it skipped data (dyld_shared_cache_arm64.35 came out 8 MB short). Advance by what was
    actually taken."""
    result = []
    while length:
        logical, physical, ext_len, crypto_id = self._lookup(offset)
        block = self.volume.container._read_block(physical, ext_len // self.align)
        if self.volume.is_encrypted:
            raise RuntimeError('encrypted volume not supported by this fix')
        block = block[offset - logical:][:length]
        if not block:
            raise RuntimeError('empty extent read at offset %d' % offset)
        result.append(block)
        offset += len(block)
        length -= len(block)
    return b''.join(result)


def install():
    import dissect.util.compression.lzbitmap as m
    m.decompress = decompress
    import dissect.apfs.stream as st
    st.FileStream._read = _filestream_read
    try:
        import dissect.apfs.stream as s
        if hasattr(s, 'lzbitmap'):
            s.lzbitmap.decompress = decompress
    except ImportError:
        pass
