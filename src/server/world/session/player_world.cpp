// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_world.h"
#include "net/tick_profile.h"

#include "spawns/npc_looks.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace aoc { namespace worldd {

namespace {

std::string verify_bare_classes(const stats::StatBundle& bundle, const std::set<int64_t>& needed) {
    for (int class_enum = 1; class_enum <= 8; ++class_enum) {
        stats::StatInputs in;
        in.class_enum = class_enum;
        stats::StatResult result;
        std::string err;
        if (!stats::run_stats(bundle, in, stats::StatState{}, result, err)) return "class " + std::to_string(class_enum) + " does not run: " + err;
        if (result.faults != 0) return "class " + std::to_string(class_enum) + " faults " + std::to_string(result.faults) + " time(s) bare";
        std::set<int64_t> have;
        for (const stats::StatOut& s : result.stats) have.insert(s.guid);
        for (const int64_t guid : needed) {
            if (!have.count(guid)) return "class " + std::to_string(class_enum) + " has no stat " + std::to_string(guid) + ", which the ability runtime reads or sets";
        }
    }
    return {};
}

std::string stat_program_problem(const content::StatProgramRow& row, const stats::StatBundle& bundle, const content::AbilityRules& rules) {
    if (row.version != stats::kBundleVersion) return "its format version is " + std::to_string(row.version) + ", this world runs " + std::to_string(stats::kBundleVersion);
    if (row.stats != static_cast<int>(bundle.stats.size()) || row.classes != static_cast<int>(bundle.classes.size())) {
        return "its row says " + std::to_string(row.stats) + " stats and " + std::to_string(row.classes) + " classes, the bundle has " +
               std::to_string(bundle.stats.size()) + " and " + std::to_string(bundle.classes.size());
    }
    // Checksum binds the row and blob to a single build so a mismatched or hand-edited program fails closed.
    if (row.crc32 != bundle.crc32) return "its row says crc32 " + std::to_string(row.crc32) + ", the bundle's header says " + std::to_string(bundle.crc32);
    const char* mode = bundle.mode == stats::Mode::Float32 ? "float32" : bundle.mode == stats::Mode::Double ? "double" : "single";
    if (row.mode != mode) return "its row says mode " + row.mode + ", the bundle's is " + mode;
    std::set<int64_t> needed = {content::kGuidStamina, content::kGuidMaxStamina};
    for (const int64_t effect : {rules.sprint.effect_guid, rules.dodge.effect_guid, rules.rest.effect_guid}) {
        const content::EffectRule* e = rules.effect(effect);
        if (!e) continue;
        for (const content::StatMod& m : e->stat_mods) {
            needed.insert(m.stat_guid);
            if (m.source_stat_guid != 0) needed.insert(m.source_stat_guid);
        }
    }
    return verify_bare_classes(bundle, needed);
}

}  // namespace

bool PlayerWorld::load_mount_speeds(const std::string& path, std::string& error) {
    if (!mounts_) {
        error = "there are no mounts";
        return false;
    }
    if (!mounts_->load_speeds(path, error)) return false;
    spdlog::info("[content] mount speeds: {} Blueprints from {}", mounts_->speeds_known(), path);
    return true;
}

bool PlayerWorld::load_mount_riders(const std::string& path, std::string& error) {
    if (!mounts_) {
        error = "there are no mounts";
        return false;
    }
    if (!mounts_->load_riders(path, error)) return false;
    spdlog::info("[content] mount riders: {} Blueprints name their rider AnimBP in {}", mounts_->riders_known(), path);
    return true;
}

bool PlayerWorld::load_roads(const std::string& path, std::string& error) {
    world::RoadMap fresh;
    if (!fresh.load(path, error, std::clamp(road_default_level_, 0, world::RoadMap::kLevels - 1))) return false;
    roads_ = std::move(fresh);
    roads_seen_version_ = ~0ull;
    spdlog::info("[content] roads: {} road(s), {} segment(s) from {}{}", roads_.roads(), roads_.segments(), path,
                 roads_.node_level() >= 0 ? " (tiers resolved at node level " + std::to_string(roads_.node_level()) + ")" : std::string());
    if (roads_.level_roads() != 0) {
        apply_road_levels();
        spdlog::info("[roads] {} of the {} roads follow their zone's node level (a zone with no node: level {}); the tiers change when a node's level does", roads_.level_roads(), roads_.roads(),
                     road_default_level_);
    }
    return true;
}

