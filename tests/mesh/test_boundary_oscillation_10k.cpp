// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "include/aocnet/mesh_envelope.h"
#include "world/mesh_border.h"

namespace sandal::test {

template <typename Fn>
[[nodiscard]] int run_case_dispatch(int argc, char** argv, std::string_view case_name, Fn&& fn) {
    const DataFreeConfig cfg = parse_test_cli(argc, argv);
    if (!should_run_case(cfg, case_name)) {
        return 0;
    }
    return fn(cfg);
}

}  // namespace sandal::test

namespace {

constexpr uint32_t kOscillationSteps = 10000U;

int test_world_border_deadband_10k() {
    using namespace aoc::worldd::world;
    AuthorityState local_state = AuthorityState::Local;
    AuthorityState ghost_state = AuthorityState::RemoteGhost;

    for (uint32_t step = 0U; step < kOscillationSteps; ++step) {
        const float ratio = static_cast<float>(step % 2001U) / 1000.0F - 1.0F;
        const float dist_cm = ratio * kBorderHysteresisBandCm;
        local_state = evaluate_border_handover(dist_cm, local_state);
        ghost_state = evaluate_border_handover(dist_cm, ghost_state);
        SANDAL_ASSERT(local_state == AuthorityState::Local);
        SANDAL_ASSERT(ghost_state == AuthorityState::RemoteGhost);
    }

    SANDAL_ASSERT(
        evaluate_border_handover(kBorderHysteresisBandCm + 0.5F, AuthorityState::Local) ==
        AuthorityState::HandoverPending);
    SANDAL_ASSERT(
        evaluate_border_handover(kBorderHysteresisBandCm + 0.5F, AuthorityState::HandoverPending) ==
        AuthorityState::RemoteGhost);
    SANDAL_ASSERT(
        evaluate_border_handover(-kBorderHysteresisBandCm - 0.5F, AuthorityState::HandoverPending) ==
        AuthorityState::Local);
    SANDAL_ASSERT(
        evaluate_border_handover(-kBorderHysteresisBandCm - 0.5F, AuthorityState::RemoteGhost) ==
        AuthorityState::Local);

    uint64_t current_epoch = 42U;
    SANDAL_ASSERT(!is_newer_lease_epoch(41U, current_epoch));
    SANDAL_ASSERT(!is_newer_lease_epoch(42U, current_epoch));
    SANDAL_ASSERT(is_newer_lease_epoch(43U, current_epoch));

    AuthorityState pending = AuthorityState::HandoverPending;
    SANDAL_ASSERT(complete_border_handover(pending, 41U, current_epoch) == AuthorityState::HandoverPending);
    SANDAL_ASSERT(complete_border_handover(pending, 42U, current_epoch) == AuthorityState::HandoverPending);
    SANDAL_ASSERT(complete_border_handover(pending, 43U, current_epoch) == AuthorityState::RemoteGhost);
    return 0;
}

int test_mesh_envelope_hysteresis_10k() {
    using namespace aoc::net;
    constexpr uint32_t kZoneA = 101U;
    constexpr uint32_t kZoneB = 202U;
    uint32_t auth_from_a = kZoneA;
    uint32_t auth_from_b = kZoneB;
    uint32_t epoch = 1U;

    for (uint32_t step = 0U; step < kOscillationSteps; ++step) {
        const float ratio = static_cast<float>(step % 2001U) / 1000.0F - 1.0F;
        const float sub_band_cm = ratio * kMeshBoundaryHysteresisCm;
        auth_from_a = resolve_hysteresis_authority(sub_band_cm, auth_from_a, kZoneA, kZoneB);
        auth_from_b = resolve_hysteresis_authority(sub_band_cm, auth_from_b, kZoneA, kZoneB);
        SANDAL_ASSERT(auth_from_a == kZoneA && auth_from_b == kZoneB);

        const float wave_cm = ((step & 1U) == 0U)
                                  ? (kMeshBoundaryHysteresisCm + 25.0F)
                                  : -(kMeshBoundaryHysteresisCm + 25.0F);
        const uint32_t next_zone = resolve_hysteresis_authority(wave_cm, kZoneA, kZoneA, kZoneB);
        SANDAL_ASSERT(next_zone == kZoneA || next_zone == kZoneB);

        const uint32_t candidate_epoch = epoch + 1U;
        SANDAL_ASSERT(is_fenced_epoch_valid(candidate_epoch, epoch));
        SANDAL_ASSERT(!is_fenced_epoch_valid(epoch, candidate_epoch));

        std::array<uint8_t, kMeshForwardHeaderBytes> wire{};
        const MeshForwardHeader hdr{kZoneA, next_zone, candidate_epoch};
        SANDAL_ASSERT(pack_mesh_forward_header(hdr, wire.data(), wire.size()));

        MeshForwardHeader decoded{};
        SANDAL_ASSERT(unpack_mesh_forward_header(wire.data(), wire.size(), decoded));
        SANDAL_ASSERT(decoded.source_zone_id == kZoneA && decoded.target_zone_id == next_zone);
        SANDAL_ASSERT(decoded.authority_epoch == candidate_epoch);
        epoch = candidate_epoch;
    }

    std::array<uint8_t, 8> truncated{};
    MeshForwardHeader dummy{};
    SANDAL_ASSERT(!pack_mesh_forward_header(dummy, truncated.data(), truncated.size()));
    SANDAL_ASSERT(!unpack_mesh_forward_header(truncated.data(), truncated.size(), dummy));
    return 0;
}

int run_boundary_oscillation_10k(const sandal::test::DataFreeConfig&) {
    if (test_world_border_deadband_10k() != 0) {
        return 1;
    }
    return test_mesh_envelope_hysteresis_10k();
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(
        argc, argv, "boundary_oscillation_10k", run_boundary_oscillation_10k);
}
