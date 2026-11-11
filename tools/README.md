# tools

Helpers to drive the `Windows-Debugger` libvirt VM (SSH `user@192.168.122.21`, NAT link must be up:
`virsh -c qemu:///system domif-setlink Windows-Debugger vnet0 up`).

- `vm shot|type|key` — screenshot / keyboard of the VM console (Italian layout)
- `vps script.ps1` — run a PowerShell script in the VM
- `uwpbuild <dir> <csproj>` — sync `apps/` and build Release|ARM (.NET Native) with VS 2019 Build Tools
- `perletest [args]` — sync `apps/Perle` and run `Perle.Tests` with the .NET 8 SDK in `C:\dotnet`

VM layout: `C:\lumia\iutool` (iutool/getdulogs), `C:\lumia\pkgs10586` (W10M cabs), `C:\lumia\src` (sources), `C:\lumia\out` (appx).
The phone is passed to the VM with `virsh attach-device Windows-Debugger lumia-usb.xml --live` (USB 0421:0661).
- After a phone reboot the USB device number changes: re-pass it (`detach-device` then `attach-device` with `lumia-usb.xml`); `wp81build ... install` does it by itself.
- `onenote ["Phone development"]` — downloads the phone's OneNote section (`...\OFFICE\Local\OneNote\OneNote Notebooks\Personal\Unfiled Notes.one`)
  via Esplora (app open, console on) and prints the latest version of each page as a tree with `[x]`/`[ ]` checkboxes;
  copy in `share/onenote/` (`unfiled.one`, `pages.txt`). Parser = pyOneNote patched by `onenote.d/patch.py` (venv in ~/.cache).
- `tvlab/` — PerleTV casting from the PC to the Samsung "Tele" (192.168.1.250:9197/dmr), to break the TV on purpose:
  `lab.py` mirrors StreamServer (YouTube HLS via the app's visionOS request in `yt.py`, segment cache, same /v.ts and
  /s.srt headers) and CastController (same SOAP/DIDL); `remux` and `srt` are built from the app's TsDemux/TsMuxer/Srt
  (`g++ -std=c++14 -O2 -o remux remux.cpp ...`, see the top of each .cpp). Scenarios: `python3 tvlab/wedge.py`,
  `recipe.py`, `stall.py` (log on stdout, seconds since start). Don't cast from the phone meanwhile.
- `phoneshot [n]` — copies the phone's last n screenshots into `share/screens` over USB (MTP through the VM):
  nothing to do on the phone, no Esplora console.
- `phonelog [product-id]` — prints an app's `run.log` from the phone (default "Internet", `afcc45ef-…`): exports its isolated
  storage with ISETool through the VM, then reads it. The app must be closed.
- `sym.py 0x239AF5 …` / `sym.py --stack "…"` — symbolises `webkit.dll+offset` from ABORT/ECCEZIONE lines against
  `engine/out/webkit/bin/webkit.map` (check that the map is from the same link as the dll).
- `trace.py [--runs N] [--slow]` — frame, tap, pan and Web-thread activity numbers from `share/inbox/internet.txt` (app "Internet").
