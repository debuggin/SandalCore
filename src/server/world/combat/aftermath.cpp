// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/aftermath.cpp (see aftermath.h)
// ============================================================================
#include "combat/aftermath.h"

#include "items/rarity.h"
#include "protocol/repl/value.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aoc::worldd::combat {

namespace {

std::vector<std::string> tags_of(const std::string& csv) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < csv.size()) {
        size_t end = csv.find(',', at);
        if (end == std::string::npos) end = csv.size();
        if (end > at) out.push_back(csv.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

std::vector<PlayerPort*> collect_death_fighters(CombatWorld& combat, const DeathEvent& d, std::set<uint64_t>& rights) {
    // who fought it and is here to be rewarded
    std::vector<PlayerPort*> fighters;
    if (d.gm) {
        // a game master's kill: nobody fought it, so every player in the world may take its loot, and nobody is given experience
        fighters = combat.players();
    } else {
        if (const auto killer = combat.find_player(d.killer)) fighters.push_back(killer);
        for (const uint64_t id : d.attackers) {
            PlayerPort* p = combat.find_player(id);
            if (p && std::find(fighters.begin(), fighters.end(), p) == fighters.end()) fighters.push_back(p);
        }
    }
    for (PlayerPort* p : fighters) rights.insert(p->actor_id());
    return fighters;
}

std::string format_looter_note(const DeathEvent& d, double max_level, const AftermathConfig& cfg) {
    // Loot_Falloff is 1 for a gap of 0 to 9 levels and 0 from 10 (the reward tables' own, settled against retail 2026-10-08): said on the line so a drop that did not come can be read
    const int gap = static_cast<int>(std::lround(std::fabs(max_level - static_cast<double>(d.level))));
    std::string looter_note = ", looter level " + std::to_string(static_cast<int>(std::lround(max_level))) + " (" + std::to_string(gap) + " level" + (gap == 1 ? "" : "s") + " apart: Loot_Falloff " +
                  (gap >= 10 ? "0, no gear, recipe or Glint loot" : "1") + ")" + (cfg.loot_chance_mult != 1.0 ? ", loot chances x" + std::to_string(cfg.loot_chance_mult).substr(0, 5) : std::string());
    if (d.gm) looter_note = ", a game master's kill, rolled as for a player of the creature's own level" + (cfg.loot_chance_mult != 1.0 ? ", loot chances x" + std::to_string(cfg.loot_chance_mult).substr(0, 5) : std::string());
    if (cfg.loot_rarity_boost != 1.0) {   // the test boost of the rarity rolls (world --loot-rarity-boost): said on every kill's line
        char boost[32];
        std::snprintf(boost, sizeof boost, "%g", cfg.loot_rarity_boost);
        looter_note += std::string(", rarity odds x") + boost + " (test boost)";
    }
    return looter_note;
}

struct DeathRollOutcome {
    Rolled rolled;
    std::string looter_note;
    double max_level = 1.0;
};

DeathRollOutcome roll_death_rewards(const DeathEvent& d, const std::vector<PlayerPort*>& fighters, const RewardBook& book, const AftermathConfig& cfg,
                                    RollContext& ctx) {
    DeathRollOutcome out;
    for (PlayerPort* p : fighters) out.max_level = std::max<double>(out.max_level, p->fighter().level());
    if (d.gm) out.max_level = std::max(1, d.level);   // rolled as for a player of the creature's own level: no level gap takes anything off
    std::vector<std::string> tags = d.motion ? tags_of(d.motion->tags) : std::vector<std::string>{};
    ctx.npc_tags = &tags;
    ctx.loot_max_level = out.max_level;
    ctx.chance_mult = cfg.loot_chance_mult;
    ctx.rarity_boost = cfg.loot_rarity_boost;
    out.looter_note = format_looter_note(d, out.max_level, cfg);
    if (ctx.nodes) {
        // the zone of influence where it died: its biome and node answer the tables' node reads
        const world::NodeSite site = ctx.nodes->node_at(d.position.x, d.position.y);
        ctx.node = site.node_rank;
        ctx.biome = site.biome;
    }
    std::vector<int64_t> roots;
    if (d.motion) roots = d.motion->rewards;
    if (d.info) roots.insert(roots.end(), d.info->loot_tables.begin(), d.info->loot_tables.end());
    ctx.player_level = out.max_level;
    out.rolled = book.roll(roots, ctx);
    ctx.npc_tags = nullptr;
    return out;
}

std::string format_roll_note(const Rolled& rolled, double elite_rank) {
    // what the rarity rolls did, for the death line: the creature's elite rank (a gear roll adds 2 per rank), the qualities rolled, and the Common outcomes the test boost threw back
    if (rolled.quality_rolls <= 0 && rolled.quality_failures <= 0) return {};
    return ", " + std::to_string(rolled.quality_rolls) + " quality roll(s) at elite rank " + std::to_string(static_cast<int>(std::lround(elite_rank))) +
           (rolled.boost_redraws > 0 ? " (" + std::to_string(rolled.boost_redraws) + " Common outcome(s) thrown back by the rarity boost)" : std::string()) +
           (rolled.quality_failures > 0 ? ", " + std::to_string(rolled.quality_failures) + " quality formula(s) did not run" : std::string());
}

std::vector<LootStack> build_loot_stacks(const Rolled& rolled, const std::function<int(int64_t)>& quality_of, const items::CurrencyTable* currencies) {
    // the loot: a stack has the quality its variation rolled (a gear or a resource drop), else the item record's default (a Glint tier keeps its 1, 21 or 41)
    std::vector<LootStack> stacks;
    for (const std::vector<RolledItem>* list : {&rolled.loot, &rolled.direct}) {
        for (const RolledItem& it : *list) {
            LootStack s;
            s.item = it.item;
            s.variation = it.variation;
            s.quantity = it.quantity;
            s.quality = it.quality > 0 ? std::clamp(it.quality, items::kMinQuality, items::kMaxQuality) : (quality_of ? std::max(1, quality_of(it.item)) : 1);
            stacks.push_back(s);
        }
    }
    if (currencies && !stacks.empty()) {
        // Glint tiers: the rolled Glint of a currency tier is summed and paid in its kinds, the biggest first, each under its rate; the other stacks stay where they are
        std::vector<items::CurrencyLine> lines;
        lines.reserve(stacks.size());
        for (const LootStack& s : stacks) lines.push_back({s.item, s.quantity});
        const items::CompressedLines split = currencies->compress(lines);
        if (split.changed) {
            std::vector<LootStack> paid;
            paid.reserve(split.lines.size());
            for (const items::CompressedLines::Line& l : split.lines) {
                if (l.from >= 0) {
                    paid.push_back(stacks[static_cast<size_t>(l.from)]);
                    continue;
                }
                LootStack s;
                s.item = l.item;
                s.quantity = l.quantity;
                s.quality = quality_of ? std::max(1, quality_of(l.item)) : 1;
                paid.push_back(s);
            }
            stacks = std::move(paid);
        }
    }
    return stacks;
}

struct LootAttachEnv {
    world::ActorRegistry& registry;
    const world::Minter& mint;
    double loot_corpse_seconds = 0.0;
    Aftermath::Stats& stats;
    std::function<int(int64_t, int)> rarity_of;
};

void attach_corpse_loot(const DeathEvent& d, const std::vector<PlayerPort*>& fighters, std::vector<LootStack> stacks, Corpse& c,
                        const LootAttachEnv& env) {
    if (!stacks.empty() && !fighters.empty()) {
        int top = 0;
        for (const LootStack& s : stacks) top = std::max(top, env.rarity_of(s.item, s.quality));
        std::vector<protocol::repl::GuidValue> guids;
        for (PlayerPort* p : fighters) {
            const auto w = p->character_guid();
            guids.push_back({w[0], w[1], w[2], w[3]});
        }
        std::string why;
        const auto parts = world::add_loot(env.registry, d.npc, env.mint, guids.front(), guids, top, why);
        if (parts) {
            c.parts = *parts;
            c.loot = std::move(stacks);
            c.top_rarity = top;
            c.close_at = d.at + env.loot_corpse_seconds;
            ++env.stats.corpses_with_loot;
            env.stats.loot_stacks += c.loot.size();
        } else {
            spdlog::warn("[loot] {} ({}) died with {} stack(s) of loot but its corpse takes no components: {}", c.name, d.blueprint, stacks.size(), why);
        }
    } else if (!stacks.empty() && d.gm) {
        // a game master's kill with nobody in the world to take it: the roll stays on the corpse (the log and Inspect say what it holds), but it gets no loot components
        c.loot = std::move(stacks);
    }
}

uint64_t award_death_xp(const DeathEvent& d, const Corpse& c, const std::vector<PlayerPort*>& fighters, const std::vector<RolledXp>& xp_rows,
                        const std::function<int64_t(double, double)>& xp_at_level) {
    // the experience: each fighter's own decay (a game master's kill gives none)
    uint64_t xp_awarded = 0;
    for (PlayerPort* p : fighters) {
        if (d.gm) break;
        const double player_level = p->fighter().level();
        KillReward kr;
        kr.npc = d.npc;
        kr.name = c.name;
        kr.blueprint = d.blueprint;
        kr.level = d.level;
        kr.instance = d.info ? d.info->instance_guid : 0;
        for (const RolledXp& x : xp_rows) {
            const int64_t amount = xp_at_level(x.amount, player_level);
            if (x.type == "Adventuring") kr.adventuring_xp += amount;
            else if (x.type == "Weapon") kr.weapon_xp += amount;
            else if (x.type == "NodeActivity") kr.node_xp += amount;
        }
        kr.loot = c.parts.has_value();
        p->receive_kill(kr, d.at);
        xp_awarded += static_cast<uint64_t>(std::max<int64_t>(0, kr.adventuring_xp));
    }
    return xp_awarded;
}

std::string format_loot_listing(const std::vector<LootStack>& loot, const std::function<std::string(int64_t)>& name_of,
                                const std::function<int(int64_t, int)>& rarity_fn) {
    std::string listing;
    for (const LootStack& s : loot) {
        const std::string n = name_of ? name_of(s.item) : std::string();
        // each stack names its quality and the rarity the client shows for it (a Glint tier is always its colour)
        listing += (listing.empty() ? "" : ", ") + (n.empty() ? "item " + std::to_string(s.item) : n) + " x" + std::to_string(s.quantity) + " q" + std::to_string(s.quality) + " " +
                 std::string(items::rarity_name(rarity_fn(s.item, s.quality)));
    }
    return listing;
}

}  // namespace

int Aftermath::rarity_of(int64_t item, int quality) const {
    return rarity_of_ ? rarity_of_(item, quality) : items::default_rarity_facts().rarity_of(static_cast<double>(quality));
}

Aftermath::Aftermath(world::ActorRegistry& registry, CombatWorld& combat, const RewardBook* book, world::Minter mint, AftermathConfig config)
    : registry_(registry), combat_(combat), book_(book), mint_(std::move(mint)), cfg_(std::move(config)) {
    rng_.state = cfg_.seed * 0x9E3779B97F4A7C15ull + 11;
}

Aftermath::~Aftermath() = default;

void Aftermath::install() {
    combat_.set_death_hook([this](const DeathEvent& d) { on_death(d); });
}

void Aftermath::on_death(const DeathEvent& d) {
    ++stats_.deaths;
    const bool huntable = huntable_ && d.info && huntable_(*d.info);
    Corpse c;
    c.npc = d.npc;
    c.died_at = d.at;
    c.close_at = d.at + (huntable ? cfg_.hunt_corpse_seconds : cfg_.corpse_seconds);
    c.huntable = huntable;
    if (!cfg_.enabled) {
        // the corpse still goes: nothing more is made of the death
        // (a huntable's corpse is hunted, not looted: it stays for the hunt whatever happens to the rewards)
        corpses_[d.npc] = std::move(c);
        return;
    }
    // Gathering stage 3: no loot pile, a corpse that waits for the hunter
    // (the Nodes 1 test of 2026-10-10: the death and loot lines had an empty creature name) the creature's name, else its Blueprint's
    c.name = d.info && !d.info->name.empty() ? d.info->name : d.blueprint;
    std::string looter_note;   // for the death line: the highest looter's level, the level gap and the data's Loot_Falloff
    std::string roll_note;     // ... and what the rarity rolls did (item rarities, step 1)
    const std::vector<PlayerPort*> fighters = collect_death_fighters(combat_, d, c.rights);
    // the respawn: from the death, not from the corpse's closing
    if (d.info) schedule_respawn(*d.info, d.at, d.npc);
// C.Y.B and C-O-A

    if (book_ && (!fighters.empty() || d.gm)) {
        Fighter* npc = combat_.fighter_of(d.npc);
        std::string err;
        if (std::unique_ptr<stats::CharacterStats::Evaluation> ev = npc ? npc->stats().evaluation({}, err) : nullptr) {
            RollContext ctx;
            ctx.npc = ev.get();
            ctx.rng = &rng_;
            ctx.nodes = nodes_;
            DeathRollOutcome out = roll_death_rewards(d, fighters, *book_, cfg_, ctx);
            looter_note = std::move(out.looter_note);
            if (huntable) {   // the kill's experience stays; its items (the Glint certificates, the Bezoar roll) are not dropped: the corpse's reward is the hunt's
                out.rolled.loot.clear();
                out.rolled.direct.clear();
            }
            roll_note = format_roll_note(out.rolled, npc->stats().npc().elite_rank);
            LootAttachEnv env{registry_, mint_, cfg_.loot_corpse_seconds, stats_, [this](int64_t item, int q) { return rarity_of(item, q); }};
            attach_corpse_loot(d, fighters, build_loot_stacks(out.rolled, quality_of_, currencies_), c, env);
            stats_.xp_awarded += award_death_xp(d, c, fighters, out.rolled.xp, [&](double amt, double lvl) {
                ctx.player_level = lvl;
                return book_->xp_for(amt, ctx);
            });
        } else {
            spdlog::debug("[loot] {} has no stats to roll its rewards with: {}", d.blueprint, err);
        }
    }
    const std::string listing = format_loot_listing(c.loot, name_of_, [this](int64_t item, int q) { return rarity_of(item, q); });
    const std::string who = d.gm ? std::to_string(fighters.size()) + " player(s) in the world may take its loot" : std::to_string(fighters.size()) + " fought it";
    spdlog::info("[loot] {} ({}, level {}) died: {}{}{}, {} stack(s) of loot{}{}{}, closes in {:.0f} s{}{}", c.name, d.blueprint, d.level, who, looter_note, roll_note, c.loot.size(),
                 listing.empty() ? "" : " (", listing, listing.empty() ? "" : ")", c.close_at - d.at, respawns_.empty() ? "" : "; its spawn point refills it",
                 huntable ? "; a huntable: its corpse is hunted with a Bow (no loot)" : "");
    corpses_[d.npc] = std::move(c);
}

bool Aftermath::refills(const world::NpcInfo& info) const {
    return cfg_.respawn && respawner_ && info.spawn_ref != 0 && info.respawn_rule != "NoRespawn";
}

double Aftermath::respawn_delay(const world::NpcInfo& info) {
    if (!refills(info)) return 0.0;
    const double base = info.respawn_seconds > 0 ? info.respawn_seconds : cfg_.default_respawn_seconds;
    const double var = std::max(0.0, info.respawn_variance);
    return std::max(cfg_.min_respawn_seconds, base + var * (2.0 * rng_.rand_double() - 1.0));
}

void Aftermath::schedule_respawn(const world::NpcInfo& info, double at, uint64_t npc) {
    if (!refills(info)) return;
    respawns_.push_back({at + respawn_delay(info), info.spawn_ref, npc});
}

double Aftermath::respawn_in(uint64_t npc, double now) const {
    if (npc == 0) return 0.0;
    for (const Respawn& r : respawns_) {
        if (r.npc == npc) return std::max(0.0, r.at - now);
    }
    return 0.0;
}

uint64_t Aftermath::respawn_now(uint64_t npc, double now) {
    if (npc == 0 || !respawner_) return 0;
    for (auto it = respawns_.begin(); it != respawns_.end(); ++it) {
        if (it->npc != npc) continue;
        const uint32_t ref = it->spawn_ref;
        respawns_.erase(it);
        const uint64_t made = respawner_(ref, now);
        if (made != 0) ++stats_.respawns;
        return made;
    }
    return 0;
}

bool Aftermath::despawn(uint64_t npc, double now, bool refill, double* seconds) {
    if (seconds) *seconds = 0.0;
    const world::WorldActor* a = registry_.find(npc);
    if (!a) return false;
    const world::NpcInfo info = a->npc;   // (a copy: the actor goes)
    const bool waiting = respawn_in(npc, now) > 0.0 || std::any_of(respawns_.begin(), respawns_.end(), [&](const Respawn& r) { return r.npc == npc; });
    corpses_.erase(npc);
    registry_.remove(npc);   // every connection that holds it closes the channel (Destroyed)
    combat_.forget(npc);
    if (refill && !waiting) schedule_respawn(info, now, npc);
    if (seconds) *seconds = respawn_in(npc, now);
    return true;
}

void Aftermath::tick(double now) {
    for (auto it = corpses_.begin(); it != corpses_.end();) {
        if (now >= it->second.close_at && it->second.living) {
            it = corpses_.erase(it);   // (a hunt nobody ended: the creature is alive, it stays; only its record goes)
        } else if (now >= it->second.close_at) {
            const uint64_t id = it->first;
            it = corpses_.erase(it);
            registry_.remove(id);   // every connection that holds it closes the channel (Destroyed)
            combat_.forget(id);
            ++stats_.corpses_closed;
        } else {
            ++it;
        }
    }
    for (auto it = respawns_.begin(); it != respawns_.end();) {
        if (now >= it->at) {
            const uint32_t ref = it->spawn_ref;
            it = respawns_.erase(it);
            if (respawner_ && respawner_(ref, now) != 0) ++stats_.respawns;
        } else {
            ++it;
        }
    }
}

const Corpse* Aftermath::corpse_of(uint64_t npc) const {
    const auto it = corpses_.find(npc);
    return it == corpses_.end() ? nullptr : &it->second;
}

const Corpse* Aftermath::corpse_by_interactable(uint64_t object_id) const {
    for (const auto& [id, c] : corpses_) {
        if (c.parts && c.parts->interactable == object_id) return &c;
    }
    return nullptr;
}

const LootStack* Aftermath::take(uint64_t npc, size_t slot) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || slot >= it->second.loot.size() || it->second.loot[slot].taken) return nullptr;
    it->second.loot[slot].taken = true;
    ++stats_.loot_taken;
    return &it->second.loot[slot];
}

