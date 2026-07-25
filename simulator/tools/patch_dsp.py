#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Replace dsp.c's Thumb-2 convolution helper with portable C.

dsp_2d_kernel_iteration() is written in ARMv7-M DSP assembly (uxtb, smlabb,
bfi). Everything else in dsp.c is portable, so only that one static helper is
swapped out; the surrounding convolution logic stays shared with the firmware.

Semantics of the original, for the record:

  uxtb chan, px, ror #N     extract byte N of the packed BGRA pixel
  smlabb acc, chan, krnl    acc += (int16)chan * (int16)krnl
  bfi ... / lsr ab, ab, #8  repack, taking bits 15:8 of each accumulator,
                            i.e. each channel divided by the kernel's 256
                            fixed-point scale
"""

import argparse
import sys
from pathlib import Path

START = "static uint32_t dsp_2d_kernel_iteration("
END = "void dsp_2d_kernel_apply("

REPLACEMENT = """/* Host build: portable replacement for the Thumb-2 original.
 * See simulator/tools/patch_dsp.py for the instruction-level semantics. */
static uint32_t dsp_2d_kernel_iteration(
    const uint32_t* source,
    size_t stride,
    size_t kernel_sz,
    const int32_t* kernel) {
    uint32_t ab = 0, ag = 0, ar = 0, aa = 0;

    for(size_t ky = 0; ky < kernel_sz; ky++) {
        for(size_t kx = 0; kx < kernel_sz; kx++) {
            const uint32_t px = *(source++);
            /* smlabb multiplies the bottom halfwords as signed. */
            const int32_t krnl = (int16_t)(*(kernel++));

            ab += (uint32_t)((int32_t)((px >> 0) & 0xFF) * krnl);
            ag += (uint32_t)((int32_t)((px >> 8) & 0xFF) * krnl);
            ar += (uint32_t)((int32_t)((px >> 16) & 0xFF) * krnl);
            aa += (uint32_t)((int32_t)((px >> 24) & 0xFF) * krnl);
        }

        source += stride;
    }

    return (((aa >> 8) & 0xFF) << 24) | (((ar >> 8) & 0xFF) << 16) |
           (((ag >> 8) & 0xFF) << 8) | ((ab >> 8) & 0xFF);
}

"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()

    start = source.find(START)
    end = source.find(END)

    if start < 0 or end < 0 or end <= start:
        print(
            f"{args.input}: could not locate dsp_2d_kernel_iteration; "
            "update simulator/tools/patch_dsp.py",
            file=sys.stderr,
        )
        return 1

    helper = source[start:end]
    if "smlabb" not in helper or "uxtb" not in helper:
        print(
            f"{args.input}: dsp_2d_kernel_iteration no longer looks like the "
            "assembly version this patch was written against; re-check the "
            "semantics before updating simulator/tools/patch_dsp.py",
            file=sys.stderr,
        )
        return 1

    patched = source[:start] + REPLACEMENT + source[end:]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patched)

    # dsp.c includes "dsp.h" relatively. Copy the header next to the patched
    # copy rather than putting lib/toolbox on the include path, where its
    # timers.h would shadow FreeRTOS's.
    (args.output.parent / "dsp.h").write_text((args.input.parent / "dsp.h").read_text())

    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
