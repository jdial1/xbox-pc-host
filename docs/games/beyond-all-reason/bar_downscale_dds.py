"""Writes half-size copies of BAR's large DDS textures (unit textures) as loose files in the engine folder, which
the engine reads before its archives. texconv (DirectXTex) keeps each file's format (BC1/BC2/BC3/uncompressed)
and rebuilds the full mip chain.

usage: python -I bar_downscale_dds.py <engine dir> <package .sdp> <texconv.exe> [--min-mb 1] [--prefix unittextures/]
       [--dry-run]"""
import argparse
import gzip
import os
import struct
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('engine')
ap.add_argument('sdp')
ap.add_argument('texconv')
ap.add_argument('--min-mb', type=float, default=1.0)
ap.add_argument('--prefix', action='append', default=None)
ap.add_argument('--dry-run', action='store_true')
a = ap.parse_args()
prefixes = a.prefix or ['unittextures/']

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

before = after = done = failed = 0
with tempfile.TemporaryDirectory() as tmp:
    for name, md5, size in entries:
        low = name.lower()
        if not low.endswith('.dds') or not any(low.startswith(p) for p in prefixes) or size < a.min_mb * (1 << 20):
            continue
        raw = gzip.open(os.path.join(a.engine, 'pool', md5[:2], md5[2:] + '.gz'), 'rb').read()
        if raw[:4] != b'DDS ':
            continue
        h, w = struct.unpack('<II', raw[12:20])
        caps2 = struct.unpack('<I', raw[112:116])[0]
        if caps2 & 0x200 or (raw[84:88] == b'DX10'):  # cube maps and DX10 headers: leave as they are
            print('skip (cube/DX10)', name)
            continue
        nw, nh = max(4, w // 2), max(4, h // 2)
        print(f'{name}: {w}x{h} -> {nw}x{nh} ({size >> 20} MB)')
        before += size
        if a.dry_run:
            continue
        src = os.path.join(tmp, os.path.basename(name))
        open(src, 'wb').write(raw)
        outdir = os.path.join(a.engine, *name.split('/')[:-1])
        os.makedirs(outdir, exist_ok=True)
        r = subprocess.run([a.texconv, '-nologo', '-y', '-w', str(nw), '-h', str(nh), '-m', '0', '-o', outdir, src],
                           capture_output=True, text=True)
        out = os.path.join(outdir, os.path.basename(name))
        if r.returncode or not os.path.exists(out):
            failed += 1
            print('  texconv failed:', (r.stdout + r.stderr).strip()[-300:])
            continue
        after += os.path.getsize(out)
        done += 1
        os.remove(src)
print(f'{done} DDS written ({failed} failed): {before >> 20} MB -> {after >> 20} MB')
