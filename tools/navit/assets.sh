#!/usr/bin/env bash
# assets.sh — package data for Mappe (Navit on WP8.1), run inside the dumbphone-navit container from /w:
#   podman run --rm --security-opt label=disable -v "$PWD":/w -w /w dumbphone-navit tools/navit/assets.sh
# Writes apps/Mappe/Mappe81/navit/{icons/*.png, locale/it/LC_MESSAGES/navit.mo, navit_layout_*.xml}.
# Icons: Navit ships SVG/XPM; the d2d graphics plugin loads PNG through WIC, so everything becomes PNG here.
set -Eeuo pipefail
src=ref/navit/navit
out=apps/Mappe/Mappe81/navit
size=${ICON_SIZE:-48}
mkdir -p "$out/icons" "$out/locale/it/LC_MESSAGES"

n=0
for f in "$src"/icons/*.svg "$src"/icons/*.svgz; do
    [ -e "$f" ] || continue
    name=$(basename "$f"); name=${name%.svgz}; name=${name%.svg}
    if [[ $f == *.svgz ]]; then
        gunzip -c "$f" | rsvg-convert -w "$size" -h "$size" -o "$out/icons/$name.png" || echo "skip $f"
    else
        rsvg-convert -w "$size" -h "$size" -o "$out/icons/$name.png" "$f" || echo "skip $f"
    fi
    n=$((n + 1))
done
for f in "$src"/icons/*.png; do
    [ -e "$f" ] || continue
    cp "$f" "$out/icons/"
done
python3 - "$src/icons" "$out/icons" <<'PY'
import os, sys
from PIL import Image
src, out = sys.argv[1:]
for name in sorted(os.listdir(src)):
    if not name.endswith('.xpm'):
        continue
    png = os.path.join(out, name[:-4] + '.png')
    if os.path.exists(png):
        continue  # an SVG of the same icon wins
    try:
        Image.open(os.path.join(src, name)).convert('RGBA').save(png)
    except Exception as e:
        print('skip', name, e)
PY
echo "icons: $(ls "$out/icons" | wc -l) png"

# Italian maneuvers: po/it.po.in is a normal .po with the package name placeholder
sed 's/@PACKAGE@/navit/g' ref/navit/po/it.po.in > /tmp/it.po
msgfmt -o "$out/locale/it/LC_MESSAGES/navit.mo" /tmp/it.po
echo "navit.mo: $(stat -c %s "$out/locale/it/LC_MESSAGES/navit.mo") bytes"

cp "$src/navit_layout_car_shipped.xml" "$src/navit_layout_car_dark_shipped.xml" "$out/"
echo "layouts copied"
