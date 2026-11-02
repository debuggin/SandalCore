// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_session.cpp
//
//  See player_session.h.
// ============================================================================
#include "session/player_session_internal.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace aoc { namespace worldd {

using namespace ::sandal::worldd::session::detail;
namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

bool PlayerSession::own_inline_object(const repl::InlineObject& o) const {
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    return o.outer == g.pawn || o.outer == g.collision_cylinder;
}

std::vector<uint64_t> PlayerSession::actors_of(const std::vector<protocol::emit::FIntrepidNetworkGUID>& objects, const std::vector<repl::InlineObject>& inline_objects) const {
    std::vector<uint64_t> out;
    if (!npc_world_ || !npc_world_->combat()) return out;
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const auto add = [&](uint64_t object_id) {
        const uint64_t actor = npc_world_->combat()->actor_of_object(object_id);
        if (actor != 0 && std::find(out.begin(), out.end(), actor) == out.end()) out.push_back(actor);
    };
    for (const protocol::emit::FIntrepidNetworkGUID& o : objects) {
        if (o == g.collision_cylinder || o == g.pawn) continue;   // the caster's own components
        add(o.ObjectId);
    }
    // E1 stage 3 fix: a component named in place (the client has no GUID for an NPC's CollisionCylinder: worldd never exported it) belongs to its
    // outer, the actor; the user's run named every NPC so and every cast "found nobody"
    for (const repl::InlineObject& o : inline_objects) {
        if (own_inline_object(o) || o.outer.ObjectId == 0) continue;
        add(o.outer.ObjectId);
    }
    return out;
}
// C.O.A with C | Y | B

void PlayerSession::take_hit(Connection& conn, const combat::IncomingHit& h, Clock::time_point now) {
    if (!pipeline_) return;
    // the mounts milestone, stage 3: a hit aimed at a mounted rider lands on the MOUNT's Health; the rider's own is untouched
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
    // the experience of the kills (a level-up sends a dozen chains: it waits for room)
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
        pipeline_->refresh(sec, combat_flag_ ? "combat starts" : "combat ends", true);   // the regeneration is five times faster out of combat
        if (pb_.set_in_combat(combat_flag_)) {
            queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("combat state"); }, conn, /*must_build=*/true);
        }
    }
}
// C.Y.B x C O A

// ============================================================================================================================
//  abilities (plan A7)
// ============================================================================================================================

AbilityRuntime* PlayerSession::ability_runtime(Clock::time_point now) {
    if (abilities_failed_ || finished_ || failed_ || !ability_rules_) return nullptr;   // a finished session answers nothing more
    if (!abilities_) {
        // The stats the runtime reads or sets, from the pawn's own arrays as they are now (the join's values and the equips' changes since).
        std::map<int64_t, double> stats;
        for (const auto& [record, value] : pb_.current_stats()) stats[record] = value;
        session_seconds(now);
        abilities_origin_ = origin_;
        abilities_.emplace(*ability_rules_, std::move(stats), pb_.setup().world_time);
        if (!abilities_->ok()) {
            spdlog::error("[session] {}: the ability runtime cannot start: {}", name(), abilities_->error());
            abilities_failed_ = true;
            abilities_.reset();
            return nullptr;
        }
        // plan D2: the generic runtime beside it (Health and Mana's regeneration runs there even without the ability tables)
        if (stats_ && stats_->started()) {
            pipeline_.emplace(world_ ? world_->ability_catalog() : nullptr, *stats_, *abilities_,
                              [this] { return bootstrap::worn_items_of(inventory_); }, pb_.setup().world_time, static_cast<uint64_t>(pb_.setup().player_id) * 7919u + 1u);
            pipeline_->set_known(known_);
            if (book_) pipeline_->set_reaction_carriers(book_->reaction_carriers(wielded_line_));
            if (world_) pipeline_->set_reactions_enabled(world_->reactions_enabled());
            pipeline_->set_weapon_tags([this] { return weapon_tags(); });
            pipeline_->set_active_target([this]() -> uint64_t { return active_targets_[0].is_valid() ? active_targets_[0].ObjectId : 0; });
            sync_ability_values();
            if (npc_world_ && npc_world_->combat()) {
                combat_side_ = std::make_shared<CombatSide>(*this);
                // the runtime's clock is the session's (0 at the join); the combat world's and the aftermath's is the server's world time: the port
                // translates (the join's world time is the runtime's zero) [E1 stage 3 fix 2: the corpses closed and respawned at the next tick]
                hit_port_ = std::make_unique<combat::WorldHitPort>(*npc_world_->combat(), npc_world_->registry(), *combat_side_, pb_.setup().world_time);
                pipeline_->set_combat(hit_port_.get());
                npc_world_->combat()->add_player(combat_side_.get());
            }
            if (world_ && world_->ability_catalog()) {
                spdlog::info("[session] {}: the generic ability runtime is on ({} abilities known, {} in the tables)", name(), known_.size(),
                             world_->ability_catalog()->abilities());
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
    // Nothing more is sent, so the client keeps whatever it was last told, never ended or removed: what the pawn lists is that (a
    // Sprint's effect and MoveSpeedMult stay until the client logs in again).
    std::string running;
    if (abilities_) {
        const auto add = [&](bool on, const char* what) {
            if (on) running += (running.empty() ? "" : ", ") + std::string(what);
        };
        add(abilities_->sprinting(), "a Sprint");
        add(abilities_->resting() || abilities_->rest_effect_on(), "a Rest");
        add(abilities_->evading(), "the Evasion");
    }
    spdlog::warn("[session] {}: abilities are off for this session ({}): the pawn lists {} ability item(s), {} effect(s) and {} cooldown(s) that are never "
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
                              (pb_.error().empty() ? std::string("its items and the runtime's keys disagree") : pb_.error()) + ")");
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
        // State changes (an ability starts or ends, a refusal) are what an in-game run reads; Stamina's steps are debug.  A client
        // can cause them as often as its budget allows: the first few of each kind at info, the rest at debug.
        const bool transition = e.end_ability || !e.confirmations.empty() || !e.zones.empty() || !e.change.abilities_added.empty() || !e.change.abilities_removed.empty() ||
                                !e.change.abilities_changed.empty() || !e.change.effects_added.empty() || !e.change.effects_removed.empty() ||
                                !e.change.effects_changed.empty() || !e.change.cooldowns_set.empty() || !e.change.cooldowns_removed.empty();
        const bool refusal = e.refusal;
        const spdlog::level::level_enum level = refusal      ? level_for(ability_logged_)
                                                : transition ? level_for(ability_transitions_logged_, kLoggedAbilityTransitions)
                                                             : spdlog::level::debug;
        spdlog::log(level, "[session] {}: {} (Stamina {:.2f}){}", name(), e.note, runtime.stamina(),
                    e.end_ability ? ", ClientEndAbility " + std::to_string(*e.end_ability) : std::string());
    }
}
// == C:Y:B | CoA ==

/// The lingering zones' actors (plan D2's dance effect): each zone the ability runtime opens is an actor of the world's registry (plan E1), owned by this
/// player: the actor system opens it on a channel of its own, sends the bFadingOut update and closes it with Destroyed, as the retail server does for a
/// dance's visual effect.  The session only builds the actor, adds it, changes it and removes it.
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
            bootstrap::LingeringSpec spec;
            if (!pawn_mesh_) pawn_mesh_ = world_->mint_guid();
            spec.actor = world_->mint_guid();
            spec.ability = world_->mint_guid();
            spec.stats = world_->mint_guid();
            spec.movement = world_->mint_guid();
            spec.pawn_mesh = *pawn_mesh_;
            spec.zone = op.zone;
            spec.source = op.ability;
            const GoodPosition at = checker_.good();
            spec.x = at.x;
            spec.y = at.y;
            spec.z = at.z;
            spec.yaw_degrees = at.yaw_degrees;
            if (zone_random_ == 0) zone_random_ = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(pb_.setup().player_id) * 0xBF58476D1CE4E5B9ull) ^ 1;
            const auto next_word = [&] {   // SplitMix64: the FGuids of an actor are random words
                zone_random_ += 0x9E3779B97F4A7C15ull;
                uint64_t z = zone_random_;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
            };
            for (int i = 0; i < 4; ++i) {
                spec.effect_guid[i] = next_word();
                spec.instance_guid[i] = next_word();
            }
            auto actor = pb_.lingering_actor_of(spec);
            if (!actor) {
                spdlog::warn("[session] {}: the zone actor of ability {} cannot be built: dropped", name(), op.ability);
                continue;
            }
            world::WorldActor w;
            w.id = spec.actor.ObjectId;
            w.kind = world::ActorKind::Effect;
            w.actor = std::move(*actor);
            w.x = at.x;
            w.y = at.y;
            w.z = at.z;
            w.yaw_degrees = at.yaw_degrees;
            w.never_dormant = true;
            w.owner = static_cast<uint64_t>(pb_.setup().player_id);
            w.npc.source = world::Source::Runtime;
            w.open_exports.push_back(pb_.lingering_mesh_export(*pawn_mesh_));
            if (!registry_->add(std::move(w))) {
                spdlog::warn("[session] {}: the registry does not take the zone actor of ability {}: dropped", name(), op.ability);
                continue;
            }
            ZoneActor& za = zone_actors_[op.id];
            za.actor = spec.actor.ObjectId;
            ++zone_opened_;
            continue;
        }
        if (existing == zone_actors_.end()) continue;   // an actor that was never made has nothing to fade or close
        if (op.kind == bootstrap::ZoneOp::Kind::Close) {
            ++zone_closed_;
            spdlog::log(level_for(zone_logged_), "[session] {}: the zone actor is removed from the world (the client's channel is closed, destroyed)", name());
            forget(op.id);
            continue;
        }
        registry_->modify(existing->second.actor, world::Change::State, [](world::WorldActor& a) {
            a.actor.obj.state.set(*a.actor.obj.schema, "bFadingOut", repl::Value::boolean(true));
        });
    }
}
// ~ C | Y | B & C-O-A ~

void PlayerSession::flush_abilities(Connection& conn) {
    if (abilities_failed_ || failed_ || !join_.empty()) return;   // the join's chains go first: the pawn's second state is theirs
    flush_zones(conn);
    if (blocked_ && conn.unacked_reliable(blocked_channel_) >= blocked_unacked_) return;
    if (abilities_dirty_) {
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        bool build_failed = false;
        std::optional<bootstrap::Send> update = pb_.ability_update(&build_failed);
        if (!update) {
            // Either the changes since the last update came to nothing (an effect added and removed), or the update does not
            // build: the shadow is as it was, so the next change sends both, and the cause is logged (a few times).
            if (build_failed) {
                spdlog::log(level_for(ability_build_logged_) == spdlog::level::info ? spdlog::level::warn : spdlog::level::debug,
                            "[session] {}: an ability update does not build ({}): it is sent with the next change, if it builds then", name(),
                            pb_.error().empty() ? "no reason" : pb_.error());
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
    while (!pending_ends_.empty()) {
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> end = pb_.end_ability(pending_ends_.front());
        if (!end) {
            spdlog::warn("[session] {}: ClientEndAbility({}) does not build: dropped", name(), pending_ends_.front());
            pending_ends_.pop_front();
            continue;
        }
        switch (send(conn, *end)) {
            case Outcome::Sent:
                ++ability_ends_;
                pending_ends_.pop_front();
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: ClientEndAbility({}) was refused by the connection: dropped", name(), pending_ends_.front());
                pending_ends_.pop_front();
                break;
        }
    }
    // the confirmations of the hits (the client's floating numbers), after the update that holds their effect: up to 20 hits a call, as retail's
    while (!pending_confirms_.empty()) {
        std::vector<bootstrap::HitConfirm> batch;
        const size_t n = std::min(pending_confirms_.size(), kConfirmsPerCall);
        for (size_t i = 0; i < n; ++i) batch.push_back(pending_confirms_[i]);
        const GoodPosition at = checker_.good();
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> send_it = pb_.hit_confirmations(batch, repl::Vec3d{at.x, at.y, at.z});
        if (!send_it) {
            spdlog::warn("[session] {}: {} hit confirmation(s) do not build: dropped", name(), n);
            pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        switch (send(conn, *send_it)) {
            case Outcome::Sent:
                confirms_sent_ += n;
                pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: the hit confirmations were refused by the connection: dropped", name());
                pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
                break;
        }
    }
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
// (CoA : C.Y.B)

void PlayerSession::on_ability_trigger(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    if (!within_budget(ability_budget_, "ServerTriggerAbility", now)) {   // (a dead character's triggers take the budget too: each costs a reliable answer)
        ++ability_dropped_;
        return;
    }
    if (dead_) {
        // a dead character casts nothing: the instance is ended (ClientEndAbility), whichever runtime would have run it
        if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos, rpc.call.param_pos + rpc.call.param_bits)) {
            end_instance(conn, t->client_instance_id, on_mount);
        }
        return;
    }
    if (on_mount && !mounted()) {
        // a trigger on a mount nobody rides (a race at a dismount, or a client that lies): the cast is ended where it was started
        if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos, rpc.call.param_pos + rpc.call.param_bits)) {
            ++mount_triggers_refused_;
            end_instance(conn, t->client_instance_id, true);
        }
        return;
    }
    ++ability_triggers_;
    cancel_gather(&conn, "an ability was triggered");   // Gathering stage 2: a new ability ends the channel [modelled: no capture]
    int64_t ability = 0;
    int32_t instance = 0;
    int32_t item_slot = -1;
    uint32_t item_location = 0;
    TriggerInfo info;
    if (const auto t = repl::parse_trigger_ability(rpc.call)) {
        ability = t->ability_guid;
        instance = t->client_instance_id;
        pb_.note_trigger_pvp_flag(instance, t->pvp_force);   // (the item this trigger makes carries the flag the trigger carried)
        item_slot = t->item_slot;
        item_location = t->item_location;
        info.animation_index = t->animation_index;
        info.input_x = t->input_x;
        info.input_y = t->input_y;
        info.target_count = t->target_count;
        info.target_actors = actors_of(t->targets, t->inline_targets);
        info.item = t->item_slot != -1;
    } else if (const auto prefix = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                                             rpc.call.param_pos + rpc.call.param_bits)) {
        // The call does not read whole, but its first two parameters do: that is enough to refuse what worldd does not run.
        // An ability it does run is not started from half a call.
        if (runtime->enabled(prefix->ability_guid) || (pipeline_ && pipeline_->handles(prefix->ability_guid))) {
            if (++ability_unreadable_logged_ <= kLoggedPerKind) {
                spdlog::warn("[session] {}: a ServerTriggerAbility for an ability worldd runs does not read ({} parameter bits): ignored", name(),
                             rpc.call.param_bits);
            }
            return;
        }
        ability = prefix->ability_guid;
        instance = prefix->client_instance_id;
    } else {
        if (++ability_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: a ServerTriggerAbility does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    // the mounts milestone, stage 3: what a rider may do, and the mount's dash
    if (mounted()) {
        if (!mounted_trigger(conn, ability, instance, on_mount, now)) return;
    } else if (mounts_ && ability != 0) {
        // the dash is the mount's: nobody rides, so it is not run on the pawn (the generic runtime has the ability: it would put its effect on the character)
        const content::MountAbilityRow* row = mounts_->book().data().ability(ability);
        if (row && row->role == "dash") {
            ++mount_triggers_refused_;
            end_instance(conn, instance, false);
            return;
        }
    }
    // the mounts milestone: Summon_Mount is the mount controller's (the pipeline refuses an ability that names an item)
    if (mounts_ && ability != 0 && ability == mounts_->book().summon_ability()) return on_summon_trigger(conn, instance, item_slot, item_location, now);
    if (pipeline_ && pipeline_->handles(ability)) {
        const Refusal accepted = pipeline_->trigger(ability, instance, ability_seconds(now), info);
        // Drop Crate: the cast plays its montage and the crate leaves the back at the montage's notify, whatever the place (the original drops nothing at a place a crate cannot lie)
        if (ability == content::kGuidDropCrate && accepted == Refusal::None) {
            crate_drop_due_ = now + kCrateDropDelay;
            crate_drop_instance_ = instance;
        }
    } else {
        runtime->trigger(ability, instance, ability_seconds(now), info);
    }
    take_ability_events();
    flush_abilities(conn);
}
// C_Y_B x CoA

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
    if (crate_drop_due_ && cancel->client_instance_id == crate_drop_instance_) crate_drop_due_.reset();   // the cast of Drop Crate was cancelled before its notify: the crate stays
    if (!runtime->cancel(cancel->client_instance_id, ability_seconds(now)) && pipeline_) pipeline_->cancel(cancel->client_instance_id, ability_seconds(now), static_cast<double>(cancel->cancelled_time));
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
    // the client's cast bar ended: a cast that waits for its branch goes on from here (the montage's jump, the cost and the hits follow)
    if (!pipeline_->branch_request(request->client_instance_id, request->branch_guid, ability_seconds(now))) {
        if (unhandled_logged_.insert("ServerRequestBranchVerification").second) {
            spdlog::info("[session] {}: the client's ServerRequestBranchVerification belongs to no cast worldd waits for (it answers it for the cast bars it knows: kBranchedCasts in ability_pipeline.cpp; told once)", name());
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
    if (crate_drop_due_ && *id == crate_drop_instance_) crate_drop_due_.reset();   // (the same for an end request of that cast)
    pipeline_->end_request(*id, ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
}

double PlayerSession::take_damage(Connection& conn, double amount, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return 0;
    if (mounted()) {
        // a rider's damage lands on its mount (stage 3); nothing reaches the rider's Health
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
        if (++hit_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a RequestHitVerification_Server does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    // what the client says it hit: the caster's own components (its pawn, its collision cylinder, by GUID or named in place) and the NPCs' (named
    // in place: the client has no GUID for a component worldd never exported; E1 stage 3 fix)
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    size_t own = 0;
    std::vector<protocol::emit::FIntrepidNetworkGUID> objects;
    std::vector<repl::InlineObject> inline_objects;
    for (const repl::HitComponentRef& c : req->components) {
        if (c.inline_object) {
            if (own_inline_object(*c.inline_object)) ++own;
            else inline_objects.push_back(*c.inline_object);
        } else if (c.object == g.collision_cylinder || c.object == g.pawn) {
            ++own;
        } else {
            objects.push_back(c.object);
        }
    }
    const AbilityPipeline::HitVerdict verdict = pipeline_->hit_request(req->ability_guid, req->targeting_guid, req->hit_guid, req->components.size(), own,
                                                                       ability_seconds(now), actors_of(objects, inline_objects));
    if (verdict != AbilityPipeline::HitVerdict::Scheduled && verdict != AbilityPipeline::HitVerdict::Quiet) {
        if (++hit_unserved_logged_ <= kLoggedPerKind) {
            spdlog::info("[session] {}: a hit request for {} / {} is {}", name(), req->ability_guid, req->hit_guid, AbilityPipeline::verdict_name(verdict));
        }
    }
    take_ability_events();
    flush_abilities(conn);
}

// ============================================================================================================================
//  items (plan B2)
// ============================================================================================================================

bool PlayerSession::set_current_value(int64_t record, float value) {
    if (!stats_ || !stats_->started() || abilities_failed_ || !std::isfinite(value)) return false;
    const stats::StatValue* v = stats_->find(record);
    if (!v || !v->held() || !pb_.has_stat_item(record)) return false;
    bootstrap::AbilityChange c;
    bootstrap::StatChange sc;
    sc.record = record;
    sc.value = value;
    sc.buckets = v->buckets();
    sc.base = v->base;
    sc.equipment = v->equip;
    c.stats.push_back(sc);
    if (!pb_.apply_ability_change(c)) return false;
    stats_->set_current(record, value);
    abilities_dirty_ = true;
    return true;
}

void PlayerSession::sync_ability_values() {
    if (pipeline_) pipeline_->sync_a7_values(pipeline_->now());
}

void PlayerSession::refresh_rank_mods() {
    if (stats_ && book_) {
        stats_->set_passives(book_->passives(wielded_line_));
        stats_->set_ranks(book_->held_ranks());
    }
    if (pipeline_ && book_) pipeline_->set_reaction_carriers(book_->reaction_carriers(wielded_line_));   // the reactions the ranks' effects carry
    sync_ability_values();   // (the ranks changed what the formulas give Sprint's speed)
}
// CoA with C_Y_B

std::vector<std::string> PlayerSession::weapon_tags() const {
    // the item in the weapon slot of the set in hand: the ranged slot while the ranged set is in hand, else the main hand
    if (!world_) return {};
    const stats::GearCatalog* gear = world_->stat_context().gear;
    const svc::InventoryItem* held = inventory_.at(svc::ItemContainer::Equipment, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    if (!gear || !held) return {};
    const stats::ItemGear* item = gear->item(held->item_record_id);
    return item ? item->tags : std::vector<std::string>{};
}

void PlayerSession::apply_gear_stats(const char* what) {
    if (!stats_ || !stats_->started()) return;
    refresh_rank_mods();   // plan D1, stage 2: the ranks the character has (and the weapon line in hand) are an input like the gear
    // plan D2: a class change takes the old class's effects off and resolves the new class's resource rule (its own stat changes go out with it)
    if (pipeline_ && pipeline_->resource_class() != stats_->class_enum()) pipeline_->refresh(pipeline_->now(), "the class changed", false);
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string error;
    // The new stats are only prepared: they become the session's once the pawn's arrays took them, so that the session's stats are
    // always what the client has (a refused change leaves both as they were, and the next gear change is a diff against that).
    if (!stats_->prepare(bootstrap::worn_items_of(inventory_), changed, next, error)) {
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the stats cannot be computed for {} ({}): the client keeps the stats it has", name(), what, error);
        return;
    }
    if (next.faults != 0) {   // as the join does: a run that faulted has stats that are not the character's (a fault is a 0)
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the stat program faulted {} time(s) for {} (a division by zero, a bad curve read or a value out of range): "
                      "its stats are not sent, the client keeps the stats it has", name(), next.faults, what);
        return;
    }
    if (changed.empty()) {
        stats_->commit(std::move(next));
        spdlog::debug("[session] {}: {} changes no stat", name(), what);
        return;
    }
    const std::optional<AbilityRuntime::GearSnapshot> runtime_before = abilities_ ? std::optional(abilities_->gear_snapshot()) : std::nullopt;
    // what the owner's arrays carry; a stat the ability runtime holds goes through it (Stamina is its own)
    bootstrap::AbilityChange change;
    size_t owner = 0, owner_proxy = 0, everyone = 0, without_item = 0;
    for (const stats::StatValue& v : changed) {
        if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) continue;
        // plan D1, stage 2: a class change lists stats the arrays were not laid out for (the new class's own resource): each gets an item in the
        // array its audience names (a fast-array add: the client takes a new item like a changed one)
        const bool new_item = !pb_.has_stat_item(v.guid);
        if (new_item) ++without_item;
        (v.replication == 1 ? owner : v.replication == 5 ? owner_proxy : everyone) += 1;
        // the GM travel tools: the GM's run speed multiplier stays on top of what the engine says of MoveSpeedMult
        const auto shown = v.guid == spawns::kStatMoveSpeedMult ? static_cast<decltype(v.value)>(v.value * gm_speed_) : v.value;
        if (!new_item && abilities_ && abilities_->tracks(v.guid)) {
            abilities_->gear_stat(change, v.guid, shown);
        } else if (!(abilities_ && v.guid == content::kGuidStamina)) {
            bootstrap::StatChange sc;
            sc.record = v.guid;
            sc.value = shown;
            sc.buckets = v.buckets();
            sc.base = v.base;
            sc.equipment = v.equip;
            sc.add_to_replication = new_item ? v.replication : 0;
            change.stats.push_back(sc);
        }
    }
    if (change.empty()) {
        stats_->commit(std::move(next));
        return;
    }
    if (!pb_.apply_ability_change(change)) {
        if (runtime_before) abilities_->restore_gear(*runtime_before);
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the pawn does not take the stats of {} ({}): the client keeps the stats it has", name(), what,
                      pb_.error().empty() ? std::string("a stat has no item in its arrays") : pb_.error());
        return;
    }
    stats_->commit(std::move(next));
    ++stat_updates_;
    stat_items_changed_ += change.stats.size();
    spdlog::log(level_for(stat_logged_), "[session] {}: {} changes {} stat(s): {} Owner, {} OwnerProxy, {} EveryoneProxy{}", name(), what,
                change.stats.size(), owner, owner_proxy, everyone,
                without_item ? " (" + std::to_string(without_item) + " of them new to the client's arrays: added)" : std::string());
}

// ============================================================================================================================
//  using an item (plan D1)
// ============================================================================================================================

double PlayerSession::xp_add_mult() const {
    if (!stats_) return 0;
    const stats::StatValue* v = stats_->find(content::kGuidAdventuringXPAddMult);
    return v && std::isfinite(v->value) ? v->value : 0.0;
}
// built by C-O-A with C_Y_B

std::string PlayerSession::fresh_guid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (;;) {
        const uint64_t a = rng(), b = rng();
        const repl::GuidValue g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
        if (!(g.a | g.b | g.c | g.d)) continue;
        const std::string hex = items::guid_hex(g);
        bool taken = false;
        for (const svc::InventoryItem& it : inventory_.items()) taken = taken || it.instance_guid == hex;
        if (!taken) return hex;
    }
}

// What a level gained does to the character (plan D1, stage 2): the pawn's level, the stats, the ranks and abilities the level grants, the passives' effect items,
// the TempHealth mark.  `class_changed`: the stats are computed once for both.
void PlayerSession::finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects,
                                    std::vector<repl::SkillOnActionbar>& skill_updates) {
    const bool leveled = xp.leveled();
    if (leveled) {
        if (!pb_.set_level(progress_.level)) spdlog::error("[session] {}: the pawn does not take level {}", name(), progress_.level);
        if (stats_) stats_->set_level(progress_.level);
        if (book_) {
            // the ranks the new level grants by itself: the Universal tree's and the class tree's
            const talents::Catalog& catalog = book_->catalog();
            std::vector<talents::RankRef> granted;
            const int from = xp.before.level, to = xp.after.level;
            book_->set_level(to);
            for (const talents::Tree* tree : {catalog.universal(), book_->class_tree()}) {
                if (!tree) continue;
                const std::vector<talents::RankRef> had = catalog.auto_ranks(*tree, from);
                for (const talents::RankRef& r : catalog.auto_ranks(*tree, to)) {
                    const bool before = std::any_of(had.begin(), had.end(), [&](const talents::RankRef& h) { return h.node->skill == r.node->skill; });
                    if (!before) granted.push_back(r);
                }
            }
            std::stable_sort(granted.begin(), granted.end(), [](const talents::RankRef& a, const talents::RankRef& b) {
                return a.node->level_requirement < b.node->level_requirement;
            });
            grant_ranks(granted, now, rank_effects, skill_updates);
            for (const talents::Tree* tree : {catalog.universal(), book_->class_tree()}) {
                if (tree && !pb_.set_tree_ranks(tree->guid, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
            }
        }
    }
    if (leveled || class_changed) {
        apply_gear_stats(leveled && class_changed ? "the new level and class" : leveled ? "the new level" : "the new class");
    }
    if (leveled) {
        if (!pb_.mark_temp_health()) spdlog::debug("[session] {}: no TempHealth item to mark", name());
    }
    if (!rank_effects.empty() && !pb_.apply_ability_change(rank_effects)) {
        spdlog::warn("[session] {}: the pawn does not take the granted passives' effect items ({})", name(), pb_.error().empty() ? "keys disagree" : pb_.error());
        for (const bootstrap::EffectItem& e : rank_effects.effects_added) {
            for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
            markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
        }
    }
    if (!rank_effects.empty()) abilities_dirty_ = true;
}

void PlayerSession::on_item_activated(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now) {
    const char* what = "ServerItemUsed (a use)";
    const auto refuse = [&](const std::string& why) {
        ++uses_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    if (!db) return refuse("worldd has no content.db to read the item's actions from");
    use::Context ctx;
    ctx.class_enum = class_id_;
    ctx.progress = progress_;
    ctx.xp_add_mult = xp_add_mult();
    ctx.curve = curve;
    // Gathering stage 2: the professions, and the runner of a Profession reward's expression (made when the first one is asked for: it runs the stat graph once)
    ctx.professions = world_->professions();
    ctx.currencies = world_->currencies();
    ctx.held = professions_;
    const abilities::Catalog* ability_catalog = world_->ability_catalog();
    std::shared_ptr<professions::XpRunner> runner;
    if (ctx.professions && ability_catalog && stats_ && stats_->started()) {
        const std::vector<stats::WornItem> worn = bootstrap::worn_items_of(inventory_);
        runner = std::make_shared<professions::XpRunner>();
        ctx.profession_xp = [this, runner, ability_catalog, worn](int64_t expr, const std::map<std::string, int>& levels, std::string& why) -> std::optional<double> {
            if (!*runner) {
                *runner = professions::make_xp_runner(*ability_catalog, *stats_, worn, why);
                if (!*runner) return std::nullopt;
            }
            return (*runner)(expr, levels, why);
        };
    }
    const use::Plan plan = use::plan_use(*db, item.item_record_id, ctx);
    if (!plan.ok) return refuse(plan.refusal);
    // what the use grants, as fresh instances (their kind decides where the inventory puts them: a resource item to a resource container, the rest to the Backpack)
    std::vector<svc::InventoryItem> fresh;
    {
        const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
        for (const use::Granted& g : plan.items) {
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = g.record;
            it.instance_guid = fresh_guid();
            it.quantity = g.quantity;
            it.quality = g.quality;
            it.durability = g.durability;
            it.created_at_ms = created;
            fresh.push_back(std::move(it));
        }
    }
    // the room: the grants need room in the storage of their kind; the consumed stack frees its slot only when it empties (tried on a copy)
    if (!fresh.empty()) {
        items::Inventory trial = inventory_;
        if (plan.consume) trial.consume(svc::ItemContainer::Backpack, use.slot, item.instance_guid, 1);
        if (!trial.can_grant(fresh, item_catalog())) {
            return refuse("the storages have no room for the use's " + std::to_string(plan.items.size()) + " item(s) (the bag has " +
                          std::to_string(static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack)) + " free slot(s)): nothing is used");
        }
    }
    const std::span<const content::AutoGrant> grants = world_->auto_grants();
    // weapon XP: a scroll's Weapon experience goes to every weapon line of what is worn, as a kill's does
    const std::vector<int64_t> worn_lines = plan.weapon_xp > 0 ? equipped_weapon_lines() : std::vector<int64_t>();
    // the steps: the events, the XPTracker, the broadcast, the pawn's state, the class bar, and a skill update for each rank a level or a class grants
    const size_t steps = 1 + (plan.items.size() + 1) / bootstrap::kMaxEventsPerChain + 1 + 1 + 1 + 1 + grants.size() + 8 + plan.profession_changes.size() + worn_lines.size();
    if (!room_for(steps)) return;
// (Cyb : C.O.A)

    // the inventory: the consumed stack, then the grants
    items::Outcome all;
    all.ok = true;
    if (plan.consume) {
        const items::Outcome c = inventory_.consume(svc::ItemContainer::Backpack, use.slot, item.instance_guid, 1);
        if (!c.ok) return refuse(c.refusal);
        all.events.insert(all.events.end(), c.events.begin(), c.events.end());
    }
    if (!fresh.empty()) {
        const items::Outcome g = inventory_.grant(std::move(fresh), item_catalog());
        if (!g.ok) {
            // the room was checked: a refusal here is a record the table refuses, not something the client can cause; the consumed stack stays consumed
            spdlog::error("[session] {}: the use's items are not granted ({}): the item was consumed and nothing came of it", name(), g.refusal);
        } else {
            all.events.insert(all.events.end(), g.events.begin(), g.events.end());
        }
    }
    const bool inventory_moved = !all.events.empty();
    if (inventory_moved) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
    }

    // the progress: the XP, the levels it crosses and the ranks they grant
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (plan.adventuring_xp > 0 && curve) {
        xp = leveling::add_xp(*curve, grants, progress_, plan.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);

    // the professions (Gathering stage 2): the XP, the Promotes and the levels they cross
    const std::vector<professions::Change> profession_changes = plan.profession_changes.empty() ? std::vector<professions::Change>() : set_professions(plan.professions_after);
    const bool professions_changed = !profession_changes.empty();

    // the class
    bool class_changed = false;
    std::string class_name;
    std::optional<content::ClassRecord> new_class;
    if (plan.set_class) {
        std::string error;
        const char* cname = bootstrap::client_class_name(*plan.set_class);
        const auto cls = cname ? db->class_for_name(cname, error) : std::nullopt;
        if (!cls) {
            spdlog::error("[session] {}: the class {} is not in content.db ({}): the class stays", name(), *plan.set_class, error);
        } else if (!pb_.set_class(cls->guid)) {
            spdlog::error("[session] {}: the pawn does not take class {}: the class stays", name(), cls->name);
        } else {
            class_id_ = *plan.set_class;
            class_name = cls->name;
            new_class = *cls;
            if (stats_) stats_->set_class(class_id_);
            class_changed = true;
            progress_changed_ = true;
        }
    }
    // plan D1, stage 2: what the ranks do (the abilities, the passives' items, the bar) when the class and the level change
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    std::vector<std::pair<int, int>> cleared_slots;
    if (class_changed && book_) switch_class(*new_class, now, rank_effects, skill_updates, cleared_slots);
    finish_level_up(xp, class_changed, now, rank_effects, skill_updates);
    // weapon XP: each worn line takes the scroll's Weapon experience on top of its total (it stops at the top); the levels it crosses are applied before any reply is built
    std::vector<weaponxp::Change> weapon_changes;
    for (const int64_t type : worn_lines) {
        const weaponxp::Change c = weapon_.add_xp(type, plan.weapon_xp);
        if (c.changed()) weapon_changes.push_back(c);
    }
    bool weapons_leveled = false;
    for (const weaponxp::Change& c : weapon_changes) weapons_leveled = weapons_leveled || c.leveled();
    if (!weapon_changes.empty()) progress_changed_ = true;
    const bool weapon_pawn = apply_weapon_levels(weapon_changes, now);

    // what the client is told, in order: the storage events, the XPTracker, the level-up, the pawn's state, the granted skills
    ++uses_applied_;
    ++items_applied_;
    if (inventory_moved) {
        const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
        for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    if (xp.changed() || leveled || class_changed || professions_changed || !weapon_changes.empty()) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(profession_changes);   // the professions that changed, in profession order
        std::vector<std::pair<int64_t, int64_t>> lines;   // weapon XP: each paid line's new total, in the same update
        for (const weaponxp::Change& c : weapon_changes) lines.emplace_back(c.line, c.xp_after);
        queue("XPTracker", [total, debt, wire, lines](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
            for (const auto& [type, line_total] : lines) {
                if (!pb.set_weapon_line(type, line_total)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
    }
    for (const weaponxp::Change& c : weapon_changes) {
        if (!c.leveled()) continue;
        const int64_t type = c.line;
        const int level = c.level_after;
        queue("BroadcastPlayLevelUpEffects", [type, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(type, level); }, conn);
    }
    if (professions_changed) queue_profession_level_ups(conn, profession_changes);
    if (leveled || class_changed) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    } else if (weapon_pawn) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn, /*must_build=*/false);
    }
    if (class_changed && book_) {
        // the old class's slots are sent cleared and the new class's skills placed: the whole bar as it is now, in one payload (a slot of UseType
        // None is a clear; retail's bar payloads hold them too). ClientClearActionbarsForInstigators is not used: no capture shows its shape.
        repl::ActionbarPayload payload = bar_payload();
        for (const auto& [bank, slot] : cleared_slots) {
            if (bar_.at(bank, slot)) continue;   // taken again by a skill of the new class
            repl::ActionbarSlot none;
            none.bank_index = bank;
            none.slot_index = slot;
            payload.slots.push_back(none);
        }
        queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn, /*must_build=*/true);
    }
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }

    std::string granted_names;
    for (const repl::SkillOnActionbar& u : skill_updates) {
        std::string skill = std::to_string(u.skill_id);
        if (book_) {
            for (const talents::Tree* t : book_->listed_trees()) {
                for (const talents::Node& n : t->nodes) {
                    if (n.skill == u.skill_id && t->guid == u.skill_tree_id) skill = n.skill_name;
                }
            }
        }
        granted_names += (granted_names.empty() ? "" : ", ") + skill;
    }
    std::string line = "used item " + std::to_string(item.item_record_id) + ": " + plan.summary;
    if (xp.changed()) line += "; Adventuring " + std::to_string(xp.after.xp) + " XP (debt " + std::to_string(xp.after.debt) + ")";
    if (leveled) line += ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level);
    for (const professions::Change& c : profession_changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf) continue;
        const professions::Book& pbook = *professions_.book();
        line += "; " + pf->name + " level " + std::to_string(c.before.level) + " -> " + std::to_string(c.after.level) + ", " + pbook.tier(c.before.tier).name + " -> " +
                pbook.tier(c.after.tier).name + ", " + std::to_string(c.after.xp) + " XP";
    }
    if (!granted_names.empty()) line += ", granted " + granted_names;
    if (plan.weapon_xp > 0) {   // weapon XP (the summary above says the amount): what each worn line holds now
        if (!weapon_.valid()) line += "; this world has no weapon lines";
        else if (worn_lines.empty()) line += "; no weapon is worn, so no line gets it";
        bool first = true;
        for (const int64_t type : worn_lines) {
            const weaponxp::Line* wl = weapon_.lines()->find(type);
            const bool up = std::any_of(weapon_changes.begin(), weapon_changes.end(), [&](const weaponxp::Change& c) { return c.line == type && c.leveled(); });
            line += (first ? "; " : ", ") + (wl ? wl->name : std::to_string(type)) + " " + std::to_string(weapon_.xp(type)) + " XP (level " + std::to_string(weapon_.level(type)) + (up ? ", level-up" : "") + ")";
            first = false;
        }
    }
    if (class_changed) line += "; the class is " + class_name + " now (its tree, abilities and bar are sent live)";
    line += "; " + std::to_string(all.events.size()) + " storage event(s)";
    for (const std::string& n : plan.notes) line += "; " + n;
    spdlog::log(level_for(use_logged_), "[session] {}: {}", name(), line);
    if (xp.changed() || class_changed || professions_changed || !weapon_changes.empty()) {
        hand_over_progress(leveled || weapons_leveled ? "a level-up" : class_changed ? "a class change" : professions_changed && !xp.changed() ? "a profession changed" : "XP earned");
    }
}
// C | O | A and Cyb

std::vector<professions::Change> PlayerSession::set_professions(const professions::Professions& after) {
    std::vector<professions::Change> changes = professions::Professions::diff(professions_, after);
    if (changes.empty()) return changes;
    professions_ = after;
    progress_changed_ = true;
    if (stats_) stats_->set_profession_levels(professions_.levels_by_name());
    for (const professions::Change& c : changes) {
        if (c.leveled()) profession_levels_gained_ += static_cast<size_t>(c.after.level - c.before.level);
    }
    return changes;
}

std::vector<PlayerSession::ProfessionWire> PlayerSession::profession_wire(const std::vector<professions::Change>& changes) const {
    std::vector<ProfessionWire> wire;
    if (!professions_.valid()) return wire;
    const professions::Book& book = *professions_.book();
    for (const professions::Change& c : changes) {
        if (const professions::Profession* pf = book.find(c.profession)) wire.push_back({pf->leveling_type, c.after.xp, book.tier(c.after.tier).guid});
    }
    return wire;
}

void PlayerSession::queue_profession_level_ups(Connection& conn, const std::vector<professions::Change>& changes) {
    if (!professions_.valid()) return;
    // a profession's level-up is the XP item and BroadcastPlayLevelUpEffects on the pawn with the profession's leveling type as the category (retail's artisan and weapon levels)
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf || !c.leveled()) continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn);
    }
}

