// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "content/content_db.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "items/acquired.h"
#include "items/currency.h"
#include "session/player_session.h"
#include "world/city_nodes.h"
namespace aoc {
namespace worldd {
namespace {
using StepBuilder = std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap &)>;
struct StationCheckResult {
    bool ok = false;
    bool quick = true;
    std::string refusal;
    const world::WorldActor *actor = nullptr;
};
StationCheckResult validate_start_craft_station(const repl::StartCraft &req, const world::ActorRegistry *registry, const ActorView *view,
                                                const PlayerWorld *world, const GoodPosition &at) {
    StationCheckResult out;
    if (!registry || !view || !world) {
        out.refusal = "the world has no actors";
        return out;
    }
    const world::WorldActor *a = registry->find(req.station.ObjectId);
    if (!a || a->kind != world::ActorKind::Station) {
        out.refusal = "the object named is no station of this world";
        return out;
    }
    out.quick = !a->station.processing;
    if (view->state_of(a->id) != world::ClientState::Awake) {
        out.refusal = "the client does not have that station open";
        return out;
    }
    const double dx = a->x - at.x;
    const double dy = a->y - at.y;
    const double dz = a->z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) {
        out.refusal = "the station " + a->station.name + " is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away";
        return out;
    }
    out.ok = true;
    out.actor = a;
    return out;
}
struct CraftFactsInput {
    const PlayerWorld *world = nullptr;
    const world::WorldActor *actor = nullptr;
    const std::vector<int64_t> *taught = nullptr;
    const professions::Professions *professions = nullptr;
    const items::Inventory *inventory = nullptr;
    const items::Wallet *wallet = nullptr;
    std::function<double(int64_t)> rating_stat;
    const std::vector<svc::ProcessingJob> *jobs = nullptr;
    int64_t now_ms = 0;
};
void populate_station_node_facts(const PlayerWorld *world, int64_t node_guid, crafting::StartFacts &facts) {
    facts.node.guid = node_guid;
    if (!world || !world->city_nodes() || !world->node_wire())
        return;
    const world::CityNodeInfo *node = world->city_nodes()->by_guid(node_guid);
    if (!node)
        return;
    facts.node.level = world->city_nodes()->level_of(node->guid);
    facts.node.biome = spawns::node_site_of(*world->city_nodes(), *world->node_wire(), *node, facts.node.level).biome;
}
crafting::StartFacts build_station_craft_facts(const CraftFactsInput &in, const content::CraftStationDef &station, const std::string &key) {
    crafting::StartFacts facts;
    facts.recipes = in.world ? in.world->craft_recipes() : nullptr;
    facts.station = &station;
    facts.quick = false;
    facts.taught = [taught = in.taught](int64_t recipe) { return taught && std::find(taught->begin(), taught->end(), recipe) != taught->end(); };
    facts.tax = crafting::station_tax(in.actor->station.tax_tag);
    populate_station_node_facts(in.world, in.actor->station.node, facts);
    facts.tier_of = [prof = in.professions](int64_t profession) { return prof && prof->valid() ? prof->tier(profession) : 0; };
    facts.inventory = in.inventory;
    facts.rarity_facts = [w = in.world](int64_t item) { return w->item_rarity_facts(item); };
    facts.copper = in.wallet ? in.wallet->copper() : 0;
    facts.rating_stat = in.rating_stat;
    if (in.jobs) {
        facts.unfinished_here = crafting::unfinished_at(*in.jobs, key, in.now_ms);
        facts.held_here = crafting::held_at(*in.jobs, key);
    }
    return facts;
}
struct StationPlanResult {
    bool ok = false;
    std::string refusal;
    std::string key;
    double tax_percent = 0.0;
    crafting::Plan plan;
};
StationPlanResult plan_station_job(const repl::StartCraft &req, const CraftFactsInput &in) {
    StationPlanResult out;
    out.key = crafting::station_key(in.actor->station.seed);
    if (out.key.empty()) {
        out.refusal = in.actor->station.name + " has no key that survives a restart: no job can be saved there";
        return out;
    }
    const content::CraftStationData *defs = in.world ? in.world->station_data() : nullptr;
    const content::CraftRecipeData *recipes = in.world ? in.world->craft_recipes() : nullptr;
    if (!defs || !recipes) {
        out.refusal = "world has no station definitions or recipes (an older content.db)";
        return out;
    }
    const auto def = defs->defs.find(in.actor->station.def);
    if (in.actor->station.def == 0 || def == defs->defs.end()) {
        out.refusal = "the data holds no definition for " + in.actor->station.name;
        return out;
    }
    if (in.jobs && in.jobs->size() >= crafting::kMaxJobsHeld) {
        out.refusal = "the character holds " + std::to_string(in.jobs->size()) + " jobs already";
        return out;
    }
    const crafting::StartFacts facts = build_station_craft_facts(in, def->second, out.key);
    out.tax_percent = facts.tax.percent;
    out.plan = crafting::plan_start(req, facts);
    out.ok = out.plan.ok;
    out.refusal = out.plan.refusal;
    return out;
}
items::Outcome take_planned_materials(items::Inventory &inventory, const std::vector<crafting::Take> &takes) {
    const items::Inventory kept = inventory;
    items::Outcome taken;
    taken.ok = true;
    for (const crafting::Take &t : takes) {
        items::Outcome o = inventory.take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first);
        if (!o.ok) {
            inventory = kept;
            return o;
        }
        taken.events.insert(taken.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (taken.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
        inventory = kept;
        taken.ok = false;
        taken.refusal = "the job takes from " + std::to_string(taken.events.size()) + " stacks: more events than one answer holds";
    }
    return taken;
}
struct StationJobStart {
    bool ok = false;
    std::string refusal;
    int64_t before_copper = 0;
    StationPlanResult planned;
    items::Outcome taken;
    svc::ProcessingJob job;
    repl::CraftItemState state{};
};
StationJobStart start_station_job(const repl::StartCraft &req, const CraftFactsInput &in, items::Inventory &inventory, items::Wallet &wallet,
                                  double sync_time) {
    StationJobStart out;
    out.planned = plan_station_job(req, in);
    if (!out.planned.ok) {
        out.refusal = out.planned.refusal;
        return out;
    }
    const items::Inventory kept = inventory;
    out.taken = take_planned_materials(inventory, out.planned.plan.takes);
    if (!out.taken.ok) {
        out.refusal = out.taken.refusal;
        return out;
    }
    out.job = crafting::make_job(*in.jobs, out.planned.key, out.planned.plan.recipe->guid, req.quantity, out.planned.plan.duration_ms, req.transaction,
                                 crafting::job_resources_of(out.planned.plan.paid), in.now_ms, out.planned.plan.ratings);
    const auto state = crafting::to_state(out.job, in.now_ms, sync_time);
    if (!state) {
        inventory = kept;
        out.refusal = "the job's guids do not read";
        return out;
    }
    out.before_copper = wallet.copper();
    if (out.planned.plan.cost > 0)
        wallet.pay(out.planned.plan.cost);
    out.ok = true;
    out.state = *state;
    return out;
}
std::string format_start_craft_ratings(const crafting::Plan &plan) {
    if (!plan.ratings.any())
        return {};
    return " (x" + std::to_string(plan.time_multiplier).substr(0, 5) + " of its base time with the ratings: time " +
           std::to_string(static_cast<int64_t>(std::lround(plan.ratings.time))) + ", quantity " +
           std::to_string(static_cast<int64_t>(std::lround(plan.ratings.quantity))) + ", rarity " +
           std::to_string(static_cast<int64_t>(std::lround(plan.ratings.rarity))) + ", experience " +
           std::to_string(static_cast<int64_t>(std::lround(plan.ratings.xp))) + ")";
}
void queue_station_craft_started(const std::vector<bootstrap::ItemEvent> &events, FIntrepidNetworkGUID station_guid, int64_t cost,
                                 const repl::CraftItemState &shown, const std::function<void(const char *, StepBuilder, std::chrono::milliseconds)> &q_after) {
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() +
                                                         static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        q_after("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); }, std::chrono::milliseconds(0));
    }
    constexpr std::chrono::milliseconds kTickAfterRemoves{40};
    q_after("OwnerInfo", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, kTickAfterRemoves);
    const int64_t delta = -cost;
    q_after(
        "craft started",
        [station_guid, delta, shown](bootstrap::PlayerBootstrap &pb) { return pb.craft_started_answer(station_guid, items::kMoneyTier, delta, shown); },
        kTickAfterRemoves);
}
struct CraftRefusalCounters {
    size_t *quick_refused = nullptr;
    size_t *quick_refused_logged = nullptr;
    size_t *crafts_refused = nullptr;
    size_t *craft_refused_logged = nullptr;
};
spdlog::level::level_enum log_level(size_t &count, size_t limit) {
    return ++count <= limit ? spdlog::level::info : spdlog::level::debug;
}
bool record_start_craft_refusal(bool quick, bool quick_active, std::string_view session_name, const std::string &why, const CraftRefusalCounters &c) {
    constexpr size_t kCraftLogged = 30;
    if (quick) {
        ++(*c.quick_refused);
        spdlog::log(log_level(*c.quick_refused_logged, kCraftLogged), "[session] {}: a craft is refused (nothing is taken): {}{}", session_name, why,
                    quick_active ? " (the craft that runs goes on)" : "");
        return !quick_active;
    }
    ++(*c.crafts_refused);
    spdlog::log(log_level(*c.craft_refused_logged, kCraftLogged),
                "[session] {}: a start at a station is refused (nothing is "
                "taken, nothing is sent): {}",
                session_name, why);
    return false;
}
struct CraftGroup {
    std::string key;
    std::vector<repl::CraftItemState> states;
};
struct CraftUpdateBatch {
    std::vector<CraftGroup> groups;
    std::vector<std::pair<std::string, uint8_t>> told;
    size_t completed = 0;
    size_t began = 0;
};
void append_group_state(std::vector<CraftGroup> &groups, const std::string &key, const repl::CraftItemState &state) {
    auto g = std::find_if(groups.begin(), groups.end(), [&](const CraftGroup &x) { return x.key == key; });
    if (g == groups.end()) {
        groups.push_back({key, {}});
        g = groups.end() - 1;
    }
    g->states.push_back(state);
}
std::optional<std::pair<uint8_t, repl::CraftItemState>> changed_job_state(
    const svc::ProcessingJob &j, std::map<std::string, uint8_t> &job_seen, int64_t now_ms, double sync,
    const std::function<std::optional<crafting::JobRewards>(const svc::ProcessingJob &)> &rewards_fn) {
    const uint8_t status = crafting::status_at(j, now_ms);
    const auto seen = job_seen.find(j.job_guid);
    if (seen == job_seen.end()) {
        job_seen[j.job_guid] = status;
        return std::nullopt;
    }
    if (seen->second == status)
        return std::nullopt;
    const std::optional<crafting::JobRewards> made = status == crafting::kComplete ? rewards_fn(j) : std::nullopt;
    const auto state = crafting::to_state(j, now_ms, sync, made ? &*made : nullptr);
    if (!state || state->status != status)
        return std::nullopt;
    return std::pair{status, *state};
}
CraftUpdateBatch collect_changed_craft_groups(const std::vector<svc::ProcessingJob> &jobs, std::map<std::string, uint8_t> &job_seen, int64_t now_ms,
                                              double sync, const std::function<std::optional<crafting::JobRewards>(const svc::ProcessingJob &)> &rewards_fn) {
    CraftUpdateBatch batch;
    for (const svc::ProcessingJob &j : jobs) {
        const auto ch = changed_job_state(j, job_seen, now_ms, sync, rewards_fn);
        if (!ch)
            continue;
        append_group_state(batch.groups, j.station_key, ch->second);
        batch.told.emplace_back(j.job_guid, ch->first);
        (ch->first == crafting::kComplete ? batch.completed : batch.began) += 1;
    }
    return batch;
}
size_t count_craft_group_steps(const std::vector<CraftGroup> &groups) {
    size_t steps = 1;
    for (const CraftGroup &g : groups) {
        steps += (g.states.size() + bootstrap::kMaxEventsPerChain - 1) / bootstrap::kMaxEventsPerChain;
    }
    return steps;
}
size_t send_awake_craft_groups(const std::vector<CraftGroup> &groups, const ActorView *view,
                               const std::function<const world::WorldActor *(const std::string &)> &find_station,
                               const std::function<void(FIntrepidNetworkGUID, std::vector<repl::CraftItemState>)> &send_part) {
    size_t sent = 0;
    for (const CraftGroup &g : groups) {
        const world::WorldActor *a = find_station(g.key);
        if (!a || !view || view->state_of(a->id) != world::ClientState::Awake)
            continue;
        const FIntrepidNetworkGUID guid = a->actor.guid;
        for (size_t at = 0; at < g.states.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<repl::CraftItemState> part(g.states.begin() + static_cast<std::ptrdiff_t>(at),
                                                         g.states.begin() +
                                                             static_cast<std::ptrdiff_t>(std::min(g.states.size(), at + bootstrap::kMaxEventsPerChain)));
            send_part(guid, part);
        }
        sent += g.states.size();
    }
    return sent;
}
template <typename SetT, typename MapT, typename RewardFn> struct StationOpenCtx {
    std::string player_name;
    SetT *station_told;
    const std::vector<svc::ProcessingJob> *jobs;
    MapT *job_seen;
    int64_t now_ms;
    double effect_now;
    RewardFn reward_fn;
};
template <typename SetT, typename MapT, typename RewardFn>
std::vector<repl::CraftItemState> open_station_states(const world::WorldActor &a, const StationOpenCtx<SetT, MapT, RewardFn> &ctx) {
    if (ctx.station_told->insert("known recipes").second) {
        spdlog::info("[session] {}: a station window opens with the client's own recipe "
                     "lists: the server sends no known recipes (told once)",
                     ctx.player_name);
    }
    if (a.station.def == 0 && ctx.station_told->insert("no def " + a.station.name).second) {
        spdlog::info("[session] {}: the station {} has no definition in the data: its "
                     "window opens with the client's own recipes and the default tax",
                     ctx.player_name, a.station.name);
    }
    const std::string key = crafting::station_key(a.station.seed);
    if (key.empty())
        return {};
    std::vector<repl::CraftItemState> states = crafting::states_at(*ctx.jobs, key, ctx.now_ms, ctx.effect_now, ctx.reward_fn);
    for (const repl::CraftItemState &st : states) {
        if (const std::string job_id = items::guid_hex(st.job); ctx.job_seen->count(job_id) != 0)
            (*ctx.job_seen)[job_id] = st.status;
    }
    return states;
}
void log_unreadable_start_craft_once(bool &logged, std::string_view player_name, size_t param_bits) {
    if (!std::exchange(logged, true)) {
        spdlog::warn("[session] {}: a ServerStartCraft does not read ({} parameter bits): "
                     "ignored, and the client is told to stop crafting (further ones are "
                     "not logged)",
                     player_name, param_bits);
    }
}
struct StationJobFinishCtx {
    std::string player_name;
    spdlog::level::level_enum log_lvl;
    int64_t copper_after;
    std::function<void(std::vector<bootstrap::ItemEvent> &)> add_removal_toasts;
    std::function<void(const char *, StepBuilder, std::chrono::milliseconds)> queue_later;
};
void finish_station_job_start(const repl::StartCraft &req, const world::WorldActor &actor, const StationJobStart &started, const StationJobFinishCtx &ctx) {
    const crafting::Plan &plan = started.planned.plan;
    std::vector<bootstrap::ItemEvent> events = item_events_for(started.taken);
    ctx.add_removal_toasts(events);
    queue_station_craft_started(events, actor.actor.guid, plan.cost, started.state, ctx.queue_later);
    spdlog::log(ctx.log_lvl,
                "[session] {}: started {} x{} at {} ({}): {} copper ({} each with the "
                "tax of {:.2f}, {} before it) leaves the purse ({} -> {}), {} stack(s) "
                "leave the material bags, the job runs {:.0f} s{} {}",
                ctx.player_name, plan.recipe->name, req.quantity, actor.station.name, started.planned.key, plan.cost, plan.unit_taxed,
                started.planned.tax_percent, plan.unit_cost, started.before_copper, ctx.copper_after, started.taken.events.size(), plan.duration_ms / 1000.0,
                format_start_craft_ratings(plan), started.state.status == crafting::kPending ? "after the one before it" : "from now");
}
} // namespace
void PlayerSession::on_station_interact(Connection &conn, const world::WorldActor &a, Clock::time_point now) {
    constexpr size_t kStationLogged = 30;
    const auto ignore = [&](const std::string &why) {
        ++interact_ignored_;
        spdlog::log(level_for(station_ignored_logged_, kStationLogged), "[session] {}: a click on the station {} is ignored: {}", name(), a.station.name, why);
    };
    if (view_->state_of(a.id) != world::ClientState::Awake)
        return ignore("the client does not have that station open");
    const GoodPosition at = checker_.good();
    const double dx = a.x - at.x;
    const double dy = a.y - at.y;
    const double dz = a.z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= crafting::kStationRangeCm) {
        return ignore("the station is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away (a window opens within " +
                      std::to_string(static_cast<int>(crafting::kStationRangeCm / 100.0)) + " m)");
    }
    if (!room_for(1))
        return;
    const protocol::repl::StationTax tax = crafting::station_tax(a.station.tax_tag);
    const FIntrepidNetworkGUID guid = a.actor.guid;
    ++station_opens_;
    const std::vector<repl::CraftItemState> states = open_station_states(a, StationOpenCtx{name(), &station_told_, &jobs_, &job_seen_, wall_ms(), sync_now(now),
                                                                                           [this](const svc::ProcessingJob &j) { return job_rewards(j); }});
    spdlog::log(level_for(station_logged_, kStationLogged),
                "[session] {}: opens the window of {} in node {} ({} m away, tax "
                "{:.2f} of the price, the owning node keeps {:.2f}; {} "
                "job(s) of the player's here)",
                name(), a.station.name, a.station.node, static_cast<int>(distance / 100.0), tax.percent, tax.owning_share, states.size());
    queue(
        "ClientOpenCraftingStationUI", [guid, tax, states](bootstrap::PlayerBootstrap &pb) { return pb.open_station(guid, tax, states); }, conn,
        /*must_build=*/true);
}
int64_t PlayerSession::wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
double PlayerSession::sync_now(Clock::time_point now) {
    return bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now));
}
bool PlayerSession::sync_active_stations(int64_t now_ms) {
    const std::vector<std::array<int32_t, 4>> stations = crafting::active_station_words(jobs_, now_ms);
    if (stations.empty() && !active_told_)
        return false;
    if (stations == active_words_ && active_told_)
        return false;
    if (!pb_.set_active_stations(stations))
        return false;
    active_words_ = stations;
    active_told_ = true;
    return true;
}
void PlayerSession::on_start_craft(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
    if (dead_)
        return;
    const auto req = repl::parse_start_craft(rpc.call);
    if (!req) {
        log_unreadable_start_craft_once(craft_unreadable_logged_, name(), rpc.call.param_bits);
        if (!quick_.active && within_budget(craft_budget_, "ServerStartCraft", now) && room_for(2)) {
            queue(
                "ClientInterruptCraft", [](bootstrap::PlayerBootstrap &pb) { return pb.craft_interrupted(); }, conn,
                /*must_build=*/true);
        }
        return;
    }
    if (!within_budget(craft_budget_, "ServerStartCraft", now) || !room_for(8))
        return;
    const StationCheckResult st = validate_start_craft_station(*req, registry_, view_.get(), world_, checker_.good());
    const auto refuse = [&](const std::string &why) {
        if (record_start_craft_refusal(st.quick, quick_.active, name(), why,
                                       {&quick_refused_, &quick_refused_logged_, &crafts_refused_, &craft_refused_logged_})) {
            queue(
                "ClientInterruptCraft", [](bootstrap::PlayerBootstrap &pb) { return pb.craft_interrupted(); }, conn,
                /*must_build=*/true);
        }
    };
    if (!st.ok)
        return refuse(st.refusal);
    if (st.quick) {
        if (quick_.active)
            return refuse("a craft of this player runs already (at " + quick_.station_name + "): a second start does nothing");
        return on_quick_start(conn, *req, *st.actor, now);
    }
    const int64_t now_ms = wall_ms();
    StationJobStart started = start_station_job(
        *req, {world_, st.actor, &taught_, &professions_, &inventory_, &wallet_, [this](int64_t s) { return artisan_stat(s); }, &jobs_, now_ms}, inventory_,
        wallet_, sync_now(now));
    if (!started.ok)
        return refuse(started.refusal);
    job_seen_[started.job.job_guid] = started.state.status;
    jobs_.push_back(std::move(started.job));
    next_job_event_ms_ = 0;
    inventory_dirty_ = inventory_changed_ = true;
    ++items_applied_;
    ++crafts_started_;
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    sync_active_stations(now_ms);
    finish_station_job_start(
        *req, *st.actor, started,
        {name(), level_for(craft_logged_, 30), wallet_.copper(), [this](auto &evs) { add_item_toasts(evs, /*removals=*/true); },
         [this, &conn, now](const char *w, StepBuilder b, std::chrono::milliseconds d) { queue_later(w, std::move(b), conn, now + d, /*must_build=*/true); }});
}
std::optional<crafting::JobRewards> PlayerSession::job_rewards(const svc::ProcessingJob &job) const {
    const content::CraftRecipeData *recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!recipes)
        return std::nullopt;
    return crafting::roll_job(*recipes, job, progress_.level, crafting::job_ratings(job), [this](int64_t item) { return world_->item_rarity_facts(item); });
}
double PlayerSession::artisan_stat(int64_t stat) const {
    const stats::StatValue *v = stats_ ? stats_->find(stat) : nullptr;
    return v ? static_cast<double>(v->value) : 0.0;
}
crafting::Ratings PlayerSession::artisan_ratings(int64_t profession) const {
    const content::CraftRecipeData *recipes = world_ ? world_->craft_recipes() : nullptr;
    if (!recipes || !stats_ || !stats_->started())
        return {};
    return crafting::ratings_of(recipes->artisan, profession, [this](int64_t stat) { return artisan_stat(stat); });
}
const world::WorldActor *PlayerSession::station_by_key(const std::string &key) const {
    const auto seed = crafting::station_seed(key);
    if (!seed || !registry_)
        return nullptr;
    const world::WorldActor *found = nullptr;
    registry_->for_each([&](const world::WorldActor &a) {
        if (!found && a.kind == world::ActorKind::Station && a.station.seed == *seed)
            found = &a;
    });
    return found;
}
int64_t PlayerSession::next_job_event(int64_t now_ms) const {
    int64_t next = INT64_MAX;
    for (const svc::ProcessingJob &j : jobs_) {
        const auto seen = job_seen_.find(j.job_guid);
        if (seen == job_seen_.end() || seen->second != crafting::status_at(j, now_ms))
            return 0;
        if (j.started_at_ms > now_ms)
            next = std::min(next, j.started_at_ms);
        if (j.end_ms() > now_ms)
            next = std::min(next, j.end_ms());
    }
    return next;
}
void PlayerSession::run_crafting(Connection &conn, Clock::time_point now) {
    if (failed_ || finished_ || !join_.empty() || jobs_.empty())
        return;
    const int64_t now_ms = wall_ms();
    if (now_ms < next_job_event_ms_)
        return;
    const CraftUpdateBatch batch =
        collect_changed_craft_groups(jobs_, job_seen_, now_ms, sync_now(now), [this](const svc::ProcessingJob &j) { return job_rewards(j); });
    if (batch.groups.empty()) {
        next_job_event_ms_ = next_job_event(now_ms);
        return;
    }
    if (!room_for(count_craft_group_steps(batch.groups)))
        return;
    if (sync_active_stations(now_ms)) {
        queue(
            "OwnerInfo", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, conn,
            /*must_build=*/true);
    }
    const size_t sent = send_awake_craft_groups(
        batch.groups, view_.get(), [this](const std::string &k) { return station_by_key(k); },
        [this, &conn](FIntrepidNetworkGUID guid, std::vector<repl::CraftItemState> part) {
            queue(
                "craft update", [guid, part = std::move(part)](bootstrap::PlayerBootstrap &pb) { return pb.craft_states_update(guid, part); }, conn,
                /*must_build=*/true);
        });
    for (const auto &[guid, status] : batch.told)
        job_seen_[guid] = status;
    next_job_event_ms_ = next_job_event(now_ms);
    spdlog::log(level_for(craft_update_logged_, 30),
                "[session] {}: {} job(s) ended and {} began: {} state(s) sent to "
                "the window{}",
                name(), batch.completed, batch.began, sent,
                sent == batch.told.size() ? ""
                                          : " (the rest are at stations the client does not have open: "
                                            "the next open shows them)");
}
} // namespace worldd
} // namespace aoc
