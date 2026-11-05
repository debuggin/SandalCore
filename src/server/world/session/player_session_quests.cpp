// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "session/player_session.h"
#include "world/city_nodes.h"
#include "world/zoi_surface.h"

namespace aoc::worldd {

namespace {

constexpr double kDialogueRangeCm = 4000.0;

class TalkFacts final : public dialogue::Facts {
   public:
    std::vector<svc::InventoryItem> items;
    int level = 1;
    double player_x = 0, player_y = 0, npc_x = 0, npc_y = 0;
    bool has_npc = false;
    int64_t citizen_node = 0;
    const world::CityNodes* nodes = nullptr;
    const quest::QuestLog* quests = nullptr;

    std::optional<int64_t> quest_count(int64_t quest, const std::string& query) const override {
        return quests ? quests->quest_count(quest, query) : std::nullopt;
    }
    std::optional<int64_t> objective_count(int64_t quest, int64_t objective, const std::string& query) const override {
        return quests ? quests->objective_count(quest, objective, query) : std::nullopt;
    }
    int64_t player_variable(const std::string& name) const override {
        return quests ? quests->variable(name) : 0;
    }
    int64_t quest_variable(int64_t quest, const std::string& name) const override {
        return quests ? quests->quest_variable(quest, name) : 0;
    }
    int64_t item_count(const std::vector<int64_t>& records, const std::vector<std::string>& /*tags*/) const override {
        if (records.empty()) return 0;
        int64_t n = 0;
        for (const svc::InventoryItem& it : items) {
            if (std::find(records.begin(), records.end(), it.item_record_id) != records.end()) n += it.quantity;
        }
        return n;
    }
    std::optional<dialogue::ExprValue> call(const std::string& name, const std::vector<dialogue::ExprValue>& args) const override {
        using dialogue::ExprValue;
        constexpr int64_t kPlayer = 1, kNpc = 2;
        if (name == "GetPlayer") return ExprValue::of(kPlayer);
        if (name == "GetInteractionTarget") return ExprValue::of(has_npc ? kNpc : 0);
        if (name == "GetCurrentNode") return ExprValue::of(node_at(player_x, player_y));
        if (name == "GetCharacterLevel" || name == "GetPlayerLevel") return ExprValue::of(level);
        return call_node_builtin(name, args);
    }

   private:
    std::optional<dialogue::ExprValue> call_node_builtin(const std::string& name, const std::vector<dialogue::ExprValue>& args) const {
        using dialogue::ExprValue;
        constexpr int64_t kNpc = 2;
        if (name == "Node_GetActorCityNode") {
            if (args.size() != 1) return std::nullopt;
            return ExprValue::of(args[0].i == kNpc && has_npc ? node_at(npc_x, npc_y) : node_at(player_x, player_y));
        }
        if (name == "Node_IsPlayerCitizen") return ExprValue::of(static_cast<int64_t>(citizen_node != 0));
        if (name == "Node_IsPlayerCitizenOfNode") {
            if (args.size() != 2) return std::nullopt;
            return ExprValue::of(static_cast<int64_t>(citizen_node != 0 && citizen_node == args[1].i));
        }
        static constexpr std::string_view kNodeActivity[] = {"Node_NumOfServiceBuildingsInState", "Node_NumOfServiceBuilding",
                                                             "Node_IsNodeMayorElectionActive",    "Node_IsNodeMayorPrevoteActive",
                                                             "Node_IsExpansionInGivenState",      "Node_IsBuyOrderActive",
                                                             "Node_IsPlayerMayorCandidate",       "Node_IsPlayerMayorCurrentNode",
                                                             "Node_HasPlayerCitizenshipCooldown", "Node_CanPlayerBecomeCitizen"};
        if (std::find(std::begin(kNodeActivity), std::end(kNodeActivity), name) != std::end(kNodeActivity)) {
            return ExprValue::of(0);
        }
        return std::nullopt;
    }

