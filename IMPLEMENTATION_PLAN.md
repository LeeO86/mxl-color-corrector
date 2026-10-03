# Implementation notes

## Platform guideline G1–G14

| Id | Status | Evidence |
| --- | --- | --- |
| G1 Configuration | met | Env then `CC_CONFIG_FILE` then defaults, unknown env ignored, invalid values throw `ConfigError` and `main` exits 78 (`src/config.cpp`, `src/main.cpp`). State only under `CC_STATE_DIR` (default `/config`). No secrets. Table in `README.md`. |
| G2 MXL domains | met | Scan parent `MXL_DOMAIN_SCAN_PATH`. Own domain created by `openOwn` (`src/engine.cpp`); a different existing id is logged and not overwritten (exit 78). Input domains use `openReadOnly` (no writes, no garbage collection). `options.json` is written only when absent. `MXL_HISTORY_DURATION_NS`. |
| G3 NMOS identity | met | UUIDv5 from `NMOS_SEED` for node, device, sources, flows, senders, receivers and the default domain id (`src/nmos.cpp` constructor, `src/config.cpp`). `NMOS_LABEL` and `NMOS_TAGS` on the node and device. Group hints stay on the flows. |
| G4 Registry, no DNS-SD | met | `NMOS_REGISTRY_*` and `NMOS_QUERY_*` (query defaults to registry port + 1). `NMOS_DNS_SD` defaults false. There is no DNS-SD browse and no mDNS advertisement, so no Avahi or D-Bus, and the nmos-cpp `pri`/`highest_pri` switch is N/A. |
| G5 Announce addresses | met | `selectHostAddress` (`src/config.cpp`) accepts only a non-loopback IP literal. `HOST_ID` is an alias only when it is already an IP. Href and `api.endpoints[].host` use that address (`src/nmos.cpp`). No SDP, ICE or SRT. The UI does not offer a hostname to copy. |
| G6 Ports | met | `WEB_PORT` and `NMOS_PORT` are the only listeners. Bind failure exits 75 (`src/main.cpp`). Nothing listens on `NMOS_PORT+1` (no NMOS control WebSocket); documented in the README. |
| G7 Health and metrics | met | `/livez` is liveness. `/readyz` is 200 only when serving, and when a registry is set only after the Query API returns the node (`src/api.cpp`, `src/nmos.cpp`). Metrics prefix `mxl_color_corrector_`. |
| G8 Clean shutdown | met | SIGTERM calls `Engine::shutdown` (release readers and writers), `NmosNode::deregister` (DELETE the node), then removes the output domain when `MXL_CLEANUP_ON_EXIT=true` (`src/main.cpp`). Exit 143. `SHUTDOWN_TIMEOUT_S` default 10. No child processes. |
| G9 IS-05 | met | Senders expose `mxl_domain_id` and `mxl_flow_id`. Receivers accept staged PATCH with `sender_id`, `master_enable`, `transport_params` and `activate_immediate`. `master_enable: false` makes `route().enable` false and the worker releases the reader. Routes persist in `CC_STATE_DIR/routes.json`. |
| G10 Export and import | met | `GET /api/v1/config/export` and `POST /api/v1/config/import` (`src/api.cpp`). Import applies channels, presets and routes. The settings snapshot is informational. No secrets. Preset endpoints remain. Layouts are N/A (this function has none). |
| G11 Image and CI | met | `.github/workflows/ci.yml` tests. `.github/workflows/publish.yml` pushes `ghcr.io/leeo86/mxl-color-corrector` as `git-<sha7>` and `nightly-dev` on `main`, and `X.Y.Z`, `X.Y`, `X` on `vX.Y.Z` (version tags are not reused). Runtime uid 1000. OCI labels in `docker/Dockerfile`. The Deployment and Compose file reference `1.0.0`. |
| G12 Kubernetes example | met | `deploy/k8s/deployment.yaml`: pod network, standard env, probes, grace 20s, MXL hostPath, `/config`, uid/gid 1000, `supplementalGroups: [1000]`, no `hostIPC`, no extra capabilities. |
| G13 Documentation | met | `README.md` settings, ports, exit codes, API, platform run. `CHANGELOG.md` 1.0.0. `SPECIFICATION.md` configuration table matches the code. |
| G14 Tests | met | `tests/unit/test_config.cpp` covers the new settings and address rules. `tests/unit/test_api.cpp` covers export/import and a restored route. `tests/integration/lifecycle.py` covers start, `/readyz`, SIGTERM, deregistration and domain removal. `tests/integration/correct.py` covers the picture. |

