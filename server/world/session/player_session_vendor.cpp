// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "dialogue/talk.h"
#include "items/acquired.h"
#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "session/player_session_internal.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"

namespace aoc {
namespace worldd {

using namespace ::sandal::worldd::session::detail;

namespace {

vendor::ShopFacts shop_facts_for(const world::CityNodes *nodes, const GoodPosition &player,
                                 const world::WorldActor *npc) {
  vendor::ShopFacts facts;
  facts.nodes = nodes;
  facts.player_x = player.x;
  facts.player_y = player.y;
  if (npc) {
    facts.has_npc = true;
    facts.npc_x = npc->x;
    facts.npc_y = npc->y;
  }
  return facts;
}

void assign_unique_guids(std::vector<svc::InventoryItem> &fresh, const std::function<std::string()> &mint_guid) {
  for (svc::InventoryItem &it : fresh) {
    it.instance_guid = mint_guid();
    for (const svc::InventoryItem &other : fresh) {
      if (&other != &it && other.instance_guid == it.instance_guid) it.instance_guid = mint_guid();
    }
  }
}

std::vector<svc::InventoryItem> build_gm_vendor_items(const std::vector<use::Granted> &stacks,
                                                      const std::function<std::string()> &mint_guid) {
  std::vector<svc::InventoryItem> fresh;
  const int64_t created =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() *
      1000;
  for (const use::Granted &g : stacks) {
    fresh.push_back({svc::ItemContainer::Backpack, 0, g.record, {}, g.quantity, g.quality, g.durability, created});
  }
  assign_unique_guids(fresh, mint_guid);
  return fresh;
}

vendor::TownShops::Quote quote_town_shop(const vendor::TownShops &shops, int64_t vendor_def,
                                         const repl::VendorPurchaseCart &cart, const vendor::ShopFacts &facts,
                                         const std::string &who) {
  bool unreadable = false;
  const vendor::TownShops::Quote quote =
      shops.quote(vendor_def, cart, shops.default_rate(vendor_def), [&](const content::VendorPredicateRow &p) {
        const auto v = dialogue::evaluate_expression(p.expression, facts);
        if (!v) unreadable = true;
        return v && v->i != 0;
      });
  if (!quote.ok && unreadable)
    spdlog::warn("[session] {}: a shop predicate does not evaluate: the listing is not sold", who);
  return quote;
}

bool build_town_shop_items(const content::ContentDb &db, const vendor::TownShops::Quote &quote,
                           std::pair<int64_t, int64_t> copper_and_glint, const std::function<std::string()> &mint_guid,
                           std::pair<std::vector<svc::InventoryItem> &, std::string &> out) {
  const bool in_glint = quote.currency == items::kGlintTier;
  if (!in_glint && copper_and_glint.first < quote.total) {
    out.second = "the purse holds " + std::to_string(copper_and_glint.first) + " copper and the cart costs " +
                 std::to_string(quote.total) + " (not enough money)";
    return false;
  }
  if (in_glint && copper_and_glint.second < quote.total) {
    out.second = "the bags hold " + std::to_string(copper_and_glint.second) + " Glint T1 and the cart costs " +
                 std::to_string(quote.total) + " (not enough Glint)";
    return false;
  }
  const int64_t created =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() *
      1000;
  for (const vendor::TownShops::Line &line : quote.lines) {
    std::string error;
    const auto rec = db.item(line.item, error);
    if (!rec) {
      out.second =
          error.empty() ? "content.db has no item record " + std::to_string(line.item) : "content.db failed: " + error;
      return false;
    }
    stacks_of({line.item, line.quantity, created, 0}, *rec, out.first);
  }
  assign_unique_guids(out.first, mint_guid);
  return true;
}

struct SalePlan {
  bool ok = false;
  std::string refusal;
  std::vector<svc::InventoryItem> sold;
  int64_t total = 0;
};

SalePlan plan_town_shop_sale(const items::Inventory &inv, const content::ContentDb &db, const vendor::TownShops &shops,
                             const repl::VendorTransactionCart &cart, const std::pair<int64_t, int> &shop_and_level) {
  if (cart.instances.empty() || cart.instances.size() > vendor::kMaxShopCartLines) {
    return {false, "a sale of " + std::to_string(cart.instances.size()) + " items", {}, 0};
  }
  SalePlan plan;
  std::set<std::string> seen;
  for (const repl::GuidValue &g : cart.instances) {
    const std::string guid = items::guid_hex(g);
    if (!seen.insert(guid).second) return {false, "the same item twice in one sale", {}, 0};
    const svc::InventoryItem *held = inv.find_guid(guid);
    if (!held) return {false, "an item that is in none of this character's bags", {}, 0};
    if (!is_bag(held->container)) return {false, "an item that is worn or in the quest storage is not sold", {}, 0};
    std::string error;
    const auto facts = db.item_sale(held->item_record_id, error);
    if (!facts) return {false, error, {}, 0};
    const vendor::SaleQuote quote =
        vendor::quote_sale(shops.data(), shop_and_level.first, held->item_record_id, held->quantity, held->quality,
                           *facts, items::kMoneyTier, shop_and_level.second);
    if (!quote.ok)
      return {false, "item " + std::to_string(held->item_record_id) + " is not sold: " + quote.refusal, {}, 0};
    plan.total += quote.total;
    if (plan.total > items::kMoneyCap) break;
    plan.sold.push_back(*held);
  }
  plan.ok = true;
  return plan;
}

struct MendedItem {
  svc::InventoryItem item;
  float full = 0;
};

struct RepairPlan {
  bool ok = false;
  std::string refusal;
  std::vector<MendedItem> mended;
  int64_t total = 0;
};

RepairPlan plan_town_shop_repair(const items::Inventory &inv, const content::ContentDb &db,
                                 const vendor::TownShops &shops, const repl::VendorTransactionCart &cart,
                                 const std::pair<int64_t, int> &shop_and_level) {
  if (cart.instances.empty() || cart.instances.size() > vendor::kMaxShopCartLines) {
    return {false, "a repair of " + std::to_string(cart.instances.size()) + " items", {}, 0};
  }
  RepairPlan plan;
  const vendor::TaxRate tax = shops.default_rate(shop_and_level.first);
  std::set<std::string> seen;
  for (const repl::GuidValue &g : cart.instances) {
    const std::string guid = items::guid_hex(g);
    if (!seen.insert(guid).second) return {false, "the same item twice in one repair", {}, 0};
    const svc::InventoryItem *held = inv.find_guid(guid);
    if (!held) return {false, "an item that is in none of this character's storages", {}, 0};
    if (held->container != svc::ItemContainer::Backpack && held->container != svc::ItemContainer::Equipment) {
      return {false, "only an item in the item bag or worn is repaired", {}, 0};
    }
    std::string error;
    const auto rec = db.item(held->item_record_id, error);
    if (!rec)
      return {false,
              error.empty() ? "content.db has no item record " + std::to_string(held->item_record_id) : error,
              {},
              0};
    const auto facts = db.item_sale(held->item_record_id, error);
    if (!facts) return {false, error, {}, 0};
    const float full = rec->max_durability_at(held->quality);
    const vendor::RepairQuote quote = vendor::quote_repair(
        shops.data(), shop_and_level.first, held->item_record_id, held->quality, static_cast<double>(held->durability),
        static_cast<double>(full), *facts, items::kMoneyTier, tax, shop_and_level.second);
    if (!quote.ok)
      return {false, "item " + std::to_string(held->item_record_id) + " is not repaired: " + quote.refusal, {}, 0};
    plan.total += quote.total;
    plan.mended.push_back({*held, full});
  }
  plan.ok = true;
  return plan;
}

repl::UnavailableList build_and_log_talk_shop_unavailable(
    const vendor::TownShops &shops, int64_t def, const vendor::ShopFacts &facts, const world::WorldActor &npc,
    std::pair<std::string_view, spdlog::level::level_enum> log_ctx) {
  bool unreadable = false;
  const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow &p) {
    const auto v = dialogue::evaluate_expression(p.expression, facts);
    if (!v) unreadable = true;
    return v && v->i != 0;
  };
  repl::UnavailableList unavailable = shops.unavailable(def, holds);
  size_t hidden_tabs = 0, listings = 0;
  for (const repl::UnavailableTab &t : unavailable) {
    hidden_tabs += t.inventory.hidden ? 1 : 0;
    listings += t.listings.size();
  }
  spdlog::log(log_ctx.second,
              "[session] {}: opens the shop {} of {} from the conversation: {} tab(s) listed as unavailable ({} "
              "hidden), {} listing(s){}",
              log_ctx.first, shops.def(def)->name, npc.npc.name, unavailable.size(), hidden_tabs, listings,
              unreadable ? "; a predicate does not evaluate and does not hold" : "");
  return unavailable;
}

vendor::PurchasePlan plan_gm_vendor_purchase(const PlayerWorld *world, int64_t vendor_def,
                                             const repl::VendorPurchaseCart &cart, size_t free_slots) {
  const content::ContentDb *db = world ? world->content() : nullptr;
  const vendor::GmVendors *gm = world ? world->gm_vendors() : nullptr;
  if (!db || !gm) return {false, "worldd has no GM vendors", {}, {}};
  const vendor::GmVendor *def = gm->find(vendor_def);
  if (!def) return {false, "the open window's vendor def is unknown", {}, {}};
  return gm->plan_purchase(*db, *def, cart, free_slots);
}

bool execute_town_shop_inventory_purchase(items::Inventory &inv, const items::ItemCatalog &catalog,
                                          std::pair<bool, int64_t> currency_and_total,
                                          std::pair<std::vector<svc::InventoryItem> &, items::Outcome &> fresh_and_paid,
                                          std::pair<items::Outcome &, std::string &> out_and_err) {
  const auto [in_glint, total] = currency_and_total;
  const items::Inventory kept = inv;
  if (in_glint) {
    fresh_and_paid.second = inv.take(items::kGlintT1Item, total);
    if (!fresh_and_paid.second.ok) {
      out_and_err.second = fresh_and_paid.second.refusal;
      return false;
    }
  }
  out_and_err.first = inv.grant(std::move(fresh_and_paid.first), catalog);
  if (!out_and_err.first.ok || (in_glint && fresh_and_paid.second.events.size() + out_and_err.first.events.size() >
                                                bootstrap::kMaxEventsPerChain)) {
    inv = kept;
    out_and_err.second = out_and_err.first.ok ? "the payment and the goods are more events than one answer holds"
                                              : out_and_err.first.refusal;
    return false;
  }
  return true;
}

template <typename QueueFn>
void queue_copper_purchase_events(const std::vector<bootstrap::ItemEvent> &events, int64_t delta, QueueFn &&queue_fn) {
  for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
    const std::vector<bootstrap::ItemEvent> part(
        events.begin() + static_cast<std::ptrdiff_t>(at_event),
        events.begin() +
            static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
    if (at_event == 0) {
      queue_fn("purchase answer", [part, delta](bootstrap::PlayerBootstrap &pb) {
        return pb.purchase_answer(items::kMoneyTier, delta, part);
      });
    } else {
      queue_fn("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); });
    }
  }
}

struct ToastItemCache {
  const content::ContentDb *db = nullptr;
  std::map<int64_t, std::string> names;
  std::map<int64_t, items::ItemRarityFacts> rarities;

  const std::string &name_of(int64_t record) {
    if (const auto known = names.find(record); known != names.end()) return known->second;
    std::string shown = "Unknown item", error;
    items::ItemRarityFacts facts = items::default_rarity_facts();
    if (const auto rec = db ? db->item(record, error) : std::nullopt) {
      shown = !rec->display_name.empty() ? rec->display_name : rec->name;
      facts = *rec;
    }
    rarities.emplace(record, facts);
    return names.emplace(record, std::move(shown)).first->second;
  }
};

bool is_same_storage_slot(const bootstrap::ItemEvent &a, const bootstrap::ItemEvent &b) {
  return a.storage == b.storage && a.slot == b.slot;
}

repl::ItemInstance effective_added_toast_item(const std::vector<bootstrap::ItemEvent> &events, size_t i) {
  const bootstrap::ItemEvent &ev = events[i];
  repl::ItemInstance shown = ev.item;
  if (i > 0 && events[i - 1].type == repl::StorageEventType::Remove && is_same_storage_slot(events[i - 1], ev) &&
      events[i - 1].item.has_item && events[i - 1].item.quantity < ev.item.quantity) {
    shown.quantity = ev.item.quantity - events[i - 1].item.quantity;
  }
  return shown;
}

}  // namespace

const world::WorldActor *PlayerSession::open_vendor(std::string &why) {
  if (vendor_actor_ == 0) {
    why = "no vendor window is open";
    return nullptr;
  }
  const world::WorldActor *a = registry_ ? registry_->find(vendor_actor_) : nullptr;
  const GoodPosition at = checker_.good();
  const vendor::TownShops *shops = world_ ? world_->town_shops() : nullptr;
  const bool town = a && !a->npc.is_gm_vendor && shops && shops->is_open(vendor_def_) &&
                    (vendor_by_talk_ || town_shop_of(*a) == vendor_def_);
  if (!a) {
    why = "the vendor is gone";
  } else if (!a->npc.is_gm_vendor && !town) {
    why = "the NPC is no GM vendor and opens no shop";
  } else if (town) {
    const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
      why = "the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) +
            " m away";
    } else {
      return a;
    }
  } else if (std::hypot(a->x - at.x, a->y - at.y) > kInteractRangeCm) {
    why = "the vendor is " + std::to_string(static_cast<int>(std::hypot(a->x - at.x, a->y - at.y) / 100.0)) + " m away";
  } else {
    return a;
  }
  vendor_actor_ = 0;
  vendor_def_ = 0;
  vendor_by_talk_ = false;
  return nullptr;
}

bool PlayerSession::open_talk_shop(Connection &conn, const world::WorldActor &npc, int64_t def,
                                   Clock::time_point /*now*/) {
  const vendor::TownShops *shops = world_ ? world_->town_shops() : nullptr;
  const dialogue::Book *book = world_ ? world_->dialogue() : nullptr;
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    spdlog::log(level_for(vendor_logged_), "[session] {}: {}'s shop {} does not open: {}", name(), npc.npc.name, def,
                why);
    return false;
  };
  if (!shops || !shops->def(def) || !shops->is_open(def)) return refuse("worldd opens no such shop yet");
  if (!book || book->vendor_defs(npc.npc.dialogue_root).count(def) == 0)
    return refuse("the conversation of " + npc.npc.name + " opens no such shop");
  const GoodPosition at = checker_.good();
  const double dx = npc.x - at.x, dy = npc.y - at.y, dz = npc.z - at.z;
  if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
    return refuse("the shopkeeper is " +
                  std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) +
                  " m away (a shop opens within 12 m)");
  }
  if (vendor_actor_ == npc.id && vendor_def_ == def && vendor_by_talk_) {
    spdlog::log(level_for(vendor_logged_), "[session] {}: the shop {} of {} is open already: not answered again",
                name(), def, npc.npc.name);
    return true;
  }
  if (!room_for(2)) return false;
  vendor_actor_ = npc.id;
  vendor_def_ = def;
  vendor_by_talk_ = true;
  ++vendor_opens_;
  const vendor::ShopFacts facts = shop_facts_for(world_->city_nodes(), at, &npc);
  repl::UnavailableList unavailable =
      build_and_log_talk_shop_unavailable(*shops, def, facts, npc, {name(), level_for(vendor_logged_)});
  const FIntrepidNetworkGUID guid = npc.actor.guid;
  queue(
      "ClientOpenVendor",
      [def, guid, unavailable = std::move(unavailable)](bootstrap::PlayerBootstrap &pb) {
        return pb.open_vendor(def, guid, unavailable);
      },
      conn, /*must_build=*/true);
  return true;
}

