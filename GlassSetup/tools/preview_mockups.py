#!/usr/bin/env python3
"""Read-only GlassSetup mockup server. No installer or platform is executed.

Run from the solution root: python GlassSetup/tools/preview_mockups.py --port 8080
Serves the durable mockup catalogue and the actual production HTML/assets.
"""
import argparse
import io
import json
import re
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

BASE = Path(__file__).resolve().parents[1]
MOCKUPS = BASE / 'docs/mockups'
RUNTIME = BASE / 'res'
SHARED_STRINGS = RUNTIME / 'strings/Common.rcinc'


def read_shared_strings():
    """Read the two RC string tables for the mock WebView host."""
    strings = {'ru': {}, 'en': {}}
    locale = None
    for line in SHARED_STRINGS.read_text(encoding='utf-8').splitlines():
        if line.startswith('LANGUAGE LANG_RUSSIAN,'):
            locale = 'ru'
        elif line.startswith('LANGUAGE LANG_ENGLISH,'):
            locale = 'en'
        elif line.lstrip().startswith('IDS_'):
            match = re.fullmatch(r'\s*(IDS_\w+)\s+"((?:[^"\\]|\\.|"")*)"\s*', line)
            if not match or locale is None or match[1] in strings[locale]:
                raise ValueError(f'Invalid or duplicate string resource: {line}')
            escapes = {'""': '"', r'\"': '"', r'\\': '\\', r'\n': '\n', r'\r': '\r', r'\t': '\t'}
            strings[locale][match[1]] = re.sub(r'""|\\.', lambda m: escapes[m[0]], match[2])
    if not strings['ru'] or strings['ru'].keys() != strings['en'].keys():
        raise ValueError('Shared RC string tables must contain matching RU/EN keys')
    return strings


class Handler(SimpleHTTPRequestHandler):
    def send_head(self):
        if urlsplit(self.path).path != '/runtime/wizard-strings.json':
            return super().send_head()
        try:
            data = json.dumps(read_shared_strings(), ensure_ascii=False).encode('utf-8')
        except (OSError, ValueError, KeyError):
            self.send_error(500, 'Unable to read the shared string catalogue')
            return None
        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        return io.BytesIO(data)

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
