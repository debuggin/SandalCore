// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "include/aocnet/mesh_envelope.h"
#include "world/mesh_border.h"

namespace sandal::test {

class DeterministicPrng {
public:
    explicit constexpr DeterministicPrng(uint64_t seed) noexcept
        : state_(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}
    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        uint64_t x = (state_ += 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31U);
    }
    [[nodiscard]] constexpr uint32_t next_u32(uint32_t bound) noexcept {
        return bound == 0U ? 0U : static_cast<uint32_t>(next_u64() % bound);
    }
private:
    uint64_t state_;
};

template <typename Fn>
[[nodiscard]] int run_case_dispatch(int argc, char** argv, std::string_view case_name, Fn&& fn) {
    const DataFreeConfig cfg = parse_test_cli(argc, argv);
    return should_run_case(cfg, case_name) ? fn(cfg) : 0;
}

}  // namespace sandal::test

namespace {

namespace world = aoc::worldd::world;

constexpr std::size_t kBorderActorCount = 200U;
constexpr uint32_t kZoneWest = 101U;
constexpr uint32_t kZoneEast = 202U;
constexpr uint32_t kDeadNodeEpoch = 50U;
constexpr uint32_t kPromotedEpoch = 51U;

struct BorderActorHandover {
    uint64_t actor_id{0U};
    world::AuthorityState source_state{world::AuthorityState::Local};
    aocnet::MeshHandoverState mesh_phase{aocnet::MeshHandoverState::Promoting};
    uint32_t owner_zone{kZoneWest};
    uint32_t active_epoch{kDeadNodeEpoch};
};

int init_200_border_actors(std::array<BorderActorHandover, kBorderActorCount>& actors,
                           sandal::test::DeterministicPrng& prng) {
    for (std::size_t i = 0U; i < kBorderActorCount; ++i) {
        BorderActorHandover& a = actors[i];
        a.actor_id = static_cast<uint64_t>((i + 1U) * 2U);
        const bool is_preparing = (i < (kBorderActorCount / 2U));
        a.mesh_phase = is_preparing ? aocnet::MeshHandoverState::Promoting
                                    : aocnet::MeshHandoverState::Demoting;
        const float crossing_dist = world::kBorderHysteresisBandCm + 50.0F +
                                    static_cast<float>(prng.next_u32(500U));
        a.source_state = world::evaluate_border_handover(crossing_dist, world::AuthorityState::Local);
        SANDAL_ASSERT(a.source_state == world::AuthorityState::HandoverPending);
        a.owner_zone = kZoneWest;
        a.active_epoch = kDeadNodeEpoch;
    }
    return 0;
}

int test_sigkill_mid_handover_200_actors() {
    sandal::test::DeterministicPrng prng(0x56355349474B494CULL);
    std::array<BorderActorHandover, kBorderActorCount> actors{};
    SANDAL_ASSERT(init_200_border_actors(actors, prng) == 0);
    SANDAL_ASSERT(world::is_newer_lease_epoch(kPromotedEpoch, kDeadNodeEpoch));
    SANDAL_ASSERT(aocnet::is_fenced_epoch_valid(kPromotedEpoch, kDeadNodeEpoch));

    for (auto& a : actors) {
        std::array<uint8_t, aocnet::kMeshForwardHeaderBytes> wire{};
        const aocnet::MeshForwardHeader takeover_hdr{kZoneWest, kZoneEast, kPromotedEpoch};
        SANDAL_ASSERT(aocnet::pack_mesh_forward_header(takeover_hdr, wire));

        aocnet::MeshForwardHeader decoded{};
        SANDAL_ASSERT(aocnet::unpack_mesh_forward_header(wire, decoded));
        SANDAL_ASSERT(decoded.source_zone_id == kZoneWest && decoded.target_zone_id == kZoneEast);
        SANDAL_ASSERT(aocnet::is_fenced_epoch_valid(decoded.authority_epoch, a.active_epoch));

        a.source_state = world::complete_border_handover(
            a.source_state, decoded.authority_epoch, a.active_epoch);
        SANDAL_ASSERT(a.source_state == world::AuthorityState::RemoteGhost);
        a.owner_zone = aocnet::resolve_hysteresis_authority(
            aocnet::kMeshBoundaryHysteresisCm + 100.0F, a.owner_zone, kZoneWest, kZoneEast);
        a.active_epoch = decoded.authority_epoch;
        a.mesh_phase = aocnet::MeshHandoverState::Authority;
        SANDAL_ASSERT(a.owner_zone == kZoneEast && a.active_epoch == kPromotedEpoch);

        const uint32_t zombie_epoch = kDeadNodeEpoch - (prng.next_u32(10U));
        SANDAL_ASSERT(!world::is_newer_lease_epoch(zombie_epoch, a.active_epoch));
        SANDAL_ASSERT(!aocnet::is_fenced_epoch_valid(zombie_epoch, a.active_epoch));
        SANDAL_ASSERT(world::complete_border_handover(
                          world::AuthorityState::HandoverPending, zombie_epoch, a.active_epoch) ==
                      world::AuthorityState::HandoverPending);
    }
    return 0;
}

int test_cascading_double_crash_and_zombie_fencing() {
    uint32_t active_epoch = 10U;
    uint32_t active_zone = kZoneWest;
    world::AuthorityState state = world::AuthorityState::Local;

    state = world::evaluate_border_handover(world::kBorderHysteresisBandCm + 10.0F, state);
    SANDAL_ASSERT(state == world::AuthorityState::HandoverPending);
    SANDAL_ASSERT(world::complete_border_handover(state, 10U, active_epoch) ==
                  world::AuthorityState::HandoverPending);

    for (uint32_t next_epoch : {11U, 12U}) {
        SANDAL_ASSERT(world::is_newer_lease_epoch(next_epoch, active_epoch));
        SANDAL_ASSERT(aocnet::is_fenced_epoch_valid(next_epoch, active_epoch));
        state = world::complete_border_handover(
            world::AuthorityState::HandoverPending, next_epoch, active_epoch);
        SANDAL_ASSERT(state == world::AuthorityState::RemoteGhost);
        active_epoch = next_epoch;
        active_zone = aocnet::resolve_hysteresis_authority(
            (next_epoch == 11U) ? (aocnet::kMeshBoundaryHysteresisCm + 50.0F) : 0.0F,
            active_zone,
            kZoneWest,
            kZoneEast);
        SANDAL_ASSERT(active_zone == kZoneEast);
    }

    for (uint32_t stale_epoch : {0U, 1U, 10U, 11U, 12U}) {
        SANDAL_ASSERT(!world::is_newer_lease_epoch(stale_epoch, active_epoch));
        SANDAL_ASSERT(!aocnet::is_fenced_epoch_valid(stale_epoch, active_epoch));
    }

    SANDAL_ASSERT(aocnet::is_within_hysteresis_band(0.0F));
    SANDAL_ASSERT(!aocnet::is_within_hysteresis_band(aocnet::kMeshBoundaryHysteresisCm + 1.0F));
    SANDAL_ASSERT(aocnet::resolve_hysteresis_authority(
                      -aocnet::kMeshBoundaryHysteresisCm - 5.0F, active_zone, kZoneWest, kZoneEast) == kZoneWest);

    std::array<uint8_t, aocnet::kMeshForwardHeaderBytes> raw{};
    const aocnet::MeshForwardHeader valid_hdr{kZoneWest, kZoneEast, active_epoch};
    SANDAL_ASSERT(aocnet::pack_mesh_forward_header(valid_hdr, raw));
    aocnet::MeshForwardHeader out{};
    SANDAL_ASSERT(!aocnet::pack_mesh_forward_header(valid_hdr, nullptr, 12U));
    SANDAL_ASSERT(!aocnet::unpack_mesh_forward_header(nullptr, 12U, out));
    for (std::size_t len = 0U; len < aocnet::kMeshForwardHeaderBytes; ++len) {
        SANDAL_ASSERT(!aocnet::unpack_mesh_forward_header(raw.data(), len, out));
    }
    return 0;
}

int run_v5_sigkill_handover(const sandal::test::DataFreeConfig&) {
    return (test_sigkill_mid_handover_200_actors() == 0 &&
            test_cascading_double_crash_and_zombie_fencing() == 0)
               ? 0
               : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "v5_sigkill_handover", run_v5_sigkill_handover);
}
