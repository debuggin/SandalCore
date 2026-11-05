// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "combat/combat_world.h"

#include "bootstrap/constants.h"
#include "combat/combat_world_npc.h"
#include "content/content_db.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>

namespace aoc::worldd::combat {

namespace {

const char* const kApplyPhases[] = {"Apply"};
const char* const kTickPhases[] = {"Tick"};
const char* const kExpirePhases[] = {"Expire", "End"};

class BookView final : public TargetEffects {
public:
    BookView(const effects::Book& book, double now) : book_(book), now_(now) {}
    double remaining(int64_t effect_guid) const override { return book_.remaining(effect_guid, now_); }

private:
    const effects::Book& book_;
    double               now_;
};

bootstrap::EffectItem item_of(const effects::Instance& e) {
    bootstrap::EffectItem item;
    item.effect_id = e.key;
    item.effect_guid = e.def->row.guid;
    item.start_time = bootstrap::effect_clock(e.start);
    item.duration = std::isinf(e.end) ? std::numeric_limits<float>::max() : static_cast<float>(e.end - e.start);
    item.tick_time = static_cast<float>(e.def->row.tick_timer);
    item.stacks = e.stacks;
    return item;
}

struct EffectHitLogCtx {
    const std::string&            leaf;
    uint64_t                      actor_id = 0;
    const abilities::HitDef&      hit;
    int64_t                       effect_guid = 0;
    uint64_t                      instigator = 0;
    double                        health_before = 0.0;
    double                        health_after = 0.0;
    uint8_t                       flags = 0;
    bool                          predicted = false;
    double                        at = 0.0;
    const stats::CharacterStats&  cs;
    const CombatStatIds&          id;
};

void build_effect_hit_note(const Resolved& r, const Delivery& d, const CombatStatIds& id, EffectHitNote& note) {
    for (size_t i = 0; i < r.mods.size(); ++i) {
        const ResolvedMod& m = r.mods[i];
        if (m.stat_guid != id.health && m.stat_guid != id.mana) continue;
        if (m.value == 0.0 && d.effective[i] == 0.0) continue;
        note.mods.push_back(m);
        note.effective.push_back(d.effective[i]);
    }
}

void log_effect_hit(size_t& logged, size_t max_logged, const EffectHitLogCtx& ctx, const Resolved& r, const EffectHitNote& note) {
    if (note.mods.empty() || r.miss()) return;
    const stats::StatValue* pool = ctx.id.status_burning ? ctx.cs.find(ctx.id.status_burning) : nullptr;
    const stats::StatValue* dur = ctx.id.status_burning_dur ? ctx.cs.find(ctx.id.status_burning_dur) : nullptr;
    spdlog::log(logged++ < max_logged ? spdlog::level::info : spdlog::level::debug,
                "[combat] {} ({}) takes {:.1f} from {} of player {}{}: Health {:.1f} -> {:.1f}{}", ctx.leaf, ctx.actor_id, note.mods[0].value, ctx.hit.row.name, ctx.instigator,
                (r.flags & stats::kHitCrit) ? " (a critical hit)" : "", ctx.health_before, ctx.health_after,
                pool && dur && pool->value > 0 ? fmt::format("; {:.1f} fire damage left to burn over {:.1f} s", pool->value, dur->value) : std::string());
}

void send_effect_hit_confirm(PlayerPort& port, const EffectHitLogCtx& ctx, const Resolved& r, EffectHitNote& note) {
    if (note.mods.empty() && !ctx.hit.row.has_fx && !r.miss()) return;
    note.holder = ctx.actor_id;
    note.hit_guid = ctx.hit.row.guid;
    note.effect_guid = ctx.effect_guid;
    note.flags = ctx.flags;
    note.predicted = ctx.predicted;
    note.at = ctx.at;
    port.confirm_effect_hit(note, ctx.at);
}

}  // namespace

void CombatWorld::send_effects(Npc& n, const effects::Book::Changes& changes) {
    if (changes.empty()) return;
    n.cs->set_effects(n.book.engine_effects());
    const auto write_of = [&](const effects::Instance& e) {
        EffectItemWrite w;
        w.item = item_of(e);
        if (const PlayerPort* p = player(e.instigator)) w.applier = p->character_guid();
        return w;
    };
    std::vector<EffectItemWrite> added, changed;
    for (const effects::Instance& e : changes.added) added.push_back(write_of(e));
    for (const effects::Instance& e : changes.changed) changed.push_back(write_of(e));
    write_effect_items(registry_, n.actor_id, n.item_ids, changes.removed, added, changed);
    if (n.book.empty()) effect_holders_.erase(n.actor_id);
    else effect_holders_.insert(n.actor_id);
    if (is_pet(n.actor_id)) pet_effects_changed(n);
}

void CombatWorld::clear_effects(Npc& n) {
    effects::Book::Changes changes;
    n.book.clear(changes);
    send_effects(n, changes);
    effect_holders_.erase(n.actor_id);
}

void CombatWorld::drop_effects_of(uint64_t instigator) {
    for (const uint64_t id : std::vector<uint64_t>(effect_holders_.begin(), effect_holders_.end())) {
        const auto it = npcs_.find(id);
        if (it == npcs_.end()) {
            effect_holders_.erase(id);
            continue;
        }
        Npc& n = *it->second;
        effects::Book::Changes changes;
        if (n.book.remove_from(instigator, changes) == 0) continue;
        send_effects(n, changes);
        if (n.book.empty()) {
            for (const int64_t stat : {data_.ids().status_burning, data_.ids().status_burning_dur}) {
                if (stat != 0) n.cs->set_current(stat, 0.0f);
            }
        }
        spdlog::info("[effects] the effects of player {} on {} ({}) end: he is gone", instigator, n.leaf, n.actor_id);
    }
}

void CombatWorld::land_effect(Npc& n, const effects::Applied& a, uint64_t instigator, double now) {
    if (n.dead || !a.def) return;
    if (a.def->row.crowd_control_guid != 0) {
        if (refused_effects_.insert(a.def->row.guid).second) {
            spdlog::info("[effects] {} is a slow, snare or CC: mobs do not slow down or stop for one yet, so it is not put on {}", a.def->row.name, n.leaf);
        }
        return;
    }
    effects::Book::Changes changes;
    effects::Instance created;
    const bool made = n.book.apply(a, instigator, now, changes, &created);
    send_effects(n, changes);
    if (!changes.empty() && a.def->row.category.find("Hidden") == std::string::npos) {
        spdlog::log(effects_logged_++ < kLoggedEffects ? spdlog::level::info : spdlog::level::debug, "[effects] {} ({}) takes {}: {} stack(s), {:.1f} s left, from player {}", n.leaf, n.actor_id,
                    a.def->row.name, n.book.stacks_of(a.def->row.guid), n.book.remaining(a.def->row.guid, now), instigator);
    }
    if (made && effect_depth_ < kMaxEffectDepth) {
        ++effect_depth_;
        run_effect_hits(n, created, kApplyPhases, 1, now);
        --effect_depth_;
    }
}

void CombatWorld::apply_effects(uint64_t npc_actor, uint64_t instigator, const std::vector<effects::Applied>& list, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return;
    for (const effects::Applied& a : list) {
        if (n->dead) break;
        land_effect(*n, a, instigator, now);
    }
}

double CombatWorld::effect_remaining(uint64_t npc_actor, int64_t effect_guid, double now) const {
    const auto it = npcs_.find(npc_actor);
    return it == npcs_.end() ? 0.0 : it->second->book.remaining(effect_guid, now);
}

int CombatWorld::effect_stacks(uint64_t npc_actor, int64_t effect_guid) const {
    const auto it = npcs_.find(npc_actor);
    return it == npcs_.end() ? 0 : it->second->book.stacks_of(effect_guid);
}

size_t CombatWorld::effect_count(uint64_t npc_actor) const {
    const auto it = npcs_.find(npc_actor);
    return it == npcs_.end() ? 0 : it->second->book.size();
}

void CombatWorld::run_effect_hits(Npc& n, const effects::Instance& e, const char* const* phases, size_t count, double at) {
    if (!e.def || n.dead) return;
    PlayerPort* port = player(e.instigator);
    if (!port || !port->fighter().alive()) {
        drop_effects_of(e.instigator);
        return;
    }
    const bool tick_phase = count == 1 && std::string_view(phases[0]) == "Tick";
    Fighter& from = port->fighter();
    const Relation rel = data_.relation(from.alignments(), n.aligns);
    const CombatStatIds& id = data_.ids();
    for (const auto& [phase, hit_guid] : e.def->hits) {
        bool wanted = false;
        for (size_t i = 0; i < count; ++i) wanted = wanted || phase == phases[i];
        if (!wanted) continue;
        if (n.dead) return;
        const abilities::HitDef* hit = catalog_.hit(hit_guid);
        if (!hit) continue;
        const int mask = hit->row.applicable_targets;
        if (rel == Relation::Hostile ? (mask & 2) == 0 : (mask & (1 | 4 | 8 | 16)) == 0) continue;
        HitMeta meta{.source_type = content::kSourceTypeEffect, .source_guid = e.def->row.guid};
        const BookView view(n.book, at);
        meta.target_effects = &view;
        if (!resolver_.requirement_holds(from, n, *hit, meta)) continue;
        Resolved r;
        std::string why;
        if (!resolver_.resolve(from, n, *hit, meta, r, why)) {
            spdlog::debug("[combat] {} ({}): the hit {} of {} does not run: {}", n.leaf, n.actor_id, hit->row.name, e.def->row.name, why);
            continue;
        }
        // Recalculated ticks evaluate one stack and scale by current stack count.
        if (tick_phase && e.def->row.recalc_on_tick && e.stacks > 1 && !r.miss()) {
            for (ResolvedMod& m : r.mods) m.value *= static_cast<double>(e.stacks);
            r.threat *= static_cast<double>(e.stacks);
        }
        const double health_before = n.health();
        HitOrigin origin{.hit = hit, .source_type = content::kSourceTypeEffect, .source_guid = e.def->row.guid, .landed_here = true};
        const Delivery d = land_hit(n, e.instigator, r, at, false, 0, &origin);
        if (!d.applied) return;
        EffectHitNote note;
        build_effect_hit_note(r, d, id, note);
        const EffectHitLogCtx log_ctx{n.leaf, n.actor_id, *hit, e.def->row.guid, e.instigator, health_before, n.health(), d.flags, tick_phase, at, *n.cs, id};
        log_effect_hit(effect_hits_logged_, kLoggedEffectHits, log_ctx, r, note);
        send_effect_hit_confirm(*port, log_ctx, r, note);
        if (!n.dead) effect_hit_fires(n, e.instigator, *hit, e.def->row.guid, at);
        if (n.dead || r.miss()) continue;
        Resolved gate;
        if (!resolver_.resolve_effects(from, n, *hit, meta, gate, why)) continue;
        if (!why.empty()) spdlog::debug("[combat] {} ({}): the effects of {}: {}", n.leaf, n.actor_id, hit->row.name, why);
        for (const effects::Applied& a : gate.effects) {
            if (n.dead) return;
            land_effect(n, a, e.instigator, at);
        }
    }
}

void CombatWorld::advance_effects(double now) {
    if (effect_holders_.empty()) return;
    for (const uint64_t id : std::vector<uint64_t>(effect_holders_.begin(), effect_holders_.end())) {
        for (size_t step = 0; step < kMaxEffectSteps; ++step) {
            const auto it = npcs_.find(id);
            if (it == npcs_.end() || it->second->dead || it->second->book.empty()) {
                effect_holders_.erase(id);
                break;
            }
            Npc& n = *it->second;
            const effects::Book::Due due = n.book.next_due(now);
            if (due.kind == effects::Book::Due::Kind::None) break;
            if (due.kind == effects::Book::Due::Kind::End) {
                effects::Instance gone;
                effects::Book::Changes changes;
                if (!n.book.end_of(due.key, gone, changes)) break;
                send_effects(n, changes);
                run_effect_hits(n, gone, kExpirePhases, 2, due.at);
            } else {
                effects::Instance ticking;
                if (!n.book.ticked(due.key, due.at, ticking)) break;
                run_effect_hits(n, ticking, kTickPhases, 1, due.at);
            }
        }
    }
}

}  // namespace aoc::worldd::combat