void PlayerSession::on_open_vendor(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
  const auto request = repl::parse_server_open_vendor(rpc.call);
  if (!request) {
    if (!vendor_unreadable_logged_) {
      vendor_unreadable_logged_ = true;
      spdlog::warn(
          "[session] {}: a ServerOpenVendor does not read ({} parameter bits): ignored (further ones are not logged)",
          name(), rpc.call.param_bits);
    }
    return;
  }
  if (!within_budget(vendor_budget_, "ServerOpenVendor", now) || !room_for(2)) return;
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    spdlog::log(level_for(vendor_logged_), "[session] {}: the client's ServerOpenVendor for shop {} is refused: {}",
                name(), request->vendor, why);
  };
  if (dead_) return refuse("the character is dead");
  if (!talk_.open()) return refuse("no conversation is open");
  const world::WorldActor *npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
  if (!npc) return refuse("the NPC the player talks to is gone");
  open_talk_shop(conn, *npc, request->vendor, now);
}

void PlayerSession::on_node_tax_request(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
  const auto node = repl::parse_node_tax_request(rpc.call);
  ++tax_requests_;
  if (!node) {
    if (!vendor_unreadable_logged_) {
      vendor_unreadable_logged_ = true;
      spdlog::warn(
          "[session] {}: a ServerGetNodeTaxTable does not read ({} parameter bits): ignored (further ones are not "
          "logged)",
          name(), rpc.call.param_bits);
    }
    return;
  }
  if (!within_budget(tax_budget_, "ServerGetNodeTaxTable", now) || !room_for(1) || dead_) return;
  std::string why;
  const world::WorldActor *a = open_vendor(why);
  if (!a) {
    spdlog::log(level_for(tax_logged_),
                "[session] {}: the client asks for the taxes of node {} but has no vendor window open ({}): ignored",
                name(), *node, why);
    return;
  }
  const int64_t asked = *node;
  if (!a->npc.is_gm_vendor) {
    const world::CityNodes *nodes = world_ ? world_->city_nodes() : nullptr;
    if (nodes && !nodes->by_guid(asked)) {
      spdlog::log(
          level_for(tax_logged_),
          "[session] {}: the window of {} asks for the taxes of node {}, which the world does not know: no answer",
          name(), a->npc.name, asked);
      return;
    }
    ++tax_tables_sent_;
    spdlog::log(level_for(tax_logged_),
                "[session] {}: the window of {} asks for the taxes of node {}: sending the default table ({} tags)",
                name(), a->npc.name, asked, vendor::default_table(asked).entries.size());
    queue(
        "ClientSetNodeTaxTable",
        [asked](bootstrap::PlayerBootstrap &pb) { return pb.node_tax_table(vendor::default_table(asked)); }, conn,
        /*must_build=*/true);
    return;
  }
  ++tax_tables_sent_;
  spdlog::log(level_for(tax_logged_),
              "[session] {}: the window of {} asks for the taxes of node {}: sending the untaxed table ({} tags)",
              name(), a->npc.name, *node, vendor::untaxed_table(*node).entries.size());
  queue(
      "ClientSetNodeTaxTable",
      [asked](bootstrap::PlayerBootstrap &pb) { return pb.node_tax_table(vendor::untaxed_table(asked)); }, conn,
      /*must_build=*/true);
}

