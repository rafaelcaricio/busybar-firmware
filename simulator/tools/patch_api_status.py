#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Make the device status API's integer formatting portable to LP64 hosts.

The firmware target uses a 32-bit ``long``. macOS and 64-bit Linux use LP64,
where ``long`` is 64 bits, so passing a uint32_t/int32_t to ``%lu``/``%ld`` is
undefined behavior. In particular, negative battery current can be serialized
as a large positive integer. Keep the firmware source untouched and compile a
host-only copy using the fixed-width PRI macros.
"""

import argparse
import sys
from pathlib import Path


REPLACEMENTS = {
    '#include "http_api.h"\n': '#include "http_api.h"\n#include <inttypes.h>\n',
    'json_str, ",\\"otp_timestamp\\":%lu", furi_hal_version_get_hw_timestamp());': (
        'json_str, ",\\"otp_timestamp\\":%" PRIu32, furi_hal_version_get_hw_timestamp());'
    ),
    '"\\"uptime\\":\\"%02lud %02luh %02lum %02lus\\","': (
        '"\\"uptime\\":\\"%02" PRIu32 "d %02" PRIu32 "h %02" PRIu32 '
        '"m %02" PRIu32 "s\\","'
    ),
    'furi_string_cat_printf(json_str, "\\"boot_time\\":%lld,", boot_timestamp);': (
        'furi_string_cat_printf('
        'json_str, "\\"boot_time\\":%" PRId64 ",", (int64_t)boot_timestamp);'
    ),
    'json_str, "\\"%s\\":%lu,", "battery_voltage", (uint32_t)info.voltage_battery);': (
        'json_str, "\\"%s\\":%" PRIu32 ",", '
        '"battery_voltage", (uint32_t)info.voltage_battery);'
    ),
    'furi_string_cat_printf(json_str, "\\"%s\\":%ld,", "battery_current", info.current_battery);': (
        'furi_string_cat_printf('
        'json_str, "\\"%s\\":%" PRId32 ",", "battery_current", info.current_battery);'
    ),
    'furi_string_cat_printf(json_str, "\\"%s\\":%lu", "usb_voltage", (uint32_t)info.voltage_usb);': (
        'furi_string_cat_printf('
        'json_str, "\\"%s\\":%" PRIu32, "usb_voltage", (uint32_t)info.voltage_usb);'
    ),
}


def fail(message: str) -> int:
    print(f"{message}; update simulator/tools/patch_api_status.py", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()
    for original in REPLACEMENTS:
        if source.count(original) != 1:
            return fail(f"{args.input}: expected exactly one occurrence of {original!r}")

    patched = source
    for original, replacement in REPLACEMENTS.items():
        patched = patched.replace(original, replacement)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patched)
    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
