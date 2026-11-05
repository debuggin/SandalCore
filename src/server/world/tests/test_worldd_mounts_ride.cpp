// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_ride_common.h"

using namespace aoc::worldd;
using namespace aoc::protocol;
using std::chrono::milliseconds;
namespace bs = aoc::worldd::bootstrap;

namespace {

emit::BunchWriter possession_ack(const emit::FIntrepidNetworkGUID& actor) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    repl::RpcParams p;
    repl::set_rpc_param(*pc.rpc("ServerAcknowledgePossession"), p, "P", repl::Value(actor));
    return actor_rpc(pc, "ServerAcknowledgePossession", p);
}

std::vector<repl::RpcCall> calls_of(const fake::Delivered& d, const char* cls) {
    std::vector<repl::RpcCall> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const auto& b : *blocks) {
        if (b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named(cls), d.content.data(), b.payload_pos + b.payload_bits, p);
        if (calls) out.insert(out.end(), std::make_move_iterator(calls->begin()), std::make_move_iterator(calls->end()));
    }
    return out;
}

std::optional<emit::FIntrepidNetworkGUID> object_of(const repl::ObjectState& o, const char* path) {
    const repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    if (!v) return std::nullopt;
    if (const auto* g = v->get_if<emit::FIntrepidNetworkGUID>()) return *g;
    return std::nullopt;
}

size_t acks_on(const Harness& h, uint32_t channel, size_t from, const char* cls = "ANPCCharacter") {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == "ClientMoveResponsePacked";
    }
    return n;
}

size_t corrections_on(const Harness& h, uint32_t channel, size_t from, const char* cls = "ANPCCharacter") {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == "ClientAdjustPosition";
    }
    return n;
}

struct PcMountUpTrace {
    bool restart_mount = false;
    bool camera = false;
    bool clear = false;
    bool slot = false;
    size_t order_restart = 99;
    size_t order_camera = 99;
    size_t order_clear = 99;
    size_t order_slot = 99;
    size_t n = 0;
};

void record_pc_mount_up_rpc(const repl::RpcCall& c, const emit::FIntrepidNetworkGUID& mount_guid, PcMountUpTrace& tr) {
    if (!c.rpc) return;
    const std::string name = c.rpc->name;
    if (name == "ClientRestart") {
        const auto it = c.params.values.find(1);
        const auto* o = it != c.params.values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
        tr.restart_mount = o && *o == mount_guid;
        tr.order_restart = tr.n;
    } else if (name == "ClientSetCameraMode") {
        tr.camera = true;
        tr.order_camera = tr.n;
    } else if (name == "ClientClearActionbarsForInstigators") {
        tr.clear = true;
        tr.order_clear = tr.n;
    } else if (name == "ClientReceiveActionbarSlotPayload") {
        tr.slot = true;
        tr.order_slot = tr.n;
    }
    ++tr.n;
}

struct MountUpStateTrace {
    bool mount_actor = false;
    bool stats_rider = false;
    bool known = false;
    bool rider_ptr = false;
    bool interaction = false;
    bool owner_arrays = false;
};

void record_mount_up_state(uint64_t id, const repl::ObjectState& o, const bs::SessionGuids& g, MountUpStateTrace& tr) {
    const std::string cls = o.schema ? o.schema->name : "";
    if (id == 0) {
        tr.mount_actor = int_of(o, "RemoteRole") == 2 && object_of(o, "Owner") == g.player_controller &&
                         object_of(o, "Controller") == g.player_controller && object_of(o, "PlayerState") == g.player_state;
    } else if (cls == "UAoCStatsComponent") {
        tr.stats_rider = true;
        const schema::NetField* f = o.schema->field("StatRepInt32Owner");
        const repl::FastArrayState* a = f ? o.arrays.find(f->index) : nullptr;
        tr.owner_arrays = tr.owner_arrays || (a && a->items().size() > 20);
    } else if (cls == "UAoCAbilityComponent") {
        tr.known = tr.known || o.state.get(*o.schema, "KnownAbilities") != nullptr;
    } else if (cls == "UNPCSecondaryInfo") {
        tr.rider_ptr = tr.rider_ptr || object_of(o, "RiderPtr") == g.pawn;
    } else if (cls == "UInteractableComponent") {
        tr.interaction = tr.interaction || o.state.get(*o.schema, "CurrentInputStates") != nullptr;
    }
}

