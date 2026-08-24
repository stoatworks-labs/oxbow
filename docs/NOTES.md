# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*oxbow — standalone FFGL host with NDI/OMT I/O so vMix (no plugin API) can run the FFGL fleet via a network round trip; NDI loop verified at 60 fps 2026-08-03*

**oxbow** (`~/Projects/oxbow`) — standalone FFGL 2.x host with NDI/OMT video
I/O: mixer output → oxbow (FFGL chain on GPU) → back in. Named for the river
loop. Born 2026-08-03 from the vMix feasibility question: vMix has **no video
plugin API** (audio VST3 only; feature request open since 2023, no staff
reply), but routes per-input outputs over NDI, and **vMix 29 added OMT
natively**. C++17/CMake, MIT. PUBLIC at stoatworks-labs/oxbow (pushed
2026-08-03, branch main).

State (2026-08-03): FFGL host core + pump **verified end to end at 60 fps
over NDI AND OMT, each side independently selectable** — NDI→NDI, OMT→OMT
(through the VMX codec), and NDI→OMT bridge all proven by frame dumps. CLI:
`probe`, `selftest`, `list`, `run`, `send-test`, `recv-probe`, all
protocol-aware. Selftest passes on 7 fleet plugins. Audio is normalised to
planar float32 — the native layout of both transports — so cross-protocol
audio is a copy.