void PlayerSession::on_vendor_purchase(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
  const auto cart = repl::parse_vendor_purchase(rpc.call);
  if (!cart) {
    if (!vendor_unreadable_logged_) {
      vendor_unreadable_logged_ = true;
      spdlog::warn(
          "[session] {}: a ServerHandleVendorPurchase does not read ({} parameter bits): ignored (further ones are not "
          "logged)",
          name(), rpc.call.param_bits);
    }
    return;
  }
  if (!within_budget(vendor_budget_, "ServerHandleVendorPurchase", now) || !room_for(6)) return;
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    refuse_item_request(conn, "ServerHandleVendorPurchase", why, now);
  };
  if (dead_) return refuse("the character is dead: the vendor window closed with the death");
  std::string why;
  const world::WorldActor *a = open_vendor(why);
  if (!a) return refuse(why);
  if (!a->npc.is_gm_vendor) return buy_from_town_shop(conn, *cart, now);
  const size_t free = static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack);
  const vendor::PurchasePlan plan = plan_gm_vendor_purchase(world_, vendor_def_, *cart, free);
  if (!plan.ok) return refuse(plan.refusal);
  std::vector<svc::InventoryItem> fresh = build_gm_vendor_items(plan.stacks, [this] { return fresh_guid(); });
  const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
  if (!outcome.ok) return refuse(outcome.refusal);
  inventory_dirty_ = inventory_changed_ = true;
  ++items_applied_;
  ++vendor_purchases_;
  const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
  for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
    const std::vector<bootstrap::ItemEvent> part(
        events.begin() + static_cast<std::ptrdiff_t>(at),
        events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
    queue(
        "ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); }, conn,
        /*must_build=*/true);
  }
  spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} (free): {} storage event(s)", name(), plan.summary,
              outcome.events.size());
}

