# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM envoyproxy/envoy:v1.33-latest@sha256:f91c972d5c99bc133233a079b5663b903e7d56b3b0b0216398924f7b80d09e47 AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG ENVOY_HTTP_INGRESS_PORT=8080
ARG ENVOY_GRPC_INGRESS_PORT=9090
ARG ENVOY_ADMIN_PORT=9901
ARG ROOT_USER=root

LABEL org.opencontainers.image.title="sandal-envoy-ingress" \
      org.opencontainers.image.description="SandalCore high-throughput Envoy HTTP/1.1, WebSocket, and HTTP/2 gRPC ingress proxy" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV ENVOY_HTTP_INGRESS_PORT=${ENVOY_HTTP_INGRESS_PORT} \
    ENVOY_GRPC_INGRESS_PORT=${ENVOY_GRPC_INGRESS_PORT} \
    ENVOY_ADMIN_PORT=${ENVOY_ADMIN_PORT}

USER ${ROOT_USER}
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --chown=65532:65532 src/deploy/envoy/envoy.yaml /etc/envoy/envoy.yaml
USER 65532:65532
EXPOSE 8080/tcp 9090/tcp 50051/tcp 50061/tcp 9901/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["curl", "-sf", "http://127.0.0.1:9901/ready"]
ENTRYPOINT ["/usr/local/bin/envoy"]
CMD ["-c", "/etc/envoy/envoy.yaml"]
