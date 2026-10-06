#!/usr/bin/env python3
"""Drive one channel from the pattern writer through IS-05 and measure the output."""

import json
import os
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.environ.get("CC_BUILD", os.path.join(ROOT, "build"))
BASE = os.environ.get("CC_TMP", "/tmp/mxl-cc-it")
DOMAIN = "11111111-1111-4111-8111-111111111111"
FLOW = "22222222-2222-4222-8222-222222222222"
WEB = int(os.environ.get("CC_WEB_PORT", "18140"))
NMOS = int(os.environ.get("CC_NMOS_PORT", "18141"))


def http(method, path, body=None, port=WEB):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=5) as res:
        raw = res.read()
        return res.status, raw.decode() if raw else ""


def wait_ok(path, seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            status, _ = http("GET", path)
            if status == 200:
                return
        except Exception:
            pass
        time.sleep(0.1)
    raise SystemExit(f"timed out waiting for {path}")


def sample(domain, flow, points, timeout_ms="800"):
    if not flow:
        raise RuntimeError("empty flow id")
    proc = subprocess.run(
        [
            os.path.join(BUILD, "mxl-cc-sample"),
            "--domain",
            domain,
            "--flow",
            flow,
            "--points",
            points,
            "--timeout-ms",
            timeout_ms,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr or proc.stdout or "sample failed")
    return json.loads(proc.stdout)


def point(doc, x, y):
    for item in doc["points"]:
        if item["x"] == x and item["y"] == y:
            return item
    raise SystemExit(f"missing point {x},{y}")


def main():
    os.makedirs(BASE, exist_ok=True)
    src = os.path.join(BASE, "src")
    state = os.path.join(BASE, "config")
    os.makedirs(state, exist_ok=True)
    pattern = subprocess.Popen(
        [
            os.path.join(BUILD, "mxl-cc-pattern"),
            "--domain",
            src,
            "--domain-id",
            DOMAIN,
            "--flow-id",
            FLOW,
            "--width",
            "1920",
            "--height",
            "1080",
            "--rate",
            "50",
            "--slices",
            "16",
            "--sleep-us",
            "2000",
            "--seconds",
            "30",
        ]
    )
    env = os.environ.copy()
    env.update(
        {
            "CC_CHANNELS": "1",
            "MXL_DOMAIN_SCAN_PATH": BASE,
            "MXL_OUTPUT_DOMAIN_DIR": os.path.join(BASE, "cc-out"),
            "MXL_OUTPUT_DOMAIN_ID": "33333333-3333-4333-8333-333333333333",
            "CC_STATE_DIR": state,
            "WEB_PORT": str(WEB),
            "NMOS_PORT": str(NMOS),
            "NMOS_SEED": "it-cc",
            "CC_PREVIEW_FPS": "1",
            "NMOS_DNS_SD": "false",
        }
    )
    corrector = subprocess.Popen([os.path.join(BUILD, "mxl-color-corrector")], env=env)
    try:
        wait_ok("/readyz")
        _, nmos = http("GET", "/api/v1/nmos")
        receiver = json.loads(nmos)["channels"][0]["receiver_id"]
        http(
            "PATCH",
            f"/x-nmos/connection/v1.1/single/receivers/{receiver}/staged",
            {
                "master_enable": True,
                "activation": {"mode": "activate_immediate"},
                "transport_params": [{"mxl_domain_id": DOMAIN, "mxl_flow_id": FLOW}],
            },
            port=NMOS,
        )
        out_domain = os.path.join(BASE, "cc-out")
        neutral = None
        last = ""
        for _ in range(50):
            try:
                status = json.loads(http("GET", "/api/v1/status")[1])
                flow = status["channels"][0]["output_flow"] or json.loads(http("GET", "/api/v1/nmos")[1])["channels"][0]["flow_id"]
                last = status["channels"][0]["state"] + " " + flow
                neutral = sample(out_domain, flow, "120,200;1800,200")
                break
            except Exception as ex:
                last = str(ex)
                time.sleep(0.2)
        if neutral is None:
            raise SystemExit("no output grain: " + last)
        with open(os.path.join(out_domain, "domain_def.json"), encoding="utf-8") as f:
            definition = json.load(f)
        # BCP-007-03 schema: id, label, description and tags are required.
        if not all(key in definition for key in ("id", "label", "description", "tags")) or not isinstance(definition["tags"], dict):
            raise SystemExit(f"domain_def.json is not BCP-007-03: {definition}")
        white = point(neutral, 120, 200)
        black = point(neutral, 1800, 200)
        if abs(white["cb"] - 512) > 8 or abs(white["cr"] - 512) > 8:
            raise SystemExit(f"neutral white chroma drifted {white}")
        if black["y_code"] > 80:
            raise SystemExit(f"black bar is not black {black}")
        http("PATCH", "/api/v1/channels/1/controls", {"gain": 50, "saturation": 0})
        time.sleep(0.4)
        graded = sample(out_domain, json.loads(http("GET", "/api/v1/status")[1])["channels"][0]["output_flow"], "120,200;1800,200")
        white2 = point(graded, 120, 200)
        black2 = point(graded, 1800, 200)
        if abs(black2["y_code"] - black["y_code"]) > 2:
            raise SystemExit(f"gain moved black {black} -> {black2}")
        if white2["y_code"] >= white["y_code"] - 20:
            raise SystemExit(f"gain did not move white {white} -> {white2}")
        if abs(white2["cb"] - 512) > 2 or abs(white2["cr"] - 512) > 2:
            raise SystemExit(f"saturation 0 left chroma {white2}")
        _, metrics = http("GET", "/metrics")
        if 'mxl_color_corrector_slice_mode{channel="1"} 1' not in metrics:
            raise SystemExit("expected slice mode\n" + metrics)
        if "mxl_color_corrector_added_latency_seconds_bucket" not in metrics:
            raise SystemExit("missing latency histogram")
        def latency_counts(text):
            count = bucket = None
            for line in text.splitlines():
                if line.startswith('mxl_color_corrector_added_latency_seconds_count{channel="1"}'):
                    count = float(line.split()[-1])
                if line.startswith('mxl_color_corrector_added_latency_seconds_bucket{channel="1",le="0.02"}'):
                    bucket = float(line.split()[-1])
            return count, bucket

        # Grains already finished when the reader attaches are processed whole and can
        # take longer than one frame on a small runner. Steady-state slice commits,
        # after the reader has caught the writer, stay inside one 1080p50 frame.
        time.sleep(0.5)
        _, warm = http("GET", "/metrics")
        base_count, base_bucket = latency_counts(warm)
        time.sleep(1.2)
        _, metrics = http("GET", "/metrics")
        count, bucket = latency_counts(metrics)
        if count is None or bucket is None or base_count is None or base_bucket is None:
            raise SystemExit("missing latency histogram")
        new_count = count - base_count
        new_under = bucket - base_bucket
        if new_count < 5 or new_under < new_count:
            raise SystemExit(f"added latency exceeded one frame new={new_count} under_20ms={new_under} total={count}")
        print("integration ok", {"white": white, "black": black, "graded_white": white2, "latency_count": count, "steady": new_count})
        interlaced_flow = "44444444-4444-4444-8444-444444444444"
        interlaced = subprocess.Popen(
            [
                os.path.join(BUILD, "mxl-cc-pattern"),
                "--domain",
                os.path.join(BASE, "src-i"),
                "--domain-id",
                "55555555-5555-4555-8555-555555555555",
                "--flow-id",
                interlaced_flow,
                "--interlace",
                "interlaced_tff",
                "--rate",
                "25",
                "--seconds",
                "12",
                "--sleep-us",
                "0",
                "--slices",
                "1080",
            ]
        )
        try:
            http(
                "PATCH",
                f"/x-nmos/connection/v1.1/single/receivers/{receiver}/staged",
                {
                    "master_enable": True,
                    "activation": {"mode": "activate_immediate"},
                    "transport_params": [{"mxl_domain_id": "55555555-5555-4555-8555-555555555555", "mxl_flow_id": interlaced_flow}],
                },
                port=NMOS,
            )
            seen = False
            for _ in range(40):
                _, body = http("GET", "/api/v1/status")
                if json.loads(body)["channels"][0]["fallback_reason"] == "interlaced":
                    seen = True
                    break
                time.sleep(0.25)
            if not seen:
                raise SystemExit("interlaced input did not report whole-grain fallback")
            print("interlaced fallback ok")
        finally:
            if interlaced.poll() is None:
                interlaced.send_signal(signal.SIGTERM)
                interlaced.wait(timeout=5)
    finally:
        for proc in (corrector, pattern):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in (corrector, pattern):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
