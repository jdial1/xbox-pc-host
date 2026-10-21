"""Finds Recoil's LuaVBO primitive-restart stores (mov dword [reg+disp], 0xFF / 0xFFFF / 0xFFFFFF with one
displacement) in spring.exe and, with --write <out>, writes a copy whose 0xFFFFFF stores become 0xFFFFFFFF,
which D3D12 (and so Mesa's d3d12 driver) can use as a native strip-cut value."""
import re
import sys

data = bytearray(open(sys.argv[1], 'rb').read())
# C7 /0 with modrm mod=01 (disp8) or mod=10 (disp32), any base register except rsp/rbp-special forms.
pat = re.compile(rb'(?:[\x40-\x4f])?\xc7([\x40-\x47\x80-\x87])')
hits = {}
for m in pat.finditer(data):
    modrm = m.group(1)[0]
    p = m.end()
    if modrm & 0xC0 == 0x40:
        disp, imm_at = data[p], p + 1
    else:
        disp, imm_at = int.from_bytes(data[p:p + 4], 'little'), p + 4
    imm = int.from_bytes(data[imm_at:imm_at + 4], 'little')
    if imm in (0xFF, 0xFFFF, 0xFFFFFF):
        hits.setdefault((modrm & 7, disp), []).append((m.start(), imm, imm_at))
found = []
for key, lst in hits.items():
    imms = {i for _, i, _ in lst}
    if {0xFF, 0xFFFF, 0xFFFFFF} <= imms:
        lo = min(o for o, _, _ in lst); hi = max(o for o, _, _ in lst)
        if hi - lo < 0x400:
            found.append((key, lst))
for key, lst in found:
    print('reg/disp', key, [(hex(o), hex(i)) for o, i, _ in lst])
# The other 0xFFFFFF store (the model VAO's index buffer) uses its own displacement; list them too.
disps = {k[1] for k, _ in found}
others = [(k, l) for k, l in hits.items() if k[1] in disps and k not in dict(found)]
for key, lst in others:
    print('same disp', key, [(hex(o), hex(i)) for o, i, _ in lst if i == 0xFFFFFF])
if len(sys.argv) > 3 and sys.argv[2] == '--write':
    n = 0
    for key, lst in found + others:
        for o, i, at in lst:
            if i == 0xFFFFFF:
                data[at:at + 4] = b'\xff\xff\xff\xff'
                n += 1
    open(sys.argv[3], 'wb').write(data)
    print('patched', n)
