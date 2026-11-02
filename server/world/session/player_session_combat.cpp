#include "combat/combat_world.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/hit_rpc.h"
#include "session/player_session_internal.h"

namespace aoc {
namespace worldd {

namespace {

using namespace ::sandal::worldd::session::detail;
using SendFn = std::function<int(bootstrap::Send&)>;

bool is_ability_transition(const AbilityEvent& e) {
  return e.end_ability || !e.confirmations.empty() || !e.zones.empty() || !e.change.abilities_added.empty() ||
         !e.change.abilities_removed.empty() || !e.change.abilities_changed.empty() ||
         !e.change.effects_added.empty() || !e.change.effects_removed.empty() || !e.change.effects_changed.empty() ||
         !e.change.cooldowns_set.empty() || !e.change.cooldowns_removed.empty();
}

uint32_t next_zone_word(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
}

bootstrap::LingeringSpec make_lingering_spec(const PlayerWorld& world, const bootstrap::ZoneOp& op,
                                             const GoodPosition& at, FIntrepidNetworkGUID pawn_mesh,
                                             uint64_t& zone_random) {
  bootstrap::LingeringSpec spec;
  spec.actor = world.mint_guid();
  spec.ability = world.mint_guid();
  spec.stats = world.mint_guid();
  spec.movement = world.mint_guid();
  spec.pawn_mesh = pawn_mesh;
  spec.zone = op.zone;
  spec.source = op.ability;
  spec.x = at.x;
  spec.y = at.y;
  spec.z = at.z;
  spec.yaw_degrees = at.yaw_degrees;
  for (int i = 0; i < 4; ++i) {
    spec.effect_guid[i] = next_zone_word(zone_random);
    spec.instance_guid[i] = next_zone_word(zone_random);
  }
  return spec;
}

world::WorldActor make_zone_world_actor(const bootstrap::LingeringSpec& spec, const GoodPosition& at, int32_t player_id,
                                        repl::ExportChain mesh_export, repl::Actor actor) {
  world::WorldActor w;
  w.id = spec.actor.ObjectId;
  w.kind = world::ActorKind::Effect;
  w.actor = std::move(actor);
  w.x = at.x;
  w.y = at.y;
  w.z = at.z;
  w.yaw_degrees = at.yaw_degrees;
  w.never_dormant = true;
  w.owner = static_cast<uint64_t>(player_id);
  w.npc.source = world::Source::Runtime;
  w.open_exports.push_back(std::move(mesh_export));
  return w;
}

bool drain_pending_ends(bootstrap::PlayerBootstrap& pb, std::deque<int32_t>& pending_ends, size_t& ability_ends,
                        const SendFn& send_fn) {
  while (!pending_ends.empty()) {
    const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb.checkpoint();
    std::optional<bootstrap::Send> end = pb.end_ability(pending_ends.front());
    if (!end) {
      spdlog::warn("[session] {}: ClientEndAbility({}) does not build: dropped", pb.setup().name, pending_ends.front());
      pending_ends.pop_front();
      continue;
    }
    const int res = send_fn(*end);
    if (res == 1) {
      pb.restore(checkpoint);
      return false;
    }
    if (res == 0) {
      ++ability_ends;
    } else {
      pb.restore(checkpoint);
      spdlog::error("[session] {}: ClientEndAbility({}) was refused by the connection: dropped", pb.setup().name,
                    pending_ends.front());
    }
    pending_ends.pop_front();
  }
  return true;
}

void drain_pending_confirms(bootstrap::PlayerBootstrap& pb, std::deque<bootstrap::HitConfirm>& pending_confirms,
                            size_t& confirms_sent, const GoodPosition& at, const SendFn& send_fn) {
  while (!pending_confirms.empty()) {
    const size_t n = std::min(pending_confirms.size(), PlayerSessionTypes::kConfirmsPerCall);
    std::vector<bootstrap::HitConfirm> batch(pending_confirms.begin(),
                                             pending_confirms.begin() + static_cast<std::ptrdiff_t>(n));
    const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb.checkpoint();
    std::optional<bootstrap::Send> send_it = pb.hit_confirmations(batch, repl::Vec3d{at.x, at.y, at.z});
    if (!send_it) {
      spdlog::warn("[session] {}: {} hit confirmation(s) do not build: dropped", pb.setup().name, n);
      pending_confirms.erase(pending_confirms.begin(), pending_confirms.begin() + static_cast<std::ptrdiff_t>(n));
      continue;
    }
    const int res = send_fn(*send_it);
    if (res == 1) {
      pb.restore(checkpoint);
      return;
    }
    if (res == 0) {
      confirms_sent += n;
    } else {
      pb.restore(checkpoint);
      spdlog::error("[session] {}: the hit confirmations were refused by the connection: dropped", pb.setup().name);
    }
    pending_confirms.erase(pending_confirms.begin(), pending_confirms.begin() + static_cast<std::ptrdiff_t>(n));
  }
}

struct ParsedTriggerInput {
  int64_t ability = 0;
  int32_t instance = 0;
  int32_t item_slot = -1;
  uint32_t item_location = 0;
  bool pvp_force = false;
  bool full_call = false;
  TriggerInfo info;
};

using ResolveActorsFn = std::function<std::vector<uint64_t>(const std::vector<FIntrepidNetworkGUID>&,
                                                            const std::vector<repl::InlineObject>&)>;

std::optional<ParsedTriggerInput> read_trigger_input(const repl::ClientRpc& rpc, const InBunch& bunch,
                                                     const ResolveActorsFn& resolve_actors) {
  ParsedTriggerInput out;
  if (const auto t = repl::parse_trigger_ability(rpc.call)) {
    out.ability = t->ability_guid;
    out.instance = t->client_instance_id;
    out.pvp_force = t->pvp_force;
    out.item_slot = t->item_slot;
    out.item_location = t->item_location;
    out.full_call = true;
    out.info.animation_index = t->animation_index;
    out.info.input_x = t->input_x;
    out.info.input_y = t->input_y;
    out.info.target_count = t->target_count;
    out.info.target_actors = resolve_actors(t->targets, t->inline_targets);
    out.info.item = t->item_slot != -1;
    return out;
  }
  if (const auto prefix = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                                    rpc.call.param_pos + rpc.call.param_bits)) {
    out.ability = prefix->ability_guid;
    out.instance = prefix->client_instance_id;
    return out;
  }
  return std::nullopt;
}

std::map<int64_t, double> current_stat_map(bootstrap::PlayerBootstrap& pb) {
  std::map<int64_t, double> stats;
  for (const auto& [record, value] : pb.current_stats()) stats[record] = value;
  return stats;
}

void attach_pipeline_catalog_and_reactions(AbilityPipeline& pipeline,
                                           const std::optional<talents::Book>& book, int64_t wielded_line,
                                           const PlayerWorld* world,
                                           std::pair<std::string_view, const std::vector<int64_t>&> session_and_known) {
  pipeline.set_known(session_and_known.second);
  if (book) pipeline.set_reaction_carriers(book->reaction_carriers(wielded_line));
  if (world) pipeline.set_reactions_enabled(world->reactions_enabled());
  if (world && world->ability_catalog()) {
    spdlog::info("[session] {}: the generic ability runtime is on ({} abilities known, {} in the tables)",
                 session_and_known.first, session_and_known.second.size(), world->ability_catalog()->abilities());
  }
}

bool is_trigger_blocked_by_state(bool dead, bool on_mount, bool is_mounted) {
  return dead || (on_mount && !is_mounted);
}

bool is_valid_trigger_input(const std::optional<ParsedTriggerInput>& parsed, const AbilityRuntime& runtime,
                            const std::optional<AbilityPipeline>& pipeline) {
  if (!parsed) return false;
  if (parsed->full_call) return true;
  return !runtime.enabled(parsed->ability) && !(pipeline && pipeline->handles(parsed->ability));
}

void log_unreadable_trigger(size_t& logged, std::string_view session_name, bool has_parsed, size_t param_bits) {
  if (++logged <= PlayerSessionTypes::kLoggedPerKind) {
    spdlog::warn("[session] {}: a ServerTriggerAbility{} does not read ({} parameter bits): ignored", session_name,
                 has_parsed ? " for an ability worldd runs" : "", param_bits);
  }
}

bool is_unmounted_dash_ability(const std::unique_ptr<MountController>& mounts, int64_t ability) {
  if (!mounts || ability == 0) return false;
  const content::MountAbilityRow* row = mounts->book().data().ability(ability);
  return row && row->role == "dash";
}

bool is_summon_mount_ability(const std::unique_ptr<MountController>& mounts, int64_t ability) {
  return mounts && ability != 0 && ability == mounts->book().summon_ability();
}

bool dispatch_trigger_to_runtime(AbilityRuntime& runtime, std::optional<AbilityPipeline>& pipeline,
                                 const ParsedTriggerInput& parsed, double sec) {
  if (pipeline && pipeline->handles(parsed.ability)) {
    const Refusal accepted = pipeline->trigger(parsed.ability, parsed.instance, sec, parsed.info);
    return parsed.ability == content::kGuidDropCrate && accepted == Refusal::None;
  }
  runtime.trigger(parsed.ability, parsed.instance, sec, parsed.info);
  return false;
}

}  // namespace

bool PlayerSession::own_inline_object(const repl::InlineObject& o) const {
  const bootstrap::SessionGuids& g = pb_.setup().guids;
  return o.outer == g.pawn || o.outer == g.collision_cylinder;
}

std::vector<uint64_t> PlayerSession::actors_of(const std::vector<protocol::emit::FIntrepidNetworkGUID>& objects,
                                               const std::vector<repl::InlineObject>& inline_objects) const {
  std::vector<uint64_t> out;
  if (!npc_world_ || !npc_world_->combat()) return out;
  const bootstrap::SessionGuids& g = pb_.setup().guids;
  const auto add = [&](uint64_t object_id) {
    const uint64_t actor = npc_world_->combat()->actor_of_object(object_id);
    if (actor != 0 && std::find(out.begin(), out.end(), actor) == out.end()) out.push_back(actor);
  };
  for (const protocol::emit::FIntrepidNetworkGUID& o : objects) {
    if (o == g.collision_cylinder || o == g.pawn) continue;
    add(o.ObjectId);
  }
  // Inline components lack exported GUIDs and resolve to their outer actor.
  for (const repl::InlineObject& o : inline_objects) {
    if (own_inline_object(o) || o.outer.ObjectId == 0) continue;
    add(o.outer.ObjectId);
  }
  return out;
}

void PlayerSession::take_hit(Connection& conn, const combat::IncomingHit& h, Clock::time_point now) {
  if (!pipeline_) return;
  if (mounted()) return hit_on_mount(conn, h, now);
  pipeline_->receive_hit(h, ability_seconds(now));
  if (mounts_ && (h.flags & stats::kHitMiss) == 0) {
    for (const combat::ResolvedMod& m : h.mods) {
      if (m.stat_guid == content::kGuidHealth && m.value < 0) {
        mount_damaged(now);
        break;
      }
    }
  }
}

void PlayerSession::drain_combat(Connection& conn, Clock::time_point now) {
  if (!combat_side_ || !pipeline_) return;
  const double sec = ability_seconds(now);
  std::vector<combat::IncomingHit> hits;
  hits.swap(combat_side_->inbox);
  for (const combat::IncomingHit& h : hits) take_hit(conn, h, now);
  while (!combat_side_->kills.empty()) {
    if (!room_for(12 + (world_ ? world_->auto_grants().size() : 0))) break;
    const combat::KillReward k = combat_side_->kills.front();
    combat_side_->kills.erase(combat_side_->kills.begin());
    apply_kill(conn, k, now);
  }
  if (combat_side_->wants_in_combat != combat_flag_ && stats_ && stats_->started()) {
    combat_flag_ = combat_side_->wants_in_combat;
    stats_->set_in_combat(combat_flag_);
    if (abilities_) abilities_->set_in_combat(combat_flag_, sec);
    pipeline_->refresh(sec, combat_flag_ ? "combat starts" : "combat ends", true);
    if (pb_.set_in_combat(combat_flag_)) {
      queue(
          "pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("combat state"); }, conn,
          /*must_build=*/true);
    }
  }
}

AbilityRuntime* PlayerSession::ability_runtime(Clock::time_point now) {
  if (abilities_failed_ || finished_ || failed_ || !ability_rules_) return nullptr;
  if (!abilities_) {
    session_seconds(now);
    abilities_origin_ = origin_;
    abilities_.emplace(*ability_rules_, current_stat_map(pb_), pb_.setup().world_time);
    if (!abilities_->ok()) {
      spdlog::error("[session] {}: the ability runtime cannot start: {}", name(), abilities_->error());
      abilities_failed_ = true;
      abilities_.reset();
      return nullptr;
    }
    if (stats_ && stats_->started()) {
      pipeline_.emplace(
          world_ ? world_->ability_catalog() : nullptr, *stats_, *abilities_,
          [this] { return bootstrap::worn_items_of(inventory_); }, pb_.setup().world_time,
          static_cast<uint64_t>(pb_.setup().player_id) * 7919u + 1u);
      attach_pipeline_catalog_and_reactions(*pipeline_, book_, wielded_line_, world_, {name(), known_});
      pipeline_->set_weapon_tags([this] { return weapon_tags(); });
      pipeline_->set_active_target(
          [this]() -> uint64_t { return active_targets_[0].is_valid() ? active_targets_[0].ObjectId : 0; });
      sync_ability_values();
      if (npc_world_ && npc_world_->combat()) {
        combat_side_ = std::make_shared<CombatSide>(*this);
        hit_port_ = std::make_unique<combat::WorldHitPort>(*npc_world_->combat(), npc_world_->registry(), *combat_side_,
                                                           pb_.setup().world_time);
        pipeline_->set_combat(hit_port_.get());
        npc_world_->combat()->add_player(combat_side_.get());
      }
    }
  }
  return &*abilities_;
}

double PlayerSession::ability_seconds(Clock::time_point now) const {
  return std::chrono::duration<double>(now - abilities_origin_).count();
}

bool PlayerSession::dash_allowed(Clock::time_point now) const {
  return abilities_ && abilities_->dash_allowed(ability_seconds(now));
}

void PlayerSession::abilities_off(const std::string& why) {
  abilities_failed_ = true;
  pending_ends_.clear();
  std::string running;
  if (abilities_) {
    const auto add = [&](bool on, const char* what) {
      if (on) running += (running.empty() ? "" : ", ") + std::string(what);
    };
    add(abilities_->sprinting(), "a Sprint");
    add(abilities_->resting() || abilities_->rest_effect_on(), "a Rest");
    add(abilities_->evading(), "the Evasion");
  }
  spdlog::warn(
      "[session] {}: abilities are off for this session ({}): the pawn lists {} ability item(s), {} effect(s) and {} "
      "cooldown(s) that are never "
      "ended or removed now{}",
      name(), why, pb_.listed_abilities(), pb_.listed_effects(), pb_.listed_cooldowns(),
      running.empty() ? "" : "; the runtime has running: " + running);
}

void PlayerSession::take_ability_events() {
  AbilityRuntime& runtime = *abilities_;
  std::vector<AbilityEvent> events = runtime.take_events();
  if (pipeline_) {
    std::vector<AbilityEvent> more = pipeline_->take_events();
    events.insert(events.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
  }
  for (AbilityEvent& e : events) {
    if (!e.change.empty()) {
      if (!pb_.apply_ability_change(e.change)) {
        abilities_off("the pawn's state does not take the ability change (" + e.note + ": " +
                      (pb_.error().empty() ? std::string("its items and the runtime's keys disagree") : pb_.error()) +
                      ")");
        return;
      }
      abilities_dirty_ = true;
    }
    for (bootstrap::HitConfirm& h : e.confirmations) {
      if (pending_confirms_.size() >= kMaxPendingConfirms) pending_confirms_.pop_front();
      pending_confirms_.push_back(std::move(h));
    }
    if (e.end_ability) pending_ends_.push_back(*e.end_ability);
    for (const bootstrap::ZoneOp& z : e.zones) {
      if (zone_ops_.size() < kMaxZoneOps) zone_ops_.push_back(z);
    }
    const spdlog::level::level_enum level = e.refusal ? level_for(ability_logged_)
                                            : is_ability_transition(e)
                                                ? level_for(ability_transitions_logged_, kLoggedAbilityTransitions)
                                                : spdlog::level::debug;
    spdlog::log(level, "[session] {}: {} (Stamina {:.2f}){}", name(), e.note, runtime.stamina(),
                e.end_ability ? ", ClientEndAbility " + std::to_string(*e.end_ability) : std::string());
  }
}

void PlayerSession::flush_zones(Connection& conn) {
  (void)conn;
  const auto forget = [&](int64_t id) {
    const auto it = zone_actors_.find(id);
    if (it == zone_actors_.end()) return;
    if (registry_) registry_->remove(it->second.actor);
    pb_.lingering_remove(it->second.actor);
    zone_actors_.erase(it);
  };
  while (!zone_ops_.empty()) {
    const bootstrap::ZoneOp op = zone_ops_.front();
    zone_ops_.pop_front();
    const auto existing = zone_actors_.find(op.id);
    if (op.kind == bootstrap::ZoneOp::Kind::Open) {
      if (!world_ || !registry_ || existing != zone_actors_.end()) continue;
      if (!pawn_mesh_) pawn_mesh_ = world_->mint_guid();
      if (zone_random_ == 0) {
        zone_random_ =
            0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(pb_.setup().player_id) * 0xBF58476D1CE4E5B9ull) ^ 1;
      }
      const GoodPosition at = checker_.good();
      const bootstrap::LingeringSpec spec = make_lingering_spec(*world_, op, at, *pawn_mesh_, zone_random_);
      auto actor = pb_.lingering_actor_of(spec);
      if (!actor) {
        spdlog::warn("[session] {}: the zone actor of ability {} cannot be built: dropped", name(), op.ability);
        continue;
      }
      world::WorldActor w = make_zone_world_actor(spec, at, pb_.setup().player_id,
                                                  pb_.lingering_mesh_export(*pawn_mesh_), std::move(*actor));
      if (!registry_->add(std::move(w))) {
        spdlog::warn("[session] {}: the registry does not take the zone actor of ability {}: dropped", name(),
                     op.ability);
        continue;
      }
      zone_actors_[op.id].actor = spec.actor.ObjectId;
      ++zone_opened_;
      continue;
    }
    if (existing == zone_actors_.end()) continue;
    if (op.kind == bootstrap::ZoneOp::Kind::Close) {
      ++zone_closed_;
      spdlog::log(level_for(zone_logged_),
                  "[session] {}: the zone actor is removed from the world (the client's channel is closed, destroyed)",
                  name());
      forget(op.id);
      continue;
    }
    registry_->modify(existing->second.actor, world::Change::State, [](world::WorldActor& a) {
      a.actor.obj.state.set(*a.actor.obj.schema, "bFadingOut", repl::Value::boolean(true));
    });
  }
}

void PlayerSession::flush_abilities(Connection& conn) {
  if (abilities_failed_ || failed_ || !join_.empty()) return;
  flush_zones(conn);
  if (blocked_ && conn.unacked_reliable(blocked_channel_) >= blocked_unacked_) return;
  if (abilities_dirty_) {
    const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
    bool build_failed = false;
    std::optional<bootstrap::Send> update = pb_.ability_update(&build_failed);
    if (!update) {
      if (build_failed) {
        spdlog::log(
            level_for(ability_build_logged_) == spdlog::level::info ? spdlog::level::warn : spdlog::level::debug,
            "[session] {}: an ability update does not build ({}): it is sent with the next change, if it builds then",
            name(), pb_.error().empty() ? "no reason" : pb_.error());
      }
      abilities_dirty_ = false;
    } else {
      switch (send(conn, *update)) {
        case Outcome::Sent:
          abilities_dirty_ = false;
          ++ability_updates_;
          break;
        case Outcome::Wait:
          pb_.restore(checkpoint);
          return;
        case Outcome::Refused:
          pb_.restore(checkpoint);
          spdlog::error("[session] {}: the ability update was refused by the connection", name());
          abilities_off("the connection refused an ability update");
          return;
      }
    }
  }
  const auto send_fn = [this, &conn](bootstrap::Send& s) { return static_cast<int>(send(conn, s)); };
  if (!drain_pending_ends(pb_, pending_ends_, ability_ends_, send_fn)) return;
  drain_pending_confirms(pb_, pending_confirms_, confirms_sent_, checker_.good(), send_fn);
}

void PlayerSession::run_abilities(Connection& conn, Clock::time_point now) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime) return;
  runtime->tick(ability_seconds(now));
  if (pipeline_) {
    pipeline_->tick(ability_seconds(now));
    drain_combat(conn, now);
  }
  take_ability_events();
  check_death(conn, now);
  flush_abilities(conn);
}

