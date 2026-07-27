#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Headless end-to-end smoke test for the BusyBar simulator."""

import argparse
import json
import os
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
REQUIRED_APPS = {
    "demo",
    "busy",
    "clock",
    "settings_menu",
    "apps_menu",
    "about",
    "input_test",
    "gui_test",
}


def fail(message: str, output: str = "") -> None:
    if output:
        print(output)
    raise RuntimeError(message)


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()[:24]
    if len(data) != 24 or data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR":
        fail(f"{path} is not a valid PNG")
    return struct.unpack(">II", data[16:24])


def check_scaled_panel(path: Path, logical_width: int, logical_height: int) -> None:
    width, height = png_size(path)
    if width % logical_width or height % logical_height:
        fail(
            f"{path} is {width}x{height}, not an integer magnification of "
            f"{logical_width}x{logical_height}"
        )
    if width // logical_width != height // logical_height:
        fail(f"{path} uses different horizontal and vertical magnification")


def control_request(path: Path, request: str, timeout: float = 5) -> list[str]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(path))
        client.sendall(request.encode() + b"\n")

        reply = b""
        while b"\n" not in reply:
            chunk = client.recv(4096)
            if not chunk:
                break
            reply += chunk

    words = reply.decode(errors="replace").split("\n", 1)[0].split()
    if not words:
        fail(f"control socket closed without answering {request!r}")
    if words[0] != "ok":
        fail(f"control request {request!r} failed: {' '.join(words[1:])}")
    return words[1:]


def check_app_registry(binary: Path) -> None:
    result = subprocess.run(
        [str(binary), "--list-apps"],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode != 0:
        fail("--list-apps failed", result.stdout + result.stderr)

    lines = [line for line in result.stdout.splitlines()[1:] if line.strip()]
    apps = [line.split()[0] for line in lines]
    missing = REQUIRED_APPS.difference(apps)
    if missing:
        fail(f"--list-apps is missing: {', '.join(sorted(missing))}")
    if len(apps) != len(set(apps)):
        fail("--list-apps contains duplicate scene names")
    if len(apps) < 20:
        fail(f"--list-apps returned only {len(apps)} entries")


def reserve_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def api_request(
    port: int,
    path: str,
    method: str = "GET",
    data: bytes | None = None,
) -> tuple[int, str]:
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=data,
        method=method,
    )
    try:
        with urllib.request.urlopen(request, timeout=2) as response:
            return response.status, response.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def wait_for_api_value(
    port: int,
    path: str,
    key: str,
    expected,
    timeout: float = 5,
):
    """Wait for an asynchronous firmware service to publish a setting."""
    deadline = time.monotonic() + timeout
    observed = None
    while time.monotonic() < deadline:
        try:
            code, body = api_request(port, path)
            if code == 200:
                observed = json.loads(body).get(key)
                if observed == expected:
                    return observed
        except (OSError, urllib.error.URLError, json.JSONDecodeError):
            pass
        time.sleep(0.05)
    return observed


