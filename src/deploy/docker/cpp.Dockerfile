# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang-18 llvm-18 lld-18 ccache cmake ninja-build git pkg-config \
    libssl-dev libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    libc-ares-dev libre2-dev libsqlite3-dev libspdlog-dev libcli11-dev nlohmann-json3-dev && \
    mkdir -p /usr/lib/x86_64-linux-gnu/cmake/unofficial-sqlite3 && \
    printf 'find_package(SQLite3 REQUIRED)\nif(NOT TARGET unofficial::sqlite3::sqlite3)\n  add_library(unofficial::sqlite3::sqlite3 ALIAS SQLite::SQLite3)\nendif()\n' \
        > /usr/lib/x86_64-linux-gnu/cmake/unofficial-sqlite3/unofficial-sqlite3-config.cmake && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY src/vcpkg.json src/CMakeLists.txt src/CMakePresets.json ./src/
COPY build/gen/ ./build/gen/
COPY build/third_party/ ./build/third_party/
COPY scripts/wsl/gen-node-constants.sh ./scripts/wsl/gen-node-constants.sh
COPY src/ ./src/

RUN mkdir -p /workspace/build/gen/cpp/aocproto && \
    if ls /workspace/src/config/proto/*.proto >/dev/null 2>&1; then \
        protoc -I /workspace/src/config/proto \
            --cpp_out=/workspace/build/gen/cpp/aocproto \
            --grpc_out=/workspace/build/gen/cpp/aocproto \
            --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
            /workspace/src/config/proto/*.proto; \
    fi && \
    if ls /workspace/src/libs/aocproto/proto/*.proto >/dev/null 2>&1; then \
        protoc -I /workspace/src/libs/aocproto/proto \
            --cpp_out=/workspace/build/gen/cpp/aocproto \
            --grpc_out=/workspace/build/gen/cpp/aocproto \
            --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
            /workspace/src/libs/aocproto/proto/*.proto; \
    fi && \
    if [ -d "/workspace/src/libs/aocproto/proto/internal" ]; then \
        protoc -I /workspace/src/libs/aocproto/proto/internal \
            --cpp_out=/workspace/build/gen/cpp/aocproto \
            --grpc_out=/workspace/build/gen/cpp/aocproto \
            --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
            /workspace/src/libs/aocproto/proto/internal/aoc/*/*/*.proto; \
    fi

RUN --mount=type=cache,target=/root/.cache/ccache \
    mkdir -p /workspace/build/gen/proto /workspace/build/gen/world/world /workspace/build/gen/go/aoc/nodeconstants && \
    cp -r /workspace/build/gen/cpp/aocproto/* /workspace/build/gen/proto/ && \
    if [ ! -f "/workspace/build/gen/world/world/node_constants.h" ]; then \
        sh /workspace/scripts/wsl/gen-node-constants.sh \
            --cpp-out /workspace/build/gen/world/world/node_constants.h \
            --go-out /workspace/build/gen/go/aoc/nodeconstants/node_constants.go; \
    fi && \
    CC=clang-18 CXX=clang++-18 \
    cmake -S src -B build/linux -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DAOC_BUILD_TESTS=OFF \
        -DAOC_PROTO_GEN_DIR=/workspace/build/gen/proto \
        -DAOC_PROTO_GEN_DIR=/workspace/build/gen/cpp/aocproto \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/workspace/build/bin && \
    (cmake --build build/linux --target world tether --parallel || cmake --build build/linux --target worldd tether --parallel) && \
    mkdir -p /workspace/build/bin && \
    WORLD_BIN="$(find /workspace/dist /workspace/build -type f \( -name world -o -name worldd \) | head -n 1)" && \
    TETHER_BIN="$(find /workspace/dist /workspace/build -type f -name tether | head -n 1)" && \
    if [ "${WORLD_BIN}" != "/workspace/build/bin/world" ]; then cp "${WORLD_BIN}" /workspace/build/bin/world; fi && \
    cp /workspace/build/bin/world /workspace/build/bin/worldd && \
    if [ "${TETHER_BIN}" != "/workspace/build/bin/tether" ]; then cp "${TETHER_BIN}" /workspace/build/bin/tether; fi && \
    llvm-strip-18 --strip-unneeded /workspace/build/bin/world /workspace/build/bin/worldd /workspace/build/bin/tether && \
    mkdir -p /workspace/build/linux/bin && \
    cp /workspace/build/bin/world /workspace/build/linux/bin/world && \
    cp /workspace/build/bin/world /workspace/build/linux/bin/worldd && \
    cp /workspace/build/bin/tether /workspace/build/linux/bin/tether && \
    cp /workspace/build/bin/tether /workspace/build/linux/bin/tether_server

ARG TARGET_BINARY=world
RUN if [ "${TARGET_BINARY}" = "tether" ] || [ "${TARGET_BINARY}" = "tether_server" ]; then \
        cp /workspace/build/bin/tether /workspace/build/linux/bin/service; \
    else \
        cp /workspace/build/bin/world /workspace/build/linux/bin/service; \
    fi

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS tether
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 libsqlite3-0 ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder --chown=65532:65532 /workspace/build/bin/tether /app/tether
COPY --from=builder --chown=65532:65532 /workspace/build/bin/tether /usr/local/bin/tether
RUN ln -s /app/tether /app/tether_server && ln -s /usr/local/bin/tether /usr/local/bin/tether_server
USER 65532:65532
EXPOSE 50051/tcp 50055/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/app/tether", "--healthcheck", "http://127.0.0.1:50055/livez"]
ENTRYPOINT ["/app/tether"]

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS world
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 libsqlite3-0 ca-certificates && \
    rm -rf /var/lib/apt/lists/*
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG TARGET_BINARY=world
ARG WORLDD_UDP_PORT=7777
ARG WORLDD_ADMIN_PORT=50061
ARG TETHER_TCP_PORT=50051
ARG TETHER_ADMIN_PORT=50055
ARG SERVICE_BIND_ADDR=0.0.0.0

LABEL org.opencontainers.image.title="sandal-cpp-runtime" \
      org.opencontainers.image.description="SandalCore C++20 authoritative world simulation and tether TCP proxy runtime" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV TARGET_BINARY=${TARGET_BINARY} \
    WORLDD_UDP_PORT=${WORLDD_UDP_PORT} \
    WORLDD_ADMIN_PORT=${WORLDD_ADMIN_PORT} \
    TETHER_TCP_PORT=${TETHER_TCP_PORT} \
    TETHER_ADMIN_PORT=${TETHER_ADMIN_PORT} \
    SERVICE_BIND_ADDR=${SERVICE_BIND_ADDR}

WORKDIR /app
COPY --from=builder --chown=65532:65532 /workspace/build/bin/world /app/world
COPY --from=builder --chown=65532:65532 /workspace/build/bin/world /usr/local/bin/world
COPY --from=builder --chown=65532:65532 /workspace/build/bin/tether /app/tether
COPY --from=builder --chown=65532:65532 /workspace/build/bin/tether /usr/local/bin/tether
COPY --from=builder --chown=65532:65532 /workspace/build/linux/bin/service /usr/local/bin/service
RUN ln -s /app/world /app/worldd && ln -s /usr/local/bin/world /usr/local/bin/worldd && \
    ln -s /app/tether /app/tether_server && ln -s /usr/local/bin/tether /usr/local/bin/tether_server
USER 65532:65532
EXPOSE 7777/udp 50061/tcp 50051/tcp 50055/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/app/world", "--healthcheck", "http://127.0.0.1:50061/livez"]
ENTRYPOINT ["/app/world"]
