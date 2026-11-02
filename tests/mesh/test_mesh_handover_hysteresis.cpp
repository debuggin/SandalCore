#include "../world/test_harness.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

enum class HandoverPhase : uint8_t {
    AuthoritativeSource = 0,
    PreWarmTarget = 1,
    PromotedTarget = 2,
    TeardownComplete = 3,
};

struct BoundarySessionState {
    HandoverPhase phase{HandoverPhase::AuthoritativeSource};
    int64_t last_swap_ms{-1000};
    bool has_active_cast_or_cc{false};
    bool target_healthy{true};
};

[[nodiscard]] float evaluate_handover_step(
    BoundarySessionState& s,
    float signed_border_dist_m,
    int64_t now_ms) noexcept {
    if (!s.target_healthy && signed_border_dist_m < -150.0F) {
        return -150.0F;
    }
    if (s.phase == HandoverPhase::AuthoritativeSource && signed_border_dist_m <= 100.0F) {
        s.phase = HandoverPhase::PreWarmTarget;
    }
    if (s.phase == HandoverPhase::PreWarmTarget && signed_border_dist_m <= 0.0F) {
        if (!s.has_active_cast_or_cc && (now_ms - s.last_swap_ms) >= 500) {
            s.phase = HandoverPhase::PromotedTarget;
            s.last_swap_ms = now_ms;
        }
    }
    if (s.phase == HandoverPhase::PromotedTarget && signed_border_dist_m <= -50.0F) {
        s.phase = HandoverPhase::TeardownComplete;
    }
    return signed_border_dist_m;
}

struct MeshForwardHeader12B {
    uint8_t version{1};
    uint8_t flags{0};
    uint32_t client_flow_ip{0};
    uint16_t client_port{0};
    uint8_t server_id{0};
    uint8_t lease_epoch{0};
};

[[nodiscard]] constexpr std::array<uint8_t, 12> encode_mesh_header_12b(const MeshForwardHeader12B& h) noexcept {
    return {
        static_cast<uint8_t>('S'),
        static_cast<uint8_t>('M'),
        h.version,
        h.flags,
        static_cast<uint8_t>((h.client_flow_ip >> 24U) & 0xFFU),
        static_cast<uint8_t>((h.client_flow_ip >> 16U) & 0xFFU),
        static_cast<uint8_t>((h.client_flow_ip >> 8U) & 0xFFU),
        static_cast<uint8_t>(h.client_flow_ip & 0xFFU),
        static_cast<uint8_t>((h.client_port >> 8U) & 0xFFU),
        static_cast<uint8_t>(h.client_port & 0xFFU),
        h.server_id,
        h.lease_epoch,
    };
}

[[nodiscard]] constexpr bool decode_mesh_header_12b(
    const std::array<uint8_t, 12>& wire,
    uint8_t min_lease_epoch,
    MeshForwardHeader12B& out) noexcept {
    if (wire[0] != static_cast<uint8_t>('S') || wire[1] != static_cast<uint8_t>('M') || wire[2] != 1U) {
        return false;
    }
    if (wire[11] < min_lease_epoch) {
        return false;
    }
    out.version = wire[2];
    out.flags = wire[3];
    out.client_flow_ip = (static_cast<uint32_t>(wire[4]) << 24U) |
                         (static_cast<uint32_t>(wire[5]) << 16U) |
                         (static_cast<uint32_t>(wire[6]) << 8U) |
                         static_cast<uint32_t>(wire[7]);
    out.client_port = static_cast<uint16_t>((static_cast<uint16_t>(wire[8]) << 8U) | wire[9]);
    out.server_id = wire[10];
    out.lease_epoch = wire[11];
    return true;
}

int test_hysteresis_band_200m_authority() {
    sandal::test::ManualClock clock{0};
    BoundarySessionState session{};

    SANDAL_ASSERT(evaluate_handover_step(session, 120.0F, clock.now_ms()) == 120.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::AuthoritativeSource);

    SANDAL_ASSERT(evaluate_handover_step(session, 100.0F, clock.now_ms()) == 100.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::PreWarmTarget);

    session.has_active_cast_or_cc = true;
    SANDAL_ASSERT(evaluate_handover_step(session, 0.0F, clock.now_ms()) == 0.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::PreWarmTarget);

    session.has_active_cast_or_cc = false;
    SANDAL_ASSERT(evaluate_handover_step(session, 0.0F, clock.now_ms()) == 0.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::PromotedTarget);

    session.phase = HandoverPhase::PreWarmTarget;
    clock.advance_ms(499);
    SANDAL_ASSERT(evaluate_handover_step(session, -5.0F, clock.now_ms()) == -5.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::PreWarmTarget);

    clock.advance_ms(1);
    SANDAL_ASSERT(evaluate_handover_step(session, -5.0F, clock.now_ms()) == -5.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::PromotedTarget);
    SANDAL_ASSERT(evaluate_handover_step(session, -50.0F, clock.now_ms()) == -50.0F);
    SANDAL_ASSERT(session.phase == HandoverPhase::TeardownComplete);

    session.target_healthy = false;
    SANDAL_ASSERT(evaluate_handover_step(session, -180.0F, clock.now_ms()) == -150.0F);
    return 0;
}

int test_mesh_forward_header_12b_epoch() {
    sandal::test::ManualClock clock{0};
    const MeshForwardHeader12B hdr{1U, 0x05U, 0x7F000001U, 7777U, 2U, 9U};
    auto wire = encode_mesh_header_12b(hdr);
    SANDAL_ASSERT(wire.size() == 12U);

    clock.advance_ticks(1, 30);
    MeshForwardHeader12B parsed{};
    SANDAL_ASSERT(decode_mesh_header_12b(wire, 9U, parsed));
    SANDAL_ASSERT(parsed.flags == 0x05U && parsed.client_flow_ip == 0x7F000001U);
    SANDAL_ASSERT(parsed.client_port == 7777U && parsed.server_id == 2U && parsed.lease_epoch == 9U);

    SANDAL_ASSERT(!decode_mesh_header_12b(wire, 10U, parsed));
    wire[0] = static_cast<uint8_t>('X');
    SANDAL_ASSERT(!decode_mesh_header_12b(wire, 1U, parsed));
    SANDAL_ASSERT(clock.now_ms() == 33);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "hysteresis_band_200m_authority")) {
        if (test_hysteresis_band_200m_authority() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "mesh_forward_header_12b_epoch")) {
        if (test_mesh_forward_header_12b_epoch() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
