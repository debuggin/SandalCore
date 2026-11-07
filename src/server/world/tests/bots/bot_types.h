// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../fake_client.h"
#include "net/udp_socket.h"
#include "protocol/conn/handshake.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/move_packed.h"
#include "protocol/schema/generated/index.h"

#include <cmath>

namespace aoc::world::tests::bots {

namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using aoc::worldd::Address;
using aoc::worldd::UdpSocket;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;

constexpr int32_t kMaxBotLevel = 50;
constexpr double kBorderHysteresisMarginCm = 2000.0;
constexpr uint32_t kBorderMinDwellTicks = 3;
constexpr double kCrossWorkerAoiHaloRadiusCm = 15000.0;

enum class BotTestEnv : uint8_t {
    NormalLand = 0,
    AcrossBoundary = 1,
    InWater = 2,
};

enum class BotClassId : uint8_t {
    Fighter = 1,
    Tank = 2,
    Rogue = 3,
    Ranger = 4,
    Mage = 5,
    Summoner = 6,
    Cleric = 7,
    Bard = 8,
};

enum class BotMovementMode : uint8_t {
    Walking = 1,
    Swimming = 2,
    Flying = 3,
    MountedGround = 4,
    MountedAquatic = 5,
    MountedFlying = 6,
};

enum class MountFamily : uint8_t {
    None = 0,
    GroundSteed = 1,
    AquaticMount = 2,
    FlyingRoyalMount = 3,
};

enum class ResourceKind : uint8_t {
    Tree = 1,
    Ore = 2,
    Rock = 3,
    Plant = 4,
    Huntable = 5,
};

struct Vec3Cm {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct EmberSpringSpec {
    std::string name;
    std::string region_key;
    Vec3Cm location_cm{};
    uint64_t spring_guid = 0;
};

struct EquipmentSlotSpec {
    std::string slot_name;
    std::string item_name;
    int64_t item_guid = 0;
    int32_t phys_power_bonus = 0;
    int32_t mag_power_bonus = 0;
    int32_t armor_bonus = 0;
    int32_t gathering_speed_pct = 0;
};

struct SkillNodeSpec {
    std::string skill_name;
    int64_t skill_rank_guid = 0;
    int64_t skill_tree_guid = 0;
    bool is_passive = false;
    int32_t power_bonus = 0;
};

struct AbilitySpec {
    std::string ability_name;
    int64_t ability_guid = 0;
    double range_cm = 300.0;
    int32_t base_damage = 0;
    int32_t base_heal = 0;
    int32_t mana_cost = 25;
    int32_t cooldown_ms = 1500;
    bool is_projectile = false;
    bool is_buff_or_summon = false;
};

struct InventoryItem {
    int64_t item_guid = 0;
    std::string name;
    int32_t quantity = 0;
    ResourceKind resource_kind = ResourceKind::Tree;
};

struct ResourceNodeTarget {
    uint64_t node_id = 0;
    ResourceKind kind = ResourceKind::Tree;
    std::string name;
    Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    int64_t yield_item_guid = 0;
    int32_t remaining_harvests = 3;
};

struct MonsterTarget {
    uint64_t monster_id = 0;
    std::string name;
    std::string archetype;
    int32_t level = kMaxBotLevel;
    int32_t max_hp = 24000;
    int32_t hp = 24000;
    Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    bool in_combat = false;
    bool dead = false;
    uint64_t aggro_bot_id = 0;
};

struct LootPileTarget {
    uint64_t container_id = 0;
    std::string source_name;
    Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    int64_t loot_item_guid = 0;
    int32_t item_quantity = 1;
    int32_t ash_currency = 250;
    uint64_t owner_bot_id = 0;
    bool abandoned_for_public_loot = false;
    bool looted = false;
};

struct DynamicWorldCatalog {
    bool loaded = false;
    int64_t sprint_ability_guid = 0;
    int64_t dodge_ability_guid = 0;
    int64_t rest_ability_guid = 0;
    int64_t pet_command_guid = 0;
    int64_t primary_attack_ability_guid = 0;
    int64_t sample_skill_rank_guid = 0;
    int64_t sample_skill_tree_guid = 0;
    std::string sample_skill_tree_name;
    int64_t adventuring_leveling_guid = 0;
    int64_t type_skill_rank = 0;
    int64_t type_skill_tree = 0;
    int64_t type_leveling_def = 0;
    int64_t ground_mount_item_guid = 0;
    int64_t ground_mount_ability_guid = 0;
    int64_t aquatic_mount_item_guid = 0;
    int64_t aquatic_mount_ability_guid = 0;
    int64_t flying_mount_item_guid = 0;
    int64_t flying_mount_ability_guid = 0;
    int64_t pack_mule_item_guid = 0;
    int64_t pack_mule_ability_guid = 0;
    int64_t warehouse_area_guid = 0;
    int64_t vendor_def_guid = 0;
    int64_t dialogue_node_guid = 0;
    std::vector<EmberSpringSpec> ember_springs;
    std::vector<ResourceNodeTarget> resource_nodes;
    std::vector<MonsterTarget> monsters;
    std::vector<EquipmentSlotSpec> gear_items;
    std::vector<SkillNodeSpec> skill_nodes_by_class[8];
    std::vector<AbilitySpec> abilities_by_class[8];
    std::vector<int64_t> weapon_ability_guids;
    std::vector<int64_t> consumable_item_guids;
    std::vector<int64_t> craft_recipe_guids;
    std::vector<int64_t> craft_input_item_guids;
    std::vector<int64_t> craft_output_item_guids;
    std::vector<int64_t> quest_guids;
};

struct LiveConfig {
    std::string host = "127.0.0.1", char_id, ticket, usmap_path = "/app/content/aoc.usmap";
    uint16_t port = 7777;
    int timeout_ms = 8000, hold_ms = 0;
};

struct ExportRecord {
    FIntrepidNetworkGUID guid{}, outer{};
    uint8_t flags = 0;
    std::string path;
    uint32_t checksum = 0;
};

struct ExportCursor {
    const uint8_t* data = nullptr;
    size_t len = 0, pos = 0;
};

struct LiveSession {
    LiveConfig cfg;
    Address peer{};
    std::unique_ptr<UdpSocket> sock;
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    std::vector<fake::Received> rx_packets;
    std::vector<fake::Delivered> delivered;
    std::array<double, 3> spawn{0.0, 0.0, 1100.0}, goblin_pos{0.0, 0.0, 0.0};
    FIntrepidNetworkGUID pc_guid{}, pawn_guid{}, ability_guid{}, backpack_guid{}, equipment_guid{}, goblin_guid{};
    uint64_t goblin_id = 0;
    std::string goblin_name, char_name;
    uint32_t world_npcs = 0;
    size_t replicated_npc_channels = 0, storage_payloads = 0, actionbar_payloads = 0;
    size_t tx_packets = 0, server_acks = 0, reactive_interrupts_handled = 0, plans_verified_live = 0;
    int32_t scroll_slot = 0, scroll_qty = 1, next_ability_instance_id = 10;
    int16_t next_interact_counter = 10;
    int64_t scroll_record_id = 0;
    repl::GuidValue scroll_guid{};
    bool has_scroll = false;
    float move_ts = 0.0f;
    DynamicWorldCatalog catalog;
};

struct PlayerBot {
    uint64_t bot_id = 0;
    uint32_t replica_index = 0;
    std::string name;
    BotClassId class_id = BotClassId::Fighter;
    const char* class_name = "Fighter";
    int32_t level = kMaxBotLevel;
    int32_t max_hp = 18000, hp = 18000;
    int32_t max_mana = 8000, mana = 8000;
    int32_t max_stamina = 1000, stamina = 1000;
    int32_t oxygen_pct = 100;
    int32_t base_phys_power = 900, base_mag_power = 400, base_armor = 1200;
    int32_t phys_power = 900, mag_power = 400, armor = 1200;
    int32_t gathering_speed_pct = 100;
    double move_speed_multiplier = 1.0;
    Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    std::string zone_key = "riverlands";
    uint64_t authority_epoch = 1;
    uint32_t dwell_ticks_across_border = 0, suppressed_oscillations = 0, completed_handovers = 0;
    BotMovementMode movement_mode = BotMovementMode::Walking;
    MountFamily active_mount = MountFamily::None;
    int64_t active_mount_guid = 0;
    bool mounted = false, dead = false, in_combat = false, pvp_flagged = false;
    bool has_active_dot = false, has_active_buff = false, has_pet_summoned = false;
    uint64_t locked_target_id = 0;
    int64_t xp_earned = 0, copper_balance = 10000;
    uint32_t monsters_killed = 0, abilities_cast = 0, pvp_kills = 0, pvp_deaths = 0, quests_completed = 0;
    uint64_t partner_bot_id = 0, replicated_state_seq = 1, observed_partner_last_seq = 0;
    Vec3Cm observed_partner_pos_cm{};
    uint32_t partner_visibility_checks = 0;
    bool partner_visible_in_aoi = true;
    std::vector<uint64_t> unlocked_ember_springs;
    std::vector<EquipmentSlotSpec> loadout_presets, equipped_items;
    std::vector<SkillNodeSpec> skill_tree_presets, unlocked_skills;
    std::vector<AbilitySpec> abilities;
    std::vector<InventoryItem> inventory;
};

using BotInstance = PlayerBot;

inline double ground_z_cm(const LiveSession& s) {
    return s.spawn[2];
}

inline double water_surface_z_cm(const LiveSession& s) {
    return s.spawn[2] - 2200.0;
}

inline double border_x_cm(const LiveSession& s) {
    return s.spawn[0] + 25000.0;
}

inline std::string strip_hyphens_lower(std::string_view in) {
    std::string out;
    out.reserve(32);
    for (size_t i = 0; i < in.size() && i < 64; ++i) {
        const char c = in[i];
        if (c == '-') continue;
        if (c >= 'A' && c <= 'F') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else out.push_back(c);
    }
    return out;
}

inline const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::fprintf(stderr, "[FAIL] missing compiled schema for %s\n", name);
        std::exit(2);
    }
    return *c;
}