int64_t PlayerSession::town_shop_of(const world::WorldActor &npc) const {
  const vendor::TownShops *shops = world_ ? world_->town_shops() : nullptr;
  if (!shops || npc.kind != world::ActorKind::Npc || npc.npc.is_gm_vendor) return 0;
  return shops->shop_of(npc.npc.instance_guid, npc.npc.asset_guid);
}

void PlayerSession::buy_from_town_shop(Connection &conn, const repl::VendorPurchaseCart &cart, Clock::time_point now) {
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    refuse_item_request(conn, "ServerHandleVendorPurchase", why, now);
  };
  const vendor::TownShops *shops = world_ ? world_->town_shops() : nullptr;
  const content::ContentDb *db = world_ ? world_->content() : nullptr;
  if (!shops || !db) return refuse("worldd has no town shops");
  const vendor::ShopFacts facts =
      shop_facts_for(world_->city_nodes(), checker_.good(), registry_ ? registry_->find(vendor_actor_) : nullptr);
  const vendor::TownShops::Quote quote = quote_town_shop(*shops, vendor_def_, cart, facts, name());
  if (!quote.ok) return refuse(quote.refusal);
  const bool in_glint = quote.currency == items::kGlintTier;
  std::vector<svc::InventoryItem> fresh;
  std::string err;
  if (!build_town_shop_items(*db, quote, {wallet_.copper(), inventory_.count_of(items::kGlintT1Item)},
                             [this] { return fresh_guid(); }, {fresh, err}))
    return refuse(err);
  items::Outcome paid, outcome;
  if (!execute_town_shop_inventory_purchase(inventory_, item_catalog(), {in_glint, quote.total}, {fresh, paid},
                                            {outcome, err}))
    return refuse(err);
  const int64_t before = wallet_.copper();
  if (!in_glint) wallet_.pay(quote.total);
  inventory_dirty_ = inventory_changed_ = true;
  ++items_applied_;
  ++vendor_purchases_;
  std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
  add_item_toasts(events, /*removals=*/false);
  const int64_t delta = -quote.total;
  const auto queue_req = [&](const char *label, auto &&fn) {
    queue(label, std::forward<decltype(fn)>(fn), conn, /*must_build=*/true);
  };
  if (in_glint) {
    const std::vector<bootstrap::ItemEvent> paid_events = item_events_for(paid);
    queue_req("purchase answer", [paid_events, events, delta](bootstrap::PlayerBootstrap &pb) {
      return pb.item_payment_answer(items::kGlintTier, delta, paid_events, events);
    });
    spdlog::log(level_for(vendor_logged_),
                "[session] {}: bought {} line(s) at the shop for {} Glint T1: {} + {} storage event(s)", name(),
                quote.lines.size(), quote.total, paid.events.size(), outcome.events.size());
    return;
  }
  queue_copper_purchase_events(events, delta, queue_req);
  pb_.set_currency(items::kCopperCurrency, wallet_.copper());
  queue_req("Currencies", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); });
  spdlog::log(level_for(vendor_logged_),
              "[session] {}: bought {} line(s) at the shop for {} copper ({} -> {}): {} storage event(s)", name(),
              quote.lines.size(), quote.total, before, wallet_.copper(), outcome.events.size());
}

