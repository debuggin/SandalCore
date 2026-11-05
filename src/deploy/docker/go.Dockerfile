# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM golang:1.27-alpine@sha256:d200d2dd76b03a99b52651fd1325aff7dcd35422c35c01bac56571b0507f3deb AS builder
RUN apk add --no-cache git protobuf protobuf-dev protoc

WORKDIR /workspace
COPY build/gen/go/ ./build/gen/go/
COPY src/server/services/go.mod src/server/services/go.sum ./src/server/services/

WORKDIR /workspace/src/server/services
RUN --mount=type=cache,target=/go/pkg/mod \
    go mod download

WORKDIR /workspace
COPY scripts/wsl/gen-node-constants.sh ./scripts/wsl/gen-node-constants.sh
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
    if [ ! -f "/workspace/build/gen/world/world/node_constants.h" ] && [ ! -f "/workspace/build/gen/go/citynodeconsts/constants_gen.go" ]; then \
        mkdir -p /workspace/build/gen/world/world /workspace/build/gen/go/aoc/nodeconstants /workspace/build/gen/go/citynodeconsts && \
        sh /workspace/scripts/wsl/gen-node-constants.sh \
            --cpp-out /workspace/build/gen/world/world/node_constants.h \
            --go-out /workspace/build/gen/go/aoc/nodeconstants/node_constants.go && \
        cp /workspace/build/gen/go/aoc/nodeconstants/node_constants.go /workspace/build/gen/go/citynodeconsts/constants_gen.go; \
    fi

WORKDIR /workspace/src/server/services
RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    mkdir -p /out && \
    for svc in auth character citynode quest mesh gateway meshgw admin; do \
        if [ -d "./cmd/${svc}" ]; then \
            CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o "/out/${svc}" "./cmd/${svc}"; \
        fi; \
    done

ARG SERVICE_NAME=gateway
RUN if [ -f "/out/${SERVICE_NAME}" ]; then \
        cp "/out/${SERVICE_NAME}" /out/service; \
    else \
        cp /out/gateway /out/service; \
    fi

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS auth
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/auth /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/auth /app/auth
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
USER 65532:65532
EXPOSE 8081/tcp 50052/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS character
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/character /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/character /app/character
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
USER 65532:65532
EXPOSE 8082/tcp 50053/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS citynode
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/citynode /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/citynode /app/citynode
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
COPY --from=builder --chown=65532:65532 /workspace/src/config/seeds/citynode_seed.json /app/config/seeds/citynode_seed.json
USER 65532:65532
EXPOSE 8083/tcp 50056/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS quest
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/quest /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/quest /app/quest
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
USER 65532:65532
EXPOSE 8084/tcp 50057/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS mesh
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/mesh /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/mesh /app/mesh
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
USER 65532:65532
EXPOSE 8085/tcp 50054/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS gateway
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/gateway /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/gateway /app/gateway
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
USER 65532:65532
EXPOSE 8080/tcp 8081/tcp 9090/tcp 50050/tcp 50061/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS meshgw
WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/meshgw /usr/local/bin/service
COPY --from=builder --chown=65532:65532 /out/meshgw /app/meshgw
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
USER 65532:65532
EXPOSE 8082/tcp 8086/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab AS admin
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
COPY --from=builder --chown=65532:65532 /workspace/src/server/services/sql /app/sql
COPY --from=builder --chown=65532:65532 /workspace/src/config/seeds/citynode_seed.json /app/config/seeds/citynode_seed.json
USER 65532:65532
EXPOSE 8081/tcp 8082/tcp 50050/tcp 50052/tcp 50053/tcp 50054/tcp 50056/tcp 50057/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]
