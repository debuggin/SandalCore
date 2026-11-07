// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_world.h"

#include "combat/npc_replicator.h"
#include "spawns/npc_looks.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc { namespace worldd {

namespace {

void log_counts(const char* prefix, std::vector<std::pair<std::string, size_t>> v) {
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::string line;
    for (const auto& [name, n] : v) {
        const std::string item = name + " " + std::to_string(n);
        if (!line.empty() && line.size() + item.size() > 400) {
            spdlog::info("{}{}", prefix, line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + item;
    }
    if (!line.empty()) spdlog::info("{}{}", prefix, line);
}

bool same_text(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::vector<const world::PlotRow*> free_plots(const std::vector<world::PlotRow>& rows) {
    std::vector<const world::PlotRow*> out;
    for (const world::PlotRow& r : rows) {
        if (r.state == 0 && !r.pre_construct) out.push_back(&r);
    }
    return out;
}

std::string joined(const std::vector<std::string>& names, size_t limit) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < limit; ++i) out += (out.empty() ? "" : ", ") + names[i];
    if (names.size() > limit) out += ", ...";
    return out;
}

void collect_quest_objective_targets(const quest::QuestBook& quests, std::set<int64_t>& props, std::set<int64_t>& asked) {
    for (const auto& [guid, q] : quests.data().quests) {
        if (q.disabled || q.flavor == "Destiny" || q.name.rfind("zLegacy", 0) == 0) continue;
        for (const content::QuestObjectiveRow& o : q.objectives) {
            if (o.type == "Interact") props.insert(o.components.begin(), o.components.end());
            if (o.type == "Collect" || o.type == "TurnIn") asked.insert(o.items.begin(), o.items.end());
        }
    }
}

std::set<int64_t> collect_active_quest_props(const quest::QuestBook* quests, const content::ObjectData& objects) {
    std::set<int64_t> props;
    std::set<int64_t> asked;
    if (quests) collect_quest_objective_targets(*quests, props, asked);
    for (const auto& [icd, row] : objects.interactables) {
        for (const auto& [item, quantity] : row.grants) {
            (void)quantity;
            if (asked.count(item) != 0) props.insert(icd);
        }
    }
    for (auto it = props.begin(); it != props.end();) {
        it = objects.interactables.count(*it) != 0 ? std::next(it) : props.erase(it);
    }
    return props;
}

int64_t find_plot_building(const world::NodeWire* wire, const world::CityNodes* nodes, int64_t node_guid, int64_t plot) {
    if (plot == 0 || !wire || !nodes) return 0;
    for (const world::PlotRow& row : wire->plot_rows_of(node_guid, nodes->level_of(node_guid))) {
        if (row.plot == plot) return row.building;
    }
    return 0;
}

bool opens_shop_by_talk(const dialogue::Book* dialogue, const vendor::TownShops& shops, const world::WorldActor& npc) {
    if (!dialogue || npc.npc.dialogue_root == 0) return false;
    for (const int64_t def : dialogue->vendor_defs(npc.npc.dialogue_root)) {
        if (shops.is_open(def)) return true;
    }
    return false;
}

std::vector<int64_t> archetype_ability_kit(const combat::NpcAbilityData& abilities, const std::string& archetype) {
    std::vector<int64_t> kit;
    if (const std::vector<combat::CreatureAbility>* tasks = abilities.abilities_of(archetype)) {
        for (const combat::CreatureAbility& t : *tasks) {
            if (std::find(kit.begin(), kit.end(), t.ability) == kit.end()) kit.push_back(t.ability);
        }
    }
    return kit;
}

void dress_and_pose_respawn(world::WorldActor& w, const spawns::SpawnRecipe& r) {
    if (r.spawn.looks) {
        std::string look_error;
        r.spawn.looks->dress(w, r.spawn, look_error);
    }
    if (std::string idle_error; !spawns::apply_idle_animation(w, r.spawn.instance, idle_error)) {
        spdlog::debug("[looks] {}: {}", r.info.name, idle_error);
    }
}

npc::NpcMotionSetup make_respawn_motion_setup(const world::WorldActor& a, const spawns::AreaReport& report,
                                              const npc::MotionTable& motion, const npc::SpeedModel& speeds) {
    const content::NpcInstanceRow* inst = nullptr;
    const content::NpcRouteRow* route = nullptr;
    if (a.npc.instance_guid != 0) {
        if (const auto it = report.instances.find(a.npc.instance_guid); it != report.instances.end()) inst = &it->second;
        if (inst && inst->patrol_route != 0) {
            if (const auto rt = report.routes.find(inst->patrol_route); rt != report.routes.end()) route = &rt->second;
        }
    }
    npc::NpcMotionSetup setup = npc::make_setup(a, motion, inst, route, speeds);
    if (setup.ambient == npc::Ambient::Patrol && setup.route.size() < 2) setup.ambient = npc::Ambient::Wander;
    return setup;
}

bool contains_nocase(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) != haystack.end();
}

const content::NpcAssetRow* first_instance_asset(const content::PopulationData& pop, const content::NpcInstanceRow& inst, const spawns::TemplateSource& templates) {
    const content::NpcAssetRow* fallback = nullptr;
    for (const int64_t set_guid : inst.asset_sets) {
        const auto st = pop.asset_sets.find(set_guid);
        if (st == pop.asset_sets.end()) continue;
        for (const auto& [asset_guid, weight] : st->second) {
            (void)weight;
            const auto at = pop.assets.find(asset_guid);
            if (at == pop.assets.end()) continue;
            if (!fallback) fallback = &at->second;
            if (templates.find(at->second, nullptr, nullptr, &inst)) return &at->second;
        }
    }
    return fallback;
}

std::string strip_npc_prefix(const std::string& q) {
    for (const char* pfx : {"DesignData_NPC_", "NPC_", "DesignData_"}) {
        const size_t n = std::strlen(pfx);
        if (q.size() > n && same_text(q.substr(0, n), pfx)) return q.substr(n);
    }
    return q;
}

const content::NpcAssetRow* resolve_spawn_npc_asset(const content::PopulationData& pop, const spawns::TemplateSource& templates,
                                                    const std::string& query, const content::NpcInstanceRow*& out_inst) {
    out_inst = nullptr;
    const std::string stem = strip_npc_prefix(query);
    char* end = nullptr;
    if (const long long parsed = std::strtoll(stem.c_str(), &end, 10); end && end != stem.c_str() && *end == '\0' && parsed != 0) {
        const int64_t g = static_cast<int64_t>(parsed);
        if (const auto it = pop.assets.find(g); it != pop.assets.end()) return &it->second;
        if (const auto it = pop.instances.find(g); it != pop.instances.end()) {
            out_inst = &it->second;
            return first_instance_asset(pop, it->second, templates);
        }
        for (const auto& [ag, a] : pop.assets) {
            if (a.interaction_def == g || a.appearance == g || a.dialogue_root == g) return &a;
        }
    }
    const content::NpcAssetRow* fallback = nullptr;
    for (const auto& [guid, a] : pop.assets) {
        const std::string leaf = spawns::blueprint_leaf(a.archetype_path), ref_leaf = spawns::blueprint_leaf(a.asset_ref);
        if (same_text(a.name, query) || same_text(a.name, stem) || same_text(leaf, query) || same_text(leaf, stem) || same_text(ref_leaf, query) || same_text(ref_leaf, stem)) {
            if (!fallback) fallback = &a;
            if (templates.find(a)) return &a;
        }
    }
    for (const auto& [guid, inst] : pop.instances) {
        if (!same_text(inst.name, query) && !same_text(inst.name, stem) && !contains_nocase(inst.name, stem)) continue;
        if (const content::NpcAssetRow* a = first_instance_asset(pop, inst, templates)) {
            out_inst = &inst;
            if (templates.find(*a, nullptr, nullptr, &inst)) return a;
            if (!fallback) fallback = a;
        }
    }
    for (const auto& [guid, a] : pop.assets) {
        if (contains_nocase(a.name, query) || contains_nocase(a.name, stem) || contains_nocase(a.archetype_path, query) || contains_nocase(a.archetype_path, stem)) {
            if (!fallback) fallback = &a;
            if (!a.dev_only && templates.find(a)) return &a;
        }
    }
    for (const auto& [guid, a] : pop.assets) {
        if (!a.name.empty()) {
            if (!fallback) fallback = &a;
            if (!a.dev_only && templates.find(a)) return &a;
        }
    }
    return fallback;
}

}  // namespace

