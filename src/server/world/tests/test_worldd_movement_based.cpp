// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_movement_common.h"

using namespace aoc::worldd;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;
using std::chrono::milliseconds;

namespace move_ack_checks {

inline std::optional<float> ack_stamp(const fake::Delivered& d) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!d.exports.empty() || d.reliable || d.content.bit_pos() != 68 || !blocks || blocks->size() != 1) {
        return std::nullopt;
    }
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    size_t pos_bits = (*blocks)[0].payload_pos;
    const auto h = repl::read_field_header(pawn, d.content.data(), pos_bits + (*blocks)[0].payload_bits, pos_bits);
    if (!h || std::string(pawn.field(h->field)->name) != "ClientMoveResponsePacked") return std::nullopt;
    aoc::protocol::wire::PacketReader r(d.content.data(), d.content.byte_size(), pos_bits + h->payload_bits);
    r.set_pos(pos_bits);
    if (r.read_bits(10) != 645) return std::nullopt;
    return std::bit_cast<float>(static_cast<uint32_t>(r.read_bits(32)));
}

inline std::vector<float> frame(
    Harness& h,
    const std::optional<repl::NetworkMove>& move,
    milliseconds ms = milliseconds(33)) {
    if (move) h.client_sends({h.client.actor(h.pawn_ch, client_move(*move), false)});
    const size_t from = h.got.size();
    h.step(ms);
    std::vector<float> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pawn_ch) continue;
        if (const auto ts = ack_stamp(h.got[i])) out.push_back(*ts);
    }
    return out;
}

}  // namespace move_ack_checks

static void check_move_acks_every_frame() {
    using namespace move_ack_checks;
    std::printf(
        "move answers: a move every frame (36 a second) is answered every fourth frame, with the latest move's TimeStamp\n");
    Harness h;
    CHECK(h.fx.world->move_ack_gap() == Clock::duration::zero(),
          "(the world answers every move at once unless a pace is set: worldd sets 100 ms)");
    h.fx.world->set_move_ack_gap(milliseconds(100));
    float ts = 1.0f;
    double x = kX;
    std::vector<float> sent, answered;
    for (int i = 0; i < 40; ++i) {
        ts += 0.033f;
        x += 13.2;
        sent.push_back(ts);
        for (const float a : frame(h, move_at(ts, x, kY, kZ))) answered.push_back(a);
    }
    CHECK(answered.size() == 10,
          "40 moves 33 ms apart: 10 answers (the first at once, then one more than 100 ms after the last: every fourth frame)");
    bool latest = answered.size() == 10;
    for (size_t k = 0; latest && k < answered.size(); ++k) latest = answered[k] == sent[4 * k];
    CHECK(latest,
          "each answer carries the TimeStamp of the move that arrived in its own frame: the 1st, 5th, 9th ... of the 40");
    CHECK(h.s->moves_acknowledged() == 10 && h.s->move_acks_held_back() == 30, "counted: 10 answered, 30 not");
    CHECK(h.s->moves_refused() == 0 && h.s->moves_stale() == 0 && h.s->corrections_sent() == 0,
          "no move is refused, stale or corrected for it");
    const GoodPosition& g = h.s->movement().good();
    CHECK(near(g.x, x) && g.time_stamp == ts,
          "the last good position is the last move's: every move is judged, answered or not");
}