void Aftermath::put_back(uint64_t npc, size_t slot) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || slot >= it->second.loot.size() || !it->second.loot[slot].taken) return;
    it->second.loot[slot].taken = false;
    if (stats_.loot_taken > 0) --stats_.loot_taken;
}
// Cyb captures, C O A code

uint64_t Aftermath::drop_pile(double x, double y, double z, const protocol::repl::GuidValue& owner, const std::string& name, std::vector<LootStack> stacks, double now,
                              std::string& error) {
    if (stacks.empty()) return 0;
    int top = 0;
    for (const LootStack& s : stacks) top = std::max(top, rarity_of(s.item, s.quality));
    const auto pile = world::add_pile(registry_, mint_, x, y, z, owner, name, {owner}, top, error);
    if (!pile) return 0;
    Corpse c;
    c.npc = pile->actor;
    c.name = name + "'s pile";
    c.died_at = now;
    c.close_at = now + cfg_.pile_seconds;
    c.loot = std::move(stacks);
    c.parts = pile->parts;
    c.top_rarity = top;
    c.is_pile = true;
    corpses_[c.npc] = std::move(c);
    ++stats_.piles;
    return pile->actor;
}

bool Aftermath::hunt_started(uint64_t npc, double now, double seconds) {
    auto it = corpses_.find(npc);
    if (it == corpses_.end()) {
        // a huntable creature that is alive: hunted as it stands
        const world::WorldActor* a = registry_.find(npc);
        if (!a || a->kind != world::ActorKind::Npc || !huntable_ || !huntable_(a->npc)) return false;
        Corpse c;
        c.npc = npc;
        c.name = a->npc.name;
        c.died_at = now;
        c.close_at = now + seconds + 5.0;
        c.huntable = true;
        c.living = true;
        corpses_[npc] = std::move(c);
        return true;
    }
    if (!it->second.huntable) return false;
    it->second.close_at = std::max(it->second.close_at, now + seconds + 5.0);   // the channel is never cut short by the corpse going
    return true;
}

