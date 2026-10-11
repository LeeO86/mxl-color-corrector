# Implementation notes

## Platform guideline G1–G14

| Id | Status | Evidence |
| --- | --- | --- |
| G1 Configuration | met | Env then `CC_CONFIG_FILE` then defaults, unknown env ignored, invalid values throw `ConfigError` and `main` exits 78 (`src/config.cpp`, `src/main.cpp`). State only under `CC_STATE_DIR` (default `/config`). No secrets. Table in `README.md`. |
| G2 MXL domains | met | Scan parent `MXL_DOMAIN_SCAN_PATH`. Own domain created by `openOwn` (`src/engine.cpp`); a different existing id is logged and not overwritten (exit 78). Input domains use `openReadOnly` (no writes, no garbage collection). `options.json` is written only when absent. `MXL_HISTORY_DURATION_NS`. |
| G3 NMOS identity | met | UUIDv5 from `NMOS_SEED` for node, device, sources, flows, senders, receivers and the default domain id (`src/nmos.cpp` constructor, `src/config.cpp`). `NMOS_LABEL` and `NMOS_TAGS` on the node and device. Group hints stay on the flows. |
| G4 Registry, no DNS-SD | met | `NMOS_REGISTRY_*` and `NMOS_QUERY_*` (query defaults to registry port + 1). `NMOS_DNS_SD` defaults false. There is no DNS-SD browse and no mDNS advertisement, so no Avahi or D-Bus, and the nmos-cpp `pri`/`highest_pri` switch is N/A. |
| G5 Announce addresses | met | `selectHostAddress` (`src/config.cpp`) accepts only a non-loopback IP literal. `HOST_ID` is an alias only when it is already an IP. Href and `api.endpoints[].host` use that address (`src/nmos.cpp`). No SDP, ICE or SRT. The UI does not offer a hostname to copy. |
| G6 Ports | met | `WEB_PORT` and `NMOS_PORT` are the only listeners. Bind failure exits 75 (`src/main.cpp`). Nothing listens on `NMOS_PORT+1` (no NMOS control WebSocket); documented in the README. Since 1.1.0 the IS-04 and IS-05 APIs also answer on `WEB_PORT` (the UI's activation form is same-origin); the announced href stays `NMOS_PORT`. |
| G7 Health and metrics | met | `/livez` is liveness. `/readyz` is 200 only when serving, and when a registry is set only after the Query API returns the node (`src/api.cpp`, `src/nmos.cpp`). Metrics prefix `mxl_color_corrector_`. |
| G8 Clean shutdown | met | SIGTERM calls `Engine::shutdown` (release readers and writers), `NmosNode::deregister` (DELETE the node), then removes the output domain when `MXL_CLEANUP_ON_EXIT=true` (`src/main.cpp`). Exit 143. `SHUTDOWN_TIMEOUT_S` default 10. No child processes. |
| G9 IS-05 | met | Senders expose `mxl_domain_id` and `mxl_flow_id`. Receivers accept staged PATCH with `sender_id`, `master_enable`, `transport_params` and `activate_immediate`. `master_enable: false` makes `route().enable` false and the worker releases the reader. Routes persist in `CC_STATE_DIR/routes.json`. |
| G10 Export and import | met | `GET /api/v1/config/export` and `POST /api/v1/config/import` (`src/api.cpp`). Import applies channels, presets and routes. The settings snapshot is informational. No secrets. Preset endpoints remain. Layouts are N/A (this function has none). |
| G11 Image and CI | met | `.github/workflows/ci.yml` tests. `.github/workflows/publish.yml` pushes `ghcr.io/leeo86/mxl-color-corrector` as `git-<sha7>` and `nightly-dev` on `main`, and `X.Y.Z`, `X.Y`, `X` on `vX.Y.Z` (version tags are not reused). Runtime uid 1000. OCI labels in `docker/Dockerfile`. The Deployment and Compose file reference the release (`1.1.0`). |
| G12 Kubernetes example | met | `deploy/k8s/deployment.yaml`: pod network, standard env, probes, grace 20s, MXL hostPath, `/config`, uid/gid 1000, `supplementalGroups: [1000]`, no `hostIPC`, no extra capabilities. |
| G13 Documentation | met | `README.md` settings, ports, exit codes, API, platform run. `CHANGELOG.md` 1.0.0. `SPECIFICATION.md` configuration table matches the code. |
| G14 Tests | met | `tests/unit/test_config.cpp` covers the new settings and address rules. `tests/unit/test_api.cpp` covers export/import and a restored route. `tests/integration/lifecycle.py` covers start, `/readyz`, SIGTERM, deregistration and domain removal. `tests/integration/correct.py` covers the picture. |

# Implementation notes

Pinned MXL: `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, Fabrics off. The library is built from that git tag inside CMake (same sources the platform pin builds).

## Behaviour choices

* **Slice commits.** On this revision `mxlFlowWriterOpenGrain` clears `validSlices` only when the index is not already the open grain, and `mxlFlowWriterCommitGrain` leaves the grain open while `validSlices < totalSlices`. The worker opens a grain once, writes the newly committed input lines, and commits with an updated `validSlices`. The struct returned by `OpenGrain` is kept so `version`, `size`, `grainSize` and `totalSlices` are not zeroed. Input grain flags, including `MXL_GRAIN_FLAG_INVALID`, are copied onto the output. An invalid grain is copied through, not replaced with black. Flow directories are `<id>.mxl-flow/`, which is the layout this MXL revision writes.

* **Interlaced grain rate.** MXL rejects an interlaced `grain_rate` other than 25/1 or 30000/1001. The corrector reports whole-grain fallback for any non-progressive `interlace_mode` once the flow can be opened.

* **Co-siting.** v210 chroma belongs to the even luma sample. Odd luma uses that same pair. Output chroma is the even sample's result. A neutral matrix with legal samples is a byte copy, including line padding.

* **Matrix.** The RGB affine correction and the saturation scale are folded with the BT.709 matrices into one 3×4 fixed-point matrix (1/65536). Neutral controls force an exact identity. RGB gamut clip is a separate per-pixel step and does not round-trip samples that are already inside 0…1. Its conversions multiply by reciprocals (no divisions); the AVX2 path first tests eight pixels in 32-bit fixed point and skips the exact double-precision test when all are clearly inside the cube (1.0.4, docs/performance.md). `clip=off` still limits codes to the 10-bit container, 0–1023. Extended clip is 4–1019.

* **Read offset.** Slice mode follows the writer's head (offset 0). Whole-grain mode reads `CC_READ_OFFSET_GRAINS` behind the head, default 1, so the grain is complete. Fallback reasons are `interlaced`, `no_slice_commits` (four grains seen only as complete) and `slice_layout` (slice size is not an integer number of v210 lines that divide the frame). The change is logged once.

* **Added latency.** The histogram is the time from the slice becoming visible (`mxlFlowReaderGetGrainSlice` has returned) until `mxlFlowWriterCommitGrain` returns for the lines just written. A later slice does not reprocess lines already committed. The wait for the source to publish the next slice is not included.

* **NMOS.** The node is an in-process IS-04 v1.3 and IS-05 v1.1 implementation with BCP-007-03 `video/v210` and `urn:x-nmos:transport:mxl`, deterministic UUIDv5 ids, and static registry registration. `NMOS_DNS_SD=true` is accepted and logged; this build does not advertise over DNS-SD. Receivers activate even when the domain or flow is missing (`waiting`, backoff to 2 s) and the IS-04 `subscription` follows the activation. A new input geometry mints a new output flow id.

* **Colour wheels and RGB pots (1.1.0).** `+x` is red (Cr), `+y` is blue (Cb). The R, G, B trims are the only parameters; the wheel is a view of their chroma part. `trimLuma` (`Kr·r + Kg·g + Kb·b`) splits the trims into a luma part and a tint; `trimsToWheel` maps the tint back with the scale of `wheelToTrims` (rim = 20 % white, 5 % black towards blue, unchanged so 1.0 clients get the same tint for the same `x, y`) and pins a larger tint to the rim in its direction. A wheel patch keeps the luma part and is clamped to ±100 %; numeric trims in the same patch win; the wheel position is recomputed after every patch (`src/store.cpp`). 1.0 dropped the luma part on every wheel move and on every restart (the stored wheel position won over the stored trims), and read the wheel from `r` and `b` without removing the luma, so R = G = B moved the dot. Decision: the wheel keeps its 1.0 span rather than covering ±100 %, for fine balance and an unchanged meaning of `x, y`; the pots cover the full range with 0.1 % steps. The UI shows the trims as pots or as the wheel per section (localStorage `mxl-color-corrector.view.white|black`) and renders only server state, except while a fader or the wheel is dragged.

* **Ranges (1.1.0).** The operators asked for pedestal and one more control at ±100 %. Every control that is a percentage of the signal level is now ±100 %: white trims (were ±20), black trims (±5), pedestal (±10), brightness (±20). Gain and saturation are ratios and keep 0–200 %. The extreme corner of the matrix (gain 200, saturation 200, pedestal and brightness −100, mixed trims) has a lane sum of 1.64·10⁹ < 2³¹, so `fits32` keeps the AVX2 block path and `buildMatrix` never clamps a coefficient; the scalar path is 64-bit. `clip` still bounds the output. The unit tests walk all 1024 corners (AVX2 = scalar, legal output, 1 LSB of the reference).

* **Web UI and widgets (1.1.0).** `web/src` follows mxl-webrtc-monitor 1.3.0 and mxl-replay 1.4.0 (same `style.css` base, `Pill`, `Segmented`, `IdCode`, the store with WebSocket and polling fallback, `#hash` tabs). Control changes go over the WebSocket, at most 30 per second, merged per channel (a trim object carries only the changed channel, so two pots moved quickly do not undo each other); REST while the socket is down. `/widget/<id>` serves the same single-file page; `main.js` mounts `WidgetPage` for those paths. `HttpResponse::cors` lets the widget routes leave out the API's `Access-Control-Allow-Origin: *`. There is no video preview (JPEG thumbnails only), so the preview contract (`PREVIEW_*`, a MediaMTX child, the transform crop) is not applicable.

* **WebSocket clients (1.1.0).** Only a client's own thread closes its socket; `broadcast` shuts a failing one down (send timeout 1 s) and removes it. Before, `broadcast` closed the descriptor while the thread still polled it, so the number could be reused by a new HTTP connection whose request the old thread then read (empty answers), and a client gone without a close frame made its thread spin.

* **State.** `/config/state.json` (or `CC_STATE_DIR`) stores both A/B slots and presets. If the directory cannot be created the process keeps running and logs a warning.

* **History.** Output domains are created with a 200 ms grain history so a 1080p50 flow stays near 60 MB.

Performance figures are in [docs/performance.md](docs/performance.md). They were measured on the build machine, not on a Dell Precision 3930. A lab run on a Xeon Gold 6136 (2026-10-03, `docs/performance.md`) reached only about 37 grains/s per 1080p50 channel at one core.