    int64_t node_at(double x, double y) const {
        if (!nodes) return 0;
        const world::NodeSite site = nodes->node_at(x, y);
        const world::CityNodeInfo* n = nodes->by_rank(site.node_rank);
        return n ? n->guid : 0;
    }
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
int64_t find_dialogue_offer_node(const world::WorldActor& npc, const quest::QuestBook* qb, const quest::QuestLog& quest_log, bool quests_on,
                                 dialogue::Evaluator& ev) {
    int64_t offered = 0;
    if (!qb || !quests_on) return 0;
    for (const content::QuestOfferRow& offer : qb->offers(npc.npc.instance_guid, npc.npc.asset_guid)) {
        if (offer.node != 0 && quest_log.can_start(qb->quest(offer.quest), ev, /*bypass_rules=*/false) == quest::Create::Ok) {
            offered = offer.node;
            break;
        }
    }
    if (const int64_t entry = quest::dialogue_entry(quest_log, *qb, npc.npc.instance_guid)) offered = entry;
    return offered;
}
std::vector<int64_t> initial_sub_objectives(const quest::QuestBook* qb, int64_t quest_id) {
    std::vector<int64_t> out;
    const content::QuestRow* def = qb ? qb->quest(quest_id) : nullptr;
    if (!def) return out;
    for (const content::QuestObjectiveRow& o : def->objectives) {
        bool starts = o.awake;
        for (const content::QuestStartAction& a : def->start_actions)
            starts = starts || (a.action == "ActivateObjective" && a.objective == o.guid);
        if (starts) out.push_back(o.guid);
    }
    return out;
}
quest::Guid128 parse_quest_guid_element(const repl::ElementState& e) {
    quest::Guid128 g;
    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
    for (uint16_t i = 0; i < 4; ++i) {
        const auto it = e.values.find(static_cast<uint16_t>(i + 1));
        const auto* v = it == e.values.end() ? nullptr : it->second.get_if<int64_t>();
        *words[i] = v ? static_cast<uint32_t>(*v) : 0;
    }
    return g;
}
bool item_bound_to_quest_delete(const std::vector<content::QuestItemLink>& links, int64_t quest) {
    for (const content::QuestItemLink& l : links) {
        if (l.quest == quest && (l.kind == "delete_all" || l.kind == "delete_one")) return true;
    }
    return false;
}
void log_unanswered_condition(std::set<std::string>& told, const std::string& player_name, const std::string& what) {
    if (told.insert(what).second)
        spdlog::info("[session] {}: a condition of a conversation could not be answered and does not hold: {} (told once)", player_name,
                     what);
}
void log_skipped_dialogue_actions(std::set<std::string>& told, std::string_view player_name, const std::vector<std::string>& skipped) {
    for (const std::string& s : skipped) {
        if (told.insert("skipped " + s).second)
            spdlog::info("[session] {}: a conversation action is not done yet: {} (told once)", player_name, s);
    }
}
int64_t ftue_element_id(const protocol::repl::ElementState& e) {
    const auto it = e.values.find(1);
    const int64_t* id = it != e.values.end() ? it->second.get_if<int64_t>() : nullptr;
    return id ? *id : 0;
}
const protocol::repl::ArrayValue* parse_ftue_remove_list(const protocol::repl::ClientRpc& rpc, std::string_view player_name,
                                                         bool& unreadable_logged, size_t& refused_logged) {
    if (!rpc.call.decoded) {
        if (!unreadable_logged) {
            unreadable_logged = true;
            spdlog::warn(
                "[session] {}: Server_RemoveLocaleFTUEs arrived and its parameters do not decode: nothing is recorded (further ones are "
                "not logged)",
                player_name);
        }
        return nullptr;
    }
    const protocol::repl::Value* v = protocol::repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "FTUEsToRemove");
    const protocol::repl::ArrayValue* list = v ? v->get_if<protocol::repl::ArrayValue>() : nullptr;
    if (!list || list->elements.empty()) return nullptr;
    if (list->elements.size() > bootstrap::kMaxCompletedFtues) {
        if (++refused_logged <= 3)
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs names {} tutorials, more than a character can have finished ({}): refused",
                         player_name, list->elements.size(), bootstrap::kMaxCompletedFtues);
        return nullptr;
    }
    return list;
}
bool is_sub_dialogue_close(const repl::DialogueResponse& r) {
    return r.response_type == static_cast<uint8_t>(repl::DialogueResponseType::SubDialogue) &&
           r.quest_response == static_cast<uint8_t>(repl::SubDialogueResponse::Close);
}
int classify_dialogue_rpc(const repl::DialogueResponse& r, std::set<std::string>& told, std::string_view player_name) {
    using repl::DialogueResponseType;
    using repl::SubDialogueResponse;
    if (r.response_type == static_cast<uint8_t>(DialogueResponseType::SubDialogue) &&
        (r.quest_response == static_cast<uint8_t>(SubDialogueResponse::Accept) ||
         r.quest_response == static_cast<uint8_t>(SubDialogueResponse::Decline)))
        return 1;
    if (r.response_type == static_cast<uint8_t>(DialogueResponseType::ActionWindow)) return 0;
    if (r.response_type == static_cast<uint8_t>(DialogueResponseType::Choice)) return 2;
    if (told.insert("response " + std::to_string(r.response_type) + "/" + std::to_string(r.quest_response)).second)
        spdlog::info("[session] {}: a talking-window response of type {} (quest response {}) is not answered yet (told once)", player_name,
                     r.response_type, r.quest_response);
    return 0;
}
const char* dialogue_choice_refusal(bool dead, const PlayerWorld* world, bool talk_open, bool has_room) {
    if (!dead && world && world->dialogue() && talk_open && has_room) return nullptr;
    return dead ? "the player is dead" : !talk_open ? "no conversation is open" : "the world has no conversations";
}
const world::WorldActor* find_dialogue_npc(const world::ActorRegistry* registry, uint64_t source, const GoodPosition& at,
                                           const char*& gone_why) {
    const world::WorldActor* npc = registry ? registry->find(source) : nullptr;
    gone_why = !npc ? "the NPC is gone"
                    : (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm ? "the player is too far from the NPC" : nullptr);
    return gone_why ? nullptr : npc;
}
std::string record_talk_step(const dialogue::Talk& talk, uint64_t& steps, uint64_t& closes) {
    ++steps;
    if (!talk.open()) ++closes;
    return talk.open() ? "on to node " + std::to_string(talk.state().shown_node) : std::string("the conversation ends");
}
const char* quest_offer_refusal_reason(bool dead, bool open, bool has_sub) {
    if (dead) return "the player is dead";
    return !open ? "no conversation is open" : !has_sub ? "no offer is open" : "the world has no quests";
}
spdlog::level::level_enum quest_log_level(size_t& count) {
    return ++count <= 100 ? spdlog::level::info : spdlog::level::debug;
}
struct OfferSubStep {
    bool taken = false, back = false, try_accept = false;
};
OfferSubStep evaluate_quest_offer_sub(const std::string& player_name, const dialogue::Talk::Sub& sub, uint8_t response,
                                      size_t& quests_logged) {
    const bool accept = response == static_cast<uint8_t>(repl::SubDialogueResponse::Accept);
    if (sub.type == 4) {
        spdlog::log(quest_log_level(quests_logged), "[session] {}: the player {} the confirmation of node {}", player_name,
                    accept ? "confirmed" : "declined", sub.node);
        return {accept, response == static_cast<uint8_t>(repl::SubDialogueResponse::Decline), false};
    }
    if (!accept)
        spdlog::log(quest_log_level(quests_logged), "[session] {}: the player declined the offer of quest {}", player_name, sub.quest);
    return {false, false, accept};
}
struct OfferAcceptCtx {
    std::string player_name;
    int64_t quest_guid = 0;
    bool try_accept = false, quests_on = false;
    const quest::QuestBook* qb = nullptr;
};
const content::QuestRow* check_quest_offer_accept(const OfferAcceptCtx& ctx, quest::QuestLog& quest_log, dialogue::Evaluator& ev,
                                                  size_t& quests_logged, uint64_t& quests_refused) {
    if (!ctx.try_accept) return nullptr;
    const content::QuestRow* def = ctx.qb->quest(ctx.quest_guid);
    if (!ctx.quests_on) {
        ++quests_refused;
        spdlog::warn(
            "[session] {}: the offer of quest {} is not taken: quests are off for this character (the quest service did not answer at the "
            "login)",
            ctx.player_name, ctx.quest_guid);
        return nullptr;
    }
    const quest::Create c = quest_log.can_start(def, ev, /*bypass_rules=*/false);
    if (c != quest::Create::Ok) {
        ++quests_refused;
        spdlog::log(quest_log_level(quests_logged), "[session] {}: the offer of quest {} is refused: {}", ctx.player_name, ctx.quest_guid,
                    quest::create_name(c));
        return nullptr;
    }
    return def;
}
bool apply_quest_log_command(quest::QuestLog& log, const svc::QuestCommand& cmd, const content::QuestRow* def, std::string_view player_name,
                             bool& quests_touched) {
    if (cmd.kind == "clear") {
        log.clear(def ? def->guid : 0);
        quests_touched = true;
        spdlog::info("[session] {}: GM clear {}", player_name, def ? def->name : "(the finished quests and the variables)");
        return true;
    }
    if (cmd.kind == "set-var") {
        log.set_variable(cmd.name, cmd.value);
        quests_touched = true;
        spdlog::info("[session] {}: GM set-var {} = {}", player_name, cmd.name, cmd.value);
        return true;
    }
    return false;
}
quest::Guid128 parse_debug_quest_guid(const repl::ClientRpc& rpc) {
    quest::Guid128 g;
    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
    const char* const letters[4] = {"Guid.A", "Guid.B", "Guid.C", "Guid.D"};
    for (int i = 0; i < 4; ++i) *words[i] = static_cast<uint32_t>(quest_int_value(quest_rpc_value(rpc, letters[i])));
    return g;
}
bool quest_rpc_bool(const repl::ClientRpc& rpc, std::string_view name) {
    const repl::Value* b = quest_rpc_value(rpc, name);
    return b && b->get_if<bool>() && *b->get_if<bool>();
}
const content::QuestRow* check_debug_start_quest(const PlayerWorld& world, const quest::QuestLog& log, const dialogue::Facts& facts,
                                                 const repl::ClientRpc& rpc, std::string_view player_name) {
    const int64_t id = quest_int_value(quest_rpc_value(rpc, "Guid"));
    const content::QuestRow* def = world.quests() ? world.quests()->quest(id) : nullptr;
    const dialogue::Book* book = world.dialogue();
    if (!def || !book) {
        spdlog::warn("[gm] {}: ServerDebugStartQuest {}: no such quest", player_name, id);
        return nullptr;
    }
    const bool bypass = quest_rpc_bool(rpc, "bBypassActivationRules");
    dialogue::Evaluator ev(*book, facts);
    const quest::Create c = log.can_start(def, ev, bypass);
    if (c != quest::Create::Ok) {
        spdlog::warn("[gm] {}: ServerDebugStartQuest {} ({}): refused, {}", player_name, def->name,
                     bypass ? "rules bypassed" : "rules asked", quest::create_name(c));
        return nullptr;
    }
    return def;
}

}  // namespace

