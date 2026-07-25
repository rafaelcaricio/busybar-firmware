#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["busylib", "pillow"]
# ///
"""A Busy Bar app, written against the simulator.

Nothing here is simulator-specific. Change ADDRESS to a device's IP and the
same script drives real hardware: the simulator serves the firmware's own HTTP
API, so busylib cannot tell the two apart.

    ./simulator/build/busybar-sim --scene clock &
    ./simulator/examples/hello_busy.py

To develop against the local busylib checkout instead of the published wheel:

    uv run --with /path/to/busylib-py simulator/examples/hello_busy.py
"""

import base64
import sys
import time
from pathlib import Path

from busylib import BusyBar, types

ADDRESS = "127.0.0.1:8042"
APP_NAME = "hello_busy"

# The front panel: 72x16 RGB LEDs.
FRONT_W, FRONT_H = 72, 16


def draw(bb: BusyBar) -> None:
    """Put a line of text and a filled bar on the front panel."""
    bb.display_draw(
        types.DisplayElements(
            application_name=APP_NAME,
            elements=[
                types.TextElement(
                    id="greeting",
                    type="text",
                    x=1,
                    y=4,
                    text="HELLO",
                    font="bold",
                    color="#33ccffff",
                    display=types.DisplayName.FRONT,
                ),
                types.RectangleElement(
                    id="bar",
                    type="rectangle",
                    x=44,
                    y=5,
                    width=26,
                    height=6,
                    # Rectangles are unfilled by default and take fill_colors,
                    # not color. Both are easy to miss: a rectangle given
                    # `color` and no `fill` draws as a white outline.
                    fill="solid",
                    fill_colors=["#ff6600ff"],
                    border_width=0,
                    display=types.DisplayName.FRONT,
                ),
            ],
        )
    )


def save_screenshot(bb: BusyBar, path: Path) -> None:
    """Read the front panel back and write it out as a PNG.

    Two details the API docs are quiet about, and the device behaves the same
    way: /api/screen replies base64-encoded, and the frame is BGR, because
    LVGL's "RGB888" is byte-order blue-green-red.
    """
    from PIL import Image

    frame = base64.b64decode(bb.screen(0))
    rgb = bytes(
        channel
        for i in range(0, len(frame), 3)
        for channel in (frame[i + 2], frame[i + 1], frame[i])
    )

    image = Image.frombytes("RGB", (FRONT_W, FRONT_H), rgb)
    image.resize((FRONT_W * 10, FRONT_H * 10), Image.NEAREST).save(path)


def main() -> int:
    with BusyBar(ADDRESS) as bb:
        print("api", bb.version().api_semver, "on", bb.name().name)

        # Assets live under /ext/user_assets/<application_name>/ on the device;
        # the simulator serves them from build/assets_root.
        bb.assets_upload(
            application_name=APP_NAME,
            filename="notes.txt",
            data=b"written by hello_busy\n",
        )
        listing = bb.storage_list(path=f"/ext/user_assets/{APP_NAME}")
        print("assets", [item.name for item in listing.list])

        draw(bb)
        time.sleep(1)

        save_screenshot(bb, Path("hello_busy.png"))
        print("wrote hello_busy.png")

        # Scroll the app underneath. The key names here are the device's own,
        # not the simulator's screen-relative ones.
        bb.input(key=types.InputKey.DOWN)

        time.sleep(2)
        bb.display_clear()

    return 0


if __name__ == "__main__":
    sys.exit(main())