void PlayerSession::apply_outcome(Connection& conn, const items::Outcome& outcome, const char* what) {
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    if (outcome.equipment_changed && book_ && world_ && world_->content()) {
        // plan D1, stage 2: the weapon line in hand follows what is worn (a weapon tree's passives apply while its line is in hand).  The weapon swap fix: so do the weapon's combo, the
        // ability lists, OwnerInfo's line and the ranged flag; they are staged in the pawn here and ride in the chain of the HeldItems step queued below (it diffs the whole pawn)
        for (const items::Event& e : outcome.events) {
            if (e.container == svc::ItemContainer::Equipment && (e.slot == items::equip::Primary || e.slot == items::equip::Ranged)) {
                follow_worn_weapon();
                break;
            }
        }
    }
    if (outcome.equipment_changed) apply_gear_stats();   // before the replies are queued: they are built later, and carry the stats' deltas
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    // the storage events go first (the PlayerController's chain), then the pawn's visible gear: the order and the packet of the retail answer
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    if (outcome.equipment_changed) {
        const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
        const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
        // the retail server marks the 23 held items once for each change of the Equipment storage its answer holds
        size_t marks = 0;
        for (const items::Event& e : outcome.events) marks += e.container == svc::ItemContainer::Equipment ? 1 : 0;
        queue("HeldItems and AppearanceIDs",
              [held, appearance, marks](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
                  if (!pb.apply_equipment(held, appearance, marks ? marks : 1)) return std::nullopt;
                  return pb.equipment_update();
              },
              conn, /*must_build=*/true);
    }
    spdlog::log(level_for(item_applied_logged_), "[session] {}: {}: {} storage event(s){}", name(), what, outcome.events.size(),
                outcome.equipment_changed ? ", the visible gear is updated" : "");
    note_cargo_crate(outcome);
    if (outcome.equipment_changed) sync_cargo(conn, last_now_);
    // a resource bag worn, replaced or taken off: its container opens or closes on the client now, after the events that moved the bag
    for (const items::BagChange& c : outcome.bags) update_resource_container(conn, c);
}

void PlayerSession::note_cargo_crate(const items::Outcome& outcome) {
    for (const items::Event& e : outcome.events) {
        if (e.type != repl::StorageEventType::Add || e.container != svc::ItemContainer::Equipment || e.slot != items::equip::CargoHold) continue;
        const content::ContentDb* db = world_ ? world_->content() : nullptr;
        std::string error;
        const auto gear = db ? db->item_gear(e.item.item_record_id, error) : std::nullopt;
        if (!gear) continue;
        for (const std::string& tag : gear->tags) {
            std::string lower;
            for (const char c : tag) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "item.crate.construction") {
                construction_crate_equipped(e.item.item_record_id);
                break;
            }
        }
    }
}

void PlayerSession::sync_cargo(Connection& conn, Clock::time_point when) {
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if (!crate && cargo_effects_.empty()) return;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !phase_in_done_) return;
    // the effects the crate's record lists for the rarity the crate shows (item_equipped_effect: the Encumbered effect lists Common alone, so a crate of a better rarity slows by its
    // stat block and shows no icon: the original's craft of an Uncommon crate puts no effect on the pawn)
    std::map<int64_t, int> wanted;
    if (crate) {
        std::string error;
        const auto listed = db->item_equipped_effects(crate->item_record_id, error);
        if (!listed) spdlog::warn("[session] {}: the effects of the crate cannot be read: {}", name(), error);
        const int rarity = world_->item_rarity_facts(crate->item_record_id).rarity_of(crate->quality);
        for (const content::EquippedEffect& e : listed ? *listed : std::vector<content::EquippedEffect>{}) {
            if (rarity >= e.min_rarity && rarity <= e.max_rarity) wanted[e.effect] = e.stacks;
        }
    }
    bootstrap::AbilityChange change;
    std::vector<std::pair<int64_t, int64_t>> removed, added;   // (effect record, key)
    for (auto it = cargo_effects_.begin(); it != cargo_effects_.end();) {
        if (wanted.count(it->first)) {
            ++it;
            continue;
        }
        change.effects_removed.push_back(it->second);
        removed.emplace_back(it->first, it->second);
        it = cargo_effects_.erase(it);
    }
    for (const auto& [effect, stacks] : wanted) {
        if (cargo_effects_.count(effect)) continue;
        bootstrap::EffectItem e;
        e.effect_id = next_passive_effect_id_++;
        e.effect_guid = effect;
        e.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(when));
        e.duration = std::numeric_limits<float>::max();   // (the effects an item lists are infinite: they last while the item is worn)
        e.tick_time = 0;
        e.stacks = stacks;
        change.effects_added.push_back(e);
        added.emplace_back(effect, e.effect_id);
        cargo_effects_[effect] = e.effect_id;
    }
    if (!change.empty()) {
        if (pb_.apply_ability_change(change)) {
            queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("the crate"); }, conn, when);
            spdlog::info("[session] {}: the crate on the back {}: {} effect item(s) added, {} removed", name(), crate ? "puts its effects on the pawn" : "is gone", added.size(), removed.size());
        } else {
            spdlog::warn("[session] {}: the pawn does not take the crate's effect items ({}): the crate is carried without them", name(),
                         pb_.error().empty() ? "keys disagree" : pb_.error());
            for (const auto& a : added) cargo_effects_.erase(a.first);
            for (const auto& r : removed) cargo_effects_[r.first] = r.second;
        }
    }
    // Drop Crate goes on the bar when a crate is carried and the ability is on no slot yet (the original sends a ClientReceiveActionbarSlotPayload with bank 2, slot 11 and bAutoGranted around
    // the crate's Add, and none in a craft that followed, with the ability on the bar already).  A slot someone else holds is not taken: the first free class slot then.
    if (crate && book_ && std::find(known_.begin(), known_.end(), content::kGuidDropCrate) != known_.end()) {
        bool on_bar = false;
        for (const repl::ActionbarSlot& s : bar_.slots()) on_bar = on_bar || (s.use.use_type == repl::ActionbarUseType::Ability && s.use.ability_id == content::kGuidDropCrate);
        if (!on_bar) {
            repl::ActionbarSlot slot;
            slot.bank_index = kDropCrateBank;
            slot.slot_index = kDropCrateSlot;
            slot.use.use_type = repl::ActionbarUseType::Ability;
            slot.use.ability_id = content::kGuidDropCrate;
            if (bar_.at(slot.bank_index, slot.slot_index)) {
                const auto spare = bar_.first_free();
                if (spare) {
                    slot.bank_index = spare->first;
                    slot.slot_index = spare->second;
                }
            }
            std::string why;
            if (!bar_.at(slot.bank_index, slot.slot_index) && bar_.set(slot, &why)) {
                repl::ActionbarPayload payload;
                payload.slots.push_back(slot);
                payload.auto_granted = true;
                queue_later("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn, when, /*must_build=*/true);
                progress_changed_ = true;
                hand_over_progress("the action bar");
                spdlog::info("[session] {}: Drop Crate is put on the action bar (bank {}, slot {}): a crate is carried", name(), slot.bank_index, slot.slot_index);
            }
        }
    }
}

void PlayerSession::run_cargo(Connection& conn, Clock::time_point now) {
    if (finished_ || failed_ || !phase_in_done_ || !join_.empty()) return;
    if (!cargo_synced_ && room_for(4)) {
        cargo_synced_ = true;
        sync_cargo(conn, now);
    }
    if (crate_drop_due_ && now >= *crate_drop_due_) {
        crate_drop_due_.reset();
        drop_crate(conn, now);
    }
}

void PlayerSession::drop_crate(Connection& conn, Clock::time_point now) {
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if (!crate || dead_ || !room_for(8)) return;
    const GoodPosition at = checker_.good();
    if (mounted() || at.movement_mode != repl::kMoveWalking) {   // [M] retail refuses a drop at a place the crate cannot lie, with no event: the ground under the character is the model
        spdlog::info("[session] {}: General_DropCrate: the crate stays on the back ({})", name(), mounted() ? "the character is mounted" : "the character is not on the ground");
        return;
    }
    const std::string guid = crate->instance_guid;
    items::Outcome o = inventory_.use(svc::ItemContainer::Equipment, items::equip::CargoHold, guid, 0xFFFFFFFFu, item_catalog());
    if (!o.ok) {
        say(conn, "There is no room in your bags for the crate.", now);
        spdlog::info("[session] {}: General_DropCrate: the crate stays on the back: {}", name(), o.refusal);
        return;
    }
    apply_outcome(conn, o, "a crate was dropped");
    spdlog::info("[session] {}: General_DropCrate: the crate left the back (into the item bag: the ground crates are not built yet)", name());
}

void PlayerSession::update_resource_container(Connection& conn, const items::BagChange& change) {
    if (change.index < 0 || change.index >= items::kResourceContainers) return;
    const size_t i = static_cast<size_t>(change.index);
    const items::ResourceGrid& grid = inventory_.grid(items::resource_container(change.index));
    bootstrap::ResourceBagSetup bag;
    FIntrepidNetworkGUID guid = pb_.setup().guids.resource_storage[i];
    if (change.open && !grid.empty()) {
        bag.present = true;
        bag.columns = grid.columns();
        bag.rows = grid.rows();
        bag.override_matrix = grid.override_matrix();
        if (world_) guid = world_->mint_guid();   // a container that opens is a new component with a guid of its own
    }
    spdlog::info("[session] {}: the resource bag in Equipment slot {} {}: container Resource{} {}", name(), items::equip::ResourceBag0 + change.index,
                 bag.present ? "is worn" : "is taken off", change.index, bag.present ? std::to_string(bag.columns) + " x " + std::to_string(bag.rows) + " opens now" : std::string("closes now"));
    const int index = change.index;
    queue("the resource containers", [index, bag, guid](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_resource_container(index, bag, guid)) return std::nullopt;
        return pb.resource_containers_update();
    }, conn, /*must_build=*/true);
    if (bag.present) {
        // the server hands the grid to the client with the container (the client asks for it too, as a client does for any container it learns of: the answer is the same)
        const bootstrap::StorageKind kind = bootstrap::resource_kind(index);
        queue("ClientReceiveStoragePayload", [kind](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(kind, Role::Pawn); }, conn);
    }
}
// (C Y B | CoA)

void PlayerSession::refuse_item_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now) {
    ++items_refused_;
    spdlog::log(level_for(item_refused_logged_), "[session] {}: {} refused, nothing changed: {}", name(), what, why);
    // the client is told what is where, as at the join; a client that is refused over and over gets that a few times, then silence
    if (!resync_budget_.take(now) || !room_for(5)) return;
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));   // Gathering stage 3: a refused drag in a resource container too
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
    }
}

void PlayerSession::on_item_used(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto use = repl::parse_item_used(rpc.call);
    if (!use) {
        if (!item_unreadable_logged_) {
            item_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerItemUsed does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(item_budget_, "ServerItemUsed", now) || !room_for(8)) return;
    if (!catalog_) return refuse_item_request(conn, "ServerItemUsed", "worldd has no item catalog", now);
    const auto storage = container_of_location(use->storage);
    if (!storage) {
        if (const auto bag = material_bag_of_location(use->storage)) return on_tier_up(conn, *use, *bag, now);   // Glint tiers: a stack used in a material bag
        return refuse_item_request(conn, "ServerItemUsed", "items of storage " + std::to_string(use->storage) + " are not used", now);
    }
    // the mounts milestone: a mount item is neither worn nor consumed; the summon or the desummon is the packet's next call
    if (mounts_ && *storage == svc::ItemContainer::Backpack) {
        const svc::InventoryItem* item = inventory_.at(*storage, use->slot);
        if (item && item->instance_guid == items::guid_hex(use->expected_item) && mounts_->is_mount_item(item->item_record_id)) {
            const svc::InventoryItem copy = *item;
            return on_mount_item_used(copy);
        }
    }
    // plan D1: a bag item that content.db lists no slot for is used, not worn (the item must be where the client says, as for an equip)
    if (*storage == svc::ItemContainer::Backpack) {
        const svc::InventoryItem* item = inventory_.at(*storage, use->slot);
        if (item && item->instance_guid == items::guid_hex(use->expected_item)) {
            const auto info = catalog_->info(item->item_record_id);
            if (!info || !info->wearable()) {
                const svc::InventoryItem copy = *item;   // the inventory changes under the use
                return on_item_activated(conn, *use, copy, now);
            }
        }
    }
    const items::Outcome outcome = inventory_.use(*storage, use->slot, items::guid_hex(use->expected_item), use->requested_equip_slot, *catalog_);
    if (!outcome.ok) {
        // a resource bag that still holds items stays where it is; the player is told why nothing happened
        if (outcome.holds_items) send_economy_callout(conn, "CantUnequipResourceBag", now);
        return refuse_item_request(conn, "ServerItemUsed", outcome.refusal, now);
    }
    apply_outcome(conn, outcome, *storage == svc::ItemContainer::Backpack ? "an item is worn" : "an item is taken off");
}

void PlayerSession::on_tier_up(Connection& conn, const repl::ItemUse& use, svc::ItemContainer container, Clock::time_point now) {
    const char* what = "ServerItemUsed (a Glint tier change)";
    const auto refuse = [&](const std::string& why) {
        ++uses_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const char* bag = items::container_name(container);
    const svc::InventoryItem* held = inventory_.at(container, use.slot);
    if (!held) return refuse(std::string("no item in ") + bag + " slot " + std::to_string(use.slot));
    if (held->instance_guid != items::guid_hex(use.expected_item)) return refuse(std::string("the item in ") + bag + " slot " + std::to_string(use.slot) + " is not the instance the client named");
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db) return refuse("worldd has no content.db to read the item's use from");
    std::string error;
    const auto activation = db->item_use(held->item_record_id, error);
    if (!error.empty()) return refuse("content.db failed: " + error);
    // only a stack of an item that turns into the next kind is used in a material bag; the other uses (a crate opened from a bag) are not made
    if (!activation || activation->activation != "TierUpItem") return refuse(std::string("items of the ") + bag + " are used only to change a Glint stack into the next kind");
    const items::CurrencyTable* currencies = world_->currencies();
    if (!currencies) return refuse("worldd has no currency tiers (rebuild content.db with this checkout's contentc): no stack changes kind");
    const items::CurrencyKind* kind = currencies->kind_of(held->item_record_id);
    const items::CurrencyKind* next = currencies->next_of(held->item_record_id);
    if (!kind) return refuse("item record " + std::to_string(held->item_record_id) + " is no kind of a currency");
    if (!next) return refuse("item record " + std::to_string(held->item_record_id) + " is the top kind of its currency: there is no next kind");
    const auto product_record = db->item(next->item, error);
    if (!product_record) return refuse(error.empty() ? "the next kind, item record " + std::to_string(next->item) + ", is not in content.db" : "content.db failed: " + error);
    const auto stack_record = db->item(held->item_record_id, error);
    // the new item: ONE of the next kind at its record's own quality (21 for a Dim Glint, 41 above), a fresh instance
    svc::InventoryItem product;
    product.container = container;
    product.item_record_id = next->item;
    product.instance_guid = fresh_guid();
    product.quantity = 1;
    product.quality = std::max(1, product_record->default_quality);
    product.durability = product_record->max_durability_at(product.quality);
    product.created_at_ms = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    const svc::InventoryItem stack = *held;   // the inventory changes under the change
    const items::Outcome outcome = inventory_.tier_up(container, use.slot, stack.instance_guid, static_cast<int32_t>(kind->rate), product, *product_record, item_catalog());
    if (!outcome.ok) return refuse(outcome.refusal);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++uses_applied_;
    ++items_applied_;
    // the whole stack goes with no toast of its own; the one new item is told as it arrives, with the "Acquired" toast of the item feed
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/false);
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    const items::Event& arrival = outcome.events.back();
    const std::string from_name = stack_record ? (!stack_record->display_name.empty() ? stack_record->display_name : stack_record->name) : std::to_string(stack.item_record_id);
    const std::string to_name = !product_record->display_name.empty() ? product_record->display_name : product_record->name;
    spdlog::log(level_for(use_logged_), "[session] {}: a Glint tier change: {} x{} in {} slot {} became {} x1 in {} slot {}, {} storage event(s)", name(), from_name, stack.quantity, bag, use.slot,
                to_name, items::container_name(arrival.container), arrival.slot, outcome.events.size());
}

void PlayerSession::on_storage_event_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto event = repl::parse_storage_event(rpc.call);
    if (!event) {
        if (!item_unreadable_logged_) {
            item_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerRequestStorageEvent does not read ({} parameter bits): ignored (further ones are not logged)", name(),
                         rpc.call.param_bits);
        }
        return;
    }
    if (warehouse_event(conn, *event, now)) return;   // storage clerks, step 0: a move that names a warehouse container is the warehouse's
    if (!within_budget(item_budget_, "ServerRequestStorageEvent", now) || !room_for(4)) return;
    const char* what = "ServerRequestStorageEvent";
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const auto container_of = [&](const protocol::emit::FIntrepidNetworkGUID& object) -> std::optional<svc::ItemContainer> {
        if (object == g.backpack) return svc::ItemContainer::Backpack;
        if (object == g.equipment) return svc::ItemContainer::Equipment;
        if (object == g.quest_storage) return svc::ItemContainer::Quest;
        for (int i = 0; i < items::kResourceContainers; ++i) {
            if (pb_.setup().resource_bags[static_cast<size_t>(i)].present && object == g.resource_storage[static_cast<size_t>(i)]) return items::resource_container(i);
        }
        return std::nullopt;
    };
    const auto type = static_cast<repl::StorageEventType>(event->type);
    if (type == repl::StorageEventType::Remove) {
        // The client destroys an item by dragging it out of a bag (after it asked the player to confirm): the request is the Remove event of the stack, naming the bag, the slot
        // and the instance.  Only the item bag and the material bags are destroyed from; the answer is the same event with the "Removed" toast.
        if (event->targets.size() != 1 || event->targets[0].slots.size() != 1) {
            return refuse_item_request(conn, what, "a destroy names one container and one slot", now);
        }
        const auto where = container_of(event->targets[0].object);
        if (!where) return refuse_item_request(conn, what, "a container that is none of this character's storages", now);
        if (!is_bag(*where)) return refuse_item_request(conn, what, "only an item in a bag is destroyed (not a worn one, not a quest item)", now);
        const repl::StorageEventSlot& slot = event->targets[0].slots[0];
        if (!(slot.item_guid == event->item.guid)) return refuse_item_request(conn, what, "the item of the event is not the instance of its slot", now);
        const std::string guid = items::guid_hex(slot.item_guid);
        const svc::InventoryItem* held = inventory_.at(*where, slot.slot);
        if (!held || held->instance_guid != guid) return refuse_item_request(conn, what, "the slot does not hold that item", now);
        const svc::InventoryItem before = *held;
        const items::Outcome outcome = inventory_.consume(*where, slot.slot, guid, slot.count > 0 ? slot.count : before.quantity);
        if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
        inventory_dirty_ = true;
        inventory_changed_ = true;
        ++items_applied_;
        ++items_destroyed_;
        std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
        add_item_toasts(events, /*removals=*/true);
        for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
        spdlog::log(level_for(item_applied_logged_), "[session] {}: an item is destroyed: {} x{} from {} slot {}", name(), before.item_record_id, before.quantity,
                    *where == svc::ItemContainer::Backpack ? "the item bag" : "a material bag", slot.slot);
        return;
    }
    if (type != repl::StorageEventType::Move && type != repl::StorageEventType::Swap) {
        // (a Fill, dragging a stack onto another: Gathering stage 3 refuses it; the backlog has it)
        return refuse_item_request(conn, what, "an event of type " + std::to_string(event->type) + " is not made (only a Move and a Swap of whole stacks, and the destroy)", now);
    }
    if (event->move_source.slots.size() != 1 || event->targets.size() != 1 || event->targets[0].slots.size() != 1) {
        return refuse_item_request(conn, what, "a move names one source slot and one target slot", now);
    }
    const auto from = container_of(event->move_source.object);
    const auto to = container_of(event->targets[0].object);
    if (!from || !to) return refuse_item_request(conn, what, "a container that is none of this character's storages", now);
    const repl::StorageEventSlot& src = event->move_source.slots[0];
    const repl::StorageEventSlot& dst = event->targets[0].slots[0];
    if (!(src.item_guid == event->item.guid)) return refuse_item_request(conn, what, "the item of the event is not the instance of its source slot", now);
    const std::string guid = items::guid_hex(src.item_guid);
    const items::Outcome outcome = type == repl::StorageEventType::Move
                                       ? inventory_.move(*from, src.slot, src.count, guid, *to, dst.slot, &item_catalog())
                                       : inventory_.swap(*from, src.slot, src.count, guid, *to, dst.slot, items::guid_hex(dst.item_guid), dst.count);
    if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
    apply_outcome(conn, outcome, type == repl::StorageEventType::Move ? "an item is moved in the bag" : "two items swap places in the bag");
}

