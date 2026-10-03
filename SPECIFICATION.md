# mxl-color-corrector — Specification

Status: Draft v0.1 (for implementation by Claude Code or Cursor in a new, empty repository)
Repository: `LeeO86/mxl-color-corrector` (name can still change)
Aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-st2110-gateway`, `LeeO86/mxl-fabrics-agent`,
`LeeO86/mxl-multiviewer`, `LeeO86/mxl-webrtc-monitor`, `LeeO86/mxl-test-player`,
platform repo `mmz-srf/mxl-poc-platform`

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

---

## 1. Purpose and scope

`mxl-color-corrector` corrects the colour of MXL video flows live, in the way a
camera operator or vision engineer works: white and black colour balance,
brightness, saturation, and highlight/lowlight correction by moving gain and
pedestal. It has one or more **channels**; each channel reads one MXL video flow
and writes one corrected MXL video flow.

The control set comes from the operators' requirements (§4). It is deliberately
small and broadcast-style, not a grading tool.

Design principles:

1. **Slice-progressive processing.** The corrector processes each grain slice by
   slice as the source commits it, and commits its own output slice by slice. It
   adds no frame of delay to the chain (§5).
2. **Exact, cheap maths.** All controls are affine operations in RGB, so the whole
   correction collapses into one 3×4 matrix applied directly to Y′CbCr, without a
   YUV↔RGB round trip per pixel (§5.2).
3. **Live control.** Every change applies on the next slice, without redeploying,
   re-creating flows or interrupting the picture.
4. **Never leaves legal range** unless explicitly allowed (§4.4).
5. **Standard NMOS, platform conventions** like the sibling repos: config model,
   admin UI, `/metrics`, exit codes, CI, Compose and Kubernetes, uid 1000, MXL root
   `/Volumes/mxl`, pod network.

Out of scope for v1: per-channel gamma, hue, multi-zone balance, curves, 3D LUTs,
HDR, secondary (qualified) correction, audio/ANC processing.

Reference: the Bobi.Studio `color_corrector` plugin (GPL-3.0) solved slice-mode
processing on MXL. It MAY be read for behaviour; **no code may be copied** (this
repo is MIT).

---

## 2. Architecture

```
 per channel:
   MXL reader (slice-aware) ─► affine 3×4 on Y′CbCr (SIMD) ─► legal clip ─► MXL writer (slice commits)
                                   ▲
   control state (atomic swap per grain) ◄── web UI / REST / WebSocket / presets
 per process: nmos-cpp node, web server, /metrics