bool PlayerWorld::init_core_tables(std::string& problem) {
    auto row = content_->stat_program(problem);
    if (!row) return false;
    stats::StatBundle bundle;
    if (!stats::parse_bundle(row->data, bundle, problem)) {
        problem = "the stat program in content.db does not verify: " + problem;
        return false;
    }
    if (const std::string why = stat_program_problem(*row, bundle, *rules_); !why.empty()) {
        problem = "the stat program in content.db is not one this world can run: " + why;
        return false;
    }
    bundle_ = std::move(bundle);
    gear_.emplace(*content_);
    problem = gear_->ok() ? std::string() : gear_->error();
    spdlog::info("[content] stat program: {} bytes, {} stats, {} classes, mode {}", row->data.size(), bundle_->stats.size(),
                 bundle_->classes.size(), row->mode);
    if (!problem.empty()) return false;

    std::string error;
    leveling_type_ = content_->leveling_type_named("Adventuring", error);
    if (!leveling_type_) {
        problem = "content.db has no usable Adventuring leveling type: " + error;
        return false;
    }
    auto curve = leveling::LevelCurve::from(*leveling_type_, error);
    if (!curve) {
        problem = "content.db's Adventuring level curve is not one this world can level with: " + error;
        return false;
    }
    curve_ = std::move(*curve);
    auto grants = content_->auto_grants(leveling_type_->guid, error);
    if (!grants) {
        problem = "content.db's auto-granted ranks cannot be read: " + error;
        return false;
    }
    for (content::AutoGrant& g : *grants) {
        if (g.tree_guid == content::kGuidUniversalTree) grants_.push_back(std::move(g));
    }
    spdlog::info("[content] Adventuring: {} levels, cap {}; {} rank(s) of the Universal tree granted by levels", curve_->max_level(),
                 curve_->cap(), grants_.size());
    return true;
}

bool PlayerWorld::init_progression_tables(std::string& problem) {
    std::string error;
    auto data = content_->talent_data(error);
    if (!data) {
        problem = "content.db's skill trees cannot be read: " + error;
        return false;
    }
    auto catalog = talents::Catalog::from(std::move(*data), error);
    if (!catalog) {
        problem = "content.db's skill trees are not what this world can run: " + error;
        return false;
    }
    talents_ = std::move(*catalog);
    spdlog::info("[content] talents: {} skill trees, {} profession tree(s) with ranks a character has from the start", talents_->tree_count(),
                 talents_->professions().size());

    std::string why;
    if (auto prof_data = content_->profession_data(why)) {
        if (auto book = professions::Book::from(*prof_data, why)) {
            professions_ = std::move(*book);
            spdlog::info("[content] professions: {} professions, {} certification levels, {} certification actions", professions_->size(),
                         prof_data->certifications.size(), prof_data->actions.size());
        } else {
            problem = "content.db's professions are not what this world can run: " + why;
            return false;
        }
    } else if (!why.empty()) {
        problem = "content.db's professions cannot be read: " + why;
        return false;
    } else {
        spdlog::warn("[content] content.db has no profession tables: characters have no professions (rebuild content.db with this checkout's contentc)");
    }
    return true;
}

