// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_session.h"

#include "content/content_db.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/storage_rpc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {

constexpr double kDialogueRangeCm = 4000.0;

struct HandInPick {
    int64_t            item = 0;
    svc::ItemContainer container = svc::ItemContainer::Backpack;
    std::string        guid;
    int32_t            quantity = 0;
};

struct HandInRow {
    int64_t                 objective = 0;
    size_t                  index = 0;
    std::vector<HandInPick> picks;
    int64_t                 offered = 0, take = 0;
};

struct HandInHandles {
    uint16_t objective = 0;
    uint16_t items = 0;
    uint16_t context[4] = {};
    uint16_t record = 0;
    uint16_t stacks = 0;
    uint16_t guid[4] = {};
    uint16_t quantity = 0;
    uint16_t location = 0;

    bool valid() const {
        return objective && items && record && stacks && quantity && location &&
               guid[0] && guid[1] && guid[2] && guid[3];
    }

    bool has_context() const {
        return context[0] && context[1] && context[2] && context[3];
    }
};

struct HandInParseCtx {
    const HandInHandles&           h;
    const dialogue::Talk::Sub&     sub;
    const content::QuestRow&       def;
    const quest::QuestInstance&    instance;
    const items::Inventory&        inventory;
    std::map<std::string, int64_t> claimed;
    std::set<std::string>&         quest_told;
    std::string_view               player_name;
};

const repl::Value* quest_rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