void PlayerSession::add_item_toasts(std::vector<bootstrap::ItemEvent> &events, bool removals) const {
  ToastItemCache cache{world_ ? world_->content() : nullptr, {}, {}};
  const int64_t made = items::ticks_of_ms(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (size_t i = 0; i < events.size(); ++i) {
    bootstrap::ItemEvent &ev = events[i];
    if (!ev.item.has_item) continue;
    if (ev.type == repl::StorageEventType::Add) {
      const repl::ItemInstance shown = effective_added_toast_item(events, i);
      const std::string &shown_name = cache.name_of(static_cast<int64_t>(shown.record_id));
      ev.callout =
          items::acquired_callout(shown, shown_name, cache.rarities.at(static_cast<int64_t>(shown.record_id)), made);
    } else if (removals && ev.type == repl::StorageEventType::Remove) {
      const bool restacked = i + 1 < events.size() && events[i + 1].type == repl::StorageEventType::Add &&
                             is_same_storage_slot(events[i + 1], ev);
      if (!restacked) {
        const std::string &removed_name = cache.name_of(static_cast<int64_t>(ev.item.record_id));
        ev.callout = items::removed_callout(ev.item, removed_name,
                                            cache.rarities.at(static_cast<int64_t>(ev.item.record_id)), made);
      }
    }
  }
}

void PlayerSession::on_vendor_transaction(Connection &conn, const repl::ClientRpc &rpc, Clock::time_point now) {
  const auto cart = repl::parse_vendor_transaction(rpc.call);
  if (!cart) {
    if (!vendor_unreadable_logged_) {
      vendor_unreadable_logged_ = true;
      spdlog::warn(
          "[session] {}: a ServerHandleVendorTransaction does not read ({} parameter bits): ignored (further ones are "
          "not logged)",
          name(), rpc.call.param_bits);
    }
    return;
  }
  if (!within_budget(vendor_budget_, "ServerHandleVendorTransaction", now) || !room_for(6)) return;
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    refuse_item_request(conn, "ServerHandleVendorTransaction", why, now);
  };
  if (dead_) return refuse("the character is dead: the vendor window closed with the death");
  std::string why;
  const world::WorldActor *a = open_vendor(why);
  if (!a) return refuse(why);
  if (a->npc.is_gm_vendor) {
    if (!vendor_sale_logged_) {
      vendor_sale_logged_ = true;
      spdlog::info("[session] {}: a sale to a vendor: GM vendors buy nothing, ignored (further ones are not logged)",
                   name());
    }
    return;
  }
  if (cart->vendor != vendor_def_) return refuse("the cart names another shop than the one that is open");
  if (cart->operation == 2) return sell_to_town_shop(conn, *cart, now);
  if (cart->operation == 4) return repair_at_town_shop(conn, *cart, now);
  return refuse("an operation of " + std::to_string(cart->operation) + " is not made (a sale is 2, a repair 4)");
}

