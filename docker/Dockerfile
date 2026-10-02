# syntax=docker/dockerfile:1
# MXL pin: 218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7 (release/v1.1, Fabrics off).

FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/build -j"$(nproc)" \
    && cmake --install /tmp/build --prefix /usr/local \
    && /tmp/build/unit-tests

FROM ubuntu:24.04 AS runtime
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 1000 --create-home cc
COPY --from=build /usr/local/bin/mxl-color-corrector /usr/local/bin/mxl-cc-pattern /usr/local/bin/mxl-cc-sample /usr/local/bin/
USER 1000
EXPOSE 8140 3292
LABEL io.dmf.mxl.revision="${MXL_REF}"
ENTRYPOINT ["/usr/local/bin/mxl-color-corrector"]
