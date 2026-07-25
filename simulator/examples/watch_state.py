#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["busylib"]
# ///
"""Watch the device's state stream, and find the device to watch.

Nothing here is simulator-specific. The simulator announces itself over mDNS
the way a device does and serves the same protobuf stream, so this script finds
whichever is on the network and follows it.

    ./simulator/build/busybar-sim &
    ./simulator/examples/watch_state.py

Move the mode lever or press a button in the window and the events show up
here. A simulator is named "BUSY Simulator" and advertises simulator=1, so it
is never mistaken for hardware.
"""

import asyncio
import sys

from busylib import AsyncBusyBar

try:
    from busylib.devices import BusyBarDevices
except ImportError:
    # Discovery is not in every busylib: the published wheel has no
    # busylib.devices, so run this against a checkout that does, or let it
    # fall through to localhost below.
    BusyBarDevices = None

API_PORT = 8042

# Screen frames arrive ten times a second and would drown everything else.
NOISY = {"frame"}


def find_device() -> str:
    """Return an address to connect to, preferring a discovered device."""
    if BusyBarDevices is None:
        print("this busylib cannot discover; using localhost")
        return f"127.0.0.1:{API_PORT}"

    for device in BusyBarDevices.discover():
        for address in sorted(device.addresses, key=lambda a: a.ip_address):
            print(f"found {device.name!r} at {address.ip_address}")
            return f"{address.ip_address}:{API_PORT}"

    print("nothing announced itself; falling back to localhost")
    return f"127.0.0.1:{API_PORT}"


async def watch(address: str) -> None:
    async with AsyncBusyBar(address) as bb:
        async for message in bb.stream_status_ws():
            if isinstance(message, str):
                continue

            for update in message.get("updates", []):
                for kind, value in update.items():
                    if kind in NOISY:
                        continue
                    print(f"{kind}: {value}")


def main() -> int:
    try:
        asyncio.run(watch(find_device()))
    except KeyboardInterrupt:
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
