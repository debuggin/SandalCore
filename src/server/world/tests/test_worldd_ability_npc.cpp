// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_ability_npc_common.h"

#include "ability_npc_known.h"
#include "capture_npc_casts.h"

#include <spdlog/spdlog.h>

#include <bit>
#include <regex>
#include <sstream>

using namespace sandal::worldd::tests;

namespace {

using NpcCase = capture_npc_casts::Case;

struct RItem {
    std::string              kind;
    int                      ms = 0;
    std::vector<std::string> f;
};

std::string join(const char* const* parts) {
    std::string out;
    for (; *parts; ++parts) out += *parts;
    return out;
}

std::vector<RItem> parse_lines(const char* const* parts) {
    std::vector<RItem> out;
    std::istringstream in(join(parts));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        RItem r;
        ls >> r.kind >> r.ms;
        for (std::string w; ls >> w;) r.f.push_back(w);
        out.push_back(std::move(r));
    }
    return out;
}

std::map<int64_t, double> parse_before(const char* const* parts) {
    std::map<int64_t, double> out;
    std::istringstream in(join(parts));
    long long rec;
    double v;
    while (in >> rec >> v) out[rec] = v;
    return out;
}

double   to_d(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
int64_t  to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }
uint64_t to_u(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

int class_enum(const std::string& name) {
    static const char* kNames[] = {"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
    for (int i = 1; i <= 8; ++i) {
        if (name == kNames[i]) return i;
    }
    return 2;
}

constexpr double kItemTolerance = 0.060, kStepTolerance = 0.120, kEffectTolerance = 0.150;

void closure(const abilities::Catalog& cat, const abilities::AbilityDef& def, std::set<int64_t>& effects, std::set<int64_t>& hits) {
    std::vector<int64_t> queue;
    for (const auto& [key, e] : def.effects) effects.insert(e);
    for (const content::AbilityHitRow& h : def.hits) queue.push_back(h.hit_guid);
    for (const auto& [key, e] : def.effects) {
        if (const abilities::EffectDef* ed = cat.effect(e))
            for (const auto& [phase, hit] : ed->hits) queue.push_back(hit);
    }
    while (!queue.empty()) {
        const int64_t g = queue.back();
        queue.pop_back();
        if (!hits.insert(g).second) continue;
        const abilities::HitDef* h = cat.hit(g);
        if (!h) continue;
        for (const content::HitEffectRow& x : h->effects) {
            if (effects.insert(x.effect_guid).second) {
                if (const abilities::EffectDef* e = cat.effect(x.effect_guid)) {
                    for (const auto& [phase, hit] : e->hits) queue.push_back(hit);
                }
            }
        }
    }
}

std::vector<stats::SkillRank> skills_in(const std::string& text) {
    static const std::regex re(R"(\$#297887639323616934:(\d+)\$\s*,\s*\$#(\d+):(\d+)\$)");
    std::vector<stats::SkillRank> out;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        stats::SkillRank r;
        r.cls = {stats::kRefTypeSkillTree, std::stoll((*it)[1])};
        r.skill = {std::stoull((*it)[2]), std::stoll((*it)[3])};
        out.push_back(r);
    }
    return out;
}

void find_best_talent_subset(Rig& r, const abilities::AbilityDef& def, const std::vector<stats::SkillRank>& cand, std::pair<double, int> target, std::vector<stats::SkillRank>& extra) {
    for (unsigned size = 0; size <= cand.size(); ++size) {
        for (unsigned mask = 0; mask < (1u << cand.size()); ++mask) {
            if (static_cast<unsigned>(std::popcount(mask)) != size) continue;
            std::vector<stats::SkillRank> trial = extra;
            for (size_t i = 0; i < cand.size(); ++i)
                if (mask & (1u << i)) trial.push_back(cand[i]);
            std::vector<stats::SkillRank> all = r.base_ranks;
            all.insert(all.end(), trial.begin(), trial.end());
            r.cs->set_ranks(all);
            std::string err;
            auto ev = r.cs->evaluation({}, err);
            if (!ev) continue;
            stats::ExprHost host;
            double cd = 0, ch = 1;
            if (def.row.cooldown_expr != 0 && !ev->eval(r.catalog->program(def.row.cooldown_expr), host, cd, err)) continue;
            if (def.row.charges_expr != 0 && !ev->eval(r.catalog->program(def.row.charges_expr), host, ch, err)) continue;
            if (std::fabs(cd - target.first) < 1e-3 && static_cast<int>(std::lround(ch)) >= target.second + 1) {
                extra = trial;
                return;
            }
        }
    }
}

std::vector<stats::SkillRank> infer_talents(Rig& r, int capture) {
    std::vector<stats::SkillRank> extra;
    std::set<int64_t> done;
    for (const NpcCase& c : capture_npc_casts::kCases) {
        if (c.capture != capture || !done.insert(c.ability).second) continue;
        const abilities::AbilityDef* def = r.catalog->ability(c.ability);
        if (!def || r.rules.ability(c.ability)) continue;
        std::vector<stats::SkillRank> cand;
        for (const int64_t id : {def->row.cooldown_expr, def->row.charges_expr}) {
            if (const content::ExprRow* e = r.catalog->expr(id)) {
                for (const stats::SkillRank& s : skills_in(e->text)) {
                    bool dup = false;
                    for (const stats::SkillRank& x : cand) dup = dup || (x.skill == s.skill && x.cls == s.cls);
                    if (!dup) cand.push_back(s);
                }
            }
        }
        if (cand.empty() || cand.size() > 4) continue;
        double cooldown = -1;
        int most_left = -1;
        for (const NpcCase& d : capture_npc_casts::kCases) {
            if (d.capture != capture || d.ability != c.ability) continue;
            for (const RItem& it : parse_lines(d.reply)) {
                if (it.kind == "CD" && to_i(it.f[0]) == c.ability) {
                    cooldown = to_d(it.f[1]);
                    most_left = std::max(most_left, static_cast<int>(to_i(it.f[2])));
                }
            }
        }
        if (cooldown >= 0) find_best_talent_subset(r, *def, cand, {cooldown, most_left}, extra);
    }
    return extra;
}

size_t grant_states(Rig& r, const abilities::AbilityDef& def, double t) {
    const content::ExprRow* e = r.catalog->expr(def.row.requirement_expr);
    if (!e) return 0;
    size_t granted = 0;
    static const std::regex re(R"(GetEffectCount\(Get(?:Instigator|Owner)\(\),\s*\$#\d+:(\d+)\$\))");
    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), re); it != std::sregex_iterator(); ++it) {
        const int64_t guid = std::stoll((*it)[1]);
        if (r.catalog->effect(guid) && !r.pawn.effect(guid) && r.pipe->grant_effect(guid, 1, t)) ++granted;
    }
    static const std::regex stat_re(R"(GetStat\(Get(?:Instigator|Owner)\(\),\s*\$#\d+:(\d+)\$\)\)?\s*>=\s*([0-9.]+))");
    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), stat_re); it != std::sregex_iterator(); ++it) {
        const int64_t stat = std::stoll((*it)[1]);
        const double need = std::stod((*it)[2]);
        const stats::StatValue* v = r.cs->find(stat);
        if (v && v->value < need) {
            r.set_current(stat, need);
            ++granted;
        }
    }
    std::string lower;
    for (const char ch : e->text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.find("pet.summon") != std::string::npos) {
        const char* pet = def.row.name.find("Mystic") != std::string::npos ? "Summoner_Pet_Mystic" : (def.row.name.find("Guardian") != std::string::npos ? "Summoner_Pet_Guardian" : "Summoner_Pet_Hunter");
        const abilities::EffectDef* ed = r.catalog->effect_named(pet);
        if (ed && !r.pawn.effect(ed->row.guid) && r.pipe->grant_effect(ed->row.guid, 1, t)) ++granted;
    }
    return granted;
}