int64_t quest_int_value(const repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

uint16_t handin_handle(const schema::RpcSchema& rpc, std::string_view path) {
    for (const schema::RepCommand& c : rpc.element_commands) {
        if (path == c.path) return c.handle;
    }
    return 0;
}

HandInHandles resolve_handin_handles(const schema::RpcSchema& schema_rpc) {
    HandInHandles h;
    h.objective = handin_handle(schema_rpc, "DeliveryItems.ObjectiveId");
    h.items = handin_handle(schema_rpc, "DeliveryItems.Items");
    h.context[0] = handin_handle(schema_rpc, "DeliveryItems.ContextGuid.A");
    h.context[1] = handin_handle(schema_rpc, "DeliveryItems.ContextGuid.B");
    h.context[2] = handin_handle(schema_rpc, "DeliveryItems.ContextGuid.C");
    h.context[3] = handin_handle(schema_rpc, "DeliveryItems.ContextGuid.D");
    h.record = handin_handle(schema_rpc, "Items.ItemRecordId");
    h.stacks = handin_handle(schema_rpc, "Items.ItemQuantityAndLocations");
    h.guid[0] = handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.A");
    h.guid[1] = handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.B");
    h.guid[2] = handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.C");
    h.guid[3] = handin_handle(schema_rpc, "ItemQuantityAndLocations.FItemInstanceGuid.D");
    h.quantity = handin_handle(schema_rpc, "ItemQuantityAndLocations.Quantity");
    h.location = handin_handle(schema_rpc, "ItemQuantityAndLocations.InventoryLocated");
    return h;
}

const repl::Value* handin_value(const repl::ElementState& e, uint16_t handle) {
    if (handle == 0) return nullptr;
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

int64_t handin_int(const repl::ElementState& e, uint16_t handle) {
    return quest_int_value(handin_value(e, handle));
}

std::optional<svc::ItemContainer> handin_container(int64_t location) {
    if (location == static_cast<int64_t>(repl::ItemLocation::Backpack)) return svc::ItemContainer::Backpack;
    if (location == static_cast<int64_t>(repl::ItemLocation::Quest)) return svc::ItemContainer::Quest;
    return std::nullopt;
}

std::string read_handin_stack(const repl::ElementState& s, int64_t record, HandInParseCtx& ctx, HandInRow& row) {
    HandInPick pick;
    pick.item = record;
    pick.quantity = static_cast<int32_t>(handin_int(s, ctx.h.quantity));
    const int64_t loc_val = handin_int(s, ctx.h.location);
    const auto container = handin_container(loc_val);
    if (!container) return "a stack lies where a hand-in does not look (location " + std::to_string(loc_val) + ")";
    pick.container = *container;
    pick.guid = items::guid_hex(repl::GuidValue{
        static_cast<uint32_t>(handin_int(s, ctx.h.guid[0])),
        static_cast<uint32_t>(handin_int(s, ctx.h.guid[1])),
        static_cast<uint32_t>(handin_int(s, ctx.h.guid[2])),
        static_cast<uint32_t>(handin_int(s, ctx.h.guid[3])),
    });
    if (pick.quantity <= 0) return "a stack is named with no quantity";
    const svc::InventoryItem* held = ctx.inventory.find_guid(pick.guid);
    if (held && held->container != pick.container) held = nullptr;
    if (!held) return "a stack the character does not carry (guid " + pick.guid + ")";
    if (held->item_record_id != record) return "a stack holds item " + std::to_string(held->item_record_id) + ", the call says " + std::to_string(record);
    int64_t& used = ctx.claimed[pick.guid];
    used += pick.quantity;
    if (used > held->quantity) return "a stack is asked for " + std::to_string(used) + ", it holds " + std::to_string(held->quantity);
    row.offered += pick.quantity;
    row.picks.push_back(std::move(pick));
    return {};
}

std::string read_handin_item(const repl::ElementState& it, const content::QuestObjectiveRow& r, HandInParseCtx& ctx, HandInRow& row) {
    const int64_t record = handin_int(it, ctx.h.record);
    if (std::find(r.items.begin(), r.items.end(), record) == r.items.end()) {
        return "item " + std::to_string(record) + " is not one objective " + r.name + " takes";
    }
    const repl::Value* stacks = handin_value(it, ctx.h.stacks);
    const repl::ArrayValue* stack_list = stacks ? stacks->get_if<repl::ArrayValue>() : nullptr;
    if (!stack_list || stack_list->elements.size() > 64) {
        return "item " + std::to_string(record) + " names no stack or too many";
    }
    for (const repl::ElementState& s : stack_list->elements) {
        std::string err = read_handin_stack(s, record, ctx, row);
        if (!err.empty()) return err;
    }
    return {};
}

size_t find_objective_index(const content::QuestRow& def, int64_t objective_guid) {
    size_t index = def.objectives.size();
    for (size_t i = 0; i < def.objectives.size(); ++i) {
        if (def.objectives[i].guid == objective_guid) index = i;
    }
    return index;
}

std::string read_handin_objective(const repl::ElementState& d, HandInParseCtx& ctx, std::vector<HandInRow>& rows) {
    HandInRow row;
    row.objective = handin_int(d, ctx.h.objective);
    if (std::find(ctx.sub.objectives.begin(), ctx.sub.objectives.end(), row.objective) == ctx.sub.objectives.end()) {
        return "objective " + std::to_string(row.objective) + " is not one the panel lists";
    }
    for (const HandInRow& other : rows) {
        if (other.objective == row.objective) return "objective " + std::to_string(row.objective) + " is named twice";
    }
    const size_t index = find_objective_index(ctx.def, row.objective);
    if (index >= ctx.def.objectives.size() || index >= ctx.instance.objectives.size()) {
        return "objective " + std::to_string(row.objective) + " is not in the quest";
    }
    row.index = index;
    const content::QuestObjectiveRow& r = ctx.def.objectives[index];
    const quest::ObjectiveState& state = ctx.instance.objectives[index];
    if (r.type != "TurnIn" || r.crate || !state.active || state.completed || state.failed) {
        return "objective " + r.name + " takes no items now";
    }
    if (r.items.empty()) {
        if (ctx.quest_told.insert("hand-in filter " + r.name).second) {
            spdlog::info("[session] {}: the hand-in objective {} filters its items by tags or rarities, which this build does not serve (told once)", ctx.player_name, r.name);
        }
        return "objective " + r.name + " filters by tags: not served";
    }
    const repl::Value* items = handin_value(d, ctx.h.items);
    const repl::ArrayValue* item_list = items ? items->get_if<repl::ArrayValue>() : nullptr;
    if (!item_list || item_list->elements.size() > 32) {
        return "objective " + r.name + " carries no items or too many rows";
    }
    for (const repl::ElementState& it : item_list->elements) {
        std::string err = read_handin_item(it, r, ctx, row);
        if (!err.empty()) return err;
    }
    if (row.offered > 0) rows.push_back(std::move(row));
    return {};
}

void add_leaving_item(std::vector<std::pair<int64_t, int64_t>>& leaving, int64_t item, int64_t part) {
    for (auto& [record, quantity] : leaving) {
        if (record == item) {
            quantity += part;
            return;
        }
    }
    leaving.emplace_back(item, part);
}

int64_t compute_handin_leaving(const content::QuestRow& def, const quest::QuestInstance& instance, std::vector<HandInRow>& rows, std::vector<std::pair<int64_t, int64_t>>& leaving) {
    int64_t total_take = 0;
    for (HandInRow& row : rows) {
        const content::QuestObjectiveRow& r = def.objectives[row.index];
        const int64_t room = std::max<int64_t>(0, r.count - instance.objectives[row.index].count);
        row.take = std::min(row.offered, room);
        int64_t left = row.take;
        for (const HandInPick& p : row.picks) {
            if (left <= 0) break;
            const int64_t part = std::min<int64_t>(p.quantity, left);
            left -= part;
            add_leaving_item(leaving, p.item, part);
        }
        total_take += row.take;
    }
    return total_take;
}

void check_handin_context_guid(const repl::ElementState& first, const HandInHandles& h, const quest::QuestInstance& instance, std::set<std::string>& quest_told, std::string_view player_name) {
    if (!h.has_context()) return;
    quest::Guid128 context;
    context.a = static_cast<uint32_t>(handin_int(first, h.context[0]));
    context.b = static_cast<uint32_t>(handin_int(first, h.context[1]));
    context.c = static_cast<uint32_t>(handin_int(first, h.context[2]));
    context.d = static_cast<uint32_t>(handin_int(first, h.context[3]));
    if (!context.zero() && !(context == instance.guid) && quest_told.insert("hand-in context").second) {
        spdlog::info("[session] {}: a hand-in names the quest instance {} and the log holds {}: the quest record decides (told once)", player_name, context.hex(), instance.guid.hex());
    }
}

}  // namespace

void PlayerSession::on_item_delivery(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto refuse = [&](const std::string& why) {
        ++hand_ins_refused_;
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: a hand-in is refused: {}", name(), why);
    };
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !book || !qb) return refuse("quests are off");
    if (dead_) return refuse("the player is dead");
    if (!talk_.open() || !talk_.sub() || talk_.sub()->type != 3) return refuse("no hand-in panel is open");
    if (!rpc.call.decoded || !rpc.call.rpc) return refuse("its parameters do not decode");
    if (!within_budget(dialogue_budget_, "ServerRequestItemDelivery", now) || !room_for(6)) return;
    const dialogue::Talk::Sub sub = *talk_.sub();
    const int64_t quest = quest_int_value(quest_rpc_value(rpc, "QuestRecordId"));
    if (quest != sub.quest) return refuse("it names quest " + std::to_string(quest) + ", the panel is for quest " + std::to_string(sub.quest));
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return end_dialogue(conn, "the player is too far from the NPC");
    const content::QuestRow* def = qb->quest(sub.quest);
    const quest::QuestInstance* instance = quest_log_.find(sub.quest);
    if (!def || !instance || instance->status != quest::Status::Active) return refuse("the quest is not active");
    const repl::Value* deliveries = quest_rpc_value(rpc, "DeliveryItems");
    const repl::ArrayValue* list = deliveries ? deliveries->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty() || list->elements.size() > 16) return refuse(!list ? "it carries no deliveries" : list->elements.empty() ? "it delivers nothing" : "it names too many objectives");
    const HandInHandles handles = resolve_handin_handles(*rpc.call.rpc);
    if (!handles.valid()) return refuse("the call's layout is not the one this build reads");
    HandInParseCtx ctx{handles, sub, *def, *instance, inventory_, {}, quest_told_, name()};
    std::vector<HandInRow> rows;
    for (const repl::ElementState& d : list->elements) {
        std::string err = read_handin_objective(d, ctx, rows);
        if (!err.empty()) return refuse(err);
    }
    if (rows.empty()) return refuse("it delivers no item");
    std::vector<std::pair<int64_t, int64_t>> leaving;
    const int64_t total_take = compute_handin_leaving(*def, *instance, rows, leaving);
    if (total_take <= 0) return refuse("the objectives take no more items");
    check_handin_context_guid(list->elements.front(), handles, *instance, quest_told_, name());
    ++hand_ins_;
    hand_in_items_ += static_cast<size_t>(total_take);
    spdlog::log(level_for(quests_logged_, 100), "[session] {}: hands {} item(s) in to {} for quest {} ({} objective(s))", name(), total_take, npc->npc.name, def->name, rows.size());
    take_quest_items(conn, leaving);
    int64_t counted = 0;
    quest_engine(conn, now, "items were handed in", [&](quest::Engine& eng) {
        for (const HandInRow& row : rows) counted += eng.turn_in(sub.quest, row.objective, row.take);
    });
    if (counted != total_take) spdlog::warn("[session] {}: the hand-in for quest {} took {} item(s) out of the bags and the objectives counted {}", name(), def->name, total_take, counted);
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", name(), what);
    });
    const dialogue::Result r = talk_.finish_sub(*book, ev, /*taken=*/true);
    run_talk_actions(conn, r, *npc, now);
    if (r.outcome == dialogue::Outcome::Changed) {
        ++dialogue_steps_;
        if (!talk_.open()) ++dialogue_closes_;
        send_dialogue(conn, "items were handed in", /*exhaust=*/true);
    }
}

}}  // namespace aoc::worldd
