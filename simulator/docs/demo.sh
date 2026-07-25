#!/bin/sh
# Re-record docs/demo.gif, the walkthrough at the top of the README.
#
# Run from the repository root, with the simulator built.
#
# The two deletions are what make this repeatable. Storage is the workstation
# filesystem here, so a simulator remembers what the last one did: a busy
# session left running means `start` pauses it rather than starting one, and an
# app the apps menu already remembers means the lever resumes it instead of
# opening on its splash. Both change what the recording shows. Removing the
# state leaves the device the way it boots from the factory, which is the one
# starting point the steps below can rely on. `done.txt` stays: without it the
# startup app waits for a button on an animation the simulator has no assets
# for.
#
# The pause after start() is not decoration either — the desktop service boots,
# the loader starts the Busy app and the wipe plays, and a button pressed
# before that lands nowhere.
set -eu

SIMCTL=simulator/tools/simctl.py
DATA=simulator/build/assets_root/data

$SIMCTL stop >/dev/null 2>&1 || true
rm -f "$DATA/state.json" "$DATA/settings.json"

$SIMCTL start
$SIMCTL run wait:5 >/dev/null

$SIMCTL record --out simulator/docs/demo.gif --settle 0.35 \
    wait:1.2 \
    start wait:3.5 \
    apps wait:1.6 \
    ok wait:1.4 \
    next wait:1.1 \
    prev wait:1.1 \
    ok wait:1.4 \
    ok wait:2.5 \
    settings wait:1.4 \
    ok wait:1.2 \
    next wait:0.9 \
    next wait:1.2 \
    busy wait:2.2

$SIMCTL stop
