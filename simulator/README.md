# BUSY Bar UI simulator

Runs the firmware's GUI stack and real applications on macOS/Linux against an
SDL window, so a UI change can be looked at before it is flashed.

What is real: LVGL and its software renderer, `lib/lvgl_addons` (themes and
fonts), the whole of `applications/services/gui` (the `Widget` class and every
module), `font_registry`, `anim_file`, `setting_provider`, furi, and FreeRTOS
itself via its POSIX port. The displays are the real 72x16 RGB888 front panel
and 160x80 L8 back panel, at real resolution, showing real converted assets.

Every GUI application in the firmware runs, the Busy app included, driven by
the real scene managers and the real busy timer, and started by the real
`loader` when the real `desktop` sees the mode selector move. Sounds come out
of the workstation's speakers. The device's HTTP API is served too — the
firmware's own `web_server` service, on the workstation's sockets, including
the protobuf state stream on `/api/status/ws` — so `busylib` and anything else
that speaks to a Busy Bar can be pointed at the simulator, or left to find it
over mDNS.

What is faked: the two display drivers push pixels into an SDL texture instead
of SPI, an on-screen control deck and the keyboard stand in for the buttons,
storage is the workstation filesystem, the RTC is the workstation clock, and
everything needing a radio or a server — Wi-Fi, BLE, Matter, MQTT, OTA — is an
inert stand-in. That is the whole of the substitution; see `shim/` and `src/`,
and "What the apps are talking to" below for which is which.

## Build and run

Requires SDL2, CMake, Ninja and `uv`; ffmpeg is optional and only converts the
sounds:

```sh
cmake -S simulator -B simulator/build -G Ninja
cmake --build simulator/build
./simulator/build/busybar-sim --scene clock
```

The submodules the simulator needs are `lib/lvgl`, `lib/cjson`, `lib/mongoose`,
`lib/nanopb`, `assets/proto`, `lib/stb/stb_repo`, `fbt_layers/core_libs` and
`fbt_layers/freertos` (the last two recursively). CMake names the missing one
if any of them is empty.

Options:

| Flag | Meaning |
| --- | --- |
| `--scene NAME` | app to boot into, or `demo` for the widget demo; by default the mode selector decides |
| `-s, --scale N` | initial window scale, default 6; the window is resizable |
| `--frames N` | run N frames then exit |
| `--keys LIST` | replay buttons before exiting, e.g. `up,ok` |
| `--screenshot DIR` | write `front.png`, `back.png` and `window.png` on exit |
| `--list-apps` | print every app that can be given to `--scene` |
| `--api-port N` | serve the device HTTP API on N, default 8042; `0` disables |
| `--no-mdns` | do not announce the simulator on the local network |
| `-v, --verbose` | furi logging at trace level |

With no `--scene`, the simulator boots the way the device does: the startup app
runs, the mode selector settles on BUSY, and the Busy app starts. `--scene`
picks a different app to land on without pinning it there — moving the lever
still switches away, which is what the desktop service does for any app started
from somewhere other than the selector.

`BUSYBAR_SIM_ASSETS` overrides the asset root; it defaults to the tree CMake
builds under `build/assets_root`, which mirrors the device layout so
`/ext/apps_assets/shared/fonts/...` and `/ext/apps_assets/clock/images/...`
both resolve.

## Controls

The strip along the bottom of the window is a stand-in for the device's top
surface: the five-position mode lever, the wide Start/Pause pad, the orange
Back stud and the scroll dial. Click any of them with the mouse, or use the
keyboard:

| Control | Key | Mouse |
| --- | --- | --- |
| Start/Pause (the wide pad) | `Space` | click the pad |
| Back (orange stud) | `Backspace` or `Esc` | click the stud |
| Dial press — Ok/Skip | `Enter` | click the dial centre |
| Dial scroll | `Up` / `Down` | wheel, or click above/below the dial centre |
| Mode: Busy | `b` | click `BUSY` |
| Mode: Custom | `c` | click `CUSTOM` |
| Mode: Off | `o` | click `OFF` |
| Mode: Apps | `a` | click `APPS` |
| Mode: Settings | `,` | click `SETTINGS` |

A control lights up while it is held, whichever way it was pressed. The deck
draws its captions with a small built-in 5x7 font so the simulator needs no
font library.