void PlayerSession::on_combat_settings(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    constexpr std::string_view kPrefix = "CharacterInGameSettings.";
    std::vector<bootstrap::PropertyValue> values;
    if (rpc.call.decoded) {
        for (const std::string_view path : kCombatEchoPaths) {
            const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, path.substr(kPrefix.size()));
            if (!c) continue;
            const auto it = rpc.call.params.values.find(c->handle);
            if (it != rpc.call.params.values.end()) values.push_back({path, it->second});
        }
    }
    if (values.empty()) {
        spdlog::log(level_for(combat_empty_logged_), "[session] {}: the client's combat settings carry none of the {} values worldd echoes",
                    name(), kCombatEchoPaths.size());
        return;
    }
    if (!within_budget(combat_budget_, "ServerUpdateAccountCombatSettings", now) || !room_for(1)) return;
    spdlog::log(level_for(combat_logged_), "[session] {}: the client's combat settings: echoing {} of the {} values worldd sends", name(),
                values.size(), kCombatEchoPaths.size());
    queue("combat settings echo",
          [values = std::move(values)](bootstrap::PlayerBootstrap& pb) { return pb.combat_settings_echo(values); }, conn);
}

void PlayerSession::on_storage_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto it = rpc.call.params.values.find(1);
    const auto* container = rpc.call.decoded && it != rpc.call.params.values.end() ? it->second.get_if<FIntrepidNetworkGUID>() : nullptr;
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    if (container && warehouse_payload(conn, *container, now)) return;   // storage clerks, step 0: a warehouse container's data
    std::optional<bootstrap::StorageKind> kind;
    if (container) {
        if (*container == g.backpack) kind = bootstrap::StorageKind::Backpack;
        else if (*container == g.equipment) kind = bootstrap::StorageKind::Equipment;
        else if (*container == g.quest_storage) kind = bootstrap::StorageKind::QuestStorage;
        else {
            for (int i = 0; i < items::kResourceContainers; ++i) {
                if (pb_.setup().resource_bags[static_cast<size_t>(i)].present && *container == g.resource_storage[static_cast<size_t>(i)]) kind = bootstrap::resource_kind(i);
            }
        }
    }
    if (!kind) {
        spdlog::debug("[session] {}: ServerRequestStorageData names a container worldd has no payload for", name());
        return;
    }
    if (!within_budget(storage_budget_, "ServerRequestStorageData", now) || !room_for(1)) return;
    const bootstrap::StorageKind k = *kind;
    queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
}

// ============================================================================================================================
//  plan E1: the GM vendor
// ============================================================================================================================

const world::WorldActor* PlayerSession::open_vendor(std::string& why) {
    if (vendor_actor_ == 0) {
        why = "no vendor window is open";
        return nullptr;
    }
    const world::WorldActor* a = registry_ ? registry_->find(vendor_actor_) : nullptr;
    const GoodPosition at = checker_.good();
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const bool town = a && !a->npc.is_gm_vendor && shops && shops->is_open(vendor_def_) && (vendor_by_talk_ || town_shop_of(*a) == vendor_def_);
    if (!a) {
        why = "the vendor is gone";
    } else if (!a->npc.is_gm_vendor && !town) {
        why = "the NPC is no GM vendor and opens no shop";
    } else if (town) {
        // a town shop: the shopkeeper must be less than 12 m away in three dimensions (the shop re-checks on every request)
        const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
        if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
            why = "the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away";
        } else {
            return a;
        }
    } else if (std::hypot(a->x - at.x, a->y - at.y) > kInteractRangeCm) {
        why = "the vendor is " + std::to_string(static_cast<int>(std::hypot(a->x - at.x, a->y - at.y) / 100.0)) + " m away";
    } else {
        return a;
    }
    vendor_actor_ = 0;
    vendor_def_ = 0;
    vendor_by_talk_ = false;
    return nullptr;
}
// -- C.O.A x C-Y-B --

void PlayerSession::on_interact(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_) return;
    const auto req = repl::parse_interact_request(rpc.call);
    if (!req) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerInteractWithInteractableComponent does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerInteractWithInteractableComponent", now)) return;
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(interact_logged_), "[session] {}: an interaction with component {} is ignored: {}", name(), req->interactable.ObjectId, why);
    };
    if (!registry_ || !view_) return ignore("the world has no actors");
    if (mounted()) return ignore("a rider does not interact (it is on its mount)");   // the mounts milestone, stage 3
    const world::WorldActor* a = registry_->find_by_component(req->interactable.ObjectId);
    if (!a) return ignore("it is no component of an actor of this world");
    if (a->kind == world::ActorKind::Mount) return on_mount_interact(conn, *a, now);   // getting on
    if (a->kind == world::ActorKind::Gatherable) return on_gather_interact(conn, *a, now);   // Gathering stage 2: a resource node
    if (a->kind == world::ActorKind::Station) return on_station_interact(conn, *a, now);     // crafting, step 0: a crafting or processing station
    if (a->kind == world::ActorKind::Shrine) return on_shrine_interact(conn, *a, now);       // Embersprings: the prompt to make it the home
    if (a->kind == world::ActorKind::Prop) return on_prop_interact(conn, *a, now);           // quests part 3: a quest object
    if (view_->state_of(a->id) != world::ClientState::Awake) return ignore("the client does not have that actor open");
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a->x - at.x, a->y - at.y);
    if (distance > kInteractRangeCm) return ignore("the actor is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    // Gathering stage 3, "Hunting": the interactable of a huntable creature starts its hunt, alive as it stands (or dead, if it was killed some other way), not a loot window
    if (a->kind == world::ActorKind::Npc && a->npc.interact_def != 0 && npc_world_ && npc_world_->gathering() && npc_world_->gathering()->hunt_kind(a->npc.interact_def)) {
        return on_hunt_interact(conn, *a, now);
    }
    if (npc_world_ && npc_world_->aftermath()) {
        if (const combat::Corpse* corpse = npc_world_->aftermath()->corpse_by_interactable(req->interactable.ObjectId)) return open_loot(conn, *corpse, *a, now);
    }
    // quests, slice 0: an NPC with a conversation opens the talking window
    if (a->kind == world::ActorKind::Npc && (a->npc.dialogue_root != 0 || npc_offers_quests(*a)) && open_dialogue(conn, *a, now)) return;
    const vendor::GmVendors* gm = world_ ? world_->gm_vendors() : nullptr;
    const vendor::GmVendor* def = (gm && a->npc.is_gm_vendor) ? gm->find(a->npc.vendor_guid) : nullptr;
    // vendors, step 0: a shopkeeper of a town whose data links it to a switched-on shop opens that shop's window
    if (!def && a->kind == world::ActorKind::Npc) {
        if (const int64_t shop = town_shop_of(*a)) {
            const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
            if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
                return ignore("the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away (a shop opens within 12 m)");
            }
            if (!room_for(1)) return;
            const vendor::TownShops* shops = world_->town_shops();
            vendor_actor_ = a->id;
            vendor_def_ = shop;
            vendor_by_talk_ = false;
            ++vendor_opens_;
            const FIntrepidNetworkGUID npc = a->actor.guid;
            spdlog::log(level_for(vendor_logged_), "[session] {}: opens the shop {} of {}", name(), shops->def(shop) ? shops->def(shop)->name : std::to_string(shop), a->npc.name);
            queue("ClientOpenVendor", [shop, npc](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(shop, npc); }, conn, /*must_build=*/true);
            return;
        }
    }
    if (!def) return ignore("the actor " + a->npc.name + " has nothing to offer yet");
    if (!room_for(1)) return;
    vendor_actor_ = a->id;
    vendor_def_ = def->guid;
    vendor_by_talk_ = false;
    ++vendor_opens_;
    const int64_t merch = def->guid;
    const FIntrepidNetworkGUID npc = a->actor.guid;
    spdlog::log(level_for(vendor_logged_), "[session] {}: opens the window of {} ({}, {} listing(s) in {} tab(s))", name(), def->label, def->name, def->listings, def->tabs);
    queue("ClientOpenVendor", [merch, npc](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(merch, npc); }, conn, /*must_build=*/true);
}

// -- crafting, step 0: the stations --------------------------------------------------------------------------------------------------------------------------

void PlayerSession::on_station_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    constexpr size_t kStationLogged = 30;   // a player tries a handful of stations: each click is a line in the log, up to this many of each kind
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(station_ignored_logged_, kStationLogged), "[session] {}: a click on the station {} is ignored: {}", name(), a.station.name, why);
    };
    if (view_->state_of(a.id) != world::ClientState::Awake) return ignore("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a.x - at.x, dy = a.y - at.y, dz = a.z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) {
        return ignore("the station is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away (a window opens within " + std::to_string(static_cast<int>(crafting::kStationRangeCm / 100.0)) + " m)");
    }
    if (!room_for(1)) return;
    const protocol::repl::StationTax tax = crafting::station_tax(a.station.tax_tag);
    const FIntrepidNetworkGUID guid = a.actor.guid;
    ++station_opens_;
    // what this step cannot put in the window is said once per kind: the recipes, prices and certification are the client's own; the player's known recipes (OwnerInfo) and jobs are not sent yet
    if (station_told_.insert("known recipes").second) {
        spdlog::info("[session] {}: a station window opens with the client's own recipe lists: the server sends no known recipes (told once)", name());
    }
    if (a.station.def == 0 && station_told_.insert("no def " + a.station.name).second) {
        spdlog::info("[session] {}: the station {} has no definition in the data: its window opens with the client's own recipes and the default tax", name(), a.station.name);
    }
    // crafting, steps 1 and 2: the player's own jobs at this station, in the order they were queued; a finished one is shown Complete with what it made
    const int64_t now_ms = wall_ms();
    std::vector<repl::CraftItemState> states;
    if (const std::string key = crafting::station_key(a.station.seed); !key.empty()) {
        states = crafting::states_at(jobs_, key, now_ms, sync_now(now), [this](const svc::ProcessingJob& j) { return job_rewards(j); });
        for (const repl::CraftItemState& s : states) {   // (the window is told what the jobs are now: later changes are sent as updates)
            if (const std::string guid = items::guid_hex(s.job); job_seen_.count(guid) != 0) job_seen_[guid] = s.status;
        }
    }
    spdlog::log(level_for(station_logged_, kStationLogged), "[session] {}: opens the window of {} in node {} ({} m away, tax {:.2f} of the price, the owning node keeps {:.2f}; {} job(s) of the player's here)", name(),
                a.station.name, a.station.node, static_cast<int>(distance / 100.0), tax.percent, tax.owning_share, states.size());
    queue("ClientOpenCraftingStationUI", [guid, tax, states](bootstrap::PlayerBootstrap& pb) { return pb.open_station(guid, tax, states); }, conn, /*must_build=*/true);
}

// -- crafting, step 1: starting a processing job -------------------------------------------------------------------------------------------------------------

int64_t PlayerSession::wall_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

double PlayerSession::sync_now(Clock::time_point now) { return bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now)); }

bool PlayerSession::sync_active_stations(int64_t now_ms) {
    const std::vector<std::array<int32_t, 4>> stations = crafting::active_station_words(jobs_, now_ms);
    if (stations.empty() && !active_told_) return false;   // nothing was ever told and nothing is: the property stays out of the state
    if (stations == active_words_ && active_told_) return false;
    if (!pb_.set_active_stations(stations)) return false;
    active_words_ = stations;
    active_told_ = true;
    return true;
}

void PlayerSession::on_start_craft(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    constexpr size_t kCraftLogged = 30;   // a player starts a few jobs: each is a line in the log, up to this many
    if (dead_) return;
    const auto req = repl::parse_start_craft(rpc.call);
    if (!req) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerStartCraft does not read ({} parameter bits): ignored, and the client is told to stop crafting (further ones are not logged)", name(), rpc.call.param_bits);
        }
        // (a quick make's client marked itself as crafting when it sent the start, and a start this server cannot read, such as one that names materials by tags, never answers: it is told to stop)
        if (!quick_.active && within_budget(vendor_budget_, "ServerStartCraft", now) && room_for(2)) {
            queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerStartCraft", now) || !room_for(8)) return;
    bool quick = false;   // the station crafts at once (set below): its refusals also stop the client's own craft
    const auto refuse = [&](const std::string& why) {
        if (quick) {   // (the quick make has its own count and its own log lines)
            ++quick_refused_;
            spdlog::log(level_for(quick_refused_logged_, kCraftLogged), "[session] {}: a craft is refused (nothing is taken): {}{}", name(), why, quick_.active ? " (the craft that runs goes on)" : "");
            if (!quick_.active) queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
            return;
        }
        ++crafts_refused_;
        spdlog::log(level_for(craft_refused_logged_, kCraftLogged), "[session] {}: a start at a station is refused (nothing is taken, nothing is sent): {}", name(), why);
    };
    if (!registry_ || !view_ || !world_) return refuse("the world has no actors");
    const world::WorldActor* a = registry_->find(req->station.ObjectId);
    if (!a || a->kind != world::ActorKind::Station) return refuse("the object named is no station of this world");
    quick = !a->station.processing;
    if (view_->state_of(a->id) != world::ClientState::Awake) return refuse("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) return refuse("the station " + a->station.name + " is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    if (quick) {
        if (quick_.active) return refuse("a craft of this player runs already (at " + quick_.station_name + "): a second start does nothing");
        return on_quick_start(conn, *req, *a, now);
    }
    const std::string key = crafting::station_key(a->station.seed);
    if (key.empty()) return refuse(a->station.name + " has no key that survives a restart: no job can be saved there");
    const content::CraftStationData* defs = world_->station_data();
    const content::CraftRecipeData* recipes = world_->craft_recipes();
    if (!defs || !recipes) return refuse("worldd has no station definitions or recipes (an older content.db)");
    const auto def = defs->defs.find(a->station.def);
    if (a->station.def == 0 || def == defs->defs.end()) return refuse("the data holds no definition for " + a->station.name);
    const int64_t now_ms = wall_ms();
    crafting::StartFacts facts;
    facts.recipes = recipes;
    facts.station = &def->second;
    facts.taught = [this](int64_t recipe) { return std::find(taught_.begin(), taught_.end(), recipe) != taught_.end(); };
    facts.tax = crafting::station_tax(a->station.tax_tag);
    facts.node.guid = a->station.node;
    if (world_->city_nodes() && world_->node_wire()) {
        if (const world::CityNodeInfo* node = world_->city_nodes()->by_guid(a->station.node)) {
            facts.node.level = world_->city_nodes()->level_of(node->guid);
            facts.node.biome = spawns::node_site_of(*world_->city_nodes(), *world_->node_wire(), *node, facts.node.level).biome;
        }
    }
    facts.tier_of = [this](int64_t profession) { return professions_.valid() ? professions_.tier(profession) : 0; };
    facts.inventory = &inventory_;
    facts.rarity_facts = [this](int64_t item) { return world_->item_rarity_facts(item); };   // (the rarity the client shows for a stack: the inputs are found by it)
    facts.copper = wallet_.copper();
    facts.rating_stat = [this](int64_t stat) { return artisan_stat(stat); };   // crafting quality: the job's time reads the character's processing time ratings
    facts.unfinished_here = crafting::unfinished_at(jobs_, key, now_ms);
    facts.held_here = crafting::held_at(jobs_, key);
    if (jobs_.size() >= crafting::kMaxJobsHeld) return refuse("the character holds " + std::to_string(jobs_.size()) + " jobs already");
    const crafting::Plan plan = crafting::plan_start(*req, facts);
    if (!plan.ok) return refuse(plan.refusal);
    // the stacks leave the material bags: the reagent first, then what the player named (the retail order); any failure puts the bags back as they were
    const items::Inventory kept = inventory_;
    items::Outcome taken;
    taken.ok = true;
    for (const crafting::Take& t : plan.takes) {
        items::Outcome o = inventory_.take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first);
        if (!o.ok) {
            inventory_ = kept;
            return refuse(o.refusal);
        }
        taken.events.insert(taken.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (taken.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
        inventory_ = kept;
        return refuse("the job takes from " + std::to_string(taken.events.size()) + " stacks: more events than one answer holds");
    }
    svc::ProcessingJob job = crafting::make_job(jobs_, key, plan.recipe->guid, req->quantity, plan.duration_ms, req->transaction, crafting::job_resources_of(plan.paid), now_ms, plan.ratings);   // (the ratings are saved with the job)
    const auto state = crafting::to_state(job, now_ms, sync_now(now));
    if (!state) {   // (the guids were made here: cannot happen)
        inventory_ = kept;
        return refuse("the job's guids do not read");
    }
    const int64_t before = wallet_.copper();
    if (plan.cost > 0) wallet_.pay(plan.cost);   // checked by the plan: it cannot be short
    job_seen_[job.job_guid] = state->status;   // (the answer below tells the window this status: a later change is sent by run_crafting)
    jobs_.push_back(std::move(job));
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++crafts_started_;
    // the answer in the retail order: the Remove events (each with its "Removed" callout), then one tick with the active stations and the new balance, the money callout and the new job's state
    std::vector<bootstrap::ItemEvent> events = item_events_for(taken);
    add_item_toasts(events, /*removals=*/true);
    const FIntrepidNetworkGUID station_guid = a->actor.guid;
    const int64_t delta = -plan.cost;
    const repl::CraftItemState shown = *state;
    constexpr auto kTickAfterRemoves = std::chrono::milliseconds(40);   // retail: the Removes at +79 ms, the tick at +121 ms
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, now, /*must_build=*/true);
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    sync_active_stations(now_ms);
    queue_later("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, now + kTickAfterRemoves, /*must_build=*/true);
    queue_later("craft started", [station_guid, delta, shown](bootstrap::PlayerBootstrap& pb) { return pb.craft_started_answer(station_guid, items::kMoneyTier, delta, shown); }, conn,
                now + kTickAfterRemoves, /*must_build=*/true);
    spdlog::log(level_for(craft_logged_, kCraftLogged),
                "[session] {}: started {} x{} at {} ({}): {} copper ({} each with the tax of {:.2f}, {} before it) leaves the purse ({} -> {}), {} stack(s) leave the material bags, the job runs {:.0f} s{} {}", name(),
                plan.recipe->name, req->quantity, a->station.name, key, plan.cost, plan.unit_taxed, facts.tax.percent, plan.unit_cost, before, wallet_.copper(), taken.events.size(),
                plan.duration_ms / 1000.0,
                plan.ratings.any() ? " (x" + std::to_string(plan.time_multiplier).substr(0, 5) + " of its base time with the ratings: time " + std::to_string(static_cast<int64_t>(std::lround(plan.ratings.time))) + ", quantity " +
                                         std::to_string(static_cast<int64_t>(std::lround(plan.ratings.quantity))) + ", rarity " + std::to_string(static_cast<int64_t>(std::lround(plan.ratings.rarity))) + ", experience " +
                                         std::to_string(static_cast<int64_t>(std::lround(plan.ratings.xp))) + ")"
                                   : std::string(),
                state->status == crafting::kPending ? "after the one before it" : "from now");
}

// -- crafting, step 3: the quick make ------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::chrono::milliseconds kQuickTickAfterRemoves{40};   // the product's tick follows the Remove events by this (the Remove events go first)

/// Whether the rarity of a crate follows the materials it was made of: a trade crate's does (the original's recorded Market crate, made of Common stacks, is Common; its recorded Citizen crate, made of three Common stacks and a
/// certificate of quality 51, is Uncommon at quality 6, the first quality of its band), a construction crate's does not (4 of 4 recorded are quality 0, from Common and from Uncommon materials alike, and the crate
/// is worth the same at every rarity).
bool crate_follows_materials(const content::ContentDb* db, int64_t record) {
    if (!db) return false;
    std::string error;
    const auto gear = db->item_gear(record, error);
    if (!gear) return false;
    for (const std::string& tag : gear->tags) {
        std::string lower;
        for (const char c : tag) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "item.crate.commodity" || lower.rfind("item.crate.commodity.", 0) == 0) return true;
    }
    return false;
}

/// What a craft's price is counted in, for the log: copper, Glint T1 items (a trade crate of the market, node, harbor and smugglers tables) or the node currency (the citizen table).
const char* price_unit(int64_t tier) { return tier == items::kGlintTier ? "Glint T1" : tier == items::kNodeCurrencyTier ? "node currency" : "copper"; }

}  // namespace

void PlayerSession::on_quick_start(Connection& conn, const repl::StartCraft& req, const world::WorldActor& a, Clock::time_point now) {
    constexpr size_t kQuickLogged = 30;   // a player crafts a few things: each start is a line in the log, up to this many
    const auto refuse = [&](const std::string& why) {
        ++quick_refused_;
        spdlog::log(level_for(quick_refused_logged_, kQuickLogged), "[session] {}: a craft is refused (nothing is taken): {}", name(), why);
        queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
    };
    const content::CraftStationData* defs = world_->station_data();
    const content::CraftRecipeData* recipes = world_->craft_recipes();
    if (!defs || !recipes) return refuse("worldd has no station definitions or recipes (an older content.db)");
    const auto def = defs->defs.find(a.station.def);
    if (a.station.def == 0 || def == defs->defs.end()) return refuse("the data holds no definition for " + a.station.name);
    crafting::StartFacts facts;
    facts.recipes = recipes;
    facts.station = &def->second;
    facts.quick = true;
    facts.taught = [this](int64_t recipe) { return std::find(taught_.begin(), taught_.end(), recipe) != taught_.end(); };
    facts.tax = crafting::station_tax(a.station.tax_tag);
    facts.node.guid = a.station.node;
    if (world_->city_nodes() && world_->node_wire()) {
        if (const world::CityNodeInfo* node = world_->city_nodes()->by_guid(a.station.node)) {
            facts.node.level = world_->city_nodes()->level_of(node->guid);
            facts.node.biome = spawns::node_site_of(*world_->city_nodes(), *world_->node_wire(), *node, facts.node.level).biome;
        }
    }
    facts.tier_of = [this](int64_t profession) { return professions_.valid() ? professions_.tier(profession) : 0; };
    facts.inventory = &inventory_;
    facts.rarity_facts = [this](int64_t item) { return world_->item_rarity_facts(item); };   // (the rarity the client shows for a stack: the inputs are found by it)
    facts.copper = wallet_.copper();
    facts.node_currency = wallet_.balance(items::kNodeCurrency);
    const crafting::Plan plan = crafting::plan_start(req, facts);
    if (!plan.ok) return refuse(plan.refusal);
    // (the room for the product is looked at when the craft ends, with the materials already out of the bags)
    const content::CraftReward* reward = recipes->reward_of(*plan.recipe);
    if (!reward || reward->containers.empty()) return refuse(plan.recipe->name + " has no reward the server can give: it makes nothing");
    // a crate is carried in the cargo slot, one at a time: a craft that makes one is refused while the slot is taken
    if (cargo_crate_made(crafting::roll_products(*reward, 1, 0)) && inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        say(conn, "You are already carrying a crate.", now);
        return refuse("the cargo slot holds a crate already, and " + plan.recipe->name + " makes another: a second crate cannot be carried");
    }
    const GoodPosition at = checker_.good();
    quick_ = crafting::QuickMake{};
    quick_.active = true;
    quick_.station = a.id;
    quick_.station_name = a.station.name;
    quick_.recipe_name = plan.recipe->name;
    quick_.request = req;
    quick_.began = now;
    quick_.ready = now + std::chrono::milliseconds(plan.duration_ms);
    quick_.x = at.x;
    quick_.y = at.y;
    quick_.z = at.z;
    ++quick_started_;
    spdlog::log(level_for(quick_logged_, kQuickLogged), "[session] {}: begins to craft {} at {} ({:.1f} s; nothing is taken until it is made): {} {} ({} each with the tax of {:.2f}), {} stack(s) of materials", name(),
                plan.recipe->name, a.station.name, plan.duration_ms / 1000.0, plan.cost, price_unit(plan.currency), plan.unit_taxed, facts.tax.percent, plan.takes.size());
}

bool PlayerSession::cancel_quick_make(Connection* conn, const std::string& why) {
    if (!quick_.active) return false;
    const std::string recipe = quick_.recipe_name, station = quick_.station_name;
    quick_ = crafting::QuickMake{};
    ++quick_interrupted_;
    spdlog::log(level_for(quick_logged_, 30), "[session] {}: the craft of {} at {} ends with nothing taken: {}", name(), recipe, station, why);
    if (conn) queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, *conn, /*must_build=*/true);
    return true;
}

void PlayerSession::on_interrupt_station_craft(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(vendor_budget_, "ServerInterruptStationCraft", now)) return;
    const auto station = repl::parse_interrupt_craft(rpc.call);
    if (!station) {
        if (!interrupt_unreadable_logged_) {
            interrupt_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerInterruptStationCraft does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!quick_.active) {
        spdlog::debug("[session] {}: ServerInterruptStationCraft with no craft under way: ignored", name());
        return;
    }
    const world::WorldActor* a = registry_ ? registry_->find(station->ObjectId) : nullptr;
    if (a && a->id != quick_.station) {   // (it names another station: this player's craft is at the one it began at; the client only sends this for the station it has open)
        spdlog::debug("[session] {}: ServerInterruptStationCraft names {}, the craft runs at {}: ignored", name(), a->station.name, quick_.station_name);
        return;
    }
    cancel_quick_make(&conn, "the client closed the window or pressed Cancel (ServerInterruptStationCraft)");
}