void PlayerSession::on_ability_trigger(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch,
                                       Clock::time_point now, bool on_mount) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime) return;
  if (!within_budget(ability_budget_, "ServerTriggerAbility", now)) {
    ++ability_dropped_;
    return;
  }
  if (is_trigger_blocked_by_state(dead_, on_mount, mounted())) {
    if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                                 rpc.call.param_pos + rpc.call.param_bits)) {
      if (!dead_) ++mount_triggers_refused_;
      end_instance(conn, t->client_instance_id, dead_ ? on_mount : true);
    }
    return;
  }
  ++ability_triggers_;
  cancel_gather(&conn, "an ability was triggered");
  const auto parsed = read_trigger_input(
      rpc, bunch, [this](const auto& objs, const auto& inlines) { return actors_of(objs, inlines); });
  if (!is_valid_trigger_input(parsed, *runtime, pipeline_)) {
    log_unreadable_trigger(ability_unreadable_logged_, name(), parsed.has_value(), rpc.call.param_bits);
    return;
  }
  if (parsed->full_call) pb_.note_trigger_pvp_flag(parsed->instance, parsed->pvp_force);
  if (mounted()) {
    if (!mounted_trigger(conn, parsed->ability, parsed->instance, on_mount, now)) return;
  } else if (is_unmounted_dash_ability(mounts_, parsed->ability)) {
    ++mount_triggers_refused_;
    end_instance(conn, parsed->instance, false);
    return;
  }
  if (is_summon_mount_ability(mounts_, parsed->ability)) {
    return on_summon_trigger(conn, parsed->instance, parsed->item_slot, parsed->item_location, now);
  }
  if (dispatch_trigger_to_runtime(*runtime, pipeline_, *parsed, ability_seconds(now))) {
    crate_drop_due_ = now + kCrateDropDelay;
    crate_drop_instance_ = parsed->instance;
  }
  take_ability_events();
  flush_abilities(conn);
}

