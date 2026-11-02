// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_session.h"

#include "items/acquired.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "world/city_nodes.h"

#include <spdlog/spdlog.h>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

std::vector<warehouse::Spec> allowed_warehouse_specs(const warehouse::Book& book, int64_t node_guid, int32_t node_level, int access_config) {
    warehouse::Facts facts;
    facts.node = node_guid;
    facts.level = node_level;
    std::vector<warehouse::Spec> specs = book.owned(node_guid, facts);
    specs.erase(std::remove_if(specs.begin(), specs.end(),
                               [&](const warehouse::Spec& s) {
                                   const int bit = s.key.area == warehouse::Area::Items       ? repl::kAccessGlobalItems
                                                   : s.key.area == warehouse::Area::Materials ? repl::kAccessNodeMaterials
                                                                                              : repl::kAccessNodeCrates;
                                   return (access_config & bit) == 0;
                               }),
                specs.end());
    return specs;
}

template <typename WhVec>
std::vector<bootstrap::WarehouseComponent> collect_wh_components(const WhVec& wh_components, bool hide_tabs) {
    std::vector<bootstrap::WarehouseComponent> out;
    out.reserve(wh_components.size());
    for (const auto& w : wh_components) {
        bootstrap::WarehouseComponent c = w.comp;
        if (hide_tabs && c.kind != bootstrap::WarehouseComponent::Kind::ItemChunk) {
            c.listed = false;
            c.read_only = true;
        }
        out.push_back(c);
    }
    return out;
}

template <typename WhVec>
bool upsert_wh_components(WhVec& wh_components, const std::vector<warehouse::Spec>& specs, const PlayerWorld& world) {
    for (auto& w : wh_components) {
        if (w.comp.kind != bootstrap::WarehouseComponent::Kind::ItemChunk) {
            w.comp.listed = false;
            w.comp.read_only = true;
        }
    }
    bool has_cargo = false;
    for (const warehouse::Spec& s : specs) {
        typename WhVec::value_type* have = nullptr;
        for (auto& w : wh_components) {
            if (w.key == s.key) have = &w;
        }
        if (!have) {
            typename WhVec::value_type made;
            made.key = s.key;
            made.comp.kind = s.key.area == warehouse::Area::Items       ? bootstrap::WarehouseComponent::Kind::ItemChunk
                             : s.key.area == warehouse::Area::Materials ? bootstrap::WarehouseComponent::Kind::MaterialTab
                                                                        : bootstrap::WarehouseComponent::Kind::CargoTab;
            made.comp.guid = world.mint_guid();
            made.comp.node = s.key.node;
            made.comp.record = s.key.record;
            wh_components.push_back(std::move(made));
            have = &wh_components.back();
        }
        have->spec = s;
        have->comp.index = s.index;
        have->comp.member = s.member;
        have->comp.listed = true;
        have->comp.read_only = false;
        has_cargo = has_cargo || s.key.area == warehouse::Area::Cargo;
    }
    return has_cargo;
}

template <typename WhVec>
const typename WhVec::value_type* find_wh_component(const WhVec& wh_components, const FIntrepidNetworkGUID& object) {
    for (const auto& w : wh_components) {
        if (w.comp.guid == object) return &w;
    }
    return nullptr;
}

std::optional<svc::ItemContainer> resolve_bag_container(const bootstrap::PlayerSetup& setup, const FIntrepidNetworkGUID& object) {
    if (object == setup.guids.backpack) return svc::ItemContainer::Backpack;
    for (int i = 0; i < items::kResourceContainers; ++i) {
        const size_t idx = static_cast<size_t>(i);
        if (setup.resource_bags[idx].present && object == setup.guids.resource_storage[idx]) return items::resource_container(i);
    }
    return std::nullopt;
}

template <typename Side, typename WhComp>
bool fill_warehouse_side(Side& side, const WhComp* wh, const bootstrap::PlayerSetup& setup, const FIntrepidNetworkGUID& object) {
    if (wh) {
        side.in_warehouse = true;
        side.key = wh->key;
        return true;
    }
    if (const auto bag = resolve_bag_container(setup, object)) {
        side.bag = *bag;
        return true;
    }
    return false;
}

