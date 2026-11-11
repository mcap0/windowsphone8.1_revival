# Posta icons

Source artwork: `source_start.tile.email_158.png` (158x158, transparent, white envelope).
Origin: Windows Phone 8.1 firmware image (read-only copy, MainOS.orig.bin):
- `Windows/system32/Manifests/EmailPackMan.xml` (ProductID 5B04B775-356B-4AA0-AAF8-6491FFEA5614, Email)
  references `res://StartAssets{ScreenResolution}!start.tile.email.png` (and `.small.png`).
- Resource source: `Windows/system32/StartAssets768x1280.dll`. The PNG was picked by visual match from the
  PNG resources in that DLL (the resource-name to PNG mapping was not parsed).
- Extraction (host): `ntfscat -f <image> 'Windows/system32/StartAssets768x1280.dll'`, then PNG scan.

Licence: Microsoft firmware asset, used for personal use on the owner's own device only. Not for redistribution.

Generation (host, Pillow):
    python3 make_icons.py source_start.tile.email_158.png out
Then copy `out/*.png` over `../Assets/` with the same names (done for version 1.0.1.0).
Tile background transparent; BackgroundColor #1F6FEB in Package.appxmanifest.