void PlayerSession::on_ability_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime) return;
  if (!within_budget(cancel_budget_, "ServerNotifyHeldAbilityCancelled", now)) {
    ++cancels_dropped_;
    return;
  }
  ++cancels_;
  const auto cancel = repl::parse_held_ability_cancelled(rpc.call);
  if (!cancel) return;
  if (crate_drop_due_ && cancel->client_instance_id == crate_drop_instance_) crate_drop_due_.reset();
  if (!runtime->cancel(cancel->client_instance_id, ability_seconds(now)) && pipeline_) {
    pipeline_->cancel(cancel->client_instance_id, ability_seconds(now), static_cast<double>(cancel->cancelled_time));
  }
  take_ability_events();
  flush_abilities(conn);
}

void PlayerSession::on_branch_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime || !pipeline_) return;
  if (!within_budget(cancel_budget_, "ServerRequestBranchVerification", now)) {
    ++cancels_dropped_;
    return;
  }
  const auto request = repl::parse_branch_verification(rpc.call);
  if (!request) return;
  if (!pipeline_->branch_request(request->client_instance_id, request->branch_guid, ability_seconds(now))) {
    if (unhandled_logged_.insert("ServerRequestBranchVerification").second) {
      spdlog::info(
          "[session] {}: the client's ServerRequestBranchVerification belongs to no cast worldd waits for (it answers "
          "it for the cast bars it knows: kBranchedCasts in ability_pipeline.cpp; told once)",
          name());
    }
    return;
  }
  take_ability_events();
  flush_abilities(conn);
}

