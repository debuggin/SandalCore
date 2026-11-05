#include "test_world_ability_session_common.h"

#include "capture_packets.h"
#include "protocol/repl/rpc_fields.h"
#include "session/player_session.h"

using namespace aoc::worldd;

static void test_server_stall() {
    std::printf("a server loop that stalls\n");
    Run r;
    r.play(0.5, false);
    r.trigger(kDodge);
    CHECK(r.m.stamina_now() == 50.0f, "(a Dodge: 50)");
    const size_t chains = r.m.chains;
    r.h.step(seconds(15));
    r.absorb();
    CHECK(r.rt().stamina() == 100.0 && r.m.stamina_now() == 100.0f && r.m.abilities.empty() && r.m.effects.empty() && r.m.cooldowns.empty(),
          "one tick 15 s later runs the timers in order (Evasion, cooldown, pause, ten regeneration ticks): the bar is full and every item is gone");
    CHECK(r.m.chains <= chains + 2, "and the client gets what changed as one update, not the ten steps in between: the state is the queue");
    CHECK(!r.m.end_before_removal && r.m.unreadable == 0 && !r.h.s->abilities_failed(), "(nothing failed)");
}

static const Address kA{0x7F000001u, 50510};
static uint8_t g_counter = 1;
static void counting_random(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = g_counter++;
    }
}

struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override { submitted.emplace_back(id, std::move(r)); }
};

struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;

    explicit Rig(EndpointConfig cfg)
        : ep(std::move(cfg), signer, logins, [this](const Address &a, const std::vector<uint8_t> &p) { sent.emplace_back(a, p); }, counting_random, t0) {}

    std::vector<std::vector<uint8_t>> take(const Address &a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto &[to, p] : sent) {
            if (to == a) {
                out.push_back(p);
            } else {
                rest.emplace_back(to, p);
            }
        }
        sent.swap(rest);
        return out;
    }
};

struct Joined {
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;
};

static Joined handshake(Rig &rig, const Address &addr, Clock::time_point now) {
    Joined j;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) {
        return j;
    }
    j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) {
        return j;
    }
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

static void drain(Rig &rig, Joined &j, const Address &addr) {
    for (const auto &p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto &d : r.delivered) {
            j.got.push_back(d);
        }
    }
}

