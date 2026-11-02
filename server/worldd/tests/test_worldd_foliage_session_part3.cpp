// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_foliage_session_helpers.h"

namespace {

static void count_client_components(const ClientFoliage &client, size_t &components, size_t &uninitialized) {
    for (const auto &[name, cell] : client.lookup) {
        for (const auto &[comp, init] : cell.components) {
            ++components;
            uninitialized += init ? 0 : 1;
        }
    }
}

static void check_delivery_spots_and_far(FRig &fr, Player &p, ClientFoliage &client, const Spot (&spots)[3],
                                         const std::optional<uint32_t> (&rows)[3]) {
    const foliage::FoliageMap &map = fr.f().map();
    char msg[240];
    for (int i = 0; i < 3; ++i) {
        const size_t from = p.got.size();
        p.s->gm_teleport(p.conn, spots[i].x, spots[i].y, spots[i].z, fr.rig.t, "the test: the user's spot");
        fr.rig.run(seconds(3));
        client.absorb(p, from);
        std::snprintf(msg, sizeof msg,
                      "after a teleport to the %s (%.0f, %.0f, %.0f) its cell's actor is on the client and the "
                      "instance's component is initialized",
                      spots[i].what, spots[i].x, spots[i].y, spots[i].z);
        CHECK(client_resolves(client, map, *rows[i]), msg);
    }
    const size_t far_from = p.got.size();
    p.s->gm_teleport(p.conn, spots[0].x + 600000.0, spots[0].y, spots[0].z, fr.rig.t, "the test: far away");
    fr.rig.run(seconds(4));
    client.absorb(p, far_from);
    CHECK(client_resolves(client, map, *rows[0]) && client_resolves(client, map, *rows[1]) &&
              client_resolves(client, map, *rows[2]),
          "6 km away the client still has the three cells (a Dormancy close keeps the actor)");
    const size_t back_from = p.got.size();
    p.s->gm_teleport(p.conn, -777947.0, 617946.0, 15952.05, fr.rig.t, "the test: back at the login spot");
    fr.rig.run(seconds(4));
    client.absorb(p, back_from);
    CHECK(client_resolves(client, map, *rows[0]) && client_resolves(client, map, *rows[2]),
          "back at the login spot the Oak and the Stone resolve");
    fr.rig.run(seconds(1));
    client.absorb(p, p.got.size());
    std::snprintf(
        msg, sizeof msg,
        "all %zu opens of state actors were closed with Dormancy (%zu); none with Destroyed (%zu) or Relevancy (%zu)",
        client.opens.size(), client.closes_dormancy, client.closes_destroyed, client.closes_relevancy);
    CHECK(client.closes_destroyed == 0 && client.closes_relevancy == 0 && client.closes_other == 0 &&
              client.closes_dormancy == client.opens.size() && client.updates == 0,
          msg);
}

static void test_delivery() {
    std::printf("every cell's state actor reaches the player, and the client keeps it (the login spot, then the user's "
                "three spots)\n");
    FRig fr;
    if (!fr.open()) return;
    const foliage::FoliageMap &map = fr.f().map();
    const Spot spots[] = {{"Oak", -775619, 617693, 16200, "Tree_Oak_Large_2"},
                          {"Daffodil", -813562, 616567, 7960, "Plant_Daffodil_1"},
                          {"Stone", -786101, 621592, 15690, "Stone_Basalt_Medium_1"}};
    std::optional<uint32_t> rows[3];
    bool found = true;
    for (int i = 0; i < 3; ++i) {
        double d = 0;
        rows[i] = nearest_row(map, spots[i], &d);
        found = found && rows[i] && d < 200.0;
    }
    CHECK(found, "the instance nearest each of the user's three spots (within 2 m) is the Oak, the Daffodil and the "
                 "Stone he named");
    if (!found) return;
    Player &p = fr.rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -777947.0, 617946.0, 15952.05));
    play(p);
    ClientFoliage client;
    client.absorb(p, 0);
    char msg[240];
    std::snprintf(msg, sizeof msg,
                  "at the login spot (-777947, 617946) the client was sent %zu state actors, with %zu components, and "
                  "closed %zu with Dormancy",
                  client.opens.size(), client.object_names.size(), client.closes_dormancy);
    CHECK(client.opens.size() >= 3 && client.closes_dormancy == client.opens.size(), msg);
    CHECK(client_resolves(client, map, *rows[0]) && client_resolves(client, map, *rows[2]),
          "the Oak and the Stone, 23 m and 89 m from the login spot, resolve: their cell's actor and the instance's "
          "component (initialized)");
    const bool daffodil_at_login = client_resolves(client, map, *rows[1]);
    std::printf("    (the Daffodil's cell, 299 m from the login spot, was %s at login)\n",
                daffodil_at_login ? "sent" : "not sent");
    size_t uninitialized = 0, components = 0;
    count_client_components(client, components, uninitialized);
    CHECK(components > 0 && uninitialized == 0, "every component the client holds is initialized");
    check_delivery_spots_and_far(fr, p, client, spots, rows);
}

