# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM golang:1.24-alpine@sha256:f8d0e5861e26f2b403198c24eb87d76d01d0a81db48f36a7a939e1a8f5e0c2b4 AS builder
RUN apk add --no-cache git protobuf protobuf-dev protoc python3

WORKDIR /workspace
COPY build/gen/go/ ./build/gen/go/
COPY src/server/services/go.mod src/server/services/go.sum ./src/server/services/

WORKDIR /workspace/src/server/services
RUN --mount=type=cache,target=/go/pkg/mod \
    go mod download && go mod verify

WORKDIR /workspace
COPY src/ ./src/

RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    if [ ! -d "/workspace/build/gen/go/aoc/proto/v1" ] && [ ! -d "/workspace/build/gen/go/aoc/auth/v1" ]; then \
        mkdir -p /workspace/build/gen/go && \
        if ls /workspace/src/libs/protocol/proto/*.proto >/dev/null 2>&1; then \
            protoc -I /workspace/src/libs/protocol/proto \
                --go_out=/workspace/build/gen/go --go_opt=paths=source_relative \
                --go-grpc_out=/workspace/build/gen/go --go-grpc_opt=paths=source_relative \
                /workspace/src/libs/protocol/proto/*.proto; \
        fi && \
        if [ -d "/workspace/src/libs/aocproto/proto/internal" ]; then \
            protoc -I /workspace/src/libs/aocproto/proto/internal \
                --go_out=/workspace/build/gen/go --go_opt=paths=source_relative \
                --go-grpc_out=/workspace/build/gen/go --go-grpc_opt=paths=source_relative \
                /workspace/src/libs/aocproto/proto/internal/aoc/*/*/*.proto; \
        fi; \
    fi && \
    if [ ! -f "/workspace/build/gen/go/citynodeconsts/constants_gen.go" ]; then \
        mkdir -p /workspace/build/gen/go/citynodeconsts /workspace/build/gen/cpp/aoc && \
        INI_SRC="/workspace/src/config/DefaultGame.ini"; \
        if [ ! -f "${INI_SRC}" ]; then INI_SRC="/tmp/DefaultGame.ini"; : > "${INI_SRC}"; fi && \
        python3 /workspace/src/tools/schemagen/gen_node_constants.py \
            --ini "${INI_SRC}" \
            --out-cpp /workspace/build/gen/cpp/aoc/citynode_constants_gen.hpp \
            --out-go /workspace/build/gen/go/citynodeconsts/constants_gen.go; \
    fi

ARG SERVICE_NAME=gateway
WORKDIR /workspace/src/server/services
RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=0 GOOS=linux \
    go build -trimpath -ldflags="-s -w" -o /out/service ./cmd/${SERVICE_NAME}

FROM gcr.io/distroless/static-debian12:nonroot@sha256:e8a404fc10b4fb422c6a6d615c0b9a9c242b9d8f3b2a1c4d5e6f7a8b9c0d1e2f AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG SERVICE_NAME=gateway
ARG SERVICE_HTTP_PORT=8081
ARG SERVICE_GRPC_PORT=50050
ARG SERVICE_BIND_ADDR=0.0.0.0
ARG AOC_ENV=production

LABEL org.opencontainers.image.title="sandal-go-service" \
      org.opencontainers.image.description="SandalCore Go control-plane, gateway, and mesh microservice runtime" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV SERVICE_NAME=${SERVICE_NAME} \
    SERVICE_HTTP_PORT=${SERVICE_HTTP_PORT} \
    SERVICE_GRPC_PORT=${SERVICE_GRPC_PORT} \
    SERVICE_BIND_ADDR=${SERVICE_BIND_ADDR} \
    AOC_ENV=${AOC_ENV}

WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/service /usr/local/bin/service
USER 65532:65532
EXPOSE 8081/tcp 8082/tcp 50050/tcp 50052/tcp 50053/tcp 50054/tcp 50056/tcp 50057/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]