void PlayerWorld::load_area_quests_and_props() {
    std::string de;
    if (auto data = content_->dialogue_data(de)) {
        dialogue_.emplace(std::move(*data));
        spdlog::info("[world] conversations: {} nodes of {} roots, {} quests named by conditions", dialogue_->nodes(), dialogue_->roots(), dialogue_->quests());
    } else {
        spdlog::warn("[world] the NPC conversations do not load, so the NPCs that have one answer as before: {}", de);
    }
    std::string qe;
    if (auto data = content_->quest_data(qe)) {
        if (dialogue_) {
            dialogue_->set_accepts(data->accepts);
            dialogue_->set_confirms(data->confirms);
            dialogue_->set_turn_ins(data->turn_ins);
        }
        quests_.emplace(std::move(*data));
        spdlog::info("[world] quests: {} quests, {} nodes that offer one, {} zones that start one", quests_->quests(), quests_->data().accepts.size(), quests_->data().zoi_quests.size());
    } else {
        spdlog::warn("[world] the quests do not load, so no NPC offers one: {}", qe);
    }
    std::string oe;
    if (auto data = content_->object_data(oe)) {
        objects_.emplace(std::move(*data));
        // Disabled, Destiny, and zLegacy quest objectives are excluded so starter-gear sparkles do not spawn prematurely.
        generic_config_.prop_components = collect_active_quest_props(quests_ ? &*quests_ : nullptr, *objects_);
        spdlog::info("[world] quest objects: {} interactable definitions ({} are quest objects), {} actor definitions, {} quest items, {} reward boxes", objects_->interactables.size(),
                     generic_config_.prop_components.size(), objects_->actor_defs.size(), objects_->quest_items.size(), objects_->item_boxes.size());
    } else {
        spdlog::warn("[world] the quest objects do not load, so none is made: {}", oe);
    }
}

