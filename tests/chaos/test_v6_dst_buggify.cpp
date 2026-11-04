// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "content/content_snapshot.h"
#include "include/aocnet/mesh_envelope.h"
#include "net/spsc_ring.h"
#include "net/tick_loop.h"
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

namespace content = aoc::worldd::content;
namespace world = aoc::worldd::world;
namespace worldd = aoc::worldd;

constexpr uint32_t kDstSteps = 500U;
constexpr std::string_view kValidShaA =
    "1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff";
constexpr std::string_view kValidShaB =
    "ffffeeeeddddccccbbbbaaaa0000999988887777666655554444333322221111";

[[nodiscard]] constexpr bool buggify(sandal::test::DeterministicPrng& prng, uint32_t pct) noexcept {
    return prng.next_u32(100U) < pct;
}

[[nodiscard]] constexpr uint64_t mix_digest(uint64_t h, uint64_t val) noexcept {
    uint64_t x = (h ^ val) * 0x100000001B3ULL;
    x ^= (x >> 30U);
    return x * 0xBF58476D1CE4E5B9ULL;
}

struct DstSimState {
    worldd::InboundNetRing in_ring{};
    worldd::OutboundNetRing out_ring{};
    worldd::AsyncTaskCompletionRing task_ring{};
    content::ContentSnapshotRegistry snap_reg{};
    worldd::TickLoopBudget budget{};
    world::AuthorityState border_state{world::AuthorityState::Local};
    uint32_t active_epoch{1U}, accepted_in{0U}, flushed_out{0U}, completed_tasks{0U}, handovers{0U}, buggify_faults{0U};
    uint64_t rcu_ver{1U}, digest{0xCBF29CE484222325ULL};
};

std::shared_ptr<content::ContentSnapshot> make_dst_snapshot(uint64_t ver, std::string_view sha) {
    auto snap = std::make_shared<content::ContentSnapshot>();
    snap->version = ver;
    snap->sha256_digest = std::string{sha};
    snap->quality_cache.emplace(42, static_cast<int>(ver));
    return snap;
}

int step_network_and_tasks(uint32_t step, DstSimState& st, sandal::test::DeterministicPrng& prng) {
    const worldd::Address peer{0x7F000001U, static_cast<uint16_t>(7000U + (step % 64U))};
    std::array<uint8_t, 4> pkt{{static_cast<uint8_t>(step & 0xFFU), 0xA5U, 0x5AU, 0x01U}};
    if (buggify(prng, 20U)) { pkt[1] = 0x00U; ++st.buggify_faults; }
    if (!buggify(prng, 15U)) {
        SANDAL_ASSERT(st.in_ring.try_push(worldd::InboundNetFrame::make(peer, pkt.data(), pkt.size())));
    } else {
        ++st.buggify_faults;
    }
    const bool task_ok = !buggify(prng, 15U);
    if (!task_ok) { ++st.buggify_faults; }
    SANDAL_ASSERT(st.task_ring.try_push(worldd::AsyncTaskCompletion{step + 1U, worldd::LoginResult{task_ok, "", {}}}));

    worldd::InboundNetFrame frame{};
    while (st.in_ring.try_pop(frame)) {
        if (frame.len == 4U && frame.payload[1] == 0xA5U) {
            ++st.accepted_in;
            SANDAL_ASSERT(st.out_ring.try_push(worldd::OutboundNetFrame::make(frame.from, frame.payload.data(), frame.len)));
        }
    }
    if (!buggify(prng, 25U)) {
        worldd::AsyncTaskCompletion ev{};
        while (st.task_ring.try_pop(ev)) {
            if (ev.result.accepted) { ++st.completed_tasks; }
        }
    }
    worldd::OutboundNetFrame out_f{};
    while (st.out_ring.try_pop(out_f)) {
        SANDAL_ASSERT(out_f.len == 4U);
        ++st.flushed_out;
    }
    return 0;
}

