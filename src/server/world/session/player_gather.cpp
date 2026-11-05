// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "bootstrap/character_setup.h"
#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "gather/rules.h"
#include "items/acquired.h"
#include "session/player_session.h"
#include "world/interaction_state.h"
namespace aoc {
namespace worldd {
namespace repl = ::aoc::protocol::repl;
namespace {
using StepBuilder = std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap &)>;
using QueueFn = std::function<void(const char *, StepBuilder, bool)>;
constexpr double kChannelSlackCm = 100.0;
constexpr double kChannelDriftCm = 60.0;
constexpr std::chrono::milliseconds kPileDelay{100};
constexpr std::chrono::seconds kCalloutLife{3};
constexpr const char *kNoRoomText = "Unable to loot, not enough inventory space.";
constexpr int32_t kCalloutType = 1;
constexpr int32_t kCalloutPointSize = 32;
constexpr uint32_t kChannelingFlags = 6;
constexpr uint32_t kChannelingOverFlags = 0x106;
repl::ChannelingData channeling_over(const protocol::emit::FIntrepidNetworkGUID &target) {
    repl::ChannelingData d;
    d.target = target;
    d.flags = kChannelingOverFlags;
    return d;
}
std::string join_names(const std::vector<std::string> &v) {
    std::string out;
    for (const std::string &s : v)
        out += (out.empty() ? "" : ", ") + s;
    return out;
}
struct ChannelSpec {
    const world::WorldActor *target = nullptr;
    const content::GatherDefRow *def = nullptr;
    std::string what;
    const char *tag = "gather", *noun = "harvest";
    bool hunt = false, gives_nothing = false;
};
struct ChannelPrecheckResult {
    bool ok = false;
    const char *kind = nullptr;
    std::string why;
    double distance = 0.0;
    gather::ToolInfo tool;
    const svc::InventoryItem *worn = nullptr;
};
ChannelPrecheckResult fail_precheck(const char *kind, std::string why, double distance = 0.0) {
    return {false, kind, std::move(why), distance, {}, nullptr};
}
ChannelPrecheckResult check_node_board_state(const ChannelSpec &spec, gather::NodeState state, double distance) {
    if (state == gather::NodeState::Channeling)
        return fail_precheck(spec.hunt ? "corpse busy" : "node busy", "another player is harvesting it", distance);
    if (state == gather::NodeState::Gathered) {
        return fail_precheck(spec.hunt ? "corpse hunted" : "node gathered", spec.hunt ? "it was hunted already" : "it was gathered and has not returned yet",
                             distance);
    }
    return {true, nullptr, {}, distance, {}, nullptr};
}
repl::ChannelingData build_active_channeling(const world::WorldActor &node, const svc::InventoryItem &worn, double seconds, double world_time,
                                             double session_sec) {
    repl::ChannelingData channeling;
    channeling.target = node.actor.guid;
    channeling.flags = kChannelingFlags;
    channeling.active = true;
    channeling.start_time = std::floor(bootstrap::effect_clock(world_time + session_sec));
    channeling.timed = true;
    channeling.duration = static_cast<float>(seconds);
    channeling.tool_record = worn.item_record_id;
    if (const auto guid = items::guid_of(worn.instance_guid))
        channeling.tool_guid = *guid;
    return channeling;
}
std::string format_channel_start_log(const ChannelSpec &spec, const professions::Professions &profs, const gather::ToolInfo &tool, double seconds,
                                     double distance) {
    const content::GatherDefRow &def = *spec.def;
    const professions::Profession *pf = profs.valid() ? profs.book()->find(def.profession_guid) : nullptr;
    const std::string tier_name = profs.valid() ? profs.book()->tier(def.cert_tier).name : std::string("?");
    const std::string prof_name = pf ? pf->name : std::string("?");
    const std::string tool_name = tool.tool ? tool.tool->item_name : std::string("?");
    const char *suffix = spec.gives_nothing ? "; this kind gives no item (RETIRED_ or empty table): the experience only" : "";
    return fmt::format("starts {} {}: a {} {} {}, with {}, a channel of {:.2f} s (base {:.2f} s), {:.0f} cm from it{}", spec.hunt ? "hunting" : "harvesting",
                       spec.what, tier_name, prof_name, spec.hunt ? "kill" : "node", tool_name, seconds, def.base_duration, distance, suffix);
}
struct ActiveGatherSpec {
    std::string_view tool_guid;
    const content::GatherDefRow *def = nullptr;
    double start_x = 0.0, start_y = 0.0;
};
struct GatherChannelCheck {
    bool cancel = false;
    std::string why;
    bool movement_interrupt = false;
};
GatherChannelCheck check_active_gather_channel(const ActiveGatherSpec &run, const world::WorldActor *node, const svc::InventoryItem *tool,
                                               const GoodPosition &at, std::pair<bool, bool> dead_or_mounted) {
    if (!node)
        return {true, "the node left the world"};
    if (dead_or_mounted.first)
        return {true, "the player is dead"};
    if (dead_or_mounted.second)
        return {true, "the player is on a mount"};
    if (!tool || tool->instance_guid != run.tool_guid)
        return {true, tool ? "the tool was swapped for another" : "the tool was taken off"};
    if (!(tool->durability > 0))
        return {true, "the tool broke"};
    const double distance = std::hypot(node->x - at.x, node->y - at.y);
    const double reach = gather::reach_cm(*run.def);
    if (distance > reach + kChannelSlackCm) {
        return {true,
                "the player moved away (" + std::to_string(static_cast<int>(distance)) + " cm from the node, the interaction reaches " +
                    std::to_string(static_cast<int>(reach)) + " cm)",
                true};
    }
    const double drift = std::hypot(at.x - run.start_x, at.y - run.start_y);
    if (drift > kChannelDriftCm) {
        return {true,
                "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) +
                    " cm since the channel began (a harvest needs standing still; at most " + std::to_string(static_cast<int>(kChannelDriftCm)) + " cm)",
                true};
    }
    return {};
}
std::string finish_gather_aftermath_and_tail(bool hunt, PlayerWorld *npc_world, uint64_t node_id, std::chrono::duration<double> respawn) {
    if (!hunt)
        return "; the node returns in " + std::to_string(static_cast<int>(respawn.count())) + " s";
    combat::Aftermath *after = npc_world ? npc_world->aftermath() : nullptr;
    if (after)
        after->hunt_done(node_id, npc_world->world_clock());
    return "; the corpse goes in " + std::to_string(static_cast<int>(after ? after->config().hunt_done_seconds : 4.0)) + " s";
}
struct ToolWearResult {
    bool wore = false;
    float before = 0.0f, after = 0.0f;
};
ToolWearResult wear_harvest_tool(items::Inventory &inventory, int tool_slot, const std::string &tool_guid, double durability_loss) {
    ToolWearResult out;
    const svc::InventoryItem *tool = inventory.at(svc::ItemContainer::Equipment, tool_slot);
    if (tool && tool->instance_guid == tool_guid) {
        out.before = tool->durability;
        out.after = static_cast<float>(std::max(0.0, static_cast<double>(out.before) - durability_loss));
        out.wore = inventory.set_durability(svc::ItemContainer::Equipment, tool_slot, tool_guid, out.after);
    }
    return out;
}
struct HarvestLootCatalog {
    std::vector<combat::LootStack> stacks;
    std::map<int64_t, std::string> shown_name;
    std::map<int64_t, items::ItemRarityFacts> shown_rarity;
};
void append_rolled_item_stacks(const combat::RolledItem &it, const std::optional<content::ItemRecord> &rec, std::vector<combat::LootStack> &stacks) {
    const int64_t max_stack = rec ? std::max(1, rec->max_stack_size) : 1;
    const int default_q = rec ? std::max(1, rec->default_quality) : 1;
    const int quality = it.quality > 0 ? it.quality : default_q;
    for (int64_t left = it.quantity; left > 0;) {
        const int64_t take = std::min(left, max_stack);
        stacks.push_back({it.item, it.variation, take, quality});
        left -= take;
    }
}
HarvestLootCatalog build_harvest_loot_stacks(const gather::Harvest &harvest, const content::ContentDb *db) {
    HarvestLootCatalog out;
    for (const combat::RolledItem &it : harvest.items) {
        std::string err;
        const auto rec = db ? db->item(it.item, err) : std::nullopt;
        if (rec) {
            out.shown_name.try_emplace(it.item, rec->display_name.empty() ? rec->name : rec->display_name);
            out.shown_rarity.try_emplace(it.item, *rec);
        }
        append_rolled_item_stacks(it, rec, out.stacks);
    }
    return out;
}
struct HarvestGrantEnv {
    const items::ItemCatalog *catalog = nullptr;
    const content::ContentDb *db = nullptr;
    std::string_view session_name;
    std::function<std::string()> mint_guid;
};
struct HarvestGrantResult {
    items::Outcome granted;
    std::vector<combat::LootStack> overflow;
};
svc::InventoryItem make_fresh_harvest_item(const combat::LootStack &s, const HarvestGrantEnv &env, const std::vector<svc::InventoryItem> &existing,
                                           int64_t created_ms) {
    std::string err;
    const auto rec = env.db ? env.db->item(s.item, err) : std::nullopt;
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Backpack;
    it.item_record_id = s.item;
    it.instance_guid = env.mint_guid();
    for (const svc::InventoryItem &other : existing) {
        if (other.instance_guid == it.instance_guid)
            it.instance_guid = env.mint_guid();
    }
    it.quantity = static_cast<int32_t>(std::min<int64_t>(s.quantity, 1 << 20));
    it.quality = s.quality;
    it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
    it.created_at_ms = created_ms;
    return it;
}
HarvestGrantResult grant_harvest_stacks(items::Inventory &inventory, const std::vector<combat::LootStack> &stacks, const HarvestGrantEnv &env) {
    HarvestGrantResult out;
    out.granted.ok = true;
    if (stacks.empty())
        return out;
    std::vector<svc::InventoryItem> fresh;
    std::map<std::string, int64_t> variation_of;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (const combat::LootStack &s : stacks) {
        svc::InventoryItem it = make_fresh_harvest_item(s, env, fresh, created);
        variation_of[it.instance_guid] = s.variation;
        fresh.push_back(std::move(it));
    }
    const auto pile_stack = [&](const svc::InventoryItem &it) {
        const auto vit = variation_of.find(it.instance_guid);
        return combat::LootStack{it.item_record_id, vit != variation_of.end() ? vit->second : 0, it.quantity, it.quality};
    };
    items::GrantResult placed = inventory.grant_what_fits(fresh, *env.catalog);
    if (!placed.outcome.ok) {
        spdlog::error("[gather] {}: the harvest's items are not granted ({}): they "
                      "go to a pile",
                      env.session_name, placed.outcome.refusal);
        for (const svc::InventoryItem &it : fresh)
            out.overflow.push_back(pile_stack(it));
        return out;
    }
    out.granted = std::move(placed.outcome);
    for (const svc::InventoryItem &it : placed.rest) {
        if (it.quantity > 0)
            out.overflow.push_back(pile_stack(it));
    }
    return out;
}
int32_t added_stack_quantity(const std::vector<bootstrap::ItemEvent> &events, size_t i) {
    const bootstrap::ItemEvent &ev = events[i];
    if (i == 0)
        return ev.item.quantity;
    const bootstrap::ItemEvent &prev = events[i - 1];
    const bool stacked = prev.type == repl::StorageEventType::Remove && prev.storage == ev.storage && prev.slot == ev.slot;
    if (stacked && prev.item.has_item && prev.item.quantity < ev.item.quantity) {
        return ev.item.quantity - prev.item.quantity;
    }
    return ev.item.quantity;
}
void attach_harvest_callouts(std::vector<bootstrap::ItemEvent> &events, const HarvestLootCatalog &cat) {
    const int64_t made = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    for (size_t i = 0; i < events.size(); ++i) {
        bootstrap::ItemEvent &ev = events[i];
        if (ev.type != repl::StorageEventType::Add || !ev.item.has_item)
            continue;
        repl::ItemInstance shown = ev.item;
        shown.quantity = added_stack_quantity(events, i);
        const auto name_it = cat.shown_name.find(static_cast<int64_t>(shown.record_id));
        const auto facts_it = cat.shown_rarity.find(static_cast<int64_t>(shown.record_id));
        const std::string_view label = name_it != cat.shown_name.end() ? std::string_view(name_it->second) : "Unknown item";
        const items::ItemRarityFacts &facts = facts_it != cat.shown_rarity.end() ? facts_it->second : items::default_rarity_facts();
        ev.callout = items::acquired_callout(shown, label, facts, made);
    }
}
void queue_harvest_item_batches(const std::vector<bootstrap::ItemEvent> &events, const QueueFn &q_fn) {
    for (size_t at2 = 0; at2 < events.size(); at2 += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at2),
                                                     events.begin() +
                                                         static_cast<std::ptrdiff_t>(std::min(events.size(), at2 + bootstrap::kMaxEventsPerChain)));
        q_fn("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); }, true);
    }
}
professions::Professions with_harvest_profession_xp(professions::Professions after, int64_t profession_guid, int64_t profession_xp) {
    if (after.valid() && profession_xp > 0)
        after.add_xp(profession_guid, profession_xp);
    return after;
}
leveling::XpResult apply_harvest_adventuring_xp(const PlayerWorld *world, int64_t adv_xp, leveling::Progress &progress, bool &changed, size_t &levels_gained) {
    leveling::XpResult xp{progress, progress};
    if (world && world->level_curve() && adv_xp > 0) {
        xp = leveling::add_xp(*world->level_curve(), world->auto_grants(), progress, adv_xp);
        progress = xp.after;
        changed = true;
        if (xp.leveled())
            levels_gained += static_cast<size_t>(xp.after.level - xp.before.level);
    }
    return xp;
}
template <typename WireT> StepBuilder make_harvest_xp_tracker(int64_t total, int64_t debt, std::vector<WireT> wire) {
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
struct HarvestLogContext {
    const PlayerWorld *world = nullptr;
    const gather::Harvest *harvest = nullptr;
    std::string_view node_name, log_tail;
    const professions::Professions *professions = nullptr;
    const std::vector<professions::Change> *changes = nullptr;
    const leveling::XpResult *xp = nullptr;
    ToolWearResult tool_wear{};
    bool has_piles = false;
};
void append_harvest_xp_log(const HarvestLogContext &ctx, std::string &line) {
    for (const professions::Change &c : (ctx.changes ? *ctx.changes : std::vector<professions::Change>{})) {
        if (const professions::Profession *pf = ctx.professions->book()->find(c.profession)) {
            line += "; " + pf->name + " +" + std::to_string(c.after.xp - c.before.xp) + " XP (now " + std::to_string(c.after.xp) + ", level " +
                    std::to_string(c.after.level) + ")";
        }
    }
    if (!ctx.xp || ctx.harvest->adventuring_xp <= 0)
        return;
    const std::string lvl = ctx.xp->leveled() ? ", level " + std::to_string(ctx.xp->before.level) + " -> " + std::to_string(ctx.xp->after.level) : "";
    line += "; Adventuring +" + std::to_string(ctx.harvest->adventuring_xp) + " XP offered (debt " + std::to_string(ctx.xp->before.debt) + " -> " +
            std::to_string(ctx.xp->after.debt) + ", total " + std::to_string(ctx.xp->after.xp) + lvl + ")";
}
std::string format_harvest_log_line(const HarvestLogContext &ctx) {
    const gather::Harvest &harvest = *ctx.harvest;
    std::vector<std::string> got;
    for (const combat::RolledItem &it : harvest.items) {
        const std::string item_label = ctx.world ? ctx.world->item_name(it.item) : std::to_string(it.item);
        got.push_back(item_label + " x" + std::to_string(it.quantity) + " q" + std::to_string(it.quality > 0 ? it.quality : 1));
    }
    std::string line = "harvested " + std::string(ctx.node_name) + ": " + (got.empty() ? std::string("nothing") : join_names(got));
    if (!harvest.items.empty())
        line += " (the first item's rarity " + std::to_string(harvest.primary_rarity) + ")";
    append_harvest_xp_log(ctx, line);
    if (ctx.tool_wear.wore) {
        line += "; the tool's durability " + std::to_string(static_cast<int>(ctx.tool_wear.before)) + " -> " +
                std::to_string(static_cast<int>(ctx.tool_wear.after));
    }
    if (ctx.has_piles)
        line += "; the bag is full: the rest goes to a pile";
    line += std::string(ctx.log_tail);
    return line;
}
struct ChannelPrecheckInput {
    gather::Gathering *g = nullptr;
    const ChannelSpec *spec = nullptr;
    GoodPosition at{};
    uint64_t active_gather_node = 0;
    bool active_foliage = false, in_combat = false;
    const items::Inventory *inventory = nullptr;
    const professions::Professions *professions = nullptr;
    const bootstrap::PlayerSetup *setup = nullptr;
    std::function<std::unique_ptr<stats::CharacterStats::Evaluation>(std::string &)> eval_fn;
};
ChannelPrecheckResult precheck_gather_channel(const ChannelPrecheckInput &in) {
    const world::WorldActor &node = *in.spec->target;
    const content::GatherDefRow &def = *in.spec->def;
    const double distance = std::hypot(node.x - in.at.x, node.y - in.at.y);
    const double reach = gather::reach_cm(def);
    if (distance > reach) {
        return fail_precheck(
            "out of range", std::to_string(static_cast<int>(distance)) + " cm away, the interaction reaches " + std::to_string(static_cast<int>(reach)) + " cm",
            distance);
    }
    if (in.active_gather_node != 0 || in.active_foliage) {
        return fail_precheck("already channeling",
                             in.active_gather_node == node.id ? "this player is harvesting it already" : "this player is harvesting something already",
                             distance);
    }
    ChannelPrecheckResult out = check_node_board_state(*in.spec, in.g->board().state_of(node.id), distance);
    if (!out.ok)
        return out;
    out.worn = in.inventory->at(svc::ItemContainer::Equipment, def.tool_slot);
    if (out.worn) {
        out.tool.worn = true;
        out.tool.tool = in.g->tool(out.worn->item_record_id);
        out.tool.durability = out.worn->durability;
    }
    const gather::Verdict verdict = gather::check_requirements(def, *in.professions, out.tool, in.in_combat);
    return verdict.ok() ? out : fail_precheck(gather::refusal_name(verdict.why), verdict.text, distance);
}
struct ChannelStartAttempt {
    bool ok = false;
    const char *kind = nullptr;
    std::string why;
    ChannelPrecheckResult pre;
    double seconds = 0.0;
};
ChannelStartAttempt try_start_gather_channel(const ChannelPrecheckInput &in, Clock::time_point now) {
    ChannelStartAttempt out;
    out.pre = precheck_gather_channel(in);
    if (!out.pre.ok)
        return {false, out.pre.kind, out.pre.why, out.pre, 0.0};
    auto run = in.eval_fn(out.why);
    if (!run)
        return {false, "no stats", out.why, out.pre, 0.0};
    const auto seconds = in.g->channel_seconds(*in.spec->def, *run, out.why);
    if (!seconds)
        return {false, "no duration", out.why, out.pre, 0.0};
    const auto &cg = in.setup->character_guid;
    const gather::Channeler by{static_cast<uint64_t>(in.setup->player_id), repl::GuidValue{cg[0], cg[1], cg[2], cg[3]}, in.setup->guids.pawn};
    if (!in.g->board().begin(in.spec->target->id, by, now, std::chrono::duration<double>(*seconds), out.why))
        return {false, "node refused", out.why, out.pre, 0.0};
    return {true, nullptr, {}, out.pre, *seconds};
}
template <typename GatherRunT> bool release_gather_board_channel(PlayerWorld *npc_world, uint64_t player_id, const GatherRunT &prev, const std::string &why) {
    gather::Gathering *g = npc_world ? npc_world->gathering() : nullptr;
    const gather::Channel *ch = g ? g->board().channel_of(prev.node) : nullptr;
    const bool mine = ch && ch->player_id == player_id;
    if (mine)
        g->board().cancel(prev.node, why);
    if (prev.hunt && mine && npc_world->aftermath())
        npc_world->aftermath()->hunt_cancelled(prev.node);
    return mine;
}
template <typename GatherRunT>
void log_gather_cancelled(spdlog::level::level_enum lvl, std::string_view session_name, const GatherRunT &prev, const std::string &why) {
    const std::string def = prev.def ? prev.def->name : std::string("?");
    if (prev.hunt) {
        spdlog::log(lvl, "[hunt] {}: the hunt of {} ends with no reward: {}", session_name, prev.what.empty() ? def : prev.what, why);
    } else {
        spdlog::log(lvl, "[gather] {}: the harvest of {} (node {}) ends with no reward: {}", session_name, def, prev.node, why);
    }
}
template <typename PileDequeT, typename CalloutDequeT> struct GatherFlushEnv {
    PileDequeT *piles = nullptr;
    CalloutDequeT *callouts = nullptr;
    std::function<void(typename PileDequeT::value_type &)> make_pile;
    std::function<bool()> can_send_callout;
    QueueFn q_fn;
};
template <typename PileDequeT, typename CalloutDequeT>
void flush_due_piles_and_callouts(const GatherFlushEnv<PileDequeT, CalloutDequeT> &env, Clock::time_point now) {
    while (!env.piles->empty() && now >= env.piles->front().due) {
        auto pile = std::move(env.piles->front());
        env.piles->pop_front();
        env.make_pile(pile);
    }
    while (!env.callouts->empty() && now >= env.callouts->front().due) {
        if (!env.can_send_callout())
            break;
        const uint64_t key = env.callouts->front().key;
        env.callouts->pop_front();
        env.q_fn("a callout is taken off", [key](bootstrap::PlayerBootstrap &pb) { return pb.callout_remove(key); }, false);
    }
}
struct GatherRollInput {
    gather::Gathering *g = nullptr;
    const PlayerWorld *world = nullptr;
    const world::ActorRegistry *registry = nullptr;
    uint64_t node_id = 0;
    const content::GatherDefRow *def = nullptr;
    int player_level = 1;
    bool hunt = false, gives_nothing = false;
    std::function<std::unique_ptr<stats::CharacterStats::Evaluation>(std::string &)> eval_fn;
};
std::optional<gather::Harvest> roll_completed_gather(const GatherRollInput &in, std::string &cancel_why) {
    const world::WorldActor *node = in.registry ? in.registry->find(in.node_id) : nullptr;
    if (!node) {
        cancel_why = "the node left the world";
        return std::nullopt;
    }
    std::string why;
    auto run = in.eval_fn(why);
    if (!run) {
        cancel_why = "no stat run for the rolls: " + why;
        return std::nullopt;
    }
    const world::CityNodes *nodes = in.world->city_nodes();
    const world::NodeSite site = nodes ? nodes->node_at(node->x, node->y) : world::NodeSite{};
    auto harvest = in.g->harvest(*in.def, *run, in.player_level, site, nodes, in.g->rng(), why);
    if (!harvest) {
        cancel_why = "the rolls do not run: " + why;
        return std::nullopt;
    }
    if (in.hunt && in.gives_nothing)
        harvest->items.clear();
    return harvest;
}
template <typename GatherRunT, typename FrameT>
bool complete_gather_board_and_frame(gather::Gathering &g, PlayerWorld *npc_world, const GatherRunT &gather, Clock::time_point now,
                                     std::pair<FrameT *, std::string *> out) {
    const content::GatherDefRow &def = *gather.def;
    const std::chrono::duration<double> respawn =
        gather.hunt ? std::chrono::duration<double>(3600.0) : g.board().respawn_of(def.override_respawn, def.empty_duration);
    std::string why;
    if (!g.board().complete(gather.node, now, respawn, why)) {
        *out.second = (gather.hunt ? "the corpse cannot be hunted: " : "the node cannot be gathered: ") + why;
        return false;
    }
    FrameT &frame = *out.first;
    frame.what = gather.hunt ? gather.what : def.name + " (node " + std::to_string(gather.node) + ")";
    if (gather.hunt)
        frame.tag = "hunt";
    frame.profession = def.profession_guid;
    frame.tool_guid = gather.tool_guid;
    frame.tool_slot = gather.tool_slot;
    frame.log_tail = finish_gather_aftermath_and_tail(gather.hunt, npc_world, gather.node, respawn);
    return true;
}
template <typename FrameT, typename CalloutDequeT, typename PileDequeT> struct HarvestOverflowEnv {
    int64_t player_id = 0;
    uint32_t *callout_seq = nullptr;
    CalloutDequeT *callouts = nullptr;
    PileDequeT *piles = nullptr;
    GoodPosition at{};
    const FrameT *frame = nullptr;
    QueueFn q_fn;
};
template <typename FrameT, typename CalloutDequeT, typename PileDequeT>
void queue_harvest_items_and_overflow(const HarvestOverflowEnv<FrameT, CalloutDequeT, PileDequeT> &env, Clock::time_point now, HarvestGrantResult &grant_res) {
    if (!grant_res.overflow.empty()) {
        const int64_t ticks =
            items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const uint64_t key = (static_cast<uint64_t>(env.player_id) << 32) | (0x40000000u + (*env.callout_seq)++);
        env.q_fn(
            "a callout", [ticks, key](bootstrap::PlayerBootstrap &pb) { return pb.callout_add(kNoRoomText, kCalloutType, kCalloutPointSize, ticks, key); },
            false);
        env.callouts->push_back({now + kCalloutLife, key});
        env.piles->push_back({now + kPileDelay, std::move(grant_res.overflow), env.at.x, env.at.y, env.at.z, env.frame->what});
    }
    if (env.frame->after_items)
        env.frame->after_items();
}
struct HarvestXpQueueInput {
    const leveling::XpResult *xp = nullptr;
    int player_level = 1;
    StepBuilder xp_tracker;
    const std::vector<professions::Change> *changes = nullptr;
    const std::vector<repl::SkillOnActionbar> *skill_updates = nullptr;
    std::function<void()> on_prof_level_ups;
    QueueFn q_fn;
};
void queue_harvest_xp_and_skills(const HarvestXpQueueInput &in) {
    if (in.xp->changed() || in.xp->leveled() || !in.changes->empty())
        in.q_fn("XPTracker", in.xp_tracker, true);
    if (in.xp->leveled()) {
        const int level = in.player_level;
        in.q_fn(
            "BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap &pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, false);
    }
    if (!in.changes->empty())
        in.on_prof_level_ups();
    if (in.xp->leveled()) {
        in.q_fn("pawn state", [](bootstrap::PlayerBootstrap &pb) { return pb.pawn_state_update("level and stats"); }, true);
    }
    for (const repl::SkillOnActionbar &u : *in.skill_updates) {
        in.q_fn("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap &pb) { return pb.skill_on_actionbar(u); }, true);
    }
}
} // namespace
std::unique_ptr<stats::CharacterStats::Evaluation> PlayerSession::gather_run(std::string &why) {
    if (!stats_ || !stats_->started()) {
        why = "the character has no stats";
        return nullptr;
    }
    return stats_->evaluation(bootstrap::worn_items_of(inventory_), why);
}
void PlayerSession::on_gather_interact(Connection &conn, const world::WorldActor &node, Clock::time_point now) {
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (!g) {
        if (!gather_off_logged_) {
            gather_off_logged_ = true;
            spdlog::info("[gather] {}: an interaction with a resource node is ignored: this world does not harvest "
                         "(content.db has no gather tables, or --no-gathering)",
                         name());
        }
        ++interact_ignored_;
        return;
    }
    const content::GatherDefRow *def = g->def_of(node);
    const std::string what = (def ? def->name : std::string("a node of no gather def")) + " (node " + std::to_string(node.id) + ")";
    const auto refuse = [&](const char *kind, const std::string &why) {
        ++harvests_refused_;
        ++harvest_refusals_[kind];
        spdlog::log(level_for(gather_refused_logged_, 12), "[gather] {}: the harvest of {} is refused ({}): {}", name(), what, kind, why);
    };
    if (!def)
        return refuse("no gather row", "the node's GatherableActorDef has no row in gather_def");
    if (view_->state_of(node.id) != world::ClientState::Awake)
        return refuse("not open", "the client does not have that node open");
    begin_channel(conn, node, *def, what, "gather", "harvest", /*hunt=*/false, /*gives_nothing=*/false, now);
}
void PlayerSession::on_hunt_interact(Connection &conn, const world::WorldActor &corpse, Clock::time_point now) {
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    const gather::Gathering::HuntKind *kind = g ? g->hunt_kind(corpse.npc.interact_def) : nullptr;
    if (!kind) {
        ++interact_ignored_;
        return;
    }
    const std::string what = kind->name + " (" + (corpse.npc.name.empty() ? std::string("creature") : corpse.npc.name) + " " + std::to_string(corpse.id) + ")";
    const auto refuse = [&](const char *why_kind, const std::string &why) {
        ++harvests_refused_;
        ++harvest_refusals_[why_kind];
        spdlog::log(level_for(gather_refused_logged_, 12), "[hunt] {}: the hunt of {} is refused ({}): {}", name(), what, why_kind, why);
    };
    combat::Aftermath *after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const combat::Corpse *body = after ? after->corpse_of(corpse.id) : nullptr;
    if (body && body->hunted)
        return refuse("corpse hunted", "the creature was hunted already");
    begin_channel(conn, corpse, *kind->row, what, "hunt", "hunt", /*hunt=*/true, kind->gives_nothing, now);
}
void PlayerSession::begin_channel(Connection &conn, const world::WorldActor &node, const content::GatherDefRow &def_row, const std::string &what,
                                  const char *tag, const char *noun, bool hunt, bool gives_nothing, Clock::time_point now) {
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (!g)
        return;
    const ChannelSpec spec{&node, &def_row, what, tag, noun, hunt, gives_nothing};
    const GoodPosition at = checker_.good();
    const ChannelStartAttempt attempt = try_start_gather_channel({g, &spec, at, gather_.node, foliage_.row != kNoInstance, stats_ && stats_->in_combat(),
                                                                  &inventory_, &professions_, &pb_.setup(), [this](std::string &w) { return gather_run(w); }},
                                                                 now);
    if (!attempt.ok) {
        ++harvests_refused_;
        ++harvest_refusals_[attempt.kind];
        spdlog::log(level_for(gather_refused_logged_, 12), "[{}] {}: the {} of {} is refused ({}): {}", spec.tag, name(), spec.noun, spec.what, attempt.kind,
                    attempt.why);
        return;
    }
    gather_ = {node.id, node.actor.guid, &def_row, def_row.tool_slot, attempt.pre.worn->instance_guid, now, attempt.seconds, at.x, at.y,
               hunt,    gives_nothing,   what};
    ++harvests_started_;
    if (hunt && npc_world_->aftermath())
        npc_world_->aftermath()->hunt_started(node.id, npc_world_->world_clock(), attempt.seconds);
    spdlog::log(level_for(gather_logged_, 20), "[{}] {}: {}", spec.tag, name(),
                format_channel_start_log(spec, professions_, attempt.pre.tool, attempt.seconds, attempt.pre.distance));
    send_channeling(conn, build_active_channeling(node, *attempt.pre.worn, attempt.seconds, pb_.setup().world_time, session_seconds(now)));
    view_->update_now(conn, node.id);
}
void PlayerSession::send_channeling(Connection &conn, const repl::ChannelingData &data) {
    if (!world_)
        return;
    if (!interact_info_)
        interact_info_ = world_->mint_guid();
    const protocol::emit::FIntrepidNetworkGUID info = *interact_info_;
    queue(
        "the pawn's channeling data",
        [info, data](bootstrap::PlayerBootstrap &pb) -> std::optional<bootstrap::Send> {
            if (!pb.ensure_interact_info(info))
                return std::nullopt;
            return pb.pawn_channeling(data);
        },
        conn, /*must_build=*/true);
}
void PlayerSession::on_interrupt_channel(Connection &conn, Clock::time_point now) {
    if (!within_budget(cancel_budget_, "ServerInterruptChannel", now))
        return;
    if (foliage_.row != kNoInstance) {
        cancel_foliage(&conn, "the client interrupted the channel (ServerInterruptChannel)");
        return;
    }
    if (gather_.node == 0) {
        spdlog::debug("[gather] {}: ServerInterruptChannel with no harvest in progress: ignored", name());
        return;
    }
    cancel_gather(&conn, "the client interrupted the channel (ServerInterruptChannel)");
}
void PlayerSession::cancel_gather(Connection *conn, const std::string &why, Interrupt interrupt) {
    cancel_foliage(conn, why, interrupt);
    if (gather_.node == 0)
        return;
    const GatherRun prev = std::exchange(gather_, GatherRun{});
    const bool mine = release_gather_board_channel(npc_world_, static_cast<uint64_t>(pb_.setup().player_id), prev, why);
    ++harvests_cancelled_;
    log_gather_cancelled(level_for(gather_logged_, 20), name(), prev, why);
    if (!conn)
        return;
    send_channeling(*conn, channeling_over(prev.target));
    if (interrupt != Interrupt::None)
        send_interrupt_callout(*conn, interrupt, last_now_);
    if (view_ && mine)
        view_->update_now(*conn, prev.node);
}
void PlayerSession::run_gather(Connection &conn, Clock::time_point now) {
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    if (g)
        g->board().advance(now);
    flush_due_piles_and_callouts(GatherFlushEnv<std::deque<GatherPile>, std::deque<GatherCallout>>{
                                     &gather_piles_, &gather_callouts_, [this](GatherPile &p) { make_gather_pile(p); },
                                     [this]() { return room_for(1); },
                                     [this, &conn](const char *w, StepBuilder b, bool m) { queue(w, std::move(b), conn, m); }},
                                 now);
    if (gather_.node == 0 || !g)
        return;
    const uint64_t me = static_cast<uint64_t>(pb_.setup().player_id);
    const gather::Channel *ch = g->board().channel_of(gather_.node);
    if (!ch || ch->player_id != me)
        return cancel_gather(&conn, "the node is not this player's channel any more");
    const world::WorldActor *node = registry_ ? registry_->find(gather_.node) : nullptr;
    const svc::InventoryItem *tool = inventory_.at(svc::ItemContainer::Equipment, gather_.tool_slot);
    const GatherChannelCheck check =
        check_active_gather_channel({gather_.tool_guid, gather_.def, gather_.start_x, gather_.start_y}, node, tool, checker_.good(), {dead_, mounted()});
    if (check.cancel) {
        return cancel_gather(&conn, check.why, check.movement_interrupt ? Interrupt::Movement : Interrupt::None);
    }
    if (g->board().beat(gather_.node, now))
        view_->update_now(conn, gather_.node);
    if (now >= ch->completes)
        complete_gather(conn, now);
}
void PlayerSession::make_gather_pile(GatherPile &pile) {
    std::string error = "the session has no world";
    const auto &cg = pb_.setup().character_guid;
    const uint64_t actor =
        npc_world_ ? npc_world_->drop_harvest_pile(pile.x, pile.y, pile.z, repl::GuidValue{cg[0], cg[1], cg[2], cg[3]}, pb_.setup().name, pile.stacks, error) : 0;
    if (actor != 0) {
        ++harvest_piles_;
        spdlog::log(level_for(gather_done_logged_, 20), "[gather] {}: the bag was full: {} stack(s) of the harvest of {} are in a pile where it stands", name(),
                    pile.stacks.size(), pile.node_name);
    } else {
        spdlog::warn("[gather] {}: the pile for {} stack(s) of the harvest of {} is not made ({}): they are lost", name(), pile.stacks.size(), pile.node_name,
                     error);
    }
}
bool PlayerSession::complete_gather(Connection &conn, Clock::time_point now) {
    gather::Gathering *g = npc_world_ ? npc_world_->gathering() : nullptr;
    const content::ContentDb *db = world_ ? world_->content() : nullptr;
    if (!g || !db || !gather_.def)
        return true;
    if (!room_for(14 + (world_ ? world_->auto_grants().size() : 0)))
        return false;
    std::string why;
    auto harvest = roll_completed_gather({g, world_, registry_, gather_.node, gather_.def, progress_.level, gather_.hunt, gather_.gives_nothing,
                                          [this](std::string &w) { return gather_run(w); }},
                                         why);
    if (!harvest) {
        cancel_gather(&conn, why);
        return true;
    }
    HarvestFrame frame;
    if (!complete_gather_board_and_frame(*g, npc_world_, gather_, now, std::pair{&frame, &why})) {
        cancel_gather(&conn, why);
        return true;
    }
    const protocol::emit::FIntrepidNetworkGUID node_target = gather_.target;
    frame.harvest = &*harvest;
    frame.after_items = [this, &conn, node_target] { send_channeling(conn, channeling_over(node_target)); };
    gather_ = GatherRun{};
    ++harvests_completed_;
    deliver_harvest(conn, now, frame);
    return true;
}
void PlayerSession::deliver_harvest(Connection &conn, Clock::time_point now, const HarvestFrame &frame) {
    const content::ContentDb *db = world_ ? world_->content() : nullptr;
    const gather::Harvest &harvest = *frame.harvest;
    const ToolWearResult tool_wear = wear_harvest_tool(inventory_, frame.tool_slot, frame.tool_guid, harvest.durability_loss);
    const HarvestLootCatalog loot_cat = build_harvest_loot_stacks(harvest, db);
    HarvestGrantResult grant_res = grant_harvest_stacks(inventory_, loot_cat.stacks, {&item_catalog(), db, name(), [this]() { return fresh_guid(); }});
    if (tool_wear.wore || !grant_res.granted.events.empty()) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
    }
    const std::vector<professions::Change> changes = professions_.valid()
                                                         ? set_professions(with_harvest_profession_xp(professions_, frame.profession, harvest.profession_xp))
                                                         : std::vector<professions::Change>();
    const leveling::XpResult xp = apply_harvest_adventuring_xp(world_, harvest.adventuring_xp, progress_, progress_changed_, levels_gained_);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (xp.leveled())
        finish_level_up(xp, false, now, rank_effects, skill_updates);
    const QueueFn q_fn = [this, &conn](const char *what, StepBuilder b, bool must) { queue(what, std::move(b), conn, must); };
    if (tool_wear.wore) {
        const int32_t slot = frame.tool_slot;
        const float value = tool_wear.after;
        q_fn(
            "ClientUpdateItemDurability",
            [slot, value](bootstrap::PlayerBootstrap &pb) { return pb.item_durability(bootstrap::ItemStorage::Equipment, slot, value); }, true);
    }
    if (!grant_res.granted.events.empty()) {
        std::vector<bootstrap::ItemEvent> events = item_events_for(grant_res.granted);
        attach_harvest_callouts(events, loot_cat);
        queue_harvest_item_batches(events, q_fn);
    }
    queue_harvest_items_and_overflow(
        HarvestOverflowEnv<HarvestFrame, std::deque<GatherCallout>, std::deque<GatherPile>>{pb_.setup().player_id, &gather_callout_seq_, &gather_callouts_,
                                                                                            &gather_piles_, checker_.good(), &frame, q_fn},
        now, grant_res);
    queue_harvest_xp_and_skills({&xp, progress_.level, make_harvest_xp_tracker(progress_.xp, progress_.debt, profession_wire(changes)), &changes,
                                 &skill_updates, [this, &conn, &changes]() { queue_profession_level_ups(conn, changes); }, q_fn});
    if (frame.after_experience)
        frame.after_experience();
    spdlog::log(level_for(gather_done_logged_, 40), "[{}] {}: {}", frame.tag, name(),
                format_harvest_log_line({world_, &harvest, frame.what, frame.log_tail, &professions_, &changes, &xp, tool_wear, !gather_piles_.empty()}));
    if (!changes.empty() || xp.changed())
        hand_over_progress(xp.leveled() ? "a level-up" : "a harvest");
    if (inventory_dirty_)
        hand_over_inventory("a harvest", /*periodic=*/false);
}
void PlayerSession::register_gather_rpcs(RpcTable &t) {
    t.add(RpcScope::Pawn, "ServerInterruptChannel",
          [](PlayerSession &s, const RpcContext &c) { s.on_interrupt_channel(c.conn, c.now); });
}
} // namespace worldd
} // namespace aoc
