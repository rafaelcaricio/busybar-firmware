#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Build, diagnose and drive the BusyBar simulator.

The simulator can be walked from a shell through the firmware's own HTTP API,
but doing that by hand has sharp edges. This wraps them:

  * Configuring, building and running the smoke regression with one command.
  * Probing the exact CMake configuration in a temporary directory when host
    dependencies or submodules are suspect.
  * Waiting for both the HTTP API and rendered displays to become ready.
  * Waiting for exact presented-frame boundaries and coherent screenshots,
    rather than sleeping and hoping.
  * The key names. `/api/input` speaks the device's names, where `up` is a
    direction of dial rotation and moves the highlight *down* the list. That is
    the device's contract and this does not change it, but it also accepts
    `next` and `prev`, which say what happens on screen.
  * Recording. `record` runs a walkthrough with the window streaming to disk as
    raw frames, then hands them to ffmpeg for the GIF — the one part that is
    not stdlib, and the only thing here that needs a tool on PATH.

Everything except GIF encoding is stdlib: this has to run anywhere the
simulator builds.

    simctl.py doctor
    simctl.py build --test
    simctl.py start --scene busy --port 0
    simctl.py press next ok
    simctl.py shot setup
    simctl.py run next ok shot:theme next shot:second-theme
    simctl.py record --out demo.gif start wait:3 apps wait:2 next ok wait:2
    simctl.py status
    simctl.py stop

`start` leaves the simulator running and returns. Session state lives in the
build directory, so later commands need no arguments.
"""

import argparse
import json
import math
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

SIMULATOR_DIR = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = SIMULATOR_DIR / "build"
DEFAULT_PORT = 8042

# What /api/input accepts. Straight from applications/services/web_server/
# http_api/api_input.c — these are the device's names, not the window's.
DEVICE_KEYS = (
    "up", "down", "ok", "back", "start",
    "busy", "custom", "off", "apps", "settings",
)

# The dial spelled by what it does on screen. The firmware names the two
# directions after the dial, and the widget layer turns InputKeyUp into
# "focus the next item", i.e. the one below. See the scroll direction note in
# simulator/README.md; this is the same inversion, made sayable.
KEY_ALIASES = {
    "next": "up",
    "prev": "down",
}

CAPTURE_TIMEOUT = 30.0
START_TIMEOUT = 60.0
CONTROL_TIMEOUT = 90.0
FRAME_SECONDS = 0.016
CAPTURE_KINDS = ("front-raw", "back-raw", "front", "back", "window")

# Recording defaults. Twelve frames a second is enough for the wipes and the
# scrolling labels, and every one of them is a whole window read back off the
# GPU. The divisor is applied in the simulator, before the frames reach the
# disk: a canvas is around four megapixels, which is 12 MB a frame raw.
RECORD_FPS = 12
RECORD_DIVISOR = 3


class SimctlError(RuntimeError):
    pass


# -- session ---------------------------------------------------------------


def session_path(build: Path) -> Path:
    return build / "simctl-session.json"


def load_session(build: Path) -> dict:
    path = session_path(build)
    if not path.is_file():
        raise SimctlError(f"no session; run `simctl.py start` first ({path} is missing)")

    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, OSError) as error:
        raise SimctlError(f"cannot read session metadata from {path}: {error}")


def process_matches(session: dict) -> bool:
    try:
        os.kill(session["pid"], 0)
    except (OSError, ProcessLookupError, KeyError):
        return False

    expected = session.get("binary")
    if not expected:
        return True

    proc_executable = Path(f"/proc/{session['pid']}/exe")
    if proc_executable.exists():
        try:
            return proc_executable.resolve() == Path(expected).resolve()
        except OSError:
            return False

    result = subprocess.run(
        ["ps", "-p", str(session["pid"]), "-o", "command="],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return False
    command = result.stdout.strip()
    return command == expected or command.startswith(expected + " ")


def read_session(build: Path) -> dict:
    session = load_session(build)
    if not process_matches(session):
        raise SimctlError(f"the simulator (pid {session['pid']}) is gone; start it again")

    return session


def write_session(build: Path, session: dict) -> None:
    session_path(build).write_text(json.dumps(session, indent=2) + "\n")


def cleanup_session(build: Path, session: dict) -> None:
    """Remove only tool-owned transient artifacts named by session metadata."""
    control_path = session.get("control")
    if control_path:
        candidate = Path(control_path)
        try:
            if (
                candidate.name.startswith("simctl-control")
                and candidate.parent.resolve() == build.resolve()
            ):
                candidate.unlink(missing_ok=True)
        except OSError:
            pass

    state_value = session.get("state")
    if session.get("ephemeral_state") and state_value:
        candidate = Path(state_value).resolve()
        temporary_root = Path(tempfile.gettempdir()).resolve()
        if (
            candidate.name.startswith("busybar-sim-state-")
            and candidate.is_relative_to(temporary_root)
        ):
            shutil.rmtree(candidate, ignore_errors=True)

    session_path(build).unlink(missing_ok=True)


# -- talking to the simulator ----------------------------------------------


def reserve_port() -> int:
    """Choose an available loopback port for `start --port 0`."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def api(session: dict, path: str, method: str = "GET", timeout: float = 5.0) -> str:
    url = f"http://127.0.0.1:{session['port']}{path}"
    request = urllib.request.Request(url, method=method)

    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode()