static bool reopen_component_matches(const tsupport::Bits &ours, const capture_foliage::Component &c) {
    if (c.has_states) {
        return ours.size() == c.block.bits && ours.slice(0, 283) == tsupport::bits_of_hex(c.block.hex, 283);
    }
    return ours == tsupport::bits_of_hex(c.block.hex, c.block.bits);
}

static size_t count_matching_reopen_blocks(const ClientFoliage::Open &open, const capture_foliage::IfaOpen &golden) {
    if (open.blocks.size() != 1 + golden.component_count) return 0;
    size_t same = (open.blocks[0] == tsupport::bits_of_hex(golden.actor.hex, golden.actor.bits)) ? 1 : 0;
    for (uint32_t i = 0; i < golden.component_count; ++i) {
        if (reopen_component_matches(open.blocks[1 + i], golden.components[i])) ++same;
    }
    return same;
}

static void check_reopen_blocks_and_regrow(FRig &fr, Player &p, ClientFoliage &client, size_t held_opens,
                                           const capture_foliage::IfaOpen &golden) {
    const ClientFoliage::Open &open = client.opens.back();
    const ClientName cell_name = client_fname(fid::kOwner);
    CHECK(open.name == cell_name && open.blocks.size() == 1 + golden.component_count,
          "the re-open has the actor block and a block for each of the cell's 8 components, as the retail one");
    CHECK(open.with_states == 1 && open.changed.size() == 1 && open.changed[0] == std::make_pair(0u, 3u) &&
              open.deleted == 0,
          "only one component carries an IndexedStates field: the instance 0 is Empty (3), nothing deleted");
    const size_t same = count_matching_reopen_blocks(open, golden);
    char msg[300];
    std::snprintf(msg, sizeof msg,
                  "the actor block (520 bits), the 7 untouched components (283 bits each) and the cut one (487 bits, "
                  "its first 283 bits) equal the retail capture's re-open (%zu of 9 blocks)",
                  same);
    CHECK(same == 1 + golden.component_count, msg);
    CHECK(client.lookup[cell_name].states[client_fname(fid::kFismc0)][0] == 3u,
          "so the client lists the instance as Empty");
    CHECK(client.closes_destroyed == 0 && client.closes_relevancy == 0 && client.closes_dormancy == client.opens.size(),
          "and every state actor close so far was a Dormancy close");
    const size_t regrow_from = p.got.size();
    fr.rig.run(seconds(40));
    client.absorb(p, regrow_from);
    CHECK(client.opens.size() == held_opens + 4,
          "the regrowth (Infancy, Juvenile, back to its painted state) opened the cell three more times");
    CHECK(client.opens.size() == held_opens + 4 && client.opens.back().deleted == 1 &&
              client.opens.back().changed.empty() && client.opens.back().with_states == 1,
          "the last one deletes the instance from the list: it is whole again");
}

