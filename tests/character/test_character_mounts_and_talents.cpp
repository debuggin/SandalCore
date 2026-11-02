#include "../world/test_harness.h"

#include <cstdint>

namespace {

struct MountSessionState {
    uint32_t active_mount_id{0};
    int64_t next_summon_ready_ms{0};
    bool has_cc_debuff{false};
};

[[nodiscard]] bool try_summon_mount(
    MountSessionState& state,
    uint32_t mount_id,
    float water_depth_cm,
    int64_t now_ms,
    int64_t cooldown_ms) noexcept {
    constexpr float kDeepWaterCm = 110.0F;
    if (mount_id == 0U || state.active_mount_id != 0U || state.has_cc_debuff) {
        return false;
    }
    if (water_depth_cm >= kDeepWaterCm || now_ms < state.next_summon_ready_ms) {
        return false;
    }
    state.active_mount_id = mount_id;
    state.next_summon_ready_ms = now_ms + cooldown_ms;
    return true;
}

[[nodiscard]] bool update_mount_water_immersion(MountSessionState& state, float water_depth_cm) noexcept {
    constexpr float kDeepWaterCm = 110.0F;
    if (state.active_mount_id != 0U && water_depth_cm >= kDeepWaterCm) {
        state.active_mount_id = 0U;
        return true;
    }
    return false;
}

struct TalentTreeState {
    uint8_t available_points{0};
    uint8_t spent_points{0};
};

struct TalentNode {
    uint8_t rank{0};
    uint8_t max_rank{3};
    uint8_t required_spent{0};
};

[[nodiscard]] bool allocate_talent_rank(TalentTreeState& tree, TalentNode& node) noexcept {
    if (tree.available_points == 0U || node.rank >= node.max_rank) {
        return false;
    }
    if (tree.spent_points < node.required_spent) {
        return false;
    }
    ++node.rank;
    --tree.available_points;
    ++tree.spent_points;
    return true;
}

int test_mount_cooldown_manual_clock() {
    sandal::test::ManualClock clock{1'000'000'000LL};
    MountSessionState mount{};

    SANDAL_ASSERT(!try_summon_mount(mount, 501U, 110.0F, clock.now_ms(), 3000));
    SANDAL_ASSERT(try_summon_mount(mount, 501U, 45.0F, clock.now_ms(), 3000));
    SANDAL_ASSERT(mount.active_mount_id == 501U);

    SANDAL_ASSERT(!update_mount_water_immersion(mount, 109.9F));
    SANDAL_ASSERT(update_mount_water_immersion(mount, 110.0F));
    SANDAL_ASSERT(mount.active_mount_id == 0U);

    clock.advance_ms(2999);
    SANDAL_ASSERT(!try_summon_mount(mount, 501U, 0.0F, clock.now_ms(), 3000));

    clock.advance_ms(1);
    mount.has_cc_debuff = true;
    SANDAL_ASSERT(!try_summon_mount(mount, 501U, 0.0F, clock.now_ms(), 3000));

    mount.has_cc_debuff = false;
    SANDAL_ASSERT(try_summon_mount(mount, 501U, 0.0F, clock.now_ms(), 3000));
    SANDAL_ASSERT(mount.active_mount_id == 501U);
    return 0;
}

int test_talent_tree_rank_allocation() {
    sandal::test::ManualClock clock{0};
    TalentTreeState tree{4U, 0U};
    TalentNode tier0{0U, 3U, 0U};
    TalentNode tier1{0U, 2U, 3U};

    SANDAL_ASSERT(!allocate_talent_rank(tree, tier1));
    SANDAL_ASSERT(allocate_talent_rank(tree, tier0));
    SANDAL_ASSERT(allocate_talent_rank(tree, tier0));
    SANDAL_ASSERT(!allocate_talent_rank(tree, tier1));
    SANDAL_ASSERT(allocate_talent_rank(tree, tier0));
    SANDAL_ASSERT(!allocate_talent_rank(tree, tier0));

    clock.advance_ticks(1, 30);
    SANDAL_ASSERT(tree.spent_points == 3U && tree.available_points == 1U);
    SANDAL_ASSERT(allocate_talent_rank(tree, tier1));
    SANDAL_ASSERT(tier1.rank == 1U && tree.available_points == 0U);
    SANDAL_ASSERT(!allocate_talent_rank(tree, tier1));
    SANDAL_ASSERT(clock.now_ms() == 33);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "mount_cooldown_manual_clock")) {
        if (test_mount_cooldown_manual_clock() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "talent_tree_rank_allocation")) {
        if (test_talent_tree_rank_allocation() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
