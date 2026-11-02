// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "area_session_test_harness.h"
#include "quest/quest_book.h"
#include "quest/quest_log.h"
#include "quest/quest_service.h"
#include "quest/quest_wire.h"
#include "world/zoi_surface.h"

#include <mutex>
#include <span>
#include <thread>

namespace qs = aoc::worldd::quest;

inline constexpr int64_t kSoshaInstance = 6064634562958262298ll;
inline constexpr int64_t kSoshaRoot = 6064634563434774564ll;
inline constexpr int64_t kSoshaOfferTop = 6064634563458629669ll;
inline constexpr int64_t kAcceptNode = 6064634569414541543ll;
inline constexpr int64_t kCorruptedMemories = 6064634563262021659ll;
inline constexpr int64_t kCorruptedFirstObjective = 6064634563268640796ll;
inline constexpr int64_t kOfTheRootways = 6064634760119582720ll;
inline constexpr int64_t kWelcomeToVerra = 6064634641390699452ll;
inline constexpr int64_t kRainOfRocks = 6064634349883364413ll;
inline constexpr int64_t kRockSack = 6064632196779935710ll;
inline constexpr int64_t kMissingQuest = 6064633689987481604ll;
inline constexpr int64_t kStartingAreaZoi = 6064629127135166465ll;
inline constexpr double kStartingAreaX = -819456;
inline constexpr double kStartingAreaY = 624384;
inline constexpr double kJoevaX = -1084437;
inline constexpr double kJoevaY = 503729;

class FakeQuestApi : public svc::QuestApi {
public:
    svc::QuestLogResult load_log(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu_);
        svc::QuestLogResult r;
        r.status = svc::CallStatus::Ok;
        r.log = logs_[id];
        return r;
    }
    svc::CallStatus save_log(const std::string& id, const svc::SavedQuestLog& log) override {
        std::lock_guard<std::mutex> lock(mu_);
        logs_[id] = log;
        ++saves_;
        return svc::CallStatus::Ok;
    }
    svc::CallStatus take_commands(const std::string& id, std::vector<svc::QuestCommand>& out) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto& box = inbox_[id];
        out.insert(out.end(), box.begin(), box.end());
        box.clear();
        return svc::CallStatus::Ok;
    }
    void queue(const std::string& id, std::string kind, std::string quest = {}, std::string name = {}, int64_t value = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        svc::QuestCommand c;
        c.id = ++next_;
        c.kind = std::move(kind);
        c.quest = std::move(quest);
        c.name = std::move(name);
        c.value = value;
        inbox_[id].push_back(std::move(c));
    }
    svc::SavedQuestLog log(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu_);
        return logs_[id];
    }
    size_t saves() {
        std::lock_guard<std::mutex> lock(mu_);
        return saves_;
    }

private:
    std::mutex mu_;
    std::map<std::string, svc::SavedQuestLog> logs_;
    std::map<std::string, std::vector<svc::QuestCommand>> inbox_;
    size_t saves_ = 0;
    int64_t next_ = 0;
};

struct QuestTestData {
    content::QuestData quests;
    std::optional<dl::Book> book;
};

inline svc::SavedQuestLog log_after_start() {
    svc::SavedQuestLog log;
    svc::SavedVariable v;
    v.name = "WelcomeToVerra";
    v.value = 1;
    log.variables.push_back(v);
    return log;
}

inline std::optional<QuestTestData> quest_test_data(std::string& why) {
    const std::string path = dialogue_db_path();
    if (path.empty()) {
        why = "no content.db (dist/content/content.db or AOC_CONTENT_DB)";
        return std::nullopt;
    }
    auto db = content::ContentDb::open(path, why);
    if (!db) return std::nullopt;
    auto dialogue = db->dialogue_data(why);
    if (!dialogue) return std::nullopt;
    auto quests = db->quest_data(why);
    if (!quests) return std::nullopt;
    QuestTestData out;
    out.book.emplace(std::move(*dialogue));
    out.book->set_accepts(quests->accepts);
    out.book->set_turn_ins(quests->turn_ins);
    out.quests = std::move(*quests);
    return out;
}

struct QuestHarness : Harness {
    FakeQuestApi& api;
    std::unique_ptr<qs::QuestWorker> worker;
    uint64_t talker = 0;
    bool ended = false;