struct Tally {
    size_t total = 0, accepted = 0, refused_listed = 0, matched = 0, exact = 0, timed = 0, with_ts = 0, hit_cases = 0, hit_landed = 0, mods_same = 0, mods_diff = 0;
    std::vector<double> ratios;
};

struct NpcRunState {
    bool verbose = false, use_known = true;
    std::string only_class, only_ability;
    int only_capture = 0;
    std::string wolf_path;
    npc::MotionTable motion;
    std::map<std::string, Tally> tally;
    std::map<std::string, size_t> reasons;
    std::map<std::string, std::vector<std::string>> reason_cases;
    std::map<std::string, size_t> kinds;
    size_t at_npc = 0, at_air = 0, at_self = 0, states_granted = 0;
};

struct CaptureSim {
    Rig& r;
    combat_rig::World& w;
    combat::CombatWorld& cw;
    double world_now = 0;
    double clock_no_ts = 10.0;
    uint64_t creature = 0;

    void advance(double t) {
        while (world_now < t - 1e-9) {
            world_now = std::min(t, world_now + 0.1);
            w.npcs->tick(world_now);
            cw.tick(world_now);
            r.run_to(world_now);
        }
        if (t < world_now) r.run_to(t);
    }
};

struct CastEval {
    const NpcCase& c;
    const abilities::AbilityDef* def = nullptr;
    double t0 = 0;
    double mana_before = 0;
    bool has_ts = false;
    std::vector<RItem> reply;
    std::vector<RItem> client;
    std::map<int64_t, double> before;
    std::vector<const Seen*> events;
    std::set<int64_t> reach_effects, reach_hits;
    std::vector<std::string> problems, late;

