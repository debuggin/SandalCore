// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/actor_view.cpp
//
//  See actor_view.h.
// ============================================================================
#include "session/actor_view.h"
#include "net/tick_loop.h"
#include "net/tick_profile.h"

#include "gather/node_wire.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;
using world::ClientState;
using world::Ring;

namespace {
/// The update chains whose events the view waits for, over all actors, before it sweeps the routes of ones it no longer holds.
constexpr size_t kMaxRoutes = 4096;

/// A creature's attack names its montage by an id the client knows only from the montage's export: the export goes with the AbilityComponent's block, and with the actor's own
/// (the creature's RepRootMotion names the montage too, and an update may carry that block alone; a chain the connection acknowledged is left out, one named twice is written once).
void add_ability_exports(std::vector<repl::ContentBlock>& blocks, const std::vector<repl::ExportChain>& exports) {
    for (repl::ContentBlock& b : blocks) {
        if (b.sub && (!b.sub->obj.schema || std::string(b.sub->obj.schema->name) != "UAoCAbilityComponent")) continue;
        for (const repl::ExportChain& x : exports) b.exports.push_back(x);
    }
}
}  // namespace

ActorView::ActorView(world::ActorRegistry& registry, Config config, std::function<bool(uint32_t)> reserved)
    : registry_(registry), config_(std::move(config)), reserved_(std::move(reserved)) {
    listener_ = registry_.on_remove([this](const world::WorldActor& a) { on_removed(a); });
}

ActorView::~ActorView() { finish(); }

void ActorView::finish() {
    if (finished_) return;
    finished_ = true;
    registry_.off_remove(listener_);
    if (phase_taken_ && config_.phases) config_.phases->leave(phase_);   // the tick it passed on is the other views' to take
    phase_taken_ = false;
}


uint32_t ActorView::channel_of(uint64_t actor) const {
    const auto it = entries_.find(actor);
    if (it == entries_.end()) return 0;
    return it->second.state == ClientState::Awake || it->second.state == ClientState::Snapshot ? it->second.channel : 0;
}

ClientState ActorView::state_of(uint64_t actor) const {
    const auto it = entries_.find(actor);
    return it == entries_.end() ? ClientState::None : it->second.state;
}

size_t ActorView::count(ClientState state) const {
    size_t n = 0;
    for (const auto& [id, e] : entries_) n += e.state == state ? 1 : 0;
    return n;
}

std::string ActorView::describe() const {
    return std::to_string(count(ClientState::Awake)) + " awake, " + std::to_string(count(ClientState::Dormant)) + " dormant, " +
           std::to_string(count(ClientState::Snapshot)) + " snapshots closing; channels " + std::to_string(channel_owner_.size()) + " (peak " +
           std::to_string(stats_.peak_channels) + "); opens " + std::to_string(stats_.opens) + ", snapshots " + std::to_string(stats_.snapshots) + ", wakes " +
           std::to_string(stats_.wakes) + ", updates " + std::to_string(stats_.updates) + ", closes (dormancy " + std::to_string(stats_.closes_dormancy) +
           ", relevancy " + std::to_string(stats_.closes_relevancy) + ", destroyed " + std::to_string(stats_.closes_destroyed) + "); throttled passes " + std::to_string(stats_.throttled);
}

world::RelevancyTier ActorView::classify_tier(double distance_cm) const noexcept {
    if (!(distance_cm >= 0.0)) return world::RelevancyTier::Culled;
    const float dist_sq = static_cast<float>(distance_cm * distance_cm);
    return world::classify_relevancy_tier(dist_sq, config_.relevancy);
}

bool ActorView::should_update_for_tick(double distance_cm, uint64_t tick_index) const noexcept {
    const world::SpatialCellCoord cell = world::compute_spatial_cell(static_cast<float>(distance_cm), 0.0f);
    if (!world::is_cell_in_aoi_ring({0, 0}, cell, 64)) return false;
    return world::should_replicate_subring(classify_tier(distance_cm), tick_index, static_cast<uint32_t>(config_.player_id));
}

uint32_t ActorView::allocate_channel(const Connection& conn) const {
    for (uint32_t index = 3; index <= config_.max_channel; ++index) {
        if (conn.channel_open(index) || conn.channel_closing(index)) continue;
        if (reserved_ && reserved_(index)) continue;
        if (channel_owner_.count(index) != 0) continue;
        return index;
    }
    return 0;
}