int step_rcu_and_border(uint32_t step, DstSimState& st, sandal::test::DeterministicPrng& prng) {
    const auto pinned = st.snap_reg.current();
    SANDAL_ASSERT(pinned != nullptr && pinned->version == st.rcu_ver);
    std::string err;
    double swap_ms = -1.0;
    if ((step % 10U) == 0U) {
        if (buggify(prng, 30U)) {
            SANDAL_ASSERT(!st.snap_reg.publish(make_dst_snapshot(st.rcu_ver, "invalid_sha"), err, &swap_ms));
            ++st.buggify_faults;
        } else {
            ++st.rcu_ver;
            const auto sha = (st.rcu_ver % 2U == 0U) ? kValidShaA : kValidShaB;
            SANDAL_ASSERT(st.snap_reg.publish(make_dst_snapshot(st.rcu_ver, sha), err, &swap_ms) && err.empty());
            SANDAL_ASSERT(swap_ms >= 0.0 && swap_ms < content::kMaxHotReloadSwapMs);
        }
    }
    SANDAL_ASSERT(pinned->item_quality(42) == static_cast<int>(pinned->version));

    const float dist = (step % 3U == 0U) ? (world::kBorderHysteresisBandCm + 250.0F) : -2500.0F;
    st.border_state = world::evaluate_border_handover(dist, st.border_state);
    if (st.border_state == world::AuthorityState::HandoverPending) {
        const uint32_t cand_epoch = buggify(prng, 30U) ? st.active_epoch : (st.active_epoch + 1U);
        std::array<uint8_t, aocnet::kMeshForwardHeaderBytes> wire{};
        SANDAL_ASSERT(aocnet::pack_mesh_forward_header(aocnet::MeshForwardHeader{1U, 2U, cand_epoch}, wire));
        aocnet::MeshForwardHeader dec{};
        SANDAL_ASSERT(aocnet::unpack_mesh_forward_header(wire, dec));
        if (aocnet::is_fenced_epoch_valid(dec.authority_epoch, st.active_epoch)) {
            st.border_state = world::complete_border_handover(st.border_state, dec.authority_epoch, st.active_epoch);
            SANDAL_ASSERT(st.border_state == world::AuthorityState::RemoteGhost);
            st.border_state = world::AuthorityState::Local;
            st.active_epoch = dec.authority_epoch;
            ++st.handovers;
        } else {
            ++st.buggify_faults;
        }
    }
    return 0;
}

int run_dst_simulation(uint64_t seed, DstSimState& st) {
    sandal::test::DeterministicPrng prng(seed);
    std::string err;
    SANDAL_ASSERT(st.snap_reg.publish(make_dst_snapshot(1U, kValidShaA), err) && err.empty());
    for (uint32_t step = 0U; step < kDstSteps; ++step) {
        SANDAL_ASSERT(step_network_and_tasks(step, st, prng) == 0);
        SANDAL_ASSERT(step_rcu_and_border(step, st, prng) == 0);
        const worldd::TickPhaseBudgets phases{900U + prng.next_u32(400U), 3500U + prng.next_u32(800U), 2400U, 800U};
        st.budget.record_tick_phases(phases);
        SANDAL_ASSERT(!st.budget.last_exceeded_slo() && !st.budget.last_overran_tick());
        st.digest = mix_digest(st.digest, st.accepted_in ^ (static_cast<uint64_t>(st.rcu_ver) << 16U));
        st.digest = mix_digest(st.digest, st.handovers ^ (static_cast<uint64_t>(st.active_epoch) << 32U));
        st.digest = mix_digest(st.digest, st.completed_tasks ^ st.buggify_faults ^ phases.total_us());
    }
    SANDAL_ASSERT(st.budget.tick_index() == kDstSteps && st.accepted_in > 200U && st.flushed_out == st.accepted_in);
    SANDAL_ASSERT(st.rcu_ver > 20U && st.handovers > 20U && st.buggify_faults > 50U);
    return 0;
}

int run_v6_dst_buggify(const sandal::test::DataFreeConfig&) {
    auto run1 = std::make_unique<DstSimState>();
    auto run2 = std::make_unique<DstSimState>();
    auto run3 = std::make_unique<DstSimState>();
    if (run_dst_simulation(0x5636445354425547ULL, *run1) != 0) return 1;
    if (run_dst_simulation(0x5636445354425547ULL, *run2) != 0) return 1;
    if (run_dst_simulation(0x5636445354425548ULL, *run3) != 0) return 1;
    SANDAL_ASSERT(run1->digest == run2->digest && run1->accepted_in == run2->accepted_in);
    SANDAL_ASSERT(run1->rcu_ver == run2->rcu_ver && run1->handovers == run2->handovers);
    SANDAL_ASSERT(run1->buggify_faults == run2->buggify_faults && run1->digest != run3->digest);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "v6_dst_buggify", run_v6_dst_buggify);
}
