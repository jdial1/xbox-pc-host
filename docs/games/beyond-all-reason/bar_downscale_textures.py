"""Writes downscaled copies of BAR's large textures as loose files in the engine folder. The engine looks for
every file on disk (next to the exe) before the game archive, so these override the originals without
touching the archives. Halving each side cuts a texture's memory to a quarter.

usage: python -I bar_downscale_textures.py <engine dir> <package .sdp> [--min-mb 1] [--scale 0.5] [--prefix bitmaps/]
       [--dry-run]   (lists what it would do)
The .sdp (rapid package index, gzip) holds, per file: name length (1 byte), name, md5 (16), crc32 (4), size (4,
big-endian). Each file's data is pool/<md5[:2]>/<md5[2:]>.gz."""
import argparse
import gzip
import io
import os
import struct
import sys

from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('engine')
ap.add_argument('sdp')
ap.add_argument('--min-mb', type=float, default=1.0)
ap.add_argument('--scale', type=float, default=0.5)
ap.add_argument('--prefix', action='append', default=None)
ap.add_argument('--dry-run', action='store_true')
a = ap.parse_args()
prefixes = a.prefix or ['bitmaps/']

data = gzip.open(a.sdp, 'rb').read()
entries = []
i = 0
while i < len(data):
    n = data[i]
    name = data[i + 1:i + 1 + n].decode('utf-8', 'replace')
    md5 = data[i + 1 + n:i + 17 + n].hex()
    size = struct.unpack('>I', data[i + 21 + n:i + 25 + n])[0]
    entries.append((name, md5, size))
    i += 25 + n

exts = ('.tga', '.png', '.bmp', '.jpg', '.jpeg')
saved_before = saved_after = 0
done = 0
for name, md5, size in entries:
    low = name.lower()
    if not low.endswith(exts) or not any(low.startswith(p) for p in prefixes):
        continue
    src = os.path.join(a.engine, 'pool', md5[:2], md5[2:] + '.gz')
    try:
        raw = gzip.open(src, 'rb').read()
        img = Image.open(io.BytesIO(raw))
        img.load()
    except Exception as e:
        print('skip', name, e)
        continue
    w, h = img.size
    decoded = w * h * 4
    if decoded < a.min_mb * (1 << 20):
        continue
    nw, nh = max(1, int(w * a.scale)), max(1, int(h * a.scale))
    out = os.path.join(a.engine, *name.split('/'))
    saved_before += decoded
    saved_after += nw * nh * 4
    done += 1
    print(f'{name}: {w}x{h} -> {nw}x{nh} ({decoded >> 20} -> {(nw * nh * 4) >> 20} MB)')
    if a.dry_run:
        continue
    os.makedirs(os.path.dirname(out), exist_ok=True)
    small = img.resize((nw, nh), Image.LANCZOS)
    fmt = {'.tga': 'TGA', '.png': 'PNG', '.bmp': 'BMP', '.jpg': 'JPEG', '.jpeg': 'JPEG'}[os.path.splitext(low)[1]]
    small.save(out, fmt)
print(f'{done} textures: {saved_before >> 20} MB decoded -> {saved_after >> 20} MB')