void Aftermath::hunt_cancelled(uint64_t npc) {
    const auto it = corpses_.find(npc);
    if (it != corpses_.end() && it->second.living) corpses_.erase(it);
}

bool Aftermath::hunt_done(uint64_t npc, double now) {
    auto it = corpses_.find(npc);
    // a completion that comes more than 5 s after the channel's end (a stall) finds no record: a huntable creature that still stands is hunted as it stands, so that it still dies and is removed
    if (it == corpses_.end() && hunt_started(npc, now, 0.0)) it = corpses_.find(npc);
    if (it == corpses_.end() || !it->second.huntable) return false;
    Corpse& c = it->second;
    if (c.living) {   // the hunt is the creature's end: it dies for its clients with the done update, and its spawn point refills it
        c.living = false;
        c.died_at = now;
        if (const world::WorldActor* a = registry_.find(npc)) schedule_respawn(a->npc, now, npc);
    }
    c.hunted = true;
    c.close_at = std::min(c.close_at, now + cfg_.hunt_done_seconds);
    return true;
}

void Aftermath::emptied(uint64_t npc, double now) {
    const auto it = corpses_.find(npc);
    if (it == corpses_.end() || it->second.emptied) return;
    Corpse& c = it->second;
    c.emptied = true;
    world::empty_loot(registry_, npc);
    c.close_at = std::min(c.close_at, now + cfg_.emptied_seconds);
}

