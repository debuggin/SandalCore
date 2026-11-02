#pragma once

#include "session/player_session_state.h"
#include "crafting/job.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/node_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "quest/quest_wire.h"

namespace aoc {
namespace worldd {

class PlayerSession : public PlayerSessionState {
  friend class CombatSide;
  friend struct SessionInternal;

 public:
    ~PlayerSession();

    static std::unique_ptr<PlayerSession> join(Connection& conn, PlayerWorld& world, const svc::Character& character,
                                               const bootstrap::LoginIdentity& login, double world_time,
                                               std::string& error, bootstrap::SetupRefusal* refusal = nullptr);
    void tick(Connection& conn);

    void tick(Connection& conn, Clock::time_point now);

    void finish(const char* why);

    void on_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now);

    void end_of_datagram(Connection& conn);

    void take_acks(Connection& conn);
    std::vector<std::string> weapon_tags() const;
    bool set_current_value(int64_t record, float value);
    double take_damage(Connection& conn, double amount, Clock::time_point now);
    void take_hit(Connection& conn, const combat::IncomingHit& hit, Clock::time_point now);
    void teach_ability(int64_t ability);
    GoodPosition last_position() const;
    uint32_t     mount_channel() const;
    crafting::Ratings artisan_ratings(int64_t profession) const;
    size_t   gm_finish_jobs(Connection& conn, Clock::time_point now, const std::string& station_name, const std::string& by);
    bool     gm_teach_recipe(Connection& conn, int64_t recipe, const std::string& by);
    bool     gm_forget_recipe(Connection& conn, int64_t recipe, const std::string& by);
    size_t   gm_reset_tutorials(Connection& conn, const std::string& by);
    void     construction_crate_equipped(int64_t item_record);
    void     construction_deposit(int64_t node, int64_t building_or_project, int64_t item_record, int32_t count);
    GmMoveResult gm_teleport(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by);
    void   run_floor_net(Connection& conn, Clock::time_point now);
    GmSpeedChange gm_set_speed(Connection& conn, double multiplier, Clock::time_point now, const std::string& by);
    GmMoneyResult gm_give_money(Connection& conn, int64_t copper, Clock::time_point now, const std::string& by);
    GmWeaponResult gm_set_weapon_xp(Connection& conn, int64_t leveling_type, int64_t xp, Clock::time_point now, const std::string& by);
    GmItemResult gm_give_item(Connection& conn, int64_t item_record, int32_t count, Clock::time_point now, const std::string& by, const std::string& rarity = std::string(),
                              int32_t quality = 0);