static void check_move_acks_walking_and_slow_paces() {
    using namespace move_ack_checks;
    std::printf("move answers: a move every second frame (15 a second) is answered every second move, as a walking client was\n");
    Harness w;
    w.fx.world->set_move_ack_gap(milliseconds(100));
    float ts = 1.0f;
    double x = kX;
    std::vector<float> sent, answered;
    for (int i = 0; i < 40; ++i) {
        std::optional<repl::NetworkMove> move;
        if (i % 2 == 0) {
            ts += 0.066f;
            x += 26.4;
            sent.push_back(ts);
            move = move_at(ts, x, kY, kZ);
        }
        for (const float a : frame(w, move)) answered.push_back(a);
    }
    bool every_second = answered.size() == 10;
    for (size_t k = 0; every_second && k < answered.size(); ++k) every_second = answered[k] == sent[2 * k];
    CHECK(sent.size() == 20 && every_second,
          "20 moves 66 ms apart: 10 answers, for the 1st, 3rd, 5th ... (the original server's way)");
    CHECK(w.s->moves_acknowledged() == 10 && w.s->move_acks_held_back() == 10 && w.s->moves_refused() == 0,
          "counted, none refused");

    std::printf("move answers: a client that moves less than ten times a second is answered for every move\n");
    Harness slow;
    slow.fx.world->set_move_ack_gap(milliseconds(100));
    ts = 1.0f;
    x = kX;
    sent.clear();
    answered.clear();
    for (int i = 0; i < 30; ++i) {
        std::optional<repl::NetworkMove> move;
        if (i % 4 == 0) {
            ts += 0.132f;
            x += 52.8;
            sent.push_back(ts);
            move = move_at(ts, x, kY, kZ);
        }
        for (const float a : frame(slow, move)) answered.push_back(a);
    }
    CHECK(sent.size() == 8 && answered == sent, "8 moves 132 ms apart: 8 answers, each its own move's");
}

static void test_move_acks_at_the_original_pace() {
    check_move_acks_every_frame();
    check_move_acks_walking_and_slow_paces();
}

static void test_move_ack_held_back_is_dropped_not_carried() {
    using namespace move_ack_checks;
    std::printf(
        "move answers: an answer the gap holds back is dropped; it is not sent later when the client stops moving\n");
    Harness h;
    h.fx.world->set_move_ack_gap(milliseconds(100));
    const auto first = frame(h, move_at(1.033f, kX + 13.2, kY, kZ));
    CHECK(first.size() == 1 && first[0] == 1.033f, "the first move is answered at once (at its frame's tick)");
    const auto second = frame(h, move_at(1.066f, kX + 26.4, kY, kZ));
    CHECK(second.empty(), "the next frame's move is 33 ms after the answer: held back");
    size_t later = 0;
    for (int i = 0; i < 8; ++i) later += frame(h, std::nullopt).size();
    CHECK(later == 0 && h.s->moves_acknowledged() == 1 && h.s->move_acks_held_back() == 1,
          "and it never goes out: the client keeps the move saved until a later answer frees it");
    const auto third = frame(h, move_at(1.3f, kX + 39.6, kY, kZ));
    CHECK(third.size() == 1 && third[0] == 1.3f,
          "the next move, long after the last answer, is answered with its own TimeStamp (which frees the held one too)");
}

static void test_move_ack_yields_to_a_correction() {
    using namespace move_ack_checks;
    std::printf("move answers: a refused move is corrected at once, and the answer held for the move before it is dropped\n");
    Harness h;
    h.fx.world->set_move_ack_gap(milliseconds(100));
    frame(h, move_at(1.033f, kX + 13.2, kY, kZ));
    h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(1.066f, kX + 26.4, kY, kZ)), false)});
    const GoodPosition good = h.s->movement().good();
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(1.083f, kX + 5026.4, kY, kZ)), false)});
    h.flush();
    std::vector<const fake::Delivered*> replies;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == h.pawn_ch) replies.push_back(&h.got[i]);
    }
    CHECK(replies.size() == 1 && correction_is(*replies[0], 1.083f, good.x, good.y, good.z),
          "the refusal's ClientAdjustPosition goes out with the datagram, not at the next tick");
    const auto after = frame(h, std::nullopt);
    CHECK(after.empty() && h.s->moves_acknowledged() == 1 && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1,
          "and the answer held for the good move before it is not sent (the correction wins, as the original server's pending adjustment)");
}

