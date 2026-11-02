#include "../world/test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>

#ifndef SANDAL_ASSERT_EQ
#define SANDAL_ASSERT_EQ(lhs, rhs) SANDAL_ASSERT((lhs) == (rhs))
#endif

namespace sandal::test {
namespace harness_ext {

class DeterministicPrng {
public:
    explicit constexpr DeterministicPrng(uint64_t seed = 0x9E3779B97F4A7C15ULL) noexcept
        : state_(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}

    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        state_ ^= state_ >> 12U;
        state_ ^= state_ << 25U;
        state_ ^= state_ >> 27U;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    [[nodiscard]] constexpr uint32_t next_u32(uint32_t bound) noexcept {
        return bound == 0U ? 0U : static_cast<uint32_t>(next_u64() % bound);
    }

private:
    uint64_t state_;
};

struct TestContext {
    DataFreeConfig config{};
    ManualClock clock{0};
    DeterministicPrng prng{0xB001C0DE9E3779B9ULL};
};

struct SuiteCase {
    std::string_view name;
    int (*fn)(TestContext&);
};

template <std::size_t N>
[[nodiscard]] int run_suite(int argc, char** argv, const std::array<SuiteCase, N>& cases) {
    TestContext ctx{parse_test_cli(argc, argv), ManualClock{0}, DeterministicPrng{0xB001C0DE9E3779B9ULL}};
    int ran = 0;
    for (const auto& tc : cases) {
        if (should_run_case(ctx.config, tc.name)) {
            ctx.clock = ManualClock{0};
            ctx.prng = DeterministicPrng{0xB001C0DE9E3779B9ULL};
            if (tc.fn(ctx) != 0) {
                return 1;
            }
            ++ran;
        }
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}

}  // namespace harness_ext
using namespace harness_ext;
}  // namespace sandal::test

namespace {

constexpr int64_t kTick30HzNs = 33'333'333LL;
constexpr float kLeashEvadeMeters = 140.0F;

enum class CastState : uint8_t {
    Idle = 0,
    Casting = 1,
    Completed = 2,
    Cancelled = 3,
};

enum class CastEventKind : uint8_t {
    TimerCompletionCallback = 0,
    TargetDeath = 1,
    IncomingStunCcDebuff = 2,
    LeashEvade = 3,
};

struct CastTickEvent {
    CastEventKind kind{CastEventKind::TimerCompletionCallback};
    uint64_t generation{0};
    float leash_distance_m{0.0F};
};

struct MobCastStateMachine {
    CastState state{CastState::Idle};
    uint64_t next_generation{0};
    uint64_t active_generation{0};
    int64_t cast_end_ns{0};
    int32_t target_hp{1000};
    bool has_stun_cc_debuff{false};
    bool leash_evading{false};
    uint32_t completed_casts{0};
    uint32_t cancelled_casts{0};
    uint32_t stale_callbacks_ignored{0};
    uint32_t phantom_hits{0};
};

uint64_t begin_mob_cast(MobCastStateMachine& mob, int64_t now_ns, int64_t duration_ns) noexcept {
    if (mob.target_hp <= 0 || mob.has_stun_cc_debuff || mob.leash_evading || duration_ns <= 0) {
        return 0ULL;
    }
    ++mob.next_generation;
    mob.active_generation = mob.next_generation;
    mob.cast_end_ns = now_ns + duration_ns;
    mob.state = CastState::Casting;
    return mob.active_generation;
}

bool stop_cast(MobCastStateMachine& mob) noexcept {
    if (mob.state != CastState::Casting) {
        return false;
    }
    mob.state = CastState::Cancelled;
    mob.active_generation = 0ULL;
    ++mob.cancelled_casts;
    return true;
}

void apply_cancellation_event(MobCastStateMachine& mob, const CastTickEvent& ev) noexcept {
    if (ev.kind == CastEventKind::TargetDeath) {
        mob.target_hp = 0;
        (void)stop_cast(mob);
    } else if (ev.kind == CastEventKind::IncomingStunCcDebuff) {
        mob.has_stun_cc_debuff = true;
        (void)stop_cast(mob);
    } else if (ev.kind == CastEventKind::LeashEvade && ev.leash_distance_m >= kLeashEvadeMeters) {
        mob.leash_evading = true;
        (void)stop_cast(mob);
    }
}

void apply_completion_callback(MobCastStateMachine& mob, const CastTickEvent& ev, int64_t now_ns) noexcept {
    if (ev.kind != CastEventKind::TimerCompletionCallback) {
        return;
    }
    if (ev.generation == 0ULL || ev.generation != mob.active_generation) {
        ++mob.stale_callbacks_ignored;
        return;
    }
    if (mob.state != CastState::Casting || now_ns < mob.cast_end_ns) {
        return;
    }
    if (mob.target_hp <= 0 || mob.has_stun_cc_debuff || mob.leash_evading) {
        ++mob.phantom_hits;
        (void)stop_cast(mob);
        return;
    }
    mob.state = CastState::Completed;
    mob.active_generation = 0ULL;
    ++mob.completed_casts;
}

template <std::size_t N>
void resolve_tick_queue(
    MobCastStateMachine& mob,
    int64_t now_ns,
    const std::array<CastTickEvent, N>& queue) noexcept {
    for (const auto& ev : queue) {
        apply_cancellation_event(mob, ev);
    }
    for (const auto& ev : queue) {
        apply_completion_callback(mob, ev, now_ns);
    }
}

int verify_same_tick_cancel_permutations(const CastTickEvent& cancel_trigger) {
    constexpr std::array<std::array<std::size_t, 3>, 6> kPermutations{{
        {0U, 1U, 2U},
        {0U, 2U, 1U},
        {1U, 0U, 2U},
        {1U, 2U, 0U},
        {2U, 0U, 1U},
        {2U, 1U, 0U},
    }};

    for (const auto& perm : kPermutations) {
        sandal::test::ManualClock clock{0};
        MobCastStateMachine mob{};
        const uint64_t gen = begin_mob_cast(mob, clock.now_ns(), 10LL * kTick30HzNs);
        SANDAL_ASSERT_EQ(gen, 1ULL);

        clock.advance_ns(10LL * kTick30HzNs);
        SANDAL_ASSERT_EQ(clock.now_ns(), mob.cast_end_ns);

        const std::array<CastTickEvent, 3> base_events{{
            {CastEventKind::TimerCompletionCallback, gen, 0.0F},
            cancel_trigger,
            {CastEventKind::TimerCompletionCallback, gen, 0.0F},
        }};
        const std::array<CastTickEvent, 3> ordered{{
            base_events[perm[0]],
            base_events[perm[1]],
            base_events[perm[2]],
        }};

        resolve_tick_queue(mob, clock.now_ns(), ordered);
        SANDAL_ASSERT(mob.state == CastState::Cancelled);
        SANDAL_ASSERT_EQ(mob.completed_casts, 0U);
        SANDAL_ASSERT_EQ(mob.phantom_hits, 0U);
        SANDAL_ASSERT_EQ(mob.cancelled_casts, 1U);
        SANDAL_ASSERT_EQ(mob.stale_callbacks_ignored, 2U);
    }
    return 0;
}

int test_mob_stop_cast_target_death_and_cc_race(sandal::test::TestContext& ctx) {
    (void)ctx;
    if (verify_same_tick_cancel_permutations({CastEventKind::TargetDeath, 0ULL, 0.0F}) != 0) {
        return 1;
    }
    if (verify_same_tick_cancel_permutations({CastEventKind::IncomingStunCcDebuff, 0ULL, 0.0F}) != 0) {
        return 1;
    }
    if (verify_same_tick_cancel_permutations({CastEventKind::LeashEvade, 0ULL, 140.0F}) != 0) {
        return 1;
    }

    sandal::test::ManualClock clock{0};
    MobCastStateMachine sub_leash_mob{};
    const uint64_t gen = begin_mob_cast(sub_leash_mob, clock.now_ns(), 6LL * kTick30HzNs);
    clock.advance_ns(6LL * kTick30HzNs);
    const std::array<CastTickEvent, 3> sub_leash_queue{{
        {CastEventKind::LeashEvade, 0ULL, 139.9F},
        {CastEventKind::TimerCompletionCallback, gen, 0.0F},
        {CastEventKind::TimerCompletionCallback, gen, 0.0F},
    }};
    resolve_tick_queue(sub_leash_mob, clock.now_ns(), sub_leash_queue);
    SANDAL_ASSERT(sub_leash_mob.state == CastState::Completed);
    SANDAL_ASSERT_EQ(sub_leash_mob.completed_casts, 1U);
    SANDAL_ASSERT_EQ(sub_leash_mob.phantom_hits, 0U);
    SANDAL_ASSERT_EQ(sub_leash_mob.stale_callbacks_ignored, 1U);
    return 0;
}

int test_mob_stop_cast_generation_token_immunity(sandal::test::TestContext& ctx) {
    MobCastStateMachine mob{};
    const uint64_t gen_n = begin_mob_cast(mob, ctx.clock.now_ns(), 20LL * kTick30HzNs);
    SANDAL_ASSERT_EQ(gen_n, 1ULL);

    ctx.clock.advance_ns(12LL * kTick30HzNs);
    SANDAL_ASSERT(stop_cast(mob));
    SANDAL_ASSERT(mob.state == CastState::Cancelled);

    const uint64_t gen_n_plus_1 = begin_mob_cast(mob, ctx.clock.now_ns(), 20LL * kTick30HzNs);
    SANDAL_ASSERT_EQ(gen_n_plus_1, 2ULL);
    SANDAL_ASSERT(mob.state == CastState::Casting);

    ctx.clock.advance_ns(8LL * kTick30HzNs);
    const std::array<CastTickEvent, 2> stale_queue{{
        {CastEventKind::TimerCompletionCallback, gen_n, 0.0F},
        {CastEventKind::TimerCompletionCallback, 999ULL, 0.0F},
    }};
    resolve_tick_queue(mob, ctx.clock.now_ns(), stale_queue);
    SANDAL_ASSERT(mob.state == CastState::Casting);
    SANDAL_ASSERT_EQ(mob.active_generation, gen_n_plus_1);
    SANDAL_ASSERT_EQ(mob.completed_casts, 0U);
    SANDAL_ASSERT_EQ(mob.stale_callbacks_ignored, 2U);

    ctx.clock.advance_ns(12LL * kTick30HzNs);
    const std::array<CastTickEvent, 2> final_queue{{
        {CastEventKind::TimerCompletionCallback, gen_n, 0.0F},
        {CastEventKind::TimerCompletionCallback, gen_n_plus_1, 0.0F},
    }};
    resolve_tick_queue(mob, ctx.clock.now_ns(), final_queue);
    SANDAL_ASSERT(mob.state == CastState::Completed);
    SANDAL_ASSERT_EQ(mob.completed_casts, 1U);
    SANDAL_ASSERT_EQ(mob.cancelled_casts, 1U);
    SANDAL_ASSERT_EQ(mob.stale_callbacks_ignored, 3U);
    SANDAL_ASSERT_EQ(mob.phantom_hits, 0U);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    constexpr std::array<sandal::test::SuiteCase, 2> kCases{{
        {"mob_stop_cast_target_death_and_cc_race", &test_mob_stop_cast_target_death_and_cc_race},
        {"mob_stop_cast_generation_token_immunity", &test_mob_stop_cast_generation_token_immunity},
    }};
    return sandal::test::run_suite(argc, argv, kCases);
}
