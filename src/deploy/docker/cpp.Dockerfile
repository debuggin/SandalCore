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
COPY src/CMakeLists.txt src/CMakePresets.json ./src/
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
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_BUILD_TYPE=Release \
        -DAOC_BUILD_TESTS=ON \
        -DAOC_PROTO_GEN_DIR=/workspace/build/gen/proto \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/workspace/build/bin && \
    cmake --build build/linux --target world test_world_live_e2e test_world_loop test_mesh_handover_hysteresis test_world_area test_world_region test_world_gather test_world_foliage test_world_foliage_cells test_world_spawn_filter test_world_bootstrap test_world_move_checker test_world_movement test_world_combat test_world_ability_pipeline test_world_ability_pipeline_chains test_world_ability_pipeline_casts --parallel && \
    mkdir -p /workspace/build/bin /workspace/build/linux/bin && \
    for bin in world test_world_live_e2e test_world_loop test_mesh_handover_hysteresis test_world_area test_world_region test_world_gather test_world_foliage test_world_foliage_cells test_world_spawn_filter test_world_bootstrap test_world_move_checker test_world_movement test_world_combat test_world_ability_pipeline test_world_ability_pipeline_chains test_world_ability_pipeline_casts; do \
        FOUND_BIN="$(find /workspace/dist /workspace/build -type f -name "${bin}" | head -n 1)"; \
        if [ -z "${FOUND_BIN}" ] && [ "${bin}" = "test_world_foliage_cells" ] && [ -f "/workspace/build/bin/test_world_foliage" ]; then \
            cp /workspace/build/bin/test_world_foliage /workspace/build/bin/test_world_foliage_cells; \
            FOUND_BIN="/workspace/build/bin/test_world_foliage_cells"; \
        fi; \
        if [ -n "${FOUND_BIN}" ] && [ "${FOUND_BIN}" != "/workspace/build/bin/${bin}" ]; then \
            cp "${FOUND_BIN}" "/workspace/build/bin/${bin}"; \
        fi; \
        llvm-strip-18 --strip-unneeded "/workspace/build/bin/${bin}"; \
        cp "/workspace/build/bin/${bin}" "/workspace/build/linux/bin/${bin}"; \
    done && \
    cp /workspace/build/bin/world /workspace/build/linux/bin/service

ARG TARGET_BINARY=world

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS world
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 libsqlite3-0 ca-certificates && \
    rm -rf /var/lib/apt/lists/*
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG TARGET_BINARY=world
ARG WORLD_UDP_PORT=7777
ARG WORLD_ADMIN_PORT=50061
ARG SERVICE_BIND_ADDR=0.0.0.0

LABEL org.opencontainers.image.title="sandal-cpp-runtime" \
      org.opencontainers.image.description="SandalCore C++20 authoritative world simulation runtime" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV TARGET_BINARY=${TARGET_BINARY} \
    WORLD_UDP_PORT=${WORLD_UDP_PORT} \
    WORLD_ADMIN_PORT=${WORLD_ADMIN_PORT} \
    SERVICE_BIND_ADDR=${SERVICE_BIND_ADDR}

WORKDIR /app
COPY --from=builder --chown=65532:65532 /workspace/build/bin/world /app/world
COPY --from=builder --chown=65532:65532 /workspace/build/bin/world /usr/local/bin/world
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_live_e2e /app/test_world_live_e2e
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_live_e2e /usr/local/bin/test_world_live_e2e
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_loop /app/test_world_loop
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_loop /usr/local/bin/test_world_loop
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_mesh_handover_hysteresis /app/test_mesh_handover_hysteresis
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_mesh_handover_hysteresis /usr/local/bin/test_mesh_handover_hysteresis
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_area /app/test_world_area
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_area /usr/local/bin/test_world_area
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_region /app/test_world_region
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_region /usr/local/bin/test_world_region
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_gather /app/test_world_gather
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_gather /usr/local/bin/test_world_gather
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_foliage /app/test_world_foliage
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_foliage /usr/local/bin/test_world_foliage
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_foliage_cells /app/test_world_foliage_cells
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_foliage_cells /usr/local/bin/test_world_foliage_cells
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_spawn_filter /app/test_world_spawn_filter
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_spawn_filter /usr/local/bin/test_world_spawn_filter
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_bootstrap /app/test_world_bootstrap
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_bootstrap /usr/local/bin/test_world_bootstrap
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_move_checker /app/test_world_move_checker
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_move_checker /usr/local/bin/test_world_move_checker
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_movement /app/test_world_movement
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_movement /usr/local/bin/test_world_movement
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_combat /app/test_world_combat
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_combat /usr/local/bin/test_world_combat
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_ability_pipeline /app/test_world_ability_pipeline
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_ability_pipeline /usr/local/bin/test_world_ability_pipeline
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_ability_pipeline_chains /app/test_world_ability_pipeline_chains
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_ability_pipeline_chains /usr/local/bin/test_world_ability_pipeline_chains
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_ability_pipeline_casts /app/test_world_ability_pipeline_casts
COPY --from=builder --chown=65532:65532 /workspace/build/bin/test_world_ability_pipeline_casts /usr/local/bin/test_world_ability_pipeline_casts
COPY --from=builder --chown=65532:65532 /workspace/build/gen/fixtures /workspace/build/gen/fixtures
COPY --from=builder --chown=65532:65532 /workspace/build/linux/bin/service /usr/local/bin/service
USER 65532:65532
EXPOSE 7777/udp 50061/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/app/world", "--healthcheck", "http://127.0.0.1:50061/livez"]
ENTRYPOINT ["/app/world"]