def check_live_api(binary: Path, assets: Path, root: Path) -> None:
    state = root / "api-state"
    state.mkdir()
    log_path = root / "api.log"
    control_path = root / "api-control.sock"
    shots = root / "api-shots"
    shots.mkdir()
    port = reserve_port()

    with log_path.open("w+b") as log:
        process = subprocess.Popen(
            [
                str(binary),
                "--headless",
                "--scale",
                "18",
                "--scene",
                "busy",
                "--api-port",
                str(port),
                "--no-mdns",
                "--state-dir",
                str(state),
                "--control",
                str(control_path),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
        )

        try:
            deadline = time.monotonic() + 20
            status = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    code, body = api_request(port, "/api/status")
                    if code == 200:
                        status = json.loads(body)
                        break
                except (OSError, urllib.error.URLError, json.JSONDecodeError):
                    pass
                time.sleep(0.1)

            if status is None:
                fail("headless API did not become ready", log_path.read_text(errors="replace"))

            renderer = None
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    words = control_request(control_path, "status", timeout=1)
                    if len(words) == 8:
                        values = [int(value) for value in words]
                        if values[1] > 0 and values[2] > 0 and values[3] > 0 and values[4] > 0:
                            renderer = values
                            break
                except (OSError, TimeoutError):
                    pass
                time.sleep(0.05)
            if renderer is None:
                fail("control socket did not report rendered displays")

            if (
                wait_for_api_value(
                    port, "/api/display/brightness", "value", "auto"
                )
                != "auto"
            ):
                fail("fresh simulator did not start in automatic brightness mode")

            target = renderer[0] + 2
            waited = control_request(control_path, f"wait frame {target} 5000")
            if len(waited) != 1 or int(waited[0]) < target:
                fail("control frame wait returned an invalid frame")

            power = control_request(control_path, "power 12 0 0")
            if power != ["12", "0", "0"]:
                fail("control power state did not round-trip")
            code, body = api_request(port, "/api/status")
            power_status = json.loads(body).get("power", {}) if code == 200 else {}
            if (
                power_status.get("battery_charge") != 12
                or power_status.get("battery_current") != -250
                or power_status.get("usb_voltage") != 0
                or power_status.get("state") != "discharging"
            ):
                fail("device status API did not observe simulated power state")
            control_request(control_path, "power 100 1 1")

            code, _ = api_request(
                port, "/api/display/brightness?value=37", method="POST"
            )
            if code != 200:
                fail(f"brightness API returned HTTP {code}")
            if (
                wait_for_api_value(
                    port, "/api/display/brightness", "value", "37"
                )
                != "37"
            ):
                fail("brightness setting did not round-trip")

            code, _ = api_request(
                port, "/api/time/timezone?timezone=Amsterdam", method="POST"
            )
            if code != 200:
                fail(f"timezone API returned HTTP {code}")
            if (
                wait_for_api_value(
                    port, "/api/time/timezone", "name", "Amsterdam"
                )
                != "Amsterdam"
            ):
                fail("timezone setting did not round-trip")

            code, _ = api_request(port, "/api/input?key=up", method="POST")
            if code != 200:
                fail(f"input API returned HTTP {code}")

            # A real device always has /ext mounted before the web service
            # creates /ext/user_assets. A pristine overlay must expose the
            # same invariant without a developer pre-creating directories.
            external_state = state / "ext"
            user_assets = external_state / "user_assets"
            if not user_assets.is_dir():
                fail("writable /ext/user_assets was not initialized automatically")

            upload_data = b"simulator asset upload smoke test\n"
            upload_app = "simulator-smoke"
            upload_name = "fixture.anim"
            upload_query = urllib.parse.urlencode(
                {"application_name": upload_app, "file": upload_name}
            )
            code, _ = api_request(
                port,
                f"/api/assets/upload?{upload_query}",
                method="POST",
                data=upload_data,
            )
            uploaded = user_assets / upload_app / upload_name
            if code != 200 or not uploaded.is_file() or uploaded.read_bytes() != upload_data:
                fail(f"asset upload failed on pristine state with HTTP {code}")
            if (assets / "ext" / "user_assets" / upload_app / upload_name).exists():
                fail("asset upload mutated the immutable base")

            base_font = (
                assets
                / "ext"
                / "apps_assets"
                / "shared"
                / "fonts"
                / "busy_regular_7.font"
            )
            if not base_font.is_file():
                fail(f"expected generated font is missing: {base_font}")
            query = urllib.parse.urlencode(
                {"path": "/ext/apps_assets/shared/fonts/busy_regular_7.font"}
            )
            code, _ = api_request(port, f"/api/storage/remove?{query}", method="DELETE")
            if code < 400 or not base_font.is_file():
                fail("storage API was able to remove an immutable base asset")

            victim = root / "outside-state"
            victim.mkdir()
            sentinel = victim / "sentinel"
            sentinel.touch()
            os.symlink(victim, external_state / "escape")

            query = urllib.parse.urlencode({"path": "/ext/escape"})
            code, _ = api_request(port, f"/api/storage/remove?{query}", method="DELETE")
            if code != 200 or not sentinel.is_file() or (external_state / "escape").exists():
                fail("recursive storage removal followed or failed to unlink a directory symlink")

            before_capture = control_request(control_path, "status")
            if len(before_capture) != 8:
                fail("control status was malformed before screenshot")
            heap_low_water_before = int(before_capture[7])

            captured = control_request(control_path, f"screenshot {shots}", timeout=15)
            if len(captured) != 2:
                fail("control screenshot returned malformed metadata")
            sequence = int(captured[0])
            suffix = f"{sequence:03d}"
            expected_sizes = {
                "front-raw": (72, 16),
                "back-raw": (160, 80),
                "front": (576, 128),
                "back": (640, 320),
            }
            for name, expected in expected_sizes.items():
                path = shots / f"{name}-{suffix}.png"
                if png_size(path) != expected:
                    fail(f"{path} has the wrong dimensions")
            png_size(shots / f"window-{suffix}.png")

            after_capture = control_request(control_path, "status")
            if len(after_capture) != 8:
                fail("control status was malformed after screenshot")
            heap_low_water_after = int(after_capture[7])
            if heap_low_water_before - heap_low_water_after > 1024 * 1024:
                fail("simulator-only screenshot workspace consumed the furi heap")

            control_request(control_path, "quit")
        finally:
            if process.poll() is None:
                try:
                    control_request(control_path, "quit", timeout=1)
                except (OSError, TimeoutError, RuntimeError):
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    output = log_path.read_text(errors="replace")
    if process.returncode not in (-15, 0):
        fail(f"headless API simulator exited with {process.returncode}", output)
    if "[E]" in output:
        fail("headless API simulator emitted an error log", output)

    # The settings endpoints above exercise the real firmware codecs. Restart
    # against the same overlay to prove that the simulator did not merely keep
    # them in process-local state.
    persistence_log_path = root / "persistence.log"
    persistence_control_path = root / "persistence-control.sock"
    persistence_port = reserve_port()
    with persistence_log_path.open("w+b") as persistence_log:
        persistence_process = subprocess.Popen(
            [
                str(binary),
                "--headless",
                "--scene",
                "busy",
                "--api-port",
                str(persistence_port),
                "--no-mdns",
                "--state-dir",
                str(state),
                "--control",
                str(persistence_control_path),
            ],
            stdout=persistence_log,
            stderr=subprocess.STDOUT,
        )

        try:
            deadline = time.monotonic() + 20
            persisted_brightness = None
            persisted_timezone = None
            while time.monotonic() < deadline:
                if persistence_process.poll() is not None:
                    break
                try:
                    brightness_code, brightness_body = api_request(
                        persistence_port, "/api/display/brightness"
                    )
                    timezone_code, timezone_body = api_request(
                        persistence_port, "/api/time/timezone"
                    )
                    if brightness_code == 200 and timezone_code == 200:
                        persisted_brightness = json.loads(brightness_body).get("value")
                        persisted_timezone = json.loads(timezone_body).get("name")
                        if (
                            persisted_brightness == "37"
                            and persisted_timezone == "Amsterdam"
                        ):
                            break
                except (OSError, urllib.error.URLError, json.JSONDecodeError):
                    pass
                time.sleep(0.1)

            if persisted_brightness != "37" or persisted_timezone != "Amsterdam":
                fail(
                    "brightness/timezone settings did not survive a restart",
                    persistence_log_path.read_text(errors="replace"),
                )
            control_request(persistence_control_path, "quit")
        finally:
            if persistence_process.poll() is None:
                try:
                    control_request(persistence_control_path, "quit", timeout=1)
                except (OSError, TimeoutError, RuntimeError):
                    persistence_process.terminate()
                try:
                    persistence_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    persistence_process.kill()
                    persistence_process.wait()

    persistence_output = persistence_log_path.read_text(errors="replace")
    if persistence_process.returncode not in (-15, 0):
        fail(
            f"persistence simulator exited with {persistence_process.returncode}",
            persistence_output,
        )
    if "[E]" in persistence_output:
        fail("persistence simulator emitted an error log", persistence_output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()

    binary = args.binary.resolve()
    assets = args.assets.resolve()
    if not binary.is_file():
        fail(f"simulator binary is missing: {binary}")
    if not assets.is_dir():
        fail(f"simulator asset root is missing: {assets}")

    check_app_registry(binary)

    source_state = assets / "data"
    if source_state.exists():
        fail(f"generated assets already contain mutable state: {source_state}")
    symlinks = [path for path in assets.rglob("*") if path.is_symlink()]
    if symlinks:
        fail(f"generated assets contain symlinks: {symlinks[0]}")

    with tempfile.TemporaryDirectory(prefix="busybar-sim-smoke-") as temporary:
        root = Path(temporary)
        shots = root / "shots"
        state = root / "state"
        shots.mkdir()
        state.mkdir()

        command = [
            str(binary),
            "--headless",
            "--scene",
            "busy",
            "--frames",
            "120",
            "--screenshot",
            str(shots),
            "--api-port",
            "0",
            "--no-mdns",
            "--state-dir",
            str(state),
        ]
        try:
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=45,
            )
        except subprocess.TimeoutExpired as error:
            partial_parts = []
            for value in (error.stdout, error.stderr):
                if isinstance(value, bytes):
                    value = value.decode(errors="replace")
                if value:
                    partial_parts.append(value)
            partial = "".join(partial_parts)
            fail("headless simulator timed out", partial)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            fail(f"headless simulator exited with {result.returncode}", output)
        if "[E]" in output:
            fail("headless simulator emitted an error log", output)
        if "control selftest: pass" not in output:
            fail("control hitbox self-test did not pass", output)
        if "Light sensor brightness: 10" not in output:
            fail("simulated max-light event did not reach brightness control", output)

        if png_size(shots / "front-raw.png") != (72, 16):
            fail("raw front capture does not match the physical panel")
        if png_size(shots / "back-raw.png") != (160, 80):
            fail("raw back capture does not match the physical panel")
        if png_size(shots / "front.png") != (576, 128):
            fail("front inspection capture is not the fixed 8x magnification")
        if png_size(shots / "back.png") != (640, 320):
            fail("back inspection capture is not the fixed 4x magnification")
        png_size(shots / "window.png")

        if not (state / "data" / "done.txt").is_file():
            fail("firmware state was not written into the writable overlay", output)
        if source_state.exists():
            fail("simulator wrote mutable state into generated assets", output)

        check_live_api(binary, assets, root)

    print("simulator smoke test: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
