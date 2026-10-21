"""Disables BAR's heavy visual / analytics widgets in LuaUI/Config/BYAR.lua (order = 0 is disabled), keeping
the gameplay UI. Writes a .orig backup once. usage: python -I bar_console_widgets.py <BYAR.lua>"""
import os
import re
import shutil
import sys

OFF = [
    # rendering effects (GL4 buffers, extra passes)
    "Airjets GL4", "Bloom Shader Deferred", "Contrast Adaptive Sharpen", "Darken map", "Decals GL4",
    "Deferred rendering GL4", "Distortion GL4", "Ground AO Plates Features GL4", "Ground AO Plates GL4",
    "Map Grass GL4", "Map Edge Extension", "Map Lighting Adjuster", "Orb Effects GL4", "Outline Shader GL4",
    "Resurrection Halos GL4", "Snow", "Fog Diagonal Lines GL4", "Unit Stencil GL4", "Paralyze Effect",
    "Commander Hurt Vignette", "CameraShake", "Mouse FX", "Commands FX", "Mapmarks FX", "Map Blockout",
    "Reclaim Field Highlight", "Highlight Commander Wrecks", "Jammed Units Highlight", "Ghost Radar GL4",
    "Picture-in-Picture Minimap", "GUI Shader",
    # range rings and previews
    "Sensor Ranges Jammer Preview", "Sensor Ranges LOS", "Sensor Ranges Radar", "Sensor Ranges Radar Preview",
    "Sensor Ranges Sonar", "Attack Range GL4", "Defense Range GL4", "EMP + decloak range",
    "Construction Turrets Range Assist", "Nano range on transport",
    # extras, analytics, dev tools
    "AdvPlayersList Music Player New", "Awards", "Analytics - Widgets", "Analytics API", "Statistics Collection",
    "Changelog Info", "Ruins Blueprint Generator", "Test Runner", "Map Editor Session", "Terraform Suite Launcher",
    "Log UnitDefIDs", "API Unit Tracker DEVMODE GL4", "Self-D Resign Analytics Bridge", "Ecostats",
]

path = sys.argv[1]
if not os.path.exists(path + '.orig'):
    shutil.copyfile(path, path + '.orig')
s = open(path, newline='').read()
start = s.index('order = {')
end = s.index('\n\t}', start)
block = s[start:end]
done = []
for name in OFF:
    key = re.escape(name)
    pat = re.compile(r'(\n\s*(?:\["' + key + r'"\]|' + (key if re.fullmatch(r'\w+', name) else r'(?!)') + r')\s*=\s*)\d+')
    block, n = pat.subn(r'\g<1>0', block)
    if n:
        done.append(name)
s = s[:start] + block + s[end:]
open(path, 'w', newline='').write(s)
missing = [n for n in OFF if n not in done]
print(f'disabled {len(done)} widgets; not in the file: {missing}')