namespace based_move_checks {

inline bool add_mount(Harness& h, uint64_t id, double x, double y, double z, double yaw) {
    world::WorldActor mount;
    mount.id = id;
    mount.kind = world::ActorKind::Mount;
    mount.actor.guid.ObjectId = id;
    mount.x = x;
    mount.y = y;
    mount.z = z;
    mount.yaw_degrees = yaw;
    mount.npc.collision_cylinder = id + 4;
    return h.fx.world->registry().add(mount);
}

inline repl::InlineObject component_of(uint64_t actor_id, const char* path) {
    repl::InlineObject o;
    o.flags = 5;
    o.outer.ObjectId = actor_id;
    o.outer.ServerId = kServerId;
    o.outer.Randomizer = kRandomizer;
    o.path = path;
    o.checksum = 0xb876932f;
    return o;
}

inline repl::NetworkMove move_on(float ts, const repl::InlineObject& base, const repl::Vec3d& offset) {
    repl::NetworkMove m = move_at(ts, offset.x, offset.y, offset.z);
    m.base_inline = base;
    return m;
}

inline repl::Vec3d client_relative(const repl::Vec3d& world, const repl::Vec3d& base, double base_yaw_degrees) {
    const double half = base_yaw_degrees * 3.14159265358979323846 / 360.0;
    const double qx = 0, qy = 0, qz = -std::sin(half), qw = std::cos(half);
    const double vx = world.x - base.x, vy = world.y - base.y, vz = world.z - base.z;
    const double tx = 2 * (qy * vz - qz * vy), ty = 2 * (qz * vx - qx * vz), tz = 2 * (qx * vy - qy * vx);
    return repl::Vec3d{
        vx + qw * tx + (qy * tz - qz * ty),
        vy + qw * ty + (qz * tx - qx * tz),
        vz + qw * tz + (qx * ty - qy * tx)};
}

inline bool close(const repl::Vec3d& a, const repl::Vec3d& b, double tolerance = 0.05) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
           std::fabs(a.z - b.z) <= tolerance;
}

inline repl::Vec3d place_of(const GoodPosition& g) { return repl::Vec3d{g.x, g.y, g.z}; }

inline bool answered_with_ack(const std::vector<const fake::Delivered*>& r, float ts) {
    return r.size() == 1 && is_ack(*r[0], ts);
}

}  // namespace based_move_checks