void PlayerSession::on_remove_ftues(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now) {
    ++ftue_calls_;
    const protocol::repl::ArrayValue* list = parse_ftue_remove_list(rpc, name(), ftue_unreadable_logged_, ftue_refused_logged_);
    if (!list) return;
    std::vector<int64_t> fresh;
    for (const protocol::repl::ElementState& e : list->elements) {
        const int64_t id = ftue_element_id(e);
        if (id <= 0) {
            ++ftue_ignored_;
            if (++ftue_ignored_logged_ <= kLoggedPerKind)
                spdlog::info("[session] {}: Server_RemoveLocaleFTUEs names tutorial {}: ignored", name(), id);
            continue;
        }
        if (std::find(completed_ftues_.begin(), completed_ftues_.end(), id) != completed_ftues_.end() ||
            std::find(fresh.begin(), fresh.end(), id) != fresh.end()) {
            ++ftue_repeats_;
            continue;
        }
        fresh.push_back(id);
    }
    if (fresh.empty()) {
        spdlog::debug("[session] {}: Server_RemoveLocaleFTUEs: nothing new ({} calls, {} repeats, {} ignored so far)", name(), ftue_calls_,
                      ftue_repeats_, ftue_ignored_);
        return;
    }
    if (completed_ftues_.size() + fresh.size() > bootstrap::kMaxCompletedFtues) {
        if (++ftue_refused_logged_ <= kLoggedPerKind)
            spdlog::warn("[session] {}: Server_RemoveLocaleFTUEs would take the finished tutorials over {}: refused, nothing changes",
                         name(), bootstrap::kMaxCompletedFtues);
        return;
    }
    for (const int64_t id : fresh) {
        completed_ftues_.push_back(id);
        spdlog::log(level_for(ftue_logged_, 20), "[session] {}: finished tutorial {} ({} finished in all)", name(), id,
                    completed_ftues_.size());
    }
    progress_changed_ = true;
    hand_over_progress("a tutorial was finished");
    if (pb_.set_completed_ftues(completed_ftues_)) {
        queue_later("FTUE Component", [](bootstrap::PlayerBootstrap& pb) { return pb.ftue_update(); }, conn, now, false);
    } else {
        spdlog::warn("[session] {}: the tutorial component does not take the {} finished tutorials: the client is not told", name(),
                     completed_ftues_.size());
    }
}

