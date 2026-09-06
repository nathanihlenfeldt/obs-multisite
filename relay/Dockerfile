# Dockerfile — the relay, as one image.
#
# Two stages so the runtime carries a binary, ffmpeg, and nothing else: no
# compiler, no headers, no build cache. The result is small enough to pull
# onto the cheapest VPS over a slow link, which is the deployment this is for.

# ── build ────────────────────────────────────────────────────────────────────
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git \
        libcurl4-openssl-dev libssl-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY relay ./relay

# The plugins and the CMAF muxer are not built here: the relay needs the core
# receive path and the S3 transport, and nothing else. That is what keeps the
# build dependencies down to curl, OpenSSL and SQLite.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DMULTISITE_BUILD_RELAY=ON \
        -DBUILD_OBS_PLUGIN=OFF \
        -DBUILD_PLAYER=OFF \
    && cmake --build build --target multisite-relay -j"$(nproc)"

# ── runtime ──────────────────────────────────────────────────────────────────
FROM debian:bookworm-slim

# ffmpeg is the relay's one external tool. ca-certificates is what lets it
# reach an HTTPS bucket at all — without it every request fails with a
# certificate error that reads like a credentials problem.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ffmpeg ca-certificates libcurl4 libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/relay/multisite-relay /app/multisite-relay
COPY relay/web /app/web

# Everything that must survive a restart lives here: the database and the
# segment cache. Mount it, or a container replacement loses the destinations.
VOLUME ["/data"]
ENV RELAY_DATA_DIR=/data \
    RELAY_WEB_ROOT=/app/web \
    RELAY_PORT=8080
EXPOSE 8080

# No health check on the stream itself on purpose: a relay with nothing to send
# because no service is on air is perfectly healthy, and restarting it would be
# the wrong response.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s \
    CMD ffmpeg -version > /dev/null 2>&1 || exit 1

ENTRYPOINT ["/app/multisite-relay"]