void PlayerWorld::load_area_economy_and_looks() {
    std::string ve;
    if (auto data = content_->vendor_data(ve)) {
        shops_.emplace(std::move(*data));
        spdlog::info("[world] shops: {} vendor defs, {} NPC links, {} priced listings; open: {}", shops_->defs(), shops_->links(), shops_->priced_rows(), shops_->open_shops().size());
    } else {
        spdlog::warn("[world] the town shops do not load, so the shopkeepers answer as before: {}", ve);
    }
    std::string we;
    if (auto data = content_->warehouse_data(we)) {
        warehouses_.emplace(std::move(*data));
        spdlog::info("[world] warehouses: {} towns, {} item chunks, {} tabs", warehouses_->nodes(), warehouses_->chunks(), warehouses_->tabs());
    } else {
        spdlog::warn("[world] the warehouses do not load, so the storage clerks answer as before: {}", we);
    }
    load_crafting_data();
    if (generic_config_.hunting && generic_config_.hunting_defs.empty()) {
        std::string fe;
        if (const auto foliage = content_->foliage_data(fe)) {
            generic_config_.hunting_defs = gather::hunting_defs(*foliage);
            generic_config_.hunting_names = gather::hunting_names(*foliage);
        }
    }
    generic_config_.abilities_of = [this](const std::string& archetype) {
        return archetype_ability_kit(npc_abilities_, archetype);
    };
    const npc::MotionTable* motion = motion_loaded_ ? &motion_ : nullptr;
    generic_.emplace(*captured_, motion, generic_config_);
    std::string le;
    std::string ce;
    const auto combat_rows = content_->combat_rows(ce);
    if (!combat_rows) le = "the combat tables are missing, so it is not known who is hostile to players: " + ce;
    looks_ = looks_dir_.empty() || !combat_rows ? nullptr : spawns::NpcLooks::load(looks_dir_, motion, &*combat_rows, &*captured_, le);
    if (looks_) {
        spdlog::info("[looks] {}", looks_->describe());
        spawn_context_.looks_ok = [this](const content::NpcAssetRow& a) { return looks_ && looks_->can_dress(a); };
    } else {
        spdlog::warn("[looks] the humanoids keep their recorded looks and names, and those no capture opened stay out ({}): run scripts/extract_game_data.ps1", le);
    }
}

void PlayerWorld::log_area_loaded() const {
    if (looks_) {
        const auto& lc = looks_->counts();
        spdlog::info("[looks] the area's people: {} dressed ({} customizations from the recorded pool, {} from a record's own preset, {} of them given a different race or gender than they would have had, {} plain), {} named by the data, {} by their Blueprint, {} with no name, {} left as recorded",
                     lc.dressed, lc.from_pool, lc.from_preset, lc.preset_body, lc.plain_customization, lc.named, lc.blueprint_named, lc.unnamed, lc.no_record);
    }
    for (const std::string& n : area_report_->notes) spdlog::warn("[area] {}", n);
    log_spawn_filter();
    log_npc_types();
    log_huntables();
    if (area_.region) {
        std::string line;
        for (size_t z = 0; z < spawns::region::kZoiCount; ++z) {
            const auto it = area_report_->population.by_zoi.find(spawns::region::kZois[z].guid);
            line += (line.empty() ? "" : ", ") + std::string(spawns::region::kZois[z].name) + " " + std::to_string(it == area_report_->population.by_zoi.end() ? 0 : it->second);
        }
        spdlog::info("[area] data NPCs by zone of influence: {}", line);
    }
    spdlog::info("[area] GM vendors: {} PTR vendor defs, {} item records in their stock; of the {} GM/PTR item records, {} are in a stock and {} are in none",
                 gm_->vendors().size(), gm_->stocked_records(), gm_->gm_item_count(), gm_->gm_items_stocked(), gm_->unstocked().size());
    if (!gm_->mount_error().empty()) {
        spdlog::warn("[area] the GM mounts vendor sells only what its def lists: {}", gm_->mount_error());
    } else {
        spdlog::info("[area] the GM mounts vendor sells all {} mount items ({} added to the {} its def lists)", gm_->mounts_total(), gm_->mounts_added(), gm_->mounts_total() - gm_->mounts_added());
    }
    std::string legend;
    const auto& defs = gm_->vendors();
    for (size_t i = 0; i < defs.size(); ++i) {
        legend += (i == 0 ? "" : "; ") + defs[i].label + " (" + (i < 12 ? "inner" : "outer") + " arc #" + std::to_string(i < 12 ? i + 1 : i - 11) + ", " + std::to_string(defs[i].items.size()) + " items)";
    }
    spdlog::info("[area] GM vendor legend, left to right along each arc: {}", legend);
}

