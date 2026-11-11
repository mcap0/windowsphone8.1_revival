# Generates the Calendario tile set: white calendar glyph on a transparent background
# (the manifest BackgroundColor #0063B1 shows through). Drawn at 8x and downscaled.
# Usage: python3 make_icons.py <out_dir>
import os, sys
from PIL import Image, ImageDraw

SS = 8  # supersampling factor

def glyph(size):
    n = size * SS
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    white = (255, 255, 255, 255)
    w = max(2, int(n * 0.07))
    # Calendar body: outlined rounded rectangle with a filled header band.
    x0, y0, x1, y1 = n * 0.12, n * 0.18, n * 0.88, n * 0.88
    r = n * 0.08
    d.rounded_rectangle([x0, y0, x1, y1], radius=r, outline=white, width=w)
    head = y0 + n * 0.20
    d.rounded_rectangle([x0, y0, x1, head], radius=r, fill=white)
    d.rectangle([x0, head - r, x1, head], fill=white)
    # Binder rings.
    for cx in (n * 0.34, n * 0.66):
        d.rounded_rectangle([cx - w * 0.8, n * 0.08, cx + w * 0.8, n * 0.26], radius=w, fill=white)
    # 3 x 2 day dots.
    dot = n * 0.075
    for row in range(2):
        for col in range(3):
            cx = n * (0.30 + col * 0.20)
            cy = n * (0.50 + row * 0.19)
            d.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=white)
    return im

def place(out, w, h, frac, name):
    g = glyph(256)
    bb = g.split()[3].getbbox()
    g = g.crop(bb)
    target = round(min(w, h) * frac)
    scale = target / max(g.size)
    gw, gh = max(1, round(g.size[0] * scale)), max(1, round(g.size[1] * scale))
    g = g.resize((gw, gh), Image.LANCZOS)
    canvas = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    canvas.alpha_composite(g, ((w - gw) // 2, (h - gh) // 2))
    canvas.save(os.path.join(out, name))
    print(name, canvas.size)

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    place(out, 150, 150, 0.62, 'Square150x150Logo.png')
    place(out, 71, 71, 0.70, 'Square71x71Logo.png')
    place(out, 44, 44, 0.78, 'Square44x44Logo.png')
    place(out, 50, 50, 0.78, 'StoreLogo.png')
    place(out, 480, 800, 0.30, 'SplashScreen.png')
