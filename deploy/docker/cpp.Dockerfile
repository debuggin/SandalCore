# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM alpine:3.21@sha256:56fa17d2a7e7f168a043a2712e63aed1f8543aeafdcee47c58dcffe38ed51099 AS builder
RUN apk add --no-cache \
    build-base clang18 llvm18 lld ccache cmake ninja git curl zip unzip tar pkgconf linux-headers \
    openssl-dev grpc-dev protobuf-dev c-ares-dev re2-dev sqlite-dev spdlog-dev cli11-dev nlohmann-json

WORKDIR /src
COPY vcpkg.json CMakeLists.txt CMakePresets.json ./
COPY third_party/recastnavigation/ ./third_party/recastnavigation/
COPY proto/ ./proto/
COPY libs/ ./libs/
COPY tools/ ./tools/
COPY server/ ./server/
COPY tests/ ./tests/

ARG TARGET_BINARY=world
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    --mount=type=cache,target=/root/.cache/ccache \
    CC=clang-18 CXX=clang++-18 \
    cmake --preset linux-release -DAOC_BUILD_TESTS=OFF -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/src/build/linux-release/bin && \
    cmake --build build/linux-release --target ${TARGET_BINARY} --parallel && \
    llvm-strip-18 --strip-unneeded build/linux-release/bin/${TARGET_BINARY}

FROM alpine:3.21@sha256:56fa17d2a7e7f168a043a2712e63aed1f8543aeafdcee47c58dcffe38ed51099 AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
LABEL org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.title="sandal-cpp-runtime"

RUN apk add --no-cache libstdc++ libgcc openssl grpc-cpp protobuf c-ares re2 sqlite-libs spdlog && \
    addgroup -g 65532 -S nonroot && adduser -u 65532 -S -G nonroot -H -D nonroot

ARG TARGET_BINARY=world
WORKDIR /app
COPY --from=builder --chown=65532:65532 /src/build/linux-release/bin/${TARGET_BINARY} /usr/local/bin/service
USER 65532:65532
EXPOSE 7777/udp 7778/udp 50056/tcp 50057/tcp 9091/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=2s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "--healthcheck", "http://127.0.0.1:9091/livez"]
ENTRYPOINT ["/usr/local/bin/service"]
