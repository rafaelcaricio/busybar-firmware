#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Make the web server's listen address configurable at run time.

web_srv_start() binds "http://0.0.0.0", which mongoose reads as port 80. That
is a privileged port on macOS and Linux, so the simulator would need to run as
root to serve its HTTP API -- and two simulators could never run at once. The
address is therefore taken from web_srv_host_listen_url(), which
src/web_api_host.c answers from the --api-port option.

Nothing else about the server changes: the same handler table, dispatch and
upload machinery serve the same routes as on the device.
"""

import argparse
import sys
from pathlib import Path

LISTEN = '    mg_http_listen(&srv.mgr, "http://0.0.0.0", http_event_handler, &srv);'

LISTEN_REPLACEMENT = """    /* Host build: address from --api-port, see simulator/tools/patch_web_server.py */
    mg_http_listen(&srv.mgr, web_srv_host_listen_url(), http_event_handler, &srv);"""

ENTRY = "\nint32_t web_srv_start(void* p) {"

DECLARATION = """
/* Host build: defined in simulator/src/web_api_host.c. */
const char* web_srv_host_listen_url(void);
"""


def fail(message: str) -> int:
    print(f"{message}; update simulator/tools/patch_web_server.py", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.input.read_text()

    if source.count(LISTEN) != 1:
        return fail(f"{args.input}: expected exactly one mg_http_listen on 0.0.0.0")
    if source.count(ENTRY) != 1:
        return fail(f"{args.input}: could not locate web_srv_start")

    patched = source.replace(LISTEN, LISTEN_REPLACEMENT)
    patched = patched.replace(ENTRY, DECLARATION + ENTRY)

    if '"http://0.0.0.0"' in patched:
        return fail(f"{args.input}: the hardcoded listen address survived patching")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patched)

    print(f"patched {args.input.name} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
