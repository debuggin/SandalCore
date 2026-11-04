// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/actor_view.h"
#include "session/scan_order.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;
using world::ClientState;
using world::Ring;

namespace {
constexpr size_t kPlayersLogged = 200;

void add_ability_exports(std::vector<repl::ContentBlock>& blocks, const std::vector<repl::ExportChain>& exports) {
    for (repl::ContentBlock& b : blocks) {
        if (b.sub && (!b.sub->obj.schema || std::string(b.sub->obj.schema->name) != "UAoCAbilityComponent")) continue;
        for (const repl::ExportChain& x : exports) b.exports.push_back(x);
    }
}
}  // namespace

std::optional<repl::Emission> ActorView::update_in_place(
    const repl::Actor& actor,
    const std::vector<repl::ExportChain>& exports,
    repl::ActorShadow& shadow,
    Built& built,
    std::vector<repl::ContentBlock>* keep_blocks) {
    built.failed = false;
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_update_blocks(actor, shadow, blocks, &built.undo)) {
        built.failed = true;
        return std::nullopt;
    }
    if (blocks.empty()) {
        built.undo.rollback(shadow);
        return std::nullopt;
    }
    if (!exports.empty()) add_ability_exports(blocks, exports);
    auto e = repl::assemble_blocks(actor, blocks, acks_);
    if (!e) {
        built.undo.rollback(shadow);
        built.failed = true;
    } else {
        built.record = repl::sent_record(built.undo, shadow);
        if (keep_blocks) *keep_blocks = std::move(blocks);
    }
    return e;
}

void ActorView::build_update(const world::WorldActor& a, Entry& e, Built& built) {
    SharedUpdates* sharing = config_.shared.get();
    if (sharing && sharing->active()) {
        build_update_shared(*sharing, a, e, built);
        return;
    }
    repl::Actor scratch;
    const repl::Actor& shown = view_of(a, scratch);
    built.private_form = &shown != &a.actor;
    built.em = update_in_place(shown, a.ability_exports, e.shadow, built);
}

bool ActorView::try_shared_update(SharedUpdates& sharing, const world::WorldActor& a, const Entry& e, Built& built) {
    const UpdateMemo::Variant* hit = sharing.find(a, e.sent_version);
    if (!hit) return false;
    SharedUpdates::Stats& stats = sharing.mine();
    const bool check = sharing.verify_due();
    if (check) ++stats.verified;
    if (check && !shared_update_holds(a, e, *hit)) {
        sharing.mismatch(a.id, "a shared update differs from the update built from the view's own shadow");
        return false;
    }
    built.private_form = false;
    if (!hit->blocks.empty()) {
        built.em = repl::assemble_blocks(a.actor, hit->blocks, acks_);
        if (!built.em) {
            built.failed = true;
            return true;
        }
    }
    built.shared_record = &hit->record;
    built.redo = &hit->redo;
    ++stats.hits;
    return true;
}

void ActorView::build_update_shared(SharedUpdates& sharing, const world::WorldActor& a, Entry& e, Built& built) {
    SharedUpdates::Stats& stats = sharing.mine();
    if (sharing.mode() == ShareMode::Verify && !(repl::shape_of(a.actor) == a.shape)) {
        sharing.mismatch(a.id, "the actor lost a value or a subobject without a counted change");
    }
    // Shared blocks are valid only when the shadow matches the canonical versioned state without per-client overrides or NAK rollback taint.
    const bool clean = sharing.active() && e.sent_version != 0 && !e.tainted && !e.private_form &&
                       e.shape_epoch == a.shape_epoch && !a.keep_fast_shadow && !a.first_open_form &&
                       a.owner == 0 && e.shadow.all_active();
    if (clean && try_shared_update(sharing, a, e, built)) return;
    repl::Actor scratch;
    const repl::Actor& shown = view_of(a, scratch);
    built.private_form = &shown != &a.actor;
    const bool publish = sharing.active() && clean && !built.private_form &&
                         (config_.player_guid.has_value() || !world::is_interactable(a.actor));
    std::vector<repl::ContentBlock> blocks;
    built.em = update_in_place(shown, a.ability_exports, e.shadow, built, publish ? &blocks : nullptr);
    if (!publish || built.failed) {
        ++stats.private_builds;
        return;
    }
    repl::ShadowRedo redo = repl::make_redo(built.undo, e.shadow);
    built.shared_record = &sharing.publish(a, e.sent_version, std::move(blocks), std::move(built.record), std::move(redo)).record;
}