template <typename WhVec>
std::vector<bootstrap::ItemEvent> build_warehouse_item_events(const warehouse::Result& res, const WhVec& wh_components) {
    std::vector<bootstrap::ItemEvent> events;
    events.reserve(res.steps.size());
    for (const warehouse::Step& s : res.steps) {
        bootstrap::ItemEvent ev;
        ev.type = s.type;
        ev.slot = s.slot;
        ev.item = items::to_instance(s.item);
        ev.instigated = true;
        if (s.in_warehouse) {
            for (const auto& w : wh_components) {
                if (w.key == s.key) ev.container = w.comp.guid;
            }
        } else {
            ev.storage = bootstrap::item_storage_of(s.bag);
        }
        events.push_back(std::move(ev));
    }
    return events;
}

}  // namespace

void PlayerSession::list_item_chunk(Connection& conn) {
    const warehouse::Book* book = world_ ? world_->warehouses() : nullptr;
    const std::optional<warehouse::Spec> spec = book ? book->base_chunk() : std::nullopt;
    if (!spec) return;
    for (const WhComponent& w : wh_components_) {
        if (w.key == spec->key) return;
    }
    WhComponent made;
    made.key = spec->key;
    made.spec = *spec;
    made.comp.kind = bootstrap::WarehouseComponent::Kind::ItemChunk;
    made.comp.guid = world_->mint_guid();
    made.comp.record = spec->key.record;
    made.comp.listed = true;
    wh_components_.push_back(std::move(made));
    const std::vector<bootstrap::WarehouseComponent> comps = collect_wh_components(wh_components_, false);
    queue("the item chunk", [comps](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_warehouse_components(comps)) return std::nullopt;
        return pb.warehouse_update();
    }, conn);
}

bool PlayerSession::open_warehouse(Connection& conn, const world::WorldActor& npc, int access_config, Clock::time_point /*now*/) {
    const auto refuse = [&](const std::string& why) {
        ++wh_refused_;
        spdlog::log(level_for(wh_logged_), "[session] {}: {}'s storage does not open: {}", name(), npc.npc.name, why);
        return false;
    };
    const warehouse::Book* book = world_ ? world_->warehouses() : nullptr;
    if (!book) return refuse("worldd has no warehouse data (rebuild content.db with this checkout's contentc)");
    const world::CityNodes* nodes = world_->city_nodes();
    if (!nodes) return refuse("worldd has no city nodes");
    const world::NodeSite site = nodes->node_at(npc.x, npc.y);
    const world::CityNodeInfo* town = nodes->by_rank(site.node_rank);
    if (!town) return refuse("the clerk stands in no town");
    if (!book->node(town->guid)) return refuse("the town " + town->display_name + " has no warehouse in the data");
    const GoodPosition at = checker_.good();
    const double dx = npc.x - at.x, dy = npc.y - at.y, dz = npc.z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
        return refuse("the clerk is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away (a window opens within 12 m)");
    }
    if (!room_for(4)) return refuse("the connection is busy");
    const std::vector<warehouse::Spec> specs = allowed_warehouse_specs(*book, town->guid, nodes->level_of(town->guid), access_config);
    if (specs.empty()) return refuse("the town offers no container for access config " + std::to_string(access_config));

    // Send tabs read-only and unlisted first so client maps their components before the window RPC arrives.
    const bool has_cargo = upsert_wh_components(wh_components_, specs, *world_);
    const std::vector<bootstrap::WarehouseComponent> first = collect_wh_components(wh_components_, true);
    const std::vector<int64_t> material_nodes{town->guid};
    const std::vector<int64_t> crate_nodes = has_cargo ? std::vector<int64_t>{town->guid} : std::vector<int64_t>{};
    queue("the warehouse components", [first, material_nodes, crate_nodes](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_warehouse_components(first) || !pb.set_warehouse_nodes(material_nodes, crate_nodes)) return std::nullopt;
        return pb.warehouse_update();
    }, conn);

    wh_open_ = true;
    ++wh_gen_;
    wh_clerk_ = npc.id;
    wh_access_ = static_cast<uint8_t>(access_config);
    wh_window_.node = town->guid;
    wh_window_.specs = specs;
    ++wh_opens_;
    size_t held = 0;
    for (const warehouse::Spec& s : specs) held += wh_storage_.items_of(s.key).size();
    spdlog::log(level_for(wh_logged_), "[session] {}: opens the storage of {} ({} container(s), {} stored item(s))", name(), town->display_name, specs.size(), held);
    return true;
}

