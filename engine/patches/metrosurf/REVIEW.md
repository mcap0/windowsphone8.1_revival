# MetroSurf overlay review (vs webkitgtk-2.54.1, 628d6ce1850e)

Source: `ref/MetroSurf/webkit` (LGPL, published by MetroSurf). Diffs in this folder, sizes in `INDEX.txt`
(+1419 −711 in 113 files, 2 new files). Verdicts: **keep** = take as is into our series, **rewrite** = same goal,
our own implementation, **drop** = not taken (reason given), **check** = decide when it builds/runs.

## The big one: JIT on 32-bit ARM Windows — keep, and test hard
Upstream 2.54 disables the JIT on every 32-bit build (`PlatformEnable.h`: `USE(JSVALUE32_64)` → `ENABLE_JIT 0`,
DFG off). MetroSurf turns JIT + DFG back on for `OS(WINDOWS) && CPU(ARM_THUMB2)` and makes it work:
- `PlatformEnable.h`, `Options.h` (`isWP81ARMJIT`, `jitEnabledByDefault`), `OptionsList.h` (DFG on, inline caches
  not forced to fail) — keep.
- `DFGSpeculativeJIT32_64.cpp` (+295 −367), `DFGSpeculativeJIT.*`, `DFGJITCompiler.*`, `DFGByteCodeParser.cpp`,
  `InlineCacheCompiler.*`, `AssemblyHelpers.*`, `BaselineJITRegisters.h`, `BaselineJITCode.h`,
  `JITInlineCacheGenerator.h`, `JSCJSValue.h`, `ScriptExecutable.cpp`: the modern handler-IC design ported to
  32-bit (`moveToICRegisters`/`shuffleRegisters`) — keep. Upstream no longer tests this code: we run a JSC
  stress/test262 subset on the phone for every engine build (plan, Verification).
- Windows ARM ABI: `MachineContext.h` (LLInt PC is r4 on Windows), `offlineasm/arm.rb` (globaladdr via movw/movt),
  `MacroAssemblerARMv7.cpp` (`.text` instead of `.previous`), `ARMv7Assembler.h` (FlushInstructionCache),
  `YarrJIT.cpp` (callee-save stack, 5th argument offset) — keep.

## Toolchain / build — keep (adapted to our tree)
- `cmake/OptionsWin.cmake` (WP81 option set), `cmake/OptionsMSVC.cmake` (no PDB, no clang builtins, NOICF),
  `cmake/OptionsJSCOnly.cmake`, `WTF/wtf/PlatformWin.cmake` (RunLoopGeneric), `WebCore/PlatformWin.cmake`,
  `WebCore/platform/Curl.cmake`, `Source/CMakeLists.txt` (hook: becomes `engine/port`, not `src/webkit-open`+`src/wkwp`).
- Compile fixes: `ThreadingWin.cpp` (no SEH thread naming on ARM), `VMAllocate.h`, `BAssert.h`, `simde/arm/neon.h`,
  mimalloc `prim.c`/`types.h`, Skia `CMakeLists.txt`/`SkFontMgr_win_dw.cpp`, `RenderText.cpp` (32-bit size assert),
  `MessagePort.cpp` (LOG), `PermissionStatus.cpp`, `MediaSource.cpp` (`ssize_t`), `CSSPlatformColorResolutionState.h`,
  `SVGPathBlender.cpp`, `BitmapImageDescriptor.*`, `jsc.cpp`, `Platform.h`, `Threading.cpp`, `LocalDOMWindow.cpp`
  (whitespace only).

## Platform glue inside WebCore — keep
- Graphics on ANGLE D3D11 FL9_3: `PlatformDisplayWin.cpp`, `GLContext.cpp`, `GLFence.cpp`, `TextureMapper*`,
  `ClipStack.h`, `BitmapTexture*`, `Coordinated*` — keep (GLES2/FL9_3 limits).
- Fonts: `FontCacheSkiaWin.cpp` + DirectWrite — keep.
- `PlatformScreenWin.cpp` (screen size set by the port), `Navigator.cpp` (maxTouchPoints 5),
  `PointerEvent.h`, `PointerCaptureController.*`, `dom/glib/PointerEventGLib.cpp` in the build — keep.
- `EventHandler.cpp`: touches also dispatch pointer events when the page has no touch handlers — keep
  (correctness: modern sites listen to pointer events only).
- `ScrollingTreeCoordinated.*`: `touchRegionInfoForPoint` lets the scrolling side ask whether a touch point has
  touch listeners / touch-action — keep (this is the basis of "only touches on listener regions wait").
- `ImageFrameWorkQueue.*`: two shared decoding queues instead of one WorkQueue (thread) per image — keep.
- Memory: `AvailableMemory.*` (app limit set by the port), `RAMSize.cpp`, `MemoryFootprintWin.cpp` (PrivateUsage),
  `Heap.cpp` (footprint-based growth, cached critical-threshold check) — keep, tune in phase 5.
- Network: `ResourceHandle*`, `ResourceHandleCurl.cpp`, `CurlResourceHandleDelegate.h`, `CurlContext.*`,
  `CurlStream.*` (stack receive buffer, error message to WebSocket), `CookieJarDB.*` (keep session cookies option,
  400-day cap) — keep for first light; the ResourceHandle path is replaced by our network-thread loader in phase 4.
- `FrameConsoleClient.cpp`: console line carries all arguments — keep (our console event).
- `EmptyClients.h`, `EmptyFrameLoaderClient.h`: `final` → `override` so a port can subclass the empty clients — keep
  for bring-up; our clients derive from the real interfaces later.
- `MediaPlayer.*`, `MediaSourcePrivate.h`, `SourceBufferPrivate.*`: hook for an external media engine — keep the
  hook, our own engine registers through it (phase 6).

## Behaviour hacks — decided individually
- `DOMTimer.cpp` + `VMEntryScope.cpp` (**rewrite**): one-shot timers ≥ 4 s are pushed back by the main-thread JS
  time spent since they were scheduled (if > 250 ms), so slow-phone load times don't trip page timeouts. Sound
  idea; our version uses the priority run loop's own busy-time accounting instead of a global counter in JSC.
- `Interpreter.cpp`, `MicrotaskQueue*.{cpp,h}` `g_wp81ScriptsBlocked` (**drop**): a global switch that makes every
  JS entry return undefined and clears microtasks. Replaced by real page suspension (timers/animations/active DOM
  objects suspended) and JSC's Watchdog for runaway scripts.
- `AutomaticThread.cpp` infinite timeout (**check**): JSC helper threads never exit. Likely a workaround for a
  thread-exit problem on WP8.1 (FLS/TLS teardown) or slow thread creation; costs only idle stacks. Keep until we
  reproduce the reason, then decide.
- `Document.cpp` (**check**): `hasTouchEventHandlers()` ignores the per-type handler counts when touch event
  regions are used. Interacts with the EventHandler change above; verify with touch listener test pages.
- `MarkedBlock.cpp` allocation hooks (**check**): lets the port supply GC block memory (a pool in the 2 GB-limited
  address space). Keep the hook; implement a pool only if fragmentation shows up.
- mimalloc `options.c` (**check**): purge delay 250 ms, reclaim on free, no full-page retain — memory over speed.
  Re-measure with alloc.html (300k objects took 502-576 ms on MetroSurf).