void verify_mount_up_pc_and_mount(const Harness& h, uint32_t mch, size_t from, const emit::FIntrepidNetworkGUID& mount_guid) {
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const world::WorldActor* m = h.mount();
    PcMountUpTrace pc_tr;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) record_pc_mount_up_rpc(c, mount_guid, pc_tr);
    }
    CHECK(pc_tr.restart_mount && pc_tr.camera && pc_tr.clear && pc_tr.slot, "the PlayerController gets ClientRestart(the mount), the camera mode and the bank-18 bar");
    CHECK(pc_tr.order_restart < pc_tr.order_camera && pc_tr.order_camera < pc_tr.order_clear && pc_tr.order_clear < pc_tr.order_slot, "in retail's order (910 packet 306)");
    MountUpStateTrace m_tr;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        for (const auto& [id, o] : states_of(*d, m->actor)) record_mount_up_state(id, o, g, m_tr);
    }
    CHECK(m_tr.mount_actor, "the mount's actor: RemoteRole 2, Owner and Controller the PlayerController, the PlayerState (910 packet 307)");
    CHECK(m_tr.stats_rider && m_tr.owner_arrays, "its StatsComponent: the rider's owner stat arrays");
    CHECK(m_tr.known && m_tr.rider_ptr && m_tr.interaction, "its KnownAbilities, RiderPtr and interaction state");
}

void record_pawn_attach_state(uint64_t id, const repl::ObjectState& o, const emit::FIntrepidNetworkGUID& mount_guid, bool& pawn_attached, bool& mount_ptr) {
    if (id == 0) {
        pawn_attached = int_of(o, "RemoteRole") == 1 && object_of(o, "AttachmentReplication.AttachParent") == mount_guid &&
                        object_of(o, "PlayerState") == emit::FIntrepidNetworkGUID{} && object_of(o, "Controller") == emit::FIntrepidNetworkGUID{} &&
                        int_of(o, "AttachmentState.bAttachParentValid") == 1 && int_of(o, "AttachmentState.InteractableZoneID") == 2;
    } else if (o.schema && o.schema->command("MountPtr")) {
        mount_ptr = object_of(o, "MountPtr") == mount_guid;
    }
}

void verify_mount_up_pawn_and_pc_attach(const Harness& h, size_t from, const emit::FIntrepidNetworkGUID& mount_guid, const emit::FIntrepidNetworkGUID& cylinder) {
    bool pawn_attached = false, mount_ptr = false;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) {
        for (const auto& [id, o] : states_of(*d, h.s->bootstrap().pawn())) record_pawn_attach_state(id, o, mount_guid, pawn_attached, mount_ptr);
    }
    CHECK(pawn_attached, "the rider's pawn is attached to the mount (MountSocket), with no PlayerState or Controller (910 packet 310)");
    CHECK(mount_ptr, "its InteractInfo names the mount (MountPtr)");
    bool pc_attached = false;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        const States st = states_of(*d, h.s->bootstrap().player_controller());
        const auto it = st.find(0);
        if (it != st.end()) {
            pc_attached = pc_attached || (object_of(it->second, "Pawn") == mount_guid && object_of(it->second, "AttachmentReplication.AttachParent") == mount_guid &&
                                          object_of(it->second, "AttachmentReplication.AttachComponent") == cylinder);
        }
    }
    CHECK(pc_attached, "the PlayerController is attached to the mount and its Pawn is the mount");
}

struct DismountPawnTrace {
    bool detached = false;
    bool detach_place = false;
    bool unmounted_ptr = false;
};

