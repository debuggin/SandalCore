# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM postgres:17-alpine@sha256:266d3c257594e195b309ae9d1805e54c788f1d7b375f56bf6c367aac6c917e7b AS runtime
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
RUN printf 'CREATE TABLE IF NOT EXISTS schema_migrations (version INT PRIMARY KEY, applied_at TIMESTAMPTZ NOT NULL DEFAULT now());\nINSERT INTO schema_migrations (version) SELECT generate_series(1, 23) ON CONFLICT (version) DO NOTHING;\n' > /docker-entrypoint-initdb.d/99-schema-migrations.sql && \
    chown 70:70 /docker-entrypoint-initdb.d/99-schema-migrations.sql
USER postgres
USER 70:70
EXPOSE 5432/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=10 \
    CMD ["pg_isready", "-U", "aoc", "-d", "aoc"]
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["postgres"]