bool PlayerWorld::load_area(const spawns::AreaConfig& area, std::string& error) {
    if (!content_) {
        error = "content.db is not loaded: " + content_problem_;
        return false;
    }
    if (area_report_) {
        error = "the area is loaded already";
        return false;
    }
    std::string e;
    auto captured = spawns::CapturedData::load(e);
    if (!captured) {
        error = "the captured spawns do not load: " + e;
        return false;
    }
    auto gm = vendor::GmVendors::load(*content_, e);
    if (!gm) {
        error = "the GM vendors do not load: " + e;
        return false;
    }
    captured_ = std::move(*captured);
    gm_ = std::move(*gm);
    load_area_quests_and_props();
    load_area_economy_and_looks();

    spawns::AreaSources sources;
    sources.db = content_;
    sources.captured = &*captured_;
    sources.gm = &*gm_;
    sources.generic = &*generic_;
    sources.looks = looks_.get();
    sources.objects = objects_ ? &*objects_ : nullptr;
    spawn_filter_.emplace(spawn_context_, motion_loaded_ ? &motion_ : nullptr);
    sources.filter = &*spawn_filter_;
    area_ = area;
    auto report = spawns::load_area(registry_, sources, area, [this] { return guids_.next(); }, server_id_, 0.0, e);
    if (!report) {
        error = "the area does not load: " + e;
        return false;
    }
    area_report_ = std::move(*report);
    spdlog::info("[area] {}", area_report_->describe());
    prop_slots_.assign(area_report_->props.size(), PropSlot{});
    for (size_t i = 0; i < prop_slots_.size(); ++i) prop_slots_[i].actor = area_report_->props[i].actor;
    if (!area_report_->props.empty()) spdlog::info("[area] quest objects: {} made (from {} recorded template(s))", area_report_->props.size(), generic_ ? generic_->props_made() : 0);
    log_area_loaded();
    return true;
}

void PlayerWorld::log_spawn_filter() const {
    const spawns::PopulationStats& pop = area_report_->population;
    const spawns::SpawnContext& ctx = spawn_filter_->context();
    if (!ctx.enabled) {
        spdlog::info("[area] spawn filter: OFF (--no-spawn-filter): every point whose Blueprint opens spawns, as in stage 2");
        return;
    }
    spdlog::info("[area] spawn filter: map {}, node level {}, culture {}, season {}, hour {:.1f}, {} gate source(s) running, humanoids {}: {} points dropped, {} spawned",
                 ctx.map_name, ctx.node_level, ctx.culture, ctx.season, ctx.hour, ctx.active_gates.size(),
                 ctx.humanoids ? "all" : (ctx.looks_ok ? "the ones not hostile to players, dressed from the data" : "only Guard_Soldier"), pop.filtered_total(), pop.spawned);
    std::vector<std::pair<spawns::Why, size_t>> reasons(pop.filtered.begin(), pop.filtered.end());
    std::sort(reasons.begin(), reasons.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    for (const auto& [why, n] : reasons) {
        spdlog::info("[area] spawn filter: {} points: {}", n, spawns::why_text(why));
        if (const auto it = pop.filtered_by_blueprint.find(why); it != pop.filtered_by_blueprint.end()) {
            log_counts("[area] ...   Blueprints: ", {it->second.begin(), it->second.end()});
        }
    }
}

void PlayerWorld::log_huntables() const {
    const std::set<int64_t>& defs = generic_config_.hunting_defs;
    if (!generic_config_.hunting) {
        spdlog::info("[hunt] --no-gathering: no mob is huntable");
        return;
    }
    if (defs.empty()) {
        spdlog::info("[hunt] no huntable mobs: content.db has no hunting interactions (rebuild it with contentc 15 or later)");
        return;
    }
    std::map<int64_t, size_t> by_def;
    size_t total = 0;
    registry_.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Npc && a.npc.interact_def != 0 && defs.count(a.npc.interact_def)) {
            ++by_def[a.npc.interact_def];
            ++total;
        }
    });
    std::map<int64_t, std::string> names;
    std::string names_error;
    if (const auto foliage = content_ ? content_->foliage_data(names_error) : std::nullopt) {
        for (const int64_t d : defs) {
            const auto it = foliage->interaction_defs.find(d);
            if (it != foliage->interaction_defs.end()) names[d] = it->second.name;
        }
    }
    std::vector<std::pair<std::string, size_t>> counts;
    for (const auto& [def, n] : by_def) counts.emplace_back(names.count(def) ? names[def] : std::to_string(def), n);
    spdlog::info("[hunt] huntable mobs in the area: {} of {} NPCs ({} kinds, eight blocks with the hunting interactable{}); a Bow in Equipment slot 38 hunts a corpse for a carcass or a trainable and Hunter XP",
                 total, registry_.count(world::ActorKind::Npc), by_def.size(), generic_ && !generic_->has_hunting_block() ? "; NOT: the Raven's captured open is missing, so they are plain creatures" : "");
    log_counts("[hunt] by kind: ", counts);
}

