# Oscar64 development for the Commander X16 with VS64

This repo is set up for C development targeting the **Commander X16**, using
the **[Oscar64](https://github.com/drmortalwombat/oscar64) C compiler** and the
**[VS64](https://marketplace.visualstudio.com/items?itemName=rosc.vs64) VSCode
extension** for build/run integration. The walkthrough below uses
[examples/bounce.c](examples/bounce.c) — a sprite bouncing around the screen
with PSG and YM2151 sound — as the working example.

Everything needed is already in the repo; nothing has to be installed
system-wide except the VS64 extension itself.

## What's where

| Path | What it is |
|---|---|
| `oscar64/` | Oscar64 compiler, repo-local copy (v1.32.272, `bin/oscar64.exe`) |
| `src_oscar64/x16/` | x16clib, the X16 support library (Oscar64 port). Header-driven: each header ends in `#pragma compile(...)`, so including it pulls the implementation in — there is no library archive to build |
| `emulator/` | Official X16 emulator (`x16emu.exe`) plus `rom.bin` |
| `box16/` | [Box16](https://github.com/indigodarkwolf/box16) (nr48.0), an alternative X16 emulator with a much richer built-in debugger |
| `box16-src/` | Box16 fork (branch `binary-monitor`) adding a **VICE binary monitor server** — this is what enables C source-level debugging in VSCode. Built exe: `build\vs2022\out\x64\Release\box16.exe` |
| `examples/` | Example programs: `hello.c`, `numbers.c`, `bounce.c` |
| `project-config.json` | VS64 project file — says what to build and how |
| `.vscode/` | VS64 settings (tool paths), build task, X16 launch config |
| `build/` | Build output (`bounce.prg` and Oscar64 listing/debug files) |
| `build_oscar64.ps1` | Stand-alone CLI build/run/test script (no VSCode needed) |
| `tutorial/` | `oscar64_guide.md` and `userguide.md` for the library itself |

## One-time setup

1. Install the **VS64** extension in VSCode (`rosc.vs64`, tested with 2.6.2).
2. Open this folder (`c:\quartus\projects\x16_test`) as the workspace root —
   VS64 looks for `project-config.json` there.
3. That's it. [.vscode/settings.json](.vscode/settings.json) already points
   VS64 at the repo-local compiler and emulator:

   ```jsonc
   "vs64.oscar64InstallDir": "c:\\quartus\\projects\\x16_test\\oscar64",
   "vs64.x16Executable":     "c:\\quartus\\projects\\x16_test\\emulator\\x16emu.exe",
   "vs64.x16Args":           "-rom c:\\quartus\\projects\\x16_test\\emulator\\rom.bin -scale 2"
   ```

   If you move the repo, update these three absolute paths (VS64 does not
   expand `${workspaceFolder}` in settings).

## The development flow

```
edit .c file  ──►  Ctrl+Shift+B (build)  ──►  F5 (run in X16 emulator)
```

1. **Edit** — open [examples/bounce.c](examples/bounce.c). IntelliSense works
   because VS64 generates `.vscode/c_cpp_properties.json` and
   `build/compile_commands.json` from the project file.
2. **Build** — `Ctrl+Shift+B` runs the default VS64 build task. VS64 generates
   a ninja build under `build/` and invokes Oscar64 roughly as:

   ```
   oscar64 -o=build/bounce.prg -n -g -tm=x16 -tf=prg -O0 -O1 -i=src_oscar64 examples/bounce.c
   ```

   (`-n` native 6502 code instead of interpreted bytecode, `-g` debug symbols,
   `-tm=x16` the Commander X16 target.) With `vs64.autoBuild` enabled — as it
   is here — saving a file rebuilds automatically.
3. **Run** — `F5` uses the `"type": "x16"` launch configuration in
   [.vscode/launch.json](.vscode/launch.json): it builds if needed, then starts
   `x16emu -prg build/bounce.prg -run` plus the `vs64.x16Args`. The bounce
   demo runs frame-locked at 60 Hz; press any key inside the emulator to quit.

### Choosing the emulator: standard x16emu or Box16

Both emulators live in the repo and there is a build-and-run task for each
(**Terminal → Run Task…**, defined in [.vscode/tasks.json](.vscode/tasks.json)):

| Task | Emulator | Use when |
|---|---|---|
| `run in x16emu (standard)` | `emulator/x16emu.exe` — the official emulator | normal run/test, closest to real hardware behavior |
| `run in Box16 (debugger)` | `box16/Box16.exe` | you want to debug: it loads `build/bounce.lbl` so the debugger shows your function and variable names |

Both tasks trigger the VS64 build first, then launch `build/bounce.prg`
(if you rename the project in `project-config.json`, update the `.prg`/`.lbl`
paths in the tasks too).

The `F5` launch flow always uses the single `vs64.x16Executable` setting —
VS64 cannot select an emulator per launch configuration. It points at the
standard x16emu; to make `F5` start Box16 instead, change
[.vscode/settings.json](.vscode/settings.json) to:

```jsonc
"vs64.x16Executable": "c:\\quartus\\projects\\x16_test\\box16\\Box16.exe",
"vs64.x16Args": "-ignore_ini -rom c:\\quartus\\projects\\x16_test\\emulator\\rom.bin -sym c:\\quartus\\projects\\x16_test\\build\\bounce.lbl -scale 2"
```

(Box16 accepts the `-prg <file> -run` arguments VS64 adds automatically, so
everything else keeps working.)

To build a **different example**, change `sources` (and, if you like, `name`)
in [project-config.json](project-config.json):

```json
"sources": [ "examples/hello.c" ]
```

### Command line alternative (no VSCode)

The same compile/run is scripted in [build_oscar64.ps1](build_oscar64.ps1):

```powershell
.\build_oscar64.ps1 -Source examples\bounce.c -Run   # compile + run windowed
.\build_oscar64.ps1 -Test                            # headless regression suite
```

## How the project file is wired (and why)

[project-config.json](project-config.json):

```json
{
    "name": "bounce",
    "toolkit": "oscar64",
    "machine": "x16",
    "format": "prg",
    "sources": [ "examples/bounce.c" ],
    "build": "debug",
    "includes": [ "src_oscar64" ],
    "args": [ "-tm=x16" ],
    "compilerFlags": [ "-O1" ]
}
```

Three entries deserve an explanation — they work around real quirks found
while setting this up:

* **`"args": ["-tm=x16"]` in addition to `"machine": "x16"`** — VS64 2.6.2 has
  a bug in its Oscar64 backend: the `machine` attribute is parsed but never
  actually emitted as `-tm=` on the compiler command line (a broken comparison
  in the generated ninja logic). Without the explicit `-tm=x16` in `args` the
  program would silently be built for the C64. The `machine` attribute is kept
  so the project file does the right thing if a future VS64 fixes this; a
  duplicate `-tm=x16` is harmless.

* **`"build": "debug"`, not `"release"`** — VS64 maps *release* to `-O3` and
  *debug* to `-O0`. **Oscar64 1.32.272 crashes with an access violation at
  `-O2`/`-O3` on this code base** (verified: `-O3` on `bounce.c` crashes the
  compiler). Stay on `debug`.

* **`"compilerFlags": ["-O1"]`** — the compiler flags are appended *after* the
  build-type's `-O0`, and the last `-O` flag wins, so this quietly upgrades
  the debug build to Oscar64's default `-O1` optimization, which is stable and
  already competitive. Verified: `bounce.prg` builds at 2455 bytes and runs.

* **`"includes": ["src_oscar64"]`** — becomes `-i=src_oscar64`, which is how
  `#include <x16/x16.h>` finds the library; the `#pragma compile` lines in the
  headers then pull the matching `.c` files into the whole-program compile.
  Oscar64 finds its own standard headers relative to `oscar64.exe`, so no
  further include paths are needed.

## Debugging

**C source-level debugging in VSCode works — via the Box16 fork in
`box16-src/`.** Neither stock emulator can do it: x16emu and stock Box16 have
no remote-debug protocol, so VS64's real debugger (which speaks VICE's
*binary monitor* TCP protocol) cannot attach to them. The fork adds exactly
that protocol to Box16, and VS64's existing `"vice"` debug type does the
rest — its debugger UI, its Oscar64 `.dbj` debug-info parsing, and its
C-line breakpoint mapping are all emulator-agnostic.

### C source-level debugging (the good path)

One step:

1. Set breakpoints on C lines in [examples/bounce.c](examples/bounce.c).
2. **F5** with **`Attach to Box16 (binary monitor)`** selected in the Run
   and Debug dropdown (Ctrl+Shift+D).

That single F5 does everything via its `preLaunchTask`: builds the PRG,
kills any stale emulator instance, starts the Box16 fork with the binary
monitor, waits for its "binary monitor: listening" stdout line, attaches,
restarts the program under the debugger — and stops at the first breakpoint
reached. Stepping (F10/F11/Shift-F11), the registers view (a, x, y, pc, sp,
fl), and watches on globals/statics (e.g. `pos_x`) are all live in the
editor. When the session ends the emulator keeps running; the next F5
reuses it (attach restarts the program anyway).

On attach VS64 resets the machine and re-runs the program (it sends RESET
then AUTOSTART), so the emulator **restarts bounce from the beginning** each
time you attach — that is expected, and the fork's RESET handler reloads the
`-prg` for you. (If you ever see it stop at the BASIC `READY.` prompt after
attaching instead of running the program, that is the symptom of an older
fork build where RESET did not reload — rebuild `box16-src`.)

**Debugging from the very first line works.** VS64 installs your breakpoints
*before* it sends the reset, and the fork keeps them armed across the
reboot — so a breakpoint on the first statement of `main()` (line 254,
`x16_screen_cls();`) stops the restarted program before any of it has run.
Attaching is therefore never "too late": every attach is a clean
run-from-start under the debugger, regardless of how long the emulator has
been sitting there. Breakpoints in Oscar64's `crt.c` startup code (before
`main`) work the same way.

Verified so far by a protocol test harness
(`box16-src\test\binmon_test.py`, 23 assertions — run it against a
`-binarymonitor` instance) that speaks the exact byte sequences VS64 sends:
breakpoint at `move_sprite` stops at the exact address, step into/over/out,
resume-and-rehit, delete-and-run-free all pass, and the fork without the
flag behaves identically to stock (no port opened).

MVP limits of the fork's monitor: main-memory (64 KB CPU view) only — fine
for Oscar64 programs, which live below `$9F00`; conditional breakpoints are
accepted but fire unconditionally; only VS64's *attach* mode is supported
(use the task to start the emulator). Oscar64's `.dbj` carries no
local-variable info in this version, so inspect locals via globals/statics
or the memory view. Box16's own ImGui debugger stays usable while attached —
pausing there shows up in VSCode and vice versa.

### The stock emulators (assembly-level only)

Everything below applies to the unmodified emulators — useful when the
binary-monitor fork isn't in play.

### What the panel on the right of the emulator is

When you Start Debugging (F5) with the `"x16"` launch type, VS64 appends
`-debug` to the x16emu command line. The panel that opens on the right is
**x16emu's own built-in machine-language monitor** — it has no connection
back to VSCode.

### VSCode breakpoints *do* reach the emulator (partially)

VS64 loads Oscar64's debug info (`build/bounce.dbj`, produced by `-g`). If a
breakpoint is set on a C line **before** pressing F5, VS64 resolves it to a
machine address and launches `x16emu -debug <address>` — the emulator stops
in its monitor exactly when execution reaches that C line.

Caveats (verified in VS64 2.6.2's code):

* only the **first** breakpoint is forwarded; all others are ignored;
* with no breakpoints set, plain `-debug` is passed, which just arms the
  monitor (press F12 in the emulator to break in).

### Working in the x16emu monitor

Debugging there is assembly-level. Keys (from the official x16-emulator
documentation):

| Key | Action |
|---|---|
| F12 | Break into the debugger |
| F9 | Set breakpoint at cursor position |
| F11 / F10 | Step into / step over |
| F5 | Continue (leave debugger) |
| F1 | Re-sync disassembly to the program counter |
| PAGE UP/DOWN | Scroll memory (with SHIFT: scroll disassembly) |
| TAB | Hide the panel while held |

To map what you see back to the C code, use the files Oscar64 writes next to
the PRG on every build:

* `build/bounce.asm` — full disassembly annotated with the C source lines;
* `build/bounce.map` / `build/bounce.lbl` — addresses of every function and
  global. Look up e.g. `move_sprite` there, then break on that address with
  F9 (or `-debug <addr>`).

### Other assembly-level options

1. **VS64's `"6502"` launch type** (add `{ "type": "6502", "request":
   "launch", "name": "Launch 6502", "preLaunchTask": "${defaultBuildTask}" }`
   to `launch.json`) gives source-level debugging on a **bare CPU
   emulator**: no VERA, no X16 KERNAL, no interrupts. `bounce.c` hangs at
   `x16_vsync_wait()` and all VERA/sprite pokes go nowhere. Only useful for
   stepping through pure algorithm code in isolation — the binary-monitor
   attach flow above supersedes it for real programs.
2. **Stock [Box16](https://github.com/indigodarkwolf/box16)** — installed in
   this repo under `box16/` (version nr48.0) — has a rich built-in ImGui
   debugger: dockable panels for CPU state, memory, VERA/VRAM inspection,
   PSG/YM state, and **symbol file loading**. Assembly-level, not
   VSCode-integrated. Run it via the `run in Box16 (debugger)` task, which
   passes Oscar64's `build/bounce.lbl` with `-sym` so the disassembly and
   breakpoints work with your C function names (`main`, `move_sprite`,
   `step_axis`, …). A `-sym` label file can also contain `break <address>`
   lines to pre-set breakpoints.

   Box16 quirks found while setting this up (both stock and fork):
   * launch it with `-ignore_ini` (the tasks do): a stale/broken ini under
     `%APPDATA%\Box16` otherwise makes it print usage and exit immediately;
   * it does not find `rom.bin` on its own here — the tasks pass
     `-rom emulator\rom.bin` explicitly. The repo's ROM is R49 (KERNAL
     version byte `$31` at `$FF80`) while Box16 nr48.0 was released against
     R48; the combination runs these examples fine, but if something looks
     off in Box16, suspect the version skew first. (The fork in `box16-src/`
     is built from Box16 master, which also runs the R49 ROM fine.)

### Rebuilding the Box16 fork

Requires Visual Studio 2022 with the Desktop C++ workload. Open
`box16-src\build\vs2022\box16.sln` and build Release x64, or headless:

```powershell
& "$(& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find MSBuild\**\Bin\MSBuild.exe)" `
  box16-src\build\vs2022\box16.sln -p:Configuration=Release -p:Platform=x64 -m
```

The monitor implementation is one new file pair
(`box16-src\src\binary_monitor.{h,cpp}`) hooked into `emulator_loop()`; the
protocol test harness is `box16-src\test\binmon_test.py` (stdlib Python;
start the emulator with `-binarymonitor` first, then
`python box16-src\test\binmon_test.py --lbl build\bounce.lbl`).

### Other notes

* Oscar64's `build/bounce.asm`, `.map` and `.lbl` are regenerated on every
  build — often the fastest way to see what the compiler actually did.
* `bounce.c` needs real video (VSYNC): run it windowed, never under the
  emulator's headless `-testbench` mode, or `x16_vsync_wait()` never returns.

## Verified

* `oscar64 -tm=x16` compile of `examples/bounce.c`: OK (exit 0, 2455 bytes
  with `-O1`, 2569 bytes plain `-O0`).
* `x16emu -prg build\bounce.prg -run`: OK — demo loads, runs, exits on key.
* `Box16 -ignore_ini -rom emulator\rom.bin -prg build\bounce.prg -run -sym
  build\bounce.lbl`: OK — demo runs with symbols loaded.
* Box16 fork binary monitor: all 23 protocol-harness assertions pass
  (transport, memory/registers, exec breakpoint at `move_sprite` stopping at
  the exact address, step into/over/out, resume-and-rehit, delete); without
  `-binarymonitor` the fork behaves like stock and opens no port.
* Oscar64 `-O3` on `bounce.c`: compiler crash (documented above).
