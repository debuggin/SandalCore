#!/usr/bin/env bash
set -euo pipefail

# WHY: Enforces systemd boot, root-owned Unix socket, and loopback TCP 2375 Docker Engine across WSL2 Linux distributions (OPS-04).

DOCKER_OVERRIDE_CHANGED=0

merge_wsl_conf_systemd() {
  local wsl_conf="$1"
  awk '
    BEGIN { in_boot = 0; boot_seen = 0; set_done = 0 }
    {
      sub(/\r$/, "")
      if ($0 ~ /^[[:space:]]*\[[^]]+\][[:space:]]*([#;].*)?$/) {
        if (in_boot && !set_done) {
          print "systemd=true"
          set_done = 1
        }
        sec = $0
        sub(/^[[:space:]]*\[/, "", sec)
        sub(/\].*$/, "", sec)
        in_boot = (tolower(sec) == "boot")
        if (in_boot) { boot_seen = 1 }
        print $0
        next
      }
      if (in_boot && $0 ~ /^[[:space:]]*systemd[[:space:]]*=/) {
        if (!set_done) {
          print "systemd=true"
          set_done = 1
        }
        next
      }
      print $0
    }
    END {
      if (in_boot && !set_done) {
        print "systemd=true"
      } else if (!boot_seen) {
        print ""
        print "[boot]"
        print "systemd=true"
      }
    }
  ' "${wsl_conf}" >"${wsl_conf}.tmp"
  mv "${wsl_conf}.tmp" "${wsl_conf}"
}

ensure_wsl_systemd() {
  local wsl_conf="${WSL_CONF_PATH:-/etc/wsl.conf}"
  if [[ ! -f "${wsl_conf}" ]]; then
    printf '[boot]\nsystemd=true\n' >"${wsl_conf}"
    echo "INFO: Configuring ${wsl_conf} -> [boot] systemd=true"
    return 0
  fi
  local has_boot_systemd
  has_boot_systemd="$(awk '
    BEGIN { in_boot = 0; ok = 0 }
    {
      sub(/\r$/, "")
      if ($0 ~ /^[[:space:]]*\[[^]]+\][[:space:]]*([#;].*)?$/) {
        sec = $0; sub(/^[[:space:]]*\[/, "", sec); sub(/\].*$/, "", sec)
        in_boot = (tolower(sec) == "boot")
        next
      }
      if (in_boot && $0 ~ /^[[:space:]]*systemd[[:space:]]*=[[:space:]]*true[[:space:]]*([#;].*)?$/) {
        ok = 1
      }
    }
    END { print ok }
  ' "${wsl_conf}")"
  if [[ "${has_boot_systemd}" == "1" ]]; then
    echo "INFO: ${wsl_conf} already has [boot] systemd=true"
    return 0
  fi
  merge_wsl_conf_systemd "${wsl_conf}"
  echo "INFO: Configuring ${wsl_conf} -> [boot] systemd=true"
}

check_systemd_pid1() {
  local wsl_conf="${WSL_CONF_PATH:-/etc/wsl.conf}"
  local systemd_dir="${SYSTEMD_RUN_DIR:-/run/systemd/system}"
  if [ ! -d "${systemd_dir}" ]; then
    echo "INFO: systemd is enabled in ${wsl_conf} but WSL2 distro must be restarted to boot systemd as PID 1."
    exit 42
  fi
}

sanitize_daemon_json() {
  local daemon_json="${DOCKER_DAEMON_JSON_PATH:-/etc/docker/daemon.json}"
  if [[ ! -f "${daemon_json}" ]] || ! grep -Fq '"hosts"' "${daemon_json}"; then
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - "${daemon_json}" <<'PY'
import json, shutil, sys
p = sys.argv[1]
try:
    with open(p, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict):
        data.pop("hosts", None)
    else:
        data = {}
except Exception:
    shutil.copyfile(p, p + ".bak")
    data = {}
with open(p, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY
    DOCKER_OVERRIDE_CHANGED=1
    return 0
  fi
  if command -v jq >/dev/null 2>&1; then
    if jq 'del(.hosts)' "${daemon_json}" >"${daemon_json}.tmp" 2>/dev/null; then
      mv "${daemon_json}.tmp" "${daemon_json}"
    else
      cp "${daemon_json}" "${daemon_json}.bak"
      printf '{}\n' >"${daemon_json}"
      rm -f "${daemon_json}.tmp"
    fi
    DOCKER_OVERRIDE_CHANGED=1
  fi
}

ensure_docker_tcp_2375_override() {
  local dropin_conf="${DOCKER_SANDAL_DROPIN_PATH:-/etc/systemd/system/docker.service.d/sandal-tcp.conf}"
  local override_conf="${DOCKER_OVERRIDE_PATH:-/etc/systemd/system/docker.service.d/override.conf}"
  sanitize_daemon_json
  mkdir -p "$(dirname "${dropin_conf}")"
  if [[ ! -f "${dropin_conf}" ]] || ! grep -Fq 'ExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375' "${dropin_conf}"; then
    printf '[Service]\nExecStart=\nExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375\n' >"${dropin_conf}"
    DOCKER_OVERRIDE_CHANGED=1
  fi
  if [[ -f "${override_conf}" ]]; then
    local custom_lines
    custom_lines="$(grep -Ev '^[[:space:]]*($|[#;]|\[Service\]|ExecStart=)' "${override_conf}" || true)"
    if [[ -z "${custom_lines}" ]]; then
      rm -f "${override_conf}"
      DOCKER_OVERRIDE_CHANGED=1
    fi
  fi
}

install_docker_apt() {
  local distro_id="$1"
  local id_like="$2"
  local codename="$3"
  export DEBIAN_FRONTEND=noninteractive
  dpkg --configure -a >/dev/null 2>&1 || true
  DEBIAN_FRONTEND=noninteractive apt-get -f install -y >/dev/null 2>&1 || true
  apt-get update -y
  apt-get install -y ca-certificates curl gnupg
  local repo_distro="ubuntu"
  if [[ "${distro_id}" == "debian" ]] || [[ " ${id_like} " == *" debian "* && "${distro_id}" != "ubuntu" ]]; then
    repo_distro="debian"
  fi
  install -m 0755 -d /etc/apt/keyrings
  curl -fsSL "https://download.docker.com/linux/${repo_distro}/gpg" -o /etc/apt/keyrings/docker.asc
  chmod a+r /etc/apt/keyrings/docker.asc
  local expected_fpr="9DC858229FC7DD38854AE2D88D81803C0EBFCD88"
  if ! gpg --show-keys --with-colons /etc/apt/keyrings/docker.asc | grep -Fq "${expected_fpr}"; then
    echo "ERROR: Docker GPG fingerprint verification failed" >&2
    return 1
  fi
  local arch
  arch="$(dpkg --print-architecture)"
  printf 'deb [arch=%s signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/%s %s stable\n' "${arch}" "${repo_distro}" "${codename}" >/etc/apt/sources.list.d/docker.list
  if apt-get update -y && apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin; then
    return 0
  fi
  rm -f /etc/apt/sources.list.d/docker.list
  apt-get update -y
  apt-get install -y docker.io docker-compose-v2 || apt-get install -y docker.io docker-compose
}

install_docker_rpm() {
  local distro_id="$1"
  local pkg_mgr="dnf"
  if ! command -v dnf >/dev/null 2>&1; then
    pkg_mgr="yum"
  fi
  local repo_family="centos"
  if [[ "${distro_id}" == "fedora" ]]; then
    repo_family="fedora"
  fi
  if [[ "${pkg_mgr}" == "dnf" ]]; then
    dnf install -y dnf-plugins-core >/dev/null 2>&1 || true
    dnf config-manager --add-repo "https://download.docker.com/linux/${repo_family}/docker-ce.repo" >/dev/null 2>&1 || true
  else
    yum install -y yum-utils >/dev/null 2>&1 || true
    yum-config-manager --add-repo "https://download.docker.com/linux/${repo_family}/docker-ce.repo" >/dev/null 2>&1 || true
  fi
  "${pkg_mgr}" install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin ||
    "${pkg_mgr}" install -y docker docker-compose-plugin ||
    "${pkg_mgr}" install -y moby-engine
}

install_docker_engine() {
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    echo "INFO: Docker Engine and Compose v2 already installed; reusing existing installation."
    return 0
  fi
  local distro_id="ubuntu"
  local id_like=""
  local codename="noble"
  if [[ -f /etc/os-release ]]; then
    distro_id="$(. /etc/os-release && echo "${ID:-ubuntu}")"
    id_like="$(. /etc/os-release && echo "${ID_LIKE:-}")"
    codename="$(. /etc/os-release && echo "${VERSION_CODENAME:-noble}")"
  fi
  if command -v apt-get >/dev/null 2>&1; then
    install_docker_apt "${distro_id}" "${id_like}" "${codename}"
  elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
    install_docker_rpm "${distro_id}"
  elif command -v pacman >/dev/null 2>&1; then
    pacman -Sy --noconfirm --needed docker docker-compose docker-buildx
  elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install docker docker-compose docker-buildx
  elif command -v apk >/dev/null 2>&1; then
    apk add --no-cache docker docker-cli-compose docker-cli-buildx
  else
    echo "ERROR: Unsupported Linux package manager for distro '${distro_id}'" >&2
    return 1
  fi
}

ensure_docker_socket_service() {
  local docker_sock="${DOCKER_SOCK_PATH:-/var/run/docker.sock}"
  systemctl unmask docker.service docker.socket containerd.service >/dev/null 2>&1 || true
  systemctl reset-failed docker.service docker.socket containerd.service >/dev/null 2>&1 || true
  if ! systemctl is-active --quiet docker.service; then
    rm -f /var/run/docker.pid
  fi
  systemctl daemon-reload
  systemctl enable --now containerd.service docker.socket docker.service
  if [[ "${DOCKER_OVERRIDE_CHANGED:-0}" -eq 1 ]]; then
    systemctl restart docker.service
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
  local systemd_only=0
  for arg in "$@"; do
    if [[ "${arg}" == "--ensure-systemd-only" ]]; then
      systemd_only=1
    fi
  done
  ensure_wsl_systemd
  check_systemd_pid1
  if [[ "${systemd_only}" -eq 1 ]]; then
    return 0
  fi
  ensure_docker_tcp_2375_override
  install_docker_engine
  ensure_docker_socket_service
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
