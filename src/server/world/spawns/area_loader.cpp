// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/area_loader.cpp
//
//  See area_loader.h.
// ============================================================================
#include "spawns/area_loader.h"
#include "respawn/make_shrines.h"

#include "spawns/gm_garden.h"
#include "spawns/npc_looks.h"
#include "spawns/spawn_util.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <sstream>

namespace aoc::worldd::spawns {

namespace {

namespace repl = ::aoc::protocol::repl;

std::string leaf_of(const std::string& archetype_path) {
    const size_t dot = archetype_path.rfind('.');
    std::string leaf = archetype_path.substr(dot == std::string::npos ? 0 : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct SpawnerAnchor {
    double x = 0.0, y = 0.0, z = 0.0;
};

const char* gatherable_rarity_name(uint64_t seed) noexcept {
    const uint64_t roll = seed % 100;
    if (roll < 50) return "Common";
    if (roll < 75) return "Uncommon";
    if (roll < 87) return "Rare";
    if (roll < 94) return "Heroic";
    if (roll < 98) return "Epic";
    return "Legendary";
}

std::string format_gatherable_display_name(const garden::Def& def, uint64_t seed) {
    const char* rarity = gatherable_rarity_name(seed);
    return std::string(def.name) + " [" + def.profession + " / " + def.resource + " | " + rarity +
           " | Tier " + std::to_string(def.tier + 1) + " " + def.tier_name + "]";
}

bool configure_gatherable_node(world::WorldActor& w, const garden::Def& def, const SpawnerAnchor& pt, double yaw) {
    std::string error;
    repl::Actor& a = w.actor;
    repl::ObjectState& actor = a.obj;
    if (!util::set_path(actor, "GatherableActorDef.Guid", repl::Value::i64(static_cast<int64_t>(def.guid)), error)) return false;
    for (repl::Subobject& s : a.subobjects) {
        if (!s.obj.schema || std::strcmp(s.obj.schema->name, "UInteractableComponent") != 0) continue;
        const repl::Value* data = s.obj.state.get(*s.obj.schema, "InteractionInputData");
        const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
        if (arr && !arr->elements.empty() && arr->elements[0].values.count(6)) {
            repl::ArrayValue copy = *arr;
            copy.elements[0].values[6] = repl::Value::i64(static_cast<int64_t>(def.interaction));
            if (!s.obj.state.set(*s.obj.schema, "InteractionInputData", repl::Value(std::move(copy)))) return false;
        }
    }
    a.open.spawn.location = repl::quantize_vector(10, pt.x, pt.y, pt.z);
    a.open.spawn.rotation = repl::Rot{0, util::angle16(yaw), 0};
    a.open.spawn.velocity.reset();
    if (const repl::Value* v = actor.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) {
            repl::RepMovement m = *old;
            m.location = repl::quantize_location(actor.opts, pt.x, pt.y, pt.z);
            m.rotation = repl::Rot{0, actor.opts.rep_movement_short_rotation ? util::angle16(yaw) : util::angle8(yaw), 0};
            m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
            m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
            m.acceleration.reset();
            if (!util::set_path(actor, "ReplicatedMovement", repl::Value(m), error)) return false;
        }
    }
    const uint64_t qseed = mix(def.guid ^ static_cast<uint64_t>(std::llround(pt.x)) ^ (static_cast<uint64_t>(std::llround(pt.y)) << 1));
    const std::string display = format_gatherable_display_name(def, qseed);
    w.x = pt.x; w.y = pt.y; w.z = pt.z; w.yaw_degrees = yaw;
    w.label = display;
    w.npc.name = display;
    w.npc.level = (def.tier + 1) * 10;
    w.npc.source = world::Source::Data;
    w.npc.home_x = pt.x; w.npc.home_y = pt.y; w.npc.home_z = pt.z;
    return true;
}

void append_area_circle_anchors(const content::NpcPlacement& pl, const content::NpcAreaRow& area_row, const AreaConfig& area,
                                bool use_halo, std::map<uint64_t, std::vector<SpawnerAnchor>>& by_zoi) {
    for (size_t ci = 0; ci < area_row.circles.size(); ++ci) {
        const content::NpcAreaCircle& c = area_row.circles[ci];
        const int n_pts = std::clamp(static_cast<int>(c.extent / 3500.0) + 1, 1, 6);
        for (int s = 0; s < n_pts; ++s) {
            const uint64_t h = mix(static_cast<uint64_t>(pl.spawn) ^ static_cast<uint64_t>(area_row.guid) ^ ((ci + 1) * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64_t>(s * 131));
            const double rad = std::max(0.0, c.extent) * std::sqrt(static_cast<double>(h & 0xFFFFu) / 65535.0);
            const double ang = (static_cast<double>((h >> 16) & 0xFFFFu) / 65535.0) * 6.283185307179586;
            const double px = pl.x + c.x + rad * std::cos(ang), py = pl.y + c.y + rad * std::sin(ang), pz = pl.z + c.z;
            if (!(use_halo ? area.contains_with_halo(px, py) : area.contains(px, py)) || pz < -5000.0) continue;
            const uint64_t zoi = world::zoi_at(px, py);
            by_zoi[zoi != 0 ? zoi : 1ULL].push_back({px, py, pz});
        }
    }
}

std::map<uint64_t, std::vector<SpawnerAnchor>> collect_spawner_anchors(const content::PopulationData& pop, const AreaConfig& area) {
    std::map<uint64_t, std::vector<SpawnerAnchor>> by_zoi;
    const bool use_halo = area.uses_boundary_halo_for_spawns();
    for (const content::NpcPlacement& pl : pop.placements) {
        const auto ws = pop.world_spawns.find(pl.spawn);
        if (ws != pop.world_spawns.end() && ws->second.map_name != "Verra_World_Master" && ws->second.name.rfind("NodeHelper_ArtisanPOI_", 0) != 0) continue;
        if (const auto sit = pop.sets.find(pl.set); sit != pop.sets.end() && sit->second.area != 0) {
            if (const auto ait = pop.areas.find(sit->second.area); ait != pop.areas.end()) {
                append_area_circle_anchors(pl, ait->second, area, use_halo, by_zoi);
            }
        }
        if (!(use_halo ? area.contains_with_halo(pl.x, pl.y) : area.contains(pl.x, pl.y)) || pl.z < -5000.0) continue;
        const uint64_t zoi = world::zoi_at(pl.x, pl.y);
        by_zoi[zoi != 0 ? zoi : 1ULL].push_back({pl.x, pl.y, pl.z});
    }
    for (const NodePlugObb& plug : kVerraNodePlugs) {
        if (!area.contains(plug.cx_cm, plug.cy_cm)) continue;
        std::vector<SpawnerAnchor>& bucket = by_zoi[plug.node_guid];
        for (int i = 0; i < 16; ++i) {
            const double ang = i * (2.0 * 3.14159265358979323846 / 16.0), r = 18000.0 + (i % 4) * 6500.0;
            const double px = plug.cx_cm + r * std::cos(ang), py = plug.cy_cm + r * std::sin(ang);
            if (area.contains(px, py)) bucket.push_back({px, py, kLoginSpotZ});
        }
    }
    return by_zoi;
}

void seed_managed_area_resource_nodes(world::ActorRegistry& registry, const AreaSources& sources, const Minter& mint, uint32_t server_id, AreaReport& report) {
    if (!report.area.whole_world || !report.data || !sources.captured) return;
    const OpenTemplate* donor = garden_donor(*sources.captured);
    if (!donor || garden::kGardenDefCount == 0) return;
    const auto by_zoi = collect_spawner_anchors(*report.data, report.area);
    std::vector<uint64_t> near;
    for (const auto& [zoi_guid, pts] : by_zoi) {
        if (pts.empty()) continue;
        const size_t target = std::min<size_t>(180, pts.size());
        const size_t stride = std::max<size_t>(1, pts.size() / target);
        for (size_t k = 0; k < target; ++k) {
            const SpawnerAnchor& base = pts[(k * stride) % pts.size()];
            const uint64_t h = mix(zoi_guid ^ (static_cast<uint64_t>(k + 1) * 0x9E3779B97F4A7C15ull));
            const double rad = 450.0 + static_cast<double>(h % 950), ang = static_cast<double>((h >> 16) % 360) * (3.14159265358979323846 / 180.0);
            SpawnerAnchor pt{base.x + rad * std::cos(ang), base.y + rad * std::sin(ang), base.z};
            if (!report.area.contains(pt.x, pt.y)) pt = base;
            near.clear();
            registry.query(pt.x, pt.y, 2500.0, near);
            bool has_nearby_node = false;
            for (uint64_t id : near) {
                if (const world::WorldActor* ex = registry.find(id); ex && ex->kind == world::ActorKind::Gatherable) { has_nearby_node = true; break; }
            }
            if (has_nearby_node) continue;
            const garden::Def& def = garden::kGardenDefs[h % garden::kGardenDefCount];
            InstanceParams p;
            p.keep_captured_position = true;
            p.server_id = server_id;
            p.seed = mix(0x7E5001CEull ^ h ^ def.guid);
            std::string why;
            auto w = instantiate(*donor, mint, p, why);
            if (!w || !configure_gatherable_node(*w, def, pt, static_cast<double>((h >> 24) % 360))) continue;
            if (registry.add(std::move(*w))) ++report.gatherables;
        }
    }
}

struct Placed {
    double x = 0.0, y = 0.0;
    std::string archetype;
};

struct LoadContext {
    world::ActorRegistry& registry;
    const AreaSources& sources;
    const Minter& mint;
    uint32_t server_id = 0;
    double world_time = 0.0;
    AreaReport& report;
    std::vector<Placed> placed;
};

void note_report(AreaReport& report, std::string n) {
    if (report.notes.size() < 20) report.notes.push_back(std::move(n));
}

void place_captured_spawns(LoadContext& ctx) {
    std::vector<Placed> nodes;
    const bool use_halo = ctx.report.area.uses_boundary_halo_for_spawns();
    for (size_t i = 0; i < ctx.sources.captured->spawns().size(); ++i) {
        const CapturedSpawn& c = ctx.sources.captured->spawns()[i];
        if (!(use_halo ? ctx.report.area.contains_with_halo(c.x, c.y) : ctx.report.area.contains(c.x, c.y))) continue;
        const bool npc = c.open->category == "npc";
        const std::vector<Placed>& against = npc ? ctx.placed : nodes;
        const double near = npc ? kSamePointCm : 50.0;
        bool dup = false;
        for (const Placed& p : against) {
            if (p.archetype == c.open->archetype_path && std::hypot(p.x - c.x, p.y - c.y) <= near) { dup = true; break; }
        }
        if (dup) { ++(npc ? ctx.report.duplicates : ctx.report.gatherable_duplicates); continue; }
        InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = ctx.server_id;
        p.world_time = ctx.world_time;
        p.seed = mix(0xC0FFEEull ^ static_cast<uint64_t>(i));
        std::string why;
        auto w = instantiate(*c.open, ctx.mint, p, why);
        if (!w) {
            ++ctx.report.refused;
            note_report(ctx.report, "a captured spawn (" + leaf_of(c.open->archetype_path) + ") was refused: " + why);
            continue;
        }
        w->x = c.x; w->y = c.y; w->z = c.z;
        w->npc.source = world::Source::Captured;
        w->npc.home_x = c.x; w->npc.home_y = c.y; w->npc.home_z = c.z;
        if (!npc && garden::kGardenDefCount > 0) {
            const garden::Def& def = garden::kGardenDefs[p.seed % garden::kGardenDefCount];
            const std::string display = format_gatherable_display_name(def, p.seed);
            w->label = display;
            w->npc.name = display;
            w->npc.level = (def.tier + 1) * 10;
        }
        const std::string path = c.open->archetype_path;
        if (!ctx.registry.add(std::move(*w))) {
            ++ctx.report.refused;
            note_report(ctx.report, "a captured spawn could not be added to the registry");
            continue;
        }
        if (npc) {
            ctx.placed.push_back({c.x, c.y, path});
            ++ctx.report.captured_npcs;
            ++ctx.report.npcs_by_archetype[leaf_of(path)];
        } else {
            nodes.push_back({c.x, c.y, path});
            ++ctx.report.gatherables;
        }
    }
}

void place_gm_vendors(LoadContext& ctx) {
    if (!ctx.sources.gm) return;
    const OpenTemplate* tpl = ctx.sources.captured->template_for(kGmVendorArchetype);
    if (!tpl) {
        note_report(ctx.report, std::string("the vendor archetype ") + kGmVendorArchetype + " has no capture: no GM vendors");
        return;
    }
    const auto& defs = ctx.sources.gm->vendors();
    const std::vector<vendor::GmPlacement> layout = vendor::gm_layout(defs.size(), ctx.report.area, kLoginSpotYaw, kLoginSpotZ);
    for (size_t i = 0; i < defs.size(); ++i) {
        InstanceParams p;
        p.x = layout[i].x; p.y = layout[i].y; p.z = layout[i].z;
        p.yaw_degrees = layout[i].yaw_degrees;
        p.server_id = ctx.server_id; p.world_time = ctx.world_time;
        p.name = defs[i].label; p.clear_population = true;
        p.seed = mix(0x6D5E11ull ^ static_cast<uint64_t>(defs[i].guid));
        std::string why;
        auto w = instantiate(*tpl, ctx.mint, p, why);
        if (!w) { ++ctx.report.refused; note_report(ctx.report, "the GM vendor " + defs[i].label + " was refused: " + why); continue; }
        if (!world::allow_inputs(w->actor)) {
            ++ctx.report.refused;
            note_report(ctx.report, "the GM vendor " + defs[i].label + " has no interactable component to allow inputs on");
            continue;
        }
        w->npc.source = world::Source::Runtime;
        w->npc.name = defs[i].label;
        w->npc.vendor_guid = defs[i].guid;
        w->npc.is_gm_vendor = true;
        w->npc.home_x = layout[i].x; w->npc.home_y = layout[i].y; w->npc.home_z = layout[i].z;
        if (!ctx.registry.add(std::move(*w))) {
            ++ctx.report.refused;
            note_report(ctx.report, "the GM vendor " + defs[i].label + " could not be added to the registry");
            continue;
        }
        ++ctx.report.gm_vendors;
    }
}

}  // namespace

std::string AreaReport::describe() const {
    std::ostringstream o;
    if (area.region) {
        o << "area: the Riverlands (" << region::kRiverlandsRectCount << " rectangles, " << region::kRiverlandsAreaCells * (region::kCellCm / 100.0) * (region::kCellCm / 100.0) / 1.0e6
          << " km2, " << region::kZoiCount << " zones of influence)";
    } else {
        o << "area: centre (" << static_cast<long long>(area.center_x) << ", " << static_cast<long long>(area.center_y) << ") cm, radius " << area.radius_cm / 100.0 << " m";
    }
    o << ": " << data_npcs << " NPCs from the data (" << population.points_in_area << " placement points and " << population.area_points << " random points of "
      << population.area_sets << " area sets in the area: " << population.generic_spawned << " NPCs made from a generic Blueprint template, " << population.area_spawned
      << " from area points, " << population.no_template << " points without any template, " << population.blacklisted << " blacklisted, " << population.no_asset
      << " without an asset, " << population.filtered_total() << " dropped by the spawn filter), " << captured_npcs << " captured NPCs (" << duplicates << " dropped as the data's), " << gatherables << " resource nodes, " << gm_vendors
      << " GM vendors, " << shrines_made << " Embersprings (" << respawn_points.size() << " respawn points in the area, " << shrines_no_record << " without a respawn definition, " << shrines_refused << " refused)";
    return o.str();
}
// (Cyb) (C.O.A)

std::optional<world::WorldActor> make_data_actor(const DataSpawn& s, const Minter& mint, uint32_t server_id, double world_time, InstanceParams* params, std::string& error,
                                                 const content::ObjectData* objects) {
    InstanceParams p;
    p.x = s.x;
    p.y = s.y;
    p.z = s.z;
    p.yaw_degrees = s.yaw;
    p.level = s.level;
    p.server_id = server_id;
    p.world_time = world_time;
    p.pop_instance = s.instance ? s.instance->guid : 0;
    p.pop_asset = s.asset ? s.asset->guid : 0;
    p.seed = s.seed;
    if (s.open && s.open->category == "prop" && s.instance) {   // a quest object: the definitions its interactable and its actor are made from
        p.component_def = s.instance->interact_component != 0 ? s.instance->interact_component : (s.asset ? s.asset->interact_component : 0);
        p.interact_def = s.instance->interact_response != 0 ? s.instance->interact_response : (s.asset ? s.asset->interaction_def : 0);
        if (objects) {
            if (const auto it = objects->instance_iad.find(s.instance->guid); it != objects->instance_iad.end()) p.actor_def = it->second;
            // the interaction of the definition's first input is what the component lists (the instance's own response is 0 on a crate or a site)
            if (const auto it = objects->interactables.find(p.component_def); it != objects->interactables.end() && it->second.pid != 0) p.interact_def = it->second.pid;
        }
    }
    if (params) *params = p;
    auto w = instantiate(*s.open, mint, p, error);
    if (!w) return std::nullopt;
    if (s.looks) {   // a humanoid is dressed and named by the data, not by the recording its open came from (a failure leaves the recorded look: the NPC still stands)
        std::string look_error;
        if (!s.looks->dress(*w, s, look_error)) spdlog::debug("[looks] {}: {}", s.asset ? s.asset->name : std::string("?"), look_error);
    }
    // its pose: the instance's idle animation, as the game's server sets it (a recorded open's own pose is the recording's)
    if (std::string idle_error; !apply_idle_animation(*w, s.instance, idle_error)) spdlog::debug("[looks] {}: {}", s.asset ? s.asset->name : std::string("?"), idle_error);
    w->npc.source = world::Source::Data;
    w->npc.ground_by_terrain = s.from_area;
    w->npc.asset_guid = p.pop_asset;
    w->npc.instance_guid = p.pop_instance;
    w->npc.vendor_guid = s.instance ? s.instance->vendor : 0;
    // its conversation: the instance's own root, else its asset's
    w->npc.dialogue_root = (s.instance && s.instance->dialogue_root != 0) ? s.instance->dialogue_root : (s.asset ? s.asset->dialogue_root : 0);
    w->npc.home_x = s.x;
    w->npc.home_y = s.y;
    w->npc.home_z = s.z;
    // how far it notices and follows: the instance's own radius when it overrides the asset's [R: research/combat 6]
    // [R: the data, retest 3] an override with a radius of 0 on a Timid asset (Rabid_Timid, Deer_Timid, Spider_Timid, Raven_Timid: 31 of 31 instances of Pop_TimidGrem) is "never
    // notices a player"; the same override on the camps' wolves, goblins and bandits keeps the Blueprint's default, because they attack on sight [user, retest 3]; no override with no
    // radius is "the Blueprint's", the AI's default
    if (s.instance && s.instance->override_aggro) {
        if (s.instance->aggro_radius > 0) w->npc.aggro_radius_cm = s.instance->aggro_radius;
        else if (s.asset && s.asset->asset_ref.find("_Timid.") != std::string::npos) w->npc.aggro_off = true;
    } else if (s.asset && s.asset->aggro_radius > 0) {
        w->npc.aggro_radius_cm = s.asset->aggro_radius;
    }
    if (s.asset && s.asset->leash_radius > 0) w->npc.leash_radius_cm = s.asset->leash_radius;
    if (s.instance) {
        w->npc.respawn_seconds = s.instance->respawn_time;
        w->npc.respawn_variance = s.instance->respawn_variance;
    }
    // what its death does: the tables its asset and instance name, how it comes back
    if (s.asset) w->npc.loot_tables.insert(w->npc.loot_tables.end(), s.asset->loot_tables.begin(), s.asset->loot_tables.end());
    if (s.instance) {
        w->npc.loot_tables.insert(w->npc.loot_tables.end(), s.instance->loot_tables.begin(), s.instance->loot_tables.end());
        w->npc.respawn_rule = s.instance->respawn_rule;
    }
    return w;
}

std::optional<AreaReport> load_area(world::ActorRegistry& registry, const AreaSources& sources, const AreaConfig& area, const Minter& mint, uint32_t server_id,
                                    double world_time, std::string& error) {
    if (!sources.db || !sources.captured) {
        error = "the area needs content.db and the captured spawns";
        return std::nullopt;
    }
    AreaReport report;
    report.area = area;
    auto loaded = sources.db->population_data(error);
    if (!loaded) return std::nullopt;
    report.data = std::make_shared<const content::PopulationData>(std::move(*loaded));
    const content::PopulationData* population = report.data.get();

    report.respawn_points = respawn_points(*population, area);
    const TemplateSource templates{sources.captured, sources.generic, sources.looks};
    const std::vector<DataSpawn> data = select_data_spawns(*population, templates, area, report.population, sources.filter);
    const auto note = [&](std::string n) { note_report(report, std::move(n)); };

    LoadContext ctx{registry, sources, mint, server_id, world_time, report, {}};
    for (const DataSpawn& s : data) {
        InstanceParams p;
        std::string why;
        auto w = make_data_actor(s, mint, server_id, world_time, &p, why, sources.objects);
        if (!w) { ++report.refused; note("a data NPC (" + leaf_of(s.open->archetype_path) + ") was refused: " + why); continue; }
        if (s.instance) {
            report.instances.emplace(s.instance->guid, *s.instance);
            if (s.instance->patrol_route != 0) {
                if (const auto r = population->routes.find(s.instance->patrol_route); r != population->routes.end()) report.routes.emplace(r->first, r->second);
            }
            report.recipes.push_back({s.open, p, w->npc, s});
            w->npc.spawn_ref = static_cast<uint32_t>(report.recipes.size());
            report.recipes.back().info.spawn_ref = w->npc.spawn_ref;
        }
        const std::string path = s.open->archetype_path;
        const double x = w->x, y = w->y;
        const bool prop = w->kind == world::ActorKind::Prop;
        if (prop) {
            report.props.push_back(PropRecipe{s.open, p, s.instance ? s.instance->guid : 0, s.instance ? s.instance->respawn_time : 0, s.instance ? s.instance->respawn_variance : 0, w->id});
            w->prop.slot = static_cast<uint32_t>(report.props.size());
        }
        if (!registry.add(std::move(*w))) {
            ++report.refused; note("a data NPC could not be added to the registry");
            if (prop) report.props.pop_back();
            continue;
        }
        if (prop) continue;
        ctx.placed.push_back({x, y, path});
        ++report.data_npcs;
        ++report.npcs_by_archetype[leaf_of(path)];
    }

    place_captured_spawns(ctx);
    seed_managed_area_resource_nodes(registry, sources, mint, server_id, report);
    place_gm_vendors(ctx);

    if (sources.db && sources.captured) {
        std::string respawn_error;
        if (const auto rdata = sources.db->respawn_data(respawn_error)) {
            respawn::MadeShrines made = respawn::make_shrines(registry, *sources.captured, *rdata, report.respawn_points, mint, server_id, world_time, note);
            report.shrines_made = made.made;
            report.shrines_no_record = made.no_record;
            report.shrines_refused = made.refused;
            if (made.no_number > 0) note(std::to_string(made.no_number) + " Emberspring(s) stand for a place with no number in the master list: they can be seen and used, not discovered");
            report.shrines = std::move(made.book);
        } else {
            note("no Embersprings: " + respawn_error);
        }
    }
    return report;
}

}  // namespace aoc::worldd::spawns
