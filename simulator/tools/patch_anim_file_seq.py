#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Stop a looping single-frame animation reading past the end of its file.

Unlike the other patch_*.py scripts, this one does not exist for portability:
the code compiles fine on the host. It works around a firmware bug, so that the
simulator's log is readable.

anim_file_seq_load_current_frame() applies the normal "advance to the next file
frame" step even when anim_file_start_last_frame() has just re-seeded the
sequence for a new loop. The advance then adds the previous frame's size to an
offset that was already reset to the start of the section. With more than one
file frame the result still lands inside the file; with exactly one it lands on
EOF, the header read comes back short, and the frame errors out.

/ext/apps_assets/busy/animations/progress_busy_41x16.anim is one such
animation -- a single frame at 1 fps -- and timer_indicator plays it looping,
so a running Busy session logs "Load error" once a second, forever.

THE DEVICE STILL HAS THIS BUG. Only the host build compiles the patched copy.
Drop this script and its CMake step once lib/anim_file carries the fix.

The original is left untouched -- the patched copy goes to the build directory.
If the source drifts away from the snippet below, this fails loudly rather than
silently emitting something stale.
"""

import argparse
import sys
from pathlib import Path

ORIGINAL = """        }

        seq->disp_frame_idx++;
        if(--seq->remaining_duration > 0) return AnimFileFrameFlagNoChange;

        seq->requested_file_frame += sizeof(*frame_hdr) + frame_hdr->encoded_length;
    }
"""

REPLACEMENT = """        } else {
            /* Host build: see simulator/tools/patch_anim_file_seq.py.
               Only advance when the last-frame handler did not already re-seed
               the sequence, which it does when looping or switching section. */
            seq->disp_frame_idx++;
            if(--seq->remaining_duration > 0) return AnimFileFrameFlagNoChange;

            seq->requested_file_frame += sizeof(*frame_hdr) + frame_hdr->encoded_length;
        }
    }
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()

    if source.count(ORIGINAL) != 1:
        print(
            f"{args.input} no longer contains the expected frame-advance block exactly "
            "once; update simulator/tools/patch_anim_file_seq.py",
            file=sys.stderr,
        )
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source.replace(ORIGINAL, REPLACEMENT))

    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
