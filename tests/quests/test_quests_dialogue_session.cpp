#include "../world/test_harness.h"

#include <array>
#include <cstdint>

namespace {

struct DialogueContext {
    uint64_t player_node_id{100U};
    uint64_t npc_node_id{100U};
    float distance_m{3.0F};
    bool is_citizen{false};
    bool player_alive{true};
};

[[nodiscard]] uint32_t count_visible_npc_lines(const DialogueContext& ctx) noexcept {
    constexpr float kMaxTalkDistM = 6.0F;
    if (!ctx.player_alive || ctx.distance_m > kMaxTalkDistM) {
        return 0U;
    }
    uint32_t visible = 2U;
    if (ctx.player_node_id == ctx.npc_node_id && ctx.is_citizen) {
        visible += 2U;
    }
    return visible;
}

struct DialogueSession {
    uint64_t active_node{0U};
    int64_t last_step_ms{-1'000};
    bool open{false};
};

[[nodiscard]] bool step_dialogue_choice(
    DialogueSession& session,
    uint64_t expected_node,
    int32_t choice_index,
    int64_t now_ms) noexcept {
    constexpr std::array<uint64_t, 3> kChildNodes{2001U, 2002U, 0U};
    if (!session.open || session.active_node != expected_node) {
        return false;
    }
    if (choice_index < 0 || static_cast<size_t>(choice_index) >= kChildNodes.size()) {
        return false;
    }
    if ((now_ms - session.last_step_ms) < 100) {
        return false;
    }
    session.last_step_ms = now_ms;
    const uint64_t next = kChildNodes[static_cast<size_t>(choice_index)];
    if (next == 0U) {
        session.open = false;
        session.active_node = 0U;
    } else {
        session.active_node = next;
    }
    return true;
}

int test_npc_dialogue_prerequisite_gate(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    sandal::test::ManualClock clock{0};

    DialogueContext ctx{};
    SANDAL_ASSERT(count_visible_npc_lines(ctx) == 2U);

    ctx.is_citizen = true;
    clock.advance_ticks(1, 30);
    SANDAL_ASSERT(count_visible_npc_lines(ctx) == 4U);

    ctx.player_node_id = 200U;
    SANDAL_ASSERT(count_visible_npc_lines(ctx) == 2U);

    ctx.player_node_id = 100U;
    ctx.distance_m = 6.1F;
    SANDAL_ASSERT(count_visible_npc_lines(ctx) == 0U);

    ctx.distance_m = 2.0F;
    ctx.player_alive = false;
    SANDAL_ASSERT(count_visible_npc_lines(ctx) == 0U);
    SANDAL_ASSERT(clock.now_ms() == 33);
    return 0;
}

int test_dialogue_choice_branch_transition() {
    sandal::test::ManualClock clock{500'000'000LL};
    DialogueSession session{1000U, 0, true};

    SANDAL_ASSERT(!step_dialogue_choice(session, 9999U, 0, clock.now_ms()));
    SANDAL_ASSERT(!step_dialogue_choice(session, 1000U, -1, clock.now_ms()));
    SANDAL_ASSERT(!step_dialogue_choice(session, 1000U, 3, clock.now_ms()));
    SANDAL_ASSERT(session.active_node == 1000U && session.open);

    SANDAL_ASSERT(step_dialogue_choice(session, 1000U, 0, clock.now_ms()));
    SANDAL_ASSERT(session.active_node == 2001U && session.open);

    clock.advance_ms(50);
    SANDAL_ASSERT(!step_dialogue_choice(session, 2001U, 1, clock.now_ms()));

    clock.advance_ms(50);
    SANDAL_ASSERT(step_dialogue_choice(session, 2001U, 2, clock.now_ms()));
    SANDAL_ASSERT(!session.open && session.active_node == 0U);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "npc_dialogue_prerequisite_gate")) {
        if (test_npc_dialogue_prerequisite_gate(cfg) != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "dialogue_choice_branch_transition")) {
        if (test_dialogue_choice_branch_transition() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
