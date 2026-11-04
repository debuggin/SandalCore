// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/bootstrap.h"
#include "combat/aftermath.h"
#include "foliage/foliage.h"
#include "npc/area_npcs.h"
#include "protocol/server_guid_allocator.h"
#include "quest/engine.h"
#include "quest/quest_service.h"
#include "session/actor_view.h"
#include "session/position_saver.h"
#include "content/content_snapshot.h"
#include "spawns/gm_garden.h"
#include "spawns/node_populations.h"
#include "vendor/town_shops.h"
#include "warehouse/warehouse.h"
#include "world/node_control.h"
#include "world/node_scenery.h"
#include "world/places.h"
#include "world/town_floor.h"
#include "world/town_platform.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc { namespace worldd {

class GmPort;

struct MovementConfig {
    MoveLimits           limits;
    std::chrono::seconds save_interval{60};
};

class PlayerWorld {
public:
    PlayerWorld(const content::ContentDb* content, std::string content_problem, bootstrap::GameStateMode mode,
                uint32_t server_id, protocol::ServerGuidAllocator& guids);
    ~PlayerWorld();

    bool ready() const { return content_ != nullptr; }
    uint32_t runtime_catalog_schema_version = 16;
    bool     runtime_catalog_loaded = false;
    void     set_runtime_catalog_loaded(bool loaded, uint32_t schema_version = 16) {
        runtime_catalog_loaded = loaded;
        runtime_catalog_schema_version = schema_version;
    }
    const content::ContentDb* content() const { return content_; }
    const std::string& content_problem() const { return content_problem_; }
    std::shared_ptr<const content::ContentSnapshot> content_snapshot() const noexcept {
        return content::detail::atomic_load_ptr(&content_snapshot_);
    }
    bool reload_content_snapshot(const content::ContentDb& db, uint64_t version, std::string sha256_digest,
                                 std::string& error) {
        const auto active = content::detail::atomic_load_ptr(&content_snapshot_);
        if (active && version <= active->version) {
            error = "version regression: candidate version must exceed active version";
            return false;
        }
        auto next = content::ContentSnapshot::load_from_db(db, version, std::move(sha256_digest), error);
        if (!next) return false;
        const auto t0 = std::chrono::steady_clock::now();
        content::detail::atomic_exchange_ptr(&content_snapshot_, std::move(next));
        const auto t1 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double, std::milli>(t1 - t0).count() >= content::kMaxHotReloadSwapMs) {
            error = "content snapshot RCU swap exceeded 50ms budget";
            return false;
        }
        return true;
    }
    const content::AbilityRules* ability_rules() const { return rules_ ? &*rules_ : nullptr; }
    const items::ItemCatalog* item_catalog() const { return catalog_override_ ? catalog_override_ : catalog_ ? &*catalog_ : nullptr; }
    void set_item_catalog(const items::ItemCatalog* catalog) { catalog_override_ = catalog; }
    bootstrap::StatContext stat_context() const {
        return {bundle_ ? &*bundle_ : nullptr, gear_ ? &*gear_ : nullptr, stats::Rounding::HalfUp, curve_ ? &*curve_ : nullptr, curve_ ? &grants_ : nullptr,
                talents_ ? &*talents_ : nullptr, mounts_ ? &mounts_->data() : nullptr, professions_ ? &*professions_ : nullptr, weapon_lines_ ? &*weapon_lines_ : nullptr};
    }
    const leveling::LevelCurve*            level_curve() const { return curve_ ? &*curve_ : nullptr; }
    const std::vector<content::AutoGrant>& auto_grants() const { return grants_; }
    const content::LevelingType*           leveling_type() const { return leveling_type_ ? &*leveling_type_ : nullptr; }
    const talents::Catalog*                talents() const { return talents_ ? &*talents_ : nullptr; }
    const stats::StatBundle*               stat_bundle() const { return bundle_ ? &*bundle_ : nullptr; }
    const professions::Book*               professions() const { return professions_ ? &*professions_ : nullptr; }
    const weaponxp::Lines*                 weapon_lines() const { return weapon_lines_ ? &*weapon_lines_ : nullptr; }
    const items::CurrencyTable*            currencies() const { return currencies_ ? &*currencies_ : nullptr; }
    const abilities::Catalog* ability_catalog() const { return catalog_override_set_ ? ability_catalog_override_ : ability_catalog_ ? &*ability_catalog_ : nullptr; }
    void set_ability_catalog(const abilities::Catalog* catalog) {
        ability_catalog_override_ = catalog;
        catalog_override_set_ = true;
    }
    const spawns::MountBook* mounts() const { return mounts_ ? &*mounts_ : nullptr; }
    void set_mounts(spawns::MountBook book) { mounts_ = std::move(book); }
    bool load_mount_speeds(const std::string& path, std::string& error);
    bool load_mount_riders(const std::string& path, std::string& error);
    const world::RoadMap& roads() const { return roads_; }
    world::RoadMap&       roads() { return roads_; }
    bool load_roads(const std::string& path, std::string& error);
    bootstrap::GameStateMode gamestate_mode() const { return mode_; }
    uint32_t server_id() const { return server_id_; }
    const protocol::emit::FIntrepidNetworkGUID& game_state_guid() const { return game_state_; }
    protocol::emit::FIntrepidNetworkGUID mint_guid() const { return guids_.next(); }

    void                  set_movement(const MovementConfig& config) { movement_ = config; }
    const MovementConfig& movement() const { return movement_; }
    void          set_position_sink(PositionSink* sink) { sink_ = sink; }
    PositionSink* position_sink() const { return sink_; }

    bootstrap::SessionValues new_session(double world_time);

    world::ActorRegistry&       registry() { return registry_; }
    const world::ActorRegistry& registry() const { return registry_; }
    bool load_area(const spawns::AreaConfig& area, std::string& error);
    bool load_blueprints(const std::string& motion_path, std::string& error);
    bool load_npc_abilities(const std::string& abilities_path, const std::string& montages_path, const std::string& targeting_path, std::string& error);
    bool load_npc_senses(const std::string& senses_path, const std::string& curves_path, std::string& error);
    void set_generic_npcs(spawns::GenericConfig config) { generic_config_ = std::move(config); }
    void set_spawn_context(spawns::SpawnContext context) { spawn_context_ = std::move(context); }
    const spawns::SpawnContext& spawn_context() const { return spawn_context_; }
    const npc::MotionTable&     blueprints() const { return motion_; }
    const spawns::GenericNpcs*  generic_npcs() const { return generic_ ? &*generic_ : nullptr; }
    const spawns::AreaReport*   area_report() const { return area_report_ ? &*area_report_ : nullptr; }
    const respawn::ShrineBook*  shrines() const { return area_report_ ? &area_report_->shrines : nullptr; }
    const spawns::AreaConfig&   area() const { return area_; }
    const vendor::GmVendors*    gm_vendors() const { return gm_ ? &*gm_ : nullptr; }
    const dialogue::Book*       dialogue() const { return dialogue_ ? &*dialogue_ : nullptr; }
    void                        set_dialogue(dialogue::Book book) { dialogue_ = std::move(book); }
    const quest::QuestBook*     quests() const { return quests_ ? &*quests_ : nullptr; }
    void                        set_quests(quest::QuestBook book) { quests_ = std::move(book); }
    const content::ObjectData*  objects() const { return objects_ ? &*objects_ : nullptr; }
    bool                        prop_gone(uint64_t actor_id, double now_seconds) const;
    void                        tick_props(double now_seconds);
    quest::QuestWorker*         quest_worker() const { return quest_worker_; }
    void                        set_quest_worker(quest::QuestWorker* worker) { quest_worker_ = worker; }
    const vendor::TownShops*    town_shops() const { return shops_ ? &*shops_ : nullptr; }
    void                        set_town_shops(vendor::TownShops shops) { shops_ = std::move(shops); }
    const warehouse::Book*      warehouses() const { return warehouses_ ? &*warehouses_ : nullptr; }
    void                        set_warehouses(warehouse::Book book) { warehouses_ = std::move(book); }
    const content::CraftStationData* station_data() const { return stations_ ? &*stations_ : nullptr; }
    const content::CraftRecipeData*  craft_recipes() const { return recipes_ ? &*recipes_ : nullptr; }
    void load_crafting_data();

    enum class StationStatus : uint8_t { Ok, NotFound, Invalid, Refused };
    struct StationResult {
        StationStatus status = StationStatus::Ok;
        std::string   message;
        uint64_t      actor = 0;
        std::string   node;
        std::string   plot;
        std::string   station;
        std::string   building;
        double        x = 0, y = 0, z = 0;
    };
    StationResult place_station(const std::string& node, const std::string& plot, const std::string& station, const std::string& by);
    StationResult remove_station(const std::string& node, const std::string& plot, const std::string& by);
    size_t        placed_station_count() const { return placed_stations_.size(); }
    void                        set_gm_garden(bool on) { gm_garden_ = on; }
    bool                        gm_garden_requested() const { return gm_garden_; }
    void                        place_gm_garden();
    const spawns::GardenReport* garden_report() const { return garden_report_ ? &*garden_report_ : nullptr; }
    void                        set_gm_vendors(vendor::GmVendors gm) { gm_ = std::move(gm); }

    bool                     load_ground(const std::string& nav_path, const std::string& motion_path, std::string& error);
    bool                     adopt_ground(std::unique_ptr<nav::AreaNav> nav, std::string& error);
    const nav::AreaNav*      nav() const { return nav_.get(); }
    npc::NpcWorld*           npcs() { return npcs_.get(); }
    const npc::GroundReport* ground_report() const { return ground_report_ ? &*ground_report_ : nullptr; }
    double                   world_clock() const { return world_clock_; }
    void                     tick_world(double now_seconds);
    void                     note_player(uint64_t player_id, double x, double y);
    void                     forget_player(uint64_t player_id);
    uint64_t                 region_exits() const { return region_exits_; }
    bool                     outside_region(uint64_t player_id) const {
        const auto it = outside_.find(player_id);
        return it != outside_.end() && it->second;
    }
    world::AuthorityState    authority_state(uint64_t player_id) const noexcept {
        const auto it = authority_states_.find(player_id);
        return it != authority_states_.end() ? it->second : world::AuthorityState::Local;
    }
    world::AuthorityState    update_border_authority(uint64_t player_id, double x, double y) noexcept {
        const auto next = area_.evaluate_handover(x, y, authority_state(player_id));
        authority_states_[player_id] = next;
        return next;
    }
    uint64_t                 lease_epoch(uint64_t player_id) const noexcept {
        const auto it = lease_epochs_.find(player_id);
        return it != lease_epochs_.end() ? it->second : 0;
    }
    bool                     try_advance_lease_epoch(uint64_t player_id, uint64_t candidate_epoch) noexcept {
        if (!world::is_newer_lease_epoch(candidate_epoch, lease_epoch(player_id))) return false;
        lease_epochs_[player_id] = candidate_epoch;
        return true;
    }
    void                     forget_border_authority(uint64_t player_id) noexcept {
        authority_states_.erase(player_id);
        lease_epochs_.erase(player_id);
    }

    void set_combat_config(combat::CombatWorldConfig config) { combat_config_ = std::move(config); }
    const combat::CombatWorldConfig& combat_config() const { return combat_config_; }
    combat::CombatWorld* combat() { return combat_.get(); }

    void set_aftermath_config(combat::AftermathConfig config) { aftermath_config_ = std::move(config); }
    const combat::AftermathConfig& aftermath_config() const { return aftermath_config_; }
    void set_city_nodes(world::CityNodes* nodes) { city_nodes_ = nodes; }
    world::CityNodes* city_nodes() const { return city_nodes_; }
    void set_node_wire(const world::NodeWire* wire) { node_wire_ = wire; }
    const world::NodeWire* node_wire() const { return node_wire_; }
    void set_node_control(world::NodeControl* control) { node_control_ = control; }
    world::NodeControl* node_control() const { return node_control_; }
    void set_node_layout_data(const world::NodeLayoutData* data) { node_layout_data_ = data; }
    size_t start_scenery(const world::NodeScenery::Config& config);
    const world::NodeScenery* scenery() const { return scenery_.get(); }
    void set_node_population_data(const spawns::NodePopulationData* data) { node_pop_data_ = data; }
    size_t start_node_people(const spawns::NodePopulations::Config& config);
    const spawns::NodePopulations* node_people() const { return node_pops_.get(); }

    bool                     start_gathering(const gather::Config& config, std::string& error);
    gather::Gathering*       gathering() { return gathering_.get(); }
    const gather::Gathering* gathering() const { return gathering_.get(); }
    bool                     start_foliage(const foliage::Config& config, std::string& error);
    foliage::Foliage*        foliage() { return foliage_.get(); }
    const foliage::Foliage*  foliage() const { return foliage_.get(); }
    struct HarvestPileDrop {
        double                          x = 0, y = 0, z = 0;
        protocol::repl::GuidValue       owner{};
        std::string                     name;
        std::vector<combat::LootStack>  stacks;
    };
    uint64_t drop_harvest_pile(HarvestPileDrop drop, std::string& error);
    uint64_t drop_harvest_pile(double x, double y, double z, const protocol::repl::GuidValue& owner, const std::string& name, std::vector<combat::LootStack> stacks, std::string& error) {
        return drop_harvest_pile(HarvestPileDrop{x, y, z, owner, name, std::move(stacks)}, error);
    }
    using PileHook = std::function<uint64_t(double x, double y, double z, const protocol::repl::GuidValue& owner, const std::string& name, const std::vector<combat::LootStack>& stacks)>;
    void set_pile_hook(PileHook hook) { pile_hook_ = std::move(hook); }
    void set_road_default_level(int level) { road_default_level_ = level; }
    size_t apply_road_levels();
    combat::Aftermath*        aftermath() { return aftermath_.get(); }
    const combat::RewardBook* rewards() const { return rewards_ ? &*rewards_ : nullptr; }
    uint64_t respawn_npc(uint32_t spawn_ref, double now);
    struct RespawnSpot {
        double      x = 0, y = 0, z = 0, yaw = 0;
        bool        shrine = false;
        std::string name;
    };
    RespawnSpot respawn_spot(double from_x, double from_y, bool nearest_point = true) const;
    static double spot_height(const nav::AreaNav* nav, double x, double y, double data_z, bool* grounded = nullptr);
    double spawn_height(double x, double y, double data_z, bool* grounded = nullptr) const;
    int item_quality(int64_t item) const;
    int item_rarity(int64_t item, int quality) const;
    const items::ItemRarityFacts& item_rarity_facts(int64_t item) const;
    std::string item_name(int64_t item) const;

    void    set_gm_commands(bool on) { gm_commands_ = on; }
    bool    gm_commands() const { return gm_commands_; }
    void    set_reactions_enabled(bool on) { reactions_enabled_ = on; }
    bool    reactions_enabled() const { return reactions_enabled_; }
    void    set_dice_seed(uint64_t seed) { dice_seed_ = seed; }
    uint64_t dice_seed() const { return dice_seed_; }
    void    set_gm_port(GmPort* port) { gm_port_ = port; }
    GmPort* gm_port() const { return gm_port_; }
    world::Places places() const;
    std::optional<double> ground_at(double x, double y) const;

    // Baked terrain under the five city bowls is ~50.6 m below the town platform; kBowlSlackCm prevents spawning under the floor.
    static constexpr double kBowlSlackCm = 200.0;
    world::TownFloor& town_floor() const;
    struct TownGround {
        int64_t node = 0;
        double  z = 0;
        bool    platform = false;
        bool    raised = false;
    };
    std::optional<TownGround> town_ground_at(double x, double y) const;
    std::optional<TownGround> bowl_floor_at(double x, double y) const;

    static constexpr double kFloorReachCm = 150.0;
    static constexpr double kNearWallCm = 100.0;
    std::vector<double>       floors_at(double x, double y, double z_lo, double z_hi) const;
    std::optional<double>     floor_under(double x, double y, double z, double below, double above = kFloorReachCm) const;
    std::optional<TownGround> town_ground_at(double x, double y, double z) const;
    struct BowlLift {
        int64_t node = 0;
        double  z = 0;
        double  under_cm = 0;
    };
    std::optional<BowlLift> lift_out_of_bowl(double x, double y, double z) const;
    bool load_town_platforms(const std::string& path, std::string& error);
    void set_town_platforms(std::unique_ptr<world::TownPlatforms> platforms) { platforms_ = std::move(platforms); }
    const world::TownPlatforms* town_platforms() const { return platforms_.get(); }
    static constexpr double kPlatformUnderCm = 2500.0;
    static constexpr double kPitStandCm = 300.0;
    static constexpr double kKillPlaneCm = -10000.0;
    static constexpr double kBelowFloorCm = 10000.0;
    struct FloorRescue {
        double      x = 0, y = 0, z = 0;
        int64_t     node = 0;
        std::string where;
    };
    std::optional<FloorRescue> floor_rescue(double x, double y, double z) const;
    static constexpr double kCapsuleHalfHeightCm = 98.0;
    static constexpr double kPlaceLiftCm = 150.0;
    double place_height(const world::Place& place) const;

    void                     set_view_config(const ActorView::Config& config) { view_config_ = config; }
    const ActorView::Config& view_config() const { return view_config_; }
    void                     set_move_ack_gap(std::chrono::milliseconds gap) { move_ack_gap_ = std::chrono::duration_cast<Clock::duration>(gap); }
    Clock::duration          move_ack_gap() const { return move_ack_gap_; }

    struct WindowCost {
        double max_interest_ms = 0, max_pump_ms = 0, max_ground_ms = 0;
        double max_npcs_ms = 0, max_combat_ms = 0, max_aftermath_ms = 0;
        double max_people_ms = 0;
        size_t grounded_chunks = 0;
    };
    const WindowCost& window_cost() const { return window_cost_; }

