# Changelog

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