def api_is_up(port: int) -> bool:
    try:
        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/status", timeout=1.0
        ) as response:
            return response.status == 200
    except (urllib.error.URLError, OSError):
        return False


def control(session: dict, request: str, timeout: float = CONTROL_TIMEOUT) -> list[str]:
    """Ask the simulator's control socket something and return the reply's words.

    One line in, one line out; see src/sim_control.h. `error ...` comes back as
    an exception, so callers only ever see a reply that worked.
    """
    path = session.get("control")
    if not path:
        raise SimctlError("this session has no control socket; start the simulator again")

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)

        try:
            client.connect(path)
        except OSError as error:
            raise SimctlError(f"cannot reach the control socket at {path}: {error}")

        client.sendall(request.encode() + b"\n")

        reply = b""
        while b"\n" not in reply:
            try:
                chunk = client.recv(4096)
            except socket.timeout:
                raise SimctlError(f"the simulator did not answer {request!r} in {timeout:.0f}s")

            if not chunk:
                break
            reply += chunk

    words = reply.decode(errors="replace").split("\n", 1)[0].split()
    if not words:
        raise SimctlError(f"the simulator closed the socket without answering {request!r}")
    if words[0] != "ok":
        raise SimctlError(" ".join(words[1:]) or "the simulator refused the request")

    return words[1:]


def renderer_status(session: dict, timeout: float = 5.0) -> dict:
    words = control(session, "status", timeout=timeout)
    if len(words) != 8:
        raise SimctlError(f"malformed renderer status: {' '.join(words)!r}")

    frames, front_updates, back_updates, width, height, quitting, heap_free, heap_min = (
        int(value) for value in words
    )
    return {
        "frames": frames,
        "front_updates": front_updates,
        "back_updates": back_updates,
        "width": width,
        "height": height,
        "quitting": bool(quitting),
        "heap_free": heap_free,
        "heap_min": heap_min,
    }


def wait_frames(session: dict, count: int, timeout: float | None = None) -> int:
    if count < 0:
        raise SimctlError("frame count must not be negative")
    if count == 0:
        return renderer_status(session)["frames"]

    status = renderer_status(session)
    target = status["frames"] + count
    if timeout is None:
        timeout = max(5.0, count * FRAME_SECONDS * 4)
    if not math.isfinite(timeout) or timeout <= 0 or timeout > 600:
        raise SimctlError("frame wait timeout must be between 0 and 600 seconds")

    timeout_ms = max(1, min(600_000, math.ceil(timeout * 1000)))
    words = control(
        session,
        f"wait frame {target} {timeout_ms}",
        timeout=timeout + 1,
    )
    if len(words) != 1:
        raise SimctlError(f"malformed frame-wait reply: {' '.join(words)!r}")
    return int(words[0])


def wait_seconds(session: dict, seconds: float) -> int:
    if not math.isfinite(seconds) or seconds < 0:
        raise SimctlError("wait duration must be a finite, non-negative number")
    return wait_frames(session, math.ceil(seconds / FRAME_SECONDS))