void PlayerWorld::log_npc_types() const {
    const spawns::PopulationStats& pop = area_report_->population;
    const spawns::GenericNpcs* g = generic_ ? &*generic_ : nullptr;
    if (g) {
        spdlog::info("[area] generic NPCs ({}): donor {}; {} Blueprint types made, {} NPCs; the exports carry checksum {} (the client does not compare 0); kill switch --npc-exclude: {} name(s)",
                     g->config().enabled ? "on" : "off", g->donor().empty() ? "none" : g->donor(), g->made(), pop.generic_spawned, g->config().export_checksum,
                     g->config().exclude.size());
        if (g->talker_interactable().empty()) {
            spdlog::warn("[area] the people who talk keep the recorded citizen's interactable (a crate vendor's: they offer to buy crates): the captures hold no plain conversation");
        } else {
            spdlog::info("[area] the people who talk are given the interactable of a plain conversation (recorded from {})", g->talker_interactable());
        }
    }
    std::string lost;
    for (const auto& [why, n] : pop.lost_by_verdict) lost += (lost.empty() ? "" : ", ") + std::string(spawns::verdict_name(why)) + " " + std::to_string(n);
    spdlog::info("[area] points that no NPC could be made for: {} ({})", pop.no_template, lost.empty() ? "none" : lost);
    if (!pop.lost_props.empty()) log_counts("[area] ... of which a Blueprint that is no pawn (a prop): ", {pop.lost_props.begin(), pop.lost_props.end()});
    if (!pop.lost_excluded.empty()) log_counts("[area] ... of which left out by --npc-exclude: ", {pop.lost_excluded.begin(), pop.lost_excluded.end()});
    log_counts("[area] NPCs by type: ", {area_report_->npcs_by_archetype.begin(), area_report_->npcs_by_archetype.end()});
    if (!pop.generic_by_archetype.empty()) log_counts("[area] ... made from a generic Blueprint: ", {pop.generic_by_archetype.begin(), pop.generic_by_archetype.end()});
    if (!area_report_->props.empty()) {
        std::map<std::string, size_t> by;
        for (const spawns::PropRecipe& r : area_report_->props) ++by[spawns::blueprint_leaf(r.open->archetype_path)];
        log_counts("[area] quest objects by Blueprint: ", {by.begin(), by.end()});
    }
}

void PlayerWorld::finish_station(uint64_t id, const world::CityNodeInfo& node, int64_t plot, int64_t building) {
    describe_station(id, node, plot, building);
    if (!npcs_) return;
    if (const world::WorldActor* placed = registry_.find(id)) {
        npc::NpcMotionSetup post;
        post.ambient = npc::Ambient::Stationary;
        post.foot_offset = 0.0;
        post.home_yaw = placed->yaw_degrees;
        npcs_->add(id, post);
    }
}

void PlayerWorld::describe_station(uint64_t id, const world::CityNodeInfo& node, int64_t plot, int64_t building) {
    registry_.modify(id, world::Change::State, [&](world::WorldActor& w) {
        w.station.node = node.guid;
        w.station.plot = plot;
        w.station.asset = w.npc.asset_guid;
        w.station.building = building != 0 ? building : find_plot_building(node_wire_, city_nodes_, node.guid, plot);
        if (stations_) {
            if (const content::CraftStationDef* d = stations_->def_of_asset(w.npc.asset_guid)) {
                w.station.def = d->guid;
                w.station.tax_tag = d->tax_tag;
                w.station.processing = d->processing;
                w.station.tier = d->max_tier >= 0 ? d->max_tier + 1 : 0;
            }
        }
        for (protocol::repl::Subobject& s : w.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCNodePlotComponent") continue;
            s.obj.state.set(*s.obj.schema, "NodeRecordGuid", protocol::repl::Value::i64(node.guid));
            s.obj.state.set(*s.obj.schema, "PlotRecordGuid", protocol::repl::Value::i64(plot));
            s.obj.state.set(*s.obj.schema, "BuildingRecordGuid", protocol::repl::Value::i64(w.station.building));
        }
    });
}

const world::PlotRow* PlayerWorld::find_free_station_plot(const world::CityNodeInfo& node, int level, const std::string& plot_name,
                                                          const std::vector<world::PlotRow>& rows, StationResult& out) const {
    if (level < 3) {
        out.status = StationStatus::Refused;
        out.message = node.display_name + " is a " + std::string(spawns::node_level_name(level)) + ": only a Village or a Town has free service plots";
        return nullptr;
    }
    const world::PlotRow* plot = nullptr;
    for (const world::PlotRow& row : rows) {
        if (same_text(row.name, plot_name)) {
            plot = &row;
            break;
        }
    }
    if (!plot) {
        std::vector<std::string> names;
        for (const world::PlotRow* p : free_plots(rows)) names.push_back(p->name);
        out.status = StationStatus::NotFound;
        out.message = node.display_name + " has no plot named \"" + plot_name + "\" (its free plots: " + joined(names, 20) + ")";
        return nullptr;
    }
    if (plot->state != 0 || plot->pre_construct) {
        out.status = StationStatus::Refused;
        out.message = plot->name + " of " + node.display_name + " is not free (a building stands on it)";
        return nullptr;
    }
    if (placed_stations_.count({node.guid, plot->plot})) {
        out.status = StationStatus::Refused;
        out.message = plot->name + " of " + node.display_name + " holds a placed station already (worldctl remove-station takes it off)";
        return nullptr;
    }
    return plot;
}

bool PlayerWorld::station_placement_ready() const {
    return city_nodes_ && node_wire_ && node_pop_data_ && area_report_ && area_report_->data && captured_ && generic_ && node_pops_;
}

