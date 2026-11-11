# Generates the TelegramWeb tile set from the official Telegram logo SVG (source_telegram_logo.svg).
# Variant: white disc with the paper plane in the tile colour #2AABEE (inverted official logo),
# transparent background so the manifest BackgroundColor (#2AABEE) shows through.
# Usage: python3 make_icons.py <source.svg> <out_dir>
import os, subprocess, sys, tempfile
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
svg = open(src, encoding='utf-8').read()
# Recolour: disc white, plane tile blue, fold shading lighter/darker blue.
svg = svg.replace('fill="url(#linear-gradient)"', 'fill="#ffffff"')
svg = svg.replace('fill="#fff"', 'fill="#2AABEE"')
svg = svg.replace('fill="#c8daea"', 'fill="#7fcff5"').replace('fill="#a9c6d8"', 'fill="#5cc2f0"')
tmp_svg = os.path.join(tempfile.mkdtemp(), 'telegram_white.svg')
open(tmp_svg, 'w', encoding='utf-8').write(svg)

BIG = 1024

def render_big():
    png = os.path.join(os.path.dirname(tmp_svg), 'big.png')
    subprocess.run(['magick', '-background', 'none', '-density', '600', tmp_svg,
                    '-resize', f'{BIG}x{BIG}', png], check=True)
    return Image.open(png).convert('RGBA')

disc = render_big()  # 1024x1024, disc fills the canvas
bbox = disc.split()[3].getbbox()
disc = disc.crop(bbox)  # tight disc (diameter = width)

def place(canvas, frac, name, cy=0.5, cx=0.5):
    d = round(canvas * frac)
    img = disc.resize((d, d), Image.LANCZOS)
    c = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
    c.alpha_composite(img, (round(cx * canvas - d / 2), round(cy * canvas - d / 2)))
    c.save(os.path.join(out, name)); print(name, c.size)

def place_rect(w, h, frac, name):
    d = round(h * frac)
    img = disc.resize((d, d), Image.LANCZOS)
    c = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    c.alpha_composite(img, (round(w / 2 - d / 2), round(h / 2 - d / 2)))
    c.save(os.path.join(out, name)); print(name, c.size)

place(150, 0.62, 'Square150x150Logo.png')
place(71, 0.62, 'Square71x71Logo.png')
place(44, 0.62, 'Square44x44Logo.png')
place(50, 0.62, 'StoreLogo.png')
place_rect(480, 800, 0.40, 'SplashScreen.png')
