#pragma once

#include "session/player_session_types.h"
#include "crafting/job.h"
#include "dialogue/talk.h"
#include "protocol/repl/node_rpc.h"
#include "quest/quest_wire.h"
#include "session/ability_pipeline.h"
#include "session/action_bar.h"

namespace aoc {
namespace worldd {

class PlayerSession {
  friend class CombatSide;
  friend struct SessionInternal;
  friend struct BasedMoveInternal;
  friend struct PetsInternal;

 public:
  class CombatSide;
  struct SessionInternal;
    using GmMoveResult = ::aoc::worldd::GmMoveResult;
    using GmSpeedChange = ::aoc::worldd::GmSpeedChange;
    using GmMoneyResult = ::aoc::worldd::GmMoneyResult;
    using GmWeaponResult = ::aoc::worldd::GmWeaponResult;
    using GmItemResult = ::aoc::worldd::GmItemResult;
    using BasedPlace = ::aoc::worldd::BasedPlace;
    using BaseKey = ::aoc::worldd::BaseKey;
    using BaseTrack = ::aoc::worldd::BaseTrack;
    using HeldMoveAck = ::aoc::worldd::HeldMoveAck;
    using TickPlan = ::aoc::worldd::TickPlan;

    static constexpr size_t kMaxPendingReplies = 64, kMaxPendingConfirms = 256, kMaxZoneOps = 64, kConfirmsPerCall = 20, kBasedHeldCap = 120;
    static constexpr uint32_t kLoadBurst = 4, kStorageBurst = 16, kAbilityBurst = 8, kHitBurst = 40, kCancelBurst = 16;
    static constexpr uint32_t kTalentBurst = 8, kBarBurst = 16, kBarPurchaseAllowance = 64, kItemBurst = 8, kResyncBurst = 4;
    static constexpr uint32_t kRideBurst = 8, kVendorBurst = 8, kTaxBurst = 3, kLootBurst = 8, kQuietBurst = 16;
    static constexpr std::chrono::seconds kLoadInterval{2}, kResyncInterval{2}, kPositionLogInterval{30}, kAbilityLogWindow{60};
    static constexpr std::chrono::seconds kTimeSyncAfterPhaseIn{1}, kTimeSyncSecondAfterPhaseIn{5}, kTimeSyncInterval{120};
    static constexpr std::chrono::milliseconds kStorageInterval{100}, kAbilityInterval{100}, kHitInterval{25}, kCancelInterval{50};
    static constexpr std::chrono::milliseconds kTalentInterval{100}, kBarInterval{100}, kPurchaseReplyDelay{84}, kItemInterval{100};
    static constexpr std::chrono::milliseconds kRideInterval{100}, kCrateDropDelay{694}, kVendorInterval{100}, kTaxInterval{1000};
    static constexpr std::chrono::milliseconds kLootInterval{100}, kQuietInterval{50};
    static constexpr int64_t kMaxRegenSteps = 5, kFirstPassiveEffectId = 1000000, kRespawnRecord = 6064632337807835941ll;
    static constexpr double kMovingSpeed = 30.0, kInteractRangeCm = 2500.0, kNetHoldSeconds = 1.0, kNetRepeatSeconds = 3.0;
    static constexpr size_t kLoggedPerKind = 3, kLoggedMounts = 40, kLoggedAbilityTransitions = 100;
    static constexpr int kDropCrateBank = 2, kDropCrateSlot = 11;
    static constexpr uint32_t kNoInstance = 0xFFFFFFFFu;
    static constexpr size_t kMaxLootTakes = 16, kMaxMultiSlots = 64, kMaxNodeRequests = 256;
    static constexpr size_t kMaxUnreadKinds = 16, kMaxInPlaceNames = 12, kMaxInPlaceSharedLogged = 20, kStaleStreakLog = 30;

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

