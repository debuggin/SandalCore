#include "libs/aoccore/config/world_config.h"
#include <cstdlib>
#include <fstream>
#include <optional>

namespace aoc::world::config {
namespace {
using EnvMap = std::unordered_map<std::string, std::string>;

std::string_view Trim(std::string_view sv) noexcept {
    const auto f = sv.find_first_not_of(" \t\r\n");
    if (f == std::string_view::npos) return {};
    sv = sv.substr(f, sv.find_last_not_of(" \t\r\n") - f + 1);
    return (sv.size() >= 2 && (sv.front() == '"' || sv.front() == '\'') && sv.back() == sv.front())
               ? sv.substr(1, sv.size() - 2) : sv;
}

EnvMap ParseDotEnv(std::string_view path) {
    EnvMap out;
    std::ifstream in{std::string(path)};
    for (std::string line; std::getline(in, line);) {
        const std::string_view sv = Trim(line);
        if (const auto eq = sv.find('='); !sv.empty() && sv.front() != '#' && eq != std::string_view::npos)
            out[std::string(Trim(sv.substr(0, eq)))] = std::string(Trim(sv.substr(eq + 1)));
    }
    return out;
}

std::optional<std::string> Lookup(const EnvMap& ov, const EnvMap& file, std::string_view key, std::string_view alt = {}) {
    const std::string k(key), s = k.starts_with("AOC_") ? ("SANDAL_" + k.substr(4)) : k;
    for (const auto& c : {s, k}) {
        if (const auto it = ov.find(c); it != ov.end()) return it->second;
        if (const char* e = std::getenv(c.c_str()); e != nullptr) return std::string(e);
        if (const auto it = file.find(c); it != file.end()) return it->second;
    }
    return alt.empty() ? std::nullopt : Lookup(ov, file, alt);
}

void ApplyScalars(const EnvMap& ov, const EnvMap& file, ConfigLoadResult& res) {
    if (const auto v = Lookup(ov, file, "AOC_MESH_MODE")) {
        if (*v == "standalone") res.config.mesh_mode = MeshMode::Standalone;
        else if (*v == "meshed") res.config.mesh_mode = MeshMode::Meshed;
        else res.errors.push_back("Invalid AOC_MESH_MODE: " + *v);
    }
    if (const auto v = Lookup(ov, file, "AOC_WORLD_UDP_PORT")) {
        const long p = std::strtol(v->c_str(), nullptr, 10);
        res.config.world_udp_port = (p >= 1 && p <= 65535) ? static_cast<std::uint16_t>(p) : 0;
    }
    if (const auto v = Lookup(ov, file, "AOC_WORLD_TICK_RATE_HZ")) {
        const long hz = std::strtol(v->c_str(), nullptr, 10);
        res.config.world_tick_rate_hz = hz > 0 ? static_cast<std::uint32_t>(hz) : 0;
    }
    if (const auto v = Lookup(ov, file, "AOC_GM_STARTER_SET"))
        res.config.gm_starter_set = (*v == "true" || *v == "1");
}
}  // namespace

std::vector<std::string> ValidateWorldConfig(const WorldConfig& cfg) {
    std::vector<std::string> errs;
    if (cfg.env != "development" && cfg.env != "staging" && cfg.env != "production")
        errs.push_back("Invalid AOC_ENV: " + cfg.env);
    if (cfg.world_udp_port < 1) errs.push_back("AOC_WORLD_UDP_PORT must be >= 1");
    if (cfg.world_tick_rate_hz < 1 || cfg.world_tick_rate_hz > 120)
        errs.push_back("AOC_WORLD_TICK_RATE_HZ out of range [1, 120]");
    if (cfg.env != "production") return errs;
    if (cfg.database_url.find("aoc:aoc@") != std::string::npos || cfg.database_url.find("sslmode=disable") != std::string::npos)
        errs.push_back("Production database_url forbids default credentials or sslmode=disable");
    if (cfg.gm_starter_set) errs.push_back("Production forbids AOC_GM_STARTER_SET=true");
    if (cfg.service_token_file.empty()) errs.push_back("Production requires AOC_SERVICE_TOKEN_FILE");
    return errs;
}

ConfigLoadResult LoadWorldConfig(const EnvMap& env_overrides, std::string_view env_file_path) {
    ConfigLoadResult res{};
    std::string path(env_file_path);
    if (env_file_path == "config/sandal.env")
        if (const auto it = env_overrides.find("SANDAL_CONFIG_FILE"); it != env_overrides.end()) path = it->second;
    const EnvMap file = ParseDotEnv(path);
    struct Bind { std::string_view k, alt; std::string WorldConfig::*m; };
    for (const auto& b : {
             Bind{"AOC_ENV", {}, &WorldConfig::env}, Bind{"AOC_REALM_ID", {}, &WorldConfig::realm_id},
             Bind{"AOC_REALM_NAME", {}, &WorldConfig::realm_name}, Bind{"AOC_WORLD_SERVER_ID", {}, &WorldConfig::world_server_id},
             Bind{"AOC_WORLD_UDP_BIND_ADDR", {}, &WorldConfig::world_udp_bind_addr}, Bind{"AOC_WORLD_UDP_ADVERTISE_ADDR", {}, &WorldConfig::world_udp_advertise_addr},
             Bind{"AOC_WORLD_ADMIN_BIND_ADDR", {}, &WorldConfig::world_admin_bind_addr}, Bind{"AOC_DATABASE_URL", {}, &WorldConfig::database_url},
             Bind{"AOC_CONTENT_DB_PATH", {}, &WorldConfig::content_db_path}, Bind{"AOC_HOTFIX_DIR", {}, &WorldConfig::hotfix_dir},
             Bind{"AOC_PMTILES_PATH", {}, &WorldConfig::pmtiles_path}, Bind{"AOC_NAVMESH_DIR", {}, &WorldConfig::navmesh_dir},
             Bind{"AOC_RIVERLANDS_NAVMESH_PATH", {}, &WorldConfig::riverlands_navmesh_path}, Bind{"AOC_VMAP_PATH", {}, &WorldConfig::vmap_path},
             Bind{"AOC_SERVICE_TOKEN_FILE", {}, &WorldConfig::service_token_file}, Bind{"AOC_AUTH_GRPC_ADVERTISE_ADDR", "AOC_AUTH_GRPC_BIND_ADDR", &WorldConfig::auth_grpc_addr},
             Bind{"AOC_CHARACTER_GRPC_ADVERTISE_ADDR", "AOC_CHARACTER_GRPC_BIND_ADDR", &WorldConfig::character_grpc_addr},
             Bind{"AOC_DIRECTOR_GRPC_ADVERTISE_ADDR", "AOC_DIRECTOR_GRPC_BIND_ADDR", &WorldConfig::director_grpc_addr},
             Bind{"AOC_MESH_GRPC_ADVERTISE_ADDR", "AOC_MESH_GRPC_BIND_ADDR", &WorldConfig::mesh_grpc_addr},
             Bind{"AOC_CITYNODE_GRPC_ADVERTISE_ADDR", "AOC_CITYNODE_GRPC_BIND_ADDR", &WorldConfig::citynode_grpc_addr},
             Bind{"AOC_QUEST_GRPC_ADVERTISE_ADDR", "AOC_QUEST_GRPC_BIND_ADDR", &WorldConfig::quest_grpc_addr}})
        if (const auto v = Lookup(env_overrides, file, b.k, b.alt)) res.config.*(b.m) = *v;
    ApplyScalars(env_overrides, file, res);
    for (auto& err : ValidateWorldConfig(res.config)) res.errors.push_back(std::move(err));
    return res;
}
}  // namespace aoc::world::config
