# The browser: how every part works

"Internet" (`apps/Browser`) is a web browser for Windows Phone 8.1 on ARM, built on **WebKit compiled from source**
(WebKitGTK 2.54.1). This page explains every part of it, from the app down to the engine, and where each line of its
code comes from.

## Every line is readable

**There is no closed-source code in the browser.** Every line that ends up in the browser package can be read:

| Part | Where the source is | Licence |
|---|---|---|
| The app (`Internet.exe`: UI, tabs, input, compose bar) | `apps/Browser/Browser` | AGPL-3.0-or-later (this project) |
| The port layer inside `webkit.dll` (C API, compositor, input, caches, blocker, media) | `engine/port` | AGPL-3.0-or-later, except the files copied from WebKit (they keep their BSD/LGPL headers) |
| WebKit (JavaScriptCore, WebCore, WTF, WebKitLegacy, bmalloc) | upstream tag `webkitgtk-2.54.1`, commit `628d6ce1850e`, fetched at build time | LGPL-2.1 / BSD-2-Clause |
| MetroSurf's changes to WebKit (32-bit ARM JIT, Windows ARM ABI, WP8.1 build) | `engine/patches/0001-metrosurf-overlay.patch`, reviewed file by file in `engine/patches/metrosurf/REVIEW.md` | same as the files they patch |
| Our changes to WebKit | `engine/patches/0002-lumia.patch` | same as the files they patch |
| Libraries linked into `webkit.dll` (curl, nghttp2, LibreSSL, ICU, HarfBuzz, SQLite, libjpeg-turbo, libpng, libwebp, libxml2, libxslt, zlib, brotli, libpsl, woff2, Skia, mimalloc) | their upstream releases, built from source | their own (all open source) |
| ANGLE (`libEGL.dll`, `libGLESv2.dll`: OpenGL ES on Direct3D 11) | Microsoft's ANGLE fork, branch `ms-master`, commit `e0a02ffcdaa5` (2016-04-18), project `winrt/8.1/windowsphone/src/angle.sln`. Our packages so far carry the prebuilt DLLs of the MetroSurf 2.0.1 package (same version, see `THIRD-PARTY.md`); the same DLLs build from this source (step 7 below, same exports and imports) and will replace them once tested on the phone | BSD-3-Clause |

### The only closed-source pieces: Microsoft's compiler runtime and the operating system

Being exact about it, four of the third-party components below are not open source. All four are Microsoft's build
tools and runtimes; **none of them holds any logic of the browser**:

