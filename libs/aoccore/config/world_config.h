#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "libs/aoccore/config/dsn_redactor.h"

namespace aoc::world::config {
enum class MeshMode : std::uint8_t { Standalone = 0, Meshed = 1 };

struct WorldConfig {
    std::string env{"development"};
    MeshMode mesh_mode{MeshMode::Standalone};
    std::string realm_id{"verra-dev"};
    std::string realm_name{"Verra Development"};
    std::string world_server_id{"world-0"};
    std::uint16_t world_udp_port{7777};
    std::string world_udp_bind_addr{"127.0.0.1:7777"};
    std::string world_udp_advertise_addr{"127.0.0.1:7777"};
    std::string world_admin_bind_addr{"127.0.0.1:50056"};
    std::uint32_t world_tick_rate_hz{30};
    bool gm_starter_set{true};
    std::string database_url{"postgres://aoc:aoc@127.0.0.1:5433/aoc?sslmode=disable"};
    std::string content_db_path{"dist/content/content.db"};
    std::string hotfix_dir{"config/hotfixes"};
    std::string pmtiles_path{"dist/content/verra.pmtiles"};
    std::string navmesh_dir{"dist/nav"};
    std::string riverlands_navmesh_path{"dist/nav/riverlands.aocnav"};
    std::string vmap_path{"dist/nav/area_vmap.bin"};
    std::string service_token_file{"dist/services/service.token"};
    std::string auth_grpc_addr{"127.0.0.1:50051"};
    std::string character_grpc_addr{"127.0.0.1:50052"};
    std::string director_grpc_addr{"127.0.0.1:50053"};
    std::string mesh_grpc_addr{"127.0.0.1:50054"};
    std::string citynode_grpc_addr{"127.0.0.1:50055"};
    std::string quest_grpc_addr{"127.0.0.1:50058"};
};

struct ConfigLoadResult {
    WorldConfig config{};
    std::vector<std::string> errors{};
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

[[nodiscard]] std::vector<std::string> ValidateWorldConfig(const WorldConfig& cfg);
[[nodiscard]] ConfigLoadResult LoadWorldConfig(
    const std::unordered_map<std::string, std::string>& env_overrides = {},
    std::string_view env_file_path = "config/sandal.env");
}  // namespace aoc::world::config