    QuestHarness(FakeQuestApi& api_, const QuestTestData& data, svc::SavedQuestLog log, svc::Character r = cyb_record(), bool gm = false)
        : Harness(std::move(r)), api(api_) {
        record.quest_log = std::move(log);
        qs::QuestWorker::Config wc;
        wc.poll_interval = milliseconds(20);
        worker = std::make_unique<qs::QuestWorker>(api, wc);
        fx.world->set_dialogue(*data.book);
        fx.world->set_quests(qs::QuestBook(data.quests));
        fx.world->set_quest_worker(worker.get());
        fx.world->set_gm_commands(gm);
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) {
            std::printf("  [FAIL] no vendor template\n");
            std::exit(2);
        }
        talker = fx.add_talker(*vendor_t, kCx + 800, kCy + 300, "Sosha Terim", kSoshaRoot);
        if (world::WorldActor* a = fx.world->registry().find(talker)) a->npc.instance_guid = kSoshaInstance;
        start();
    }
    ~QuestHarness() override { end(); }

    void end() {
        if (ended) return;
        ended = true;
        s->finish("the test ends");
        worker->flush(seconds(3));
    }

    void saved() {
        run(milliseconds(100));
        worker->flush(seconds(3));
    }

    void commands() {
        std::this_thread::sleep_for(milliseconds(150));
        saved();
    }

    void click() {
        client_request(interact(fx.world->registry().find(talker)->npc.interact_component), pawn_ch);
    }
};

using Cmds = std::span<const schema::RepCommand>;

struct QuestBlock {
    std::vector<repl::FastArrayField> fields;
    size_t bits = 0;
};

inline std::vector<QuestBlock> read_quest_blocks_at(
    const fake::Delivered& d,
    const emit::FIntrepidNetworkGUID& guid,
    const schema::ClassSchema& cls,
    size_t start) {
    std::vector<QuestBlock> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks || blocks->empty()) return out;
    if (d.open && !(*blocks)[0].header.is_actor) return out;
    for (const repl::BlockView& b : *blocks) {
        if (b.header.is_actor || !(b.header.object == guid)) continue;
        size_t pos = b.payload_pos;
        auto f = repl::read_fast_array_fields(cls, d.content.data(), b.payload_pos + b.payload_bits, pos);
        if (f) out.push_back({std::move(*f), b.payload_bits});
    }
    return out;
}

inline std::vector<QuestBlock> quest_blocks_of(const fake::Delivered& d, const emit::FIntrepidNetworkGUID& guid, const schema::ClassSchema& cls) {
    const size_t end = d.content.bit_pos();
    if (d.close || end == 0) return {};
    for (size_t start = 0; start < (d.open ? 4000u : 1u) && start < end; ++start) {
        auto out = read_quest_blocks_at(d, guid, cls, start);
        if (!out.empty()) return out;
    }
    return {};
}

inline std::vector<QuestBlock> quest_blocks_since(const Harness& h, size_t from, size_t* others = nullptr) {
    std::vector<QuestBlock> out;
    const schema::ClassSchema* cls = qs::component_schema();
    if (!cls) return out;
    const emit::FIntrepidNetworkGUID guid = h.s->bootstrap().setup().guids.quest;
    for (size_t i = from; i < h.got.size(); ++i) {
        auto blocks = quest_blocks_of(h.got[i], guid, *cls);
        for (QuestBlock& b : blocks) {
            if (h.got[i].channel == h.pc_ch) out.push_back(std::move(b));
            else if (others) ++*others;
        }
    }
    return out;
}

inline const schema::RepCommand* cmd_at(Cmds cmds, std::string_view path) {
    for (const schema::RepCommand& c : cmds) {
        if (path == c.path) return &c;
    }
    return nullptr;
}

inline const repl::Value* value_at(Cmds cmds, const repl::ElementState& es, std::string_view path) {
    const schema::RepCommand* c = cmd_at(cmds, path);
    if (!c) return nullptr;
    const auto it = es.values.find(c->handle);
    return it == es.values.end() ? nullptr : &it->second;
}

inline int64_t int_at(Cmds cmds, const repl::ElementState& es, std::string_view path) {
    const repl::Value* v = value_at(cmds, es, path);
    if (!v) return INT64_MIN;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return INT64_MIN;
}

inline std::string guid_at(Cmds cmds, const repl::ElementState& es, const std::string& prefix) {
    qs::Guid128 g;
    uint32_t* w[4] = {&g.a, &g.b, &g.c, &g.d};
    const char* const letters[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; i < 4; ++i) {
        const int64_t v = int_at(cmds, es, prefix + letters[i]);
        if (v == INT64_MIN) return "missing";
        *w[i] = static_cast<uint32_t>(v);
    }
    return g.hex();
}

