# mxl-color-corrector

Live colour correction for MXL `video/v210` flows. Each channel reads one video flow and writes one corrected flow. White and black balance, master gain and pedestal, brightness and saturation are one affine 3×4 matrix in Y′CbCr, applied slice by slice.

Version 1.0.0 is the stable settings and API contract. A breaking change needs 2.0.0.

## Build

```bash
cd web && npm ci && npm run build && cd ..
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/unit-tests
```

MXL `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` is fetched by CMake. `CC_WITH_MXL=OFF` builds the maths, API and unit tests without it.

## Settings

Environment overrides `CC_CONFIG_FILE`, which overrides the defaults. Unknown environment variables are ignored. An invalid value exits **78**. State the process writes (`state.json`, `routes.json`) lives only under `CC_STATE_DIR` (default `/config`). There are no secrets.

| Key | Default | Meaning |
| --- | --- | --- |
| `CC_CHANNELS` | `2` | Channels, 1–16 |
| `CC_CLIP` | `legal` | `legal`, `extended`, or `off` |
| `CC_RGB_CLIP` | `off` | RGB gamut clip |
| `CC_READ_OFFSET_GRAINS` | `1` | Whole-grain read offset. Slice mode always follows the writer |
| `CC_PREVIEW_FPS` | `4` | Thumbnail rate, 1–30 |
| `CC_LOG_LEVEL` | `info` | `error`, `warn`, `info` |
| `CC_CONFIG_FILE` | unset | JSON object of the keys in this table |
| `CC_STATE_DIR` | `/config` | Writable state directory |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | Parent of domain directories, mirrors included |
| `MXL_OUTPUT_DOMAIN_DIR` | `/Volumes/mxl/cc-<8 hex>` | This function's domain directory |
| `MXL_OUTPUT_DOMAIN_ID` | UUIDv5 of `NMOS_SEED` | This function's domain id |
| `MXL_HISTORY_DURATION_NS` | `200000000` | Written into a new domain's `options.json` only |
| `MXL_CLEANUP_ON_EXIT` | `false` | On shutdown, delete only this function's output domain |
| `NMOS_REGISTRY_ADDRESS` | empty | Static registry. Empty means do not register |
| `NMOS_REGISTRY_PORT` | `3210` | Registration API |
| `NMOS_QUERY_ADDRESS` | registry address | Query API host |
| `NMOS_QUERY_PORT` | registry port + 1 | Query API |
| `NMOS_DNS_SD` | `false` | Must stay false. This build has no DNS-SD browse and no mDNS advertisement |
| `NMOS_PORT` | `3292` | IS-04 and IS-05 HTTP. Nothing listens on `NMOS_PORT+1` |
| `NMOS_SEED` | see below | UUIDv5 seed for node, device, flows, senders, receivers, domain id |
| `NMOS_LABEL` | see below | Node label. Device label is `<label> Color Corrector` when this is set |
| `NMOS_TAGS` | `{}` | JSON object of tag name to array of strings, copied onto the node and device |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | Address announced in the node href and `api.endpoints[].host` |
| `HOST_ID` | hostname | Alias. Used as `NMOS_LABEL` when that is unset. Used as `NMOS_HOST_ADDRESS` only when the value is a non-loopback IP literal |
| `WEB_PORT` | `8140` | Admin UI, REST, `/livez`, `/readyz`, `/metrics`, UI WebSocket |
| `SHUTDOWN_TIMEOUT_S` | `10` | Budget for SIGTERM. The Deployment grace period must be larger |

`NMOS_SEED` defaults to `<HOST_ID>-cc` when `HOST_ID` is set, otherwise `<hostname>-cc`. `NMOS_LABEL` defaults to `HOST_ID` when that is set, otherwise `MXL Color Corrector`.

`NMOS_HOST_ADDRESS` is the only address announced to other systems. It must be an IP literal. `0.0.0.0`, loopback and hostnames are rejected. The UI does not offer a hostname to copy. Browser access to the UI uses the ingress name; that name is not announced on NMOS.

## Ports and exit codes

| Port | Setting | Role |
| --- | --- | --- |
| 8140 | `WEB_PORT` | UI, API, probes, metrics, `WS /api/v1/events` |
| 3292 | `NMOS_PORT` | IS-04 node API and IS-05 connection API |

Two instances on one host need distinct `WEB_PORT` and `NMOS_PORT`. A port that cannot be bound exits **75**.

| Code | When |
| --- | --- |
| 0 | Clean exit (SIGINT) |
| 75 | Temporary failure, including a port that is in use or an MXL domain that cannot be created |
| 78 | Invalid configuration, including an output `domain_def.json` whose id does not match |
| 143 | SIGTERM |

## API

- `GET /livez`, `GET /readyz`, `GET /metrics`, `GET /statusz`
- `GET /api/v1/channels`, `GET/PATCH /api/v1/channels/{n}/controls`
- `POST /api/v1/channels/{n}/bypass`, `/ab`, `/reset`
- Presets: `GET/POST /api/v1/channels/{n}/presets`, `POST …/{name}/recall`, `DELETE …/{name}`, and the same under `/api/v1/presets` for every channel
- `GET /api/v1/channels/{n}/preview/input.jpg` and `output.jpg`
- `GET /api/v1/config/export`, `POST /api/v1/config/import`
- `GET /api/v1/settings`, `GET /api/v1/nmos`
- `WS /api/v1/events`
- IS-04 `/x-nmos/node/v1.3/` and IS-05 `/x-nmos/connection/v1.1/single/` on `NMOS_PORT`; the device lists the IS-05 control `urn:x-nmos:control:sr-ctrl/v1.1` with href `http://NMOS_HOST_ADDRESS:NMOS_PORT/x-nmos/connection/v1.1/`

Import restores channels, presets and IS-05 routes. The `settings` object in an export is a snapshot for `production-export`; import does not change ports or identity, which stay on the environment. There is nothing secret to omit.

## Platform

The image is `ghcr.io/leeo86/mxl-color-corrector:1.0.5`, uid 1000, pod network, MXL root hostPath `/Volumes/mxl`, writable `/config`. `deploy/k8s/deployment.yaml` sets `NMOS_HOST_ADDRESS` from `status.podIP`, `MXL_CLEANUP_ON_EXIT=true`, and `terminationGracePeriodSeconds: 20`. `production-down` should SIGTERM and then see the node and this instance's domain disappear.

## Controls

`PATCH /api/v1/channels/{n}/controls` accepts any subset. A colour wheel is `white_wheel` / `black_wheel` as `{x, y}` in −1…+1. Open UIs stay in sync on the WebSocket, which sends at most 30 updates per second from the page.
