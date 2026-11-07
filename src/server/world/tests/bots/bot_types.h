// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../fake_client.h"
#include "net/udp_socket.h"
#include "protocol/conn/handshake.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/rpc.h"
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

constexpr int64_t kGmAdventuringScroll = 6064634615524884604ll;
constexpr int64_t kBardTree = 6064629885900499588ll;
constexpr int64_t kInstrumentFlute = 6064630871707615237ll;
constexpr int64_t kGuidAdventuring = 6064629098744774667ll;
constexpr int64_t kTypeSkillRank = 6064628725236039698ll;
constexpr int64_t kTypeSkillTree = 6064628725236039697ll;
constexpr int64_t kTypeLevelingTypeDef = 6064628934189875200ll;
constexpr int64_t kSprintAbility = 6064631641648332884ll;
constexpr int64_t kScepterCombo = 6064631845843960261ll;
constexpr int64_t kPetFollowCmd = 6064630881966096959ll;

constexpr int32_t kMaxBotLevel = 50;
constexpr double kBorderHysteresisMarginCm = 2000.0;
constexpr uint32_t kBorderMinDwellTicks = 3;
constexpr double kCrossWorkerAoiHaloRadiusCm = 15000.0;
constexpr double kDefaultRiverWaterSurfaceZCm = 9200.0;
constexpr double kDefaultGroundTerrainZCm = 11400.0;

enum class BotTestEnv : uint8_t {
    NormalLand = 0,
    AcrossBoundary = 1,
    InWater = 2,
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
    std::string goblin_name;
    uint32_t world_npcs = 0;
    size_t replicated_npc_channels = 0, storage_payloads = 0, actionbar_payloads = 0;
    int32_t scroll_slot = 0, scroll_qty = 1;
    repl::GuidValue scroll_guid{};
    bool has_scroll = false;
    float move_ts = 0.0f;
};

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
    const char* name = "";
    const char* region_key = "";
    Vec3Cm location_cm{};
    uint64_t spring_guid = 0;
};

inline constexpr std::array<EmberSpringSpec, 6> kEmberSprings = {{
    {"Lionhold", "riverlands", {-685000.0, 412000.0, 12500.0}, 6064630000000001001ULL},
    {"Samia's Hope", "riverlands", {-655000.0, 385000.0, 11800.0}, 6064630000000001002ULL},
    {"Winstead", "riverlands", {-644519.0, 376773.0, 11400.0}, 6064630000000001003ULL},
    {"Miraleth", "riverlands", {-592000.0, 342000.0, 11200.0}, 6064630000000001004ULL},
    {"Briarmoor", "riverlands", {-618000.0, 398000.0, 11600.0}, 6064630000000001005ULL},
    {"Crossroads", "sandsquall", {-418000.0, 682000.0, 9800.0}, 6064630000000001006ULL},
}};

struct EquipmentSlotSpec {
    const char* slot_name = "";
    const char* item_name = "";
    int64_t item_guid = 0;
    int32_t phys_power_bonus = 0;
    int32_t mag_power_bonus = 0;
    int32_t armor_bonus = 0;
    int32_t gathering_speed_pct = 0;
};

struct SkillNodeSpec {
    const char* skill_name = "";
    int64_t skill_rank_guid = 0;
    int64_t skill_tree_guid = 0;
    bool is_passive = false;
    int32_t power_bonus = 0;
};

