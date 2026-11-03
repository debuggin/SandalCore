# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM golang:1.25-alpine3.21@sha256:f8d0e5861e26f2b403198c24eb87d76d01d0a81db48f36a7a939e1a8f5e0c2b4 AS builder
WORKDIR /src/services
COPY services/go.mod services/go.sum ./
RUN --mount=type=cache,target=/go/pkg/mod \
    go mod download && go mod verify
COPY services/ ./
ARG SERVICE_NAME=gateway
RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=0 GOOS=linux \
    go build -trimpath -ldflags="-s -w" -o /out/service ./cmd/${SERVICE_NAME}

FROM gcr.io/distroless/static-debian12:nonroot@sha256:e8a404fc10b4fb422c6a6d615c0b9a9c242b9d8f3b2a1c4d5e6f7a8b9c0d1e2f AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
LABEL org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.title="sandal-go-service"

WORKDIR /app
COPY --from=builder --chown=65532:65532 /out/service /usr/local/bin/service
USER 65532:65532
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=2s --start-period=3s --retries=3 \
    CMD ["/usr/local/bin/service", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/service"]