static void exchange(Rig &rig, Joined &j, const Address &addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

static void join_world(Rig &rig, Joined &j, const Address &addr, const svc::Character &rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}

static void absorb_pawn_got(const Joined &j, uint32_t ch, size_t &seen, Mirror &m, Clock::time_point t) {
    for (; seen < j.got.size(); ++seen) {
        if (j.got[seen].channel == ch) {
            m.absorb(j.got[seen], std::chrono::duration<double>(t - t0).count());
        }
    }
}

static void run_endpoint_sprint(Rig &rig, Joined &j, Mirror &m, const PlayerSession &p, Clock::time_point t) {
    const uint32_t ch = p.channel_of(bs::Role::Pawn);
    const emit::FIntrepidNetworkGUID ability = p.bootstrap().setup().guids.ability;
    size_t seen = j.got.size();
    exchange(rig, j, kA, {j.client->actor(ch, trigger_bunch(ability, kSprint, 0), true)}, t);
    absorb_pawn_got(j, ch, seen, m, t);
    const double started = std::chrono::duration<double>(t - t0).count();
    CHECK(m.has_ability(kSprint) && m.has_effect(kSprintEffect), "a Sprint trigger sent through the Endpoint starts the Sprint");
    float ts = 10.0f;
    double x = kX;
    for (int i = 0; i < 215; ++i) {
        t += milliseconds(100);
        ts += 0.1f;
        x += 10.0;
        exchange(rig, j, kA, {j.client->actor(ch, client_move(move_at(ts, x, true)), false)}, t);
        absorb_pawn_got(j, ch, seen, m, t);
    }
    CHECK(m.stamina_now() == 0.0f && m.stamina.size() == 80 && !m.has_ability(kSprint) && !m.has_effect(kSprintEffect),
          "driven by the Endpoint's own ticks the bar empties in 80 steps and the Sprint's items are gone");
    CHECK(m.ends.size() == 1 && m.ends[0].second == 0 && m.ends[0].first - started >= 19.9 && m.ends[0].first - started <= 20.5 && !m.end_before_removal,
          "ClientEndAbility(0) arrives about 20 s after the trigger, after the removal");
    CHECK(rig.ep.session_count() == 1 && !p.abilities_failed() && m.unreadable == 0, "(the session is still there and nothing failed)");
}

static void test_through_the_endpoint() {
    std::printf("through the endpoint\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    Joined j = handshake(rig, kA, t0);
    join_world(rig, j, kA, cyb_record(), t0);
    const PlayerSession *p = rig.ep.player_at(kA);
    CHECK(rig.ep.session_count() == 1 && p != nullptr, "(a player is in the world)");
    if (!p) {
        return;
    }
    Mirror m(p->bootstrap().setup());
    const uint32_t ch = p->channel_of(bs::Role::Pawn);
    size_t seen = j.got.size();
    Clock::time_point t = t0 + milliseconds(100);
    for (int i = 0; i < 30; ++i) {
        exchange(rig, j, kA, {}, t);
        t += milliseconds(100);
    }
    absorb_pawn_got(j, ch, seen, m, t);
    CHECK(m.stamina_now() == 100.0f && p->abilities() != nullptr, "Stamina 100; the runtime exists once the Endpoint has ticked the session");
    run_endpoint_sprint(rig, j, m, *p, t);
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t u(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

static int g_violations = 0;
static void violate(const std::string &msg) {
    if (g_violations++ < 25) {
        std::printf("  [FAIL] fuzz: %s\n", msg.c_str());
    }
}

static void add_hostile_targets(const schema::RpcSchema &rpc, repl::RpcParams &p, size_t n_targets, bool inline_targets, const emit::FIntrepidNetworkGUID &pawn) {
    if (!n_targets) {
        return;
    }
    repl::ArrayValue arr;
    for (size_t i = 0; i < n_targets; ++i) {
        repl::ElementState e;
        if (inline_targets) {
            repl::InlineObject o;
            o.flags = 5;
            o.outer = pawn;
            o.path = "CollisionCylinder";
            o.checksum = 0xb876932f;
            e.values[1] = repl::Value(o);
        } else {
            emit::FIntrepidNetworkGUID g;
            g.ObjectId = 1000 + i;
            g.ServerId = 60;
            e.values[1] = repl::Value(g);
        }
        arr.elements.push_back(std::move(e));
    }
    repl::set_rpc_param(rpc, p, "Targets", repl::Value(std::move(arr)));
}

static void add_hostile_transforms(const schema::RpcSchema &rpc, repl::RpcParams &p, size_t n_transforms) {
    if (!n_transforms) {
        return;
    }
    const schema::RepCommand *cmd = repl::rpc_command(rpc, "TargetTransforms");
    const auto elems = rpc.element_commands.subspan(cmd->elem_begin, cmd->elem_end - cmd->elem_begin);
    repl::ArrayValue arr;
    for (size_t i = 0; i < n_transforms; ++i) {
        repl::ElementState e;
        for (size_t k = 0; k < elems.size(); ++k) {
            auto z = repl::zero_value(elems[k]);
            if (z) {
                e.values[static_cast<uint16_t>(k + 1)] = *z;
            }
        }
        arr.elements.push_back(std::move(e));
    }
    repl::set_rpc_param(rpc, p, "TargetTransforms", repl::Value(std::move(arr)));
}

static repl::RpcParams hostile_params(int64_t guid, int32_t id, Rng &rng, const emit::FIntrepidNetworkGUID &pawn) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (id != 0) {
        repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    }
    repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    const size_t n_targets = rng.u(6);
    const bool inline_targets = rng.u(2) != 0;
    const size_t n_transforms = rng.u(3);
    add_hostile_targets(*rpc, p, n_targets, inline_targets, pawn);
    add_hostile_transforms(*rpc, p, n_transforms);
    return p;
}

static void flip_random_bits(std::vector<uint8_t> &bytes, size_t bits, Rng &rng) {
    const int n = 1 + static_cast<int>(rng.u(4));
    for (int i = 0; i < n; ++i) {
        const size_t b = rng.u(static_cast<uint32_t>(bits));
        bytes[b >> 3] ^= static_cast<uint8_t>(1u << (b & 7));
    }
}

static void replace_byte_run(std::vector<uint8_t> &bytes, Rng &rng) {
    const size_t b = rng.u(static_cast<uint32_t>(bytes.size()));
    const size_t n = 1 + rng.u(4);
    for (size_t i = 0; i < n && b + i < bytes.size(); ++i) {
        bytes[b + i] = static_cast<uint8_t>(rng.u(256));
    }
}

static BunchWriter mutate(const BunchWriter &in, Rng &rng) {
    std::vector<uint8_t> bytes(in.data(), in.data() + in.byte_size());
    size_t bits = in.bit_pos();
    switch (rng.u(6)) {
    case 0:
        flip_random_bits(bytes, bits, rng);
        break;
    case 1:
        bits = 1 + rng.u(static_cast<uint32_t>(bits));
        break;
    case 2: {
        static const uint8_t edge[] = {0x00, 0xff, 0x7f, 0x80};
        bytes[rng.u(static_cast<uint32_t>(bytes.size()))] = edge[rng.u(4)];
        break;
    }
    case 3: {
        const size_t extra = 1 + rng.u(96);
        for (size_t i = 0; i < (extra + 7) / 8; ++i) {
            bytes.push_back(static_cast<uint8_t>(rng.u(256)));
        }
        bits += extra;
        break;
    }
    case 4:
        replace_byte_run(bytes, rng);
        break;
    default: {
        const size_t b = rng.u(static_cast<uint32_t>(bits));
        for (size_t i = 0; i < 16 && b + i < bits; ++i) {
            bytes[(b + i) >> 3] |= static_cast<uint8_t>(1u << ((b + i) & 7));
        }
        break;
    }
    }
    BunchWriter out;
    out.write_bit_range(bytes.data(), 0, bits);
    return out;
}

static void check_settled_invariants(Run &r, const AbilityRuntime &rt, const std::string &w) {
    if (!(std::fabs(r.m.stamina_now() - static_cast<float>(rt.stamina())) < 1e-3)) {
        violate(w + ": settled, the client's Stamina is not the runtime's");
    }
    if (rt.sprinting() != r.m.has_ability(kSprint)) {
        violate(w + ": settled, the runtime's Sprint and the client's differ");
    }
    if (r.m.stats[kMoveSpeedMult] != (rt.sprinting() ? 1.2f : 1.0f)) {
        violate(w + ": settled, MoveSpeedMult is not 1.2 exactly while the runtime sprints");
    }
}

static void check_invariants(Run &r, const char *where, bool settled) {
    const AbilityRuntime *rt = r.h.s->abilities();
    if (!rt) {
        return;
    }
    const std::string w = where;
    if (!(rt->stamina() >= 0.0 && rt->stamina() <= rt->max_stamina())) {
        violate(w + ": the runtime's Stamina is outside [0, max]");
    }
    if (r.h.s->abilities_failed()) {
        violate(w + ": abilities failed");
    }
    if (r.m.unreadable) {
        violate(w + ": the client could not read a chain");
    }
    if (r.m.abilities.size() > 3 || r.m.effects.size() > 5 || r.m.cooldowns.size() > 1) {
        violate(w + ": the client's item counts are out of bounds");
    }
    for (const auto &s : r.m.stamina) {
        if (s.second < 0.0f || s.second > 100.0f) {
            violate(w + ": the client saw a Stamina outside [0, 100]");
            break;
        }
    }
    if (settled && !r.h.hold) {
        check_settled_invariants(r, *rt, w);
    }
    if (r.h.s->ability_ends_pending() > 64) {
        violate(w + ": more than 64 ClientEndAbility calls pending");
    }
}

static int32_t random_trigger_id(Run &r, Rng &rng, int32_t last_id) {
    switch (rng.u(8)) {
    case 0:
        return last_id;
    case 1:
        return static_cast<int32_t>(rng.next());
    case 2:
        return INT32_MIN;
    case 3:
        return INT32_MAX;
    case 4:
        return -1;
    case 5:
        return 0;
    default:
        return r.next_id++;
    }
}

static void random_trigger_op(Run &r, Rng &rng, int32_t &last_id, int &refused_unanswered) {
    const int64_t guids[] = {kSprint, kSprint, kDodge, kRest, kScepterCombo, kQuickWit, 12345, 0, -1, INT64_MIN, INT64_MAX};
    const int64_t guid = guids[rng.u(11)];
    const int32_t id = random_trigger_id(r, rng, last_id);
    last_id = id;
    const size_t handled_before = r.h.s->ability_triggers();
    const size_t refused_before = r.rt().refused();
    const size_t ends_before = r.m.ends.size();
    r.send_trigger(guid, id);
    r.h.step(milliseconds(20));
    r.absorb();
    const bool handled = r.h.s->ability_triggers() > handled_before;
    if (handled && r.rt().refused() > refused_before && !r.h.hold) {
        if (r.m.ends.size() == ends_before || r.m.ends.back().second != id) {
            ++refused_unanswered;
        }
    }
}

static void random_other_op(Run &r, Rng &rng, uint32_t op, bool with_holds) {
    if (op < 10) {
        const int32_t id = rng.u(3) == 0 ? static_cast<int32_t>(rng.next()) : (r.next_id > 0 ? static_cast<int32_t>(rng.u(r.next_id)) : 0);
        r.cancel(id);
    } else if (op < 12) {
        r.h.step(milliseconds(1000 + rng.u(30000)));
        r.absorb();
    } else if (op < 13 && with_holds) {
        r.h.hold = true;
        r.play(0.3 + rng.u(30) / 10.0, rng.u(2) == 0);
        r.h.release();
        r.absorb();
    } else {
        r.play(0.1 + rng.u(25) / 10.0, rng.u(3) != 0);
    }
}

static void scenario_random_ops(uint64_t seed, int iterations, bool with_holds) {
    Run r;
    Rng rng(seed);
    r.play(0.5, false);
    int32_t last_id = 0;
    int refused_unanswered = 0;
    for (int it = 0; it < iterations; ++it) {
        const uint32_t op = rng.u(20);
        if (op < 7) {
            random_trigger_op(r, rng, last_id, refused_unanswered);
        } else {
            random_other_op(r, rng, op, with_holds);
        }
        for (int i = 0; i < 3; ++i) {
            r.h.step(milliseconds(20));
            r.absorb();
        }
        check_invariants(r, "random operations", it % 4 == 0);
        if (g_violations > 20) {
            break;
        }
    }
    r.play(2.0, false);
    check_invariants(r, "random operations, at the end", true);
    if (refused_unanswered) {
        violate("a refused trigger was not answered (seed " + std::to_string(seed) + ")");
    }
}

static BunchWriter random_good_bunch(Run &r, Rng &rng, const emit::FIntrepidNetworkGUID &pawn) {
    const int64_t guids[] = {kSprint, kDodge, kRest, kScepterCombo};
    switch (rng.u(5)) {
    case 0:
        return trigger_bunch(r.h.ability_guid(), guids[rng.u(4)], r.next_id++);
    case 1:
        return cancel_bunch(r.h.ability_guid(), static_cast<int32_t>(rng.u(8)));
    case 2:
        return ability_rpc(r.h.ability_guid(), "ServerTriggerAbility", hostile_params(guids[rng.u(4)], r.next_id++, rng, pawn));
    case 3:
        return trigger_bunch(r.h.ability_guid(), kSprint, r.next_id++);
    default:
        return trigger_bunch(r.h.ability_guid(), kDodge, r.next_id++);
    }
}

static void scenario_bit_fuzz(uint64_t seed, int iterations) {
    Run r;
    Rng rng(seed);
    r.play(0.5, false);
    const auto pawn = r.h.s->bootstrap().setup().guids.pawn;
    for (int it = 0; it < iterations; ++it) {
        const BunchWriter good = random_good_bunch(r, rng, pawn);
        if (good.bit_pos() == 0) {
            continue;
        }
        const BunchWriter bad = rng.u(8) == 0 ? good : mutate(good, rng);
        r.h.client_sends({r.h.client.actor(r.h.pawn_ch, bad, rng.u(4) != 0)});
        r.h.flush();
        r.absorb();
        for (int i = 0; i < 2; ++i) {
            r.h.step(milliseconds(20 + rng.u(400)));
            r.absorb();
        }
        if (rng.u(5) == 0) {
            r.play(0.2, rng.u(2) == 0);
        }
        check_invariants(r, "bit mutations", false);
        if (g_violations > 20) {
            break;
        }
    }
    r.play(2.0, false);
    check_invariants(r, "bit mutations, at the end", false);
}

static void test_hostile_client() {
    std::printf("a seeded hostile client\n");
    const spdlog::level::level_enum level = spdlog::default_logger()->level();
    spdlog::default_logger()->set_level(spdlog::level::off);
    g_violations = 0;
    for (uint64_t seed = 1; seed <= 3; ++seed) {
        scenario_random_ops(seed, 150, seed % 2 == 0);
    }
    const int random_violations = g_violations;
    for (uint64_t seed = 101; seed <= 103; ++seed) {
        scenario_bit_fuzz(seed, 450);
    }
    spdlog::default_logger()->set_level(level);
    CHECK(random_violations == 0, "random operations (hostile ids, guids, stalls, held acknowledgements): every invariant holds, 3 seeds of 150 operations");
    CHECK(g_violations == random_violations, "triggers and cancels damaged bit by bit (and hostile Targets): every invariant holds, 3 seeds of 450 mutated bunches");
}

int main() {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_server_stall();
    test_through_the_endpoint();
    test_hostile_client();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