```

- One worker thread per channel (optionally pinned); channels are independent.
- The control state is a small immutable struct (the computed matrix plus clip
  settings). The UI/API builds a new one and swaps it atomically; the worker picks
  it up **at the start of a grain**, so one grain is never processed with two
  different settings.

---

## 3. Technology and build

- C++20, CMake ≥ 3.24, Ninja; GCC ≥ 12 or Clang ≥ 16.
- MXL `release/v1.1` at `218ddaa` (platform pin), Fabrics OFF; nmos-cpp at the
  siblings' commit.
- CPU only. The inner loop works on v210 (10-bit 4:2:2) words directly, with SIMD
  (AVX2, with a scalar reference path used in tests). No GPU required.
- Web UI: Vue 3 SPA embedded, no CDN.
- Tests: doctest (vendored), shell integration tests.
- Follow the real MXL API for slice reading and partial commits (the fabrics agent's
  implementation plan records a `validSlices` commit fix — check how slice commits
  behave in the pinned revision). Record every deviation in `IMPLEMENTATION_PLAN.md`.

---

## 4. Controls

All controls are per channel, live, with a reset to default per control and for
the whole channel.

### 4.1 Operator controls (requirement)

| Control | Operator term | What it does | Range / unit | Default |
| --- | --- | --- | --- | --- |
| **White colour** | "Farbe in Weiss" — white balance | R, G, B gain trims: tint the whites without moving black | each −20 % … +20 % | 0 % |
| **Black colour** | "Farbe in Schwarz" — black balance | R, G, B pedestal trims: tint the blacks without moving white | each −5 % … +5 % of full range | 0 % |
| **Gain** (master) | highlight correction | scales the signal from black: whites move, black stays | 0 % … 200 % | 100 % |
| **Pedestal** (master) | lowlight correction, "Pedestal verschieben" | shifts the black level; the effect fades to zero at white, so whites stay | −10 % … +10 % | 0 % |
| **Brightness** | "Helligkeit" | shifts the whole picture up/down (black and white move together) | −20 % … +20 % | 0 % |
| **Saturation** | "Farbsättigung" | scales chroma; 0 % is black-and-white | 0 % … 200 % | 100 % |

Definitions (normalised RGB, 0 = black, 1 = white, per channel c ∈ {R, G, B}):

```
gain_c      = master_gain × (1 + white_trim_c)
pedestal_c  = master_pedestal + black_trim_c
out_c       = pedestal_c + (gain_c − pedestal_c) × in_c + brightness
```

So at `in = 0` the output is the pedestal (black level/colour), at `in = 1` it is
the gain (white level/colour), and brightness offsets both. Saturation is then
applied as a scale of Cb/Cr around zero (luma unchanged). Percentages in the UI are
relative to the nominal range (BT.709 narrow range: Y 64–940, C 64–960 at 10 bit).

### 4.2 Colour wheels

For "white colour" and "black colour" the UI offers a **colour wheel each** (like a
camera control panel): dragging the point towards a hue tints the whites/blacks
towards that hue; the wheel maps to the three R/G/B trims (with the trims kept
luminance-neutral, i.e. dragging the wheel does not change brightness). Numeric
R/G/B fields remain available for exact values. A "neutral" button resets a wheel.

### 4.3 Operation

- **Bypass** per channel (passes input unchanged; the output flow stays the same).
- **Presets**: save/recall named settings per channel and globally; import/export
  as JSON.
- **A/B**: two settings slots per channel, toggled with one button for comparison.
- **Fine/coarse** stepping in the UI (e.g. arrow keys, shift for coarse).
- Settings persist across restarts (state file under `/config`).

### 4.4 Legal range protection

- Output clipped to legal Y′CbCr range by default (Y 64–940, C 64–960 at 10 bit);
  configurable `clip=legal|extended|off` (extended = 4–1019).
- Optional **RGB gamut clip** (`rgb_clip=on`): limits values that would produce
  out-of-gamut RGB. This needs a per-pixel RGB check and is therefore off by
  default; its cost is measured and documented.
- A per-channel indicator in the UI and a metric show the share of clipped samples
  in the last second.

---

## 5. Processing

### 5.1 Slice-progressive, grain by grain

- Output grain index = input grain index; the output flow has the same rate and
  geometry as the input.
- The worker waits for the next committed slice of the input grain, processes it,
  and commits the corresponding output slice before moving on. Because the
  correction is line-local (each output pixel depends only on the same input pixel),
  the result MUST be byte-identical to processing the full grain at once — this is
  a tested property (§10).
- Added latency target: one slice plus processing time, not one frame.
- **Fallback to whole-grain processing** (logged once per change and shown in
  status and metrics) when: the input is interlaced, the input does not provide
  slice commits, or the frame height does not divide into the slice layout. Whole
  grain then adds up to one grain of latency.
- Grain metadata (timestamps, flags such as invalid/gap) is carried over unchanged;
  an invalid/gap input grain produces the same flag on the output, not a corrected
  black frame.

### 5.2 Maths

- The RGB-domain affine correction (§4.1) and the saturation scale are combined with
  the BT.709 Y′CbCr↔R′G′B′ matrices into **one 3×4 matrix in the Y′CbCr domain**
  (fixed-point, 1/4096 or finer), computed once per settings change.
- 4:2:2 handling: each output sample needs the other components at its position.
  For luma samples, use the co-sited chroma pair; for chroma samples, use the
  co-sited luma sample (v210 chroma is co-sited with the first of the two luma
  samples). Document the choice and test that a neutral setting is bit-exact.
- Default settings produce a bit-exact copy of the input (identity matrix,
  unchanged values even with clipping on, as long as the input is legal).
- Operates on BT.709 narrow-range video. Other matrices are out of scope in v1;
  the flow's colorimetry from `flow_def.json` is checked and a warning is shown if
  it is not BT.709.

---

## 6. NMOS

- One Node, one Device ("MXL Color Corrector"); deterministic UUIDv5 IDs.
- Per channel: one video **receiver** (BCP-007-03, `video/v210`) and one video
  **sender** with IS-04 Source and Flow, grouped by group hint
  `<channel label>:Video`.
- Receiver activations are accepted even if the domain or flow does not exist yet
  (`waiting`, retry with backoff); the IS-04 `subscription` is updated.
- The output flow is written into the corrector's own domain (stable ID, never a
  mirror domain). Its `flow_def.json` mirrors the input format; a change of the input
  format mints a new output flow ID and updates the IS-04 Flow and the sender's
  active params (the crosspoint follows).
- Audio and ANC are not processed: downstream functions take them from the source
  directly. Because the corrector adds less than one frame of delay in slice mode,
  lip sync is not affected. (Audio/ANC pass-through is a candidate, §12.)
- Static registry, DNS-SD off by default.

---

## 7. Web UI and API

- **Channel page**: thumbnail (low-rate JPEG) of input and output side by side, with
  a split/wipe view option; the two colour wheels; sliders for gain, pedestal,
  brightness, saturation; bypass, A/B, presets, reset; state (`waiting`, `running`,
  `whole-grain fallback`), clip indicator.
- **Overview**: all channels with state, routed source label, bypass/A-B state.
- **NMOS** and **Settings** pages like the siblings.
- REST: `GET/PATCH /api/v1/channels/{n}/controls` (any subset of controls, values
  validated against ranges), presets CRUD, `POST …/bypass`, `POST …/ab`;
  WebSocket `/api/v1/events` for live state and control changes, so several open
  UIs stay in sync. Control changes from the UI are sent at most 30 times per
  second.
- `/livez`, `/readyz`, `/statusz`, `/metrics` on `WEB_PORT`.

---

## 8. Configuration

Env > JSON file (`CC_CONFIG_FILE`) > defaults; invalid configuration exits 78.

| Key | Default |
| --- | --- |
| `CC_CHANNELS` | 2 (max 16) |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` |
| `MXL_OUTPUT_DOMAIN_DIR` / `MXL_OUTPUT_DOMAIN_ID` | `/Volumes/mxl/cc-<seed-short>` / UUIDv5 of `NMOS_SEED` |
| `MXL_HISTORY_DURATION_NS` | `200000000` |
| `MXL_CLEANUP_ON_EXIT` | `false` |
| `CC_CLIP` | `legal` |
| `CC_RGB_CLIP` | `off` |
| `CC_READ_OFFSET_GRAINS` | 0 in slice mode (follow the writer), 1 in whole-grain mode |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / 3210 |
| `NMOS_QUERY_ADDRESS` / `NMOS_QUERY_PORT` | registry address / registry port + 1 |
| `NMOS_DNS_SD` | `false` (no browse and no mDNS; this build has neither) |
| `NMOS_PORT` | 3292 (node and connection APIs; no listener on `NMOS_PORT+1`) |
| `NMOS_SEED` | `HOST_ID-cc` when `HOST_ID` is set, otherwise `<hostname>-cc` |
| `NMOS_LABEL` | `HOST_ID` when that is set, otherwise `MXL Color Corrector` |
| `NMOS_TAGS` | empty JSON object |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4; `HOST_ID` is an alias when it is an IP literal |
| `WEB_PORT` | 8140 |
| `CC_STATE_DIR` | `/config` |
| `CC_PREVIEW_FPS` | 4 |
| `SHUTDOWN_TIMEOUT_S` | 10 |
| `CC_LOG_LEVEL` | `info` |
| `CC_CONFIG_FILE` | unset |
| `HOST_ID` | hostname; label alias, and address alias only when it is an IP literal |