def resolve_key(name: str) -> str:
    key = KEY_ALIASES.get(name, name)
    if key not in DEVICE_KEYS:
        known = ", ".join(list(DEVICE_KEYS) + list(KEY_ALIASES))
        raise SimctlError(f"unknown key {name!r}; known keys: {known}")

    return key


# -- commands --------------------------------------------------------------


def run_checked(command: list[str], purpose: str) -> None:
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise SimctlError(f"{purpose} failed with exit code {result.returncode}")


def cmake_generator(requested: str | None, build: Path | None = None) -> list[str]:
    if requested:
        return ["-G", requested]
    if build is not None and (build / "CMakeCache.txt").is_file():
        return []
    if shutil.which("ninja"):
        return ["-G", "Ninja"]
    return []


def command_doctor(args) -> int:
    """Exercise the real configure path without leaving a build behind."""
    tools = {
        "cmake": shutil.which("cmake"),
        "uv": shutil.which("uv"),
        "ninja": shutil.which("ninja"),
        "ffmpeg": shutil.which("ffmpeg"),
    }
    roles = {
        "cmake": "required",
        "uv": "required",
        "ninja": "preferred",
        "ffmpeg": "optional; GIF recording",
    }
    for name, path in tools.items():
        print(f"{name:<7} {path or 'missing'} ({roles[name]})")

    if not tools["cmake"] or not tools["uv"]:
        raise SimctlError("cmake and uv are required")

    with tempfile.TemporaryDirectory(prefix="busybar-sim-doctor-") as temporary:
        probe = Path(temporary)
        command = [
            tools["cmake"],
            "-S",
            str(SIMULATOR_DIR),
            "-B",
            str(probe),
            *cmake_generator(args.generator),
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DBUILD_TESTING=ON",
        ]
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        if result.returncode != 0:
            output = (result.stdout + result.stderr).strip()
            if output:
                print(output, file=sys.stderr)
            raise SimctlError("the simulator CMake dependency probe failed")

    print("configure probe passed: compiler, SDL2, host networking and submodules are ready")
    return 0


def command_build(args) -> int:
    """Configure and build the simulator, optionally running its smoke test."""
    cmake = shutil.which("cmake")
    if not cmake:
        raise SimctlError("cmake is not on PATH")
    if args.heap_mb < 8:
        raise SimctlError("--heap-mb must be at least 8")
    if args.jobs is not None and args.jobs < 1:
        raise SimctlError("--jobs must be at least 1")

    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    configure = [
        cmake,
        "-S",
        str(SIMULATOR_DIR),
        "-B",
        str(build),
        *cmake_generator(args.generator, build),
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        f"-DBUSYBAR_SIM_HEAP_MB={args.heap_mb}",
        f"-DBUILD_TESTING={'ON' if args.test else 'OFF'}",
    ]
    run_checked(configure, "CMake configure")

    build_command = [cmake, "--build", str(build), "--parallel"]
    if args.jobs:
        build_command.append(str(args.jobs))
    run_checked(build_command, "simulator build")

    if args.test:
        ctest = shutil.which("ctest")
        if not ctest:
            raise SimctlError("ctest is not on PATH")
        run_checked(
            [ctest, "--test-dir", str(build), "--output-on-failure"],
            "simulator smoke test",
        )

    print(f"simulator ready: {build / 'busybar-sim'}")
    return 0