static void test_cut_on_a_held_cell() {
    std::printf(
        "a cut on a cell the client holds: the actor opens again with the change [R: the retail capture packet 901]\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    ClientFoliage client;
    client.absorb(p, 0);
    const ClientName cell_name = client_fname(fid::kOwner);
    CHECK(client.lookup.count(cell_name) == 1 && client.resolves(fid::kOwner, fid::kFismc0),
          "before the cut the client holds the Oak's cell (sent at login, closed with Dormancy)");
    const size_t held_opens = client.opens.size();
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(run_until(fr.rig, [&] { return p.s->foliage_completed() == 1; }, seconds(20)), "the Oak is cut");
    fr.rig.run(milliseconds(600));
    client.absorb(p, from);
    CHECK(client.opens.size() == held_opens + 1,
          "the cut made the server open the cell's actor once more, on a new channel");
    if (client.opens.size() != held_opens + 1) return;
    const capture_foliage::IfaOpen *golden = nullptr;
    for (const auto &g : capture_foliage::kIfaOpens) {
        if (g.capture == 1428 && g.packet == 901) golden = &g;
    }
    CHECK(golden != nullptr && std::string(golden->name) == fid::kOwner && golden->component_count == 8,
          "the retail capture's re-open (packet 901) is the same cell: 8 components");
    if (!golden) return;
    check_reopen_blocks_and_regrow(fr, p, client, held_opens, *golden);
}

static size_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<size_t>(pmc.WorkingSetSize);
#endif
    return 0;
}

struct ViewLoadStats {
    double step_us[2] = {0, 0};
    size_t tracked[2] = {0, 0}, opens_at_login[2] = {0, 0};
    double last_open_ms[2] = {0, 0};
    size_t snapshots[2] = {0, 0}, capped[2] = {0, 0};
    size_t tour_cells = 0, tour_components = 0, tour_opens = 0, tour_uninitialized = 0;
    size_t tour_tracked = 0, tour_dormancy = 0, tour_other_closes = 0;
    double tour_ms = 0, tour_mb = 0, tour_kb_per_open = 0;
};

static void run_view_tour(FRig &fr, Player &p, ViewLoadStats &st) {
    const foliage::FoliageMap &map = fr.f().map();
    p.got.clear();
    ClientFoliage tourist;
    const size_t ws0 = working_set_bytes();
    const auto t1 = std::chrono::steady_clock::now();
    size_t bits_total = 0;
    for (uint16_t o = 0; o < map.owners().size(); ++o) {
        double cx = 0, cy = 0;
        map.cell_center(o, cx, cy);
        p.s->gm_teleport(p.conn, cx, cy, static_cast<double>(map.owners()[o].centre_z_cm()) + 500.0, fr.rig.t,
                         "the test: the tour");
        fr.rig.run(milliseconds(300));
        tourist.absorb(p, 0);
        for (const fake::Delivered &d : p.got) bits_total += d.content.bit_pos();
        p.got.clear();
    }
    fr.rig.run(seconds(1));
    tourist.absorb(p, 0);
    p.got.clear();
    st.tour_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    const size_t ws1 = working_set_bytes();
    st.tour_mb = ws1 > ws0 ? static_cast<double>(ws1 - ws0) / (1024.0 * 1024.0) : 0.0;
    st.tour_cells = tourist.lookup.size();
    count_client_components(tourist, st.tour_components, st.tour_uninitialized);
    st.tour_opens = tourist.opens.size();
    st.tour_kb_per_open =
        st.tour_opens ? static_cast<double>(bits_total) / 8.0 / 1024.0 / static_cast<double>(st.tour_opens) : 0.0;
    st.tour_tracked = p.s->actor_view()->tracked();
    st.tour_dormancy = tourist.closes_dormancy;
    st.tour_other_closes = tourist.closes_destroyed + tourist.closes_relevancy + tourist.closes_other;
}

