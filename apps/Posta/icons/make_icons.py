# Generates the Posta tile set from the native Windows Phone 8.1 Email tile glyph
# (white envelope, START.TILE.EMAIL in StartAssets768x1280.dll, PNG 158x158, transparent).
# The glyph is cropped to its alpha bbox and placed centred at a fixed width fraction.
# Background stays transparent so the manifest BackgroundColor (#1F6FEB) shows through.
# Usage: python3 make_icons.py <source_158.png> <out_dir>
import os, sys
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
im = Image.open(src).convert('RGBA')
glyph = im.crop(im.split()[3].getbbox())  # tight envelope (69x54 px)

def place(w, h, frac, name):
    gw = round(w * frac)
    gh = round(glyph.size[1] * gw / glyph.size[0])
    img = glyph.resize((gw, gh), Image.LANCZOS)
    c = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    c.alpha_composite(img, (round((w - gw) / 2), round((h - gh) / 2)))
    c.save(os.path.join(out, name)); print(name, c.size)

place(150, 150, 0.437, 'Square150x150Logo.png')   # native proportions (glyph 44% of tile)
place(71, 71, 0.52, 'Square71x71Logo.png')        # small tiles: slightly larger glyph for legibility
place(44, 44, 0.60, 'Square44x44Logo.png')
place(50, 50, 0.50, 'StoreLogo.png')
place(480, 800, 0.30, 'SplashScreen.png')         # upscaled from 69 px glyph; splash only
