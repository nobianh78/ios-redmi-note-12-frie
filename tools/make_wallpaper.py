#!/usr/bin/env python3
"""
DarwinFire: pull an Apple iOS 16 wallpaper out of ios16_root.img and turn it into the raw
half-resolution BGRA blob (540x1200, 4 B/px, top-down) that df_mfb.cpp composites under the
black pixels of every SpringBoard frame (PosterBoard needs Metal, so there is no real wallpaper).
Usage: python tools/make_wallpaper.py [hfs path of image] [out.bin]
"""
import io, struct, sys
sys.path.insert(0, 'tools')
from hfs_inject_cache import Img, BS
from hfs_fill_empty import catalog
from PIL import Image

W, H = 540, 1200
SRC = sys.argv[1] if len(sys.argv) > 1 else \
    '/System/Library/PrivateFrameworks/Settings/DisplayAndBrightnessSettings.framework/ContinuousExposeLayout-Light-LTR.ca/assets/SydneyWallpaper.jpg'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'xnu/df_wallpaper.bin'

im = Img('ios16_root.img', False)
files, path = catalog(im)
by_path = {path(parent, name): rec for rec_disk, parent, name, rec in files}
if SRC not in by_path:
    cands = [p for p in by_path if 'Wallpaper' in p and p.lower().endswith(('.jpg', '.png', '.heic'))]
    print('not found; candidates:\n  ' + '\n  '.join(sorted(cands)[:40]))
    sys.exit(1)
rec = by_path[SRC]
sz = struct.unpack('>Q', rec[88:96])[0]
data = bytearray()
for i in range(8):
    s, c = struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i])
    if c:
        im.f.seek(s * BS)
        data += im.f.read(c * BS)
img = Image.open(io.BytesIO(bytes(data[:sz]))).convert('RGB')
print('source', SRC, img.size)
# cover-fit to 9:20 portrait, centre crop
sw, sh = img.size
scale = max(W / sw, H / sh)
img = img.resize((max(W, round(sw * scale)), max(H, round(sh * scale))), Image.LANCZOS)
x0, y0 = (img.width - W) // 2, (img.height - H) // 2
img = img.crop((x0, y0, x0 + W, y0 + H))
# slight darkening so white lockscreen text stays readable
img = Image.eval(img, lambda v: int(v * 0.85))
r, g, b = img.split()
a = Image.new('L', img.size, 255)
raw = Image.merge('RGBA', (b, g, r, a)).tobytes()   # BGRA little-endian = 0xAARRGGBB words
open(OUT, 'wb').write(raw)
img.save(OUT.replace('.bin', '_preview.png'))
print('wrote', OUT, len(raw), 'bytes')