void record_pawn_detach_state(uint64_t id, const repl::ObjectState& o, const bs::SessionGuids& g, const world::WorldActor& m, DismountPawnTrace& tr) {
    const double yaw = m.yaw_degrees * 3.14159265358979 / 180.0;
    if (id == 0) {
        tr.detached = int_of(o, "RemoteRole") == 2 && object_of(o, "AttachmentReplication.AttachParent") == emit::FIntrepidNetworkGUID{} &&
                      object_of(o, "PlayerState") == g.player_state && object_of(o, "Controller") == g.player_controller &&
                      int_of(o, "AttachmentState.bAttachParentValid") == 0;
        const repl::Value* loc = o.state.get(*o.schema, "RepDetachmentLocation");
        const auto* v = loc ? loc->get_if<repl::Vec3d>() : nullptr;
        tr.detach_place = v && std::fabs(v->x - (m.x + 54 * std::cos(yaw))) < 1.0 &&
                          std::fabs(v->y - (m.y + 54 * std::sin(yaw))) < 1.0 && std::fabs(v->z - (m.z + 197)) < 1.0;
    } else if (o.schema && o.schema->command("MountPtr")) {
        tr.unmounted_ptr = object_of(o, "MountPtr") == emit::FIntrepidNetworkGUID{};
    }
}

void verify_dismount_packets(const Harness& h, uint32_t mch, size_t from2, const world::WorldActor& m) {
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    bool restart_pawn = false;
    size_t restarts = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from2)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientRestart") continue;
            const auto it = c.params.values.find(1);
            const auto* o = it != c.params.values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
            restart_pawn = o && *o == g.pawn;
            ++restarts;
        }
    }
    CHECK(restart_pawn && restarts == 1, "the PlayerController is restarted on its own pawn, once (912 packet 139)");
    DismountPawnTrace p_tr;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from2)) {
        for (const auto& [id, o] : states_of(*d, h.s->bootstrap().pawn())) record_pawn_detach_state(id, o, g, m, p_tr);
    }
    CHECK(p_tr.detached && p_tr.unmounted_ptr, "the pawn is detached: RemoteRole 2, its PlayerState and Controller back, MountPtr cleared (912 packet 139)");
    CHECK(p_tr.detach_place, "RepDetachmentLocation is 54 cm ahead of the mount and 197 cm above its origin (912)");
    bool mount_released = false;
    for (const fake::Delivered* d : chains_on(h, mch, from2)) {
        const States st = states_of(*d, h.mount()->actor);
        const auto it = st.find(0);
        if (it != st.end()) {
            mount_released = int_of(it->second, "RemoteRole") == 1 && object_of(it->second, "Owner") == emit::FIntrepidNetworkGUID{} &&
                             object_of(it->second, "Controller") == emit::FIntrepidNetworkGUID{};
        }
    }
    CHECK(mount_released, "the mount's Owner, Controller and PlayerState are released, RemoteRole 1 (912 packet 139)");
    CHECK(h.s->mounts()->phase() == MountController::Phase::Out && mount_count(h) == 1, "the mount stays");
    CHECK(h.s->movement().good().z > m.z + 150, "the pawn's checker starts where it lands");
}

void test_get_on_and_off() {
    std::printf("getting on and off a mount\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(300));
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Summoning, "(the mount is being summoned)");
    get_on(h);
    h.run(milliseconds(100));
    CHECK(!h.s->mounted() && h.s->mount_interactions_refused() == 1, "an interaction while the mount is being summoned is refused");
    h.run(milliseconds(1500));
    CHECK(h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    const world::WorldActor* m = h.mount();
    const uint32_t mch = h.mount_ch();
    CHECK(m && mch != 0 && spawns::sub_of(*m, "UInteractableComponent") != nullptr, "(the mount is open on a channel)");
    if (!m || mch == 0) return;
    const emit::FIntrepidNetworkGUID mount_guid = m->actor.guid;
    emit::FIntrepidNetworkGUID cylinder = mount_guid;
    cylinder.ObjectId = m->npc.collision_cylinder;
    get_on(h);
    h.run(milliseconds(100));
    CHECK(!h.s->mounted() && h.s->mount_interactions_refused() == 2, "an interaction 575 cm from the mount is refused (too far)");
    walk_up_to_mount(h);
    const size_t from = h.got.size();
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted() && h.s->mount_ups() == 1, "the player is on the mount");
    m = h.mount();
    verify_mount_up_pc_and_mount(h, mch, from, mount_guid);
    verify_mount_up_pawn_and_pc_attach(h, from, mount_guid, cylinder);
    h.client_sends({h.client.actor(h.pc_ch, possession_ack(mount_guid), true)});
    h.run(milliseconds(100));
    CHECK(!h.s->failed(), "the session takes the acknowledgement of the mount's possession");
    CHECK(h.s->ride() && h.s->ride()->speed_limit(1, 0) > 1400 && h.s->ride()->speed_limit(1, 0) < 1500, "the mount's move limit is its Blueprint's 990 cm/s times 1.5");
    CHECK(h.s->movement().good().x == m->x && h.s->movement().good().y == m->y, "the session's last good position is the mount's");

    const size_t from2 = h.got.size();
    get_off(h);
    h.run(milliseconds(200));
    CHECK(!h.s->mounted() && h.s->dismounts() == 1, "ServerDismountInteraction puts the rider off the mount");
    verify_dismount_packets(h, mch, from2, *m);
}

