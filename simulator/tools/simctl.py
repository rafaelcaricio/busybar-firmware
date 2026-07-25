#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Drive a running simulator: press buttons, take screenshots, read state.

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

Everything is stdlib: this has to run anywhere the simulator builds.

    simctl.py start --scene busy
    simctl.py press next ok
    simctl.py shot setup
    simctl.py run next ok shot:theme next shot:second-theme
    simctl.py status
    simctl.py stop

`start` leaves the simulator running and returns. Session state lives in the
build directory, so later commands need no arguments.
"""

import argparse
import json
import os
import signal
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

    command = [str(binary), "--screenshot", str(shots), "--api-port", str(args.port)]
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


def command_run(args) -> int:
    """A whole walkthrough in one line: keys, `shot`, `shot:label`, `wait:1.5`."""
    session = read_session(args.build)

    for step in args.steps:
        if step == "shot":
            shot(session, None)
        elif step.startswith("shot:"):
            shot(session, step.split(":", 1)[1])
        elif step.startswith("wait:"):
            time.sleep(float(step.split(":", 1)[1]))
        else:
            press(session, step, args.settle)

    return 0


def command_status(args) -> int:
    session = read_session(args.build)

    print(f"pid {session['pid']}, port {session['port']}")
    print(f"captures {session['shots']}")
    print(f"log      {session['log']}")

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
