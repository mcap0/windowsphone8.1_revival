#!/usr/bin/env python3
"""mappe_icons.py — Mappe tile/splash assets from the system Maps glyph (StartAssets768x1280.dll,
START.TILE.MAPS.PNG, extracted to data/mapsicon/). Same sizes and glyph fractions as tools/icons; white glyph on
transparent so the tile takes the accent colour, splash/store logo on the app colour. Run in the dumbphone-navit
container: podman run --rm --security-opt label=disable -v "$PWD":/w -w /w dumbphone-navit python3 tools/navit/mappe_icons.py"""
import os
from PIL import Image

SRC = 'data/mapsicon/START.TILE.MAPS.PNG'
OUT = 'apps/Mappe/Mappe81/Assets'
COLOUR = (0x2E, 0x7D, 0x32, 255)
ASSETS = {  # name: (w, h at scale 100, glyph fraction of the shorter side, filled)
    'Square150x150Logo': (150, 150, 0.46, False),
    'Square71x71Logo': (71, 71, 0.56, False),
    'Square44x44Logo': (44, 44, 0.70, False),
    'StoreLogo': (50, 50, 0.66, True),
    'SplashScreen': (480, 800, 0.30, False),
}
SCALES = (100, 140, 240)

glyph = Image.open(SRC).convert('RGBA')
glyph = glyph.crop(glyph.getbbox())
for name, (w, h, fraction, filled) in ASSETS.items():
    plain = os.path.join(OUT, name + '.png')
    if os.path.exists(plain):
        os.remove(plain)
    for scale in SCALES:
        W, H = round(w * scale / 100), round(h * scale / 100)
        side = min(W, H) * fraction
        k = side / max(glyph.size)
        g = glyph.resize((max(1, round(glyph.size[0] * k)), max(1, round(glyph.size[1] * k))), Image.LANCZOS)
        im = Image.new('RGBA', (W, H), COLOUR if filled else (0, 0, 0, 0))
        im.alpha_composite(g, ((W - g.size[0]) // 2, (H - g.size[1]) // 2))
        im.save(os.path.join(OUT, '%s.scale-%d.png' % (name, scale)))
print(sorted(os.listdir(OUT)))