inline Cmds nested_at(const repl::FastArrayLayout& layout, std::string_view path) {
    const schema::RepCommand* c = cmd_at(layout.element, path);
    if (!c || c->type != schema::PropType::DynamicArray || c->elem_end <= c->elem_begin || c->elem_end > layout.table.size()) return {};
    return layout.table.subspan(c->elem_begin, c->elem_end - c->elem_begin);
}

inline const repl::FastArrayField* field_named(const std::vector<repl::FastArrayField>& fields, const schema::ClassSchema& cls, const char* property) {
    const auto layout = repl::fast_array_layout(cls, property);
    if (!layout) return nullptr;
    for (const repl::FastArrayField& f : fields) {
        if (f.field == layout->field) return &f;
    }
    return nullptr;
}

struct SeenQuest {
    int64_t record = 0;
    std::string instance, parent;
    int64_t locality = -1;
    int32_t cache_id = -1, vars_id = -1;
    std::vector<std::pair<std::string, int64_t>> objectives;
    std::vector<std::pair<uint64_t, int32_t>> variables;
    std::string variables_key;
};

inline SeenQuest parse_seen_quest(const repl::FastArrayLayout& lc, Cmds objective_cmds, const repl::FastArrayDelta::Changed& ch) {
    const repl::ElementState& item = ch.values;
    SeenQuest q;
    q.cache_id = ch.id;
    q.record = int_at(lc.element, item, "Quests.RecordId");
    q.instance = guid_at(lc.element, item, "Quests.InstanceGuid");
    q.parent = guid_at(lc.element, item, "Quests.ParentInstanceGuid");
    q.locality = int_at(lc.element, item, "Quests.Locality");
    const repl::Value* ov = value_at(lc.element, item, "Quests.Objectives");
    const auto* list = ov ? ov->get_if<repl::ArrayValue>() : nullptr;
    if (list) {
        for (const repl::ElementState& e : list->elements) {
            q.objectives.push_back({guid_at(objective_cmds, e, "Objectives.InstanceGuid"), int_at(objective_cmds, e, "Objectives.RecordId")});
        }
    }
    return q;
}

inline void attach_quest_vars(const repl::FastArrayField& vars, const repl::FastArrayLayout& lv, std::vector<SeenQuest>& out) {
    for (const auto& ch : vars.delta.changed) {
        const std::string key = guid_at(lv.element, ch.values, "Items.Key");
        for (SeenQuest& q : out) {
            if (q.instance != key) continue;
            q.vars_id = ch.id;
            q.variables_key = key;
            const repl::Value* hv = value_at(lv.element, ch.values, "Items.Value");
            if (hv && hv->get_if<repl::HashedVariable>()) q.variables = hv->get_if<repl::HashedVariable>()->variables;
        }
    }
}

inline std::vector<SeenQuest> quests_in(const QuestBlock& b) {
    std::vector<SeenQuest> out;
    const schema::ClassSchema* cls = qs::component_schema();
    const repl::FastArrayField* cache = cls ? field_named(b.fields, *cls, "ReplicatedQuestCache") : nullptr;
    const repl::FastArrayField* vars = cls ? field_named(b.fields, *cls, "RepQuestVariables") : nullptr;
    const auto lc = cls ? repl::fast_array_layout(*cls, "ReplicatedQuestCache") : std::nullopt;
    const auto lv = cls ? repl::fast_array_layout(*cls, "RepQuestVariables") : std::nullopt;
    if (!cache || !lc) return out;
    const Cmds objective_cmds = nested_at(*lc, "Quests.Objectives");
    for (const auto& ch : cache->delta.changed) out.push_back(parse_seen_quest(*lc, objective_cmds, ch));
    if (vars && lv) attach_quest_vars(*vars, *lv, out);
    return out;
}

inline std::vector<uint64_t> expected_variable_keys(const content::QuestRow& def) {
    std::vector<uint64_t> keys;
    for (const char* s : {"Completed", "Active", "Failed", "Paused"}) keys.push_back(qs::city_hash64(qs::quest_variable_name(def.guid, s)));
    for (const content::QuestObjectiveRow& o : def.objectives) {
        for (const char* s : {"Completed", "Active", "Failed", "Succeeded", "Hidden", "Count", "Awake"}) {
            keys.push_back(qs::city_hash64(qs::objective_variable_name(def.guid, o.guid, s)));
        }
    }
    return keys;
}