def command_start(args) -> int:
    binary = (args.build / "busybar-sim").resolve()
    if not binary.is_file():
        raise SimctlError(f"{binary} is missing; build the simulator first")
    if args.port < 0 or args.port > 65535:
        raise SimctlError("--port must be between 0 and 65535")
    if args.port == 0:
        args.port = reserve_port()

    if session_path(args.build).is_file():
        existing = load_session(args.build)
        if process_matches(existing):
            raise SimctlError(
                f"a simulator session is already running as pid {existing['pid']}; "
                "stop it before starting another"
            )
        cleanup_session(args.build, existing)

    if api_is_up(args.port):
        raise SimctlError(
            f"something is already answering on port {args.port}; "
            "stop it or pass --port"
        )

    shots = args.shots.resolve()
    shots.mkdir(parents=True, exist_ok=True)

    log_path = shots / "sim.log"
    log = log_path.open("wb")

    if args.state_dir:
        state_dir = args.state_dir.resolve()
        state_dir.mkdir(parents=True, exist_ok=True)
        ephemeral_state = False
    else:
        state_dir = Path(tempfile.mkdtemp(prefix="busybar-sim-state-")).resolve()
        ephemeral_state = True

    # Beside the session file rather than in the capture directory: it belongs
    # to this simulator, and a unix socket path is short enough to run out of.
    control_path = args.build.resolve() / "simctl-control.sock"

    command = [
        str(binary),
        "--screenshot",
        str(shots),
        "--api-port",
        str(args.port),
        "--control",
        str(control_path),
        "--state-dir",
        str(state_dir),
    ]
    if args.scene:
        command += ["--scene", args.scene]
    command += args.extra

    process = subprocess.Popen(
        command,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )

    session = {
        "pid": process.pid,
        "binary": str(binary),
        "port": args.port,
        "shots": str(shots),
        "log": str(log_path),
        "scene": args.scene,
        "control": str(control_path),
        "state": str(state_dir),
        "ephemeral_state": ephemeral_state,
    }

    try:
        deadline = time.monotonic() + START_TIMEOUT
        api_ready = False
        render_status = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise SimctlError(
                    f"the simulator exited with {process.returncode}; see {log_path}"
                )

            if not api_ready:
                api_ready = api_is_up(args.port)

            try:
                candidate = renderer_status(session, timeout=1)
                if (
                    candidate["front_updates"] > 0
                    and candidate["back_updates"] > 0
                    and candidate["width"] > 0
                    and candidate["height"] > 0
                    and not candidate["quitting"]
                ):
                    render_status = candidate
            except SimctlError:
                pass

            if api_ready and render_status:
                break
            time.sleep(0.05)
        else:
            missing = []
            if not api_ready:
                missing.append("HTTP API")
            if not render_status:
                missing.append("rendered displays")
            raise SimctlError(f"{' and '.join(missing)} never became ready; see {log_path}")

        # A status request can observe a framebuffer submission during the
        # render that uploads it. Two subsequent presents establish a stable
        # handoff without an arbitrary startup sleep.
        target = render_status["frames"] + 2
        remaining = max(1.0, deadline - time.monotonic())
        control(
            session,
            f"wait frame {target} {min(600_000, math.ceil(remaining * 1000))}",
            timeout=remaining + 1,
        )
    except BaseException:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()
        control_path.unlink(missing_ok=True)
        if ephemeral_state:
            shutil.rmtree(state_dir, ignore_errors=True)
        raise
    finally:
        if not log.closed:
            log.close()

    write_session(args.build, session)

    print(
        f"simulator up: pid {process.pid}, port {args.port}, "
        f"captures in {shots}, state in {state_dir}"
    )
    return 0


def press(session: dict, name: str, settle: float) -> None:
    key = resolve_key(name)

    api(session, f"/api/input?key={key}", method="POST")
    if key != name:
        print(f"press {name} (device key {key})")
    else:
        print(f"press {key}")

    wait_seconds(session, settle)


def command_press(args) -> int:
    session = read_session(args.build)

    for name in args.keys:
        press(session, name, args.settle)

    return 0


def shot(session: dict, label: str | None) -> list[Path]:
    """Capture one coherent rendered frame and wait for all five PNGs."""
    if label and (
        label in (".", "..")
        or any(
            character
            not in "-_.abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
            for character in label
        )
    ):
        raise SimctlError("capture labels may contain only letters, digits, '.', '_' and '-'")

    shots = Path(session["shots"])
    if label:
        existing = [
            shots / f"{label}-{name}.png"
            for name in CAPTURE_KINDS
            if (shots / f"{label}-{name}.png").exists()
        ]
        if existing:
            raise SimctlError(f"capture label {label!r} already exists at {existing[0]}")

    reply = control(
        session,
        f"screenshot {shots}",
        timeout=CAPTURE_TIMEOUT,
    )
    if len(reply) != 2:
        raise SimctlError(f"malformed screenshot reply: {' '.join(reply)!r}")

    sequence = int(reply[0])
    captured_frame = int(reply[1])
    suffix = f"{sequence:03d}"
    captured = []

    for name in CAPTURE_KINDS:
        source = shots / f"{name}-{suffix}.png"
        if not source.is_file():
            raise SimctlError(f"simulator reported a capture, but {source} is missing")
        if label:
            target = shots / f"{label}-{name}.png"
            source.replace(target)
            source = target
        captured.append(source)

    print(f"captured rendered frame {captured_frame}", file=sys.stderr)
    print("\n".join(str(path) for path in captured))
    return captured


