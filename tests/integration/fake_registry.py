#!/usr/bin/env python3
"""Tiny IS-04 registration endpoint for the compose demo."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    nodes = {}

    def _send(self, code, body):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if code != 204:
            self.wfile.write(data)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b""
        if self.path.startswith("/x-nmos/registration/v1.3/resource"):
            try:
                import json
                doc = json.loads(raw.decode() or "{}")
                if doc.get("type") == "node":
                    Handler.nodes[doc["data"]["id"]] = doc["data"]
            except Exception:
                pass
        self._send(200, b'{"health":"ok"}')

    def do_GET(self):
        prefix = "/x-nmos/query/v1.3/nodes/"
        if self.path.startswith(prefix):
            import json
            node_id = self.path[len(prefix):].strip("/")
            if node_id in Handler.nodes:
                self._send(200, json.dumps(Handler.nodes[node_id]))
                return
            self._send(404, b'{"error":"missing"}')
            return
        self._send(200, b'{"health":"ok"}')

    def do_DELETE(self):
        marker = "/resource/nodes/"
        if marker in self.path:
            node_id = self.path.split(marker, 1)[1].strip("/")
            Handler.nodes.pop(node_id, None)
            self._send(204, b"")
            return
        self._send(404, b"{}")

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", 3210), Handler).serve_forever()