The five mode positions are not buttons. On the device they are a rotary lever,
so what the firmware reacts to is not the press but which position is now
selected: the input service holds it in a `FuriState`, and the desktop service
watches that, stops whatever is running and asks the loader for the app the
position maps to — `busy`, `busy custom`, `soft_off`, `apps_menu`,
`settings_menu`. Pressing a mode key here means "move the lever there", and the
deck draws the selected position latched down rather than only while clicked.

That chain is the firmware's own: `applications/services/desktop` and
`applications/services/loader` are compiled in, so the wipe animation between
apps, the app-exit handshake and the startup app are the device's behaviour
rather than an imitation.

Scroll direction is named after the screen, not the dial. The firmware names it
after the dial: `InputKeyUp` is one rotation direction, and the widget layer
turns it into `lv_group_focus_next()` — the item *below* the highlighted one.
Pressing `Up` here moves the highlight up, so `Up`, the wheel, the on-screen
pads and `--keys up` all send `InputKeyDown`. `/api/input?key=up` is left alone:
that is the device's own contract, and a script written against the simulator
should send what it would send to a Busy Bar. `SIM_KEY_SCROLL_UP` in
`src/input_host.h` is the single place this is inverted.

## Applications

Every GUI application in the tree runs, plus a hand-built `demo` scene for
widget experiments. `--list-apps` prints the current set:

```
$ ./simulator/build/busybar-sim --list-apps
SCENE                  TYPE       NAME
demo                   scene      built-in widget demo
busy                   app        Busy
clock                  app        Clock
...
```

That list is generated at configure time from the same `application.fam`
manifests fbt reads, so `FLIPPER_APPS`, `FLIPPER_SETTINGS_APPS` and the rest
hold what the simulator actually links — which is why the settings menu
enumerates the real settings apps and the debug app list the real debug apps.

Adding an app is one line in `SIM_APP_DIRS` in `CMakeLists.txt`. Nothing else
lists them; `tools/gen_app_registry.py` picks up the manifest.

Six debug apps are deliberately absent: `crash_test` faults on purpose,
`front_display_test`, `light_sensor_test` and `led_indicator` drive GPIO,
`storage_bench` and `unit_tests` are CLI rather than GUI, and `anim_test` uses a
GCC nested function that clang rejects.

To run any of these headlessly and look at the result:

```sh
./simulator/build/busybar-sim --scene clock --frames 60 --keys ok \
    --screenshot /tmp/shots
```

`front.png` and `back.png` are the panels magnified; `window.png` is the whole
window, which is the only one that shows the layout itself.

## What the apps are talking to

Three levels of realism, and it is worth knowing which one a screen is on
before trusting what it shows.

**Real.** The GUI stack and every widget, the fonts and assets, the scene
manager, `busy_timer` — the countdown service is compiled and run as-is,
because it needs only the RTC and records that already exist, so the Busy app's
timing behaves as it does on the device — and `desktop`, `loader`, `canvas`,
`log_storage` and `state_publisher`. Audio is real too: `audio_play_file()`
reaches the workstation's sound device (see below).

**Substituted.** Displays go to SDL, buttons come from the control deck,
storage is the filesystem, the RTC is the host clock. The HTTP API is the
firmware's own server on host sockets rather than lwIP; `src/web_api_host.c`
answers for the network stack and the load estimator that the handlers consult,
and `src/discovery_host.c` announces over the platform's mDNS responder rather
than lwIP's.

**Inert.** `src/platform_services_host.c` stands in for everything that needs a
radio or a server: Wi-Fi, BLE, Matter, MQTT, the OTA updater, status lights,
brightness, and the version/OTP HAL. Each is an object of the right shape that
always reports the same disconnected, idle, factory state. The screens draw and
navigate correctly, and they draw their *disconnected* branches — you will not
see a connected Wi-Fi screen here, because nothing ever connects. Calls that
would touch hardware log at info level rather than silently doing nothing, so
the log shows when a screen tried something real.

The version record is not inert: `CMakeLists.txt` generates `version.inc.h`
from git the way fbt does, so the About screen shows the real branch and commit.

## Audio

Sounds play on the workstation's audio device. `assets/**/*.wav` is converted
to the firmware's `.snd` — headerless mono s16le at 44.1 kHz — by the same
ffmpeg pipeline `scripts/audio.py` uses, and `src/audio_host.c` queues it
through SDL.

