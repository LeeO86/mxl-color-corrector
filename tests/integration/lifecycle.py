#!/usr/bin/env python3
"""Start, wait until registered, SIGTERM, and check the node and domain are gone."""

import json
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BUILD = os.environ.get("CC_BUILD", "build")
BASE = os.environ.get("CC_TMP", "/tmp/mxl-cc-life")
REG_PORT = int(os.environ.get("CC_REG_PORT", "18210"))
WEB = int(os.environ.get("CC_WEB_PORT", "18142"))
NMOS = int(os.environ.get("CC_NMOS_PORT", "18143"))
DOMAIN_ID = "66666666-6666-4666-8666-666666666666"


class Registry(BaseHTTPRequestHandler):
    nodes = {}
    devices = {}

    def _read(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        return self.rfile.read(n) if n else b""

    def _send(self, code, body=b"{}"):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self._read()
        if self.path.startswith("/x-nmos/registration/v1.3/resource"):
            try:
                doc = json.loads(raw.decode() or "{}")
            except json.JSONDecodeError:
                doc = {}
            if doc.get("type") == "node":
                Registry.nodes[doc["data"]["id"]] = doc["data"]
            if doc.get("type") == "device":
                Registry.devices[doc["data"]["id"]] = doc["data"]
            self._send(201, b'{"ok":true}')
            return
        if "/health/nodes/" in self.path:
            self._send(200, b'{"health":"ok"}')
            return
        self._send(404, b'{"error":"no"}')

    def do_GET(self):
        prefix = "/x-nmos/query/v1.3/nodes/"
        if self.path.startswith(prefix):
            node_id = self.path[len(prefix):].strip("/")
            if node_id in Registry.nodes:
                self._send(200, json.dumps(Registry.nodes[node_id]).encode())
                return
            self._send(404, b'{"error":"missing"}')
            return
        self._send(200, b"[]")

    def do_DELETE(self):
        marker = "/resource/nodes/"
        if marker in self.path:
            node_id = self.path.split(marker, 1)[1].strip("/")
            Registry.nodes.pop(node_id, None)
            self._send(204, b"")
            return
        self._send(404, b"{}")

    def log_message(self, fmt, *args):
        return


def http(method, path):
    import urllib.request
    req = urllib.request.Request(f"http://127.0.0.1:{WEB}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=2) as res:
            return res.status
    except Exception as ex:
        code = getattr(getattr(ex, "code", None), "__int__", lambda: None)()
        if hasattr(ex, "code"):
            return ex.code
        return 0


def main():
    os.makedirs(BASE, exist_ok=True)
    domain = os.path.join(BASE, "cc-life")
    state = os.path.join(BASE, "config")
    os.makedirs(state, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", REG_PORT), Registry)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = os.environ.copy()
    env.update(
        {
            "CC_CHANNELS": "1",
            "MXL_DOMAIN_SCAN_PATH": BASE,
            "MXL_OUTPUT_DOMAIN_DIR": domain,
            "MXL_OUTPUT_DOMAIN_ID": DOMAIN_ID,
            "MXL_CLEANUP_ON_EXIT": "true",
            "CC_STATE_DIR": state,
            "WEB_PORT": str(WEB),
            "NMOS_PORT": str(NMOS),
            "NMOS_SEED": "life-cc",
            "NMOS_LABEL": "life-cc",
            "NMOS_HOST_ADDRESS": "10.255.0.1",
            "NMOS_REGISTRY_ADDRESS": "127.0.0.1",
            "NMOS_REGISTRY_PORT": str(REG_PORT),
            "NMOS_QUERY_ADDRESS": "127.0.0.1",
            "NMOS_QUERY_PORT": str(REG_PORT),
            "NMOS_TAGS": '{"urn:x-srf:production":["life"],"urn:x-srf:function":["cc"]}',
            "SHUTDOWN_TIMEOUT_S": "10",
        }
    )
    # 10.255.0.1 is an address literal for the announcement. Registration still
    # dials 127.0.0.1, which is the registry, not the announced host.
    proc = subprocess.Popen([os.path.join(BUILD, "mxl-color-corrector")], env=env)
    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            if http("GET", "/readyz") == 200:
                break
            time.sleep(0.1)
        else:
            raise SystemExit("readyz did not become 200")
        if not Registry.nodes:
            raise SystemExit("node was not registered")
        node_id = next(iter(Registry.nodes))
        # A controller finds the IS-05 API through the device's control.
        controls = [d.get("controls") for d in Registry.devices.values()]
        want = [{"href": f"http://10.255.0.1:{NMOS}/x-nmos/connection/v1.1/", "type": "urn:x-nmos:control:sr-ctrl/v1.1"}]
        if controls != [want]:
            raise SystemExit(f"device controls {controls}, expected {want}")
        proc.send_signal(signal.SIGTERM)
        code = proc.wait(timeout=15)
        if code != 143:
            raise SystemExit(f"expected exit 143, got {code}")
        if node_id in Registry.nodes:
            raise SystemExit("node was not deregistered")
        if os.path.exists(domain):
            raise SystemExit("output domain was not removed")
        print("lifecycle ok", node_id)
    finally:
        if proc.poll() is None:
            proc.kill()
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main() or 0)
