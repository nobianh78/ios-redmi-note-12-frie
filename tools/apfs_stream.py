import struct, plistlib, lzfse, io
from functools import lru_cache

class DmgStream(io.RawIOBase):
    def __init__(self, dmg_path, partition_idx=3):
        self.f = open(dmg_path, 'rb')
        self.f.seek(-512, 2)
        koly = self.f.read(512)
        xml_offset, xml_length = struct.unpack('>QQ', koly[216:232])
        self.f.seek(xml_offset)
        plist = plistlib.loads(self.f.read(xml_length))
        data = plist['resource-fork']['blkx'][partition_idx]['Data']
        descriptors = struct.unpack('>I', data[32:36])[0]
        self.chunks = []
        self.total_size = 0
        for i in range(descriptors):
            entry = data[204 + i*40 : 204 + (i+1)*40]
            if len(entry) < 40: break
            ctype, comment, out_sec, sec_cnt, in_off, in_len = struct.unpack('>IIQQQQ', entry)
            if ctype == 0xffffffff: break
            start = out_sec * 512
            size = sec_cnt * 512
            self.chunks.append((start, size, ctype, in_off, in_len))
            self.total_size = max(self.total_size, start + size)
        self.pos = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            self.pos = offset
        elif whence == io.SEEK_CUR:
            self.pos += offset
        elif whence == io.SEEK_END:
            self.pos = self.total_size + offset
        return self.pos

    def tell(self):
        return self.pos

    @lru_cache(maxsize=32)
    def _get_chunk_data(self, chunk_idx):
        start, size, ctype, in_off, in_len = self.chunks[chunk_idx]
        if ctype == 0x80000007: # LZFSE
            self.f.seek(in_off)
            return lzfse.decompress(self.f.read(in_len))
        elif ctype == 0x1: # RAW
            self.f.seek(in_off)
            return self.f.read(in_len)
        elif ctype == 0x2: # ZERO
            return b'\x00' * size
        return b'\x00' * size

    def readinto(self, b):
        if self.pos >= self.total_size:
            return 0
        req_len = len(b)
        bytes_read = 0
        # Find which chunk covers self.pos
        # Linear or binary search
        while bytes_read < req_len and self.pos < self.total_size:
            # find chunk
            # Usually chunks are contiguous
            chunk_idx = None
            for idx, (start, size, ctype, in_off, in_len) in enumerate(self.chunks):
                if start <= self.pos < start + size:
                    chunk_idx = idx
                    break
            if chunk_idx is None:
                break
            start, size, ctype, in_off, in_len = self.chunks[chunk_idx]
            offset_in_chunk = self.pos - start
            avail_in_chunk = size - offset_in_chunk
            to_copy = min(req_len - bytes_read, avail_in_chunk)
            chunk_data = self._get_chunk_data(chunk_idx)
            b[bytes_read : bytes_read + to_copy] = chunk_data[offset_in_chunk : offset_in_chunk + to_copy]
            bytes_read += to_copy
            self.pos += to_copy
        return bytes_read

if __name__ == '__main__':
    import dissect.apfs
    stream = DmgStream('extracted_binaries/ios16_x/rootfs.dmg')
    print(f'DmgStream initialized: total_size = {stream.total_size / (1024**3):.2f} GB')
    apfs = dissect.apfs.APFS(stream)
    print(f'APFS container loaded! Block size: {apfs.block_size}')
    print(f'Volumes count: {len(apfs.volumes)}')
    for v in apfs.volumes:
        print(f'  Volume: name={v.name}, role={v.role}, uuid={v.uuid}')
        # List root directory
        print('  Listing root entries:')
        for entry in v.listdir('/'):
            print(f'    {entry}')
