# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM node:22-alpine3.21@sha256:e4bf9a522a04b038a027f8187b887b4a028cb98901d83d2590291401c8b680b0 AS web-builder
WORKDIR /web
COPY web/admin/package.json web/admin/package-lock.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci --ignore-scripts
COPY web/admin/ ./
RUN npm run build

FROM golang:1.25-alpine3.21@sha256:f8d0e5861e26f2b403198c24eb87d76d01d0a81db48f36a7a939e1a8f5e0c2b4 AS go-builder
WORKDIR /src/services
COPY services/go.mod services/go.sum ./
RUN --mount=type=cache,target=/go/pkg/mod \
    go mod download && go mod verify
COPY services/ ./
COPY --from=web-builder /web/dist ./internal/admin/ui/dist
RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=0 GOOS=linux \
    go build -trimpath -ldflags="-s -w" -o /out/admin ./cmd/admin

FROM gcr.io/distroless/static-debian12:nonroot@sha256:e8a404fc10b4fb422c6a6d615c0b9a9c242b9d8f3b2a1c4d5e6f7a8b9c0d1e2f AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
LABEL org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.title="sandal-web-admin"

WORKDIR /app
COPY --from=go-builder --chown=65532:65532 /out/admin /usr/local/bin/admin
USER 65532:65532
EXPOSE 8090/tcp 8443/tcp 8443/udp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=2s --start-period=3s --retries=3 \
    CMD ["/usr/local/bin/admin", "-healthcheck"]
ENTRYPOINT ["/usr/local/bin/admin"]
