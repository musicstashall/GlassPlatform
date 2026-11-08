#!/usr/bin/env python3
"""Read-only GlassSetup mockup server. No installer or platform is executed.

Run from the solution root: python GlassSetup/tools/preview_mockups.py --port 8080
Serves the durable mockup catalogue and the actual production HTML/assets.
"""
import argparse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

BASE = Path(__file__).resolve().parents[1]
MOCKUPS = BASE / 'docs/mockups'
RUNTIME = BASE / 'res'


class Handler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = unquote(urlsplit(path).path)
        prefix = next((value for value in ('/runtime/', '/res/') if path.startswith(value)), '')
        root = RUNTIME if prefix else MOCKUPS
        relative = path[len(prefix):] if prefix else path.lstrip('/')
        if root == RUNTIME and relative == 'assets/banner.png':
            relative = 'welcome_banner.png'  # Native virtual-host mapping.
        candidate = (root / relative).resolve()
        if not candidate.is_relative_to(root):
            return str(MOCKUPS / '.not-served')
        return str(candidate)

    def list_directory(self, path):
        self.send_error(404, 'Directory listing is disabled')
        return None


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    print(f'GlassSetup mockups: port {args.port}; read-only UI fixtures, no system operations', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
