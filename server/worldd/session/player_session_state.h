#pragma once

#include "session/player_session_types.h"
#include "dialogue/talk.h"
#include "quest/engine.h"
#include "session/ability_pipeline.h"
#include "session/action_bar.h"
#include "session/item_use.h"

namespace aoc {
namespace worldd {

class PlayerSessionState : public PlayerSessionTypes {
  friend class CombatSide;
  friend struct SessionInternal;

 public:
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
    protocol::emit::FIntrepidNetworkGUID warehouse_guid(warehouse::Area area) const {
        const WhComponent* first = nullptr;
        for (const WhComponent& w : wh_components_) {
            if (w.key.area != area) continue;
            if (wh_open_ && wh_window_.find(w.key)) return w.comp.guid;
            if (!first) first = &w;
        }
        return first ? first->comp.guid : protocol::emit::FIntrepidNetworkGUID{};
    }
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

 protected:
    PlayerSessionState(bootstrap::PlayerSetup setup, bootstrap::GameStateMode mode, std::string character_id, const MovementConfig& movement,
                  PositionSink* sink, const content::AbilityRules* rules, items::Inventory inventory, const items::ItemCatalog* catalog,
                  std::optional<stats::CharacterStats> character_stats, const PlayerWorld* world, int class_id, leveling::Progress progress)
        : pb_(std::move(setup), mode),
          character_id_(std::move(character_id)),
          checker_(movement.limits, start_of(pb_.setup())),
          saved_(checker_.good()),
          loaded_(checker_.good()),
          sink_(sink),
          save_interval_(movement.save_interval),
          ability_rules_(rules),
          inventory_(std::move(inventory)),
          catalog_(catalog),
          stats_(std::move(character_stats)),
          world_(world),
          class_id_(class_id),
          progress_(progress),
          base_limits_(movement.limits) {}