size_t PlayerSession::gm_reset_tutorials(Connection& conn, const std::string& by) {
    const size_t n = completed_ftues_.size();
    if (n == 0) return 0;
    completed_ftues_.clear();
    progress_changed_ = true;
    hand_over_progress("the tutorials were put back by a game master");
    if (pb_.set_completed_ftues(completed_ftues_))
        queue("FTUE Component", [](bootstrap::PlayerBootstrap& pb) { return pb.ftue_update(); }, conn, false);
    spdlog::info("[session] {}: had {} finished tutorial(s) put back by {}", name(), n, by);
    return n;
}

std::unique_ptr<dialogue::Facts> PlayerSession::dialogue_facts(const world::WorldActor* npc) const {
    auto f = std::make_unique<TalkFacts>();
    f->items = inventory_.items();
    f->level = progress_.level;
    const GoodPosition at = checker_.good();
    f->player_x = at.x;
    f->player_y = at.y;
    if (npc) {
        f->has_npc = true;
        f->npc_x = npc->x;
        f->npc_y = npc->y;
    }
    f->citizen_node = pb_.setup().citizen_node;
    f->nodes = world_ ? world_->city_nodes() : nullptr;
    if (quests_on_) f->quests = &quest_log_;
    return f;
}

bool PlayerSession::open_dialogue(Connection& conn, const world::WorldActor& npc, Clock::time_point now) {
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    if (!book) return false;
    if (talk_.open()) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a click on {} while the talking window is open: ignored", name(),
                    npc.npc.name);
        return true;
    }
    if (!room_for(1)) return true;
    const auto facts = dialogue_facts(&npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) { log_unanswered_condition(dialogue_told_, name(), what); });
    const int64_t offered = find_dialogue_offer_node(npc, world_->quests(), quest_log_, quests_on_, ev);
    if (npc.npc.dialogue_root == 0 && offered == 0) return false;
    const dialogue::Result r = talk_.begin(*book, ev, npc.id, npc.npc.dialogue_root, offered);
    log_skipped_dialogue_actions(dialogue_told_, name(), r.skipped);
    if (r.outcome != dialogue::Outcome::Changed) {
        if (r.silent) {
            spdlog::log(level_for(dialogue_logged_), "[session] {}: talks to {}: {}", name(), npc.npc.name, r.why);
            run_talk_actions(conn, r, npc, now);
            return true;
        }
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: {} has a conversation that cannot be shown ({}): it answers as before",
                    name(), npc.npc.name, r.why);
        return false;
    }
    ++dialogue_opens_;
    const dialogue::Talk::State& s = talk_.state();
    spdlog::log(level_for(dialogue_logged_), "[session] {}: talks to {} (node {}, {} line(s) shown)", name(), npc.npc.name, s.shown_node,
                s.lines.size());
    run_talk_actions(conn, r, npc, now);
    send_dialogue(conn, "greeting");
    return true;
}

