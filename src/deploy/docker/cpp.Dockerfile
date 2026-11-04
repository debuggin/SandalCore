# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM ubuntu:24.04@sha256:80dd3c3b9c6cecb9f1667e9290b3bc61b78c2678c02cbdae5f0fea92cc6734ab AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang-18 llvm-18 lld-18 ccache cmake ninja-build git pkg-config python3 \
    libssl-dev libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    libc-ares-dev libre2-dev libsqlite3-dev libspdlog-dev libcli11-dev nlohmann-json3-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY src/vcpkg.json src/CMakeLists.txt src/CMakePresets.json ./src/
COPY build/gen/ ./build/gen/
COPY build/third_party/ ./build/third_party/
COPY src/ ./src/

ARG TARGET_BINARY=world
RUN --mount=type=cache,target=/root/.cache/ccache \
    mkdir -p /workspace/build/gen/proto /workspace/build/gen/cpp/aoc /workspace/build/gen/go/citynodeconsts && \
    if [ ! -f "/workspace/build/gen/cpp/aoc/citynode_constants_gen.hpp" ]; then \
        INI_SRC="/workspace/src/config/DefaultGame.ini"; \
        if [ ! -f "${INI_SRC}" ]; then INI_SRC="/tmp/DefaultGame.ini"; : > "${INI_SRC}"; fi && \
        python3 /workspace/src/tools/schemagen/gen_node_constants.py \
            --ini "${INI_SRC}" \
            --out-cpp /workspace/build/gen/cpp/aoc/citynode_constants_gen.hpp \
            --out-go /workspace/build/gen/go/citynodeconsts/constants_gen.go; \
    fi && \
    CC=clang-18 CXX=clang++-18 \
    cmake -S src -B build/linux -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DAOC_BUILD_TESTS=OFF \
        -DAOC_PROTO_GEN_DIR=/workspace/build/gen/proto \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/workspace/build/linux/bin && \
    cmake --build build/linux --target world tether --parallel && \
    llvm-strip-18 --strip-unneeded /workspace/build/linux/bin/world /workspace/build/linux/bin/tether && \
    cp /workspace/build/linux/bin/world /workspace/build/linux/bin/worldd && \
    cp /workspace/build/linux/bin/tether /workspace/build/linux/bin/tether_server && \
    cp /workspace/build/linux/bin/${TARGET_BINARY} /workspace/build/linux/bin/service

FROM gcr.io/distroless/cc-debian12:nonroot@sha256:f2c2764a02641e2f90e84909ef124f20b0393b6f7b0f6b18040c8b0f74a2232c AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG TARGET_BINARY=world
ARG WORLDD_UDP_PORT=7777
ARG WORLDD_ADMIN_PORT=50061
ARG TETHER_TCP_PORT=50051
ARG TETHER_ADMIN_PORT=50055
ARG SERVICE_BIND_ADDR=0.0.0.0

LABEL org.opencontainers.image.title="sandal-cpp-runtime" \
      org.opencontainers.image.description="SandalCore C++20 authoritative worldd simulation and tether_server TCP proxy runtime" \
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
COPY --from=builder --chown=65532:65532 /workspace/build/linux/bin/worldd /usr/local/bin/worldd
COPY --from=builder --chown=65532:65532 /workspace/build/linux/bin/tether_server /usr/local/bin/tether_server
COPY --from=builder --chown=65532:65532 /workspace/build/linux/bin/service /usr/local/bin/service
USER 65532:65532
EXPOSE 7777/udp 50061/tcp 50051/tcp 50055/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "--healthcheck", "http://127.0.0.1:50061/livez"]
ENTRYPOINT ["/usr/local/bin/service"]
