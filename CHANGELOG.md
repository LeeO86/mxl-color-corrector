# Changelog

## 1.2.2

- **A source flow that comes back is opened at once, however long it was missing.** While a source flow is missing, the channel looks every 20 ms at its files: the domain's `domain_def.json`, the flow directory and its data file (inodes, three `stat` calls). When they change and the flow is there, it opens the flow at once. MXL publishes a new flow directory with a rename, so a flow that is back is complete. 1.2.1 looked only after an attempt that found no data file. An attempt that failed with the files half there fell back to the retries, which run every second after 800 ms: an old flow still being deleted, or a domain directory made again with its `domain_def.json` written after the flow. That could add up to 1 s of hold. The slower retries are now left for a flow that is there but does not open.
- **No output grain is left invalid by the corrector.** When the output index moved on by more than one grain, MXL marked every skipped output grain `MXL_GRAIN_FLAG_INVALID`, and readers take that as no signal: the multiviewer flashed `no_signal` and replay dropped the frames. This happened in two places:
  - When the channel fell behind its source by more than half the source's ring, it jumped to the head. The late check in 1.0 and later, which half a 200 ms ring triggers after a 100 ms delay of the channel's thread.
  - When a hold started more than 21 grains after the last output grain (1.2.1; the thread was late, or the old flow was deleted slowly).
  Now a gap before the next output grain is filled with the last output grain, up to the output's ring (`output_gap_filled`, at most every 5 s), and the hold fills in its whole start gap the same way. Found by looping `reopen.py` 20 times with every output jump logged. The first 1.2.2 build skipped output grains 23 times in 9 of 20 runs (16 late jumps, 6 at a hold start, 1 at a resume); the test's invalid-grain check saw it in 1 run (a 22-grain jump at a hold start). 1.2.1 passed 20 of 20 runs of the test, which samples only some windows, but it has the same jumps. This build filled 80 gaps in 20 runs and left no invalid grain. Before the hold limit the source never sent an invalid grain. A source grain flagged invalid is still passed through as it is.
- A route to a flow that is not there yet (the corrector started before its source, or a route to a flow still to come) is opened as soon as the flow shows up in a known domain, rather than after the next backoff step (up to 2 s).
- Test: `reopen.py` also checks for invalid output grains after the first re-create (no grain may be invalid before the hold limit). It keeps the writer away for 2 s; the corrector must read it within 0.15 s of its data file appearing. In the passes with a writer behind the clock, the whole domain directory is removed. The writer then makes it again with `domain_def.json` 300 ms after the flow (`mxl-cc-pattern --late-domain-def`), and the corrector must read within 0.15 s of that file appearing. 1.2.1 fails this. Unit test for the file check (`flowFiles`) on a real MXL domain. `mxl-cc-sample --count-invalid` lists the invalid grains; `CC_REOPEN_PASSES` picks passes of `reopen.py`.
- The Compose and Kubernetes examples use the `1.2.2` image. No setting changes.

## 1.2.1