void verify_riding_speed_and_dismount(Harness& h, Rider& r, uint32_t mch, size_t from) {
    const size_t corr_before = corrections_on(h, mch, from);
    const double good_x = h.s->movement().good().x;
    ride_step(h, r, 3000, 0);
    CHECK(h.s->moves_refused() == 1 && corrections_on(h, mch, from) == corr_before + 1, "a move of 3,000 cm in 0.1 s is refused and corrected on the mount's channel");
    CHECK(std::fabs(h.s->movement().good().x - good_x) < 1.0, "the last good position stays");
    r.x = good_x;
    const size_t refused_before = h.s->moves_refused();
    for (int i = 0; i < 30; ++i) ride_step(h, r, 140, 0);
    CHECK(h.s->moves_refused() == refused_before, "30 moves at 1,400 cm/s (under 990 x 1.5) are all accepted");
    for (int i = 0; i < 30; ++i) ride_step(h, r, 170, 0);
    CHECK(h.s->moves_refused() > refused_before, "30 moves at 1,700 cm/s are refused once the credit is spent");
    CHECK(h.s->ride()->speed_limit(1, 0) < 1500, "the limit is the mount's own: 990 cm/s x 1.5");
    get_off(h);
    h.run(milliseconds(100));
    const size_t acked_pawn = h.s->moves_acknowledged();
    g_pawn_ts = 0.05f;
    const GoodPosition landed = h.s->movement().good();
    move_pawn(h, falling_move_at(g_pawn_ts, landed.x, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees)));
    CHECK(h.s->moves_acknowledged() == acked_pawn + 1, "the pawn's first move after the dismount, TimeStamp 0.05, is acknowledged (its clock restarted)");
    const size_t wrong = h.s->moves_on_the_wrong_channel();
    h.client_sends({h.client.actor(mch, client_move("ANPCCharacter", move_at(0.1f, r.x, r.y, r.z)), false)});
    CHECK(h.s->moves_on_the_wrong_channel() == wrong, "a move on the mount's channel after the dismount is nobody's (not routed)");
}

void test_riding_moves() {
    std::printf("riding: moves on the mount's channel\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const uint32_t mch = h.mount_ch();
    const world::WorldActor* m = h.mount();
    const size_t from = h.got.size();
    Rider r = start_riding(h);
    CHECK(g_pawn_ts > 0.5f, "(the pawn's clock is past 0.5 s: the mount's restarts)");
    for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);
    CHECK(h.s->mount_moves_acknowledged() == 20 && acks_on(h, mch, from) == 20, "20 moves at 990 cm/s are acknowledged on the mount's channel, the TimeStamp restarting near 0");
    CHECK(acks_on(h, h.pawn_ch, from, "PlayerPawn_C") == 0, "and none on the pawn's");
    CHECK(h.s->moves_refused() == 0 && std::fabs(h.s->movement().good().x - r.x) < 1.0, "none is refused; the mount's last good position follows");
    CHECK(std::fabs(h.mount()->x - r.x) < 1.0, "the mount's place in the registry follows too");
    size_t movement_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        const States st = states_of(*d, m->actor);
        const auto it = st.find(0);
        if (it != st.end() && it->second.state.get(7) != nullptr) ++movement_updates;
    }
    CHECK(movement_updates == 0, "the owner is sent no ReplicatedMovement of its possessed mount (910, 959, 871)");
    const size_t acked = h.s->moves_acknowledged();
    move_pawn(h, move_at(5.0f, r.x, r.y, r.z));
    CHECK(h.s->moves_on_the_wrong_channel() == 1 && h.s->moves_acknowledged() == acked, "a move on the pawn's channel while the player rides is dropped");
    verify_riding_speed_and_dismount(h, r, mch, from);
}