static void test_a_move_on_a_mount_is_put_where_its_yaw_says() {
    using namespace based_move_checks;
    std::printf("moves on a base worldd can place: the offset is turned by the base's yaw\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    constexpr uint64_t kHorse = 0x4000;
    const repl::Vec3d base{kX + 20.0, kY, kZ};
    CHECK(add_mount(h, kHorse, base.x, base.y, base.z, 0.0), "(the horse is in the registry)");

    const repl::Vec3d east{10.0, 0.0, 0.0};
    CHECK(close(client_relative(east, repl::Vec3d{0, 0, 0}, 0), repl::Vec3d{10, 0, 0}) &&
              close(client_relative(east, repl::Vec3d{0, 0, 0}, 90), repl::Vec3d{0, -10, 0}) &&
              close(client_relative(east, repl::Vec3d{0, 0, 0}, 180), repl::Vec3d{-10, 0, 0}) &&
              close(client_relative(east, repl::Vec3d{0, 0, 0}, -90), repl::Vec3d{0, 10, 0}),
          "(the conversion the checks use agrees with the four hand-worked cases)");

    struct Case {
        double yaw;
        repl::Vec3d beside;
    };
    const Case cases[] = {
        {0, {10, 0, 0}},
        {90, {10, 0, 0}},
        {180, {10, 0, 0}},
        {-90, {10, 0, 0}},
        {30, {25, 15, 4}},
        {-120, {-8, 20, -2}},
        {200, {30, -10, 0}}};
    float ts = 1.0f;
    bool placed = true, acknowledged = true, turn_agrees = true;
    for (const Case& c : cases) {
        ts += 0.1f;
        h.fx.world->registry().move(kHorse, base.x, base.y, base.z, c.yaw);
        const repl::Vec3d world{base.x + c.beside.x, base.y + c.beside.y, base.z + c.beside.z};
        const repl::Vec3d offset = client_relative(world, base, c.yaw);
        const auto r = h.send_move(move_on(ts, component_of(kHorse, "CollisionCylinder"), offset));
        const repl::Vec3d at = place_of(h.s->movement().good());
        if (!close(at, world)) {
            placed = false;
            std::printf(
                "    (yaw %.0f: the player is at (%.2f, %.2f, %.2f), expected (%.2f, %.2f, %.2f); the offset sent was (%.2f, %.2f, %.2f))\n",
                c.yaw,
                at.x,
                at.y,
                at.z,
                world.x,
                world.y,
                world.z,
                offset.x,
                offset.y,
                offset.z);
        }
        acknowledged = acknowledged && answered_with_ack(r, ts);
        turn_agrees = turn_agrees && close(PlayerSession::turn_by_yaw(c.yaw, offset), c.beside);
    }
    CHECK(placed,
          "a move on the horse's cylinder is applied at the horse's place plus the offset turned by its yaw, for seven yaws (the horse stood 20 cm away; the player is put where he is)");
    CHECK(acknowledged && h.s->moves_refused() == 0 && h.s->corrections_sent() == 0 && h.s->moves_based() == 7 &&
              h.s->moves_based_unplaced() == 0,
          "and each is acknowledged, none refused or corrected, counted as a based move that was placed");
    CHECK(turn_agrees,
          "(the acceleration and the offset are turned by the same function: it brings the base's frame back to the world's for each yaw)");

    repl::NetworkMove plain = move_at(ts + 0.1f, 0, 0, 0);
    plain.location = repl::quantize_vector(100, 0.0, -10.0, 0.0);
    h.fx.world->registry().move(kHorse, base.x, base.y, base.z, 90.0);
    plain.base = emit::FIntrepidNetworkGUID{};
    plain.base->ObjectId = kHorse + 4;
    plain.base->ServerId = kServerId;
    plain.base->Randomizer = kRandomizer;
    const auto by_id = h.send_move(plain);
    CHECK(answered_with_ack(by_id, ts + 0.1f) &&
              close(place_of(h.s->movement().good()), repl::Vec3d{base.x + 10.0, base.y, base.z}) &&
              h.s->moves_based_unplaced() == 0,
          "a plain reference to the horse's cylinder is placed too (offset (0, -10, 0) at yaw 90 degrees: 10 cm east)");
}

static void test_moves_on_a_base_worldd_cannot_place_are_followed() {
    using namespace based_move_checks;
    std::printf("moves on a base worldd cannot place: followed from the move before, judged like any move\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    constexpr uint64_t kHorse = 0x4000;
    CHECK(add_mount(h, kHorse, kX + 20.0, kY, kZ, 90.0), "(the horse is in the registry)");
    const repl::Vec3d start = place_of(h.s->movement().good());

    const auto first = h.send_move(move_on(1.1f, component_of(kHorse, "CharacterMesh0"), repl::Vec3d{3.0, 4.0, 5.0}));
    CHECK(answered_with_ack(first, 1.1f) && close(place_of(h.s->movement().good()), start) &&
              h.s->moves_refused() == 0 && h.s->corrections_sent() == 0 && h.s->moves_based() == 1 &&
              h.s->moves_based_unplaced() == 1 && h.s->moves_based_held() == 1,
          "the first move on a component worldd cannot place is acknowledged and the player stays where he was (a relative location is no place to move to)");

    float ts = 1.1f;
    bool all_acked = true;
    for (int i = 1; i <= 5; ++i) {
        ts += 0.05f;
        all_acked = answered_with_ack(
                        h.send_move(move_on(
                            ts, component_of(kHorse, "CharacterMesh0"), repl::Vec3d{3.0 + 10.0 * i, 4.0, 5.0})),
                        ts) &&
                    all_acked;
    }
    CHECK(all_acked && close(place_of(h.s->movement().good()), repl::Vec3d{start.x + 50.0, start.y, start.z}) &&
              h.s->moves_refused() == 0 && h.s->moves_based() == 6 && h.s->moves_based_held() == 1,
          "the five moves after it carry the player 50 cm along, as the offset changed (the base is taken to stand still), all acknowledged");

    const repl::Vec3d followed = place_of(h.s->movement().good());
    ts += 0.05f;
    const auto other = h.send_move(move_on(ts, component_of(0x7770, "CollisionCylinder"), repl::Vec3d{-900.0, 40.0, 7.0}));
    CHECK(answered_with_ack(other, ts) && close(place_of(h.s->movement().good()), followed) &&
              h.s->moves_based_held() == 2,
          "a move on another base (an actor worldd does not hold) stays in place again, wherever its offset is");

    ts += 0.05f;
    const auto off = h.send_move(move_at(ts, followed.x + 10.0, followed.y, followed.z));
    CHECK(answered_with_ack(off, ts) &&
              close(place_of(h.s->movement().good()), repl::Vec3d{followed.x + 10.0, followed.y, followed.z}) &&
              h.s->corrections_sent() == 0 && h.s->moves_refused() == 0,
          "the first move off the base (an absolute place 10 cm on) is acknowledged: the followed place and the player's agree, no jump and no correction");
}

static void test_a_jump_on_a_followed_base_is_refused_like_any_jump() {
    using namespace based_move_checks;
    std::printf("a jump on a base worldd cannot place is refused like any other jump\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const repl::InlineObject prop = component_of(0x6600, "StaticMeshComponent0");
    float ts = 1.0f;
    for (int i = 0; i < 3; ++i) {
        ts += 0.05f;
        h.send_move(move_on(ts, prop, repl::Vec3d{100.0 + 10.0 * i, 0, 0}));
    }
    const GoodPosition before = h.s->movement().good();
    ts += 0.05f;
    const auto jump = h.send_move(move_on(ts, prop, repl::Vec3d{100.0 + 8000.0, 0, 0}));
    CHECK(jump.size() == 1 && correction_is(*jump[0], ts, before.x, before.y, before.z) &&
              h.s->moves_refused() == 1 && h.s->movement().good() == before,
          "an offset that changes by 80 m in 50 ms moves the player too fast: refused, corrected to the last good position, the player stays");
    ts += 0.05f;
    const size_t held = h.s->moves_based_held();
    const auto next = h.send_move(move_on(ts, prop, repl::Vec3d{140.0, 0, 0}));
    CHECK(answered_with_ack(next, ts) && h.s->moves_based_held() == held + 1 &&
              h.s->movement().good().x == before.x + 0.0 && h.s->movement().good().time_stamp == ts,
          "and the next move on that base is a first one again: acknowledged, in place");
}

static void check_based_held_cap_single() {
    using namespace based_move_checks;
    constexpr size_t kCap = PlayerSession::kBasedHeldCap;
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const GoodPosition start = h.s->movement().good();
    float ts = 1.0f;
    size_t acked = 0, corrected = 0;
    for (size_t i = 0; i <= kCap; ++i) {
        ts += 0.02f;
        for (const auto* d :
             h.send_move(move_on(ts, component_of(0x9000 + 2 * i, "Hull"), repl::Vec3d{300.0 * i, 0, 0}))) {
            if (is_ack(*d, ts)) ++acked;
            else if (correction_is(*d, ts, start.x, start.y, start.z)) ++corrected;
        }
    }
    CHECK(acked == kCap && corrected == 1 && h.s->moves_based_cut() == 1 &&
              close(place_of(h.s->movement().good()), place_of(start)) && h.s->moves_refused() == 0,
          "the first 120 are acknowledged and stay in place; the 121st is answered with a correction to the last good position (it is not a refusal)");
    ts += 0.02f;
    const auto after = h.send_move(move_on(ts, component_of(0x9900, "Hull"), repl::Vec3d{0, 0, 0}));
    CHECK(answered_with_ack(after, ts), "and the run starts again after it");
}

static void check_based_held_cap_reset_and_datagram() {
    using namespace based_move_checks;
    constexpr size_t kCap = PlayerSession::kBasedHeldCap;
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        float ts = 1.0f;
        size_t fixes = 0;
        for (size_t i = 0; i < 100; ++i) {
            ts += 0.02f;
            for (const auto* d :
                 h.send_move(move_on(ts, component_of(0x9000 + 2 * i, "Hull"), repl::Vec3d{300.0 * i, 0, 0}))) {
                fixes += as_correction(*d) ? 1 : 0;
            }
        }
        ts += 0.02f;
        const float plain_ts = ts;
        const auto plain = h.send_move(move_at(plain_ts, kX, kY, kZ));
        CHECK(answered_with_ack(plain, plain_ts), "(the ordinary move between the runs is acknowledged)");
        for (size_t i = 0; i < 100; ++i) {
            ts += 0.02f;
            for (const auto* d :
                 h.send_move(move_on(ts, component_of(0xa000 + 2 * i, "Hull"), repl::Vec3d{300.0 * i, 0, 0}))) {
                fixes += as_correction(*d) ? 1 : 0;
            }
        }
        CHECK(fixes == 0 && h.s->moves_based_cut() == 0 && h.s->moves_based_held() == 200,
              "100 moves on new bases, one ordinary move, 100 more: no correction (the count starts again at a move whose place the server knows)");
    }
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const GoodPosition start = h.s->movement().good();
        float ts = 1.0f;
        for (size_t i = 0; i < kCap; ++i) {
            ts += 0.02f;
            h.send_move(move_on(ts, component_of(0xb000 + 2 * i, "Hull"), repl::Vec3d{0, 0, 0}));
        }
        const float cut_ts = ts + 0.02f, next_ts = ts + 0.04f;
        size_t fixes = 0;
        bool next_acked = false;
        for (const auto* d : h.send_move(client_moves(
                 {move_on(cut_ts, component_of(0xc000, "Hull"), repl::Vec3d{0, 0, 0}),
                  move_on(next_ts, component_of(0xc002, "Hull"), repl::Vec3d{0, 0, 0})}))) {
            if (correction_is(*d, cut_ts, start.x, start.y, start.z)) ++fixes;
            else if (is_ack(*d, next_ts)) next_acked = true;
        }
        CHECK(fixes == 1 && next_acked && h.s->moves_based_cut() == 1,
              "a datagram with the 121st move and another: the move after it is acknowledged, and the correction goes out all the same (a client cannot cancel it by packing a move behind it)");
    }
}

static void test_a_client_naming_a_new_base_with_every_move_is_put_back() {
    std::printf(
        "a client that names a new base with every move is put back after %zu of them\n",
        PlayerSession::kBasedHeldCap);
    check_based_held_cap_single();
    check_based_held_cap_reset_and_datagram();
}

static void test_a_packet_that_repeats_a_move_on_a_base_counts_it_once() {
    using namespace based_move_checks;
    std::printf("a pending move the game repeats is not judged or counted twice\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const repl::InlineObject prop = component_of(0x6600, "StaticMeshComponent0");
    const repl::NetworkMove a = move_on(1.05f, prop, repl::Vec3d{100.0, 0, 0});
    const repl::NetworkMove b = move_on(1.10f, prop, repl::Vec3d{110.0, 0, 0});
    const repl::NetworkMove c = move_on(1.15f, prop, repl::Vec3d{120.0, 0, 0});
    h.send_move(a);
    repl::MovePacked packet;
    packet.pending = a;
    packet.new_move = b;
    const bool first_acked = answered_with_ack(h.send_move(client_move(packet)), 1.10f);
    packet.pending = b;
    packet.new_move = c;
    const bool second_acked = answered_with_ack(h.send_move(client_move(packet)), 1.15f);
    CHECK(first_acked && second_acked, "each packet is acknowledged, with its new move's TimeStamp");
    CHECK(h.s->moves_based() == 3 && h.s->moves_based_held() == 1 && h.s->moves_stale() == 0,
          "three moves on one base, each sent twice (as the new move and then as the pending move): counted three times, the first held, the others followed");
    CHECK(close(place_of(h.s->movement().good()), repl::Vec3d{kX + 20.0, kY, kZ}),
          "and the player went on by the two changes of the offset (10 cm each)");
    const auto again = h.send_move(client_move(packet));
    CHECK(again.empty() && h.s->moves_based() == 3 && h.s->moves_stale() == 1 &&
              close(place_of(h.s->movement().good()), repl::Vec3d{kX + 20.0, kY, kZ}),
          "a datagram that arrives twice is dropped as stale: nothing counted, nothing moved, whatever offset its moves carry");
}

static void test_moves_that_do_not_read_are_counted() {
    using namespace based_move_checks;
    std::printf("moves that do not read, and what the position line says\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    CHECK(h.s->based_move_summary().empty(), "(a session with no such moves says nothing about them)");
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter params, field, bunch;
    params.write_bit(0);
    CHECK(f && repl::write_field(cls, f->index, params, field) != 0, "(a zeroed ServerMovePacked is built)");
    repl::BlockHeader header;
    repl::write_content_block(bunch, header, field, false);
    h.send_move(bunch);
    h.send_move(bunch);
    CHECK(h.s->moves_unreadable() == 2 && h.s->moves_refused() == 0,
          "a move that does not read is counted (the log says the first and every 100th)");
    const repl::InlineObject prop = component_of(0x6600, "StaticMeshComponent0\nforged line");
    h.send_move(move_on(1.05f, prop, repl::Vec3d{100.0, 0, 0}));
    h.send_move(move_on(1.10f, prop, repl::Vec3d{110.0, 0, 0}));
    const std::string summary = h.s->based_move_summary();
    if (summary != ", 2 unreadable, 2 on a movement base (2 not placed)") {
        std::printf("    (the summary reads \"%s\")\n", summary.c_str());
    }
    CHECK(summary == ", 2 unreadable, 2 on a movement base (2 not placed)",
          "the position line gets the counts after the moves dropped as stale");
}

static void test_the_log_of_a_base_cannot_be_forged_by_its_name() {
    using namespace based_move_checks;
    std::printf("a base's name from the client cannot start a log line of its own\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    g_log.str("");
    h.send_move(move_on(1.05f, component_of(0x6600, "Mesh\n[info] [session] forged: all is well"), repl::Vec3d{1, 2, 3}));
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("a move on a movement base (") != std::string::npos &&
              log.find("\n[info] [session] forged") == std::string::npos &&
              log.find("Mesh?[info]") != std::string::npos,
          "the line names the base with the newline made harmless");
}

void run_movement_based_tests() {
    test_move_acks_at_the_original_pace();
    test_move_ack_held_back_is_dropped_not_carried();
    test_move_ack_yields_to_a_correction();
    test_a_move_on_a_mount_is_put_where_its_yaw_says();
    test_moves_on_a_base_worldd_cannot_place_are_followed();
    test_a_jump_on_a_followed_base_is_refused_like_any_jump();
    test_a_client_naming_a_new_base_with_every_move_is_put_back();
    test_a_packet_that_repeats_a_move_on_a_base_counts_it_once();
    test_moves_that_do_not_read_are_counted();
    test_the_log_of_a_base_cannot_be_forged_by_its_name();
}