PlayerWorld::StationResult PlayerWorld::place_station(const std::string& node_name, const std::string& plot_name, const std::string& station_name, const std::string& by) {
    StationResult r;
    const auto fail = [&](StationStatus s, std::string m) {
        r.status = s;
        r.message = std::move(m);
        spdlog::info("[stations] {}: place {} {} {}: refused: {}", by, node_name, plot_name, station_name, r.message);
        return r;
    };
    if (!station_placement_ready()) {
        return fail(StationStatus::Refused, "the towns' people are not running (world needs the area, the nodes and their people)");
    }
    if (node_name.empty() || plot_name.empty() || station_name.empty()) return fail(StationStatus::Invalid, "a node, a plot and a station are needed");
    const world::CityNodeInfo* node = city_nodes_->by_name(node_name);
    if (!node) return fail(StationStatus::NotFound, "no city node is named \"" + node_name + "\" (worldctl places lists them)");
    const int level = city_nodes_->level_of(node->guid);
    const std::vector<world::PlotRow> rows = node_wire_->plot_rows_of(node->guid, level);
    const world::PlotRow* plot = find_free_station_plot(*node, level, plot_name, rows, r);
    if (!plot) return fail(r.status, std::move(r.message));

    const spawns::NodeSite site = spawns::node_site_of(*city_nodes_, *node_wire_, *node, level);
    const std::vector<int64_t> buildings = node_wire_->plot_buildings(plot->plot);
    const spawns::TemplateSource templates{&*captured_, &*generic_};
    int64_t building = 0;
    std::string why;
    const auto spawn = spawns::select_station_on_plot(*area_report_->data, templates, *node_pop_data_, site, *plot, buildings, station_name, building, why);
    if (!spawn) {
        return fail(StationStatus::NotFound, why + " (the stations the plot's buildings have: " + joined(spawns::stations_on_plot(*area_report_->data, *node_pop_data_, site, buildings), 40) + ")");
    }
    std::string error;
    auto w = spawns::make_data_actor(spawn->spawn, [this] { return guids_.next(); }, server_id_, world_clock_, nullptr, error);
    if (!w) return fail(StationStatus::Refused, "the station could not be made: " + error);
    w->station.placed = true;
    w->npc.spawn_ref = 0;
    w->npc.ground_by_data = true;
    const uint64_t id = w->id;
    const std::string blueprint = w->station.name;
    if (!registry_.add(std::move(*w))) return fail(StationStatus::Refused, "the registry refused the station's actor");
    finish_station(id, *node, plot->plot, building);
    placed_stations_[{node->guid, plot->plot}] = id;
    placed_seen_version_ = city_nodes_->version();
    const world::WorldActor* made = registry_.find(id);
    r.actor = id;
    r.node = node->display_name;
    r.plot = plot->name;
    r.station = blueprint;
    r.building = node_wire_->building_name(plot->plot, building);
    if (made) {
        r.x = made->x;
        r.y = made->y;
        r.z = made->z;
    }
    spdlog::info("[stations] {}: placed {} (actor {}) on {} of {} at ({:.0f}, {:.0f}, {:.0f}), a station of {}; it stays until removed, until the node drops below a Village, or until world stops",
                 by, blueprint, id, plot->name, node->display_name, r.x, r.y, r.z, r.building);
    return r;
}

PlayerWorld::StationResult PlayerWorld::remove_station(const std::string& node_name, const std::string& plot_name, const std::string& by) {
    StationResult r;
    const auto fail = [&](StationStatus s, std::string m) {
        r.status = s;
        r.message = std::move(m);
        spdlog::info("[stations] {}: remove {} {}: refused: {}", by, node_name, plot_name, r.message);
        return r;
    };
    if (!city_nodes_) return fail(StationStatus::Refused, "world has no city nodes");
    const world::CityNodeInfo* node = city_nodes_->by_name(node_name);
    if (!node) return fail(StationStatus::NotFound, "no city node is named \"" + node_name + "\"");
    for (auto it = placed_stations_.begin(); it != placed_stations_.end(); ++it) {
        if (it->first.first != node->guid) continue;
        const world::WorldActor* a = registry_.find(it->second);
        std::string plot_record;
        for (const world::PlotRow& row : node_wire_ ? node_wire_->plot_rows_of(node->guid, city_nodes_->level_of(node->guid)) : std::vector<world::PlotRow>{}) {
            if (row.plot == it->first.second) plot_record = row.name;
        }
        if (!same_text(plot_record, plot_name)) continue;
        r.node = node->display_name;
        r.plot = plot_record;
        r.station = a ? a->station.name : std::string();
        r.actor = it->second;
        if (a) {
            r.x = a->x;
            r.y = a->y;
            r.z = a->z;
            r.building = node_wire_ ? node_wire_->building_name(it->first.second, a->station.building) : std::string();
        }
        registry_.remove(it->second);
        placed_stations_.erase(it);
        spdlog::info("[stations] {}: removed {} from {} of {}", by, r.station, plot_record, node->display_name);
        return r;
    }
    return fail(StationStatus::NotFound, "no station was placed on " + plot_name + " of " + node->display_name);
}

void PlayerWorld::reap_placed_stations() {
    placed_seen_version_ = city_nodes_ ? city_nodes_->version() : 0;
    for (auto it = placed_stations_.begin(); it != placed_stations_.end();) {
        const int64_t node = it->first.first;
        const int64_t plot = it->first.second;
        const int level = city_nodes_->level_of(node);
        bool keep = false;
        if (level >= 3 && node_wire_) {
            for (const world::PlotRow& row : node_wire_->plot_rows_of(node, level)) {
                if (row.plot == plot && row.state == 0 && !row.pre_construct) keep = true;
            }
        }
        if (keep && registry_.find(it->second)) {
            ++it;
            continue;
        }
        const world::WorldActor* a = registry_.find(it->second);
        spdlog::info("[stations] the placed station {} of node {} leaves: the node is a {} now{}", a ? a->station.name : std::string("?"), node, spawns::node_level_name(level),
                     level >= 3 ? " and the plot is not free any more" : "");
        registry_.remove(it->second);
        it = placed_stations_.erase(it);
    }
}

