# mxl-color-corrector

Live colour correction for MXL `video/v210` flows. Each channel reads one video flow and writes one corrected flow. White and black balance, master gain and pedestal, brightness and saturation are one affine 3×4 matrix in Y′CbCr, applied slice by slice. The web UI has colour wheels or RGB pots per correction section, and operator screens can frame the `controls` and `bypass` widgets.

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
| `WEB_PORT` | `8140` | Admin UI, REST, `/livez`, `/readyz`, `/metrics`, UI WebSocket, widgets |
| `WIDGET_FRAME_ANCESTORS` | `'self'` | CSP `frame-ancestors` of the `/widget` pages; `/widgets` answers these origins with CORS. `;`, `,` or control characters exit 78 |
| `SHUTDOWN_TIMEOUT_S` | `10` | Budget for SIGTERM. The Deployment grace period must be larger |

`NMOS_SEED` defaults to `<HOST_ID>-cc` when `HOST_ID` is set, otherwise `<hostname>-cc`. `NMOS_LABEL` defaults to `HOST_ID` when that is set, otherwise `MXL Color Corrector`.

`NMOS_HOST_ADDRESS` is the only address announced to other systems. It must be an IP literal. `0.0.0.0`, loopback and hostnames are rejected. The UI does not offer a hostname to copy. Browser access to the UI uses the ingress name; that name is not announced on NMOS.

## Ports and exit codes

| Port | Setting | Role |
| --- | --- | --- |
| 8140 | `WEB_PORT` | UI, API, probes, metrics, `WS /api/v1/events`, widgets; also the IS-04 and IS-05 APIs (for the UI) |
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
- `GET /api/v1/config`: every setting as `{key, value, source}`, `source` being `environment`, `file` or `default`
- `GET /api/v1/settings`, `GET /api/v1/nmos`
- `GET /api/v1/status` (also `/statusz`): `version`, `mxl_revision`, `label`, the channels and the NMOS summary
- `WS /api/v1/events`
- `GET /widgets`, `GET /widget/controls?channel=<n>`, `GET /widget/bypass?channel=<n>` (with `&theme=dark|light|transparent`), see [Widgets](#widgets)
- IS-04 `/x-nmos/node/v1.3/` and IS-05 `/x-nmos/connection/v1.1/single/` on `NMOS_PORT`, and on `WEB_PORT` for the UI's activation form; the device lists the IS-05 control `urn:x-nmos:control:sr-ctrl/v1.1` with href `http://NMOS_HOST_ADDRESS:NMOS_PORT/x-nmos/connection/v1.1/`

Import restores channels, presets and IS-05 routes. The `settings` object in an export is a snapshot for `production-export`; import does not change ports or identity, which stay on the environment. There is nothing secret to omit.

## Platform

The image is `ghcr.io/leeo86/mxl-color-corrector:1.1.0`, uid 1000, pod network, MXL root hostPath `/Volumes/mxl`, writable `/config`. `deploy/k8s/deployment.yaml` sets `NMOS_HOST_ADDRESS` from `status.podIP`, `MXL_CLEANUP_ON_EXIT=true`, and `terminationGracePeriodSeconds: 20`. `production-down` should SIGTERM and then see the node and this instance's domain disappear.

## Controls

`PATCH /api/v1/channels/{n}/controls` accepts any subset; a value out of range answers 400 and changes nothing. Percentages are of the nominal range (Y 64–940), gain and saturation of unity.

| Control | Field | Range | Default |
| --- | --- | --- | --- |
| White colour (R, G, B gain trims) | `white: {r, g, b}` | −100 … +100 % (was ±20 % before 1.1.0) | 0 |
| Black colour (R, G, B pedestal trims) | `black: {r, g, b}` | −100 … +100 % (was ±5 %) | 0 |
| Gain | `gain` | 0 … 200 % | 100 |
| Pedestal | `pedestal` | −100 … +100 % (was ±10 %) | 0 |
| Brightness | `brightness` | −100 … +100 % (was ±20 %) | 0 |
| Saturation | `saturation` | 0 … 200 % | 100 |
| Colour wheels | `white_wheel`, `black_wheel: {x, y}` | −1 … +1 | 0 |

The R, G, B trims are the parameters. A colour wheel is a view of their tint: `+x` is red (Cr), `+y` is blue (Cb), and its rim is a luma-neutral tint of 20 % (white) or 5 % (black), as in 1.0. Sending a wheel position sets the trims' tint and keeps their luma part (what R, G and B share), so a wheel never changes brightness; the wheel position in every answer is read back from the trims. A tint beyond the wheel's reach (set with the trims) is shown on the rim in its direction. When a patch has both `white` and `white_wheel`, the trims win. A trim object may name one channel only, e.g. `{"white": {"g": 4}}`.

Clipping keeps the output legal at any setting (`clip`: `legal` Y 64–940 and C 64–960, `extended` 4–1019, `off` 0–1023). Every corner of the ranges fits the 32-bit fixed-point path; the unit tests check each one against the double-precision reference.

## Web UI

Tabs with their own address: **Overview** (every channel with its output picture, state, source, processing mode, A/B and clipping), **Channel** (input and output pictures with a wipe; bypass, A/B and reset; white and black colour as a wheel or as RGB pots; gain, pedestal, brightness and saturation faders; clip and RGB gamut clip; presets of the channel and of every channel with save, recall and delete), **NMOS** (node, registration, receivers and senders, IS-05 activation and disable), **Status** (probes, versions, per channel counters from `/metrics`) and **Settings** (every setting with its origin; export and import). Light and dark follow the browser.

Each colour section has a *Wheel* / *RGB* switch, kept in the browser. An RGB pot is a slider with − and + buttons for 0.1 % steps (held, they repeat), an exact value field and a reset; it works with a mouse, a keyboard and a touch screen. The wheel and the pots move the same trims, so each shows what the other set. Reset asks for a second press. Open UIs and widgets stay in sync on the WebSocket; the page sends at most 30 changes per second.

## Widgets

Operator screens (the platform's production designer) frame single controls of the corrector:

| Widget | Query | Minimum size | Shows |
| --- | --- | --- | --- |
| `controls` | `channel` (required) | 480×360 | white and black colour (wheel or RGB pots), gain, pedestal, brightness and saturation |
| `bypass` | `channel` (required) | 260×110 | bypass on and off, the A/B slot, reset (press twice) |

`GET /widgets` lists them with a JSON schema of their parameters; it answers an `Origin` that `WIDGET_FRAME_ANCESTORS` names (or `*`) with `Access-Control-Allow-Origin` and `Vary: Origin`, and answers GET only. `GET /widget/<id>?channel=<n>[&theme=dark|light|transparent]` is the page without the app around it, on the corrector's own API; a bad parameter answers 400, an unknown widget 404. Only these routes carry `Content-Security-Policy: frame-ancestors <WIDGET_FRAME_ANCESTORS>` (default `'self'`), and none carries `X-Frame-Options`. The page posts `{type: "widget-ready"}` and `{type: "widget-size", w, h}` to its parent.

The pictures are low-rate JPEG thumbnails (`CC_PREVIEW_FPS`), not video: the corrector has no WebRTC or HLS preview, so the platform's preview settings (`PREVIEW_*`) do not apply.