void PlayerWorld::init_optional_tables() {
    std::string why;
    if (auto lines = weaponxp::Lines::from(*content_, why)) {
        weapon_lines_ = std::move(*lines);
        spdlog::info("[content] weapon lines: {} lines on one curve of {} levels (the last level at {} XP)", weapon_lines_->size(), weapon_lines_->max_level(),
                     weapon_lines_->max_xp());
    } else if (!why.empty()) {
        spdlog::warn("[content] the weapon lines are not usable ({}): weapon XP is not tracked and a weapon tree is judged at the character's level", why);
    } else {
        spdlog::warn("[content] content.db has no weapon lines: weapon XP is not tracked and a weapon tree is judged at the character's level (rebuild content.db with this checkout's contentc)");
    }

    why.clear();
    if (auto data = content_->currency_data(why)) {
        if (auto table = items::CurrencyTable::from(*data, why)) {
            currencies_ = std::move(*table);
            size_t carried = 0;
            for (const items::CurrencyTier& t : currencies_->tiers()) carried += t.carried ? 1 : 0;
            spdlog::info("[content] currencies: {} tiers ({} of them made of items, the Glint kinds among them), {} kinds", currencies_->tiers().size(), carried, data->defs.size());
        } else {
            spdlog::warn("[content] the currency tiers are not usable ({}): no stack of Glint changes kind and a reward of Glint is paid as it is rolled", why);
        }
    } else if (!why.empty()) {
        spdlog::warn("[content] the currency tiers cannot be read ({}): no stack of Glint changes kind and a reward of Glint is paid as it is rolled", why);
    } else {
        spdlog::warn("[content] content.db has no currency tables: no stack of Glint changes kind (rebuild content.db with this checkout's contentc)");
    }

    why.clear();
    if (auto cat = abilities::Catalog::load(content_->path(), *bundle_, why)) {
        ability_catalog_ = std::move(*cat);
        spdlog::info("[content] abilities: {} abilities, {} effects, {} hits, {} expressions (the generic ability runtime runs them)",
                     ability_catalog_->abilities(), ability_catalog_->effects(), ability_catalog_->hits(), ability_catalog_->exprs());
    } else {
        spdlog::warn("[content] the ability tables are not usable ({}): only Sprint, Dodge and Rest run; rebuild content.db with this checkout's contentc", why);
    }

    why.clear();
    if (auto book = spawns::MountBook::load(*content_, why)) {
        mounts_ = std::move(*book);
        spdlog::info("[content] mounts: {} mount items, {} mount abilities, {} mount effects", mounts_->data().mounts.size(), mounts_->data().abilities.size(),
                     mounts_->data().effects.size());
    } else {
        spdlog::warn("[content] the mount tables are not usable ({}): no mount can be summoned", why);
    }
}

void PlayerWorld::reset_on_content_error(const std::string& problem) {
    ability_catalog_.reset();
    professions_.reset();
    weapon_lines_.reset();
    currencies_.reset();
    talents_.reset();
    bundle_.reset();
    gear_.reset();
    catalog_.reset();
    leveling_type_.reset();
    curve_.reset();
    grants_.clear();
    content_ = nullptr;
    content_problem_ = problem + " (rebuild content.db with this checkout's contentc: cd services && go run ./cmd/contentc)";
}

PlayerWorld::PlayerWorld(const content::ContentDb* content, std::string content_problem, bootstrap::GameStateMode mode,
                         uint32_t server_id, protocol::ServerGuidAllocator& guids)
    : content_(content), content_problem_(std::move(content_problem)), mode_(mode), server_id_(server_id), guids_(guids),
      game_state_(guids.next()), day_clock_(world::default_world_clock()) {
    if (!content_) return;
    std::string error;
    rules_ = content_->ability_rules(error);
    if (!rules_) {
        // Fail closed when ability rows cannot be loaded: refuse logins until content.db is rebuilt.
        content_ = nullptr;
        content_problem_ = error;
        return;
    }
    catalog_.emplace(*content_);
    std::string problem;
    if (init_core_tables(problem) && init_progression_tables(problem)) {
        init_optional_tables();
    }
    if (!problem.empty()) {
        reset_on_content_error(problem);
    }
}

PlayerWorld::~PlayerWorld() = default;

