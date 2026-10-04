# Changelog

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