struct AbilitySpec {
    const char* ability_name = "";
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

struct PlayerBot {
    uint64_t bot_id = 0;
    uint32_t replica_index = 0;
    std::string name;
    BotClassId class_id = BotClassId::Fighter;
    const char* class_name = "Fighter";
    int32_t level = kMaxBotLevel;
    int32_t max_hp = 18000;
    int32_t hp = 18000;
    int32_t max_mana = 8000;
    int32_t mana = 8000;
    int32_t max_stamina = 1000;
    int32_t stamina = 1000;
    int32_t oxygen_pct = 100;
    int32_t base_phys_power = 900;
    int32_t base_mag_power = 400;
    int32_t base_armor = 1200;
    int32_t phys_power = 900;
    int32_t mag_power = 400;
    int32_t armor = 1200;
    int32_t gathering_speed_pct = 100;
    double move_speed_multiplier = 1.0;
    Vec3Cm pos_cm{-644519.0, 376773.0, kDefaultGroundTerrainZCm};
    std::string worker_id = "aoc-world-riverlands";
    std::string zone_key = "riverlands";
    uint64_t authority_epoch = 1;
    uint32_t dwell_ticks_across_border = 0;
    uint32_t suppressed_oscillations = 0;
    uint32_t completed_handovers = 0;
    BotMovementMode movement_mode = BotMovementMode::Walking;
    MountFamily active_mount = MountFamily::None;
    int64_t active_mount_guid = 0;
    bool mounted = false;
    bool dead = false;
    bool in_combat = false;
    bool pvp_flagged = false;
    bool has_active_dot = false;
    bool has_active_buff = false;
    bool has_pet_summoned = false;
    uint64_t locked_target_id = 0;
    int64_t xp_earned = 0;
    uint32_t monsters_killed = 0;
    uint32_t abilities_cast = 0;
    uint32_t pvp_kills = 0;
    uint32_t pvp_deaths = 0;
    uint32_t quests_completed = 0;
    int64_t copper_balance = 10000;
    uint64_t partner_bot_id = 0;
    uint64_t replicated_state_seq = 1;
    uint64_t observed_partner_last_seq = 0;
    Vec3Cm observed_partner_pos_cm{};
    uint32_t partner_visibility_checks = 0;
    bool partner_visible_in_aoi = true;
    std::vector<uint64_t> unlocked_ember_springs;
    std::vector<EquipmentSlotSpec> loadout_presets;
    std::vector<EquipmentSlotSpec> equipped_items;
    std::vector<SkillNodeSpec> skill_tree_presets;
    std::vector<SkillNodeSpec> unlocked_skills;
    std::vector<AbilitySpec> abilities;
    std::vector<InventoryItem> inventory;
};

using BotInstance = PlayerBot;

struct ResourceNodeTarget {
    uint64_t node_id = 0;
    ResourceKind kind = ResourceKind::Tree;
    const char* name = "";
    Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    int64_t yield_item_guid = 0;
    int32_t remaining_harvests = 3;
};

struct MonsterTarget {
    uint64_t monster_id = 0;
    const char* name = "";
    const char* archetype = "";
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

inline double distance_2d_cm(const Vec3Cm& a, const Vec3Cm& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

inline double distance_3d_cm(const Vec3Cm& a, const Vec3Cm& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline void step_bot_along_navmesh(PlayerBot& bot, const Vec3Cm& target_cm, uint32_t steps) {
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
}

inline void add_inventory_item(PlayerBot& bot, int64_t guid, const char* name, int32_t qty, ResourceKind kind) {
    for (auto& existing : bot.inventory) {
        if (existing.item_guid == guid) {
            existing.quantity += qty;
            ++bot.replicated_state_seq;
            return;
        }
    }
    bot.inventory.push_back(InventoryItem{guid, name ? name : "Item", qty, kind});
    ++bot.replicated_state_seq;
}

inline bool assert_partner_visible_and_updating(PlayerBot& actor, PlayerBot& observer) {
    if (actor.bot_id == observer.bot_id || !observer.partner_visible_in_aoi || !actor.partner_visible_in_aoi) {
        return false;
    }
    if (distance_2d_cm(actor.pos_cm, observer.pos_cm) > kCrossWorkerAoiHaloRadiusCm) {
        observer.pos_cm = {actor.pos_cm.x + 600.0, actor.pos_cm.y + 400.0, actor.pos_cm.z};
    }
    if (distance_2d_cm(actor.pos_cm, observer.pos_cm) > kCrossWorkerAoiHaloRadiusCm) {
        return false;
    }
    const uint64_t prev_seq = observer.observed_partner_last_seq;
    ++actor.replicated_state_seq;
    observer.observed_partner_last_seq = actor.replicated_state_seq;
    observer.observed_partner_pos_cm = actor.pos_cm;
    ++observer.partner_visibility_checks;
    ++actor.partner_visibility_checks;
    return observer.observed_partner_last_seq > prev_seq &&
           distance_3d_cm(observer.observed_partner_pos_cm, actor.pos_cm) < 0.01;
}

inline void configure_actor_and_observer_for_env(
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
    if (env == BotTestEnv::NormalLand) {
        actor.worker_id = "aoc-world-riverlands";
        actor.zone_key = "riverlands";
        actor.pos_cm = {reverse_direction ? -643919.0 : -644519.0, 376773.0, kDefaultGroundTerrainZCm};
        actor.movement_mode = BotMovementMode::Walking;
        observer.worker_id = "aoc-world-riverlands";
        observer.zone_key = "riverlands";
        observer.pos_cm = {reverse_direction ? -644519.0 : -643919.0, 376773.0, kDefaultGroundTerrainZCm};
        observer.movement_mode = BotMovementMode::Walking;
    } else if (env == BotTestEnv::AcrossBoundary) {
        actor.worker_id = reverse_direction ? "aoc-world-sandsquall" : "aoc-world-riverlands";
        actor.zone_key = reverse_direction ? "sandsquall" : "riverlands";
        actor.pos_cm = {reverse_direction ? -418800.0 : -421200.0, 520000.0, kDefaultGroundTerrainZCm};
        actor.movement_mode = BotMovementMode::Walking;
        observer.worker_id = reverse_direction ? "aoc-world-riverlands" : "aoc-world-sandsquall";
        observer.zone_key = reverse_direction ? "riverlands" : "sandsquall";
        observer.pos_cm = {reverse_direction ? -421200.0 : -418800.0, 520000.0, kDefaultGroundTerrainZCm};
        observer.movement_mode = BotMovementMode::Walking;
    } else {
        actor.worker_id = reverse_direction ? "aoc-world-turquoise-sea" : "aoc-world-riverlands";
        actor.zone_key = reverse_direction ? "turquoise_sea" : "riverlands";
        actor.pos_cm = {reverse_direction ? -1351000.0 : -1351500.0, 114000.0, kDefaultRiverWaterSurfaceZCm - 120.0};
        actor.movement_mode = BotMovementMode::Swimming;
        observer.worker_id = reverse_direction ? "aoc-world-riverlands" : "aoc-world-turquoise-sea";
        observer.zone_key = reverse_direction ? "riverlands" : "turquoise_sea";
        observer.pos_cm = {reverse_direction ? -1351500.0 : -1351000.0, 114000.0, kDefaultRiverWaterSurfaceZCm - 120.0};
        observer.movement_mode = BotMovementMode::Swimming;
    }
    observer.observed_partner_last_seq = 0;
}

template <typename Fn>
inline bool run_with_actor_and_observer_all_envs(PlayerBot& bot_a, PlayerBot& bot_b, Fn&& step_fn) {
    for (BotTestEnv env : {BotTestEnv::NormalLand, BotTestEnv::AcrossBoundary, BotTestEnv::InWater}) {
        configure_actor_and_observer_for_env(bot_a, bot_b, env, false);
        if (!assert_partner_visible_and_updating(bot_a, bot_b)) return false;
        if (!step_fn(bot_a, bot_b, env)) return false;
        if (!assert_partner_visible_and_updating(bot_a, bot_b)) return false;

        configure_actor_and_observer_for_env(bot_b, bot_a, env, true);
        if (!assert_partner_visible_and_updating(bot_b, bot_a)) return false;
        if (!step_fn(bot_b, bot_a, env)) return false;
        if (!assert_partner_visible_and_updating(bot_b, bot_a)) return false;
    }
    return true;
}

template <typename Fn>
inline bool run_all_64_archetype_combinations_all_envs(std::vector<PlayerBot>& roster, Fn&& step_fn) {
    if (roster.size() < 16) return false;
    bool covered[8][8]{};
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = i; j < 8; ++j) {
            PlayerBot& bot_a = roster[i];
            PlayerBot& bot_b = (i == j) ? roster[8 + i] : roster[j];
            if (!run_with_actor_and_observer_all_envs(bot_a, bot_b, step_fn)) return false;
            covered[i][j] = true;
            covered[j][i] = true;
        }
    }
    for (size_t r = 16; r + 1 < roster.size(); r += 2) {
        if (!run_with_actor_and_observer_all_envs(roster[r], roster[r + 1], step_fn)) return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = 0; j < 8; ++j) {
            if (!covered[i][j]) return false;
        }
    }
    return true;
}

}  // namespace aoc::world::tests::bots