bool PlayerWorld::load_blueprints(const std::string& motion_path, std::string& error) {
    if (area_report_) {
        error = "the area is loaded already";
        return false;
    }
    const std::filesystem::path dir = std::filesystem::path(motion_path).parent_path();
    if (!dir.empty() && std::filesystem::exists(dir / "npc_looks.tsv")) {
        looks_dir_ = dir.string();
    } else if (std::filesystem::exists(std::filesystem::path("dist/content") / "npc_looks.tsv")) {
        looks_dir_ = "dist/content";
    } else if (std::filesystem::exists(std::filesystem::path("content") / "npc_looks.tsv")) {
        looks_dir_ = "content";
    } else {
        looks_dir_ = dir.string();
    }
    std::string tree_error;
    if (!npc::load_standing_archetypes((std::filesystem::path(looks_dir_) / "npc_trees.tsv").string(), &standing_archetypes_, tree_error)) {
        spdlog::warn("[nav] no state tree table ({}): every NPC moves as its population instance says, even those whose tree cannot (run the extractor's npcabilities step)", tree_error);
    } else {
        spdlog::info("[nav] state tree table: {} Blueprints whose tree has no task to wander or patrol with (their NPCs stand at their post)", standing_archetypes_.size());
    }
    npc::MotionTable table;
    if (!table.load(motion_path, error)) return false;
    motion_ = std::move(table);
    motion_loaded_ = true;
    spdlog::info("[nav] Blueprint table {}: {} pawn Blueprints", motion_path, motion_.size());
    return true;
}

bool PlayerWorld::load_npc_abilities(const std::string& abilities_path, const std::string& montages_path, const std::string& targeting_path, std::string& error) {
    if (combat_) {
        error = "the fights have started";
        return false;
    }
    combat::NpcAbilityData data;
    if (!data.load(abilities_path, montages_path, targeting_path, error)) return false;
    npc_abilities_ = std::move(data);
    spdlog::info("[combat] mob attack animations: {} Blueprints, {} montages", npc_abilities_.kinds(), npc_abilities_.montages());
    for (const std::string& line : npc_abilities_.coverage_lines()) spdlog::info("[combat]   {}", line);
    return true;
}

bool PlayerWorld::load_npc_senses(const std::string& senses_path, const std::string& curves_path, std::string& error) {
    if (combat_) {
        error = "the fights have started";
        return false;
    }
    npc::SensesTable table;
    if (!table.load(senses_path, curves_path, error)) return false;
    npc_senses_ = std::move(table);
    spdlog::info("[combat] mob senses: {} Blueprints (what each notices, how far it follows)", npc_senses_.size());
    return true;
}

void PlayerWorld::log_hunting_kinds() {
    if (!gathering_ || gathering_->hunting().empty() || !stat_bundle() || !stat_context().gear) return;
    const bootstrap::StatContext ctx = stat_context();
    stats::CharacterStats cs(*ctx.bundle, *ctx.gear, 2, 25, ctx.rounding);
    std::string err;
    if (!cs.start({}, err)) {
        spdlog::warn("[hunt] the hunting kinds are not checked: the probe character's stats do not start ({})", err);
        return;
    }
    auto run = cs.evaluation({}, err);
    if (!run) {
        spdlog::warn("[hunt] the hunting kinds are not checked: {}", err);
        return;
    }
    const size_t none = gathering_->classify_hunting(*run);
    std::string names;
    for (const auto& [def, k] : gathering_->hunting()) {
        if (k.gives_nothing) names += (names.empty() ? "" : ", ") + k.name;
    }
    spdlog::info("[hunt] {} hunting kinds: {} give no usable item (their items are RETIRED_ or NOTUSED_, or the table never selects one): {}; their corpses are hunted for the experience only. "
                 "The data's NumberOfSubtablesToSelect of 2 is read as 1 for {} tables with two subtables (a hunt gives a carcass or a trainable, not both)",
                 gathering_->hunting().size(), none, names.empty() ? "none" : names, gathering_->hunting_one_of());
}

void PlayerWorld::place_gm_garden() {
    if (garden_report_ || !captured_ || !area_report_) return;
    spawns::GardenGround ground;
    if (nav_) {
        ground = [this](double x, double y, double hint, double* z) {
            double terrain = hint;
            if (!nav_->terrain_height(x, y, &terrain)) return false;
            return nav_->ground_height(x, y, terrain, z, 150, 150);   // (a floor within 1.5 m of the terrain, else the terrain: never on a roof)
        };
    }
    garden_report_ = spawns::place_gm_garden(registry_, spawns::garden_donor(*captured_), area_, [this] { return guids_.next(); }, server_id_, 0.0, ground);
    spdlog::info("[area] {}", garden_report_->describe());
    for (const std::string& n : garden_report_->notes) spdlog::warn("[area] {}", n);
}

