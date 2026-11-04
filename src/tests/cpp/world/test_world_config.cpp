#include "libs/aoccore/config/world_config.h"

#include <cassert>
#include <filesystem>
#include <fstream>

namespace {

using aoc::world::config::LoadWorldConfig;
using aoc::world::config::MeshMode;
using aoc::world::config::RedactDsnCredentials;
using aoc::world::config::ValidateWorldConfig;
using aoc::world::config::WorldConfig;

void test_redaction_and_precedence() {
    assert(RedactDsnCredentials("postgres://aoc:supersecret@127.0.0.1:5433/aoc") ==
           "postgres://aoc:***REDACTED***@127.0.0.1:5433/aoc");
    assert(RedactDsnCredentials("host=db password=secret") == "host=db password=***REDACTED***");

    const auto defaults = LoadWorldConfig({}, "nonexistent_sandal.env");
    assert(defaults.ok());
    assert(defaults.config.world_udp_port == 7777);
    assert(defaults.config.mesh_mode == MeshMode::Standalone);

    const auto tmp_env = std::filesystem::temp_directory_path() / "sandal_test_world_config.env";
    {
        std::ofstream out(tmp_env);
        out << "AOC_WORLD_UDP_PORT=7780\n";
    }
    const auto from_file = LoadWorldConfig({}, tmp_env.string());
    assert(from_file.ok() && from_file.config.world_udp_port == 7780);
    const auto from_aoc = LoadWorldConfig({{"AOC_WORLD_UDP_PORT", "7781"}}, tmp_env.string());
    assert(from_aoc.ok() && from_aoc.config.world_udp_port == 7781);
    const auto from_sandal = LoadWorldConfig(
        {{"AOC_WORLD_UDP_PORT", "7781"}, {"SANDAL_WORLD_UDP_PORT", "7782"}}, tmp_env.string());
    assert(from_sandal.ok() && from_sandal.config.world_udp_port == 7782);
    std::filesystem::remove(tmp_env);
}

void test_production_validation() {
    WorldConfig cfg{};
    cfg.env = "production";
    assert(!ValidateWorldConfig(cfg).empty());

    cfg.database_url = "postgres://aoc:prodpass@db:5432/aoc?sslmode=verify-full";
    assert(!ValidateWorldConfig(cfg).empty());

    cfg.gm_starter_set = false;
    assert(ValidateWorldConfig(cfg).empty());
}

}  // namespace

int main() {
    test_redaction_and_precedence();
    test_production_validation();
    return 0;
}