    void problem(const std::string& p) { problems.push_back(p); }
};

int spawn_cast_creature(CaptureSim& sim, const NpcRunState& st, const NpcCase& c) {
    if (sim.creature) {
        sim.w.registry.remove(sim.creature);
        sim.cw.forget(sim.creature);
    }
    std::string path = st.wolf_path;
    if (std::strlen(c.archetype) > 0 && st.motion.rows().count(c.archetype)) path = c.archetype;
    const int npc_level = std::max(1, c.level);
    double distance_cm = 300;
    if (const abilities::AbilityDef* d0 = sim.r.catalog->ability(c.ability)) {
        if (const content::ExprRow* e = sim.r.catalog->expr(d0->row.requirement_expr)) {
            static const std::regex re(R"(GetDistanceBetween\(GetInstigator\(\), GetTarget\(\)\)\s*>=\s*([0-9.]+))");
            for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), re); it != std::sregex_iterator(); ++it) distance_cm = std::max(distance_cm, std::stod((*it)[1]) + 150.0);
            distance_cm = std::min(distance_cm, 1200.0);
        }
    }
    sim.creature = sim.w.spawn(path, distance_cm, 0, npc_level, 1.0);
    return npc_level;
}

void replay_client_sends(CaptureSim& sim, CastEval& ev, int32_t trigger_id, Refusal refusal, bool verbose) {
    const double end_t = ev.t0 + (ev.c.window_ms > 0 ? std::max(ev.c.window_ms / 1000.0, 0.8) : 6.0);
    std::vector<std::pair<double, const RItem*>> sends;
    for (const RItem& q : ev.client) sends.emplace_back(ev.has_ts ? ev.t0 + q.ms / 1000.0 : ev.t0 + 0.3, &q);
    std::stable_sort(sends.begin(), sends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [t, q] : sends) {
        if (refusal != Refusal::None) break;
        if (q->kind == "REQ") {
            sim.advance(t);
            sim.r.clock = std::max(sim.r.clock, t);
            sim.r.a7->tick(t);
            const size_t components = static_cast<size_t>(to_i(q->f[3])), own = static_cast<size_t>(to_i(q->f[4]));
            const abilities::HitDef* hd = sim.r.catalog->hit(to_i(q->f[2]));
            const bool harms = hd && (hd->row.applicable_targets & 2) != 0;
            std::vector<uint64_t> targets;
            size_t own_used = own;
            if (components > own) {
                if (harms) targets = {sim.creature};
                else own_used = components;
            }
            const auto verdict = sim.r.pipe->hit_request(to_i(q->f[0]), to_i(q->f[1]), to_i(q->f[2]), components, own_used, t, targets);
            if (verbose) std::printf("      request at %.3f: %s\n", t - ev.t0, AbilityPipeline::verdict_name(verdict));
            sim.r.take();
        } else if (q->kind == "ENDREQ") {
            sim.advance(t);
            int32_t sid = -1;
            for (const auto& [k, v] : sim.r.pawn.abilities)
                if (v.ability_guid == ev.c.ability) sid = static_cast<int32_t>(k);
            if (sid >= 0) sim.r.pipe->end_request(sid, t);
            sim.r.take();
        } else if (q->kind == "HELD") {
            sim.advance(t);
            sim.r.pipe->cancel(trigger_id, t);
            sim.r.take();
        } else if (q->kind == "BRANCH") {
            sim.advance(t);
            sim.r.pipe->branch_request(trigger_id, to_i(q->f[0]), t);
            sim.r.take();
        }
    }
    sim.advance(end_t);
}