void PlayerWorld::load_crafting_data() {
    if (!content_ || stations_) return;
    std::string se;
    if (auto data = content_->craft_station_data(se)) {
        stations_.emplace(std::move(*data));
        spdlog::info("[world] stations: {} definitions, {} assets that are a station", stations_->defs.size(), stations_->asset_defs.size());
    } else {
        spdlog::warn("[world] the station definitions do not load, so the stations open with the default tax: {}", se);
    }
    std::string re;
    if (auto data = content_->craft_recipe_data(re)) {
        recipes_.emplace(std::move(*data));
        size_t unread = 0, unknown_xp = 0;
        for (const auto& [guid, reward] : recipes_->rewards) {
            unread += reward.unread != 0 ? 1 : 0;
            unknown_xp += reward.unknown_formula.empty() ? 0 : 1;
        }
        spdlog::info("[world] recipes: {} recipes, {} cost records, {} recipe sets, {} reward tables ({} with a part left out, {} with an experience formula not worked out), experience curves {}, artisan ratings of {} crafting and {} processing professions{}",
                     recipes_->recipes.size(), recipes_->costs.size(), recipes_->set_lists.size(), recipes_->rewards.size(), unread, unknown_xp, recipes_->xp_curves.ok() ? "read" : "MISSING: no Adventuring experience for a job",
                     recipes_->artisan.crafting_professions, recipes_->artisan.processing_professions, recipes_->artisan.ok() ? "" : " (MISSING: crafts and jobs ignore the crafter's ratings)");
    } else {
        spdlog::warn("[world] the recipes do not load, so no job can be started at a station: {}", re);
    }
}

void PlayerWorld::start_combat() {
    if (!combat_config_.enabled) {
        spdlog::info("[combat] OFF (--no-combat): the NPCs do not fight and take no hits");
        return;
    }
    if (!bundle_ || !gear_ || !ability_catalog() || !npcs_) {
        spdlog::warn("[combat] off: it needs the stat program, the item catalog, the ability tables and the NPCs' ground");
        return;
    }
    std::string e;
    const auto rows = content_->combat_rows(e);
    if (!rows) {
        spdlog::warn("[combat] off: {}", e);
        return;
    }
    combat_data_ = combat::CombatData::build(*rows, *bundle_, e);
    if (!combat_data_) {
        spdlog::warn("[combat] off: {}", e);
        return;
    }
    if (!npc_book_) npc_book_.emplace(*bundle_, *gear_);
    combat_ = std::make_unique<combat::CombatWorld>(registry_, npcs_.get(), nav_.get(), *combat_data_, *ability_catalog(), *npc_book_, motion_, combat_config_);
    combat_->set_ability_data(&npc_abilities_);
    if (!npc_senses_.empty()) combat_->set_senses_data(&npc_senses_);
    if (!npc_abilities_.empty()) spdlog::info("[combat] {}", combat_->gate_census().text());
    start_aftermath();
    spdlog::info("[combat] on: {} abilities in the tables, damage profile of {} elements, in combat for {} s, NPCs fight within their aggro radius (default {} cm) and leash at {} cm, and their hits do {} x the data's damage to a player",
                 ability_catalog()->abilities(), combat_data_->profile().elements.size(), combat_data_->in_combat_timer(), combat_config_.default_aggro_cm,
                 combat_config_.default_leash_cm, combat_config_.npc_damage_mult);
}