| Component | What of it is in the browser |
|---|---|
| MSVC 14.44 C runtime and C++ standard library | Linked statically **into `webkit.dll`**: basic functions such as `malloc`, `memcpy`, `std::` containers. Microsoft ships their sources with Visual Studio, but under its own licence, not an open-source one. |
| Windows SDK 10.0.22621 | Headers only (the declarations of Windows' APIs). No code of it ends up in the binaries. |
| Visual Studio 2013 + Windows Phone 8.1 SDK | The compiler of the app and of ANGLE. Build only. |
| Visual C++ 2013 runtime (`msvcr120_app.dll`, `msvcp120_app.dll`, the VCLibs package) | Not in our package: a separate Microsoft package that the app and ANGLE use, like every C++ app for Windows Phone 8.1. |

This is the same situation as Firefox or Chromium built for Windows, which also use Microsoft's C runtime. Beyond
these, the browser runs on Windows Phone itself (Direct3D 11, DirectWrite, the Media Foundation decoders, XAudio2, the
kernel): the operating system, as for any app on the phone.

**MetroSurf.** [MetroSurf](https://github.com/MetroSurf/MetroSurf) first brought this WebKit to Windows Phone 8.1, and
its published (LGPL) changes to WebKit are the base of ours. MetroSurf's app is made of two more parts, a .NET shell and
a native bridge `wkwp.dll`, whose sources are not published: **neither is used here, not as code and not as
binaries.** The port layer that replaces them (`engine/port`) and the app (`apps/Browser`) were written for this
project.

## Architecture in one picture

```
 Internet.exe (C++/CX, VS2013)                         webkit.dll (clang-cl, C++23)
 ┌───────────────────────────────┐   lumiaweb.h C API  ┌─────────────────────────────────────────────────┐
 │ UI thread: XAML, address box, │ ───────────────────▶│ Web thread: JavaScript, DOM, style, layout,     │
 │ tabs, compose bar, menus      │   (every call       │ network callbacks. Run loop with lanes:         │
 │                               │    returns at once) │ input > rendering > normal > idle               │
 │ Input thread per tab:         │                     ├─────────────────────────────────────────────────┤
 │ CoreIndependentInputSource ───┼── pans, pinches ───▶│ Scrolling thread: pans, flings, zoom on the     │
 │                               │                     │ scrolling tree, at every display refresh        │
 │ SwapChainPanel per tab ◀──────┼──── frames ─────────┤ Compositor thread: layers drawn with ANGLE      │
 └───────────────────────────────┘                     │ (D3D11 feature level 9_3) into the panel        │
                ▲                                      ├─────────────────────────────────────────────────┤
                └──── events (lw_config.callback) ─────┤ Raster workers (Skia, CPU), curl, JSC helpers  │
                                                       └─────────────────────────────────────────────────┘
```

The one rule the whole design follows: **the UI and the fingers never wait for the Web thread.** A page can run
JavaScript for two seconds and the page still scrolls, the address box still types, the tab switcher still opens.

## The engine (`engine/`)

### How it is built

- `engine/configure.sh` configures WebKit's own CMake build (`PORT=Win` with the `WP81` option set) for ARMv7
  Thumb-2, cross-compiling from Linux with clang-cl and lld-link (`engine/cmake/wp81-arm.cmake`, `-mcpu=krait`).
  The sysroot is MSVC + Windows SDK headers and libraries; the import libraries are generated **from the phone's own
  DLLs** (`engine/tools/implib.py`), so the engine can only call what Windows Phone 8.1 really exports.
- `engine/tools/checkimports.py` checks every DLL and function `webkit.dll` imports against the phone after each link
  (API sets resolved with the phone's schema). `engine/port/shims/wp81shims.c` provides the few Win32 functions
  WebKit calls and the phone lacks (`LoadLibraryW` over `LoadLibraryExW`, `CopyFileW` over `CopyFile2`...).
- WebKit is not copied into this repository: the tag is checked out and `engine/patches/` applied on top
  (`engine/tools/exportpatches.py` writes the patches back from a working tree).
- The final engine is built with **ThinLTO and PGO**: `tools/pgo` builds an instrumented `webkit.dll`, runs it on the
  phone through fixed bench pages, live sites and test pages, brings the profile back and builds again with it
  (`engine/pgo/`). Identical functions are folded where it is safe (`/OPT:SAFEICF`).
- The result is one `webkit.dll` (~60 MB, everything static) that exports only the `lw_*` functions of
  `engine/include/lumiaweb.h`.

### What MetroSurf's patches do

Upstream WebKit 2.54 switches the JavaScript JIT off on every 32-bit CPU. MetroSurf switched the Baseline JIT, the DFG
optimising JIT and the RegExp JIT back on for 32-bit ARM Windows and made them work: the modern inline-cache design
ported to 32-bit, the Windows ARM calling convention (LLInt PC register, callee-saved registers, instruction cache
flush), plus the build for WP8.1, the ANGLE/Direct3D 9_3 graphics glue, DirectWrite fonts and pointer events. This is
the most valuable part of the engine and it is taken as it is; see `engine/patches/metrosurf/REVIEW.md` for the
verdict on every file.

### What our patches to WebKit do (`0002-lumia.patch`)

- **Priority run loop** (WTF `RunLoop`): the Web thread's queue has lanes. Input tasks (taps, keys, edits from the
  compose bar, the app's commands) run before the rendering update, which runs before network and timer tasks, which run
  before idle work. Without this, a tap waits behind every task the page has queued.
- **Activity recorder** (`wtf/Activity.h`): long tasks of the Web thread are labelled (js, style, layout, parser,
  timer, render) and logged, so slowness is measured, not guessed.
- **Style without full rebuilds**: a web font that finishes loading, a touch listener added to the document, or a late
  `@keyframes`/`@supports` sheet used to recompute the style of every element (350-480 ms each on a 2013 phone); now
  only the affected elements are invalidated.
- **Load pacing**: while a page loads, an expensive rendering update is followed by a longer pause (never near a touch
  or a key), so the parser and scripts get the CPU.
- **JavaScript bytecode cache hooks** (`bindings/js`): compiled bytecode is written to disk and reused on the next visit.
  Fixes in JSC for stale entries after garbage collection.
- **Network** (curl): HTTP disk cache in front of curl, authentication, downloads that take over a page's own load,
  WebSockets, the ad and tracker blocker hook.
- **Compositing** (TextureMapper, coordinated graphics): drawing limits of Direct3D feature level 9_3, frame pacing,
  damage tracking, the tap highlight.
- **Memory**: WebKit's memory pressure policy tied to the limit the OS gives the app.

### The port layer (`engine/port`)

WebKit needs a "port": the code that connects WebCore to a real platform. On Windows Phone there was none we could
use, so this one was written for the phone, borrowing from WebKit's own ports where they fitted.

| File | What it does |
|---|---|
| `Engine.cpp/.h` | The C API of `lumiaweb.h`. Starts the Web thread (WTF's main thread), turns every API call into a task on the right lane, sends events back to the app, handles suspension and resume, memory pressure, crash logging. |
| `WebPage.cpp/.h` | One tab: the `WebCore::Page`, its main frame, settings, and the `LayerTreeHost` that hands the page's layers to the compositor. |
| `ChromeClient`, `FrameLoaderClient`, `EditorClient` | WebCore's client interfaces: windows and popups, loading policy and events, focus and text of editable fields. |
| `compositor/` | The threaded compositor from WebKit2's PlayStation port (BSD), adapted to one process: `ThreadedCompositor` draws the layer tree on its own thread at every display refresh; `AcceleratedSurface` is the EGL surface on the tab's `SwapChainPanel` through ANGLE. Tiles are rasterized by Skia on CPU workers. |
| `ScrollingTreeDispatcher` | WebKit2's EventDispatcher in one process: pans and flings are applied to the scrolling tree on the scrolling thread, without the Web thread. Only touches over non-passive touch listeners wait for the page. |
| `loader/` | WebKitLegacy's resource load scheduler, so loads go through curl's `ResourceHandle`. |
| `DiskCache` + `networkcache/` | HTTP disk cache: WebKit2's network cache storage, with HTTP freshness and revalidation rules. |
| `BytecodeCache` | Storage and policy of the JavaScript bytecode cache on flash. |
| `Blocker` + `blocklist/` | Ad and tracker blocker: a sorted table of ~3,500 host names (Peter Lowe's list + ours) checked before every request. |
| `Storage`, `VisitedLinks`, `BackForwardList` | localStorage/sessionStorage/IndexedDB (WebKitLegacy's in-process SQLite implementations), `:visited`, session history. |
| `Downloads`, `Authentication`, `Permissions`, `Pickers` | Downloads, HTTP passwords, permission questions/geolocation/notifications/share, `<select>` and date pickers: each asks the app with an event and waits for its reply without blocking it. |
| `SocketStreamHandleImplCurl` | WebSocket byte stream over curl. |
| `media/` | Own media pipeline: MP4/MP3/ADTS demuxers, the phone's Media Foundation decoders, XAudio2 output as the clock, YUV video layer. **Built, not yet proven on the phone.** |
| `Profiler` | Sampling profiler of the Web thread, symbolized on the PC with the link map. |
| `tests/` | `jsctest` (JavaScriptCore alone, for the "Prova motore" app) and `demuxtest` (demuxers checked against ffprobe). |

## The app (`apps/Browser`)

A C++/CX XAML app for Windows Phone 8.1 (Visual Studio 2013, `v120_wp81`). It loads `webkit.dll` at run time, so a
missing DLL is reported instead of killing the app at start.

- **Tabs**: one `SwapChainPanel` and one engine view per tab. Hidden tabs stop rendering; under memory pressure a tab
  is discarded down to its address and title and loads again when shown. Tabs come back after the system closed the app.
- **Touch**: each panel has its own `CoreIndependentInputSource` on its own thread. A move beyond 8 px is a pan
  (`lw_input_pan`, applied on the scrolling thread); anything shorter is a tap (`lw_input_tap`, the Web thread's input
  lane). Pinch and double-tap zoom are animated by the compositor.
- **Compose bar**: the page's text fields are edited in a native XAML text box above the keyboard. Every keystroke is
  drawn by XAML immediately; the engine receives only the resulting edit (`lw_text_edit`, numbered so late answers from
  the page are recognised). This is why typing stays fluid on heavy pages.
- **Address box** with suggestions from favourites and history, find in page, long-press menu (open, copy, save
  image), downloads, file picker, form pickers, permission questions, mobile/desktop version per site.
- **Site apps** (`Site.h`, `apps/Browser/sites/`): the same shell and engine built as a single-site app (Gmail, Poste)
  with its own cookies and the login kept in a PIN vault (`Vault.cpp`: PBKDF2 + AES-256-GCM, then DPAPI; all crypto from
  the system).

## Why it is fast

Measured on a Lumia 1020 (dual-core Krait 1.5 GHz, 2 GB), against MetroSurf 2.0.1 on the same phone:

1. **Threads.** MetroSurf runs WebKit with WebKit2 off and the generic event loop: touches, keys, network, timers and
   the page share one first-in-first-out queue, which Google's home page blocked for up to 2.5 s. Here input never goes
   through the page's queue (scrolling thread, input lane), and the compositor draws on its own thread.
2. **Always on the GPU.** MetroSurf's shell falls back for good to software drawing (a 4 MB bitmap per frame passed
   through .NET) at the first redraw request; here every frame is composited with ANGLE into the swap chain.
3. **Input that does not wait.** No double delivery of every touch, no resending of the whole text on every key, no
   script injected into every page: native compose bar and a touch map that only waits where the page listens.
4. **Caches.** HTTP disk cache and JavaScript bytecode cache on flash.
5. **JIT fixes.** After an inline-cache fix: 300k objects 196-241 ms (MetroSurf 482-576), 100k strings 14-19 ms
   (107-115), 20k regexps 43-48 ms (171-195).
6. **Less work in WebCore.** Tracker blocker, no full style rebuilds, load pacing: poste.it went from 11.8 s to 3.4 s
   to load, repubblica.it from 6.3 to 3.3 s, wikipedia.org from 3.5 to 2.2 s, on the same engine version.
7. **Compiler.** clang tuned for Krait, ThinLTO and a profile collected on the phone itself (20-30% on page loads).

## Third-party components, one by one

Everything below comes from somewhere else, and every one of them can be fetched by anyone from its original source.
Nothing in the build depends on a file only we have.

| Component | Version | Where to get it | Licence | Used for |
|---|---|---|---|---|
| WebKit | tag `webkitgtk-2.54.1` (commit `628d6ce1850e17225185a5807a9d0aa375bae2be`) | https://github.com/WebKit/WebKit | LGPL-2.1 / BSD-2-Clause | the engine |
| MetroSurf's WebKit changes | v2.0.1 | https://github.com/MetroSurf/MetroSurf (already in this repository as `engine/patches/0001-metrosurf-overlay.patch`) | as the files they patch | 32-bit ARM JIT, WP8.1 build |
| ANGLE (Microsoft's WinRT fork) | `ms-master`, commit `e0a02ffcdaa58405c7660a552e723606f5ae12a9` (version 2.1.8) | https://github.com/microsoft/angle | BSD-3-Clause | OpenGL ES 2 on Direct3D 11: `libEGL.dll`, `libGLESv2.dll` |
| zlib | 1.3.2 | https://github.com/madler/zlib | zlib | compression |
| brotli | 1.2.1 | https://github.com/google/brotli | MIT | `Content-Encoding: br`, WOFF2 |
| nghttp2 | 1.70.0 | https://github.com/nghttp2/nghttp2 | MIT | HTTP/2 |
| LibreSSL | 4.3.3 | https://github.com/libressl/portable | OpenSSL/ISC | TLS |
| libpsl | 0.23.3 | https://github.com/rockdaboot/libpsl | MIT | public suffix list (cookies) |
| curl | 8.23.0 | https://github.com/curl/curl | curl (MIT-like) | the network stack |
| ICU | 77.1 (code + `icudt77l.dat` data) | https://github.com/unicode-org/icu | Unicode | text, locales |
| HarfBuzz | 14.6.0 | https://github.com/harfbuzz/harfbuzz | MIT | text shaping |
| libjpeg-turbo | 3.2.0 | https://github.com/libjpeg-turbo/libjpeg-turbo | BSD-3-Clause / IJG / zlib | JPEG |
| libpng | 1.6.59 | https://github.com/pnggroup/libpng | libpng | PNG |
| libwebp | 1.6.0 | https://chromium.googlesource.com/webm/libwebp | BSD-3-Clause | WebP |
| libxml2 | 2.15.4 | https://gitlab.gnome.org/GNOME/libxml2 | MIT | XML |
| libxslt | 1.1.45 | https://gitlab.gnome.org/GNOME/libxslt | MIT | XSLT |
| SQLite | 3.54.1 | https://www.sqlite.org | public domain | localStorage, IndexedDB, cookies |
| woff2 | 1.0.2 | https://github.com/google/woff2 | MIT | web fonts |
| Skia, mimalloc, simde, and WebKit's other bundled libraries | as in the WebKit tag | inside WebKit's `Source/ThirdParty` | their own (BSD/MIT) | drawing, memory allocator |
| Mozilla CA bundle | any current one | https://curl.se/docs/caextract.html | MPL-2.0 | HTTPS root certificates (`cacert.pem`) |
| Ad and tracker list | — | https://pgl.yoyo.org/adservers/ (already in `engine/port/blocklist/pgl.txt`) | McRae GPL, credit required | the blocker |
| LLVM (clang-cl, lld-link, llvm-dlltool, llvm-profdata) | 21.1.8 | https://llvm.org | Apache-2.0 with LLVM exception | compiler (build only) |
| MSVC 14.44 headers + static CRT, Windows SDK 10.0.22621 (ARM) | — | Microsoft, downloaded with xwin (https://github.com/Jake-Shadle/xwin) | Microsoft's licence, you accept it yourself | headers and C runtime (build only) |
| Visual Studio 2013 + Windows Phone 8.1 SDK | Update 5 | Microsoft | Microsoft's licence | building the app and ANGLE (build only) |

The import libraries the engine links against are made from Windows Phone 8.1's own system DLLs, so that a function
the phone lacks is a link error on the PC. Their export lists are in `engine/sysroot/phone/*.def` (names only, no
Microsoft code); the `.lib` files are generated from them (step 3 below).

## Rebuild the app package yourself

You do not need anything from us beyond this repository. What follows rebuilds `Browser_<version>_ARM.appx` from
nothing. Some paths and addresses in the scripts are those of our setup (a Fedora PC, a Windows VM at
`192.168.122.21`, the PC at `192.168.1.148` as the app's default start page and log receiver): change them to yours.

**On a Linux PC** (we use Fedora; 8 cores and 32 GB are comfortable, a WebCore source file takes up to 2 GB):

1. Tools: LLVM 21 (`clang-cl`, `lld-link`, `llvm-lib`, `llvm-dlltool`, `llvm-profdata`), `cmake`, `ninja`, `ruby`,
   `perl` (with `JSON::PP`, `English`, `FindBin`), `python3`, `gperf`, `git`, `curl`, `unzip`.
2. MSVC sysroot for ARM32, with [xwin](https://github.com/Jake-Shadle/xwin) (you accept Microsoft's licence):
   ```
   xwin --accept-license --arch aarch --crt-version 14.44 --sdk-version 10.0.22621 splat --output engine/sysroot/msvc
   ```
   (`engine/sysroot/msvc/crt` and `engine/sysroot/msvc/sdk` are what `engine/cmake/wp81-arm.cmake` expects.)
3. Import libraries of the phone, from the `.def` files in this repository:
   ```
   cd engine/sysroot/phone
   for d in *.def; do llvm-dlltool -m arm -d "$d" -l "${d%.def}.lib"; done
   cp kernelbase.lib advapi32.lib; cp kernelbase.lib synchronization.lib   # WP8.1 has no ADVAPI32.DLL: KERNELBASE exports them
   echo > empty.c && clang-cl --target=armv7-pc-windows-msvc -c empty.c -Foempty.obj
   for l in dbghelp DbgHelp winmm WinMM; do llvm-lib /machine:arm /out:$l.lib empty.obj; done   # empty on purpose
   ```
   With a dump of a phone's `Windows\System32` you can instead make them from the DLLs themselves:
   `python3 engine/tools/implib.py <System32>/KERNELBASE.DLL ... -o engine/sysroot/phone`.
4. Dependencies: `engine/deps/fetch.sh` downloads the pinned sources and checks them against
   `engine/deps/SHA256SUMS`; `engine/deps/build.sh` builds them all as static libraries into `engine/out/deps`.
5. WebKit:
   ```
   git clone --filter=blob:none --branch webkitgtk-2.54.1 https://github.com/WebKit/WebKit.git engine/webkit
   cd engine/webkit
   git apply ../patches/0001-metrosurf-overlay.patch
   git apply ../patches/0002-lumia.patch
   ```
6. Engine: `engine/configure.sh`, then `tools/enginebuild webkit` (about 45 minutes from nothing). The result is
   `engine/out/webkit/bin/webkit.dll`. For the faster ThinLTO + PGO build, see `tools/pgo`: the profile is collected on
   a phone, so it is not in the repository; without it you get the plain build, 20-30% slower on page loads.

**On Windows** (Visual Studio 2013 Update 5 with the Windows Phone 8.1 tools):

7. ANGLE: clone https://github.com/microsoft/angle, check out `e0a02ffcdaa58405c7660a552e723606f5ae12a9`, open
   `winrt/8.1/windowsphone/src/angle.sln` and build **Release | ARM**. Copy `libEGL.dll` and `libGLESv2.dll` to
   `engine/out/angle/bin/` on the PC.
8. The app: `tools/browser stage` copies `webkit.dll`, the two ANGLE DLLs, `icudt77l.dat`, `lumiaweb.h` and a CA
   bundle (`cacert.pem`) into `apps/Browser/Browser/`. Then build `apps/Browser/Browser.sln` as **Release | ARM**
   (from the PC, `tools/browser` does it through the VM with `tools/wp81build`). The package is in `AppPackages/`.

**On the phone:** a developer-unlocked Windows Phone 8.1 on ARM. Install with the SDK's deployment tool
(`AppDeployCmd /install Browser_<version>_ARM.appx /targetdevice:de`, or `/update` to keep the data of an earlier
install). The Visual C++ 2013 runtime for phones (`Microsoft.VCLibs.ARM.12.00.Phone.appx`, from the SDK) must be
installed once.

## Ready-made package, for convenience

To save you the steps above, the [`builds/`](../builds) folder of this repository carries a ready-made
`Browser_<version>_ARM.appx`. It is only a convenience: anyone can rebuild the package from this page.

**Be aware of how the current package differs from these sources.** The first package, **0.1.0.64**, is the last
version tested on the phone, and it is not an exact build of this repository:

- **Code from a day earlier.** It was built on 2026-11-11 at 13:00 from our working tree of that moment; this
  repository holds the code of a few hours later (0.1.0.67), which adds the first stage of the media pipeline
  (`engine/port/media`) and small changes in the compositor and `WebPage`. Building this repository gives you
  0.1.0.67, not 0.1.0.64. Nothing in 0.1.0.64 is missing here: the differences are additions.
- **ThinLTO + PGO engine.** Its `webkit.dll` was optimised with a profile collected on our phone, which is not in the
  repository (`tools/pgo` makes your own). Without it you get the plain build: the same code, 20-30% slower on loads.
- **MetroSurf's prebuilt ANGLE.** Its `libEGL.dll` and `libGLESv2.dll` are the ones from the MetroSurf 2.0.1 package
  (MD5 in `THIRD-PARTY.md`), not built by us from source.

The next tested package will be built from exactly the sources of its release tag, with our own ANGLE.

## Status

Work in progress and **not a safe browser**: no sandbox, no security updates, custom network and cache code. Audio and
video are not proven yet. See the Status section of the main README.