static bool run_view_load_pass(int with, ViewLoadStats &st) {
    FRig fr;
    if (!fr.open(0.01, with == 1)) return false;
    Player &p = fr.rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -777947.0, 617946.0, 15952.05));
    fr.rig.settle();
    p.client_sends(
        {p.client.actor(p.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    ClientFoliage client;
    size_t absorbed = 0, counted = 0;
    const Clock::time_point joined = fr.rig.t;
    double last = 0;
    for (int s = 0; s < 300; ++s) {
        fr.rig.step();
        client.absorb(p, absorbed);
        absorbed = p.got.size();
        if (client.opens.size() != counted) {
            counted = client.opens.size();
            last = std::chrono::duration<double, std::milli>(fr.rig.t - joined).count();
        }
    }
    st.opens_at_login[with] = client.opens.size();
    st.last_open_ms[with] = last;
    fr.rig.settle();
    const auto t0 = std::chrono::steady_clock::now();
    const int steps = 1500;
    for (int s = 0; s < steps; ++s) fr.rig.step();
    st.step_us[with] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / steps;
    st.tracked[with] = p.s->actor_view()->tracked();
    st.snapshots[with] = p.s->actor_view()->stats().snapshots;
    st.capped[with] = p.s->actor_view()->stats().capped;
    if (with == 1) run_view_tour(fr, p, st);
    return true;
}

static void test_view_load() {
    std::printf("the load on the view: 671 state actors, tracked and sent to one player\n");
    ViewLoadStats st;
    for (int with = 0; with < 2; ++with) {
        if (!run_view_load_pass(with, st)) return;
    }
    std::printf("    without state actors: %.1f us a 20 ms step (%zu actors tracked, %zu snapshots, %zu passes capped "
                "by the 16 opens)\n",
                st.step_us[0], st.tracked[0], st.snapshots[0], st.capped[0]);
    std::printf("    with 671 state actors: %.1f us a 20 ms step (%zu actors tracked, %zu snapshots, %zu passes "
                "capped); %zu state actors sent at login, the last %.0f ms after the phase-in\n",
                st.step_us[1], st.tracked[1], st.snapshots[1], st.capped[1], st.opens_at_login[1], st.last_open_ms[1]);
    std::printf("    a tour of all %zu cells (a stop at each centre): %zu cells and %zu components delivered, %zu "
                "opens (%.1f KB each on the wire, exports included), the view tracking %zu at the end; "
                "%.0f ms of real time; the process grew by %.1f MB\n",
                st.tour_cells, st.tour_cells, st.tour_components, st.tour_opens, st.tour_kb_per_open, st.tour_tracked,
                st.tour_ms, st.tour_mb);
    CHECK(st.opens_at_login[0] == 0 && st.opens_at_login[1] >= 3,
          "the player is sent the state actors of the cells around him at login (none without them)");
    CHECK(st.last_open_ms[1] < 4000.0, "the last of them arrives within four seconds of the phase-in (retail: 4.1 to "
                                       "4.7 s after its capture began; the view paces 16 opens a pass)");
    CHECK(st.step_us[1] < st.step_us[0] + 2000.0 && st.step_us[1] < 20000.0,
          "a standing player's step costs less than 2 ms more with all the cells tracked");
    CHECK(st.tour_cells == 671 && st.tour_components == 10613 && st.tour_uninitialized == 0,
          "a player who stops in every cell is sent all 671 state actors, 10,613 components, each initialized");
    std::printf("    (the tour: %zu opens, %zu Dormancy closes, %zu other closes)\n", st.tour_opens, st.tour_dormancy,
                st.tour_other_closes);
    CHECK(st.tour_other_closes == 0 && st.tour_dormancy == st.tour_opens,
          "... each closed with Dormancy, none destroyed");
    CHECK(st.tour_mb < 200.0,
          "and the server holds under 200 MB for him (the shadows of 671 cells and what he was sent)");
}

static bool open_real_world_riverlands(Rig &rig, double &load_s) {
    std::string open_error;
    rig.db = content::ContentDb::open(g_path, open_error);
    if (!rig.db) {
        std::printf("  [skip] the content.db does not open: %s\n", open_error.c_str());
        return false;
    }
    rig.world = std::make_unique<PlayerWorld>(&*rig.db, "", bs::GameStateMode::Tier2, kServerId, rig.guids);
    rig.world->set_position_sink(&rig.sink);
    std::string error;
    const std::string motion = (fs::path(g_tsv).parent_path() / "npc_motion.tsv").string();
    if (!rig.world->load_blueprints(motion, error)) std::printf("    (no Blueprint table: %s)\n", error.c_str());
    const auto loading = std::chrono::steady_clock::now();
    if (!rig.world->load_area(spawns::AreaConfig::riverlands(), error)) {
        std::printf("  [skip] the Riverlands do not load here: %s\n", error.c_str());
        return false;
    }
    load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - loading).count();
    gather::Config gcfg;
    if (!rig.world->start_gathering(gcfg, error)) {
        std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
        return false;
    }
    foliage::Config cfg;
    cfg.map_path = g_tsv;
    cfg.regrow_mult = 0.01;
    if (!rig.world->start_foliage(cfg, error)) {
        std::printf("  [FAIL] foliage does not start: %s\n", error.c_str());
        return false;
    }
    return true;
}

static void test_login_in_the_real_world() {
    std::printf("the login in the real world: the Riverlands' creatures and nodes beside the 671 state actors\n");
    Rig rig;
    double load_s = 0;
    if (!open_real_world_riverlands(rig, load_s)) return;
    const size_t registry_size = rig.world->registry().size();
    const size_t npcs = rig.world->registry().count(world::ActorKind::Npc),
                 nodes = rig.world->registry().count(world::ActorKind::Gatherable);
    Player &p = rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", -777947.0, 617946.0, 15952.05));
    rig.settle();
    p.client_sends(
        {p.client.actor(p.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    ClientFoliage client;
    size_t absorbed = 0, counted = 0;
    const Clock::time_point joined = rig.t;
    double first_ms = -1, last_ms = 0;
    for (int s = 0; s < 500; ++s) {
        rig.step();
        client.absorb(p, absorbed);
        absorbed = p.got.size();
        if (client.opens.size() != counted) {
            counted = client.opens.size();
            const double ms = std::chrono::duration<double, std::milli>(rig.t - joined).count();
            if (first_ms < 0) first_ms = ms;
            last_ms = ms;
        }
    }
    const auto &st = p.s->actor_view()->stats();
    std::printf("    the area: %zu actors in the registry (%zu creatures, %zu nodes), loaded in %.1f s\n",
                registry_size, npcs, nodes, load_s);
    std::printf("    in the first 10 s after the phase-in the view opened %zu actors (%zu as snapshots, %zu awake), "
                "%zu state actors among them, the first at %.0f ms and the last at %.0f ms; "
                "%zu passes held opens back at the cap of 16, %zu were throttled by the connection\n",
                st.opens + st.snapshots, st.snapshots, st.opens, client.opens.size(), first_ms, last_ms, st.capped,
                st.throttled);
    const auto t0 = std::chrono::steady_clock::now();
    const int steps = 1500;
    for (int s = 0; s < steps; ++s) rig.step();
    const double step_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / steps;
    std::printf(
        "    a standing player's 20 ms step takes %.0f us with the view tracking %zu actors (%zu awake, %zu dormant)\n",
        step_us, p.s->actor_view()->tracked(), p.s->actor_view()->count(world::ClientState::Awake),
        p.s->actor_view()->count(world::ClientState::Dormant));
    CHECK(client.opens.size() >= 3 && last_ms < 4000.0,
          "the state actors of the cells around the login spot all arrive within four seconds of the phase-in, behind "
          "the creatures nearer than they");
    double oak_distance = 0;
    const Spot oak{"Oak", -775619, 617693, 16200, "Tree_Oak_Large_2"};
    const auto oak_row = nearest_row(rig.world->foliage()->map(), oak, &oak_distance);
    CHECK(oak_row && client_resolves(client, rig.world->foliage()->map(), *oak_row),
          "and the user's Oak, 23 m away, resolves on his client by then");
    CHECK(step_us < 5000.0, "and a step takes under 5 ms");
}

} // namespace

int main() {
    if (!init_foliage_session_env()) return 0;
    if (std::getenv("FOLIAGE_ONLY_UNNAMED_CALL")) {
        return 0;
    }
    test_delivery();
    test_cut_on_a_held_cell();
    test_view_load();
    test_login_in_the_real_world();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