inline BunchWriter abandon_rpc(const std::vector<qs::Guid128>& guids, uint64_t state = 7) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerAbandonQuests");
    repl::RpcParams params;
    repl::ArrayValue list;
    for (const qs::Guid128& g : guids) {
        repl::ElementState e;
        e.values[1] = repl::Value::i64(static_cast<int32_t>(g.a));
        e.values[2] = repl::Value::i64(static_cast<int32_t>(g.b));
        e.values[3] = repl::Value::i64(static_cast<int32_t>(g.c));
        e.values[4] = repl::Value::i64(static_cast<int32_t>(g.d));
        list.elements.push_back(std::move(e));
    }
    if (!rpc || !repl::set_rpc_param(*rpc, params, "QuestGuids", repl::Value(std::move(list))) || !repl::set_rpc_param(*rpc, params, "QuestState", repl::Value::u64(state))) {
        return BunchWriter();
    }
    return actor_rpc(pc, "ServerAbandonQuests", params);
}

inline BunchWriter blacklist_rpc(const qs::Guid128& g, bool on) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSetQuestBlacklisted");
    repl::RpcParams params;
    if (!rpc) return BunchWriter();
    const char* const names[4] = {"QuestInstanceGuid.A", "QuestInstanceGuid.B", "QuestInstanceGuid.C", "QuestInstanceGuid.D"};
    const uint32_t words[4] = {g.a, g.b, g.c, g.d};
    for (int i = 0; i < 4; ++i) {
        if (!repl::set_rpc_param(*rpc, params, names[i], repl::Value::i64(static_cast<int32_t>(words[i])))) return BunchWriter();
    }
    if (!repl::set_rpc_param(*rpc, params, "bSetBlacklisted", repl::Value::boolean(on))) return BunchWriter();
    return actor_rpc(pc, "ServerSetQuestBlacklisted", params);
}

inline BunchWriter sub_dialogue_response(repl::SubDialogueResponse answer) {
    repl::DialogueResponse r;
    r.response_type = static_cast<uint8_t>(repl::DialogueResponseType::SubDialogue);
    r.quest_response = static_cast<uint8_t>(answer);
    r.node_type_id = repl::kDialogueNodeTypeId;
    return dialogue_choice(r);
}

inline BunchWriter debug_start_rpc(int64_t quest, bool bypass) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerDebugStartQuest");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "Guid", repl::Value::i64(quest)) || !repl::set_rpc_param(*rpc, params, "bBypassActivationRules", repl::Value::boolean(bypass))) {
        return BunchWriter();
    }
    return actor_rpc(pc, "ServerDebugStartQuest", params);
}

inline bool walk_to_offer(QuestHarness& h, ClientDialogue& client, std::vector<PcBlock>& last) {
    for (int step = 0; step < 12 && !h.s->talk().sub(); ++step) {
        const auto& st = h.s->talk().state();
        if (st.lines.empty()) return false;
        const size_t from = h.got.size();
        h.client_request(dialogue_choice(choice_of(st.lines.front().node, st.lines.front().index)), h.pc_ch);
        last = pc_blocks_since(h, from);
        for (const auto& b : last) client.apply(b.state);
    }
    return h.s->talk().sub().has_value();
}

struct SeenPanel {
    size_t count = 0;
    int64_t type = -1, quest = 0, preview_quest = 0, node = 0, pending_next = -1;
    std::vector<int64_t> initial;
};

inline SeenPanel panel_of(const PcBlock& b) {
    SeenPanel out;
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* array = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    if (!array) return out;
    const repl::Value* v = b.state.get(array->handle);
    const auto* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!list) return out;
    out.count = list->elements.size();
    if (list->elements.size() != 1) return out;
    const Cmds elems = pc.elements(*array);
    const repl::ElementState& e = list->elements[0];
    out.type = int_at(elems, e, "QueuedSubDialogues.SubDialogueType");
    out.quest = int_at(elems, e, "QueuedSubDialogues.QuestId.Guid");
    out.preview_quest = int_at(elems, e, "QueuedSubDialogues.QuestPreview.QuestId.Guid");
    out.node = int_at(elems, e, "QueuedSubDialogues.SubDialogueNodeId.Guid");
    out.pending_next = int_at(elems, e, "QueuedSubDialogues.PendingNextNode.Guid");
    const repl::Value* iv = value_at(elems, e, "QueuedSubDialogues.QuestPreview.InitialObjectives");
    const auto* items = iv ? iv->get_if<repl::ArrayValue>() : nullptr;
    if (!items) return out;
    for (const repl::ElementState& it : items->elements) {
        const auto g = it.values.find(1);
        if (g != it.values.end() && g->second.get_if<int64_t>()) out.initial.push_back(*g->second.get_if<int64_t>());
    }
    return out;
}

inline bool has_handle(const std::vector<PcBlock>& blocks, uint16_t handle) {
    for (const PcBlock& b : blocks) {
        if (std::find(b.handles.begin(), b.handles.end(), handle) != b.handles.end()) return true;
    }
    return false;
}
