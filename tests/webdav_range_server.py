#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: webdav_range_server.py <port-file>")

port_path = Path(sys.argv[1])
SIZE = 2 * 1024 * 1024 + 123
ETAG = '"astranas-range-v1"'
CHANGED_ETAG = '"astranas-range-v2"'
RANGE_RE = re.compile(r"^bytes=(\d+)-(\d+)$")
changing_requests = 0


def byte_at(index: int) -> int:
    return (index * 31 + 7) & 0xFF


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        global changing_requests
        if self.path not in ("/package.nsp", "/ignore-range.nsp", "/changing.nsp"):
            self.send_error(404)
            return
        if self.path == "/ignore-range.nsp":
            if self.headers.get("If-Match") != ETAG:
                self.send_response(412)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("ETag", ETAG)
            self.send_header("Content-Length", "1")
            self.end_headers()
            self.wfile.write(bytes([byte_at(0)]))
            return

        active_etag = ETAG
        if self.path == "/changing.nsp" and changing_requests > 0:
            active_etag = CHANGED_ETAG
        if_match = self.headers.get("If-Match")
        if if_match != active_etag:
            self.send_response(412)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        match = RANGE_RE.fullmatch(self.headers.get("Range", ""))
        if not match:
            self.send_response(200)
            self.send_header("ETag", ETAG)
            self.send_header("Content-Length", str(SIZE))
            self.end_headers()
            self.wfile.write(bytes(byte_at(i) for i in range(SIZE)))
            return
        start, end = map(int, match.groups())
        if start < 0 or end < start or end >= SIZE:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{SIZE}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        payload = bytes(byte_at(i) for i in range(start, end + 1))
        self.send_response(206)
        self.send_header("ETag", active_etag)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Range", f"bytes {start}-{end}/{SIZE}")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        if self.path == "/changing.nsp":
            changing_requests += 1

    def log_message(self, *_args):
        pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port_path.write_text(str(server.server_port), encoding="ascii")
server.serve_forever()