- **A re-created source flow is re-opened at once.** A writer that restarts (a test player, a gateway) re-creates its flow under the same id with a new data file. 1.2.0 read a grain only once the old flow's head had reached it, and that head no longer moved, so the channel never saw MXL's `MXL_ERR_FLOW_INVALID` and its output stood until an IS-05 disable and enable (on small on 2026-10-15 the corrector's output froze for about 2 minutes: the multiviewer input showed `no_signal`, and the replay camera fed from it stopped recording). Now a channel re-opens the source when a read returns `MXL_ERR_FLOW_INVALID`, or when the head has stood for two grain periods and the flow's data file is gone or replaced. It tries at once, then after 100, 200 and 500 ms and every second while the flow is missing, and it opens the flow as soon as its file is back (checked every 20 ms). No IS-05 activation is needed. In the integration test the channel reads the new flow 0.06–0.11 s after the writer starts again, both at once and after a 1 s gap; 1.2.0 never read it.
- **The output holds while the source is gone or stalled.** The channel repeats its last output grain at the grain rate. The status, the UI and `mxl_color_corrector_channel_state` show the new state `holding` (source gone or stalled). A writer killed with its flow left behind counts as stalled after ten grain periods (at least 250 ms) without a write (log `mxl_source_stalled`). A writer that pauses for less and then catches up loses no grain. The held grains keep the output's own timing against the grain clock, so a source that indexes behind the clock (a gateway indexing by capture time) or ahead of it comes back right after them. Output indices only go up (MXL refuses a grain at or below the last one committed). When the hold starts it fills in the time since the last output grain, so no grain is flagged invalid before the limit; a hole right after the resume gets the held picture too. A source that comes back a little later than the held grains (up to the stall threshold plus 8 grains) is waited for (log `mxl_flow_resume_waiting` with `lag_grains`), and the output pauses for the difference. A source further behind, such as a writer indexing by UTC 37 s behind TAI, would never catch up, so the output flow is made again in its timing (log `mxl_output_recreated` with `lag_grains`, same flow id). In the lab behind mxl-test-player 1.1.0 (polled every 70 ms), the output head never went back. It stood still at most 0.11 s across a graceful stop and start (1 s gap), and 0.20 s across a SIGKILL restart (the 250 ms stall threshold; 1.9 s before any stall hold). Behind a pattern writer 12 grains behind the clock, it stood still at most 0.11 s when the writer was re-created. No output grain was refused.
- A grain the source never finished or never wrote is skipped once the head is past it. Before, the channel waited on it until the head was half a ring ahead (about 0.45 s with a 1 s history). That hit every re-open in whole-grain mode, which reads one grain behind the head and so started one grain before the new flow's first. It also hit a brand-new flow with a single grain. In the lab, whole-grain mode behind mxl-test-player 1.1.0 took 0.42 s from the player's first grain to the corrector's first grain without this. With it, the corrector's first grain came within the same 70 ms poll in which the new flow appeared.
- Each channel reads its source on an MXL instance of its own, made again when the route's domain changes or the domain directory is made again. MXL hands out the same reader for a flow id while anyone holds it on an instance, so on the shared instance of 1.2.0 a channel could get a stale reader back while another channel read the same flow.
- **New setting `CC_HOLD_MAX_S`** (default `10`; `0` holds forever). After that long, the held picture turns black and its grains carry `MXL_GRAIN_FLAG_INVALID`. The new state is `no_signal` (log `mxl_hold_expired`); in the lab, 10.06 s after the source went away. The output keeps its grain rate. The multiviewer treats invalid grains as missing and raises `no_signal`, replay does not record them, and other readers see black. The corrector carries no audio.
- An output grain that cannot be written no longer closes and re-opens the reader at once. The channel logs `output grain write failed`, waits 20 ms and re-syncs. This was a busy loop in the first build of this branch: a source 8 or more grains behind the clock made it resume below its held grains, and MXL refused the grain.
- New metric `mxl_color_corrector_flow_reopens_total{channel}` (also `flow_reopens` in the status and a column in Status). New logs `mxl_source_stalled`, `mxl_flow_recreated` (`reason=flow_invalid|flow_replaced`), `mxl_reader_error` (`reason=read_error`, any other failed read; re-opened the same way), `mxl_flow_reopen_failed` (first failed attempt), `mxl_flow_reopened` (`attempts`, `after_ms`), `mxl_flow_resume_waiting` (`lag_grains`), `mxl_output_recreated` (`lag_grains`), `mxl_flow_resumed` (`after_ms`, `held_grains`) and `mxl_hold_expired`.
- Tests:
  - `unit-tests-mxl` on a real MXL domain in a temporary directory: a flow deleted and re-created at once, after a gap and before its first grain; a domain directory made again; a stale reader held by another channel; the schedule.
  - `test_hold.cpp`: held timing, resume never at or below the held grains (with a source 12 grains behind the clock and 20 000 random cases), the limit, black.
  - `tests/integration/reopen.py`: the binary with a paced pattern writer, on the clock and 12 grains behind it, in slice and whole-grain mode. The writer is stopped and started again, killed and started again, and kept away past the limit. Then it is replaced by a writer 37 s behind the clock. No output grain may be flagged invalid before the limit. The first build of this branch fails it with the lagging writer.
  - `mxl-cc-pattern --paced 1 --lag-grains N` writes each grain at its time, N grains behind the clock; `mxl-cc-sample` prints the grain's flags and counts invalid grains (`--count-invalid N`).
- The Compose and Kubernetes examples use the `1.2.1` image.

## 1.2.0

AVX-512 at run time and cheaper clip statistics (plan G). The output is byte for byte the same as 1.1.1's; nothing to configure.

- **AVX-512 at run time.** The colour processing has AVX-512 code on 512-bit registers (`avx512`) and on 256-bit registers (`avx512vl`) next to AVX2 and the scalar reference, chosen at start from cpuid and the OS's saved register state; the image stays plain x86-64. New setting `CC_SIMD` (`auto`, `avx512`, `avx512vl`, `avx2`, `scalar`; default `auto`): `avx512` on Ice Lake and later Xeons, Zen 4 and Zen 5, `avx2` on Skylake-SP and Cascade Lake (the 512-bit frequency licence slows the core's other hyperthread by 3.4–4 %) and on CPUs without AVX-512. A level the CPU lacks falls back with a warning. The level is in the start log, in the status (`simd`) and in the metric `mxl_color_corrector_simd_level{level}`. Every level is bit-exact with the scalar path, clip statistics included (unit tests on every width, edge codes, padding bits and strides; in CI also under Intel SDE as Haswell, Skylake-SP and Ice Lake).
- **Less CPU on every level.** The clip statistics (the clipped-samples ratio) cost the AVX2 path a third of its time: the odd sample's chroma rows were computed again in full. Now the rows share the chroma pair's part, the odd chroma is computed only for the count, and the count is a lane counter per line (a mask popcount with AVX-512); the scalar path counts per line instead of per pixel through a pointer. The gamut clip's inside test uses 16-bit multiplies. Lab Xeon Gold 6136, 2 × 1080p50 bars: 0.199 → 0.166 cores per channel with RGB gamut clip off and 0.841 → 0.725 with it on (`avx2`); `avx512` there: 0.135 and 0.435. Numbers per CPU in `docs/performance.md`.
- `mxl-cc-bench` measures one case per call (`run <level> <matrix|rgb> <camera|bench> <height> <frames> <stats> <seconds> [fps]`) and prints every level this CPU has (`table`).
- The Compose and Kubernetes examples use the `1.2.0` image.