def command_shot(args) -> int:
    shot(read_session(args.build), args.label)
    return 0


def walk(session: dict, steps: list[str], settle: float) -> None:
    """Play keys, shots, frame waits and duration waits."""
    for step in steps:
        if step == "shot":
            shot(session, None)
        elif step.startswith("shot:"):
            label = step.split(":", 1)[1]
            if not label:
                raise SimctlError("shot: requires a label")
            shot(session, label)
        elif step.startswith("frames:"):
            value = step.split(":", 1)[1]
            try:
                frames = int(value)
            except ValueError:
                raise SimctlError(f"invalid frame count {value!r}")
            wait_frames(session, frames)
        elif step.startswith("wait:"):
            value = step.split(":", 1)[1]
            try:
                seconds = float(value)
            except ValueError:
                raise SimctlError(f"invalid wait duration {value!r}")
            wait_seconds(session, seconds)
        else:
            press(session, step, settle)


def command_run(args) -> int:
    """A whole walkthrough in one line: keys, `shot`, `shot:label`, `wait:1.5`."""
    walk(read_session(args.build), args.steps, args.settle)

    return 0


def encode_gif(
    raw: Path, gif: Path, width: int, height: int, fps: float, target_width: int
) -> None:
    """Turn the raw stream into a GIF.

    The palette is the whole problem: a front panel is a few thousand lit dots
    on black, and a fixed 256-colour table either loses the dots or loses the
    device around them. ffmpeg's palettegen reads the frames first and builds
    the table from what is actually there, and `stats_mode=diff` weights it
    towards the parts that move — the panels — rather than the case, which
    never changes. The two passes are one command: `split` feeds the frames to
    the generator and to the mapper.
    """
    if not shutil.which("ffmpeg"):
        raise SimctlError("ffmpeg is not on PATH; recording needs it to make the GIF")

    scale = f"scale={target_width}:-1:flags=lanczos," if target_width else ""
    palette = (
        f"[0:v]{scale}split[frames][sample];"
        "[sample]palettegen=stats_mode=diff[palette];"
        "[frames][palette]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle"
    )

    command = [
        "ffmpeg",
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-f", "rawvideo",
        "-pixel_format", "rgb24",
        "-video_size", f"{width}x{height}",
        "-framerate", f"{fps:.3f}",
        "-i", str(raw),
        "-filter_complex", palette,
        "-loop", "0",
        str(gif),
    ]

    result = subprocess.run(command, capture_output=True)
    if result.returncode != 0:
        raise SimctlError(f"ffmpeg failed: {result.stderr.decode(errors='replace').strip()}")


