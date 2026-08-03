# oxbow

Standalone FFGL host with NDI/OMT video I/O — FFGL effects for vMix and other
plugin-less mixers via a network round trip. C++17/CMake. Public MIT.

Read `AGENTS.md` before touching the FFGL host or GL code.

## Commands
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Probe a plugin: `./build/oxbow probe <bundle-or-dll>`
- Selftest a plugin: `./build/oxbow selftest <bundle-or-dll> [--set Name=value …]`

## Notes
- Submodule: `third_party/ffgl` (resolume/ffgl @ b1afaf9 — same as the fleet).
- NDI/OMT are loaded at run time via `src/core/dylib.*`; never link them.
- Public repo. "Commit" = commit **and** push.