void check_cast_ability_and_cd(CastEval& ev) {
    const bootstrap::ActiveAbilityItem* my_item = nullptr;
    double my_item_at = 0;
    for (const Seen* e : ev.events) {
        if (!my_item && !e->change.abilities_added.empty()) {
            my_item = &e->change.abilities_added[0];
            my_item_at = e->at - ev.t0;
        }
    }
    for (const RItem& it : ev.reply) {
        if (it.kind != "AB" || to_i(it.f[0]) != ev.c.ability || to_u(it.f[3]) == 0) continue;
        if (!my_item) { ev.problem("AB:missing"); break; }
        if (my_item->ability_guid != ev.c.ability || my_item->montage_group != it.f[2]) ev.problem("AB:group " + it.f[2] + " (" + my_item->montage_group + ")");
        if (my_item->montage_guid != to_u(it.f[3])) ev.problem("AB:montage");
        const bool mine_has_target = my_item->targets_self || !my_item->target_objects.empty();
        if (mine_has_target != (to_i(it.f[5]) > 0)) ev.problem("AB:targets");
        if (ev.has_ts && std::fabs(my_item_at - it.ms / 1000.0) > kItemTolerance) ev.late.push_back("AB " + std::to_string(static_cast<int>(my_item_at * 1000)) + "/" + std::to_string(it.ms));
        break;
    }
    const bootstrap::CooldownItem* my_cd = nullptr;
    double my_cd_at = -1;
    for (const Seen* e : ev.events) {
        for (const auto& cd : e->change.cooldowns_set) {
            if (!my_cd && cd.ability_guid == ev.c.ability) {
                my_cd = &cd;
                my_cd_at = e->at - ev.t0;
            }
        }
    }
    for (const RItem& it : ev.reply) {
        if (it.kind != "CD" || to_i(it.f[0]) != ev.c.ability) continue;
        if (!my_cd) { ev.problem("CD:missing"); break; }
        if (my_cd->initial_cooldown != static_cast<float>(to_d(it.f[1]))) ev.problem("CD:cooldown " + it.f[1]);
        if (my_cd->charges != to_i(it.f[2])) ev.problem("CD:charges " + it.f[2] + " (" + std::to_string(my_cd->charges) + ")");
        if (ev.has_ts && std::fabs(my_cd_at - it.ms / 1000.0) > kStepTolerance) ev.late.push_back("CD " + std::to_string(static_cast<int>(my_cd_at * 1000)) + "/" + std::to_string(it.ms));
        break;
    }
}

void check_cast_mana_and_effects(Rig& r, CastEval& ev) {
    int cd_ms = -1;
    for (const RItem& it : ev.reply)
        if (it.kind == "CD" && to_i(it.f[0]) == ev.c.ability) cd_ms = it.ms;
    double retail_before = ev.before.count(content::kGuidMana) ? ev.before.at(content::kGuidMana) : -1;
    const RItem* step = nullptr;
    for (const RItem& it : ev.reply) {
        if (it.kind != "STAT" || to_i(it.f[1]) != content::kGuidMana) continue;
        if (cd_ms >= 0 && it.ms == cd_ms) { step = &it; break; }
        if (cd_ms < 0 || it.ms < cd_ms) retail_before = to_d(it.f[2]);
    }
    if (step && retail_before >= 0) {
        const double retail_delta = retail_before - to_d(step->f[2]);
        double my_after = ev.mana_before;
        for (const Seen* e : ev.events) {
            if (e->change.cooldowns_set.empty()) continue;
            for (const auto& s : e->change.stats)
                if (s.record == content::kGuidMana) my_after = s.value;
            break;
        }
        if (retail_delta > 1 && std::fabs((ev.mana_before - my_after) - retail_delta) > 1e-3)
            ev.problem("COST:mana " + std::to_string(retail_delta) + " (" + std::to_string(ev.mana_before - my_after) + ")");
    }
    std::set<int64_t> first_seen;
    for (const RItem& it : ev.reply) {
        if (it.kind != "EFF") continue;
        const int64_t guid = to_i(it.f[0]);
        if (!ev.reach_effects.count(guid) || !first_seen.insert(guid).second) continue;
        const abilities::EffectDef* ed = r.catalog->effect(guid);
        const std::string name = ed ? ed->row.name : std::to_string(guid);
        bool produced = false, same = false, timely = false;
        for (const Seen* e : ev.events) {
            for (const auto* list : {&e->change.effects_added, &e->change.effects_changed}) {
                for (const auto& x : *list) {
                    if (x.effect_guid != guid) continue;
                    produced = true;
                    const bool inf = to_d(it.f[1]) > 1e30;
                    const bool extends = ed && ed->row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
                    const bool same_here = (inf ? x.duration > 1e30f : std::fabs(x.duration - to_d(it.f[1])) < (extends ? 0.2 : 1e-3)) &&
                                           std::fabs(x.tick_time - to_d(it.f[2])) < 1e-4 && x.stacks == to_i(it.f[3]);
                    if (same_here) {
                        same = true;
                        if (!ev.has_ts || std::fabs((e->at - ev.t0) * 1000 - it.ms) <= kEffectTolerance * 1000) timely = true;
                    }
                }
            }
        }
        if (!produced) ev.problem("EFF:missing " + name);
        else if (!same) ev.problem("EFF:differs " + name + " " + it.f[1] + "/" + it.f[2] + "/" + it.f[3]);
        else if (!timely) ev.late.push_back("EFF " + name);
    }
}