void ActorView::release(Entry& e) {
    if (e.channel != 0) channel_owner_.erase(e.channel);
    e.channel = 0;
}

bool ActorView::send(Connection& conn, uint32_t channel, bool open, bool reliable, repl::Emission& em, ChainId* chain_id, size_t* chain_parts) {
    Chain chain;
    chain.channel = channel;
    chain.open = open;
    chain.reliable = reliable;
    chain.exports = std::move(em.exports);
    chain.payload = std::move(em.payload);
    if (!conn.can_send_chain(chain)) {
        em.exports = std::move(chain.exports);
        em.payload = std::move(chain.payload);
        return false;
    }
    size_t bits = chain.payload.bit_pos();
    for (const auto& x : chain.exports) bits += x.bit_pos();
    const size_t export_parts = chain.exports.size();
    const size_t parts = Connection::part_count(chain);
    const ChainId id = conn.send_chain(chain);
    if (id == 0) {
        em.exports = std::move(chain.exports);
        em.payload = std::move(chain.payload);
        return false;
    }
    stats_.bits_sent += bits;
    if (!em.exported_by_bunch.empty()) {
        ChainExports ce;
        ce.by_bunch = std::move(em.exported_by_bunch);
        ce.reliable = reliable;
        ce.outstanding = std::min(export_parts, ce.by_bunch.size());
        exports_[id] = std::move(ce);
    }
    if (trace_) trace_(channel, open, reliable, bits);
    if (chain_id) *chain_id = id;
    if (chain_parts) *chain_parts = parts;
    return true;
}

void ActorView::forget_chains(Entry& e) {
    for (const ChainId id : e.chains) chain_owner_.erase(id);
    e.chains.clear();
    e.ledger.clear();
}

const repl::Actor& ActorView::view_of(const world::WorldActor& a, repl::Actor& scratch) const {
    if (!config_.player_guid || !world::is_interactable(a.actor)) return a.actor;
    scratch = a.actor;
    world::set_player(scratch, *config_.player_guid, true);
    // Gathering stage 2: the harvester's own client has its pawn in the node's InteractingPlayer while the harvest shows it (no other connection does: gather/node_wire.h); Gathering stage 3: and
    // in the corpse's of a huntable creature while a hunt of it runs
    if (a.kind == world::ActorKind::Gatherable || (a.kind == world::ActorKind::Npc && a.gather.player_id != 0)) gather::show_interacting_player(a, config_.player_id, scratch);
    return scratch;
}

std::optional<repl::Emission> ActorView::build_open(const world::WorldActor& a, repl::ActorShadow& work, bool first, bool* private_form) {
    bool failed = false;
    repl::Actor scratch;
    const repl::Actor* shown = &view_of(a, scratch);
    if (first && a.first_open_form) {   // the connection's first look at the actor: its first-open form (a copy; the registry's actor is never touched)
        if (shown != &scratch) scratch = *shown;
        a.first_open_form(scratch);
        shown = &scratch;
    }
    if (private_form) *private_form = shown != &a.actor;
    const repl::Actor& actor = *shown;
    if (a.open_exports.empty() && a.ability_exports.empty()) return repl::open_actor(actor, work, acks_, &failed);
    // the blocks by hand: the actor's block carries the exports its values name by path, the AbilityComponent's those of the attack it shows
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    work.forget_sent();
    if (!repl::collect_open_blocks(actor, work, blocks, &undo) || blocks.empty() || (!a.open_exports.empty() && blocks[0].sub)) {
        undo.rollback(work);
        return std::nullopt;
    }
    for (const repl::ExportChain& x : a.open_exports) blocks[0].exports.push_back(x);
    add_ability_exports(blocks, a.ability_exports);
    auto em = repl::assemble_open(actor, blocks, acks_);
    if (!em) undo.rollback(work);
    return em;
}