## 1.1.1

- The image's `org.opencontainers.image.version` label is the release version (1.1.0 showed `24.04`, inherited from the Ubuntu base image); untagged builds carry `git-<sha>`. No code change.

## 1.1.0

- **Ranges.** White colour (R, G, B gain trims) ±20 % → ±100 %, black colour (R, G, B pedestal trims) ±5 % → ±100 %, pedestal ±10 % → ±100 %, brightness ±20 % → ±100 %, in the API validation, the UI and the specification. Gain and saturation stay 0–200 %, the wheels ±1. Every corner of the new ranges fits the 32-bit AVX2 path (largest lane sum 1.64·10⁹ of 2.15·10⁹), stays within 1 LSB of the double-precision reference and is legal with `clip=legal` (unit tests over all 1024 corners).
- **RGB pots** as the alternative to the colour wheels, per correction section and kept in the browser: a fader per R, G, B with − and + buttons for 0.1 % steps (held, they repeat), an exact value and a reset; mouse, keyboard and touch. The wheel and the pots set the same trims and show each other's changes: a wheel sets the trims' tint and keeps their luma part (it no longer drops a level set with R = G = B), the wheel position is read back from the trims (a pure luma offset no longer moves the dot), and a tint beyond the wheel's ±20 % / ±5 % reach sits on the marked rim. Numeric trims win over a wheel in the same patch, so a state file, preset or import keeps its trims exactly (1.0 rebuilt them from the stored wheel position and lost the luma part on restart). A trim object may carry one channel only.
- **Web UI** in the look of the other LeeO86 media functions (mxl-webrtc-monitor, mxl-replay, mxl-multiviewer, mxl-srt-gateway, mxl-test-player): header with label, state, bypass and registration pills and the version; banners for a lost API, lost live updates and failed actions; tabs with their own address (Overview, Channel, NMOS, Status, Settings); light and dark theme. Every function stays and more are reachable: the preset list with recall and delete, per-control reset, IS-05 disable, the probes and `/metrics` counters (Status), every setting with its origin plus export and import (Settings). The colour wheel's hues now lie where the correction goes (red right, blue down; 1.0 drew red at the top). Reset asks for a second press.
- **Widgets** for operator screens, the contract of mxl-webrtc-monitor 1.3.0 and mxl-replay 1.4.0: `GET /widgets` (CORS for the `WIDGET_FRAME_ANCESTORS` origins, GET only), `/widget/controls?channel=<n>` (wheels or RGB pots, gain, pedestal, brightness, saturation; 480×360) and `/widget/bypass?channel=<n>` (bypass, A/B, reset; 260×110), with `theme=dark|light|transparent`, `Content-Security-Policy: frame-ancestors` and no `X-Frame-Options`, posting `widget-ready` and `widget-size`. New setting `WIDGET_FRAME_ANCESTORS` (default `'self'`).
- API additions: `GET /api/v1/config` (every setting with value and `source`), `version`, `mxl_revision` and `label` in the status. Preset names in paths are percent-decoded (a name with a space can be recalled and deleted).
- No video previews: the pictures are JPEG thumbnails, so the platform's preview contract (`PREVIEW_*`) does not apply.

### Fixes

- A WebSocket client that went away without a close frame (a closed laptop, a killed browser, a dropped network) could stop the web server answering: the broadcast closed its socket while the client's thread kept reading the number, took the requests of the next connection that got it, and spun on a core. The broadcast now only shuts the socket down and the client's thread closes it; a client that does not take a status message within 1 s is dropped. Unit test.
- The NMOS page's activation form sent IS-05 to `WEB_PORT`, where it was not served unless `NMOS_PORT` was the same port. The IS-04 and IS-05 APIs now answer on `WEB_PORT` too.
- Typing a value into a field no longer gets overwritten by the next status push.