void PlayerSession::run_quick_make(Connection& conn, Clock::time_point now) {
    if (!quick_.active || failed_ || finished_ || !join_.empty()) return;
    if (dead_) {
        cancel_quick_make(&conn, "the player is dead");
        return;
    }
    if (mounted()) {
        cancel_quick_make(&conn, "the player is on a mount");
        return;
    }
    const world::WorldActor* a = registry_ ? registry_->find(quick_.station) : nullptr;
    if (!a) {
        cancel_quick_make(&conn, "the station left the world");
        return;
    }
    if (!view_ || view_->state_of(a->id) != world::ClientState::Awake) {
        cancel_quick_make(&conn, "the client does not have the station open any more");
        return;
    }
    const GoodPosition at = checker_.good();
    const double sx = a->x - at.x, sy = a->y - at.y, sz = a->z - at.z;
    const double distance = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (distance >= crafting::kStationRangeCm) {
        cancel_quick_make(&conn, "the station is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
        return;
    }
    // standing still: measured from where the craft began, so a slow walk adds up
    const double drift = std::sqrt((at.x - quick_.x) * (at.x - quick_.x) + (at.y - quick_.y) * (at.y - quick_.y) + (at.z - quick_.z) * (at.z - quick_.z));
    if (drift > crafting::kQuickDriftCm) {
        cancel_quick_make(&conn, "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) + " cm since the craft began (a craft needs standing still; at most " +
                                     std::to_string(static_cast<int>(crafting::kQuickDriftCm)) + " cm)");
        return;
    }
    if (now < quick_.ready) return;
    if (!room_for(24)) return;   // the replies are full: the next tick tries again
    complete_quick_make(conn, now);   // (a craft that cannot be made now says why in the log and is over)
}

bool PlayerSession::complete_quick_make(Connection& conn, Clock::time_point now) {
    constexpr size_t kMadeLogged = 30;
    constexpr const char* kNoRoomText = "Not enough inventory space.";
    const auto fail = [&](const std::string& why) {
        const std::string recipe = quick_.recipe_name, station = quick_.station_name;
        quick_ = crafting::QuickMake{};
        ++quick_interrupted_;
        spdlog::log(level_for(quick_logged_, 30), "[session] {}: the craft of {} at {} cannot be made now, nothing was taken: {}", name(), recipe, station, why);
        queue("ClientInterruptCraft", [](bootstrap::PlayerBootstrap& pb) { return pb.craft_interrupted(); }, conn, /*must_build=*/true);
        return false;
    };
    const world::WorldActor* a = registry_ ? registry_->find(quick_.station) : nullptr;
    const content::CraftStationData* defs = world_ ? world_->station_data() : nullptr;
    const content::CraftRecipeData* recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!a || !defs || !recipes) return fail("the station or the recipes are gone");
    const auto def = defs->defs.find(a->station.def);
    if (a->station.def == 0 || def == defs->defs.end()) return fail("the data holds no definition for " + a->station.name);
    // the checks run again on what the player holds now (materials, purse, certification, ...)
    crafting::StartFacts facts;
    facts.recipes = recipes;
    facts.station = &def->second;
    facts.quick = true;
    facts.taught = [this](int64_t recipe) { return std::find(taught_.begin(), taught_.end(), recipe) != taught_.end(); };
    facts.tax = crafting::station_tax(a->station.tax_tag);
    facts.node.guid = a->station.node;
    if (world_->city_nodes() && world_->node_wire()) {
        if (const world::CityNodeInfo* node = world_->city_nodes()->by_guid(a->station.node)) {
            facts.node.level = world_->city_nodes()->level_of(node->guid);
            facts.node.biome = spawns::node_site_of(*world_->city_nodes(), *world_->node_wire(), *node, facts.node.level).biome;
        }
    }
    facts.tier_of = [this](int64_t profession) { return professions_.valid() ? professions_.tier(profession) : 0; };
    facts.inventory = &inventory_;
    facts.rarity_facts = [this](int64_t item) { return world_->item_rarity_facts(item); };   // (the rarity the client shows for a stack: the inputs are found by it)
    facts.copper = wallet_.copper();
    facts.node_currency = wallet_.balance(items::kNodeCurrency);
    const repl::StartCraft req = quick_.request;
    const crafting::Plan plan = crafting::plan_start(req, facts);
    if (!plan.ok) return fail(plan.refusal);
    const content::CraftRecipe& recipe = *plan.recipe;

    // 1. the materials leave the bags (the reagent first, then what the player named: the order of the Remove events), 2. the price leaves the purse
    const items::Inventory kept_bags = inventory_;
    const items::Wallet kept_purse = wallet_;
    items::Outcome taken;
    taken.ok = true;
    std::vector<crafting::PaidUnits> paid;   // what the recipe's own materials really were (the reagent is not part of the quality)
    for (const crafting::Take& t : plan.takes) {
        items::Outcome o = inventory_.take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first);
        if (!o.ok) {
            inventory_ = kept_bags;
            return fail(o.refusal);
        }
        if (!t.lowest_first) {
            for (const items::Event& e : o.events) {
                paid.push_back({t.slot, e.item.quality, e.count > 0 ? e.count : e.item.quantity, world_->item_rarity_facts(e.item.item_record_id).rarity_of(e.item.quality)});
            }
        }
        taken.events.insert(taken.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (taken.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
        inventory_ = kept_bags;
        return fail("the craft takes from " + std::to_string(taken.events.size()) + " stacks: more events than one answer holds");
    }
    const int64_t before = wallet_.copper();
    items::Outcome glint_paid;   // the Glint T1 stacks that pay a price in Glint (the Remove events of the answer)
    if (plan.cost > 0) {   // (checked by the plan: none of them can be short)
        if (plan.currency == items::kGlintTier) {
            glint_paid = inventory_.take(items::kGlintT1Item, plan.cost);
            if (!glint_paid.ok) {
                inventory_ = kept_bags;
                wallet_ = kept_purse;
                return fail(glint_paid.refusal);
            }
            if (glint_paid.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
                inventory_ = kept_bags;
                wallet_ = kept_purse;
                return fail("the Glint comes from " + std::to_string(glint_paid.events.size()) + " stacks: more events than the answers hold");
            }
        } else if (plan.currency == items::kNodeCurrencyTier) {
            wallet_.pay_currency(items::kNodeCurrency, plan.cost);
        } else {
            wallet_.pay(plan.cost);
        }
    }

    // 3. the product: its quality starts from the paid materials' rarity and the crafter's artisan ratings add to it, its experience is the recipe's reward table's times the experience rating's multiplier
    const content::ContentDb* db = world_->content();
    const double crate_mean = crafting::craft_mean_rarity(paid);   // (a trade crate starts where the mean rarity of what it was made of puts it in its band, like a piece of gear, and no rating adds to it)
    const auto facts_of = [db](int64_t item) -> std::optional<content::ItemSaleFacts> {
        if (!db) return std::nullopt;
        std::string error;
        auto f = db->item_sale(item, error);
        if (!f || !f->known) return std::nullopt;
        return f;
    };
    const crafting::Ratings ratings = artisan_ratings(recipe.profession);
    const std::optional<crafting::JobRewards> made = crafting::roll_craft(*recipes, recipe, paid, ratings, progress_.level, crafting::seed_of(items::guid_hex(req.transaction)), facts_of);
    if (!made || made->products.empty()) {
        inventory_ = kept_bags;
        wallet_ = kept_purse;
        return fail(recipe.name + " makes nothing the server knows");
    }
    const crafting::JobRewards& rewards = *made;
    // a crate goes to the cargo slot, never to the bag, and only one is carried: the slot may have been filled since the craft began
    const bool cargo = cargo_crate_made(rewards.products);
    if (cargo && inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        inventory_ = kept_bags;
        wallet_ = kept_purse;
        say(conn, "You are already carrying a crate.", now);
        return fail("the cargo slot holds a crate already, so " + recipe.name + " cannot be made (the player was told)");
    }
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    std::vector<svc::InventoryItem> fresh;                    // every stack, product after product
    std::vector<std::vector<svc::InventoryItem>> by_product;  // ... and each product's own
    std::map<int64_t, std::string> shown_name;
    for (const crafting::Product& p : rewards.products) {
        const auto info = item_catalog().info(p.item);
        if (!info) {
            inventory_ = kept_bags;
            wallet_ = kept_purse;
            return fail("the item table does not know product " + std::to_string(p.item) + " of " + recipe.name);
        }
        std::string error;
        const auto rec = db ? db->item(p.item, error) : std::nullopt;
        if (rec) shown_name[p.item] = rec->display_name.empty() ? rec->name : rec->display_name;
        const int64_t max_stack = std::max(1, info->max_stack);
        by_product.emplace_back();
        for (int64_t left = p.quantity; left > 0;) {
            const int64_t take = std::min(left, max_stack);
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = p.item;
            it.instance_guid = fresh_guid();
            for (const svc::InventoryItem& other : fresh) {
                if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();   // (fresh_guid looks at the inventory only)
            }
            it.quantity = static_cast<int32_t>(take);
            it.quality = p.quality > 0 ? p.quality : rewards.quality;
            if (cargo) {
                // a crate has no quality numbers of its own to add up: a trade crate starts where the mean rarity of the paid stacks puts it in its band (the first quality of that rarity and its share of the
                // band, to the nearest whole number, as a crafted piece has it; its own rarity bounds hold it inside them), a construction crate has the record's own
                it.quality = std::max(1, rec ? rec->default_quality : 1);
                if (crate_follows_materials(db, p.item)) {
                    const items::ItemRarityFacts crate_facts = world_->item_rarity_facts(p.item);
                    it.quality = std::clamp(crafting::quality_from(crafting::craft_base_quality(crate_mean, crate_facts)), 1, crafting::kMaxQuality);
                    const int shown = crate_facts.rarity_of(it.quality);
                    if (shown != crate_facts.band_of(it.quality)) it.quality = items::first_quality_of(shown);
                }
            }
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            by_product.back().push_back(it);
            fresh.push_back(std::move(it));
            left -= take;
        }
    }
    // the room, all or nothing, with the materials already gone from the bags (their cells are free): with none nothing is taken and the player is told so
    if (!cargo && !inventory_.can_grant(fresh, item_catalog())) {
        inventory_ = kept_bags;
        wallet_ = kept_purse;
        say(conn, kNoRoomText, now);
        return fail("the bags have no room for the " + std::to_string(fresh.size()) + " stack(s) of " + recipe.name + " (the player was told)");
    }
    std::vector<items::Outcome> granted;
    if (cargo) {
        items::Outcome o = inventory_.grant_cargo(fresh.front(), item_catalog());
        if (!o.ok) {
            const std::string why = o.refusal;
            inventory_ = kept_bags;
            wallet_ = kept_purse;
            return fail("the crate is not put in the cargo slot: " + why);
        }
        granted.push_back(std::move(o));
    } else {
        for (std::vector<svc::InventoryItem>& stacks : by_product) {
            items::Outcome o = inventory_.grant(stacks, item_catalog());
            if (!o.ok) {
                const std::string why = o.refusal;
                inventory_ = kept_bags;
                wallet_ = kept_purse;
                return fail("the product is not granted: " + why);
            }
            granted.push_back(std::move(o));
        }
    }

    // 4. the craft is made: the experience is paid once, the bags and the purse are saved
    const std::string station_name = quick_.station_name;
    const repl::GuidValue txn = req.transaction;
    quick_ = crafting::QuickMake{};
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++quick_made_;
    if (cargo) apply_gear_stats("a crate was made");   // the worn stats (the crate's stat block is among them): before the replies are queued, which are built later and carry the stats' deltas
    professions::Professions after = professions_;
    if (professions_.valid() && rewards.profession_xp > 0 && rewards.profession != 0) after.add_xp(rewards.profession, rewards.profession_xp);
    const std::vector<professions::Change> changes = professions_.valid() ? set_professions(after) : std::vector<professions::Change>();
    const leveling::LevelCurve* curve = world_->level_curve();
    const std::span<const content::AutoGrant> grant_list = world_->auto_grants();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && rewards.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grant_list, progress_, rewards.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);

    // 5. what the client is told, in the order the game does it: the Remove events of the materials; then in one tick the money callout and the product's Add events (the last one carries the
    // id of the start, which ends the client's craft); then the purse and the experience
    std::vector<bootstrap::ItemEvent> removes = item_events_for(taken);
    for (bootstrap::ItemEvent& e : removes) e.instigated = true;   // (the character's own guid, as the game names it for a craft)
    add_item_toasts(removes, /*removals=*/true);
    std::vector<bootstrap::ItemEvent> adds;
    for (const items::Outcome& o : granted) {
        std::vector<bootstrap::ItemEvent> part = item_events_for(o);
        adds.insert(adds.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    add_item_toasts(adds, /*removals=*/false);
    for (auto it = adds.rbegin(); it != adds.rend(); ++it) {
        if (it->type == repl::StorageEventType::Add) {
            it->craft_txn = txn;
            break;
        }
    }
    const int64_t delta = -plan.cost;
    const int64_t tier = plan.currency;
    // a price in Glint: its Remove events come before the callout, with no toast; the answer holds the last of them (a send holds kMaxEventsPerChain events, the crate's Add among them), the ones
    // before are sent ahead in chains of their own
    std::vector<bootstrap::ItemEvent> glint_removes = item_events_for(glint_paid), glint_ahead;
    if (!glint_removes.empty()) {
        const size_t first_adds = std::min(adds.size(), bootstrap::kMaxEventsPerChain);
        const size_t room = bootstrap::kMaxEventsPerChain > first_adds ? bootstrap::kMaxEventsPerChain - first_adds : 0;
        if (glint_removes.size() > room) {
            glint_ahead.assign(glint_removes.begin(), glint_removes.end() - static_cast<std::ptrdiff_t>(room));
            glint_removes.erase(glint_removes.begin(), glint_removes.end() - static_cast<std::ptrdiff_t>(room));
        }
    }
    for (size_t at_event = 0; at_event < removes.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(removes.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     removes.begin() + static_cast<std::ptrdiff_t>(std::min(removes.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, now, /*must_build=*/true);
    }
    const Clock::time_point tick = now + kQuickTickAfterRemoves;
    for (size_t at_event = 0; at_event < glint_ahead.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(glint_ahead.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     glint_ahead.begin() + static_cast<std::ptrdiff_t>(std::min(glint_ahead.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, tick, /*must_build=*/true);
    }
    for (size_t at_event = 0; at_event < adds.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(adds.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     adds.begin() + static_cast<std::ptrdiff_t>(std::min(adds.size(), at_event + bootstrap::kMaxEventsPerChain)));
        if (at_event == 0) {
            queue_later("craft made", [tier, delta, part, glint_removes](bootstrap::PlayerBootstrap& pb) {
                return tier == items::kGlintTier ? pb.item_payment_answer(tier, delta, glint_removes, part) : pb.purchase_answer(tier, delta, part);
            }, conn, tick, /*must_build=*/true);
        } else {
            queue_later("craft items", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, tick, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    pb_.set_currency(items::kNodeCurrency, wallet_.balance(items::kNodeCurrency));
    queue_later("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, tick, /*must_build=*/true);
    if (cargo) {   // the crate shows on the carrier's back: the pawn's held items and appearance follow the Add, as for any item put on
        const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
        const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
        queue_later("HeldItems and AppearanceIDs",
                    [held, appearance](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
                        if (!pb.apply_equipment(held, appearance, 1)) return std::nullopt;
                        return pb.equipment_update();
                    },
                    conn, tick, /*must_build=*/true);
        for (const items::Outcome& o : granted) note_cargo_crate(o);   // the quest module is told a construction crate is carried now
        sync_cargo(conn, tick);   // the crate's effects and the Drop Crate slot of the bar follow the Add
    }
    if (xp.changed() || leveled || !changes.empty()) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(changes);
        queue_later("XPTracker", [total, debt, wire](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, tick, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue_later("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn, tick);
    }
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf || !c.leveled()) continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue_later("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn, tick);
    }
    if (leveled) queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, tick, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, tick, /*must_build=*/true);
    }

    // the materials, the price and the product are saved together
    hand_over_inventory("a craft was made", /*periodic=*/false);
    if (!changes.empty() || xp.changed()) hand_over_progress(leveled ? "a level-up" : "a craft was made");

    std::string line = "made " + recipe.name + " at " + station_name + ": ";
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        line += (i ? ", " : "") + (shown_name.count(rewards.products[i].item) ? shown_name[rewards.products[i].item] : std::to_string(rewards.products[i].item)) + " x" +
                std::to_string(rewards.products[i].quantity);
    }
    // the quality: the whole number the item has (1 to 200: the nearest to the exact number the calculation came to, nothing is cut off before) and what it reads as on the community crafting calculator's tier
    // scale (the rarity, the way through its band: 10 in Uncommon is "Uncommon +44.4 %, 2.4444")
    const items::ItemRarityFacts product_facts = world_->item_rarity_facts(rewards.products.front().item);
    const double on_scale = crafting::tier_scale(product_facts, rewards.quality);
    const std::string as_tier = fmt::format("{:.2f}", rewards.quality_exact) + " exactly, nearest whole; " + std::string(items::rarity_name(product_facts.rarity_of(rewards.quality))) + " +" +
                                fmt::format("{:.1f}", (on_scale - std::floor(on_scale)) * 100.0) + " %, " + fmt::format("{:.4f}", on_scale) + " on the tier scale";
    line += (cargo ? " into the cargo slot at quality " + std::to_string(fresh.front().quality) + " (rarity " + std::string(items::rarity_name(world_->item_rarity_facts(fresh.front().item_record_id).rarity_of(fresh.front().quality))) + ")"
                   : " at quality " + std::to_string(rewards.quality) + " (" + as_tier + ") in " + std::to_string(fresh.size()) + " stack(s)") +
            "; " + std::to_string(plan.cost) + " " + price_unit(plan.currency) + " paid" +
            (plan.currency == items::kMoneyTier ? " (" + std::to_string(before) + " -> " + std::to_string(wallet_.copper()) + ")" : std::string()) + ", " + std::to_string(taken.events.size()) +
            " stack(s) of materials taken";
    if (rewards.ratings.any()) {
        line += "; artisan ratings: quality " + std::to_string(std::lround(rewards.ratings.quality)) + " adds " + fmt::format("{:.2f}", rewards.quality_added) + " to the base, experience " +
                std::to_string(std::lround(rewards.ratings.xp)) + " (x" + fmt::format("{:.3f}", rewards.xp_multiplier) + ")";
    }
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (pf) line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " + std::to_string(c.after.level) + ")";
    }
    if (rewards.adventuring_xp > 0) {
        line += "; Adventuring +" + std::to_string(rewards.adventuring_xp) + " XP offered (debt " + std::to_string(xp.before.debt) + " -> " + std::to_string(xp.after.debt) + ", total " +
                std::to_string(xp.after.xp) + (leveled ? ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level) : std::string()) + ")";
    }
    spdlog::log(level_for(quick_made_logged_, kMadeLogged), "[session] {}: {}", name(), line);
    return true;
}

bool PlayerSession::cargo_crate_made(const std::vector<crafting::Product>& made) const {
    if (made.size() != 1 || made.front().quantity != 1) return false;
    const auto info = item_catalog().info(made.front().item);
    return info && info->cargo_crate();
}

void PlayerSession::say(Connection& conn, const char* text, Clock::time_point now) {
    constexpr int32_t kCalloutType = 1, kCalloutPointSize = 32;
    constexpr std::chrono::seconds kCalloutLife{3};
    if (!room_for(2)) return;
    const uint64_t key_of_callout = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
    const int64_t ticks = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    queue("a callout", [ticks, key_of_callout, text](bootstrap::PlayerBootstrap& pb) { return pb.callout_add(text, kCalloutType, kCalloutPointSize, ticks, key_of_callout); }, conn);
    gather_callouts_.push_back({now + kCalloutLife, key_of_callout});
}

bool PlayerSession::gm_teach_recipe(Connection& conn, int64_t recipe, const std::string& by) {
    if (std::find(taught_.begin(), taught_.end(), recipe) != taught_.end()) return false;
    if (taught_.size() >= bootstrap::kMaxKnownRecipes) return false;
    taught_.push_back(recipe);
    hand_over_inventory("a recipe was taught by a game master", /*periodic=*/false);
    if (pb_.set_known_recipes(taught_)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[session] {}: was taught recipe {} by {} ({} taught in all)", name(), recipe, by, taught_.size());
    return true;
}

bool PlayerSession::gm_forget_recipe(Connection& conn, int64_t recipe, const std::string& by) {
    const auto it = std::find(taught_.begin(), taught_.end(), recipe);
    if (it == taught_.end()) return false;
    taught_.erase(it);
    hand_over_inventory("a taught recipe was taken away by a game master", /*periodic=*/false);
    if (pb_.set_known_recipes(taught_)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[session] {}: was made to forget recipe {} by {} ({} taught in all)", name(), recipe, by, taught_.size());
    return true;
}

// -- missing systems, step 0: tutorials stay done -------------------------------------------------------------------------------------------------------

void PlayerSession::on_remove_ftues(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now) {
    ++ftue_calls_;
    if (!rpc.call.decoded) {
        if (!ftue_unreadable_logged_) {
            ftue_unreadable_logged_ = true;
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs arrived and its parameters do not decode: nothing is recorded (further ones are not logged)", name());
        }
        return;
    }
    const protocol::repl::Value* v = protocol::repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "FTUEsToRemove");
    const protocol::repl::ArrayValue* list = v ? v->get_if<protocol::repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty()) return;   // (an empty list is the default: not on the wire)
    // the ids that would join the list: 0 and below are ignored, a repeat adds nothing, and the whole call is refused when the list would go over its cap
    if (list->elements.size() > bootstrap::kMaxCompletedFtues) {
        if (++ftue_refused_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs names {} tutorials, more than a character can have finished ({}): refused", name(), list->elements.size(),
                         bootstrap::kMaxCompletedFtues);
        }
        return;
    }
    std::vector<int64_t> fresh;
    for (const protocol::repl::ElementState& e : list->elements) {
        const auto it = e.values.find(1);
        const int64_t* id = it != e.values.end() ? it->second.get_if<int64_t>() : nullptr;
        if (!id || *id <= 0) {   // (0 is the id of no tutorial; an element that is not on the wire is 0)
            ++ftue_ignored_;
            if (++ftue_ignored_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: Server_RemoveLocaleFTUEs names tutorial {}: ignored", name(), id ? *id : 0);
            continue;
        }
        if (std::find(completed_ftues_.begin(), completed_ftues_.end(), *id) != completed_ftues_.end() || std::find(fresh.begin(), fresh.end(), *id) != fresh.end()) {
            ++ftue_repeats_;
            continue;
        }
        fresh.push_back(*id);
    }
    if (fresh.empty()) {
        spdlog::debug("[session] {}: Server_RemoveLocaleFTUEs: nothing new ({} calls, {} repeats, {} ignored so far)", name(), ftue_calls_, ftue_repeats_, ftue_ignored_);
        return;
    }
    if (completed_ftues_.size() + fresh.size() > bootstrap::kMaxCompletedFtues) {
        if (++ftue_refused_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs would take the finished tutorials over {}: refused, nothing changes", name(), bootstrap::kMaxCompletedFtues);
        }
        return;
    }
    for (const int64_t id : fresh) {
        completed_ftues_.push_back(id);
        spdlog::log(level_for(ftue_logged_, 20), "[session] {}: finished tutorial {} ({} finished in all)", name(), id, completed_ftues_.size());
    }
    progress_changed_ = true;
    hand_over_progress("a tutorial was finished");
    // the list goes back to the client a moment later, as the retail server's quiet replies do: the client takes the finished ones off its running list when it arrives
    if (pb_.set_completed_ftues(completed_ftues_)) {
        queue_later("FTUE Component", [](bootstrap::PlayerBootstrap& pb) { return pb.ftue_update(); }, conn, now, /*must_build=*/false);
    } else {
        spdlog::warn("[session] {}: the tutorial component does not take the {} finished tutorials: the client is not told", name(), completed_ftues_.size());
    }
}

size_t PlayerSession::gm_reset_tutorials(Connection& conn, const std::string& by) {
    const size_t n = completed_ftues_.size();
    if (n == 0) return 0;
    completed_ftues_.clear();
    progress_changed_ = true;
    hand_over_progress("the tutorials were put back by a game master");
    if (pb_.set_completed_ftues(completed_ftues_)) queue("FTUE Component", [](bootstrap::PlayerBootstrap& pb) { return pb.ftue_update(); }, conn, /*must_build=*/false);
    spdlog::info("[session] {}: had {} finished tutorial(s) put back by {}", name(), n, by);
    return n;
}

// -- missing systems, step 1: the client's quiet requests ------------------------------------------------------------------------------------------------------
// A target, a hover, the PvP flag, a closed loot window, a marker and a batch of bar slots: the client sends them and waits for nothing, but the game's own server keeps
// them (and answers the first three with a property update a tenth of a second later).  Nothing here is saved: the flag, the targets and the marker last as long as the session.

void PlayerSession::on_set_active_target(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++target_calls_;
    std::optional<uint64_t> slot;
    const repl::Value* named = nullptr;
    if (rpc.call.decoded) {
        slot = repl::rpcf::uint_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Slot"), 0);   // (a slot at 0 is not on the wire)
        named = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "InActiveTarget");               // (no object is not on the wire either)
    }
    const FIntrepidNetworkGUID* object = named ? named->get_if<FIntrepidNetworkGUID>() : nullptr;
    if (!rpc.call.decoded || !slot || (named && !object)) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetActiveTarget does not read ({} parameter bits): the target is not kept (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetActiveTarget", now) || !room_for(1)) return;
    if (*slot < 1 || *slot > 2) {
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: ServerSetActiveTarget names target slot {}: only the slots 1 and 2 exist, ignored", name(), *slot);
        return;
    }
    // the target is an actor of the world or the player's own pawn; any other object is no target
    FIntrepidNetworkGUID target;
    if (object && object->is_valid()) {
        const FIntrepidNetworkGUID& own = pb_.setup().guids.pawn;
        const world::WorldActor* actor = registry_ ? registry_->find(object->ObjectId) : nullptr;
        if (object->ObjectId == own.ObjectId) {
            target = own;
        } else if (actor) {
            target = actor->actor.guid;
        } else {
            ++target_unknown_;
            if (++quiet_refused_logged_ <= kLoggedPerKind) {
                spdlog::info("[session] {}: ServerSetActiveTarget names object {}, which is no actor the world knows: the slot is left empty", name(), object->ObjectId);
            }
        }
    }
    FIntrepidNetworkGUID& held = active_targets_[static_cast<size_t>(*slot - 1)];
    if (held == target) return;   // (the same target again: the client is told what it was told, so nothing goes out)
    held = target;
    ++target_changes_;
    if (!pb_.set_active_targets(active_targets_)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.ActiveTargets: the client is not told", name());
        return;
    }
    // the answer is the property update of the game's own server, about 84 ms after the call
    queue_later("CombatInfo (target)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("active target"); }, conn, now, /*must_build=*/false);
    spdlog::log(level_for(target_logged_, 20), "[session] {}: target slot {} is {} now", name(), *slot, target.is_valid() ? "object " + std::to_string(target.ObjectId) : std::string("empty"));
}

void PlayerSession::on_set_hover_target(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++hover_calls_;
    std::array<uint32_t, 4> words{};
    bool ok = rpc.call.decoded;
    constexpr const char* kWords[4] = {"InHoverTargetGuid.A", "InHoverTargetGuid.B", "InHoverTargetGuid.C", "InHoverTargetGuid.D"};
    for (size_t i = 0; ok && i < 4; ++i) {
        const auto w = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, kWords[i]), 0);   // (a word at 0 is not on the wire)
        ok = w && *w >= std::numeric_limits<int32_t>::min() && *w <= std::numeric_limits<uint32_t>::max();
        if (ok) words[i] = static_cast<uint32_t>(*w);
    }
    if (!ok) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetHoverTargetGuid does not read ({} parameter bits): the hover target is not kept (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetHoverTargetGuid", now) || !room_for(1)) return;
    if (words == hover_words_) return;
    hover_words_ = words;
    ++hover_changes_;
    if (!pb_.set_hover_target(words)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.HoverTargetGuid: the client is not told", name());
        return;
    }
    queue_later("CombatInfo (hover)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("hover target"); }, conn, now, /*must_build=*/false);
    const bool nothing = words == std::array<uint32_t, 4>{};
    spdlog::log(level_for(hover_logged_, 20), "[session] {}: points at {}", name(), nothing ? std::string("nothing") : fmt::format("{:08x}-{:08x}-{:08x}-{:08x}", words[0], words[1], words[2], words[3]));
}

void PlayerSession::on_set_pvp_flag(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    ++flag_calls_;
    const auto flag = rpc.call.decoded ? repl::rpcf::bool_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Flag"), false) : std::nullopt;
    if (!flag) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetPvpFlag does not read ({} parameter bits): the flag is not changed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetPvpFlag", now) || !room_for(1)) return;
    if (*flag == pvp_flag_) return;   // (a flag that is the flag: the game's server marks it and sends nothing, as nothing changed)
    pvp_flag_ = *flag;
    ++flag_changes_;
    if (!pb_.set_pvp_force_flag(pvp_flag_)) {
        spdlog::warn("[session] {}: the pawn does not take CombatInfo.bPvPForceFlag: the client is not told", name());
        return;
    }
    queue_later("CombatInfo (PvP flag)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("PvP flag"); }, conn, now, /*must_build=*/false);
    spdlog::log(level_for(flag_logged_, 20), "[session] {}: the PvP flag is {} (kept until the session ends)", name(), pvp_flag_ ? "on" : "off");
}

void PlayerSession::on_close_loot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    const repl::Value* named = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "LootContainer") : nullptr;
    const FIntrepidNetworkGUID* container = named ? named->get_if<FIntrepidNetworkGUID>() : nullptr;
    if (!rpc.call.decoded || (named && !container)) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerCloseLoot does not read ({} parameter bits): the loot window is not closed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(quiet_budget_, "ServerCloseLoot", now)) return;
    if (loot_actor_ == 0) return;   // (the window is closed already: the corpse was emptied, left behind or looted out)
    combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const combat::Corpse* corpse = after ? after->corpse_of(loot_actor_) : nullptr;
    // the call names the container the window shows (the game's server ignores a close that names another one)
    if (!container || (corpse && corpse->parts && container->ObjectId != corpse->parts->container)) {
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: ServerCloseLoot names another container than the open loot window's: the window stays open", name());
        return;
    }
    const std::string closed = corpse ? corpse->name : std::string("a corpse that is gone");
    loot_actor_ = 0;
    ++loot_closes_;
    spdlog::log(level_for(close_logged_, 20), "[session] {}: closes the loot window of {}", name(), closed);
}

void PlayerSession::on_set_slots(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!book_) return;
    ++slot_calls_;
    const auto slots = repl::parse_set_actionbar_slots(rpc.call);
    if (!slots) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetActionbarMultiSlotUseData does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (slots->empty()) return;
    if (slots->size() > kMaxMultiSlots) {
        ++bar_refused_;
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: ServerSetActionbarMultiSlotUseData names {} slots, more than {}: refused, nothing changed", name(), slots->size(), kMaxMultiSlots);
        return;
    }
    if (!within_budget(bar_budget_, "ServerSetActionbarMultiSlotUseData", now)) return;
    // each slot is judged and stored as the single-slot call would (the game's server runs that call for each), the progress is handed over once
    size_t stored = 0, refused = 0;
    for (const repl::ActionbarSlot& slot : *slots) {
        std::string why;
        if (!bar_.set(slot, &why)) {
            ++refused;
            ++bar_refused_;
            spdlog::log(level_for(bar_refused_logged_), "[session] {}: ServerSetActionbarMultiSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(), slot.bank_index, slot.slot_index, why);
            continue;
        }
        if (slot.use.use_type == repl::ActionbarUseType::Skill && slot.use.skill_rank_id != 0 && slot.bank_index < actionbar::kClassBanks) {
            bar_.remove_other_copies(slot.use.skill_rank_id, slot.bank_index, slot.slot_index);   // (a skill is on the class pages once: the client moved it)
        }
        ++stored;
        ++bar_sets_;
    }
    spdlog::log(level_for(slots_logged_, 20), "[session] {}: the client set {} action bar slot(s) at once: {} stored, {} refused", name(), slots->size(), stored, refused);
    if (stored > 0) {
        progress_changed_ = true;
        hand_over_progress("the action bar");
    }
}

void PlayerSession::on_set_map_marker(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    ++marker_calls_;
    const repl::Value* named = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Location") : nullptr;
    const repl::Vec3d* at = named ? named->get_if<repl::Vec3d>() : nullptr;
    if (!rpc.call.decoded || (named && !at)) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetMapMarkerLocation does not read ({} parameter bits): the marker is not changed (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    const repl::Vec3d where = at ? *at : repl::Vec3d{};   // (a location of 0, 0, 0 is not on the wire: it is the call that takes the marker away)
    if (!std::isfinite(where.x) || !std::isfinite(where.y) || !std::isfinite(where.z)) {
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: ServerSetMapMarkerLocation names a location that is not a number: the marker is not changed", name());
        return;
    }
    if (!within_budget(quiet_budget_, "ServerSetMapMarkerLocation", now)) return;
    if (where.x == 0 && where.y == 0 && where.z == 0) {
        if (!map_marker_) return;
        map_marker_.reset();
        spdlog::log(level_for(marker_logged_, 20), "[session] {}: the map marker is taken away", name());
        return;
    }
    if (map_marker_ && *map_marker_ == where) return;
    map_marker_ = where;
    spdlog::log(level_for(marker_logged_, 20), "[session] {}: the map marker is at ({:.0f}, {:.0f}, {:.0f}) (kept until the session ends)", name(), where.x, where.y, where.z);
}

// -- crafting, step 2: collecting a job ---------------------------------------------------------------------------------------------------------------------

std::optional<crafting::JobRewards> PlayerSession::job_rewards(const svc::ProcessingJob& job) const {
    const content::CraftRecipeData* recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!recipes) return std::nullopt;
    // crafting quality: the job's extra units, steps up and experience come from the artisan ratings it was started with, saved with it (a job saved before they were kept has none)
    return crafting::roll_job(*recipes, job, progress_.level, crafting::job_ratings(job), [this](int64_t item) { return world_->item_rarity_facts(item); });
}

double PlayerSession::artisan_stat(int64_t stat) const {
    const stats::StatValue* v = stats_ ? stats_->find(stat) : nullptr;
    return v ? static_cast<double>(v->value) : 0.0;
}

crafting::Ratings PlayerSession::artisan_ratings(int64_t profession) const {
    const content::CraftRecipeData* recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!recipes || !stats_ || !stats_->started()) return {};
    return crafting::ratings_of(recipes->artisan, profession, [this](int64_t stat) { return artisan_stat(stat); });
}

const world::WorldActor* PlayerSession::station_by_key(const std::string& key) const {
    const auto seed = crafting::station_seed(key);
    if (!seed || !registry_) return nullptr;
    const world::WorldActor* found = nullptr;
    registry_->for_each([&](const world::WorldActor& a) {
        if (!found && a.kind == world::ActorKind::Station && a.station.seed == *seed) found = &a;
    });
    return found;
}

int64_t PlayerSession::next_job_event(int64_t now_ms) const {
    int64_t next = std::numeric_limits<int64_t>::max();
    for (const svc::ProcessingJob& j : jobs_) {
        const auto seen = job_seen_.find(j.job_guid);
        if (seen == job_seen_.end() || seen->second != crafting::status_at(j, now_ms)) return 0;   // a change is waiting to be sent
        if (j.started_at_ms > now_ms) next = std::min(next, j.started_at_ms);
        if (j.end_ms() > now_ms) next = std::min(next, j.end_ms());
    }
    return next;
}

void PlayerSession::run_crafting(Connection& conn, Clock::time_point now) {
    if (failed_ || finished_ || !join_.empty() || jobs_.empty()) return;
    const int64_t now_ms = wall_ms();
    if (now_ms < next_job_event_ms_) return;
    // the jobs whose status changed since the window was last told of them, by station, in the order the jobs were queued
    struct Group {
        std::string                       key;
        std::vector<repl::CraftItemState> states;
    };
    std::vector<Group> groups;
    std::vector<std::pair<std::string, uint8_t>> told;
    size_t completed = 0, began = 0;
    const double sync = sync_now(now);
    for (const svc::ProcessingJob& j : jobs_) {
        const uint8_t status = crafting::status_at(j, now_ms);
        const auto seen = job_seen_.find(j.job_guid);
        if (seen == job_seen_.end()) {
            job_seen_[j.job_guid] = status;
            continue;
        }
        if (seen->second == status) continue;
        std::optional<crafting::JobRewards> made;
        if (status == crafting::kComplete) made = job_rewards(j);
        const auto state = crafting::to_state(j, now_ms, sync, made ? &*made : nullptr);
        if (!state || state->status != status) continue;   // (it cannot be shown so yet: the next tick looks again)
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.key == j.station_key; });
        if (g == groups.end()) {
            groups.push_back({j.station_key, {}});
            g = groups.end() - 1;
        }
        g->states.push_back(*state);
        told.emplace_back(j.job_guid, status);
        (status == crafting::kComplete ? completed : began) += 1;
    }
    if (groups.empty()) {
        next_job_event_ms_ = next_job_event(now_ms);
        return;
    }
    size_t steps = 1;
    for (const Group& g : groups) steps += (g.states.size() + bootstrap::kMaxEventsPerChain - 1) / bootstrap::kMaxEventsPerChain;
    if (!room_for(steps)) return;   // (the replies are full: the next tick tries again)
    // the player's list of active stations first (a station with no job left running leaves it), then the window's states
    if (sync_active_stations(now_ms)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    size_t sent = 0;
    for (const Group& g : groups) {
        // the window can only be open at a station the client has: for any other the next open shows the jobs as they are
        const world::WorldActor* a = station_by_key(g.key);
        if (!a || !view_ || view_->state_of(a->id) != world::ClientState::Awake) continue;
        const FIntrepidNetworkGUID guid = a->actor.guid;
        for (size_t at = 0; at < g.states.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<repl::CraftItemState> part(g.states.begin() + static_cast<std::ptrdiff_t>(at),
                                                         g.states.begin() + static_cast<std::ptrdiff_t>(std::min(g.states.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("craft update", [guid, part](bootstrap::PlayerBootstrap& pb) { return pb.craft_states_update(guid, part); }, conn, /*must_build=*/true);
        }
        sent += g.states.size();
    }
    for (const auto& [guid, status] : told) job_seen_[guid] = status;
    next_job_event_ms_ = next_job_event(now_ms);
    constexpr size_t kUpdatesLogged = 30;
    spdlog::log(level_for(craft_update_logged_, kUpdatesLogged), "[session] {}: {} job(s) ended and {} began: {} state(s) sent to the window{}", name(), completed, began, sent,
                sent == told.size() ? "" : " (the rest are at stations the client does not have open: the next open shows them)");
}

void PlayerSession::on_processing_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    constexpr size_t kClaimLogged = 30;   // a player collects a few jobs: each is a line in the log, up to this many
    constexpr const char* kNoRoomText = "Not enough inventory space.";
    constexpr int32_t kCalloutType = 1, kCalloutPointSize = 32;
    constexpr std::chrono::seconds kCalloutLife{3};
    constexpr auto kBetweenProducts = std::chrono::milliseconds(75);   // retail: the second product's items 75 ms after the first's
    if (dead_) return;
    const auto req = repl::parse_processing_request(rpc.call);
    if (!req) {
        if (!claim_unreadable_logged_) {
            claim_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerSendProcessingStationRequest does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerSendProcessingStationRequest", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++claims_refused_;
        spdlog::log(level_for(claim_refused_logged_, kClaimLogged), "[session] {}: a request at a station is refused (nothing is changed): {}", name(), why);
    };
    if (req->type == crafting::kCancelled) return refuse("cancelling a job is not made yet: the job runs on");
    if (req->type != crafting::kClaimed) return refuse("a request of type " + std::to_string(req->type) + ": only a claim (type " + std::to_string(crafting::kClaimed) + ") is served");
    if (!registry_ || !view_ || !world_) return refuse("the world has no actors");
    const world::WorldActor* a = registry_->find(req->station.ObjectId);
    if (!a || a->kind != world::ActorKind::Station) return refuse("the object named is no station of this world");
    if (view_->state_of(a->id) != world::ClientState::Awake) return refuse("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) return refuse("the station " + a->station.name + " is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    if (!a->station.processing) return refuse(a->station.name + " is a station that crafts at once: it holds no jobs to collect");
    const std::string key = crafting::station_key(a->station.seed);
    if (key.empty()) return refuse(a->station.name + " has no key that survives a restart: it holds no jobs");
    const content::CraftRecipeData* recipes = world_->craft_recipes();
    if (!recipes) return refuse("worldd has no recipes (an older content.db)");
    // the job: found by its guid among the player's own at this station; what the client sends back of it (its products, its experience) is never read
    const std::string job_guid = items::guid_hex(req->state.job);
    const svc::ProcessingJob* found = crafting::find_job(jobs_, key, job_guid);
    if (!found) return refuse("the player has no job " + job_guid + " at " + a->station.name);
    const int64_t now_ms = wall_ms();
    if (crafting::unfinished(*found, now_ms)) {
        return refuse("the job is not over: " + std::to_string((found->end_ms() - now_ms + 999) / 1000) + " s to go");
    }
    const svc::ProcessingJob job = *found;
    const auto recipe_it = recipes->recipes.find(job.recipe_guid);
    const std::optional<crafting::JobRewards> made = job_rewards(job);
    if (recipe_it == recipes->recipes.end() || !made) return refuse("recipe " + std::to_string(job.recipe_guid) + " of the job is none of the data's");
    const crafting::JobRewards& rewards = *made;
    if (const content::CraftReward* table = recipes->reward_of(recipe_it->second); table && !table->unknown_formula.empty() && station_told_.insert("xp " + table->unknown_formula).second) {
        spdlog::info("[session] {}: {} pays its experience by the formula {}, which this server does not work out: none is paid for it (told once)", name(), recipe_it->second.name, table->unknown_formula);
    }
    if (req->warehouse && station_told_.insert("warehouse claim").second) {
        spdlog::info("[session] {}: a claim asks for the warehouse: none exists yet, the items go to the bags (told once)", name());
    }

    // what the job made, as stacks of the item's size: one call of the answer for each product, the last product first (retail's order)
    const content::ContentDb* db = world_->content();
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    std::vector<svc::InventoryItem> fresh;                    // every stack, product after product
    std::vector<std::vector<svc::InventoryItem>> by_product;  // ... and each product's own
    std::map<int64_t, std::string> shown_name;
    for (size_t at_product = rewards.products.size(); at_product-- > 0;) {
        const crafting::Product& p = rewards.products[at_product];
        if (p.quantity < 1) continue;   // (a line with no unit has no stack: an empty entry would make the grant refuse and the job could never be collected)
        const auto info = item_catalog().info(p.item);
        if (!info) return refuse("the item table does not know product " + std::to_string(p.item) + " of " + recipe_it->second.name);
        std::string error;
        const auto rec = db ? db->item(p.item, error) : std::nullopt;
        if (rec) shown_name[p.item] = rec->display_name.empty() ? rec->name : rec->display_name;
        const int64_t max_stack = std::max(1, info->max_stack);
        by_product.emplace_back();
        for (int64_t left = p.quantity; left > 0;) {
            const int64_t take = std::min(left, max_stack);
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = p.item;
            it.instance_guid = fresh_guid();
            for (const svc::InventoryItem& other : fresh) {
                if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();   // (fresh_guid looks at the inventory only)
            }
            it.quantity = static_cast<int32_t>(take);
            it.quality = p.quality > 0 ? p.quality : rewards.quality;
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            by_product.back().push_back(it);
            fresh.push_back(std::move(it));
            left -= take;
        }
    }
    // the room, all or nothing: with none the player is told so and the job stays as it is
    if (!fresh.empty() && !inventory_.can_grant(fresh, item_catalog())) {
        const uint64_t key_of_callout = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
        const int64_t ticks = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        if (room_for(2)) {
            queue("a callout", [ticks, key_of_callout, kNoRoomText](bootstrap::PlayerBootstrap& pb) { return pb.callout_add(kNoRoomText, kCalloutType, kCalloutPointSize, ticks, key_of_callout); }, conn);
            gather_callouts_.push_back({now + kCalloutLife, key_of_callout});
        }
        return refuse("the bags have no room for the " + std::to_string(fresh.size()) + " stack(s) of " + recipe_it->second.name + " (the player was told; the job stays)");
    }
    size_t steps = 4 + rewards.products.size() + 8;
    if (!room_for(steps)) return;

    // the items into the bags, a product after the other (the room was checked: a refusal here is a record the table refuses, not something the client can cause)
    const items::Inventory before_grant = inventory_;
    std::vector<items::Outcome> granted;
    for (std::vector<svc::InventoryItem>& stacks : by_product) {
        items::Outcome o = inventory_.grant(stacks, item_catalog());
        if (!o.ok) {
            spdlog::error("[session] {}: the items of {} are not granted ({}): the job stays", name(), recipe_it->second.name, o.refusal);
            inventory_ = before_grant;
            return;
        }
        granted.push_back(std::move(o));
    }
    // the job is gone: the experience is paid once
    const std::string station_name = a->station.name;
    const FIntrepidNetworkGUID station_guid = a->actor.guid;
    const auto claimed = crafting::claimed_state(job, now_ms, sync_now(now));
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const svc::ProcessingJob& j) { return j.job_guid == job_guid; }), jobs_.end());
    job_seen_.erase(job_guid);
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++crafts_claimed_;

    // the experience: the profession's, then Adventuring's (the debt first), as a harvest pays it
    professions::Professions after = professions_;
    if (professions_.valid() && rewards.profession_xp > 0 && rewards.profession != 0) after.add_xp(rewards.profession, rewards.profession_xp);
    const std::vector<professions::Change> changes = professions_.valid() ? set_professions(after) : std::vector<professions::Change>();
    const leveling::LevelCurve* curve = world_->level_curve();
    const std::span<const content::AutoGrant> grant_list = world_->auto_grants();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && rewards.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grant_list, progress_, rewards.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);

    // what the client is told, in the retail order: the job at Claimed with the first product's items, the other products' items 75 ms apart, then the experience and the level-ups
    // one call of events for each product: its stacks' Adds (a stack that was topped up is the Remove of the old stack and the Add of the new total, kept together)
    Clock::time_point due = now;
    for (size_t i = 0; i < granted.size(); ++i) {
        std::vector<bootstrap::ItemEvent> batch = item_events_for(granted[i]);
        add_item_toasts(batch, /*removals=*/false);
        if (i == 0) {
            const repl::CraftItemState state = claimed ? *claimed : repl::CraftItemState{};
            queue_later("craft claimed", [station_guid, state, batch](bootstrap::PlayerBootstrap& pb) { return pb.craft_claimed_answer(station_guid, state, batch); }, conn, due, /*must_build=*/true);
        } else {
            due += kBetweenProducts;
            queue_later("craft items", [batch](bootstrap::PlayerBootstrap& pb) { return pb.craft_batched_events(batch); }, conn, due, /*must_build=*/true);
        }
    }
    if (granted.empty() && claimed) {   // (a job that made no item)
        const repl::CraftItemState state = *claimed;
        queue_later("craft claimed", [station_guid, state](bootstrap::PlayerBootstrap& pb) { return pb.craft_claimed_answer(station_guid, state, {}); }, conn, due, /*must_build=*/true);
    }
    if (xp.changed() || leveled || !changes.empty()) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(changes);
        queue_later("XPTracker", [total, debt, wire](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, due, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue_later("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn, due);
    }
    if (!changes.empty()) {
        for (const professions::Change& c : changes) {
            const professions::Profession* pf = professions_.book()->find(c.profession);
            if (!pf || !c.leveled()) continue;
            const int64_t category = pf->leveling_type;
            const int level = c.after.level;
            queue_later("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn, due);
        }
    }
    if (leveled) queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, due, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, due, /*must_build=*/true);
    }

    // the job is deleted and the items are in the bags: saved together
    hand_over_inventory("a job was collected", /*periodic=*/false);
    if (!changes.empty() || xp.changed()) hand_over_progress(leveled ? "a level-up" : "a job was collected");

    std::string line = "collected " + recipe_it->second.name + " x" + std::to_string(job.quantity) + " at " + station_name + " (" + key + "): ";
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        line += (i ? ", " : "") + (shown_name.count(rewards.products[i].item) ? shown_name[rewards.products[i].item] : std::to_string(rewards.products[i].item)) + " x" +
                std::to_string(rewards.products[i].quantity) + " (q" + std::to_string(rewards.products[i].quality > 0 ? rewards.products[i].quality : rewards.quality) + ")";
    }
    line += " at base quality " + std::to_string(rewards.quality) + " in " + std::to_string(fresh.size()) + " stack(s)";
    if (rewards.ratings.any()) {
        line += "; artisan ratings of the start: quantity " + std::to_string(std::lround(rewards.ratings.quantity)) + " (" + std::to_string(rewards.extra_units) + " extra unit(s)), rarity " +
                std::to_string(std::lround(rewards.ratings.rarity)) + " (" + std::to_string(rewards.stepped_units) + " step(s) up), experience " + std::to_string(std::lround(rewards.ratings.xp)) + " (x" +
                fmt::format("{:.3f}", rewards.xp_multiplier) + ")";
    }
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (pf) line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " + std::to_string(c.after.level) + ")";
    }
    if (rewards.adventuring_xp > 0) {
        line += "; Adventuring +" + std::to_string(rewards.adventuring_xp) + " XP offered (debt " + std::to_string(xp.before.debt) + " -> " + std::to_string(xp.after.debt) + ", total " + std::to_string(xp.after.xp) +
                (leveled ? ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level) : std::string()) + ")";
    }
    spdlog::log(level_for(claim_logged_, kClaimLogged), "[session] {}: {}", name(), line);
}

