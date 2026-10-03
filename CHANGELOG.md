# Changelog

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