def command_record(args) -> int:
    """Record the window while a walkthrough plays, and write it as a GIF.

    The simulator streams whole frames; a still would miss the wipe between
    scenes, the scrolling labels and the timer counting down, which is most of
    what the interface does.
    """
    session = read_session(args.build)

    gif = args.out.resolve()
    gif.parent.mkdir(parents=True, exist_ok=True)

    # In the capture directory rather than beside the GIF: a minute of raw
    # frames is hundreds of megabytes, and this is the simulator's own scratch
    # space. It only outlives the encode with --keep-raw.
    raw = Path(session["shots"]) / f"{gif.stem}.raw"

    width, height, fps = (int(value) for value in control(
        session, f"record start {args.fps} {args.divisor} {raw}"))
    print(f"recording {width}x{height} at {fps} fps")

    try:
        walk(session, args.steps, args.settle)
    finally:
        frames, dropped, width, height, fps, elapsed_ms = (
            int(value) for value in control(session, "record stop"))

    print(f"captured {frames} frames ({dropped} dropped) in {raw}")
    if dropped:
        print("dropped frames are the writer falling behind; try a lower --fps")

    if not frames:
        raise SimctlError("nothing was recorded")

    # Reading a window back off the GPU costs more than the frame period asks
    # for, so the stream is usually slower than --fps. Encoding at the rate the
    # frames were actually spaced by is what keeps the GIF in real time.
    achieved = (frames - 1) * 1000 / elapsed_ms if frames > 1 and elapsed_ms else float(fps)
    if abs(achieved - fps) > 0.5:
        print(f"the window kept up with {achieved:.1f} of the {fps} fps asked for")

    try:
        encode_gif(raw, gif, width, height, achieved, args.width)
    except SimctlError:
        # Whatever went wrong, the frames took a walkthrough to capture and are
        # worth more than the disk they cost.
        print(f"keeping {raw}: {width}x{height} rgb24, {fps} fps", file=sys.stderr)
        raise

    if not args.keep_raw:
        raw.unlink(missing_ok=True)

    size = gif.stat().st_size
    print(f"{gif} ({elapsed_ms / 1000:.1f}s, {size / 1024 / 1024:.1f} MB)")

    return 0


def command_status(args) -> int:
    session = read_session(args.build)
    render = renderer_status(session)

    print(f"pid {session['pid']}, port {session['port']}")
    print(
        f"render   frame {render['frames']}, "
        f"front {render['front_updates']} updates, "
        f"back {render['back_updates']} updates, "
        f"{render['width']}x{render['height']}"
    )
    print(
        f"heap     {render['heap_free'] / 1024 / 1024:.1f} MiB free, "
        f"{render['heap_min'] / 1024 / 1024:.1f} MiB low-water"
    )
    print(f"captures {session['shots']}")
    print(f"log      {session['log']}")
    if session.get("control"):
        print(f"control  {session['control']}")
    if session.get("state"):
        kind = "temporary" if session.get("ephemeral_state") else "persistent"
        print(f"state    {session['state']} ({kind})")

    status = json.loads(api(session, "/api/status"))
    firmware = status.get("firmware", {})
    system = status.get("system", {})
    print(f"firmware {firmware.get('version')} ({firmware.get('commit_hash')})")
    print(f"uptime   {system.get('uptime')}")

    return 0


def command_power(args) -> int:
    session = read_session(args.build)
    current = control(session, "power")
    if len(current) != 3:
        raise SimctlError(f"malformed power status: {' '.join(current)!r}")

    charge, usb, charging = (int(value) for value in current)
    if args.charge is not None:
        if args.charge < 0 or args.charge > 100:
            raise SimctlError("--charge must be between 0 and 100")
        charge = args.charge
    if args.usb is not None:
        usb = int(args.usb == "connected")
        if not usb and args.charging is None:
            charging = 0
    if args.charging is not None:
        charging = int(args.charging == "yes")
    if charging and not usb:
        raise SimctlError("the simulated battery cannot charge while USB is disconnected")

    if args.charge is not None or args.usb is not None or args.charging is not None:
        current = control(session, f"power {charge} {usb} {charging}")
        if len(current) != 3:
            raise SimctlError(f"malformed power update: {' '.join(current)!r}")
        charge, usb, charging = (int(value) for value in current)

    print(
        f"battery {charge}%, USB {'connected' if usb else 'disconnected'}, "
        f"{'charging' if charging else 'not charging'}"
    )
    return 0


def command_log(args) -> int:
    # Logs are most valuable after a crash, so unlike interactive commands this
    # deliberately accepts a session whose process has already exited.
    session = load_session(args.build)
    lines = Path(session["log"]).read_text(errors="replace").splitlines()

    for line in lines[-args.lines :]:
        print(line)

    return 0