bool ActorView::open(Connection& conn, const world::WorldActor& a, Entry& e, bool snapshot, Clock::time_point now) {
    TickProfileSpan span(&tick_profile().open_us);   // (--loop-stats)
    if (tick_profile().on) ++tick_profile().opens;
    const uint32_t channel = allocate_channel(conn);
    if (channel == 0) {
        ++stats_.no_channel;
        ++stats_.refused_opens;
        return false;
    }
    repl::ActorShadow work;   // a new channel: the client has nothing of the actor (the shadow's masks stay: there are none)
    bool first = true;
    if (a.keep_fast_shadow) {   // ... but for the fast arrays of an actor it keeps them of: it has the list it was last sent, and the open sends the changes
        if (const auto kept = kept_.find(a.id); kept != kept_.end()) {
            work = kept->second;
            work.keep_fast = true;
            first = false;
        }
    }
    bool private_form = false;
    auto em = build_open(a, work, first, &private_form);
    if (!em) {
        e.broken = true;
        ++stats_.broken;
        spdlog::warn("[actors] the open of actor {} ({}) does not build: it is never sent", a.id, a.npc.name.empty() ? a.npc.archetype : a.npc.name);
        return false;
    }
    ChainId open_chain = 0;
    size_t open_parts = 1;
    const size_t open_bits = em->payload.bit_pos();
    if (!send(conn, channel, /*open=*/true, /*reliable=*/true, *em, &open_chain, &open_parts)) {
        ++stats_.refused_opens;
        return false;
    }
    if (a.kind == world::ActorKind::NodeLayout) {
        spdlog::info("[scenery] open for player {} on channel {}: {} (node {}), level {}, {} items, PlotChangeVersion {}, {} payload bits, {} bits in the open", config_.player_id, channel,
                     a.scenery.name, a.scenery.node, a.scenery.level, a.scenery.items, a.scenery.plot_version, a.scenery.payload_bits, open_bits);
    }
    if (a.kind == world::ActorKind::Player) log_player(a, true, nullptr);
    if (a.kind == world::ActorKind::Foliage) {
        const bool changed = a.version > 1, say = changed || foliage_opens_logged_ < 100;
        if (!changed) ++foliage_opens_logged_;
        spdlog::log(say ? spdlog::level::info : spdlog::level::debug, "[foliage] state actor open for player {} on channel {}: {} ({}, {} bits)", config_.player_id, channel, a.label,
                    first ? "first look at the cell" : "the changes since the last look", open_bits);
    }
    forget_chains(e);   // a new channel: the client has nothing, the open sends it all
    kept_.erase(a.id);  // (the shadow is the entry's again until the channel closes)
    e.open_chain = open_chain;
    e.open_parts_left = open_parts;
    e.open_acked = false;
    e.chains.push_back(open_chain);
    chain_owner_[open_chain] = a.id;
    e.shadow = std::move(work);
    e.channel = channel;
    e.sent_version = a.version;
    e.sent_reliable = a.reliable_version;
    e.shape_epoch = a.shape_epoch;   // (shared updates: this shadow is started now, from the values the actor has now)
    e.private_form = private_form;
    e.tainted = false;
    e.state = snapshot ? ClientState::Snapshot : ClientState::Awake;
    e.close_due = now + (a.dormancy_delay.count() > 0 ? a.dormancy_delay : config_.relevancy.dormancy_delay);
    channel_owner_[channel] = a.id;
    stats_.peak_channels = std::max(stats_.peak_channels, channel_owner_.size());
    if (snapshot) pending_snapshots_.push_back(a.id);
    ++(snapshot ? stats_.snapshots : stats_.opens);
    return true;
}