struct DashWireTrace {
    bool cooldown_sent = false;
    bool effect_sent = false;
    bool speed_sent = false;
};

void record_dash_mount_state(const repl::ObjectState& o, DashWireTrace& tr) {
    const std::string cls = o.schema ? o.schema->name : "";
    if (cls == "UAoCAbilityComponent") {
        const schema::NetField* f = o.schema->field("Cooldowns");
        const repl::FastArrayState* a = f ? o.arrays.find(f->index) : nullptr;
        tr.cooldown_sent = tr.cooldown_sent || (a && !a->items().empty());
    } else if (cls == "UAoCStatsComponent") {
        const schema::NetField* fe = o.schema->field("CurrentEffects");
        const repl::FastArrayState* ae = fe ? o.arrays.find(fe->index) : nullptr;
        tr.effect_sent = tr.effect_sent || (ae && !ae->items().empty());
        const schema::NetField* fs = o.schema->field("StatRepInt32EveryoneProxy");
        const repl::FastArrayState* as = fs ? o.arrays.find(fs->index) : nullptr;
        tr.speed_sent = tr.speed_sent || (as && !as->items().empty());
    }
}

void verify_dash_packets_and_arrays(const Harness& h, uint32_t mch, size_t from, int64_t dash) {
    const world::WorldActor* m = h.mount();
    const spawns::MountBook& book = *h.fx.world->mounts();
    const auto cds = items_of(*m, "UAoCAbilityComponent", "Cooldowns");
    const auto fx = items_of(*m, "UAoCStatsComponent", "CurrentEffects");
    CHECK(cds.size() == 1 && cds[0].values.at(1).get_if<int64_t>() && *cds[0].values.at(1).get_if<int64_t>() == dash && cds[0].values.at(2).get_if<bool>() && *cds[0].values.at(2).get_if<bool>() &&
              cds[0].values.at(3).get_if<float>() && *cds[0].values.at(3).get_if<float>() == 30.0f && cds[0].values.at(6).get_if<int64_t>() && *cds[0].values.at(6).get_if<int64_t>() == 0,
          "the Cooldowns item: the dash, triggered, 30 s, Charges 0");
    const content::MountEffectRow* dash_row = book.data().dash();
    CHECK(fx.size() == 1 && dash_row && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == dash_row->effect && fx[0].values.at(4).get_if<float>() &&
              *fx[0].values.at(4).get_if<float>() == 5.0f,
          "the CurrentEffects item: the dash's effect for 5 s");
    DashWireTrace tr;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        for (const auto& [id, o] : states_of(*d, m->actor)) record_dash_mount_state(o, tr);
    }
    CHECK(tr.cooldown_sent && tr.effect_sent && tr.speed_sent, "the client is sent the Cooldowns item, the effect and MoveSpeedMult on the mount's channel");
    size_t confirmations = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) confirmations += c.rpc && std::string(c.rpc->name) == "ClientHitConfirmationsReliable";
    }
    CHECK(confirmations == 1, "the PlayerController is sent the dash's hit confirmation (ClientHitConfirmationsReliable)");
}

