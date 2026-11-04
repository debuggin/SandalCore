#include "test_harness.h"

#include "stats/stat_program.h"
#include "world/city_nodes.h"
#include "world/node_constants.h"
#include "world/zoi_surface.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

namespace world = aoc::worldd::world;
namespace stats = aoc::worldd::stats;

constexpr int64_t kAelaNodeGuid = 6064629190001000001LL;
constexpr int64_t kAelaZoiGuid = 6064629127135166465LL;
constexpr int64_t kJoevaNodeGuid = 6064629190001000002LL;
constexpr int64_t kJoevaZoiGuid = 6064629126905528345LL;
constexpr int kDivineType = 2;
constexpr int kMartialType = 3;
constexpr int kRiverlandsBiome = 14;
constexpr int kMetropolisStage = 6;

world::CityNodes make_hermetic_city_nodes() {
    world::CityNodeInfo aela{};
    aela.rank = 0;
    aela.guid = kAelaNodeGuid;
    aela.name = "Verra_RVR_NewAela";
    aela.display_name = "New Aela";
    aela.type = kDivineType;
    aela.x = -777664.5;
    aela.y = 616578.7;
    aela.z = 13685.0;
    aela.map_name = "Verra_World_Master";
    aela.zoi_guid = kAelaZoiGuid;
    aela.levelable = true;

    world::CityNodeInfo joeva{};
    joeva.rank = 1;
    joeva.guid = kJoevaNodeGuid;
    joeva.name = "Verra_RVR_Joeva";
    joeva.display_name = "Joeva";
    joeva.type = kMartialType;
    joeva.x = -1094396.0;
    joeva.y = 521336.0;
    joeva.z = 10000.0;
    joeva.map_name = "Verra_World_Master";
    joeva.zoi_guid = kJoevaZoiGuid;
    joeva.levelable = true;

    world::CityZoneInfo aela_zoi{kAelaZoiGuid, "ZOI_NewAela", kRiverlandsBiome, kAelaNodeGuid, 0};
    world::CityZoneInfo joeva_zoi{kJoevaZoiGuid, "ZOI_Joeva", kRiverlandsBiome, kJoevaNodeGuid, 1};
    return world::CityNodes({aela, joeva}, {aela_zoi, joeva_zoi});
}

int test_zoi_spatial_cell_partition() {
    const auto info = world::zoi_surface_info();
    SANDAL_ASSERT(info.chunks == 4635U);
    SANDAL_ASSERT(info.uniform == 3240U);

    const auto palette = world::zoi_surface_palette();
    SANDAL_ASSERT(palette.size() == 58U);
    SANDAL_ASSERT(palette.front() == 0ULL);
    for (size_t i = 1; i < palette.size(); ++i) {
        SANDAL_ASSERT(palette[i - 1] < palette[i]);
    }
    SANDAL_ASSERT(std::binary_search(palette.begin(), palette.end(), static_cast<uint64_t>(kAelaZoiGuid)));
    SANDAL_ASSERT(std::binary_search(palette.begin(), palette.end(), static_cast<uint64_t>(kJoevaZoiGuid)));

    const uint64_t aela = world::zoi_at(-777664.5, 616578.7);
    SANDAL_ASSERT(aela == static_cast<uint64_t>(kAelaZoiGuid));
    SANDAL_ASSERT(world::zoi_at(-777663.5, 616579.7) == aela);
    SANDAL_ASSERT(world::zoi_at(-1094396.0, 521336.0) == static_cast<uint64_t>(kJoevaZoiGuid));

    SANDAL_ASSERT(world::zoi_at(1.0e9, 1.0e9) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(-1.0e9, -1.0e9) == 0ULL);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    SANDAL_ASSERT(world::zoi_at(nan, 616578.7) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(-777664.5, inf) == 0ULL);
    return 0;
}

