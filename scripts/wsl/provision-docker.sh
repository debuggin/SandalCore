#!/usr/bin/env bash
set -euo pipefail

# WHY: Enforces systemd boot and root-owned Unix socket Docker Engine inside WSL2 without TCP 2375 exposure (SEC-12, INFRA-02).

ensure_wsl_systemd() {
  local wsl_conf="${WSL_CONF_PATH:-/etc/wsl.conf}"
  if [[ ! -f "${wsl_conf}" ]]; then
    printf '[boot]\nsystemd=true\n' >"${wsl_conf}"
    return 0
  fi
  if grep -Eq '^[[:space:]]*systemd[[:space:]]*=[[:space:]]*true[[:space:]]*$' "${wsl_conf}"; then
    return 0
  fi
  if grep -Eq '^[[:space:]]*systemd[[:space:]]*=' "${wsl_conf}"; then
    sed -i -E 's/^[[:space:]]*systemd[[:space:]]*=.*/systemd=true/' "${wsl_conf}"
    return 0
  fi
  if grep -Eq '^[[:space:]]*\[boot\][[:space:]]*$' "${wsl_conf}"; then
    sed -i -E '/^[[:space:]]*\[boot\][[:space:]]*$/a systemd=true' "${wsl_conf}"
    return 0
  fi
  printf '\n[boot]\nsystemd=true\n' >>"${wsl_conf}"
}

purge_legacy_tcp_2375() {
  local override_conf="${DOCKER_OVERRIDE_PATH:-/etc/systemd/system/docker.service.d/override.conf}"
  if [[ ! -f "${override_conf}" ]]; then
    return 0
  fi
  if grep -Eq '(-H[[:space:]]+tcp://127\.0\.0\.1:2375|tcp://[^[:space:]]*:2375)' "${override_conf}"; then
    sed -i 's/ -H tcp:\/\/[^ ]*//g; s/-H tcp:\/\/[^ ]*//g' "${override_conf}"
    if command -v systemctl >/dev/null 2>&1 && systemctl is-system-running >/dev/null 2>&1; then
      systemctl daemon-reload
    fi
  fi
}

install_docker_engine() {
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    return 0
  fi
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -y
  apt-get install -y ca-certificates curl gnupg
  install -m 0755 -d /etc/apt/keyrings
  curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
  chmod a+r /etc/apt/keyrings/docker.asc
  local expected_fpr="9DC858229FC7DD38854AE2D88D81803C0EBFCD88"
  if ! gpg --show-keys --with-colons /etc/apt/keyrings/docker.asc | grep -Fq "${expected_fpr}"; then
    echo "ERROR: Docker GPG fingerprint verification failed" >&2
    return 1
  fi
  local arch
  arch="$(dpkg --print-architecture)"
  local codename
  codename="$(. /etc/os-release && echo "$VERSION_CODENAME")"
  printf 'deb [arch=%s signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu %s stable\n' "${arch}" "${codename}" >/etc/apt/sources.list.d/docker.list
  apt-get update -y
  apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
}

ensure_docker_socket_service() {
  local docker_sock="${DOCKER_SOCK_PATH:-/var/run/docker.sock}"
  systemctl enable docker.service
  systemctl start docker.service
  if [[ ! -S "${docker_sock}" ]]; then
    echo "ERROR: Docker Unix socket ${docker_sock} not found" >&2
    return 1
  fi
  if command -v ss >/dev/null 2>&1 && ss -tlnp | grep -Fq ':2375'; then
    echo "ERROR: Forbidden TCP 2375 listener detected" >&2
    return 1
  fi
}

main() {
  ensure_wsl_systemd
  purge_legacy_tcp_2375
  install_docker_engine
  ensure_docker_socket_service
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