bool PlayerWorld::prop_gone(uint64_t actor_id, double now_seconds) const {
    const world::WorldActor* a = registry_.find(actor_id);
    if (!a || a->kind != world::ActorKind::Prop || !area_report_ || a->prop.slot == 0 || a->prop.slot > area_report_->props.size()) return false;
    const size_t i = a->prop.slot - 1;
    const spawns::PropRecipe& r = area_report_->props[i];
    // it comes back after its instance's own time (the data's 1 to 150 s, plus up to the variance), at least a second
    const double jitter = r.variance > 0 ? r.variance * static_cast<double>((a->prop.slot * 2654435761ull) % 1000ull) / 1000.0 : 0.0;
    prop_slots_[i].actor = 0;
    prop_slots_[i].due = now_seconds + std::max(1.0, r.respawn) + jitter;
    ++props_gone_;
    return true;
}

void PlayerWorld::tick_props(double now_seconds) {
    if (props_gone_ == 0 || !area_report_) return;
    for (size_t i = 0; i < prop_slots_.size(); ++i) {
        PropSlot& s = prop_slots_[i];
        if (s.actor != 0 || now_seconds < s.due) continue;
        const spawns::PropRecipe& r = area_report_->props[i];
        std::string why;
        auto w = spawns::instantiate(*r.open, [this] { return guids_.next(); }, r.params, why);
        if (!w) {
            s.due = now_seconds + 60.0;
            spdlog::warn("[props] a quest object could not be made again: {}", why);
            continue;
        }
        w->prop.slot = static_cast<uint32_t>(i + 1);
        const uint64_t id = w->id;
        if (!registry_.add(std::move(*w))) {
            s.due = now_seconds + 60.0;
            spdlog::warn("[props] a quest object could not be added to the registry again");
            continue;
        }
        s.actor = id;
        s.due = 0;
        --props_gone_;
    }
}

void PlayerWorld::tick_town_people_and_roads(double now_seconds) {
    if (node_pops_) node_pops_->tick(now_seconds);
    if (!placed_stations_.empty() && city_nodes_ && placed_seen_version_ != city_nodes_->version()) reap_placed_stations();
    if (city_nodes_ && roads_.level_roads() != 0 && roads_seen_version_ != city_nodes_->version()) apply_road_levels();
}

void PlayerWorld::tick_world(double now_seconds) {
    world_clock_ = now_seconds;
    if (bootstrap::has_global_time_of_day_override()) {
        day_clock_ = world::WorldClock::reading(bootstrap::client_times(now_seconds).game_time, now_seconds);
    }
    if (view_config_.phases) view_config_.phases->begin_tick();
    tick_props(now_seconds);
    TickProfile& profile = tick_profile();
    if (nav_ && nav_->windowed() && npcs_ && now_seconds - last_window_drive_ >= kWindowPeriodSeconds) {
        last_window_drive_ = now_seconds;
        const auto w = std::chrono::steady_clock::now();
        drive_window(now_seconds);
        if (profile.on) profile.window_us += tick_profile_us_since(w);
    }
    const auto ms = [](std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); };
    auto t = std::chrono::steady_clock::now();
    // Town people and roads follow node levels after the ground window and before the NPC step so newly spawned NPCs enter the current tick cleanly.
    tick_town_people_and_roads(now_seconds);
    const double people_ms = ms(t);
    window_cost_.max_people_ms = std::max(window_cost_.max_people_ms, people_ms);
    t = std::chrono::steady_clock::now();
    if (npcs_) npcs_->tick(now_seconds);
    check_towns(now_seconds);
    const double npcs_ms = ms(t);
    window_cost_.max_npcs_ms = std::max(window_cost_.max_npcs_ms, npcs_ms);
    t = std::chrono::steady_clock::now();
    if (combat_) combat_->tick(now_seconds);
    const double combat_ms = ms(t);
    window_cost_.max_combat_ms = std::max(window_cost_.max_combat_ms, combat_ms);
    t = std::chrono::steady_clock::now();
    if (aftermath_) aftermath_->tick(now_seconds);
    const double aftermath_ms = ms(t);
    window_cost_.max_aftermath_ms = std::max(window_cost_.max_aftermath_ms, aftermath_ms);
    if (profile.on) {
        profile.people_us += people_ms * 1000.0;
        profile.npcs_us += npcs_ms * 1000.0;
        profile.combat_us += combat_ms * 1000.0;
        profile.aftermath_us += aftermath_ms * 1000.0;
    }
    if (scenery_) scenery_->tick();   // a town whose level changed gets a new full payload (every connection that holds its replicator is sent the update)
}

