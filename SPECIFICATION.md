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
  (AVX2, AVX-512VL and AVX-512 chosen at run time by `CC_SIMD`, with a scalar reference path
  used in tests). No GPU required.
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
| **White colour** | "Farbe in Weiss" — white balance | R, G, B gain trims: tint the whites without moving black | each −100 % … +100 % | 0 % |
| **Black colour** | "Farbe in Schwarz" — black balance | R, G, B pedestal trims: tint the blacks without moving white | each −100 % … +100 % of full range | 0 % |
| **Gain** (master) | highlight correction | scales the signal from black: whites move, black stays | 0 % … 200 % | 100 % |
| **Pedestal** (master) | lowlight correction, "Pedestal verschieben" | shifts the black level; the effect fades to zero at white, so whites stay | −100 % … +100 % | 0 % |
| **Brightness** | "Helligkeit" | shifts the whole picture up/down (black and white move together) | −100 % … +100 % | 0 % |
| **Saturation** | "Farbsättigung" | scales chroma; 0 % is black-and-white | 0 % … 200 % | 100 % |

Definitions (normalised RGB, 0 = black, 1 = white, per channel c ∈ {R, G, B}):

```
gain_c      = master_gain × (1 + white_trim_c)
pedestal_c  = master_pedestal + black_trim_c
out_c       = pedestal_c + (gain_c − pedestal_c) × in_c + brightness
```

So at `in = 0` the output is the pedestal (black level/colour), at `in = 1` it is
the gain (white level/colour), and brightness offsets both. The ranges of the trims,
pedestal and brightness were ±20 %, ±5 %, ±10 % and ±20 % up to 1.0.5; operators needed
the whole signal range (1.1.0). Gain and saturation are ratios and keep 0 … 200 %. Saturation is then
applied as a scale of Cb/Cr around zero (luma unchanged). Percentages in the UI are
relative to the nominal range (BT.709 narrow range: Y 64–940, C 64–960 at 10 bit).

### 4.2 Colour wheels

For "white colour" and "black colour" the UI offers a **colour wheel each** (like a
camera control panel): dragging the point towards a hue tints the whites/blacks
towards that hue; the wheel maps to the three R/G/B trims (with the trims kept
luminance-neutral, i.e. dragging the wheel does not change brightness). Numeric
R/G/B fields remain available for exact values. A "neutral" button resets a wheel.

The **RGB pots** (1.1.0) are the alternative to a wheel, picked per section and kept in
the browser: one fader per channel R, G, B with − and + buttons for fine steps (0.1 %,
repeated while held), usable with a mouse, a keyboard and a touch screen.

Both views set the same parameters, the three trims, and always show the same state:

- trims = luma part (equal on R, G, B: `Kr·r + Kg·g + Kb·b`) + tint (the chroma part);
- the wheel position is the tint, `x` = Cr, `y` = Cb, scaled so the rim is a tint of
  20 % (white) or 5 % (black) towards blue, as in 1.0; the server reads it back from the
  trims after every change;
- moving the wheel replaces the tint and keeps the luma part (clamped to ±100 %); moving a
  pot changes one trim, and the wheel follows;
- a tint beyond the wheel's reach (only possible with the pots) is shown on the rim in its
  direction and the rim is marked;
- numeric trims in the same change win over a wheel position (state files, presets and
  exports carry both).

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
  status and metrics) when: the input is interlaced (§5.4), the input does not provide
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

### 5.3 Source gone, stalled or re-created (1.2.1)

A writer that restarts (a test player, a camera gateway) re-creates its flow under the same
id: MXL deletes the flow with its last writer, and the new writer makes a new data file. A
reader still holds the old file; its head stands, and a read past it returns
`MXL_ERR_FLOW_INVALID` (MXL `218ddaa`, `PosixDiscreteFlowReader.cpp:96-104`, `244-254`).

- **Re-open.** The channel re-opens the source at once, without an IS-05 activation, when a
  read returns `MXL_ERR_FLOW_INVALID`, or when the head has stood for two grain periods (at
  least 20 ms) and the flow's data file is gone or has another inode (checked every 20 ms
  while the head stands; the channel waits on the head, so it would not read past it by
  itself). While the flow is missing it tries again after 100, 200 and 500 ms, then every
  second. For the whole time it is missing, it also looks every 20 ms at the flow's files: the
  domain's `domain_def.json`, the flow directory and its data file (inodes, three `stat` calls).
  When they change and the flow is there, it opens the flow at once (1.2.2; 1.2.1 looked only
  after an attempt that found no data file). MXL publishes a flow directory with a rename, so a
  flow that is back is complete. The slower retries are left for a flow that is there but does
  not open. A route to a flow that is not there yet is opened in the same way, as soon as the
  flow shows up in a known domain. Any other failed read re-opens the source the same way
  (`mxl_reader_error`, `reason=read_error`).