bootstrap::PlayerBootstrap::DialogueView PlayerSession::dialogue_view(bool exhaust) const {
    const dialogue::Talk::State s = talk_.state();
    bootstrap::PlayerBootstrap::DialogueView v;
    v.exhaust = exhaust;
    if (s.sub) {
        bootstrap::PlayerBootstrap::DialogueView::SubDialogue sd;
        sd.type = s.sub->type;
        sd.pending_next = s.sub->pending_next;
        sd.previous_node = s.sub->previous_node;
        sd.node = s.sub->node;
        sd.quest = s.sub->quest;
        sd.objectives = s.sub->objectives;
        if (world_ && s.sub->type == 2) {
            sd.initial_objectives = initial_sub_objectives(world_->quests(), s.sub->quest);
        }
        v.sub = std::move(sd);
    }
    v.previous_node = s.previous_node;
    v.previous_choice_node = s.previous_choice_node;
    v.previous_choice_index = s.previous_choice_index;
    v.current_node = s.current_node;
    v.shown_node = s.shown_node;
    v.greeting = s.greeting;
    for (const dialogue::Talk::Line& l : s.lines) v.lines.push_back({l.node, l.index});
    if (s.source != 0 && registry_) {
        if (const world::WorldActor* a = registry_->find(s.source)) v.source = a->actor.guid;
    }
    return v;
}

void PlayerSession::send_dialogue(Connection& conn, const char* /*why*/, bool exhaust) {
    const bootstrap::PlayerBootstrap::DialogueView v = dialogue_view(exhaust);
    queue("dialogue", [v](bootstrap::PlayerBootstrap& pb) { return pb.dialogue_update(v); }, conn);
}

void PlayerSession::end_dialogue(Connection& conn, const char* why) {
    const world::WorldActor* npc = talk_.open() && registry_ ? registry_->find(talk_.state().source) : nullptr;
    const dialogue::Result r = talk_.close(world_ ? world_->dialogue() : nullptr);
    if (r.outcome != dialogue::Outcome::Changed) return;
    ++dialogue_closes_;
    spdlog::log(level_for(dialogue_logged_), "[session] {}: the conversation ends: {}", name(), why);
    if (npc && !r.actions.empty()) run_talk_actions(conn, r, *npc, quest_last_now_);
    send_dialogue(conn, why, /*exhaust=*/r.offer_answered);
}

bool PlayerSession::npc_offers_quests(const world::WorldActor& npc) const {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    return qb && quests_on_ && !qb->offers(npc.npc.instance_guid, npc.npc.asset_guid).empty();
}

