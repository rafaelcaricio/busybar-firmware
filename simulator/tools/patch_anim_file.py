#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Rewrite anim_file.c's one nested function so clang can compile it.

anim_file_set_section() uses a GCC nested function to capture `section` from
the enclosing scope. arm-none-eabi-gcc accepts that; clang has never supported
it, so the host build needs the same logic expressed with an explicit context
struct.

The original is left untouched — the patched copy goes to the build directory.
If the source drifts away from the snippet below, this fails loudly rather than
silently emitting something stale.
"""

import argparse
import sys
from pathlib import Path

ORIGINAL = """    const AnimFileSection* section = NULL;

    void callback(size_t cur_index, const AnimFileSection* cur_section, void* context) {
        UNUSED(cur_index);
        UNUSED(context);
        if(strcmp(cur_section->name, name) == 0) section = cur_section;
    }

    if(!anim_file_load_iterate_sections(header, sections, callback, &section)) return false;
"""

REPLACEMENT = """    AnimFileSectionSearch search = {.name = name, .section = NULL};

    if(!anim_file_load_iterate_sections(
           header, sections, anim_file_section_search_callback, &search))
        return false;

    const AnimFileSection* section = search.section;
"""

ANCHOR = """bool FURI_WARN_UNUSED
    anim_file_set_section("""

PREAMBLE = """/* Host build: replaces a GCC nested function, see simulator/tools/patch_anim_file.py */
typedef struct {
    const char* name;
    const AnimFileSection* section;
} AnimFileSectionSearch;

static void anim_file_section_search_callback(
    size_t cur_index,
    const AnimFileSection* cur_section,
    void* context) {
    UNUSED(cur_index);
    AnimFileSectionSearch* search = context;
    if(strcmp(cur_section->name, search->name) == 0) search->section = cur_section;
}

"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()

    if ORIGINAL not in source:
        print(
            f"{args.input} no longer contains the expected nested-function block; "
            "update simulator/tools/patch_anim_file.py",
            file=sys.stderr,
        )
        return 1

    if ANCHOR not in source:
        print(f"{args.input}: anchor for the helper insertion not found", file=sys.stderr)
        return 1

    patched = source.replace(ORIGINAL, REPLACEMENT).replace(ANCHOR, PREAMBLE + ANCHOR, 1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patched)

    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