void PlayerSession::finish_open_warehouse(Connection& conn, Clock::time_point now, bool clear) {
    if (!wh_open_) return;
    const uint32_t gen = wh_gen_;
    const int64_t node = wh_window_.node;
    const uint8_t config = wh_access_;
    queue_later("ClientOpenWarehouseWindow", [this, gen, node, config](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (gen != wh_gen_ || !wh_open_) return std::nullopt;
        return pb.open_warehouse_window(node, config);
    }, conn, now);
    if (clear) send_dialogue_later(conn, now);
    const std::vector<bootstrap::WarehouseComponent> comps = collect_wh_components(wh_components_, false);
    queue_later("the warehouse lists", [this, gen, comps](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (gen != wh_gen_ || !wh_open_) return std::nullopt;
        if (!pb.set_warehouse_components(comps)) return std::nullopt;
        return pb.warehouse_update();
    }, conn, now);
}

void PlayerSession::send_dialogue_later(Connection& conn, Clock::time_point now) {
    const bootstrap::PlayerBootstrap::DialogueView v = dialogue_view();
    queue_later("dialogue", [v](bootstrap::PlayerBootstrap& pb) { return pb.dialogue_update(v); }, conn, now);
}

void PlayerSession::close_warehouse(Connection& conn, const char* why) {
    if (!wh_open_) return;
    wh_open_ = false;
    ++wh_gen_;
    wh_clerk_ = 0;
    wh_window_ = warehouse::OpenWindow{};
    for (WhComponent& w : wh_components_) {
        if (w.comp.kind != bootstrap::WarehouseComponent::Kind::ItemChunk) {
            w.comp.listed = false;
            w.comp.read_only = true;
        }
    }
    const std::vector<bootstrap::WarehouseComponent> comps = collect_wh_components(wh_components_, false);
    queue("the warehouse components", [comps](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_warehouse_components(comps)) return std::nullopt;
        return pb.warehouse_update();
    }, conn);
    spdlog::log(level_for(wh_logged_), "[session] {}: the storage window closes: {}", name(), why);
}

void PlayerSession::on_warehouse_close(Connection& conn) { close_warehouse(conn, "the client closed the window"); }

void PlayerSession::run_warehouse(Connection& conn, Clock::time_point /*now*/) {
    if (!wh_open_) return;
    if (dead_) return close_warehouse(conn, "the player is dead");
    const world::WorldActor* clerk = registry_ ? registry_->find(wh_clerk_) : nullptr;
    if (!clerk) return close_warehouse(conn, "the clerk is gone");
    const GoodPosition at = checker_.good();
    const double dx = clerk->x - at.x, dy = clerk->y - at.y, dz = clerk->z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) close_warehouse(conn, "the player walked away from the clerk");
}

void PlayerSession::on_warehouse_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string rpc_name = rpc.call.rpc && rpc.call.rpc->name ? rpc.call.rpc->name : "?";
    const auto request = repl::parse_warehouse_request(rpc.call);
    if (!request) {
        if (wh_unreadable_logged_++ == 0) {
            spdlog::warn("[session] {}: a {} does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc_name, rpc.call.param_bits);
        }
        return;
    }
    if (rpc_name == "ServerVisitWarehouse") {
        spdlog::log(level_for(wh_logged_), "[session] {}: a ServerVisitWarehouse for node {} opens nothing (only the storage clerk's line opens a warehouse)", name(), request->id.record);
        return;
    }
    if (!wh_open_ || request->id.area != repl::kWarehouseAreaNode || request->id.record != wh_window_.node) {
        ++wh_refused_;
        spdlog::log(level_for(wh_refused_logged_), "[session] {}: a {} for node {} is refused: {}", name(), rpc_name, request->id.record,
                    wh_open_ ? "it names another town than the window's" : "no storage window is open");
        return;
    }
    if (rpc_name == "ServerFetchWarehouse") return;
    if (!within_budget(wh_budget_, "ServerRequestWarehouseSummary", now) || !room_for(2)) return;
    const warehouse::Book* book = world_ ? world_->warehouses() : nullptr;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!book || !nodes) return;
    warehouse::Facts facts;
    facts.node = wh_window_.node;
    facts.level = nodes->level_of(wh_window_.node);
    const auto summary = book->summary(wh_window_.node, facts);
    if (!summary) return;
    const repl::WarehouseSummary s = *summary;
    queue("ClientReceiveWarehouseSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.warehouse_summary(s); }, conn, /*must_build=*/true);
}

bool PlayerSession::warehouse_payload(Connection& conn, const FIntrepidNetworkGUID& container, Clock::time_point now) {
    const WhComponent* w = find_wh_component(wh_components_, container);
    if (!w) return false;
    const warehouse::Spec* spec = wh_open_ ? wh_window_.find(w->key) : nullptr;
    if (!spec && w->key.area == warehouse::Area::Items) spec = &w->spec;
    if (!spec) {
        spdlog::debug("[session] {}: the data of a warehouse container is asked while its window is closed: not sent", name());
        return true;
    }
    if (!within_budget(storage_budget_, "ServerRequestStorageData", now) || !room_for(1)) return true;
    repl::StoragePayload data = warehouse::payload_of(*spec, wh_storage_);
    data.target = container;
    queue("ClientReceiveStoragePayload", [data](bootstrap::PlayerBootstrap& pb) { return pb.container_payload(data); }, conn);
    return true;
}