void PlayerSession::sell_to_town_shop(Connection &conn, const repl::VendorTransactionCart &cart,
                                      Clock::time_point now) {
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    refuse_item_request(conn, "ServerHandleVendorTransaction", why, now);
  };
  const vendor::TownShops *shops = world_ ? world_->town_shops() : nullptr;
  const content::ContentDb *db = world_ ? world_->content() : nullptr;
  if (!shops || !db) return refuse("worldd has no town shops");
  const SalePlan plan = plan_town_shop_sale(inventory_, *db, *shops, cart, {vendor_def_, progress_.level});
  if (!plan.ok) return refuse(plan.refusal);
  if (plan.total > wallet_.room()) {
    return refuse("the sale comes to " + std::to_string(plan.total) + " copper and the purse holds " +
                  std::to_string(wallet_.copper()) + " of at most " + std::to_string(items::kMoneyCap) +
                  " (a sale past the cap is refused whole)");
  }
  const items::Inventory backup = inventory_;
  items::Outcome all{true, {}, {}};
  for (const svc::InventoryItem &it : plan.sold) {
    items::Outcome o = inventory_.consume(it.container, it.slot, it.instance_guid, it.quantity);
    if (!o.ok) {
      inventory_ = backup;
      return refuse(o.refusal);
    }
    all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()),
                      std::make_move_iterator(o.events.end()));
  }
  const int64_t before = wallet_.copper();
  wallet_.give(plan.total);
  inventory_dirty_ = inventory_changed_ = true;
  ++items_applied_;
  ++vendor_sales_;
  std::vector<bootstrap::ItemEvent> events = item_events_for(all);
  add_item_toasts(events, /*removals=*/true);
  const int64_t total = plan.total;
  for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
    const std::vector<bootstrap::ItemEvent> part(
        events.begin() + static_cast<std::ptrdiff_t>(at),
        events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
    if (at + bootstrap::kMaxEventsPerChain >= events.size()) {
      queue(
          "sale answer",
          [part, total](bootstrap::PlayerBootstrap &pb) { return pb.sale_answer(items::kMoneyTier, total, part); },
          conn, /*must_build=*/true);
    } else {
      queue(
          "ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap &pb) { return pb.item_events(part); }, conn,
          /*must_build=*/true);
    }
  }
  pb_.set_currency(items::kCopperCurrency, wallet_.copper());
  queue("Currencies", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
  spdlog::log(level_for(vendor_logged_),
              "[session] {}: sold {} stack(s) to the shop for {} copper ({} -> {}): {} storage event(s)", name(),
              plan.sold.size(), total, before, wallet_.copper(), all.events.size());
}

