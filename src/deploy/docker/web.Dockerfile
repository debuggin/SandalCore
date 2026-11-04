# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-only

FROM node:22-alpine@sha256:fe4f6d74e20370acb04ad889077d14d0f64a9c8ea6a42bc60cddf1e844699914 AS builder
WORKDIR /workspace/src/clients/web-admin
COPY src/clients/web-admin/package.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm install --ignore-scripts
COPY src/clients/web-admin/ ./
RUN npm run build

FROM nginxinc/nginx-unprivileged:1.27-alpine@sha256:65e3e85dbaed8ba248841d9d58a899b6197106c23cb0ff1a132b7bfe0547e4c0 AS runtime
ARG GIT_REVISION="unknown"
ARG BUILD_CREATED="1970-01-01T00:00:00Z"
ARG WEB_PORT=8088
ARG WEB_BIND_ADDR=0.0.0.0
ARG ROOT_USER=root

LABEL org.opencontainers.image.title="sandal-web-admin" \
      org.opencontainers.image.description="SandalCore Web Admin SPA served by unprivileged Nginx" \
      org.opencontainers.image.source="https://github.com/SandalCore/SandalCore" \
      org.opencontainers.image.revision="${GIT_REVISION}" \
      org.opencontainers.image.created="${BUILD_CREATED}" \
      org.opencontainers.image.vendor="SandalCore"

ENV WEB_PORT=${WEB_PORT} \
    WEB_BIND_ADDR=${WEB_BIND_ADDR}

USER ${ROOT_USER}
RUN printf 'server {\n    listen 3000;\n    listen 8088;\n    server_name _;\n    root /usr/share/nginx/html;\n    index index.html;\n    location = /healthz {\n        access_log off;\n        default_type text/plain;\n        return 200 "ok\\n";\n    }\n    location / {\n        try_files $uri $uri/ /index.html;\n    }\n}\n' > /etc/nginx/conf.d/default.conf && \
    chown -R 65532:65532 /var/cache/nginx /var/run /etc/nginx/conf.d /usr/share/nginx/html
COPY --from=builder --chown=65532:65532 /workspace/src/clients/web-admin/dist /usr/share/nginx/html
USER 101:101
USER 65532:65532
EXPOSE 3000/tcp 8088/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
    CMD ["wget", "-q", "-O", "/dev/null", "http://127.0.0.1:3000/healthz"]
ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["nginx", "-g", "daemon off;"]