size_t PlayerSession::gm_finish_jobs(Connection& conn, Clock::time_point now, const std::string& station_name, const std::string& by) {
    const int64_t now_ms = wall_ms();
    size_t finished = 0;
    if (station_name.empty()) {
        finished = crafting::finish_now(jobs_, std::string(), now_ms);
    } else {
        const auto same = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        };
        std::vector<std::string> keys;
        for (const svc::ProcessingJob& j : jobs_) {
            if (std::find(keys.begin(), keys.end(), j.station_key) != keys.end()) continue;
            const world::WorldActor* a = station_by_key(j.station_key);
            if (!a) continue;
            const content::CraftStationData* defs = world_ ? world_->station_data() : nullptr;
            const auto def = defs ? defs->defs.find(a->station.def) : decltype(defs->defs.end()){};
            if (same(a->station.name, station_name) || (defs && def != defs->defs.end() && same(def->second.name, station_name))) keys.push_back(j.station_key);
        }
        for (const std::string& key : keys) finished += crafting::finish_now(jobs_, key, now_ms);
    }
    if (finished == 0) return 0;
    crafts_finished_ += finished;
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    hand_over_inventory("jobs were finished by a game master", /*periodic=*/false);
    spdlog::info("[session] {}: {} job(s) finished at once by {}{}", name(), finished, by, station_name.empty() ? "" : " at " + station_name);
    run_crafting(conn, now);   // the window is told now
    return finished;
}

// -- quests, slice 0: the talking window --------------------------------------------------------------------------------------------------------------------

namespace {

/// How far from its NPC a conversation may go before the server ends it (the client closes its own window sooner: the interaction range is 25 m).
constexpr double kDialogueRangeCm = 4000.0;

/// What a conversation's conditions ask of the character who talks: the bag, the level, where the player and the NPC stand.
class TalkFacts final : public dialogue::Facts {
public:
    std::vector<svc::InventoryItem> items;
    int                             level = 1;
    double                          player_x = 0, player_y = 0, npc_x = 0, npc_y = 0;
    bool                            has_npc = false;
    int64_t                         citizen_node = 0;
    const world::CityNodes*         nodes = nullptr;
    const quest::QuestLog*          quests = nullptr;   // quests, slice 1: the character's log (null: it has none)

    std::optional<int64_t> quest_count(int64_t quest, const std::string& query) const override { return quests ? quests->quest_count(quest, query) : std::nullopt; }
    std::optional<int64_t> objective_count(int64_t quest, int64_t objective, const std::string& query) const override {
        return quests ? quests->objective_count(quest, objective, query) : std::nullopt;
    }
    int64_t player_variable(const std::string& name) const override { return quests ? quests->variable(name) : 0; }
    int64_t quest_variable(int64_t quest, const std::string& name) const override { return quests ? quests->quest_variable(quest, name) : 0; }

    int64_t item_count(const std::vector<int64_t>& records, const std::vector<std::string>& /*tags*/) const override {
        if (records.empty()) return 0;   // a count by tag needs the items' tags, which the bag does not hold
        int64_t n = 0;
        for (const svc::InventoryItem& it : items) {
            if (std::find(records.begin(), records.end(), it.item_record_id) != records.end()) n += it.quantity;
        }
        return n;
    }

    std::optional<dialogue::ExprValue> call(const std::string& name, const std::vector<dialogue::ExprValue>& args) const override {
        using dialogue::ExprValue;
        constexpr int64_t kPlayer = 1, kNpc = 2;   // the objects an expression passes around
        if (name == "GetPlayer") return ExprValue::of(kPlayer);
        if (name == "GetInteractionTarget") return ExprValue::of(has_npc ? kNpc : 0);
        if (name == "GetCurrentNode") return ExprValue::of(node_at(player_x, player_y));
        if (name == "Node_GetActorCityNode") {
            if (args.size() != 1) return std::nullopt;
            return ExprValue::of(args[0].i == kNpc && has_npc ? node_at(npc_x, npc_y) : node_at(player_x, player_y));
        }
        if (name == "GetCharacterLevel" || name == "GetPlayerLevel") return ExprValue::of(level);
        if (name == "Node_IsPlayerCitizen") return ExprValue::of(citizen_node != 0 ? 1 : 0);
        if (name == "Node_IsPlayerCitizenOfNode") {
            if (args.size() != 2) return std::nullopt;
            return ExprValue::of(citizen_node != 0 && citizen_node == args[1].i ? 1 : 0);
        }
        // the world around: worldd runs no elections, constructions, expansions or buy orders yet, and has no mayors
        static const char* const kNodeActivity[] = {"Node_NumOfServiceBuildingsInState", "Node_NumOfServiceBuilding", "Node_IsNodeMayorElectionActive", "Node_IsNodeMayorPrevoteActive",
                                                    "Node_IsExpansionInGivenState", "Node_IsBuyOrderActive", "Node_IsPlayerMayorCandidate", "Node_IsPlayerMayorCurrentNode",
                                                    "Node_HasPlayerCitizenshipCooldown", "Node_CanPlayerBecomeCitizen"};
        for (const char* f : kNodeActivity) {
            if (name == f) return ExprValue::of(0);
        }
        return std::nullopt;
    }

private:
    int64_t node_at(double x, double y) const {
        if (!nodes) return 0;
        const world::NodeSite site = nodes->node_at(x, y);
        const world::CityNodeInfo* n = nodes->by_rank(site.node_rank);
        return n ? n->guid : 0;
    }
};

}  // namespace

std::unique_ptr<dialogue::Facts> PlayerSession::dialogue_facts(const world::WorldActor* npc) const {
    auto f = std::make_unique<TalkFacts>();
    f->items = inventory_.items();
    f->level = progress_.level;
    const GoodPosition at = checker_.good();
    f->player_x = at.x;
    f->player_y = at.y;
    if (npc) {
        f->has_npc = true;
        f->npc_x = npc->x;
        f->npc_y = npc->y;
    }
    f->citizen_node = pb_.setup().citizen_node;
    f->nodes = world_ ? world_->city_nodes() : nullptr;
    if (quests_on_) f->quests = &quest_log_;
    return f;
}

bool PlayerSession::open_dialogue(Connection& conn, const world::WorldActor& npc, Clock::time_point now) {
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (!book) return false;   // a content.db without the conversations: the NPC answers as it did
    if (talk_.open()) {
        // the window is open already (the client does not click through its own window): a second click on the same NPC changes nothing, on another is ignored
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a click on {} while the talking window is open: ignored", name(), npc.npc.name);
        return true;
    }
    if (!room_for(1)) return true;
    const auto facts = dialogue_facts(&npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    // quests, slice 1: an NPC that offers a quest the character can take starts its talk at the node the offer names; otherwise at its root
    int64_t offered = 0;
    if (const quest::QuestBook* qb = world_->quests(); qb && quests_on_) {
        for (const content::QuestOfferRow& offer : qb->offers(npc.npc.instance_guid, npc.npc.asset_guid)) {
            if (offer.node != 0 && quest_log_.can_start(qb->quest(offer.quest), ev, /*bypass_rules=*/false) == quest::Create::Ok) {
                offered = offer.node;
                break;
            }
        }
    }
    // quests, slice 2: an active Dialogue objective that names this NPC and a node of its conversation starts the talk at that node
    if (const quest::QuestBook* qb2 = world_->quests(); qb2 && quests_on_) {
        if (const int64_t entry = quest::dialogue_entry(quest_log_, *qb2, npc.npc.instance_guid)) offered = entry;
    }
    if (npc.npc.dialogue_root == 0 && offered == 0) return false;   // nothing on offer and no conversation
    const dialogue::Result r = talk_.begin(*book, ev, npc.id, npc.npc.dialogue_root, offered);
    for (const std::string& s : r.skipped) {
        if (dialogue_told_.insert("skipped " + s).second) spdlog::info("[session] {}: a conversation action is not done yet: {} (told once)", name(), s);
    }
    if (r.outcome != dialogue::Outcome::Changed) {
        if (r.silent) {   // quests, slice 2: a placeholder with nothing to show whose entering did its work: the click is answered, no window opens
            spdlog::log(level_for(dialogue_logged_), "[session] {}: talks to {}: {}", name(), npc.npc.name, r.why);
            run_talk_actions(conn, r, npc, now);
            return true;
        }
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: {} has a conversation that cannot be shown ({}): it answers as before", name(), npc.npc.name, r.why);
        return false;
    }
    ++dialogue_opens_;
    const dialogue::Talk::State& s = talk_.state();
    spdlog::log(level_for(dialogue_logged_), "[session] {}: talks to {} (node {}, {} line(s) shown)", name(), npc.npc.name, s.shown_node, s.lines.size());
    run_talk_actions(conn, r, npc, now);   // quests, slice 2: what entering the greeting's nodes does
    send_dialogue(conn, "greeting");
    return true;
}

bootstrap::PlayerBootstrap::DialogueView PlayerSession::dialogue_view(bool exhaust) const {
    const dialogue::Talk::State s = talk_.state();
    bootstrap::PlayerBootstrap::DialogueView v;
    v.exhaust = exhaust;
    if (s.sub) {
        bootstrap::PlayerBootstrap::DialogueView::SubDialogue sd;
        sd.type = s.sub->type;   // a quest offer (2) or a confirmation (4)
        sd.pending_next = s.sub->pending_next;
        sd.previous_node = s.sub->previous_node;
        sd.node = s.sub->node;
        sd.quest = s.sub->quest;
        sd.objectives = s.sub->objectives;
        // the objectives the quest starts with: the ones that are awake or that a start action activates (a quest offer's preview; the hand-in panel has none)
        if (const quest::QuestBook* qb = world_ && s.sub->type == 2 ? world_->quests() : nullptr) {
            if (const content::QuestRow* def = qb->quest(s.sub->quest)) {
                for (const content::QuestObjectiveRow& o : def->objectives) {
                    bool starts = o.awake;
                    for (const content::QuestStartAction& a : def->start_actions) starts = starts || (a.action == "ActivateObjective" && a.objective == o.guid);
                    if (starts) sd.initial_objectives.push_back(o.guid);
                }
            }
        }
        v.sub = std::move(sd);
    }
    v.previous_node = s.previous_node;
    v.previous_choice_node = s.previous_choice_node;
    v.previous_choice_index = s.previous_choice_index;
    v.current_node = s.current_node;
    v.shown_node = s.shown_node;
    v.greeting = s.greeting;
    for (const dialogue::Talk::Line& l : s.lines) v.lines.push_back({l.node, l.index});
    if (s.source != 0 && registry_) {
        if (const world::WorldActor* a = registry_->find(s.source)) v.source = a->actor.guid;
    }
    return v;
}

void PlayerSession::send_dialogue(Connection& conn, const char* /*why*/, bool exhaust) {
    const bootstrap::PlayerBootstrap::DialogueView v = dialogue_view(exhaust);   // a copy: the reply is built when the connection has room for it
    queue("dialogue", [v](bootstrap::PlayerBootstrap& pb) { return pb.dialogue_update(v); }, conn);
}

void PlayerSession::end_dialogue(Connection& conn, const char* why) {
    const world::WorldActor* npc = talk_.open() && registry_ ? registry_->find(talk_.state().source) : nullptr;
    const dialogue::Result r = talk_.close(world_ ? world_->dialogue() : nullptr);
    if (r.outcome != dialogue::Outcome::Changed) return;
    ++dialogue_closes_;
    spdlog::log(level_for(dialogue_logged_), "[session] {}: the conversation ends: {}", name(), why);
    if (npc && !r.actions.empty()) run_talk_actions(conn, r, *npc, quest_last_now_);   // quests, slice 2: leaving the node it showed
    send_dialogue(conn, why, /*exhaust=*/r.offer_answered);
}

bool PlayerSession::npc_offers_quests(const world::WorldActor& npc) const {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    return qb && quests_on_ && !qb->offers(npc.npc.instance_guid, npc.npc.asset_guid).empty();
}

void PlayerSession::on_dialogue_choice(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto response = repl::parse_dialogue_response(rpc.call);
    if (!response) {
        if (dialogue_told_.insert("unreadable").second) {
            spdlog::warn("[session] {}: a ServerSelectDialogueChoice does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    using repl::DialogueResponseType;
    using repl::SubDialogueResponse;
    if (response->response_type == static_cast<uint8_t>(DialogueResponseType::SubDialogue) && response->quest_response == static_cast<uint8_t>(SubDialogueResponse::Close)) {
        // the window closes: the client says it twice and then names the window it opened; the first clears, the others find nothing to clear.  (A close is not held back by the
        // budget: a lost close would leave the conversation open on the server for good.)
        end_dialogue(conn, "the client closed the window");
        return;
    }
    if (!within_budget(dialogue_budget_, "ServerSelectDialogueChoice", now)) return;
    if (response->response_type == static_cast<uint8_t>(DialogueResponseType::SubDialogue) &&
        (response->quest_response == static_cast<uint8_t>(SubDialogueResponse::Accept) || response->quest_response == static_cast<uint8_t>(SubDialogueResponse::Decline))) {
        return on_quest_offer(conn, response->quest_response, now);
    }
    if (response->response_type == static_cast<uint8_t>(DialogueResponseType::ActionWindow)) return;   // a window a line opened was closed: no window opens yet
    if (response->response_type != static_cast<uint8_t>(DialogueResponseType::Choice)) {
        if (dialogue_told_.insert("response " + std::to_string(response->response_type) + "/" + std::to_string(response->quest_response)).second) {
            spdlog::info("[session] {}: a talking-window response of type {} (quest response {}) is not answered yet (told once)", name(), response->response_type, response->quest_response);
        }
        return;
    }
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (dead_ || !book || !talk_.open() || !room_for(1)) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a talking-window choice is refused: {}", name(), dead_ ? "the player is dead" : !talk_.open() ? "no conversation is open" : "the world has no conversations");
        return;
    }
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) {
        end_dialogue(conn, "the NPC is gone");
        return;
    }
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) {
        end_dialogue(conn, "the player is too far from the NPC");
        return;
    }
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    const dialogue::Result r = talk_.choose(*book, ev, *response);
    for (const std::string& s : r.skipped) {
        if (dialogue_told_.insert("skipped " + s).second) spdlog::info("[session] {}: a conversation action is not done yet: {} (told once)", name(), s);
    }
    // storage clerks, step 0: a line that opens the warehouse ends the conversation (it clears below) and the window opens in its place
    bool warehouse_opened = false;
    if (r.open_warehouse != 0) {
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: it opens the storage (access {})", name(), response->choice_index, response->node, r.open_warehouse);
        warehouse_opened = open_warehouse(conn, *npc, r.open_warehouse, now);
    }
    // vendors, step 2: a line that opens a shop opens its window; the conversation is left as it is (the client closes its own window as the shop's opens)
    if (r.open_vendor != 0) {
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: it opens the shop {}", name(), response->choice_index, response->node, r.open_vendor);
        open_talk_shop(conn, *npc, r.open_vendor, now);
    }
    if (r.outcome == dialogue::Outcome::Changed) run_talk_actions(conn, r, *npc, now);   // quests, slice 2: the line's actions, the node left and the nodes entered
    switch (r.outcome) {
        case dialogue::Outcome::Changed:
            ++dialogue_steps_;
            if (!talk_.open()) ++dialogue_closes_;
            spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: {}", name(), response->choice_index, response->node,
                        talk_.open() ? "on to node " + std::to_string(talk_.state().shown_node) : std::string("the conversation ends"));
            if (!warehouse_opened) send_dialogue(conn, "a line was chosen");   // (the warehouse's second half sends the clear after its window call)
            break;
        case dialogue::Outcome::Nothing:
            if (r.open_vendor == 0) spdlog::log(level_for(dialogue_logged_), "[session] {}: line {} of node {} changes nothing: {}", name(), response->choice_index, response->node, r.why);
            break;
        case dialogue::Outcome::Refused:
            ++dialogue_refused_;
            spdlog::log(level_for(dialogue_logged_), "[session] {}: a talking-window choice is refused: {}", name(), r.why);
            break;
    }
    if (warehouse_opened) finish_open_warehouse(conn, now, /*clear=*/r.outcome == dialogue::Outcome::Changed);   // (retail's packet: the window call, the clear, then the lists)
}

