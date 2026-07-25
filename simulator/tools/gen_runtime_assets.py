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

Conversions are skipped when the output is newer than its source, so
reconfiguring does not redo the whole tree.
"""

import argparse
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


def convert_sounds(source_dir: Path, target_dir: Path) -> tuple[int, int]:
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


def convert_images(source_dir: Path, target_dir: Path) -> int:
    if not source_dir.is_dir():
        return 0

    target_dir.mkdir(parents=True, exist_ok=True)
    converted = 0

    for png in sorted(source_dir.glob("*.png")):
        target = target_dir / f"{png.stem}.image"
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


def convert_animations(source_dir: Path, target_dir: Path) -> tuple[int, int]:
    if not source_dir.is_dir():
        return 0, 0

    target_dir.mkdir(parents=True, exist_ok=True)
    converted = 0
    failed = 0

    for archive in sorted(source_dir.glob("*.zip")):
        target = target_dir / f"{archive.stem}.anim"
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True, help="Asset root to populate")
    args = parser.parse_args()

    apps_assets = args.root / "ext" / "apps_assets"
    apps_assets.mkdir(parents=True, exist_ok=True)

    shared = apps_assets / "shared"
    shared.mkdir(parents=True, exist_ok=True)

    # Fonts ship in their final form; link rather than copy.
    fonts_link = shared / "fonts"
    if not fonts_link.exists():
        fonts_link.symlink_to(REPO_ROOT / "assets" / "shared" / "fonts")

    images = convert_images(REPO_ROOT / "assets" / "shared" / "images" / "external",
                            shared / "images")

    app_images = 0
    app_root = REPO_ROOT / "assets" / "images" / "external"
    if app_root.is_dir():
        for app_dir in sorted(p for p in app_root.iterdir() if p.is_dir()):
            app_images += convert_images(app_dir, apps_assets / app_dir.name / "images")

    anims, anim_failures = convert_animations(
        REPO_ROOT / "assets" / "shared" / "animations", shared / "animations")

    anim_root = REPO_ROOT / "assets" / "animations"
    if anim_root.is_dir():
        for app_dir in sorted(p for p in anim_root.iterdir() if p.is_dir()):
            converted, failures = convert_animations(
                app_dir, apps_assets / app_dir.name / "animations")
            anims += converted
            anim_failures += failures

    sounds, sound_failures = convert_sounds(
        REPO_ROOT / "assets" / "shared" / "sounds", shared / "sounds")

    sound_root = REPO_ROOT / "assets" / "sounds"
    if sound_root.is_dir():
        for app_dir in sorted(p for p in sound_root.iterdir() if p.is_dir()):
            converted, failures = convert_sounds(
                app_dir, apps_assets / app_dir.name / "sounds")
            sounds += converted
            sound_failures += failures

    anim_failures += sound_failures

    print(f"assets: {images} shared images, {app_images} app images, {anims} animations, {sounds} sounds"
          + (f", {anim_failures} conversion failures" if anim_failures else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