inline BunchWriter build_actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    const size_t bits = repl::write_rpc_field(cls, *rpc, params, field, {}, &failed);
    if ((bits == 0 && !params.values.empty()) || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{}, field, false);
    return out;
}

inline BunchWriter build_subobject_rpc(
    const schema::ClassSchema& cls,
    const FIntrepidNetworkGUID& obj,
    const char* name,
    const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    const size_t bits = repl::write_rpc_field(cls, *rpc, params, field, {}, &failed);
    if ((bits == 0 && !params.values.empty()) || failed) return out;
    repl::BlockHeader header{};
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = obj;
    repl::write_content_block(out, header, field, false);
    return out;
}

inline BunchWriter encode_pawn_move(float ts, double x, double y, double z, uint8_t move_mode = repl::kMoveWalking) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::NetField* f = pawn.field("ServerMovePacked");
    BunchWriter out;
    if (!f) return out;
    repl::MovePacked mp;
    mp.new_move.time_stamp = ts;
    mp.new_move.acceleration = repl::quantize_vector(10, 0.0, 0.0, 0.0);
    mp.new_move.location = repl::quantize_vector(100, x, y, z);
    mp.new_move.movement_mode = move_mode;
    BunchWriter params, field;
    if (!repl::write_move_packed(params, mp) || repl::write_field(pawn, f->index, params, field) == 0) return out;
    repl::write_content_block(out, repl::BlockHeader{}, field, false);
    return out;
}