def command_stop(args) -> int:
    if not session_path(args.build).is_file():
        print("no session")
        return 0

    session = load_session(args.build)
    if process_matches(session):
        try:
            control(session, "quit", timeout=2)
        except SimctlError:
            os.kill(session["pid"], signal.SIGTERM)
        deadline = time.monotonic() + 5
        while process_matches(session) and time.monotonic() < deadline:
            time.sleep(0.1)
        if process_matches(session):
            os.kill(session["pid"], signal.SIGKILL)
        print(f"stopped pid {session['pid']}")
    else:
        print(f"pid {session['pid']} was already gone")

    cleanup_session(args.build, session)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD, help="build directory")
    subparsers = parser.add_subparsers(dest="command", required=True)

    doctor = subparsers.add_parser(
        "doctor", help="check host dependencies and required submodules"
    )
    doctor.add_argument("--generator", help="CMake generator to probe")
    doctor.set_defaults(func=command_doctor)

    build = subparsers.add_parser("build", help="configure and build the simulator")
    build.add_argument(
        "--build-type",
        choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"),
        default="Debug",
    )
    build.add_argument("--generator", help="CMake generator (fresh builds default to Ninja)")
    build.add_argument("--heap-mb", type=int, default=16, help="simulated furi heap size")
    build.add_argument("--jobs", type=int, help="parallel build jobs")
    build.add_argument("--test", action="store_true", help="run the end-to-end smoke test")
    build.set_defaults(func=command_build)

    start = subparsers.add_parser("start", help="launch a simulator and wait for its API")
    start.add_argument("--scene", help="app to boot into, as --scene does")
    start.add_argument("--shots", type=Path, default=Path("/tmp/busybar-sim-shots"))
    start.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help="loopback API port; 0 selects an available port",
    )
    start.add_argument(
        "--state-dir",
        type=Path,
        help="persistent writable state directory (default: fresh temporary state)",
    )
    start.add_argument("extra", nargs="*", help="further arguments for the simulator")
    start.set_defaults(func=command_start)

    press_parser = subparsers.add_parser("press", help="send button presses")
    press_parser.add_argument("keys", nargs="+")
    press_parser.add_argument("--settle", type=float, default=0.5, help="pause after each")
    press_parser.set_defaults(func=command_press)

    shot_parser = subparsers.add_parser("shot", help="capture and wait for it to land")
    shot_parser.add_argument("label", nargs="?", help="name the files instead of numbering")
    shot_parser.set_defaults(func=command_shot)

    run = subparsers.add_parser("run", help="a sequence of keys, shot[:label] and wait:N")
    run.add_argument("steps", nargs="+")
    run.add_argument("--settle", type=float, default=0.5)
    run.set_defaults(func=command_run)

    record = subparsers.add_parser(
        "record",
        help="record the window into a GIF while a walkthrough plays",
        description="Steps are the same as `run`; `wait:N` alone records N seconds "
        "of whatever is on screen. Needs ffmpeg on PATH.",
    )
    record.add_argument("--out", type=Path, required=True, help="the GIF to write")
    record.add_argument("--fps", type=int, default=RECORD_FPS, help="frames a second")
    record.add_argument(
        "--divisor",
        type=int,
        default=RECORD_DIVISOR,
        help="shrink each frame by this whole factor as it is captured",
    )
    record.add_argument(
        "--width", type=int, default=0, help="scale to this width when encoding"
    )
    record.add_argument(
        "--keep-raw", action="store_true", help="leave the raw frames beside the GIF"
    )
    record.add_argument("--settle", type=float, default=0.5)
    record.add_argument("steps", nargs="+", help="keys, shot[:label] and wait:N")
    record.set_defaults(func=command_record)

    status = subparsers.add_parser("status", help="session and device state")
    status.set_defaults(func=command_status)

    power = subparsers.add_parser("power", help="inspect or change simulated battery state")
    power.add_argument("--charge", type=int, help="battery percentage (0..100)")
    power.add_argument("--usb", choices=("connected", "disconnected"))
    power.add_argument("--charging", choices=("yes", "no"))
    power.set_defaults(func=command_power)

    log = subparsers.add_parser("log", help="tail the simulator's log")
    log.add_argument("--lines", type=int, default=40)
    log.set_defaults(func=command_log)

    stop = subparsers.add_parser("stop", help="stop the simulator")
    stop.set_defaults(func=command_stop)

    args = parser.parse_args()

    try:
        return args.func(args)
    except SimctlError as error:
        print(f"simctl: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
