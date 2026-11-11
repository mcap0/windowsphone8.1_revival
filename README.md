# Windows Phone 8.1 revival

**A 2013 Nokia Lumia 1020 as a daily phone in 2026.** Microsoft switched the platform off; this repository
switches it back on: a current WebKit, push notifications that arrive instantly again, a voice assistant that works
with no network, Telegram, mail, maps, music, 2FA codes, and the apps of an Android phone on the Lumia's screen.

No store, no Microsoft services, no emulation: native code for a dual-core ARM phone with 2 GB of RAM.

Everything here was built for one rooted/interop-unlocked Lumia 1020. It is a personal project published as it is:
comments and UI strings are partly in Italian, the build scripts assume my setup (a Linux host driving a Windows VM
with Visual Studio 2013/2019), and a lot of it is unfinished. Read [Status](#status-what-works-and-what-does-not)
before expecting anything.

## What is new here

As far as I could find, these had not been done on Windows Phone 8.1 before:

- **Live push notifications without Microsoft's servers** — the phone's own WNS/MPNS push client is pointed at a
  server of ours, so apps get real toasts (and raw pushes) with the app closed and the phone locked, on Wi-Fi and
  on 4G. The wire protocol was read out of `WPNPRV.DLL`; the server is ~400 lines of Python (`tools/wnsfake`) or
  Java (`ponte/agent/.../WnsServer.java`). Full guide, including how to use it from your own app:
  **[docs/live-notifications.md](docs/live-notifications.md)**.
- **Native incoming-call screen from a push** — a locked Lumia rings with the system call UI from a raw push of
  that server (`apps/ProvaVoip`): the VoIP agent registration, the out-of-process host that a Silverlight 8.1
  package needs, and the CRT-free 3.5 KB host exe that makes it start.
- **A fast browser on a current WebKit** — `engine/` builds WebKitGTK 2.54.1 from source with clang for ARM
  Windows Phone 8.1. It stands on the work of [MetroSurf](https://github.com/MetroSurf/MetroSurf), who first
  brought this WebKit to the platform: their changes to WebKit are the base (`engine/patches/metrosurf`), ours come
  on top (`engine/patches/0002-lumia.patch`) together with a port layer written here (`engine/port`: threaded compositor on ANGLE/Direct3D 11 level 9_3, own network and disk cache, ad blocker,
  ThinLTO + PGO with profiles collected on the phone). `apps/Browser` ("Internet") is the browser on top of it.
- **Offline voice assistant** — `apps/Assistente` understands Italian commands (timers, alarms, calls, music, open
  app, notes, calendar, weather) with nothing leaving the phone: speech through a Vosk/Kaldi build for WP8.1 ARM
  (`vosk/`), and intent parsing by a tiny neural network whose weights are *written by hand*, not trained
  (`tools/wsp/assistente.py` generates `intent.wsp`, ~36k weights, almost all zero). A second, experimental
  recogniser (`tools/wsr`) is built from scratch around the Lumia's microphone.
- **A Tailscale client in C++11 for WP8.1** — `apps/TsNet`: control protocol, DERP relay, WireGuard and a small
  userspace TCP, on mbedTLS, as a static library. Apps reach a tailnet node from inside the app sandbox, no VPN
  support needed from the OS.
- **Android apps on the Lumia** — `apps/Remoto` shows and drives apps running on an Android phone (H.264 stream
  of a private virtual display, touch, keyboard, clipboard, microphone), over LAN or Tailscale; the Android side
  is `ponte/`.
- **A native Telegram client core in portable C++** — `apps/TgNative` (MTProto 2.0, own TL schema runtime) with a
  C++/CX app on top (`apps/Telegram`), plus background notifications from the app's own background task.

## Layout

| Path | What |
|---|---|
| `engine/` | WebKit port: `port/` (our port layer), `patches/` (our changes to WebKit as patches), `cmake/`, `tools/`, `pgo/`. WebKit itself is not vendored. |
| `apps/Browser` | "Internet": the browser UI (C++/CX, XAML) |
| `apps/Assistente` | Voice assistant, offline first; optional Gemini fallback with your own key |
| `apps/Telegram`, `apps/TgNative`, `apps/Avvisi` | Native Telegram client, its portable core, and notification tasks |
| `apps/TelegramNative` | Fork of the official Telegram for Windows Phone (GPL-2), kept to build on 8.1 |
| `apps/TelegramWeb` | Telegram Web in an embedded view (older experiment) |
| `apps/Mappe` | Maps: a port of Navit (GPL-2) plus our UI and OpenStreetMap search |
| `apps/Posta` | IMAP/SMTP mail client with a background task |
| `apps/Musica` | Music player (local library and online sources) |
| `apps/Perle`, `Perle81`, `PerleTV` | YouTube client and DLNA casting to a TV |
| `apps/Meteo`, `Orologio`, `Calendario`, `Sincro`, `Appunti`, `File`, `Registratore`, `Serpente` | Weather (design and assets from [Haru](https://github.com/FriendsNone/Haru) by FriendsNone, MIT), clock/alarms, calendar, Google sync, clipboard from the PC, file manager, recorder, snake |
| `apps/Authenticator81` | TOTP/HOTP codes with QR import (ZXing) |
| `apps/Cerca`, `Risultati` | What the hardware Search key opens instead of Bing |
| `apps/Remoto`, `FotoPixel`, `Telecomando` | Android apps on the Lumia; photo upload through an Android phone; Samsung TV remote |
| `apps/TsNet` | Tailscale client library (used by Remoto, FotoPixel, Musica, Telecomando) |
| `apps/ProvaVoip`, `ProvaChiamate` | Test apps for push, VoIP agents and the native call screen |
| `apps/Esplora` | File/registry explorer with a remote console (the phone-side half of most `tools/`) |
| `apps/Claude` | Small chat client for the Claude API (your own key) |
| `ponte/` | Android side: `agent/` (Remoto server, push server, run as the shell user through `app_process`), `app/` (companion app with a local adb client and the Remoto keyboard) |
| `pixelfoto/` | Android app that receives the Lumia's photos and files them for backup |
| `vosk/` | Vosk + Kaldi + OpenFst + OpenBLAS for WP8.1 ARM: patches and build |
| `batteria/` | Battery probe that runs as a system service slot, and its report tool |
| `tools/` | Host tools: build through the VM, deploy, logs, screenshots, profiling, reverse engineering helpers, push server |
| `share/browser/` | Test pages for the browser |
| `docs/` | Guides |

## Status: what works and what does not

This is work in progress. Plainly:

- **Browser**: loads and renders modern sites, scrolls and types responsively, has tabs, downloads, a password
  vault and an ad blocker. **Audio/video does not work yet**: the media pipeline (`engine/port/media`, own demuxers
  + Media Foundation decoders) builds and installs but has not been proven on the phone; MSE/HLS/WebM are not
  there. Many web features are missing or partial. It is slow on heavy pages (the CPU is a 2012 dual core).
  **Do not treat it as a safe browser**: it is an unreviewed port with disabled features, custom network and cache
  code, no sandbox and no security updates. Do not use it for banking or anything you care about.
- **Push server**: works on Wi-Fi and 4G for toasts and raw pushes. Redirecting the push client is global: while
  it points to your server, Microsoft-delivered pushes (what is left of them) no longer arrive.
- **Assistant**: offline commands work in Italian only; recognition speed and model loading are still being
  tuned; `tools/wsr` is an experiment.
- **Telegram (native)**: login, chats, messages and notifications work; many message types and features are missing.
- **Remoto**: works on LAN and through Tailscale; keyboard/clipboard/microphone passthrough are recent and rough.
- **Calls**: the native ring from a push works in the test app; there is no finished calling app here.
- **Everything else**: small apps written for my own use, tested on one phone, with no guarantee.

Code quality varies. Parts were written quickly, parts were written with AI assistance, and none of it has had a
security review. Network-facing code (the Tailscale client, the MTProto core, the push server, the mail client)
is hand-rolled and should be assumed to have bugs.

## What is deliberately not in this repository

- Keys and credentials. You need your own: Telegram `api_id`/`api_hash` (`ApiKeys.h`), Gemini key
  (`tools/geminikey`), Google OAuth client (`tools/google-auth`), signing certificate. The tools read them from an
  untracked `secrets/` directory.
- Anything extracted from Microsoft's ROM or other proprietary software: system DLLs, speech data, keyboard
  lexicons, icons. The patch tools under `tools/` (`searchpatch`, `patchie`, `tools/re`) describe what to change in
  files you pull from your own phone; they ship no binaries.
- WebKit, Kaldi, OpenFst, OpenBLAS and Vosk sources: fetched at build time, our changes are in `engine/patches/`
  and `vosk/patches/`.
- Build output and the development journal.

## Building

There is no one-command build. In short:

- **Windows side**: a VM with Visual Studio 2013 (WP8.1 SDK, C++/CX and Silverlight 8.1 projects) and VS 2019
  Build Tools; the host talks to it over SSH (`tools/vmssh`, `tools/vps`, `tools/wp81build`). Addresses such as
  `user@192.168.122.21`, `192.168.1.x` and the `~/dumbphone` checkout path are hard-coded in the scripts and app
  defaults: change them to yours.
- **Engine**: `engine/configure.sh` and `tools/enginebuild` cross-compile WebKit with clang against a WP8.1 sysroot
  taken from the SDK (not included).
- **Android side**: `ponte/agent/build.sh`, `ponte/app/build.sh`, `pixelfoto/app/build.sh` (javac + d8, no Gradle).
- **Phone**: interop-unlocked with capabilities unlocked; several features need registry writes or patched system
  files, which can brick a phone. Keep a full backup.

## Licence

The original code in this repository is licensed under the **GNU Affero General Public License, version 3 or
later** ([LICENSE](LICENSE)): if you run a modified version as a network service (the push server, for instance),
you must offer its source to the users of that service.

Exceptions, where other licences apply:

- `apps/Mappe` links Navit, which is GPL version 2 only: everything under `apps/Mappe` is **GPL-2.0-only**.
- `apps/TelegramNative` is the official client's fork: **GPL-2.0-or-later**.
- `apps/Perle`, `apps/Perle81`, `apps/PerleTV` contain code derived from NewPipe: **GPL-3.0-or-later**.
- `engine/patches` and `vosk/patches` carry the licence of the files they patch.
- Everything else listed in [THIRD-PARTY.md](THIRD-PARTY.md) keeps its own licence.

This project is not affiliated with Microsoft, Nokia, Telegram, Google, Tailscale or anyone else named here.