size_t PlayerWorld::apply_road_levels() {
    if (roads_.level_roads() == 0) return 0;
    const world::CityNodes* nodes = city_nodes_;
    if (nodes) roads_seen_version_ = nodes->version();
    const size_t changed = roads_.apply_levels(
        [nodes](int64_t zoi) -> int {
            if (!nodes) return -1;
            const world::CityZoneInfo* z = nodes->zone_by_guid(zoi);
            return z && z->node_guid != 0 ? nodes->level_of(z->node_guid) : -1;   // a zone with no node (a starting area): the world's default level
        },
        road_default_level_);
    if (changed != 0) spdlog::info("[roads] the nodes' levels changed the tier or width of {} of {} roads", changed, roads_.level_roads());
    return changed;
}

uint64_t PlayerWorld::drop_harvest_pile(HarvestPileDrop drop, std::string& error) {
    error.clear();
    if (aftermath_) return aftermath_->drop_pile(drop.x, drop.y, drop.z, drop.owner, drop.name, std::move(drop.stacks), world_clock_, error);
    if (pile_hook_) return pile_hook_(drop.x, drop.y, drop.z, drop.owner, drop.name, drop.stacks);
    error = "the world has no loot piles (no area, or --no-loot)";
    return 0;
}

bool PlayerWorld::start_gathering(const gather::Config& config, std::string& error) {
    error.clear();
    if (gathering_) return true;
    if (!content_) {
        error = "no content.db";
        return false;
    }
    if (!ability_catalog()) {
        error = "no ability catalog (the formulas of a harvest are its programs)";
        return false;
    }
    auto g = gather::Gathering::make(*content_, registry_, *ability_catalog(), config, error);
    if (!g) return false;
    gathering_ = std::move(g);
    spdlog::info("[gather] on: {} node defs, {} tools, {} reward tables; a gathered node returns after the data's respawn x {}{}", gathering_->defs(), gathering_->tools(), gathering_->tables(),
                 config.board.respawn_mult, config.quality_literal ? "; GetRolledItemQuality() answers the literal quality" : "");
    log_hunting_kinds();
    return true;
}

bool PlayerWorld::start_foliage(const foliage::Config& config, std::string& error) {
    error.clear();
    if (foliage_) return true;
    if (!gathering_) {
        error = "gathering is off";
        return false;
    }
    auto f = foliage::Foliage::make(*content_, *gathering_, registry_, [this] { return mint_guid(); }, config, error);
    if (!f) return false;
    foliage_ = std::move(f);
    spdlog::info("[foliage] on: {}", foliage_->describe());
    return true;
}

size_t PlayerWorld::start_scenery(const world::NodeScenery::Config& config) {
    if (scenery_ || !config.enabled || !city_nodes_ || !node_wire_ || !node_layout_data_) return 0;
    scenery_ = std::make_unique<world::NodeScenery>(registry_, *city_nodes_, *node_wire_, *node_layout_data_, [this] { return guids_.next(); }, config);
    return scenery_->start();
}

std::set<int64_t> PlayerWorld::collect_reward_roots(size_t& quest_tables) const {
    std::set<int64_t> roots;
    for (const auto& [path, row] : motion_.rows()) roots.insert(row.rewards.begin(), row.rewards.end());
    registry_.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Npc) roots.insert(a.npc.loot_tables.begin(), a.npc.loot_tables.end());
    });
    if (area_report_) {
        for (const spawns::SpawnRecipe& r : area_report_->recipes) roots.insert(r.info.loot_tables.begin(), r.info.loot_tables.end());
    }
    quest_tables = 0;
    std::set<int64_t> named;
    if (quests_) named = quests_->reward_tables();
    if (dialogue_) {
        const std::set<int64_t> given = dialogue_->reward_tables();
        named.insert(given.begin(), given.end());
    }
    for (const int64_t t : named) quest_tables += roots.insert(t).second ? 1 : 0;
    return roots;
}