void PlayerWorld::setup_node_shopkeeper(uint64_t id, const world::CityNodeInfo& node) {
    if (!shops_ || node.guid == 0) return;
    const world::WorldActor* keeper = registry_.find(id);
    if (!keeper || keeper->kind != world::ActorKind::Npc) return;
    if (shops_->shop_of(keeper->npc.instance_guid, keeper->npc.asset_guid) == 0 && !opens_shop_by_talk(dialogue_ ? &*dialogue_ : nullptr, *shops_, *keeper)) return;
    registry_.modify(id, world::Change::State, [&](world::WorldActor& w) {
        world::allow_inputs(w.actor);
        for (protocol::repl::Subobject& s : w.actor.subobjects) {
            if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCNodePlotComponent") s.obj.state.set(*s.obj.schema, "NodeRecordGuid", protocol::repl::Value::i64(node.guid));
        }
    });
}

void PlayerWorld::on_node_person_spawned(uint64_t id, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route,
                                         const world::CityNodeInfo& node, int64_t plot) {
    if (bundle_ && gear_) {
        if (const world::WorldActor* person = registry_.find(id); person && person->kind == world::ActorKind::Npc) {
            if (!npc_book_) npc_book_.emplace(*bundle_, *gear_);
            std::string why;
            if (const auto cs = npc_book_->make(motion_.get(person->npc.archetype), person->npc.level, why)) {
                combat::write_stats(registry_, id, *cs);
            } else {
                spdlog::warn("[people] {} ({}): its stats do not run, it keeps its template's: {}", id, spawns::blueprint_leaf(person->npc.archetype), why);
            }
        }
    }
    setup_node_shopkeeper(id, node);
    if (const world::WorldActor* st = registry_.find(id); st && st->kind == world::ActorKind::Station) {
        finish_station(id, node, plot);
        return;
    }
    if (!npcs_) return;
    const world::WorldActor* a = registry_.find(id);
    if (!a) return;
    npc::NpcMotionSetup setup = npc::make_setup(*a, motion_, instance, route, speeds(), nullptr, &node);
    if (setup.ambient == npc::Ambient::Patrol && setup.route.size() < 2) setup.ambient = npc::Ambient::Wander;
    if (plot != 0) npc::keep_to_workshop(&setup, npc::kWorkshopRadiusCm);
    npcs_->add(id, setup);
}

size_t PlayerWorld::start_node_people(const spawns::NodePopulations::Config& config_in) {
    if (node_pops_ || !config_in.enabled || !city_nodes_ || !node_wire_ || !node_pop_data_ || !area_report_ || !area_report_->data || !captured_ || !generic_) return 0;
    spawns::NodePopulations::Config config = config_in;
    const std::set<std::string> leaves = captured_->npc_leaves();
    config.shown_humanoids.insert(leaves.begin(), leaves.end());
    spawns::NodePopulations::Hooks hooks;
    hooks.spawned = [this](uint64_t id, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route, const world::CityNodeInfo& node, int64_t plot) {
        on_node_person_spawned(id, instance, route, node, plot);
    };
    hooks.despawned = [this](uint64_t id) {
        if (combat_) combat_->forget(id);
    };
    const spawns::TemplateSource templates{&*captured_, &*generic_, looks_.get()};
    node_pops_ = std::make_unique<spawns::NodePopulations>(registry_, *city_nodes_, *node_wire_, *node_pop_data_, area_report_->data, templates, spawn_context_,
                                                           motion_loaded_ ? &motion_ : nullptr, area_, [this] { return guids_.next(); }, server_id_, std::move(hooks), config);
    const size_t made = node_pops_->start(world_clock_);
    if (looks_) {
        const auto& lc = looks_->counts();
        spdlog::info("[looks] with the towns' people: {} dressed in all, {} named by the data, {} by their Blueprint, {} with no name, {} left as recorded",
                     lc.dressed, lc.named, lc.blueprint_named, lc.unnamed, lc.no_record);
    }
    return made;
}

uint64_t PlayerWorld::respawn_npc(uint32_t spawn_ref, double now) {
    if (spawns::NodePopulations::is_node_ref(spawn_ref)) return node_pops_ ? node_pops_->respawn(spawn_ref, now) : 0;
    if (!area_report_ || spawn_ref == 0 || spawn_ref > area_report_->recipes.size() || !npcs_ || !captured_) return 0;
    const spawns::SpawnRecipe& r = area_report_->recipes[spawn_ref - 1];
    if (!r.open) return 0;
    spawns::InstanceParams p = r.params;
    p.seed = (p.seed ^ (0xA24BAED4963EE407ull * ++respawns_made_)) * 0x9E3779B97F4A7C15ull;
    p.world_time = now;
    std::string why;
    auto w = spawns::instantiate(*r.open, [this] { return guids_.next(); }, p, why);
    if (!w) {
        spdlog::warn("[loot] a respawn ({}) was refused: {}", r.info.name, why);
        return 0;
    }
    dress_and_pose_respawn(*w, r);
    // Preserve fresh subobject GUIDs so a lingering corpse's collision cylinder does not cause a NetGuidMismatch on the new actor.
    const uint64_t controller = w->npc.controller;
    const uint64_t interact = w->npc.interact_component;
    const uint64_t cylinder = w->npc.collision_cylinder;
    w->npc = r.info;
    w->npc.controller = controller;
    w->npc.interact_component = interact;
    w->npc.collision_cylinder = cylinder;
    const uint64_t id = w->id;
    const std::string name = w->npc.name;
    if (!registry_.add(std::move(*w))) {
        spdlog::warn("[loot] a respawn ({}, spawn point {}) was refused by the registry: actor {} or one of its components is held by another actor", name, spawn_ref, id);
        return 0;
    }
    const world::WorldActor* a = registry_.find(id);
    npcs_->add(id, make_respawn_motion_setup(*a, *area_report_, motion_, speeds()));
    return id;
}

