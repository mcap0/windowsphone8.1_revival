# Generates the Sincro tile set: white two-arrow sync glyph on a transparent background
# (the manifest BackgroundColor shows through). Drawn at 8x and downscaled for smooth edges.
# Usage: python3 make_icons.py <out_dir>
import math, os, sys
from PIL import Image, ImageDraw

SS = 8  # supersampling factor

def glyph(size):
    """Returns an RGBA image of the sync glyph filling a size x size square (no padding)."""
    n = size * SS
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = n / 2.0
    r = n * 0.36
    w = max(2, int(n * 0.085))
    white = (255, 255, 255, 255)

    def pt(theta_deg, rad=r):
        t = math.radians(theta_deg)
        return (c + rad * math.cos(t), c - rad * math.sin(t))  # image y grows downward

    for start, end, head in ((30, 150, 150), (210, 330, 330)):
        # Arc drawn clockwise in image space: PIL angles are image angles (negated math angles).
        bbox = [c - r, c - r, c + r, c + r]
        d.arc(bbox, start=-end, end=-start, fill=white, width=w)
        # Arrowhead at the arc end, pointing in the direction of travel (increasing math angle).
        t = math.radians(head)
        p = pt(head)
        dx, dy = -math.sin(t), -math.cos(t)          # travel direction (image coords)
        px, py = math.cos(t), -math.sin(t)           # perpendicular
        L, Wd = n * 0.20, n * 0.15
        tip = (p[0] + dx * L * 1.6, p[1] + dy * L * 1.6)
        d.polygon([tip,
                   (p[0] + px * Wd, p[1] + py * Wd),
                   (p[0] - px * Wd, p[1] - py * Wd)], fill=white)
    return im

def place(out, w, h, frac, name):
    g = glyph(256)  # 2048 px master, then crop and fit
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
