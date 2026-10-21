#!/usr/bin/env python3
"""Local offline UI preview. No serial device access or firmware emulation."""
from __future__ import annotations

import argparse
import functools
import http.server
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    web = Path(__file__).resolve().parent.parent / "web"
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(web))
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"Demo UI: http://127.0.0.1:{args.port}/?demo=1", flush=True)
        print("This preview does not access UART or an ESP32. Ctrl+C stops it.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