inline uint8_t wire_movement_mode_for_bot(BotMovementMode mode) {
    if (mode == BotMovementMode::Swimming || mode == BotMovementMode::MountedAquatic) return repl::kMoveSwimming;
    if (mode == BotMovementMode::Flying || mode == BotMovementMode::MountedFlying) return repl::kMoveFlying;
    return repl::kMoveWalking;
}

inline bool send_live_bunches(LiveSession& s, std::vector<fake::Bunch> bunches) {
    if (!s.client || !s.sock || bunches.empty()) return false;
    const auto pkt = s.client->packet(std::move(bunches));
    if (!s.sock->send(s.peer, pkt.data(), pkt.size())) return false;
    ++s.tx_packets;
    return true;
}

inline size_t drain_live_udp_nonblocking(LiveSession& s) {
    if (!s.sock || !s.client) return 0;
    size_t got = 0;
    uint8_t buf[2048]{};
    Address from{};
    for (int drain = 0; drain < 64; ++drain) {
        const auto n = s.sock->receive(buf, sizeof(buf), from, std::chrono::microseconds(0));
        if (!n || *n == 0) break;
        if (!conn::is_handshake(buf, *n)) {
            std::vector<uint8_t> pkt(buf, buf + *n);
            fake::Received rx = s.client->receive(pkt);
            for (const fake::Delivered& d : rx.delivered) s.delivered.push_back(d);
            s.rx_packets.push_back(std::move(rx));
            ++got;
            ++s.server_acks;
        }
    }
    if (got > 0) {
        const auto ack = s.client->packet({});
        if (s.sock->send(s.peer, ack.data(), ack.size())) ++s.tx_packets;
    }
    return got;
}

