#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: webdav_put_server.py <output-file> <port-file>")

output_path = Path(sys.argv[1])
port_path = Path(sys.argv[2])

class Handler(BaseHTTPRequestHandler):
    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        output_path.write_bytes(self.rfile.read(length))
        self.send_response(201)
        self.end_headers()

    def log_message(self, *_args):
        pass

server = HTTPServer(("127.0.0.1", 0), Handler)
port_path.write_text(str(server.server_port), encoding="ascii")
server.handle_request()