 private:
  using PlayerSessionState::PlayerSessionState;
    bool allocate_channels(const Connection& conn);
    void pump(Connection& conn);
    Outcome send(Connection& conn, bootstrap::Send& send);
    void queue(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build,
               Connection& conn, bool must_build = false);
    void queue_later(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build, Connection& conn,
                     Clock::time_point now, bool must_build = false);
    bool room_for(size_t steps);
    bool within_budget(RequestBudget& budget, const char* what, Clock::time_point now);
    static spdlog::level::level_enum level_for(size_t& count, size_t limit = kLoggedPerKind);
    void dispatch(Connection& conn, bootstrap::Role role, const protocol::repl::ClientRpc& rpc, const InBunch& bunch,
                  Clock::time_point now);
    std::unique_ptr<dialogue::Facts> dialogue_facts(const world::WorldActor* npc) const;
    bool open_dialogue(Connection& conn, const world::WorldActor& npc, Clock::time_point now);
    void on_dialogue_choice(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    bool npc_offers_quests(const world::WorldActor& npc) const;
    bootstrap::PlayerBootstrap::DialogueView dialogue_view(bool exhaust = false) const;
    void send_dialogue(Connection& conn, const char* why, bool exhaust = false);
    void init_quests(PlayerWorld& world, const svc::Character& character);
    void quest_sync(Connection& conn, const char* why);
    void run_quests(Connection& conn, Clock::time_point now);
    void start_quest(Connection& conn, const content::QuestRow& def, int64_t source, const char* why, Clock::time_point now);
    void on_quest_offer(Connection& conn, uint8_t response, Clock::time_point now);
    void on_item_delivery(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_abandon_quests(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_quest_debug_rpc(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void apply_quest_command(Connection& conn, const svc::QuestCommand& cmd, Clock::time_point now);
    bool end_quest(Connection& conn, int64_t record, const char* kind, Clock::time_point now);
    void remove_quest_items(Connection& conn, int64_t quest, Clock::time_point now);
    void end_dialogue(Connection& conn, const char* why);
    void run_dialogue(Connection& conn, Clock::time_point now);
    double quest_clock(Clock::time_point now);
    void quest_engine(Connection& conn, Clock::time_point now, const char* why, const std::function<void(quest::Engine&)>& fn);
    void apply_quest_effects(Connection& conn, std::vector<quest::Effect>& effects, Clock::time_point now);
    void grant_quest_rewards(Connection& conn, const std::vector<int64_t>& tables, const char* why, Clock::time_point now, int64_t quest = 0);
    bool deliver_quest_rewards(Connection& conn, const std::vector<int64_t>& tables, const char* why, Clock::time_point now, int64_t quest = 0);
    void take_quest_items(Connection& conn, const std::vector<std::pair<int64_t, int64_t>>& items);
    void quest_toast(Connection& conn, const std::string& text, Clock::time_point now);
    void run_talk_actions(Connection& conn, const dialogue::Result& r, const world::WorldActor& npc, Clock::time_point now);
    void quest_kill(Connection& conn, const combat::KillReward& k, Clock::time_point now);
    void quest_shrine(Connection& conn, int64_t place, Clock::time_point now);
    void tick_quests(Connection& conn, Clock::time_point now);
    void tick_quest_givers(Connection& conn, Clock::time_point now);
    void log_quest_markers(const quest::MarkerContext& markers);
    bool carry_quest_crate(Connection& conn, const combat::RolledItem& crate, Clock::time_point now);
    void list_item_chunk(Connection& conn);
    bool open_warehouse(Connection& conn, const world::WorldActor& npc, int access_config, Clock::time_point now);
    void finish_open_warehouse(Connection& conn, Clock::time_point now, bool clear);
    void send_dialogue_later(Connection& conn, Clock::time_point now);
    void on_warehouse_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_warehouse_close(Connection& conn);
    void close_warehouse(Connection& conn, const char* why);
    void run_warehouse(Connection& conn, Clock::time_point now);
    bool warehouse_event(Connection& conn, const protocol::repl::ItemStorageEvent& event, Clock::time_point now);
    bool warehouse_payload(Connection& conn, const protocol::emit::FIntrepidNetworkGUID& container, Clock::time_point now);
    void refuse_warehouse_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now);
    void note_unserved_blocks(bootstrap::Role role, const protocol::repl::ClientBunch& parsed, const InBunch& bunch);
    void on_move(Connection& conn, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount);
    bool position_changed(bool with_yaw) const;
    void hand_over(const char* why, bool periodic);
    void on_combat_settings(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    AbilityRuntime* ability_runtime(Clock::time_point now);
    double ability_seconds(Clock::time_point now) const;
    void on_ability_trigger(Connection& conn, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount = false);
    void on_ability_cancel(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_branch_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_hit_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_ability_end(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void run_abilities(Connection& conn, Clock::time_point now);
    void take_ability_events();
    void sync_ability_values();
    void abilities_off(const std::string& why);
    bool dash_allowed(Clock::time_point now) const;
    void flush_abilities(Connection& conn);
    bool is_moving(const GoodPosition& before, const protocol::repl::NetworkMove& move) const;
    void on_storage_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_item_used(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_storage_event_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void apply_gear_stats(const char* what = "the new gear");
    void on_purchase(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void refuse_purchase(Connection& conn, const protocol::repl::SkillBuild& build, const std::string& why, Clock::time_point now);
    void on_set_slot(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_ranged(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void put_weapon_in_hand(int64_t combo, int64_t line, const std::string& line_name);
    void follow_worn_weapon();
    void grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now, bootstrap::AbilityChange& effects,
                     std::vector<protocol::repl::SkillOnActionbar>& updates);
    void add_rank_effect(const talents::RankRef& ref, Clock::time_point now, bootstrap::AbilityChange& effects);
    void remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects);
    void refresh_rank_mods();
    void switch_class(const content::ClassRecord& cls, Clock::time_point now, bootstrap::AbilityChange& effects,
                      std::vector<protocol::repl::SkillOnActionbar>& updates, std::vector<std::pair<int, int>>& cleared);
    void run_timers(Connection& conn, Clock::time_point now);
    protocol::repl::ActionbarPayload bar_payload() const;
    void rebuild_ability_lists();
    bool push_abilities();
    void sync_item_abilities(Connection& conn);
    double session_seconds(Clock::time_point now);
    void on_item_activated(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now);
    void on_tier_up(Connection& conn, const protocol::repl::ItemUse& use, svc::ItemContainer container, Clock::time_point now);
    void finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects,
                         std::vector<protocol::repl::SkillOnActionbar>& skill_updates);
    double xp_add_mult() const;
    std::vector<professions::Change> set_professions(const professions::Professions& after);
    std::vector<ProfessionWire> profession_wire(const std::vector<professions::Change>& changes) const;
    void queue_profession_level_ups(Connection& conn, const std::vector<professions::Change>& changes);
    std::vector<int64_t> equipped_weapon_lines() const;
    const talents::Tree* weapon_tree_of(int64_t leveling_type) const;
    bool apply_weapon_levels(const std::vector<weaponxp::Change>& changes, Clock::time_point now);
    void hand_over_progress(const char* why);
    void on_remove_ftues(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_active_target(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_hover_target(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_pvp_flag(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_close_loot(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_slots(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_map_marker(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    std::string fresh_guid();
    void apply_outcome(Connection& conn, const items::Outcome& outcome, const char* what);
    void note_cargo_crate(const items::Outcome& outcome);
    void sync_cargo(Connection& conn, Clock::time_point when);
    void run_cargo(Connection& conn, Clock::time_point now);
    void drop_crate(Connection& conn, Clock::time_point now);
    void update_resource_container(Connection& conn, const items::BagChange& change);
    void refuse_item_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now);
    void hand_over_inventory(const char* why, bool periodic);
    void on_client_load(Connection& conn, Clock::time_point now);
    void on_interact(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_station_interact(Connection& conn, const world::WorldActor& station, Clock::time_point now);
    void on_start_craft(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_processing_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_quick_start(Connection& conn, const protocol::repl::StartCraft& req, const world::WorldActor& station, Clock::time_point now);
    void on_interrupt_station_craft(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void run_quick_make(Connection& conn, Clock::time_point now);
    bool cancel_quick_make(Connection* conn, const std::string& why);
    bool complete_quick_make(Connection& conn, Clock::time_point now);
    bool cargo_crate_made(const std::vector<crafting::Product>& made) const;
    void say(Connection& conn, const char* text, Clock::time_point now);
    void run_crafting(Connection& conn, Clock::time_point now);
    int64_t next_job_event(int64_t now_ms) const;
    std::optional<crafting::JobRewards> job_rewards(const svc::ProcessingJob& job) const;
    double artisan_stat(int64_t stat) const;
    const world::WorldActor* station_by_key(const std::string& key) const;
    static int64_t wall_ms();
    double         sync_now(Clock::time_point now);
    bool sync_active_stations(int64_t now_ms);
    void on_vendor_purchase(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_open_vendor(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    bool open_talk_shop(Connection& conn, const world::WorldActor& npc, int64_t def, Clock::time_point now);
    void on_vendor_transaction(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_node_tax_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_node_summary_request(const protocol::repl::ClientRpc& rpc);
    void on_siege_summary_request(const protocol::repl::ClientRpc& rpc);
    void on_siege_cooldowns_request();
    void on_citizenship_window_request();
    void on_gm_node_rpc(const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void run_nodes(Connection& conn);
    void note_node_request(std::deque<int64_t>& wait, std::set<int64_t>& queued, int64_t node, const char* what);
    void on_gm_teleport_xyz(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_teleport_key(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_teleport_player(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    bool gm_rpc_admitted(const char* rpc_name, const std::string& params, Clock::time_point now);
    void apply_gm_speed(Connection* conn, Clock::time_point now);
    double gm_limit_scale(Clock::time_point now) const;
    void   scale_pawn_limits(Clock::time_point now);
    void on_mount_item_used(const svc::InventoryItem& item);
    void on_summon_trigger(Connection& conn, int32_t client_instance, int32_t slot, uint32_t location, Clock::time_point now);
    void on_desummon_mount(Connection& conn, Clock::time_point now);
    void run_mounts(Connection& conn, Clock::time_point now);
    void apply_mount_effects(Connection& conn, MountController::Effects&& fx);
    void mount_damaged(Clock::time_point now);
    void on_mount_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now);
    void dispatch_mount(Connection& conn, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now);
    void on_mount_interact(Connection& conn, const world::WorldActor& mount, Clock::time_point now);
    bool mount_up(Connection& conn, const world::WorldActor& mount, Clock::time_point now);
    void dismount(Connection& conn, Clock::time_point now, DismountWhy why);
    std::optional<double> ground_below(double x, double y, double z) const;
    void on_dismount_request(Connection& conn, Clock::time_point now);
    void run_ride(Connection& conn, Clock::time_point now);
    bool mounted_trigger(Connection& conn, int64_t ability, int32_t instance, bool on_mount, Clock::time_point now);
    void end_instance(Connection& conn, int32_t instance, bool on_mount);
    void hit_on_mount(Connection& conn, const combat::IncomingHit& hit, Clock::time_point now);
    void mount_died(Connection& conn, Clock::time_point now);
    void apply_persist(Connection* conn, const MountController::Persist& p);
    std::optional<bootstrap::Send> mount_ack(float time_stamp);
    std::optional<bootstrap::Send> mount_correction(float time_stamp, const GoodPosition& good);
    void on_gather_interact(Connection& conn, const world::WorldActor& node, Clock::time_point now);
    void on_hunt_interact(Connection& conn, const world::WorldActor& corpse, Clock::time_point now);
    void begin_channel(Connection& conn, const world::WorldActor& target, const content::GatherDefRow& def, const std::string& what, const char* tag, const char* noun, bool hunt, bool gives_nothing,
                       Clock::time_point now);
    void on_interrupt_channel(Connection& conn, Clock::time_point now);
    void on_foliage_interact(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void run_gather(Connection& conn, Clock::time_point now);
    bool complete_gather(Connection& conn, Clock::time_point now);
    static const char* interrupt_key(Interrupt k);
    void cancel_gather(Connection* conn, const std::string& why, Interrupt interrupt = Interrupt::None);
    void cancel_foliage(Connection* conn, const std::string& why, Interrupt interrupt = Interrupt::None);
    void unlock_foliage_client(Connection& conn, uint32_t index, const std::string& owner, const std::string& component);
    void run_foliage(Connection& conn, Clock::time_point now);
    bool complete_foliage(Connection& conn, Clock::time_point now);
    void send_foliage_data(Connection& conn, const bootstrap::PlayerBootstrap::FoliageChannel& data);
    void send_interrupt_callout(Connection& conn, Interrupt key, Clock::time_point now);
    void send_economy_callout(Connection& conn, const std::string& message_key, Clock::time_point now);
    void deliver_harvest(Connection& conn, Clock::time_point now, const HarvestFrame& frame);
    void send_channeling(Connection& conn, const protocol::repl::ChannelingData& data);
    std::unique_ptr<stats::CharacterStats::Evaluation> gather_run(std::string& why);
    void open_loot(Connection& conn, const combat::Corpse& corpse, const world::WorldActor& actor, Clock::time_point now);
    void on_loot_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void check_death(Connection& conn, Clock::time_point now);
    void die(Connection& conn, Clock::time_point now);
    void on_respawn_choices_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_respawn_choice(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    const respawn::ShrineBook* shrine_book() const;                                   // the area's Emberspring (null: none)
    void init_attunements(const svc::Character& character);                           // the join: the saved places on the pawn
    void send_attunements(Connection& conn, Clock::time_point now);                   // the places changed: the pawn's update, and saved with the progress
    void run_embersprings(Connection& conn, Clock::time_point now);                   // discovery: a shrine whose radius holds the player
    void on_shrine_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now);   // using a shrine: the prompt to make it the home
    void on_prop_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now);     // quests part 3: using a quest object (a body, a crate, a root, a site)
    void run_props(Connection& conn, Clock::time_point now);                                         // ... a use that takes time ends (or is cancelled by a step away)
    void finish_prop_use(Connection& conn, uint64_t actor_id, Clock::time_point now);                // ... the object answers, gives what it gives, counts for the quests, leaves
    void on_home_attunement(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);   // RequestServerSetHomeAttunement(bYes)
    void send_respawn_choices(Connection& conn, bool previous_invalid, bool home_on_cooldown);
    void on_plain_respawn(Connection& conn, Clock::time_point now);                   // the Respawn RPC: the window listed nothing
    void finish_respawn(Connection& conn, const PlayerWorld::RespawnSpot& spot, Clock::time_point now);
    void apply_kill(Connection& conn, const combat::KillReward& kill, Clock::time_point now);
    const world::WorldActor* open_vendor(std::string& why);
    int64_t town_shop_of(const world::WorldActor& npc) const;
    void buy_from_town_shop(Connection& conn, const protocol::repl::VendorPurchaseCart& cart, Clock::time_point now);
    void sell_to_town_shop(Connection& conn, const protocol::repl::VendorTransactionCart& cart, Clock::time_point now);
    void repair_at_town_shop(Connection& conn, const protocol::repl::VendorTransactionCart& cart, Clock::time_point now);
    void add_item_toasts(std::vector<bootstrap::ItemEvent>& events, bool removals) const;
    const protocol::schema::ClassSchema* class_of(bootstrap::Role role) const;
    bool role_of_channel(uint32_t channel, bootstrap::Role* role) const;
    void fail(std::string why);
    const items::ItemCatalog& item_catalog() const;
    void     flush_zones(Connection& conn);
    void drain_combat(Connection& conn, Clock::time_point now);
    void leave_combat();
    std::vector<uint64_t> actors_of(const std::vector<protocol::emit::FIntrepidNetworkGUID>& objects,
                                    const std::vector<protocol::repl::InlineObject>& inline_objects = {}) const;
    bool own_inline_object(const protocol::repl::InlineObject& o) const;
    GmMoveResult move_to(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by, bool gm);
    void log_position(Clock::time_point now);
    void tally_speed(const GoodPosition& before, const GoodPosition& after, bool sprinting, bool evading);
};

std::vector<bootstrap::ItemEvent> item_events_for(const items::Outcome& outcome);

struct PlayerHandle {
    PlayerSession* player = nullptr;
    Connection*    connection = nullptr;
    explicit operator bool() const { return player != nullptr && connection != nullptr; }
};

}  // namespace worldd
}  // namespace aoc