# Implementation notes

Pinned MXL: `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, Fabrics off. The library is built from that git tag inside CMake (same sources the platform pin builds).

## Behaviour choices

* **Slice commits.** On this revision `mxlFlowWriterOpenGrain` clears `validSlices` only when the index is not already the open grain, and `mxlFlowWriterCommitGrain` leaves the grain open while `validSlices < totalSlices`. The worker opens a grain once, writes the newly committed input lines, and commits with an updated `validSlices`. The struct returned by `OpenGrain` is kept so `version`, `size`, `grainSize` and `totalSlices` are not zeroed. Input grain flags, including `MXL_GRAIN_FLAG_INVALID`, are copied onto the output. An invalid grain is copied through, not replaced with black. Flow directories are `<id>.mxl-flow/`, which is the layout this MXL revision writes.

* **Interlaced grain rate.** MXL rejects an interlaced `grain_rate` other than 25/1 or 30000/1001. The corrector reports whole-grain fallback for any non-progressive `interlace_mode` once the flow can be opened.

* **Co-siting.** v210 chroma belongs to the even luma sample. Odd luma uses that same pair. Output chroma is the even sample's result. A neutral matrix with legal samples is a byte copy, including line padding.

* **Matrix.** The RGB affine correction and the saturation scale are folded with the BT.709 matrices into one 3×4 fixed-point matrix (1/65536). Neutral controls force an exact identity. RGB gamut clip is a separate per-pixel step and does not round-trip samples that are already inside 0…1. `clip=off` still limits codes to the 10-bit container, 0–1023. Extended clip is 4–1019.

* **Read offset.** Slice mode follows the writer's head (offset 0). Whole-grain mode reads `CC_READ_OFFSET_GRAINS` behind the head, default 1, so the grain is complete. Fallback reasons are `interlaced`, `no_slice_commits` (four grains seen only as complete) and `slice_layout` (slice size is not an integer number of v210 lines that divide the frame). The change is logged once.

* **Added latency.** The histogram is the time from the slice becoming visible (`mxlFlowReaderGetGrainSlice` has returned) until `mxlFlowWriterCommitGrain` returns for the lines just written. A later slice does not reprocess lines already committed. The wait for the source to publish the next slice is not included.

* **NMOS.** The node is an in-process IS-04 v1.3 and IS-05 v1.1 implementation with BCP-007-03 `video/v210` and `urn:x-nmos:transport:mxl`, deterministic UUIDv5 ids, and static registry registration. `NMOS_DNS_SD=true` is accepted and logged; this build does not advertise over DNS-SD. Receivers activate even when the domain or flow is missing (`waiting`, backoff to 2 s) and the IS-04 `subscription` follows the activation. A new input geometry mints a new output flow id.

* **Colour wheels.** `+x` is red (Cr), `+y` is blue (Cb). The resulting RGB trims are a luma-neutral chroma offset scaled so a full deflection stays inside the trim range. Numeric RGB fields may leave that plane; the wheel position is then the closest Cr/Cb reading.

* **State.** `/config/state.json` (or `CC_STATE_DIR`) stores both A/B slots and presets. If the directory cannot be created the process keeps running and logs a warning.

* **History.** Output domains are created with a 200 ms grain history so a 1080p50 flow stays near 60 MB.

Performance figures are in [docs/performance.md](docs/performance.md). They were measured on the build machine, not on a Dell Precision 3930. A lab run on a Xeon Gold 6136 (2026-10-03, `docs/performance.md`) reached only about 37 grains/s per 1080p50 channel at one core.
