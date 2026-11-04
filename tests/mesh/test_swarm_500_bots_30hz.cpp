// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "net/tick_loop.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

#include <algorithm>

namespace sandal::test {

class DeterministicPrng {
public:
    explicit constexpr DeterministicPrng(uint64_t seed = 0x9E3779B97F4A7C15ULL) noexcept
        : state_(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}

    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        uint64_t x = (state_ += 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31U);
    }

    [[nodiscard]] constexpr float next_signed_unit() noexcept {
        const uint32_t bits = static_cast<uint32_t>(next_u64() & 0xFFFFFFULL);
        return (static_cast<float>(bits) / static_cast<float>(0x800000U)) - 1.0F;
    }

private:
    uint64_t state_;
};

class LatencyRecorder {
public:
    void record_us(uint64_t sample_us) {
        samples_.push_back(sample_us);
    }

    [[nodiscard]] uint64_t p99_us() const {
        if (samples_.empty()) {
            return 0U;
        }
        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t idx = ((sorted.size() * 99U) + 99U) / 100U - 1U;
        return sorted[std::min(idx, sorted.size() - 1U)];
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return samples_.size();
    }

private:
    std::vector<uint64_t> samples_{};
};

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

constexpr std::size_t kBotCount = 500U;
constexpr uint64_t kTotalTicks = 90U;

struct BotCoord {
    uint64_t id{0U};
    float x_cm{0.0F};
    float y_cm{0.0F};
};

int populate_swarm_bots(
    aoc::worldd::world::ActorRegistry& registry,
    std::vector<BotCoord>& bots,
    sandal::test::DeterministicPrng& prng) {
    using namespace aoc::worldd::world;
    bots.resize(kBotCount);
    for (std::size_t i = 0U; i < kBotCount; ++i) {
        const uint64_t id = static_cast<uint64_t>((i + 1U) * 2U);
        const float cell_x = static_cast<float>(static_cast<int32_t>(i % 10U) - 5) * kSpatialCellSizeCm;
        const float cell_y = static_cast<float>(static_cast<int32_t>((i / 10U) % 10U) - 5) * kSpatialCellSizeCm;
        const float x_cm = cell_x + (prng.next_signed_unit() * 4500.0F);
        const float y_cm = cell_y + (prng.next_signed_unit() * 4500.0F);

        WorldActor actor{};
        actor.id = id;
        actor.actor.guid.ObjectId = id;
        actor.x = x_cm;
        actor.y = y_cm;
        actor.z = 100.0;
        SANDAL_ASSERT(registry.add(actor));
        bots[i] = BotCoord{id, x_cm, y_cm};
    }
    SANDAL_ASSERT(registry.size() == kBotCount && registry.occupied_cell_count() >= 25U);
    return 0;
}

int simulate_single_tick(
    uint64_t tick,
    aoc::worldd::world::ActorRegistry& registry,
    std::vector<BotCoord>& bots,
    sandal::test::DeterministicPrng& prng,
    uint64_t& replicated_Accum) {
    using namespace aoc::worldd::world;
    for (auto& bot : bots) {
        bot.x_cm += prng.next_signed_unit() * 200.0F;
        bot.y_cm += prng.next_signed_unit() * 200.0F;
        SANDAL_ASSERT(registry.move(bot.id, bot.x_cm, bot.y_cm, 100.0, 0.0));
    }

    std::array<uint64_t, kBotCount> candidates{};
    for (std::size_t observer = 0U; observer < kBotCount; observer += 25U) {
        const auto& obs = bots[observer];
        const std::size_t ring_count = registry.count_actors_in_cell_ring(obs.x_cm, obs.y_cm, 2);
        const std::size_t gathered = registry.gather_aoi_candidates(
            obs.x_cm, obs.y_cm, 2, candidates.data(), candidates.size());
        SANDAL_ASSERT(gathered == ring_count && gathered > 0U);
        for (std::size_t c = 0U; c < gathered; ++c) {
            const BotCoord& target = bots[(candidates[c] / 2U) - 1U];
            const float dx = target.x_cm - obs.x_cm;
            const float dy = target.y_cm - obs.y_cm;
            const RelevancyTier tier = classify_relevancy_tier((dx * dx) + (dy * dy));
            if (should_replicate_subring(tier, tick, static_cast<uint32_t>(candidates[c]))) {
                ++replicated_Accum;
            }
        }
    }
    return 0;
}

int run_swarm_500_bots_30hz(const sandal::test::DataFreeConfig&) {
    aoc::worldd::world::ActorRegistry registry;
    std::vector<BotCoord> bots;
    sandal::test::DeterministicPrng prng{0xC0FFEE500B075ULL};
    if (populate_swarm_bots(registry, bots, prng) != 0) {
        return 1;
    }

    aoc::worldd::TickLoopBudget budget;
    sandal::test::LatencyRecorder latency;
    uint64_t replicated_total = 0U;

    for (uint64_t tick = 0U; tick < kTotalTicks; ++tick) {
        const auto start = std::chrono::steady_clock::now();
        if (simulate_single_tick(tick, registry, bots, prng, replicated_total) != 0) {
            return 1;
        }
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        budget.record_tick_duration_us(elapsed_us);
        latency.record_us(elapsed_us);
    }

    SANDAL_ASSERT(latency.count() == kTotalTicks && budget.tick_index() == kTotalTicks);
    SANDAL_ASSERT(replicated_total > 0U);
    SANDAL_ASSERT(latency.p99_us() < aoc::worldd::TickLoopBudget::kTickSloP99Us);
    SANDAL_ASSERT(budget.overrun_count() == 0U && budget.slo_violation_count() == 0U);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "swarm_500_bots_30hz", run_swarm_500_bots_30hz);
}
