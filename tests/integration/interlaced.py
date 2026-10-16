#!/usr/bin/env python3
"""1080i50 through the corrector: a field in, a field out (1.2.3).

MXL stores an interlaced flow as fields: each grain is one field (540 lines of 1080i) at twice the
declared rate (50 fields/s for grain_rate 25/1). 1.2.2 read and wrote 1080 lines per grain and
crashed (SIGSEGV) on the first grain.

For interlaced_tff and interlaced_bff (a paced pattern writer, 1920x1080, grain_rate 25/1, whose
bottom field has its bars the other way round):
- the output flow declares the same interlace_mode, grain_rate 25/1 and frame_height 1080, and
  MXL gives it 540-line grains at 50/1; NMOS advertises the same interlace_mode;
- the output's even grains are the first field (top for tff, bottom for bff), both corrected
  (a gain moves both fields' white);
- the output runs at 50 grains/s with no invalid grain;
- the source killed (its flow left behind) is held at the field rate, and resumes.
Then a flow whose flow_def.json does not match its grains (an interlaced flow re-labelled
progressive 1080p50: 540-line grains read as 1080 lines; and progressive at 25/1: the grain rate
differs) leaves the channel `unsupported`, counted, never crashed; routed back, it runs again.
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.environ.get("CC_BUILD", os.path.join(ROOT, "build"))
BASE = os.environ.get("CC_TMP", "/tmp/mxl-cc-interlaced-it")
WEB = int(os.environ.get("CC_WEB_PORT", "18146"))
NMOS = int(os.environ.get("CC_NMOS_PORT", "18147"))
STRIDE = (1920 + 47) // 48 * 128
FIELD_BYTES = STRIDE * 540


def http(method, path, body=None, port=WEB):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=5) as res:
        raw = res.read()
        return res.status, raw.decode() if raw else ""


def channel():
    return json.loads(http("GET", "/api/v1/status")[1])["channels"][0]


def metric(name, labels='channel="1"'):
    _, text = http("GET", "/metrics")
    m = re.search(r"^mxl_color_corrector_" + re.escape(name) + r"\{" + re.escape(labels) + r"\} (\S+)$", text, re.M)
    return float(m.group(1)) if m else None


def sample(domain, flow, points="120,100;1800,100", extra=()):
    proc = subprocess.run(
        [os.path.join(BUILD, "mxl-cc-sample"), "--domain", domain, "--flow", flow, "--points", points, "--timeout-ms", "800", *extra],
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
    raise SystemExit(f"missing point {x},{y} in {doc}")


def writer(domain, domain_id, flow, mode, seconds=90):
    return subprocess.Popen(
        [
            os.path.join(BUILD, "mxl-cc-pattern"),
            "--domain", domain,
            "--domain-id", domain_id,
            "--flow-id", flow,
            "--width", "1920",
            "--height", "1080",
            "--rate", "25",
            "--interlace", mode,
            "--slices", "540",
            "--sleep-us", "0",
            "--paced", "1",
            "--seconds", str(seconds),
        ]
    )


def wait_for(what, check, seconds=10.0, step=0.1):
    deadline = time.time() + seconds
    last = None
    while time.time() < deadline:
        try:
            last = check()
            if last:
                return last
        except Exception as ex:  # noqa: BLE001 - the condition is retried until the deadline
            last = ex
        time.sleep(step)
    raise SystemExit(f"timed out waiting for {what}: {last}")


def route(receiver, domain_id, flow):
    http(
        "PATCH",
        f"/x-nmos/connection/v1.1/single/receivers/{receiver}/staged",
        {"master_enable": True, "activation": {"mode": "activate_immediate"}, "transport_params": [{"mxl_domain_id": domain_id, "mxl_flow_id": flow}]},
        port=NMOS,
    )


def stop(proc, sig=signal.SIGTERM):
    if proc and proc.poll() is None:
        proc.send_signal(sig)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def alive(corrector):
    if corrector.poll() is not None:
        raise SystemExit(f"mxl-color-corrector exited with {corrector.returncode}")


def output_def(out_domain, out_flow):
    with open(os.path.join(out_domain, out_flow + ".mxl-flow", "flow_def.json"), encoding="utf-8") as f:
        return json.load(f)


def check_fields(corrector, out_domain, mode, source_flow):
    def ready():
        st = channel()
        if st["state"] != "whole_grain" or st["source_flow"] != source_flow or not st["output_flow"]:
            return None
        return st if output_def(out_domain, st["output_flow"])["interlace_mode"] == mode else None

    st = wait_for(f"whole-grain output of {mode}", ready)
    out_flow = st["output_flow"]
    if not st["interlaced"] or st["fallback_reason"] != "interlaced" or st["height"] != 1080:
        raise SystemExit(f"status does not show the interlaced input: {st}")
    definition = output_def(out_domain, out_flow)
    if definition["interlace_mode"] != mode or definition["grain_rate"] != {"numerator": 25, "denominator": 1} or definition["frame_height"] != 1080:
        raise SystemExit(f"output flow_def does not mirror the input: {definition}")
    flows = json.loads(http("GET", "/x-nmos/node/v1.3/flows", port=NMOS)[1])
    mine = [f for f in flows if f["id"] == out_flow]
    if not mine or mine[0]["interlace_mode"] != mode or mine[0]["grain_rate"] != {"numerator": 25, "denominator": 1}:
        raise SystemExit(f"NMOS flow does not advertise {mode} at 25/1: {mine or flows}")
    # Even grains are the first field: top (bars, white at x=120) for tff, bottom (bars the other
    # way, black at x=120, white at x=1800) for bff.
    seen = {}
    deadline = time.time() + 10
    while len(seen) < 2 and time.time() < deadline:
        doc = sample(out_domain, out_flow)
        if doc["total_slices"] != 540 or doc["grain_size"] != FIELD_BYTES or doc["lines"] != 540 or doc["grain_rate"] != [50, 1]:
            raise SystemExit(f"output grain is not a 1080i50 field: {doc}")
        left, right = point(doc, 120, 100), point(doc, 1800, 100)
        top = left["y_code"] > 800 and right["y_code"] < 100
        bottom = left["y_code"] < 100 and right["y_code"] > 800
        if not (top or bottom):
            raise SystemExit(f"output field is neither top nor bottom: {doc}")
        first = doc["index"] % 2 == 0
        if top != (first == (mode == "interlaced_tff")):
            raise SystemExit(f"{mode}: grain {doc['index']} holds the {'top' if top else 'bottom'} field")
        seen[doc["index"] % 2] = max(left["y_code"], right["y_code"])
        time.sleep(0.013)
    if len(seen) < 2:
        raise SystemExit(f"did not see both fields: {seen}")
    # 50 grains a second, none invalid.
    h0 = sample(out_domain, out_flow)["index"]
    time.sleep(2.0)
    h1 = sample(out_domain, out_flow)["index"]
    if not 90 <= h1 - h0 <= 110:
        raise SystemExit(f"output ran {h1 - h0} grains in 2 s, expected about 100 (50 fields/s)")
    invalid = sample(out_domain, out_flow, extra=("--count-invalid", "100"))
    if invalid["invalid"]:
        raise SystemExit(f"invalid output grains: {invalid}")
    alive(corrector)
    return out_flow, seen


def main():
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(BASE)
    state = os.path.join(BASE, "config")
    os.makedirs(state)
    out_domain = os.path.join(BASE, "cc-out")
    env = os.environ.copy()
    env.update(
        {
            "CC_CHANNELS": "1",
            "MXL_DOMAIN_SCAN_PATH": BASE,
            "MXL_OUTPUT_DOMAIN_DIR": out_domain,
            "MXL_OUTPUT_DOMAIN_ID": "33333333-3333-4333-8333-3333333333a1",
            "CC_STATE_DIR": state,
            "WEB_PORT": str(WEB),
            "NMOS_PORT": str(NMOS),
            "NMOS_SEED": "it-cc-interlaced",
            "CC_PREVIEW_FPS": "2",
            "NMOS_DNS_SD": "false",
        }
    )
    corrector = subprocess.Popen([os.path.join(BUILD, "mxl-color-corrector")], env=env)
    writers = []
    try:
        wait_for("readyz", lambda: http("GET", "/readyz")[0] == 200)
        receiver = json.loads(http("GET", "/api/v1/nmos")[1])["channels"][0]["receiver_id"]
        receivers = json.loads(http("GET", "/x-nmos/node/v1.3/receivers", port=NMOS)[1])
        sets = receivers[0]["caps"]["constraint_sets"]
        modes = [m for s in sets for m in s["urn:x-nmos:cap:format:interlace_mode"]["enum"]]
        if "interlaced_tff" not in modes or "interlaced_bff" not in modes or "interlaced_psf" in modes:
            raise SystemExit(f"receiver caps interlace modes {modes}")

        tff_flow = None
        for n, mode in enumerate(("interlaced_tff", "interlaced_bff")):
            domain = os.path.join(BASE, f"src-{n}")
            domain_id = f"5555555{n}-5555-4555-8555-555555555555"
            flow = f"4444444{n}-4444-4444-8444-444444444444"
            w = writer(domain, domain_id, flow, mode)
            writers.append(w)
            route(receiver, domain_id, flow)
            out_flow, seen = check_fields(corrector, out_domain, mode, flow)
            # A gain moves both fields' white.
            http("PATCH", "/api/v1/channels/1/controls", {"gain": 50})
            time.sleep(0.4)
            graded = {}
            deadline = time.time() + 5
            while len(graded) < 2 and time.time() < deadline:
                doc = sample(out_domain, out_flow)
                graded[doc["index"] % 2] = max(point(doc, 120, 100)["y_code"], point(doc, 1800, 100)["y_code"])
                time.sleep(0.013)
            if len(graded) < 2 or any(graded[p] > seen[p] - 20 for p in (0, 1)):
                raise SystemExit(f"{mode}: gain did not move both fields' white {seen} -> {graded}")
            http("PATCH", "/api/v1/channels/1/controls", {"gain": 100})
            print(f"{mode} ok: output {out_flow} 540-line fields at 50/1, white {seen} graded {graded}")
            if mode == "interlaced_tff":
                tff_flow = (domain, domain_id, flow)
                # The source killed with its flow left behind: held at the field rate, then resumed.
                stop(w, signal.SIGKILL)
                wait_for("holding", lambda: channel()["state"] == "holding", seconds=3)
                h0 = sample(out_domain, out_flow)["index"]
                time.sleep(1.0)
                h1 = sample(out_domain, out_flow)["index"]
                if not 40 <= h1 - h0 <= 60:
                    raise SystemExit(f"held output ran {h1 - h0} grains in 1 s, expected about 50")
                w = writer(domain, domain_id, flow, mode)
                writers.append(w)
                wait_for("resume", lambda: channel()["state"] == "whole_grain", seconds=5)
                time.sleep(1.0)
                invalid = sample(out_domain, out_flow, extra=("--count-invalid", "150"))
                if invalid["invalid"]:
                    raise SystemExit(f"invalid output grains around the hold: {invalid}")
                print(f"hold ok: {h1 - h0} held grains in 1 s, resumed, no invalid grain")
                alive(corrector)

        # Flows whose flow_def.json does not match their grains.
        for n, (lie_mode, lie_rate) in enumerate((("progressive", 50), ("progressive", 25))):
            domain = os.path.join(BASE, f"src-lie-{n}")
            domain_id = f"6666666{n}-6666-4666-8666-666666666666"
            flow = f"9999999{n}-9999-4999-8999-999999999999"
            w = writer(domain, domain_id, flow, "interlaced_tff")
            writers.append(w)
            path = os.path.join(domain, flow + ".mxl-flow", "flow_def.json")
            wait_for("lie flow", lambda: os.path.exists(path) and os.path.getsize(path) > 0)
            with open(path, encoding="utf-8") as f:
                definition = json.load(f)
            definition["interlace_mode"] = lie_mode
            definition["grain_rate"] = {"numerator": lie_rate, "denominator": 1}
            with open(path, "w", encoding="utf-8") as f:
                json.dump(definition, f)
            before = metric("unsupported_grains_total") or 0
            route(receiver, domain_id, flow)
            wait_for("unsupported", lambda: channel()["state"] == "unsupported" and channel()["source_flow"] == flow, seconds=5)
            time.sleep(1.0)
            alive(corrector)
            after = metric("unsupported_grains_total") or 0
            if after <= before or channel()["state"] != "unsupported":
                raise SystemExit(f"mismatch {lie_mode}@{lie_rate}: unsupported_grains {before} -> {after}, state {channel()['state']}")
            print(f"mismatch {lie_mode} at {lie_rate}/1 ok: unsupported, {after - before:.0f} counted, no crash")

        # Back to a good interlaced source: running again.
        domain, domain_id, flow = tff_flow
        route(receiver, domain_id, flow)
        wait_for("running again", lambda: channel()["state"] == "whole_grain" and channel()["source_flow"] == flow, seconds=5)
        alive(corrector)
        print("interlaced integration ok")
    finally:
        died = corrector.poll()
        stop(corrector)
        for w in writers:
            stop(w)
        if died is not None:
            print(f"mxl-color-corrector had exited with {died}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
