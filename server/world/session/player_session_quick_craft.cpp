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
namespace aoc::worldd {
namespace {
using StepBuilder = std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap &)>;
using QueueLaterFn = std::function<void(const char *, StepBuilder, Clock::time_point, bool)>;
constexpr std::chrono::milliseconds kQuickTickAfterRemoves{40};
bool crate_follows_materials(const content::ContentDb *db, int64_t record) {
    std::string error;
    const auto gear = db ? db->item_gear(record, error) : std::nullopt;
    if (!gear)
        return false;
    for (std::string lower : gear->tags) {
        for (char &c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "item.crate.commodity" || lower.rfind("item.crate.commodity.", 0) == 0)
            return true;
    }
    return false;
}
const char *price_unit(int64_t tier) {
    if (tier == items::kGlintTier)
        return "Glint T1";
    if (tier == items::kNodeCurrencyTier)
        return "node currency";
    return "copper";
}
struct QuickFactsInput {
    const PlayerWorld *world = nullptr;
    const world::WorldActor *actor = nullptr;
    const std::vector<int64_t> *taught = nullptr;
    const professions::Professions *professions = nullptr;
    const items::Inventory *inventory = nullptr;
    const items::Wallet *wallet = nullptr;
};
struct QuickPlanResult {
    bool ok = false;
    std::string refusal;
    const content::CraftRecipe *recipe = nullptr;
    protocol::repl::StationTax tax{};
    crafting::Plan plan{};
};
void populate_quick_node_facts(const PlayerWorld *world, const world::WorldActor &actor, crafting::StartFacts &facts) {
    facts.tax = crafting::station_tax(actor.station.tax_tag);
    facts.node.guid = actor.station.node;
    const world::CityNodeInfo *node = (world && world->city_nodes() && world->node_wire()) ? world->city_nodes()->by_guid(actor.station.node) : nullptr;
    if (!node)
        return;
    facts.node.level = world->city_nodes()->level_of(node->guid);
    facts.node.biome = spawns::node_site_of(*world->city_nodes(), *world->node_wire(), *node, facts.node.level).biome;
}
void populate_quick_facts(const QuickFactsInput &in, const content::CraftStationDef &station, crafting::StartFacts &facts) {
    facts.station = &station;
    facts.quick = true;
    facts.taught = [taught = in.taught](int64_t r) { return taught && std::find(taught->begin(), taught->end(), r) != taught->end(); };
    populate_quick_node_facts(in.world, *in.actor, facts);
    facts.tier_of = [prof = in.professions](int64_t p) { return prof && prof->valid() ? prof->tier(p) : 0; };
    facts.inventory = in.inventory;
    facts.rarity_facts = [w = in.world](int64_t item) { return w->item_rarity_facts(item); };
    facts.copper = in.wallet ? in.wallet->copper() : 0;
    facts.node_currency = in.wallet ? in.wallet->balance(items::kNodeCurrency) : 0;
}
const content::CraftStationDef *find_quick_station_def(const QuickFactsInput &in, std::string &refusal) {
    const content::CraftStationData *defs = in.world ? in.world->station_data() : nullptr;
    const content::CraftRecipeData *recipes = in.world ? in.world->craft_recipes() : nullptr;
    if (!in.actor || !defs || !recipes) {
        refusal = in.actor ? "worldd has no station definitions or recipes (an "
                             "older content.db)"
                           : "the station or the recipes are gone";
        return nullptr;
    }
    const auto def = defs->defs.find(in.actor->station.def);
    if (in.actor->station.def == 0 || def == defs->defs.end()) {
        refusal = "the data holds no definition for " + in.actor->station.name;
        return nullptr;
    }
    return &def->second;
}
QuickPlanResult plan_quick_craft(const repl::StartCraft &req, const QuickFactsInput &in) {
    std::string refusal;
    const content::CraftStationDef *def = find_quick_station_def(in, refusal);
    if (!def)
        return {false, std::move(refusal)};
    crafting::StartFacts facts;
    facts.recipes = in.world->craft_recipes();
    populate_quick_facts(in, *def, facts);
    crafting::Plan plan = crafting::plan_start(req, facts);
    if (!plan.ok)
        return {false, plan.refusal};
    return {true, {}, plan.recipe, facts.tax, std::move(plan)};
}
void record_paid_units(const crafting::Take &t, const std::vector<items::Event> &events, const PlayerWorld *world, std::vector<crafting::PaidUnits> &paid) {
    for (const items::Event &e : events) {
        const int64_t count = e.count > 0 ? e.count : e.item.quantity;
        const int rarity = world ? world->item_rarity_facts(e.item.item_record_id).rarity_of(e.item.quality) : 1;
        paid.push_back({t.slot, e.item.quality, count, rarity});
    }
}
items::Outcome take_quick_materials(items::Inventory &inventory, const std::vector<crafting::Take> &takes, const PlayerWorld *world,
                                    std::vector<crafting::PaidUnits> &paid) {
    items::Outcome taken{true, {}, {}};
    for (const crafting::Take &t : takes) {
        items::Outcome o = inventory.take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first);
        if (!o.ok)
            return o;
        if (!t.lowest_first)
            record_paid_units(t, o.events, world, paid);
        taken.events.insert(taken.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (taken.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
        return {false, "the craft takes from " + std::to_string(taken.events.size()) + " stacks: more events than one answer holds", {}};
    }
    return taken;
}
struct QuickPaymentResult {
    bool ok = false;
    std::string refusal;
    items::Outcome taken;
    items::Outcome glint_paid;
    std::vector<crafting::PaidUnits> paid;
};
QuickPaymentResult take_and_pay_quick_make(items::Inventory &inventory, items::Wallet &wallet, const crafting::Plan &plan, const PlayerWorld *world) {
    QuickPaymentResult out;
    out.taken = take_quick_materials(inventory, plan.takes, world, out.paid);
    if (!out.taken.ok)
        return {false, out.taken.refusal};
    if (plan.cost <= 0)
        return {true, {}, std::move(out.taken), {}, std::move(out.paid)};
    if (plan.currency == items::kGlintTier) {
        out.glint_paid = inventory.take(items::kGlintT1Item, plan.cost);
        if (!out.glint_paid.ok)
            return {false, out.glint_paid.refusal};
        if (out.glint_paid.events.size() > 8 * bootstrap::kMaxEventsPerChain) {
            return {false, "the Glint comes from " + std::to_string(out.glint_paid.events.size()) + " stacks: more events than the answers hold"};
        }
    } else if (plan.currency == items::kNodeCurrencyTier) {
        wallet.pay_currency(items::kNodeCurrency, plan.cost);
    } else {
        wallet.pay(plan.cost);
    }
    out.ok = true;
    return out;
}
std::optional<crafting::JobRewards> roll_quick_make_rewards(const content::ContentDb *db, const content::CraftRecipeData &recipes,
                                                            const content::CraftRecipe &recipe, const std::vector<crafting::PaidUnits> &paid,
                                                            const std::pair<crafting::Ratings, std::pair<int, uint64_t>> &rls) {
    const auto facts_of = [db](int64_t item) -> std::optional<content::ItemSaleFacts> {
        std::string error;
        auto f = db ? db->item_sale(item, error) : std::nullopt;
        return (f && f->known) ? f : std::nullopt;
    };
    return crafting::roll_craft(recipes, recipe, paid, rls.first, rls.second.first, rls.second.second, facts_of);
}
struct QuickStackContext {
    const PlayerWorld *world = nullptr;
    const content::ContentDb *db = nullptr;
    const items::ItemCatalog *catalog = nullptr;
    std::string_view recipe_name;
    bool cargo = false;
    double crate_mean = 1.0;
    int32_t reward_quality = 1;
    int64_t created_ms = 0;
    std::function<std::string()> mint_guid;
};
struct QuickProductStacks {
    bool ok = false;
    std::string refusal;
    std::vector<svc::InventoryItem> fresh;
    std::vector<std::vector<svc::InventoryItem>> by_product;
    std::map<int64_t, std::string> shown_name;
};
int32_t compute_quick_item_quality(const crafting::Product &p, const std::optional<content::ItemRecord> &rec, const QuickStackContext &ctx) {
    if (!ctx.cargo)
        return p.quality > 0 ? p.quality : ctx.reward_quality;
    int32_t quality = std::max(1, rec ? rec->default_quality : 1);
    if (crate_follows_materials(ctx.db, p.item) && ctx.world) {
        const items::ItemRarityFacts crate_facts = ctx.world->item_rarity_facts(p.item);
        quality = std::clamp(crafting::quality_from(crafting::craft_base_quality(ctx.crate_mean, crate_facts)), 1, crafting::kMaxQuality);
        const int shown = crate_facts.rarity_of(quality);
        if (shown != crate_facts.band_of(quality))
            quality = items::first_quality_of(shown);
    }
    return quality;
}
void append_quick_product_stacks(const crafting::Product &p, int64_t max_stack, const std::optional<content::ItemRecord> &rec, const QuickStackContext &ctx,
                                 QuickProductStacks &out) {
    out.by_product.emplace_back();
    const int32_t quality = compute_quick_item_quality(p, rec, ctx);
    const float durability = rec ? rec->max_durability_at(quality) : 0.0f;
    for (int64_t left = p.quantity; left > 0;) {
        const int64_t take = std::min(left, max_stack);
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = p.item;
        it.instance_guid = ctx.mint_guid();
        for (const svc::InventoryItem &other : out.fresh) {
            if (other.instance_guid == it.instance_guid)
                it.instance_guid = ctx.mint_guid();
        }
        it.quantity = static_cast<int32_t>(take);
        it.quality = quality;
        it.durability = durability;
        it.created_at_ms = ctx.created_ms;
        out.by_product.back().push_back(it);
        out.fresh.push_back(std::move(it));
        left -= take;
    }
}
QuickProductStacks build_quick_product_stacks(const crafting::JobRewards &rewards, const QuickStackContext &ctx) {
    QuickProductStacks out;
    for (const crafting::Product &p : rewards.products) {
        const auto info = ctx.catalog->info(p.item);
        if (!info) {
            return {false, "the item table does not know product " + std::to_string(p.item) + " of " + std::string(ctx.recipe_name)};
        }
        std::string error;
        const auto rec = ctx.db ? ctx.db->item(p.item, error) : std::nullopt;
        if (rec)
            out.shown_name[p.item] = rec->display_name.empty() ? rec->name : rec->display_name;
        append_quick_product_stacks(p, std::max(1, info->max_stack), rec, ctx, out);
    }
    out.ok = true;
    return out;
}
struct QuickGrantResult {
    bool ok = false;
    const char *callout = nullptr;
    std::string refusal;
    QuickProductStacks stacks;
    std::vector<items::Outcome> granted;
};
QuickGrantResult grant_quick_stacks_into_inventory(items::Inventory &inventory, bool cargo, QuickProductStacks &stacks, const items::ItemCatalog &catalog) {
    QuickGrantResult out{false, nullptr, {}, std::move(stacks), {}};
    if (cargo) {
        items::Outcome o = inventory.grant_cargo(out.stacks.fresh.front(), catalog);
        if (!o.ok)
            return {false, nullptr, "the crate is not put in the cargo slot: " + o.refusal};
        out.granted.push_back(std::move(o));
        out.ok = true;
        return out;
    }
    for (std::vector<svc::InventoryItem> &prod_stacks : out.stacks.by_product) {
        items::Outcome o = inventory.grant(prod_stacks, catalog);
        if (!o.ok)
            return {false, nullptr, "the product is not granted: " + o.refusal};
        out.granted.push_back(std::move(o));
    }
    out.ok = true;
    return out;
}
QuickGrantResult prepare_and_grant_quick_products(items::Inventory &inventory, const crafting::JobRewards &rewards, const QuickStackContext &ctx) {
    if (ctx.cargo && inventory.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        return {false, "You are already carrying a crate.",
                "the cargo slot holds a crate already, so " + std::string(ctx.recipe_name) + " cannot be made (the player was told)"};
    }
    QuickProductStacks stacks = build_quick_product_stacks(rewards, ctx);
    if (!stacks.ok)
        return {false, nullptr, stacks.refusal};
    if (!ctx.cargo && !inventory.can_grant(stacks.fresh, *ctx.catalog)) {
        return {false, "Not enough inventory space.",
                "the bags have no room for the " + std::to_string(stacks.fresh.size()) + " stack(s) of " + std::string(ctx.recipe_name) +
                    " (the player was told)"};
    }
    return grant_quick_stacks_into_inventory(inventory, ctx.cargo, stacks, *ctx.catalog);
}
struct QuickWireEvents {
    std::vector<bootstrap::ItemEvent> removes;
    std::vector<bootstrap::ItemEvent> adds;
    std::vector<bootstrap::ItemEvent> glint_removes;
    std::vector<bootstrap::ItemEvent> glint_ahead;
};
QuickWireEvents build_quick_wire_events(const items::Outcome &taken, const std::vector<items::Outcome> &granted, const items::Outcome &glint_paid,
                                        const repl::GuidValue &txn, const std::function<void(std::vector<bootstrap::ItemEvent> &, bool)> &add_toasts) {
    QuickWireEvents ev;
    ev.removes = item_events_for(taken);
    for (bootstrap::ItemEvent &e : ev.removes)
        e.instigated = true;
    add_toasts(ev.removes, true);
    for (const items::Outcome &o : granted) {
        std::vector<bootstrap::ItemEvent> part = item_events_for(o);
        ev.adds.insert(ev.adds.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    add_toasts(ev.adds, false);
    for (auto it = ev.adds.rbegin(); it != ev.adds.rend(); ++it) {
        if (it->type == repl::StorageEventType::Add) {
            it->craft_txn = txn;
            break;
        }
    }
    ev.glint_removes = item_events_for(glint_paid);
    if (!ev.glint_removes.empty()) {
        const size_t first_adds = std::min(ev.adds.size(), bootstrap::kMaxEventsPerChain);
        const size_t room = bootstrap::kMaxEventsPerChain > first_adds ? bootstrap::kMaxEventsPerChain - first_adds : 0;
        if (ev.glint_removes.size() > room) {
            ev.glint_ahead.assign(ev.glint_removes.begin(), ev.glint_removes.end() - static_cast<std::ptrdiff_t>(room));
            ev.glint_removes.erase(ev.glint_removes.begin(), ev.glint_removes.end() - static_cast<std::ptrdiff_t>(room));
        }
    }
    return ev;
}
void queue_item_event_chains(const std::vector<bootstrap::ItemEvent> &events, Clock::time_point when, const QueueLaterFn &queue_later) {
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() +
                                                         static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue_later("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); }, when, true);
    }
}
Clock::time_point queue_quick_wire_events(const QuickWireEvents &ev, int64_t tier, int64_t delta, Clock::time_point now, const QueueLaterFn &queue_later) {
    queue_item_event_chains(ev.removes, now, queue_later);
    const Clock::time_point tick = now + kQuickTickAfterRemoves;
    queue_item_event_chains(ev.glint_ahead, tick, queue_later);
    const std::vector<bootstrap::ItemEvent> glint_rem = ev.glint_removes;
    for (size_t at_event = 0; at_event < ev.adds.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(ev.adds.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     ev.adds.begin() +
                                                         static_cast<std::ptrdiff_t>(std::min(ev.adds.size(), at_event + bootstrap::kMaxEventsPerChain)));
        if (at_event == 0) {
            queue_later(
                "craft made",
                [tier, delta, part, glint_rem](bootstrap::PlayerBootstrap &pb) {
                    return tier == items::kGlintTier ? pb.item_payment_answer(tier, delta, glint_rem, part) : pb.purchase_answer(tier, delta, part);
                },
                tick, true);
        } else {
            queue_later("craft items", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); }, tick, true);
        }
    }
    queue_later("OwnerInfo", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, tick, true);
    return tick;
}
void queue_quick_cargo_equipment(const items::Inventory &inventory, Clock::time_point tick, const QueueLaterFn &queue_later) {
    const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory);
    const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory);
    queue_later(
        "HeldItems and AppearanceIDs",
        [held, appearance](bootstrap::PlayerBootstrap &pb) -> std::optional<bootstrap::Send> {
            return pb.apply_equipment(held, appearance, 1) ? pb.equipment_update() : std::nullopt;
        },
        tick, true);
}
professions::Professions with_quick_profession_xp(professions::Professions after, const crafting::JobRewards &made) {
    if (after.valid() && made.profession_xp > 0 && made.profession != 0)
        after.add_xp(made.profession, made.profession_xp);
    return after;
}
leveling::XpResult apply_quick_xp(const PlayerWorld *world, int64_t adv_xp, leveling::Progress &progress, bool &changed, size_t &levels_gained) {
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
template <typename WireT> StepBuilder make_quick_xp_tracker(int64_t total, int64_t debt, std::vector<WireT> wire) {
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
struct QuickProgressWire {
    bool xp_changed = false;
    bool leveled = false;
    int level = 1;
    StepBuilder xp_tracker;
    const professions::Professions *professions = nullptr;
    const std::vector<professions::Change> *changes = nullptr;
    const std::vector<repl::SkillOnActionbar> *skill_updates = nullptr;
};
void queue_quick_profession_level_ups(const professions::Professions *profs, const std::vector<professions::Change> *changes, Clock::time_point tick,
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
            "BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap &pb) { return pb.level_up_broadcast(category, level); }, tick, false);
    }
}
void queue_quick_progression(const QuickProgressWire &pw, Clock::time_point tick, const QueueLaterFn &queue_later) {
    if (pw.xp_changed || pw.leveled || (pw.changes && !pw.changes->empty()))
        queue_later("XPTracker", pw.xp_tracker, tick, true);
    if (pw.leveled) {
        const int level = pw.level;
        queue_later(
            "BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap &pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, tick,
            false);
        queue_later("pawn state", [](bootstrap::PlayerBootstrap &pb) { return pb.pawn_state_update("level and stats"); }, tick, true);
    }
    queue_quick_profession_level_ups(pw.professions, pw.changes, tick, queue_later);
    if (!pw.skill_updates)
        return;
    for (const repl::SkillOnActionbar &u : *pw.skill_updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap &pb) { return pb.skill_on_actionbar(u); }, tick, true);
    }
}
struct QuickLogSummary {
    const PlayerWorld *world = nullptr;
    std::string_view recipe_name;
    std::string_view station_name;
    const crafting::Plan *plan = nullptr;
    const crafting::JobRewards *rewards = nullptr;
    const std::map<int64_t, std::string> *shown_name = nullptr;
    const std::vector<svc::InventoryItem> *fresh = nullptr;
    bool cargo = false;
    bool leveled = false;
    int64_t before_copper = 0;
    int64_t after_copper = 0;
    size_t taken_events = 0;
    const professions::Professions *professions = nullptr;
    const std::vector<professions::Change> *changes = nullptr;
    const leveling::XpResult *xp = nullptr;
};
void append_quick_xp_log_suffix(const QuickLogSummary &s, std::string &line) {
    if (s.changes && s.professions) {
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
std::string format_quick_make_log_line(const QuickLogSummary &s) {
    const crafting::JobRewards &rewards = *s.rewards;
    std::string line = "made " + std::string(s.recipe_name) + " at " + std::string(s.station_name) + ": ";
    for (size_t i = 0; i < rewards.products.size(); ++i) {
        const crafting::Product &p = rewards.products[i];
        const auto it = s.shown_name->find(p.item);
        const std::string label = it != s.shown_name->end() ? it->second : std::to_string(p.item);
        line += (i ? ", " : "") + label + " x" + std::to_string(p.quantity);
    }
    const items::ItemRarityFacts product_facts = s.world->item_rarity_facts(rewards.products.front().item);
    const double on_scale = crafting::tier_scale(product_facts, rewards.quality);
    const std::string as_tier =
        fmt::format("{:.2f}", rewards.quality_exact) + " exactly, nearest whole; " + std::string(items::rarity_name(product_facts.rarity_of(rewards.quality))) +
        " +" + fmt::format("{:.1f}", (on_scale - std::floor(on_scale)) * 100.0) + " %, " + fmt::format("{:.4f}", on_scale) + " on the tier scale";
    const svc::InventoryItem &first_fresh = s.fresh->front();
    const std::string cargo_desc =
        s.cargo ? " into the cargo slot at quality " + std::to_string(first_fresh.quality) + " (rarity " +
                      std::string(items::rarity_name(s.world->item_rarity_facts(first_fresh.item_record_id).rarity_of(first_fresh.quality))) + ")"
                : " at quality " + std::to_string(rewards.quality) + " (" + as_tier + ") in " + std::to_string(s.fresh->size()) + " stack(s)";
    line += cargo_desc + "; " + std::to_string(s.plan->cost) + " " + price_unit(s.plan->currency) + " paid" +
            (s.plan->currency == items::kMoneyTier ? " (" + std::to_string(s.before_copper) + " -> " + std::to_string(s.after_copper) + ")" : std::string()) +
            ", " + std::to_string(s.taken_events) + " stack(s) of materials taken";
    if (rewards.ratings.any()) {
        line += "; artisan ratings: quality " + std::to_string(std::lround(rewards.ratings.quality)) + " adds " + fmt::format("{:.2f}", rewards.quality_added) +
                " to the base, experience " + std::to_string(std::lround(rewards.ratings.xp)) + " (x" + fmt::format("{:.3f}", rewards.xp_multiplier) + ")";
    }
    append_quick_xp_log_suffix(s, line);
    return line;
}
struct QuickCraftTxEnv {
    const items::ItemCatalog *catalog = nullptr;
    int level = 1;
    int64_t created_ms = 0;
    std::function<crafting::Ratings(int64_t)> ratings_of;
    std::function<bool(const std::vector<crafting::Product> &)> is_cargo;
    std::function<std::string()> mint_guid;
};
struct QuickCraftTxResult {
    bool ok = false;
    const char *callout = nullptr;
    std::string refusal;
    bool cargo = false;
    int64_t before_copper = 0;
    QuickPlanResult planned;
    std::optional<crafting::JobRewards> made;
    QuickPaymentResult payment;
    QuickGrantResult grant_res;
};
QuickCraftTxResult execute_quick_craft_tx(const repl::StartCraft &req, const QuickFactsInput &in, const QuickCraftTxEnv &env, items::Inventory &inventory,
                                          items::Wallet &wallet) {
    const items::Inventory kept_bags = inventory;
    const items::Wallet kept_purse = wallet;
    const auto fail = [&](const char *callout, std::string why) {
        inventory = kept_bags;
        wallet = kept_purse;
        return QuickCraftTxResult{false, callout, std::move(why)};
    };
    QuickCraftTxResult out;
    out.planned = plan_quick_craft(req, in);
    if (!out.planned.ok)
        return fail(nullptr, out.planned.refusal);
    const content::CraftRecipe &recipe = *out.planned.recipe;
    out.before_copper = wallet.copper();
    out.payment = take_and_pay_quick_make(inventory, wallet, out.planned.plan, in.world);
    if (!out.payment.ok)
        return fail(nullptr, out.payment.refusal);
    const uint64_t seed = crafting::seed_of(items::guid_hex(req.transaction));
    out.made = roll_quick_make_rewards(in.world->content(), *in.world->craft_recipes(), recipe, out.payment.paid,
                                       {env.ratings_of(recipe.profession), {env.level, seed}});
    if (!out.made || out.made->products.empty())
        return fail(nullptr, recipe.name + " makes nothing the server knows");
    out.cargo = env.is_cargo(out.made->products);
    out.grant_res = prepare_and_grant_quick_products(inventory, *out.made,
                                                     {in.world, in.world->content(), env.catalog, recipe.name, out.cargo,
                                                      crafting::craft_mean_rarity(out.payment.paid), out.made->quality, env.created_ms, env.mint_guid});
    if (!out.grant_res.ok)
        return fail(out.grant_res.callout, out.grant_res.refusal);
    out.ok = true;
    return out;
}
std::optional<std::string> check_active_quick_make(const crafting::QuickMake &quick, const world::ActorRegistry *registry, const ActorView *view,
                                                   const GoodPosition &at) {
    const world::WorldActor *a = registry ? registry->find(quick.station) : nullptr;
    if (!a)
        return "the station left the world";
    if (!view || view->state_of(a->id) != world::ClientState::Awake)
        return "the client does not have the station open any more";
    const double sx = a->x - at.x;
    const double sy = a->y - at.y;
    const double sz = a->z - at.z;
    const double distance = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (distance >= crafting::kStationRangeCm)
        return "the station is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away";
    const double drift = std::sqrt((at.x - quick.x) * (at.x - quick.x) + (at.y - quick.y) * (at.y - quick.y) + (at.z - quick.z) * (at.z - quick.z));
    if (drift > crafting::kQuickDriftCm) {
        return "the player moved " + std::to_string(static_cast<int>(std::lround(drift))) +
               " cm since the craft began (a craft needs standing still; at most " + std::to_string(static_cast<int>(crafting::kQuickDriftCm)) + " cm)";
    }
    return std::nullopt;
}
const world::WorldActor *find_quick_station(const world::ActorRegistry *reg, int64_t station_id) {
    return reg ? reg->find(station_id) : nullptr;
}
const char *quick_progress_reason(const std::vector<professions::Change> &ch, const leveling::XpResult &xp) {
    if (ch.empty() && !xp.changed())
        return nullptr;
    return xp.leveled() ? "a level-up" : "a craft was made";
}
bool fail_quick_make_tx(const QuickCraftTxResult &tx, crafting::QuickMake &quick, size_t &quick_interrupted, std::string_view player_name,
                        spdlog::level::level_enum log_lvl) {
    spdlog::log(log_lvl,
                "[session] {}: the craft of {} at {} cannot be made now, "
                "nothing was taken: {}",
                player_name, quick.recipe_name, quick.station_name, tx.refusal);
    quick = crafting::QuickMake{};
    ++quick_interrupted;
    return false;
}
struct QuickMakeCommitEnv {
    const PlayerWorld *world;
    std::string player_name;
    spdlog::level::level_enum log_lvl;
    const items::Inventory *inventory;
    int64_t copper_after;
    const professions::Professions *professions;
    const std::vector<professions::Change> *changes;
    StepBuilder xp_tracker;
    const leveling::Progress *progress;
    const leveling::XpResult *xp;
    const std::vector<repl::SkillOnActionbar> *skill_updates;
    std::function<void(std::vector<bootstrap::ItemEvent> &, bool)> add_toasts;
    QueueLaterFn q_later;
    std::function<void(const items::Outcome &)> note_crate;
    std::function<void(Clock::time_point)> on_cargo_made;
};
void dispatch_completed_quick_make(const repl::StartCraft &req, const QuickCraftTxResult &tx, const std::string &station_name, Clock::time_point now,
                                   const QuickMakeCommitEnv &env) {
    const QuickWireEvents wire_ev = build_quick_wire_events(tx.payment.taken, tx.grant_res.granted, tx.payment.glint_paid, req.transaction, env.add_toasts);
    const Clock::time_point tick = queue_quick_wire_events(wire_ev, tx.planned.plan.currency, -tx.planned.plan.cost, now, env.q_later);
    if (tx.cargo) {
        queue_quick_cargo_equipment(*env.inventory, tick, env.q_later);
        for (const items::Outcome &o : tx.grant_res.granted)
            env.note_crate(o);
        env.on_cargo_made(tick);
    }
    queue_quick_progression({env.xp->changed(), env.xp->leveled(), env.progress->level, env.xp_tracker, env.professions, env.changes, env.skill_updates}, tick,
                            env.q_later);
    spdlog::log(env.log_lvl, "[session] {}: {}", env.player_name,
                format_quick_make_log_line({env.world, tx.planned.recipe->name, station_name, &tx.planned.plan, &*tx.made, &tx.grant_res.stacks.shown_name,
                                            &tx.grant_res.stacks.fresh, tx.cargo, env.xp->leveled(), tx.before_copper, env.copper_after,
                                            tx.payment.taken.events.size(), env.professions, env.changes, env.xp}));
}
} // namespace
void PlayerSession::on_quick_start(Connection &conn, const repl::StartCraft &req, const world::WorldActor &a, Clock::time_point now) {
    constexpr size_t kQuickLogged = 30;
    const auto refuse = [&](const std::string &why) {
        ++quick_refused_;
        spdlog::log(level_for(quick_refused_logged_, kQuickLogged), "[session] {}: a craft is refused (nothing is taken): {}", name(), why);
        queue(
            "ClientInterruptCraft", [](bootstrap::PlayerBootstrap &pb) { return pb.craft_interrupted(); }, conn,
            /*must_build=*/true);
    };
    const QuickPlanResult planned = plan_quick_craft(req, {world_, &a, &taught_, &professions_, &inventory_, &wallet_});
    if (!planned.ok)
        return refuse(planned.refusal);
    const content::CraftReward *reward = world_->craft_recipes()->reward_of(*planned.recipe);
    if (!reward || reward->containers.empty())
        return refuse(planned.recipe->name + " has no reward the server can give: it makes nothing");
    if (cargo_crate_made(crafting::roll_products(*reward, 1, 0)) && inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        say(conn, "You are already carrying a crate.", now);
        return refuse("the cargo slot holds a crate already, and " + planned.recipe->name + " makes another: a second crate cannot be carried");
    }
    const GoodPosition at = checker_.good();
    quick_ = {true, a.id, a.station.name, planned.recipe->name, req, now, now + std::chrono::milliseconds(planned.plan.duration_ms), at.x, at.y, at.z};
    ++quick_started_;
    spdlog::log(level_for(quick_logged_, kQuickLogged),
                "[session] {}: begins to craft {} at {} ({:.1f} s; nothing is "
                "taken until it is made): {} {} ({} each with the tax of "
                "{:.2f}), {} stack(s) of materials",
                name(), planned.recipe->name, a.station.name, planned.plan.duration_ms / 1000.0, planned.plan.cost, price_unit(planned.plan.currency),
                planned.plan.unit_taxed, planned.tax.percent, planned.plan.takes.size());
}
bool PlayerSession::cancel_quick_make(Connection *conn, const std::string &why) {
    if (!quick_.active)
        return false;
    const std::string recipe = quick_.recipe_name;
    const std::string station = quick_.station_name;
    quick_ = crafting::QuickMake{};
    ++quick_interrupted_;
    spdlog::log(level_for(quick_logged_, 30), "[session] {}: the craft of {} at {} ends with nothing taken: {}", name(), recipe, station, why);
    if (conn)
        queue(
            "ClientInterruptCraft", [](bootstrap::PlayerBootstrap &pb) { return pb.craft_interrupted(); }, *conn,
            /*must_build=*/true);
    return true;
}
void PlayerSession::on_interrupt_station_craft(Connection &conn, const protocol::repl::ClientRpc &rpc, Clock::time_point now) {
    if (!within_budget(vendor_budget_, "ServerInterruptStationCraft", now))
        return;
    const auto station = repl::parse_interrupt_craft(rpc.call);
    if (!station) {
        if (!std::exchange(interrupt_unreadable_logged_, true)) {
            spdlog::warn("[session] {}: a ServerInterruptStationCraft does not read "
                         "({} parameter bits): ignored (further ones are not logged)",
                         name(), rpc.call.param_bits);
        }
        return;
    }
    if (!quick_.active)
        return spdlog::debug("[session] {}: ServerInterruptStationCraft with no "
                             "craft under way: ignored",
                             name());
    const world::WorldActor *a = registry_ ? registry_->find(station->ObjectId) : nullptr;
    if (a && a->id != quick_.station) {
        return spdlog::debug("[session] {}: ServerInterruptStationCraft names {}, "
                             "the craft runs at {}: ignored",
                             name(), a->station.name, quick_.station_name);
    }
    cancel_quick_make(&conn, "the client closed the window or pressed Cancel "
                             "(ServerInterruptStationCraft)");
}
void PlayerSession::run_quick_make(Connection &conn, Clock::time_point now) {
    if (!quick_.active || failed_ || finished_ || !join_.empty())
        return;
    if (dead_)
        return (void)cancel_quick_make(&conn, "the player is dead");
    if (mounted())
        return (void)cancel_quick_make(&conn, "the player is on a mount");
    if (const auto cancel_why = check_active_quick_make(quick_, registry_, view_.get(), checker_.good())) {
        cancel_quick_make(&conn, *cancel_why);
        return;
    }
    if (now >= quick_.ready && room_for(24))
        complete_quick_make(conn, now);
}
bool PlayerSession::complete_quick_make(Connection &conn, Clock::time_point now) {
    const repl::StartCraft req = quick_.request;
    const QuickFactsInput facts_in{world_, find_quick_station(registry_, quick_.station), &taught_, &professions_, &inventory_, &wallet_};
    const QuickCraftTxEnv tx_env{&item_catalog(),
                                 progress_.level,
                                 wall_ms() / 1000 * 1000,
                                 [this](int64_t p) { return artisan_ratings(p); },
                                 [this](const auto &prods) { return cargo_crate_made(prods); },
                                 [this]() { return fresh_guid(); }};
    const QuickCraftTxResult tx = execute_quick_craft_tx(req, facts_in, tx_env, inventory_, wallet_);
    if (!tx.ok) {
        if (tx.callout)
            say(conn, tx.callout, now);
        queue(
            "ClientInterruptCraft", [](bootstrap::PlayerBootstrap &pb) { return pb.craft_interrupted(); }, conn,
            /*must_build=*/true);
        return fail_quick_make_tx(tx, quick_, quick_interrupted_, name(), level_for(quick_logged_, 30));
    }
    const std::string station_name = std::exchange(quick_, crafting::QuickMake{}).station_name;
    inventory_dirty_ = inventory_changed_ = true;
    ++items_applied_;
    ++quick_made_;
    const std::vector<professions::Change> changes =
        professions_.valid() ? set_professions(with_quick_profession_xp(professions_, *tx.made)) : std::vector<professions::Change>();
    const leveling::XpResult xp = apply_quick_xp(world_, tx.made->adventuring_xp, progress_, progress_changed_, levels_gained_);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (xp.leveled())
        finish_level_up(xp, false, now, rank_effects, skill_updates);
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    pb_.set_currency(items::kNodeCurrency, wallet_.balance(items::kNodeCurrency));
    dispatch_completed_quick_make(req, tx, station_name, now,
                                  {world_, name(), level_for(quick_made_logged_, 30), &inventory_, wallet_.copper(), &professions_, &changes,
                                   make_quick_xp_tracker(progress_.xp, progress_.debt, profession_wire(changes)), &progress_, &xp, &skill_updates,
                                   [this](auto &evs, bool rem) { add_item_toasts(evs, rem); },
                                   [this, &conn](const char *w, StepBuilder b, Clock::time_point t, bool m) { queue_later(w, std::move(b), conn, t, m); },
                                   [this](const items::Outcome &o) { note_cargo_crate(o); },
                                   [this, &conn](Clock::time_point tick) {
                                       apply_gear_stats("a crate was made");
                                       sync_cargo(conn, tick);
                                   }});
    hand_over_inventory("a craft was made", /*periodic=*/false);
    if (const char *why = quick_progress_reason(changes, xp))
        hand_over_progress(why);
    return true;
}
bool PlayerSession::cargo_crate_made(const std::vector<crafting::Product> &made) const {
    if (made.size() != 1 || made.front().quantity != 1)
        return false;
    const auto info = item_catalog().info(made.front().item);
    return info && info->cargo_crate();
}
void PlayerSession::say(Connection &conn, const char *text, Clock::time_point now) {
    constexpr int32_t kCalloutType = 1;
    constexpr int32_t kCalloutPointSize = 32;
    constexpr std::chrono::seconds kCalloutLife{3};
    if (!room_for(2))
        return;
    const uint64_t key_of_callout = (static_cast<uint64_t>(pb_.setup().player_id) << 32) | (0x40000000u + gather_callout_seq_++);
    const int64_t ticks =
        items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    queue(
        "a callout",
        [ticks, key_of_callout, text](bootstrap::PlayerBootstrap &pb) { return pb.callout_add(text, kCalloutType, kCalloutPointSize, ticks, key_of_callout); },
        conn);
    gather_callouts_.push_back({now + kCalloutLife, key_of_callout});
}
bool PlayerSession::gm_teach_recipe(Connection &conn, int64_t recipe, const std::string &by) {
    if (std::find(taught_.begin(), taught_.end(), recipe) != taught_.end() || taught_.size() >= bootstrap::kMaxKnownRecipes)
        return false;
    taught_.push_back(recipe);
    hand_over_inventory("a recipe was taught by a game master",
                        /*periodic=*/false);
    if (pb_.set_known_recipes(taught_))
        queue(
            "OwnerInfo", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, conn,
            /*must_build=*/true);
    spdlog::info("[session] {}: was taught recipe {} by {} ({} taught in all)", name(), recipe, by, taught_.size());
    return true;
}
bool PlayerSession::gm_forget_recipe(Connection &conn, int64_t recipe, const std::string &by) {
    const auto it = std::find(taught_.begin(), taught_.end(), recipe);
    if (it == taught_.end())
        return false;
    taught_.erase(it);
    hand_over_inventory("a taught recipe was taken away by a game master",
                        /*periodic=*/false);
    if (pb_.set_known_recipes(taught_))
        queue(
            "OwnerInfo", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, conn,
            /*must_build=*/true);
    spdlog::info("[session] {}: was made to forget recipe {} by {} ({} taught in all)", name(), recipe, by, taught_.size());
    return true;
}
} // namespace aoc::worldd