void PlayerWorld::init_reward_book() {
    size_t quest_tables = 0;
    const std::set<int64_t> roots = collect_reward_roots(quest_tables);
    std::string e;
    std::map<int64_t, content::LootTable> tables;
    if (aftermath_config_.enabled && !roots.empty()) {
        auto read = content_->loot_tables(std::vector<int64_t>(roots.begin(), roots.end()), e);
        if (!read) {
            spdlog::warn("[loot] no reward tables: {} (the kills give no experience and no loot)", e);
        } else {
            tables = std::move(*read);
        }
    }
    int64_t decay = 0;
    if (aftermath_config_.enabled) {
        if (const auto r = content_->xp_rule("adventuring_decay", e)) decay = *r;
    }
    if (!tables.empty()) {
        const size_t n = tables.size();
        rewards_.emplace(*ability_catalog(), std::move(tables), decay);
        size_t formulas = 0;
        std::string variation_error;
        if (auto variations = content_->gather_data(variation_error)) {
            formulas = variations->quality_expr.size();
            rewards_->set_quality_rules(std::move(variations->quality_expr), [this](int64_t item, int quality) { return item_rarity(item, quality); });
        } else if (!variation_error.empty()) {
            spdlog::warn("[loot] no item variations: {} (the kills' items keep their record's quality)", variation_error);
        }
        spdlog::info("[loot] {} reward tables of the {} the area's mobs and the quests name ({} of them only the quests'), the experience decay {}, {} item variation formulas for the quality of the drops", n,
                     roots.size(), quest_tables, decay != 0 ? "of the Adventuring leveling type" : "(none)", formulas);
    }
}

void PlayerWorld::start_aftermath() {
    init_reward_book();
    aftermath_ = std::make_unique<combat::Aftermath>(registry_, *combat_, rewards_ ? &*rewards_ : nullptr, [this] { return guids_.next(); }, aftermath_config_);
    aftermath_->set_quality_of([this](int64_t item) { return item_quality(item); });
    aftermath_->set_rarity_of([this](int64_t item, int quality) { return item_rarity(item, quality); });
    aftermath_->set_name_of([this](int64_t item) { return item_name(item); });
    aftermath_->set_currencies(currencies());   // a kill's Glint is paid in kinds, the biggest first
    aftermath_->set_respawner([this](uint32_t ref, double now) { return respawn_npc(ref, now); });
    aftermath_->set_city_nodes(city_nodes_);
    const auto huntable = [this](const world::NpcInfo& i) { return gathering_ && i.interact_def != 0 && gathering_->hunt_kind(i.interact_def) != nullptr; };
    aftermath_->set_huntable(huntable);
    combat_->set_huntable(huntable);
    aftermath_->install();
    if (!aftermath_config_.enabled) {
        spdlog::info("[loot] OFF (--no-loot): a death gives no experience and no loot, and nothing respawns (the corpse still goes)");
    } else {
        char boost[32];
        std::snprintf(boost, sizeof boost, "%g", aftermath_config_.loot_rarity_boost);
        spdlog::info("[loot] on: a corpse stays {:.0f} s ({:.0f} s after its loot is taken, {:.0f} s unlooted), a spawn point refills its NPC after the instance's respawn time{}{}{}",
                     aftermath_config_.corpse_seconds, aftermath_config_.emptied_seconds, aftermath_config_.loot_corpse_seconds, aftermath_config_.respawn ? "" : " (respawn off)",
                     aftermath_config_.loot_chance_mult != 1.0 ? "; every RollEachByPercent loot chance x" + std::to_string(aftermath_config_.loot_chance_mult).substr(0, 5) + " (--loot-chance-mult)" : std::string(),
                     aftermath_config_.loot_rarity_boost != 1.0 ? std::string("; TEST BOOST: every outcome above Common in the rarity rolls of a kill's gear and resources x") + boost + " (--loot-rarity-boost; harvests are never boosted)" : std::string());
    }
}

}}  // namespace aoc::worldd
