#include "../world/test_harness.h"

#include "net/cookie.h"
#include "protocol/conn/packet_notify.h"
#include "protocol/conn/sequence.h"
#include "protocol/emit/bunch_writer.h"
#include "quest/quest_log.h"
#include "session/move_checker.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace qst = aoc::worldd::quest;
namespace cnt = aoc::worldd::content;

int test_udp_jitter_loss_reorder_seam() {
    SANDAL_ASSERT(conn::seq_diff(0U, 16382U) == 2);
    SANDAL_ASSERT(conn::seq_greater(1U, 16383U));
    SANDAL_ASSERT(conn::seq_add(16382U, 4) == 2U);
    SANDAL_ASSERT(conn::make_relative(2, 16382, 1024) == 16386);

    conn::PacketNotify sender{};
    conn::PacketNotify receiver{};
    sender.init(49U, 16382U);
    receiver.init(16381U, 50U);

    const conn::NotifyHeader pkt0 = sender.make_header();
    const uint16_t s0 = sender.commit_and_increment_out_seq();
    const uint16_t s1 = sender.commit_and_increment_out_seq();
    const conn::NotifyHeader pkt2 = sender.make_header();
    const uint16_t s2 = sender.commit_and_increment_out_seq();
    SANDAL_ASSERT(s0 == 16382U && s1 == 16383U && s2 == 0U);

    SANDAL_ASSERT(receiver.update(pkt0, [](uint16_t, bool) {}) == 1);
    receiver.ack_seq(pkt0.seq);
    SANDAL_ASSERT(receiver.update(pkt2, [](uint16_t, bool) {}) == 2);
    receiver.ack_seq(pkt2.seq);
    const conn::NotifyHeader wire_hdr = receiver.make_header();
    (void)receiver.commit_and_increment_out_seq();

    emit::BunchWriter bw{};
    conn::write_notify_header(bw, wire_hdr);
    size_t bit_pos = 0U;
    const auto parsed_hdr = conn::read_notify_header(bw.data(), bw.byte_size(), bw.bit_pos(), bit_pos);
    SANDAL_ASSERT(parsed_hdr.has_value() && parsed_hdr->seq == wire_hdr.seq && parsed_hdr->acked_seq == s2);

    std::vector<uint16_t> acked{};
    std::vector<uint16_t> naked{};
    const int delta = sender.update(*parsed_hdr, [&](uint16_t seq, bool delivered) {
        if (delivered) acked.push_back(seq);
        else naked.push_back(seq);
    });
    SANDAL_ASSERT(delta == 1);
    SANDAL_ASSERT(acked.size() == 2U && acked[0] == 16382U && acked[1] == 0U);
    SANDAL_ASSERT(naked.size() == 1U && naked[0] == 16383U);
    SANDAL_ASSERT(sender.update(*parsed_hdr, [](uint16_t, bool) {}) == 0);
    return 0;
}

int test_cookie_and_quest_snapshot_fence() {
    uint8_t seed_counter = 1U;
    aoc::worldd::CookieSigner signer([&](uint8_t* out, size_t n) {
        for (size_t i = 0U; i < n; ++i) out[i] = static_cast<uint8_t>(seed_counter + i);
        ++seed_counter;
    });
    const uint8_t gen0 = signer.secret_id();
    const auto cookie0 = signer.sign(gen0, 10.0, "127.0.0.1:7777");
    SANDAL_ASSERT(signer.verify(gen0, 10.0, "127.0.0.1:7777", cookie0, 12.0));

    signer.update(26.0);
    SANDAL_ASSERT(signer.secret_id() != gen0);
    SANDAL_ASSERT(signer.verify(gen0, 10.0, "127.0.0.1:7777", cookie0, 26.0));
    SANDAL_ASSERT(!signer.verify(gen0, 10.0, "127.0.0.1:7777", cookie0, 51.0));
    SANDAL_ASSERT(!signer.verify(gen0, 10.0, "10.0.0.1:7777", cookie0, 26.0));

    cnt::QuestData qdata{};
    cnt::QuestRow row{};
    row.guid = 300;
    cnt::QuestObjectiveRow obj{};
    obj.guid = 301;
    obj.type = "Kill";
    obj.count = 4;
    obj.awake = true;
    row.objectives.push_back(obj);
    qdata.quests[300] = row;

    qst::QuestLog log{};
    SANDAL_ASSERT(log.start(row, 10.0, 99).instance != nullptr);
    log.instances_for_wire()[0].objectives[0].count = 3;

    qst::QuestLog restored{};
    SANDAL_ASSERT(restored.load(log.to_saved(), qdata, 20.0) == 0U);
    SANDAL_ASSERT(restored.find(300) != nullptr && restored.find(300)->objectives[0].count == 3);
    qst::QuestLog pruned{};
    SANDAL_ASSERT(pruned.load(log.to_saved(), cnt::QuestData{}, 20.0) == 1U && pruned.find(300) == nullptr);
    return 0;
}

int test_rcu_snapshot_generation_fence() {
    if (test_cookie_and_quest_snapshot_fence() != 0) return 1;

    aoc::worldd::MoveLimits limits{};
    aoc::worldd::GoodPosition start{0.0, 0.0, 0.0, 0.0, aoc::protocol::repl::kMoveWalking, 0U, false, 0.0F};
    aoc::worldd::MoveChecker checker(limits, start);
    const auto t0 = std::chrono::steady_clock::now();

    aoc::protocol::repl::NetworkMove m1{};
    m1.time_stamp = 10.0F;
    m1.location = aoc::protocol::repl::quantize_vector(100, 500.0, 0.0, 0.0);
    m1.movement_mode = aoc::protocol::repl::kMoveWalking;
    SANDAL_ASSERT(checker.check(m1, t0).accepted);
    SANDAL_ASSERT(checker.good().x == 500.0 && checker.good().time_stamp == 10.0F);

    aoc::protocol::repl::NetworkMove stale = m1;
    stale.time_stamp = 9.5F;
    stale.location = aoc::protocol::repl::quantize_vector(100, 900.0, 0.0, 0.0);
    SANDAL_ASSERT(checker.is_stale(stale.time_stamp));
    const auto v_stale = checker.check(stale, t0 + std::chrono::seconds(1));
    SANDAL_ASSERT(!v_stale.accepted && v_stale.fault == aoc::worldd::MoveFault::TimeStampNotRising);
    SANDAL_ASSERT(checker.good().x == 500.0);

    aoc::protocol::repl::NetworkMove teleport_hack = m1;
    teleport_hack.time_stamp = 10.5F;
    teleport_hack.location = aoc::protocol::repl::quantize_vector(100, 50000.0, 0.0, 0.0);
    const auto v_fast = checker.check(teleport_hack, t0 + std::chrono::seconds(1));
    SANDAL_ASSERT(!v_fast.accepted && v_fast.fault == aoc::worldd::MoveFault::TooFast);
    SANDAL_ASSERT(checker.good().x == 500.0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "udp_jitter_loss_reorder_seam")) {
        if (test_udp_jitter_loss_reorder_seam() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "rcu_snapshot_generation_fence")) {
        if (test_rcu_snapshot_generation_fence() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
