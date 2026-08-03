# oxbow

FFGL effects for hosts that have no video plugin interface.

vMix, OBS, and most broadcast software cannot load video effect plugins. oxbow
is a standalone FFGL host with NDI/OMT video in and out: route a source out of
your mixer, through a chain of FFGL plugins, and back in — like an oxbow, a
loop in the river.

**Status: early development.** The FFGL host core works (verified against nine
real plugins); the NDI/OMT frame pump, chain configuration, and web UI are in
progress. macOS first, Windows to follow.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Current commands

```
oxbow probe <plugin>                          # load a plugin, print metadata
oxbow selftest <plugin> [--set Name=value …]  # instantiate offscreen, render frames, report
```

`<plugin>` is a `.bundle` (macOS) or `.dll` (Windows) FFGL 2.x plugin.

## Licence

MIT. The NDI runtime is not bundled — see the note at the top of
`src/io/` once the pump lands, and `third_party/ndi/README.md`. OMT is MIT and
its runtime can ship alongside.
