#!/usr/bin/env bash
set -euo pipefail

# WHY: Enforces systemd boot, root-owned Unix socket, and localhost-mirrored TCP 2375 Docker Engine inside WSL2 for Windows standalone Docker CLI (OPS-04).

DOCKER_OVERRIDE_CHANGED=0

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

ensure_docker_tcp_2375_override() {
  local override_conf="${DOCKER_OVERRIDE_PATH:-/etc/systemd/system/docker.service.d/override.conf}"
  local daemon_json="${DOCKER_DAEMON_JSON_PATH:-/etc/docker/daemon.json}"
  if [[ -f "${daemon_json}" ]] && grep -Fq '"hosts"' "${daemon_json}"; then
    sed -i -E '/"hosts"[[:space:]]*:/d' "${daemon_json}"
    DOCKER_OVERRIDE_CHANGED=1
  fi
  mkdir -p "$(dirname "${override_conf}")"
  if [[ ! -f "${override_conf}" ]] || ! grep -Fq 'ExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375' "${override_conf}"; then
    printf '[Service]\nExecStart=\nExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375\n' >"${override_conf}"
    DOCKER_OVERRIDE_CHANGED=1
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
  systemctl daemon-reload
  systemctl enable docker.service containerd.service || true
  if [[ "${DOCKER_OVERRIDE_CHANGED:-0}" -eq 1 ]]; then
    systemctl restart docker.service
  elif ! systemctl is-active --quiet docker.service; then
    systemctl start docker.service
  fi
  if [[ ! -S "${docker_sock}" ]]; then
    echo "ERROR: Docker Unix socket ${docker_sock} not found" >&2
    return 1
  fi
  if command -v ss >/dev/null 2>&1 && ! ss -tlnp | grep -Fq ':2375'; then
    echo "ERROR: Expected TCP 2375 listener not detected" >&2
    return 1
  fi
}

main() {
  ensure_wsl_systemd
  ensure_docker_tcp_2375_override
  install_docker_engine
  ensure_docker_socket_service
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
