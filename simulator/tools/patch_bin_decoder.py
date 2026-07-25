#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Retarget LVGL's binary image decoder at the firmware's .image extension.

lib/lvgl.scons rewrites lv_bin_decoder.c on the way into the firmware build,
swapping the "bin" extension it registers for "image" and rebasing its
relative includes. Without the same treatment the decoder never claims the
files the UI asks for, and every icon fails with "failed to get image info".

The substitutions match lib/lvgl.scons exactly; keep them in step.
"""

import argparse
import sys
from pathlib import Path

SUBSTITUTIONS = {
    '"bin"': '"image"',
    "../..": "src",
    '"lv_bin_decoder.h"': '"src/libs/bin_decoder/lv_bin_decoder.h"',
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()

    if '"bin"' not in source:
        print(
            f"{args.input}: no \"bin\" extension literal to replace; "
            "re-check against lib/lvgl.scons",
            file=sys.stderr,
        )
        return 1

    for original, replacement in SUBSTITUTIONS.items():
        source = source.replace(original, replacement)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source)

    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