Two deliberate differences. The loudness normalisation and speaker EQ that
`scripts/audio.py` applies are skipped, because that curve compensates for the
device's small speaker and is wrong for desk speakers. And there is no mixing:
a sound started while another plays is queued behind it, which is what the
device's codec does too.

ffmpeg is needed at configure time for this; without it the sounds are skipped
and the build still succeeds.

## The HTTP API

The simulator serves the device's REST API on `0.0.0.0:8042`. It is not a
reimplementation: `applications/services/web_server` and all eighteen of its
`http_api/api_*.c` handlers are compiled and run here, and mongoose needs no
porting — it detects the host as `MG_ARCH_UNIX` and uses ordinary BSD sockets
where on the device it runs over lwIP. Routing, JSON parsing, validation and
error codes are the firmware's.

So a client is pointed at the simulator by changing one string:

```python
from busylib import BusyBar

bb = BusyBar("127.0.0.1:8042")          # a device would be "10.0.4.20"
print(bb.version())
```

Two runnable examples, neither of them simulator-specific:

- `examples/hello_busy.py` — a complete app: upload an asset, draw to the front
  panel, play a sound, read the screen back.
- `examples/watch_state.py` — find the device over mDNS, then follow its state
  stream. Move the mode lever in the window and the events appear.

Port 8042 rather than the device's 80: binding 80 needs root, and a high port
lets two simulators run side by side. `--api-port 0` turns the server off.

What is served, and how real it is:

| Route | Behaviour |
| --- | --- |
| `/api/display/*` | Real. The canvas service is compiled in, so priority arbitration against the running app works as it does on the device. |
| `/api/assets/*`, `/api/storage/*` | Real, against the asset tree under `build/assets_root`. |
| `/api/audio/play`, `/api/audio/stop` | Real; sound comes out of the workstation's speakers. |
| `/api/input` | Real. A key is injected into the same queue the keyboard feeds, so the UI cannot tell it apart from a keypress. |
| `/api/screen` | Real, the actual framebuffer. |
| `/api/busy/*` | Real; `busy_timer` is the firmware's own service. |
| `/api/version`, `/api/status`, `/api/name`, `/api/time` | Real values — git commit, uptime, host clock. |
| `/api/log_dump` | Real; `log_storage` is compiled in and captures the simulator's log. |
| `/api/wifi/*`, `/api/ble/*`, `/api/smart_home/*`, `/api/account` | Answered from the inert services: scans come back empty, connects fail, settings round-trip in memory. |
| `/api/update/*` | Answered, and refuses to install. There is no firmware image this process could reboot into. |
| `/api/status/ws` | Real. See below. |

Two things that surprise people writing against it, neither specific to the
simulator — the device does both:

