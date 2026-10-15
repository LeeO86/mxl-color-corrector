#!/usr/bin/env python3
"""Re-create the source flow under the same id and check that the channel re-opens it.

A paced pattern writer feeds channel 1. It stops (MXL deletes the flow with its last writer) and
starts again: once at once and once after a 1 s gap. Each time the channel must read the new
flow again within 1 s, without an IS-05 activation, count one re-open, and keep its output flow
going meanwhile (state holding, the output head advancing). Twice: with slice commits (slice
mode) and with whole grains (whole-grain mode, which reads one grain behind the head and so
starts before the new flow's first grain).
"""

import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.environ.get("CC_BUILD", os.path.join(ROOT, "build"))
ROOT_TMP = os.environ.get("CC_TMP", "/tmp/mxl-cc-reopen-it")
BASE = ROOT_TMP
SLICE_LINES = "36"
DOMAIN = "77777777-7777-4777-8777-777777777777"
FLOW = "88888888-8888-4888-8888-888888888888"
WEB = int(os.environ.get("CC_WEB_PORT", "18144"))
NMOS = int(os.environ.get("CC_NMOS_PORT", "18145"))
RESUME_S = float(os.environ.get("CC_RESUME_S", "1.0"))


def http(method, path, body=None, port=WEB):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=5) as res:
        raw = res.read()
        return res.status, raw.decode() if raw else ""


def metric(name):
    _, text = http("GET", "/metrics")
    m = re.search(r"^mxl_color_corrector_" + name + r'\{channel="1"\} (\S+)$', text, re.M)
    return float(m.group(1)) if m else None


def channel():
    return json.loads(http("GET", "/api/v1/status")[1])["channels"][0]


def writer(seconds):
    return subprocess.Popen(
        [
            os.path.join(BUILD, "mxl-cc-pattern"),
            "--domain", os.path.join(BASE, "src"),
            "--domain-id", DOMAIN,
            "--flow-id", FLOW,
            "--width", "640",
            "--height", "360",
            "--rate", "50",
            "--slices", SLICE_LINES,
            "--sleep-us", "200",
            "--paced", "1",
            "--seconds", str(seconds),
        ]
    )


def output_head(flow):
    proc = subprocess.run(
        [os.path.join(BUILD, "mxl-cc-sample"), "--domain", os.path.join(BASE, "cc-out"), "--flow", flow, "--points", "0,0", "--timeout-ms", "100"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return None
    return json.loads(proc.stdout)["index"]


def wait_until(what, pred, seconds):
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        if pred():
            return time.monotonic() - start
        time.sleep(0.02)
    raise SystemExit(f"timed out after {seconds} s waiting for {what}")


def main():
    global BASE, SLICE_LINES
    for mode, lines in (("running", "36"), ("whole_grain", "360")):
        BASE = os.path.join(ROOT_TMP, mode)
        SLICE_LINES = lines
        run(mode)


def run(mode):
    os.makedirs(os.path.join(BASE, "config"), exist_ok=True)
    log_path = os.path.join(BASE, "cc.log")
    env = os.environ.copy()
    env.update(
        {
            "CC_CHANNELS": "1",
            "MXL_DOMAIN_SCAN_PATH": BASE,
            "MXL_OUTPUT_DOMAIN_DIR": os.path.join(BASE, "cc-out"),
            "MXL_OUTPUT_DOMAIN_ID": "99999999-9999-4999-8999-999999999999",
            "CC_STATE_DIR": os.path.join(BASE, "config"),
            "WEB_PORT": str(WEB),
            "NMOS_PORT": str(NMOS),
            "NMOS_SEED": "reopen-cc",
            "CC_PREVIEW_FPS": "1",
            "NMOS_DNS_SD": "false",
            "CC_LOG_LEVEL": "info",
        }
    )
    first = writer(4)
    log = open(log_path, "w")
    corrector = subprocess.Popen([os.path.join(BUILD, "mxl-color-corrector")], env=env, stderr=log)
    procs = [first, corrector]
    try:
        wait_until("readyz", lambda: _ok("/readyz"), 15)
        receiver = json.loads(http("GET", "/api/v1/nmos")[1])["channels"][0]["receiver_id"]
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
        wait_until("grains", lambda: (metric("grains_processed_total") or 0) > 20, 10)
        wait_until(mode, lambda: channel()["state"] == mode, 2)
        out_flow = channel()["output_flow"]
        results = {}

        # 1: stop and start again at once (a new data file under the same flow id).
        first.wait(timeout=10)
        second = writer(4)
        procs.append(second)
        base = metric("grains_processed_total")
        results["immediate"] = wait_until("grains from the re-created flow", lambda: metric("grains_processed_total") > base + 2, RESUME_S + 1)
        wait_until("one re-open", lambda: metric("flow_reopens_total") == 1, 2)

        # 2: stop, 1 s without the flow, start again. The output holds its last grain.
        second.wait(timeout=10)
        wait_until("holding", lambda: channel()["state"] == "holding", 1)
        h0 = output_head(out_flow)
        time.sleep(0.4)
        h1 = output_head(out_flow)
        if h0 is None or h1 is None or h1 < h0 + 10:
            raise SystemExit(f"output did not advance while the source was missing: {h0} -> {h1}")
        time.sleep(0.4)
        third = writer(3)
        procs.append(third)
        base = metric("grains_processed_total")
        results["after_gap"] = wait_until("grains after the gap", lambda: metric("grains_processed_total") > base + 2, RESUME_S + 1)
        wait_until("two re-opens", lambda: metric("flow_reopens_total") == 2, 2)
        wait_until(mode, lambda: channel()["state"] == mode, 1)
        h2 = output_head(out_flow)
        if h2 is None or h2 <= h1:
            raise SystemExit(f"output did not continue after the re-open: {h1} -> {h2}")
        if channel()["output_flow"] != out_flow:
            raise SystemExit("the output flow changed")
        log.flush()
        text = open(log_path).read()
        for event in ("mxl_flow_recreated", "mxl_flow_reopened", "mxl_flow_resumed"):
            if text.count(event) < 2:
                raise SystemExit(f"expected two {event} logs\n{text}")
        for name, seconds in results.items():
            if seconds > RESUME_S:
                raise SystemExit(f"{name}: grains came back after {seconds:.2f} s, more than {RESUME_S} s")
        print("reopen ok", mode, {k: round(v, 3) for k, v in results.items()}, "held output", h0, "->", h1)
        print("\n".join(line for line in text.splitlines() if "mxl_flow" in line))
    finally:
        for proc in procs:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in procs:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()


def _ok(path):
    try:
        return http("GET", path)[0] == 200
    except Exception:
        return False


if __name__ == "__main__":
    sys.exit(main())
