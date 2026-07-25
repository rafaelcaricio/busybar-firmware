#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Drive a running simulator: press buttons, take screenshots, record a GIF.

The simulator can be walked from a shell — the firmware's own HTTP API takes
button presses, and SIGUSR2 makes the window capture itself — but doing that by
hand has sharp edges. This wraps them:

  * Waiting for the API to come up, rather than sleeping and hoping.
  * Waiting for a capture to finish. An encode takes a couple of seconds, so a
    naive `ls` right after the signal finds nothing, or finds a half-written
    file.
  * The key names. `/api/input` speaks the device's names, where `up` is a
    direction of dial rotation and moves the highlight *down* the list. That is
    the device's contract and this does not change it, but it also accepts
    `next` and `prev`, which say what happens on screen.
  * Recording. `record` runs a walkthrough with the window streaming to disk as
    raw frames, then hands them to ffmpeg for the GIF — the one part that is
    not stdlib, and the only thing here that needs a tool on PATH.

Everything else is stdlib: this has to run anywhere the simulator builds.

    simctl.py start --scene busy
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
import os
import shutil
import signal
import socket
import subprocess
import sys
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


def read_session(build: Path) -> dict:
    path = session_path(build)
    if not path.is_file():
        raise SimctlError(f"no session; run `simctl.py start` first ({path} is missing)")

    session = json.loads(path.read_text())

    try:
        os.kill(session["pid"], 0)
    except (OSError, ProcessLookupError):
        raise SimctlError(f"the simulator (pid {session['pid']}) is gone; start it again")

    return session


def write_session(build: Path, session: dict) -> None:
    session_path(build).write_text(json.dumps(session, indent=2) + "\n")


# -- talking to the simulator ----------------------------------------------


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


def resolve_key(name: str) -> str:
    key = KEY_ALIASES.get(name, name)
    if key not in DEVICE_KEYS:
        known = ", ".join(list(DEVICE_KEYS) + list(KEY_ALIASES))
        raise SimctlError(f"unknown key {name!r}; known keys: {known}")

    return key


# -- commands --------------------------------------------------------------


def command_start(args) -> int:
    binary = args.build / "busybar-sim"
    if not binary.is_file():
        raise SimctlError(f"{binary} is missing; build the simulator first")

    if api_is_up(args.port):
        raise SimctlError(
            f"something is already answering on port {args.port}; "
            "stop it or pass --port"
        )

    shots = args.shots.resolve()
    shots.mkdir(parents=True, exist_ok=True)

    log_path = shots / "sim.log"
    log = log_path.open("wb")

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
    ]
    if args.scene:
        command += ["--scene", args.scene]
    command += args.extra

    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)

    deadline = time.monotonic() + START_TIMEOUT
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SimctlError(
                f"the simulator exited with {process.returncode}; see {log_path}"
            )
        if api_is_up(args.port):
            break
        time.sleep(0.2)
    else:
        process.terminate()
        raise SimctlError(f"the API never came up; see {log_path}")

    write_session(
        args.build,
        {
            "pid": process.pid,
            "port": args.port,
            "shots": str(shots),
            "log": str(log_path),
            "scene": args.scene,
            "control": str(control_path),
        },
    )

    print(f"simulator up: pid {process.pid}, port {args.port}, captures in {shots}")
    return 0


def press(session: dict, name: str, settle: float) -> None:
    key = resolve_key(name)

    api(session, f"/api/input?key={key}", method="POST")
    if key != name:
        print(f"press {name} (device key {key})")
    else:
        print(f"press {key}")

    time.sleep(settle)


def command_press(args) -> int:
    session = read_session(args.build)

    for name in args.keys:
        press(session, name, args.settle)

    return 0


def shot(session: dict, label: str | None) -> list[Path]:
    """Capture, and wait for the encoder to finish rather than for a guess.

    The simulator logs one line naming the window file once all three images
    are written, so that line is the completion signal.
    """
    log_path = Path(session["log"])
    start = log_path.stat().st_size if log_path.is_file() else 0

    os.kill(session["pid"], signal.SIGUSR2)

    deadline = time.monotonic() + CAPTURE_TIMEOUT
    written = None

    while time.monotonic() < deadline:
        with log_path.open() as log:
            log.seek(start)
            for line in log:
                if "[sim] wrote " in line:
                    written = line.split("[sim] wrote ", 1)[1].split(" and ")[0].strip()
                    break

        if written:
            break
        time.sleep(0.1)

    if not written:
        raise SimctlError(f"the capture did not finish within {CAPTURE_TIMEOUT:.0f}s")

    window = Path(written)
    # window-007.png -> the -007 the other two share.
    sequence = window.stem.split("-")[-1]
    captured = []

    for name in ("front", "back", "window"):
        source = window.with_name(f"{name}-{sequence}.png")
        if label:
            target = window.with_name(f"{label}-{name}.png")
            source.replace(target)
            source = target
        captured.append(source)

    print("\n".join(str(path) for path in captured))
    return captured


def command_shot(args) -> int:
    shot(read_session(args.build), args.label)
    return 0


def walk(session: dict, steps: list[str], settle: float) -> None:
    """Play a walkthrough: keys, `shot`, `shot:label` and `wait:1.5`."""
    for step in steps:
        if step == "shot":
            shot(session, None)
        elif step.startswith("shot:"):
            shot(session, step.split(":", 1)[1])
        elif step.startswith("wait:"):
            time.sleep(float(step.split(":", 1)[1]))
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

    print(f"pid {session['pid']}, port {session['port']}")
    print(f"captures {session['shots']}")
    print(f"log      {session['log']}")
    if session.get("control"):
        print(f"control  {session['control']}")

    status = json.loads(api(session, "/api/status"))
    firmware = status.get("firmware", {})
    system = status.get("system", {})
    print(f"firmware {firmware.get('version')} ({firmware.get('commit_hash')})")
    print(f"uptime   {system.get('uptime')}")

    return 0


def command_log(args) -> int:
    session = read_session(args.build)
    lines = Path(session["log"]).read_text(errors="replace").splitlines()

    for line in lines[-args.lines :]:
        print(line)

    return 0


def command_stop(args) -> int:
    path = session_path(args.build)
    if not path.is_file():
        print("no session")
        return 0

    session = json.loads(path.read_text())
    try:
        os.kill(session["pid"], signal.SIGTERM)
        print(f"stopped pid {session['pid']}")
    except (OSError, ProcessLookupError):
        print(f"pid {session['pid']} was already gone")

    path.unlink()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD, help="build directory")
    subparsers = parser.add_subparsers(dest="command", required=True)

    start = subparsers.add_parser("start", help="launch a simulator and wait for its API")
    start.add_argument("--scene", help="app to boot into, as --scene does")
    start.add_argument("--shots", type=Path, default=Path("/tmp/busybar-sim-shots"))
    start.add_argument("--port", type=int, default=DEFAULT_PORT)
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