bool ActorView::shared_update_holds(const world::WorldActor& a, const Entry& e, const UpdateMemo::Variant& hit) {
    repl::ActorShadow copy = e.shadow;
    Built plain_built;
    const auto plain = update_in_place(a.actor, a.ability_exports, copy, plain_built);
    std::optional<repl::Emission> shared;
    if (!hit.blocks.empty()) shared = repl::assemble_blocks(a.actor, hit.blocks, acks_);
    if (plain_built.failed || plain.has_value() != shared.has_value()) return false;
    if (plain && (!repl::same_emission(*plain, *shared) || !repl::same_record(plain_built.record, hit.record))) return false;
    repl::ActorShadow after = e.shadow;
    hit.redo.apply(after);
    return repl::shadows_equal(copy, after);
}

bool ActorView::for_this_player(const world::WorldActor& a) const {
    if (a.kind == world::ActorKind::Player) return a.owner != config_.player_id;
    return a.owner == 0 || a.owner == config_.player_id;
}

void ActorView::log_player(const world::WorldActor& a, bool sees, const char* why) {
    const spdlog::level::level_enum level = players_logged_ < kPlayersLogged ? spdlog::level::info : spdlog::level::debug;
    ++players_logged_;
    const std::string me = config_.player_name.empty() ? "player " + std::to_string(config_.player_id) : config_.player_name;
    if (sees) spdlog::log(level, "[players] {} now sees {} (PlayerId {}, pawn {}) at ({:.0f}, {:.0f}, {:.0f}) cm", me, a.player.name, a.owner, a.id, a.x, a.y, a.z);
    else spdlog::log(level, "[players] {} no longer sees {} (PlayerId {}, pawn {}): {}", me, a.player.name, a.owner, a.id, why);
}

void ActorView::scan_consider(uint64_t id, const Entry* entry, double x, double y) {
    const world::WorldActor* a = reg().find(id);
    if (!a || !for_this_player(*a)) return;
    const ClientState prior = entry ? entry->state : ClientState::None;
    const double d = world::horizontal_distance(a->x, a->y, x, y);
    Ring ring = world::classify(config_.relevancy, d, a->awake_radius_cm, a->never_dormant, prior);
    if (a->snapshot_only && ring == Ring::Awake) ring = Ring::Snapshot;
    cands_.push_back({d, id, ring, entry ? entry->scan_rank : kNoRank});
    if (a->awake_radius_cm > largest_awake_ && a->awake_radius_cm <= world::ActorRegistry::kWideCm) {
        largest_awake_ = a->awake_radius_cm;
    }
}

void ActorView::order_scan(double x, double y) {
    const auto nearer = [](const Cand& l, const Cand& r) { return l.d != r.d ? l.d < r.d : l.id < r.id; };
    if (!std::isfinite(x) || !std::isfinite(y)) {
        std::sort(cands_.begin(), cands_.end(), nearer);
        scan_listed_ = cands_.size();
        return;
    }
    // Consecutive passes 67ms apart preserve nearly the same distance order; seed from prior scan_rank so sort_after_small_changes runs in near-linear time.
    place_.assign(scan_listed_, kNoRank);
    for (uint32_t j = 0; j < cands_.size(); ++j) {
        const uint32_t r = cands_[j].rank;
        if (r < place_.size() && place_[r] == kNoRank) place_[r] = j;
    }
    sorted_.clear();
    for (const uint32_t j : place_) {
        if (j != kNoRank) sorted_.push_back(cands_[j]);
    }
    const size_t ordered = sorted_.size();
    for (uint32_t j = 0; j < cands_.size(); ++j) {
        const uint32_t r = cands_[j].rank;
        if (!(r < place_.size() && place_[r] == j)) sorted_.push_back(cands_[j]);
    }
    sort_after_small_changes(sorted_, ordered, nearer, merged_);
    cands_.swap(sorted_);
    scan_listed_ = cands_.size();
}

void ActorView::scan(double x, double y) {
    const world::RelevancyConfig& cfg = config_.relevancy;
    if (++scan_gen_ == 0) {
        for (auto& [id, e] : entries_) e.scan_stamp = 0;
        scan_gen_ = 1;
    }
    const uint32_t gen = scan_gen_;
    scratch_.clear();
    registry_.query(x, y, world::query_radius(cfg, largest_awake_), scratch_);
    const size_t from_grid = scratch_.size();
    registry_.query_wide(x, y, cfg.hysteresis_cm, scratch_);
    cands_.clear();
    for (size_t i = 0; i < scratch_.size(); ++i) {
        const uint64_t id = scratch_[i];
        const auto it = entries_.find(id);
        if (it != entries_.end()) {
            if (it->second.scan_stamp == gen) continue;
            it->second.scan_stamp = gen;
            scan_consider(id, &it->second, x, y);
        } else {
            if (i >= from_grid && std::any_of(cands_.begin(), cands_.end(), [id](const Cand& c) { return c.id == id; })) continue;
            scan_consider(id, nullptr, x, y);
        }
    }
    for (auto& [id, e] : entries_) {
        if (e.scan_stamp == gen) continue;
        e.scan_stamp = gen;
        scan_consider(id, &e, x, y);
    }
    order_scan(x, y);
}

}  // namespace aoc::worldd
