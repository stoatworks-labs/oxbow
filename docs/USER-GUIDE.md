# oxbow — user guide

oxbow runs FFGL video effect plugins for mixers that cannot load plugins
themselves. Video leaves your mixer over NDI or OMT, passes through a chain
of FFGL effects on oxbow's GPU, and comes back as a new source — a loop in
the river.

It exists first for **vMix**, which has no video plugin interface but routes
any input to an NDI output and (since vMix 29) speaks OMT natively. It works
just as well with anything else that can send and receive NDI or OMT.

## What you need

- **The oxbow binary** for your platform (macOS or Windows x64).
- **At least one transport runtime**, found at run time, never bundled:
  - *NDI*: install the NDI runtime or NDI Tools from ndi.video.
  - *OMT*: download the binaries release from
    github.com/openmediatransport/libomtnet and put `libomt`, `libvmx`
    (and `libomtnet.dll`) together somewhere on the library path — or beside
    a directory you point `DYLD_LIBRARY_PATH` (macOS) / `PATH` (Windows) at.
  - A transport that is not installed simply reports itself unavailable;
    the other keeps working.
- **FFGL 2.x plugins** — `.dll` on Windows, `.bundle` on macOS. Any plugin
  that loads in Resolume Arena/Avenue should load here.

## Quick start (vMix)

1. In vMix, route the input you want processed to an output: *Settings →
   Outputs / NDI / OMT*, set e.g. **Output 3** to that input, enabled as NDI
   or OMT.
2. Write a config, `oxbow.json`:

   ```json
   {
     "input":  { "protocol": "ndi", "source": "Output 3" },
     "output": { "protocol": "omt", "name": "oxbow" },
     "control": { "port": 8720 },
     "chain": [
       { "plugin": "C:/Program Files/Common Files/FFGL/Effect.dll",
         "params": { "Mix": 0.8 } }
     ]
   }
   ```

   `input.source` is matched case-insensitively as a substring of the full
   source name (`"HOST (Output 3)"`), so the short form is enough.
3. Run it:

   ```
   oxbow run --config oxbow.json
   ```

4. Add the processed source back into vMix: *Add Input → NDI / OMT* and pick
   `oxbow`. Open `http://127.0.0.1:8720/` for live parameter control.

Run oxbow on the vMix machine or on a second machine on the same network —
the second machine keeps the GPU load away from vMix and only costs network
transport.

## Commands

```
oxbow probe <plugin>                 plugin metadata and parameters
oxbow selftest <plugin> [--set N=V]  offscreen render test, PASS/FAIL
oxbow list [--proto ndi|omt]         discover sources
oxbow run --config <file.json>       the loop (or --in/--out/--plugin flags)
oxbow send-test [--proto ndi|omt]    built-in 720p60 test pattern source
oxbow recv-probe --in <source>       receive + fingerprint frames
           [--proto ndi|omt] [--dump frame.ppm]
```

`send-test` and `recv-probe` mean the whole loop can be tested with no mixer
at all: pattern out, through the chain, probed and dumped at the far end.

## The control page and API

With `"control": { "port": 8720 }` (or `--port`), oxbow serves a page on
loopback: status, the effect chain, and a slider for every parameter, applied
between frames. The API underneath is three endpoints:

```
GET  /api/state
POST /api/param?effect=0&name=Mix&value=0.5
GET  /api/sources?proto=ndi|omt
```

Anything that can issue HTTP — Companion, a script, a cron job — can drive
parameters. Set `"bind"` to expose the page beyond the machine, knowingly.

## Latency

Expect the loop to add roughly 3–6 frames end to end: transport encode and
decode on each leg plus one frame of processing. Running at 50/60p halves the
wall-clock cost. Keep the source's audio inside the mixer and delay it to
match, or take the loop's audio back — oxbow passes audio through untouched.

## Troubleshooting

- **"connects but no frames arrive" over OMT** — look for a lingering oxbow
  (or other OMT sender) holding the listen port: `lsof -iTCP:6400` (macOS) /
  `netstat -ano | findstr 6400` (Windows), and kill it. Old processes can
  survive Ctrl-C if they wedged before the signal handlers were in place.
- **OMT internals** — set `OXBOW_OMT_LOG=1` for libomt's own log on stderr.
- **A plugin fails to instantiate** — run `oxbow selftest <plugin>`. If the
  log callback prints nothing and instantiate still fails, the plugin may
  reject a text-parameter default set during instantiation (a stock-SDK bug
  pattern); ask the plugin's author about `SetTextParameter`.
- **Logs** — a rotating log lives in the platform log directory
  (`~/Library/Logs/oxbow` on macOS, `%LOCALAPPDATA%\oxbow\logs` on Windows);
  `OXBOW_LOG=debug` raises the level.