Since then (same day): **JSON config** (`run --config`, input/output/chain/
control), **control server** — HttpServer + JSON module lifted from WebLinked,
embedded web page, live param sliders verified browser→pump→output; Pump is
now a class with a frame thread + pending-set queue. **Windows port compiles
green in CI** (WGL hidden-window context, GLEW from the FFGL submodule's
deps, `OMT_NO_IMPLICIT_LINK` guards the vendored libomt.h's MSVC auto-link
pragma — the only local edit to that header). `launchers/oxbow.toml` added to
[av launcher](https://github.com/stoatworks-labs/av-launcher/blob/main/docs/NOTES.md) (`av-launcher`) (args mode onto `--bind`/`--port`). Fleet diag
vendored (crash handler installed AFTER transports — .NET replaces handlers
like CEF); tag-triggered release.yml written but **v0.1.0 not tagged** (fleet
release workflow = user's call). docs/USER-GUIDE.md is the guide master.

**Windows runtime FULLY verified in the ARM64 Parallels VM under x64
emulation (2026-08-03)**: exe runs, `probe` reads Windows FFGL dlls, full OMT
send/receive at 720p60 (frame dump correct), and **GL selftest PASSES** on
both Downpour plugins — but only from the interactive console session:
`prlctl exec` lands in session 0 where there is no GPU, and the classifier
blocks scheduling into the interactive session, so the user runs the .ps1
from the VM console. Parallels forwards guest GL onto host Metal ("4.1
Metal" renderer string in the *Windows* guest); rgb sums match the native
mac render to within timing noise. Files reach this VM via an hdiutil DMG
mounted on the Mac (appears as a \\Mac share); test kit persists at
C:\oxbow-test. Remaining: real vMix loop, launcher shell exercise, release
(v0.1.0 untagged).

Decisions: NDI runtime-loaded like WebLinked (licence), libomt runtime-loaded
too (no Linux binary). Control page default port 8720, loopback bind.

Traps already hit: **libomt embeds .NET, which replaces SIGINT/SIGTERM
handlers at runtime init — install handlers AFTER creating the first OMT
sender/receiver** or the process ignores Ctrl-C and lingers holding OMT port
6400, and the *next* sender announces while the zombie owns the port, so
receivers "connect" to a source that never sends (this also threatens
WebLinked's OMT output). NDI's `BGRX_BGRA_flipped` recv format is
**Windows-only** (`#ifdef _WIN32` in the header) — elsewhere normalise rows
on ingest. FFGL plugins trash GL bindings (rebind everything each frame).
Selftest input needs **edges** — outline effects correctly output black on a
smooth ramp. The host exposed the fleet-wide `SetTextParameter` instantiate
bug (see [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md)); porthole/old-cathode/luma-keyer fix
spawned as a separate task.

Fleet test bundles live in each plugin repo's `build/` dir; NDI 6 runtime at
/usr/local/lib/libndi.dylib + NDI Tools installed. OMT runtime v1.0.0.16
staged (gitignored) at `oxbow/build/omt-runtime/` — run with
`DYLD_LIBRARY_PATH` pointing there; `OXBOW_OMT_LOG=1` for libomt's log.

**Syphon and Spout outputs added 2026-08-06.** `--out-proto syphon` (macOS) and
`--out-proto spout` (Windows); vendored server subsets and both backends lifted
from [weblinked](https://github.com/stoatworks-labs/weblinked/blob/main/docs/NOTES.md) (`weblinked`)'s `src/outputs/shared_surface_*`.

**Syphon is VERIFIED end to end against two implementations that are not oxbow's
code**: WebLinked's `tools/syphon_probe.mm` (which links *Resolume Arena's
bundled Syphon 5*) receives a 1280x720 frame, and **OBS's own `syphon-input`
source lists `[oxbow] OxbowSyphon` and renders the colour bars in the correct
order** — which is also the check that BGRA is not channel-swapped. That makes
Syphon a second, no-round-trip route from the FFGL fleet into OBS, alongside
[obs ffgl](https://github.com/stoatworks-labs/obs-ffgl/blob/main/docs/NOTES.md) (`obs-ffgl`).

**The trap that cost the time: a Syphon server must be created on the MAIN
thread.** `SyphonServerBase` registers for the announce-request notification in
`-init`, and NSDistributedNotificationCenter delivers those on the **main** run
loop. Measured: on a private CFRunLoop thread the server is perfectly
well-formed — right name, right UUID, SyphonSurfaceTypeIOSurface — announces
itself, and is then **invisible to every consumer**, which looks identical to
never having been created. WebLinked got a main run loop from CEF; oxbow has
none, so `src/app/main_loop.h` makes every main-thread wait service the run loop
(`waitServicingMainLoop`) instead of sleeping — **a plain sleep there deadlocks
the `dispatch_sync` the frame thread uses to create the server**. Also: a
private run loop kept alive by a zeroed `CFRunLoopSourceContext` fails silently
too (version 0 + NULL `perform` is not a source CFRunLoop stays alive for), and
a CLI must link **AppKit** explicitly because Syphon uses
`NSRunningApplication`.

**Spout has NEVER BEEN RUN**, but it now genuinely compiles: `spout.cpp` and all
seven vendored Spout sources built in the **Windows job of run 31129182404**
(commit 6072eb9, 2026-08-06), both matrix jobs green. Spout also
tells a *sender* nothing about receivers, so it has no skip-when-idle
optimisation, and the sender must be named before the first `SendImage`.

**DeckLink output added 2026-08-06**, optional behind `-DDECKLINK_SDK_DIR`
(Blackmagic's SDK is not vendored). Scheduled playback, 3-frame pre-roll,
**8-bit BGRA straight onto the card** — the pump already holds BGRA, so unlike
WebLinked there is no colour conversion in the path at all. **VERIFIED ON A REAL DUO 2, 2026-08-06** — connector 1 out, connector 4 in,
cabled: `tools/sdi_probe` captured all eight bars in the right places within 1-3
counts of BGRA -> SDI 4:2:2 -> BGRA, and a full chain (NDI in, Asciify on the
GPU, DeckLink out) ran at **60.0 fps** with the plugin's output correct off the
wire.

**The bug the hardware found was in the PACING, not the card code.**
`runTestSender`'s `frame` is `uint64_t`, and `uint64_t * nanoseconds` promotes
the duration's rep to **unsigned** — so once the deadline was past, `next - now`
wrapped to ~1.8e19 ns = **585 years**. Opening a DeckLink takes longer than one
frame period, so the first wait did exactly that: one frame ever sent, card
emitting a valid black raster, indistinguishable from an output that renders
nothing. `sleep_until` never showed it because it compares rather than
subtracts. Cast to signed before multiplying a duration.

**Card profiles — why a connector can be dead.** A Duo 2 lists all four
sub-devices whatever the profile, and **each PAIR has its own profile manager**
(here {Duo 1, Duo 3} and {Duo 2, Duo 4}). A pair in `1dfd` leaves its second
sub-device `duplex=INACTIVE`, refusing BOTH EnableVideoInput and
EnableVideoOutput **while still offering a full display-mode list** — reads as
broken hardware. `tools/dl_profile --set 2dhd` fixed it (writes to the card,
persists for every app). `tools/sdi_probe --list` shows input/output/duplex per
sub-device. CMake checks the SDK version because this Mac has 10.11.2 inside the NDI
SDK's examples and 12.2 inside Unreal's BlackmagicMedia, and below 11.0 there is
no `IDeckLinkProfileAttributes` (same trap [kestrel](https://github.com/stoatworks-labs/kestrel/blob/main/docs/NOTES.md) (`kestrel`) hit).

**oxbow's GitHub Actions stopped creating runs for pushes on 2026-08-06** while
the rest of the fleet built normally (coinop and idler both ran *after* the
ignored oxbow pushes). Actions enabled, both workflows active, YAML valid,
triggers correct, and GitHub's events API shows every push to refs/heads/main —
but the commits' check-suites list Render and Cloudflare and **no
github-actions suite at all**. A manual `workflow_dispatch` (added to build.yml
for this reason) *is* accepted but then sits queued. Not diagnosable from the
API; needs the Actions tab. Consequence: **Spout has not even been
compile-checked**.