void PlayerSession::run_dialogue(Connection& conn, Clock::time_point /*now*/) {
    if (!talk_.open()) return;
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) end_dialogue(conn, "the player walked away");
}

// -- quests, slice 1: take a quest ------------------------------------------------------------------------------------------------------------------------

namespace {

/// The value of the RPC parameter named `name`, null when the call was not decoded or the parameter is absent (a parameter at its default is not sent).
const repl::Value* quest_rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

/// An integer parameter, whatever its width, exact (a record id is 63 bits: a double would round it): 0 when absent.
int64_t quest_int_value(const repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

}  // namespace


void PlayerSession::init_quests(PlayerWorld& world, const svc::Character& character) {
    quest_worker_ = world.quest_worker();
    const quest::QuestBook* book = world.quests();
    if (!quest_worker_ || !book || !character.quest_log) {
        spdlog::info("[session] {}: quests are off for this character ({})", pb_.setup().name,
                     !book ? "this content.db has no quest tables" : !quest_worker_ ? "worldd has no quest service" : "its quest log did not load at the login");
        return;
    }
    const double now = pb_.setup().world_time;
    const size_t repaired = quest_log_.load(*character.quest_log, book->data(), now);
    quests_on_ = true;
    std::string err;
    repl::FastArrays* arrays = pb_.quest_arrays();
    const spawns::AreaReport* area = world.area_report();
    quest::RegistryPoints points(world.registry(), area ? area->data.get() : nullptr);   // (the map pins of the objectives that are active now: the client draws them from the join)
    quest::MarkerContext markers;
    markers.book = book;
    markers.points = &points;
    if (!arrays || !quest::sync(*arrays, quest_log_, &err, &markers)) {
        spdlog::warn("[session] {}: the quest log does not fit the quest component, so quests are off for this character: {}", pb_.setup().name, err);
        quests_on_ = false;
        return;
    }
    log_quest_markers(markers);
    quest_worker_->watch(character_id_);
    spdlog::info("[session] {}: quest log: {} quest(s) taken, {} finished, {} variable(s){}", pb_.setup().name, quest_log_.instances().size(), quest_log_.completed().size(),
                 quest_log_.variables().size(), repaired ? " (" + std::to_string(repaired) + " repaired or dropped: the log is saved again)" : "");
}

void PlayerSession::quest_sync(Connection& conn, const char* why) {
    quests_touched_ = true;
    givers_dirty_ = true;   // (what the NPCs around offer depends on the log)
    repl::FastArrays* arrays = pb_.quest_arrays();
    std::string err;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    std::optional<quest::RegistryPoints> points;
    quest::MarkerContext markers;
    if (qb && registry_) {   // the map pins of the objectives that became active
        const spawns::AreaReport* area = world_ ? world_->area_report() : nullptr;
        points.emplace(*registry_, area ? area->data.get() : nullptr);
        markers.book = qb;
        markers.points = &*points;
    }
    if (!arrays || !quest::sync(*arrays, quest_log_, &err, markers.points ? &markers : nullptr)) {
        spdlog::warn("[session] {}: the quest component is not brought in line with the log ({}): {}", name(), why, err);
        return;
    }
    log_quest_markers(markers);
    queue("QuestConsumerComponent", [](bootstrap::PlayerBootstrap& pb) { return pb.quest_update(); }, conn, /*must_build=*/false);
}

void PlayerSession::start_quest(Connection& conn, const content::QuestRow& def, int64_t source, const char* why, Clock::time_point now) {
    ++quests_taken_;
    spdlog::log(level_for(quests_logged_, 100), "[session] {}: took quest {} ({}), {} objective(s) ({})", name(), def.name, def.guid, def.objectives.size(), why);
    // quests, slice 2: the quest engine takes the quest (all of its start actions run and the objectives settle) and the client is told in one update
    quest_engine(conn, now, "a quest was taken", [&](quest::Engine& eng) { eng.start(def, source); });
    if (!quest_log_.find(def.guid)) {   // (a world with no conversations' book has no engine: the quest joins the log as it did in the first slice)
        quest_log_.start(def, quest_clock(now), source);
        quest_sync(conn, "a quest was taken");
    }
}

void PlayerSession::on_quest_offer(Connection& conn, uint8_t response, Clock::time_point now) {
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (dead_ || !book || !qb || !talk_.open() || !talk_.sub() || !room_for(2)) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: an answer to a quest offer is refused: {}", name(), dead_ ? "the player is dead" : !talk_.open() ? "no conversation is open" : !talk_.sub() ? "no offer is open" : "the world has no quests");
        return;
    }
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return end_dialogue(conn, "the player is too far from the NPC");
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    const dialogue::Talk::Sub sub = *talk_.sub();
    bool taken = false, back = false;
    if (sub.type == 4) {   // quests, slice 2: a confirmation (the path a gatewatcher's welcome asks the player to confirm): a yes goes on, a no goes back to the line's node
        taken = response == static_cast<uint8_t>(repl::SubDialogueResponse::Accept);
        back = response == static_cast<uint8_t>(repl::SubDialogueResponse::Decline);
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: the player {} the confirmation of node {}", name(), taken ? "confirmed" : "declined", sub.node);
    } else if (response == static_cast<uint8_t>(repl::SubDialogueResponse::Accept)) {
        const content::QuestRow* def = qb->quest(sub.quest);
        if (!quests_on_) {
            ++quests_refused_;
            spdlog::warn("[session] {}: the offer of quest {} is not taken: quests are off for this character (the quest service did not answer at the login)", name(), sub.quest);
        } else if (const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false); c != quest::Create::Ok) {
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: the offer of quest {} is refused: {}", name(), sub.quest, quest::create_name(c));
        } else {
            start_quest(conn, *def, npc->npc.instance_guid, "the player took the offer", now);
            taken = true;
        }
    } else {
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: the player declined the offer of quest {}", name(), sub.quest);
    }
    const dialogue::Result r = talk_.finish_sub(*book, ev, taken, back);
    run_talk_actions(conn, r, *npc, now);   // quests, slice 2: what entering the node after the answer does
    if (r.outcome == dialogue::Outcome::Changed) {
        ++dialogue_steps_;
        if (!talk_.open()) ++dialogue_closes_;
        send_dialogue(conn, "a quest offer was answered", /*exhaust=*/true);
    }
}

bool PlayerSession::end_quest(Connection& conn, int64_t record, const char* kind, Clock::time_point now) {
    const std::string k = kind;
    const bool ok = k == "finish" ? quest_log_.finish(record) : quest_log_.remove(record);
    if (!ok) return false;
    if (k == "abandon") ++quests_abandoned_;
    remove_quest_items(conn, record, now);
    quest_sync(conn, kind);
    return true;
}

void PlayerSession::remove_quest_items(Connection& conn, int64_t quest, Clock::time_point /*now*/) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    items::Outcome all;
    all.ok = true;
    for (const svc::InventoryItem& it : inventory_.items()) {
        if (it.container != svc::ItemContainer::Quest) continue;   // slice 1 clears the quest bag only
        const auto* links = qb->item_links(it.item_record_id);
        if (!links) continue;
        bool bound = false;
        for (const content::QuestItemLink& l : *links) bound = bound || (l.quest == quest && (l.kind == "delete_all" || l.kind == "delete_one"));
        if (!bound) continue;
        items::Outcome o = inventory_.consume(it.container, it.slot, it.instance_guid, it.quantity);
        if (!o.ok) continue;
        all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (all.events.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::info("[session] {}: {} quest item event(s) left the quest bag with quest {}", name(), events.size(), quest);
}

void PlayerSession::on_abandon_quests(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const repl::Value* guids = quest_rpc_value(rpc, "QuestGuids");
    const repl::ArrayValue* list = guids ? guids->get_if<repl::ArrayValue>() : nullptr;
    if (!rpc.call.decoded || !list || !qb || !quests_on_) {
        ++quests_refused_;
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: ServerAbandonQuests is ignored: {}", name(), !quests_on_ ? "quests are off" : !rpc.call.decoded ? "its parameters do not decode" : "it names no quest");
        return;
    }
    if (list->elements.size() > 64) {
        ++quests_refused_;
        return;
    }
    for (const repl::ElementState& e : list->elements) {
        quest::Guid128 g;
        uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
        for (uint16_t i = 0; i < 4; ++i) {
            const auto it = e.values.find(static_cast<uint16_t>(i + 1));
            const auto* v = it == e.values.end() ? nullptr : it->second.get_if<int64_t>();
            *words[i] = v ? static_cast<uint32_t>(*v) : 0;
        }
        const quest::QuestInstance* q = quest_log_.find_guid(g);
        if (!q) {
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: abandon of quest instance {}: it is not in the log", name(), g.hex());
            continue;
        }
        const content::QuestRow* def = qb->quest(q->record);
        if (def && !def->can_abandon) {   // the client drops these itself; the server refuses them too
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: quest {} cannot be abandoned (its record says so)", name(), def->name);
            continue;
        }
        const int64_t record = q->record;
        end_quest(conn, record, "abandon", now);
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: abandoned quest {}", name(), def ? def->name : std::to_string(record));
    }
}

void PlayerSession::apply_quest_command(Connection& conn, const svc::QuestCommand& cmd, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb || !quests_on_) return;
    const content::QuestRow* def = cmd.quest.empty() ? nullptr : qb->find(cmd.quest);
    if (cmd.kind != "clear" && cmd.kind != "set-var" && !def) {
        spdlog::warn("[session] {}: GM command {} {}: no such quest", name(), cmd.kind, cmd.quest);
        return;
    }
    if (cmd.kind == "give") {
        const auto facts = dialogue_facts(nullptr);
        const dialogue::Book* book = world_->dialogue();
        if (!book) return;
        dialogue::Evaluator ev(*book, *facts);
        const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/true);
        if (c != quest::Create::Ok) {
            spdlog::warn("[session] {}: GM give {}: refused, {}", name(), def->name, quest::create_name(c));
            return;
        }
        start_quest(conn, *def, 0, "a GM gave it", now);
    } else if (cmd.kind == "finish" || cmd.kind == "fail" || cmd.kind == "abandon") {
        if (!end_quest(conn, def->guid, cmd.kind.c_str(), now)) spdlog::warn("[session] {}: GM {} {}: the quest is not in the log", name(), cmd.kind, def->name);
        else spdlog::info("[session] {}: GM {} {}", name(), cmd.kind, def->name);
    } else if (cmd.kind == "clear") {
        quest_log_.clear(def ? def->guid : 0);
        quests_touched_ = true;
        spdlog::info("[session] {}: GM clear {}", name(), def ? def->name : "(the finished quests and the variables)");
    } else if (cmd.kind == "set-var") {
        quest_log_.set_variable(cmd.name, cmd.value);
        quests_touched_ = true;
        spdlog::info("[session] {}: GM set-var {} = {}", name(), cmd.name, cmd.value);
    }
}

void PlayerSession::on_quest_debug_rpc(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string_view n = rpc.call.rpc->name;
    char params[160];
    std::snprintf(params, sizeof params, "%s", rpc.call.decoded ? "" : "the parameters did not decode");
    if (!gm_rpc_admitted(rpc.call.rpc->name, params, now) || !rpc.call.decoded) return;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb || !quests_on_) return;
    if (n == "ServerDebugStartQuest") {
        const int64_t id = quest_int_value(quest_rpc_value(rpc, "Guid"));
        const repl::Value* b = quest_rpc_value(rpc, "bBypassActivationRules");
        const bool bypass = b && b->get_if<bool>() && *b->get_if<bool>();
        const content::QuestRow* def = qb->quest(id);
        const dialogue::Book* book = world_->dialogue();
        if (!def || !book) {
            spdlog::warn("[gm] {}: ServerDebugStartQuest {}: no such quest", name(), id);
            return;
        }
        const auto facts = dialogue_facts(nullptr);
        dialogue::Evaluator ev(*book, *facts);
        const quest::Create c = quest_log_.can_start(def, ev, bypass);
        if (c != quest::Create::Ok) {
            spdlog::warn("[gm] {}: ServerDebugStartQuest {} ({}): refused, {}", name(), def->name, bypass ? "rules bypassed" : "rules asked", quest::create_name(c));
            return;
        }
        ++gm_rpcs_answered_;
        start_quest(conn, *def, 0, "ServerDebugStartQuest", now);
    } else if (n == "ServerDebugEndQuest") {
        const int64_t id = quest_int_value(quest_rpc_value(rpc, "Guid"));
        if (end_quest(conn, id, "finish", now)) ++gm_rpcs_answered_;
        else spdlog::warn("[gm] {}: ServerDebugEndQuest {}: the quest is not in the log", name(), id);
    } else if (n == "ServerDebugFailQuestByInstance") {
        quest::Guid128 g;
        uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
        const char* const letters[4] = {"Guid.A", "Guid.B", "Guid.C", "Guid.D"};
        for (int i = 0; i < 4; ++i) *words[i] = static_cast<uint32_t>(quest_int_value(quest_rpc_value(rpc, letters[i])));
        const quest::QuestInstance* q = quest_log_.find_guid(g);
        if (q && end_quest(conn, q->record, "fail", now)) ++gm_rpcs_answered_;
        else spdlog::warn("[gm] {}: ServerDebugFailQuestByInstance {}: the quest is not in the log", name(), g.hex());
    } else if (n == "ServerClearCompletedQuests") {
        quest_log_.clear(0);
        quests_touched_ = true;
        ++gm_rpcs_answered_;
        spdlog::info("[gm] {}: ServerClearCompletedQuests: the finished quests and the variables are forgotten", name());
    }
}

void PlayerSession::run_quests(Connection& conn, Clock::time_point now) {
    if (!quests_on_ || !quest_worker_ || dead_) return;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    // the GM commands that arrived
    for (const svc::QuestCommand& cmd : quest_worker_->take(character_id_)) apply_quest_command(conn, cmd, now);
    // a zone of influence that starts a quest for a character who stands in it (once a second): the starting areas' first quests
    if (phase_in_done_ && now - zone_checked_ >= std::chrono::seconds(1)) {
        zone_checked_ = now;
        const GoodPosition at = checker_.good();
        const int64_t zoi = static_cast<int64_t>(world::zoi_at(at.x, at.y));
        if (zoi != zoi_seen_) {
            zoi_seen_ = zoi;
            for (const int64_t q : qb->zone_quests(zoi)) {
                const content::QuestRow* def = qb->quest(q);
                const dialogue::Book* book = world_->dialogue();
                if (!def || !book) continue;
                const auto facts = dialogue_facts(nullptr);
                dialogue::Evaluator ev(*book, *facts);
                const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false);
                if (c == quest::Create::Ok) start_quest(conn, *def, 0, "the character stands in the zone that starts it", now);
                else spdlog::log(level_for(quests_logged_, 100), "[session] {}: the zone {} would start quest {}: not taken, {}", name(), zoi, def->name, quest::create_name(c));
            }
        }
    }
    // quests, slice 2: the places, the items carried, the rules; the rewards that waited
    tick_quests(conn, now);
    // the save
    if (quest_log_.changed()) {
        quest_worker_->save(character_id_, quest_log_.to_saved());
        quest_log_.clear_changed();
    }
}


// -- vendors, step 2: the shopkeepers who talk ----------------------------------------------------------------------------------------------------------

namespace {

/// What a shop's predicates are asked: the node the player stands in and the node the shopkeeper stands in.
vendor::ShopFacts shop_facts_for(const world::CityNodes* nodes, const GoodPosition& player, const world::WorldActor* npc) {
    vendor::ShopFacts facts;
    facts.nodes = nodes;
    facts.player_x = player.x;
    facts.player_y = player.y;
    if (npc) {
        facts.has_npc = true;
        facts.npc_x = npc->x;
        facts.npc_y = npc->y;
    }
    return facts;
}

}  // namespace

bool PlayerSession::open_talk_shop(Connection& conn, const world::WorldActor& npc, int64_t def, Clock::time_point /*now*/) {
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        spdlog::log(level_for(vendor_logged_), "[session] {}: {}'s shop {} does not open: {}", name(), npc.npc.name, def, why);
        return false;
    };
    if (!shops || !shops->def(def) || !shops->is_open(def)) return refuse("worldd opens no such shop yet");
    // only a shop of the shopkeeper's own conversation
    if (!book || book->vendor_defs(npc.npc.dialogue_root).count(def) == 0) return refuse("the conversation of " + npc.npc.name + " opens no such shop");
    const GoodPosition at = checker_.good();
    const double dx = npc.x - at.x, dy = npc.y - at.y, dz = npc.z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
        return refuse("the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away (a shop opens within 12 m)");
    }
    // the window answers once: the line the server ran and the request the client may send for the same shop are one opening
    if (vendor_actor_ == npc.id && vendor_def_ == def && vendor_by_talk_) {
        spdlog::log(level_for(vendor_logged_), "[session] {}: the shop {} of {} is open already: not answered again", name(), def, npc.npc.name);
        return true;
    }
    if (!room_for(2)) return false;
    vendor_actor_ = npc.id;
    vendor_def_ = def;
    vendor_by_talk_ = true;
    ++vendor_opens_;
    // what the window hides or greys out for this player and this shopkeeper
    const vendor::ShopFacts facts = shop_facts_for(world_->city_nodes(), at, &npc);
    bool unreadable = false;
    const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow& p) {
        const auto v = dialogue::evaluate_expression(p.expression, facts);
        if (!v) unreadable = true;
        return v && v->i != 0;
    };
    repl::UnavailableList unavailable = shops->unavailable(def, holds);
    size_t hidden_tabs = 0, listings = 0;
    for (const repl::UnavailableTab& t : unavailable) {
        hidden_tabs += t.inventory.hidden ? 1 : 0;
        listings += t.listings.size();
    }
    spdlog::log(level_for(vendor_logged_), "[session] {}: opens the shop {} of {} from the conversation: {} tab(s) listed as unavailable ({} hidden), {} listing(s){}", name(), shops->def(def)->name,
                npc.npc.name, unavailable.size(), hidden_tabs, listings, unreadable ? "; a predicate does not evaluate and does not hold" : "");
    const FIntrepidNetworkGUID guid = npc.actor.guid;
    queue("ClientOpenVendor", [def, guid, unavailable = std::move(unavailable)](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(def, guid, unavailable); }, conn, /*must_build=*/true);
    return true;
}

void PlayerSession::on_open_vendor(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto request = repl::parse_server_open_vendor(rpc.call);
    if (!request) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerOpenVendor does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerOpenVendor", now) || !room_for(2)) return;
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        spdlog::log(level_for(vendor_logged_), "[session] {}: the client's ServerOpenVendor for shop {} is refused: {}", name(), request->vendor, why);
    };
    if (dead_) return refuse("the character is dead");
    // the NPC the client may name is the one it talks to; the shopkeeper's own guid in the request is not what decides
    if (!talk_.open()) return refuse("no conversation is open");
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return refuse("the NPC the player talks to is gone");
    open_talk_shop(conn, *npc, request->vendor, now);
}

// The vendor window waits for this answer (vendor/node_tax.h): the exe's UVendorMenu::InitializeVendor sent the request, and only the table's
// arrival makes the window fill its listings, taxes and funds.  Retail answers the next packet with the node's table.
void PlayerSession::on_node_tax_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto node = repl::parse_node_tax_request(rpc.call);
    ++tax_requests_;
    if (!node) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerGetNodeTaxTable does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(tax_budget_, "ServerGetNodeTaxTable", now) || !room_for(1)) return;
    if (dead_) return;   // the window closed with the death
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) {
        spdlog::log(level_for(tax_logged_), "[session] {}: the client asks for the taxes of node {} but has no vendor window open ({}): ignored", name(), *node, why);
        return;
    }
    const int64_t asked = *node;
    if (!a->npc.is_gm_vendor) {
        // a town shop: the table of a node nobody has changed, for a node the world knows; nothing for any other (the client's window then stays empty, and a made-up node id gets no answer)
        const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
        if (nodes && !nodes->by_guid(asked)) {
            spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}, which the world does not know: no answer", name(), a->npc.name, asked);
            return;
        }
        ++tax_tables_sent_;
        spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}: sending the default table ({} tags)", name(), a->npc.name, asked,
                    vendor::default_table(asked).entries.size());
        queue("ClientSetNodeTaxTable", [asked](bootstrap::PlayerBootstrap& pb) { return pb.node_tax_table(vendor::default_table(asked)); }, conn, /*must_build=*/true);
        return;
    }
    ++tax_tables_sent_;
    spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}: sending the untaxed table ({} tags)", name(), a->npc.name, *node,
                vendor::untaxed_table(*node).entries.size());
    queue("ClientSetNodeTaxTable", [asked](bootstrap::PlayerBootstrap& pb) { return pb.node_tax_table(vendor::untaxed_table(asked)); }, conn, /*must_build=*/true);
}

void PlayerSession::on_vendor_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto cart = repl::parse_vendor_purchase(rpc.call);
    if (!cart) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerHandleVendorPurchase does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerHandleVendorPurchase", now) || !room_for(6)) return;
    const char* what = "ServerHandleVendorPurchase";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const vendor::GmVendors* gm = world_ ? world_->gm_vendors() : nullptr;
    if (dead_) return refuse("the character is dead: the vendor window closed with the death");
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) return refuse(why);
    // vendors, step 0: a town shop is paid for from the purse
    if (!a->npc.is_gm_vendor) return buy_from_town_shop(conn, *cart, now);
    if (!db || !gm) return refuse("worldd has no GM vendors");
    const vendor::GmVendor* def = gm->find(vendor_def_);
    if (!def) return refuse("the open window's vendor def is unknown");
    const size_t free = static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack);
    const vendor::PurchasePlan plan = gm->plan_purchase(*db, *def, *cart, free);
    if (!plan.ok) return refuse(plan.refusal);
    std::vector<svc::InventoryItem> fresh;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (const use::Granted& g : plan.stacks) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = g.record;
        it.instance_guid = fresh_guid();
        it.quantity = g.quantity;
        it.quality = g.quality;
        it.durability = g.durability;
        it.created_at_ms = created;
        // an instance guid must be new within this batch too (fresh_guid only looks at the inventory)
        for (const svc::InventoryItem& other : fresh) {
            if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
        fresh.push_back(std::move(it));
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok) return refuse(outcome.refusal);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_purchases_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} (free): {} storage event(s)", name(), plan.summary, outcome.events.size());
}
// ~ Cyb - C-O-A ~

// ============================================================================================================================
//  the vendors milestone, step 0: the purse and the general merchant
// ============================================================================================================================

namespace {

/// The stacks of `count` of an item: the record's stack size each, at `quality` (0: its default quality) and with the full durability of that quality, as the GM vendors' goods are made.
void stacks_of(int64_t record, int32_t count, const content::ItemRecord& rec, std::vector<svc::InventoryItem>& out, int64_t created_ms, int32_t quality = 0) {
    const int32_t stack = rec.max_stack_size > 0 ? rec.max_stack_size : 1;
    for (int32_t left = count; left > 0;) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;   // Inventory::grant routes it
        it.item_record_id = record;
        it.quantity = std::min(left, stack);
        it.quality = quality > 0 ? quality : rec.default_quality;
        it.durability = rec.max_durability_at(it.quality);
        it.created_at_ms = created_ms;
        left -= it.quantity;
        out.push_back(std::move(it));
    }
}

}  // namespace

int64_t PlayerSession::town_shop_of(const world::WorldActor& npc) const {
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    if (!shops || npc.kind != world::ActorKind::Npc || npc.npc.is_gm_vendor) return 0;
    return shops->shop_of(npc.npc.instance_guid, npc.npc.asset_guid);
}

void PlayerSession::buy_from_town_shop(Connection& conn, const repl::VendorPurchaseCart& cart, Clock::time_point now) {
    const char* what = "ServerHandleVendorPurchase";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!shops || !db) return refuse("worldd has no town shops");
    // the shop's predicates: the node the player stands in, and the node the shopkeeper stands in
    const GoodPosition at = checker_.good();
    const world::WorldActor* keeper = registry_ ? registry_->find(vendor_actor_) : nullptr;
    const vendor::ShopFacts facts = shop_facts_for(world_->city_nodes(), at, keeper);
    bool unreadable = false;
    const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow& p) {
        const auto v = dialogue::evaluate_expression(p.expression, facts);
        if (!v) unreadable = true;
        return v && v->i != 0;
    };
    const vendor::TownShops::Quote quote = shops->quote(vendor_def_, cart, shops->default_rate(vendor_def_), holds);
    if (!quote.ok) {
        if (unreadable) spdlog::warn("[session] {}: a shop predicate does not evaluate: the listing is not sold", name());
        return refuse(quote.refusal);
    }
    // a price in Glint is paid with Glint T1 items of the bags (the material bags hold them); a price in money, from the purse
    const bool in_glint = quote.currency == items::kGlintTier;
    if (!in_glint && !wallet_.can_pay(quote.total)) {
        return refuse("the purse holds " + std::to_string(wallet_.copper()) + " copper and the cart costs " + std::to_string(quote.total) + " (not enough money)");
    }
    if (in_glint && inventory_.count_of(items::kGlintT1Item) < quote.total) {
        return refuse("the bags hold " + std::to_string(inventory_.count_of(items::kGlintT1Item)) + " Glint T1 and the cart costs " + std::to_string(quote.total) + " (not enough Glint)");
    }
    // the goods, as stacks, routed by the bag rule; a cart that does not fit whole is refused whole
    std::vector<svc::InventoryItem> fresh;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (const vendor::TownShops::Line& line : quote.lines) {
        std::string error;
        const auto rec = db->item(line.item, error);
        if (!rec) return refuse(error.empty() ? "content.db has no item record " + std::to_string(line.item) : "content.db failed: " + error);
        stacks_of(line.item, line.quantity, *rec, fresh, created);
    }
    for (svc::InventoryItem& it : fresh) {
        it.instance_guid = fresh_guid();
        // an instance guid must be new within this batch too (fresh_guid only looks at the inventory)
        for (const svc::InventoryItem& other : fresh) {
            if (&other != &it && other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
    }
    // paying in Glint: the stacks give first, and the goods must fit behind that; any failure puts the bags back as they were
    const items::Inventory kept = inventory_;
    items::Outcome paid;
    if (in_glint) {
        paid = inventory_.take(items::kGlintT1Item, quote.total);
        if (!paid.ok) return refuse(paid.refusal);
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok || (in_glint && paid.events.size() + outcome.events.size() > bootstrap::kMaxEventsPerChain)) {
        inventory_ = kept;
        return refuse(outcome.ok ? "the payment and the goods are more events than one answer holds" : outcome.refusal);
    }
    const int64_t before = wallet_.copper();
    if (!in_glint) wallet_.pay(quote.total);   // checked above: it cannot be short
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_purchases_;
    if (in_glint) {
        // the retail answer to a purchase paid in items: the Remove of the Glint stack(s) (no toast), the callout in the Glint tier (no Currencies change), then the Add of the goods
        std::vector<bootstrap::ItemEvent> paid_events = item_events_for(paid);
        std::vector<bootstrap::ItemEvent> bought = item_events_for(outcome);
        add_item_toasts(bought, /*removals=*/false);
        const int64_t glint_delta = -quote.total;
        queue("purchase answer", [paid_events, bought, glint_delta](bootstrap::PlayerBootstrap& pb) { return pb.item_payment_answer(items::kGlintTier, glint_delta, paid_events, bought); }, conn, /*must_build=*/true);
        spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} line(s) at the shop for {} Glint T1: {} + {} storage event(s)", name(), quote.lines.size(), quote.total, paid.events.size(),
                    outcome.events.size());
        return;
    }
    // the answer: the callout first, then the Add events (in one chain), then the new balance in the pawn's Currencies; each Add carries the "Acquired" toast of the item feed
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/false);
    const int64_t delta = -quote.total;
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        if (at_event == 0) {
            queue("purchase answer", [part, delta](bootstrap::PlayerBootstrap& pb) { return pb.purchase_answer(items::kMoneyTier, delta, part); }, conn, /*must_build=*/true);
        } else {
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} line(s) at the shop for {} copper ({} -> {}): {} storage event(s)", name(), quote.lines.size(), quote.total, before,
                wallet_.copper(), outcome.events.size());
}

