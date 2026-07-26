#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pypng", "pillow", "lz4", "colorlog"]
# ///
"""Build the simulator's asset root: fonts, .image and .anim files.

fbt converts PNGs to LVGL binary images and animation zips to the firmware's
.anim container, then lays them out on the device under /ext/apps_assets. The
simulator serves the same layout from a directory, so the same conversions
have to happen here.

Layout produced, mirroring the device:

    <root>/ext/apps_assets/shared/fonts       -> symlink to assets/shared/fonts
    <root>/ext/apps_assets/shared/images      <- assets/shared/images/external
    <root>/ext/apps_assets/shared/animations  <- assets/shared/animations
    <root>/ext/apps_assets/<app>/images       <- assets/images/external/<app>
    <root>/ext/apps_assets/<app>/animations   <- assets/animations/<app>
    <root>/ext/apps_assets/shared/sounds      <- assets/shared/sounds
    <root>/ext/apps_assets/<app>/sounds       <- assets/sounds/<app>
    <root>/ext                                <- applications/*/*/resources

Conversions are skipped when the output is newer than its source, so
reconfiguring does not redo the whole tree.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "lib" / "lvgl" / "scripts"))

from LVGLImage import ColorFormat, LVGLImage  # noqa: E402

# fbt lets LVGLImage pick an indexed format from the palette, which shells out
# to pngquant. ARGB8888 needs no external tool and the LVGL binary decoder
# reads the format from the header either way.
COLOR_FORMAT = ColorFormat.ARGB8888


MANIFEST_NAME = ".simulator-generated-files"

# Runtime data written by simulator versions that predate the state overlay.
# None of these paths is a generated input; leaving one in the asset root would
# make supposedly immutable state visible to every future simulator run.
LEGACY_MUTABLE_PATHS = (
    "data",
    "bkp",
    "assets",
    "ext/user_assets",
    "ext/apps_data",
    "ext/update",
)


def convert_sounds(
    source_dir: Path, target_dir: Path, expected: set[Path]
) -> tuple[int, int]:
    """Convert .wav sources to the firmware's .snd, which the simulator plays.

    A .snd is headerless PCM: signed 16-bit little-endian, mono, 44100 Hz. See
    scripts/audio.py, which is what fbt runs.

    That script also normalises loudness and applies an EQ curve shaped for the
    device's speaker. Neither is reproduced here: the sound comes out of
    workstation speakers, where the compensation for a small piezo would be
    wrong, and skipping it keeps the conversion to one ffmpeg call.
    """
    if not source_dir.is_dir():
        return 0, 0

    converted = 0
    failures = 0

    for source in sorted(source_dir.glob("*.wav")):
        target = target_dir / (source.stem + ".snd")
        expected.add(target)
        if not is_stale(source, target):
            continue

        target_dir.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            ["ffmpeg", "-y", "-i", str(source), "-ac", "1",
             "-acodec", "pcm_s16le", "-f", "s16le", "-ar", "44100", str(target)],
            capture_output=True,
        )

        if result.returncode != 0:
            print(f"{source.name}: ffmpeg failed: {result.stderr.decode()[-200:]}",
                  file=sys.stderr)
            failures += 1
        else:
            converted += 1

    return converted, failures


def is_stale(source: Path, target: Path) -> bool:
    return not target.exists() or target.stat().st_mtime < source.stat().st_mtime


def convert_images(source_dir: Path, target_dir: Path, expected: set[Path]) -> int:
    if not source_dir.is_dir():
        return 0

    target_dir.mkdir(parents=True, exist_ok=True)
    converted = 0

    for png in sorted(source_dir.glob("*.png")):
        target = target_dir / f"{png.stem}.image"
        expected.add(target)
        if not is_stale(png, target):
            continue

        image = LVGLImage().from_png(str(png), cf=COLOR_FORMAT)
        image.adjust_stride(align=1)
        # to_bin insists on a .bin suffix, so write and rename as image.py does.
        staging = target.with_suffix(".bin")
        image.to_bin(str(staging))
        staging.replace(target)
        converted += 1

    return converted


def convert_animations(
    source_dir: Path, target_dir: Path, expected: set[Path]
) -> tuple[int, int]:
    if not source_dir.is_dir():
        return 0, 0

    target_dir.mkdir(parents=True, exist_ok=True)
    converted = 0
    failed = 0

    for archive in sorted(source_dir.glob("*.zip")):
        target = target_dir / f"{archive.stem}.anim"
        expected.add(target)
        if not is_stale(archive, target):
            continue

        result = subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "seq2anim.py"),
             "-o", str(target), str(archive)],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            converted += 1
        else:
            failed += 1
            print(f"  anim {archive.name}: {result.stderr.strip().splitlines()[-1:]}",
                  file=sys.stderr)

    return converted, failed


def copy_tree(source_dir: Path, target_dir: Path, expected: set[Path]) -> int:
    """Copy a tree without symlinking the build back into the source checkout."""
    if not source_dir.is_dir():
        return 0

    copied = 0
    for source in sorted(path for path in source_dir.rglob("*") if path.is_file()):
        target = target_dir / source.relative_to(source_dir)
        expected.add(target)
        if not is_stale(source, target):
            continue

        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied += 1

    return copied


def copy_app_resources(root: Path, expected: set[Path]) -> int:
    """Install every applications/*/resources tree, which ships as it is.

    These are files an app reads at runtime but that no converter produces —
    the busy app's themes, matter's certificates, the web server's 404 page.
    Each `resources` directory mirrors the device's `/ext` below it, so
    resources/apps_assets/busy/themes/on_air/theme.json is exactly
    /ext/apps_assets/busy/themes/on_air/theme.json.
    """
    ext = root / "ext"
    copied = 0

    for resources in sorted(REPO_ROOT.glob("applications/*/*/resources")):
        for source in sorted(p for p in resources.rglob("*") if p.is_file()):
            target = ext / source.relative_to(resources)
            expected.add(target)
            if not is_stale(source, target):
                continue

            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            copied += 1

    return copied