void PlayerSession::teach_ability(int64_t ability) {
  if (std::find(known_.begin(), known_.end(), ability) == known_.end()) known_.push_back(ability);
  if (pipeline_) pipeline_->set_known(known_);
}

void PlayerSession::on_ability_end(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime || !pipeline_) return;
  if (!within_budget(cancel_budget_, "ServerEndAbility", now)) {
    ++cancels_dropped_;
    return;
  }
  const auto id = repl::parse_end_ability(rpc.call);
  if (!id) return;
  if (crate_drop_due_ && *id == crate_drop_instance_) crate_drop_due_.reset();
  pipeline_->end_request(*id, ability_seconds(now));
  take_ability_events();
  flush_abilities(conn);
}

double PlayerSession::take_damage(Connection& conn, double amount, Clock::time_point now) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime || !pipeline_) return 0;
  if (mounted()) {
    if (amount > 0 && ride_) {
      const Ride::Hurt hurt = ride_->hurt(amount);
      ++mount_hits_;
      if (hurt.died) mount_died(conn, now);
    }
    return 0;
  }
  if (amount > 0) mount_damaged(now);
  const double through = pipeline_->take_damage(amount, ability_seconds(now));
  take_ability_events();
  flush_abilities(conn);
  return through;
}

void PlayerSession::on_hit_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
  AbilityRuntime* runtime = ability_runtime(now);
  if (!runtime || !pipeline_) return;
  if (!within_budget(hit_budget_, "RequestHitVerification_Server", now)) {
    ++hit_dropped_;
    return;
  }
  ++hit_requests_;
  if (dead_) return;
  const auto req = repl::parse_hit_request(rpc.call);
  if (!req) {
    if (++hit_unreadable_logged_ <= kLoggedPerKind) {
      spdlog::warn("[session] {}: a RequestHitVerification_Server does not read ({} parameter bits): ignored", name(),
                   rpc.call.param_bits);
    }
    return;
  }
  const bootstrap::SessionGuids& g = pb_.setup().guids;
  size_t own = 0;
  std::vector<protocol::emit::FIntrepidNetworkGUID> objects;
  std::vector<repl::InlineObject> inline_objects;
  for (const repl::HitComponentRef& c : req->components) {
    if (c.inline_object) {
      if (own_inline_object(*c.inline_object))
        ++own;
      else
        inline_objects.push_back(*c.inline_object);
    } else if (c.object == g.collision_cylinder || c.object == g.pawn) {
      ++own;
    } else {
      objects.push_back(c.object);
    }
  }
  const AbilityPipeline::HitVerdict verdict =
      pipeline_->hit_request(req->ability_guid, req->targeting_guid, req->hit_guid, req->components.size(), own,
                             ability_seconds(now), actors_of(objects, inline_objects));
  if (verdict != AbilityPipeline::HitVerdict::Scheduled && verdict != AbilityPipeline::HitVerdict::Quiet) {
    if (++hit_unserved_logged_ <= kLoggedPerKind) {
      spdlog::info("[session] {}: a hit request for {} / {} is {}", name(), req->ability_guid, req->hit_guid,
                   AbilityPipeline::verdict_name(verdict));
    }
  }
  take_ability_events();
  flush_abilities(conn);
}

}  // namespace worldd
}  // namespace aoc
