#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Give simulator screenshot encoding a host-only lodepng allocation scope.

LVGL normally (and correctly) routes lodepng through lv_malloc, which reaches
the furi heap. A whole-window screenshot is simulator tooling and can need
several MiB of temporary codec storage. The patched copy keeps LVGL decoding on
furi by default but lets the screenshot task opt into mmap-backed allocation
through a thread-local scope.
"""

import argparse
import sys
from pathlib import Path


REPLACEMENTS = {
    '#include "lodepng.h"': (
        "#include <libs/lodepng/lodepng.h>\n"
        "#include <sim_host_alloc.h>"
    ),
    '#include "../../core/lv_global.h"': "#include <core/lv_global.h>",
    "    return lv_malloc(size);": (
        "    return sim_host_allocator_active() ? sim_host_alloc(size) : lv_malloc(size);"
    ),
    "    return lv_realloc(ptr, new_size);": (
        "    return sim_host_allocator_active() ? "
        "sim_host_realloc(ptr, new_size) : lv_realloc(ptr, new_size);"
    ),
    "    lv_free(ptr);": (
        "    if(sim_host_allocator_active()) {\n"
        "        sim_host_free(ptr);\n"
        "    } else {\n"
        "        lv_free(ptr);\n"
        "    }"
    ),
}


def fail(message: str) -> int:
    print(f"{message}; update simulator/tools/patch_lodepng_allocator.py", file=sys.stderr)
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