`/readyz` is 200 when the process is serving and, if `NMOS_REGISTRY_ADDRESS` is set, the node is visible on the Query API. `/livez` is process liveness. SIGTERM deregisters the node, releases MXL readers and writers, removes the output domain when `MXL_CLEANUP_ON_EXIT=true`, and exits 143. `GET/POST /api/v1/config/export` and `/import` round-trip channels, presets and IS-05 routes. There are no secrets in this function.

---

## 9. Metrics (prefix `mxl_color_corrector_`)

Per channel: `channel_state`, `grains_processed_total`, `slice_mode` (1/0),
`fallback_reason` (info), `added_latency_seconds` (histogram: input slice commit →
output slice commit), `processing_seconds` (histogram per grain),
`late_grains_total`, `clipped_ratio`, `bypass`, `settings_changes_total`;
process CPU. Grafana dashboard in `deploy/grafana/`.

---

## 10. Testing

- Unit:
  - neutral settings are bit-exact on random legal v210 input;
  - each control against a floating-point RGB reference implementation
    (max error ≤ 1 LSB at 10 bit);
  - gain moves white but not black; pedestal moves black but not white; brightness
    moves both; saturation 0 % gives Cb = Cr = 512; colour wheels are
    luminance-neutral;
  - legal clipping limits; RGB gamut clip on known out-of-gamut values;
  - slice processing equals whole-grain processing byte for byte, for several
    slice layouts;
  - settings swap happens only at grain boundaries.
- Integration (CI): a test writer (mxl-test-player or a small tool) produces a
  1080p50 flow with slice commits; the corrector channel is activated via IS-05;
  bars and greyscale ramp are measured at the output for a set of control changes;
  added latency stays below one frame; interlaced input triggers the fallback and
  it is reported.
- NMOS conformance: AMWA IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01.
- Performance (documented): channels per Precision 3930 core at 1080p50 and
  2160p50, with and without RGB gamut clip.

## 11. Deployment

Pod network; MXL root `hostPath`; uid 1000; no GPU; `/config` volume; CPU requests
per channel from the measured performance. Compose demo (registry, mxl-test-player
with bars, the corrector, mxl-webrtc-monitor showing input and output) and a
Kubernetes Deployment that `mxl-poc-platform` can vendor. CI and tags as the
siblings (`ghcr.io/leeo86/mxl-color-corrector`), label `io.dmf.mxl.revision`.
Exit codes 0, 75, 78, 143.

## 12. Candidates for later

- Master gamma and per-channel gamma (midtone control).
- Hue rotation.
- Audio and ANC pass-through with the same timing.
- Scopes in the UI (waveform, vectorscope) from the output.
- Control from a hardware panel (e.g. Skaarhoj) or Ember+.
