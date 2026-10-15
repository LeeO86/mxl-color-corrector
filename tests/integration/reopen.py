#!/usr/bin/env python3
"""Re-create the source flow under the same id and check that the channel re-opens it.

A paced pattern writer feeds channel 1.
1. It stops (MXL deletes the flow with its last writer) and starts again at once.
2. It stops and starts again after a 1 s gap: the output holds its last grain (state holding).
3. It is killed and leaves its flow behind: no write, the flow stays valid; the output holds.
4. It stops for longer than CC_HOLD_MAX_S (2 s here): the output turns black, flagged invalid
   (state no_signal), and resumes when the writer is back.
Each time the channel must read the new flow within 1 s without an IS-05 activation, and the
output head must keep going and never go back. Four passes: slice and whole-grain mode, each
with a writer on the clock and one 12 grains behind it (a gateway indexing by capture time).
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
LAG = 0
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


def metric_plain(name):
    _, text = http("GET", "/metrics")
    m = re.search(r"^mxl_color_corrector_" + name + r" (\S+)$", text, re.M)
    return float(m.group(1)) if m else 0.0


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
            "--lag-grains", str(LAG),
            "--seconds", str(seconds),
        ]
    )


def output_grain(flow):
    proc = subprocess.run(
        [os.path.join(BUILD, "mxl-cc-sample"), "--domain", os.path.join(BASE, "cc-out"), "--flow", flow, "--points", "0,0", "--timeout-ms", "100"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return None
    return json.loads(proc.stdout)


heads = []


def output_head(flow):
    doc = output_grain(flow)
    if doc is None:
        return None
    heads.append(doc["index"])
    return doc["index"]


def wait_until(what, pred, seconds):
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        if pred():
            return time.monotonic() - start
        time.sleep(0.02)
    raise SystemExit(f"timed out after {seconds} s waiting for {what}")


def main():
    global BASE, SLICE_LINES, LAG
    for mode, lines, lag in (("running", "36", 0), ("whole_grain", "360", 0), ("running", "36", 12), ("whole_grain", "360", 12)):
        BASE = os.path.join(ROOT_TMP, f"{mode}-lag{lag}")
        SLICE_LINES = lines
        LAG = lag
        heads.clear()
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
            "CC_HOLD_MAX_S": "2",
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
        cpu0, t0 = metric_plain("process_cpu_seconds_total"), time.monotonic()
        results["immediate"] = wait_until("grains from the re-created flow", lambda: metric("grains_processed_total") > base + 2, RESUME_S + 1)
        wait_until("one re-open", lambda: metric("flow_reopens_total") == 1, 2)
        time.sleep(1.0)
        cpu = (metric_plain("process_cpu_seconds_total") - cpu0) / (time.monotonic() - t0)
        if cpu > 0.6:
            raise SystemExit(f"{cpu:.2f} cores after the re-create: a busy loop?")
        results["cores_after_recreate"] = cpu

        # 2: stop, 1 s without the flow, start again. The output holds its last grain.
        second.wait(timeout=10)
        wait_until("holding", lambda: channel()["state"] == "holding", 1)
        h0 = output_head(out_flow)
        time.sleep(0.4)
        h1 = output_head(out_flow)
        if h0 is None or h1 is None or h1 < h0 + 10:
            raise SystemExit(f"output did not advance while the source was missing: {h0} -> {h1}")
        time.sleep(0.4)
        third = writer(8)
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

        # 3: the writer is killed and leaves its flow: no write, the flow stays valid. Hold, and
        # follow the writer when it is back.
        reopens = metric("flow_reopens_total")
        third.send_signal(signal.SIGKILL)
        third.wait(timeout=5)
        wait_until("holding after a kill", lambda: channel()["state"] == "holding", 1)
        k0 = output_head(out_flow)
        time.sleep(0.4)
        k1 = output_head(out_flow)
        if k0 is None or k1 is None or k1 < k0 + 10:
            raise SystemExit(f"output did not advance while the writer was dead: {k0} -> {k1}")
        fourth = writer(3)
        procs.append(fourth)
        base = metric("grains_processed_total")
        results["after_kill"] = wait_until("grains after the kill", lambda: metric("grains_processed_total") > base + 2, RESUME_S + 1)
        wait_until(mode, lambda: channel()["state"] == mode, 1)
        k2 = output_head(out_flow)
        if k2 is None or k2 <= k1:
            raise SystemExit(f"output did not continue after the kill: {k1} -> {k2}")
        results["reopens_after_kill"] = metric("flow_reopens_total") - reopens

        # 4: gone for longer than CC_HOLD_MAX_S: black, flagged invalid, state no_signal.
        reopens = metric("flow_reopens_total")
        fourth.wait(timeout=10)
        wait_until("holding", lambda: channel()["state"] == "holding", 1)
        results["no_signal_after"] = wait_until("no_signal", lambda: channel()["state"] == "no_signal", 3.5)
        grain = output_grain(out_flow)
        if grain is None or not grain["flags"] & 1 or grain["points"][0]["y_code"] != 64 or grain["points"][0]["cb"] != 512:
            raise SystemExit(f"expected a black grain flagged invalid after the limit: {grain}")
        n0 = output_head(out_flow)
        time.sleep(0.3)
        n1 = output_head(out_flow)
        if n0 is None or n1 is None or n1 < n0 + 8:
            raise SystemExit(f"output stopped after the limit: {n0} -> {n1}")
        fifth = writer(3)
        procs.append(fifth)
        base = metric("grains_processed_total")
        results["after_limit"] = wait_until("grains after the limit", lambda: metric("grains_processed_total") > base + 2, RESUME_S + 1)
        wait_until(mode, lambda: channel()["state"] == mode, 1)
        if metric("flow_reopens_total") != reopens + 1:
            raise SystemExit("expected one re-open after the limit")
        grain = output_grain(out_flow)
        if grain is None or grain["flags"] & 1:
            raise SystemExit(f"output still flagged invalid after the source came back: {grain}")
        output_head(out_flow)
        if any(b < a for a, b in zip(heads, heads[1:])):
            raise SystemExit(f"the output head went back: {heads}")
        log.flush()
        text = open(log_path).read()
        for event in ("mxl_flow_recreated", "mxl_flow_reopened", "mxl_flow_resumed", "mxl_source_stalled", "mxl_hold_expired"):
            if event not in text:
                raise SystemExit(f"expected a {event} log\n{text}")
        if "output grain write failed" in text:
            raise SystemExit(f"an output grain was refused\n{text}")
        for name, seconds in results.items():
            if not name.startswith(("immediate", "after")):
                continue
            if seconds > RESUME_S:
                raise SystemExit(f"{name}: grains came back after {seconds:.2f} s, more than {RESUME_S} s")
        print("reopen ok", mode, f"lag {LAG}", {k: round(v, 3) for k, v in results.items()}, "held output", h0, "->", h1)
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