    bool failed() const { return failed_; }
    const std::string& failure() const { return failure_; }
    const bootstrap::PlayerBootstrap& bootstrap() const { return pb_; }
    const std::string& name() const { return pb_.setup().name; }
    uint32_t channel_of(bootstrap::Role role) const { return channels_[static_cast<size_t>(role)]; }
    size_t join_pending() const { return join_.size(); }
    size_t replies_pending() const { return replies_.size() + delayed_.size(); }
    void   set_purchase_reply_delay(std::chrono::milliseconds d) { purchase_delay_ = d; }
    std::chrono::milliseconds purchase_reply_delay() const { return purchase_delay_; }
    size_t moves_acknowledged() const { return moves_acked_; }
    const MoveChecker& movement() const { return checker_; }
    size_t moves_refused() const { return moves_refused_; }
    size_t moves_stale() const { return moves_stale_; }
    size_t corrections_sent() const { return corrections_; }
    size_t positions_handed_over() const { return handed_over_; }
    const std::string& character_id() const { return character_id_; }
    const items::Inventory& inventory() const { return inventory_; }
    const stats::CharacterStats* character_stats() const { return stats_ ? &*stats_ : nullptr; }
    size_t stat_updates() const { return stat_updates_; }
    size_t stat_items_changed() const { return stat_items_changed_; }
    size_t stat_updates_failed() const { return stat_updates_failed_; }
    bool   inventory_changed() const { return inventory_changed_; }
    bool   inventory_dirty() const { return inventory_dirty_; }
    void   inventory_may_be_unsaved() { inventory_changed_ = true; }
    size_t item_requests_applied() const { return items_applied_; }
    size_t item_requests_refused() const { return items_refused_; }
    size_t inventories_handed_over() const { return inventories_handed_over_; }
    const leveling::Progress&   progress() const { return progress_; }
    int                         class_id() const { return class_id_; }
    std::vector<int64_t>        universal_ranks() const { return pb_.tree_ranks(content::kGuidUniversalTree); }
    size_t uses_applied() const { return uses_applied_; }
    size_t uses_refused() const { return uses_refused_; }
    size_t item_reservations() const { return item_reserved_.size(); }
    size_t levels_gained() const { return levels_gained_; }
    const professions::Professions& professions() const { return professions_; }
    size_t profession_levels_gained() const { return profession_levels_gained_; }
    const weaponxp::State& weapon_lines() const { return weapon_; }
    uint64_t harvesting_node() const { return gather_.node; }
    size_t   harvests_started() const { return harvests_started_; }
    size_t   foliage_interacts() const { return foliage_interacts_; }
    uint32_t cutting_instance() const { return foliage_.row; }
    size_t   foliage_started() const { return foliage_started_; }
    size_t   foliage_completed() const { return foliage_completed_; }
    size_t   foliage_cancelled() const { return foliage_cancelled_; }
    size_t   foliage_refused() const { return foliage_refused_; }
    size_t   foliage_interrupts_sent() const { return foliage_interrupts_sent_; }
    const std::map<std::string, size_t>& foliage_refusals() const { return foliage_refusals_; }
    size_t   harvests_completed() const { return harvests_completed_; }
    size_t   harvests_cancelled() const { return harvests_cancelled_; }
    size_t   harvests_refused() const { return harvests_refused_; }
    const std::map<std::string, size_t>& harvest_refusals() const { return harvest_refusals_; }
    size_t   harvest_piles() const { return harvest_piles_; }
    size_t progress_handed_over() const { return progress_handed_over_; }
    bool   progress_changed() const { return progress_changed_; }
    const talents::Book*            talents() const { return book_ ? &*book_ : nullptr; }
    const actionbar::Bar&           action_bar() const { return bar_; }
    const std::vector<int64_t>&     known_abilities() const { return known_; }
    const std::vector<int64_t>&     record_abilities() const { return records_; }
    size_t                          item_ability_syncs() const { return item_ability_syncs_; }
    size_t talent_requests_applied() const { return talents_applied_; }
    size_t talent_requests_refused() const { return talents_refused_; }
    size_t bar_sets_applied() const { return bar_sets_; }
    size_t bar_sets_refused() const { return bar_refused_; }
    size_t weapon_swaps() const { return weapon_swaps_; }
    bool   using_ranged_weapon() const { return use_ranged_; }
    size_t regen_ticks() const { return pipeline_ ? pipeline_->ticks_regen() : 0; }
    size_t effect_markers_pending() const { return markers_.size(); }
    const AbilityRuntime* abilities() const { return abilities_ ? &*abilities_ : nullptr; }
    const AbilityPipeline* pipeline() const { return pipeline_ ? &*pipeline_ : nullptr; }
    size_t ability_triggers() const { return ability_triggers_; }
    size_t ability_triggers_dropped() const { return ability_dropped_; }
    size_t hit_requests() const { return hit_requests_; }
    size_t hit_requests_dropped() const { return hit_dropped_; }
    size_t hit_confirmations_sent() const { return confirms_sent_; }
    size_t hit_confirmations_pending() const { return pending_confirms_.size(); }
    size_t held_cancels() const { return cancels_; }
    size_t held_cancels_dropped() const { return cancels_dropped_; }
    size_t zones_opened() const { return zone_opened_; }
    size_t zones_closed() const { return zone_closed_; }
    size_t zone_actors() const { return zone_actors_.size(); }
    size_t zone_ops_pending() const { return zone_ops_.size(); }
    size_t ability_updates_sent() const { return ability_updates_; }
    size_t ability_ends_sent() const { return ability_ends_; }
    size_t ability_ends_pending() const { return pending_ends_.size(); }
    bool   abilities_failed() const { return abilities_failed_; }
    size_t exports_tracked() const { return exports_.size(); }
    size_t requests_dropped() const { return dropped_; }
    const std::vector<std::string>& setup_notes() const { return notes_; }
    uint64_t loot_actor() const { return loot_actor_; }
    size_t   loot_opens() const { return loot_opens_; }
    size_t   loot_taken() const { return loot_taken_; }
    size_t   loot_refused() const { return loot_refused_; }
    bool     dead() const { return dead_; }
    size_t   deaths() const { return deaths_; }
    size_t   respawns() const { return respawns_; }
    const respawn::Attunements& attunements() const { return attunements_; }
    size_t   shrine_uses() const { return shrine_uses_; }
    size_t   discoveries() const { return discoveries_; }
    size_t   homes_set() const { return homes_set_; }
    size_t   home_uses() const { return home_uses_; }
    uint64_t pile_actor() const { return pile_actor_; }
    size_t   kills_rewarded() const { return kills_rewarded_; }
    const ActorView* actor_view() const { return view_.get(); }
    size_t scenery_opened() const { return scenery_opened_; }
    double scenery_opened_at() const { return scenery_at_; }
    const MountController* mounts() const { return mounts_.get(); }
    size_t mount_item_uses() const { return mount_item_uses_; }
    size_t mount_summon_requests() const { return mount_summon_requests_; }
    size_t mount_desummon_requests() const { return mount_desummon_requests_; }
    size_t mount_requests_ignored() const { return mount_ignored_; }
    bool        mounted() const { return ride_ && ride_->riding(); }
    const Ride* ride() const { return ride_ ? &*ride_ : nullptr; }
    size_t mount_ups() const { return mount_ups_; }
    size_t dismounts() const { return dismounts_; }
    size_t mount_interactions_refused() const { return mount_refused_; }
    size_t moves_on_the_wrong_channel() const { return moves_wrong_channel_; }
    size_t mount_moves_acknowledged() const { return mount_moves_acked_; }
    size_t dashes_started() const { return dashes_; }
    size_t dashes_refused() const { return dashes_refused_; }
    size_t mount_triggers_refused() const { return mount_triggers_refused_; }
    size_t mount_hits() const { return mount_hits_; }
    size_t mount_deaths() const { return mount_deaths_; }
    size_t air_dismounts() const { return air_dismounts_; }
    size_t distance_desummons() const { return distance_desummons_; }
    void   set_desummon_distance(double cm) { desummon_distance_ = cm; }
    uint64_t vendor_actor() const { return vendor_actor_; }
    int64_t  vendor_def() const { return vendor_def_; }
    size_t   vendor_opens() const { return vendor_opens_; }
    size_t   station_opens() const { return station_opens_; }
    size_t   crafts_started() const { return crafts_started_; }
    size_t   crafts_refused() const { return crafts_refused_; }
    size_t   crafts_claimed() const { return crafts_claimed_; }
    size_t   claims_refused() const { return claims_refused_; }
    const std::vector<svc::ProcessingJob>& processing_jobs() const { return jobs_; }
    const std::vector<int64_t>& taught_recipes() const { return taught_; }
    uint64_t active_target(int slot) const { return slot >= 1 && slot <= 2 ? active_targets_[static_cast<size_t>(slot - 1)].ObjectId : 0; }
    const std::array<uint32_t, 4>& hover_target() const { return hover_words_; }
    bool     pvp_flag() const { return pvp_flag_; }
    const std::optional<protocol::repl::Vec3d>& map_marker() const { return map_marker_; }
    size_t   loot_closes() const { return loot_closes_; }
    const std::vector<int64_t>& completed_ftues() const { return completed_ftues_; }
    bool     quick_active() const { return quick_.active; }
    size_t   quick_started() const { return quick_started_; }
    size_t   quick_made() const { return quick_made_; }
    size_t   quick_interrupted() const { return quick_interrupted_; }
    size_t   quick_refused() const { return quick_refused_; }
    size_t   vendor_purchases() const { return vendor_purchases_; }
    size_t   vendor_sales() const { return vendor_sales_; }
    size_t   vendor_repairs() const { return vendor_repairs_; }
    size_t   items_destroyed() const { return items_destroyed_; }
    size_t   vendor_refused() const { return vendor_refused_; }
    size_t   interactions_ignored() const { return interact_ignored_; }
    uint64_t talk_actor() const { return talk_.state().source; }
    const dialogue::Talk& talk() const { return talk_; }
    size_t   dialogue_opens() const { return dialogue_opens_; }
    size_t   dialogue_steps() const { return dialogue_steps_; }
    size_t   dialogue_closes() const { return dialogue_closes_; }
    size_t   dialogue_refused() const { return dialogue_refused_; }
    const quest::QuestLog& quest_log() const { return quest_log_; }
    bool     quests_on() const { return quests_on_; }
    size_t   quests_taken() const { return quests_taken_; }
    size_t   quests_finished() const { return quests_finished_; }
    size_t   quest_objectives_done() const { return quest_objectives_done_; }
    size_t   quests_refused() const { return quests_refused_; }
    size_t   hand_ins() const { return hand_ins_; }
    size_t   hand_ins_refused() const { return hand_ins_refused_; }
    size_t   hand_in_items() const { return hand_in_items_; }
    size_t   quests_abandoned() const { return quests_abandoned_; }
    bool     warehouse_open() const { return wh_open_; }
    protocol::emit::FIntrepidNetworkGUID warehouse_guid(warehouse::Area area) const;
    size_t   warehouse_opens() const { return wh_opens_; }
    size_t   warehouse_moves() const { return wh_moves_; }
    size_t   warehouse_refused() const { return wh_refused_; }
    const warehouse::Storage& warehouse_storage() const { return wh_storage_; }
    size_t   tax_requests() const { return tax_requests_; }
    size_t   tax_tables_sent() const { return tax_tables_sent_; }
    size_t floor_net_rescues() const { return net_rescues_; }
    double        gm_speed() const { return gm_speed_; }
    size_t        gm_rpcs_received() const { return gm_rpcs_; }
    size_t        gm_rpcs_answered() const { return gm_rpcs_answered_; }
    size_t        gm_teleports() const { return gm_teleports_; }
    const items::Wallet& wallet() const { return wallet_; }
    size_t node_requests() const { return node_requests_; }
    size_t node_summaries_sent() const { return node_summaries_sent_; }
    size_t siege_summaries_sent() const { return siege_summaries_sent_; }
    size_t siege_cooldowns_sent() const { return siege_cooldowns_sent_; }
    size_t node_map_updates() const { return node_map_updates_; }
    size_t node_requests_dropped() const { return node_requests_dropped_; }
    size_t node_gm_rpcs() const { return node_gm_rpcs_; }
    size_t citizenship_requests() const { return citizenship_requests_; }
    uint64_t node_maps_version() const { return node_maps_version_; }
    size_t move_acks_held_back() const { return move_acks_held_back_; }
    uint64_t summoned_pet() const { return pet_actor_; }
    bool pet_following() const { return pet_follow_; }
    pets::Stance pet_stance() const { return pet_stance_; }
    int64_t pet_attack_ability() const { const auto* d = pet_definition(); return (d && d->effect == pets::kHunterEffect && hunter_ranged_) ? pets::kHunterRangedAttack : (d ? d->attack : 0); }
    const pets::Definition* pet_definition() const { const auto it = pet_effects_.find(pet_effect_); return it == pet_effects_.end() ? nullptr : pets::for_effect(it->second); }
    bool hunter_ranged() const { return hunter_ranged_; }
    bool pet_automation() const { return pipeline_ && pipeline_->stacks_of(pets::kAutomationEffect) > 0; }
    size_t moves_unreadable() const { return moves_unreadable_; }
    size_t moves_based() const { return moves_based_; }
    size_t moves_based_unplaced() const { return moves_based_unplaced_; }
    size_t moves_based_held() const { return moves_based_held_; }
    size_t moves_based_cut() const { return moves_based_cut_; }
    static protocol::repl::Vec3d turn_by_yaw(double yaw_degrees, const protocol::repl::Vec3d& v);
    std::string based_move_summary() const;
    TickPlan tick_logic(Connection& conn, Clock::time_point now, bool plan_view);
    void run_view(Connection& conn, const TickPlan& plan, Clock::time_point now);
    void tick_after(Clock::time_point now, const TickPlan& plan);