void PlayerSession::on_dialogue_choice(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto response = repl::parse_dialogue_response(rpc.call);
    if (!response) {
        if (dialogue_told_.insert("unreadable").second) {
            spdlog::warn(
                "[session] {}: a ServerSelectDialogueChoice does not read ({} parameter bits): ignored (further ones "
                "are not logged)",
                name(), rpc.call.param_bits);
        }
        return;
    }
    if (is_sub_dialogue_close(*response)) return end_dialogue(conn, "the client closed the window");
    if (!within_budget(dialogue_budget_, "ServerSelectDialogueChoice", now)) return;
    const int kind = classify_dialogue_rpc(*response, dialogue_told_, name());
    if (kind == 1) return on_quest_offer(conn, response->quest_response, now);
    if (kind != 2) return;
    if (const char* why = dialogue_choice_refusal(dead_, world_, talk_.open(), room_for(1))) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a talking-window choice is refused: {}", name(), why);
        return;
    }
    const char* gone_why = nullptr;
    const world::WorldActor* npc = find_dialogue_npc(registry_, talk_.state().source, checker_.good(), gone_why);
    if (!npc) return end_dialogue(conn, gone_why);
    const dialogue::Book* book = world_->dialogue();
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) { log_unanswered_condition(dialogue_told_, name(), what); });
    const dialogue::Result r = talk_.choose(*book, ev, *response);
    log_skipped_dialogue_actions(dialogue_told_, name(), r.skipped);
    const bool warehouse_opened = r.open_warehouse != 0 && open_warehouse(conn, *npc, r.open_warehouse, now);
    if (r.open_vendor != 0) open_talk_shop(conn, *npc, r.open_vendor, now);
    if (r.outcome == dialogue::Outcome::Changed) {
        run_talk_actions(conn, r, *npc, now);
        spdlog::log(level_for(dialogue_logged_), "[session] {}: chose line {} of node {}: {}", name(), response->choice_index,
                    response->node, record_talk_step(talk_, dialogue_steps_, dialogue_closes_));
        if (!warehouse_opened) send_dialogue(conn, "a line was chosen");
    } else if (r.outcome == dialogue::Outcome::Refused) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: a talking-window choice is refused: {}", name(), r.why);
    }
    if (warehouse_opened) finish_open_warehouse(conn, now, /*clear=*/r.outcome == dialogue::Outcome::Changed);
}