void PlayerSession::repair_at_town_shop(Connection &conn, const repl::VendorTransactionCart &cart,
                                        Clock::time_point now) {
  const auto refuse = [&](const std::string &why) {
    ++vendor_refused_;
    refuse_item_request(conn, "ServerHandleVendorTransaction", why, now);
  };
  const vendor::TownShops *shops = world_ ? world_->town_shops() : nullptr;
  const content::ContentDb *db = world_ ? world_->content() : nullptr;
  if (!shops || !db) return refuse("worldd has no town shops");
  const RepairPlan plan = plan_town_shop_repair(inventory_, *db, *shops, cart, {vendor_def_, progress_.level});
  if (!plan.ok) return refuse(plan.refusal);
  if (!wallet_.can_pay(plan.total)) {
    return refuse("the purse holds " + std::to_string(wallet_.copper()) + " copper and the repair costs " +
                  std::to_string(plan.total) + " (not enough money)");
  }
  struct Update {
    bootstrap::ItemStorage storage;
    int32_t slot;
    float durability;
  };
  std::vector<Update> updates;
  for (const MendedItem &m : plan.mended) {
    if (!inventory_.set_durability(m.item.container, m.item.slot, m.item.instance_guid, m.full))
      return refuse("the item's durability could not be set");
    updates.push_back({bootstrap::item_storage_of(m.item.container), m.item.slot, m.full});
  }
  const int64_t before = wallet_.copper();
  wallet_.pay(plan.total);
  inventory_dirty_ = inventory_changed_ = true;
  ++items_applied_;
  ++vendor_repairs_;
  const int64_t delta = -plan.total;
  for (size_t i = 0; i < updates.size(); ++i) {
    const Update u = updates[i];
    if (i + 1 < updates.size()) {
      queue(
          "ClientUpdateItemDurability",
          [u](bootstrap::PlayerBootstrap &pb) { return pb.item_durability(u.storage, u.slot, u.durability); }, conn,
          /*must_build=*/true);
    } else {
      queue(
          "repair answer",
          [u, delta](bootstrap::PlayerBootstrap &pb) {
            return pb.repair_answer(u.storage, u.slot, u.durability, items::kMoneyTier, delta);
          },
          conn, /*must_build=*/true);
    }
  }
  pb_.set_currency(items::kCopperCurrency, wallet_.copper());
  queue("Currencies", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
  spdlog::log(level_for(vendor_logged_), "[session] {}: repaired {} item(s) at the shop for {} copper ({} -> {})",
              name(), plan.mended.size(), plan.total, before, wallet_.copper());
}

PlayerSession::GmMoneyResult PlayerSession::gm_give_money(Connection &conn, int64_t copper, Clock::time_point now,
                                                          const std::string &by) {
  (void)now;
  GmMoneyResult r;
  r.previous = wallet_.copper();
  r.balance = r.previous;
  if (copper < 1) {
    r.refusal = "the amount must be at least 1 copper";
    return r;
  }
  if (wallet_.room() == 0) {
    r.refusal = name() + "'s purse is full (" + std::to_string(items::kMoneyCap) + " copper, 1,000 gold)";
    return r;
  }
  if (!room_for(3)) {
    r.refusal = name() + "'s connection is busy: try again in a moment";
    return r;
  }
  r.added = wallet_.give(copper);
  r.balance = wallet_.copper();
  r.ok = true;
  inventory_dirty_ = inventory_changed_ = true;
  pb_.set_currency(items::kCopperCurrency, wallet_.copper());
  const int64_t shown = r.added;
  queue(
      "ClientShowCurrencyCallout",
      [shown](bootstrap::PlayerBootstrap &pb) { return pb.currency_callout(items::kMoneyTier, shown); }, conn,
      /*must_build=*/true);
  queue("Currencies", [](bootstrap::PlayerBootstrap &pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
  spdlog::info("[gm] {}: {} copper added by {} ({} -> {}{})", name(), r.added, by, r.previous, r.balance,
               r.added < copper ? "; the purse is full, the rest is not added" : "");
  return r;
}

}  // namespace worldd
}  // namespace aoc