 private:
    PlayerSession(bootstrap::PlayerSetup setup, bootstrap::GameStateMode mode, std::string character_id, const MovementConfig& movement,
                  PositionSink* sink, const content::AbilityRules* rules, items::Inventory inventory, const items::ItemCatalog* catalog,
                  std::optional<stats::CharacterStats> character_stats, const PlayerWorld* world, int class_id, leveling::Progress progress);

    bool allocate_channels(const Connection& conn);
    void pump(Connection& conn);
    Outcome send(Connection& conn, bootstrap::Send& send);
    void queue(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build, Connection& conn, bool must_build = false);
    void queue_later(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build, Connection& conn, Clock::time_point now, bool must_build = false);
    bool room_for(size_t steps);
    bool within_budget(RequestBudget& budget, const char* what, Clock::time_point now);
    static spdlog::level::level_enum level_for(size_t& count, size_t limit = kLoggedPerKind);
    void dispatch(Connection& conn, bootstrap::Role role, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now);
    static void register_talents_rpcs(RpcTable&);
    static void register_gather_rpcs(RpcTable&);
    static void register_foliage_rpcs(RpcTable&);
    static void register_pets_rpcs(RpcTable&);
    void on_foliage_start(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
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
    void grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now, bootstrap::AbilityChange& effects, std::vector<protocol::repl::SkillOnActionbar>& updates);
    void add_rank_effect(const talents::RankRef& ref, Clock::time_point now, bootstrap::AbilityChange& effects);
    void remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects);
    void refresh_rank_mods();
    void switch_class(const content::ClassRecord& cls, Clock::time_point now, bootstrap::AbilityChange& effects, std::vector<protocol::repl::SkillOnActionbar>& updates, std::vector<std::pair<int, int>>& cleared);
    void run_timers(Connection& conn, Clock::time_point now);
    protocol::repl::ActionbarPayload bar_payload() const;
    void rebuild_ability_lists();
    bool push_abilities();
    void sync_item_abilities(Connection& conn);
    bool item_casts_ability(int64_t record) const;
    std::shared_ptr<const ItemCast> consumable_cast(int64_t ability, int32_t slot, uint32_t location);
    void take_consumable(const std::shared_ptr<const ItemCast>& cast, int32_t client_instance_id);
    bool consumable_paid(int32_t client_instance_id);
    double session_seconds(Clock::time_point now);
    void on_item_activated(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now);
    void on_tier_up(Connection& conn, const protocol::repl::ItemUse& use, svc::ItemContainer container, Clock::time_point now);
    void finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects, std::vector<protocol::repl::SkillOnActionbar>& skill_updates);
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
    const respawn::ShrineBook* shrine_book() const;
    void init_attunements(const svc::Character& character);
    void send_attunements(Connection& conn, Clock::time_point now);
    void run_embersprings(Connection& conn, Clock::time_point now);
    void on_shrine_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now);
    void on_prop_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now);
    void run_props(Connection& conn, Clock::time_point now);
    void finish_prop_use(Connection& conn, uint64_t actor_id, Clock::time_point now);
    void on_home_attunement(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void send_respawn_choices(Connection& conn, bool previous_invalid, bool home_on_cooldown);
    void on_plain_respawn(Connection& conn, Clock::time_point now);
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
    void make_gather_pile(GatherPile& pile);
    void end_gathering(const char* why);
    bool hold_move_ack(float time_stamp, bool on_mount);
    bool answer_move(Connection& conn, float time_stamp, bool on_mount);
    void send_move_ack(Connection& conn, Clock::time_point now);
    std::optional<BasedPlace> place_on_base(const protocol::repl::NetworkMove& m) const;
    bool stand_on_bases(protocol::repl::MovePacked& packed);
    bool release_based_fix();
    void run_pets(Connection& conn, Clock::time_point now);
    void clear_pet();
    void dismiss_pet(Clock::time_point now);
    void on_pet_command(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void drain_pet(Connection& conn, Clock::time_point now);
    bool pet_takes_trigger(Connection& conn, int64_t ability, int32_t instance, const TriggerInfo& info, Clock::time_point now);
    void pet_command(Connection& conn, int64_t command, uint64_t target, const std::optional<nav::Vec3>& place, Clock::time_point now);
    void set_pet_stance(Connection& conn, pets::Stance stance);
    void pet_use_ability(Connection& conn, int64_t ability, uint64_t target, Clock::time_point now, const char* by = nullptr);
    void pet_to_bolt_target(Connection& conn, uint64_t target, bool behind, Clock::time_point now);
    void pet_joins_owners_cast(uint64_t target);
    void fill_pet_bar(Connection& conn, const pets::Definition& def);
    void run_pet_automation(Connection& conn, Clock::time_point now);
    void sync_presence(Clock::time_point now);
    void leave_presence(const char* why);

    bootstrap::PlayerBootstrap pb_;
    std::string character_id_;
    MoveChecker checker_;
    GoodPosition saved_, loaded_;
    PositionSink* sink_;
    std::chrono::seconds save_interval_;
    const content::AbilityRules* ability_rules_;
    items::Inventory inventory_;
    items::Wallet    wallet_;
    const items::ItemCatalog* catalog_;
    bool inventory_dirty_ = false, inventory_changed_ = false;
    std::optional<stats::CharacterStats> stats_;
    size_t stat_updates_ = 0, stat_items_changed_ = 0, stat_updates_failed_ = 0, stat_logged_ = 0;
    const PlayerWorld*   world_ = nullptr;
    int                  class_id_ = 0;
    leveling::Progress   progress_;
    bool                 progress_changed_ = false;
    professions::Professions professions_;
    size_t               profession_levels_gained_ = 0;
    weaponxp::State      weapon_;
    GatherRun            gather_;
    std::deque<GatherPile>        gather_piles_;
    std::deque<GatherCallout>     gather_callouts_;
    uint32_t                      gather_callout_seq_ = 0, foliage_callout_seq_ = 0;
    std::map<std::string, size_t> harvest_refusals_, foliage_refusals_;
    size_t harvests_started_ = 0, harvests_completed_ = 0, harvests_cancelled_ = 0, harvests_refused_ = 0, harvest_piles_ = 0;
    size_t gather_logged_ = 0, gather_refused_logged_ = 0, gather_done_logged_ = 0, foliage_interacts_ = 0;
    bool   gather_off_logged_ = false, foliage_off_logged_ = false;
    FoliageRun                    foliage_;
    Clock::time_point             moved_at_{}, last_now_{}, origin_{}, abilities_origin_{}, ability_log_window_start_{};
    size_t                        foliage_started_ = 0, foliage_completed_ = 0, foliage_cancelled_ = 0, foliage_refused_ = 0, foliage_interrupts_sent_ = 0;
    size_t                        foliage_logged_ = 0, foliage_refused_logged_ = 0, foliage_done_logged_ = 0;
    std::optional<talents::Book> book_;
    actionbar::Bar               bar_;
    std::vector<int64_t>         known_, records_, item_ability_records_;
    int64_t                      combo_ = 0, wielded_line_ = 0, next_passive_effect_id_ = kFirstPassiveEffectId;
    bool                         use_ranged_ = false, cargo_synced_ = false, talents_unreadable_logged_ = false, origin_set_ = false;
    std::map<int64_t, int64_t>   rank_effects_, cargo_effects_;
    std::optional<Clock::time_point> crate_drop_due_;
    int32_t                      crate_drop_instance_ = 0;
    std::vector<Marker>          markers_;
    RequestBudget                talent_budget_{kTalentBurst, kTalentInterval}, bar_budget_{kBarBurst, kBarInterval};
    size_t talents_applied_ = 0, talents_refused_ = 0, bar_sets_ = 0, bar_refused_ = 0, weapon_swaps_ = 0;
    size_t talent_logged_ = 0, talent_refused_logged_ = 0, bar_logged_ = 0, bar_refused_logged_ = 0, regen_logged_ = 0;
    size_t uses_applied_ = 0, uses_refused_ = 0, levels_gained_ = 0, progress_handed_over_ = 0, use_logged_ = 0, use_refused_logged_ = 0;
    RequestBudget item_budget_{kItemBurst, kItemInterval}, foliage_budget_{kItemBurst, kItemInterval}, resync_budget_{kResyncBurst, kResyncInterval};
    size_t items_applied_ = 0, items_refused_ = 0, inventories_handed_over_ = 0;
    size_t item_applied_logged_ = 0, item_refused_logged_ = 0, item_periodic_logged_ = 0;
    bool item_unreadable_logged_ = false, activate_logged_ = false, abilities_failed_ = false, abilities_dirty_ = false;
    std::optional<AbilityRuntime> abilities_;
    std::optional<AbilityPipeline> pipeline_;
    std::deque<int32_t>           pending_ends_, item_taken_;
    std::map<int32_t, std::vector<bootstrap::ItemEvent>> item_sends_;
    std::map<int32_t, std::shared_ptr<const ItemCast>> item_reserved_;
    RequestBudget                 ability_budget_{kAbilityBurst, kAbilityInterval}, hit_budget_{kHitBurst, kHitInterval}, cancel_budget_{kCancelBurst, kCancelInterval};
    std::deque<bootstrap::HitConfirm> pending_confirms_;
    std::map<int64_t, ZoneActor>   zone_actors_;
    std::deque<bootstrap::ZoneOp>  zone_ops_;
    std::optional<protocol::emit::FIntrepidNetworkGUID> pawn_mesh_;
    uint64_t                       zone_random_ = 0;
    size_t                         zone_opened_ = 0, zone_closed_ = 0, zone_logged_ = 0;
    size_t hit_requests_ = 0, hit_dropped_ = 0, confirms_sent_ = 0, cancels_ = 0, cancels_dropped_ = 0, hit_unreadable_logged_ = 0, hit_unserved_logged_ = 0;
    size_t ability_triggers_ = 0, ability_dropped_ = 0, ability_updates_ = 0, ability_ends_ = 0;
    size_t ability_logged_ = 0, ability_unreadable_logged_ = 0, ability_transitions_logged_ = 0, ability_build_logged_ = 0, item_ability_logged_ = 0;
    world::ActorRegistry*          registry_ = nullptr;
    PlayerWorld*                   npc_world_ = nullptr;
    std::shared_ptr<CombatSide>               combat_side_;
    std::unique_ptr<combat::WorldHitPort>     hit_port_;
    bool                                      combat_flag_ = false, dead_ = false, prop_using_ = false;
    RequestBudget                             loot_budget_{kLootBurst, kLootInterval};
    uint64_t                                  loot_actor_ = 0, prop_actor_ = 0, pile_actor_ = 0;
    size_t                                    deaths_ = 0, respawns_ = 0, death_logged_ = 0;
    respawn::Attunements                      attunements_;
    int64_t                                   pending_attune_ = 0;
    Clock::time_point                         prop_end_{};
    double                                    prop_x_ = 0, prop_y_ = 0, death_x_ = 0, death_y_ = 0, death_z_ = 0;
    size_t                                    prop_uses_ = 0, prop_cancelled_ = 0, prop_logged_ = 0;
    size_t                                    shrine_uses_ = 0, discoveries_ = 0, homes_set_ = 0, home_uses_ = 0, shrine_logged_ = 0;
    size_t loot_opens_ = 0, loot_taken_ = 0, loot_refused_ = 0, loot_logged_ = 0, kills_rewarded_ = 0;
    std::unique_ptr<ActorView>     view_;
    MoveLimits                     base_limits_;
    double                         gm_speed_ = 1.0, gm_grace_speed_ = 1.0;
    Clock::time_point              gm_grace_until_{}, net_last_{}, dismounted_at_{};
    RequestBudget                  gm_budget_{4, std::chrono::milliseconds(250)};
    size_t gm_rpcs_ = 0, gm_rpcs_answered_ = 0, gm_teleports_ = 0, gm_logged_ = 0, net_rescues_ = 0;
    std::optional<Clock::time_point> net_since_;
    bool                           net_last_valid_ = false, cooldowns_pending_ = false;
    std::deque<int64_t>            siege_wait_, node_summary_wait_;
    std::set<int64_t>              siege_queued_, node_summary_queued_;
    uint64_t                       node_maps_version_ = 0;
    size_t node_requests_ = 0, node_summaries_sent_ = 0, siege_summaries_sent_ = 0, siege_cooldowns_sent_ = 0, node_map_updates_ = 0, node_requests_dropped_ = 0,
           node_gm_rpcs_ = 0, citizenship_requests_ = 0, node_logged_ = 0;
    bool                           node_unreadable_logged_ = false, node_overflow_logged_ = false, citizenship_logged_ = false, node_unknown_logged_ = false;
    std::optional<Ride>            ride_;
    double                         dash_ready_at_ = -1.0, desummon_distance_ = Ride::kDesummonDistanceCm, scenery_at_ = -1.0;
    protocol::emit::FIntrepidNetworkGUID mount_guid_, mount_cylinder_, mount_ability_, mount_mesh_;
    std::optional<protocol::emit::FIntrepidNetworkGUID> interact_info_;
    RequestBudget                  ride_budget_{kRideBurst, kRideInterval};
    size_t air_dismounts_ = 0, distance_desummons_ = 0, scenery_opened_ = 0, item_ability_syncs_ = 0, time_syncs_sent_ = 0;
    size_t mount_ups_ = 0, dismounts_ = 0, mount_refused_ = 0, moves_wrong_channel_ = 0, mount_moves_acked_ = 0, dashes_ = 0, dashes_refused_ = 0,
           mount_triggers_refused_ = 0, mount_hits_ = 0, mount_deaths_ = 0, ride_logged_ = 0, ride_wrong_logged_ = 0;
    bool                           dismounted_valid_ = false, dismount_extra_sent_ = false, phase_in_done_ = false, scenery_done_ = false;
    std::unique_ptr<MountController> mounts_;
    size_t                         mount_item_uses_ = 0, mount_summon_requests_ = 0, mount_desummon_requests_ = 0, mount_ignored_ = 0, mount_logged_ = 0;
    std::deque<Clock::time_point>  time_syncs_;
    uint64_t                       vendor_actor_ = 0, wh_clerk_ = 0;
    int64_t                        vendor_def_ = 0;
    bool                           vendor_by_talk_ = false, wh_open_ = false, craft_unreadable_logged_ = false;
    RequestBudget                  vendor_budget_{kVendorBurst, kVendorInterval}, craft_budget_{kVendorBurst, kVendorInterval};
    uint32_t                       wh_gen_ = 0;
    warehouse::Storage             wh_storage_;
    warehouse::OpenWindow          wh_window_;
    uint8_t                        wh_access_ = 0;
    std::vector<WhComponent>       wh_components_;
    RequestBudget                  wh_budget_{kVendorBurst, kVendorInterval}, tax_budget_{kTaxBurst, kTaxInterval};
    size_t wh_opens_ = 0, wh_moves_ = 0, wh_refused_ = 0, wh_logged_ = 0, wh_refused_logged_ = 0, wh_unreadable_logged_ = 0;
    size_t tax_requests_ = 0, tax_tables_sent_ = 0, tax_logged_ = 0;
    size_t vendor_opens_ = 0, vendor_purchases_ = 0, vendor_refused_ = 0, interact_ignored_ = 0, vendor_logged_ = 0, interact_logged_ = 0;
    size_t station_opens_ = 0, station_logged_ = 0, station_ignored_logged_ = 0;
    std::vector<svc::ProcessingJob> jobs_;
    size_t crafts_started_ = 0, crafts_refused_ = 0, craft_logged_ = 0, craft_refused_logged_ = 0;
    size_t crafts_claimed_ = 0, claims_refused_ = 0, claim_logged_ = 0, claim_refused_logged_ = 0, crafts_finished_ = 0, craft_update_logged_ = 0;
    bool   claim_unreadable_logged_ = false, ftue_unreadable_logged_ = false, pvp_flag_ = false, interrupt_unreadable_logged_ = false, active_told_ = false;
    crafting::QuickMake quick_;
    std::vector<int64_t> taught_, completed_ftues_;
    size_t ftue_calls_ = 0, ftue_repeats_ = 0, ftue_ignored_ = 0, ftue_ignored_logged_ = 0, ftue_logged_ = 0, ftue_refused_logged_ = 0;
    RequestBudget                                       quiet_budget_{kQuietBurst, kQuietInterval};
    std::array<protocol::emit::FIntrepidNetworkGUID, 2> active_targets_{};
    std::array<uint32_t, 4>                             hover_words_{};
    std::optional<protocol::repl::Vec3d>                map_marker_;
    size_t target_calls_ = 0, target_changes_ = 0, target_unknown_ = 0, hover_calls_ = 0, hover_changes_ = 0, flag_calls_ = 0, flag_changes_ = 0, marker_calls_ = 0, loot_closes_ = 0, slot_calls_ = 0;
    size_t target_logged_ = 0, hover_logged_ = 0, flag_logged_ = 0, marker_logged_ = 0, close_logged_ = 0, slots_logged_ = 0, quiet_refused_logged_ = 0, quiet_unreadable_logged_ = 0;
    size_t quick_started_ = 0, quick_made_ = 0, quick_interrupted_ = 0, quick_refused_ = 0, quick_logged_ = 0, quick_refused_logged_ = 0, quick_made_logged_ = 0;
    std::vector<std::array<int32_t, 4>> active_words_;
    std::map<std::string, uint8_t> job_seen_;
    int64_t next_job_event_ms_ = 0;
    std::set<std::string> station_told_, dialogue_told_, quest_told_, unhandled_logged_, unread_kinds_;
    size_t vendor_sales_ = 0, vendor_repairs_ = 0, items_destroyed_ = 0;
    bool   vendor_unreadable_logged_ = false, vendor_sale_logged_ = false;
    dialogue::Talk talk_;
    RequestBudget  dialogue_budget_{kVendorBurst, kVendorInterval};
    size_t         dialogue_opens_ = 0, dialogue_steps_ = 0, dialogue_closes_ = 0, dialogue_refused_ = 0, dialogue_logged_ = 0;
    quest::QuestLog      quest_log_;
    bool                 quests_on_ = false, quests_touched_ = false, quest_giver_set_ = false, givers_dirty_ = true, givers_told_ = false;
    quest::QuestWorker*  quest_worker_ = nullptr;
    int64_t              zoi_seen_ = -1;
    Clock::time_point    zone_checked_{}, quest_checked_{}, quest_last_now_{}, givers_checked_{}, last_save_{}, last_position_log_{};
    size_t               quests_taken_ = 0, quests_refused_ = 0, quests_abandoned_ = 0, quests_logged_ = 0;
    size_t               hand_ins_ = 0, hand_ins_refused_ = 0, hand_in_items_ = 0;
    stats::SplitMix      quest_rng_{0x51e57a11d0fe5eedull};
    size_t               quests_finished_ = 0, quest_objectives_done_ = 0, quest_rewards_given_ = 0;
    double               quest_giver_x_ = 0, quest_giver_y_ = 0;
    std::deque<WaitingReward> quest_rewards_waiting_;
    std::deque<CrateEvent> quest_crate_events_;
    std::map<int64_t, GiverEntry> givers_;
    bool save_clock_started_ = false, finished_ = false, failed_ = false, phase_in_requested_ = false, replies_full_logged_ = false, waiting_logged_ = false, blocked_ = false;
    std::array<uint32_t, 4> channels_{};
    std::deque<bootstrap::Send> join_;
    std::deque<Step> replies_;
    std::deque<Delayed> delayed_;
    std::chrono::milliseconds purchase_delay_ = kPurchaseReplyDelay;
    uint32_t bar_allow_ = 0, blocked_channel_ = 0;
    std::unordered_map<ChainId, ChainExports> exports_;
    std::vector<std::string> notes_;
    std::string failure_;
    size_t blocked_unacked_ = 0, malformed_bunches_ = 0, foliage_unreadable_logged_ = 0, moves_acked_ = 0;
    std::unordered_map<const char*, size_t> client_calls_;
    size_t in_place_blocks_ = 0, in_place_bunches_ = 0, in_place_shared_logged_ = 0, skipped_fields_ = 0, skipped_fields_logged_ = 0;
    std::unordered_map<std::string, size_t> in_place_names_;
    size_t moves_refused_ = 0, moves_stale_ = 0, corrections_ = 0, handed_over_ = 0, moves_borderline_ = 0, stale_streak_ = 0, speed_logged_ = 0;
    bool   stale_streak_logged_ = false, position_log_started_ = false, sprint_seen_ = false, unreadable_move_logged_ = false;
    SpeedTally sprint_speed_, run_speed_;
    std::optional<float> pending_fix_, based_fix_;
    size_t refused_logged_ = 0, periodic_save_logged_ = 0, dropped_ = 0, possession_logged_ = 0, load_logged_ = 0, combat_logged_ = 0, combat_empty_logged_ = 0, reply_sent_logged_ = 0;
    RequestBudget load_budget_{kLoadBurst, kLoadInterval}, combat_budget_{kLoadBurst, kLoadInterval}, storage_budget_{kStorageBurst, kStorageInterval};
    std::optional<HeldMoveAck> held_move_ack_;
    bool move_acked_ = false, move_acked_on_mount_ = false, pet_dirty_ = false, pet_follow_ = true, hunter_ranged_ = false, hunter_stance_set_ = false, pet_chasing_owner_ = false, presence_failed_ = false, presence_moving_ = false;
    Clock::time_point last_move_ack_{}, pet_clock_{}, presence_moved_at_{};
    size_t move_acks_held_back_ = 0, based_held_run_ = 0, moves_unreadable_ = 0, moves_based_ = 0, moves_based_unplaced_ = 0, moves_based_held_ = 0, moves_based_cut_ = 0;
    std::map<int64_t, int64_t> pet_effects_;
    uint64_t pet_actor_ = 0, pet_held_target_ = 0, pet_incap_target_ = 0, world_pawn_ = 0;
    int64_t pet_effect_ = -1;
    double pet_follow_at_ = 0, pet_auto_at_ = 0;
    pets::Stance pet_stance_ = pets::Stance::Aggressive;
    std::map<int64_t, double> pet_cooldowns_;
    std::map<int64_t, int32_t> pet_cooldown_items_;
    std::shared_ptr<nav::Vec3> pet_owner_at_;
    std::optional<BaseTrack> base_track_;
    GoodPosition presence_at_;
};

std::vector<bootstrap::ItemEvent> item_events_for(const items::Outcome& outcome);

struct PlayerHandle {
    PlayerSession* player = nullptr;
    Connection*    connection = nullptr;
    explicit operator bool() const { return player != nullptr && connection != nullptr; }
};

}  // namespace worldd
}  // namespace aoc