def prepare_output_root(root: Path) -> None:
    removed = []
    for relative in LEGACY_MUTABLE_PATHS:
        target = root / relative
        if not target.exists() and not target.is_symlink():
            continue

        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        else:
            target.unlink()
        removed.append(relative)

    if removed:
        print(f"removed legacy mutable asset state: {', '.join(removed)}")

    manifest = root / MANIFEST_NAME
    if manifest.exists():
        return

    # Older simulator builds did not track their outputs and linked the fonts
    # directory back into the checkout. Recreate only the generated /ext tree;
    # shutil removes directory symlinks instead of following them, so this
    # cannot modify the source asset directory.
    ext = root / "ext"
    if ext.exists() or ext.is_symlink():
        if ext.is_symlink():
            ext.unlink()
        else:
            shutil.rmtree(ext)


def remove_stale_outputs(root: Path, expected: set[Path]) -> None:
    manifest = root / MANIFEST_NAME
    if not manifest.exists():
        return

    for line in manifest.read_text().splitlines():
        relative = Path(line)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe path in {manifest}: {line}")

        target = root / relative
        if target in expected:
            continue
        if target.is_file() or target.is_symlink():
            target.unlink()


def write_manifest(root: Path, expected: set[Path]) -> None:
    manifest = root / MANIFEST_NAME
    text = "".join(f"{path.relative_to(root)}\n" for path in sorted(expected))
    if not manifest.exists() or manifest.read_text() != text:
        manifest.write_text(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True, help="Asset root to populate")
    args = parser.parse_args()

    args.root.mkdir(parents=True, exist_ok=True)
    expected: set[Path] = set()
    prepare_output_root(args.root)

    apps_assets = args.root / "ext" / "apps_assets"
    apps_assets.mkdir(parents=True, exist_ok=True)

    shared = apps_assets / "shared"
    shared.mkdir(parents=True, exist_ok=True)

    # Fonts ship in their final form. Copy them so storage operations against a
    # simulator build can never traverse a symlink into the source checkout.
    fonts = copy_tree(
        REPO_ROOT / "assets" / "shared" / "fonts", shared / "fonts", expected
    )

    images = convert_images(REPO_ROOT / "assets" / "shared" / "images" / "external",
                            shared / "images", expected)

    app_images = 0
    app_root = REPO_ROOT / "assets" / "images" / "external"
    if app_root.is_dir():
        for app_dir in sorted(p for p in app_root.iterdir() if p.is_dir()):
            app_images += convert_images(
                app_dir, apps_assets / app_dir.name / "images", expected
            )

    anims, anim_failures = convert_animations(
        REPO_ROOT / "assets" / "shared" / "animations",
        shared / "animations",
        expected,
    )

    anim_root = REPO_ROOT / "assets" / "animations"
    if anim_root.is_dir():
        for app_dir in sorted(p for p in anim_root.iterdir() if p.is_dir()):
            converted, failures = convert_animations(
                app_dir, apps_assets / app_dir.name / "animations", expected
            )
            anims += converted
            anim_failures += failures

    sounds, sound_failures = convert_sounds(
        REPO_ROOT / "assets" / "shared" / "sounds", shared / "sounds", expected
    )

    sound_root = REPO_ROOT / "assets" / "sounds"
    if sound_root.is_dir():
        for app_dir in sorted(p for p in sound_root.iterdir() if p.is_dir()):
            converted, failures = convert_sounds(
                app_dir, apps_assets / app_dir.name / "sounds", expected
            )
            sounds += converted
            sound_failures += failures

    anim_failures += sound_failures

    resources = copy_app_resources(args.root, expected)

    if anim_failures:
        return 1

    remove_stale_outputs(args.root, expected)
    write_manifest(args.root, expected)

    print(f"assets: {fonts} fonts, {images} shared images, {app_images} app images, "
          f"{anims} animations, {sounds} sounds, {resources} app resources"
          + (f", {anim_failures} conversion failures" if anim_failures else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