int verify_node_host_functions(const world::CityNodes& nodes) {
    world::NodeContext ctx{};
    ctx.node = 0;
    ctx.biome = kRiverlandsBiome;
    ctx.actor_node = 0;
    ctx.player_node = 1;

    double out = -1.0;
    SANDAL_ASSERT(world::node_host_fn(&nodes, ctx, stats::kHostNodeBiome, nullptr, 0, out));
    SANDAL_ASSERT(std::fabs(out - static_cast<double>(kRiverlandsBiome)) < 1e-6);

    SANDAL_ASSERT(world::node_host_fn(&nodes, ctx, stats::kHostNodeRecord, nullptr, 0, out));
    SANDAL_ASSERT(std::fabs(out - 0.0) < 1e-6);

    const double rank0 = 0.0;
    const double rank1 = 1.0;
    SANDAL_ASSERT(world::node_host_fn(&nodes, ctx, stats::kHostNodeType, &rank0, 1, out));
    SANDAL_ASSERT(std::fabs(out - static_cast<double>(kDivineType)) < 1e-6);

    SANDAL_ASSERT(world::node_host_fn(&nodes, ctx, stats::kHostNodeLevel, &rank0, 1, out));
    SANDAL_ASSERT(std::fabs(out - 6.0) < 1e-6);

    SANDAL_ASSERT(world::node_host_fn(&nodes, ctx, stats::kHostNodeLevel, &rank1, 1, out));
    SANDAL_ASSERT(std::fabs(out - 4.0) < 1e-6);

    SANDAL_ASSERT(world::node_host_fn(&nodes, ctx, stats::kHostPlayerCitizen, &rank0, 1, out));
    SANDAL_ASSERT(std::fabs(out - 0.0) < 1e-6);
    SANDAL_ASSERT(!world::node_host_fn(&nodes, ctx, 999999, nullptr, 0, out));
    return 0;
}

int test_city_node_stage_progression() {
    constexpr auto& kXp = world::node_constants::kExpToLevel;
    SANDAL_ASSERT(kXp.size() == 7U && kXp[0] == 0LL && kXp[6] == 1162745780LL);
    for (size_t stage = 1; stage < kXp.size(); ++stage) {
        SANDAL_ASSERT(kXp[stage] > kXp[stage - 1]);
    }
    SANDAL_ASSERT(world::node_constants::kMinimumOperationalNodeLevel == 3);
    SANDAL_ASSERT(world::node_constants::kNodeCitizenshipPlayerLevelRequirement == 10);
    SANDAL_ASSERT(world::node_constants::kNodePassiveIncomeSeconds == 3600);
    SANDAL_ASSERT(world::node_constants::kServiceBuildingMaintenanceFrequencyWeeks == 2);

    auto nodes = make_hermetic_city_nodes();
    SANDAL_ASSERT(nodes.size() == 2U && nodes.zones() == 2U);
    SANDAL_ASSERT(nodes.source() == world::LevelSource::Default);
    SANDAL_ASSERT(nodes.by_name("New Aela") != nullptr && nodes.by_name("new_aela")->rank == 0);
    SANDAL_ASSERT(nodes.by_name("NonExistentNode") == nullptr && nodes.by_name("") == nullptr);

    const auto site = nodes.node_at(-777664.5, 616578.7);
    SANDAL_ASSERT(site.zoi_guid == kAelaZoiGuid && site.node_rank == 0 && site.biome == kRiverlandsBiome);
    SANDAL_ASSERT(nodes.site_of_zoi(kJoevaZoiGuid).node_rank == 1);
    SANDAL_ASSERT(nodes.level_of(kAelaNodeGuid) == 0 && nodes.level_of_rank(1) == 0);

    for (int stage = 1; stage <= kMetropolisStage; ++stage) {
        const uint64_t prev_ver = nodes.version();
        SANDAL_ASSERT(nodes.apply_change({kAelaNodeGuid, stage, kXp[stage], 0LL}));
        SANDAL_ASSERT(nodes.level_of(kAelaNodeGuid) == stage);
        SANDAL_ASSERT(nodes.version() == prev_ver + 1U);
    }
    SANDAL_ASSERT(nodes.source() == world::LevelSource::Service);
    SANDAL_ASSERT(nodes.apply_change({kJoevaNodeGuid, 99, kXp[6], 0LL}));
    SANDAL_ASSERT(nodes.level_of_rank(1) == kMetropolisStage);
    SANDAL_ASSERT(nodes.apply_change({kJoevaNodeGuid, -4, 0LL, 0LL}));
    SANDAL_ASSERT(nodes.level_of_rank(1) == 0);
    SANDAL_ASSERT(nodes.apply_change({kJoevaNodeGuid, 4, kXp[4], kAelaNodeGuid}));
    SANDAL_ASSERT(!nodes.apply_change({999999LL, 2, 0LL, 0LL}));
    SANDAL_ASSERT(!nodes.describe_levels().empty());
    return verify_node_host_functions(nodes);
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "zoi_spatial_cell_partition")) {
        if (test_zoi_spatial_cell_partition() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "city_node_stage_progression")) {
        if (test_city_node_stage_progression() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
