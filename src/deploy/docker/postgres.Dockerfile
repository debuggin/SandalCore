# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM postgres:17-alpine@sha256:4a90d26c0c849f2f98f46a53a5489a3420a181301980d3e722645a9e9195296c AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG PGPORT=5432
ARG POSTGRES_USER=aoc
ARG POSTGRES_DB=aoc

LABEL org.opencontainers.image.title="sandal-postgres" \
      org.opencontainers.image.description="SandalCore PostgreSQL 17 database with forward-only SQL migrations" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV PGPORT=${PGPORT} \
    POSTGRES_USER=${POSTGRES_USER} \
    POSTGRES_DB=${POSTGRES_DB}

COPY --chown=70:70 src/server/services/sql/migrations/*.sql /docker-entrypoint-initdb.d/
COPY --chown=70:70 src/deploy/postgres/init/*.sql /docker-entrypoint-initdb.d/
USER postgres
USER 70:70
EXPOSE 5432/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=10 \
    CMD ["pg_isready", "-U", "aoc", "-d", "aoc"]
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["postgres"]
