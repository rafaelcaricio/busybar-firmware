---
name: busybar-simulator
description: Run the BUSY Bar firmware on the workstation and check a change by looking at it — walk the UI, capture the two displays, read the log. Use whenever a change touches the GUI, a widget, an app scene, an animation or an asset, or when a question is about what the device actually shows rather than what the code says.
---

# Validating firmware in the simulator

`simulator/` builds the firmware as a native binary with the displays on SDL.
Both panels render, every GUI app runs, and the firmware's own HTTP API takes
button presses — so a UI change can be *seen* rather than reasoned about. Do
that. A screenshot settles in one step what reading three widget files does not.

## Build it once

```sh
cmake -S simulator -B simulator/build -G Ninja
cmake --build simulator/build
```

Reconfigure (not just rebuild) after touching anything under `assets/` or an
app's `resources/`: the asset tree is generated at configure time.

## The loop

`simulator/tools/simctl.py` is the driver. It handles waiting for the API,
waiting for a capture to finish, and the key names.

```sh
simulator/tools/simctl.py start --scene busy --shots /tmp/shots
simulator/tools/simctl.py run next ok shot:setup next ok shot:theme
simulator/tools/simctl.py log --lines 40
simulator/tools/simctl.py stop
```

`run` takes button names, `shot`, `shot:<label>` and `wait:<seconds>` in one
line. Each capture prints the three files it wrote; read them.

| command | what it does |
| --- | --- |
| `start [--scene NAME] [--shots DIR] [--port N]` | launch and wait for the API to answer |
| `press KEY...` | one or more buttons |
| `shot [LABEL]` | capture, wait for the encode, print the paths |
| `run STEP...` | keys, `shot`, `shot:label`, `wait:N` in sequence |
| `status` | pid, port, capture directory, firmware version, uptime |
| `log [--lines N]` | tail the simulator's output |
| `stop` | end the session |

Each capture writes three files. **`back.png` and `front.png` are the ones to
read**: they are the framebuffers magnified pixel for pixel, so text is legible
and a one-pixel misalignment is visible. `window.png` is the whole window with
the device drawn around the panels — read it to judge presentation, not layout.

## Button names

`next` and `prev` move the highlight down and up the list. Prefer them.

They exist because the underlying `/api/input` speaks the *device's* names,
where `up` and `down` are directions of dial rotation, and rotating "up" moves
the highlight **down**. That inversion is the device's real contract, so
`simctl.py` still accepts `up`/`down` and passes them through untouched — but
if you type `up` meaning "move up the list", you will get the opposite and
misread the result. The other buttons are literal: `ok`, `back`, `start`, and
the five lever positions `busy`, `custom`, `off`, `apps`, `settings`.

## Reading the result

- Screenshots are the evidence. Look at the image before saying a change works.
- `log --lines N` catches what a screenshot cannot: `[E]` lines from a service,
  a failed asset load, an assert. Check it even when the picture looks right.
- `status` returns the device's own `/api/status` JSON, so the firmware version
  and uptime come from the running build rather than from assumption.
- Anything the firmware exposes is reachable: `curl` the API directly for
  endpoints `simctl.py` does not wrap.

## Things that will cost you an hour

- **A capture takes about two seconds.** Nearly all of it is encoding the
  window. `simctl.py shot` waits for the simulator's own "wrote" log line, so
  use it rather than sleeping and listing the directory — a bare `ls` right
  after the trigger finds nothing, or finds a half-written file.
- **The first frames are not the app.** The desktop service boots, the loader
  starts the app, and the wipe animation plays. `start` returns once the API
  answers, which is earlier than that. Capture, look, and add `wait:1` if the
  screen is still mid-transition.
- **A blank or stale panel is usually an asset, not a widget.** Check the log
  for `AnimFile` or asset errors first.
- **Reconfigure after asset changes.** A plain `cmake --build` will not notice
  a new PNG, animation zip or `resources/` file.

## When the simulator is the thing being changed

`simulator/README.md` is thorough and current — read the section that covers
what you are touching before editing. In particular: every SDL call belongs on
the main thread, the PNG encoder allocates from furi's heap and so must run in
a task, and the FreeRTOS POSIX port has already claimed `SIGALRM` and
`SIGUSR1`.