void PlayerSession::run_dialogue(Connection& conn, Clock::time_point /*now*/) {
    if (!talk_.open()) return;
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return end_dialogue(conn, "the NPC is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) end_dialogue(conn, "the player walked away");
}

void PlayerSession::init_quests(PlayerWorld& world, const svc::Character& character) {
    quest_worker_ = world.quest_worker();
    const quest::QuestBook* book = world.quests();
    if (!quest_worker_ || !book || !character.quest_log) {
        spdlog::info("[session] {}: quests are off for this character ({})", pb_.setup().name,
                     !book            ? "this content.db has no quest tables"
                     : !quest_worker_ ? "world has no quest service"
                                      : "its quest log did not load at the login");
        return;
    }
    const double now = pb_.setup().world_time;
    const size_t repaired = quest_log_.load(*character.quest_log, book->data(), now);
    quests_on_ = true;
    std::string err;
    repl::FastArrays* arrays = pb_.quest_arrays();
    const spawns::AreaReport* area = world.area_report();
    quest::RegistryPoints points(world.registry(), area ? area->data.get() : nullptr);
    quest::MarkerContext markers;
    markers.book = book;
    markers.points = &points;
    if (!arrays || !quest::sync(*arrays, quest_log_, &err, &markers)) {
        spdlog::warn("[session] {}: the quest log does not fit the quest component, so quests are off for this character: {}",
                     pb_.setup().name, err);
        quests_on_ = false;
        return;
    }
    log_quest_markers(markers);
    quest_worker_->watch(character_id_);
    spdlog::info("[session] {}: quest log: {} quest(s) taken, {} finished, {} variable(s){}", pb_.setup().name,
                 quest_log_.instances().size(), quest_log_.completed().size(), quest_log_.variables().size(),
                 repaired ? " (" + std::to_string(repaired) + " repaired or dropped: the log is saved again)" : "");
}

void PlayerSession::quest_sync(Connection& conn, const char* why) {
    quests_touched_ = true;
    givers_dirty_ = true;
    repl::FastArrays* arrays = pb_.quest_arrays();
    std::string err;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    std::optional<quest::RegistryPoints> points;
    quest::MarkerContext markers;
    if (qb && registry_) {
        const spawns::AreaReport* area = world_ ? world_->area_report() : nullptr;
        points.emplace(*registry_, area ? area->data.get() : nullptr);
        markers.book = qb;
        markers.points = &*points;
    }
    if (!arrays || !quest::sync(*arrays, quest_log_, &err, markers.points ? &markers : nullptr)) {
        spdlog::warn("[session] {}: the quest component is not brought in line with the log ({}): {}", name(), why, err);
        return;
    }
    log_quest_markers(markers);
    queue("QuestConsumerComponent", [](bootstrap::PlayerBootstrap& pb) { return pb.quest_update(); }, conn, false);
}

void PlayerSession::start_quest(Connection& conn, const content::QuestRow& def, int64_t source, const char* why, Clock::time_point now) {
    ++quests_taken_;
    spdlog::log(level_for(quests_logged_, 100), "[session] {}: took quest {} ({}), {} objective(s) ({})", name(), def.name, def.guid,
                def.objectives.size(), why);
    quest_engine(conn, now, "a quest was taken", [&](quest::Engine& eng) { eng.start(def, source); });
    if (!quest_log_.find(def.guid)) {
        quest_log_.start(def, quest_clock(now), source);
        quest_sync(conn, "a quest was taken");
    }
}

void PlayerSession::on_quest_offer(Connection& conn, uint8_t response, Clock::time_point now) {
    if (dead_ || !world_ || !world_->dialogue() || !world_->quests() || !talk_.open() || !talk_.sub() || !room_for(2)) {
        ++dialogue_refused_;
        spdlog::log(level_for(dialogue_logged_), "[session] {}: an answer to a quest offer is refused: {}", name(),
                    quest_offer_refusal_reason(dead_, talk_.open(), talk_.sub().has_value()));
        return;
    }
    const char* gone_why = nullptr;
    const world::WorldActor* npc = find_dialogue_npc(registry_, talk_.state().source, checker_.good(), gone_why);
    if (!npc) return end_dialogue(conn, gone_why);
    const auto facts = dialogue_facts(npc);
    dialogue::Evaluator ev(*world_->dialogue(), *facts,
                           [this](const std::string& what) { log_unanswered_condition(dialogue_told_, name(), what); });
    const dialogue::Talk::Sub sub = *talk_.sub();
    OfferSubStep step = evaluate_quest_offer_sub(name(), sub, response, quests_logged_);
    const OfferAcceptCtx ctx{name(), sub.quest, step.try_accept, quests_on_, world_->quests()};
    if (const content::QuestRow* def = check_quest_offer_accept(ctx, quest_log_, ev, quests_logged_, quests_refused_)) {
        start_quest(conn, *def, npc->npc.instance_guid, "the player took the offer", now);
        step.taken = true;
    }
    const dialogue::Result r = talk_.finish_sub(*world_->dialogue(), ev, step.taken, step.back);
    run_talk_actions(conn, r, *npc, now);
    if (r.outcome != dialogue::Outcome::Changed) return;
    ++dialogue_steps_;
    if (!talk_.open()) ++dialogue_closes_;
    send_dialogue(conn, "a quest offer was answered", true);
}

bool PlayerSession::end_quest(Connection& conn, int64_t record, const char* kind, Clock::time_point now) {
    const std::string k = kind;
    const bool ok = k == "finish" ? quest_log_.finish(record) : quest_log_.remove(record);
    if (!ok) return false;
    if (k == "abandon") ++quests_abandoned_;
    remove_quest_items(conn, record, now);
    quest_sync(conn, kind);
    return true;
}

void PlayerSession::remove_quest_items(Connection& conn, int64_t quest, Clock::time_point /*now*/) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    items::Outcome all;
    all.ok = true;
    for (const svc::InventoryItem& it : inventory_.items()) {
        if (it.container != svc::ItemContainer::Quest) continue;
        const auto* links = qb->item_links(it.item_record_id);
        if (!links || !item_bound_to_quest_delete(*links, quest)) continue;
        items::Outcome o = inventory_.consume(it.container, it.slot, it.instance_guid, it.quantity);
        if (!o.ok) continue;
        all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    if (all.events.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(
            events.begin() + static_cast<std::ptrdiff_t>(at),
            events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, true);
    }
    spdlog::info("[session] {}: {} quest item event(s) left the quest bag with quest {}", name(), events.size(), quest);
}

void PlayerSession::on_abandon_quests(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const repl::Value* guids = quest_rpc_value(rpc, "QuestGuids");
    const repl::ArrayValue* list = guids ? guids->get_if<repl::ArrayValue>() : nullptr;
    if (!rpc.call.decoded || !list || !qb || !quests_on_) {
        ++quests_refused_;
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: ServerAbandonQuests is ignored: {}", name(),
                    !quests_on_         ? "quests are off"
                    : !rpc.call.decoded ? "its parameters do not decode"
                                        : "it names no quest");
        return;
    }
    if (list->elements.size() > 64) {
        ++quests_refused_;
        return;
    }
    for (const repl::ElementState& e : list->elements) {
        const quest::Guid128 g = parse_quest_guid_element(e);
        const quest::QuestInstance* q = quest_log_.find_guid(g);
        if (!q) {
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: abandon of quest instance {}: it is not in the log", name(),
                        g.hex());
            continue;
        }
        const content::QuestRow* def = qb->quest(q->record);
        if (def && !def->can_abandon) {
            ++quests_refused_;
            spdlog::log(level_for(quests_logged_, 100), "[session] {}: quest {} cannot be abandoned (its record says so)", name(),
                        def->name);
            continue;
        }
        const int64_t record = q->record;
        end_quest(conn, record, "abandon", now);
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: abandoned quest {}", name(), def ? def->name : std::to_string(record));
    }
}