PlayerWorld::SpawnNpcResult PlayerWorld::spawn_npc(const SpawnNpcParams& req, const std::string& by) {
    SpawnNpcResult r;
    const auto fail = [&](StationStatus s, std::string m) {
        r.status = s;
        r.message = std::move(m);
        spdlog::info("[npc] {}: spawn {}: refused: {}", by, req.npc, r.message);
        return r;
    };
    if (req.npc.empty()) return fail(StationStatus::Invalid, "an NPC name or GUID is required");
    if (!area_report_ || !area_report_->data || !captured_ || !generic_) return fail(StationStatus::Refused, "the area population is not loaded");
    double x = req.x, y = req.y, z = req.z;
    const float yaw = std::isfinite(req.yaw_degrees) ? req.yaw_degrees : 0.0f;
    if (req.place.empty() && (!std::isfinite(x) || !std::isfinite(y) || (req.has_z && !std::isfinite(z)))) return fail(StationStatus::Invalid, "the point is not a finite number");

    const spawns::TemplateSource templates{&*captured_, &*generic_, looks_.get()};
    const content::NpcInstanceRow* inst = nullptr;
    const content::NpcAssetRow* asset = resolve_spawn_npc_asset(*area_report_->data, templates, req.npc, inst);
    if (!asset) return fail(StationStatus::NotFound, "no NPC in content.db matches \"" + req.npc + "\"");

    bool is_generic = false;
    spawns::Verdict verdict = spawns::Verdict::NoPath;
    const spawns::OpenTemplate* open = templates.find(*asset, &is_generic, &verdict, inst);
    if (!open && !captured_->opens().empty()) open = &captured_->opens().front();
    if (!open) return fail(StationStatus::Refused, "NPC \"" + asset->name + "\" has no spawnable visual template (" + std::string(spawns::verdict_name(verdict)) + ")");

    const double half_h = motion_.get(asset->archetype_path).half_height;
    if (!req.place.empty()) {
        const world::Places all_places = places();
        const world::Place* p = all_places.find(req.place);
        if (!p) return fail(StationStatus::NotFound, "no place named \"" + req.place + "\" (worldctl places lists them)");
        x = p->x; y = p->y; r.place = p->name;
        if (const auto town = town_ground_at(x, y)) z = town->z + half_h;
        else if (const auto gz = ground_at(x, y)) z = *gz + half_h;
        else z = p->z;
    } else if (!req.has_z) {
        if (const auto town = town_ground_at(x, y)) z = town->z + half_h;
        else if (const auto gz = ground_at(x, y)) z = *gz + half_h;
        else z = (std::isfinite(req.z) && req.z != 0.0) ? req.z : (spawns::kLoginSpotZ + half_h);
    }
    const int level = std::clamp(req.level > 0 ? req.level : (inst && inst->level_min > 0 ? inst->level_min : 1), 1, 50);
    spawns::DataSpawn ds{x, y, z, yaw, level, asset, inst, open, is_generic, false, 0, 0, looks_.get(), 0, 0,
                         spawns::point_hash(asset->guid, static_cast<int64_t>(x), static_cast<int64_t>(y), static_cast<int64_t>(++respawns_made_)), 0};
    std::string make_err;
    auto w = spawns::make_data_actor(ds, [this] { return guids_.next(); }, server_id_, world_clock_, nullptr, make_err, objects_ ? &*objects_ : nullptr);
    if (!w) return fail(StationStatus::Refused, "the NPC could not be made: " + make_err);
    w->npc.source = world::Source::Runtime; w->npc.spawn_ref = 0;
    const uint64_t id = w->id;
    if (!registry_.add(std::move(*w))) return fail(StationStatus::Refused, "the registry refused the NPC actor");
    on_node_person_spawned(id, inst, nullptr, world::CityNodeInfo{}, 0);
    const world::WorldActor* made = registry_.find(id);
    r.id = id; r.npc = asset->name; r.asset_guid = asset->guid; r.level = level; r.yaw_degrees = yaw;
    r.display_name = made && !made->npc.name.empty() ? made->npc.name : spawns::blueprint_leaf(asset->archetype_path);
    r.x = made ? made->x : x; r.y = made ? made->y : y; r.z = made ? made->z : z;
    spdlog::info("[npc] {}: spawned {} ({}, actor {}, asset {}, level {}) at ({:.0f}, {:.0f}, {:.0f})",
                 by, r.npc, r.display_name, id, r.asset_guid, level, r.x, r.y, r.z);
    return r;
}

}}  // namespace aoc::worldd