// ============================================================================================================================
//  the vendors milestone, step 1: selling, repairing and the item toasts
// ============================================================================================================================

void PlayerSession::add_item_toasts(std::vector<bootstrap::ItemEvent>& events, bool removals) const {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    std::map<int64_t, std::string> names;
    std::map<int64_t, items::ItemRarityFacts> rarities;   // the rarity each toast's colour comes from: the record's quality curve held inside its RarityMin..RarityMax
    const auto name_of = [&](int64_t record) -> const std::string& {
        const auto known = names.find(record);
        if (known != names.end()) return known->second;
        std::string shown = "Unknown item";
        std::string error;
        items::ItemRarityFacts facts = items::default_rarity_facts();
        if (db) {
            if (const auto rec = db->item(record, error)) {
                shown = !rec->display_name.empty() ? rec->display_name : rec->name;
                facts = *rec;
            }
        }
        rarities.emplace(record, facts);
        return names.emplace(record, std::move(shown)).first->second;
    };
    const int64_t made = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto same_slot = [](const bootstrap::ItemEvent& a, const bootstrap::ItemEvent& b) { return a.storage == b.storage && a.slot == b.slot; };
    for (size_t i = 0; i < events.size(); ++i) {
        bootstrap::ItemEvent& ev = events[i];
        if (!ev.item.has_item) continue;
        if (ev.type == repl::StorageEventType::Add) {
            // a stack that was topped up arrives as the Remove of the old stack and the Add of the new total: the toast says what was added
            repl::ItemInstance shown = ev.item;
            if (i > 0 && events[i - 1].type == repl::StorageEventType::Remove && same_slot(events[i - 1], ev) && events[i - 1].item.has_item &&
                events[i - 1].item.quantity < ev.item.quantity) {
                shown.quantity = ev.item.quantity - events[i - 1].item.quantity;
            }
            const std::string& shown_name = name_of(static_cast<int64_t>(shown.record_id));
            ev.callout = items::acquired_callout(shown, shown_name, rarities.at(static_cast<int64_t>(shown.record_id)), made);
        } else if (removals && ev.type == repl::StorageEventType::Remove) {
            const bool restacked = i + 1 < events.size() && events[i + 1].type == repl::StorageEventType::Add && same_slot(events[i + 1], ev);
            if (!restacked) {
                const std::string& removed_name = name_of(static_cast<int64_t>(ev.item.record_id));
                ev.callout = items::removed_callout(ev.item, removed_name, rarities.at(static_cast<int64_t>(ev.item.record_id)), made);
            }
        }
    }
}

void PlayerSession::on_vendor_transaction(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto cart = repl::parse_vendor_transaction(rpc.call);
    if (!cart) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerHandleVendorTransaction does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerHandleVendorTransaction", now) || !room_for(6)) return;
    const char* what = "ServerHandleVendorTransaction";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    if (dead_) return refuse("the character is dead: the vendor window closed with the death");
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) return refuse(why);
    if (a->npc.is_gm_vendor) {
        if (!vendor_sale_logged_) {
            vendor_sale_logged_ = true;
            spdlog::info("[session] {}: a sale to a vendor: GM vendors buy nothing, ignored (further ones are not logged)", name());
        }
        return;
    }
    if (cart->vendor != vendor_def_) return refuse("the cart names another shop than the one that is open");
    if (cart->operation == 2) return sell_to_town_shop(conn, *cart, now);
    if (cart->operation == 4) return repair_at_town_shop(conn, *cart, now);
    return refuse("an operation of " + std::to_string(cart->operation) + " is not made (a sale is 2, a repair 4)");
}

void PlayerSession::sell_to_town_shop(Connection& conn, const repl::VendorTransactionCart& cart, Clock::time_point now) {
    const char* what = "ServerHandleVendorTransaction";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!shops || !db) return refuse("worldd has no town shops");
    if (cart.instances.empty() || cart.instances.size() > vendor::kMaxShopCartLines) return refuse("a sale of " + std::to_string(cart.instances.size()) + " items");
    // every item first: it is the character's, in a bag, one the shop buys; the whole stack is sold
    std::vector<svc::InventoryItem> sold;
    std::set<std::string> seen;
    int64_t total = 0;
    for (const repl::GuidValue& g : cart.instances) {
        const std::string guid = items::guid_hex(g);
        if (!seen.insert(guid).second) return refuse("the same item twice in one sale");
        const svc::InventoryItem* held = inventory_.find_guid(guid);
        if (!held) return refuse("an item that is in none of this character's bags");
        if (!is_bag(held->container)) return refuse("an item that is worn or in the quest storage is not sold");
        std::string error;
        const auto facts = db->item_sale(held->item_record_id, error);
        if (!facts) return refuse(error);
        const vendor::SaleQuote quote = vendor::quote_sale(shops->data(), vendor_def_, held->item_record_id, held->quantity, held->quality, *facts, items::kMoneyTier, progress_.level);
        if (!quote.ok) return refuse("item " + std::to_string(held->item_record_id) + " is not sold: " + quote.refusal);
        total += quote.total;
        if (total > items::kMoneyCap) break;
        sold.push_back(*held);
    }
    if (total > wallet_.room()) {
        return refuse("the sale comes to " + std::to_string(total) + " copper and the purse holds " + std::to_string(wallet_.copper()) + " of at most " + std::to_string(items::kMoneyCap) +
                      " (a sale past the cap is refused whole)");
    }
    // the stacks leave; all or nothing
    const items::Inventory backup = inventory_;
    items::Outcome all;
    all.ok = true;
    for (const svc::InventoryItem& it : sold) {
        items::Outcome o = inventory_.consume(it.container, it.slot, it.instance_guid, it.quantity);
        if (!o.ok) {
            inventory_ = backup;
            return refuse(o.refusal);
        }
        all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    const int64_t before = wallet_.copper();
    wallet_.give(total);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_sales_;
    // the answer, in the order of a sale: the Remove events (each with its "Removed" toast), then the money callout; then the new balance in the pawn's Currencies
    std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    add_item_toasts(events, /*removals=*/true);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        if (at + bootstrap::kMaxEventsPerChain >= events.size()) {
            queue("sale answer", [part, total](bootstrap::PlayerBootstrap& pb) { return pb.sale_answer(items::kMoneyTier, total, part); }, conn, /*must_build=*/true);
        } else {
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: sold {} stack(s) to the shop for {} copper ({} -> {}): {} storage event(s)", name(), sold.size(), total, before, wallet_.copper(),
                all.events.size());
}

void PlayerSession::repair_at_town_shop(Connection& conn, const repl::VendorTransactionCart& cart, Clock::time_point now) {
    const char* what = "ServerHandleVendorTransaction";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!shops || !db) return refuse("worldd has no town shops");
    if (cart.instances.empty() || cart.instances.size() > vendor::kMaxShopCartLines) return refuse("a repair of " + std::to_string(cart.instances.size()) + " items");
    const vendor::TaxRate tax = shops->default_rate(vendor_def_);
    struct Mended {
        svc::InventoryItem item;
        float              full = 0;
    };
    std::vector<Mended> mended;
    std::set<std::string> seen;
    int64_t total = 0;
    for (const repl::GuidValue& g : cart.instances) {
        const std::string guid = items::guid_hex(g);
        if (!seen.insert(guid).second) return refuse("the same item twice in one repair");
        const svc::InventoryItem* held = inventory_.find_guid(guid);
        if (!held) return refuse("an item that is in none of this character's storages");
        if (held->container != svc::ItemContainer::Backpack && held->container != svc::ItemContainer::Equipment) return refuse("only an item in the item bag or worn is repaired");
        std::string error;
        const auto rec = db->item(held->item_record_id, error);
        if (!rec) return refuse(error.empty() ? "content.db has no item record " + std::to_string(held->item_record_id) : error);
        const auto facts = db->item_sale(held->item_record_id, error);
        if (!facts) return refuse(error);
        const float full = rec->max_durability_at(held->quality);
        const vendor::RepairQuote quote = vendor::quote_repair(shops->data(), vendor_def_, held->item_record_id, held->quality, static_cast<double>(held->durability), static_cast<double>(full),
                                                               *facts, items::kMoneyTier, tax, progress_.level);
        if (!quote.ok) return refuse("item " + std::to_string(held->item_record_id) + " is not repaired: " + quote.refusal);
        total += quote.total;
        mended.push_back({*held, full});
    }
    if (!wallet_.can_pay(total)) {
        return refuse("the purse holds " + std::to_string(wallet_.copper()) + " copper and the repair costs " + std::to_string(total) + " (not enough money)");
    }
    // the wear is mended to full; the client is told with one durability update per storage and the money callout
    struct Update {
        bootstrap::ItemStorage storage;
        int32_t                slot;
        float                  durability;
    };
    std::vector<Update> updates;
    for (const Mended& m : mended) {
        if (!inventory_.set_durability(m.item.container, m.item.slot, m.item.instance_guid, m.full)) return refuse("the item's durability could not be set");
        updates.push_back({bootstrap::item_storage_of(m.item.container), m.item.slot, m.full});
    }
    const int64_t before = wallet_.copper();
    wallet_.pay(total);   // checked above: it cannot be short
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_repairs_;
    for (size_t i = 0; i < updates.size(); ++i) {
        const Update u = updates[i];
        if (i + 1 < updates.size()) {
            queue("ClientUpdateItemDurability", [u](bootstrap::PlayerBootstrap& pb) { return pb.item_durability(u.storage, u.slot, u.durability); }, conn, /*must_build=*/true);
        } else {
            const int64_t delta = -total;
            queue("repair answer", [u, delta](bootstrap::PlayerBootstrap& pb) { return pb.repair_answer(u.storage, u.slot, u.durability, items::kMoneyTier, delta); }, conn, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: repaired {} item(s) at the shop for {} copper ({} -> {})", name(), mended.size(), total, before, wallet_.copper());
}

PlayerSession::GmMoneyResult PlayerSession::gm_give_money(Connection& conn, int64_t copper, Clock::time_point now, const std::string& by) {
    (void)now;
    GmMoneyResult r;
    r.previous = wallet_.copper();
    r.balance = r.previous;
    if (copper < 1) {
        r.refusal = "the amount must be at least 1 copper";
        return r;
    }
    if (wallet_.room() == 0) {
        r.refusal = name() + "'s purse is full (" + std::to_string(items::kMoneyCap) + " copper, 1,000 gold)";
        return r;
    }
    if (!room_for(3)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    r.added = wallet_.give(copper);
    r.balance = wallet_.copper();
    r.ok = true;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    const int64_t shown = r.added;
    queue("ClientShowCurrencyCallout", [shown](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(items::kMoneyTier, shown); }, conn, /*must_build=*/true);
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[gm] {}: {} copper added by {} ({} -> {}{})", name(), r.added, by, r.previous, r.balance, r.added < copper ? "; the purse is full, the rest is not added" : "");
    return r;
}

// ============================================================================================================================
//  weapon XP: the weapon lines of what is worn, and what a weapon line's new level does (a kill and the test command both end here)
// ============================================================================================================================

std::vector<int64_t> PlayerSession::equipped_weapon_lines() const {
    std::vector<int64_t> found;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!weapon_.valid() || !db) return found;
    // every weapon in the Equipment storage names its weapon line (armor, shields and tools name none); a line counts once, whichever slot or weapon set holds it
    for (const svc::InventoryItem& it : inventory_.items()) {
        if (it.container != svc::ItemContainer::Equipment) continue;
        std::string error;
        const auto w = db->item_weapon(it.item_record_id, error);
        if (!w || w->skill_line == 0 || w->skill_line_name.empty()) continue;
        const weaponxp::Line* line = weapon_.lines()->find_named(w->skill_line_name);
        if (line && std::find(found.begin(), found.end(), line->leveling_type) == found.end()) found.push_back(line->leveling_type);
    }
    std::vector<int64_t> ordered;   // in the lines' order, whatever the order of the slots
    for (const weaponxp::Line& l : weapon_.lines()->all()) {
        if (std::find(found.begin(), found.end(), l.leveling_type) != found.end()) ordered.push_back(l.leveling_type);
    }
    return ordered;
}

const talents::Tree* PlayerSession::weapon_tree_of(int64_t leveling_type) const {
    if (!book_) return nullptr;
    for (const talents::Tree& t : book_->catalog().trees()) {
        if (t.kind == talents::Kind::Weapon && t.leveling_type == leveling_type) return &t;
    }
    return nullptr;
}

bool PlayerSession::apply_weapon_levels(const std::vector<weaponxp::Change>& changes, Clock::time_point now) {
    if (!book_) return false;
    const auto holds = [](const std::vector<talents::RankRef>& v, const talents::RankRef& x) {
        return std::any_of(v.begin(), v.end(), [&](const talents::RankRef& y) { return y.rank->guid == x.rank->guid; });
    };
    bootstrap::AbilityChange effects;
    bool lists_changed = false, level_moved = false;
    for (const weaponxp::Change& c : changes) {
        if (c.line == 0 || c.level_before == c.level_after) continue;
        level_moved = true;
        const talents::Tree* tree = weapon_tree_of(c.line);
        const std::vector<talents::RankRef> had = tree ? book_->auto_ranks_of(*tree) : std::vector<talents::RankRef>();
        book_->set_line_level(c.line, c.level_after);
        if (!tree) continue;   // a line no item names has no weapon tree yet: its level is all there is
        const std::vector<talents::RankRef> has = book_->auto_ranks_of(*tree);
        for (const talents::RankRef& gone : had) {
            if (holds(has, gone)) continue;
            remove_rank_effect(gone.rank->guid, effects);
            if (gone.rank->ability != 0) {
                known_.erase(std::remove(known_.begin(), known_.end(), gone.rank->ability), known_.end());
                records_.erase(std::remove(records_.begin(), records_.end(), gone.rank->ability), records_.end());
                lists_changed = true;
            }
        }
        for (const talents::RankRef& granted : has) {
            if (holds(had, granted)) continue;
            if (granted.rank->ability != 0) {
                if (std::find(known_.begin(), known_.end(), granted.rank->ability) == known_.end()) known_.push_back(granted.rank->ability);
                if (std::find(records_.begin(), records_.end(), granted.rank->ability) == records_.end()) records_.push_back(granted.rank->ability);
                lists_changed = true;
            }
            add_rank_effect(granted, now, effects);   // a weapon rank's passive is on the pawn while the weapon of its line is in hand
        }
        if (!pb_.set_tree_ranks(tree->guid, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
    }
    if (lists_changed) push_abilities();
    if (!effects.empty() && !pb_.apply_ability_change(effects)) {
        spdlog::warn("[session] {}: the pawn does not take the weapon ranks' effect items ({}): the ranks stand without them", name(), pb_.error().empty() ? "keys disagree" : pb_.error());
        for (const bootstrap::EffectItem& e : effects.effects_added) {
            for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
            markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
        }
    }
    if (!effects.empty()) abilities_dirty_ = true;
    if (level_moved) apply_gear_stats("a weapon line's level");   // the ranks held are an input of the stats, and so are the passives of the line in hand
    return level_moved || !effects.empty();
}

PlayerSession::GmWeaponResult PlayerSession::gm_set_weapon_xp(Connection& conn, int64_t leveling_type, int64_t xp, Clock::time_point now, const std::string& by) {
    GmWeaponResult r;
    const weaponxp::Lines* lines = weapon_.lines();
    const weaponxp::Line* line = lines ? lines->find(leveling_type) : nullptr;
    if (!line) {
        r.refusal = weapon_.valid() ? "no weapon line has the leveling type " + std::to_string(leveling_type) : "this world has no weapon lines (its content.db lacks their leveling types)";
        return r;
    }
    r.line = line->name;
    r.xp_before = weapon_.xp(leveling_type);
    r.level_before = weapon_.level(leveling_type);
    if (xp < 0) {
        r.refusal = "an XP total cannot be negative";
        return r;
    }
    r.xp_after = std::min(xp, lines->max_xp());
    r.level_after = lines->level_of(r.xp_after);
    // the line's weapon tree (a line no item names has none yet)
    const talents::Tree* tree = weapon_tree_of(leveling_type);
    if (tree && r.level_after < r.level_before) {
        const std::string why = book_->line_shortfall(*tree, r.level_after);
        if (!why.empty()) {
            r.refusal = name() + "'s " + line->name + " cannot go down to level " + std::to_string(r.level_after) + ": " + why + " (refund them in the Skill Tree first)";
            return r;
        }
    }
    if (!room_for(4)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }

    // ---- the state: the total, and what the new level grants by itself in the line's weapon tree
    const weaponxp::Change change = weapon_.set_xp(leveling_type, r.xp_after);
    progress_changed_ = true;
    const bool pawn_changed = apply_weapon_levels({change}, now);
    if (book_ && tree) {
        const int64_t point = book_->catalog().point_type(talents::Point::Weapon);
        r.points_earned = book_->catalog().earned(tree->leveling_type, r.level_after, point);
        r.points_left = book_->points_left(*tree, point);
    } else if (book_) {
        r.points_earned = book_->catalog().earned(leveling_type, r.level_after, book_->catalog().point_type(talents::Point::Weapon));
        r.points_left = r.points_earned;
    }

    // ---- what the client is told: the line's total (and the weapon tree's list), the level-up effect, the pawn's changed state
    const int64_t total = r.xp_after;
    queue("XPTracker (a weapon line)", [leveling_type, total](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_weapon_line(leveling_type, total)) return std::nullopt;
        return pb.leveling_update();
    }, conn, /*must_build=*/true);
    if (change.leveled()) {
        const int level = r.level_after;
        queue("BroadcastPlayLevelUpEffects", [leveling_type, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(leveling_type, level); }, conn);
    }
    if (pawn_changed) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn, /*must_build=*/false);
    }
    r.ok = true;
    spdlog::info("[gm] {}: {} set by {}: {} XP, level {} -> {} XP, level {}; {} Weapon point(s) earned, {} left", name(), line->name, by, r.xp_before, r.level_before, r.xp_after, r.level_after,
                 r.points_earned, r.points_left);
    hand_over_progress("a game master set a weapon line");
    return r;
}

PlayerSession::GmItemResult PlayerSession::gm_give_item(Connection& conn, int64_t item_record, int32_t count, Clock::time_point now, const std::string& by, const std::string& rarity,
                                                         int32_t quality) {
    (void)now;
    GmItemResult r;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db) {
        r.refusal = "worldd has no content.db to read the item from";
        return r;
    }
    if (count < 1 || count > 100000) {
        r.refusal = "the count must be 1 to 100,000";
        return r;
    }
    std::string error;
    const auto rec = db->item(item_record, error);
    if (!rec) {
        r.refusal = error.empty() ? "the data holds no item record " + std::to_string(item_record) : "content.db failed: " + error;
        return r;
    }
    r.item_name = rec->name;
    // item rarities, step 0: the rarity or the quality asked for; an item that can never show it is refused, and the answer lists what it can be
    int32_t given_quality = 0;   // 0: the record's default quality
    if (!rarity.empty() || quality != 0) {
        r.bad_request = true;
        if (!rec->has_quality) {
            r.refusal = rec->name + " has no quality in the game's data (its rarity is fixed by the record): give it without a rarity or a quality";
            return r;
        }
        std::string why;
        given_quality = items::quality_for_request(*rec, rec->name, rarity, quality, why);
        if (given_quality == 0) {
            r.refusal = why;
            return r;
        }
        r.bad_request = false;
    }
    r.quality = given_quality > 0 ? given_quality : rec->default_quality;
    r.rarity = std::string(items::rarity_name(rec->rarity_of(static_cast<double>(r.quality))));
    if (!room_for(2 + static_cast<size_t>(count / std::max(1, rec->max_stack_size)) / bootstrap::kMaxEventsPerChain)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    std::vector<svc::InventoryItem> fresh;
    stacks_of(item_record, count, *rec, fresh, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000, given_quality);
    for (svc::InventoryItem& it : fresh) {
        it.instance_guid = fresh_guid();
        for (const svc::InventoryItem& other : fresh) {
            if (&other != &it && other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
    }
    // Crates, slice 1: a crate goes where the game puts one that is made or picked up, the Equipment's cargo slot (a second crate, or several, go to the bag as any item does)
    if (const auto crate = item_catalog().info(item_record); crate && crate->cargo_crate() && count == 1 && fresh.size() == 1 && !inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        items::Outcome put = inventory_.grant_cargo(fresh.front(), item_catalog());
        if (!put.ok) {
            r.refusal = "the cargo slot does not take it: " + put.refusal;
            return r;
        }
        r.ok = true;
        r.count = 1;
        r.container = items::container_name(svc::ItemContainer::Equipment);
        apply_outcome(conn, put, "a crate was put in the cargo slot");   // the Add, the worn stats and the visible gear
        spdlog::info("[gm] {}: {} given by {} into the cargo slot (quality {})", name(), r.item_name, by, r.quality);
        return r;
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok) {
        r.refusal = "no room: " + outcome.refusal;
        return r;
    }
    r.ok = true;
    r.count = count;
    r.container = outcome.events.empty() ? "" : items::container_name(outcome.events.front().container);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::info("[gm] {}: {} x {} given by {} as {} (quality {}), {} storage event(s), first into {}", name(), count, r.item_name, by, r.rarity, r.quality, outcome.events.size(), r.container);
    return r;
}

// ============================================================================================================================
//  plan E1, stage 3, part 2: kills and loot
// ============================================================================================================================

namespace {

/// The instance guid of the stack in slot `slot` of the corpse `npc` (the window and the events agree on it; it is not a guid the bag holds).
repl::GuidValue loot_guid(uint64_t npc, size_t slot) {
    uint64_t s = npc * 0x9E3779B97F4A7C15ull + slot * 0xBF58476D1CE4E5B9ull + 0x5851F42D4C957F2Dull;
    const auto next = [&s]() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    repl::GuidValue g;
    const uint64_t a = next(), b = next();
    g.a = static_cast<uint32_t>(a);
    g.b = static_cast<uint32_t>(a >> 32);
    g.c = static_cast<uint32_t>(b);
    g.d = static_cast<uint32_t>(b >> 32) | 1u;   // never the zero guid
    return g;
}

}  // namespace

// ============================================================================================================================
//  plan E1, stage 3, part 2: the player's death
// ============================================================================================================================

namespace {

/// Whether an item record is a material or currency (the retail PvE death takes about half of each stack of those, not the gear): its gameplay tags
/// include Item.Resource... (the Glint certificates, reagents, raw materials).
bool is_material(const content::ContentDb& db, int64_t record) {
    std::string err;
    const auto gear = db.item_gear(record, err);
    if (!gear) return false;
    for (const std::string& t : gear->tags) {
        std::string lower;
        for (const char c : t) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("item.resource", 0) == 0) return true;
    }
    return false;
}

}  // namespace

void PlayerSession::check_death(Connection& conn, Clock::time_point now) {
    if (dead_ || !stats_ || !stats_->started()) return;
    const stats::StatValue* h = stats_->find(content::kGuidHealth);
    if (!h || h->value > 0) return;
    die(conn, now);
}

void PlayerSession::die(Connection& conn, Clock::time_point now) {
    dead_ = true;
    ++deaths_;
    cancel_quick_make(&conn, "the player died");   // Crafting, step 3
    cancel_gather(&conn, "the player died", Interrupt::Death);   // Gathering stage 2
    if (pipeline_) pipeline_->set_dead(true);
    // the mounts milestone, stage 3: the dead player is dismounted (the death frame below restarts its pawn) and its mount is desummoned
    if (mounted()) dismount(conn, now, DismountWhy::PlayerDied);
    if (mounts_ && abilities_) mounts_->desummon(ability_seconds(now));
    const GoodPosition at = checker_.good();
    death_x_ = at.x;
    death_y_ = at.y;
    death_z_ = at.z;
    loot_actor_ = 0;
    vendor_actor_ = 0;   // the windows close with the death: a purchase or a loot take from a dead character is refused (E1 review A4)
    vendor_by_talk_ = false;
    end_dialogue(conn, "the player died");
    // the frame of the retail captures: where it fell, the restart of the pawn, the pawn dead with no Health and no Mana
    set_current_value(content::kGuidMana, 0.0f);
    pb_.set_dead(true);
    const double x = at.x, y = at.y, z = at.z;
    queue("ClientSetLastDeathLocation", [x, y, z](bootstrap::PlayerBootstrap& pb) { return pb.death_location(x, y, z); }, conn, /*must_build=*/true);
    queue("ClientRestart (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.death_restart(); }, conn, /*must_build=*/true);
    // what it costs: the experience debt and the materials of the bag (about half of each stack), which fall into a pile where it died
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    int64_t debt_added = 0;
    if (curve) {
        const leveling::Progress next = leveling::with_death_debt(*curve, progress_, 0.02);
        debt_added = next.debt - progress_.debt;
        progress_ = next;
        progress_changed_ = true;
    }
    std::vector<combat::LootStack> lost;
    items::Outcome all;
    all.ok = true;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const bool loss_on = !npc_world_ || npc_world_->aftermath_config().death_loss;
    if (db && loss_on) {
        for (const svc::InventoryItem& it : inventory_.items()) {
            // Gathering stage 3: the materials are in the resource containers now (the Backpack holds them only for a bag that had no room at the login)
            if ((it.container != svc::ItemContainer::Backpack && !items::is_resource(it.container)) || it.quantity <= 0 || !is_material(*db, it.item_record_id)) continue;
            const int32_t take = it.quantity == 1 ? 1 : (it.quantity + 1) / 2;
            const items::Outcome c = inventory_.consume(it.container, it.slot, it.instance_guid, take);
            if (!c.ok) continue;
            all.events.insert(all.events.end(), c.events.begin(), c.events.end());
            combat::LootStack s;
            s.item = it.item_record_id;
            s.quantity = take;
            s.quality = it.quality;
            lost.push_back(s);
        }
    }
    if (!all.events.empty()) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
        ++items_applied_;
        const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
        for (size_t at2 = 0; at2 < events.size(); at2 += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at2),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at2 + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent (death)", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    queue("pawn state (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("death"); }, conn, /*must_build=*/true);
    if (curve) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        queue("XPTracker (death)", [total, debt](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
        hand_over_progress("a death");
    }
    // the pile
    pile_actor_ = 0;
    if (!lost.empty() && npc_world_ && npc_world_->aftermath()) {
        const std::array<uint32_t, 4>& g = pb_.setup().character_guid;
        std::string err;
        pile_actor_ = npc_world_->aftermath()->drop_pile(x, y, z, repl::GuidValue{g[0], g[1], g[2], g[3]}, pb_.setup().name, lost, npc_world_->world_clock(), err);
        if (pile_actor_ == 0) spdlog::warn("[session] {}: the pile of the death is not made: {} ({} stack(s) of materials are lost)", name(), err, lost.size());
    }
    // the bag goes to the saver with the death, not at the next periodic save: a crash within the minute would give the materials back to this
    // character while the pile's taker keeps them too (E1 review A5)
    if (inventory_dirty_) hand_over_inventory("a death", /*periodic=*/false);
    spdlog::log(level_for(death_logged_), "[session] {}: died at ({:.0f}, {:.0f}, {:.0f}): experience debt +{} (now {}), {} stack(s) of materials lost{}", name(), x, y, z, debt_added, progress_.debt,
                lost.size(), pile_actor_ != 0 ? " into a pile where it fell" : "");
    (void)now;
}
// -- C | O | A & Cyb --

void PlayerSession::apply_kill(Connection& conn, const combat::KillReward& k, Clock::time_point now) {
    ++kills_rewarded_;
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    const std::span<const content::AutoGrant> grants = world_ ? world_->auto_grants() : std::span<const content::AutoGrant>();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && k.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grants, progress_, k.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);
    // weapon XP: the kill's Weapon experience goes to every weapon line of what is worn, each line once; a line has no debt to pay first and stops at its top
    const std::vector<int64_t> worn_lines = k.weapon_xp > 0 ? equipped_weapon_lines() : std::vector<int64_t>();
    std::vector<weaponxp::Change> weapon_changes;
    for (const int64_t type : worn_lines) {
        const weaponxp::Change c = weapon_.add_xp(type, k.weapon_xp);
        if (c.changed()) weapon_changes.push_back(c);
    }
    bool weapons_leveled = false;
    for (const weaponxp::Change& c : weapon_changes) weapons_leveled = weapons_leveled || c.leveled();
    if (!weapon_changes.empty()) progress_changed_ = true;
    const bool weapon_pawn = apply_weapon_levels(weapon_changes, now);   // the new levels' ranks, effect items and stats, before any reply is built
    if (xp.changed() || leveled || !weapon_changes.empty()) {
        // one XPTracker block with everything that moved: Adventuring when its total or debt did (a kill at the Adventuring cap moves only the lines) and each paid line, with the lists of
        // the weapon trees that gained a rank
        const bool adventuring = xp.changed() || leveled;
        const int64_t total = progress_.xp, debt = progress_.debt;
        std::vector<std::pair<int64_t, int64_t>> lines;
        for (const weaponxp::Change& c : weapon_changes) lines.emplace_back(c.line, c.xp_after);
        queue("XPTracker", [adventuring, total, debt, lines](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (adventuring && !pb.set_progress(total, debt)) return std::nullopt;
            for (const auto& [type, line_total] : lines) {
                if (!pb.set_weapon_line(type, line_total)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
    }
    for (const weaponxp::Change& c : weapon_changes) {
        if (!c.leveled()) continue;
        const int64_t type = c.line;
        const int level = c.level_after;
        queue("BroadcastPlayLevelUpEffects", [type, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(type, level); }, conn);
    }
    if (leveled) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    } else if (weapon_pawn) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn, /*must_build=*/false);
    }
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }
    std::string weapon_text;   // the log: what each worn line holds now
    if (k.weapon_xp > 0) {
        weapon_text = ", weapon lines +" + std::to_string(k.weapon_xp) + " each";
        if (!weapon_.valid()) weapon_text += " (this world has no weapon lines)";
        else if (worn_lines.empty()) weapon_text += " (no weapon is worn)";
        for (const int64_t type : worn_lines) {
            const weaponxp::Line* line = weapon_.lines()->find(type);
            const bool up = std::any_of(weapon_changes.begin(), weapon_changes.end(), [&](const weaponxp::Change& c) { return c.line == type && c.leveled(); });
            weapon_text += ", " + (line ? line->name : std::to_string(type)) + " " + std::to_string(weapon_.xp(type)) + " XP (level " + std::to_string(weapon_.level(type)) + (up ? ", level-up" : "") + ")";
        }
    }
    spdlog::log(level_for(use_logged_), "[session] {}: killed {} (level {}): Adventuring +{} XP{}{} (now {} XP, debt {}){}", name(), k.name.empty() ? k.blueprint : k.name, k.level,
                k.adventuring_xp, weapon_text, leveled ? ", level " + std::to_string(xp.after.level) : "", progress_.xp, progress_.debt, k.loot ? "; there is loot" : "");
    if (xp.changed() || !weapon_changes.empty()) hand_over_progress(leveled || weapons_leveled ? "a level-up" : "XP earned");
    quest_kill(conn, k, now);   // quests, slice 2: the Kill objectives
}
// -- Cyb x C | O | A --

void PlayerSession::open_loot(Connection& conn, const combat::Corpse& corpse, const world::WorldActor& actor, Clock::time_point now) {
    const auto ignore = [&](const std::string& why) {
        ++loot_refused_;
        spdlog::log(level_for(loot_logged_), "[session] {}: the loot of {} is not opened: {}", name(), corpse.name, why);
    };
    if (!within_budget(loot_budget_, "a loot interaction", now)) return;
    if (!corpse.parts || corpse.remaining() == 0) return ignore("there is nothing left in it");
    if (!corpse.is_pile && !corpse.rights.count(pb_.setup().guids.pawn.ObjectId)) return ignore("the player did not fight it");
    const GoodPosition at = checker_.good();
    if (std::hypot(actor.x - at.x, actor.y - at.y) > kInteractRangeCm) return ignore("it is " + std::to_string(static_cast<int>(std::hypot(actor.x - at.x, actor.y - at.y) / 100.0)) + " m away");
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !room_for(2)) return;
    repl::StoragePayload data;
    data.maximum_items = static_cast<int32_t>(corpse.loot.size());
    data.client_retrieve_only = true;
    const repl::GuidValue me{pb_.setup().character_guid[0], pb_.setup().character_guid[1], pb_.setup().character_guid[2], pb_.setup().character_guid[3]};
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (size_t i = 0; i < corpse.loot.size(); ++i) {
        repl::StorageSlot slot;
        const combat::LootStack& s = corpse.loot[i];
        if (!s.taken) {
            std::string err;
            const auto rec = db->item(s.item, err);
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = s.item;
            it.instance_guid = items::guid_hex(loot_guid(corpse.npc, i));
            it.quantity = static_cast<int32_t>(std::min<int64_t>(s.quantity, 1 << 20));
            it.quality = s.quality;
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            slot.item = items::to_instance(it);
            slot.slot = static_cast<int32_t>(i);
            slot.check_loot_permission = true;
            slot.loot_permission.push_back(me);
        }
        data.storage.push_back(std::move(slot));
    }
    const FIntrepidNetworkGUID loot_actor_guid = actor.actor.guid;
    loot_actor_ = corpse.npc;
    ++loot_opens_;
    spdlog::log(level_for(loot_logged_), "[session] {}: opens the loot of {} ({} stack(s))", name(), corpse.name, corpse.remaining());
    queue("ClientReceiveLootStoragePayload", [loot_actor_guid, data](bootstrap::PlayerBootstrap& pb) { return pb.loot_payload(loot_actor_guid, data); }, conn, /*must_build=*/true);
}

void PlayerSession::on_loot_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto events = repl::parse_loot_events(rpc.call);
    if (!events) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerRequestLootStorageEvents does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(loot_budget_, "ServerRequestLootStorageEvents", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++loot_refused_;
        spdlog::log(level_for(loot_logged_), "[session] {}: a loot request is refused: {}", name(), why);
    };
    combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!after || !db || !registry_) return refuse("the world has no loot");
    if (loot_actor_ == 0) return refuse("no loot window is open");
    const combat::Corpse* corpse = after->corpse_of(loot_actor_);
    const world::WorldActor* actor = registry_->find(loot_actor_);
    if (!corpse || !corpse->parts || !actor) {
        loot_actor_ = 0;
        return refuse("the corpse is gone");
    }
    const GoodPosition at = checker_.good();
    if (std::hypot(actor->x - at.x, actor->y - at.y) > kInteractRangeCm) {
        loot_actor_ = 0;
        return refuse("the corpse is too far");
    }
    if (!corpse->is_pile && !corpse->rights.count(pb_.setup().guids.pawn.ObjectId)) return refuse("the player may not take from it");
    if (events->empty() || events->size() > kMaxLootTakes) return refuse(events->empty() ? "no events" : "too many events");
    if (!room_for(4)) return;

    // what the client asks for, checked event by event: the container is the corpse's, the slot holds a stack that is still there, the item is that stack.
    // An event that fails is skipped and the others are taken (the game's server judges each event of a request on its own); a request with no event left is refused.
    std::vector<size_t> slots;
    size_t skipped = 0;
    std::string first_why;
    const auto skip = [&](const std::string& why) {
        ++skipped;
        if (first_why.empty()) first_why = why;
    };
    for (const repl::ItemStorageEvent& e : *events) {
        if (e.move_source.object.ObjectId != corpse->parts->container || e.move_source.slots.size() != 1) { skip("an event names another container"); continue; }
        const repl::StorageEventSlot& s = e.move_source.slots[0];
        if (s.slot < 0 || static_cast<size_t>(s.slot) >= corpse->loot.size()) { skip("an event names a slot that is none"); continue; }
        const size_t slot = static_cast<size_t>(s.slot);
        if (corpse->loot[slot].taken) { skip("a stack was taken already"); continue; }
        if (!e.item.has_item || e.item.record_id != static_cast<uint64_t>(corpse->loot[slot].item) || !(e.item.guid == loot_guid(corpse->npc, slot))) {
            skip("an event's item is not the stack of its slot");
            continue;
        }
        if (std::find(slots.begin(), slots.end(), slot) != slots.end()) { skip("a slot twice"); continue; }
        slots.push_back(slot);
    }
    if (slots.empty()) return refuse(first_why.empty() ? std::string("no event can be taken") : first_why);
    if (skipped > 0) spdlog::log(level_for(loot_logged_), "[session] {}: {} event(s) of the loot request are skipped ({}); the other {} are taken", name(), skipped, first_why, slots.size());
    // the bag takes each stack or leaves it (a full bag leaves the rest in the corpse)
    std::vector<bootstrap::LootRemoval> removed;
    items::Outcome all;
    all.ok = true;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    size_t bag_full = 0;
    for (const size_t slot : slots) {
        const combat::LootStack* stack = after->take(corpse->npc, slot);
        if (!stack) continue;
        const auto rec = [&] {
            std::string err;
            return db->item(stack->item, err);
        }();
        const int32_t max_stack = rec ? std::max(1, rec->max_stack_size) : 1;
        std::vector<svc::InventoryItem> fresh;
        int64_t left = stack->quantity;
        while (left > 0) {
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = stack->item;
            it.instance_guid = fresh_guid();
            for (const svc::InventoryItem& other : fresh) {
                if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
            }
            it.quantity = static_cast<int32_t>(std::min<int64_t>(left, max_stack));
            it.quality = stack->quality;
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            left -= it.quantity;
            fresh.push_back(std::move(it));
        }
        // Gathering stage 3: a stack goes where its kind says (a pile mixes both kinds) and stays in the corpse when that storage has no room for it
        const items::Outcome got = inventory_.grant(std::move(fresh), item_catalog());
        if (!got.ok) {
            after->put_back(corpse->npc, slot);
            ++bag_full;
            continue;
        }
        // the container's side: the stack leaves its slot (the same instance guid the window showed)
        svc::InventoryItem shown;
        shown.container = svc::ItemContainer::Backpack;
        shown.item_record_id = stack->item;
        shown.instance_guid = items::guid_hex(loot_guid(corpse->npc, slot));
        shown.quantity = static_cast<int32_t>(std::min<int64_t>(stack->quantity, 1 << 20));
        shown.quality = stack->quality;
        shown.durability = rec ? rec->max_durability_at(shown.quality) : 0.0f;
        shown.created_at_ms = created;
        removed.push_back({static_cast<int32_t>(slot), items::to_instance(shown)});
        all.events.insert(all.events.end(), got.events.begin(), got.events.end());
        ++loot_taken_;
    }
    if (bag_full > 0) spdlog::log(level_for(loot_logged_), "[session] {}: the bag has no room for {} stack(s): they stay in the corpse", name(), bag_full);
    if (removed.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    // retail's order: the removal from the loot container, the add to the bag, and the window closes when nothing is left
    const FIntrepidNetworkGUID container = corpse->parts->container_guid;
    queue("ClientReceiveStorageEvent (loot)", [container, removed](bootstrap::PlayerBootstrap& pb) { return pb.loot_removed(container, removed); }, conn, /*must_build=*/true);
    std::vector<bootstrap::ItemEvent> adds = item_events_for(all);
    add_item_toasts(adds, /*removals=*/false);   // every Add of the bag carries the "Acquired" popup of the item feed, in the stack's own quality and rarity
    for (size_t at2 = 0; at2 < adds.size(); at2 += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(adds.begin() + static_cast<std::ptrdiff_t>(at2),
                                                     adds.begin() + static_cast<std::ptrdiff_t>(std::min(adds.size(), at2 + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(loot_logged_), "[session] {}: takes {} stack(s) from {}: {} storage event(s) into the bag", name(), removed.size(), corpse->name, all.events.size());
    if (corpse->remaining() == 0) {
        const FIntrepidNetworkGUID loot_actor_guid = actor->actor.guid;
        queue("ClientRemoveListenerFromStorage", [loot_actor_guid](bootstrap::PlayerBootstrap& pb) { return pb.loot_listener_removed(loot_actor_guid); }, conn, /*must_build=*/true);
        after->emptied(corpse->npc, npc_world_->world_clock());
        loot_actor_ = 0;
    }
}
// ~ C.O.A - C Y B ~

repl::ActionbarPayload PlayerSession::bar_payload() const {
    repl::ActionbarPayload p;
    p.slots = bar_.wire_slots();   // the weapon bank three times, as retail lists it
    p.auto_granted = false;
    return p;
}

bool PlayerSession::push_abilities() {
    if (pipeline_) pipeline_->set_known(known_);   // plan D2: what the character may trigger follows what the client was told it knows
    std::vector<bootstrap::AbilityRecord> records;
    records.reserve(records_.size());
    for (const int64_t g : records_) records.push_back({g, content::kTypeAbilityRecord});
    if (!pb_.set_abilities(known_, records)) {
        spdlog::debug("[session] {}: the pawn does not take the ability lists yet", name());
        return false;
    }
    return true;
}

void PlayerSession::rebuild_ability_lists() {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!book_ || !db) return;
    const talents::AbilityLists lists =
        talents::join_abilities(*book_, bootstrap::general_unstuck(), combo_, bootstrap::carried_combos(*db, inventory_), bootstrap::static_known_tail(),
                                bootstrap::carried_item_abilities(world_->mounts() ? &world_->mounts()->data() : nullptr, inventory_));
    known_ = lists.known;
    records_ = lists.records;
}

void PlayerSession::sync_item_abilities(Connection& conn) {
    if (!world_ || !world_->mounts() || !phase_in_done_ || !join_.empty() || finished_ || failed_ || abilities_failed_) return;
    const std::vector<int64_t> carried = bootstrap::carried_item_abilities(&world_->mounts()->data(), inventory_);
    std::vector<int64_t> missing;
    for (const int64_t a : carried) {
        if (std::find(records_.begin(), records_.end(), a) == records_.end()) missing.push_back(a);
    }
    if (missing.empty() || !room_for(1)) return;   // (no room now: the next tick tries again)
    for (const int64_t a : missing) records_.push_back(a);
    if (!push_abilities()) {
        for (const int64_t a : missing) records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
        return;
    }
    ++item_ability_syncs_;
    spdlog::log(level_for(item_ability_logged_),
                "[session] {}: {} item ability record(s) added to ReplicatedRecordsToLoad (a mount item is carried: the client needs Summon_Mount loaded to send the summon)", name(),
                missing.size());
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("item abilities"); }, conn);
}