## 1.0.5

- A new output `domain_def.json` carries `description` and `tags`, as BCP-007-03 requires (`id`, `label`, `description`, `tags`). The corrector wrote only `id` and `label`, and mxl-st2110-gateway 1.0.2 skipped such domains. An existing file is still not rewritten. The integration test checks the four fields.

## 1.0.4

RGB gamut clip takes less CPU; the matrix path and all settings are unchanged. Lab host (2× Xeon Gold 6136), 4 channels of 1080p50 test-player bars with gain 1.1, saturation 1.2, pedestal 0.01: 1.89 → 1.33 cores with RGB gamut clip on, 0.79 cores off (unchanged). Bars sit on the faces of the RGB cube, the worst case; pictures mostly inside the cube gain more.

- Pixels clearly inside the RGB cube skip the exact double-precision test: a 32-bit fixed-point R′G′B′ estimate, with a margin larger than its rounding error, shows that the exact test would leave them alone. AVX2 only, eight pixels at a time; the result is the same.
- The Y′CbCr ↔ R′G′B′ conversions multiply by reciprocals instead of dividing, in the scalar reference and the AVX2 path alike (a vector division costs about as much as 16 multiplications). Codes can differ from 1.0.3 by one where the double-precision result lay within a rounding step of .5; the AVX2 path stays bit-exact with the scalar reference.

## 1.0.3

- The NMOS node lists `interfaces` (empty), which the IS-04 v1.3 node schema requires. An nmos-cpp registry rejected the node ("schema validation failed"), then the device, source, flow, sender and receiver ("unknown parent"), so `/readyz` never turned 200 against a real registry and nothing could be connected. Found on the lab host with the platform's registry; the integration test's registry stand-in did not validate. With the field, all resources register.

## 1.0.2

- The NMOS device lists its IS-05 control (`urn:x-nmos:control:sr-ctrl/v1.1`, href `http://NMOS_HOST_ADDRESS:NMOS_PORT/x-nmos/connection/v1.1/`). It registered `"controls": []`, so a controller that finds the Connection API through the device (the platform's production-up and production-down, nmos-crosspoint) could not connect or disable the receivers.

## 1.0.1

- The whole v210 path runs in AVX2: eight 6-pixel groups at a time through a transposed block (unpack, 32-bit fixed-point matrix, clamp, pack). A 1080p50 frame takes 0.7 ms instead of 14 ms on a CI runner; on the lab's Xeon Gold 6136, 1.0.0 needed 27 ms and missed real time. Bit-exact with the scalar reference ([docs/performance.md](docs/performance.md)).
- RGB gamut clip runs in AVX2 as well (double precision, same operations and rounding as the scalar path): 11.8 ms instead of 52.6 ms per 1080p50 frame, so it keeps real time on one core.
- `clipped_ratio` counted chroma clipping only on even samples on AVX2 CPUs and on every sample on the scalar path; both now count like the scalar reference.
- CI runs `mxl-cc-bench` for the change and for `main` on the same runner.

## 1.0.0

Stable settings, APIs and shutdown behaviour for the MXL PoC platform.

- Announce `NMOS_HOST_ADDRESS` (an IP literal) on the IS-04 href and API endpoints. `HOST_ID` remains a label, and an address alias only when its value is already an IP literal. A hostname is no longer announced.
- `NMOS_LABEL` and `NMOS_TAGS` set the node label, the device label prefix, and the node/device tags. Group hints are unchanged.
- `NMOS_QUERY_ADDRESS` and `NMOS_QUERY_PORT` (default: registry address and registry port + 1). `/readyz` stays 503 until the Query API shows the node when a registry is configured.
- `MXL_HISTORY_DURATION_NS` is written once into a new output domain. An existing `domain_def.json` with a different id is not overwritten. Input domains are opened read-only.
- SIGTERM deletes the registry node and, when `MXL_CLEANUP_ON_EXIT=true`, removes this function's output domain. `SHUTDOWN_TIMEOUT_S` defaults to 10. Exit code 143.
- Active IS-05 routes are stored under `CC_STATE_DIR` and restored on startup. `master_enable: false` stops reading.
- `GET /api/v1/config/export` and `POST /api/v1/config/import` cover channels, presets and routes. The settings object in the export is informational; ports and identity still come from the environment. This function has no secrets.
- Container tags: `git-<sha7>` and `nightly-dev` on `main`; `X.Y.Z`, `X.Y` and `X` on `vX.Y.Z`. The image runs as uid 1000.

## 0.1.0

Initial live colour corrector: slice-progressive v210 correction, operator controls, IS-04/IS-05 node, and the admin UI.