    bootstrap::PlayerBootstrap pb_;
    std::string character_id_;
    MoveChecker checker_;
    GoodPosition saved_;                 // what was loaded, then what was last handed to the sink
    GoodPosition loaded_;                // what the record held: finish() compares with this, not with a hand-over the saver may have lost
    PositionSink* sink_;
    std::chrono::seconds save_interval_;
    const content::AbilityRules* ability_rules_;   // plan A7: PlayerWorld's, null in a test that wants no abilities
    items::Inventory inventory_;                   // plan B2
    items::Wallet    wallet_;                      // the vendors milestone, step 0: the purse (saved with the inventory, shown through OwnerInfo.Currencies)
    const items::ItemCatalog* catalog_;
    bool inventory_dirty_ = false;                 // changed since the last hand-over to the sink
    bool inventory_changed_ = false;               // changed at all this session
    std::optional<stats::CharacterStats> stats_;   // plan B3
    size_t stat_updates_ = 0, stat_items_changed_ = 0, stat_updates_failed_ = 0, stat_logged_ = 0;
    const PlayerWorld*   world_ = nullptr;           // the content (item uses, the level curve, the auto grants); null in a test without one
    int                  class_id_ = 0;              // ECharacterClass, as the session plays it (a class scroll changes it)
    leveling::Progress   progress_;
    bool                 progress_changed_ = false;  // at all this session
    professions::Professions professions_;           // Gathering stage 2: the 22 professions (not valid() without the world's book)
    size_t               profession_levels_gained_ = 0;
    weaponxp::State      weapon_;                    // weapon XP: the XP total of each weapon line (not valid() without the world's lines)
    struct GatherRun {
        uint64_t                     node = 0;          // the registry actor being harvested (0: none)
        protocol::emit::FIntrepidNetworkGUID target;    // the node's guid as the client has it (the pawn's ChannelingData names it, and the clear names it again)
        const content::GatherDefRow* def = nullptr;
        int                          tool_slot = 0;     // the Equipment slot of the tool, its instance and the durability it had when the channel began
        std::string                  tool_guid;
        Clock::time_point            started{};
        double                       seconds = 0;
        double                       start_x = 0, start_y = 0;   // where the player stood when the channel began (the stand-still check measures from here)
        bool                         hunt = false;      // Gathering stage 3: the node is a huntable creature's corpse (its channel is a hunt)
        bool                         gives_nothing = false;   // ... of a kind whose reward table gives no usable item (the session drops the items)
        std::string                  what;              // the log's name of what is harvested
    } gather_;
    struct GatherPile {          // the stacks a full bag could not take: an APlayerLootPile 100 ms after the experience frame [R: 1461 packets 897 and 904]
        Clock::time_point due{};
        std::vector<combat::LootStack> stacks;
        double x = 0, y = 0, z = 0;
        std::string node_name;
    };
    std::deque<GatherPile>           gather_piles_;
    struct GatherCallout {       // the "not enough inventory space" callout and when it is taken off
        Clock::time_point due{};
        uint64_t          key = 0;
    };
    std::deque<GatherCallout>        gather_callouts_;
    uint32_t                         gather_callout_seq_ = 0;
    std::map<std::string, size_t>    harvest_refusals_;
    size_t harvests_started_ = 0, harvests_completed_ = 0, harvests_cancelled_ = 0, harvests_refused_ = 0, harvest_piles_ = 0;
    size_t gather_logged_ = 0, gather_refused_logged_ = 0, gather_done_logged_ = 0;
    size_t foliage_interacts_ = 0;   // ServerStartInteractionWithFoliage calls
    bool   gather_off_logged_ = false;
    struct FoliageRun {
        uint32_t                       row = 0xFFFFFFFFu;   // the instance (foliage_map.h); kNoInstance: none
        int                            state = 0;           // the state it is cut in (the client's and the world's agree)
        uint32_t                       index = 0;           // as the client named it
        std::string                    owner, component;
        const content::GatherDefRow*   interaction = nullptr;
        std::string                    name;                // the def's, for the log
        int                            tool_slot = 0;
        std::string                    tool_guid;
        int64_t                        tool_record = 0;
        Clock::time_point              started{}, data_at{}, completes{};
        double                         seconds = 0;
        double                         start_x = 0, start_y = 0;
        bool                           data_sent = false;
    } foliage_;
    Clock::time_point                moved_at_{};              // the last accepted move that was a walk (a held direction or a speed)
    Clock::time_point                last_now_{};              // the time of the last tick (what a cancel with no time of its own is stamped with)
    size_t                           foliage_started_ = 0, foliage_completed_ = 0, foliage_cancelled_ = 0, foliage_refused_ = 0, foliage_interrupts_sent_ = 0;
    size_t                           foliage_logged_ = 0, foliage_refused_logged_ = 0, foliage_done_logged_ = 0;
    uint32_t                         foliage_callout_seq_ = 0;
    std::map<std::string, size_t>    foliage_refusals_;
    bool                             foliage_off_logged_ = false;
    std::optional<talents::Book> book_;
    actionbar::Bar               bar_;
    std::vector<int64_t>         known_, records_;          // as the client was told
    int64_t                      combo_ = 0;                // the combo of the weapon in hand, as KnownAbilities holds it (0: none)
    bool                         use_ranged_ = false;
    int64_t                      wielded_line_ = 0;         // the weapon skill line of the weapon in hand (0: none)
    std::map<int64_t, int64_t>   rank_effects_;             // rank guid -> the CurrentEffects key of the item the session added for it
    std::map<int64_t, int64_t>   cargo_effects_;            // effect record -> the CurrentEffects key of the item the session added for the crate on the back
    bool                         cargo_synced_ = false;     // sync_cargo ran once for the crate (or the empty slot) the character joined with
    std::optional<Clock::time_point> crate_drop_due_;       // General_DropCrate was accepted: the crate leaves the back at this time
    int32_t                      crate_drop_instance_ = 0;  // ... and the ClientInstanceId of that cast: a cancel of it (Esc, bMayEscCancel) keeps the crate
    struct Marker {
        Clock::time_point due;
        int64_t           key = 0;
    };
    std::vector<Marker>          markers_;                  // effect items that end by themselves
    RequestBudget                talent_budget_{kTalentBurst, kTalentInterval};
    RequestBudget                bar_budget_{kBarBurst, kBarInterval};
    size_t talents_applied_ = 0, talents_refused_ = 0, bar_sets_ = 0, bar_refused_ = 0, weapon_swaps_ = 0;
    size_t talent_logged_ = 0, talent_refused_logged_ = 0, bar_logged_ = 0, bar_refused_logged_ = 0, regen_logged_ = 0;
    bool   talents_unreadable_logged_ = false;
    Clock::time_point            origin_{};                 // the session's clock 0 (the first tick or request)
    bool                         origin_set_ = false;
    size_t uses_applied_ = 0, uses_refused_ = 0, levels_gained_ = 0, progress_handed_over_ = 0;
    size_t use_logged_ = 0, use_refused_logged_ = 0;
    int64_t next_passive_effect_id_ = kFirstPassiveEffectId;
    RequestBudget item_budget_{kItemBurst, kItemInterval};
    RequestBudget resync_budget_{kResyncBurst, kResyncInterval};
    size_t items_applied_ = 0, items_refused_ = 0, inventories_handed_over_ = 0;
    size_t item_applied_logged_ = 0, item_refused_logged_ = 0, item_periodic_logged_ = 0;
    bool item_unreadable_logged_ = false, activate_logged_ = false;
    std::optional<AbilityRuntime> abilities_;
    std::optional<AbilityPipeline> pipeline_;            // plan D2: every other ability, and Health and Mana's regeneration
    Clock::time_point             abilities_origin_{};   // the runtime's clock 0
    bool                          abilities_failed_ = false;
    bool                          abilities_dirty_ = false;   // changes applied to the pawn and not sent yet
    std::deque<int32_t>           pending_ends_;               // ClientEndAbility calls not sent yet, in order
    RequestBudget                 ability_budget_{kAbilityBurst, kAbilityInterval};
    RequestBudget                 hit_budget_{kHitBurst, kHitInterval};
    RequestBudget                 cancel_budget_{kCancelBurst, kCancelInterval};
    std::deque<bootstrap::HitConfirm> pending_confirms_;       // plan D2, stage 2: confirmations not sent yet, in order
    struct ZoneActor {
        uint64_t                   actor = 0;      // the actor's ObjectId, in the registry
    };
    std::map<int64_t, ZoneActor>   zone_actors_;               // by the runtime's zone instance id
    std::deque<bootstrap::ZoneOp>  zone_ops_;                  // opens, fades and closes not done yet, in order
    std::optional<protocol::emit::FIntrepidNetworkGUID> pawn_mesh_;   // the pawn's CharacterMesh0 the zones' actors attach to: one guid for the session
    uint64_t                       zone_random_ = 0;
    size_t                         zone_opened_ = 0, zone_closed_ = 0, zone_logged_ = 0;
    size_t hit_requests_ = 0, hit_dropped_ = 0, confirms_sent_ = 0, cancels_ = 0, cancels_dropped_ = 0, hit_unreadable_logged_ = 0, hit_unserved_logged_ = 0;
    size_t ability_triggers_ = 0, ability_dropped_ = 0, ability_updates_ = 0, ability_ends_ = 0;
    size_t ability_logged_ = 0, ability_unreadable_logged_ = 0, ability_transitions_logged_ = 0, ability_build_logged_ = 0, item_ability_logged_ = 0;
    world::ActorRegistry*          registry_ = nullptr;
    PlayerWorld*                   npc_world_ = nullptr;   // plan E1, stage 2: the NPCs' world (the player's place is told to it every tick)
    std::shared_ptr<CombatSide>               combat_side_;
    std::unique_ptr<combat::WorldHitPort>     hit_port_;
    bool                                      combat_flag_ = false;   // bInCombat as the client was last told
    RequestBudget                             loot_budget_{kLootBurst, kLootInterval};
    uint64_t                                  loot_actor_ = 0;
    bool                                      dead_ = false;
    size_t                                    deaths_ = 0, respawns_ = 0, death_logged_ = 0;
    respawn::Attunements                      attunements_;          // Embersprings: the character's discovered places, home and the last use of the home respawn
    int64_t                                   pending_attune_ = 0;   // the place whose prompt waits for the player's yes or no
    bool                                      prop_using_ = false;
    uint64_t                                  prop_actor_ = 0;
    Clock::time_point                         prop_end_{};
    double                                    prop_x_ = 0, prop_y_ = 0;
    size_t                                    prop_uses_ = 0, prop_cancelled_ = 0, prop_logged_ = 0;
    size_t                                    shrine_uses_ = 0, discoveries_ = 0, homes_set_ = 0, home_uses_ = 0, shrine_logged_ = 0;
    uint64_t                                  pile_actor_ = 0;
    double                                    death_x_ = 0, death_y_ = 0, death_z_ = 0;
    size_t loot_opens_ = 0, loot_taken_ = 0, loot_refused_ = 0, loot_logged_ = 0, kills_rewarded_ = 0;
    std::unique_ptr<ActorView>     view_;
    MoveLimits                     base_limits_;                 // the pawn's: the limits every possession's checker starts from
    double                         gm_speed_ = 1.0;
    double                         gm_grace_speed_ = 1.0;
    Clock::time_point              gm_grace_until_{};
    RequestBudget                  gm_budget_{4, std::chrono::milliseconds(250)};
    size_t gm_rpcs_ = 0, gm_rpcs_answered_ = 0, gm_teleports_ = 0, gm_logged_ = 0;
    std::optional<Clock::time_point> net_since_;
    Clock::time_point              net_last_{};
    bool                           net_last_valid_ = false;
    size_t                         net_rescues_ = 0;
    std::deque<int64_t>            siege_wait_, node_summary_wait_;      // the nodes asked for, in the order asked
    std::set<int64_t>              siege_queued_, node_summary_queued_;  // ... and which are waiting (a repeated request is one answer)
    bool                           cooldowns_pending_ = false;
    uint64_t                       node_maps_version_ = 0;               // the registry version the GameState's maps were last brought to
    size_t node_requests_ = 0, node_summaries_sent_ = 0, siege_summaries_sent_ = 0, siege_cooldowns_sent_ = 0, node_map_updates_ = 0, node_requests_dropped_ = 0,
           node_gm_rpcs_ = 0, citizenship_requests_ = 0, node_logged_ = 0;
    bool                           node_unreadable_logged_ = false, node_overflow_logged_ = false, citizenship_logged_ = false, node_unknown_logged_ = false;
    std::optional<Ride>            ride_;                        // the mount the player rides, or has been on and has not lost (desummoned)
    double                         dash_ready_at_ = -1.0;        // review A I3: when the mount dash is ready again (the session's ability clock): a new mount's Ride starts with it, so a desummon and a summon do not reset the 30 s
    protocol::emit::FIntrepidNetworkGUID mount_guid_, mount_cylinder_, mount_ability_, mount_mesh_;   // of the ride's mount
    std::optional<protocol::emit::FIntrepidNetworkGUID> interact_info_;   // the pawn's InteractInfo (MountPtr), minted at the first ride
    RequestBudget                  ride_budget_{kRideBurst, kRideInterval};
    size_t air_dismounts_ = 0, distance_desummons_ = 0;
    double desummon_distance_ = Ride::kDesummonDistanceCm;
    size_t mount_ups_ = 0, dismounts_ = 0, mount_refused_ = 0, moves_wrong_channel_ = 0, mount_moves_acked_ = 0, dashes_ = 0, dashes_refused_ = 0,
           mount_triggers_refused_ = 0, mount_hits_ = 0, mount_deaths_ = 0, ride_logged_ = 0, ride_wrong_logged_ = 0;
    Clock::time_point              dismounted_at_{};             // the moment of the last dismount: the second request of a pair in one datagram sends the extra restart
    bool                           dismounted_valid_ = false, dismount_extra_sent_ = false;
    std::unique_ptr<MountController> mounts_;
    size_t                         mount_item_uses_ = 0, mount_summon_requests_ = 0, mount_desummon_requests_ = 0, mount_ignored_ = 0, mount_logged_ = 0;
    bool                           phase_in_done_ = false;       // the phase-in is built: the view may start
    bool                           scenery_done_ = false;        // nothing is left for open_scenery to open (none within reach of the join, or all opened)
    size_t                         scenery_opened_ = 0;          // replicators the join opened
    double                         scenery_at_ = -1.0;           // ... when the last of them went out (the session's seconds; -1: none)
    size_t                         item_ability_syncs_ = 0;      // the pawn updates sync_item_abilities sent
    std::deque<Clock::time_point>  time_syncs_;
    size_t                         time_syncs_sent_ = 0;
    uint64_t                       vendor_actor_ = 0;
    int64_t                        vendor_def_ = 0;
    bool                           vendor_by_talk_ = false;   // the open window was opened by a line of the shopkeeper's conversation, not by a use of it
    RequestBudget                  vendor_budget_{kVendorBurst, kVendorInterval};
    struct WhComponent {
        warehouse::Key                  key;
        warehouse::Spec                 spec;   // the container as the window shows it (the base chunk's data is sent with no window open)
        bootstrap::WarehouseComponent   comp;   // as it is meant to be once the window is open (the tabs listed and writable); the first half of an open sends them read-only and unlisted
    };
    uint32_t                       wh_gen_ = 0;                // counts the opens and closes: a reply of an earlier open that finds it changed is dropped
    warehouse::Storage             wh_storage_;                // what the character keeps in the warehouse (loaded at the join, saved with the inventory)
    warehouse::OpenWindow          wh_window_;                 // the containers the window has open now
    bool                           wh_open_ = false;
    uint64_t                       wh_clerk_ = 0;              // the clerk's actor id
    uint8_t                        wh_access_ = 0;             // the StorageAccessConfig the window opened with
    std::vector<WhComponent>       wh_components_;             // the pawn's warehouse components of this connection, with their guids
    RequestBudget                  wh_budget_{kVendorBurst, kVendorInterval};
    size_t wh_opens_ = 0, wh_moves_ = 0, wh_refused_ = 0, wh_logged_ = 0, wh_refused_logged_ = 0, wh_unreadable_logged_ = 0;
    RequestBudget                  tax_budget_{kTaxBurst, kTaxInterval};
    size_t tax_requests_ = 0, tax_tables_sent_ = 0, tax_logged_ = 0;
    size_t vendor_opens_ = 0, vendor_purchases_ = 0, vendor_refused_ = 0, interact_ignored_ = 0, vendor_logged_ = 0, interact_logged_ = 0;
    size_t station_opens_ = 0, station_logged_ = 0, station_ignored_logged_ = 0;   // crafting, step 0: windows opened, log lines said
    std::vector<svc::ProcessingJob> jobs_;            // crafting, step 1: the character's processing jobs, in the order they were queued (saved with the inventory)
    size_t crafts_started_ = 0, crafts_refused_ = 0, craft_logged_ = 0, craft_refused_logged_ = 0;
    size_t crafts_claimed_ = 0, claims_refused_ = 0, claim_logged_ = 0, claim_refused_logged_ = 0, crafts_finished_ = 0, craft_update_logged_ = 0;   // crafting, step 2
    bool   claim_unreadable_logged_ = false;
    crafting::QuickMake quick_;                       // crafting, step 3: the craft that runs
    std::vector<int64_t> taught_;                     // the recipes the character was taught, in the order they were taught (saved with the inventory)
    std::vector<int64_t> completed_ftues_;            // missing systems, step 0: the tutorials the client reported finished, in the order they came (saved with the progress)
    size_t ftue_calls_ = 0, ftue_repeats_ = 0, ftue_ignored_ = 0, ftue_ignored_logged_ = 0, ftue_logged_ = 0, ftue_refused_logged_ = 0;
    bool   ftue_unreadable_logged_ = false;
    RequestBudget                                       quiet_budget_{kQuietBurst, kQuietInterval};
    std::array<protocol::emit::FIntrepidNetworkGUID, 2> active_targets_{};   // the target of slot 1 and 2 as CombatInfo.ActiveTargets holds them
    std::array<uint32_t, 4>                             hover_words_{};
    bool                                                pvp_flag_ = false;
    std::optional<protocol::repl::Vec3d>                map_marker_;
    size_t target_calls_ = 0, target_changes_ = 0, target_unknown_ = 0, hover_calls_ = 0, hover_changes_ = 0, flag_calls_ = 0, flag_changes_ = 0, marker_calls_ = 0, loot_closes_ = 0, slot_calls_ = 0;
    size_t target_logged_ = 0, hover_logged_ = 0, flag_logged_ = 0, marker_logged_ = 0, close_logged_ = 0, slots_logged_ = 0, quiet_refused_logged_ = 0, quiet_unreadable_logged_ = 0;
    size_t quick_started_ = 0, quick_made_ = 0, quick_interrupted_ = 0, quick_refused_ = 0, quick_logged_ = 0, quick_refused_logged_ = 0, quick_made_logged_ = 0;
    bool   interrupt_unreadable_logged_ = false;
    std::vector<std::array<int32_t, 4>> active_words_;   // the stations OwnerInfo.ActiveProcessingStations lists
    bool   active_told_ = false;                      // the list was set on the pawn at least once (before that an empty list stays out of the state)
    std::map<std::string, uint8_t> job_seen_;         // job guid -> the status the window was last told of (or an open shows): a change is sent
    int64_t next_job_event_ms_ = 0;                   // the earliest wall-clock moment a job changes status (0: look at once)
    std::set<std::string> station_told_;              // what was said once about a station kind
    size_t vendor_sales_ = 0, vendor_repairs_ = 0, items_destroyed_ = 0;
    bool   vendor_unreadable_logged_ = false, vendor_sale_logged_ = false;
    dialogue::Talk talk_;
    RequestBudget  dialogue_budget_{kVendorBurst, kVendorInterval};
    size_t         dialogue_opens_ = 0, dialogue_steps_ = 0, dialogue_closes_ = 0, dialogue_refused_ = 0, dialogue_logged_ = 0;
    std::set<std::string> dialogue_told_;   // what the conversation skipped or could not answer, told once for each text
    quest::QuestLog      quest_log_;
    bool                 quests_on_ = false;        // the log loaded
    quest::QuestWorker*  quest_worker_ = nullptr;   // saves the log, takes the GM commands (null: no quest service)
    bool                 quests_touched_ = false;   // the log changed during this session (it is saved once more when the session ends)
    int64_t              zoi_seen_ = -1;            // the zone of influence the character stood in at the last check (-1: none checked yet)
    Clock::time_point    zone_checked_{};
    size_t               quests_taken_ = 0, quests_refused_ = 0, quests_abandoned_ = 0, quests_logged_ = 0;
    size_t               hand_ins_ = 0, hand_ins_refused_ = 0, hand_in_items_ = 0;   // quests part 3, slice 2: deliveries taken and refused, and the items they took
    std::set<std::string> quest_told_;               // what a quest's start skipped, told once for each text
    stats::SplitMix      quest_rng_{0x51e57a11d0fe5eedull};   // the dice of the reward tables (their weights and chances)
    Clock::time_point    quest_checked_{};          // the last once-a-second quest tick
    Clock::time_point    quest_last_now_{};         // the time of the last quest work (what happens outside a tick uses it)
    size_t               quests_finished_ = 0, quest_objectives_done_ = 0, quest_rewards_given_ = 0;
    double               quest_giver_x_ = 0, quest_giver_y_ = 0;   // where the NPC a conversation's quest actions ran for stands (node experience goes to its town)
    bool                 quest_giver_set_ = false;
    struct WaitingReward {
        std::vector<int64_t> tables;
        std::string          why;
        int64_t              quest = 0;   // the quest the tables belong to (its level is the narrative content level of their formulas); 0: none
    };
    std::deque<WaitingReward> quest_rewards_waiting_;   // rewards that wait for room in the reply queue
    struct CrateEvent {   // what construction_crate_equipped / construction_deposit noted for the next quest tick
        bool    deposit = false;
        int64_t node = 0, project = 0, item = 0;
        int32_t count = 0;
    };
    std::deque<CrateEvent> quest_crate_events_;
    struct GiverEntry {
        int32_t id = -1;                                    // the item's replication id in the quest giver array
        int64_t registration = 0;                           // the map registration it shows (0: none)
        uint8_t flavor = 7;                                 // EQuestFlavor (7: none)
        int64_t asset = 0;                                  // the NPC's population asset (its offers when the instance has none of its own)
        std::string name;                                   // the NPC's name, for the log
    };
    std::map<int64_t, GiverEntry> givers_;
    bool              givers_dirty_ = true;                 // the log changed since the last look
    Clock::time_point givers_checked_{};
    bool              givers_told_ = false;                 // the "too many givers" warning was written
    Clock::time_point last_save_{};
    bool save_clock_started_ = false;
    bool finished_ = false;
    std::array<uint32_t, 4> channels_{};
    std::deque<bootstrap::Send> join_;
    std::deque<Step> replies_;
    std::deque<Delayed> delayed_;                       // replies waiting for their time, in the order they were asked for
    std::chrono::milliseconds purchase_delay_ = kPurchaseReplyDelay;
    uint32_t bar_allow_ = 0;                            // ServerSetActionbarSlotUseData the purchases made the client send, not yet seen
    std::unordered_map<ChainId, ChainExports> exports_;
    std::vector<std::string> notes_;
    std::set<std::string> unhandled_logged_;
    bool failed_ = false;
    std::string failure_;
    bool phase_in_requested_ = false;
    bool replies_full_logged_ = false;
    bool waiting_logged_ = false;
    bool blocked_ = false;              // the head of the queue waits for room on blocked_channel_ (it held blocked_unacked_ bunches)
    uint32_t blocked_channel_ = 0;
    size_t blocked_unacked_ = 0;
    size_t malformed_bunches_ = 0;
    std::set<std::string> unread_kinds_;
    std::unordered_map<const char*, size_t> client_calls_;
    size_t in_place_blocks_ = 0, in_place_bunches_ = 0, in_place_shared_logged_ = 0, skipped_fields_ = 0, skipped_fields_logged_ = 0;
    std::unordered_map<std::string, size_t> in_place_names_;
    size_t foliage_unreadable_logged_ = 0;   // the foliage starts whose parameters did not decode and were said (the first few)
    size_t moves_acked_ = 0;
    size_t moves_refused_ = 0, moves_stale_ = 0, corrections_ = 0, handed_over_ = 0, moves_borderline_ = 0;
    size_t stale_streak_ = 0;
    bool   stale_streak_logged_ = false;
    Clock::time_point last_position_log_{};
    bool position_log_started_ = false;
    SpeedTally sprint_speed_, run_speed_;
    bool sprint_seen_ = false;
    size_t speed_logged_ = 0;
    std::optional<float> pending_fix_;   // the TimeStamp of the latest refused move of this datagram: its correction waits for end_of_datagram
    bool unreadable_move_logged_ = false;
    size_t refused_logged_ = 0, periodic_save_logged_ = 0;
    RequestBudget load_budget_{kLoadBurst, kLoadInterval};
    RequestBudget combat_budget_{kLoadBurst, kLoadInterval};
    RequestBudget storage_budget_{kStorageBurst, kStorageInterval};
    size_t dropped_ = 0;
    size_t possession_logged_ = 0;
    size_t load_logged_ = 0;
    size_t combat_logged_ = 0;
    size_t combat_empty_logged_ = 0;
    size_t reply_sent_logged_ = 0;
};

}  // namespace worldd
}  // namespace aoc