void PlayerSession::add_rank_effect(const talents::RankRef& ref, Clock::time_point now, bootstrap::AbilityChange& effects) {
    const talents::Rank& rank = *ref.rank;
    if (rank.effect == 0 || rank_effects_.count(rank.guid)) return;
    // a weapon rank's passive applies while the weapon of its line is in hand (retail sent no item for a line that was not wielded)
    if (ref.tree->kind == talents::Kind::Weapon && ref.tree->weapon_line != wielded_line_) return;
    bootstrap::EffectItem e;
    e.effect_id = next_passive_effect_id_++;
    e.effect_guid = rank.effect;
    e.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now));
    e.duration = rank.effect_infinite ? std::numeric_limits<float>::max() : static_cast<float>(rank.effect_duration);
    e.tick_time = 0;
    e.stacks = 1;
    effects.effects_added.push_back(e);
    rank_effects_[rank.guid] = e.effect_id;
    // a marker effect (0.1 s of a weapon rank, 1 s of a Universal rank) is ended by the server
    if (!rank.effect_infinite) {
        markers_.push_back({now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(rank.effect_duration)), e.effect_id});
    }
}
// CoA captures, C-Y-B code

void PlayerSession::remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects) {
    const auto it = rank_effects_.find(rank_guid);
    if (it == rank_effects_.end()) return;
    const int64_t key = it->second;
    rank_effects_.erase(it);
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == key; }), markers_.end());
    // an item this same change adds and that is not on the pawn yet: it never was
    const auto added = std::find_if(effects.effects_added.begin(), effects.effects_added.end(), [&](const bootstrap::EffectItem& e) { return e.effect_id == key; });
    if (added != effects.effects_added.end()) {
        effects.effects_added.erase(added);
        return;
    }
    effects.effects_removed.push_back(key);
}

void PlayerSession::grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now, bootstrap::AbilityChange& effects,
                                std::vector<repl::SkillOnActionbar>& updates) {
    bool lists_changed = false;
    for (const talents::RankRef& r : granted) {
        if (r.rank->ability != 0) {
            if (std::find(known_.begin(), known_.end(), r.rank->ability) == known_.end()) {
                known_.push_back(r.rank->ability);   // an append, as retail's: the live order is not the join's
                lists_changed = true;
            }
            if (std::find(records_.begin(), records_.end(), r.rank->ability) == records_.end()) {
                records_.push_back(r.rank->ability);
                lists_changed = true;
            }
        }
        add_rank_effect(r, now, effects);
        if (r.node->auto_hotbar) {
            if (r.rank->ability != 0 && bar_.place_skill(r.rank->ability, r.rank->guid)) progress_changed_ = true;   // a design choice (action_bar.h)
            repl::SkillOnActionbar u;
            u.skill_id = r.node->skill;
            u.skill_tree_id = r.tree->guid;
            u.rank_id = 0;
            u.is_refunding = false;
            u.use_hint = true;
            updates.push_back(u);
        }
    }
    if (lists_changed) push_abilities();
}

void PlayerSession::switch_class(const content::ClassRecord& cls, Clock::time_point now, bootstrap::AbilityChange& effects,
                                 std::vector<repl::SkillOnActionbar>& updates, std::vector<std::pair<int, int>>& cleared) {
    (void)updates;   // the whole bar goes in one payload (the caller), not a skill update per rank
    const talents::Tree* old_tree = book_->class_tree();
    std::set<int64_t> old_ranks;
    if (old_tree) {
        for (const talents::RankRef& r : book_->listed(*old_tree)) {
            old_ranks.insert(r.rank->guid);
            remove_rank_effect(r.rank->guid, effects);
        }
    }
    book_->set_class(cls.guid);
    const talents::Tree* new_tree = book_->class_tree();
    // the trees: the old class's list leaves the client's map, the new one's comes (each class keeps its bought ranks under its own tree)
    if (old_tree && old_tree != new_tree) pb_.set_tree_ranks(old_tree->guid, {});
    std::vector<talents::RankRef> listed;
    if (new_tree) {
        pb_.set_tree_ranks(new_tree->guid, book_->list(*new_tree));
        listed = book_->listed(*new_tree);
    }
    // the bar: the old class's skills leave, the new class's are placed
    for (const auto& slot : bar_.remove_skills(old_ranks)) cleared.push_back(slot);
    for (const talents::RankRef& r : listed) {
        add_rank_effect(r, now, effects);
        if (r.node->auto_hotbar && r.rank->ability != 0) bar_.place_skill(r.rank->ability, r.rank->guid);
    }
    // the abilities: the join's lists for the new class (the old class's go, the new class's come)
    rebuild_ability_lists();
    push_abilities();
    progress_changed_ = true;
}

void PlayerSession::refuse_purchase(Connection& conn, const repl::SkillBuild& build, const std::string& why, Clock::time_point now) {
    ++talents_refused_;
    spdlog::log(level_for(talent_refused_logged_), "[session] {}: ServerPurchaseSkillBuild ({} change(s)) refused, nothing changed: {}", name(), build.changes.size(), why);
    // the client is corrected: the unchanged list of each tree the request names (and the class tree when it names none we know), under the
    // resync budget
    if (!resync_budget_.take(now) || !room_for(1)) return;
    const talents::Catalog& catalog = book_->catalog();
    std::set<int64_t> trees;
    for (const repl::SkillRankChange& c : build.changes) {
        if (trees.size() >= 4) break;
        if (catalog.tree(c.tree.guid)) trees.insert(c.tree.guid);
    }
    if (trees.empty()) {
        if (const talents::Tree* ct = book_->class_tree()) trees.insert(ct->guid);
    }
    bool any = false;
    for (const int64_t t : trees) {
        const talents::Tree* tree = catalog.tree(t);
        if (!tree) continue;
        const std::vector<int64_t> list = book_->list(*tree);
        if (list.empty()) continue;
        if (!pb_.resend_tree(t)) pb_.set_tree_ranks(t, list);   // no item yet: it is added
        any = true;
    }
    if (any) queue("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn);
}

void PlayerSession::on_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!book_) return;
    const auto build = repl::parse_purchase_skill_build(rpc.call);
    if (!build) {
        if (!talents_unreadable_logged_) {
            talents_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerPurchaseSkillBuild does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(talent_budget_, "ServerPurchaseSkillBuild", now)) return;
    // the answer's steps: the pawn's update, the XPTracker and a skill update for each bought rank; a request that cannot be answered whole is refused
    const size_t purchases = build->purchases();
    if (purchases + 3 > kMaxPendingReplies) return refuse_purchase(conn, *build, "more ranks than one reply can announce", now);
    if (!room_for(purchases + 3)) return;
    const talents::Plan plan = book_->plan(*build);
    if (!plan.ok) return refuse_purchase(conn, *build, plan.refusal, now);

    // ---- the state: what the ranks take, what they give
    bootstrap::AbilityChange effects;
    std::vector<repl::SkillOnActionbar> updates;
    bool lists_changed = false;
    bool bar_changed = false;
    for (const talents::Change& c : plan.changes) {
        if (c.purchase) continue;
        remove_rank_effect(c.ref.rank->guid, effects);
        if (c.ref.rank->ability != 0) {
            const int64_t a = c.ref.rank->ability;
            known_.erase(std::remove(known_.begin(), known_.end(), a), known_.end());
            records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
            lists_changed = true;
        }
        if (!bar_.remove_skills({c.ref.rank->guid}).empty()) bar_changed = true;   // the client clears its own bar on a refund
    }
    book_->apply(plan);
    for (const talents::Change& c : plan.changes) {
        if (!c.purchase) continue;
        if (c.ref.rank->ability != 0) {
            const int64_t a = c.ref.rank->ability;
            if (std::find(known_.begin(), known_.end(), a) == known_.end()) known_.push_back(a);   // an append: the live order is not the join's
            if (std::find(records_.begin(), records_.end(), a) == records_.end()) records_.push_back(a);
            lists_changed = true;
        }
        add_rank_effect(c.ref, now, effects);
        if (c.ref.node->auto_hotbar) {
            if (c.ref.rank->ability != 0 && bar_.place_skill(c.ref.rank->ability, c.ref.rank->guid)) bar_changed = true;   // a design choice (action_bar.h)
            repl::SkillOnActionbar u;
            u.skill_id = c.ref.node->skill;
            u.skill_tree_id = c.ref.tree->guid;
            u.rank_id = 0;
            u.is_refunding = false;
            u.use_hint = true;
            updates.push_back(u);
        }
    }
    (void)bar_changed;
    for (const int64_t t : plan.trees) {
        const talents::Tree* tree = book_->catalog().tree(t);
        if (tree && !pb_.set_tree_ranks(t, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
    }
    if (lists_changed) push_abilities();
    if (!effects.empty()) {
        if (!pb_.apply_ability_change(effects)) {
            spdlog::warn("[session] {}: the pawn does not take the rank effects' items ({}): the ranks stand without them", name(),
                         pb_.error().empty() ? "keys disagree" : pb_.error());
            for (const bootstrap::EffectItem& e : effects.effects_added) {
                for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
                markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
            }
        }
    }
    apply_gear_stats("the talents");   // the ranks' constant mods are an input of the stats
// C:Y:B + CoA

    // ---- what the client is told, in the retail order: the pawn's update (abilities, effects, stats), the XPTracker's lists, the skill updates
    ++talents_applied_;
    // The reply goes out purchase_delay_ later (retail's 84 ms median): the state above is applied now, the steps are built when they go.
    queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("talents"); }, conn, now);
    queue_later("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn, now, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, now, /*must_build=*/true);
    }
    // the client reports each skill it puts on its bar with a ServerSetActionbarSlotUseData, all in one frame: they are not the bar budget's
    bar_allow_ = static_cast<uint32_t>(std::min<size_t>(kBarPurchaseAllowance, static_cast<size_t>(bar_allow_) + updates.size()));
    size_t bought = 0, refunded = 0;
    for (const talents::Change& c : plan.changes) (c.purchase ? bought : refunded) += 1;
    spdlog::log(level_for(talent_logged_), "[session] {}: ServerPurchaseSkillBuild: {} rank(s) bought, {} refunded{}; {} effect item(s) added, {} removed; KnownAbilities {}",
                name(), bought, refunded, build->refund_only() ? " (a respec)" : "", effects.effects_added.size(), effects.effects_removed.size(), known_.size());
    progress_changed_ = true;
    hand_over_progress("a talent change");
}

void PlayerSession::on_set_slot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!book_) return;
    const auto slot = repl::parse_set_actionbar_slot(rpc.call);
    if (!slot) {
        if (!talents_unreadable_logged_) {
            talents_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerSetActionbarSlotUseData does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (bar_allow_ > 0) --bar_allow_;   // a Set a purchase made the client send
    else if (!within_budget(bar_budget_, "ServerSetActionbarSlotUseData", now)) return;
    const std::optional<std::pair<int, int>> server_slot = bar_.first_free();   // where the server puts a bought skill, for the log
    std::string why;
    if (!bar_.set(*slot, &why)) {
        ++bar_refused_;
        spdlog::log(level_for(bar_refused_logged_), "[session] {}: ServerSetActionbarSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(), slot->bank_index,
                    slot->slot_index, why);
        return;   // retail answers none, and nothing here is wrong with the client's own bar
    }
    // a skill is on the class pages once: the client moved it, not copied it
    if (slot->use.use_type == repl::ActionbarUseType::Skill && slot->use.skill_rank_id != 0 && slot->bank_index < actionbar::kClassBanks) {
        bar_.remove_other_copies(slot->use.skill_rank_id, slot->bank_index, slot->slot_index);
    }
    ++bar_sets_;
    // a probe of the D1 draft (the bar animation): every Set at info, with what the server's own placement would have been
    ++bar_logged_;
    spdlog::info("[session] {}: the client set action bar bank {} slot {} (use type {}, ability {}, rank {}): stored; the server's first free class slot was {}", name(),
                 slot->bank_index, slot->slot_index, static_cast<unsigned>(slot->use.use_type), slot->use.ability_id, slot->use.skill_rank_id,
                 server_slot ? std::to_string(server_slot->first) + "/" + std::to_string(server_slot->second) : std::string("none"));
    progress_changed_ = true;
    hand_over_progress("the action bar");
}

void PlayerSession::on_set_ranged(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!book_ || !world_ || !world_->content()) return;
    const auto use = repl::parse_set_use_ranged_weapon(rpc.call);
    if (!use) return;
    if (!within_budget(item_budget_, "ServerSetUseRangedWeapon", now) || !room_for(2)) return;
    if (*use == use_ranged_) return;
    use_ranged_ = *use;
    ++weapon_swaps_;
    // E1 stage 3 fix: the pawn's CombatInfo.bIsUsingRangedWeapon follows the request (retail 403, every swap: an 18-bit UPlayerCombatInfo block in the
    // same chain as the AbilityComponent's, the StatsComponent's and OwnerInfo's); the client's weapon in hand follows this flag, not KnownAbilities
    if (!pb_.set_using_ranged_weapon(use_ranged_)) spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
    const auto weapon = bootstrap::worn_weapon(*world_->content(), inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    if (!weapon) {
        spdlog::info("[session] {}: the weapon set is the {} one now and nothing is worn in its slot: the combo stays", name(), use_ranged_ ? "ranged" : "melee");
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
        return;
    }
    // KnownAbilities: the old combo leaves, the new one is the last element; the records: the new one is the last (the old one stays carried)
    put_weapon_in_hand(weapon->combo, weapon->line, weapon->line_name);
    apply_gear_stats("the weapon swap");   // a weapon tree's constant mods apply while its line is in hand
    spdlog::info("[session] {}: the {} weapon set: combo {} is the last known ability, weapon line {}", name(), use_ranged_ ? "ranged" : "melee", weapon->combo,
                 weapon->line_name);
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
}

void PlayerSession::put_weapon_in_hand(int64_t combo, int64_t line, const std::string& line_name) {
    if (combo_ != 0) known_.erase(std::remove(known_.begin(), known_.end(), combo_), known_.end());
    known_.erase(std::remove(known_.begin(), known_.end(), combo), known_.end());
    known_.push_back(combo);
    records_.erase(std::remove(records_.begin(), records_.end(), combo), records_.end());
    records_.push_back(combo);
    combo_ = combo;
    wielded_line_ = line;
    push_abilities();   // (the pipeline answers to the new combo from here on, and not to the old one)
    pb_.set_weapon_skill_line(line, line_name);
}

void PlayerSession::follow_worn_weapon() {
    if (!book_ || !world_ || !world_->content()) return;
    const content::ContentDb& db = *world_->content();
    // The weapon swap fix (a wand worn over a longbow used to leave the longbow's combo the last known ability: the longbow kept shooting and the wand's combo was refused).  What a
    // ServerItemUsed wears or takes off in a weapon slot moves the weapon in hand the way the retail server's reply did, in the chain of the HeldItems
    // update [R: the recorded equips and unequips of a weapon; the replacement of the weapon in hand by another is the same reply, M]:
    //  - the set in hand holds a weapon: its combo is the last known ability, its line is the weapon line, and the combo that was in hand stops being answered; a weapon put into the set
    //    that is not in hand changes none of this (the swap to that set reads the slot) [R: a weapon worn into the other set changed no ability list, line or flag];
    //  - the set in hand holds no weapon and the other set does (the weapon in hand was taken off, or the first weapon was worn into the other set): the hand moves to that set and the
    //    flag follows [R: the weapon taken off; M: the first weapon worn, where one recording sends the flag that two like it do not];
    //  - nothing is left to hold: the combo leaves KnownAbilities and the line stays as the client has it [R; the records keep the carried combo, M].
    std::optional<bootstrap::WeaponInfo> weapon = bootstrap::worn_weapon(db, inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    bool moved_hand = false;
    if (!weapon) {
        if (const auto other = bootstrap::worn_weapon(db, inventory_, use_ranged_ ? items::equip::Primary : items::equip::Ranged)) {
            use_ranged_ = !use_ranged_;
            if (!pb_.set_using_ranged_weapon(use_ranged_)) spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
            weapon = other;
            moved_hand = true;
        }
    }
    if (weapon) {
        if (!moved_hand && weapon->combo == combo_) {   // the same combo stays in hand (another item of the same weapon, or a weapon in the other set)
            wielded_line_ = weapon->line;
            return;
        }
        put_weapon_in_hand(weapon->combo, weapon->line, weapon->line_name);
        spdlog::info("[session] {}: {}: the {} weapon set is in hand, combo {} is the last known ability, weapon line {}", name(),
                     moved_hand ? "the weapon in hand was taken off and the other set holds a weapon" : "the weapon worn in the set in hand changed", use_ranged_ ? "ranged" : "melee",
                     weapon->combo, weapon->line_name);
        return;
    }
    wielded_line_ = 0;
    if (combo_ == 0) return;
    const int64_t gone = combo_;
    known_.erase(std::remove(known_.begin(), known_.end(), gone), known_.end());
    combo_ = 0;
    push_abilities();
    spdlog::info("[session] {}: the weapon in hand was taken off and no other is worn: combo {} leaves KnownAbilities", name(), gone);
}







}}  // namespace aoc::worldd
