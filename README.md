# mxl-color-corrector

Live colour correction for MXL `video/v210` flows. Each channel reads one video flow and writes one corrected flow. White and black balance, master gain and pedestal, brightness and saturation are one affine 3×4 matrix in Y′CbCr, applied slice by slice so the chain does not gain a frame of delay.

The operator controls are the broadcast set in [SPECIFICATION.md](SPECIFICATION.md): colour in the whites, colour in the blacks, highlight gain, pedestal, brightness and saturation. They update on the next grain without recreating flows.

## Build

```bash
cd web && npm ci && npm run build && cd ..
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/unit-tests
```

MXL `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` is fetched by CMake. `CC_WITH_MXL=OFF` builds the maths, API and unit tests without it.

## Run

```bash
CC_CHANNELS=2 WEB_PORT=8140 NMOS_PORT=3292 ./build/mxl-color-corrector
```

The admin UI is on `WEB_PORT` (default 8140). The IS-04 / IS-05 node is on `NMOS_PORT` (default 3292). Invalid configuration exits 78. A port that is already taken exits 75. SIGTERM exits 143.

Environment overrides `CC_CONFIG_FILE`, which overrides the defaults. See the configuration table in the specification. Settings are stored in `CC_STATE_DIR` (`/config` by default).

Route a receiver with an IS-05 `activate_immediate` PATCH. `mxl_domain_id` and `mxl_flow_id` are UUIDs. If the domain or flow is not on `MXL_DOMAIN_SCAN_PATH` yet, the channel stays `waiting` and retries.

## Controls

`PATCH /api/v1/channels/{n}/controls` accepts any subset. Percent fields use the ranges in the specification. A colour wheel is `white_wheel` / `black_wheel` as `{x, y}` in −1…+1 (`+x` toward red, `+y` toward blue) and is kept luminance-neutral. `POST …/bypass`, `POST …/ab` and `POST …/reset` cover the panel actions. Presets are per channel and global. Open UIs stay in sync on `WS /api/v1/events`.

Default settings copy legal v210 bit-exactly. Output is clipped to legal range unless `clip` is `extended` or `off`.