void compare_hit_target_mods(const RItem& rf, const bootstrap::HitConfirm& mf, const std::string& hn, Tally& ty, CastEval& ev) {
    ++ty.hit_cases;
    if (mf.targets.empty()) {
        ev.problem("HIT:nobody " + hn);
        return;
    }
    ++ty.hit_landed;
    std::multiset<int64_t> retail_mods, my_mods;
    double retail_sum = 0, my_sum = 0;
    for (size_t k = 7; k < rf.f.size(); ++k) {
        const std::string& m = rf.f[k];
        const size_t a = m.find(':'), b = m.find(':', a + 1);
        retail_mods.insert(std::stoll(m.substr(0, a)));
        retail_sum += std::bit_cast<float>(static_cast<uint32_t>(std::stoul(m.substr(a + 1, b - a - 1), nullptr, 16)));
    }
    for (const auto& tg : mf.targets) {
        for (const auto& m : tg.mods) {
            my_mods.insert(m.stat_mod_guid);
            my_sum += m.value;
        }
    }
    const std::set<int64_t> rs(retail_mods.begin(), retail_mods.end()), ms(my_mods.begin(), my_mods.end());
    if (rs.empty() || ms.empty()) return;
    if (rs == ms) {
        ++ty.mods_same;
        if (retail_sum != 0 && my_sum != 0 && retail_mods.size() == my_mods.size() && my_sum / retail_sum > 0) ty.ratios.push_back(my_sum / retail_sum);
    } else {
        ++ty.mods_diff;
        ev.problem("HIT:mods " + hn);
    }
}

void check_cast_confirmations(Rig& r, Tally& ty, CastEval& ev) {
    std::map<int64_t, int> retail_conf, my_conf;
    std::map<int64_t, const RItem*> retail_first;
    for (const RItem& it : ev.reply) {
        if (it.kind != "CONF") continue;
        ++retail_conf[to_i(it.f[1])];
        if (!retail_first.count(to_i(it.f[1]))) retail_first[to_i(it.f[1])] = &it;
        else if (to_i(it.f[6]) > 0 && to_i(retail_first[to_i(it.f[1])]->f[6]) == 0) retail_first[to_i(it.f[1])] = &it;
    }
    std::map<int64_t, const bootstrap::HitConfirm*> my_first;
    for (const Seen* e : ev.events) {
        for (const auto& h : e->confirmations) {
            if (!ev.reach_hits.count(h.record_guid)) continue;
            ++my_conf[h.record_guid];
            if (!my_first.count(h.record_guid) || (!h.targets.empty() && my_first[h.record_guid]->targets.empty())) my_first[h.record_guid] = &h;
        }
    }
    for (const auto& [rec, n] : retail_conf) {
        if (!ev.reach_hits.count(rec)) continue;
        const int mine = my_conf.count(rec) ? my_conf[rec] : 0;
        const abilities::HitDef* hd = r.catalog->hit(rec);
        const std::string hn = hd ? hd->row.name : std::to_string(rec);
        if (mine == 0) ev.problem("CONF:missing " + hn);
        else if (mine < n && n <= 3) ev.problem("CONF:fewer " + hn + " " + std::to_string(mine) + "/" + std::to_string(n));
        const RItem* rf = retail_first.at(rec);
        if (static_cast<long>(to_i(rf->f[6])) > 0 && mine > 0) compare_hit_target_mods(*rf, *my_first[rec], hn, ty, ev);
    }
    for (const auto& [rec, n] : my_conf) {
        if (!retail_conf.count(rec)) {
            const abilities::HitDef* hd = r.catalog->hit(rec);
            ev.problem("CONF:extra " + (hd ? hd->row.name : std::to_string(rec)));
        }
    }
    int retail_n = -1;
    for (const RItem& it : ev.reply)
        if (it.kind == "SHIELD") retail_n = static_cast<int>(to_i(it.f[0]));
    if (retail_n > 0 && r.pawn.shields.empty()) ev.problem("SHIELD:missing");
    for (const RItem& it : ev.reply) {
        if (it.kind == "END") {
            bool mine = false;
            for (const Seen* e : ev.events) mine = mine || (e->end && *e->end == to_i(it.f[0]));
            if (!mine) ev.problem("END:missing");
            break;
        }
    }
}