bool ActorView::send_update(Connection& conn, const world::WorldActor& a, Entry& e) {
    if (a.version == e.sent_version && a.reliable_version == e.sent_reliable) return true;
    TickProfile& profile = tick_profile();   // (--loop-stats: the building of the change, the rest)
    TickProfileSpan span(&profile.update_us);
    std::chrono::steady_clock::time_point t1{};
    if (profile.on) {
        ++profile.updates;
        t1 = std::chrono::steady_clock::now();
    }
    Built built;
    build_update(a, e, built);
    if (profile.on) profile.update_build_us += tick_profile_us_since(t1);
    if (built.failed) {
        e.broken = true;
        ++stats_.broken;
        spdlog::warn("[actors] an update of actor {} does not build: it is not updated again", a.id);
        return false;
    }
    const bool reliable = a.reliable_version != e.sent_reliable || !e.open_acked || e.ledger.reliable_pending();
    if (!built.em) {   // the version moved but no handle changed (a value set to what it was)
        e.sent_version = a.version;
        e.sent_reliable = a.reliable_version;
        e.private_form = built.private_form;
        e.tainted = false;
        return true;
    }
    ChainId chain = 0;
    size_t parts = 1;
    if (!send(conn, e.channel, /*open=*/false, reliable, *built.em, &chain, &parts)) {
        ++stats_.refused_updates;
        built.undo.rollback(e.shadow);   // the connection did not take the chain: the client has what it had
        return false;
    }
    if (built.redo) {   // a shared update: the shadow is given what the update wrote
        built.redo->apply(e.shadow);
        ++stats_.shared_updates;
    }
    e.sent_version = a.version;
    e.sent_reliable = a.reliable_version;
    e.private_form = built.private_form;
    e.tainted = false;
    const size_t naks_before = e.ledger.naks();
    e.ledger.sent(chain, reliable, parts, built.shared_record ? *built.shared_record : built.record, e.shadow);
    if (e.ledger.naks() > naks_before) e.tainted = true;   // it dropped its oldest chain as a NAK: values went back into the shadow, so it is no copy of the others until the next update
    if (chain_trace_) chain_trace_(chain, a.id, reliable, parts, e.ledger.entries());
    if (e.ledger.tracks(chain)) {
        e.chains.push_back(chain);
        chain_owner_[chain] = a.id;
        if (chain_owner_.size() > kMaxRoutes) {   // routes of actors the view dropped while their events were in flight, or whose events never came
            for (auto it = chain_owner_.begin(); it != chain_owner_.end();) {
                const auto en = entries_.find(it->second);
                it = en == entries_.end() || !en->second.ledger.tracks(it->first) ? chain_owner_.erase(it) : std::next(it);
            }
        }
    }
    ++(reliable ? stats_.updates : stats_.updates_unreliable);
    if (a.kind == world::ActorKind::NodeLayout) {
        spdlog::info("[scenery] update for player {} on channel {}: {} (node {}), now level {}, {} items, PlotChangeVersion {}", config_.player_id, e.channel, a.scenery.name,
                     a.scenery.node, a.scenery.level, a.scenery.items, a.scenery.plot_version);
    }
    return true;
}

bool ActorView::close(Connection& conn, uint64_t id, Entry& e, uint32_t reason, ClientState next) {
    if (!conn.close_channel(e.channel, reason)) {
        ++stats_.refused_closes;
        return false;
    }
    if (reason != kCloseDormancy) {
        const world::WorldActor* a = reg().find(id);
        if (a && a->kind == world::ActorKind::NodeLayout) {
            spdlog::info("[scenery] close for player {} on channel {}: {} (node {}), {}", config_.player_id, e.channel, a->scenery.name, a->scenery.node,
                         reason == kCloseRelevancy ? "out of range" : "destroyed");
        }
        if (a && a->kind == world::ActorKind::Player) log_player(*a, false, reason == kCloseRelevancy ? "out of range" : "destroyed");
    }
    release(e);
    if (const world::WorldActor* held = reg().find(id); held && held->keep_fast_shadow) {
        kept_[id] = std::move(e.shadow);
        kept_[id].keep_fast = true;
    }
    e.shadow = repl::ActorShadow{};
    // A Dormancy close leaves the client's actor as the last updates left it: an update still in flight that is lost makes that stale, so
    // the ledger and the routing of its events stay until they come (on_ack).  Any other close ends the entry.
    if (reason != kCloseDormancy) forget_chains(e);
    e.state = next;
    if (reason == kCloseDormancy) ++stats_.closes_dormancy;
    else if (reason == kCloseRelevancy) ++stats_.closes_relevancy;
    else ++stats_.closes_destroyed;
    return true;
}

void ActorView::on_removed(const world::WorldActor& a) {
    kept_.erase(a.id);
    const auto it = entries_.find(a.id);
    if (it == entries_.end() || it->second.state == ClientState::None) return;
    Removed r;
    r.id = a.id;
    r.state = it->second.state;
    if (it->second.state == ClientState::Dormant) r.actor = a.actor;   // opened once more, to be destroyed
    removed_.push_back(std::move(r));
    if (a.kind == world::ActorKind::Player && !finished_) log_player(a, false, "left the world");
}