- **Stall.** A writer that stops without deleting its flow (killed, flow directory left) leaves
  a valid flow that is no longer written. After no write (slice commits count) for ten grain
  periods, at least 250 ms, the channel holds (`mxl_source_stalled`). A writer that pauses
  for less and then catches up in sequence loses no grain and adds no delay; the output
  pauses with it. A restart takes 1–2 s, so it is still bridged.
- **Hold** (state `holding`: source gone or stalled). The channel repeats its last output grain
  at the grain rate, in the output's own timing: the held grains keep the distance to the
  grain clock that the output had at its last grain (`hold.hpp`), also for a source that
  indexes behind the clock (a gateway indexing by capture time) or ahead of it. When the hold
  starts it fills in the time since the last output grain (within the fill cap, below).
- **No holes.** MXL marks every grain a writer skips `MXL_GRAIN_FLAG_INVALID`, and readers take
  that as no signal. So whenever the next output grain is more than one past the last, the gap is
  filled with the last output grain (`output_gap_filled`, at most every 5 s; 1.2.2). One fill
  writes at most twice the catch-up (42 grains at 50p) and at most 256 MB (12 grains at 2160p),
  the newest grains, so it never blocks the channel long. The older grains of a longer gap stay
  invalid; readers that follow the head never reach them. This covers the channel falling behind
  (the late check jumps to the head), a source grain that is skipped, the start of a hold, and
  the resume after it. A source grain flagged invalid is passed through as it is (§5.1).
- **Limit.** After `CC_HOLD_MAX_S` (default 10 s; 0 holds forever) the held picture turns
  black (Y 64, Cb and Cr 512) and the grains carry `MXL_GRAIN_FLAG_INVALID`
  (state `no_signal`, log `mxl_hold_expired`). The output flow keeps its grain rate. MXL
  readers that treat invalid grains as missing raise their no-signal alarm (the
  multiviewer after `MV_HOLD_MS`); replay does not record them; other readers see black.
  The corrector carries no audio.
- **Resume.** Output grain indices only ever go up: MXL refuses a grain at or below the last
  one committed (`PosixDiscreteFlowWriter::openGrain` → `MXL_ERR_INVALID_ARG`). The output
  resumes at the source's head (less the whole-grain offset) or right after the held grains,
  whichever is later. A source that comes back running a little later than the held grains (a
  new writer with more delay than the old one, up to the catch-up bound: the stall threshold
  plus 8 grains, 21 at 50p) is waited for without holding further (`mxl_flow_resume_waiting`
  with `lag_grains`). The output then pauses for that difference instead of going back. The
  hold limit still counts during the wait. A source further behind (a writer indexing by UTC
  is 37 s behind TAI) would never catch up while the output keeps its timing. The output flow
  is then made again in the source's timing (`mxl_output_recreated` with `lag_grains`, same
  flow id), and readers re-open it as a re-created flow. Grains of a late writer that the held
  ones already cover are skipped. A grain
  that is still too early while the head is past it (left unfinished, or never written: a
  re-created flow starts at its own head, and whole-grain mode reads one grain behind it)
  is skipped instead of waited for. An output grain that cannot be written does not close
  the reader: the channel waits 20 ms and re-syncs (`output grain write failed`). A
  re-created flow with another format mints a new output flow id (§6).
- **Instances.** Each channel reads on an MXL instance of its own. MXL gives the same reader
  back for a flow id while anyone holds it on that instance (`Instance.cpp:121-143`), so a
  channel re-opening a flow that another channel also reads would get the stale reader back
  on a shared instance. The instance follows the route's domain and is made again when the
  domain directory was removed and made again.
- **Logs and metrics.**
  - `mxl_source_stalled`;
  - `mxl_flow_recreated` (once per event, `reason=flow_invalid|flow_replaced`);
  - `mxl_reader_error` (`reason=read_error`);
  - `mxl_flow_reopen_failed` (the first failed attempt of an event);
  - `mxl_flow_reopened` (`attempts`, `after_ms`);
  - `mxl_flow_resume_waiting` (`lag_grains`);
  - `mxl_output_recreated` (`lag_grains`);
  - `mxl_flow_resumed` (`after_ms`, `held_grains`);
  - `mxl_hold_expired`;
  - metric `flow_reopens_total`.

### 5.4 Interlaced input (1.2.3)

MXL (`218ddaa`, `FlowParser.cpp`) stores a flow with `interlace_mode` `interlaced_tff` or
`interlaced_bff` as fields: `grain_rate` must be 25/1 or 30000/1001 (the frame rate, as IS-04
has it), MXL doubles it to index fields, and each grain is one field of `frame_height / 2`
lines (`height / 2` slices). MXL refuses any other interlace mode, `interlaced_psf` too.

