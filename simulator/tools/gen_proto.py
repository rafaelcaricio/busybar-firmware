#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["grpcio-tools", "protobuf"]
# ///
"""Generate the nanopb sources the state stream is encoded with.

/api/status/ws speaks protobuf, and the message definitions live in the
assets/proto submodule. fbt turns them into C with nanopb's generator (see
lib/proto.scons and scripts/fbt_tools/fbt_assets.py); this runs the same
generator with the same arguments, so the simulator serialises byte for byte
what the device does.

The generator needs protoc, which arrives here as a Python wheel rather than a
system package, so the simulator still builds without a firmware toolchain.
"""

import argparse
import runpy
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "lib" / "nanopb" / "generator" / "nanopb_generator.py"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--proto", type=Path, required=True, help="assets/proto")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    sources = sorted(args.proto.rglob("*.proto"))
    if not sources:
        print(
            f"no .proto files under {args.proto}\n"
            "    git submodule update --init assets/proto",
            file=sys.stderr,
        )
        return 1

    if not GENERATOR.is_file():
        print(
            f"missing {GENERATOR}\n    git submodule update --init lib/nanopb",
            file=sys.stderr,
        )
        return 1

    args.out.mkdir(parents=True, exist_ok=True)

    # The generator is a script, not a library, so it is run as one: in-process
    # so it inherits this script's dependencies, with its own directory on the
    # path because it imports the `proto` package sitting next to it.
    sys.path.insert(0, str(GENERATOR.parent))
    sys.argv = [
        str(GENERATOR),
        "-q",
        "--strip-path",
        f"-I{args.proto}",
        f"-D{args.out}",
        *(str(source) for source in sources),
    ]
    runpy.run_path(str(GENERATOR), run_name="__main__")

    generated = sorted(args.out.rglob("*.pb.c"))
    print(f"proto: {len(generated)} generated from {len(sources)} definitions")

    return 0


if __name__ == "__main__":
    sys.exit(main())