void PlayerSession::refuse_warehouse_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now) {
    ++wh_refused_;
    spdlog::log(level_for(wh_refused_logged_), "[session] {}: {} in the warehouse refused, nothing changed: {}", name(), what, why);
    if (!resync_budget_.take(now) || !room_for(3 + wh_window_.specs.size())) return;
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
    }
    for (const warehouse::Spec& s : wh_window_.specs) {
        for (const WhComponent& w : wh_components_) {
            if (!(w.key == s.key)) continue;
            repl::StoragePayload data = warehouse::payload_of(s, wh_storage_);
            data.target = w.comp.guid;
            queue("ClientReceiveStoragePayload", [data](bootstrap::PlayerBootstrap& pb) { return pb.container_payload(data); }, conn);
        }
    }
}

bool PlayerSession::warehouse_event(Connection& conn, const repl::ItemStorageEvent& event, Clock::time_point now) {
    const WhComponent* from_wh = find_wh_component(wh_components_, event.move_source.object);
    const WhComponent* to_wh = event.targets.empty() ? nullptr : find_wh_component(wh_components_, event.targets[0].object);
    if (!from_wh && !to_wh) return false;
    const char* what = "ServerRequestStorageEvent";
    if (!wh_open_) {
        refuse_warehouse_request(conn, what, "no storage window is open", now);
        return true;
    }
    if (event.targets.size() != 1 || event.targets[0].slots.size() != 1 || event.move_source.slots.size() != 1) {
        refuse_warehouse_request(conn, what, "a move names one source slot and one target slot", now);
        return true;
    }
    const world::WorldActor* clerk = registry_ ? registry_->find(wh_clerk_) : nullptr;
    const GoodPosition at = checker_.good();
    if (!clerk || (clerk->x - at.x) * (clerk->x - at.x) + (clerk->y - at.y) * (clerk->y - at.y) + (clerk->z - at.z) * (clerk->z - at.z) >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
        refuse_warehouse_request(conn, what, "the clerk is not within reach", now);
        return true;
    }
    warehouse::Request req;
    req.type = static_cast<repl::StorageEventType>(event.type);
    if (!fill_warehouse_side(req.from, from_wh, pb_.setup(), event.move_source.object)) {
        refuse_warehouse_request(conn, what, "a source that is none of this character's bags or warehouse containers", now);
        return true;
    }
    if (!fill_warehouse_side(req.to, to_wh, pb_.setup(), event.targets[0].object)) {
        refuse_warehouse_request(conn, what, "a target that is none of this character's bags or warehouse containers", now);
        return true;
    }
    const repl::StorageEventSlot& src = event.move_source.slots[0];
    const repl::StorageEventSlot& dst = event.targets[0].slots[0];
    req.from.slot = src.slot;
    req.to.slot = dst.slot;
    req.count = src.count;
    req.guid = items::guid_hex(src.item_guid);
    req.to_guid = items::guid_hex(dst.item_guid);
    req.to_count = dst.count;
    if (!(src.item_guid == event.item.guid)) {
        refuse_warehouse_request(conn, what, "the item of the event is not the instance of its source slot", now);
        return true;
    }
    if (!within_budget(item_budget_, what, now) || !room_for(4)) return true;
    const warehouse::Result res = warehouse::transfer(inventory_, wh_storage_, item_catalog(), wh_window_, req);
    if (!res.ok) {
        refuse_warehouse_request(conn, what, res.refusal, now);
        return true;
    }
    std::vector<bootstrap::ItemEvent> events = build_warehouse_item_events(res, wh_components_);
    if (res.steps.size() == 2) {
        for (bootstrap::ItemEvent& ev : events) {
            if (ev.container) continue;
            std::vector<bootstrap::ItemEvent> one{ev};
            add_item_toasts(one, /*removals=*/true);
            ev.callout = one[0].callout;
        }
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++wh_moves_;
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    spdlog::log(level_for(wh_logged_), "[session] {}: a stack moves {} the warehouse ({} event(s))", name(), from_wh && to_wh ? "inside" : from_wh ? "out of" : "into", events.size());
    return true;
}

}}  // namespace aoc::worldd
