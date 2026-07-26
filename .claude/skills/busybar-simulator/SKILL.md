---
name: busybar-simulator
description: Run the BUSY Bar firmware on the workstation and check a change by looking at it — walk the UI, capture the two displays, read the log. Use whenever a change touches the GUI, a widget, an app scene, an animation or an asset, or when a question is about what the device actually shows rather than what the code says.
---

# Validating firmware in the simulator

`simulator/` builds the firmware as a native binary with both displays on SDL.
Every GUI app runs, and the firmware's own HTTP API takes button presses. Use
rendered evidence for GUI, scene, widget, animation and asset work; do not infer
the final screen only from code.

## Build and diagnose

```sh
simulator/tools/simctl.py doctor
simulator/tools/simctl.py build --test
```

`doctor` probes the real CMake configuration in a temporary directory.
`build --test` configures, builds and runs the headless end-to-end smoke test.
Generated sources and runtime assets are tracked build dependencies, so a
plain rebuild notices edits under `assets/` and application `resources/`.

For a non-default build directory, place the global option before the command:

```sh
simulator/tools/simctl.py --build /tmp/busybar-build build --test
```

## The validation loop

```sh
simulator/tools/simctl.py start --scene busy --shots /tmp/shots --port 0
simulator/tools/simctl.py run next ok frames:30 shot:setup next ok shot:theme
simulator/tools/simctl.py status
simulator/tools/simctl.py log --lines 60
simulator/tools/simctl.py stop
```

`start` waits for the HTTP API, updates from both displays, valid canvas
dimensions and two stable presented frames. It does not return merely because a
socket opened. Port `0` selects an available loopback port. State is isolated
in a fresh temporary overlay by default; pass `--state-dir DIR` when a restart
must retain settings.

| command | what it does |
| --- | --- |
| `doctor` | check compiler, SDL2, networking dependencies and submodules |
| `build [--test]` | configure/build and optionally run the smoke regression |
| `start [--scene NAME] [--shots DIR] [--port N] [--state-dir DIR]` | launch and wait for deterministic readiness |
| `press KEY...` | send one or more buttons and settle on rendered frames |
| `shot [LABEL]` | synchronously capture one coherent render and print five paths |
| `run STEP...` | keys, `shot`, `shot:label`, `frames:N`, `wait:N` |
| `record --out FILE.gif STEP...` | record the window while the same steps run |
| `status` | process/render revisions, heap free/low-water, firmware and uptime |
| `power ...` | inspect or inject charge, USB and charging scenarios |
| `log [--lines N]` | tail output; it remains useful after a crash |
| `stop` | request graceful shutdown and clean only tool-owned temporary state |

Duration waits and per-key settling are converted to presented-frame waits.
Use `frames:N` where an exact boundary matters.

## Reading captures

Each shot writes five PNGs from one render pass:

- `front-raw`: exact 72x16 physical front framebuffer.
- `back-raw`: exact 160x80 physical back framebuffer.
- `front`: fixed 8x inspection image, 576x128.
- `back`: fixed 4x inspection image, 640x320.
- `window`: the device renders, displays and controls together.

For a label such as `setup`, the names are `setup-front-raw.png`,
`setup-back.png`, and so on. Without a label, captures are numbered without
overwriting earlier runs.

Read `front`/`back` or their raw counterparts to judge a one-pixel layout
detail. Read `window` to judge presentation. A GIF is scaled and palettised;
use it for transitions, scrolling and timers, not pixel alignment.

Before reporting success:

1. Open the relevant panel image and the window image.
2. Run `log --lines N` and check for `[E]`, asserts and asset-load failures.
3. Use `status` when heap behavior, exact build identity or uptime matters.
4. Stop the session, including after a failed validation.

Anything the firmware exposes is reachable directly with `curl` on the port
printed by `start`. Brightness and timezone settings persist with an explicit
state directory. `power --charge 5 --usb disconnected` exercises low-battery
branches without changing firmware APIs.

## Button names

`next` and `prev` move the highlight down and up the list. Prefer them.

The underlying `/api/input` speaks the device's names: `up` and `down` are dial
rotation directions, and device `up` moves the highlight down. `simctl.py`
passes those names through because that is the hardware contract, but also
provides the unambiguous aliases. Other buttons are `ok`, `back`, `start`, and
the lever positions `busy`, `custom`, `off`, `apps`, `settings`.

## Common traps

- A startup wipe can still be in progress after readiness. Add `frames:N` or
  `wait:N` for the state being tested; do not add an arbitrary shell sleep.
- A blank or stale panel is commonly an asset failure. Check the log first.
- `[E][AnimFile] Load error` names neither reason nor file. Temporarily enable
  `ANIM_FILE_DETAILED_ERRORS` in `lib/anim_file/anim_file_i.h` and log the
  player's `instance->file_path` when `AnimFileFrameFlagError` is returned.
- The HTTP API binds only to `127.0.0.1` and mDNS is disabled by default. Use
  `--network` only when LAN exposure is intended.
- A default session has disposable state. Persistence tests must pass the same
  explicit `--state-dir` to both starts.

## When changing the simulator itself

Read the relevant section of `simulator/README.md` first. Keep these invariants:

- SDL calls and GPU read-back stay on the main thread.
- PNG encoding stays in a task, while raw/scaled buffers and codec workspace
  use the mmap-backed host allocator so evidence capture does not perturb the
  measured furi heap. Large buffers never belong on an 8 KiB task stack.
- The FreeRTOS port owns `SIGALRM` and `SIGUSR1`. A pthread mutex reachable
  from task code must not allow preemption while a task owns it.
- On ELF hosts, furi's `malloc`/`free` definitions must stay hidden from the
  dynamic symbol table so SDL and other shared libraries keep using libc.
- Generated files and assets are build outputs with complete dependencies;
  never require developers to remember a manual reconfigure.
- Base assets are immutable. All runtime writes go to the contained state
  overlay, and recursive operations must never follow symlinks out of it.
- Finish with a clean build, CTest, a driven capture that you inspect, and a
  log check.