- `/api/screen` returns **base64**, not raw bytes, and the frame is **BGR**, not
  RGB (LVGL's "RGB888" is byte-order blue-green-red). Decode and swap:
  `base64.b64decode(...)`, then channels 2,1,0.
- Rectangle elements default to `fill: "none"` and take `fill_colors`, not
  `color`. A rectangle given `color` and no `fill` draws as a white outline.

### The state stream

`/api/status/ws` is a WebSocket carrying protobuf, and it is the real thing:
`assets/proto` is compiled to C by nanopb's generator — the same generator fbt
runs, driven by `tools/gen_proto.py` — and `state_publisher` and
`api_status_streaming.c` are the firmware's own sources. A client decoding the
simulator's frames is decoding the device's schema.

The protocol is two text messages over the socket:

| Send | Effect |
| --- | --- |
| `{"enable":true}` | start the stream: screen frames at 10 fps, plus an update whenever something changes |
| `{"send":"all"}` | push a complete snapshot of everything at once |

Screen frames arrive continuously; everything else is sent on change. So a
client that only sends `{"enable":true}` — busylib's `stream_status_ws()` is
one — sees nothing but `frame` updates until something moves. Send
`{"send":"all"}` first if you want the current state.

What comes through, verified against a running simulator: `frame` (72x16,
run-length encoded), `input` (button press/release, encoder deltas, and mode
selector positions), `device_name`, `power`, `brightness`, `audio_volume`,
`wifi`, `matter`, `ble`, `timezone`, `timer` and `timer_profiles`. The inert
services report their idle state and then stay quiet, because nothing changes
them.

### Discovery

The simulator announces itself over mDNS, so `BusyBarDevices.discover()` finds
it:

```python
from busylib.devices import BusyBarDevices

for device in BusyBarDevices.discover():
    print(device.name, device.addresses)     # BUSY Simulator {...}
```

Two services are registered, and only one of them is the firmware's:

- `_http._tcp` — what `web_server` asks for through `discovery_service_add()`,
  forwarded unchanged except for the port, which is the one the simulator is
  really listening on rather than 80.
- `_busybar._tcp` — what busylib's `discover()` actually browses for. This
  firmware does not announce it; the simulator adds it so the client library
  can find it at all.

**Nothing found this way can be mistaken for hardware.** Both instance names
end in `-sim-<hostname>`, both carry a `simulator=1` TXT record, and the
advertised name is whatever the device name is set to — "BUSY Simulator" out of
the box. The hostname suffix also keeps two simulators on one network apart.

`--no-mdns` turns the announcements off; `--api-port 0` turns off the server and
the announcements with it.

Announcing goes through the platform's own responder — mDNSResponder on macOS,
avahi's compatibility layer on Linux — rather than lwIP's, which the device
uses. That is deliberate: it handles name conflicts, interface changes and
goodbye packets, which matters when the "device" is a laptop that sleeps and
moves between networks.

## Writing a scene

`src/scene_host.c` is the file to edit for hand-built scenes. A scene gets a
`Gui*` and builds a widget tree with the same calls an application makes.

Note that `Color` carries alpha and the widget APIs use it — a colour built
without `.a` is fully transparent and nothing will appear.

## Window layout

The front LED bar gets the full window width and the back panel takes what is
left underneath, so their on-screen sizes deliberately do not reflect their
physical sizes: the front panel is only 16 rows tall and needs the
magnification to be readable. Both scales stay integers, because a fractional
one makes some source pixels a row wider than their neighbours — distortion in
exactly the layouts this tool exists to check. Resizing the window recomputes
both.

## How it fits together

Three kinds of thread:

- **main** — SDL only: window, event pump, present. It blocks every signal
  except `SIGINT` *before* `SDL_Init`, so that the threads SDL and AppKit
  create inherit the mask. The tick handler switches context and then suspends
  the thread it ran on, so a tick caught by a thread that is not a FreeRTOS
  task parks that thread on some task's condvar and deadlocks the scheduler.
  The port is also patched to aim the tick at one thread — see below — which
  is the actual guarantee; the mask is a second line of defence.
- **scheduler** — `furi_init()` then `vTaskStartScheduler()`.
- **tasks** — `gui_srv`, the input translator, the services, the app, LVGL's
  draw thread, and the reaper.

The reaper is easy to miss and nothing works without it. A finished FreeRTOS
task cannot free the stack it is standing on, so furi hands it to
`furi_thread_scrub()` to be deleted — and that is also where
`FuriThreadStateStopped` is delivered, which is how the loader learns an app has
exited. The device runs that loop on its init task once startup is done (see
`targets/f21/src/main.c`); the simulator gives it a task of its own. Without it
an app can start but never end, and the mode selector moves once and then
stops.

Pixels are the only thing crossing between SDL and FreeRTOS, copied under a
plain pthread mutex. No furi primitive is touched from the main thread and no
SDL handle from a task. Screenshots are the one place the two must meet: the
window read-back happens on the SDL thread, while the PNG encoder allocates
from furi's heap and so runs in a task, with a flag as the handshake.

## Sources that needed the host treatment

Six files. Each is patched into the build directory at configure time — the
originals are never touched, and each patch fails loudly if the source drifts
out from under it.

| Source | Why | Handled by |
| --- | --- | --- |
| `furi/core/check.h` and `.c` | The crash message travels in `r12` via inline asm, expanded at every `furi_check()` call site, and the handler dumps Cortex-M registers and reads `CoreDebug->DHCSR`. | `shim/check_host.{h,c}`, copied over the staged tree |
| `lib/anim_file/anim_file.c` | One GCC nested function, which clang has never supported. | `tools/patch_anim_file.py` |
| `lib/toolbox/dsp.c` | The convolution inner loop is Thumb-2 DSP assembly (`uxtb`, `smlabb`, `bfi`). | `tools/patch_dsp.py` |
| `lvgl/src/libs/bin_decoder/lv_bin_decoder.c` | fbt rewrites it to claim the firmware's `.image` extension instead of `.bin`; unpatched, every icon fails to decode. | `tools/patch_bin_decoder.py` |
| `FreeRTOS-Kernel/portable/ThirdParty/GCC/Posix/port.c` | It compiles fine but deadlocks: the tick is a process-directed `SIGALRM`, so any thread can be handed it. See below. | `tools/patch_posix_port.py` |
| `applications/services/web_server/web_server.c` | One line: it binds `http://0.0.0.0`, which mongoose reads as port 80 — privileged here, and single-instance. | `tools/patch_web_server.py` |

Two more substitutions are by design rather than necessity: `lvgl_addons/fs` is
replaced by `src/lv_fs_host.c` because the original routes LVGL file access
through the storage service, and the assets are converted by
`tools/gen_internal_assets.py` and `tools/gen_runtime_assets.py` rather than by
fbt.

### The tick

Worth its own note, because it was the one bug that made the simulator look
unreliable rather than broken. The vendored kernel is V10.5.1 (Nov 2022) and
its POSIX port raises the tick with `setitimer(ITIMER_REAL)`. That signal is
directed at the *process*, so the kernel hands it to any thread that has not
blocked it. `SDL_Init` brings AppKit along, and AppKit's `_NSEventThread`
caught a tick: it ran `vPortSystemTickHandler` on a thread that is not a
FreeRTOS task, called `vTaskSwitchContext()`, and then suspended itself on the
condvar of whichever task happened to be current. Nothing ever resumed it and
that task never ran again — every FreeRTOS thread ends up parked in
`prvSuspendSelf()`, which is what `sample` shows on a wedged process.

The same corruption of `pxCurrentTCB`, caught a beat later, is what the
sporadic `taskSELECT_HIGHEST_PRIORITY_TASK` and `xTaskPriorityDisinherit`
asserts were. They were never a priority-inheritance approximation and had
nothing to do with machine load.

Upstream FreeRTOS replaced the interval timer with a tick thread that aims the
signal with `pthread_kill()` at the thread running the current task, which no
foreign thread can intercept. `tools/patch_posix_port.py` backports that, and
additionally sleeps to absolute deadlines: upstream's plain `usleep(period)`
makes the real period the period *plus* the cost of the tick, which measures
1.25 ms for a 1 ms sleep here and ran the whole UI 30% slow.

Measured after the fix: 36 runs of 300–400 frames across all four scenes, idle
and under a saturated CPU, with no hang and no assert; simulated time tracks
wall time at 1.00.

The allocator is furi's own `memmgr_heap.c` running over a static 32 MB region
handed to it by `shim/furi_hal_host.c`, not a FreeRTOS `heap_N.c`. furi
overrides `malloc`, so heap_3 — which forwards to `malloc` — recurses until the
stack dies.

## Known gaps

- Images are converted as ARGB8888 rather than the indexed formats fbt picks,
  to avoid a `pngquant` dependency. Icons with gradients therefore look
  smoother here than on the device, which quantises them.
- `pthread_attr_setstack` warnings at startup are harmless. macOS wants
  page-aligned stacks and furi's thread stacks are not, so the port falls back
  to default-sized pthread stacks — more headroom than the device, not less.
  It also means the simulator will not reproduce stack-overflow bugs.
- Simulated time can lag the host clock. Each tick sleeps to an absolute
  deadline, so short overshoots are corrected on the next tick and the rate
  comes out right, but a backlog past `portTICK_RESYNC_THRESHOLD_MS` (250 ms,
  e.g. after the host suspends) is dropped rather than replayed as a burst.
- Renaming the device over `/api/name` lasts until the process exits; there is
  no settings partition to persist it to. The mDNS announcement carries the
  name it had at startup and is not re-registered when it changes.
- The out-of-the-box flow is skipped. The startup app waits for a button and
  plays an animation off the recovery partition, which the simulator has no
  counterpart for, so it marks setup complete on first run
  (`<assets_root>/data/done.txt`) and boots as an unboxed device.
- `services_host.c` stubs are inert: autoupdates and low-power locks do
  nothing.
- The mode selector is a lever with no detents here. Every position is
  reachable, but the simulator has no notion of "between positions", so the
  brief invalid state a real lever passes through is not reproduced.