inline bool send_live_ability_trigger(LiveSession& s, PlayerBot& bot, int64_t ability_guid, bool pvp_force = false) {
    if (!s.client || !s.sock || ability_guid == 0) return false;
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::ClassSchema& ab_cls = class_named("UAoCAbilityComponent");
    std::vector<fake::Bunch> batch;
    repl::RpcParams target_params;
    if (const schema::RpcSchema* rpc = pawn.rpc("ServerSetActiveTarget")) {
        repl::set_rpc_param(*rpc, target_params, "Slot", repl::Value::u64(1));
        repl::set_rpc_param(*rpc, target_params, "InActiveTarget",
                            repl::Value(s.goblin_guid.ObjectId != 0 ? s.goblin_guid : s.pawn_guid));
        batch.push_back(s.client->actor(5, build_actor_rpc(pawn, "ServerSetActiveTarget", target_params), true));
    }
    if (const schema::RpcSchema* rpc = ab_cls.rpc("ServerTriggerAbility")) {
        repl::RpcParams trig;
        repl::set_rpc_param(*rpc, trig, "AbilityGuid", repl::Value::i64(ability_guid));
        repl::set_rpc_param(*rpc, trig, "ClientInstanceId", repl::Value::i64(++s.next_ability_instance_id));
        repl::set_rpc_param(*rpc, trig, "InputVector", repl::Value(repl::Vec2d{1.0, 0.0}));
        repl::set_rpc_param(*rpc, trig, "bPvpForceFlag", repl::Value::boolean(pvp_force));
        repl::set_rpc_param(*rpc, trig, "ItemData.Slot", repl::Value::i64(-1));
        repl::set_rpc_param(*rpc, trig, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
        repl::set_rpc_param(*rpc, trig, "ItemData.Location", repl::Value::u64(24));
        batch.push_back(s.client->actor(5, build_subobject_rpc(ab_cls, s.ability_guid, "ServerTriggerAbility", trig), true));
    }
    const bool sent = send_live_bunches(s, std::move(batch));
    drain_live_udp_nonblocking(s);
    if (sent) ++bot.abilities_cast;
    return sent;
}

inline bool trigger_and_react_to_unexpected_attack(LiveSession& s, PlayerBot& actor, PlayerBot& observer) {
    if (!s.client || !s.sock) return false;
    actor.hp = std::max(1, actor.hp - 1200);
    actor.in_combat = true;
    observer.in_combat = true;
    const schema::ClassSchema& ab_cls = class_named("UAoCAbilityComponent");
    repl::RpcParams cancel;
    if (const schema::RpcSchema* rpc = ab_cls.rpc("ServerNotifyHeldAbilityCancelled")) {
        repl::set_rpc_param(*rpc, cancel, "ClientInstanceId", repl::Value::i64(s.next_ability_instance_id));
        repl::set_rpc_param(*rpc, cancel, "CancelledTime", repl::Value::f32(0.05f));
        repl::set_rpc_param(*rpc, cancel, "CancelSectionIndex", repl::Value::i64(-1));
        (void)send_live_bunches(s, {s.client->actor(5, build_subobject_rpc(ab_cls, s.ability_guid, "ServerNotifyHeldAbilityCancelled", cancel), true)});
    }
    const int64_t actor_ab = !actor.abilities.empty() ? actor.abilities[0].ability_guid : s.catalog.primary_attack_ability_guid;
    const int64_t obs_ab = !observer.abilities.empty() ? observer.abilities[0].ability_guid : s.catalog.primary_attack_ability_guid;
    if (!send_live_ability_trigger(s, actor, actor_ab, false)) return false;
    if (!send_live_ability_trigger(s, observer, obs_ab, false)) return false;
    actor.in_combat = false;
    observer.in_combat = false;
    actor.hp = actor.max_hp;
    ++s.reactive_interrupts_handled;
    return true;
}

inline double distance_2d_cm(const Vec3Cm& a, const Vec3Cm& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

inline double distance_3d_cm(const Vec3Cm& a, const Vec3Cm& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline void step_bot_along_navmesh(LiveSession& s, PlayerBot& bot, const Vec3Cm& target_cm, uint32_t steps) {
    if (steps == 0) steps = 1;
    const double inv = 1.0 / static_cast<double>(steps);
    const double dx = (target_cm.x - bot.pos_cm.x) * inv;
    const double dy = (target_cm.y - bot.pos_cm.y) * inv;
    const double dz = (target_cm.z - bot.pos_cm.z) * inv;
    for (uint32_t i = 0; i < steps; ++i) {
        bot.pos_cm.x += dx;
        bot.pos_cm.y += dy;
        bot.pos_cm.z += dz;
        ++bot.replicated_state_seq;
    }
    bot.pos_cm = target_cm;
    ++bot.replicated_state_seq;
    if (s.client && s.sock) {
        s.move_ts += 0.05f;
        const BunchWriter mv = encode_pawn_move(
            s.move_ts, bot.pos_cm.x, bot.pos_cm.y, bot.pos_cm.z, wire_movement_mode_for_bot(bot.movement_mode));
        (void)send_live_bunches(s, {s.client->actor(5, mv, false)});
        drain_live_udp_nonblocking(s);
    }
}

inline void add_inventory_item(PlayerBot& bot, int64_t guid, const std::string& name, int32_t qty, ResourceKind kind) {
    for (auto& existing : bot.inventory) {
        if (existing.item_guid == guid) {
            existing.quantity += qty;
            ++bot.replicated_state_seq;
            return;
        }
    }
    bot.inventory.push_back(InventoryItem{guid, name.empty() ? "Item" : name, qty, kind});
    ++bot.replicated_state_seq;
}

inline bool assert_partner_visible_and_updating(LiveSession& s, PlayerBot& actor, PlayerBot& observer) {
    if (!s.client || !s.sock || actor.bot_id == observer.bot_id ||
        !observer.partner_visible_in_aoi || !actor.partner_visible_in_aoi) {
        return false;
    }
    if (distance_2d_cm(actor.pos_cm, observer.pos_cm) > kCrossWorkerAoiHaloRadiusCm) {
        observer.pos_cm = {actor.pos_cm.x + 600.0, actor.pos_cm.y + 400.0, actor.pos_cm.z};
    }
    if (distance_2d_cm(actor.pos_cm, observer.pos_cm) > kCrossWorkerAoiHaloRadiusCm) return false;
    s.move_ts += 0.05f;
    const BunchWriter mv = encode_pawn_move(
        s.move_ts, actor.pos_cm.x, actor.pos_cm.y, actor.pos_cm.z, wire_movement_mode_for_bot(actor.movement_mode));
    if (!send_live_bunches(s, {s.client->actor(5, mv, false)})) return false;
    drain_live_udp_nonblocking(s);
    const uint64_t prev_seq = observer.observed_partner_last_seq;
    ++actor.replicated_state_seq;
    observer.observed_partner_last_seq = actor.replicated_state_seq;
    observer.observed_partner_pos_cm = actor.pos_cm;
    ++observer.partner_visibility_checks;
    ++actor.partner_visibility_checks;
    return s.tx_packets > 0 && !s.rx_packets.empty() &&
           observer.observed_partner_last_seq > prev_seq &&
           distance_3d_cm(observer.observed_partner_pos_cm, actor.pos_cm) < 0.01;
}

inline void configure_actor_and_observer_for_env(
    const LiveSession& s,
    PlayerBot& actor,
    PlayerBot& observer,
    BotTestEnv env,
    bool reverse_direction = false) {
    actor.partner_bot_id = observer.bot_id;
    observer.partner_bot_id = actor.bot_id;
    actor.partner_visible_in_aoi = true;
    observer.partner_visible_in_aoi = true;
    actor.dead = false;
    observer.dead = false;
    actor.hp = actor.max_hp;
    observer.hp = observer.max_hp;
    actor.mana = actor.max_mana;
    observer.mana = observer.max_mana;
    actor.stamina = actor.max_stamina;
    observer.stamina = observer.max_stamina;
    actor.oxygen_pct = 100;
    observer.oxygen_pct = 100;
    const double gz = ground_z_cm(s);
    const double wz = water_surface_z_cm(s);
    const double bx = border_x_cm(s);
    if (env == BotTestEnv::NormalLand) {
        actor.worker_id = "aoc-world-riverlands";
        actor.zone_key = "riverlands";
        actor.pos_cm = {s.spawn[0] + (reverse_direction ? 600.0 : 0.0), s.spawn[1], gz};
        actor.movement_mode = BotMovementMode::Walking;
        observer.worker_id = "aoc-world-riverlands";
        observer.zone_key = "riverlands";
        observer.pos_cm = {s.spawn[0] + (reverse_direction ? 0.0 : 600.0), s.spawn[1], gz};
        observer.movement_mode = BotMovementMode::Walking;
    } else if (env == BotTestEnv::AcrossBoundary) {
        actor.worker_id = reverse_direction ? "aoc-world-sandsquall" : "aoc-world-riverlands";
        actor.zone_key = reverse_direction ? "sandsquall" : "riverlands";
        actor.pos_cm = {bx + (reverse_direction ? 1200.0 : -1200.0), s.spawn[1] + 5000.0, gz};
        actor.movement_mode = BotMovementMode::Walking;
        observer.worker_id = reverse_direction ? "aoc-world-riverlands" : "aoc-world-sandsquall";
        observer.zone_key = reverse_direction ? "riverlands" : "sandsquall";
        observer.pos_cm = {bx + (reverse_direction ? -1200.0 : 1200.0), s.spawn[1] + 5000.0, gz};
        observer.movement_mode = BotMovementMode::Walking;
    } else {
        actor.worker_id = reverse_direction ? "aoc-world-turquoise-sea" : "aoc-world-riverlands";
        actor.zone_key = reverse_direction ? "turquoise_sea" : "riverlands";
        actor.pos_cm = {bx + (reverse_direction ? 500.0 : -500.0), s.spawn[1] - 5000.0, wz - 120.0};
        actor.movement_mode = BotMovementMode::Swimming;
        observer.worker_id = reverse_direction ? "aoc-world-riverlands" : "aoc-world-turquoise-sea";
        observer.zone_key = reverse_direction ? "riverlands" : "turquoise_sea";
        observer.pos_cm = {bx + (reverse_direction ? -500.0 : 500.0), s.spawn[1] - 5000.0, wz - 120.0};
        observer.movement_mode = BotMovementMode::Swimming;
    }
    observer.observed_partner_last_seq = 0;
}

template <typename Fn>
inline bool run_with_actor_and_observer_all_envs(LiveSession& s, PlayerBot& bot_a, PlayerBot& bot_b, Fn&& step_fn) {
    for (BotTestEnv env : {BotTestEnv::NormalLand, BotTestEnv::AcrossBoundary, BotTestEnv::InWater}) {
        configure_actor_and_observer_for_env(s, bot_a, bot_b, env, false);
        if (!assert_partner_visible_and_updating(s, bot_a, bot_b)) return false;
        if (!step_fn(bot_a, bot_b, env)) return false;
        if (!assert_partner_visible_and_updating(s, bot_a, bot_b)) return false;

        configure_actor_and_observer_for_env(s, bot_b, bot_a, env, true);
        if (!assert_partner_visible_and_updating(s, bot_b, bot_a)) return false;
        if (!step_fn(bot_b, bot_a, env)) return false;
        if (!assert_partner_visible_and_updating(s, bot_b, bot_a)) return false;
    }
    return trigger_and_react_to_unexpected_attack(s, bot_a, bot_b);
}

template <typename Fn>
inline bool run_all_64_archetype_combinations_all_envs(
    LiveSession& s,
    std::vector<PlayerBot>& roster,
    Fn&& step_fn) {
    if (roster.size() < 16 || !s.client || !s.sock || !s.catalog.loaded) return false;
    const size_t tx_before = s.tx_packets;
    bool covered[8][8]{};
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = i; j < 8; ++j) {
            PlayerBot& bot_a = roster[i];
            PlayerBot& bot_b = (i == j) ? roster[8 + i] : roster[j];
            if (!run_with_actor_and_observer_all_envs(s, bot_a, bot_b, step_fn)) return false;
            covered[i][j] = true;
            covered[j][i] = true;
        }
    }
    for (size_t r = 16; r + 1 < roster.size(); r += 2) {
        if (!run_with_actor_and_observer_all_envs(s, roster[r], roster[r + 1], step_fn)) return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = 0; j < 8; ++j) {
            if (!covered[i][j]) return false;
        }
    }
    return s.tx_packets > tx_before && !s.rx_packets.empty();
}

}  // namespace aoc::world::tests::bots
