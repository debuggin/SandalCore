# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM node:22-alpine@sha256:e4bf9a522a04b038a027f8187b887b4a028cb98901d83d2590291401c8b680b0 AS web-builder
WORKDIR /workspace/src/clients/web-admin
COPY src/clients/web-admin/package.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm install --ignore-scripts
COPY src/clients/web-admin/ ./
RUN npm run build

FROM nginxinc/nginx-unprivileged:1.27-alpine@sha256:9f7d6e61a39f1e9c23f44517a4d60c64b0e57f2d0f8772e2d4d2c79d6bc48e7b AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG WEB_PORT=8088
ARG WEB_BIND_ADDR=0.0.0.0

LABEL org.opencontainers.image.title="sandal-web-admin" \
      org.opencontainers.image.description="SandalCore Web Admin SPA served by unprivileged Nginx" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV WEB_PORT=${WEB_PORT} \
    WEB_BIND_ADDR=${WEB_BIND_ADDR}

COPY --from=web-builder --chown=65532:65532 /workspace/src/clients/web-admin/dist /usr/share/nginx/html
USER 101:101
USER 65532:65532
EXPOSE 8088/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["wget", "-q", "-O", "/dev/null", "http://127.0.0.1:8088/"]
ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["nginx", "-g", "daemon off;"]