void verify_dash_cooldown_and_expiry(Harness& h, Rider& r, uint32_t mch, size_t from, int64_t dash) {
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*h.mount());
    CHECK(std::fabs(h.s->ride()->speed_limit(1, 0.2) - 990.0 * 1.35 * 1.5) < 1.0, "the move limit is 990 x 1.35 x 1.5 while the dash lasts");
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 133.65, 0);
    CHECK(h.s->moves_refused() == refused, "20 moves at 1,336 cm/s (990 x 1.35) are accepted");
    const size_t ends_before = chains_on(h, mch, from).size();
    h.client_sends({h.client.actor(mch, trigger_on(h, comp, dash, 22), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == 1 && h.s->dashes_refused() == 1, "the dash off cooldown is refused");
    size_t end_calls = 0;
    for (const fake::Delivered* d : chains_on(h, mch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "UAoCAbilityComponent")) end_calls += c.rpc && std::string(c.rpc->name) == "ClientEndAbility";
    }
    CHECK(end_calls == 1 && chains_on(h, mch, from).size() > ends_before, "the client's cast is ended with ClientEndAbility on the mount's AbilityComponent");
    for (int i = 0; i < 60; ++i) ride_step(h, r, 99, 0);
    CHECK(!h.s->ride()->dash_active() && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(), "after 5 s the effect and MoveSpeedMult go");
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").size() == 1, "the cooldown item stays until 30 s after the start");
    for (int i = 0; i < 240; ++i) ride_step(h, r, 99, 0);
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").empty(), "30 s after its start the Cooldowns item is removed");
    h.client_sends({h.client.actor(mch, trigger_on(h, comp, dash, 23), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == 2 && h.s->ride()->dash_active(), "after the cooldown the dash starts again");
}

void test_dash() {
    std::printf("the dash\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const world::WorldActor* m = h.mount();
    const uint32_t mch = h.mount_ch();
    const spawns::MountBook& book = *h.fx.world->mounts();
    const int64_t dash = book.dash_ability();
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*m);
    Rider r = start_riding(h);
    CHECK(mult_of(*m).value_or(0) == 1.0 && h.s->ride()->dash_ability() == dash, "(MoveSpeedMult is 1, the mount's dash is MountAbility_Sprint)");
    const size_t from = h.got.size();
    const Clock::time_point trig = h.t;
    h.client_sends({h.client.actor(mch, trigger_on(h, comp, dash, 21), true)});
    CHECK(h.s->dashes_started() == 1, "the dash trigger on the mount's AbilityComponent starts the dash");
    h.step(milliseconds(40));
    CHECK(h.s->ride()->dash_pending() && !h.s->ride()->dash_active() && mult_of(*h.mount()).value_or(0) == 1.0, "nothing is changed before the reply");
    h.run(milliseconds(120));
    CHECK(h.s->ride()->dash_active() && std::fabs(mult_of(*h.mount()).value_or(0) - 1.35) < 1e-6, "94 ms after the trigger MoveSpeedMult is 1.35");
    verify_dash_packets_and_arrays(h, mch, from, dash);
    CHECK(h.since(trig) < 0.5, "(all within half a second)");
    verify_dash_cooldown_and_expiry(h, r, mch, from, dash);
}

std::optional<emit::FIntrepidNetworkGUID> client_mountable_pet(const Harness& h) {
    const repl::Actor& pawn = h.s->bootstrap().pawn();
    const repl::Subobject* info = nullptr;
    for (const repl::Subobject& s : pawn.subobjects) {
        if (s.name == "InteractInfo") info = &s;
    }
    if (!info) return std::nullopt;
    std::optional<emit::FIntrepidNetworkGUID> out;
    for (const fake::Delivered& d : h.got) {
        if (d.channel != h.pawn_ch || d.open || d.close) continue;
        const States st = states_of(d, pawn);
        const auto it = st.find(info->guid.ObjectId);
        if (it == st.end()) continue;
        if (const auto g = object_of(it->second, "MountablePet")) out = *g;
    }
    return out;
}

bool client_can_use_mount_slot(const Harness& h, int64_t ability) {
    const auto pet = client_mountable_pet(h);
    const world::WorldActor* m = h.mount();
    if (!pet || pet->ObjectId == 0 || !m || m->actor.guid.ObjectId != pet->ObjectId) return false;
    const repl::Subobject* ab = spawns::sub_of(*m, "UAoCAbilityComponent");
    const repl::Value* v = ab ? ab->obj.state.get(*ab->obj.schema, "KnownAbilities") : nullptr;
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr) return false;
    for (const repl::ElementState& e : arr->elements) {
        const auto it = e.values.find(1);
        if (it != e.values.end() && it->second.get_if<int64_t>() && *it->second.get_if<int64_t>() == ability) return true;
    }
    return false;
}

void verify_mountable_pet_for_slot(int slot) {
    Harness h(cyb_record());
    h.play();
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string kind = item ? std::to_string(item->item_record_id) : std::string("?");
    CHECK(!client_mountable_pet(h).has_value() || client_mountable_pet(h)->ObjectId == 0, "before any summon the pawn has no mountable pet");
    summon_with(h, slot);
    h.run(milliseconds(1900));
    const world::WorldActor* m = h.mount();
    CHECK(m != nullptr, ("(the mount of item " + kind + " is out)").c_str());
    if (!m) return;
    const auto pet = client_mountable_pet(h);
    CHECK(pet.has_value() && pet->ObjectId == m->actor.guid.ObjectId && pet->ObjectId != 0,
          ("the summon of item " + kind + " tells the client its mountable pet: the pawn's InteractInfo.MountablePet is the mount").c_str());
    desummon_with(h, slot);
    h.run(milliseconds(2800));
    const auto still = client_mountable_pet(h);
    CHECK(still.has_value() && still->ObjectId == m->actor.guid.ObjectId, "while the mount dissolves it is still the pet");
    h.run(milliseconds(1000));
    const auto gone = client_mountable_pet(h);
    CHECK(h.mount() == nullptr && gone.has_value() && gone->ObjectId == 0, "when the mount closes the pawn's MountablePet is 0");
}

void test_dash_gate() {
    std::printf("the dash: the client's gate (the pawn's MountablePet)\n");
    for (const int slot : {kMountSlot, kAquaticSlot, kFlyingSlot}) verify_mountable_pet_for_slot(slot);
    Harness h(cyb_record());
    h.play();
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    CHECK(!client_can_use_mount_slot(h, dash), "summoned and not mounted: the mount does not know the dash yet (retail sends KnownAbilities at the mount-up)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player rides)");
    if (!h.s->mounted()) return;
    CHECK(client_can_use_mount_slot(h, dash), "mounted: the client's gate for the bank-18 dash is open (the pet is the mount, which knows MountAbility_Sprint)");
    if (client_can_use_mount_slot(h, dash)) {
        const world::WorldActor* m = h.mount();
        h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*m), dash, 31), true)});
        h.run(milliseconds(200));
        CHECK(h.s->dashes_started() == 1, "the trigger the client sends starts the dash");
    }
}