private:
    bool init_core_tables(std::string& problem);
    bool init_progression_tables(std::string& problem);
    void init_optional_tables();
    void reset_on_content_error(const std::string& problem);
    void load_area_quests_and_props();
    void load_area_economy_and_looks();
    void log_area_loaded() const;
    void log_npc_types() const;
    void log_huntables() const;
    void log_hunting_kinds();
    void refresh_town_floor() const;
    void preload_window();
    void check_ground_covers_area() const;
    void drive_window(double now_seconds);
    void log_window_errors_and_repairs(const nav::WindowStats& w);
    void log_window(double now_seconds, const nav::WindowStats& w);
    void check_town_node(int64_t node, const spawns::NodePopulations::Entry& entry, double now_seconds);
    void check_towns(double now_seconds);
    void tick_town_people_and_roads(double now_seconds);
    bool station_placement_ready() const;
    const world::PlotRow* find_free_station_plot(const world::CityNodeInfo& node, int level, const std::string& plot_name,
                                                 const std::vector<world::PlotRow>& rows, StationResult& out) const;
    void describe_station(uint64_t id, const world::CityNodeInfo& node, int64_t plot, int64_t building = 0);
    void finish_station(uint64_t id, const world::CityNodeInfo& node, int64_t plot, int64_t building = 0);
    void reap_placed_stations();
    void setup_node_shopkeeper(uint64_t id, const world::CityNodeInfo& node);
    void on_node_person_spawned(uint64_t id, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route,
                                const world::CityNodeInfo& node, int64_t plot);
    void start_combat();
    std::set<int64_t> collect_reward_roots(size_t& quest_tables) const;
    void init_reward_book();
    void start_aftermath();
    void log_spawn_filter() const;
    npc::SpeedModel speeds() const;

    struct TownCheck {
        int    level = -1;
        size_t people = 0, ready = 0;
        double changed_at = 0;
        double logged_at = -1.0e9;
        int    logged_level = -1;
        bool   logged = true;
    };
    std::map<int64_t, TownCheck> town_checks_;
    double                       last_town_check_ = -1.0e9;
    static constexpr double kTownCheckSeconds = 2.0, kTownSettleSeconds = 4.0, kTownRelogSeconds = 60.0;

    WindowCost window_cost_;
    static constexpr double kWindowBudgetMs = 3.0;
    // Window is driven at 20 Hz rather than per network datagram.
    static constexpr double kWindowPeriodSeconds = 0.045;
    static constexpr double kWindowLogSeconds = 30.0;
    double last_window_drive_ = -1.0e9;
    double last_window_log_ = -1.0e9;

    const content::ContentDb*                  content_;
    std::string                                content_problem_;
    mutable std::shared_ptr<const content::ContentSnapshot> content_snapshot_;
    std::optional<content::AbilityRules>       rules_;
    std::optional<items::ContentItemCatalog>   catalog_;
    std::optional<stats::StatBundle>           bundle_;
    std::optional<content::ContentGearCatalog> gear_;
    std::optional<content::LevelingType>       leveling_type_;
    std::optional<leveling::LevelCurve>        curve_;
    std::vector<content::AutoGrant>            grants_;
    std::optional<talents::Catalog>            talents_;
    std::optional<professions::Book>           professions_;
    std::optional<weaponxp::Lines>             weapon_lines_;
    std::optional<items::CurrencyTable>        currencies_;
    std::optional<abilities::Catalog>          ability_catalog_;
    std::optional<spawns::MountBook>           mounts_;
    world::RoadMap                             roads_;
    const abilities::Catalog*                  ability_catalog_override_ = nullptr;
    bool                                       catalog_override_set_ = false;
    const items::ItemCatalog*                  catalog_override_ = nullptr;
    bootstrap::GameStateMode                   mode_;
    uint32_t                                   server_id_;
    protocol::ServerGuidAllocator&             guids_;
    protocol::emit::FIntrepidNetworkGUID       game_state_;
    int32_t                                    next_player_id_ = 1;
    MovementConfig                             movement_;
    PositionSink*                              sink_ = nullptr;
    world::ActorRegistry                       registry_;
    spawns::AreaConfig                         area_;
    std::optional<spawns::CapturedData>        captured_;
    std::optional<vendor::GmVendors>           gm_;
    std::optional<spawns::AreaReport>          area_report_;
    bool                                       gm_garden_ = false;
    std::optional<spawns::GardenReport>        garden_report_;
    ActorView::Config                          view_config_;
    Clock::duration                            move_ack_gap_{};
    std::unique_ptr<nav::AreaNav>              nav_;
    npc::MotionTable                           motion_;
    bool                                       motion_loaded_ = false;
    spawns::GenericConfig                      generic_config_;
    spawns::SpawnContext                       spawn_context_;
    std::optional<spawns::SpawnFilter>         spawn_filter_;
    std::optional<spawns::GenericNpcs>         generic_;
    std::unique_ptr<spawns::NpcLooks>          looks_;
    std::set<std::string>                      standing_archetypes_;
    std::string                                looks_dir_;
    std::unique_ptr<npc::NpcWorld>             npcs_;
    std::optional<npc::GroundReport>           ground_report_;
    combat::CombatWorldConfig                  combat_config_;
    combat::NpcAbilityData                     npc_abilities_;
    npc::SensesTable                           npc_senses_;
    std::optional<combat::NpcStatBook>         npc_book_;
    std::optional<combat::CombatData>          combat_data_;
    // Declaration order preserves teardown safety: aftermath_ and node_pops_ outlive nothing they reference in combat_/npcs_.
    std::unique_ptr<combat::CombatWorld>       combat_;
    combat::AftermathConfig                    aftermath_config_;
    std::optional<combat::RewardBook>          rewards_;
    std::unique_ptr<combat::Aftermath>         aftermath_;
    world::CityNodes*                          city_nodes_ = nullptr;
    const world::NodeWire*                     node_wire_ = nullptr;
    world::NodeControl*                        node_control_ = nullptr;
    std::optional<dialogue::Book>              dialogue_;
    std::optional<quest::QuestBook>            quests_;
    std::optional<content::ObjectData>         objects_;
    struct PropSlot {
        uint64_t actor = 0;
        double   due = 0;
    };
    mutable std::vector<PropSlot>              prop_slots_;
    mutable size_t                             props_gone_ = 0;
    quest::QuestWorker*                        quest_worker_ = nullptr;
    std::optional<vendor::TownShops>           shops_;
    std::optional<warehouse::Book>             warehouses_;
    std::optional<content::CraftStationData>   stations_;
    std::optional<content::CraftRecipeData>    recipes_;
    std::map<std::pair<int64_t, int64_t>, uint64_t> placed_stations_;
    uint64_t                                   placed_seen_version_ = 0;
    const world::NodeLayoutData*               node_layout_data_ = nullptr;
    std::unique_ptr<world::NodeScenery>        scenery_;
    const spawns::NodePopulationData*          node_pop_data_ = nullptr;
    std::unique_ptr<spawns::NodePopulations>   node_pops_;
    std::unique_ptr<gather::Gathering>         gathering_;
    std::unique_ptr<foliage::Foliage>          foliage_;
    PileHook                                   pile_hook_;
    int                                        road_default_level_ = 0;
    uint64_t                                   roads_seen_version_ = ~0ull;
    std::unique_ptr<world::TownPlatforms>      platforms_;
    mutable world::TownFloor                   town_floor_;
    mutable uint64_t                           town_floor_stamp_ = ~0ull;
    bool                                       gm_commands_ = false;
    bool                                       reactions_enabled_ = true;
    GmPort*                                    gm_port_ = nullptr;
    uint64_t                                   respawns_made_ = 0;
    uint64_t                                   dice_seed_ = 0;
    mutable std::map<int64_t, int>             quality_cache_;
    mutable std::map<int64_t, items::ItemRarityFacts> rarity_cache_;
    mutable std::map<int64_t, std::string>     name_cache_;
    double                                     world_clock_ = 0;
    std::unordered_map<uint64_t, bool>         outside_;
    std::unordered_map<uint64_t, double>       outside_logged_at_;
    std::unordered_map<uint64_t, bool>         exit_loud_;
    static constexpr double kRegionLogSeconds = 30.0;
    uint64_t                                   region_exits_ = 0;
    std::unordered_map<uint64_t, world::AuthorityState> authority_states_;
    std::unordered_map<uint64_t, uint64_t>     lease_epochs_;
    uint64_t                                   window_failures_logged_ = 0;
    nav::TileRepair                            window_repairs_logged_;
};

}}  // namespace aoc::worldd
