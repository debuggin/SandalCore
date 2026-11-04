# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM postgres:17.2-alpine3.21@sha256:4a90d26c0c849f2f98f46a53a5489a3420a181301980d3e722645a9e9195296c AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
LABEL org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.title="sandal-postgres"

COPY --chown=70:70 services/sql/migrations/ /docker-entrypoint-initdb.d/migrations/
COPY --chown=70:70 deploy/postgres/init/ /docker-entrypoint-initdb.d/
USER 70:70
EXPOSE 5432/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=30 \
    CMD ["pg_isready", "-h", "127.0.0.1", "-U", "sandal_admin", "-d", "sandal"]
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["postgres"]