void record_cast_verdict(CaptureSim& sim, NpcRunState& st, CastEval& ev, Refusal refusal, int npc_level) {
    Tally& ty = st.tally[ev.c.cls];
    std::vector<std::string> unlisted;
    for (const std::string& p : ev.problems) {
        bool listed = false;
        for (const Known& k : kKnown) listed = listed || (st.use_known && std::string(k.ability) == ev.c.name && k.capture == ev.c.capture && p.rfind(k.tag, 0) == 0);
        if (!listed) unlisted.push_back(p);
    }
    if (refusal != Refusal::None && unlisted.empty()) ++ty.refused_listed;
    if (unlisted.empty()) ++ty.matched;
    if (ev.problems.empty()) ++ty.exact;
    if (unlisted.empty() && ev.late.empty()) ++ty.timed;
    for (const std::string& p : unlisted) {
        const size_t sp = p.find(' ');
        const std::string head = sp == std::string::npos ? p : p.substr(0, sp);
        ++st.reasons[head + " [" + ev.c.cls + "]"];
        st.reason_cases[head + " [" + ev.c.cls + "]"].push_back(std::string(ev.c.name) + "@" + std::to_string(ev.c.capture) + ":" + std::to_string(ev.c.line) + " " + p);
    }
    if (st.verbose || (!unlisted.empty() && !st.only_ability.empty())) {
        std::printf("%s %s cap %d line %d (%s): %s  [the creature: level %d, Health %.1f of %.1f after the cast]", unlisted.empty() ? "ok  " : "FAIL", ev.c.name, ev.c.capture, ev.c.line,
                    ev.c.target_kind, unlisted.empty() ? "matches" : "", npc_level, sim.cw.health_of(sim.creature), sim.cw.max_health_of(sim.creature));
        for (const std::string& p : unlisted) std::printf("\n      %s", p.c_str());
        for (const std::string& p : ev.late) std::printf("\n      late %s", p.c_str());
        std::printf("\n");
    }
}

void run_single_npc_cast(CaptureSim& sim, NpcRunState& st, const NpcCase& c) {
    Tally& ty = st.tally[c.cls];
    ++ty.total;
    ++st.kinds[c.target_kind];
    CastEval ev{c};
    ev.has_ts = c.at_ms >= 0;
    if (ev.has_ts) ++ty.with_ts;
    ev.reply = parse_lines(c.reply);
    ev.client = parse_lines(c.client);
    ev.before = parse_before(c.before);
    ev.t0 = ev.has_ts ? 5.0 + c.at_ms / 1000.0 + 0.0123 : (sim.clock_no_ts += 15.0);
    const int npc_level = spawn_cast_creature(sim, st, c);
    if (!sim.creature) {
        std::printf("  [FAIL] no creature for %s (capture %d)\n", c.name, c.capture);
        ++g_fail;
        return;
    }
    sim.advance(ev.t0 - 0.2);
    sim.r.set_current(content::kGuidMana, sim.r.pawn.stat(content::kGuidMaxMana));
    sim.r.set_current(content::kGuidHealth, sim.r.pawn.stat(content::kGuidMaxHealth));
    sim.r.take();
    for (const int64_t guid : content::kResourceStats) {
        const auto b = ev.before.find(guid);
        if (b != ev.before.end()) sim.r.set_current(guid, b->second);
    }
    sim.r.take();
    sim.advance(ev.t0 - 0.001);
    ev.mana_before = sim.r.pawn.stat(content::kGuidMana);
    const size_t first_event = sim.r.log.size();
    ev.def = sim.r.catalog->ability(c.ability);
    const bool enemy_ok = ev.def && (ev.def->row.applicable_targets & 2) != 0;
    const bool ally_ok = ev.def && (ev.def->row.applicable_targets & (1 | 4 | 8 | 16)) != 0;
    const bool names_target = c.targets > 0;
    const bool aim_at_creature = names_target && enemy_ok && (std::strcmp(c.target_kind, "npc") == 0 || !ally_ok);
    TriggerInfo info; info.animation_index = c.animation_index; info.target_count = static_cast<uint32_t>(c.targets); info.item = false;
    if (aim_at_creature) info.target_actors = {sim.creature};
    (aim_at_creature ? st.at_npc : (names_target ? st.at_self : st.at_air)) += 1;
    int32_t trigger_id = c.client_instance_id;
    Refusal refusal = sim.r.pipe->trigger(c.ability, trigger_id, ev.t0, info);
    sim.r.take();
    if (refusal == Refusal::Requirement && ev.def && grant_states(sim.r, *ev.def, ev.t0) > 0) {
        sim.r.pipe->refresh(ev.t0, "the state before the capture", true);
        sim.r.take();
        ++st.states_granted;
        trigger_id = c.client_instance_id + 500000;
        refusal = sim.r.pipe->trigger(c.ability, trigger_id, ev.t0 + 0.0005, info);
        sim.r.take();
    }
    replay_client_sends(sim, ev, trigger_id, refusal, st.verbose);
    if (refusal != Refusal::None) ev.problem(std::string("refused:") + refusal_name(refusal));
    else ++ty.accepted;
    if (!sim.r.pawn.broken.empty()) ev.problem("pawn:" + sim.r.pawn.broken);
    if (refusal == Refusal::None) {
        for (size_t i = first_event; i < sim.r.log.size(); ++i) ev.events.push_back(&sim.r.log[i]);
        if (ev.def) closure(*sim.r.catalog, *ev.def, ev.reach_effects, ev.reach_hits);
        check_cast_ability_and_cd(ev);
        check_cast_mana_and_effects(sim.r, ev);
        check_cast_confirmations(sim.r, ty, ev);
    }
    record_cast_verdict(sim, st, ev, refusal, npc_level);
}

