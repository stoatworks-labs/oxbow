# oxbow — agent onboarding

oxbow is a standalone FFGL 2.x host that will move video over NDI and OMT so
that mixers with no plugin interface (vMix foremost) can use FFGL effects via a
network round trip. C++17, CMake. Public MIT repo — "commit" means commit
**and** push.

## Why it exists

vMix has no video plugin API (audio VST3 only; the feature request has been
open since 2023). It does have per-output routing over NDI, and vMix 29 added
OMT natively. So: vMix output → oxbow (FFGL chain on the GPU) → back into vMix
as an input. One added round trip ≈ 3–6 frames.

## Architecture

- `src/core/` — `Dylib` (runtime .so/.dylib/.dll loading, lifted from
  WebLinked). Everything SDK-shaped is loaded at run time, never linked:
  libndi's licence forbids bundling under MIT, libomt has no Linux binary.
- `src/gl/` — `GlContext`: offscreen core-profile GL. macOS = CGL 4.1 core, no
  window, no Objective-C, safe on any thread. Windows (pending) = WGL hidden
  window. All FFGL GL calls happen with this context current on one thread.
- `src/ffgl/` — `FfglLibrary` (load, initialise, probe metadata) and
  `FfglInstance` (instantiate at a resolution, set params, process). The FFGL
  ABI is one C entry point, `plugMain(opcode, FFMixed, instanceID)`; floats
  cross bit-cast into `FFMixed.UIntValue`. Structs come from the vendored SDK
  submodule at `third_party/ffgl` (same rev the fleet's plugins pin, b1afaf9).
- `src/io/` — `video_io.h` is the protocol-neutral surface (BGRA VideoFrame,
  planar-float AudioFrame — the native audio layout of BOTH transports, so
  bridging is a memcpy); `ndi.cpp` and `omt.cpp` implement it, both
  runtime-loaded. All four direction combinations are supported and NDI→NDI,
  OMT→OMT, NDI→OMT are verified at 60 fps.
- `src/app/` — CLI. `probe` and `selftest` exist and are the regression
  harness: `selftest` renders 120 frames offscreen and fails on GL errors or
  all-black output.

## Invariants and traps

- **Call order**: `FF_INITIALISE_V2` before any metadata query — plugMain
  answers FF_GET_* from a prototype that only exists after initialise.
- **After `process()` assume all GL bindings are trashed.** SDK-built plugins
  "restore" bindings to 0 (the Scoped* helpers). Rebind FBO/viewport every
  frame; never cache binding state across a plugin call.
- **`FF_INSTANTIATE_GL` sets every param default and fails the whole instance
  if one set fails.** A plugin with a TEXT param and no SetTextParameter
  override cannot instantiate at all — porthole, old-cathode and luma-keyer
  v1.0.1 had exactly this. oxbow's selftest is what catches it; keep it honest.
- Wire the plugin's `SetLogCallback` export (optional, SDK-built plugins have
  it) before initialise — it is the only place shader compile errors surface.
- The selftest input pattern must contain **edges** (checkerboard), not just a
  ramp: outline/edge effects legitimately emit nothing on smooth gradients and
  an all-black PASS-looking failure is really a test-pattern bug.
- macOS FFGL plugins are `.bundle` directories; dlopen the single binary under
  `Contents/MacOS/`. Fleet test bundles live in each repo's `build/` dir
  (downpour, tinsel, porthole, …).
- **libomt embeds the .NET runtime, and .NET replaces the process's
  SIGINT/SIGTERM handlers when it starts.** Install signal handlers AFTER
  creating the first OMT sender/receiver or Ctrl-C is swallowed and the
  process lingers holding the OMT listen port (6400) — and the NEXT sender
  then announces itself while the zombie owns the port, so receivers connect
  to a source that never sends. If OMT "connects but no frames arrive", first
  check `ps` for a stale oxbow and `lsof -iTCP:6400`.
- NDI's bottom-up receive format (`BGRX_BGRA_flipped`) is **Windows-only** —
  guarded by `#ifdef _WIN32` in the SDK header. Everywhere else frames arrive
  top-down and the pump flips at ingest.
- OMT testing on this Mac: runtime staged at `build/omt-runtime/`; run with
  `DYLD_LIBRARY_PATH=$PWD/build/omt-runtime`. `OXBOW_OMT_LOG=1` turns on
  libomt's internal log.

## Verify

- Build: `cmake --build build`
- Known-good run: `./build/oxbow selftest ~/Projects/downpour/build/Downpour.bundle`
  and `…/tinsel/build/Tinsel.bundle` — both must PASS.
- Param plumbing: `./build/oxbow selftest "…/downpour/build/Downpour Over.bundle" --set 'Background Opaci=0' --set 'Density=0'`
  → rgb sum jumps by ~20× (input ramp showing through) versus default run.