void ActorView::flush_removed(Connection& conn) {
    for (size_t i = 0; i < removed_.size();) {
        Removed& r = removed_[i];
        const auto it = entries_.find(r.id);
        if (it == entries_.end()) {
            removed_.erase(removed_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        Entry& e = it->second;
        bool done = false;
        if (e.state == ClientState::Awake || e.state == ClientState::Snapshot) {
            done = close(conn, r.id, e, kCloseDestroyed, ClientState::None);
        } else if (e.state == ClientState::Dormant) {
            // the client holds it dormant: a channel opened and closed with Destroyed makes it forget the actor
            const uint32_t channel = allocate_channel(conn);
            if (channel != 0) {
                repl::ActorShadow work;
                bool failed = false;
                auto em = repl::open_actor(r.actor, work, acks_, &failed);
                if (!em) {
                    done = true;   // cannot be written: the client keeps a ghost
                } else if (send(conn, channel, true, true, *em)) {
                    channel_owner_[channel] = r.id;
                    e.channel = channel;
                    e.state = ClientState::Awake;
                    done = close(conn, r.id, e, kCloseDestroyed, ClientState::None);
                }
            }
        } else {
            done = true;
        }
        if (done) {
            entries_.erase(r.id);
            removed_.erase(removed_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ActorView::on_ack(const AckEvent& e) {
    const auto owner = chain_owner_.find(e.chain);
    if (owner != chain_owner_.end()) {
        const auto en = entries_.find(owner->second);
        if (en == entries_.end()) {
            chain_owner_.erase(owner);
        } else {
            Entry& entry = en->second;
            if (e.chain == entry.open_chain && e.delivered && entry.open_parts_left > 0 && --entry.open_parts_left == 0) entry.open_acked = true;
            const size_t naks = entry.ledger.naks(), before = entry.ledger.resent();
            entry.ledger.on_event(e.chain, e.delivered, entry.shadow);
            if (entry.ledger.naks() > naks) {
                ++stats_.naks;
                stats_.nak_entries += entry.ledger.resent() - before;
                // The shadow has been given back what was lost, but the view only builds an update for an actor whose version moved: it
                // moves it by forgetting what it sent (versions start at 1).  An awake actor is updated at the next pass; a dormant one's
                // snapshot is stale, and the next pass in the snapshot or the awake ring opens it again from the current state.
                // (`sent_reliable` stays: the resend of an awake actor's lost values is unreliable, as the chain was; a stale dormant snapshot
                // is opened again on `sent_version` alone)
                if (entry.sent_version != 0) {
                    entry.sent_version = 0;
                    if (entry.state == ClientState::Dormant) ++stats_.stale_snapshots;
                }
            }
            if (!entry.ledger.tracks(e.chain) && (e.chain != entry.open_chain || entry.open_acked)) {
                chain_owner_.erase(owner);
                entry.chains.erase(std::remove(entry.chains.begin(), entry.chains.end(), e.chain), entry.chains.end());
            }
        }
    }
    const auto it = exports_.find(e.chain);
    if (it == exports_.end() || !e.export_part) return;
    ChainExports& c = it->second;
    if (e.delivered) {
        if (e.part < c.by_bunch.size()) acks_.mark(c.by_bunch[e.part]);
    } else if (c.reliable) {
        return;   // the connection sends the part again and its event follows
    }
    if (c.outstanding > 0 && --c.outstanding == 0) exports_.erase(it);   // a lost unreliable part is final, too
}


bool ActorView::open_scenery(Connection& conn, double x, double y, Clock::time_point now, size_t* opened) {
    if (opened) *opened = 0;
    if (finished_) return true;
    const world::RelevancyConfig& cfg = config_.relevancy;
    struct Cand {
        double   d;
        uint64_t id;
    };
    std::vector<Cand> cands;
    scratch_.clear();
    registry_.query_wide(x, y, cfg.hysteresis_cm, scratch_);   // the towns' replicators are wide actors: the grid query never finds them
    for (const uint64_t id : scratch_) {
        const world::WorldActor* a = reg().find(id);
        if (!a || a->kind != world::ActorKind::NodeLayout) continue;
        if (!for_this_player(*a)) continue;
        if (entries_.count(id) != 0) continue;   // the view has it already (open, or given up as broken): the passes look after it
        const double d = world::horizontal_distance(a->x, a->y, x, y);
        if (world::classify(cfg, d, a->awake_radius_cm, a->never_dormant, ClientState::None) != Ring::Awake) continue;   // (a replicator is awake or nothing)
        cands.push_back({d, id});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& l, const Cand& r) { return l.d != r.d ? l.d < r.d : l.id < r.id; });
    cands.erase(std::unique(cands.begin(), cands.end(), [](const Cand& l, const Cand& r) { return l.id == r.id; }), cands.end());   // (query_wide may name an actor twice)
    if (cands.empty()) return true;
    // the pass's pacing: the player's own data goes first on a busy connection, and the opens of one call are capped
    if (conn.packets_in_flight() > config_.max_packets_in_flight || conn.queued_bunches() > config_.max_queued_bunches) {
        ++stats_.throttled;
        return false;
    }
    size_t n = 0;
    bool all = true;
    for (const Cand& c : cands) {
        if (n >= cfg.max_opens_per_tick) {
            ++stats_.capped;
            all = false;
            break;
        }
        if (conn.queued_bunches() > config_.max_queued_bunches) {   // this call's own sends filled the queue
            ++stats_.throttled;
            all = false;
            break;
        }
        const world::WorldActor* a = reg().find(c.id);
        if (!a) continue;
        Entry& e = entries_.emplace(c.id, Entry{}).first->second;
        if (open(conn, *a, e, /*snapshot=*/false, now)) {
            ++n;
        } else if (!e.broken) {   // refused by the connection (its window): no entry, the next call tries again
            entries_.erase(c.id);
            all = false;
        }   // (an actor that does not build stays as a broken entry, as in the passes: never retried, so nothing waits for it)
    }
    if (opened) *opened = n;
    return all;
}

bool ActorView::update_now(Connection& conn, uint64_t actor) {
    if (finished_) return false;
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.state != ClientState::Awake || it->second.broken) return false;
    const world::WorldActor* a = reg().find(actor);
    return a && send_update(conn, *a, it->second);
}

void ActorView::tick(Connection& conn, double x, double y, Clock::time_point now) {
    if (finished_) return;
    TickProfile& profile = tick_profile();   // (--loop-stats: where the tick's time goes)
    std::chrono::steady_clock::time_point started{};
    if (profile.on) started = std::chrono::steady_clock::now();
    tick_unprofiled(conn, x, y, now);
    if (profile.on) profile.view_us += tick_profile_us_since(started);
}

void ActorView::tick_unprofiled(Connection& conn, double x, double y, Clock::time_point now) {
    run_due_closes(conn, now);
    if (pass_due(now)) pass(conn, x, y, now);
}

void ActorView::run_due_closes(Connection& conn, Clock::time_point now) {
    flush_removed(conn);
    // the Dormancy closes whose time has come: the snapshots in the order they were opened (an actor with a delay of its own may be due after one opened later)
    for (size_t i = 0; i < pending_snapshots_.size();) {
        const uint64_t id = pending_snapshots_[i];
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.state != ClientState::Snapshot) {   // woke up, was destroyed or forgotten meanwhile
            pending_snapshots_.erase(pending_snapshots_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (now < it->second.close_due) {
            ++i;
            continue;
        }
        if (!close(conn, id, it->second, kCloseDormancy, ClientState::Dormant)) break;
        pending_snapshots_.erase(pending_snapshots_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

bool ActorView::pass_due(Clock::time_point now) {
    if (passed_ && now - last_pass_ < config_.relevancy.tick) return false;
    if (config_.phases) {   // the pass runs on the tick of this view's phase, so that the players' passes do not all land on one tick of the cycle (view_phases.h)
        if (!phase_taken_) {
            phase_ = config_.phases->join();
            phase_taken_ = true;
        }
        if (!config_.phases->due(phase_)) return false;
        phase_ = config_.phases->balance(phase_);
    }
    passed_ = true;
    last_pass_ = now;
    return true;
}

bool ActorView::plan(Clock::time_point now) { return !finished_ && pass_due(now); }

void ActorView::run(Connection& conn, double x, double y, Clock::time_point now, bool pass_is_due) {
    if (finished_) return;
    TickProfile& profile = tick_profile();   // (--loop-stats: the worker's own, while the passes run on several threads)
    std::chrono::steady_clock::time_point started{};
    if (profile.on) started = std::chrono::steady_clock::now();
    run_due_closes(conn, now);
    if (pass_is_due) pass(conn, x, y, now);
    if (profile.on) profile.view_us += tick_profile_us_since(started);
}

void ActorView::pass(Connection& conn, double x, double y, Clock::time_point now) {
    ++stats_.passes;
    if (conn.packets_in_flight() > config_.max_packets_in_flight || conn.queued_bunches() > config_.max_queued_bunches) {
        ++stats_.throttled;   // the player's own data goes first: nothing of the world is added to a busy connection
        return;
    }
    TickProfile& profile = tick_profile();   // (--loop-stats: the scan below, apart from the sends after it)
    std::chrono::steady_clock::time_point scan_started{};
    if (profile.on) {
        scan_started = std::chrono::steady_clock::now();
        ++profile.view_passes;
    }
    scan(x, y);
    if (profile.on) profile.view_scan_us += tick_profile_us_since(scan_started);

    size_t opens = 0;
    bool capped = false;
    for (size_t index = 0; index < cands_.size(); ++index) {
        const Cand& c = cands_[index];
        const world::WorldActor* a = reg().find(c.id);
        if (!a) continue;
        if (conn.queued_bunches() > config_.max_queued_bunches) {   // this pass's own sends filled the queue
            ++stats_.throttled;
            break;
        }
        auto it = entries_.find(c.id);
        if (it == entries_.end()) {
            if (c.ring == Ring::Out) continue;
            it = entries_.emplace(c.id, Entry{}).first;
        }
        it->second.scan_rank = static_cast<uint32_t>(index);   // (the next scan starts from this order)
        if (!it->second.broken && pass_entry(conn, c, *a, now, opens)) capped = true;
    }
    if (capped) ++stats_.capped;
}

bool ActorView::pass_entry(Connection& conn, const Cand& c, const world::WorldActor& a, Clock::time_point now, size_t& opens) {
    const auto it = entries_.find(c.id);
    if (it == entries_.end()) return false;
    Entry& e = it->second;
    const size_t max_opens = config_.relevancy.max_opens_per_tick;
    switch (e.state) {
        case ClientState::None:
            if (c.ring == Ring::Out) {
                entries_.erase(it);
                return false;
            }
            if (opens >= max_opens) {
                entries_.erase(it);
                return true;
            }
            if (open(conn, a, e, c.ring == Ring::Snapshot, now)) ++opens;
            else if (!e.broken && e.state == ClientState::None) entries_.erase(it);
            return false;
        case ClientState::Dormant:
            if (c.ring == Ring::Out) {
                ++stats_.forgotten;
                entries_.erase(it);   // the client keeps the actor it has
                return false;
            }
            if (c.ring == Ring::Snapshot && a.reliable_version == e.sent_reliable && e.sent_version != 0) return false;
            if (opens >= max_opens) return true;
            if (open(conn, a, e, c.ring == Ring::Snapshot, now)) {
                ++opens;
                if (c.ring != Ring::Snapshot) ++stats_.wakes;   // (a new snapshot of a changed dormant actor is not a wake)
            }
            return false;
        case ClientState::Snapshot:
            if (c.ring == Ring::Awake) {   // came close before the Dormancy close was due: it stays open
                e.state = ClientState::Awake;
                ++stats_.wakes;
            }
            return false;
        case ClientState::Awake:
            if (c.ring == Ring::Awake) {
                send_update(conn, a, e);
            } else if (c.ring == Ring::Snapshot) {
                if (send_update(conn, a, e) || e.broken) {
                    const uint32_t version = a.version;
                    if (close(conn, c.id, e, kCloseDormancy, ClientState::Dormant)) e.sent_version = version;
                }
            } else if (close(conn, c.id, e, kCloseRelevancy, ClientState::None)) {
                entries_.erase(it);
            }
            return false;
    }
    return false;
}

}  // namespace aoc::worldd
