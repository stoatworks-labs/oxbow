# oxbow

FFGL effects for hosts that have no video plugin interface.

vMix, OBS, and most broadcast software cannot load video effect plugins. oxbow
is a standalone FFGL host with NDI/OMT video in and out: route a source out of
your mixer, through a chain of FFGL plugins, and back in — like an oxbow, a
loop in the river.

**Status: early development.** The FFGL host core works (verified against nine
real plugins) and the frame pump runs at 60 fps over **NDI and OMT, in any
combination per side** — NDI→NDI, OMT→OMT, and NDI→OMT all verified end to
end. Chain configuration and the web UI are in progress. macOS first, Windows
to follow.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Current commands

```
oxbow probe <plugin>                          # load a plugin, print metadata
oxbow selftest <plugin> [--set Name=value …]  # instantiate offscreen, render frames, report
oxbow list [--proto ndi|omt]                  # discover sources
oxbow run --in <source> --out <name>          # the loop
          [--in-proto ndi|omt] [--out-proto ndi|omt]
          [--plugin <path> [--set Name=value …]]…
oxbow send-test [--out <name>] [--proto ndi|omt]   # built-in test pattern
oxbow recv-probe --in <source> [--proto ndi|omt] [--dump out.ppm]
```

`<plugin>` is a `.bundle` (macOS) or `.dll` (Windows) FFGL 2.x plugin.
`--plugin` repeats to build a chain; `--set` applies to the plugin before it.

## Control page

`oxbow run --config file.json` with `"control": { "port": 8720 }` (or
`--port 8720`) serves a live control page at `http://127.0.0.1:8720/` —
status, the chain, and a slider per parameter, applied on the frame thread
between frames. The JSON API underneath:

```
GET  /api/state
POST /api/param?effect=0&name=Mix&value=0.5
GET  /api/sources?proto=ndi|omt
```

Binds loopback by default; set `"bind"` to expose it and understand what that
means on your network.

## Runtimes

Neither transport library is linked or bundled; both are found at run time:

- **NDI**: install the NDI runtime (or Tools/SDK) from ndi.video. Not bundled
  because the NDI licence is incompatible with this repository's MIT terms.
- **OMT**: MIT — grab the binaries from
  [libomtnet releases](https://github.com/openmediatransport/libomtnet/releases)
  and put `libomt`, `libvmx`, and `libomtnet.dll` together on the library
  path (or point `DYLD_LIBRARY_PATH`/`PATH` at them). Set `OXBOW_OMT_LOG=1`
  for libomt's internal log on stderr.

## Documentation

[docs/USER-GUIDE.md](docs/USER-GUIDE.md) — setup, the vMix loop, config
reference, control API, latency, troubleshooting.

## Licence

MIT.