void test_dash_survives_a_resummon() {
    std::printf("the dash's cooldown survives a desummon and a summon (review A I3: the tooltip says 30 s)\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*h.mount()), dash, 21), true)});
    h.run(milliseconds(300));
    CHECK(h.s->dashes_started() == 1 && h.s->ride()->dash_active(), "(the dash starts)");
    get_off(h);
    h.run(milliseconds(100));
    g_pawn_ts = 0.0f;
    desummon_with(h, kMountSlot);
    h.run(milliseconds(3700));
    CHECK(mount_count(h) == 0, "(the mount is gone)");
    summon_with(h, kMountSlot, 41);
    h.run(milliseconds(1900));
    CHECK(h.mount() != nullptr, "(summoned again: a new actor, a new Ride)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(on the new mount, about 7 s after the first dash)");
    if (!h.s->mounted()) return;
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").size() == 1, "the new mount's bar shows the dash still cooling down (a Cooldowns item)");
    const size_t started = h.s->dashes_started(), refused = h.s->dashes_refused();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*h.mount()), dash, 22), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == started && h.s->dashes_refused() == refused + 1 && !h.s->ride()->dash_active(), "the dash on the new mount is refused: it cools down for 30 s from the first");
    h.run(milliseconds(24500));
    CHECK(items_of(*h.mount(), "UAoCAbilityComponent", "Cooldowns").empty(), "30 s after the first dash's start the Cooldowns item goes");
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*h.mount()), dash, 23), true)});
    h.run(milliseconds(200));
    CHECK(h.s->dashes_started() == started + 1 && h.s->ride()->dash_active(), "and the dash works again");
}