- **Field in, field out.** The correction is per pixel, so a channel corrects each field grain
  as `frame_height / 2` lines. Its output flow declares the input's `interlace_mode`,
  `grain_rate` and frame size, so MXL gives it the same field grains at the field rate; output
  grain index = input grain index, so the field order passes through (even indices are the
  first field: top for tff, bottom for bff).
- **Timing in fields.** The hold, the gap fill, the stall threshold, the catch-up and the head
  probe (§5.3) use the flow's grain rate, the field rate.
- Interlaced input runs in whole-grain mode (`fallback_reason` `interlaced`): one field of delay.
- The preview shows a field at the frame's aspect.
- **Grain check.** Every source grain and every output grain is checked against the declared
  format: at least `lines × v210 line length` bytes and exactly `lines` slices (v210: one slice per
  line). The flow's grain rate (`mxlFlowInfo.config`) must be the declared one (doubled when
  interlaced). A mismatch is never read or written past the grain's end: the grain is skipped,
  the state is `unsupported`, `unsupported_grains_total` counts it (once per grain period for a
  rate mismatch), and `unsupported_grain` is logged with the sizes at most every 5 s. The next
  grain that fits, or a new route, runs again. 1.2.2 read a 1080i field as 1080 lines and
  crashed (SIGSEGV).
- **NMOS.** The output flow and sender carry the input's `interlace_mode`. The receiver's caps
  have two constraint sets: progressive at 24000/1001 … 60/1, and `interlaced_tff` or
  `interlaced_bff` at 25/1 and 30000/1001.

---

## 6. NMOS

- One Node, one Device ("MXL Color Corrector"); deterministic UUIDv5 IDs.
- Per channel: one video **receiver** (BCP-007-03, `video/v210`) and one video
  **sender** with IS-04 Source and Flow, grouped by group hint
  `<channel label>:Video`.