void Aftermath::upsert_halo_corpse(const Corpse& c, double now) {
    if (c.npc == 0) return;
    auto it = corpses_.find(c.npc);
    if (it == corpses_.end()) {
        Corpse copy = c;
        const double lifetime = c.is_pile ? cfg_.pile_seconds : cfg_.corpse_seconds;
        if (copy.close_at <= now) copy.close_at = now + lifetime;
        if (!copy.is_pile && !copy.rights.empty()) {
            registry_.for_each([&](const world::WorldActor& a) {
                if (a.kind == world::ActorKind::Player) copy.rights.insert(a.id);
            });
        }
        corpses_.emplace(c.npc, std::move(copy));
        return;
    }
    Corpse& local = it->second;
    for (const uint64_t r : c.rights) local.rights.insert(r);
    if (!local.is_pile && !local.rights.empty()) {
        registry_.for_each([&](const world::WorldActor& a) {
            if (a.kind == world::ActorKind::Player) local.rights.insert(a.id);
        });
    }
    const size_t n = std::min(local.loot.size(), c.loot.size());
    for (size_t i = 0; i < n; ++i) {
        if (c.loot[i].taken && !local.loot[i].taken) local.loot[i].taken = true;
    }
    if ((c.emptied || (!local.loot.empty() && local.remaining() == 0)) && !local.emptied) {
        emptied(c.npc, now);
    }
}

}  // namespace aoc::worldd::combat