void run_capture_cases(int capture, NpcRunState& st) {
    std::string rig_cls;
    int level = 0;
    std::vector<int64_t> known;
    for (const auto& c : capture_npc_casts::kCases) {
        if (c.capture != capture) continue;
        rig_cls = c.rig_cls;
        level = std::max(level, c.level);
        known.push_back(c.ability);
    }
    if (level == 0) level = 25;
    if (!st.only_class.empty() && rig_cls != st.only_class) return;
    if (st.only_capture != 0 && st.only_capture != capture) return;
    if (!st.only_ability.empty()) {
        bool has = false;
        for (const auto& c : capture_npc_casts::kCases) has = has || (c.capture == capture && st.only_ability == c.name);
        if (!has) return;
    }
    Rig r(class_enum(rig_cls), level);
    if (!r.ok()) {
        std::printf("  [FAIL] the rig of capture %d (%s %d): %s\n", capture, rig_cls.c_str(), level, r.problem.c_str());
        ++g_fail;
        return;
    }
    r.pipe->set_known(known);
    r.pipe->set_data_timings(true);
    r.take();
    r.use_extra_ranks(infer_talents(r, capture));
    std::string error;
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    if (!data) { std::printf("  [FAIL] the combat data of capture %d: %s\n", capture, error.c_str()); ++g_fail; return; }
    combat_rig::World w;
    if (!w.problem.empty()) { std::printf("  [FAIL] the world of capture %d: %s\n", capture, w.problem.c_str()); ++g_fail; return; }
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, st.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    combat::WorldHitPort port(cw, w.registry, player.fighter());
    r.pipe->set_combat(&port);
    CaptureSim sim{r, w, cw};
    std::vector<const NpcCase*> in_order;
    for (const NpcCase& x : capture_npc_casts::kCases)
        if (x.capture == capture) in_order.push_back(&x);
    std::stable_sort(in_order.begin(), in_order.end(), [](const NpcCase* a, const NpcCase* b) { return a->line < b->line; });
    for (const NpcCase* cp : in_order) {
        if (!st.only_ability.empty() && st.only_ability != cp->name) continue;
        run_single_npc_cast(sim, st, *cp);
    }
}

