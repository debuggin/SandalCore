// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "items/acquired.h"
#include "session/player_session_internal.h"
#include <iterator>
#include <limits>
namespace aoc {
namespace worldd {
using namespace ::sandal::worldd::session::detail;
namespace {
using StepBuilder = std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap &)>;
using QueueLaterFn = std::function<void(const char *, StepBuilder, Clock::time_point, bool)>;
bool has_primary_or_ranged_event(const std::vector<items::Event> &events) {
    for (const items::Event &e : events) {
        if (e.container == svc::ItemContainer::Equipment && (e.slot == items::equip::Primary || e.slot == items::equip::Ranged))
            return true;
    }
    return false;
}
size_t count_equipment_marks(const std::vector<items::Event> &events) {
    size_t marks = 0;
    for (const items::Event &e : events) {
        if (e.container == svc::ItemContainer::Equipment)
            ++marks;
    }
    return marks ? marks : 1;
}
bool has_construction_crate_tag(const std::vector<std::string> &tags) {
    for (const std::string &tag : tags) {
        if (iequals(tag, "item.crate.construction"))
            return true;
    }
    return false;
}
std::map<int64_t, int> wanted_cargo_effects(const svc::InventoryItem *crate, const PlayerWorld *world, std::string_view session_name) {
    std::map<int64_t, int> wanted;
    if (!crate || !world || !world->content())
        return wanted;
    std::string error;
    const auto listed = world->content()->item_equipped_effects(crate->item_record_id, error);
    if (!listed)
        spdlog::warn("[session] {}: the effects of the crate cannot be read: {}", session_name, error);
    const int rarity = world->item_rarity_facts(crate->item_record_id).rarity_of(crate->quality);
    for (const content::EquippedEffect &e : listed ? *listed : std::vector<content::EquippedEffect>{}) {
        if (rarity >= e.min_rarity && rarity <= e.max_rarity)
            wanted[e.effect] = e.stacks;
    }
    return wanted;
}
struct CargoEffectDiff {
    bootstrap::AbilityChange change;
    std::vector<std::pair<int64_t, int64_t>> added;
    std::vector<std::pair<int64_t, int64_t>> removed;
};
CargoEffectDiff diff_cargo_effects(std::map<int64_t, int64_t> &cargo_effects, const std::map<int64_t, int> &wanted, int64_t &next_passive_effect_id,
                                   double start_time) {
    CargoEffectDiff out;
    for (auto it = cargo_effects.begin(); it != cargo_effects.end();) {
        if (wanted.count(it->first)) {
            ++it;
            continue;
        }
        out.change.effects_removed.push_back(it->second);
        out.removed.emplace_back(it->first, it->second);
        it = cargo_effects.erase(it);
    }
    for (const auto &[effect, stacks] : wanted) {
        if (cargo_effects.count(effect))
            continue;
        bootstrap::EffectItem e;
        e.effect_id = next_passive_effect_id++;
        e.effect_guid = effect;
        e.start_time = start_time;
        e.duration = std::numeric_limits<float>::max();
        e.tick_time = 0;
        e.stacks = stacks;
        out.change.effects_added.push_back(e);
        out.added.emplace_back(effect, e.effect_id);
        cargo_effects[effect] = e.effect_id;
    }
    return out;
}
bool is_drop_crate_on_bar(const actionbar::Bar &bar) {
    for (const repl::ActionbarSlot &s : bar.slots()) {
        if (s.use.use_type == repl::ActionbarUseType::Ability && s.use.ability_id == content::kGuidDropCrate)
            return true;
    }
    return false;
}
repl::ActionbarSlot select_drop_crate_slot(const actionbar::Bar &bar, int default_bank, int default_slot) {
    repl::ActionbarSlot slot;
    slot.bank_index = default_bank;
    slot.slot_index = default_slot;
    slot.use.use_type = repl::ActionbarUseType::Ability;
    slot.use.ability_id = content::kGuidDropCrate;
    if (bar.at(slot.bank_index, slot.slot_index)) {
        if (const auto spare = bar.first_free()) {
            slot.bank_index = spare->first;
            slot.slot_index = spare->second;
        }
    }
    return slot;
}
struct ClaimCheckContext {
    const world::ActorRegistry *registry = nullptr;
    const ActorView *view = nullptr;
    const PlayerWorld *world = nullptr;
    GoodPosition at{};
    const std::vector<svc::ProcessingJob> *jobs = nullptr;
    int64_t now_ms = 0;
};
struct ClaimTargetResult {
    bool ok = false;
    std::string refusal;
    const world::WorldActor *station = nullptr;
    std::string key;
    std::string job_guid;
    svc::ProcessingJob job;
    const content::CraftRecipe *recipe = nullptr;
};
ClaimTargetResult fail_claim_target(std::string why) {
    ClaimTargetResult r;
    r.refusal = std::move(why);
    return r;
}
ClaimTargetResult check_claim_station_actor(const world::WorldActor *a, const ActorView &view, const GoodPosition &at) {
    if (!a || a->kind != world::ActorKind::Station)
        return fail_claim_target("the object named is no station of this world");
    if (view.state_of(a->id) != world::ClientState::Awake)
        return fail_claim_target("the client does not have that station open");
    const double dx = a->x - at.x;
    const double dy = a->y - at.y;
    const double dz = a->z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) {
        return fail_claim_target("the station " + a->station.name + " is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    }
    if (!a->station.processing)
        return fail_claim_target(a->station.name + " is a station that crafts at once: it holds no jobs to collect");
    const std::string key = crafting::station_key(a->station.seed);
    if (key.empty())
        return fail_claim_target(a->station.name + " has no key that survives a restart: it holds no jobs");
    ClaimTargetResult out;
    out.ok = true;
    out.station = a;
    out.key = key;
    return out;
}
ClaimTargetResult validate_claim_station(const repl::ProcessingRequest &req, const ClaimCheckContext &ctx) {
    if (req.type == crafting::kCancelled)
        return fail_claim_target("cancelling a job is not made yet: the job runs on");
    if (req.type != crafting::kClaimed) {
        return fail_claim_target("a request of type " + std::to_string(req.type) + ": only a claim (type " + std::to_string(crafting::kClaimed) +
                                 ") is served");
    }
    if (!ctx.registry || !ctx.view || !ctx.world || !ctx.jobs)
        return fail_claim_target("the world has no actors");
    return check_claim_station_actor(ctx.registry->find(req.station.ObjectId), *ctx.view, ctx.at);
}
ClaimTargetResult validate_claim_target(const repl::ProcessingRequest &req, const ClaimCheckContext &ctx) {
    ClaimTargetResult out = validate_claim_station(req, ctx);
    if (!out.ok)
        return out;
    const content::CraftRecipeData *recipes = ctx.world->craft_recipes();
    if (!recipes)
        return fail_claim_target("worldd has no recipes (an older content.db)");
    out.job_guid = items::guid_hex(req.state.job);
    const svc::ProcessingJob *found = crafting::find_job(*ctx.jobs, out.key, out.job_guid);
    if (!found)
        return fail_claim_target("the player has no job " + out.job_guid + " at " + out.station->station.name);
    if (crafting::unfinished(*found, ctx.now_ms)) {
        return fail_claim_target("the job is not over: " + std::to_string((found->end_ms() - ctx.now_ms + 999) / 1000) + " s to go");
    }
    const auto recipe_it = recipes->recipes.find(found->recipe_guid);
    if (recipe_it == recipes->recipes.end())
        return fail_claim_target("recipe " + std::to_string(found->recipe_guid) + " of the job is none of the data's");
    out.job = *found;
    out.recipe = &recipe_it->second;
    return out;
}
void note_claim_once_warnings(std::set<std::string> &station_told, std::string_view session_name, const content::CraftRecipeData &recipes,
                              const content::CraftRecipe &recipe, bool warehouse) {
    if (const content::CraftReward *table = recipes.reward_of(recipe);
        table && !table->unknown_formula.empty() && station_told.insert("xp " + table->unknown_formula).second) {
        spdlog::info("[session] {}: {} pays its experience by the formula {}, which this "
                     "server does not work out: none is paid for it (told once)",
                     session_name, recipe.name, table->unknown_formula);
    }
    if (warehouse && station_told.insert("warehouse claim").second) {
        spdlog::info("[session] {}: a claim asks for the warehouse: none exists "
                     "yet, the items go to the bags (told once)",
                     session_name);
    }
}
struct ClaimProductStacks {
    bool ok = false;
    std::string refusal;
    int64_t created_ms = 0;
    std::function<std::string()> mint_guid;
    std::vector<svc::InventoryItem> fresh;
    std::vector<std::vector<svc::InventoryItem>> by_product;
    std::map<int64_t, std::string> shown_name;
};
void append_claim_product_stacks(const crafting::Product &p, int64_t max_stack, int32_t fallback_quality, const std::optional<content::ItemRecord> &rec,
                                 ClaimProductStacks &out) {
    out.by_product.emplace_back();
    for (int64_t left = p.quantity; left > 0;) {
        const int64_t take = std::min(left, max_stack);
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = p.item;
        it.instance_guid = out.mint_guid();
        for (const svc::InventoryItem &other : out.fresh) {
            if (other.instance_guid == it.instance_guid)
                it.instance_guid = out.mint_guid();
        }
        it.quantity = static_cast<int32_t>(take);
        it.quality = p.quality > 0 ? p.quality : fallback_quality;
        it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
        it.created_at_ms = out.created_ms;
        out.by_product.back().push_back(it);
        out.fresh.push_back(std::move(it));
        left -= take;
    }
}
std::optional<content::ItemRecord> lookup_claim_product_record(const content::ContentDb *db, int64_t item, std::map<int64_t, std::string> &shown_name) {
    std::string error;
    const auto rec = db ? db->item(item, error) : std::nullopt;
    if (rec)
        shown_name[item] = rec->display_name.empty() ? rec->name : rec->display_name;
    return rec;
}
ClaimProductStacks build_claim_product_stacks(const crafting::JobRewards &rewards, const content::ContentDb *db, const items::ItemCatalog &catalog,
                                              std::string_view recipe_name, const std::function<std::string()> &mint_guid) {
    ClaimProductStacks out;
    out.created_ms = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    out.mint_guid = mint_guid;
    for (size_t at_product = rewards.products.size(); at_product-- > 0;) {
        const crafting::Product &p = rewards.products[at_product];
        if (p.quantity < 1)
            continue;
        const auto info = catalog.info(p.item);
        if (!info) {
            out.refusal = "the item table does not know product " + std::to_string(p.item) + " of " + std::string(recipe_name);
            return out;
        }
        const auto rec = lookup_claim_product_record(db, p.item, out.shown_name);
        append_claim_product_stacks(p, std::max(1, info->max_stack), rewards.quality, rec, out);
    }
    out.ok = true;
    return out;
}
std::optional<std::vector<items::Outcome>> grant_claim_stacks(items::Inventory &inventory, std::vector<std::vector<svc::InventoryItem>> &by_product,
                                                              const items::ItemCatalog &catalog, std::string_view session_name, std::string_view recipe_name) {
    const items::Inventory before_grant = inventory;
    std::vector<items::Outcome> granted;
    for (std::vector<svc::InventoryItem> &stacks : by_product) {
        items::Outcome o = inventory.grant(stacks, catalog);
        if (!o.ok) {
            spdlog::error("[session] {}: the items of {} are not granted ({}): the job stays", session_name, recipe_name, o.refusal);
            inventory = before_grant;
            return std::nullopt;
        }
        granted.push_back(std::move(o));
    }
    return granted;
}
leveling::XpResult apply_claim_adventuring_xp(const PlayerWorld *world, int64_t adv_xp, leveling::Progress &progress, bool &changed) {
    if (!world || !world->level_curve() || adv_xp <= 0)
        return {progress, progress};
    const leveling::XpResult xp = leveling::add_xp(*world->level_curve(), world->auto_grants(), progress, adv_xp);
    progress = xp.after;
    changed = true;
    return xp;
}
std::vector<std::vector<bootstrap::ItemEvent>> build_claim_event_batches(const std::vector<items::Outcome> &granted,
                                                                         const std::function<void(std::vector<bootstrap::ItemEvent> &)> &add_toasts) {
    std::vector<std::vector<bootstrap::ItemEvent>> batches;
    for (const items::Outcome &o : granted) {
        std::vector<bootstrap::ItemEvent> batch = item_events_for(o);
        add_toasts(batch);
        batches.push_back(std::move(batch));
    }
    return batches;
}
template <typename WireT> StepBuilder make_xp_tracker_builder(int64_t total, int64_t debt, std::vector<WireT> wire) {
    return [total, debt, wire = std::move(wire)](bootstrap::PlayerBootstrap &pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_progress(total, debt))
            return std::nullopt;
        for (const auto &w : wire) {
            if (!pb.set_profession(w.leveling_type, w.xp, w.certification))
                return std::nullopt;
        }
        return pb.leveling_update();
    };
}
Clock::time_point queue_claim_product_batches(FIntrepidNetworkGUID station_guid, const std::optional<repl::CraftItemState> &claimed,
                                              const std::vector<std::vector<bootstrap::ItemEvent>> &batches, Clock::time_point now,
                                              const QueueLaterFn &queue_later) {
    constexpr auto kBetweenProducts = std::chrono::milliseconds(75);
    Clock::time_point due = now;
    for (size_t i = 0; i < batches.size(); ++i) {
        const std::vector<bootstrap::ItemEvent> &batch = batches[i];
        if (i == 0) {
            const repl::CraftItemState state = claimed ? *claimed : repl::CraftItemState{};
            queue_later(
                "craft claimed", [station_guid, state, batch](bootstrap::PlayerBootstrap &pb) { return pb.craft_claimed_answer(station_guid, state, batch); },
                due, true);
        } else {
            due += kBetweenProducts;
            queue_later("craft items", [batch](bootstrap::PlayerBootstrap &pb) { return pb.craft_batched_events(batch); }, due, true);
        }
    }
    if (batches.empty() && claimed) {
        const repl::CraftItemState state = *claimed;
        queue_later(
            "craft claimed", [station_guid, state](bootstrap::PlayerBootstrap &pb) { return pb.craft_claimed_answer(station_guid, state, {}); }, due, true);
    }
    return due;
}
struct ClaimProgressWire {
    bool xp_changed = false;
    bool leveled = false;
    int level = 1;
    StepBuilder xp_tracker;
    const professions::Professions *professions = nullptr;
    const std::vector<professions::Change> *changes = nullptr;
    const std::vector<repl::SkillOnActionbar> *skill_updates = nullptr;
};
void queue_claim_profession_level_ups(const professions::Professions *profs, const std::vector<professions::Change> *changes, Clock::time_point due,
                                      const QueueLaterFn &queue_later) {
    if (!changes || !profs || !profs->valid())
        return;
    for (const professions::Change &c : *changes) {
        const professions::Profession *pf = profs->book()->find(c.profession);
        if (!pf || !c.leveled())
            continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue_later(
            "BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap &pb) { return pb.level_up_broadcast(category, level); }, due, false);
    }
}
void queue_claim_level_updates(const ClaimProgressWire &pw, Clock::time_point due, const QueueLaterFn &queue_later) {
    if (pw.xp_changed || pw.leveled || (pw.changes && !pw.changes->empty())) {
        queue_later("XPTracker", pw.xp_tracker, due, true);
    }
    if (pw.leveled) {
        const int level = pw.level;
        queue_later(
            "BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap &pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, due,
            false);
        queue_later("pawn state", [](bootstrap::PlayerBootstrap &pb) { return pb.pawn_state_update("level and stats"); }, due, true);
    }
    queue_claim_profession_level_ups(pw.professions, pw.changes, due, queue_later);
    if (!pw.skill_updates)
        return;
    for (const repl::SkillOnActionbar &u : *pw.skill_updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap &pb) { return pb.skill_on_actionbar(u); }, due, true);
    }
}
struct ClaimLogSummary {
    std::string_view recipe_name;
    int32_t quantity = 0;
    std::string_view station_name;
    std::string_view station_key;
    const crafting::JobRewards *rewards = nullptr;
    const std::map<int64_t, std::string> *shown_name = nullptr;
    size_t fresh_count = 0;
    const professions::Professions *professions = nullptr;
    const std::vector<professions::Change> *changes = nullptr;
    const leveling::XpResult *xp = nullptr;
    bool leveled = false;
};
void append_claim_xp_log(const ClaimLogSummary &s, std::string &line) {
    if (s.changes && s.professions && s.professions->valid()) {
        for (const professions::Change &c : *s.changes) {
            if (const professions::Profession *pf = s.professions->book()->find(c.profession)) {
                line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " +
                        std::to_string(c.after.level) + ")";
            }
        }
    }
    if (!s.xp || s.rewards->adventuring_xp <= 0)
        return;
    const std::string lvl = s.leveled ? ", level " + std::to_string(s.xp->before.level) + " -> " + std::to_string(s.xp->after.level) : std::string();
    line += "; Adventuring +" + std::to_string(s.rewards->adventuring_xp) + " XP offered (debt " + std::to_string(s.xp->before.debt) + " -> " +
            std::to_string(s.xp->after.debt) + ", total " + std::to_string(s.xp->after.xp) + lvl + ")";
}
std::string format_claim_log_line(const ClaimLogSummary &s) {
    const crafting::JobRewards &rewards = *s.rewards;
    std::string line = "collected " + std::string(s.recipe_name) + " x" + std::to_string(s.quantity) + " at " + std::string(s.station_name) + " (" +
                       std::string(s.station_key) + "): ";
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        const crafting::Product &p = rewards.products[i];
        const auto it = s.shown_name->find(p.item);
        const std::string item_label = it != s.shown_name->end() ? it->second : std::to_string(p.item);
        line += (i ? ", " : "") + item_label + " x" + std::to_string(p.quantity) + " (q" + std::to_string(p.quality > 0 ? p.quality : rewards.quality) + ")";
    }
    line += " at base quality " + std::to_string(rewards.quality) + " in " + std::to_string(s.fresh_count) + " stack(s)";
    if (rewards.ratings.any()) {
        line += "; artisan ratings of the start: quantity " + std::to_string(std::lround(rewards.ratings.quantity)) + " (" +
                std::to_string(rewards.extra_units) + " extra unit(s)), rarity " + std::to_string(std::lround(rewards.ratings.rarity)) + " (" +
                std::to_string(rewards.stepped_units) + " step(s) up), experience " + std::to_string(std::lround(rewards.ratings.xp)) + " (x" +
                fmt::format("{:.3f}", rewards.xp_multiplier) + ")";
    }
    append_claim_xp_log(s, line);
    return line;
}
bool station_matches_name(const world::WorldActor &a, const content::CraftStationData *defs, std::string_view station_name) {
    if (iequals(a.station.name, station_name))
        return true;
    if (!defs)
        return false;
    const auto def = defs->defs.find(a.station.def);
    return def != defs->defs.end() && iequals(def->second.name, station_name);
}
std::vector<std::string> matching_station_keys(const std::vector<svc::ProcessingJob> &jobs, const content::CraftStationData *defs,
                                               std::string_view station_name,
                                               const std::function<const world::WorldActor *(const std::string &)> &find_station) {
    std::vector<std::string> keys;
    for (const svc::ProcessingJob &j : jobs) {
        if (std::find(keys.begin(), keys.end(), j.station_key) != keys.end())
            continue;
        const world::WorldActor *a = find_station(j.station_key);
        if (a && station_matches_name(*a, defs, station_name))
            keys.push_back(j.station_key);
    }
    return keys;
}
bool apply_cargo_effect_diff(bootstrap::PlayerBootstrap &pb, std::map<int64_t, int64_t> &cargo_effects, const CargoEffectDiff &diff, bool has_crate,
                             std::string_view session_name) {
    if (diff.change.empty())
        return false;
    if (pb.apply_ability_change(diff.change)) {
        spdlog::info("[session] {}: the crate on the back {}: {} effect item(s) "
                     "added, {} removed",
                     session_name, has_crate ? "puts its effects on the pawn" : "is gone", diff.added.size(), diff.removed.size());
        return true;
    }
    spdlog::warn("[session] {}: the pawn does not take the crate's effect items "
                 "({}): the crate is carried without them",
                 session_name, pb.error().empty() ? "keys disagree" : pb.error());
    for (const auto &a : diff.added)
        cargo_effects.erase(a.first);
    for (const auto &r : diff.removed)
        cargo_effects[r.first] = r.second;
    return false;
}
std::optional<repl::ActionbarPayload> try_auto_grant_drop_crate(actionbar::Bar &bar, const std::vector<int64_t> &known, int default_bank, int default_slot,
                                                                std::string_view session_name) {
    if (std::find(known.begin(), known.end(), content::kGuidDropCrate) == known.end() || is_drop_crate_on_bar(bar))
        return std::nullopt;
    const repl::ActionbarSlot slot = select_drop_crate_slot(bar, default_bank, default_slot);
    std::string why;
    if (bar.at(slot.bank_index, slot.slot_index) || !bar.set(slot, &why))
        return std::nullopt;
    repl::ActionbarPayload payload;
    payload.slots.push_back(slot);
    payload.auto_granted = true;
    spdlog::info("[session] {}: Drop Crate is put on the action bar (bank {}, "
                 "slot {}): a crate is carried",
                 session_name, slot.bank_index, slot.slot_index);
    return payload;
}
professions::Professions with_claimed_profession_xp(professions::Professions after, const crafting::JobRewards &made) {
    if (after.valid() && made.profession_xp > 0 && made.profession != 0)
        after.add_xp(made.profession, made.profession_xp);
    return after;
}
void erase_claimed_job(std::vector<svc::ProcessingJob> &jobs, std::map<std::string, uint8_t> &job_seen, const std::string &job_guid) {
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const svc::ProcessingJob &j) { return j.job_guid == job_guid; }), jobs.end());
    job_seen.erase(job_guid);
}
StepBuilder make_equipment_outcome_step(const items::Inventory &inventory, const items::Outcome &outcome) {
    const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory);
    const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory);
    const size_t marks = count_equipment_marks(outcome.events);
    return [held, appearance, marks](bootstrap::PlayerBootstrap &pb) -> std::optional<bootstrap::Send> {
        if (!pb.apply_equipment(held, appearance, marks))
            return std::nullopt;
        return pb.equipment_update();
    };
}
struct ClaimCommitState {
    items::Inventory *inventory = nullptr;
    const items::ItemCatalog *catalog = nullptr;
    std::string_view session_name;
    Clock::time_point now{};
    int64_t now_ms = 0;
    double sync_now_sec = 0.0;
    std::vector<svc::ProcessingJob> *jobs = nullptr;
    std::map<std::string, uint8_t> *job_seen = nullptr;
    int64_t *next_job_event_ms = nullptr;
    const PlayerWorld *world = nullptr;
    const professions::Professions *professions = nullptr;
    leveling::Progress *progress = nullptr;
    bool *progress_changed = nullptr, *inventory_dirty = nullptr, *inventory_changed = nullptr;
    size_t *levels_gained = nullptr, *items_applied = nullptr, *crafts_claimed = nullptr;
    spdlog::level::level_enum log_lvl = spdlog::level::info;
};
struct ClaimCommitEnv {
    ClaimCommitState st;
    std::function<std::vector<professions::Change>(const professions::Professions &)> set_profs;
    std::function<StepBuilder(const std::vector<professions::Change> &)> make_xp_tracker;
    std::function<void(const leveling::XpResult &, bootstrap::AbilityChange &, std::vector<repl::SkillOnActionbar> &)> finish_level;
    std::function<void(std::vector<bootstrap::ItemEvent> &)> add_toasts;
    QueueLaterFn q_later;
    std::function<void()> save_inv_fn;
    std::function<void(const char *)> save_prog_fn;
};
void commit_claimed_job(const ClaimCommitEnv &env, const ClaimTargetResult &target, const crafting::JobRewards &made, ClaimProductStacks &stacks) {
    const ClaimCommitState &st = env.st;
    const auto granted = grant_claim_stacks(*st.inventory, stacks.by_product, *st.catalog, st.session_name, target.recipe->name);
    if (!granted)
        return;
    const auto claimed = crafting::claimed_state(target.job, st.now_ms, st.sync_now_sec);
    erase_claimed_job(*st.jobs, *st.job_seen, target.job_guid);
    *st.next_job_event_ms = 0;
    *st.inventory_dirty = true;
    *st.inventory_changed = true;
    ++(*st.items_applied);
    ++(*st.crafts_claimed);
    const std::vector<professions::Change> changes =
        st.professions->valid() ? env.set_profs(with_claimed_profession_xp(*st.professions, made)) : std::vector<professions::Change>();
    const leveling::XpResult xp = apply_claim_adventuring_xp(st.world, made.adventuring_xp, *st.progress, *st.progress_changed);
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (xp.leveled()) {
        *st.levels_gained += static_cast<size_t>(xp.after.level - xp.before.level);
        bootstrap::AbilityChange re;
        env.finish_level(xp, re, skill_updates);
    }
    const Clock::time_point due =
        queue_claim_product_batches(target.station->actor.guid, claimed, build_claim_event_batches(*granted, env.add_toasts), st.now, env.q_later);
    queue_claim_level_updates({xp.changed(), xp.leveled(), st.progress->level, env.make_xp_tracker(changes), st.professions, &changes, &skill_updates}, due,
                              env.q_later);
    env.save_inv_fn();
    if (!changes.empty() || xp.changed())
        env.save_prog_fn(xp.leveled() ? "a level-up" : "a job was collected");
    spdlog::log(st.log_lvl, "[session] {}: {}", st.session_name,
                format_claim_log_line({target.recipe->name, target.job.quantity, target.station->station.name, target.key, &made, &stacks.shown_name,
                                       stacks.fresh.size(), st.professions, &changes, &xp, xp.leveled()}));
}
} // namespace
std::vector<professions::Change> PlayerSession::set_professions(const professions::Professions &after) {
    std::vector<professions::Change> changes = professions::Professions::diff(professions_, after);
    if (changes.empty())
        return changes;
    professions_ = after;
    progress_changed_ = true;
    if (stats_)
        stats_->set_profession_levels(professions_.levels_by_name());
    for (const professions::Change &c : changes) {
        if (c.leveled())
            profession_levels_gained_ += static_cast<size_t>(c.after.level - c.before.level);
    }
    return changes;
}
std::vector<PlayerSession::ProfessionWire> PlayerSession::profession_wire(const std::vector<professions::Change> &changes) const {
    std::vector<ProfessionWire> wire;
    if (!professions_.valid())
        return wire;
    const professions::Book &book = *professions_.book();
    for (const professions::Change &c : changes) {
        if (const professions::Profession *pf = book.find(c.profession))
            wire.push_back({pf->leveling_type, c.after.xp, book.tier(c.after.tier).guid});
    }
    return wire;
}
void PlayerSession::queue_profession_level_ups(Connection &conn, const std::vector<professions::Change> &changes) {
    if (!professions_.valid())
        return;
    for (const professions::Change &c : changes) {
        const professions::Profession *pf = professions_.book()->find(c.profession);
        if (!pf || !c.leveled())
            continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap &pb) { return pb.level_up_broadcast(category, level); }, conn);
    }
}
void PlayerSession::apply_outcome(Connection &conn, const items::Outcome &outcome, const char *what) {
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    queue(
        "ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap &pb) { return pb.item_events(events); }, conn,
        /*must_build=*/true);
    if (outcome.equipment_changed) {
        if (book_ && world_ && world_->content() && has_primary_or_ranged_event(outcome.events))
            follow_worn_weapon();
        apply_gear_stats();
        queue("HeldItems and AppearanceIDs", make_equipment_outcome_step(inventory_, outcome), conn,
              /*must_build=*/true);
    }
    spdlog::log(level_for(item_applied_logged_), "[session] {}: {}: {} storage event(s){}", name(), what, outcome.events.size(),
                outcome.equipment_changed ? ", the visible gear is updated" : "");
    note_cargo_crate(outcome);
    if (outcome.equipment_changed)
        sync_cargo(conn, last_now_);
    for (const items::BagChange &c : outcome.bags)
        update_resource_container(conn, c);
}
void PlayerSession::note_cargo_crate(const items::Outcome &outcome) {
    const content::ContentDb *db = world_ ? world_->content() : nullptr;
    if (!db)
        return;
    for (const items::Event &e : outcome.events) {
        if (e.type != repl::StorageEventType::Add || e.container != svc::ItemContainer::Equipment || e.slot != items::equip::CargoHold)
            continue;
        std::string error;
        const auto gear = db->item_gear(e.item.item_record_id, error);
        if (gear && has_construction_crate_tag(gear->tags))
            construction_crate_equipped(e.item.item_record_id);
    }
}
void PlayerSession::sync_cargo(Connection &conn, Clock::time_point when) {
    const svc::InventoryItem *crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if ((!crate && cargo_effects_.empty()) || !world_ || !world_->content() || !phase_in_done_)
        return;
    const std::map<int64_t, int> wanted = wanted_cargo_effects(crate, world_, name());
    const double start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(when));
    const CargoEffectDiff diff = diff_cargo_effects(cargo_effects_, wanted, next_passive_effect_id_, start_time);
    if (apply_cargo_effect_diff(pb_, cargo_effects_, diff, crate != nullptr, name())) {
        queue_later("pawn state", [](bootstrap::PlayerBootstrap &pb) { return pb.pawn_state_update("the crate"); }, conn, when);
    }
    if (!crate || !book_)
        return;
    if (const auto payload = try_auto_grant_drop_crate(bar_, known_, kDropCrateBank, kDropCrateSlot, name())) {
        queue_later(
            "ClientReceiveActionbarSlotPayload", [p = *payload](bootstrap::PlayerBootstrap &pb) { return pb.action_bar(p); }, conn, when,
            /*must_build=*/true);
        progress_changed_ = true;
        hand_over_progress("the action bar");
    }
}
void PlayerSession::run_cargo(Connection &conn, Clock::time_point now) {
    if (finished_ || failed_ || !phase_in_done_ || !join_.empty())
        return;
    if (!cargo_synced_ && room_for(4)) {
        cargo_synced_ = true;
        sync_cargo(conn, now);
    }
    if (crate_drop_due_ && now >= *crate_drop_due_) {
        crate_drop_due_.reset();
        drop_crate(conn, now);
    }
}
void PlayerSession::drop_crate(Connection &conn, Clock::time_point now) {
    const svc::InventoryItem *crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if (!crate || dead_ || !room_for(8))
        return;
    const GoodPosition at = checker_.good();
    if (mounted() || at.movement_mode != repl::kMoveWalking) {
        spdlog::info("[session] {}: General_DropCrate: the crate stays on the back ({})", name(),
                     mounted() ? "the character is mounted" : "the character is not on the ground");
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
    spdlog::info("[session] {}: General_DropCrate: the crate left the back (into the item bag: the ground crates "
                 "are not built yet)",
                 name());
}
void PlayerSession::update_resource_container(Connection &conn, const items::BagChange &change) {
    if (change.index < 0 || change.index >= items::kResourceContainers)
        return;
    const size_t i = static_cast<size_t>(change.index);
    const items::ResourceGrid &grid = inventory_.grid(items::resource_container(change.index));
    const bool present = change.open && !grid.empty();
    const bootstrap::ResourceBagSetup bag{present, present ? grid.columns() : 0, present ? grid.rows() : 0,
                                          present ? grid.override_matrix() : std::vector<std::vector<int32_t>>{}};
    const FIntrepidNetworkGUID guid = (present && world_) ? world_->mint_guid() : pb_.setup().guids.resource_storage[i];
    const std::string state_desc = present ? std::to_string(bag.columns) + " x " + std::to_string(bag.rows) + " opens now" : std::string("closes now");
    spdlog::info("[session] {}: the resource bag in Equipment slot {} {}: container Resource{} {}", name(), items::equip::ResourceBag0 + change.index,
                 present ? "is worn" : "is taken off", change.index, state_desc);
    const int index = change.index;
    queue(
        "the resource containers",
        [index, bag, guid](bootstrap::PlayerBootstrap &pb) -> std::optional<bootstrap::Send> {
            return pb.set_resource_container(index, bag, guid) ? pb.resource_containers_update() : std::nullopt;
        },
        conn, /*must_build=*/true);
    if (present) {
        const bootstrap::StorageKind kind = bootstrap::resource_kind(index);
        queue("ClientReceiveStoragePayload", [kind](bootstrap::PlayerBootstrap &pb) { return pb.storage_payload(kind, Role::Pawn); }, conn);
    }
}
void PlayerSession::on_processing_request(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
    if (dead_)
        return;
    const auto req = repl::parse_processing_request(rpc.call);
    if (!req) {
        if (!std::exchange(claim_unreadable_logged_, true)) {
            spdlog::warn("[session] {}: a ServerSendProcessingStationRequest does not read ({} parameter bits): "
                         "ignored (further ones are not logged)",
                         name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerSendProcessingStationRequest", now))
        return;
    const auto refuse = [&](const std::string &why) {
        ++claims_refused_;
        spdlog::log(level_for(claim_refused_logged_, 30), "[session] {}: a request at a station is refused (nothing is changed): {}", name(), why);
    };
    const ClaimTargetResult target = validate_claim_target(*req, {registry_, view_.get(), world_, checker_.good(), &jobs_, wall_ms()});
    if (!target.ok)
        return refuse(target.refusal);
    const std::optional<crafting::JobRewards> made = job_rewards(target.job);
    if (!made)
        return refuse("recipe " + std::to_string(target.job.recipe_guid) + " of the job is none of the data's");
    note_claim_once_warnings(station_told_, name(), *world_->craft_recipes(), *target.recipe, req->warehouse);
    ClaimProductStacks stacks = build_claim_product_stacks(*made, world_->content(), item_catalog(), target.recipe->name, [this]() { return fresh_guid(); });
    if (!stacks.ok)
        return refuse(stacks.refusal);
    if (!stacks.fresh.empty() && !inventory_.can_grant(stacks.fresh, item_catalog())) {
        say(conn, "Not enough inventory space.", now);
        return refuse("the bags have no room for the " + std::to_string(stacks.fresh.size()) + " stack(s) of " + target.recipe->name +
                      " (the player was told; the job stays)");
    }
    if (!room_for(12 + made->products.size()))
        return;
    commit_claimed_job(
        {{&inventory_, &item_catalog(), name(), now, wall_ms(), sync_now(now), &jobs_, &job_seen_, &next_job_event_ms_, world_, &professions_, &progress_,
          &progress_changed_, &inventory_dirty_, &inventory_changed_, &levels_gained_, &items_applied_, &crafts_claimed_, level_for(claim_logged_, 30)},
         [this](const professions::Professions &p) { return set_professions(p); },
         [this](const auto &ch) { return make_xp_tracker_builder(progress_.xp, progress_.debt, profession_wire(ch)); },
         [this, now](const leveling::XpResult &x, auto &re, auto &su) { finish_level_up(x, false, now, re, su); },
         [this](auto &b) { add_item_toasts(b, false); },
         [this, &conn](const char *what, StepBuilder b, Clock::time_point t, bool must) { queue_later(what, std::move(b), conn, t, must); },
         [this]() { hand_over_inventory("a job was collected", /*periodic=*/false); },
         [this](const char *why) { hand_over_progress(why); }},
        target, *made, stacks);
}
size_t PlayerSession::gm_finish_jobs(Connection &conn, Clock::time_point now, const std::string &station_name, const std::string &by) {
    const int64_t now_ms = wall_ms();
    size_t finished = 0;
    if (station_name.empty()) {
        finished = crafting::finish_now(jobs_, std::string(), now_ms);
    } else {
        const std::vector<std::string> keys =
            matching_station_keys(jobs_, world_ ? world_->station_data() : nullptr, station_name, [this](const std::string &k) { return station_by_key(k); });
        for (const std::string &key : keys)
            finished += crafting::finish_now(jobs_, key, now_ms);
    }
    if (finished == 0)
        return 0;
    crafts_finished_ += finished;
    next_job_event_ms_ = 0;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    hand_over_inventory("jobs were finished by a game master",
                        /*periodic=*/false);
    spdlog::info("[session] {}: {} job(s) finished at once by {}{}", name(), finished, by, station_name.empty() ? "" : " at " + station_name);
    run_crafting(conn, now);
    return finished;
}
} // namespace worldd
} // namespace aoc
