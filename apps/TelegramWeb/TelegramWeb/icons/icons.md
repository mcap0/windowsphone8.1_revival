# TelegramWeb icons

Source artwork: `source_telegram_logo.svg` (official Telegram logo, 240x240 viewBox).
Downloaded from Wikimedia Commons: https://upload.wikimedia.org/wikipedia/commons/8/82/Telegram_logo.svg
(Telegram logo; trademark of Telegram. Used only to identify the app on the owner's own phone.)

Variant: white disc with the paper plane in the tile colour #2AABEE (inverted official logo),
transparent background, so the manifest BackgroundColor #2AABEE shows through.
Disc = 62% of tile width (small tiles and StoreLogo), 40% of width on the splash.

Generation (host, Pillow + ImageMagick with the RSVG delegate):
    magick -background none -density 600 <recoloured svg> -resize 1024x1024 big.png
    python3 make_icons.py source_telegram_logo.svg out
(make_icons.py recolours the SVG fills, renders at 1024 px, then downsamples with LANCZOS.)
Output in `out/`, copied to `../Assets/` with the same names (version 1.0.1.0).