void PlayerSession::apply_quest_command(Connection& conn, const svc::QuestCommand& cmd, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb || !quests_on_) return;
    const content::QuestRow* def = cmd.quest.empty() ? nullptr : qb->find(cmd.quest);
    if (apply_quest_log_command(quest_log_, cmd, def, name(), quests_touched_)) return;
    if (!def) {
        spdlog::warn("[session] {}: GM command {} {}: no such quest", name(), cmd.kind, cmd.quest);
        return;
    }
    if (cmd.kind == "give") {
        const dialogue::Book* book = world_->dialogue();
        if (!book) return;
        const auto facts = dialogue_facts(nullptr);
        dialogue::Evaluator ev(*book, *facts);
        const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/true);
        if (c != quest::Create::Ok) {
            spdlog::warn("[session] {}: GM give {}: refused, {}", name(), def->name, quest::create_name(c));
            return;
        }
        start_quest(conn, *def, 0, "a GM gave it", now);
    } else if (cmd.kind == "finish" || cmd.kind == "fail" || cmd.kind == "abandon") {
        if (!end_quest(conn, def->guid, cmd.kind.c_str(), now))
            spdlog::warn("[session] {}: GM {} {}: the quest is not in the log", name(), cmd.kind, def->name);
        else
            spdlog::info("[session] {}: GM {} {}", name(), cmd.kind, def->name);
    }
}

void PlayerSession::on_quest_debug_rpc(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string_view n = rpc.call.rpc->name;
    if (!gm_rpc_admitted(rpc.call.rpc->name, rpc.call.decoded ? "" : "the parameters did not decode", now) || !rpc.call.decoded) return;
    if (!world_ || !world_->quests() || !quests_on_) return;
    if (n == "ServerDebugStartQuest") {
        const auto facts = dialogue_facts(nullptr);
        if (const content::QuestRow* def = check_debug_start_quest(*world_, quest_log_, *facts, rpc, name())) {
            ++gm_rpcs_answered_;
            start_quest(conn, *def, 0, "ServerDebugStartQuest", now);
        }
    } else if (n == "ServerDebugEndQuest") {
        const int64_t id = quest_int_value(quest_rpc_value(rpc, "Guid"));
        if (end_quest(conn, id, "finish", now))
            ++gm_rpcs_answered_;
        else
            spdlog::warn("[gm] {}: ServerDebugEndQuest {}: the quest is not in the log", name(), id);
    } else if (n == "ServerDebugFailQuestByInstance") {
        const quest::Guid128 g = parse_debug_quest_guid(rpc);
        const quest::QuestInstance* q = quest_log_.find_guid(g);
        if (q && end_quest(conn, q->record, "fail", now))
            ++gm_rpcs_answered_;
        else
            spdlog::warn("[gm] {}: ServerDebugFailQuestByInstance {}: the quest is not in the log", name(), g.hex());
    } else if (n == "ServerClearCompletedQuests") {
        quest_log_.clear(0);
        quests_touched_ = true;
        ++gm_rpcs_answered_;
        spdlog::info("[gm] {}: ServerClearCompletedQuests: the finished quests and the variables are forgotten", name());
    }
}

void PlayerSession::run_quests(Connection& conn, Clock::time_point now) {
    if (!quests_on_ || !quest_worker_ || dead_) return;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    for (const svc::QuestCommand& cmd : quest_worker_->take(character_id_)) apply_quest_command(conn, cmd, now);
    if (phase_in_done_ && now - zone_checked_ >= std::chrono::seconds(1)) {
        zone_checked_ = now;
        const GoodPosition at = checker_.good();
        const int64_t zoi = static_cast<int64_t>(world::zoi_at(at.x, at.y));
        const dialogue::Book* book = zoi != zoi_seen_ ? world_->dialogue() : nullptr;
        zoi_seen_ = zoi;
        for (const int64_t q : book ? qb->zone_quests(zoi) : std::vector<int64_t>{}) {
            const content::QuestRow* def = qb->quest(q);
            if (!def) continue;
            const auto facts = dialogue_facts(nullptr);
            dialogue::Evaluator ev(*book, *facts);
            const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false);
            if (c == quest::Create::Ok)
                start_quest(conn, *def, 0, "the character stands in the zone that starts it", now);
            else
                spdlog::log(level_for(quests_logged_, 100), "[session] {}: the zone {} would start quest {}: not taken, {}", name(), zoi,
                            def->name, quest::create_name(c));
        }
    }
    tick_quests(conn, now);
    if (quest_log_.changed()) {
        quest_worker_->save(character_id_, quest_log_.to_saved());
        quest_log_.clear_changed();
    }
}

}  // namespace aoc::worldd