- Receiver activations are accepted even if the domain or flow does not exist yet
  (`waiting`, retry with backoff); the IS-04 `subscription` is updated. A source flow
  re-created under the same id is re-opened without an activation (§5.3).
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
  `whole-grain fallback`, `holding` (source gone or stalled), `no_signal` (held past
  `CC_HOLD_MAX_S`), §5.3, `unsupported` (grains that do not match the flow's format, §5.4)),
  clip indicator.
- **Overview**: all channels with state, routed source label, bypass/A-B state.
- **NMOS** and **Settings** pages like the siblings.
- 1.1.0: the LeeO86 family look (mxl-webrtc-monitor, mxl-replay, mxl-multiviewer,
  mxl-srt-gateway, mxl-test-player): header with label, state and registration pills and the
  version, banners, tabs with their own address (`#overview`, `#channel`, `#nmos`, `#status`,
  `#settings`), light and dark theme. The Channel page adds the RGB pots (§4.2), per-control
  reset, and the preset list (recall, delete); Status shows the probes and the counters of
  `/metrics`; Settings shows every setting with its origin (`GET /api/v1/config`) and the
  export and import. The NMOS APIs answer on `WEB_PORT` too, so the activation form works.
- The pictures are JPEG thumbnails at `CC_PREVIEW_FPS`. There is no video preview, so the
  platform's preview contract (`PREVIEW_PUBLISH_URL`, `PREVIEW_WHEP_URL`, …) does not apply.
- REST: `GET/PATCH /api/v1/channels/{n}/controls` (any subset of controls, values
  validated against ranges), presets CRUD, `POST …/bypass`, `POST …/ab`;
  WebSocket `/api/v1/events` for live state and control changes, so several open
  UIs stay in sync. Control changes from the UI are sent at most 30 times per
  second.
- `/livez`, `/readyz`, `/statusz`, `/metrics` on `WEB_PORT`.

### 7.1 Widgets (1.1.0, operator screens)

The contract of mxl-webrtc-monitor 1.3.0 §6.6 and mxl-replay 1.4.0 §8.7.

- `GET /widgets` answers `[{id, title, params, min_size: {w, h}, version}]` (`params` a
  JSON schema of the query). It carries `Access-Control-Allow-Origin` for an `Origin`
  that `WIDGET_FRAME_ANCESTORS` lists (or `*`), and `Vary: Origin`; GET only (405 otherwise).
- `controls` (`channel`, required, 1..`CC_CHANNELS`; `min_size` 480×360): white and black
  colour, each as a wheel or RGB pots (§4.2), and the gain, pedestal, brightness and
  saturation faders of one channel. Clip settings stay on the Channel page.
- `bypass` (`channel`, required; `min_size` 260×110): bypass on and off, the A/B slot, and
  reset of every control (a second press within 3 s confirms).
- `GET /widget/<id>?channel=<n>[&theme=dark|light|transparent]` is the embedded page with
  only that widget, no app chrome, on the corrector's own API (same origin). An invalid
  parameter answers 400, an unknown widget 404. These routes carry `Content-Security-Policy:
  frame-ancestors <WIDGET_FRAME_ANCESTORS>` (default `'self'`; `;`, `,` or control
  characters exit 78), no `X-Frame-Options`, and not the API's CORS headers.
- The page posts `{type: "widget-ready"}` once it shows the channel, and
  `{type: "widget-size", w, h}` then and on every resize, to `window.parent`.

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
| `CC_HOLD_MAX_S` | 10 (s a channel holds its last picture while the source is gone or stalled, then black flagged invalid; 0 holds forever; §5.3) |
| `CC_SIMD` | `auto` (`avx512`, `avx512vl`, `avx2`, `scalar`; see the README) |
| `SHUTDOWN_TIMEOUT_S` | 10 |
| `WIDGET_FRAME_ANCESTORS` | `'self'` (§7.1) |
| `CC_LOG_LEVEL` | `info` |
| `CC_CONFIG_FILE` | unset |
| `HOST_ID` | hostname; label alias, and address alias only when it is an IP literal |

`/readyz` is 200 when the process is serving and, if `NMOS_REGISTRY_ADDRESS` is set, the node is visible on the Query API. `/livez` is process liveness. SIGTERM deregisters the node, releases MXL readers and writers, removes the output domain when `MXL_CLEANUP_ON_EXIT=true`, and exits 143. `GET/POST /api/v1/config/export` and `/import` round-trip channels, presets and IS-05 routes. There are no secrets in this function.

---

## 9. Metrics (prefix `mxl_color_corrector_`)

Per channel: `channel_state`, `grains_processed_total`, `slice_mode` (1/0),
`fallback_reason` (info), `added_latency_seconds` (histogram: input slice commit →
output slice commit), `processing_seconds` (histogram per grain),
`late_grains_total`, `clipped_ratio`, `bypass`, `settings_changes_total`,
`flow_reopens_total` (source flows re-opened after they were deleted or re-created under the
same id, §5.3; 1.2.1); `channel_state` has the states `holding` and `no_signal` (1.2.1) and
`unsupported` (1.2.3); `unsupported_grains_total` (grains skipped because they do not match the
flow's format, §5.4; 1.2.3); process CPU. Grafana dashboard in `deploy/grafana/`.

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
  - settings swap happens only at grain boundaries;
  - on a real MXL domain in a temporary directory (`unit-tests-mxl`): a flow deleted and
    re-created under the same id, at once or after a gap, before or after its first grain,
    and with its domain directory made again, is caught as stale and read again after a
    re-open, also while another channel holds the stale reader; the re-open schedule;
  - the held output (`test_hold.cpp`): held grains keep the output's timing for a source
    behind or ahead of the clock; a re-created source 12 grains behind the clock resumes
    right after the held grains and never at or below them (also over 20 000 random cases);
    `CC_HOLD_MAX_S` and the black picture;
  - interlaced input (`test_interlace.cpp`, and `test_interlaced.cpp` on a real MXL domain): the
    field geometry and rate, the grain check, the mirrored output flow_def (MXL makes it 540-line
    fields at 50/1 for 1080i50), the preview aspect, the NMOS caps and flow (1.2.3).
- Integration (CI): a test writer (mxl-test-player or a small tool) produces a
  1080p50 flow with slice commits; the corrector channel is activated via IS-05;
  bars and greyscale ramp are measured at the output for a set of control changes;
  added latency stays below one frame; interlaced input triggers the fallback and
  it is reported. `tests/integration/reopen.py` (1.2.1): in slice and whole-grain
  mode, with a writer on the clock and one 12 grains behind it, the source writer stops and
  starts again under the same flow id (at once and after 1 s), is killed and started again,
  stays away past `CC_HOLD_MAX_S` (2 s there), and stays away 2 s and must be read within
  0.15 s of its data file appearing. In the lagging passes the whole domain directory is
  removed and made again with `domain_def.json` 300 ms after the flow, and the flow must be read
  within 0.15 s of that (1.2.2). The channel reads the new flow within 1 s
  each time and counts one re-open per re-created flow. Its output head advances while the
  source is away, turns black and invalid after the limit, and never goes back. No output grain
  is refused, and the process stays below 0.6 cores after a re-create.
  `tests/integration/interlaced.py` (1.2.3): 1080i50 tff and bff; the output declares the
  input's mode at 25/1 and carries 540-line grains at 50/1, even grains are the first field, a
  gain moves both fields, the output runs at 50 grains/s with no invalid grain and holds at that
  rate; flows whose `flow_def.json` does not match their grains leave the channel
  `unsupported`, counted, running.
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