void test_pace_bank_survives_a_dismount() {
    std::printf("the pace bank is the session's: a dismount does not refill it (review A I1)\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 6; ++i) ride_step(h, r, 99, 0);
    for (int i = 0; i < 4; ++i) {
        r.ts += 4.0f;
        move_mount(h, move_at(r.ts, r.x, r.y, r.z, r.yaw));
        h.step(milliseconds(20));
    }
    CHECK(h.s->movement().pace_bank() < 1.0, "(the bank is spent)");
    get_off(h);
    h.step(milliseconds(20));
    CHECK(!h.s->mounted() && h.s->movement().pace_bank() < 1.5, "after the dismount the pawn's checker has the same bank, not a full one");
    const size_t refused = h.s->moves_refused();
    g_pawn_ts = 0.05f;
    const GoodPosition landed = h.s->movement().good();
    move_pawn(h, falling_move_at(g_pawn_ts, landed.x + 2000.0, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees)));
    CHECK(h.s->moves_refused() == refused + 1, "so the first move of the pawn cannot claim the first-move second: 2,000 cm in an instant is refused");
}

const world::WorldActor* rider_pawn(const Harness& h, uint64_t id) {
    const world::WorldActor* found = nullptr;
    h.fx.world->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player && a.owner == id) found = &a;
    });
    return found;
}

bool rider_pawn_walks(const world::WorldActor& a) {
    const repl::ObjectState& o = a.actor.obj;
    const repl::Value* v = o.schema ? o.state.get(*o.schema, "CurrentMovementInput") : nullptr;
    const auto* input = v ? v->get_if<repl::PlayerMovementInput>() : nullptr;
    return input && input->bits != 0;
}

void test_presence_a_riders_pawn_stays_where_the_player_got_on() {
    std::printf("players seeing each other: the pawn other players are shown stays where its player got on, and follows again after the dismount\n");
    Harness h(cyb_record());
    h.play();
    const uint64_t id = static_cast<uint64_t>(h.s->bootstrap().setup().player_id);
    CHECK(rider_pawn(h, id) != nullptr, "(the player's pawn is in the registry after the phase-in)");
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted() || !rider_pawn(h, id)) return;
    h.run(milliseconds(400));
    const double got_on_x = rider_pawn(h, id)->x, got_on_y = rider_pawn(h, id)->y;
    Rider r = start_riding(h);
    for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);
    h.run(milliseconds(400));
    const world::WorldActor* pawn = rider_pawn(h, id);
    const world::WorldActor* mount = h.mount();
    CHECK(pawn && mount && std::fabs(mount->x - got_on_x) > 1500.0, "(the mount carried the rider more than 15 m)");
    CHECK(pawn && std::fabs(pawn->x - got_on_x) < 1.0 && std::fabs(pawn->y - got_on_y) < 1.0,
          "20 moves on the mount later the registry's pawn is where the player got on (within a centimetre), not where the mount is");
    CHECK(pawn && !rider_pawn_walks(*pawn), "...and shows no walking input (it stands, it does not run in place)");
    get_off(h);
    h.run(milliseconds(100));
    const GoodPosition landed = h.s->movement().good();
    g_pawn_ts = 0.05f;
    move_pawn(h, falling_move_at(g_pawn_ts, landed.x, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees)));
    h.run(milliseconds(300));
    pawn = rider_pawn(h, id);
    CHECK(pawn && std::fabs(pawn->x - landed.x) < 600.0 && std::fabs(pawn->y - landed.y) < 600.0 && std::fabs(pawn->x - got_on_x) > 1500.0,
          "after the dismount the pawn is where the player landed (beside the mount, more than 15 m from where it got on)");
}

} // namespace

int main() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    std::string error;
    {
        auto real = content::ContentDb::open(real_db_path(), error);
        const auto data = real ? real->mount_data(error) : std::nullopt;
        if (!data) {
            std::printf("  [FAIL] the real content.db has no mount tables (%s): %s\n", real_db_path().c_str(), error.c_str());
            return 2;
        }
        for (const content::MountRow& r : data->mounts) {
            if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
            if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
            if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
            if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
        }
    }
    test_get_on_and_off();
    test_riding_moves();
    test_dash();
    test_dash_gate();
    test_dash_survives_a_resummon();
    test_pace_bank_survives_a_dismount();
    test_presence_a_riders_pawn_stays_where_the_player_got_on();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
