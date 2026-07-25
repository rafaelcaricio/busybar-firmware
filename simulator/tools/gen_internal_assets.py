#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pypng", "pillow", "lz4"]
# ///
"""Generate assets_images.h and the internal image descriptors for the simulator.

Mirrors what lib/internal_assets.scons does under fbt: every PNG in
assets/images/internal becomes an LVGL C descriptor named I_<stem>, and the
header declares them all. Reimplemented here so the simulator builds with
plain CMake instead of requiring the firmware toolchain environment.
"""

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "lib" / "lvgl" / "scripts"))

from LVGLImage import ColorFormat, LVGLImage  # noqa: E402

# fbt lets LVGLImage pick an indexed format from the palette size, which shells
# out to pngquant. ARGB8888 reaches the same pixels without that dependency;
# the simulator has no flash budget to answer to.
COLOR_FORMAT = ColorFormat.ARGB8888


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--images", type=Path, required=True, help="PNG source directory")
    parser.add_argument("--out", type=Path, required=True, help="Generated output directory")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    names = []
    for png in sorted(args.images.glob("*.png")):
        name = f"I_{png.stem}"
        target = args.out / f"{name}.c"

        image = LVGLImage().from_png(str(png), cf=COLOR_FORMAT)
        image.adjust_stride(align=1)
        image.to_c_array(str(target))

        names.append(name)

    header = args.out / "assets_images.h"
    with header.open("w") as out:
        out.write("#pragma once\n\n")
        out.write("#include <lvgl.h>\n\n")
        for name in names:
            out.write(f"extern const lv_image_dsc_t {name};\n")

    print(f"generated {len(names)} internal images into {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