void print_ability_npc_report(const NpcRunState& st, size_t n_cases) {
    std::printf("\n%-10s %7s %9s %9s %9s %9s %9s %9s %9s\n", "class", "casts", "accepted", "listed ok", "exact", "on time", "with ts", "hits", "landed");
    size_t total = 0, accepted = 0, matched = 0, exact = 0, refused_listed = 0;
    std::vector<double> all_ratios;
    for (const auto& [cls, t] : st.tally) {
        std::printf("%-10s %7zu %9zu %9zu %9zu %9zu %9zu %9zu %9zu\n", cls.c_str(), t.total, t.accepted, t.matched, t.exact, t.timed, t.with_ts, t.hit_cases, t.hit_landed);
        total += t.total;
        exact += t.exact;
        accepted += t.accepted;
        matched += t.matched;
        refused_listed += t.refused_listed;
        all_ratios.insert(all_ratios.end(), t.ratios.begin(), t.ratios.end());
    }
    std::printf("%-10s %7zu %9zu %9zu %9zu\n", "all", total, accepted, matched, exact);
    std::printf("\n%zu casts were made after the state their requirement names was given to the caster (a stealth, a stack, an enchant, a summoned pet's effect) [modelled]", st.states_granted);
    std::printf("\ncast at: %zu at the creature, %zu at the caster (an ally's place), %zu with no target; retail's targets: ", st.at_npc, st.at_self, st.at_air);
    for (const auto& [k, n] : st.kinds) std::printf("%s %zu  ", k.c_str(), n);
    std::printf("\namounts (ours / retail's, hits that landed on both sides with the same stat mods):\n");
    for (const auto& [cls, t] : st.tally) {
        if (t.ratios.empty()) continue;
        std::vector<double> v = t.ratios;
        std::sort(v.begin(), v.end());
        const auto q = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; };
        std::printf("  %-9s %5zu hits: quartiles %.2f / %.2f / %.2f   (mods the same set: %zu, a different set: %zu)\n", cls.c_str(), v.size(), q(0.25), q(0.5), q(0.75), t.mods_same, t.mods_diff);
    }
    if (!all_ratios.empty()) {
        std::sort(all_ratios.begin(), all_ratios.end());
        std::printf("  all       %5zu hits: quartiles %.2f / %.2f / %.2f\n", all_ratios.size(), all_ratios[all_ratios.size() / 4], all_ratios[all_ratios.size() / 2], all_ratios[all_ratios.size() * 3 / 4]);
    }
    std::printf("\nwhat is left:\n");
    for (const auto& [reason, n] : st.reasons) {
        std::printf("  %5zu  %s\n", n, reason.c_str());
        if (st.verbose) {
            size_t shown = 0;
            for (const std::string& s : st.reason_cases.at(reason)) {
                if (++shown > 6) break;
                std::printf("           %s\n", s.c_str());
            }
        }
    }
    std::printf("%zu casts are refused and listed in kKnown with the reason\n", refused_listed);
    if (st.only_class.empty() && st.only_ability.empty() && st.only_capture == 0) {
        CHECKF(total == n_cases, "every cast of the header was run (%zu of %zu)", total, n_cases);
        CHECKF(accepted + refused_listed == total, "every cast is accepted, or its refusal is listed (%zu + %zu of %zu)", accepted, refused_listed, total);
        CHECKF(matched == total, "every accepted cast matches retail's reply (%zu of %zu)", matched, total);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (!real_db_present() || !std::filesystem::exists(NPC_MOTION_PATH)) {
        std::printf("[skip] no content.db or npc_motion.tsv at %s: the NPC casts' acceptance test is skipped\n", real_db_path().c_str());
        return 0;
    }
    NpcRunState st;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) st.verbose = true;
        else if (!std::strcmp(argv[i], "--no-known")) st.use_known = false;
        else if (!std::strcmp(argv[i], "--class") && i + 1 < argc) st.only_class = argv[++i];
        else if (!std::strcmp(argv[i], "--ability") && i + 1 < argc) st.only_ability = argv[++i];
        else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) st.only_capture = std::atoi(argv[++i]);
    }
    spdlog::set_level(spdlog::level::warn);
    const size_t n_cases = sizeof(capture_npc_casts::kCases) / sizeof(capture_npc_casts::kCases[0]);
    std::printf("the ability runtime against %zu retail casts of every class that need another actor\n", n_cases);
    std::set<int> captures;
    for (const auto& c : capture_npc_casts::kCases) captures.insert(c.capture);

    std::string error;
    if (!st.motion.load(NPC_MOTION_PATH, error)) {
        std::printf("  [FAIL] npc_motion.tsv: %s\n", error.c_str());
        return 2;
    }
    for (const auto& [p, row] : st.motion.rows()) {
        if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) st.wolf_path = p;
    }
    for (const int capture : captures) run_capture_cases(capture, st);
    print_ability_npc_report(st, n_cases);
    run_ability_npc_pets_tests();
    run_ability_npc_pets_cmds_tests();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
