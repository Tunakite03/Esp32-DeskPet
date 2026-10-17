"""Serves the repository for the customizer at http://localhost:8000/web/.

`python -m http.server` takes MIME types from the Windows registry, which often
maps .js to text/plain; browsers then refuse to run app.js as a module and the
page stays empty. This server pins the types the page needs.

Usage (from anywhere): python web/serve.py [port]
"""

import functools
import http.server
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".bin": "application/octet-stream",
    ".webp": "image/webp",
    ".png": "image/png",
    ".gif": "image/gif",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **TYPES}

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    handler = functools.partial(Handler, directory=str(ROOT))
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as server:
        print(f"DeskPet Customizer: http://localhost:{port}/web/  (Ctrl+C to stop)")
        server.serve_forever()


if __name__ == "__main__":
    main()
